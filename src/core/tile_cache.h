/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

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
#include "image.h"
#include "platform.h"

typedef enum tile_cache_request_state_enum {
	TILE_CACHE_UNREQUESTED,
	TILE_CACHE_QUEUED,
	TILE_CACHE_IN_FLIGHT,
	TILE_CACHE_READY,
	TILE_CACHE_FAILED,
} tile_cache_request_state_enum;

typedef enum tile_cache_demand_flags_enum {
	TILE_CACHE_DEMAND_VIEWER_VISIBLE = 1 << 0,
	TILE_CACHE_DEMAND_VIEWER_PREFETCH = 1 << 1,
	TILE_CACHE_DEMAND_READ_REGION = 1 << 2,
	TILE_CACHE_DEMAND_EXPORT = 1 << 3,
	TILE_CACHE_DEMAND_REGISTRATION = 1 << 4,
	TILE_CACHE_DEMAND_CPU_RESIDENCY = 1 << 5,
	TILE_CACHE_DEMAND_GPU_RESIDENCY = 1 << 6,
} tile_cache_demand_flags_enum;

typedef struct tile_cache_policy_t {
	i32 max_inflight_tiles;
	i32 max_submit_per_tick;
	i32 batch_size;
	bool8 discard_stale_before_read;
	bool8 discard_stale_before_decode;
	bool8 discard_stale_before_upload;
	bool8 latency_bound;
} tile_cache_policy_t;

typedef struct tile_cache_viewer_request_t {
	bounds2f camera_bounds;
	bounds2f crop_bounds;
	v2f camera_center;
	i32 zoom_level;
	i32 client_width;
	i32 client_height;
	bool8 is_cropped;
} tile_cache_viewer_request_t;

typedef struct tile_cache_result_t {
	u8* pixel_memory;
	i32 level;
	i32 tile_index;
	i32 tile_width;
	i32 tile_height;
	i32 resource_id;
	bool8 want_gpu_residency;
	bool8 want_cpu_residency;
	bool8 is_empty;
	bool8 failed;
	bool8 stale;
	bool8 upload_from_cached_pixels;
} tile_cache_result_t;

tile_cache_t* tile_cache_create(image_t* image);
/* The caller must serialize first creation; worker/result paths never create a cache. */
tile_cache_t* tile_cache_get_or_create(image_t* image);
void tile_cache_destroy(tile_cache_t* cache);
tile_cache_policy_t tile_cache_get_policy(image_t* image);
bool tile_cache_post_load_result(image_t* image, tile_cache_result_t* task);
bool tile_cache_post_stale_result(image_t* image, i32 resource_id, i32 level, i32 tile_index);
bool tile_cache_poll_load_result(image_t* image, tile_cache_result_t* out_task);
void tile_cache_pin_cpu_tile(image_t* image, i32 level, i32 tile_index, u32 demand_flags);
void tile_cache_unpin_cpu_tile(image_t* image, i32 level, i32 tile_index, u32 demand_flags);
bool tile_cache_tile_has_cpu_pixels(image_t* image, i32 level, i32 tile_index);
bool tile_cache_tile_is_cpu_pinned(image_t* image, i32 level, i32 tile_index);
bool tile_cache_tile_is_busy(image_t* image, i32 level, i32 tile_index);
bool tile_cache_try_begin_decode(image_t* image, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation);
bool tile_cache_try_begin_upload(image_t* image, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation);
void tile_cache_cancel_decode(image_t* image, i32 level, i32 tile_index);
void tile_cache_cancel_upload(image_t* image, i32 level, i32 tile_index);
void tile_cache_release_upload_cpu_pin(image_t* image, i32 level, i32 tile_index);
void tile_cache_mark_decode_finished(image_t* image, i32 level, i32 tile_index, bool failed);
void tile_cache_mark_upload_pending(image_t* image, i32 level, i32 tile_index);
void tile_cache_mark_upload_finished(image_t* image, i32 level, i32 tile_index);
renderer_texture_handle_t tile_cache_get_gpu_texture(image_t* image, i32 level, i32 tile_index);
void tile_cache_store_gpu_texture(image_t* image, i32 level, i32 tile_index, renderer_texture_handle_t texture);
renderer_texture_handle_t tile_cache_take_gpu_texture(image_t* image, i32 level, i32 tile_index);
/* The returned pointer is valid only while the caller's CPU pin remains held. */
u8* tile_cache_get_cpu_pixels(image_t* image, i32 level, i32 tile_index);
void tile_cache_store_cpu_pixels(image_t* image, i32 level, i32 tile_index, u8* pixels);
void tile_cache_release_cpu_pixels_if_unpinned(image_t* image, i32 level, i32 tile_index);
i32 tile_cache_request_viewer_tiles(image_t* image, tile_cache_viewer_request_t* request);
bool tile_cache_task_is_stale(image_t* image, i32 level, i32 tile_index, i32 generation);

#ifdef __cplusplus
}
#endif
