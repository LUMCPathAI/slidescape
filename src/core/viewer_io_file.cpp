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

#include <ctype.h>
#include "viewer.h"
#include "dicom_wsi.h"
#include "stringutils.h"
#include "listing.h"
#include "stb_image.h"
#include "remote.h"
#include "jpeg_decoder.h"
#include "ssi_transport.h"
#include "tile_cache.h"

#include "gui.h" // for global data, TODO: refactor

static void remote_jpeg_post_tile_result(load_tile_task_t* task, u8* pixel_memory, bool failed, bool is_empty) {
	image_t* image = task->image;
	level_image_t* level_image = image->level_images + task->level;

	tile_cache_result_t completion_task = {};
	completion_task.resource_id = task->resource_id;
	completion_task.pixel_memory = pixel_memory;
	completion_task.tile_width = level_image->tile_width;
	completion_task.tile_height = level_image->tile_height;
	completion_task.level = task->level;
	completion_task.tile_index = task->tile_index;
	completion_task.want_gpu_residency = task->need_gpu_residency;
	completion_task.want_cpu_residency = task->need_cpu_residency;
	completion_task.failed = failed;
	completion_task.retryable_failure = failed && image->backend == IMAGE_BACKEND_SSI;
	completion_task.is_empty = is_empty;

	if (!tile_cache_post_load_result(image, &completion_task)) {
		if (completion_task.pixel_memory) {
			free(completion_task.pixel_memory);
		}
	}
}

typedef struct slide_score_worker_connection_t {
	tls_connection_t* connection;
	char hostname[256];
} slide_score_worker_connection_t;

static thread_local slide_score_worker_connection_t slide_score_worker_connection = {};

static tls_connection_t* slide_score_get_worker_connection(const char* hostname) {
	if (slide_score_worker_connection.connection &&
	    strcmp(slide_score_worker_connection.hostname, hostname) != 0) {
		remote_connection_close_and_free(slide_score_worker_connection.connection);
		slide_score_worker_connection = {};
	}
	if (!slide_score_worker_connection.connection) {
		slide_score_worker_connection.connection = remote_connection_open(hostname, 443);
		if (slide_score_worker_connection.connection) {
			copy_cstring(slide_score_worker_connection.hostname, hostname, sizeof(slide_score_worker_connection.hostname));
		}
	}
	return slide_score_worker_connection.connection;
}

static void slide_score_drop_worker_connection(void) {
	if (slide_score_worker_connection.connection) {
		remote_connection_close_and_free(slide_score_worker_connection.connection);
		slide_score_worker_connection = {};
	}
}

void slide_score_load_tile_batch_func(i32 logical_thread_index, void* userdata) {
	load_tile_task_batch_t* batch = (load_tile_task_batch_t*) userdata;
	load_tile_task_t* first_task = batch->tile_tasks;
	image_t* image = first_task->image;

	i32 refcount_decrement_amount = 0;
	for (i32 i = 0; i < batch->task_count; ++i) {
		refcount_decrement_amount += batch->tile_tasks[i].refcount_to_decrement;
	}
	if (image->is_deleted) {
		atomic_subtract(&image->refcount, refcount_decrement_amount);
		return;
	}

	ASSERT(image->backend == IMAGE_BACKEND_SLIDE_SCORE);
	slide_score_remote_image_t* remote = &image->slide_score;

	char cookie[512];
	snprintf(cookie, sizeof(cookie), "t=%s", remote->tile_server.cookie_part);

	for (i32 i = 0; i < batch->task_count; ++i) {
		load_tile_task_t* task = batch->tile_tasks + i;
		level_image_t* level_image = image->level_images + task->level;
		i32 tile_x = task->tile_index % level_image->width_in_tiles;
		i32 tile_y = task->tile_index / level_image->width_in_tiles;
		if (task->may_discard_if_stale &&
		    tile_cache_task_is_stale(image, task->level, task->tile_index, task->generation)) {
			if (!tile_cache_post_stale_result(image, task->resource_id, task->level, task->tile_index)) {
				tile_cache_cancel_decode(image, task->level, task->tile_index);
			}
			continue;
		}
		size_t pixel_memory_size = level_image->tile_width * level_image->tile_height * BYTES_PER_PIXEL;
		u8* pixel_memory = (u8*)malloc(pixel_memory_size);
		memset(pixel_memory, image->is_background_black ? 0 : 0xFF, pixel_memory_size);

		bool failed = false;
		char path[512];
		if (remote->use_qupath_tile_endpoint) {
			slide_score_build_qupath_tile_path(path, sizeof(path), remote, task->level, tile_x, tile_y,
			                                   level_image->tile_width, level_image->tile_height,
			                                   level_image->width_in_pixels, level_image->height_in_pixels);
		} else {
			snprintf(path, sizeof(path), "/i/%d/%s/i_files/%d/%d_%d.jpeg",
			         remote->image_id, remote->tile_server.url_part, level_image->pyramid_image_index,
			         tile_x, tile_y);
		}

		http_response_t* response = NULL;
		for (i32 attempt = 0; attempt < 2; ++attempt) {
			tls_connection_t* connection = slide_score_get_worker_connection(remote->client.server_name);
			if (!connection) break;
			response = remote_connection_request(connection, remote->client.server_name, path,
			                                     remote->client.api_key, cookie, false);
			if (response && response->status_code > 0) {
				break;
			}
			if (response) {
				http_response_destroy(response);
				response = NULL;
			}
			slide_score_drop_worker_connection();
		}

		if (response) {
			if (response && response->status_code == 200 && response->content_length > 0) {
				i32 jpeg_width = 0;
				i32 jpeg_height = 0;
				i32 channels_in_file = 0;
				u8* decoded = jpeg_decode_image((u8*)response->buffer.data, (u32)response->content_length,
				                                &jpeg_width, &jpeg_height, &channels_in_file);
				if (decoded) {
					i32 copy_width = ATMOST(jpeg_width, (i32)level_image->tile_width);
					i32 copy_height = ATMOST(jpeg_height, (i32)level_image->tile_height);
					i32 dest_pitch = level_image->tile_width * BYTES_PER_PIXEL;
					i32 src_pitch = jpeg_width * BYTES_PER_PIXEL;
					for (i32 row = 0; row < copy_height; ++row) {
						memcpy(pixel_memory + row * dest_pitch, decoded + row * src_pitch, copy_width * BYTES_PER_PIXEL);
					}
					free(decoded);
				} else {
					failed = true;
				}
			} else {
				i32 status_code = response ? response->status_code : 0;
				console_print_error("[thread %d] Slide Score tile request failed: HTTP %d, level %d tile (%d, %d)\n",
				                    logical_thread_index, status_code, task->level, tile_x, tile_y);
				if (remote->use_qupath_tile_endpoint) {
					console_print_error("Slide Score QuPath tile endpoint failed for '%s'. The link may be expired or this server may not support unauthenticated raw tile access.\n", path);
				}
				failed = true;
			}
			if (response) http_response_destroy(response);
		} else {
			failed = true;
		}

		if (failed) {
			free(pixel_memory);
			pixel_memory = NULL;
		}
		remote_jpeg_post_tile_result(task, pixel_memory, failed, false);
	}

	atomic_subtract(&image->refcount, refcount_decrement_amount);
}

typedef struct ssi_tile_stats_t {
	i32 request_count;
	i32 useful_tile_count;
	i32 rectangle_tile_count;
	i32 decoded_count;
	i32 stale_count;
	i32 failed_count;
	i32 window_request_count;
	i32 window_useful_tile_count;
	i32 window_rectangle_tile_count;
	i32 window_decoded_tile_count;
	i32 window_request_latency_ms;
	i32 window_response_kib;
	i64 window_start_clock;
	i64 window_last_completion_clock;
} ssi_tile_stats_t;

static ssi_tile_stats_t ssi_tile_stats;
static platform_mutex_t ssi_tile_stats_mutex = PLATFORM_MUTEX_INITIALIZER;

static void ssi_record_tile_result(i32 useful_tiles, i32 rectangle_tiles,
		i32 decoded_tiles, i32 stale_tiles, i32 failed_tiles, i64 request_start_clock,
		i32 latency_ms, size_t response_size) {
	platform_mutex_lock(&ssi_tile_stats_mutex);
	i64 now = get_clock();
	bool start_new_window = ssi_tile_stats.window_request_count == 0 ||
		get_seconds_elapsed(ssi_tile_stats.window_last_completion_clock, now) > 1.0f;
	if (start_new_window) {
		ssi_tile_stats.window_request_count = 0;
		ssi_tile_stats.window_useful_tile_count = 0;
		ssi_tile_stats.window_rectangle_tile_count = 0;
		ssi_tile_stats.window_decoded_tile_count = 0;
		ssi_tile_stats.window_request_latency_ms = 0;
		ssi_tile_stats.window_response_kib = 0;
		ssi_tile_stats.window_start_clock = request_start_clock;
	} else if (request_start_clock < ssi_tile_stats.window_start_clock) {
		ssi_tile_stats.window_start_clock = request_start_clock;
	}
	ssi_tile_stats.window_last_completion_clock = now;
	++ssi_tile_stats.request_count;
	ssi_tile_stats.useful_tile_count += useful_tiles;
	ssi_tile_stats.rectangle_tile_count += rectangle_tiles;
	ssi_tile_stats.decoded_count += decoded_tiles;
	ssi_tile_stats.stale_count += stale_tiles;
	ssi_tile_stats.failed_count += failed_tiles;
	++ssi_tile_stats.window_request_count;
	ssi_tile_stats.window_useful_tile_count += useful_tiles;
	ssi_tile_stats.window_rectangle_tile_count += rectangle_tiles;
	ssi_tile_stats.window_decoded_tile_count += decoded_tiles;
	ssi_tile_stats.window_request_latency_ms += latency_ms;
	ssi_tile_stats.window_response_kib += (i32)(response_size / 1024);
	bool print_stats = ssi_tile_stats.window_request_count % 8 == 0;
	if (print_stats) {
		float elapsed = get_seconds_elapsed(ssi_tile_stats.window_start_clock, now);
		float throughput = elapsed > 0.0f
			? (float)ssi_tile_stats.window_decoded_tile_count / elapsed : 0.0f;
		float average_rectangle = (float)ssi_tile_stats.window_rectangle_tile_count /
		                      (float)ssi_tile_stats.window_request_count;
		float average_useful = (float)ssi_tile_stats.window_useful_tile_count /
		                       (float)ssi_tile_stats.window_request_count;
		float usefulness = ssi_tile_stats.window_rectangle_tile_count > 0
			? (float)ssi_tile_stats.window_useful_tile_count * 100.0f /
			  (float)ssi_tile_stats.window_rectangle_tile_count : 0.0f;
		float average_latency = (float)ssi_tile_stats.window_request_latency_ms /
		                        (float)ssi_tile_stats.window_request_count;
		float average_kib = (float)ssi_tile_stats.window_response_kib /
		                    (float)ssi_tile_stats.window_request_count;
		ssi_transport_stats_t transport_stats = {};
		ssi_transport_get_stats(&transport_stats);
		console_print_verbose(
			"SSI tile stats: requests=%d, useful tiles=%d, "
			"rectangle tiles=%d (decoded=%d, stale=%d, failed=%d), "
			"window=%.1f decoded tiles/s, average rectangle=%.1f tiles, "
			"average useful=%.1f tiles (%.0f%%), "
			"average request=%.1f ms, average response=%.1f KiB; "
			"pool opens=%d, reuses=%d, waits=%d; "
			"control reuses=%d, fallback handshakes=%d\n",
			ssi_tile_stats.request_count, ssi_tile_stats.useful_tile_count,
			ssi_tile_stats.rectangle_tile_count,
			ssi_tile_stats.decoded_count, ssi_tile_stats.stale_count,
			ssi_tile_stats.failed_count, throughput, average_rectangle, average_useful,
			usefulness, average_latency, average_kib,
			transport_stats.pool_opens, transport_stats.pool_reuses,
			transport_stats.pool_waits, transport_stats.control_reuses,
			transport_stats.control_fallback_handshakes);
	}
	platform_mutex_unlock(&ssi_tile_stats_mutex);
}

static bool ssi_response_body_contains(const http_response_t* response, const char* pattern) {
	if (!response || !response->buffer.data || !pattern || !pattern[0]) return false;
	size_t pattern_size = strlen(pattern);
	for (size_t i = 0; i + pattern_size <= response->content_length; ++i) {
		bool matches = true;
		for (size_t j = 0; j < pattern_size; ++j) {
			if (tolower(response->buffer.data[i + j]) != tolower((u8)pattern[j])) {
				matches = false;
				break;
			}
		}
		if (matches) return true;
	}
	return false;
}

static bool ssi_response_requires_reauthentication(const http_response_t* response) {
	if (!response) return false;
	if (response->status_code == 401 || response->status_code == 403) return true;
	return response->status_code == 400 &&
	       (ssi_response_body_contains(response, "token") ||
	        ssi_response_body_contains(response, "session"));
}

void ssi_load_tile_batch_func(i32 logical_thread_index, void* userdata) {
	load_tile_task_batch_t* batch = (load_tile_task_batch_t*)userdata;
	image_t* image = batch->tile_tasks[0].image;
	i32 refcount_decrement = 0;
	for (i32 i = 0; i < batch->task_count; ++i) {
		refcount_decrement += batch->tile_tasks[i].refcount_to_decrement;
	}
	if (image->is_deleted) {
		atomic_subtract(&image->refcount, refcount_decrement);
		return;
	}
	ASSERT(image->backend == IMAGE_BACKEND_SSI);
	ssi_remote_image_t* remote = &image->ssi;

	/* Defensive fallback for callers that did not use the compact SSI grouper. */
	level_image_t* first_level = image->level_images + batch->tile_tasks[0].level;
	i32 first_x = batch->tile_tasks[0].tile_index % first_level->width_in_tiles;
	i32 first_y = batch->tile_tasks[0].tile_index / first_level->width_in_tiles;
	for (i32 i = 1; i < batch->task_count; ++i) {
		load_tile_task_t* task = batch->tile_tasks + i;
		level_image_t* level = image->level_images + task->level;
		i32 tile_x = task->tile_index % level->width_in_tiles;
		i32 tile_y = task->tile_index / level->width_in_tiles;
		if (task->level != batch->tile_tasks[0].level ||
		    tile_x / 4 != first_x / 4 || tile_y / 2 != first_y / 2) {
			for (i32 single_index = 0; single_index < batch->task_count; ++single_index) {
				load_tile_task_batch_t single = {0};
				single.task_count = 1;
				single.tile_tasks[0] = batch->tile_tasks[single_index];
				ssi_load_tile_batch_func(logical_thread_index, &single);
			}
			return;
		}
	}

	load_tile_task_t* active_tasks[TILE_LOAD_BATCH_MAX] = {0};
	i32 active_count = 0;
	for (i32 i = 0; i < batch->task_count; ++i) {
		load_tile_task_t* task = batch->tile_tasks + i;
		if (task->may_discard_if_stale &&
		    tile_cache_task_is_stale(image, task->level, task->tile_index, task->generation)) {
			if (!tile_cache_post_stale_result(image, task->resource_id, task->level,
			                                  task->tile_index)) {
				tile_cache_cancel_decode(image, task->level, task->tile_index);
			}
			continue;
		}
		active_tasks[active_count++] = task;
	}
	if (active_count == 0) {
		atomic_subtract(&image->refcount, refcount_decrement);
		return;
	}

	level_image_t* level = image->level_images + active_tasks[0]->level;
	ssi_level_geometry_t geometry = {0};
	bool geometry_valid = ssi_get_level_geometry(&remote->properties,
	                                             active_tasks[0]->level, &geometry);
	i32 active_tile_x = active_tasks[0]->tile_index % level->width_in_tiles;
	i32 active_tile_y = active_tasks[0]->tile_index / level->width_in_tiles;
	i32 block_x_min = (active_tile_x / 4) * 4;
	i32 block_y_min = (active_tile_y / 2) * 2;
	i32 tile_x_min = ATLEAST(block_x_min, geometry.tile_x_min);
	i32 tile_y_min = ATLEAST(block_y_min, geometry.tile_y_min);
	i32 tile_x_max = ATMOST(block_x_min + 3, geometry.tile_x_max);
	i32 tile_y_max = ATMOST(block_y_min + 1, geometry.tile_y_max);
	i32 rectangle_tile_count = geometry_valid
		? (tile_x_max - tile_x_min + 1) * (tile_y_max - tile_y_min + 1) : 0;

	i64 request_start_clock = get_clock();
	char path[1024];
	bool failed = !geometry_valid || !ssi_build_tile_rectangle_path(path, sizeof(path), remote,
		active_tasks[0]->level, tile_x_min, tile_y_min, tile_x_max, tile_y_max);
	http_response_t* response = NULL;
	for (i32 attempt = 0; !failed && attempt < 2; ++attempt) {
		remote_request_options_t options = {};
		options.read_poll_timeout_ms = 250;
		options.write_timeout_seconds = 10.0f;
		options.idle_read_timeout_seconds = 30.0f;
		options.total_timeout_seconds = 60.0f;
		options.max_response_body_size = MEGABYTES(32);
		response = ssi_transport_request(
			&remote->endpoint, image, SSI_TRANSPORT_WAIT_FOR_POOL,
			REMOTE_HTTP_GET, path, "application/octet-stream", NULL,
			NULL, 0, &options);
		if (response && response->status_code == 429 && attempt == 0) {
			http_response_destroy(response);
			response = NULL;
			platform_sleep(100);
			continue;
		}
		if (response && response->status_code > 0) {
			break;
		}
		if (response) {
			http_response_destroy(response);
			response = NULL;
		}
	}
	if (!response || response->status_code != 200 || response->content_length == 0) {
		failed = true;
		if (ssi_response_requires_reauthentication(response)) {
			atomic_compare_exchange(&remote->reauthentication_state, 1, 0);
		}
		console_print_error("[thread %d] SSI rectangle request failed: HTTP %d, "
		                    "level %d tiles (%d,%d)-(%d,%d).\n", logical_thread_index,
		                    response ? response->status_code : 0, active_tasks[0]->level,
		                    tile_x_min, tile_y_min, tile_x_max, tile_y_max);
	}

	i32 stale_after_request = 0;
	for (i32 i = 0; i < active_count; ++i) {
		load_tile_task_t* task = active_tasks[i];
		if (task->may_discard_if_stale &&
		    tile_cache_task_is_stale(image, task->level, task->tile_index, task->generation)) {
			if (!tile_cache_post_stale_result(image, task->resource_id, task->level,
			                                  task->tile_index)) {
				tile_cache_cancel_decode(image, task->level, task->tile_index);
			}
			active_tasks[i] = NULL;
			++stale_after_request;
		}
	}

	i32 decoded_width = 0;
	i32 decoded_height = 0;
	i32 channels = 0;
	u8* decoded = NULL;
	if (!failed && stale_after_request < active_count) {
		decoded = jpeg_decode_image((u8*)response->buffer.data,
		                            (u32)response->content_length,
		                            &decoded_width, &decoded_height, &channels);
		if (!decoded) failed = true;
	}

	i32 decoded_tile_count = 0;
	i32 failed_tile_count = 0;
	for (i32 i = 0; i < active_count; ++i) {
		load_tile_task_t* task = active_tasks[i];
		if (!task) continue;
		size_t pixel_size = level->tile_width * level->tile_height * BYTES_PER_PIXEL;
		u8* pixels = failed ? NULL : (u8*)malloc(pixel_size);
		bool tile_failed = failed || pixels == NULL;
		i32 tile_x = task->tile_index % level->width_in_tiles;
		i32 tile_y = task->tile_index / level->width_in_tiles;
		if (!tile_failed) {
			memset(pixels, image->is_background_black ? 0 : 0xff, pixel_size);
			i32 source_x = (tile_x - tile_x_min) * level->tile_width;
			i32 source_y = (tile_y - tile_y_min) * level->tile_height;
			i32 copy_width = ATMOST((i32)level->tile_width, decoded_width - source_x);
			i32 copy_height = ATMOST((i32)level->tile_height, decoded_height - source_y);
			if (copy_width <= 0 || copy_height <= 0) {
				tile_failed = true;
			} else {
				for (i32 row = 0; row < copy_height; ++row) {
					memcpy(pixels + row * level->tile_width * BYTES_PER_PIXEL,
					       decoded + ((source_y + row) * decoded_width + source_x) * BYTES_PER_PIXEL,
					       copy_width * BYTES_PER_PIXEL);
				}
			}
		}
		if (tile_failed) {
			free(pixels);
			pixels = NULL;
			++failed_tile_count;
		} else {
			++decoded_tile_count;
			if (atomic_compare_exchange(&remote->first_tile_decoded_reported, 1, 0)) {
				ssi_transport_stats_t transport_stats = {};
				ssi_transport_get_stats(&transport_stats);
				console_print_verbose(
					"SSI first tile decoded: %d ms after viewer ready "
					"(level=%d, tile=%d,%d; pool opens=%d, reuses=%d, waits=%d)\n",
					(i32)(get_seconds_elapsed(remote->viewer_ready_clock, get_clock()) * 1000.0f + 0.5f),
					task->level, tile_x, tile_y,
					transport_stats.pool_opens, transport_stats.pool_reuses,
					transport_stats.pool_waits);
			}
		}
		remote_jpeg_post_tile_result(task, pixels, tile_failed, false);
	}
	free(decoded);
	i32 request_latency_ms = (i32)(
		get_seconds_elapsed(request_start_clock, get_clock()) * 1000.0f + 0.5f);
	ssi_record_tile_result(active_count, rectangle_tile_count, decoded_tile_count,
	                       stale_after_request, failed_tile_count,
	                       request_start_clock, request_latency_ms,
	                       response ? response->content_length : 0);
	if (response) http_response_destroy(response);
	atomic_subtract(&image->refcount, refcount_decrement);
}



bool viewer_load_new_image(app_state_t* app_state, file_info_t* file, directory_info_t* directory, u32 filetype_hint) {
	// assume it is an image file?
	bool is_base_image = filetype_hint != FILETYPE_HINT_OVERLAY;
	if (is_base_image) {
		unload_all_images(app_state);
		// Unload any old annotations if necessary
		unload_and_reinit_annotations(&app_state->scene.annotation_set);
	}
	load_next_image_as_overlay = false; // reset after use (don't keep stacking on more overlays unintendedly)
	image_t* image = load_image_from_file(app_state, file, directory, filetype_hint);
	if (image->is_valid) {
		add_image(app_state, image, is_base_image, !is_base_image);
		if (is_base_image && image->backend == IMAGE_BACKEND_DICOM &&
		    arrlen(image->dicom.slides) > 1) {
			show_slide_tray_window = true;
		}

        if (is_base_image) {
            annotation_set_t* annotation_set = &app_state->scene.annotation_set;
//            unload_and_reinit_annotations(annotation_set); // no need, already unloaded!
            annotation_set->mpp = V2F(image->mpp_x, image->mpp_y);

            // Check if there is an associated annotation file.
            char temp_filename[512];
            temp_filename[0] = '\0';
            const char* annotation_name = file->type == VIEWER_FILE_TYPE_ZIP ? image->name : file->filename_in_directory;
            const char* prefix = (app_state->annotation_directory[0] != '\0') ? app_state->annotation_directory : file->filename_prefix;
            snprintf(temp_filename, sizeof(temp_filename), "%s%s", prefix, annotation_name);
            bool were_annotations_loaded = false;

			const char* annotation_extensions[] = { "xml", "geojson", "json" };
			for (i32 extension_index = 0; extension_index < COUNT(annotation_extensions); ++extension_index) {
				snprintf(temp_filename, sizeof(temp_filename), "%s%s", prefix, annotation_name);
				replace_file_extension(temp_filename, sizeof(temp_filename), annotation_extensions[extension_index]);
				if (file_exists(temp_filename)) {
					if (were_annotations_loaded) {
						console_print("Ignoring additional annotation file: '%s'\n", temp_filename);
						continue;
					}
					console_print("Found annotations: '%s'\n", temp_filename);
					if (load_annotations(app_state, temp_filename)) {
						were_annotations_loaded = true;
						// Don't hide annotations when first loading the slide, that might lead the user to believe that there are none.
						app_state->scene.enable_annotations = true;
					}
				}
			}

            if (app_state->remember_annotation_groups_as_template && !were_annotations_loaded && app_state->scene.annotation_set_template.is_valid) {
                annotation_set_init_from_template(annotation_set, &app_state->scene.annotation_set_template);
            }

        }

		console_print("Loaded '%s'\n", file->full_filename);
		if (image->backend == IMAGE_BACKEND_ISYNTAX) {
			console_print("   iSyntax: loading took %g seconds\n", image->isyntax.loading_time);
		}
		return true;

	} else {
		return false;
	}
}


bool load_generic_file(app_state_t* app_state, const char* filename, u32 filetype_hint) {
	file_info_t file = viewer_get_file_info(filename);
	bool success = false;
	if (file.is_valid) {
		if (file.is_regular_file) {
			if (file.type == VIEWER_FILE_TYPE_DICOM) {
				success = viewer_load_new_image(app_state, &file, NULL, filetype_hint);
			} else if (file.is_image) {
				success = viewer_load_new_image(app_state, &file, NULL, filetype_hint);
			} else {
				if (file.type == VIEWER_FILE_TYPE_XML || file.type == VIEWER_FILE_TYPE_JSON) {
					// TODO: how to get the correct scale factor for the annotations?
					// Maybe a placeholder value, which gets updated based on the scale of the scene image?
					annotation_set_t* annotation_set = &app_state->scene.annotation_set;
					unload_and_reinit_annotations(annotation_set);
					if (arrlen(app_state->loaded_images) > 0) {
						image_t* image = app_state->loaded_images[0];
						annotation_set->mpp = V2F(image->mpp_x, image->mpp_y);
					} else {
						annotation_set->mpp = V2F(0.25f, 0.25f);
					}
					success = load_annotations(app_state, filename);
				}
			}
		} else if (file.is_directory) {
			directory_info_t directory = viewer_get_dicom_directory_info(filename);
			if (directory.is_valid) {
                console_print("Trying to open a directory '%s'\n", filename);
                if (directory.contains_dicom_files) {
                    file.type = VIEWER_FILE_TYPE_DICOM;
					success = viewer_load_new_image(app_state, &file, &directory, filetype_hint);
				} else if (directory.contains_mrxs_files) {
                    file.type = VIEWER_FILE_TYPE_MRXS;
                    success = viewer_load_new_image(app_state, &file, &directory, filetype_hint);
                }
			}
			viewer_directory_info_destroy(&directory); // TODO: transfer ownership of directory structure info?
		}
	}

	if (!success) {
		console_print_error("Could not load '%s'\n", filename);
		gui_add_modal_message_popup("Error##load_generic_file", "Could not load '%s'.\n", filename);
	}
	return success;

}

bool load_dicom_file_from_directory(app_state_t* app_state, const char* filename,
		const char* directory_path, u32 filetype_hint) {
	// A ZIP is the virtual root of a DICOM study. Preserve its selected member on reload.
	file_info_t container = viewer_get_file_info(directory_path);
	if (container.is_valid && container.type == VIEWER_FILE_TYPE_ZIP) {
		copy_cstring(container.zip_member, filename, sizeof(container.zip_member));
		return viewer_load_new_image(app_state, &container, NULL, filetype_hint);
	}
	file_info_t file = viewer_get_file_info(filename);
	if (!file.is_valid || !file.is_regular_file || file.type != VIEWER_FILE_TYPE_DICOM) {
		console_print_error("Could not load DICOM file '%s'\n", filename);
		gui_add_modal_message_popup("Error##load_dicom_file_from_directory",
		                            "Could not load '%s'.\n", filename);
		return false;
	}

	directory_info_t directory = viewer_get_dicom_directory_info(directory_path);
	bool success = directory.is_valid && directory.contains_dicom_files &&
	               viewer_load_new_image(app_state, &file, &directory, filetype_hint);
	viewer_directory_info_destroy(&directory);
	if (!success) {
		console_print_error("Could not load '%s' from DICOM directory '%s'\n",
		                    filename, directory_path);
		gui_add_modal_message_popup("Error##load_dicom_file_from_directory",
		                            "Could not load '%s'.\n", filename);
	}
	return success;
}

const char* get_active_directory(app_state_t* app_state) {
	if (arrlen(app_state->loaded_images) > 0) {
		for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
			image_t* image = app_state->loaded_images[i];
			if (image->is_local) {
				return image->directory;
			}
		}
	} else {
        if (strlen(app_state->last_active_directory) > 0) {
            return app_state->last_active_directory;
        }
    }
	return get_default_save_directory();
}

const char* get_annotation_directory(app_state_t* app_state) {
    if (app_state->is_annotation_directory_set) {
        return app_state->annotation_directory;
    } else {
        return get_active_directory(app_state);
    }
}

void set_annotation_directory(app_state_t* app_state, const char* path) {
	copy_cstring(app_state->annotation_directory, path, COUNT(app_state->annotation_directory)-2);
	i32 prefix_len = (i32)strlen(app_state->annotation_directory);
	if (prefix_len > 0) {
		// discard folder names "." and ".."
		char* folder_name = (char*)one_past_last_slash(app_state->annotation_directory, prefix_len);
		if (strcmp(folder_name, ".") == 0) {
			folder_name[0] = '\0';
			prefix_len -= 1;
		} else if (strcmp(folder_name, "..") == 0) {
			folder_name[0] = '\0';
			prefix_len -= 2;
		}
	}
	if (prefix_len > 0) {
		// add trailing slash
		if (app_state->annotation_directory[prefix_len-1] != '/' && app_state->annotation_directory[prefix_len-1] != '\\') {
			app_state->annotation_directory[prefix_len++] = PATH_SEP[0];
		}
	}
	app_state->is_annotation_directory_set = true;
}


image_t* load_image_from_file(app_state_t* app_state, file_info_t* file, directory_info_t* directory, u32 filetype_hint) {
	if (!is_openslide_available && !is_openslide_loading_done) {
#if DO_DEBUG
		console_print("Waiting for OpenSlide to finish loading...\n");
#endif
		thread_pool_wait_for_completion(&global_thread_pool);
	}
	if ((file->type == VIEWER_FILE_TYPE_DICOM || file->type == VIEWER_FILE_TYPE_ZIP) &&
	    !is_dicom_available && !is_dicom_loading_done) {
#if DO_DEBUG
		console_print("Waiting for DICOM support to finish loading...\n");
#endif
		thread_pool_wait_for_completion(&global_thread_pool);
		if (!is_dicom_loading_done) {
			// Headless startup does not enqueue the background DICOM initialization task.
			is_dicom_available = dicom_init();
			is_dicom_loading_done = true;
		}
	}

	image_load_options_t options = {};
	options.is_overlay = (filetype_hint == FILETYPE_HINT_OVERLAY);
	options.use_builtin_tiff_backend = app_state->use_builtin_tiff_backend;
	options.use_native_mrxs_backend = global_use_native_mrxs_backend;
	options.openslide_available = is_openslide_available;
	options.openslide_loading_done = is_openslide_loading_done;
	options.resource_id = global_next_resource_id++;
	options.thread_pool = &global_thread_pool;
	if (options.is_overlay && arrlen(app_state->loaded_images)) {
		options.parent_image = app_state->loaded_images[0];
	}
	return image_load_from_file(file, directory, &options);
}
