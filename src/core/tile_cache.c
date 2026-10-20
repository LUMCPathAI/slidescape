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
#include <float.h>

#include "dicomweb_client.h"
#include "mathutils.h"
#include "tile_buffer_pool.h"
#include "tile_loader.h"
#include "profiler.h"

typedef enum tile_cache_completion_event_kind_t {
	TILE_CACHE_COMPLETION_EVENT_RESULT = 1,
} tile_cache_completion_event_kind_t;

#define TILE_CACHE_VIEWER_WISHLIST_MAX 512
#define TILE_CACHE_INITIAL_CAPACITY 4096

typedef struct tile_cache_tile_t {
	u8 request_state;
	u32 demand_mask;
	u32 cpu_pin_count;
	u32 gpu_pin_count;
	u32 read_region_pin_count;
	u32 export_pin_count;
	u32 registration_pin_count;
	bool8 cpu_resident;
	bool8 gpu_resident;
	bool8 decode_in_flight;
	bool8 upload_pending;
	bool8 upload_cpu_pinned;
	bool8 viewer_inflight_counted;
	u8* pixels;
	renderer_texture_handle_t texture;
	i32 generation;
	i32 priority;
	u16 retryable_failure_count;
	i64 last_cpu_access_time;
	i64 last_gpu_access_time;
	i64 last_retryable_failure_time;
	u64 profile_capture_id;
	i64 profile_request_clock;
} tile_cache_tile_t;

typedef struct tile_cache_entry_t {
	u64 key; // Packed (plane:16, level:16, spatial tile:32) + 1; zero is an unused slot.
	tile_cache_tile_t value;
} tile_cache_entry_t;

struct tile_cache_t {
	image_t* image;
	platform_mutex_t lock;
	bool8 lock_initialized;
	bool8 stopping;
	i32 active_read_count;
	i32 inflight_viewer_tile_count;
	completion_queue_t result_queue;
	i32 viewer_generation;
	bool8 viewer_state_initialized;
	bounds2f last_camera_bounds;
	bounds2f last_crop_bounds;
	v2f last_camera_center;
	i32 last_zoom_level;
	i32 last_plane;
	tile_cache_entry_t* entries;
	size_t entry_capacity;
	size_t entry_count;
	bool8 last_is_cropped;
	tile_cache_policy_t policy;
};

static size_t tile_cache_pixel_size(image_t* image, i32 level) {
	if (!image || level < 0 || level >= image->level_count) return 0;
	level_image_t* level_image = image->level_images + level;
	return (size_t)level_image->tile_width * level_image->tile_height * BYTES_PER_PIXEL;
}

static int tile_cache_priority_compare(const void* a, const void* b) {
	return ((load_tile_task_t*)b)->priority - ((load_tile_task_t*)a)->priority;
}

static bool tile_cache_ssi_tasks_share_chunk(image_t* image,
		const load_tile_task_t* a, const load_tile_task_t* b) {
	if (!image || !a || !b || a->level != b->level) return false;
	level_image_t* level = image->level_images + a->level;
	i32 a_x = a->tile_index % level->width_in_tiles;
	i32 a_y = a->tile_index / level->width_in_tiles;
	i32 b_x = b->tile_index % level->width_in_tiles;
	i32 b_y = b->tile_index / level->width_in_tiles;
	return a_x / 4 == b_x / 4 && a_y / 2 == b_y / 2;
}

static i32 tile_cache_group_complete_ssi_chunks(image_t* image,
		load_tile_task_t* wishlist, i32 wishlist_count, i32 tile_limit) {
	load_tile_task_t grouped[TILE_CACHE_VIEWER_WISHLIST_MAX];
	bool8 consumed[TILE_CACHE_VIEWER_WISHLIST_MAX] = {0};
	i32 grouped_count = 0;
	for (i32 anchor_index = 0; anchor_index < wishlist_count; ++anchor_index) {
		if (consumed[anchor_index]) continue;
		i32 chunk_count = 0;
		for (i32 i = anchor_index; i < wishlist_count; ++i) {
			if (!consumed[i] &&
			    tile_cache_ssi_tasks_share_chunk(image, wishlist + anchor_index, wishlist + i)) {
				++chunk_count;
			}
		}
		if (grouped_count + chunk_count > tile_limit) break;
		for (i32 i = anchor_index; i < wishlist_count; ++i) {
			if (!consumed[i] &&
			    tile_cache_ssi_tasks_share_chunk(image, wishlist + anchor_index, wishlist + i)) {
				grouped[grouped_count++] = wishlist[i];
				consumed[i] = true;
			}
		}
	}
	memcpy(wishlist, grouped, grouped_count * sizeof(*wishlist));
	return grouped_count;
}

static bool tile_cache_bounds2f_equal(bounds2f a, bounds2f b) {
	return a.min.x == b.min.x && a.min.y == b.min.y && a.max.x == b.max.x && a.max.y == b.max.y;
}

static bool tile_cache_v2f_equal(v2f a, v2f b) {
	return a.x == b.x && a.y == b.y;
}

float tile_cache_retry_delay_seconds(i32 consecutive_failure_count) {
	if (consecutive_failure_count <= 0) return 0.0f;
	i32 shift = ATMOST(consecutive_failure_count - 1, 6);
	return ATMOST(0.5f * (float)(1 << shift), 30.0f);
}

/* cache->lock must be held. */
static bool tile_cache_retry_is_due_locked(tile_cache_tile_t* tile, i64 now) {
	if (!tile || tile->retryable_failure_count == 0) return true;
	float delay = tile_cache_retry_delay_seconds(tile->retryable_failure_count);
	return get_seconds_elapsed(tile->last_retryable_failure_time, now) >= delay;
}

static tile_cache_policy_t tile_cache_make_policy(image_t* image) {
	tile_cache_policy_t result = {0};
	result.max_inflight_tiles = 16;
	result.max_submit_per_tick = 10;
	result.batch_size = TILE_LOAD_BATCH_MAX;
	result.discard_stale_before_read = true;
	result.discard_stale_before_decode = true;
	result.discard_stale_before_upload = true;

	if (image->backend == IMAGE_BACKEND_SLIDE_SCORE) {
		result.max_inflight_tiles = 32;
		result.max_submit_per_tick = 8;
		result.batch_size = 1;
		result.latency_bound = true;
	} else if (image->backend == IMAGE_BACKEND_SSI) {
		/*
		 * SSI uses fixed 4x2 chunks. Twelve concurrent rectangles were the live
		 * SlideCenter throughput sweet spot.
		 */
		result.max_inflight_tiles = 12 * TILE_LOAD_BATCH_MAX;
		result.max_submit_per_tick = 12 * TILE_LOAD_BATCH_MAX;
		result.batch_size = TILE_LOAD_BATCH_MAX;
		result.latency_bound = true;
	} else if (image->backend == IMAGE_BACKEND_DICOMWEB) {
		result.max_inflight_tiles = 32;
		result.max_submit_per_tick = 8;
		/*
		 * The DICOMweb worker groups up to four already-scheduled frames from
		 * one SOP Instance. No coalescing delay is introduced here.
		 */
		result.batch_size = DICOMWEB_FRAME_BATCH_MAX;
		result.latency_bound = true;
	} else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
		result.max_inflight_tiles = 8;
	}

	return result;
}

/* Mix all key fields before masking into the power-of-two table (SplitMix64 finalizer). */
static size_t tile_cache_hash(u64 key) {
	key = (key ^ (key >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	key = (key ^ (key >> 27)) * UINT64_C(0x94d049bb133111eb);
	return (size_t)(key ^ (key >> 31));
}

/* Entries never escape the cache lock. Growth preserves pixel pointers and pin counts. */
static bool tile_cache_reserve(tile_cache_t* cache, size_t capacity) {
	if (capacity > SIZE_MAX / sizeof(tile_cache_entry_t)) return false;
	tile_cache_entry_t* entries = calloc(capacity, sizeof(*entries));
	if (!entries) return false;
	for (size_t i = 0; i < cache->entry_capacity; ++i) {
		tile_cache_entry_t* old = cache->entries + i;
		if (!old->key) continue;
		size_t slot = tile_cache_hash(old->key) & (capacity - 1);
		while (entries[slot].key) slot = (slot + 1) & (capacity - 1);
		entries[slot] = *old;
	}
	free(cache->entries);
	cache->entries = entries;
	cache->entry_capacity = capacity;
	return true;
}

/* Caller holds cache->lock. Queries and cleanup never insert entries. */
static tile_cache_tile_t* tile_cache_lookup_locked(tile_cache_t* cache, i32 plane, i32 level, i32 tile_index, bool insert) {
	image_t* image = cache ? cache->image : NULL;
	if (!image || plane < 0 || plane >= ATLEAST(image->plane_count, 1) || plane >= 64 ||
	    level < 0 || level >= image->level_count || level >= IMAGE_PYRAMID_MAX_LEVELS ||
	    tile_index < 0 || (u64)tile_index >= image->level_images[level].tile_count) return NULL;
	u64 key = (((u64)plane << 48) | ((u64)level << 32) | (u32)tile_index) + 1;
	size_t slot = tile_cache_hash(key) & (cache->entry_capacity - 1);
	while (cache->entries[slot].key && cache->entries[slot].key != key)
		slot = (slot + 1) & (cache->entry_capacity - 1);
	if (cache->entries[slot].key) return &cache->entries[slot].value;
	if (!insert) return NULL;
	if (cache->entry_count >= cache->entry_capacity - cache->entry_capacity / 4) {
		if (cache->entry_capacity > SIZE_MAX / 2 ||
		    !tile_cache_reserve(cache, cache->entry_capacity * 2)) return NULL;
		slot = tile_cache_hash(key) & (cache->entry_capacity - 1);
		while (cache->entries[slot].key) slot = (slot + 1) & (cache->entry_capacity - 1);
	}
	cache->entries[slot].key = key;
	++cache->entry_count;
	return &cache->entries[slot].value;
}

static tile_cache_tile_t* tile_cache_get_tile_locked(tile_cache_t* cache, i32 plane, i32 level, i32 tile_index) {
	return tile_cache_lookup_locked(cache, plane, level, tile_index, true);
}

static tile_cache_tile_t* tile_cache_find_tile_locked(tile_cache_t* cache, i32 plane, i32 level, i32 tile_index) {
	return tile_cache_lookup_locked(cache, plane, level, tile_index, false);
}

static bool tile_cache_has_hard_demand_locked(tile_cache_tile_t* tile) {
	if (!tile) {
		return false;
	}
	u32 hard_cpu_pin_count = tile->cpu_pin_count;
	if (tile->upload_cpu_pinned && hard_cpu_pin_count > 0) {
		--hard_cpu_pin_count;
	}
	return tile->read_region_pin_count > 0 || tile->export_pin_count > 0 ||
	       tile->registration_pin_count > 0 || hard_cpu_pin_count > 0 ||
	       tile->gpu_pin_count > 0;
}

static void tile_cache_mark_viewer_inflight_locked(tile_cache_t* cache,
		tile_cache_tile_t* tile) {
	if (!cache || !tile || tile->viewer_inflight_counted ||
	    !(tile->demand_mask & (TILE_CACHE_DEMAND_VIEWER_VISIBLE |
	                           TILE_CACHE_DEMAND_VIEWER_PREFETCH))) return;
	tile->viewer_inflight_counted = true;
	++cache->inflight_viewer_tile_count;
}

static void tile_cache_unmark_viewer_inflight_locked(tile_cache_t* cache,
		tile_cache_tile_t* tile) {
	if (!cache || !tile || !tile->viewer_inflight_counted) return;
	tile->viewer_inflight_counted = false;
	ASSERT(cache->inflight_viewer_tile_count > 0);
	if (cache->inflight_viewer_tile_count > 0) {
		--cache->inflight_viewer_tile_count;
	}
}

static void tile_cache_add_pin_demand_locked(tile_cache_tile_t* tile, u32 demand_flags) {
	if (demand_flags & TILE_CACHE_DEMAND_READ_REGION) {
		++tile->read_region_pin_count;
	}
	if (demand_flags & TILE_CACHE_DEMAND_EXPORT) {
		++tile->export_pin_count;
	}
	if (demand_flags & TILE_CACHE_DEMAND_REGISTRATION) {
		++tile->registration_pin_count;
	}
	tile->demand_mask |= demand_flags;
}

static void tile_cache_remove_pin_demand_locked(tile_cache_tile_t* tile, u32 demand_flags) {
	if ((demand_flags & TILE_CACHE_DEMAND_READ_REGION) && tile->read_region_pin_count > 0) {
		if (--tile->read_region_pin_count == 0) {
			tile->demand_mask &= ~TILE_CACHE_DEMAND_READ_REGION;
		}
	}
	if ((demand_flags & TILE_CACHE_DEMAND_EXPORT) && tile->export_pin_count > 0) {
		if (--tile->export_pin_count == 0) {
			tile->demand_mask &= ~TILE_CACHE_DEMAND_EXPORT;
		}
	}
	if ((demand_flags & TILE_CACHE_DEMAND_REGISTRATION) && tile->registration_pin_count > 0) {
		if (--tile->registration_pin_count == 0) {
			tile->demand_mask &= ~TILE_CACHE_DEMAND_REGISTRATION;
		}
	}
}

/* cache->lock must be held. The returned allocation must be freed after unlocking. */
static u8* tile_cache_detach_unpinned_cpu_pixels_locked(tile_cache_tile_t* tile) {
	if (!tile || tile->cpu_pin_count > 0) {
		return NULL;
	}
	u8* pixels = tile->pixels;
	tile->pixels = NULL;
	tile->cpu_resident = false;
	tile->demand_mask &= ~TILE_CACHE_DEMAND_CPU_RESIDENCY;
	return pixels;
}

/* cache->lock must be held. */
static u8* tile_cache_release_upload_cpu_pin_locked(tile_cache_tile_t* tile, bool release_pixels) {
	if (!tile || !tile->upload_cpu_pinned) {
		return NULL;
	}
	tile->upload_cpu_pinned = false;
	if (tile->cpu_pin_count > 0) {
		--tile->cpu_pin_count;
	}
	return release_pixels ? tile_cache_detach_unpinned_cpu_pixels_locked(tile) : NULL;
}

static bool tile_cache_accepts_work_locked(tile_cache_t* cache) {
	return cache && (!cache->stopping || cache->active_read_count > 0);
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

	/* One reusable allocation; double at 75% occupancy, never shrink during viewing. */
	if (!tile_cache_reserve(cache, TILE_CACHE_INITIAL_CAPACITY)) {
		tile_cache_destroy(cache);
		return NULL;
	}

	return cache;
}

tile_cache_t* tile_cache_get_or_create(image_t* image) {
	if (!image || image->is_deleted) {
		return NULL;
	}
	if (!image->tile_cache) {
		image->tile_cache = tile_cache_create(image);
	}
	return image->tile_cache;
}

void tile_cache_begin_shutdown(image_t* image) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	cache->stopping = true;
	platform_mutex_unlock(&cache->lock);
}

bool tile_cache_begin_read(image_t* image) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	if (cache->stopping) {
		platform_mutex_unlock(&cache->lock);
		return false;
	}
	++cache->active_read_count;
	platform_mutex_unlock(&cache->lock);
	return true;
}

void tile_cache_end_read(image_t* image) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	ASSERT(cache->active_read_count > 0);
	if (cache->active_read_count > 0) {
		--cache->active_read_count;
	}
	platform_mutex_unlock(&cache->lock);
}

i32 tile_cache_get_active_read_count(image_t* image) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return 0;
	}
	platform_mutex_lock(&cache->lock);
	i32 result = cache->active_read_count;
	platform_mutex_unlock(&cache->lock);
	return result;
}

void tile_cache_destroy(tile_cache_t* cache) {
	if (!cache) {
		return;
	}

	if (cache->result_queue.entries) {
		tile_cache_result_t task = {0};
		while (tile_cache_poll_load_result(cache->image, &task)) {
			if (task.pixel_memory) {
				tile_buffer_release(task.pixel_memory,
				                    (size_t)task.tile_width * task.tile_height * BYTES_PER_PIXEL);
			}
		}
	}

	for (size_t i = 0; i < cache->entry_capacity; ++i) {
		if (!cache->entries[i].key) continue;
		i32 level = ((cache->entries[i].key - 1) >> 32) & 0xffff;
		tile_buffer_release(cache->entries[i].value.pixels, tile_cache_pixel_size(cache->image, level));
	}
	free(cache->entries);
	if (cache->lock_initialized) {
		platform_mutex_destroy(&cache->lock);
		cache->lock_initialized = false;
	}
	completion_queue_destroy(&cache->result_queue);
	free(cache);
}

tile_cache_policy_t tile_cache_get_policy(image_t* image) {
	tile_cache_policy_t result = {0};
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return result;
	}
	platform_mutex_lock(&cache->lock);
	result = cache->policy;
	platform_mutex_unlock(&cache->lock);
	return result;
}

bool tile_cache_post_load_result(image_t* image, tile_cache_result_t* task) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || !task) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	bool posted = completion_queue_post(&cache->result_queue, TILE_CACHE_COMPLETION_EVENT_RESULT, task, sizeof(*task));
	platform_mutex_unlock(&cache->lock);
	return posted;
}

bool tile_cache_post_stale_result(image_t* image, i32 plane, i32 resource_id, i32 level, i32 tile_index) {
	tile_cache_result_t result = {0};
	result.plane = plane;
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
	platform_mutex_lock(&cache->lock);
	bool polled = completion_queue_poll(&cache->result_queue, &event);
	platform_mutex_unlock(&cache->lock);
	if (!polled) {
		return false;
	}
	if (event.kind != TILE_CACHE_COMPLETION_EVENT_RESULT) {
		return false;
	}
	*out_task = *(tile_cache_result_t*)event.userdata;
	return true;
}

bool tile_cache_apply_result(image_t* image, tile_cache_result_t* result,
                             bool renderer_available, tile_cache_result_action_t* out_action) {
	if (!out_action) {
		return false;
	}
	*out_action = (tile_cache_result_action_t){0};
	i32 plane = result ? result->plane : 0;
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || !result) {
		if (result) {
			out_action->pixels_to_free = result->pixel_memory;
		}
		return false;
	}

	u8* pixels = result->pixel_memory;
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, result->level, result->tile_index);
	if (!tile) {
		out_action->pixels_to_free = pixels;
		platform_mutex_unlock(&cache->lock);
		return false;
	}

	bool stale = result->stale;
	if (!stale && tile->generation > 0 && !tile_cache_has_hard_demand_locked(tile)) {
		stale = tile->generation != cache->viewer_generation;
	}

	if (result->upload_from_cached_pixels) {
		if (stale || !renderer_available || !result->want_gpu_residency ||
		    !tile->cpu_resident || !tile->pixels) {
			tile->upload_pending = false;
			tile_cache_unmark_viewer_inflight_locked(cache, tile);
			/*
			 * A non-renderer consumer retains cached pixels so the viewer can
			 * reserve a new upload. Stale/invalid work releases viewer-only data.
			 */
			bool release_pixels = stale || !tile->cpu_resident || !tile->pixels;
			out_action->pixels_to_free =
					tile_cache_release_upload_cpu_pin_locked(tile, release_pixels);
		} else {
			out_action->upload_pixels = tile->pixels;
			out_action->should_upload = true;
			out_action->release_upload_cpu_pin = true;
		}
		platform_mutex_unlock(&cache->lock);
		return true;
	}

	tile->decode_in_flight = false;
	if (stale) {
		tile_cache_unmark_viewer_inflight_locked(cache, tile);
		if (!tile->cpu_resident && !tile->gpu_resident) {
			tile->request_state = TILE_CACHE_UNREQUESTED;
		}
		out_action->pixels_to_free = pixels;
		platform_mutex_unlock(&cache->lock);
		return true;
	}

	/* A decoder can return an allocated blank/error buffer; it is not usable data. */
	if (result->is_empty || result->failed || result->retryable_failure) {
		out_action->pixels_to_free = pixels;
		pixels = NULL;
	}
	if (!pixels) {
		tile_cache_unmark_viewer_inflight_locked(cache, tile);
		if (result->retryable_failure) {
			if (tile->retryable_failure_count < UINT16_MAX) {
				++tile->retryable_failure_count;
			}
			tile->last_retryable_failure_time = get_clock();
		} else if (!result->failed) {
			tile->retryable_failure_count = 0;
			tile->last_retryable_failure_time = 0;
		}
		/* Only explicit emptiness on a single-plane image changes shared coverage.
		 * Missing/corrupt z-stack tiles are per-plane failures, never global holes. */
		out_action->mark_empty = result->is_empty && !result->retryable_failure &&
		                         image->plane_count <= 1 && plane == 0;
		tile->request_state = result->retryable_failure ? TILE_CACHE_UNREQUESTED
		                    : out_action->mark_empty ? TILE_CACHE_EMPTY : TILE_CACHE_FAILED;
		platform_mutex_unlock(&cache->lock);
		return true;
	}

	tile->request_state = TILE_CACHE_READY;
	tile->retryable_failure_count = 0;
	tile->last_retryable_failure_time = 0;
	bool retain_cpu_pixels = tile->cpu_pin_count > 0 || (!renderer_available &&
	                         (result->want_cpu_residency || result->want_gpu_residency));
	bool cache_owns_result_pixels = false;
	if (retain_cpu_pixels) {
		if (!tile->pixels) {
			tile->pixels = pixels;
			tile->cpu_resident = true;
			tile->last_cpu_access_time = get_clock();
			cache_owns_result_pixels = true;
		} else if (tile->pixels == pixels) {
			cache_owns_result_pixels = true;
		} else if (tile->pixels != pixels) {
			out_action->pixels_to_free = pixels;
		}
	}

	if (renderer_available && result->want_gpu_residency) {
		tile->upload_pending = true;
		out_action->upload_pixels = pixels;
		out_action->should_upload = true;
		out_action->free_upload_pixels = !cache_owns_result_pixels;
		if (cache_owns_result_pixels) {
			ASSERT(!tile->upload_cpu_pinned);
			tile->upload_cpu_pinned = true;
			++tile->cpu_pin_count;
			out_action->release_upload_cpu_pin = true;
		}
		if (out_action->pixels_to_free == pixels) {
			out_action->pixels_to_free = NULL;
		}
	} else if (!retain_cpu_pixels) {
		out_action->pixels_to_free = pixels;
	}
	if (!tile->upload_pending) {
		tile_cache_unmark_viewer_inflight_locked(cache, tile);
	}

	platform_mutex_unlock(&cache->lock);
	return true;
}

void tile_cache_pin_cpu_tile(image_t* image, i32 plane, i32 level, i32 tile_index, u32 demand_flags) {
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	if (!tile_cache_accepts_work_locked(cache)) {
		platform_mutex_unlock(&cache->lock);
		return;
	}
	tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, level, tile_index);
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		return;
	}
	++tile->cpu_pin_count;
	tile_cache_add_pin_demand_locked(tile, demand_flags);
	tile->demand_mask |= TILE_CACHE_DEMAND_CPU_RESIDENCY;
	platform_mutex_unlock(&cache->lock);
}

void tile_cache_unpin_cpu_tile(image_t* image, i32 plane, i32 level, i32 tile_index, u32 demand_flags) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	u8* pixels_to_free = NULL;
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		return;
	}
	if (tile->cpu_pin_count > 0) {
		--tile->cpu_pin_count;
	}
	tile_cache_remove_pin_demand_locked(tile, demand_flags);
	if (tile->cpu_pin_count == 0) {
		pixels_to_free = tile_cache_detach_unpinned_cpu_pixels_locked(tile);
	}
	platform_mutex_unlock(&cache->lock);
	tile_buffer_release(pixels_to_free, tile_cache_pixel_size(image, level));
}

bool tile_cache_tile_has_cpu_pixels(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	bool result = tile && tile->cpu_resident && tile->pixels;
	platform_mutex_unlock(&cache->lock);
	return result;
}

bool tile_cache_tile_is_cpu_pinned(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	bool result = tile && tile->cpu_pin_count > 0;
	platform_mutex_unlock(&cache->lock);
	return result;
}

bool tile_cache_tile_is_busy(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	bool result = tile && (tile->decode_in_flight || tile->upload_pending);
	platform_mutex_unlock(&cache->lock);
	return result;
}

bool tile_cache_try_begin_decode(image_t* image, i32 plane, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation) {
	if (!image_level_index_ready(image, plane, level)) {
		return false;
	}
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, level, tile_index);
	if (!tile_cache_accepts_work_locked(cache) || !tile ||
	    tile->decode_in_flight || tile->upload_pending ||
	    tile->request_state == TILE_CACHE_FAILED || tile->request_state == TILE_CACHE_EMPTY ||
	    (image->backend == IMAGE_BACKEND_SSI &&
	     image->ssi.reauthentication_state != 0) ||
	    !tile_cache_retry_is_due_locked(tile, get_clock())) {
		platform_mutex_unlock(&cache->lock);
		return false;
	}
	u32 residency_demand = demand_flags &
	                       (TILE_CACHE_DEMAND_CPU_RESIDENCY | TILE_CACHE_DEMAND_GPU_RESIDENCY);
	bool requested_residency_satisfied = residency_demand ?
			(!(residency_demand & TILE_CACHE_DEMAND_CPU_RESIDENCY) || tile->cpu_resident) &&
			(!(residency_demand & TILE_CACHE_DEMAND_GPU_RESIDENCY) || tile->gpu_resident) :
			(tile->cpu_resident || tile->gpu_resident);
	if (requested_residency_satisfied) {
		platform_mutex_unlock(&cache->lock);
		return false;
	}
	tile->profile_capture_id = profiler_capture_id();
	tile->profile_request_clock = tile->profile_capture_id ? get_clock() : 0;
	tile->decode_in_flight = true;
	tile->request_state = TILE_CACHE_IN_FLIGHT;
	tile->demand_mask |= demand_flags;
	tile->priority = priority;
	tile->generation = generation;
	tile_cache_mark_viewer_inflight_locked(cache, tile);
	platform_mutex_unlock(&cache->lock);
	return true;
}

bool tile_cache_try_begin_upload(image_t* image, i32 plane, i32 level, i32 tile_index, u32 demand_flags, i32 priority, i32 generation) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	if (!tile_cache_accepts_work_locked(cache) || !tile ||
	    tile->decode_in_flight || tile->upload_pending ||
	    !tile->cpu_resident || !tile->pixels) {
		platform_mutex_unlock(&cache->lock);
		return false;
	}
	tile->profile_capture_id = profiler_capture_id();
	tile->profile_request_clock = tile->profile_capture_id ? get_clock() : 0;
	tile->upload_pending = true;
	tile->upload_cpu_pinned = true;
	++tile->cpu_pin_count;
	tile->demand_mask |= demand_flags | TILE_CACHE_DEMAND_GPU_RESIDENCY;
	tile->priority = priority;
	tile->generation = generation;
	tile_cache_mark_viewer_inflight_locked(cache, tile);
	platform_mutex_unlock(&cache->lock);
	return true;
}

void tile_cache_cancel_decode(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_cancel_decode_batch(image, plane, &level, &tile_index, 1);
}

void tile_cache_cancel_decode_batch(image_t* image, i32 plane, const i32* levels,
		const i32* tile_indices, i32 tile_count) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || !levels || !tile_indices || tile_count <= 0) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	for (i32 i = 0; i < tile_count; ++i) {
		tile_cache_tile_t* tile = tile_cache_find_tile_locked(
			cache, plane, levels[i], tile_indices[i]);
		if (!tile || !tile->decode_in_flight) continue;
		tile->decode_in_flight = false;
		tile_cache_unmark_viewer_inflight_locked(cache, tile);
		if (!tile->cpu_resident && !tile->gpu_resident) {
			tile->request_state = TILE_CACHE_UNREQUESTED;
		}
	}
	platform_mutex_unlock(&cache->lock);
}

void tile_cache_cancel_upload(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	u8* pixels_to_free = NULL;
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		return;
	}
	tile->upload_pending = false;
	tile_cache_unmark_viewer_inflight_locked(cache, tile);
	pixels_to_free = tile_cache_release_upload_cpu_pin_locked(tile, true);
	platform_mutex_unlock(&cache->lock);
	tile_buffer_release(pixels_to_free, tile_cache_pixel_size(image, level));
}

void tile_cache_release_upload_cpu_pin(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	u8* pixels_to_free = tile_cache_release_upload_cpu_pin_locked(tile, true);
	platform_mutex_unlock(&cache->lock);
	tile_buffer_release(pixels_to_free, tile_cache_pixel_size(image, level));
}

renderer_texture_handle_t tile_cache_get_gpu_texture(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return 0;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	if (!tile || !tile->gpu_resident) {
		platform_mutex_unlock(&cache->lock);
		return 0;
	}
	tile->last_gpu_access_time = get_clock();
	renderer_texture_handle_t result = tile->texture;
	platform_mutex_unlock(&cache->lock);
	return result;
}

void tile_cache_store_gpu_texture(image_t* image, i32 plane, i32 level, i32 tile_index, renderer_texture_handle_t texture) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || texture == 0) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, level, tile_index);
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		return;
	}
	if (!tile->gpu_resident) {
		profiler_record_tile_ready(tile->profile_capture_id, tile->profile_request_clock);
	}
	tile->texture = texture;
	tile->gpu_resident = true;
	tile->upload_pending = false;
	tile_cache_unmark_viewer_inflight_locked(cache, tile);
	tile->last_gpu_access_time = get_clock();
	platform_mutex_unlock(&cache->lock);
}

renderer_texture_handle_t tile_cache_take_gpu_texture(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return 0;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		return 0;
	}
	renderer_texture_handle_t result = tile->texture;
	tile->texture = 0;
	tile->gpu_resident = false;
	tile->upload_pending = false;
	tile_cache_unmark_viewer_inflight_locked(cache, tile);
	platform_mutex_unlock(&cache->lock);
	return result;
}

u8* tile_cache_get_cpu_pixels(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return NULL;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	if (!tile || !tile->cpu_resident || tile->cpu_pin_count == 0) {
		platform_mutex_unlock(&cache->lock);
		return NULL;
	}
	tile->last_cpu_access_time = get_clock();
	u8* result = tile->pixels;
	platform_mutex_unlock(&cache->lock);
	return result;
}

void tile_cache_store_cpu_pixels(image_t* image, i32 plane, i32 level, i32 tile_index, u8* pixels) {
	if (!pixels) {
		return;
	}
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		tile_buffer_release(pixels, tile_cache_pixel_size(image, level));
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, level, tile_index);
	if (!tile) {
		platform_mutex_unlock(&cache->lock);
		tile_buffer_release(pixels, tile_cache_pixel_size(image, level));
		return;
	}
	if (tile->pixels && tile->pixels != pixels) {
		platform_mutex_unlock(&cache->lock);
		tile_buffer_release(pixels, tile_cache_pixel_size(image, level));
		return;
	}
	tile->pixels = pixels;
	tile->cpu_resident = true;
	tile->last_cpu_access_time = get_clock();
	platform_mutex_unlock(&cache->lock);
}

void tile_cache_release_cpu_pixels_if_unpinned(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	u8* pixels_to_free = tile_cache_detach_unpinned_cpu_pixels_locked(tile);
	platform_mutex_unlock(&cache->lock);
	tile_buffer_release(pixels_to_free, tile_cache_pixel_size(image, level));
}

bool tile_cache_task_is_stale(image_t* image, i32 plane, i32 level, i32 tile_index, i32 generation) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache || generation <= 0) {
		return false;
	}
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	/* A still-visible in-flight tile is adopted by the newest viewer generation. */
	bool result = tile && !tile_cache_has_hard_demand_locked(tile) &&
	              tile->generation != cache->viewer_generation;
	platform_mutex_unlock(&cache->lock);
	return result;
}

static bool tile_cache_viewer_changed(tile_cache_t* cache, tile_cache_viewer_request_t* request) {
	if (!cache->viewer_state_initialized || cache->last_plane != request->plane) {
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
		cache->last_plane = request->plane;
		cache->last_is_cropped = request->is_cropped;
		cache->viewer_state_initialized = true;
	}
}

/* cache->lock must be held by the caller. */
static i32 tile_cache_count_inflight_viewer_tiles_locked(image_t* image) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	return cache ? cache->inflight_viewer_tile_count : 0;
}

i32 tile_cache_request_viewer_tiles(image_t* image, tile_cache_viewer_request_t* request) {
	i32 plane = request ? request->plane : 0;
	if (image && image->backend == IMAGE_BACKEND_SSI &&
	    image->ssi.reauthentication_state != 0) {
		return 0;
	}
	tile_cache_t* cache = tile_cache_get_or_create(image);
	if (!cache || !request || plane < 0 || plane >= ATLEAST(image->plane_count, 1) || plane >= 64) {
		return 0;
	}
	platform_mutex_lock(&cache->lock);
	if (!tile_cache_accepts_work_locked(cache)) {
		platform_mutex_unlock(&cache->lock);
		return 0;
	}
	tile_cache_update_viewer_generation(cache, request);
	i32 viewer_generation = cache->viewer_generation;
	platform_mutex_unlock(&cache->lock);
	if (image->backend == IMAGE_BACKEND_SSI) {
		tile_loader_cancel_stale_ssi_pending(image, viewer_generation);
	}
	platform_mutex_lock(&cache->lock);
	if (!tile_cache_accepts_work_locked(cache)) {
		platform_mutex_unlock(&cache->lock);
		return 0;
	}

	i32 inflight_room = cache->policy.max_inflight_tiles - tile_cache_count_inflight_viewer_tiles_locked(image);
	if (inflight_room <= 0) {
		platform_mutex_unlock(&cache->lock);
		return 0;
	}

	load_tile_task_t wishlist[TILE_CACHE_VIEWER_WISHLIST_MAX];
	i32 wishlist_count = 0;
	tile_cache_policy_t policy = {0};
	i64 now = get_clock();
	float screen_radius = ATLEAST(1.0f, sqrtf(SQUARE(request->client_width / 2) + SQUARE(request->client_height / 2)));

	i32 highest_visible_scale = ATLEAST(image->level_count - 1, 0);
	i32 lowest_visible_scale = ATLEAST(request->zoom_level, 0);
	lowest_visible_scale = ATMOST(highest_visible_scale, lowest_visible_scale);
	for (; lowest_visible_scale > 0; --lowest_visible_scale) {
		if (image->level_images[lowest_visible_scale].exists &&
		    !(image->level_images[lowest_visible_scale].missing_planes & (UINT64_C(1) << plane))) {
			break;
		}
	}

	for (i32 level = lowest_visible_scale; level <= highest_visible_scale; ++level) {
		level_image_t* level_image = image->level_images + level;
		if (!level_image->exists || (level_image->missing_planes & (UINT64_C(1) << plane)) || level_image->needs_indexing ||
		    !image_level_index_ready(image, plane, level)) {
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
		bounds2i directly_visible_tiles = visible_tiles;
		if (image->backend == IMAGE_BACKEND_SSI) {
			visible_tiles.min.x = (visible_tiles.min.x / 4) * 4;
			visible_tiles.min.y = (visible_tiles.min.y / 2) * 2;
			visible_tiles.max.x = ((visible_tiles.max.x + 3) / 4) * 4;
			visible_tiles.max.y = ((visible_tiles.max.y + 1) / 2) * 2;
			visible_tiles = clip_bounds2i(visible_tiles, level_tiles_bounds);
		}

		i32 base_priority = (image->level_count - level) * 100;
		for (i32 tile_y = visible_tiles.min.y; tile_y < visible_tiles.max.y; ++tile_y) {
			for (i32 tile_x = visible_tiles.min.x; tile_x < visible_tiles.max.x; ++tile_x) {
				tile_t* geometry = get_tile(level_image, tile_x, tile_y);
				if (geometry->is_empty) continue; // Reject known blank positions before allocating cache state.
				i32 tile_index = geometry->tile_index;
				tile_cache_tile_t* tile = tile_cache_get_tile_locked(cache, plane, level, tile_index);
				if (!tile || (tile->gpu_resident && tile->texture != 0) || tile->request_state == TILE_CACHE_FAILED ||
				    tile->request_state == TILE_CACHE_EMPTY ||
				    !tile_cache_retry_is_due_locked(tile, now)) {
					continue;
				}

				float priority_x = image->backend == IMAGE_BACKEND_SSI
					? (float)((tile_x / 4) * 4 + 2) : tile_x + 0.5f;
				float priority_y = image->backend == IMAGE_BACKEND_SSI
					? (float)((tile_y / 2) * 2 + 1) : tile_y + 0.5f;
				float tile_distance_from_center_of_screen_x =
						(request->camera_center.x - (priority_x * level_image->x_tile_side_in_um)) / level_image->um_per_pixel_x;
				float tile_distance_from_center_of_screen_y =
						(request->camera_center.y - (priority_y * level_image->y_tile_side_in_um)) / level_image->um_per_pixel_y;
				float tile_distance_from_center_of_screen =
						sqrtf(SQUARE(tile_distance_from_center_of_screen_x) + SQUARE(tile_distance_from_center_of_screen_y));
				tile_distance_from_center_of_screen /= screen_radius;
				float priority_bonus = (1.0f - tile_distance_from_center_of_screen) * 300.0f;
				i32 priority = base_priority + (i32)priority_bonus;

				bool directly_visible = tile_x >= directly_visible_tiles.min.x &&
				                        tile_x < directly_visible_tiles.max.x &&
				                        tile_y >= directly_visible_tiles.min.y &&
				                        tile_y < directly_visible_tiles.max.y;
				tile->demand_mask |= (directly_visible ? TILE_CACHE_DEMAND_VIEWER_VISIBLE
				                                      : TILE_CACHE_DEMAND_VIEWER_PREFETCH) |
				                     TILE_CACHE_DEMAND_GPU_RESIDENCY;
				tile->generation = cache->viewer_generation;
				tile->priority = priority;
				if (tile->decode_in_flight || tile->upload_pending) {
					continue;
				}

				if (wishlist_count >= COUNT(wishlist)) {
					goto wishlist_full;
				}
				wishlist[wishlist_count++] = (load_tile_task_t) {
						.plane = plane,
						.resource_id = image->resource_id,
						.image = image,
						.level = level,
						.tile_index = tile_index,
						.priority = priority,
						.need_gpu_residency = true,
						.need_cpu_residency = tile->cpu_pin_count > 0,
						.may_discard_if_stale = true,
						.generation = cache->viewer_generation,
						.refcount_to_decrement = 1,
				};
			}
		}
	}

wishlist_full:
	policy = cache->policy;
	platform_mutex_unlock(&cache->lock);
	qsort(wishlist, wishlist_count, sizeof(load_tile_task_t), tile_cache_priority_compare);
	i32 tiles_to_load = ATMOST(wishlist_count, policy.max_submit_per_tick);
	tiles_to_load = ATMOST(tiles_to_load, inflight_room);
	if (image->backend == IMAGE_BACKEND_SSI) {
		tiles_to_load = tile_cache_group_complete_ssi_chunks(
			image, wishlist, wishlist_count, tiles_to_load);
	}
	return tile_loader_submit_requests(image, wishlist, tiles_to_load);
}

renderer_texture_handle_t tile_cache_get_focus_texture(image_t* image, i32 level, i32 tile_index, bool profile_target_level) {
	tile_cache_t* cache = image->tile_cache;
	if (!cache || !image_level_index_ready(image, 0, level)) {
		return 0;
	}
	if (get_tile_from_tile_index(image, level, tile_index)->is_empty) {
		if (profile_target_level) {
			profiler_record_visible_tile(PROFILER_TILE_EMPTY);
		}
		return 0;
	}
	platform_mutex_lock(&cache->lock);
	i32 selected = image->selected_plane;
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, selected, level, tile_index);
	renderer_texture_handle_t texture = tile && tile->gpu_resident ? tile->texture : 0;
	tile_cache_tile_t* drawn_tile = texture ? tile : NULL;
	bool selected_ready = texture != 0;
	if (texture) {
		tile->last_gpu_access_time = get_clock();
	}
	/* Adjacent planes only fill loading gaps, never terminal failures or known holes. */
	if (!texture && selected > 0 && image->backend == IMAGE_BACKEND_MRXS &&
	    (!tile || (tile->request_state != TILE_CACHE_FAILED && tile->request_state != TILE_CACHE_EMPTY))) {
		float best_distance = FLT_MAX;
		i32 best_plane = -1;
		mrxs_plane_t* planes = image->mrxs.planes;
		for (i32 p = 1; p < image->plane_count; ++p) {
			if (p == selected || planes[p].is_extended_focus) continue;
			tile_cache_tile_t* candidate = tile_cache_find_tile_locked(cache, p, level, tile_index);
			if (!candidate || !candidate->gpu_resident || !candidate->texture) continue;
			float distance = planes[p].offset_known && planes[selected].offset_known
				? fabsf(planes[p].offset_um - planes[selected].offset_um) : (float)abs(p - selected);
			bool preferred_side = (p - selected) * image->plane_approach_direction > 0;
			if (distance < best_distance || (distance == best_distance && preferred_side &&
			    (best_plane - selected) * image->plane_approach_direction <= 0)) {
				texture = candidate->texture;
				best_distance = distance;
				best_plane = p;
				drawn_tile = candidate;
				candidate->last_gpu_access_time = get_clock();
			}
		}
	}
	if (profile_target_level) {
		profiler_visible_tile_state_t state = PROFILER_TILE_MISSING;
		if (selected_ready) {
			state = PROFILER_TILE_READY;
		} else if (texture) {
			state = PROFILER_TILE_ADJACENT_PLANE;
		} else if (tile && tile->request_state == TILE_CACHE_FAILED) {
			state = PROFILER_TILE_FAILED;
		} else if (tile && tile->request_state == TILE_CACHE_EMPTY) {
			state = PROFILER_TILE_EMPTY;
		}
		profiler_record_visible_tile(state);
	}
	if (drawn_tile && drawn_tile->profile_request_clock) {
		// This lookup is used by the draw loop. Count once, including coarser or
		// adjacent-plane draws; stencil occlusion and scanout are not measured.
		profiler_record_tile_draw(drawn_tile->profile_capture_id, drawn_tile->profile_request_clock);
		drawn_tile->profile_request_clock = 0;
	}
	platform_mutex_unlock(&cache->lock);
	return texture;
}

void tile_cache_destroy_gpu_textures(image_t* image, void (*destroy_texture)(renderer_texture_handle_t)) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) return;
	platform_mutex_lock(&cache->lock);
	for (size_t i = 0; i < cache->entry_capacity; ++i) {
		if (!cache->entries[i].key) continue;
		tile_cache_tile_t* tile = &cache->entries[i].value;
		if (tile->texture) destroy_texture(tile->texture);
		tile->texture = 0;
		tile->gpu_resident = false;
	}
	platform_mutex_unlock(&cache->lock);
}

u32 tile_cache_get_tile_status(image_t* image, i32 plane, i32 level, i32 tile_index) {
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) return 0;
	platform_mutex_lock(&cache->lock);
	tile_cache_tile_t* tile = tile_cache_find_tile_locked(cache, plane, level, tile_index);
	u32 status = 0;
	if (tile) {
		if (tile->cpu_resident && tile->pixels) status |= TILE_CACHE_STATUS_CPU_READY;
		if (tile->decode_in_flight || tile->upload_pending) status |= TILE_CACHE_STATUS_BUSY;
		if (tile->request_state == TILE_CACHE_EMPTY) status |= TILE_CACHE_STATUS_EMPTY;
		if (tile->request_state == TILE_CACHE_FAILED) status |= TILE_CACHE_STATUS_FAILED;
	}
	platform_mutex_unlock(&cache->lock);
	return status;
}


tile_cache_memory_stats_t tile_cache_get_memory_stats(image_t* image) {
	tile_cache_memory_stats_t result = {0};
	tile_cache_t* cache = image ? image->tile_cache : NULL;
	if (!cache) {
		return result;
	}
	platform_mutex_lock(&cache->lock);
	result.table_bytes = cache->entry_capacity * sizeof(*cache->entries);
	result.entries = cache->entry_count;
	for (size_t i = 0; i < cache->entry_capacity; ++i) {
		tile_cache_entry_t* entry = cache->entries + i;
		if (!entry->key) {
			continue;
		}
		i32 level = (i32)(((entry->key - 1) >> 32) & 0xffff);
		size_t bytes = tile_cache_pixel_size(image, level);
		if (entry->value.texture) {
			result.texture_bytes += bytes;
		}
		if (entry->value.pixels) {
			result.cpu_bytes += bytes;
		}
	}
	platform_mutex_unlock(&cache->lock);
	return result;
}
