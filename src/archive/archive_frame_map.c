/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_frame_map.h"

#include "dicom.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>

static const u8 archive_frame_map_magic[8] = {'S', 'S', 'F', 'M', 'A', 'P', '1', '\0'};
STATIC_ASSERT(sizeof(float) == 4);

static void archive_frame_map_set_error(char* error, size_t capacity,
		const char* format, ...) {
	if (!error || !capacity) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static void archive_frame_map_write_u32(u8* dest, u32 value) {
	dest[0] = (u8)value;
	dest[1] = (u8)(value >> 8);
	dest[2] = (u8)(value >> 16);
	dest[3] = (u8)(value >> 24);
}

static u32 archive_frame_map_read_u32(const u8* source) {
	return (u32)source[0] | ((u32)source[1] << 8) |
	       ((u32)source[2] << 16) | ((u32)source[3] << 24);
}

static void archive_frame_map_write_float(u8* dest, float value) {
	u32 bits = 0;
	memcpy(&bits, &value, sizeof(bits));
	archive_frame_map_write_u32(dest, bits);
}

static float archive_frame_map_read_float(const u8* source) {
	u32 bits = archive_frame_map_read_u32(source);
	float value = 0.0f;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static bool archive_frame_map_metadata_is_valid(const archive_frame_map_t* map) {
	if (!map || !map->entries || !map->frame_count || !map->rows || !map->columns ||
	    !map->total_pixel_matrix_rows || !map->total_pixel_matrix_columns ||
	    !map->optical_path_count || !map->focal_plane_count) return false;
	for (u32 frame = 0; frame < map->frame_count; ++frame) {
		const archive_frame_map_entry_t* entry = map->entries + frame;
		if (!entry->row_position || !entry->column_position ||
		    entry->row_position > map->total_pixel_matrix_rows ||
		    entry->column_position > map->total_pixel_matrix_columns ||
		    entry->optical_path_index >= map->optical_path_count ||
		    entry->focal_plane_index >= map->focal_plane_count ||
		    (entry->flags & ~(ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN |
		                      ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN |
		                      ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN))) return false;
		if ((entry->flags & ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN) &&
		    (!isfinite(entry->x_offset_mm) || !isfinite(entry->y_offset_mm))) return false;
		if ((entry->flags & ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN) &&
		    !isfinite(entry->z_offset_um)) return false;
	}
	return true;
}

static i32 archive_frame_map_find_optical_path(const dicom_instance_t* instance,
		const char* identifier) {
	for (i32 i = 0; i < arrlen(instance->optical_paths); ++i) {
		if (strcmp(instance->optical_paths[i].OpticalPathIdentifier.value,
		           identifier) == 0) return i;
	}
	return -1;
}

static i32 archive_frame_map_compare_float(const void* lhs, const void* rhs) {
	float a = *(const float*)lhs;
	float b = *(const float*)rhs;
	return a < b ? -1 : a > b ? 1 : 0;
}

static i32 archive_frame_map_find_float(const float* values, i32 count, float value) {
	for (i32 i = 0; i < count; ++i) {
		if (values[i] == value) return i;
	}
	return -1;
}

static bool archive_frame_map_build_tiled_full(archive_frame_map_t* map,
		const dicom_instance_t* instance, char* error, size_t error_capacity) {
	u64 tiles_per_row =
		((u64)instance->total_pixel_matrix_columns + instance->columns - 1) /
		instance->columns;
	u64 tiles_per_column =
		((u64)instance->total_pixel_matrix_rows + instance->rows - 1) /
		instance->rows;
	u64 spatial_count = tiles_per_row * tiles_per_column;
	u64 total_frame_count = spatial_count * map->focal_plane_count *
		map->optical_path_count;
	u64 first_frame = instance->concatenation_frame_offset_number;
	if (!spatial_count || total_frame_count > UINT32_MAX ||
	    first_frame + map->frame_count > total_frame_count ||
	    (!instance->concatenation_uid.len && map->frame_count != total_frame_count)) {
		archive_frame_map_set_error(error, error_capacity,
			"TILED_FULL dimensions do not match the instance frame range");
		return false;
	}

	bool can_calculate_xy =
		instance->is_total_pixel_matrix_origin_known &&
		instance->is_image_orientation_slide_known &&
		instance->pixel_spacing.x > 0.0f && instance->pixel_spacing.y > 0.0f;
	for (u32 local_frame = 0; local_frame < map->frame_count; ++local_frame) {
		u64 global_frame = first_frame + local_frame;
		u64 spatial_index = global_frame % spatial_count;
		u64 dimension_index = global_frame / spatial_count;
		archive_frame_map_entry_t* entry = map->entries + local_frame;
		entry->column_position =
			(u32)((spatial_index % tiles_per_row) * instance->columns + 1);
		entry->row_position =
			(u32)((spatial_index / tiles_per_row) * instance->rows + 1);
		entry->focal_plane_index =
			(u32)(dimension_index % map->focal_plane_count);
		entry->optical_path_index =
			(u32)(dimension_index / map->focal_plane_count);
		entry->flags = ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN;
		if (can_calculate_xy) {
			float row_distance_mm =
				(float)(entry->row_position - 1) * instance->pixel_spacing.x;
			float column_distance_mm =
				(float)(entry->column_position - 1) * instance->pixel_spacing.y;
			entry->x_offset_mm = instance->total_pixel_matrix_origin_x_mm +
				column_distance_mm * instance->image_orientation_slide[0] +
				row_distance_mm * instance->image_orientation_slide[3];
			entry->y_offset_mm = instance->total_pixel_matrix_origin_y_mm +
				column_distance_mm * instance->image_orientation_slide[1] +
				row_distance_mm * instance->image_orientation_slide[4];
			entry->flags |= ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN;
		}
	}
	return true;
}

static bool archive_frame_map_build_tiled_sparse(archive_frame_map_t* map,
		const dicom_instance_t* instance, char* error, size_t error_capacity) {
	if (arrlen(instance->per_frame_plane_position_slide) != instance->number_of_frames) {
		archive_frame_map_set_error(error, error_capacity,
			"sparse instance does not have one slide-plane position per frame");
		return false;
	}
	if (map->optical_path_count > 1 &&
	    arrlen(instance->per_frame_optical_path_identifiers) != instance->number_of_frames) {
		archive_frame_map_set_error(error, error_capacity,
			"sparse multi-path instance does not identify an optical path per frame");
		return false;
	}

	float* focal_offsets = NULL;
	for (i32 frame = 0; frame < arrlen(instance->per_frame_plane_position_slide); ++frame) {
		float offset = instance->per_frame_plane_position_slide[frame].
			z_offset_in_slide_coordinate_system;
		if (!isfinite(offset)) {
			arrfree(focal_offsets);
			archive_frame_map_set_error(error, error_capacity,
				"sparse frame %u has an invalid focal-plane offset", frame + 1);
			return false;
		}
		if (archive_frame_map_find_float(focal_offsets, arrlen(focal_offsets), offset) < 0)
			arrput(focal_offsets, offset);
	}
	qsort(focal_offsets, (size_t)arrlen(focal_offsets), sizeof(*focal_offsets),
	      archive_frame_map_compare_float);
	if (!instance->total_pixel_matrix_focal_planes)
		map->focal_plane_count = (u32)arrlen(focal_offsets);
	if ((u32)arrlen(focal_offsets) != map->focal_plane_count) {
		arrfree(focal_offsets);
		archive_frame_map_set_error(error, error_capacity,
			"sparse focal-plane metadata is inconsistent");
		return false;
	}

	for (u32 frame = 0; frame < map->frame_count; ++frame) {
		const dicom_plane_position_slide_t* position =
			instance->per_frame_plane_position_slide + frame;
		archive_frame_map_entry_t* entry = map->entries + frame;
		if (position->row_position_in_total_image_pixel_matrix <= 0 ||
		    position->column_position_in_total_image_pixel_matrix <= 0) {
			arrfree(focal_offsets);
			archive_frame_map_set_error(error, error_capacity,
				"sparse frame %u has an invalid tile position", frame + 1);
			return false;
		}
		entry->row_position =
			(u32)position->row_position_in_total_image_pixel_matrix;
		entry->column_position =
			(u32)position->column_position_in_total_image_pixel_matrix;
		entry->x_offset_mm = position->offset_in_slide_coordinate_system.x;
		entry->y_offset_mm = position->offset_in_slide_coordinate_system.y;
		entry->z_offset_um = position->z_offset_in_slide_coordinate_system;
		entry->focal_plane_index = (u32)archive_frame_map_find_float(
			focal_offsets, arrlen(focal_offsets), entry->z_offset_um);
		entry->flags = ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN |
			ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN |
			ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN;
		if (map->optical_path_count == 1) {
			entry->optical_path_index = 0;
		} else {
			const dicom_sh_t* identifier =
				instance->per_frame_optical_path_identifiers + frame;
			i32 optical_path_index = archive_frame_map_find_optical_path(
				instance, identifier->value);
			if (!identifier->present || optical_path_index < 0) {
				arrfree(focal_offsets);
				archive_frame_map_set_error(error, error_capacity,
					"sparse frame %u references an unknown optical path", frame + 1);
				return false;
			}
			entry->optical_path_index = (u32)optical_path_index;
		}
	}
	arrfree(focal_offsets);
	return true;
}

bool archive_frame_map_build_part10(archive_frame_map_t* map,
		const dicom_instance_t* instance, char* error, size_t error_capacity) {
	if (error && error_capacity) error[0] = '\0';
	if (!map || !instance || instance->number_of_frames <= 0 ||
	    instance->number_of_frames > UINT32_MAX || !instance->rows || !instance->columns ||
	    !instance->total_pixel_matrix_rows || !instance->total_pixel_matrix_columns ||
	    arrlen(instance->optical_paths) <= 0 ||
	    (instance->number_of_optical_paths &&
	     instance->number_of_optical_paths != (u32)arrlen(instance->optical_paths)) ||
	    (u64)instance->number_of_frames > SIZE_MAX / sizeof(*map->entries)) {
		archive_frame_map_set_error(error, error_capacity,
			"Part 10 frame-map input is incomplete");
		return false;
	}
	memset(map, 0, sizeof(*map));
	map->frame_count = (u32)instance->number_of_frames;
	map->rows = instance->rows;
	map->columns = instance->columns;
	map->total_pixel_matrix_rows = instance->total_pixel_matrix_rows;
	map->total_pixel_matrix_columns = instance->total_pixel_matrix_columns;
	map->optical_path_count = (u32)arrlen(instance->optical_paths);
	map->focal_plane_count = instance->total_pixel_matrix_focal_planes ?
		instance->total_pixel_matrix_focal_planes : 1;
	map->entries = calloc(map->frame_count, sizeof(*map->entries));
	if (!map->entries) {
		archive_frame_map_set_error(error, error_capacity,
			"could not allocate %u frame-map entries", map->frame_count);
		return false;
	}
	bool success = instance->is_tiled_full ?
		archive_frame_map_build_tiled_full(map, instance, error, error_capacity) :
		archive_frame_map_build_tiled_sparse(map, instance, error, error_capacity);
	if (!success || !archive_frame_map_metadata_is_valid(map)) {
		if (success) archive_frame_map_set_error(error, error_capacity,
			"generated frame map is invalid");
		archive_frame_map_destroy(map);
		return false;
	}
	return true;
}

static void archive_frame_map_encode_header(const archive_frame_map_t* map, u8* dest) {
	memset(dest, 0, ARCHIVE_FRAME_MAP_HEADER_SIZE);
	memcpy(dest, archive_frame_map_magic, sizeof(archive_frame_map_magic));
	archive_frame_map_write_u32(dest + 8, ARCHIVE_FRAME_MAP_SCHEMA_VERSION);
	archive_frame_map_write_u32(dest + 12, ARCHIVE_FRAME_MAP_HEADER_SIZE);
	archive_frame_map_write_u32(dest + 16, ARCHIVE_FRAME_MAP_ENTRY_SIZE);
	archive_frame_map_write_u32(dest + 20, ARCHIVE_FRAME_MAP_CHUNK_ENTRY_COUNT);
	archive_frame_map_write_u32(dest + 24, map->frame_count);
	archive_frame_map_write_u32(dest + 28, map->rows);
	archive_frame_map_write_u32(dest + 32, map->columns);
	archive_frame_map_write_u32(dest + 36, map->total_pixel_matrix_rows);
	archive_frame_map_write_u32(dest + 40, map->total_pixel_matrix_columns);
	archive_frame_map_write_u32(dest + 44, map->optical_path_count);
	archive_frame_map_write_u32(dest + 48, map->focal_plane_count);
}

static void archive_frame_map_encode_entry(const archive_frame_map_entry_t* entry, u8* dest) {
	memset(dest, 0, ARCHIVE_FRAME_MAP_ENTRY_SIZE);
	archive_frame_map_write_u32(dest, entry->row_position);
	archive_frame_map_write_u32(dest + 4, entry->column_position);
	archive_frame_map_write_float(dest + 8, entry->x_offset_mm);
	archive_frame_map_write_float(dest + 12, entry->y_offset_mm);
	archive_frame_map_write_float(dest + 16, entry->z_offset_um);
	archive_frame_map_write_u32(dest + 20, entry->optical_path_index);
	archive_frame_map_write_u32(dest + 24, entry->focal_plane_index);
	archive_frame_map_write_u32(dest + 28, entry->flags);
}

bool archive_frame_map_serialize(const archive_frame_map_t* map, u8* dest,
		size_t capacity, size_t* encoded_size, char* error, size_t error_capacity) {
	if (encoded_size) *encoded_size = 0;
	if (!archive_frame_map_metadata_is_valid(map)) {
		archive_frame_map_set_error(error, error_capacity, "frame map is invalid");
		return false;
	}
	u64 required = ARCHIVE_FRAME_MAP_HEADER_SIZE +
		(u64)map->frame_count * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
	if (required > SIZE_MAX || !dest || capacity < (size_t)required) {
		if (encoded_size && required <= SIZE_MAX) *encoded_size = (size_t)required;
		archive_frame_map_set_error(error, error_capacity,
			"frame-map output buffer is too small");
		return false;
	}
	archive_frame_map_encode_header(map, dest);
	for (u32 frame = 0; frame < map->frame_count; ++frame) {
		archive_frame_map_encode_entry(map->entries + frame,
			dest + ARCHIVE_FRAME_MAP_HEADER_SIZE +
			(size_t)frame * ARCHIVE_FRAME_MAP_ENTRY_SIZE);
	}
	if (encoded_size) *encoded_size = (size_t)required;
	return true;
}

bool archive_frame_map_parse(archive_frame_map_t* map, const u8* data, size_t size,
		char* error, size_t error_capacity) {
	if (!map || !data || size < ARCHIVE_FRAME_MAP_HEADER_SIZE ||
	    memcmp(data, archive_frame_map_magic, sizeof(archive_frame_map_magic)) != 0 ||
	    archive_frame_map_read_u32(data + 8) != ARCHIVE_FRAME_MAP_SCHEMA_VERSION ||
	    archive_frame_map_read_u32(data + 12) != ARCHIVE_FRAME_MAP_HEADER_SIZE ||
	    archive_frame_map_read_u32(data + 16) != ARCHIVE_FRAME_MAP_ENTRY_SIZE ||
	    archive_frame_map_read_u32(data + 20) != ARCHIVE_FRAME_MAP_CHUNK_ENTRY_COUNT) {
		archive_frame_map_set_error(error, error_capacity,
			"frame-map header is invalid or unsupported");
		return false;
	}
	archive_frame_map_t parsed = {};
	parsed.frame_count = archive_frame_map_read_u32(data + 24);
	parsed.rows = archive_frame_map_read_u32(data + 28);
	parsed.columns = archive_frame_map_read_u32(data + 32);
	parsed.total_pixel_matrix_rows = archive_frame_map_read_u32(data + 36);
	parsed.total_pixel_matrix_columns = archive_frame_map_read_u32(data + 40);
	parsed.optical_path_count = archive_frame_map_read_u32(data + 44);
	parsed.focal_plane_count = archive_frame_map_read_u32(data + 48);
	u64 required = ARCHIVE_FRAME_MAP_HEADER_SIZE +
		(u64)parsed.frame_count * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
	if (required != size || required > SIZE_MAX) {
		archive_frame_map_set_error(error, error_capacity,
			"frame-map length does not match its header");
		return false;
	}
	parsed.entries = calloc(parsed.frame_count, sizeof(*parsed.entries));
	if (!parsed.entries) {
		archive_frame_map_set_error(error, error_capacity,
			"could not allocate parsed frame map");
		return false;
	}
	for (u32 frame = 0; frame < parsed.frame_count; ++frame) {
		const u8* source = data + ARCHIVE_FRAME_MAP_HEADER_SIZE +
			(size_t)frame * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
		archive_frame_map_entry_t* entry = parsed.entries + frame;
		entry->row_position = archive_frame_map_read_u32(source);
		entry->column_position = archive_frame_map_read_u32(source + 4);
		entry->x_offset_mm = archive_frame_map_read_float(source + 8);
		entry->y_offset_mm = archive_frame_map_read_float(source + 12);
		entry->z_offset_um = archive_frame_map_read_float(source + 16);
		entry->optical_path_index = archive_frame_map_read_u32(source + 20);
		entry->focal_plane_index = archive_frame_map_read_u32(source + 24);
		entry->flags = archive_frame_map_read_u32(source + 28);
	}
	if (!archive_frame_map_metadata_is_valid(&parsed)) {
		archive_frame_map_destroy(&parsed);
		archive_frame_map_set_error(error, error_capacity,
			"frame-map contents are invalid");
		return false;
	}
	archive_frame_map_destroy(map);
	*map = parsed;
	return true;
}

void archive_frame_map_destroy(archive_frame_map_t* map) {
	if (!map) return;
	free(map->entries);
	memset(map, 0, sizeof(*map));
}
