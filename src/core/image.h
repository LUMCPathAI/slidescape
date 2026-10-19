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

#include "common.h"
#include "mathutils.h"
#include "renderer.h"

// backends
#include "mrxs.h"
#include "openslide_api.h"
#include "tiff.h"
#include "isyntax.h"
#include "libisyntax.h"
#include "dicom.h"
#include "dicomweb.h"
#include "slide_score.h"
#include "ssi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BYTES_PER_PIXEL
#define BYTES_PER_PIXEL 4
#endif

typedef enum pixel_format_enum {
    PIXEL_FORMAT_UNDEFINED = 0,
    PIXEL_FORMAT_U8_BGRA = 1,
    PIXEL_FORMAT_U8_RGBA = 2,
    PIXEL_FORMAT_F32_Y = 3,
} pixel_format_enum;

#define WSI_TILE_DIM 512

typedef struct image_t image_t;
typedef struct tile_cache_t tile_cache_t;

typedef struct {
    i64 width;
    i64 height;
    i64 width_in_tiles;
    i64 height_in_tiles;
    u32 tile_width;
    u32 tile_height;
    i32 tile_count;
    float um_per_pixel_x;
    float um_per_pixel_y;
    float x_tile_side_in_um;
    float y_tile_side_in_um;
    i32 downsample_level;
    float downsample_factor;
} wsi_level_t;

#define WSI_MAX_LEVELS 16

typedef struct wsi_t {
    i64 width;
    i64 height;
    i32 level_count;
    openslide_t* osr;
    const char* barcode;
    float mpp_x;
    float mpp_y;
    bool is_mpp_known;
    i32 max_downsample_level;
    u32 tile_width;
    u32 tile_height;

    wsi_level_t levels[WSI_MAX_LEVELS];
} wsi_t;





#define IMAGE_PYRAMID_MAX_LEVELS 16

typedef enum {
    IMAGE_TYPE_NONE,
//	IMAGE_TYPE_SIMPLE,
//	IMAGE_TYPE_TIFF,
    IMAGE_TYPE_WSI,
} image_type_enum;

typedef enum {
    IMAGE_BACKEND_NONE,
    IMAGE_BACKEND_STBI,
    IMAGE_BACKEND_TIFF,
    IMAGE_BACKEND_OPENSLIDE,
    IMAGE_BACKEND_ISYNTAX,
    IMAGE_BACKEND_DICOM,
    IMAGE_BACKEND_MRXS,
    IMAGE_BACKEND_SLIDE_SCORE,
    IMAGE_BACKEND_DICOMWEB,
    IMAGE_BACKEND_SSI,
} image_backend_enum;

typedef struct tile_t {
    u32 tile_index;
    i32 tile_x;
    i32 tile_y;
    bool8 is_empty;
    i64 time_last_drawn;
} tile_t;

typedef struct cached_tile_t {
    i32 tile_width;
    u8* pixels;
} cached_tile_t;

typedef struct {
    i64 width_in_pixels;
    i64 height_in_pixels;
    tile_t* tiles;
    u64 tile_count;
    u32 width_in_tiles;
    u32 height_in_tiles;
    u32 tile_width;
    u32 tile_height;
    float x_tile_side_in_um;
    float y_tile_side_in_um;
    float um_per_pixel_x;
    float um_per_pixel_y;
    float downsample_factor;
    v2f origin_offset;
    i32 pyramid_image_index;
    bool exists;
	u64 missing_planes; // Zero for single-plane backends; bit p means this level is absent in plane p.
    bool needs_indexing; //TODO: implement
    bool indexing_job_submitted;
} level_image_t;

typedef struct simple_image_t {
    i32 channels_in_file;
    i32 channels;
    i32 width;
    i32 height;
    u8* pixels;
    renderer_texture_handle_t texture;
    float mpp;
    v2f world_pos;
    bool is_valid;
} simple_image_t;

typedef struct image_t {
    char name[512];
    char directory[512];
    image_type_enum type;
    image_backend_enum backend;
    rect2f clip_rects[128];
    i32 clip_rect_count;
    bool is_freshly_loaded; // TODO: remove or refactor, is this still needed?
    bool is_local; // i.e. not remote (accessed over network using client/server interface)
    bool is_valid;
    bool is_deleted; // TODO: prevent delay due to refcount > 0 when closing WSI (garbage collect off the main thread)
    bool is_enabled;
    bool is_overlay;
	float initial_rotation;
    bool is_background_black; // if false: background = white (for brightfield)
    // Set type/backend before opening in this storage, so partial loads can be destroyed.
    // Never move an opened backend or an image with initialized runtime state.
    union {
        simple_image_t simple;
        tiff_t tiff;
        isyntax_t isyntax;
        wsi_t openslide_wsi;
        dicom_series_t dicom;
        mrxs_t mrxs;
        slide_score_remote_image_t slide_score;
        dicomweb_remote_image_t dicomweb;
		ssi_remote_image_t ssi;
    };
    /* Plane zero is the default image (extended focus for MRXS stacks). */
    i32 plane_count;
    i32 selected_plane;
    i32 zstack_plane;
    i32 plane_approach_direction; // -1: prefer lower offsets on equal-distance fallback.
    i32 level_count;
    u32 tile_width;
    u32 tile_height;
    level_image_t level_images[IMAGE_PYRAMID_MAX_LEVELS];
    float mpp_x;
    float mpp_y;
    bool is_mpp_known;
    i64 width_in_pixels;
    float width_in_um;
    i64 height_in_pixels;
    float height_in_um;
    v2f origin_offset;
    simple_image_t macro_image;
    simple_image_t label_image;
    i32 resource_id;
	tile_cache_t* tile_cache;
	volatile i32 refcount;
	platform_mutex_t lock;
	bool lock_initialized;
} image_t;


static inline tile_t* get_tile(level_image_t* image_level, i32 tile_x, i32 tile_y) {
	ASSERT(tile_x >= 0 && tile_x < image_level->width_in_tiles);
	ASSERT(tile_y >= 0 && tile_y < image_level->height_in_tiles);
	i32 tile_index = tile_y * image_level->width_in_tiles + tile_x;
	ASSERT(tile_index >= 0 && tile_index < image_level->tile_count);
	tile_t* result = image_level->tiles + tile_index;
	return result;
}

static inline tile_t* get_tile_from_tile_index(image_t* image, i32 scale, i32 tile_index) {
	ASSERT(image);
	ASSERT(scale < image->level_count);
	level_image_t* level_image = image->level_images + scale;
	tile_t* tile = level_image->tiles + tile_index;
	return tile;
}

float f32_rgb_to_f32_y(float R, float G, float B);
void image_convert_u8_rgba_to_f32_y(u8* src, float* dest, i32 w, i32 h, i32 components);
void image_convert_u8_bgra_to_f32_y(u8* src, float* dest, i32 w, i32 h, i32 components);
const char* get_image_backend_name(image_t* image);
const char* get_image_descriptive_type_name(image_t* image);
// These helpers derive image metadata from a backend already opened in image storage.
// Initialize image runtime before submitting any image background jobs.
bool init_image_from_tiff(image_t* image, bool is_overlay, image_t* parent_image);
u8* decode_associated_image_from_isyntax(isyntax_t* isyntax, isyntax_image_t* image);
bool init_image_from_isyntax(image_t* image, bool is_overlay);
bool init_image_from_dicom(image_t* image, bool is_overlay);
bool init_image_from_dicomweb(image_t* image, bool is_overlay);
bool init_image_from_ssi(image_t* image, bool is_overlay);
bool init_image_from_mrxs(image_t* image, bool is_overlay);
bool init_image_from_stbi(image_t* image, bool is_overlay);
void init_image_from_openslide(image_t* image, bool is_overlay);
void unload_openslide_wsi(wsi_t* wsi);
bool image_initialize_runtime(image_t* image);
void image_set_ssi_close_callback(void (*callback)(ssi_remote_image_t* remote));
bool image_read_region(image_t* image, i32 plane, i32 level, i32 x, i32 y, i32 w, i32 h, void* dest, pixel_format_enum desired_pixel_format);
void begin_level_image_indexing(image_t* image, level_image_t* level_image, i32 scale);
void image_select_plane(image_t* image, i32 plane);
void image_destroy(image_t* image);

#ifdef __cplusplus
}
#endif
