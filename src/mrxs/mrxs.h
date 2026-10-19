/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2025  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "platform.h"
#include "source_fs.h"

enum mrxs_section_enum {
    MRXS_SECTION_UNKNOWN = 0,
    MRXS_SECTION_GENERAL,
    MRXS_SECTION_HIERARCHICAL,
    MRXS_SECTION_DATAFILE,
    MRXS_SECTION_LAYER_N_LEVEL_N_SECTION,
    MRXS_SECTION_LAYER_N_SECTION,
    MRXS_SECTION_NONHIERLAYER_N_LEVEL_N_SECTION,
    MRXS_SECTION_NONHIERLAYER_N_SECTION,
};

enum mrxs_hier_enum {
    MRXS_HIER_UNKNOWN = 0,
    MRXS_HIER_SLIDE_ZOOM_LEVEL,
    MRXS_HIER_SLIDE_FILTER_LEVEL,
    MRXS_HIER_MICROSCOPE_FOCUS_LEVEL,
    MRXS_HIER_SCAN_INFO_LAYER,
};

enum mrxs_nonhier_enum {
    MRXS_NONHIER_UNKNOWN = 0,
    MRXS_NONHIER_SCAN_DATA_LAYER,
    MRXS_NONHIER_STITCHING_LAYER,
    MRXS_NONHIER_STITCHING_INTENSITY_LAYER,
    MRXS_NONHIER_VIMSLIDE_HISTOGRAM_DATA,
};

enum mrxs_hier_val_enum {
    MRXS_HIER_VAL_UNKNOWN = 0,
    MRXS_HIER_VAL_ZOOMLEVEL,
};

enum mrxs_nonhier_val_enum {
	MRXS_NONHIER_VAL_UNKNOWN = 0,
    MRXS_NONHIER_VAL_SCANDATALAYER_SCANMAP,
    MRXS_NONHIER_VAL_SCANDATALAYER_XMLINFOHEADER,
    MRXS_NONHIER_VAL_SCANDATALAYER_SLIDETHUMBNAIL,
    MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEBARCODE,
    MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEPREVIEW,
    MRXS_NONHIER_VAL_SCANDATALAYER_STAGEPOSITIONMAP,
    MRXS_NONHIER_VAL_SCANDATALAYER_EMPTY,
    MRXS_NONHIER_VAL_PROFILEXMLHEADER,
    MRXS_NONHIER_VAL_PROFILEXML,
    MRXS_NONHIER_VAL_SCANNEDFOVSMAP,
    MRXS_NONHIER_VAL_DATALEVEL_V1_0,
    MRXS_NONHIER_VAL_STITCHING_INTENSITY_LEVEL,
    MRXS_NONHIER_VAL_VIMSLIDE_HISTOGRAM_DATA_DEFAULT,
};

enum mrxs_image_format_enum {
	MRXS_IMAGE_FORMAT_UNKNOWN = 0,
	MRXS_IMAGE_FORMAT_JPEG,
	MRXS_IMAGE_FORMAT_PNG,
	MRXS_IMAGE_FORMAT_BMP,
};

#pragma pack(push, 1)
typedef struct mrxs_hier_entry_t {
	u32 image;
	u32 offset;
	u32 length;
	u32 file;
} mrxs_hier_entry_t;

typedef struct mrxs_nonhier_entry_t {
	u32 padding1;
	u32 padding2;
	u32 offset;
	u32 length;
	u32 file;
} mrxs_nonhier_entry_t;
#pragma pack(pop)

typedef struct mrxs_tile_t {
	mrxs_hier_entry_t hier_entry;
} mrxs_tile_t;

typedef struct mrxs_level_t {
//    i32 level;
    const char* section_name;
    i32 hier_val_index;
	mrxs_tile_t* tiles;
	i32 width_in_tiles;
	i32 height_in_tiles;
	i32 tile_width;
	i32 tile_height;
    float overlap_x;
    float overlap_y;
	float um_per_pixel_x;
	float um_per_pixel_y;
	u32 image_fill_color_bgr;
	enum mrxs_image_format_enum image_format;
} mrxs_level_t;

typedef struct mrxs_hier_val_t {
    const char* name;
	const char* section;
	enum mrxs_hier_val_enum type;
	i32 index;
	float offset_um;
	bool offset_known;
	bool is_ini_section_parsed;
} mrxs_hier_val_t;

typedef struct mrxs_nonhier_val_t {
	const char* name;
	const char* section;
	enum mrxs_nonhier_val_enum type;
	i32 index;
    i32 imagenumber_x;
    i32 imagenumber_y;
	bool is_ini_section_parsed;
} mrxs_nonhier_val_t;

typedef struct mrxs_hier_t {
	enum mrxs_hier_enum name;
	i32 val_count;
	i32 default_value;
	mrxs_hier_val_t* val;
	const char* section;
	bool is_ini_section_parsed;
} mrxs_hier_t;

typedef struct mrxs_nonhier_t {
    enum mrxs_nonhier_enum name;
	i32 val_count;
	mrxs_nonhier_val_t* val;
	const char* section;
	bool is_ini_section_parsed;
} mrxs_nonhier_t;

#pragma pack(push, 1)
typedef struct mrxs_slide_position_t {
    u8 flag;
    i32 x;
    i32 y;
} mrxs_slide_position_t;
#pragma pack(pop)

typedef struct mrxs_simple_image_t {
    enum mrxs_image_format_enum format;
    i32 width;
    i32 height;
    bool flip;
    mrxs_nonhier_entry_t entry;
} mrxs_simple_image_t;

typedef struct mrxs_camera_t {
    bool present;
    i32 raw_x;
    i32 raw_y;
    i32 x;
    i32 y;
} mrxs_camera_t;

typedef struct mrxs_plane_t {
	const char* name;
	float offset_um;
	bool offset_known;
	bool is_extended_focus;
	i32 hier_value;
	/* Plane zero borrows levels[].tiles; other planes own these arrays. */
	mrxs_tile_t* tiles[16];
	bool level_available[16];
} mrxs_plane_t;

typedef struct mrxs_decoded_tile_cache_entry_t {
	i32 plane;
    i32 level;
    i32 stored_tile_index;
    u8* pixels;
    i32 refcount;
    u64 last_used;
} mrxs_decoded_tile_cache_entry_t;

typedef struct mrxs_t {
    memrw_t string_pool; // NOTE: need destroy
    const char* index_dat_filename;
    const char** dat_filenames; // NOTE: need free
	source_file_t* dat_files; // Borrowed views owned by source.
	source_fs_t* source; // Closed after tile workers and views are released.
    i32 dat_count;
    i32 hier_count;
    i32 nonhier_count;
    mrxs_hier_t* hier; // NOTE: need free
    mrxs_nonhier_t* nonhier; // NOTE: need free
    i32 slide_zoom_level_hier_index;
	i32 extended_focus_value;
	i32 zero_focus_value;
	i32 plane_count;
	i32 zero_plane;
	mrxs_plane_t* planes;
	volatile i32 extra_tile_warning_count;
	volatile i32 missing_tile_warning_count;
    i32 base_width_in_tiles;
    i32 base_height_in_tiles;
    i32 base_width_in_pixels;
    i32 base_height_in_pixels;
    u8 slide_version_major;
    u8 slide_version_minor;
    i32 camera_image_divisions_per_slide;
    mrxs_nonhier_entry_t stitching_intensity_layer_entry;
    i32 camera_position_count;
    mrxs_slide_position_t* camera_positions;
    i32 camera_grid_width;
    i32 camera_grid_height;
    i32 camera_width;
    i32 camera_height;
    i32 camera_origin_x;
    i32 camera_origin_y;
    i32 max_aligned_overlap_level;
    mrxs_camera_t* cameras;
    mrxs_simple_image_t scanmap_image;
    mrxs_simple_image_t stageposmap_image;
    mrxs_simple_image_t thumbnail_image;
    mrxs_simple_image_t barcode_image;
    platform_mutex_t decoded_tile_cache_mutex;
    bool decoded_tile_cache_mutex_initialized;
    mrxs_decoded_tile_cache_entry_t* decoded_tile_cache_entries;
    i32 decoded_tile_cache_count;
    i32 decoded_tile_cache_capacity;
    u64 decoded_tile_cache_tick;
	i32 level_count;
	mrxs_level_t levels[16];
	i32 tile_width;
	i32 tile_height;
	float mpp_x;
	float mpp_y;
	bool is_mpp_known;
	bool has_overlapping_tiles;
	bool is_fluorescence;
	thread_pool_t* work_submission_pool;
	volatile i32 refcount;
	bool is_valid;
} mrxs_t;

// forward declarations so we don't need to include viewer.h
typedef struct file_info_t file_info_t;
typedef struct directory_info_t directory_info_t;

/* Takes ownership of source, also on failure; release through mrxs_destroy(). */
bool mrxs_open_from_source(mrxs_t* mrxs, source_fs_t* source, const char* directory);
bool mrxs_open_from_zip(mrxs_t* mrxs, const char* filename, char* error, size_t error_capacity);
bool mrxs_open_from_directory(mrxs_t* mrxs, file_info_t* file, directory_info_t* directory);
void mrxs_init_runtime_cache(mrxs_t* mrxs);
u8* mrxs_decode_simple_image_to_rgba(mrxs_t* mrxs, mrxs_simple_image_t* image);
u8* mrxs_decode_tile_to_bgra(mrxs_t* mrxs, i32 plane, i32 level, i32 tile_index);
bool mrxs_tile_is_empty(mrxs_t* mrxs, i32 plane, i32 level, i32 tile_index);
void mrxs_set_thread_pool(mrxs_t* mrxs, thread_pool_t* thread_pool);
void mrxs_destroy(mrxs_t* mrxs);

#ifdef __cplusplus
}
#endif
