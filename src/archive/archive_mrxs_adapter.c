/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_mrxs_adapter.h"

#include "archive_sha256.h"
#include "dicom.h"
#include "image_loader.h"
#include "jpeg_decoder.h"

#include <stdarg.h>

#define ARCHIVE_MRXS_MAX_FRAME_SIZE MEGABYTES(64)

static void archive_mrxs_set_error(char* error, size_t capacity,
		const char* format, ...) {
	if (!error || !capacity) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_mrxs_uid(char uid[65], const char* fingerprint,
		const char* role, u32 ordinal) {
	char input[192];
	i32 length = snprintf(input, sizeof(input), "%s\n%s\n%u",
	                      fingerprint, role, ordinal);
	char digest[65];
	if (length <= 0 || (size_t)length >= sizeof(input) ||
	    !archive_sha256_bytes(input, (size_t)length, digest))
		return false;
	u8 decimal[40] = {0};
	i32 decimal_count = 1;
	for (i32 byte_index = 0; byte_index < 16; ++byte_index) {
		u8 high = (u8)(digest[byte_index * 2] <= '9' ?
			digest[byte_index * 2] - '0' :
			digest[byte_index * 2] - 'a' + 10);
		u8 low = (u8)(digest[byte_index * 2 + 1] <= '9' ?
			digest[byte_index * 2 + 1] - '0' :
			digest[byte_index * 2 + 1] - 'a' + 10);
		u32 carry = (u32)((high << 4) | low);
		for (i32 digit = 0; digit < decimal_count; ++digit) {
			u32 value = (u32)decimal[digit] * 256 + carry;
			decimal[digit] = (u8)(value % 10);
			carry = value / 10;
		}
		while (carry) {
			decimal[decimal_count++] = (u8)(carry % 10);
			carry /= 10;
		}
	}
	memcpy(uid, "2.25.", 5);
	for (i32 i = 0; i < decimal_count; ++i)
		uid[5 + i] = (char)('0' + decimal[decimal_count - i - 1]);
	uid[5 + decimal_count] = '\0';
	return true;
}

static void archive_mrxs_set_uid(dicom_ui_t* value, const char* uid) {
	snprintf(value->value, sizeof(value->value), "%s", uid);
	value->len = (u16)strlen(value->value);
	value->present = true;
}

bool archive_mrxs_adapter_open(archive_mrxs_adapter_t* adapter,
		const char* mrxs_path,
		char* error, size_t error_capacity) {
	if (adapter) memset(adapter, 0, sizeof(*adapter));
	if (error && error_capacity) error[0] = '\0';
	if (!adapter || !mrxs_path ||
	    strlen(mrxs_path) >= sizeof(((file_info_t*)0)->full_filename))
		return false;
	struct stat source_info = {0};
	if (platform_stat(mrxs_path, &source_info) != 0 ||
	    !S_ISREG(source_info.st_mode)) {
		archive_mrxs_set_error(error, error_capacity,
		                       "MRXS index is missing or invalid: '%s'",
		                       mrxs_path);
		return false;
	}
	char directory_path[512];
	snprintf(directory_path, sizeof(directory_path), "%s", mrxs_path);
	char* extension = strrchr(directory_path, '.');
	if (!extension || strcasecmp(extension, ".mrxs") != 0) return false;
	*extension = '\0';
	struct stat directory_info = {0};
	file_info_t directory = {0};
	snprintf(directory.full_filename, sizeof(directory.full_filename),
	         "%s", directory_path);
	directory.is_valid =
		platform_stat(directory_path, &directory_info) == 0;
	directory.is_directory =
		directory.is_valid && S_ISDIR(directory_info.st_mode);
	if (!directory.is_directory ||
	    !mrxs_open_from_directory(&adapter->slide, &directory, NULL)) {
		archive_mrxs_set_error(error, error_capacity,
		                       "built-in MRXS reader rejected '%s'",
		                       mrxs_path);
		archive_mrxs_adapter_close(adapter);
		return false;
	}
	if (adapter->slide.is_fluorescence ||
	    adapter->slide.has_overlapping_tiles ||
	    !adapter->slide.is_mpp_known ||
	    adapter->slide.level_count <= 0 ||
	    adapter->slide.level_count > ARCHIVE_MRXS_MAX_LEVELS) {
		archive_mrxs_set_error(
			error, error_capacity,
			"MRXS profile requires brightfield, known spacing, no overlap, "
			"and at most %d pyramid levels", ARCHIVE_MRXS_MAX_LEVELS);
		archive_mrxs_adapter_close(adapter);
		return false;
	}
	for (i32 i = 0; i < adapter->slide.level_count; ++i) {
		const mrxs_level_t* level = adapter->slide.levels + i;
		if (level->image_format != MRXS_IMAGE_FORMAT_JPEG ||
		    level->tile_width <= 0 || level->tile_width > UINT16_MAX ||
		    level->tile_height <= 0 || level->tile_height > UINT16_MAX ||
		    level->width_in_tiles <= 0 || level->height_in_tiles <= 0 ||
		    !level->tiles) {
			archive_mrxs_set_error(
				error, error_capacity,
				"MRXS profile requires a regular power-of-two JPEG pyramid");
			archive_mrxs_adapter_close(adapter);
			return false;
		}
		u32 scale = 1u << i;
		i32 matrix_columns =
			(adapter->slide.base_width_in_pixels + (i32)scale - 1) /
			(i32)scale;
		i32 matrix_rows =
			(adapter->slide.base_height_in_pixels + (i32)scale - 1) /
			(i32)scale;
		i32 grid_columns =
			(matrix_columns + level->tile_width - 1) / level->tile_width;
		i32 grid_rows =
			(matrix_rows + level->tile_height - 1) / level->tile_height;
		float expected_mpp_x = adapter->slide.mpp_x * scale;
		float expected_mpp_y = adapter->slide.mpp_y * scale;
		if (level->width_in_tiles != grid_columns ||
		    level->height_in_tiles != grid_rows ||
		    fabsf(level->um_per_pixel_x - expected_mpp_x) >
			    MAX(0.0001f, expected_mpp_x * 0.001f) ||
		    fabsf(level->um_per_pixel_y - expected_mpp_y) >
			    MAX(0.0001f, expected_mpp_y * 0.001f)) {
			archive_mrxs_set_error(
				error, error_capacity,
				"MRXS profile requires a regular power-of-two JPEG pyramid");
			archive_mrxs_adapter_close(adapter);
			return false;
		}
	}
	/* Admission validates every stored tile header, with bounded memory and no
	 * pixel decode. Native payloads must actually satisfy the advertised syntax. */
	u8* header = malloc(KILOBYTES(64));
	bool valid_headers = header != NULL;
	for (i32 i = 0; i < adapter->slide.level_count && valid_headers; ++i) {
		const mrxs_level_t* level = adapter->slide.levels + i;
		u64 tile_count = (u64)level->width_in_tiles * level->height_in_tiles;
		for (u64 tile = 0; tile < tile_count && valid_headers; ++tile) {
			const mrxs_hier_entry_t* entry = &level->tiles[tile].hier_entry;
			if (!entry->length) continue;
			size_t bytes = MIN(entry->length, KILOBYTES(64));
			valid_headers = entry->file < (u32)adapter->slide.dat_count &&
				entry->length <= ARCHIVE_MRXS_MAX_FRAME_SIZE &&
				file_handle_read_at_offset(header, adapter->slide.dat_file_handles[entry->file],
					entry->offset, bytes) == bytes &&
				jpeg_validate_baseline_ycbcr_header(header, (u32)bytes, level->tile_width, level->tile_height);
		}
	}
	free(header);
	if (!valid_headers) {
		archive_mrxs_set_error(error, error_capacity,
			"MRXS requires standalone baseline 8-bit YCbCr JPEG tiles with matching dimensions and headers within 64 KiB");
		archive_mrxs_adapter_close(adapter);
		return false;
	}

	snprintf(adapter->source_path, sizeof(adapter->source_path), "%s",
	         mrxs_path);
	adapter->is_open = true;
	return true;
}

bool archive_mrxs_adapter_set_identity(archive_mrxs_adapter_t* adapter, const char* bundle_sha256) {
	if (!adapter || !adapter->is_open || !bundle_sha256 || strlen(bundle_sha256) != 64) return false;
	if (!archive_mrxs_uid(adapter->study_instance_uid,
	                      bundle_sha256, "study", 0) ||
	    !archive_mrxs_uid(adapter->series_instance_uid,
	                      bundle_sha256, "series", 0)) {
		return false;
	}
	for (i32 i = 0; i < adapter->slide.level_count; ++i)
		if (!archive_mrxs_uid(adapter->sop_instance_uids[i],
		                      bundle_sha256, "volume-level",
		                      (u32)i)) {
			return false;
		}
	return true;
}

void archive_mrxs_adapter_close(archive_mrxs_adapter_t* adapter) {
	if (!adapter) return;
	mrxs_destroy(&adapter->slide);
	memset(adapter, 0, sizeof(*adapter));
}

bool archive_mrxs_adapter_build_recipe(const char* mrxs_path,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, char** recipe,
		size_t* recipe_size) {
	if (recipe) *recipe = NULL;
	if (recipe_size) *recipe_size = 0;
	if (!mrxs_path || !mrxs_path[0] || strchr(mrxs_path, '\n') ||
	    !bundle_descriptor_sha256 ||
	    strlen(bundle_descriptor_sha256) != 64 ||
	    !bundle_descriptor_size || !representation_fingerprint ||
	    strlen(representation_fingerprint) != 64 || !recipe || !recipe_size)
		return false;
	size_t capacity = strlen(mrxs_path) + 384;
	char* result = malloc(capacity);
	if (!result) return false;
	i32 length = snprintf(
		result, capacity,
		ARCHIVE_MRXS_RECIPE_MAGIC "\n"
		"adapter\tslidescape-mrxs\t" ARCHIVE_MRXS_ADAPTER_VERSION_STRING "\n"
		"bundle-descriptor-sha256\t%s\n"
		"bundle-descriptor-size\t%llu\n"
		"representation-fingerprint\t%s\n"
		"source-path\t%s\n",
		bundle_descriptor_sha256,
		(unsigned long long)bundle_descriptor_size,
		representation_fingerprint, mrxs_path);
	if (length <= 0 || (size_t)length >= capacity) {
		free(result);
		return false;
	}
	*recipe = result;
	*recipe_size = (size_t)length;
	return true;
}

bool archive_mrxs_adapter_build_shell(archive_mrxs_adapter_t* adapter,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, archive_shell_buffer_t* shell,
		char* error, size_t error_capacity) {
	if (shell) memset(shell, 0, sizeof(*shell));
	if (!adapter || !adapter->is_open || !shell ||
	    !bundle_descriptor_sha256 || !bundle_descriptor_size ||
	    !representation_fingerprint) return false;
	u32 count = (u32)adapter->slide.level_count;
	dicom_instance_t* instances = calloc(count, sizeof(*instances));
	archive_shell_build_part10_input_t* inputs =
		calloc(count, sizeof(*inputs));
	archive_shell_native_payload_t** payloads =
		calloc(count, sizeof(*payloads));
	if (!instances || !inputs || !payloads) {
		free(payloads);
		free(inputs);
		free(instances);
		return false;
	}
	for (u32 i = 0; i < count; ++i) {
		dicom_instance_t* instance = instances + i;
		const mrxs_level_t* level = adapter->slide.levels + i;
		archive_mrxs_set_uid(&instance->general_study.StudyInstanceUID,
		                    adapter->study_instance_uid);
		archive_mrxs_set_uid(&instance->general_series.SeriesInstanceUID,
		                    adapter->series_instance_uid);
		archive_mrxs_set_uid(&instance->sop_instance_uid,
		                    adapter->sop_instance_uids[i]);
		archive_mrxs_set_uid(
			&instance->sop_class_uid,
			"1.2.840.10008.5.1.4.1.1.77.1.6");
		archive_mrxs_set_uid(
			&instance->transfer_syntax_uid,
			"1.2.840.10008.1.2.4.50");
		snprintf(instance->general_series.Modality.value,
		         sizeof(instance->general_series.Modality.value), "SM");
		instance->image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
		instance->has_valid_wsi_image_type = true;
		/* The virtual pyramid is a derived representation of the native source. */
		instance->is_image_original = false;
		instance->is_image_resampled = i > 0;
		instance->photometric_interpretation =
			DICOM_PHOTOMETRIC_INTERPRETATION_YBR_FULL_422;
		instance->instance_number = (i64)i + 1;
		instance->rows = (u16)level->tile_height;
		instance->columns = (u16)level->tile_width;
		u32 occupied_frame_count = 0;
		for (i32 tile_index = 0;
		     tile_index < level->width_in_tiles * level->height_in_tiles;
		     ++tile_index)
			if (level->tiles[tile_index].hier_entry.length)
				++occupied_frame_count;
		if (!occupied_frame_count) {
			archive_mrxs_set_error(
				error, error_capacity,
				"MRXS pyramid level %u has no stored tiles", i);
			for (u32 j = 0; j <= i; ++j) {
				arrfree(instances[j].per_frame_plane_position_slide);
				arrfree(instances[j].optical_paths);
				free(payloads[j]);
			}
			free(payloads);
			free(inputs);
			free(instances);
			return false;
		}
		payloads[i] = calloc(
			occupied_frame_count, sizeof(*payloads[i]));
		if (!payloads[i]) {
			for (u32 j = 0; j <= i; ++j) {
				arrfree(instances[j].per_frame_plane_position_slide);
				arrfree(instances[j].optical_paths);
				free(payloads[j]);
			}
			free(payloads);
			free(inputs);
			free(instances);
			return false;
		}
		instance->number_of_frames = occupied_frame_count;
		u32 scale = 1u << i;
		instance->total_pixel_matrix_columns =
			((u32)adapter->slide.base_width_in_pixels + scale - 1) / scale;
		instance->total_pixel_matrix_rows =
			((u32)adapter->slide.base_height_in_pixels + scale - 1) / scale;
		instance->samples_per_pixel = 3;
		instance->bits_allocated = 8;
		instance->bits_stored = 8;
		instance->high_bit = 7;
		instance->pixel_spacing =
			V2F(level->um_per_pixel_y / 1000.0f,
			    level->um_per_pixel_x / 1000.0f);
		instance->is_tiled_full = false;
		instance->number_of_optical_paths = 1;
		instance->total_pixel_matrix_focal_planes = 1;
		instance->is_total_pixel_matrix_origin_known = true;
		instance->is_image_orientation_slide_known = true;
		instance->image_orientation_slide[0] = 1.0f;
		instance->image_orientation_slide[4] = 1.0f;
		u32 frame_index = 0;
		for (i32 tile_index = 0;
		     tile_index < level->width_in_tiles * level->height_in_tiles;
		     ++tile_index) {
			if (!level->tiles[tile_index].hier_entry.length) continue;
			mrxs_hier_entry_t entry =
				level->tiles[tile_index].hier_entry;
			u32 tile_x = (u32)(tile_index % level->width_in_tiles);
			u32 tile_y = (u32)(tile_index / level->width_in_tiles);
			dicom_plane_position_slide_t position = {0};
			position.column_position_in_total_image_pixel_matrix =
				(i32)(tile_x * instance->columns + 1);
			position.row_position_in_total_image_pixel_matrix =
				(i32)(tile_y * instance->rows + 1);
			position.offset_in_slide_coordinate_system.x =
				(float)(position.column_position_in_total_image_pixel_matrix - 1) *
				instance->pixel_spacing.y;
			position.offset_in_slide_coordinate_system.y =
				(float)(position.row_position_in_total_image_pixel_matrix - 1) *
				instance->pixel_spacing.x;
			arrput(instance->per_frame_plane_position_slide, position);
			payloads[i][frame_index++] =
				(archive_shell_native_payload_t){
					.file_index = entry.file,
					.offset = entry.offset,
					.size = entry.length,
				};
		}
		dicom_optical_path_t optical_path = {0};
		optical_path.OpticalPathIdentifier.present = true;
		snprintf(optical_path.OpticalPathIdentifier.value,
		         sizeof(optical_path.OpticalPathIdentifier.value), "0");
		/* JPEG YCbCr coding does not establish an sRGB color profile. */
		arrput(instance->optical_paths, optical_path);
		inputs[i] = (archive_shell_build_part10_input_t){
			.instance = instance,
			.source_sha256 = bundle_descriptor_sha256,
			.source_byte_size = bundle_descriptor_size,
			.representation_fingerprint = representation_fingerprint,
			.provider_kind = ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE,
			.provider_parameter_version = ARCHIVE_MRXS_ADAPTER_VERSION,
			.native_payloads = payloads[i],
			.native_payload_count = occupied_frame_count,
		};
	}
	archive_shell_build_part10_aggregate_input_t aggregate = {
		.instances = inputs,
		.instance_count = count,
		.representation_fingerprint = representation_fingerprint,
	};
	bool success = archive_shell_build_part10_aggregate(
		&aggregate, shell, error, error_capacity);
	for (u32 i = 0; i < count; ++i) {
		arrfree(instances[i].per_frame_plane_position_slide);
		arrfree(instances[i].optical_paths);
		free(payloads[i]);
	}
	free(payloads);
	free(inputs);
	free(instances);
	return success;
}

bool archive_mrxs_adapter_get_rgb_tile(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 tile_index, u8** data, size_t* size,
		char* error, size_t error_capacity) {
	if (data) *data = NULL;
	if (size) *size = 0;
	if (!adapter || !adapter->is_open || !data || !size ||
	    level_index >= (u32)adapter->slide.level_count) return false;
	mrxs_level_t* level = adapter->slide.levels + level_index;
	u64 frame_count = (u64)level->width_in_tiles * level->height_in_tiles;
	if (tile_index >= frame_count) return false;
	size_t pixel_count = (size_t)level->tile_width * level->tile_height;
	u8* bgra = NULL;
	if (level->tiles[tile_index].hier_entry.length) {
		bgra = mrxs_decode_tile_to_bgra(
			&adapter->slide, (i32)level_index, (i32)tile_index);
	} else if (pixel_count <= SIZE_MAX / 4) {
		bgra = malloc(pixel_count * 4);
		if (bgra) {
			u32 fill = level->image_fill_color_bgr;
			for (size_t i = 0; i < pixel_count; ++i) {
				bgra[i * 4] = (u8)fill;
				bgra[i * 4 + 1] = (u8)(fill >> 8);
				bgra[i * 4 + 2] = (u8)(fill >> 16);
				bgra[i * 4 + 3] = 255;
			}
		}
	}
	if (!bgra || pixel_count > SIZE_MAX / 3) {
		free(bgra);
		archive_mrxs_set_error(error, error_capacity,
		                       "built-in MRXS decoder could not read frame");
		return false;
	}
	u8* rgb = malloc(pixel_count * 3);
	if (!rgb) {
		free(bgra);
		return false;
	}
	for (size_t i = 0; i < pixel_count; ++i) {
		rgb[i * 3] = bgra[i * 4 + 2];
		rgb[i * 3 + 1] = bgra[i * 4 + 1];
		rgb[i * 3 + 2] = bgra[i * 4];
	}
	free(bgra);
	*data = rgb;
	*size = pixel_count * 3;
	return true;
}

bool archive_mrxs_adapter_get_jpeg_tile(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 tile_index, u8** data, size_t* size,
		char* error, size_t error_capacity) {
	if (data) *data = NULL;
	if (size) *size = 0;
	if (!adapter || !adapter->is_open || !data || !size ||
	    level_index >= (u32)adapter->slide.level_count) return false;
	mrxs_level_t* level = adapter->slide.levels + level_index;
	u64 frame_count = (u64)level->width_in_tiles * level->height_in_tiles;
	if (tile_index >= frame_count) return false;
	mrxs_hier_entry_t entry =
		level->tiles[tile_index].hier_entry;
	if (entry.length) {
		if (!adapter->slide.dat_file_handles ||
		    entry.file >= (u32)adapter->slide.dat_count ||
		    entry.length > ARCHIVE_MRXS_MAX_FRAME_SIZE)
			return false;
		u8* jpeg = malloc(entry.length);
		if (!jpeg) return false;
		size_t read = file_handle_read_at_offset(
			jpeg, adapter->slide.dat_file_handles[entry.file],
			entry.offset, entry.length);
		if (read != entry.length || entry.length < 4 ||
		    jpeg[0] != 0xff || jpeg[1] != 0xd8) {
			free(jpeg);
			archive_mrxs_set_error(
				error, error_capacity,
				"MRXS tile is not a standalone JPEG frame");
			return false;
		}
		*data = jpeg;
		*size = entry.length;
		return true;
	}
	archive_mrxs_set_error(
		error, error_capacity,
		"requested MRXS grid position has no stored tile");
	return false;
}
