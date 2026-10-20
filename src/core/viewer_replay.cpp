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

#include "viewer_replay.h"
#include "viewer.h"
#include "gui.h"
#include "replay.h"
#include "profiler.h"
#include "tile_cache.h"
#include "tile_buffer_pool.h"

static replay_t playback;
static bool playing;
static int exit_code;
static FILE* recording;
static double recording_origin;
static bool recording_waiting;
static bool have_recorded_camera;
static float recorded_camera[3];
static bool camera_pending;
static float pending_camera[3];
static bool pending_zoom;
static profiler_metrics_t metrics;

static double replay_clock(void) {
	static i64 epoch;
	if (!epoch) {
		epoch = get_clock();
	}
	return get_seconds_elapsed(epoch, get_clock());
}

static void record_command(replay_command_t* command) {
	if (recording && !replay_write_command(recording, command)) {
		console_print_error("Could not write replay recording.\n");
		fclose(recording);
		recording = NULL;
		exit_code = 1;
	}
}

void viewer_replay_record_open(const char* path) {
	if (!recording) {
		return;
	}
	replay_command_t command = {};
	command.timestamp = (float)(replay_clock() - recording_origin);
	command.kind = REPLAY_OPEN;
	command.path = (char*)path;
	record_command(&command);
	command.kind = REPLAY_WAIT;
	record_command(&command);
	recording_waiting = true;
	have_recorded_camera = false;
}

void viewer_replay_record_close(void) {
	if (!recording) {
		return;
	}
	if (recording_waiting) {
		// An interrupted open is outside the minimal recorder's synchronized workflow.
		recording_origin = replay_clock();
		recording_waiting = false;
	}
	replay_command_t command = {};
	command.timestamp = (float)(replay_clock() - recording_origin);
	command.kind = REPLAY_CLOSE;
	record_command(&command);
	have_recorded_camera = false;
}

bool viewer_replay_init(app_state_t* app) {
	if (app->command.replay_path) {
		// Read into mem_t without the fatal short-read behavior of platform_read_entire_file().
		file_stream_t file = file_stream_open_for_reading(app->command.replay_path);
		mem_t* buffer = NULL;
		if (file) {
			i64 size = file_stream_get_filesize(file);
			if (size >= 12 && size <= REPLAY_MAX_FILE_SIZE) {
				buffer = (mem_t*)calloc(1, sizeof(mem_t) + (size_t)size);
				if (buffer) {
					buffer->capacity = (size_t)size;
					buffer->len = file_stream_read(buffer->data, (size_t)size, file);
					if (buffer->len != (size_t)size) {
						free(buffer);
						buffer = NULL;
					}
				}
			}
			file_stream_close(file);
		}
		if (!replay_init(&playback, buffer)) {
			console_print_error("Replay: %s.\n", playback.error);
			exit_code = 1;
			return false;
		}
		playing = true;
		app->allow_idling_next_frame = false;
	}
	if (app->command.record_replay_path) {
		recording = fopen(app->command.record_replay_path, "wb");
		if (!recording || !replay_write_header(recording)) {
			console_print_error("Cannot create replay recording.\n");
			exit_code = 1;
			return false;
		}
		recording_origin = replay_clock();
	}
	if (app->command.profile_path && !profiler_start_capture(app->command.profile_path)) {
		console_print_error("Cannot create profiling CSV.\n");
		exit_code = 1;
		return false;
	}
	return true;
}

// File readiness, not visible-tile readiness. Keep rendering while workers initialize.
static replay_result_t replay_files_ready(app_state_t* app) {
	if (app->scene.need_zoom_reset && arrlen(app->loaded_images)) {
		return REPLAY_PENDING;
	}
	for (i32 i = 0; i < arrlen(app->loaded_images); ++i) {
		image_t* image = app->loaded_images[i];
		if (image->backend == IMAGE_BACKEND_MRXS) {
			if (!atomic_load_acquire(&image->indexing_finished)) {
				return REPLAY_PENDING;
			}
			if (atomic_load_acquire(&image->indexing_failed)) {
				return REPLAY_ERROR;
			}
		} else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
			isyntax_t* isyntax = &image->isyntax;
			if (!isyntax->images[isyntax->wsi_image_index].first_load_complete) {
				return REPLAY_PENDING;
			}
		} else if (image->backend == IMAGE_BACKEND_DICOM) {
			for (i32 level = 0; level < image->level_count; ++level) {
				if (image->level_images[level].needs_indexing) {
					return REPLAY_PENDING;
				}
			}
		}
	}
	return REPLAY_DONE;
}

static replay_result_t execute_replay_command(void* userdata, const replay_command_t* command) {
	app_state_t* app = (app_state_t*)userdata;
	switch (command->kind) {
		case REPLAY_OPEN: {
			console_print("Replay: open '%s'.\n", command->path);
			if (!load_generic_file(app, command->path, 0) || !arrlen(app->loaded_images)) {
				return REPLAY_ERROR;
			}
			break;
		}
		case REPLAY_WAIT: {
			return replay_files_ready(app);
		}
		case REPLAY_CAMERA: {
			if (!arrlen(app->loaded_images)) {
				return REPLAY_ERROR;
			}
			pending_camera[0] = command->camera[0];
			pending_camera[1] = command->camera[1];
			if (command->has_zoom) {
				pending_camera[2] = command->camera[2];
				pending_zoom = true;
			}
			camera_pending = true;
			break;
		}
		case REPLAY_CLOSE: {
			menu_close_file(app);
			camera_pending = false;
			pending_zoom = false;
			break;
		}
		case REPLAY_EXIT: {
			need_quit = true;
			break;
		}
	}
	return REPLAY_DONE;
}

bool viewer_replay_is_playing(void) {
	return playing;
}

void viewer_replay_update(app_state_t* app) {
	if (playing) {
		replay_tick(&playback, replay_clock(), execute_replay_command, app);
		if (playback.error) {
			console_print_error("Replay command %u: %s.\n", playback.command_index, playback.error);
			exit_code = 1;
			need_quit = true;
			playing = false;
		} else if (playback.finished) {
			playing = false;
			console_print("Replay finished (%u commands).\n", playback.command_index);
		}
	}
}

void viewer_replay_apply_camera(app_state_t* app) {
	if (!camera_pending || !arrlen(app->loaded_images)) {
		return;
	}
	scene_t* scene = &app->scene;
	float zoom_target = scene->need_zoom_animation ? scene->zoom_target_state.pos : scene->zoom.pos;
	if (pending_zoom) {
		zoom_target = CLAMP(pending_camera[2], (float)viewer_min_level, (float)viewer_max_level);
	}
	v2f target = V2F(pending_camera[0], pending_camera[1]);
	float ratio = exp2f(zoom_target - scene->zoom.pos);
	if (fabsf(ratio - 1.0f) > 1.0e-5f) {
		// Existing zoom animation keeps a pivot fixed. Choose the pivot that ends at target.
		scene->zoom_pivot = v2f_scale(1.0f / (1.0f - ratio),
		                            v2f_subtract(target, v2f_scale(ratio, scene->camera)));
		scene->zoom_target_state = scene->zoom;
		zoom_update_pos(&scene->zoom_target_state, zoom_target);
		scene->need_zoom_animation = true;
	} else {
		scene_update_camera_pos(scene, target);
		scene->zoom_pivot = target;
	}
	scene->panning_velocity = V2F(0.0f, 0.0f);
	camera_pending = false;
	pending_zoom = false;
}

void viewer_replay_after_navigation(app_state_t* app) {
	if (!recording && !profiler_is_capturing()) {
		return;
	}
	scene_t* scene = &app->scene;
	if (recording_waiting && replay_files_ready(app) == REPLAY_DONE) {
		recording_origin = replay_clock();
		recording_waiting = false;
	}
	float zoom_target = scene->need_zoom_animation ? scene->zoom_target_state.pos : scene->zoom.pos;
	v2f camera_target = scene->camera;
	if (scene->need_zoom_animation) {
		float ratio = exp2f(zoom_target - scene->zoom.pos);
		camera_target = v2f_subtract(scene->zoom_pivot,
		                            v2f_scale(ratio, v2f_subtract(scene->zoom_pivot, scene->camera)));
	}
	float camera[3] = {camera_target.x, camera_target.y, zoom_target};
	bool zoom_changed = !have_recorded_camera || camera[2] != recorded_camera[2];
	// Ignore floating-point drift in the inferred target during animation (units: micrometres).
	bool position_changed = fabsf(camera[0] - recorded_camera[0]) > 0.05f ||
	                        fabsf(camera[1] - recorded_camera[1]) > 0.05f;
	if (recording && !recording_waiting && arrlen(app->loaded_images) &&
	    (zoom_changed || position_changed)) {
		replay_command_t command = {};
		command.timestamp = (float)(replay_clock() - recording_origin);
		command.kind = REPLAY_CAMERA;
		command.has_zoom = zoom_changed;
		memcpy(command.camera, camera, sizeof(camera));
		record_command(&command);
		memcpy(recorded_camera, camera, sizeof(camera));
		have_recorded_camera = true;
	}
	if (!profiler_is_capturing()) {
		return;
	}
	if (profiler_memory_sample_due() || metrics.image_count != (u32)arrlen(app->loaded_images)) {
		profiler_begin(PROFILER_SECTION_MEMORY_SAMPLE);
		metrics.texture_bytes = 0;
		metrics.cpu_tile_bytes = 0;
		metrics.cache_table_bytes = 0;
		metrics.cache_entries = 0;
		metrics.tile_metadata_bytes = 0;
		metrics.mrxs_index_bytes = 0;
		metrics.resident_bytes = profiler_resident_bytes();
		for (i32 i = 0; i < arrlen(app->loaded_images); ++i) {
			image_t* image = app->loaded_images[i];
			tile_cache_memory_stats_t cache = tile_cache_get_memory_stats(image);
			metrics.texture_bytes += cache.texture_bytes;
			metrics.cpu_tile_bytes += cache.cpu_bytes;
			metrics.cache_table_bytes += cache.table_bytes;
			metrics.cache_entries += cache.entries;
			for (i32 level = 0; level < image->level_count; ++level) {
				level_image_t* pyramid = image->level_images + level;
				// MRXS pointers are only safe to inspect after acquire publication.
				if (image_level_index_ready(image, 0, level) && pyramid->tiles) {
					metrics.tile_metadata_bytes += pyramid->tile_count * sizeof(tile_t);
				}
				if (image->backend == IMAGE_BACKEND_MRXS) {
					for (i32 plane = 0; plane < image->plane_count; ++plane) {
						if (image_level_index_ready(image, plane, level) && image->mrxs.planes[plane].tiles[level]) {
							mrxs_level_t* index = image->mrxs.levels + level;
							metrics.mrxs_index_bytes += (u64)index->width_in_tiles * index->height_in_tiles * sizeof(mrxs_tile_t);
						}
					}
				}
			}
		}
		tile_buffer_pool_stats_t pool = tile_buffer_pool_get_stats();
		metrics.pooled_bytes = pool.retained_bytes;
		profiler_end(PROFILER_SECTION_MEMORY_SAMPLE);
	}
	metrics.image_count = (u32)arrlen(app->loaded_images);
	metrics.replay_segment = playback.segment;
	metrics.replay_command = playback.completed_commands;
	metrics.camera_x = scene->camera.x;
	metrics.camera_y = scene->camera.y;
	metrics.zoom = scene->zoom.pos;
	metrics.viewport_width = app->client_viewport.w;
	metrics.viewport_height = app->client_viewport.h;
	metrics.display_scale = app->display_scale_factor;
	profiler_set_metrics(&metrics);
}

int viewer_replay_shutdown(void) {
	if (recording) {
		replay_command_t command = {};
		command.timestamp = recording_waiting ? 0.0f : (float)(replay_clock() - recording_origin);
		command.kind = REPLAY_EXIT;
		record_command(&command);
		if (recording && fclose(recording) != 0) {
			exit_code = 1;
		}
		recording = NULL;
	}
	if (!profiler_stop_capture()) {
		exit_code = 1;
	}
	replay_destroy(&playback);
	return exit_code;
}
