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

#include "tile_cache.h"

#include "mathutils.h"
#include "tile_loader.h"

typedef enum tile_cache_completion_event_kind_t {
	TILE_CACHE_COMPLETION_EVENT_RESULT = 1,
} tile_cache_completion_event_kind_t;

#define TILE_CACHE_VIEWER_WISHLIST_MAX 128

static int tile_cache_priority_compare(const void* a, const void* b) {
	return ((load_tile_task_t*)b)->priority - ((load_tile_task_t*)a)->priority;
}

static bool tile_cache_bounds2f_equal(bounds2f a, bounds2f b) {
	return a.min.x == b.min.x && a.min.y == b.min.y && a.max.x == b.max.x && a.max.y == b.max.y;
}

static bool tile_cache_v2f_equal(v2f a, v2f b) {
	return a.x == b.x && a.y == b.y;
}

static tile_cache_policy_t tile_cache_make_policy(image_t* image) {
	tile_cache_policy_t result = {0};
	result.max_inflight_tiles = 16;
	result.max_submit_per_tick = 10;
	result.batch_size = TILE_LOAD_BATCH_MAX;
	result.discard_stale_before_read = true;
	result.discard_stale_before_decode = true;
	result.discard_stale_before_upload = true;

	if (image->backend == IMAGE_BACKEND_SLIDE_SCORE || image->backend == IMAGE_BACKEND_DICOMWEB) {
		result.max_inflight_tiles = 32;
		result.max_submit_per_tick = 8;
		result.batch_size = 1;
		result.latency_bound = true;
	} else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
		result.max_inflight_tiles = 8;
	} else if (image->backend == IMAGE_BACKEND_TIFF && image->tiff.is_remote) {
		result.max_inflight_tiles = 8;
		result.max_submit_per_tick = 3;
		result.latency_bound = true;
	}

	return result;
}

tile_cache_t* tile_cache_create(image_t* image) {
	if (!image) {
		return NULL;
	}

	tile_cache_t* cache = (tile_cache_t*)calloc(1, sizeof(tile_cache_t));
	if (!cache) {
		return NULL;
	}
	cache->image = image;
	platform_mutex_init(&cache->lock);
	cache->lock_initialized = true;
	cache->result_queue = completion_queue_create(4096);
	cache->policy = tile_cache_make_policy(image);

	for (i32 level = 0; level < image->level_count; ++level) {
		level_image_t* level_image = image->level_images + level;
		if (level_image->exists && level_image->tile_count > 0) {
			cache->level_tiles[level] = (tile_cache_tile_t*)calloc(level_image->tile_count, sizeof(tile_cache_tile_t));
			if (!cache->level_tiles[level]) {
				tile_cache_destroy(cache);
				return NULL;
			}
		}
	}

	return cache;
}

tile_cache_t* tile_cache_get_or_create(image_t* image) {
	if (!image) {
		return NULL;
	}
	if (!image->tile_cache) {
		image->tile_cache = tile_cache_create(image);
	}
	return image->tile_cache;
}

void tile_cache_destroy(tile_cache_t* cache) {
	if (!cache) {
		return;
	}

	if (cache->result_queue.entries) {
		tile_cache_result_t task = {0};
		while (tile_cache_poll_load_result(cache->image, &task)) {
			if (task.pixel_memory) {
				free(task.pixel_memory);
			}
		}
	}

	for (i32 level = 0; level < COUNT(cache->level_tiles); ++level) {
		tile_cache_tile_t* tiles = cache->level_tiles[level];
		if (tiles) {
			image_t* image = cache->image;
			i32 tile_count = (image && level < image->level_count) ? image->level_images[level].tile_count : 0;
			for (i32 tile_index = 0; tile_index < tile_count; ++tile_index) {
				tile_cache_tile_t* tile = tiles + tile_index;
				if (tile->pixels) {
					free(tile->pixels);
					tile->pixels = NULL;
				}
			}
			free(tiles);
			cache->level_tiles[level] = NULL;
		}
	}

	if (cache->lock_initialized) {
		platform_mutex_destroy(&cache->lock);
		cache->lock_initialized = false;
	}
	completion_queue_destroy(&cache->result_queue);
	free(cache);
}

tile_cache_tile_t* tile_cache_get_tile_state(image_t* image, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || level < 0 || level >= COUNT(cache->level_tiles)) {
		return NULL;
	}
	if (level >= image->level_count) {
		return NULL;
	}
	level_image_t* level_image = image->level_images + level;
	if (tile_index < 0 || tile_index >= level_image->tile_count) {
		return NULL;
	}
	return cache->level_tiles[level] ? cache->level_tiles[level] + tile_index : NULL;
}

bool tile_cache_post_load_result(image_t* image, tile_cache_result_t* task) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache || !task) {
		return false;
	}
	return completion_queue_post(&cache->result_queue, TILE_CACHE_COMPLETION_EVENT_RESULT, task, sizeof(*task));
}

bool tile_cache_post_stale_result(image_t* image, i32 resource_id, i32 level, i32 tile_index) {
	tile_cache_result_t result = {0};
	result.resource_id = resource_id;
	result.level = level;
	result.tile_index = tile_index;
	result.stale = true;
	return tile_cache_post_load_result(image, &result);
}

bool tile_cache_poll_load_result(image_t* image, tile_cache_result_t* out_task) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || !out_task) {
		return false;
	}
	completion_event_t event = {0};
	if (!completion_queue_poll(&cache->result_queue, &event)) {
		return false;
	}
	if (event.kind != TILE_CACHE_COMPLETION_EVENT_RESULT) {
		return false;
	}
	*out_task = *(tile_cache_result_t*)event.userdata;
	return true;
}

void tile_cache_pin_cpu_tile(image_t* image, i32 level, i32 tile_index, u32 demand_flags) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	++tile->cpu_pin_count;
	tile->demand_mask |= demand_flags | TILE_CACHE_DEMAND_CPU_RESIDENCY;
}

void tile_cache_unpin_cpu_tile(image_t* image, i32 level, i32 tile_index, u32 demand_flags) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	if (tile->cpu_pin_count > 0) {
		--tile->cpu_pin_count;
	}
	if (tile->cpu_pin_count == 0) {
		tile->demand_mask &= ~(demand_flags | TILE_CACHE_DEMAND_CPU_RESIDENCY);
		tile_cache_release_cpu_pixels_if_unpinned(image, level, tile_index);
	}
}

bool tile_cache_tile_has_cpu_pixels(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	return tile && tile->cpu_resident && tile->pixels;
}

bool tile_cache_tile_is_cpu_pinned(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	return tile && tile->cpu_pin_count > 0;
}

bool tile_cache_tile_is_busy(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	return tile && (tile->decode_in_flight || tile->upload_pending);
}

bool tile_cache_try_begin_decode(image_t* image, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return false;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || tile->decode_in_flight || tile->upload_pending) {
		return false;
	}
	tile->decode_in_flight = true;
	tile->request_state = TILE_CACHE_IN_FLIGHT;
	tile->demand_mask |= demand_flags;
	tile->priority = priority;
	tile->generation = generation;
	return true;
}

bool tile_cache_try_begin_upload(image_t* image, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return false;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || tile->decode_in_flight || tile->upload_pending) {
		return false;
	}
	tile->upload_pending = true;
	tile->demand_mask |= demand_flags | TILE_CACHE_DEMAND_GPU_RESIDENCY;
	tile->priority = priority;
	tile->generation = generation;
	return true;
}

void tile_cache_cancel_decode(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->decode_in_flight = false;
	if (!tile->cpu_resident && !tile->gpu_resident) {
		tile->request_state = TILE_CACHE_UNREQUESTED;
	}
}

void tile_cache_cancel_upload(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->upload_pending = false;
}

void tile_cache_mark_decode_finished(image_t* image, i32 level, i32 tile_index, bool failed) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->decode_in_flight = false;
	tile->request_state = failed ? TILE_CACHE_FAILED : TILE_CACHE_READY;
}

void tile_cache_mark_upload_pending(image_t* image, i32 level, i32 tile_index) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->upload_pending = true;
	tile->demand_mask |= TILE_CACHE_DEMAND_GPU_RESIDENCY;
}

void tile_cache_mark_upload_finished(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->upload_pending = false;
	tile->gpu_resident = true;
	tile->last_gpu_access_time = get_clock();
}

renderer_texture_handle_t tile_cache_get_gpu_texture(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || !tile->gpu_resident) {
		return 0;
	}
	tile->last_gpu_access_time = get_clock();
	return tile->texture;
}

void tile_cache_store_gpu_texture(image_t* image, i32 level, i32 tile_index, renderer_texture_handle_t texture) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache || texture == 0) {
		return;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return;
	}
	tile->texture = texture;
	tile->gpu_resident = true;
	tile->upload_pending = false;
	tile->last_gpu_access_time = get_clock();
}

renderer_texture_handle_t tile_cache_take_gpu_texture(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		return 0;
	}
	renderer_texture_handle_t result = tile->texture;
	tile->texture = 0;
	tile->gpu_resident = false;
	tile->upload_pending = false;
	return result;
}

u8* tile_cache_get_cpu_pixels(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || !tile->cpu_resident) {
		return NULL;
	}
	tile->last_cpu_access_time = get_clock();
	return tile->pixels;
}

void tile_cache_store_cpu_pixels(image_t* image, i32 level, i32 tile_index, u8* pixels) {
	if (!pixels) {
		return;
	}
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		free(pixels);
		return;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile) {
		free(pixels);
		return;
	}
	if (tile->pixels && tile->pixels != pixels) {
		free(pixels);
		return;
	}
	tile->pixels = pixels;
	tile->cpu_resident = true;
	tile->last_cpu_access_time = get_clock();
}

void tile_cache_release_cpu_pixels_if_unpinned(image_t* image, i32 level, i32 tile_index) {
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || tile->cpu_pin_count > 0) {
		return;
	}
	if (tile->pixels) {
		free(tile->pixels);
		tile->pixels = NULL;
	}
	tile->cpu_resident = false;
}

static bool tile_cache_has_hard_demand(tile_cache_tile_t* tile) {
	if (!tile) {
		return false;
	}
	u32 hard_demand = TILE_CACHE_DEMAND_READ_REGION | TILE_CACHE_DEMAND_EXPORT | TILE_CACHE_DEMAND_REGISTRATION;
	return (tile->demand_mask & hard_demand) != 0 || tile->cpu_pin_count > 0 || tile->gpu_pin_count > 0;
}

bool tile_cache_task_is_stale(image_t* image, i32 level, i32 tile_index, i32 generation) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || generation <= 0) {
		return false;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
	if (!tile || tile_cache_has_hard_demand(tile)) {
		return false;
	}
	return generation != cache->viewer_generation;
}

static bool tile_cache_viewer_changed(tile_cache_t* cache, tile_cache_viewer_request_t* request) {
	if (!cache->viewer_state_initialized) {
		return true;
	}
	if (!tile_cache_bounds2f_equal(cache->last_camera_bounds, request->camera_bounds)) {
		return true;
	}
	if (!tile_cache_bounds2f_equal(cache->last_crop_bounds, request->crop_bounds)) {
		return true;
	}
	if (!tile_cache_v2f_equal(cache->last_camera_center, request->camera_center)) {
		return true;
	}
	if (cache->last_zoom_level != request->zoom_level) {
		return true;
	}
	return cache->last_is_cropped != request->is_cropped;
}

static void tile_cache_update_viewer_generation(tile_cache_t* cache, tile_cache_viewer_request_t* request) {
	if (tile_cache_viewer_changed(cache, request)) {
		++cache->viewer_generation;
		if (cache->viewer_generation <= 0) {
			cache->viewer_generation = 1;
		}
		cache->last_camera_bounds = request->camera_bounds;
		cache->last_crop_bounds = request->crop_bounds;
		cache->last_camera_center = request->camera_center;
		cache->last_zoom_level = request->zoom_level;
		cache->last_is_cropped = request->is_cropped;
		cache->viewer_state_initialized = true;
	}
}

static i32 tile_cache_count_inflight_viewer_tiles(image_t* image) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return 0;
	}
	i32 result = 0;
	for (i32 level = 0; level < image->level_count; ++level) {
		level_image_t* level_image = image->level_images + level;
		tile_cache_tile_t* tiles = cache->level_tiles[level];
		if (!level_image->exists || !tiles) {
			continue;
		}
		for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
			tile_cache_tile_t* tile = tiles + tile_index;
			if ((tile->decode_in_flight || tile->upload_pending) &&
			    (tile->demand_mask & TILE_CACHE_DEMAND_VIEWER_VISIBLE) &&
			    !tile_cache_has_hard_demand(tile)) {
				++result;
			}
		}
	}
	return result;
}

i32 tile_cache_request_viewer_tiles(image_t* image, tile_cache_viewer_request_t* request) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache || !request) {
		return 0;
	}
	tile_cache_update_viewer_generation(cache, request);

	i32 inflight_room = cache->policy.max_inflight_tiles - tile_cache_count_inflight_viewer_tiles(image);
	if (inflight_room <= 0) {
		return 0;
	}

	load_tile_task_t wishlist[TILE_CACHE_VIEWER_WISHLIST_MAX];
	i32 wishlist_count = 0;
	float screen_radius = ATLEAST(1.0f, sqrtf(SQUARE(request->client_width / 2) + SQUARE(request->client_height / 2)));

	i32 highest_visible_scale = ATLEAST(image->level_count - 1, 0);
	i32 lowest_visible_scale = ATLEAST(request->zoom_level, 0);
	lowest_visible_scale = ATMOST(highest_visible_scale, lowest_visible_scale);
	for (; lowest_visible_scale > 0; --lowest_visible_scale) {
		if (image->level_images[lowest_visible_scale].exists) {
			break;
		}
	}

	for (i32 level = lowest_visible_scale; level <= highest_visible_scale; ++level) {
		level_image_t* level_image = image->level_images + level;
		if (!level_image->exists || level_image->needs_indexing) {
			continue;
		}

		bounds2i level_tiles_bounds = BOUNDS2I(0, 0, (i32)level_image->width_in_tiles, (i32)level_image->height_in_tiles);
		bounds2i visible_tiles = world_bounds_to_tile_bounds(&request->camera_bounds,
		                                                     level_image->x_tile_side_in_um,
		                                                     level_image->y_tile_side_in_um,
		                                                     image->origin_offset);
		visible_tiles = clip_bounds2i(visible_tiles, level_tiles_bounds);
		if (request->is_cropped) {
			bounds2i crop_tile_bounds = world_bounds_to_tile_bounds(&request->crop_bounds,
			                                                        level_image->x_tile_side_in_um,
			                                                        level_image->y_tile_side_in_um,
			                                                        image->origin_offset);
			visible_tiles = clip_bounds2i(visible_tiles, crop_tile_bounds);
		}

		i32 base_priority = (image->level_count - level) * 100;
		for (i32 tile_y = visible_tiles.min.y; tile_y < visible_tiles.max.y; ++tile_y) {
			for (i32 tile_x = visible_tiles.min.x; tile_x < visible_tiles.max.x; ++tile_x) {
				tile_t* geometry = get_tile(level_image, tile_x, tile_y);
				i32 tile_index = geometry->tile_index;
				tile_cache_tile_t* tile = tile_cache_get_tile_state(image, level, tile_index);
				if (!tile || tile_cache_get_gpu_texture(image, level, tile_index) != 0 || geometry->is_empty ||
				    tile_cache_tile_is_busy(image, level, tile_index)) {
					continue;
				}

				float tile_distance_from_center_of_screen_x =
						(request->camera_center.x - ((tile_x + 0.5f) * level_image->x_tile_side_in_um)) / level_image->um_per_pixel_x;
				float tile_distance_from_center_of_screen_y =
						(request->camera_center.y - ((tile_y + 0.5f) * level_image->y_tile_side_in_um)) / level_image->um_per_pixel_y;
				float tile_distance_from_center_of_screen =
						sqrtf(SQUARE(tile_distance_from_center_of_screen_x) + SQUARE(tile_distance_from_center_of_screen_y));
				tile_distance_from_center_of_screen /= screen_radius;
				float priority_bonus = (1.0f - tile_distance_from_center_of_screen) * 300.0f;
				i32 priority = base_priority + (i32)priority_bonus;

				tile->demand_mask |= TILE_CACHE_DEMAND_VIEWER_VISIBLE | TILE_CACHE_DEMAND_GPU_RESIDENCY;
				tile->generation = cache->viewer_generation;
				tile->priority = priority;

				if (wishlist_count >= COUNT(wishlist)) {
					goto wishlist_full;
				}
				wishlist[wishlist_count++] = (load_tile_task_t) {
						.resource_id = image->resource_id,
						.image = image,
						.level = level,
						.tile_index = tile_index,
						.priority = priority,
						.need_gpu_residency = true,
						.need_cpu_residency = tile_cache_tile_is_cpu_pinned(image, level, tile_index),
						.may_discard_if_stale = true,
						.generation = cache->viewer_generation,
						.refcount_to_decrement = 1,
				};
			}
		}
	}

wishlist_full:
	qsort(wishlist, wishlist_count, sizeof(load_tile_task_t), tile_cache_priority_compare);
	i32 tiles_to_load = ATMOST(wishlist_count, cache->policy.max_submit_per_tick);
	tiles_to_load = ATMOST(tiles_to_load, inflight_room);
	return tile_loader_submit_requests(image, wishlist, tiles_to_load);
}
