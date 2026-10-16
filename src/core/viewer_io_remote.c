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

#include "common.h"
#include "viewer.h"
#include "remote.h"
#include "jpeg_decoder.h"
#include "tile_cache.h"
#include "tile_buffer_pool.h"

void tiff_load_tile_batch_func(i32 logical_thread_index, void* userdata) {
	load_tile_task_batch_t* batch = (load_tile_task_batch_t*) userdata;
	load_tile_task_t* first_task = batch->tile_tasks;
	image_t* image = first_task->image;

	i32 refcount_decrement_amount = 0;
	for (i32 i = 0; i < batch->task_count; ++i) {
		load_tile_task_t *task = batch->tile_tasks + i;
		refcount_decrement_amount += task->refcount_to_decrement;
	}
	if (image->is_deleted) {
		// Early out to save time if the image was already closed/waiting for destruction
		atomic_subtract(&image->refcount, refcount_decrement_amount);
		return;
	}

	// Note: when the thread started up we allocated a large blob of memory for the thread to use privately
	// TODO: better/more explicit allocator (instead of some setting some hard-coded pointers)
//	thread_memory_t* thread_memory = (thread_memory_t*) thread_local_storage[logical_thread_index];
//	u8* temp_memory = (u8*) thread_memory->aligned_rest_of_thread_memory; //malloc(WSI_BLOCK_SIZE);
//	memset(temp_memory, 0xFF, WSI_BLOCK_SIZE);

	ASSERT(image->type == IMAGE_TYPE_WSI);
	if (image->backend == IMAGE_BACKEND_TIFF) {
		tiff_t* tiff = &image->tiff;

		if (tiff->is_remote) {

			i32 bytes_read = 0;
			i32 batch_size = batch->task_count;
			load_tile_task_t* active_tasks[TILE_LOAD_BATCH_MAX] = {0};
			i32 active_count = 0;
			i64 chunk_offsets[TILE_LOAD_BATCH_MAX];
			i64 chunk_sizes[TILE_LOAD_BATCH_MAX];
			i64 total_read_size = 0;
			for (i32 i = 0; i < batch_size; ++i) {
				load_tile_task_t* task = batch->tile_tasks + i;

				i32 level = task->level;
				level_image_t* level_image = image->level_images + level;
				i32 tile_index = task->tile_index;
				i32 tile_x = tile_index % level_image->width_in_tiles;
				i32 tile_y = tile_index / level_image->width_in_tiles;
				if (task->may_discard_if_stale &&
				    tile_cache_task_is_stale(image, level, tile_index, task->generation)) {
					if (!tile_cache_post_stale_result(image, task->resource_id, level, tile_index)) {
						tile_cache_cancel_decode(image, level, tile_index);
					}
					continue;
				}
				tiff_ifd_t* level_ifd = tiff->level_images_ifd + level_image->pyramid_image_index;
				u64 tile_offset = level_ifd->tile_offsets[tile_index];
				u64 chunk_size = level_ifd->tile_byte_counts[tile_index];

				// It doesn't make sense to ask for empty tiles, this should never happen!
				ASSERT(tile_offset != 0);
				ASSERT(chunk_size != 0);

				active_tasks[active_count] = task;
				chunk_offsets[active_count] = tile_offset;
				chunk_sizes[active_count] = chunk_size;
				total_read_size += chunk_size;
				++active_count;
			}
			if (active_count == 0) {
				atomic_subtract(&image->refcount, refcount_decrement_amount);
				return;
			}


//			u32 new_textures[TILE_LOAD_BATCH_MAX] = {0};

			// Note: First download everything, then decode and upload everything to the GPU.
			// It would be faster to pipeline this somehow.
			u8* read_buffer = download_remote_batch(tiff->location.hostname, tiff->location.portno,
			                                        tiff->location.filename,
			                                        chunk_offsets, chunk_sizes, active_count, &bytes_read, logical_thread_index);
			if (read_buffer && bytes_read > 0) {
				i64 content_offset = find_end_of_http_headers(read_buffer, bytes_read);
				i64 content_length = bytes_read - content_offset;
				u8* content = read_buffer + content_offset;

				// TODO: better way to check the real content length?
				if (content_length >= total_read_size) {

					i64 chunk_offset_in_read_buffer = 0;
					for (i32 i = 0; i < active_count; ++i) {
						load_tile_task_t* task = active_tasks[i];
						level_image_t* level_image = image->level_images + task->level;
						u8* current_chunk = content + chunk_offset_in_read_buffer;
						chunk_offset_in_read_buffer += chunk_sizes[i];

						size_t pixel_memory_size = level_image->tile_width * level_image->tile_height * BYTES_PER_PIXEL;
						u8* pixel_memory = tile_buffer_acquire(pixel_memory_size);
						if (!pixel_memory) {
							tile_cache_cancel_decode(image, task->level, task->tile_index);
							continue;
						}
						memset(pixel_memory, 0xFF, pixel_memory_size);

						tiff_ifd_t* level_ifd = tiff->level_images_ifd + level_image->pyramid_image_index;
						u8* jpeg_tables = level_ifd->jpeg_tables;
						u64 jpeg_tables_length = level_ifd->jpeg_tables_length;

						if (content[0] == 0xFF && content[1] == 0xD9) {
							// JPEG stream is empty
						} else {
							if (jpeg_decode_tile(jpeg_tables, jpeg_tables_length, current_chunk, chunk_sizes[i],
							                     pixel_memory, (level_ifd->color_space == TIFF_PHOTOMETRIC_YCBCR))) {
//		                    console_print("thread %d: successfully decoded level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);
							} else {
								i32 tile_x = task->tile_index % level_image->width_in_tiles;
								i32 tile_y = task->tile_index / level_image->width_in_tiles;
								console_print_error("[thread %d] failed to decode level %d, tile (%d, %d)\n", logical_thread_index, task->level, tile_x, tile_y);
							}
						}

						tile_cache_result_t completion_task = {};
						completion_task.resource_id = task->resource_id;
						completion_task.pixel_memory = pixel_memory;
						// TODO: check if we need to pass the tile height here too?
						completion_task.tile_width = level_image->tile_width;
						completion_task.tile_height = level_image->tile_height;
						completion_task.level = task->level;
						completion_task.tile_index = task->tile_index;
						completion_task.want_gpu_residency = task->need_gpu_residency;
						completion_task.want_cpu_residency = task->need_cpu_residency;

						if (!tile_cache_post_load_result(image, &completion_task)) {
							if (completion_task.pixel_memory) {
								tile_buffer_release(completion_task.pixel_memory, pixel_memory_size);
							}
						}

						//new_textures[i] = renderer_create_texture(pixel_memory, TILE_DIM, TILE_DIM, RENDERER_PIXEL_FORMAT_BGRA);
					}

				}

			}

#if 0
			// Note: publishing the texture handle lets the main thread know that the texture
			// is ready for use. However, the texture may still not *actually* be available until the renderer has
			// finished the work, so to be 100% sure we wait before publishing the texture
			// We only want to wait once, so we store the new texture handles temporarily while the
			// rest of the batch is still loading.
			// Better would be to flag the texture as 'ready for use' as soon as it's done, but I don't know
			// how we could query this / be notified of this. (maybe an optimization for later?)
			renderer_finish();
			write_barrier;
			for (i32 i = 0; i < batch_size; ++i) {
				load_tile_task_t* task = batch->tile_tasks + i;
				tile_cache_store_gpu_texture(image, task->level, task->tile_index, new_textures[i]);
			}
#endif

			free(read_buffer);
		}

	}

    // NOTE: we guarantee existence of image_t until the jobs submitted from the main thread are done.
    // However, we will NOT wait for the completion queues to also be finished (usually the responsibility of the main thread).
    // This means that when we receive the completion tasks on the main thread, we have to check if the image is still valid.
    atomic_subtract(&image->refcount, refcount_decrement_amount);

}
