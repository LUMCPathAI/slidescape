/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_manifest.h"

#include "archive_frame_map.h"
#include "common.h"
#include "dicom.h"
#include "dicom_uid.h"

#include <math.h>
#include <stdarg.h>

#define ARCHIVE_WSI_SOP_CLASS_UID "1.2.840.10008.5.1.4.1.1.77.1.6"

/*
 * Staging is deliberately separate from publication. These JSON files let ingest
 * and adapter work establish validated inputs without making them discoverable.
 * The current temp-file rename is only single-process development scaffolding: it is
 * neither the future transaction log nor a crash-durability guarantee. Production
 * publication will commit database rows that reference immutable object-store keys.
 */

static void archive_manifest_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static size_t archive_manifest_bounded_length(const char* value, size_t capacity) {
	size_t length = 0;
	while (length < capacity && value[length]) ++length;
	return length;
}

static bool archive_manifest_id_is_valid(const char* value, size_t capacity) {
	size_t length = archive_manifest_bounded_length(value, capacity);
	if (!length || length >= capacity) return false;
	for (size_t i = 0; i < length; ++i) {
		char c = value[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
	}
	return true;
}

static bool archive_manifest_uid_is_valid(const char* value, size_t capacity) {
	size_t length = archive_manifest_bounded_length(value, capacity);
	return length > 0 && length < capacity && dicom_uid_is_valid_string(value, length);
}

static bool archive_sha256_is_valid(const char* value) {
	if (archive_manifest_bounded_length(value, 65) != 64) return false;
	for (i32 i = 0; i < 64; ++i) {
		char c = value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

static bool archive_manifest_photometric_is_valid(const char* value) {
	static const char* supported[] = {
		"MONOCHROME1", "MONOCHROME2", "PALETTE COLOR", "RGB",
		"YBR_FULL", "YBR_FULL_422", "YBR_ICT", "YBR_RCT",
	};
	for (i32 i = 0; i < COUNT(supported); ++i) {
		if (strcmp(value, supported[i]) == 0) return true;
	}
	return false;
}

static const char* archive_source_format_string(archive_source_format_t format) {
	switch (format) {
		case ARCHIVE_SOURCE_FORMAT_DICOM: return "dicom";
		case ARCHIVE_SOURCE_FORMAT_MRXS: return "mrxs";
		case ARCHIVE_SOURCE_FORMAT_ISYNTAX: return "isyntax";
		case ARCHIVE_SOURCE_FORMAT_TIFF: return "tiff";
		default: return NULL;
	}
}

static const char* archive_manifest_provider_kind_string(
		archive_manifest_provider_kind_t kind) {
	switch (kind) {
		case ARCHIVE_MANIFEST_PROVIDER_PART10: return "part10";
		case ARCHIVE_MANIFEST_PROVIDER_NATIVE: return "native";
		default: return NULL;
	}
}

static const char* archive_manifest_representation_source_string(
		archive_manifest_representation_source_t source) {
	switch (source) {
		case ARCHIVE_MANIFEST_REPRESENTATION_PASSTHROUGH: return "pass-through";
		case ARCHIVE_MANIFEST_REPRESENTATION_GENERATED: return "generated";
		default: return NULL;
	}
}

static const char* archive_manifest_frame_map_kind_string(
		archive_manifest_frame_map_kind_t kind) {
	switch (kind) {
		case ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR: return "regular-row-major";
		case ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT: return "explicit";
		default: return NULL;
	}
}

static bool archive_manifest_relative_locator_is_valid(
		const char* value, size_t capacity) {
	size_t length = archive_manifest_bounded_length(value, capacity);
	if (!length || length >= capacity || value[0] == '/' || value[0] == '\\' ||
	    strstr(value, "..")) return false;
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)value[i];
		if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
	}
	return true;
}

static bool archive_manifest_json_string_is_valid(const char* value, size_t capacity,
		bool allow_empty) {
	size_t length = archive_manifest_bounded_length(value, capacity);
	if (length >= capacity || (!allow_empty && !length)) return false;
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)value[i];
		if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
	}
	return true;
}

static const char* archive_manifest_image_flavor(const dicom_instance_t* instance) {
	switch (instance->image_flavor) {
		case DICOM_IMAGE_FLAVOR_VOLUME: return "VOLUME";
		case DICOM_IMAGE_FLAVOR_LABEL: return "LABEL";
		case DICOM_IMAGE_FLAVOR_OVERVIEW: return "OVERVIEW";
		case DICOM_IMAGE_FLAVOR_THUMBNAIL: return "THUMBNAIL";
		default: return NULL;
	}
}

static const char* archive_manifest_photometric(const dicom_instance_t* instance) {
	switch (instance->photometric_interpretation) {
		case DICOM_PHOTOMETRIC_INTERPRETATION_MONOCHROME1: return "MONOCHROME1";
		case DICOM_PHOTOMETRIC_INTERPRETATION_MONOCHROME2: return "MONOCHROME2";
		case DICOM_PHOTOMETRIC_INTERPRETATION_PALETTE_COLOR: return "PALETTE COLOR";
		case DICOM_PHOTOMETRIC_INTERPRETATION_RGB: return "RGB";
		case DICOM_PHOTOMETRIC_INTERPRETATION_YBR_FULL: return "YBR_FULL";
		case DICOM_PHOTOMETRIC_INTERPRETATION_YBR_FULL_422: return "YBR_FULL_422";
		case DICOM_PHOTOMETRIC_INTERPRETATION_YBR_ICT: return "YBR_ICT";
		case DICOM_PHOTOMETRIC_INTERPRETATION_YBR_RCT: return "YBR_RCT";
		default: return NULL;
	}
}

bool archive_manifest_populate_part10_instance(archive_virtual_instance_manifest_t* manifest,
		const dicom_instance_t* instance) {
	if (!manifest || !instance || instance->number_of_frames <= 0 ||
	    instance->instance_number <= 0 || instance->rows == 0 || instance->columns == 0 ||
	    instance->total_pixel_matrix_rows == 0 || instance->total_pixel_matrix_columns == 0 ||
	    arrlen(instance->optical_paths) <= 0) return false;
	if (instance->number_of_optical_paths &&
	    instance->number_of_optical_paths != (u32)arrlen(instance->optical_paths)) return false;
	const char* flavor = archive_manifest_image_flavor(instance);
	const char* photometric = archive_manifest_photometric(instance);
	if (!flavor || !photometric) return false;

	snprintf(manifest->study_instance_uid, sizeof(manifest->study_instance_uid), "%s",
	         instance->general_study.StudyInstanceUID.value);
	snprintf(manifest->series_instance_uid, sizeof(manifest->series_instance_uid), "%s",
	         instance->general_series.SeriesInstanceUID.value);
	snprintf(manifest->sop_instance_uid, sizeof(manifest->sop_instance_uid), "%s",
	         instance->sop_instance_uid.value);
	snprintf(manifest->sop_class_uid, sizeof(manifest->sop_class_uid), "%s",
	         instance->sop_class_uid.value);
	snprintf(manifest->transfer_syntax_uid, sizeof(manifest->transfer_syntax_uid), "%s",
	         instance->transfer_syntax_uid.value);
	snprintf(manifest->image_flavor, sizeof(manifest->image_flavor), "%s", flavor);
	snprintf(manifest->photometric_interpretation,
	         sizeof(manifest->photometric_interpretation), "%s", photometric);
	manifest->instance_number = (u32)instance->instance_number;
	manifest->frame_count = (u32)instance->number_of_frames;
	manifest->rows = instance->rows;
	manifest->columns = instance->columns;
	manifest->total_pixel_matrix_rows = instance->total_pixel_matrix_rows;
	manifest->total_pixel_matrix_columns = instance->total_pixel_matrix_columns;
	manifest->provider_kind = ARCHIVE_MANIFEST_PROVIDER_PART10;
	manifest->representation_source = ARCHIVE_MANIFEST_REPRESENTATION_PASSTHROUGH;
	manifest->optical_path_count = (u32)arrlen(instance->optical_paths);
	if (manifest->optical_path_count == 1 &&
	    instance->optical_paths[0].OpticalPathIdentifier.present) {
		snprintf(manifest->default_optical_path_identifier,
		         sizeof(manifest->default_optical_path_identifier), "%s",
		         instance->optical_paths[0].OpticalPathIdentifier.value);
	}

	float* focal_offsets = NULL;
	if (arrlen(instance->per_frame_plane_position_slide) == instance->number_of_frames) {
		for (i32 frame = 0; frame < arrlen(instance->per_frame_plane_position_slide); ++frame) {
			float offset = instance->per_frame_plane_position_slide[frame].
				z_offset_in_slide_coordinate_system;
			bool found = false;
			for (i32 focal = 0; focal < arrlen(focal_offsets); ++focal) {
				if (focal_offsets[focal] == offset) {
					found = true;
					break;
				}
			}
			if (!found) arrput(focal_offsets, offset);
		}
	}
	manifest->focal_plane_count = instance->total_pixel_matrix_focal_planes ?
		instance->total_pixel_matrix_focal_planes :
		(arrlen(focal_offsets) > 0 ? (u32)arrlen(focal_offsets) : 1);
	if (instance->total_pixel_matrix_focal_planes && arrlen(focal_offsets) > 0 &&
	    instance->total_pixel_matrix_focal_planes != (u32)arrlen(focal_offsets)) {
		arrfree(focal_offsets);
		return false;
	}
	if (arrlen(focal_offsets) == 1) {
		manifest->has_default_focal_plane_z_offset = true;
		manifest->default_focal_plane_z_offset_um = focal_offsets[0];
	}
	arrfree(focal_offsets);

	manifest->pixel_spacing_row_mm = instance->pixel_spacing.x;
	manifest->pixel_spacing_column_mm = instance->pixel_spacing.y;
	manifest->has_slide_coordinate_origin =
		instance->is_total_pixel_matrix_origin_known;
	manifest->slide_coordinate_origin_x_mm =
		instance->total_pixel_matrix_origin_x_mm;
	manifest->slide_coordinate_origin_y_mm =
		instance->total_pixel_matrix_origin_y_mm;
	manifest->slide_coordinate_origin_z_mm =
		instance->total_pixel_matrix_origin_z_mm;
	manifest->has_image_orientation_slide =
		instance->is_image_orientation_slide_known;
	memcpy(manifest->image_orientation_slide, instance->image_orientation_slide,
	       sizeof(manifest->image_orientation_slide));

	u64 tiles_per_row =
		((u64)manifest->total_pixel_matrix_columns + manifest->columns - 1) /
		manifest->columns;
	u64 tiles_per_column =
		((u64)manifest->total_pixel_matrix_rows + manifest->rows - 1) /
		manifest->rows;
	bool is_regular_single_dimension =
		manifest->optical_path_count == 1 && manifest->focal_plane_count == 1 &&
		tiles_per_row * tiles_per_column == manifest->frame_count &&
		(instance->is_tiled_full || manifest->frame_count == 1);
	if (manifest->frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_UNKNOWN) {
		manifest->frame_map_kind = is_regular_single_dimension ?
			ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR :
			ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT;
	}
	if (manifest->frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR) {
		manifest->tiles_per_row = (u32)tiles_per_row;
		manifest->tiles_per_column = (u32)tiles_per_column;
	}
	return true;
}

bool archive_manifest_materialize_part10_frame_map(const archive_repository_t* repository,
		archive_virtual_instance_manifest_t* manifest, const dicom_instance_t* instance,
		char* error, size_t error_capacity) {
	if (!repository || !manifest || !instance ||
	    !archive_manifest_populate_part10_instance(manifest, instance)) {
		archive_manifest_set_error(error, error_capacity,
			"could not populate Part 10 virtual-instance metadata");
		return false;
	}
	if (manifest->frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR)
		return true;
	if (manifest->frame_map_kind != ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT) {
		archive_manifest_set_error(error, error_capacity,
			"Part 10 instance has no supported frame-map organization");
		return false;
	}
	archive_frame_map_t map = {};
	if (!archive_frame_map_build_part10(&map, instance, error, error_capacity))
		return false;
	char object_key[257];
	bool success = archive_frame_map_stage(repository, manifest->representation_id,
		manifest->representation_version, &map, object_key, sizeof(object_key),
		error, error_capacity);
	archive_frame_map_destroy(&map);
	if (!success) return false;
	snprintf(manifest->frame_map_object_key, sizeof(manifest->frame_map_object_key),
	         "%s", object_key);
	snprintf(manifest->frame_dimension_map_object_key,
	         sizeof(manifest->frame_dimension_map_object_key), "%s", object_key);
	return true;
}

bool archive_source_bundle_manifest_is_valid(const archive_source_bundle_manifest_t* manifest) {
	return manifest && archive_manifest_id_is_valid(manifest->bundle_id, sizeof(manifest->bundle_id)) &&
	       archive_source_format_string(manifest->source_format) &&
	       archive_sha256_is_valid(manifest->bundle_digest_sha256) &&
	       archive_manifest_id_is_valid(manifest->adapter_name, sizeof(manifest->adapter_name)) &&
	       archive_manifest_id_is_valid(manifest->adapter_version, sizeof(manifest->adapter_version)) &&
	       manifest->object_count > 0 && manifest->total_bytes > 0;
}

bool archive_virtual_instance_manifest_is_valid(const archive_virtual_instance_manifest_t* manifest) {
	if (!manifest || !archive_manifest_id_is_valid(manifest->representation_id,
			sizeof(manifest->representation_id)) ||
	    !archive_manifest_id_is_valid(manifest->bundle_id, sizeof(manifest->bundle_id)) ||
	    manifest->representation_version == 0 ||
	    !archive_manifest_uid_is_valid(manifest->study_instance_uid,
			sizeof(manifest->study_instance_uid)) ||
	    !archive_manifest_uid_is_valid(manifest->series_instance_uid,
			sizeof(manifest->series_instance_uid)) ||
	    !archive_manifest_uid_is_valid(manifest->sop_instance_uid,
			sizeof(manifest->sop_instance_uid)) ||
	    !archive_manifest_uid_is_valid(manifest->sop_class_uid,
			sizeof(manifest->sop_class_uid)) ||
	    !archive_manifest_uid_is_valid(manifest->transfer_syntax_uid,
			sizeof(manifest->transfer_syntax_uid)) ||
	    strcmp(manifest->sop_class_uid, ARCHIVE_WSI_SOP_CLASS_UID) != 0) return false;
	if (strcmp(manifest->image_flavor, "VOLUME") != 0 &&
	    strcmp(manifest->image_flavor, "LABEL") != 0 &&
	    strcmp(manifest->image_flavor, "OVERVIEW") != 0 &&
	    strcmp(manifest->image_flavor, "THUMBNAIL") != 0) return false;
	if (!archive_manifest_photometric_is_valid(manifest->photometric_interpretation) ||
	    manifest->instance_number == 0 ||
	    manifest->frame_count == 0 || manifest->rows == 0 || manifest->columns == 0 ||
	    manifest->total_pixel_matrix_rows == 0 ||
	    manifest->total_pixel_matrix_columns == 0 ||
	    !archive_manifest_provider_kind_string(manifest->provider_kind) ||
	    !archive_manifest_relative_locator_is_valid(
		    manifest->provider_locator, sizeof(manifest->provider_locator)) ||
	    !archive_manifest_representation_source_string(manifest->representation_source) ||
	    !archive_manifest_frame_map_kind_string(manifest->frame_map_kind) ||
	    manifest->optical_path_count == 0 || manifest->focal_plane_count == 0) return false;
	if (manifest->provider_kind == ARCHIVE_MANIFEST_PROVIDER_PART10 &&
	    strncmp(manifest->provider_locator, "objects/", 8) != 0) return false;
	if (manifest->optical_path_count == 1) {
		if (!archive_manifest_json_string_is_valid(
			    manifest->default_optical_path_identifier,
			    sizeof(manifest->default_optical_path_identifier), false)) return false;
	} else {
		if (manifest->default_optical_path_identifier[0]) return false;
	}
	bool needs_dimension_map =
		manifest->optical_path_count > 1 || manifest->focal_plane_count > 1;
	if (needs_dimension_map) {
		if (!archive_manifest_relative_locator_is_valid(
			    manifest->frame_dimension_map_object_key,
			    sizeof(manifest->frame_dimension_map_object_key))) return false;
	} else if (manifest->frame_dimension_map_object_key[0]) {
		return false;
	}
	if (manifest->has_default_focal_plane_z_offset &&
	    (!isfinite(manifest->default_focal_plane_z_offset_um) ||
	     manifest->focal_plane_count != 1)) return false;
	bool spacing_absent =
		manifest->pixel_spacing_row_mm == 0.0f &&
		manifest->pixel_spacing_column_mm == 0.0f;
	bool spacing_present =
		manifest->pixel_spacing_row_mm > 0.0f &&
		manifest->pixel_spacing_column_mm > 0.0f;
	if (!spacing_absent && !spacing_present) return false;
	if (manifest->has_slide_coordinate_origin &&
	    (!isfinite(manifest->slide_coordinate_origin_x_mm) ||
	     !isfinite(manifest->slide_coordinate_origin_y_mm) ||
	     !isfinite(manifest->slide_coordinate_origin_z_mm))) return false;
	if (manifest->has_image_orientation_slide) {
		float row_length_squared = 0.0f;
		float column_length_squared = 0.0f;
		for (i32 i = 0; i < 6; ++i) {
			if (!isfinite(manifest->image_orientation_slide[i])) return false;
			if (i < 3)
				row_length_squared += manifest->image_orientation_slide[i] *
					manifest->image_orientation_slide[i];
			else
				column_length_squared += manifest->image_orientation_slide[i] *
					manifest->image_orientation_slide[i];
		}
		if (row_length_squared < 0.5f || column_length_squared < 0.5f) return false;
	}
	if (manifest->icc_profile_digest_sha256[0] &&
	    !archive_sha256_is_valid(manifest->icc_profile_digest_sha256)) return false;
	if (manifest->frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR) {
		u64 expected_columns =
			((u64)manifest->total_pixel_matrix_columns + manifest->columns - 1) /
			manifest->columns;
		u64 expected_rows =
			((u64)manifest->total_pixel_matrix_rows + manifest->rows - 1) /
			manifest->rows;
		if (manifest->tiles_per_row != expected_columns ||
		    manifest->tiles_per_column != expected_rows ||
		    expected_columns * expected_rows != manifest->frame_count ||
		    manifest->frame_map_object_key[0]) return false;
	} else {
		if (!archive_manifest_relative_locator_is_valid(
			    manifest->frame_map_object_key,
			    sizeof(manifest->frame_map_object_key)) ||
		    manifest->tiles_per_row || manifest->tiles_per_column) return false;
	}
	return true;
}

static bool archive_manifest_write_staged(const archive_repository_t* repository, const char* id,
		const char* suffix, const char* json, size_t json_size, char* error, size_t error_capacity) {
	char relative[256];
	char final_path[ARCHIVE_PATH_CAPACITY];
	char temp_path[ARCHIVE_PATH_CAPACITY];
	i32 relative_length = snprintf(relative, sizeof(relative), "staging/%s.%s.json", id, suffix);
	if (relative_length < 0 || (size_t)relative_length >= sizeof(relative) ||
	    !archive_repository_make_path(repository, relative, final_path, sizeof(final_path))) return false;
	i32 temp_length = snprintf(temp_path, sizeof(temp_path), "%s.tmp", final_path);
	if (temp_length < 0 || (size_t)temp_length >= sizeof(temp_path)) return false;
	FILE* existing = fopen(final_path, "rb");
	if (existing) {
		fclose(existing);
		archive_manifest_set_error(error, error_capacity, "staged manifest '%s' already exists", final_path);
		return false;
	}
	FILE* file = fopen(temp_path, "wb");
	if (!file) {
		archive_manifest_set_error(error, error_capacity, "could not create staged manifest '%s'", temp_path);
		return false;
	}
	bool failed = fwrite(json, 1, json_size, file) != json_size;
	if (fclose(file) != 0) failed = true;
	if (!failed && rename(temp_path, final_path) != 0) failed = true;
	if (failed) {
		remove(temp_path);
		archive_manifest_set_error(error, error_capacity, "could not commit staged manifest '%s'", final_path);
		return false;
	}
	return true;
}

bool archive_manifest_stage_source_bundle(const archive_repository_t* repository,
		const archive_source_bundle_manifest_t* manifest, char* error, size_t error_capacity) {
	if (!repository || !archive_source_bundle_manifest_is_valid(manifest)) {
		archive_manifest_set_error(error, error_capacity, "source-bundle manifest is invalid");
		return false;
	}
	char json[2048];
	i32 length = snprintf(json, sizeof(json),
		"{\n  \"kind\": \"source-bundle\",\n  \"schemaVersion\": %d,\n"
		"  \"state\": \"staged\",\n  \"bundleId\": \"%s\",\n"
		"  \"sourceFormat\": \"%s\",\n  \"bundleDigestSha256\": \"%s\",\n"
		"  \"adapterName\": \"%s\",\n  \"adapterVersion\": \"%s\",\n"
		"  \"objectCount\": %u,\n  \"totalBytes\": %llu\n}\n",
		ARCHIVE_MANIFEST_SCHEMA_VERSION, manifest->bundle_id,
		archive_source_format_string(manifest->source_format), manifest->bundle_digest_sha256,
		manifest->adapter_name, manifest->adapter_version, manifest->object_count,
		(unsigned long long)manifest->total_bytes);
	if (length < 0 || (size_t)length >= sizeof(json)) return false;
	return archive_manifest_write_staged(repository, manifest->bundle_id, "source", json,
			(size_t)length, error, error_capacity);
}

bool archive_manifest_stage_virtual_instance(const archive_repository_t* repository,
		const archive_virtual_instance_manifest_t* manifest, char* error, size_t error_capacity) {
	if (!repository || !archive_virtual_instance_manifest_is_valid(manifest)) {
		archive_manifest_set_error(error, error_capacity, "virtual-instance manifest is invalid");
		return false;
	}
	char json[6144];
	char icc_json[96];
	if (manifest->icc_profile_digest_sha256[0]) {
		snprintf(icc_json, sizeof(icc_json), "\"%s\"",
		         manifest->icc_profile_digest_sha256);
	} else {
		snprintf(icc_json, sizeof(icc_json), "null");
	}
	char frame_map_json[300];
	if (manifest->frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT) {
		snprintf(frame_map_json, sizeof(frame_map_json), "\"%s\"",
		         manifest->frame_map_object_key);
	} else {
		snprintf(frame_map_json, sizeof(frame_map_json), "null");
	}
	char optical_path_json[40];
	if (manifest->default_optical_path_identifier[0]) {
		snprintf(optical_path_json, sizeof(optical_path_json), "\"%s\"",
		         manifest->default_optical_path_identifier);
	} else {
		snprintf(optical_path_json, sizeof(optical_path_json), "null");
	}
	char focal_offset_json[48];
	if (manifest->has_default_focal_plane_z_offset) {
		snprintf(focal_offset_json, sizeof(focal_offset_json), "%g",
		         manifest->default_focal_plane_z_offset_um);
	} else {
		snprintf(focal_offset_json, sizeof(focal_offset_json), "null");
	}
	char dimension_map_json[300];
	if (manifest->frame_dimension_map_object_key[0]) {
		snprintf(dimension_map_json, sizeof(dimension_map_json), "\"%s\"",
		         manifest->frame_dimension_map_object_key);
	} else {
		snprintf(dimension_map_json, sizeof(dimension_map_json), "null");
	}
	char origin_json[128];
	if (manifest->has_slide_coordinate_origin) {
		snprintf(origin_json, sizeof(origin_json), "[%g, %g, %g]",
		         manifest->slide_coordinate_origin_x_mm,
		         manifest->slide_coordinate_origin_y_mm,
		         manifest->slide_coordinate_origin_z_mm);
	} else {
		snprintf(origin_json, sizeof(origin_json), "null");
	}
	char orientation_json[192];
	if (manifest->has_image_orientation_slide) {
		snprintf(orientation_json, sizeof(orientation_json),
		         "[%g, %g, %g, %g, %g, %g]",
		         manifest->image_orientation_slide[0],
		         manifest->image_orientation_slide[1],
		         manifest->image_orientation_slide[2],
		         manifest->image_orientation_slide[3],
		         manifest->image_orientation_slide[4],
		         manifest->image_orientation_slide[5]);
	} else {
		snprintf(orientation_json, sizeof(orientation_json), "null");
	}
	i32 length = snprintf(json, sizeof(json),
		"{\n  \"kind\": \"virtual-dicom-instance\",\n  \"schemaVersion\": %d,\n"
		"  \"state\": \"staged\",\n  \"representationId\": \"%s\",\n"
		"  \"bundleId\": \"%s\",\n  \"representationVersion\": %u,\n"
		"  \"studyInstanceUid\": \"%s\",\n  \"seriesInstanceUid\": \"%s\",\n"
		"  \"sopInstanceUid\": \"%s\",\n  \"sopClassUid\": \"%s\",\n"
		"  \"transferSyntaxUid\": \"%s\",\n  \"imageFlavor\": \"%s\",\n"
		"  \"photometricInterpretation\": \"%s\",\n  \"instanceNumber\": %u,\n"
		"  \"frameCount\": %u,\n  \"rows\": %u,\n  \"columns\": %u,\n"
		"  \"totalPixelMatrixRows\": %u,\n  \"totalPixelMatrixColumns\": %u,\n"
		"  \"provider\": {\"kind\": \"%s\", \"locator\": \"%s\"},\n"
		"  \"representationSource\": \"%s\",\n"
		"  \"frameGeometry\": {\"mapKind\": \"%s\", "
		"\"frameMapObjectKey\": %s, \"tilesPerRow\": %u, \"tilesPerColumn\": %u},\n"
		"  \"dimensions\": {\"opticalPathCount\": %u, \"focalPlaneCount\": %u, "
		"\"defaultOpticalPathIdentifier\": %s, \"defaultFocalPlaneZOffsetUm\": %s, "
		"\"frameDimensionMapObjectKey\": %s},\n"
		"  \"pixelSpacingMm\": [%g, %g],\n"
		"  \"slideCoordinateSystem\": {\"originMm\": %s, "
		"\"imageOrientationSlide\": %s},\n"
		"  \"iccProfileDigestSha256\": %s\n}\n",
		ARCHIVE_MANIFEST_SCHEMA_VERSION, manifest->representation_id, manifest->bundle_id,
		manifest->representation_version, manifest->study_instance_uid, manifest->series_instance_uid,
		manifest->sop_instance_uid, manifest->sop_class_uid, manifest->transfer_syntax_uid,
		manifest->image_flavor, manifest->photometric_interpretation, manifest->instance_number,
		manifest->frame_count, manifest->rows, manifest->columns,
		manifest->total_pixel_matrix_rows, manifest->total_pixel_matrix_columns,
		archive_manifest_provider_kind_string(manifest->provider_kind),
		manifest->provider_locator,
		archive_manifest_representation_source_string(manifest->representation_source),
		archive_manifest_frame_map_kind_string(manifest->frame_map_kind),
		frame_map_json,
		manifest->tiles_per_row, manifest->tiles_per_column,
		manifest->optical_path_count, manifest->focal_plane_count,
		optical_path_json, focal_offset_json, dimension_map_json,
		manifest->pixel_spacing_row_mm, manifest->pixel_spacing_column_mm,
		origin_json, orientation_json,
		icc_json);
	if (length < 0 || (size_t)length >= sizeof(json)) return false;
	return archive_manifest_write_staged(repository, manifest->representation_id,
			"representation", json, (size_t)length, error, error_capacity);
}
