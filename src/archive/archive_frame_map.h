/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHIVE_FRAME_MAP_SCHEMA_VERSION 1
#define ARCHIVE_FRAME_MAP_HEADER_SIZE 64
#define ARCHIVE_FRAME_MAP_ENTRY_SIZE 32
#define ARCHIVE_FRAME_MAP_CHUNK_ENTRY_COUNT 4096

typedef struct dicom_instance_t dicom_instance_t;

typedef enum archive_frame_map_entry_flags_t {
	ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN = 1 << 0,
	ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN = 1 << 1,
	ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN = 1 << 2,
} archive_frame_map_entry_flags_t;

typedef struct archive_frame_map_entry_t {
	u32 row_position;
	u32 column_position;
	float x_offset_mm;
	float y_offset_mm;
	float z_offset_um;
	u32 optical_path_index;
	u32 focal_plane_index;
	u32 flags;
} archive_frame_map_entry_t;

typedef struct archive_frame_map_t {
	u32 frame_count;
	u32 rows;
	u32 columns;
	u32 total_pixel_matrix_rows;
	u32 total_pixel_matrix_columns;
	u32 optical_path_count;
	u32 focal_plane_count;
	archive_frame_map_entry_t* entries;
} archive_frame_map_t;

/*
 * Output maps must be zero-initialized before their first build/parse and destroyed
 * when no longer needed. The serialized format is fixed-size little-endian records;
 * record N describes one-based DICOM frame number N+1.
 */
bool archive_frame_map_build_part10(archive_frame_map_t* map,
		const dicom_instance_t* instance, char* error, size_t error_capacity);
bool archive_frame_map_serialize(const archive_frame_map_t* map, u8* dest,
		size_t capacity, size_t* encoded_size, char* error, size_t error_capacity);
bool archive_frame_map_parse(archive_frame_map_t* map, const u8* data, size_t size,
		char* error, size_t error_capacity);
void archive_frame_map_destroy(archive_frame_map_t* map);

#ifdef __cplusplus
}
#endif
