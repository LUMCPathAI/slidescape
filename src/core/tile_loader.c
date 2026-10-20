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

#include "tile_loader.h"
#include "tile_buffer_pool.h"

#include "dicom_wsi.h"
#include "mrxs.h"
#include "tile_cache.h"
#include "profiler.h"
#include "tiff.h"

static work_queue_callback_t* slide_score_load_tile_batch_func;
static work_queue_callback_t* dicomweb_load_tile_batch_func;
static work_queue_callback_t* ssi_load_tile_batch_callback;

#define SSI_PENDING_BATCH_MAX 128
#define SSI_PENDING_WORKER_MAX 12

typedef struct ssi_pending_batch_queue_t {
	platform_mutex_t mutex;
	load_tile_task_batch_t batches[SSI_PENDING_BATCH_MAX];
	i32 batch_count;
	i32 pump_count;
} ssi_pending_batch_queue_t;

static ssi_pending_batch_queue_t ssi_pending_queue = {
	.mutex = PLATFORM_MUTEX_INITIALIZER,
};


void tile_loader_set_slide_score_batch_callback(work_queue_callback_t* callback) {
	slide_score_load_tile_batch_func = callback;
}

void tile_loader_set_dicomweb_batch_callback(work_queue_callback_t* callback) {
	dicomweb_load_tile_batch_func = callback;
}

void tile_loader_set_ssi_batch_callback(work_queue_callback_t* callback) {
	ssi_load_tile_batch_callback = callback;
}

static void ssi_pending_batch_release(load_tile_task_batch_t* batch, bool cancel_tiles) {
	if (!batch || batch->task_count <= 0) return;
	i32 refcount_decrement = 0;
	i32 levels[TILE_LOAD_BATCH_MAX];
	i32 tile_indices[TILE_LOAD_BATCH_MAX];
	for (i32 i = 0; i < batch->task_count; ++i) {
		load_tile_task_t* task = batch->tile_tasks + i;
		refcount_decrement += task->refcount_to_decrement;
		levels[i] = task->level;
		tile_indices[i] = task->tile_index;
	}
	if (cancel_tiles) {
		tile_cache_cancel_decode_batch(batch->tile_tasks[0].image, 0,
		                               levels, tile_indices, batch->task_count);
	}
	atomic_subtract(&batch->tile_tasks[0].image->refcount, refcount_decrement);
	task_group_end(batch->tile_tasks[0].task_group);
}

void tile_loader_cancel_stale_ssi_pending(image_t* image, i32 viewer_generation) {
	if (!image || image->backend != IMAGE_BACKEND_SSI) return;
	load_tile_task_batch_t cancelled_batches[SSI_PENDING_BATCH_MAX];
	i32 cancelled_batch_count = 0;
	platform_mutex_lock(&ssi_pending_queue.mutex);
	for (i32 i = 0; i < ssi_pending_queue.batch_count;) {
		load_tile_task_batch_t* batch = ssi_pending_queue.batches + i;
		if (batch->tile_tasks[0].image == image &&
		    batch->tile_tasks[0].generation != viewer_generation) {
			cancelled_batches[cancelled_batch_count++] = *batch;
			ssi_pending_queue.batches[i] =
				ssi_pending_queue.batches[--ssi_pending_queue.batch_count];
			continue;
		}
		++i;
	}
	platform_mutex_unlock(&ssi_pending_queue.mutex);
	if (cancelled_batch_count > 0) {
		i32 levels[SSI_PENDING_BATCH_MAX * TILE_LOAD_BATCH_MAX];
		i32 tile_indices[SSI_PENDING_BATCH_MAX * TILE_LOAD_BATCH_MAX];
		i32 cancelled_tile_count = 0;
		for (i32 batch_index = 0; batch_index < cancelled_batch_count; ++batch_index) {
			load_tile_task_batch_t* batch = cancelled_batches + batch_index;
			for (i32 tile_index = 0; tile_index < batch->task_count; ++tile_index) {
				levels[cancelled_tile_count] = batch->tile_tasks[tile_index].level;
				tile_indices[cancelled_tile_count] = batch->tile_tasks[tile_index].tile_index;
				++cancelled_tile_count;
			}
		}
		tile_cache_cancel_decode_batch(image, 0, levels, tile_indices, cancelled_tile_count);
		for (i32 batch_index = 0; batch_index < cancelled_batch_count; ++batch_index) {
			ssi_pending_batch_release(cancelled_batches + batch_index, false);
		}
		console_print_verbose("SSI pending queue: discarded %d stale chunks (%d tiles) "
		                      "for viewer generation %d\n",
		                      cancelled_batch_count, cancelled_tile_count,
		                      viewer_generation);
	}
}

static void tile_loader_profiled_batch(i32 logical_thread_index, void* userdata) {
	load_tile_task_batch_t* batch = (load_tile_task_batch_t*)userdata;
	load_tile_task_t* task = &batch->tile_tasks[0];
	work_queue_callback_t* callback = NULL;
	switch (task->image->backend) {
		case IMAGE_BACKEND_SLIDE_SCORE: {
			callback = slide_score_load_tile_batch_func;
		} break;
		case IMAGE_BACKEND_DICOMWEB: {
			callback = dicomweb_load_tile_batch_func;
		} break;
		case IMAGE_BACKEND_SSI: {
			callback = ssi_load_tile_batch_callback;
		} break;
		default: {
			ASSERT(!"unexpected tile batch backend");
		} break;
	}
	profiler_worker_begin(task->profile_capture_id, task->profile_queued_clock);
	callback(logical_thread_index, userdata);
	profiler_worker_end();
}

static void ssi_pending_pump(i32 logical_thread_index, void* userdata) {
	for (;;) {
		load_tile_task_batch_t batch = {0};
		platform_mutex_lock(&ssi_pending_queue.mutex);
		if (ssi_pending_queue.batch_count == 0) {
			--ssi_pending_queue.pump_count;
			platform_mutex_unlock(&ssi_pending_queue.mutex);
			return;
		}
		i32 selected_index = 0;
		for (i32 i = 1; i < ssi_pending_queue.batch_count; ++i) {
			if (ssi_pending_queue.batches[i].tile_tasks[0].priority >
			    ssi_pending_queue.batches[selected_index].tile_tasks[0].priority) {
				selected_index = i;
			}
		}
		batch = ssi_pending_queue.batches[selected_index];
		ssi_pending_queue.batches[selected_index] =
			ssi_pending_queue.batches[--ssi_pending_queue.batch_count];
		platform_mutex_unlock(&ssi_pending_queue.mutex);

		tile_loader_profiled_batch(logical_thread_index, &batch);
		task_group_end(batch.tile_tasks[0].task_group);
	}
}

static bool ssi_pending_enqueue(load_tile_task_batch_t* batch) {
	if (!batch || batch->task_count <= 0 || !ssi_load_tile_batch_callback) return false;
	platform_mutex_lock(&ssi_pending_queue.mutex);
	if (ssi_pending_queue.batch_count >= SSI_PENDING_BATCH_MAX) {
		platform_mutex_unlock(&ssi_pending_queue.mutex);
		return false;
	}
	task_group_begin(batch->tile_tasks[0].task_group);
	for (i32 i = 0; i < batch->task_count; ++i) {
		atomic_add(&batch->tile_tasks[i].image->refcount,
		           batch->tile_tasks[i].refcount_to_decrement);
	}
	ssi_pending_queue.batches[ssi_pending_queue.batch_count++] = *batch;
	bool start_pump = ssi_pending_queue.pump_count < SSI_PENDING_WORKER_MAX;
	if (start_pump) ++ssi_pending_queue.pump_count;
	platform_mutex_unlock(&ssi_pending_queue.mutex);

	if (start_pump && !thread_pool_submit_task(&global_thread_pool,
	                                          ssi_pending_pump, NULL, 0)) {
		load_tile_task_batch_t cancelled = {0};
		platform_mutex_lock(&ssi_pending_queue.mutex);
		--ssi_pending_queue.pump_count;
		if (ssi_pending_queue.pump_count == 0) {
			for (i32 i = 0; i < ssi_pending_queue.batch_count; ++i) {
				load_tile_task_batch_t* candidate = ssi_pending_queue.batches + i;
				if (candidate->tile_tasks[0].image == batch->tile_tasks[0].image &&
				    candidate->tile_tasks[0].level == batch->tile_tasks[0].level &&
				    candidate->tile_tasks[0].tile_index == batch->tile_tasks[0].tile_index &&
				    candidate->tile_tasks[0].generation == batch->tile_tasks[0].generation) {
					cancelled = *candidate;
					* candidate = ssi_pending_queue.batches[--ssi_pending_queue.batch_count];
					break;
				}
			}
		}
		platform_mutex_unlock(&ssi_pending_queue.mutex);
		if (cancelled.task_count > 0) {
			ssi_pending_batch_release(&cancelled, true);
			return false;
		}
		/* Another active pump can still consume the queued batch. */
	}
	return true;
}

static void tile_loader_get_tile_xy(image_t* image, i32 level, i32 tile_index, i32* out_tile_x, i32* out_tile_y) {
	level_image_t* level_image = image->level_images + level;
	*out_tile_x = tile_index % level_image->width_in_tiles;
	*out_tile_y = tile_index / level_image->width_in_tiles;
}

static void tile_loader_post_stale_result(load_tile_task_t* task) {
	if (!tile_cache_post_stale_result(task->image, task->plane, task->resource_id, task->level, task->tile_index)) {
		tile_cache_cancel_decode(task->image, task->plane, task->level, task->tile_index);
	}
}

static void tile_loader_post_failed_result(load_tile_task_t* task) {
	level_image_t* level_image = task->image->level_images + task->level;
	tile_cache_result_t result = {
		.plane = task->plane,
		.resource_id = task->resource_id,
		.level = task->level,
		.tile_index = task->tile_index,
		.tile_width = level_image->tile_width,
		.tile_height = level_image->tile_height,
		.want_gpu_residency = task->need_gpu_residency,
		.want_cpu_residency = task->need_cpu_residency,
		.failed = true,
	};
	if (!tile_cache_post_load_result(task->image, &result)) {
		tile_cache_cancel_decode(task->image, task->plane, task->level, task->tile_index);
	}
}

static bool ssi_tasks_share_rectangle_block(image_t* image,
		const load_tile_task_t* anchor, const load_tile_task_t* candidate) {
	if (!image || !anchor || !candidate || anchor->level != candidate->level) return false;
	i32 anchor_x = 0;
	i32 anchor_y = 0;
	i32 candidate_x = 0;
	i32 candidate_y = 0;
	tile_loader_get_tile_xy(image, anchor->level, anchor->tile_index, &anchor_x, &anchor_y);
	tile_loader_get_tile_xy(image, candidate->level, candidate->tile_index,
	                        &candidate_x, &candidate_y);
	return anchor_x / 4 == candidate_x / 4 && anchor_y / 2 == candidate_y / 2;
}

static i32 ssi_group_rectangle_tasks(image_t* image, load_tile_task_t* wishlist,
		i32 first_tile, i32 tiles_to_load, i32 max_batch_size) {
	load_tile_task_t anchor = wishlist[first_tile];
	i32 selected_count = 0;
	while (selected_count < max_batch_size) {
		i32 selected_index = -1;
		for (i32 i = first_tile + selected_count; i < tiles_to_load; ++i) {
			if (ssi_tasks_share_rectangle_block(image, &anchor, wishlist + i)) {
				selected_index = i;
				break;
			}
		}
		if (selected_index < 0) break;
		load_tile_task_t swap = wishlist[first_tile + selected_count];
		wishlist[first_tile + selected_count] = wishlist[selected_index];
		wishlist[selected_index] = swap;
		++selected_count;
	}
	return selected_count;
}

i32 tile_loader_submit_requests(image_t* image, load_tile_task_t* wishlist, i32 tiles_to_load) {
	i32 tasks_waiting = thread_pool_get_task_count(&global_thread_pool);
	i32 max_acceptable_tasks = thread_pool_get_task_capacity(&global_thread_pool);
	i32 usable_slots = max_acceptable_tasks - tasks_waiting;
	i32 tile_capacity = usable_slots;
	if (image->backend == IMAGE_BACKEND_SSI) {
		tile_capacity *= TILE_LOAD_BATCH_MAX;
	}
	if (tiles_to_load > tile_capacity) {
		console_print_error("tile_loader_submit_requests(): requested %d tiles, but only %d tasks fit into the work queue", tiles_to_load, usable_slots);
		tiles_to_load = tile_capacity;
	}

	i32 tile_loads_submitted = 0;

	if (tiles_to_load > 0) {
		bool use_remote_batch = image->backend == IMAGE_BACKEND_SLIDE_SCORE ||
		                        image->backend == IMAGE_BACKEND_DICOMWEB ||
		                        image->backend == IMAGE_BACKEND_SSI;
		if (use_remote_batch) {
			work_queue_callback_t* load_func = NULL;
			if (image->backend == IMAGE_BACKEND_SLIDE_SCORE) {
				load_func = slide_score_load_tile_batch_func;
			} else if (image->backend == IMAGE_BACKEND_DICOMWEB) {
				load_func = dicomweb_load_tile_batch_func;
			} else if (image->backend == IMAGE_BACKEND_SSI) {
				load_func = ssi_load_tile_batch_callback;
			}

			if (!load_func) {
				console_print_error("tile_loader_submit_requests(): remote tile batch callback is not registered\n");
				return 0;
			}

			i32 work_items_submitted = 0;
			i32 policy_batch_size = TILE_LOAD_BATCH_MAX;
			tile_cache_policy_t policy = tile_cache_get_policy(image);
			if (policy.batch_size > 0) {
				policy_batch_size = policy.batch_size;
			}
			policy_batch_size = CLAMP(policy_batch_size, 1, TILE_LOAD_BATCH_MAX);
			for (i32 first_tile = 0;
			     first_tile < tiles_to_load && work_items_submitted < usable_slots;) {
				i32 candidate_count = ATMOST(policy_batch_size, tiles_to_load - first_tile);
				if (image->backend == IMAGE_BACKEND_SSI) {
					candidate_count = ssi_group_rectangle_tasks(image, wishlist, first_tile,
					                                            tiles_to_load, policy_batch_size);
				}
				ASSERT(candidate_count > 0);
				load_tile_task_batch_t batch = {0};
				for (i32 i = first_tile; i < first_tile + candidate_count; ++i) {
					load_tile_task_t* task = wishlist + i;
					u32 demand_flags = task->need_gpu_residency ? TILE_CACHE_DEMAND_GPU_RESIDENCY : 0;
					demand_flags |= task->need_cpu_residency ? TILE_CACHE_DEMAND_CPU_RESIDENCY : 0;
					if (tile_cache_try_begin_decode(image, task->plane, task->level, task->tile_index, demand_flags, task->priority, task->generation)) {
						load_tile_task_t* submitted_task = &batch.tile_tasks[batch.task_count++];
						*submitted_task = *task;
						submitted_task->profile_capture_id = profiler_capture_id();
						submitted_task->profile_queued_clock = submitted_task->profile_capture_id ? get_clock() : 0;
					} else {
						console_print_verbose("tile_loader_submit_requests(): tile already requested by another thread (%d)\n", task->tile_index);
					}
				}
				if (batch.task_count == 0) {
					first_tile += candidate_count;
					continue;
				}
				bool submitted = image->backend == IMAGE_BACKEND_SSI
					? ssi_pending_enqueue(&batch)
					: thread_pool_submit_task_to_group(&global_thread_pool,
					                                   batch.tile_tasks[0].task_group,
					                                   tile_loader_profiled_batch, &batch, sizeof(batch));
				if (submitted) {
					++work_items_submitted;
					for (i32 i = 0; i < batch.task_count; ++i) {
						load_tile_task_t* task = batch.tile_tasks + i;
						if (image->backend != IMAGE_BACKEND_SSI) {
							atomic_add(&image->refcount, task->refcount_to_decrement);
						}
						++tile_loads_submitted;
					}
				} else {
					for (i32 i = 0; i < batch.task_count; ++i) {
						load_tile_task_t* task = batch.tile_tasks + i;
						tile_cache_cancel_decode(image, task->plane, task->level, task->tile_index);
					}
				}
				first_tile += candidate_count;
			}
		} else {
			for (i32 i = 0; i < tiles_to_load; ++i) {
				load_tile_task_t task = wishlist[i];
				task.profile_capture_id = profiler_capture_id();
				task.profile_queued_clock = task.profile_capture_id ? get_clock() : 0;
				u32 demand_flags = task.need_gpu_residency ? TILE_CACHE_DEMAND_GPU_RESIDENCY : 0;
				demand_flags |= task.need_cpu_residency ? TILE_CACHE_DEMAND_CPU_RESIDENCY : 0;

				if (tile_cache_tile_has_cpu_pixels(image, task.plane, task.level, task.tile_index) &&
					    tile_cache_get_gpu_texture(image, task.plane, task.level, task.tile_index) == 0 && task.need_gpu_residency) {
					if (tile_cache_try_begin_upload(image, task.plane, task.level, task.tile_index, demand_flags, task.priority, task.generation)) {
						task_group_begin(task.task_group);
						level_image_t* level_image = image->level_images + task.level;
						tile_cache_result_t upload_request = {0};
						upload_request.plane = task.plane;
						upload_request.resource_id = task.resource_id;
						upload_request.level = task.level;
						upload_request.tile_index = task.tile_index;
						upload_request.tile_width = level_image->tile_width;
						upload_request.tile_height = level_image->tile_height;
						upload_request.want_gpu_residency = task.need_gpu_residency;
						upload_request.want_cpu_residency = task.need_cpu_residency;
						upload_request.upload_from_cached_pixels = true;
						if (!tile_cache_post_load_result(image, &upload_request)) {
							tile_cache_cancel_upload(image, task.plane, task.level, task.tile_index);
						} else {
							++tile_loads_submitted;
						}
						task_group_end(task.task_group);
					} else {
						console_print_verbose("tile_loader_submit_requests(): tile already requested by another thread (%d)\n", task.tile_index);
					}
				} else if (tile_cache_try_begin_decode(image, task.plane, task.level, task.tile_index, demand_flags, task.priority, task.generation)) {
					if (thread_pool_submit_task_to_group(&global_thread_pool, task.task_group, load_tile_func, &task, sizeof(task))) {
						atomic_add(&image->refcount, task.refcount_to_decrement);
						++tile_loads_submitted;
					} else {
						// TODO: should we even allow this to fail?
						tile_cache_cancel_decode(image, task.plane, task.level, task.tile_index);
					}
				} else {
					console_print_verbose("tile_loader_submit_requests(): tile already requested by another thread (%d)\n", task.tile_index);
				}
			}
		}
	}
	return tile_loads_submitted;
}

static void load_tile_impl(i32 logical_thread_index, void* userdata) {
	load_tile_task_t* task = (load_tile_task_t*) userdata;
	image_t* image = task->image;

	if (image->is_deleted) {
		// Early out to save time if the image was already closed/waiting for destruction
		atomic_subtract(&image->refcount, task->refcount_to_decrement);
		return;
	}

	i32 level = task->level;
	level_image_t* level_image = image->level_images + level;
	ASSERT(level_image->exists);
	i32 tile_index = task->tile_index;
	i32 tile_x = 0;
	i32 tile_y = 0;
	tile_loader_get_tile_xy(image, level, tile_index, &tile_x, &tile_y);

	if (task->may_discard_if_stale && tile_cache_task_is_stale(image, task->plane, level, tile_index, task->generation)) {
		tile_loader_post_stale_result(task);
		atomic_subtract(&image->refcount, task->refcount_to_decrement);
		return;
	}
	ASSERT(level_image->x_tile_side_in_um > 0 && level_image->y_tile_side_in_um > 0);
	float tile_world_pos_x_end = (tile_x + 1) * level_image->x_tile_side_in_um;
	float tile_world_pos_y_end = (tile_y + 1) * level_image->y_tile_side_in_um;
	float tile_x_excess = tile_world_pos_x_end - image->width_in_um;
	float tile_y_excess = tile_world_pos_y_end - image->height_in_um;

	size_t pixel_memory_size = level_image->tile_width * level_image->tile_height * BYTES_PER_PIXEL;
	u32 image_background_color = image->is_background_black ? 0 : 0xFFFFFFFF;
	u8 image_background_byte = image->is_background_black ? 0 : 0xFF;
	u8* temp_memory = NULL;
	if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
		temp_memory = tile_buffer_acquire(pixel_memory_size);
		if (!temp_memory) {
			tile_loader_post_failed_result(task);
			atomic_subtract(&image->refcount, task->refcount_to_decrement);
			return;
		}
		memset(temp_memory, image_background_byte, pixel_memory_size);
	}

	bool failed = false;
	bool is_empty = false; // we might 'discover' that the tile is empty for OpenSlide backend
	ASSERT(image->type == IMAGE_TYPE_WSI);
	if (image->backend == IMAGE_BACKEND_TIFF) {
		tiff_t* tiff = &image->tiff;
		tiff_ifd_t* level_ifd = tiff->level_images_ifd + level_image->pyramid_image_index;
		u8* pixels = tiff_decode_tile(logical_thread_index, tiff, level_ifd, tile_index, level, tile_x, tile_y);
		if (pixels) {
			tile_buffer_release(temp_memory, pixel_memory_size);
			temp_memory = pixels;
		} else {
			failed = true;
		}

		// Trim the tile if it extends beyond the image size.
		i32 new_tile_height = level_image->tile_height;
		i32 pitch = level_image->tile_width * BYTES_PER_PIXEL;
		if (temp_memory && tile_y_excess > 0) {
			i32 excess_rows = (int)((tile_y_excess / level_image->y_tile_side_in_um) * level_image->tile_height);
			ASSERT(excess_rows >= 0);
			new_tile_height = level_image->tile_height - excess_rows;
			memset(temp_memory + (new_tile_height * pitch), image_background_byte, excess_rows * pitch);
		}
		if (temp_memory && tile_x_excess > 0) {
			i32 excess_pixels = (int)((tile_x_excess / level_image->x_tile_side_in_um) * level_image->tile_width);
			ASSERT(excess_pixels >= 0);
			i32 new_tile_width = level_image->tile_width - excess_pixels;
			for (i32 row = 0; row < new_tile_height; ++row) {
				u8* write_pos = temp_memory + (row * pitch) + (new_tile_width * BYTES_PER_PIXEL);
				memset(write_pos, image_background_byte, excess_pixels * BYTES_PER_PIXEL);
			}
		}
	} else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
		wsi_t* wsi = &image->openslide_wsi;
		i32 wsi_file_level = level_image->pyramid_image_index;
		i64 x = (tile_x * level_image->tile_width) << level;
		i64 y = (tile_y * level_image->tile_height) << level;
		openslide.read_region(wsi->osr, (u32*)temp_memory, x, y, wsi_file_level, level_image->tile_width, level_image->tile_height);
		const char* read_error = openslide.get_error(wsi->osr);
		if (read_error) {
			console_print_error("OpenSlide tile read failed: %s\n", read_error);
			failed = true;
		}

		u32 pixel_count = level_image->tile_width * level_image->tile_height;
		u32* pixels = (u32*)temp_memory;
		u32 nonempty_pixel_count = 0;
		for (i32 i = 0; i < pixel_count; ++i) {
			u32 p = pixels[i];
			nonempty_pixel_count += (p > 0);
			pixels[i] = p ? p : image_background_color;
		}
		if (!failed && nonempty_pixel_count == 0) {
			console_print_verbose("thread %d: tile level %d, tile %d (%d, %d): openslide.read_region() returned zeroes (empty tile)\n",
			                      logical_thread_index, level, tile_index, tile_x, tile_y);
			is_empty = true;
		}
	} else if (image->backend == IMAGE_BACKEND_DICOM) {
		u8* pixels = dicom_wsi_decode_tile_to_bgra(&image->dicom, level_image->pyramid_image_index, tile_index);
		if (pixels) {
			tile_buffer_release(temp_memory, pixel_memory_size);
			temp_memory = pixels;
		} else {
			failed = true;
		}
	} else if (image->backend == IMAGE_BACKEND_MRXS) {
		u8* pixels = mrxs_decode_tile_to_bgra(&image->mrxs, task->plane, level, tile_index);
		if (pixels) {
			tile_buffer_release(temp_memory, pixel_memory_size);
			temp_memory = pixels;
		} else {
			failed = true;
		}
	} else {
		console_print_error("thread %d: tile level %d, tile %d (%d, %d): unsupported image type\n",
		                    logical_thread_index, level, tile_index, tile_x, tile_y);
		failed = true;
	}

	if (temp_memory && task->invert_colors) {
		u32 pixel_count = level_image->tile_width * level_image->tile_height;
		u8* pos = temp_memory;
		for (u32 j = 0; j < pixel_count; ++j) {
			pos[0] = 255 - pos[0];
			pos[1] = 255 - pos[1];
			pos[2] = 255 - pos[2];
			pos += 4;
		}
	}

	if ((failed || is_empty) && temp_memory != NULL) {
		tile_buffer_release(temp_memory, pixel_memory_size);
		temp_memory = NULL;
	}

	tile_cache_result_t completion_task = {0};
	completion_task.plane = task->plane;
	completion_task.resource_id = task->resource_id;
	completion_task.pixel_memory = temp_memory;
	completion_task.tile_width = level_image->tile_width;
	completion_task.tile_height = level_image->tile_height;
	completion_task.level = level;
	completion_task.tile_index = tile_index;
	completion_task.want_gpu_residency = task->need_gpu_residency;
	completion_task.want_cpu_residency = task->need_cpu_residency;
	completion_task.failed = failed;
	completion_task.is_empty = is_empty;

	if (!tile_cache_post_load_result(image, &completion_task)) {
		if (completion_task.pixel_memory) {
			tile_buffer_release(completion_task.pixel_memory, pixel_memory_size);
		}
	}

	// NOTE: we guarantee existence of image_t until the jobs submitted from the main thread are done.
	// However, we will NOT wait for the completion queues to also be finished (usually the responsibility of the main thread).
	atomic_subtract(&image->refcount, task->refcount_to_decrement);
}

void load_tile_func(i32 logical_thread_index, void* userdata) {
	load_tile_task_t* task = (load_tile_task_t*)userdata;
	profiler_worker_begin(task->profile_capture_id, task->profile_queued_clock);
	load_tile_impl(logical_thread_index, userdata);
	profiler_worker_end();
}
