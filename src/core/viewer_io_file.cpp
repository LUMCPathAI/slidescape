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
#include "dicom_wsi.h"
#include "stringutils.h"
#include "listing.h"
#include "stb_image.h"
#include "remote.h"
#include "jpeg_decoder.h"
#include "tile_cache.h"

#include "gui.h" // for global data, TODO: refactor

static void slide_score_post_tile_result(load_tile_task_t* task, u8* pixel_memory, bool failed, bool is_empty) {
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
		slide_score_post_tile_result(task, pixel_memory, failed, false);
	}

	atomic_subtract(&image->refcount, refcount_decrement_amount);
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
			show_dicom_slides_window = true;
		}

        if (is_base_image) {
            annotation_set_t* annotation_set = &app_state->scene.annotation_set;
//            unload_and_reinit_annotations(annotation_set); // no need, already unloaded!
            annotation_set->mpp = V2F(image->mpp_x, image->mpp_y);

            // Check if there is an associated annotation file.
            char temp_filename[512];
            temp_filename[0] = '\0';
            const char* prefix = (app_state->annotation_directory[0] != '\0') ? app_state->annotation_directory : file->filename_prefix;
            snprintf(temp_filename, sizeof(temp_filename), "%s%s", prefix, file->filename_in_directory);
            bool were_annotations_loaded = false;

			const char* annotation_extensions[] = { "xml", "geojson", "json" };
			for (i32 extension_index = 0; extension_index < COUNT(annotation_extensions); ++extension_index) {
				snprintf(temp_filename, sizeof(temp_filename), "%s%s", prefix, file->filename_in_directory);
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
	if (file->type == VIEWER_FILE_TYPE_DICOM && !is_dicom_available && !is_dicom_loading_done) {
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
