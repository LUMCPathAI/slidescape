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

#if WINDOWS
#include "win32_graphical_app.h"
#include <io.h>
#else
#include <sys/mman.h>
#endif

#include "platform.h"
#include "intrinsics.h"
#include "stringutils.h"

#include "openslide_api.h"
#include <linmath.h>

#include "arena.h"

#define VIEWER_IMPL
#include "viewer.h"
#include "listing.h"

#define STBI_ASSERT(x) ASSERT(x)
#include "stb_image.h"

#include "image.h"
#include "tiff.h"
#include "isyntax.h"
#include "mrxs.h"
#include "tif_lzw.h"
#include "dicom.h"
#include "dicom_wsi.h"
#include "jpeg_decoder.h"
#include "remote.h"
#include "dicomweb_client.h"
#include "gui.h"
#include "annotation.h"
#include "ini.h"
#include "image_registration.h"
#include "renderer.h"
#include "tile_cache.h"
#include "tile_buffer_pool.h"
#include "profiler.h"


void add_image(app_state_t* app_state, image_t* image, bool need_zoom_reset, bool need_image_registration) {
	arrput(app_state->loaded_images, image);
	arrput(app_state->active_resources, image->resource_id);
	app_state->scene.active_layer = arrlen(app_state->loaded_images)-1;
    layers_window_selected_image_index = app_state->scene.active_layer;
	if (need_zoom_reset) {
		app_state->scene.need_zoom_reset = true;
	}
    image_t* added_image = arrlast(app_state->loaded_images);

	// Enable the new image as a displayable layer
	i32 enabled_image_count = 0;
	for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
		if (app_state->loaded_images[i]->is_enabled) ++enabled_image_count;
	}
	if (enabled_image_count < 2) {
		added_image->is_enabled = true;
		app_state->last_layer_enabled = app_state->scene.active_layer;
		app_state->scene.layer_time = 0.0f;
		app_state->scene.target_layer_time = 0.0f;
	} else {
		// TODO: disable previous layer?
	}

	// TODO: recount enabled image count

    if (need_image_registration && arrlen(app_state->loaded_images) > 1) {
	    app_state->scene.target_layer_time = 1.0f;
        image_t* parent_image = app_state->loaded_images[0];
        image_transform_t transform = do_image_registration(parent_image, added_image, 1);
        if (transform.is_valid) {
            // apply translation
            if (transform.translate.x != 0.0f || transform.translate.y != 0.0f) {
                added_image->origin_offset = transform.translate;
            }
        }
    }
    copy_cstring(app_state->last_active_directory, image->directory, COUNT(app_state->last_active_directory));
}

void renderer_destroy_image_resources(app_state_t* app_state, image_t* image) {
	if (!image) return;

	tile_cache_begin_shutdown(image);
	if (app_state) {
		for (i32 i = 0; i < COUNT(app_state->pixel_transfer_states); ++i) {
			pixel_transfer_state_t* transfer_state = app_state->pixel_transfer_states + i;
			if (transfer_state->image != image) {
				continue;
			}
			renderer_finalize_texture_upload(transfer_state);
			if (transfer_state->texture) {
				renderer_destroy_texture(transfer_state->texture);
				transfer_state->texture = 0;
			}
			tile_cache_cancel_upload(image, transfer_state->plane, transfer_state->level, transfer_state->tile_index);
			transfer_state->image = NULL;
		}
	}

	if (image->backend == IMAGE_BACKEND_STBI) {
		renderer_texture_handle_t simple_destroyed_texture_handle = 0;
		if (image->simple.texture != 0) {
			simple_destroyed_texture_handle = image->simple.texture;
			renderer_destroy_texture(image->simple.texture);
			image->simple.texture = 0;
		}
		// NOTE: simple/stbi images have only 1 tile, the texture of which is just the whole image.
		// We shouldn't destroy that texture twice, so here we're just setting it to 0.
		level_image_t* level_image = image->level_images;
		if (level_image && level_image[0].tiles) {
			renderer_texture_handle_t texture = tile_cache_get_gpu_texture(image, 0, 0, 0);
			if (texture != 0 && texture == simple_destroyed_texture_handle) {
				tile_cache_take_gpu_texture(image, 0, 0, 0);
			}
		}
	}

	tile_cache_destroy_gpu_textures(image, renderer_destroy_texture);

	if (image->macro_image.texture != 0) {
		renderer_destroy_texture(image->macro_image.texture);
		image->macro_image.texture = 0;
	}
	if (image->label_image.texture != 0) {
		renderer_destroy_texture(image->label_image.texture);
		image->label_image.texture = 0;
	}
}

// TODO: make this based on scene (allow loading multiple images independently side by side)
void unload_all_images(app_state_t *app_state) {
	autosave(app_state, true, false); // save recent changes to annotations, if necessary

	i32 current_image_count = arrlen(app_state->loaded_images);
	if (current_image_count > 0) {
		ASSERT(app_state->loaded_images);
		for (i32 i = 0; i < current_image_count; ++i) {
			image_t* old_image = app_state->loaded_images[i];
			renderer_destroy_image_resources(app_state, old_image);
			image_destroy(old_image);
            free(old_image);
		}
		arrfree(app_state->loaded_images);
		arrfree(app_state->active_resources); // TODO: now that image_t pointers are stable, can active_resources be removed?
	}
	mouse_show();
	app_state->scene.is_cropped = false;
	app_state->scene.has_selection_box = false;
	viewer_switch_tool(app_state, TOOL_NONE);

	global_export_save_as_filename[0] = '\0'; // reset output filename hint after export
}

bool was_button_pressed(button_state_t* button) {
	bool result = button->down && button->transition_count > 0;
	return result;
}

bool was_button_released(button_state_t* button) {
	bool result = (!button->down) && button->transition_count > 0;
	return result;
}

bool was_key_pressed(input_t* input, i32 keycode) {
	u8 key = keycode & 0xFF;
	button_state_t* button = &input->keyboard.keys[key];
	bool result = was_button_pressed(button);
	return result;
}

bool is_key_down(input_t* input, i32 keycode) {
	u8 key = keycode & 0xFF;
	bool result = input->keyboard.keys[key].down;
	return result;
}

int priority_cmp_func (const void* a, const void* b) {
	return ( (*(load_tile_task_t*)b).priority - (*(load_tile_task_t*)a).priority );
}

void init_app_state(app_state_t* app_state, app_command_t command) {
	ASSERT(!app_state->initialized); // check sanity
	ASSERT(app_state->temp_storage_memory == NULL);
//	memset(app_state, 0, sizeof(app_state_t));

	tile_loader_set_remote_tiff_batch_callback(tiff_load_tile_batch_func);
	tile_loader_set_slide_score_batch_callback(slide_score_load_tile_batch_func);
	tile_loader_set_dicomweb_batch_callback(dicomweb_load_tile_batch_func);
	tile_loader_set_ssi_batch_callback(ssi_load_tile_batch_func);
	image_set_ssi_close_callback(ssi_close_remote_image);

	app_state->command = command;
	app_state->headless = command.headless;

	if (app_state->display_points_per_pixel == 0.0f) {
		app_state->display_points_per_pixel = 1.0f;
	}
	if (app_state->display_scale_factor == 0.0f) {
		app_state->display_scale_factor = 1.0f;
	}

	// TODO: remove
	size_t temp_storage_size = MEGABYTES(16); // Note: what is a good size to use here?
	app_state->temp_storage_memory = platform_alloc(temp_storage_size);
	init_arena(&app_state->temp_arena, temp_storage_size, app_state->temp_storage_memory);

	app_state->clear_color = V4F(1.0f, 1.0f, 1.0f, 1.00f);
	app_state->black_level = 0.10f;
	app_state->white_level = 0.95f;
	app_state->use_builtin_tiff_backend = true; // If disabled, revert to OpenSlide when loading TIFF files.

	app_state->keyboard_base_panning_speed = 10.0f;
	app_state->mouse_sensitivity = 12.0f;
	app_state->enable_autosave = true;

	init_scene(app_state, &app_state->scene);

	unload_and_reinit_annotations(&app_state->scene.annotation_set);

	app_state->initialized = true;
}

void autosave(app_state_t* app_state, bool force_ignore_delay, bool async) {
	annotation_set_t* annotation_set = &app_state->scene.annotation_set;

    bool proceed = force_ignore_delay;
    if (!force_ignore_delay) {
        float seconds_since_last_modified = get_seconds_elapsed(annotation_set->last_modification_time, get_clock());
        // only autosave if there haven't been any additional changes for some time (don't do it too often)
        if (seconds_since_last_modified > 2.0f) {
            proceed = true;
        }
    }

    if (proceed) {
        if (app_state->enable_autosave) {
            save_annotations(app_state, annotation_set, force_ignore_delay, async);
        }
        if (app_state->remember_annotation_groups_as_template) {
            annotation_set_template_destroy(&app_state->scene.annotation_set_template);
            app_state->scene.annotation_set_template = create_annotation_set_template(annotation_set);
        }
    }

}

bool is_resource_valid(app_state_t* app_state, i32 resource_id) {
	for (i32 i = 0; i < arrlen(app_state->active_resources); ++i) {
		if (app_state->active_resources[i] == resource_id) {
			return true;
		}
	}
	return false;
}

image_t* get_image_from_resource_id(app_state_t* app_state, i32 resource_id) {
	i32 image_index = 0;
	for (; image_index < arrlen(app_state->loaded_images); ++image_index) {
		image_t* image = app_state->loaded_images[image_index];
		if (image->resource_id == resource_id) {
			return image;
		}
	}
	return NULL;
}

static bool viewer_process_loaded_tile(app_state_t* app_state, image_t* image, tile_cache_result_t* task) {
	size_t pixel_memory_size = task ?
			(size_t)task->tile_width * task->tile_height * BYTES_PER_PIXEL : 0;
	bool submitted_texture_upload = false;
	if (!image || !task) {
		if (task && task->pixel_memory) tile_buffer_release(task->pixel_memory, pixel_memory_size);
		return false;
	}

	/*
	 * Generic backends create the cache before submitting work. iSyntax still
	 * uses its legacy streamer/global completion queue, so its first viewer
	 * completion is the boundary where it joins cache-backed texture storage.
	 */
	if (!tile_cache_get_or_create(image)) {
		tile_buffer_release(task->pixel_memory, pixel_memory_size);
		return false;
	}

	tile_cache_result_action_t action = {};
	tile_cache_apply_result(image, task, true, &action);
	if (action.mark_empty) {
		platform_mutex_lock(&image->lock);
		tile_t* tile = get_tile_from_tile_index(image, task->level, task->tile_index);
		ASSERT(tile);
		tile->is_empty = true;
		platform_mutex_unlock(&image->lock);
	}

	if (action.should_upload && action.upload_pixels) {
		if (image->backend == IMAGE_BACKEND_SSI &&
		    atomic_compare_exchange(&image->ssi.first_tile_displayed_reported, 1, 0)) {
			console_print_verbose(
				"SSI first tile submitted for display: %d ms after viewer ready\n",
				(i32)(get_seconds_elapsed(image->ssi.viewer_ready_clock, get_clock()) *
				      1000.0f + 0.5f));
		}
		pixel_transfer_state_t* transfer_state =
				renderer_submit_texture_upload(app_state, task->tile_width, task->tile_height,
				                               4, action.upload_pixels, finalize_textures_immediately);
		submitted_texture_upload = true;
		if (image->backend == IMAGE_BACKEND_DICOMWEB &&
		    !image->dicomweb.first_tile_upload_reported && image->dicomweb.open_start_clock) {
			image->dicomweb.first_tile_upload_reported = true;
			console_print_verbose("DICOMweb first tile upload submitted: %.2f ms after open started\n",
				get_seconds_elapsed(image->dicomweb.open_start_clock, get_clock()) * 1000.0);
		}
		if (finalize_textures_immediately) {
			tile_cache_store_gpu_texture(image, task->plane, task->level, task->tile_index, transfer_state->texture);
		} else {
			transfer_state->image = image;
			transfer_state->plane = task->plane;
			transfer_state->level = task->level;
			transfer_state->tile_index = task->tile_index;
		}
		if (action.release_upload_cpu_pin) {
			tile_cache_release_upload_cpu_pin(image, task->plane, task->level, task->tile_index);
		}
		if (action.free_upload_pixels) {
			tile_buffer_release(action.upload_pixels, pixel_memory_size);
		}
	}
	tile_buffer_release(action.pixels_to_free, pixel_memory_size);
	return submitted_texture_upload;
}

void viewer_process_completion_queue(app_state_t* app_state) {
	profiler_begin(PROFILER_SECTION_COMPLETION_QUEUE);
	float max_texture_load_time = 0.007f; // TODO: pin to frame time
#if 1
	if (!finalize_textures_immediately) {
		// Finalize textures that were uploaded via PBO the previous frame
		for (i32 transfer_index = 0; transfer_index < COUNT(app_state->pixel_transfer_states); ++transfer_index) {
			pixel_transfer_state_t* transfer_state = app_state->pixel_transfer_states + transfer_index;
			if (transfer_state->need_finalization) {
				renderer_finalize_texture_upload(transfer_state);
				if (transfer_state->image) {
					tile_cache_store_gpu_texture(transfer_state->image, transfer_state->plane, transfer_state->level, transfer_state->tile_index, transfer_state->texture);
					transfer_state->image = NULL;
				}
			}
			float time_elapsed = get_seconds_elapsed(app_state->last_frame_start, get_clock());
			if (time_elapsed > max_texture_load_time) {
//			    	console_print("Warning: texture finalization is taking too much time\n");
				break;
			}
		}
	}

	/*time_elapsed = get_seconds_elapsed(last_section, get_clock());
	if (time_elapsed > 0.005f) {
		console_print("Warning: texture finalization took %g ms\n", time_elapsed * 1000.0f);
	}*/

//	last_section = profiler_end_section(last_section, "viewer_update_and_render: texture finalization", 7.0f);
#endif

	i32 pixel_transfer_index_start = app_state->next_pixel_transfer_to_submit;
	bool stop_processing_completions = false;
	for (i32 image_index = 0; image_index < arrlen(app_state->loaded_images) && !stop_processing_completions; ++image_index) {
		image_t* image = app_state->loaded_images[image_index];
		tile_cache_result_t task = {0};
		while (tile_cache_poll_load_result(image, &task)) {
			bool submitted_texture_upload = viewer_process_loaded_tile(app_state, image, &task);
			float time_elapsed = get_seconds_elapsed(app_state->last_frame_start, get_clock());
			if (time_elapsed > max_texture_load_time) {
//				console_print("Warning: texture submission is taking too much time\n");
				stop_processing_completions = true;
				break;
			}

			if (submitted_texture_upload && pixel_transfer_index_start == app_state->next_pixel_transfer_to_submit) {
//				console_print("Warning: not enough PBO's to do all the pixel transfers\n");
				stop_processing_completions = true;
				break;
			}
		}
	}

	// Retrieve completed tasks from the worker threads
	completion_event_t entry = {0};
	while (!stop_processing_completions && completion_queue_poll(&global_completion_queue, &entry)) {
        if (entry.kind == TILE_LOADER_COMPLETION_EVENT_TILE_LOADED) {
            tile_load_completion_task_t* isyntax_task = (tile_load_completion_task_t*) entry.userdata;
            image_t* image = get_image_from_resource_id(app_state, isyntax_task->resource_id);
			tile_cache_result_t task = {0};
			task.pixel_memory = isyntax_task->pixel_memory;
			task.level = isyntax_task->scale;
			task.tile_index = isyntax_task->tile_index;
			task.tile_width = isyntax_task->tile_width;
			task.tile_height = isyntax_task->tile_height;
			task.resource_id = isyntax_task->resource_id;
			task.want_gpu_residency = isyntax_task->want_gpu_residency;
			task.want_cpu_residency = isyntax_task->want_cpu_residency;
			task.is_empty = isyntax_task->is_empty;
			task.failed = isyntax_task->failed;
            viewer_process_loaded_tile(app_state, image, &task);
		} else if (entry.kind == SSI_COMPLETION_EVENT_OPTIONAL_DATA) {
			void* result_pointer = NULL;
			memcpy(&result_pointer, entry.userdata, sizeof(result_pointer));
			ssi_commit_optional_data(app_state, result_pointer);
        }

		float time_elapsed = get_seconds_elapsed(app_state->last_frame_start, get_clock());
		if (time_elapsed > max_texture_load_time) {
//				console_print("Warning: texture submission is taking too much time\n");
			break;
		}

		if (pixel_transfer_index_start == app_state->next_pixel_transfer_to_submit) {
//				console_print("Warning: not enough PBO's to do all the pixel transfers\n");
			break;
		}
	}
	profiler_end(PROFILER_SECTION_COMPLETION_QUEUE);
}

void update_and_render_image(app_state_t* app_state, image_t* image) {
	scene_t* scene = &app_state->scene;

	i32 client_width = app_state->client_viewport.w;
	i32 client_height = app_state->client_viewport.h;

	if (image->type == IMAGE_TYPE_WSI) {

//		last_section = profiler_end_section(last_section, "viewer_update_and_render: process input (2)", 5.0f);

		// IO
		profiler_begin(PROFILER_SECTION_TILE_LOADING);
		platform_mutex_lock(&image->lock);

		// Upload macro and label images (just-in-time)
		simple_image_t* macro_image = &image->macro_image;
		simple_image_t* label_image = &image->label_image;
		if (macro_image->is_valid) {
			if (macro_image->texture == 0 && macro_image->pixels != NULL) {
				macro_image->texture = renderer_create_texture(macro_image->pixels, macro_image->width, macro_image->height, RENDERER_PIXEL_FORMAT_RGBA);
				stbi_image_free(macro_image->pixels);
				macro_image->pixels = NULL;
			}
		}
		if (label_image->is_valid) {
			if (label_image->texture == 0 && label_image->pixels != NULL) {
				label_image->texture = renderer_create_texture(label_image->pixels, label_image->width, label_image->height, RENDERER_PIXEL_FORMAT_RGBA);
				stbi_image_free(label_image->pixels);
				label_image->pixels = NULL;
			}
		}

		// Determine the highest and lowest levels with image data that need to be loaded and rendered.
		// The lowest needed level might be lower than the actual current downsampling level,
		// because some levels may not have image data available (-> need to fall back to lower level).
		ASSERT(image->level_count >= 0);
		i32 highest_visible_scale = ATLEAST(image->level_count - 1, 0);
		i32 lowest_visible_scale = ATLEAST(scene->zoom.level, 0);
		lowest_visible_scale = ATMOST(highest_visible_scale, lowest_visible_scale);
		for (; lowest_visible_scale > 0; --lowest_visible_scale) {
			if (image->level_images[lowest_visible_scale].exists &&
			    !(image->level_images[lowest_visible_scale].missing_planes & (UINT64_C(1) << image->selected_plane))) {
				break; // done, no need to go lower
			}
		}

        // Check if some levels still need indexing, and if so, start doing that in the background
        for (i32 scale = highest_visible_scale; scale >= 0; --scale) {
            ASSERT(scale >= 0 && scale < COUNT(image->level_images));
            level_image_t *drawn_level = image->level_images + scale;
            if (drawn_level->needs_indexing) {
				if (!drawn_level->indexing_job_submitted) {
					begin_level_image_indexing(image, drawn_level, scale);
				}
                break; // only do one at a time
            }
        }

        // Start pulling image data from the WSI
		if (image->backend == IMAGE_BACKEND_ISYNTAX) {
			isyntax_t* isyntax = &image->isyntax;
			isyntax_image_t* wsi = isyntax->images + isyntax->wsi_image_index;
			isyntax_streamer_t tile_streamer = {0};
			tile_streamer.isyntax = isyntax;
			tile_streamer.wsi = wsi;
			tile_streamer.resource_id = image->resource_id;
			tile_streamer.tile_completion_queue = &global_completion_queue;
			tile_streamer.tile_completed_event_kind = TILE_LOADER_COMPLETION_EVENT_TILE_LOADED;
            tile_streamer.pixel_format = LIBISYNTAX_PIXEL_FORMAT_BGRA;
			if (!wsi->first_load_complete && !wsi->first_load_in_progress) {
				// NOTE: this fails if isyntax_tile_read() is called once already
				// TODO: harden isyntax_begin_first_load() to permit running in a non-clean state?
				wsi->first_load_in_progress = true;
				isyntax_begin_first_load(&tile_streamer);
			} else if (wsi->first_load_complete) {
				tile_streamer.origin_offset = image->origin_offset; // TODO: superfluous?
				if (!scene->restrict_load_bounds) {
					tile_streamer.camera_bounds = scene->camera_bounds;
				} else {
					tile_streamer.camera_bounds = scene->tile_load_bounds;
				}
				tile_streamer.camera_center = scene->camera;
				tile_streamer.crop_bounds = scene->crop_bounds;
				tile_streamer.is_cropped = scene->is_cropped;
				tile_streamer.zoom_level = scene->zoom.level;
				isyntax_begin_stream_image_tiles(&tile_streamer);
			}
		} else if (image->backend == IMAGE_BACKEND_STBI) {
			simple_image_t* simple = &image->simple;
			if (image->simple.texture == 0 && image->simple.pixels != NULL) {
				image->simple.texture = renderer_create_texture(image->simple.pixels, image->simple.width, image->simple.height, RENDERER_PIXEL_FORMAT_RGBA);
//			    image->origin_offset = (v2f) {50, 100};
				image->is_freshly_loaded = false;
				level_image_t* level_image = image->level_images + 0;
				ASSERT(level_image->tiles && level_image->tile_count > 0);
				tile_t* tile = level_image->tiles + 0;
				tile_cache_get_or_create(image); // TODO(pvalkema): why? we don't really need a full tile-cache for the STBI backend (which has no tiles), do we?
				tile_cache_store_gpu_texture(image, 0, 0, 0, image->simple.texture);
			}

		} else {

			tile_cache_viewer_request_t request = {0};
			request.plane = image->selected_plane;
			request.camera_bounds = scene->restrict_load_bounds ? scene->tile_load_bounds : scene->camera_bounds;
			request.camera_center = scene->camera;
			request.crop_bounds = scene->crop_bounds;
			request.is_cropped = scene->is_cropped;
			request.zoom_level = scene->zoom.level;
			request.client_width = client_width;
			request.client_height = client_height;
			if (tile_cache_request_viewer_tiles(image, &request) > 0) {
				app_state->allow_idling_next_frame = false;
			}
		}

		platform_mutex_unlock(&image->lock);
		profiler_end(PROFILER_SECTION_TILE_LOADING);

		// RENDERING
		profiler_begin(PROFILER_SECTION_SCENE_RENDER);
		mat4x4 projection = {};
		{
			float l = -0.5f * scene->r_minus_l;
			float r = +0.5f * scene->r_minus_l;
			float b = +0.5f * scene->t_minus_b;
			float t = -0.5f * scene->t_minus_b;
			float n = 100.0f;
			float f = -100.0f;
			mat4x4_ortho(projection, l, r, b, t, n, f);
		}

		mat4x4 I;
		mat4x4_identity(I);

		// define view matrix
		mat4x4 view_matrix;
		mat4x4_rotate_Z(view_matrix, I, scene->rotation);
		mat4x4_translate_in_place(view_matrix,
		                          -scene->camera.x + image->origin_offset.x,
		                          -scene->camera.y + image->origin_offset.y,
		                          0.0f);

		mat4x4 projection_view_matrix;
		mat4x4_mul(projection_view_matrix, projection, view_matrix);

		renderer_begin_image_render(app_state, scene, projection_view_matrix);

//		last_section = profiler_end_section(last_section, "viewer_update_and_render: render (1)", 5.0f);

		// Render label and macro images
		if (scene->draw_macro_image) {
			renderer_disable_stencil_test();
			if (macro_image->is_valid && macro_image->texture != 0) {
				v2f pmax = {};
				pmax.x = macro_image->width * macro_image->mpp;
				pmax.y = macro_image->height * macro_image->mpp;

				mat4x4 model_matrix;

				mat4x4_translate(model_matrix,
				                 image->origin_offset.x + macro_image->world_pos.x,
				                 image->origin_offset.y + macro_image->world_pos.y,
				                 10.0f);
				mat4x4_scale_aniso(model_matrix, model_matrix, pmax.x, pmax.y, 1.0f);
				renderer_set_image_model_matrix(model_matrix);

				renderer_draw_textured_rect(macro_image->texture);
			}
		}

		if (scene->draw_label_image) {
			renderer_disable_stencil_test();
			if (label_image->is_valid && label_image->texture != 0 && macro_image->is_valid) {
				v2f pmax = {};
				pmax.x = label_image->width * label_image->mpp;
				pmax.y = label_image->height * label_image->mpp;

				mat4x4 model_matrix;
				mat4x4_translate(model_matrix,
				                 image->origin_offset.x + label_image->world_pos.x,
				                 image->origin_offset.y + label_image->world_pos.y,
				                 10.0f);
				mat4x4_scale_aniso(model_matrix, model_matrix, pmax.x, pmax.y, 1.0f);
				mat4x4 model_matrix_rot;
				mat4x4_rotate_Z(model_matrix_rot, model_matrix, 0.5f * PI_F);
				renderer_set_image_model_matrix(model_matrix_rot);

				renderer_draw_textured_rect(label_image->texture);
			}
		}

		if (scene->draw_wsi_background && image->clip_rect_count > 1){
			// todo: the image width and height are not sufficient to contain all data both in iSyntax v1 and v2
			// why are the width and heights this way / incorrect?
			bounds2f stencil_bounds = {0, 0, image->width_in_um, image->height_in_um};
			if (scene->is_cropped) {
				stencil_bounds = clip_bounds2f(stencil_bounds, scene->crop_bounds);
			}

			{
				mat4x4 model_matrix;
				mat4x4_translate(model_matrix, stencil_bounds.left, stencil_bounds.top, 5.0f);
				mat4x4_scale_aniso(model_matrix, model_matrix,
				                   stencil_bounds.right - stencil_bounds.left,
				                   stencil_bounds.bottom - stencil_bounds.top,
				                   1.0f);
				renderer_set_image_model_matrix(model_matrix);
				renderer_draw_textured_rect(renderer_get_transparent_texture());
			}
		}

		if (scene->draw_wsi_image){
			// Set up the stencil buffer to prevent rendering outside the image area
			renderer_begin_stencil_write();

			for(i32 i=0; i<image->clip_rect_count; ++i){
				// Remove 1 pixel of the outline to prevent rendering invalid areas / black outlines,
				// cause the stencil buffer isnt perfect (likely due to rounding errors)
				// Due to the rectangle being a continuum of eachother, you cannot simply make each individual rectangle smaller
				// Instead we only want to remove the 'outline' of the rectangles, 
				// for that purpose we draw one slightly bigger and one slightly smaller rectangle
				// When a particular pixel is on the outline it will be covered by at most one rectangle
				// Every other pixel is covered at least two rectangles 
				// (the pixels on the border between two adjecent rectangles, are covered by the bigger rectangle of the two adjecent rectangles)
				// (x and y need to be scaled independently, otherwise inner corners are not removed)

				float overlap_x = image->level_images[lowest_visible_scale].um_per_pixel_x * 1.0;
				float overlap_y = image->level_images[lowest_visible_scale].um_per_pixel_y * 1.0;

				rect2f* clip_rect = &image->clip_rects[i];
				{
					mat4x4 model_matrix;
					mat4x4_translate(model_matrix, clip_rect->x - overlap_x, clip_rect->y + overlap_y, 0.0f);
					mat4x4_scale_aniso(model_matrix, model_matrix,
									   clip_rect->w + (2 * overlap_x),
									   clip_rect->h - (2 * overlap_y),
									   1.0f);
					renderer_set_image_model_matrix(model_matrix);
					renderer_draw_textured_rect(renderer_get_dummy_texture());
				}
				{
					mat4x4 model_matrix;
					mat4x4_translate(model_matrix, clip_rect->x + overlap_x, clip_rect->y - overlap_y, 0.0f);
					mat4x4_scale_aniso(model_matrix, model_matrix,
									   clip_rect->w - (2 * overlap_x),
									   clip_rect->h + (2 * overlap_y),
									   1.0f);
					renderer_set_image_model_matrix(model_matrix);
					renderer_draw_textured_rect(renderer_get_dummy_texture());
				}
			}

			renderer_end_stencil_write();

		}

		// If a background image has already been rendered, we need to blend the tiles on top
		// while taking into account transparency.
		renderer_set_tile_blend_enabled(scene->draw_macro_image);

		// Draw tiles
		// Draw all levels within the viewport, up to the current zoom factor
		if (scene->draw_wsi_image){
			i32 lowest_level_to_draw = ATLEAST(lowest_visible_scale, global_lowest_scale_to_render);
			i32 highest_level_to_draw = ATMOST(highest_visible_scale, global_highest_scale_to_render);
			for (i32 level = lowest_level_to_draw; level <= highest_level_to_draw; ++level) {
				level_image_t *drawn_level = image->level_images + level;
				if (!drawn_level->exists || (drawn_level->missing_planes & (UINT64_C(1) << image->selected_plane)) ||
				    !image_level_index_ready(image, 0, level)) {
					continue;
				}

				bounds2i level_tiles_bounds = BOUNDS2I(0, 0, (i32)drawn_level->width_in_tiles, (i32)drawn_level->height_in_tiles);

				bounds2i visible_tiles = world_bounds_to_tile_bounds(&scene->camera_bounds, drawn_level->x_tile_side_in_um,
																	drawn_level->y_tile_side_in_um, image->origin_offset);
				visible_tiles = clip_bounds2i(visible_tiles, level_tiles_bounds);

				if (scene->is_cropped) {
					bounds2i crop_tile_bounds = world_bounds_to_tile_bounds(&scene->crop_bounds,
															drawn_level->x_tile_side_in_um,
															drawn_level->y_tile_side_in_um, image->origin_offset);
					visible_tiles = clip_bounds2i(visible_tiles, crop_tile_bounds);
				}

				i32 missing_tiles_on_this_level = 0;
				for (i32 tile_y = visible_tiles.min.y; tile_y < visible_tiles.max.y; ++tile_y) {
					for (i32 tile_x = visible_tiles.min.x; tile_x < visible_tiles.max.x; ++tile_x) {

						tile_t *tile = get_tile(drawn_level, tile_x, tile_y);
						renderer_texture_handle_t texture = tile_cache_get_focus_texture(image, level, tile->tile_index);
						if (texture) {
							tile->time_last_drawn = app_state->frame_counter;

							float tile_pos_x = drawn_level->origin_offset.x + drawn_level->x_tile_side_in_um * tile_x;
							float tile_pos_y = drawn_level->origin_offset.y + drawn_level->y_tile_side_in_um * tile_y;

							// define model matrix
							mat4x4 model_matrix;
							mat4x4_translate(model_matrix, tile_pos_x, tile_pos_y, 0.0f);
							mat4x4_scale_aniso(model_matrix, model_matrix, drawn_level->x_tile_side_in_um,
							                   drawn_level->y_tile_side_in_um, 1.0f);
							renderer_set_image_model_matrix(model_matrix);

							renderer_draw_textured_rect(texture);
						} else {
							++missing_tiles_on_this_level;
						}
					}
				}

				if (missing_tiles_on_this_level == 0) {
					break; // don't need to bother drawing the next level, there are no gaps left to fill in!
				}

			}
		}

		renderer_finish_image_render();
		profiler_end(PROFILER_SECTION_SCENE_RENDER);

//		last_section = profiler_end_section(last_section, "viewer_update_and_render: render (2)", 5.0f);

	}
}


v2f viewer_do_2d_control(v2f velocity, v2f control, float dt, float time_since_start_moving, bool is_shift_pressed) {
	float old_speed = v2f_length(velocity);
	float control_force = v2f_length(control);
	float max_force = 120.0f;
	if (time_since_start_moving < 0.20f) {
		control_force *= (0.25f + 0.75f * time_since_start_moving * (1.0f / 0.2f)) * max_force;
	} else {
		control_force *= max_force;
	}
	float friction = 15.0f;
	friction += control_force * 0.5f;
	if (is_shift_pressed && control_force > 0.0f) {
		friction *= 0.25f;
	}
	float net_force = control_force - ((1.0f + old_speed * old_speed) * friction);
	float dv = net_force * dt;
	float new_speed = ATLEAST(0.0f, old_speed + dv);

#if 0
	static i32 times_zero;
	if (new_speed != 0.0f || times_zero < 5) {
		if (new_speed == 0.0f) {
			times_zero++;
		} else times_zero = 0;
		console_print("old_speed = %g, control_force = %g, friction = %g, net_force = %g, dv = %g, new_speed = %g\n", old_speed, control_force, friction, net_force, dv, new_speed);
	}
#endif

	v2f new_velocity = {};
	if (control_force > 0.01f) {
		new_velocity = v2f_scale(new_speed, control);
	} else if (old_speed > 0.01f) {
		new_velocity = v2f_scale(new_speed / old_speed, velocity);
	}
	return new_velocity;
}

controller_input_t* get_preferred_controller(input_t* input) {
	ASSERT(input->preferred_controller_index < COUNT(input->controllers));
	controller_input_t* controller = &input->controllers[input->preferred_controller_index];
	return controller;
}

v2f get_2d_control_from_input(input_t* input, bool allow_keyboard_input) {
	v2f control = {};
	if (input) {

		if (allow_keyboard_input) {
			controller_input_t* keyboard = &input->keyboard;
			if (!keyboard->key_ctrl.down) {
				if (keyboard->action_down.down || is_key_down(input, KEY_S) || is_key_down(input, KEY_Down)) {
					control.y += 1.0f;
				}
				if (keyboard->action_up.down || is_key_down(input, KEY_W) || is_key_down(input, KEY_Up)) {
					control.y += -1.0f;
				}
				if (keyboard->action_right.down || is_key_down(input, KEY_D) || is_key_down(input, KEY_Right)) {
					control.x += 1.0f;
				}
				if (keyboard->action_left.down || is_key_down(input, KEY_A) || is_key_down(input, KEY_Left)) {
					control.x += -1.0f;
				}
			}
		}

		controller_input_t* controller = get_preferred_controller(input);
		if (controller->action_down.down) {
			control.y += 1.0f;
		}
		if (controller->action_up.down) {
			control.y += -1.0f;
		}
		if (controller->action_right.down) {
			control.x += 1.0f;
		}
		if (controller->action_left.down) {
			control.x += -1.0f;
		}


		// Analog stick input
		if (controller->is_analog) {
			v2f left_stick = controller->left_stick.end;
			v2f right_stick = controller->right_stick.end;
			control.x += left_stick.x + right_stick.x;
			control.y -= left_stick.y + right_stick.y;
		}

		// Normalize
		float length_squared = v2f_length_squared(control);
		if (length_squared > 1.0f) {
			float length = sqrtf(length_squared);
			control = v2f_scale(1.0f / length, control);
		}

	}
	return control;
}

static inline void scene_update_camera_bounds(scene_t* scene) {
//	scene->camera_bounds = bounds_from_center_point(scene->camera, scene->r_minus_l, scene->t_minus_b);
	float sin_theta = scene->sin_rotation;
	float cos_theta = scene->cos_rotation;

	float right = 0.5f * scene->r_minus_l;
	float left = -right;
	float bottom = 0.5f * scene->t_minus_b;
	float top = -bottom;

	v2f rotated_points[4] = {
			{left * cos_theta - top * sin_theta, top * cos_theta + left * sin_theta }, // top left
			{right * cos_theta - top * sin_theta, top * cos_theta + right * sin_theta }, // top right
			{left * cos_theta - bottom * sin_theta, bottom * cos_theta + left * sin_theta }, // bottom left
			{right * cos_theta - bottom * sin_theta, bottom * cos_theta + right * sin_theta }, // bottom right
	};

	bounds2f rotated_bounds = bounds_from_points(rotated_points, 4);
	rotated_bounds.min = v2f_add(rotated_bounds.min, scene->camera);
	rotated_bounds.max = v2f_add(rotated_bounds.max, scene->camera);
	scene->camera_bounds = rotated_bounds;
}

void scene_update_camera_pos(scene_t* scene, v2f pos) {
	scene->camera = pos;
	scene_update_camera_bounds(scene);
}

static void scene_update_mouse_pos(scene_t* scene, v2f scene_mouse_xy) {
	if (scene_mouse_xy.x >= 0 && scene_mouse_xy.y < scene->viewport.w &&
        scene_mouse_xy.y >= 0 && scene_mouse_xy.y < scene->viewport.h) {
		if (scene->rotation == 0.0f) {
			scene->mouse.x = scene->camera_bounds.min.x + scene_mouse_xy.x * scene->zoom.screen_point_width;
			scene->mouse.y = scene->camera_bounds.min.y + scene_mouse_xy.y * scene->zoom.screen_point_width;
		} else {
			// Calculate mouse pos in polar coordinates, and use that to translate to world position
			float scene_mouse_x_from_center = (scene->viewport.w * 0.5f) - scene_mouse_xy.x;
			float scene_mouse_y_from_center = (scene->viewport.h * 0.5f) - scene_mouse_xy.y;
			float scene_distance_from_center = v2f_length(V2F(scene_mouse_x_from_center, scene_mouse_y_from_center));
			if (scene_distance_from_center == 0.0f) {
				scene->mouse = scene->camera;
			} else {
				float theta = atan2f(scene_mouse_y_from_center, scene_mouse_x_from_center);
				float radius = scene->zoom.screen_point_width * scene_distance_from_center;

				scene->mouse.x = scene->camera.x - cosf(theta - scene->rotation) * radius;
				scene->mouse.y = scene->camera.y - sinf(theta - scene->rotation) * radius;
			}
		}
	} else {
		scene->mouse = scene->camera;
	}
}

void viewer_switch_tool(app_state_t* app_state, placement_tool_enum tool) {
	if (app_state->mouse_tool == tool) {
		// no action needed
		return;
	}

	// TODO: finalize in-progress annotations?

	switch(tool) {
		default: case TOOL_NONE: {
			if (app_state->mouse_mode != MODE_VIEW) {
				app_state->mouse_mode = MODE_VIEW;
				set_cursor_default();
				// TODO: reset edit functionality?
			}
		} break;
		case TOOL_CREATE_OUTLINE: // TODO: how to handle creation of selection boxes; different from annotations?
		case TOOL_CREATE_POINT:
		case TOOL_CREATE_LINE:
		case TOOL_CREATE_ARROW:
		case TOOL_CREATE_FREEFORM:
		case TOOL_CREATE_ELLIPSE:
		case TOOL_CREATE_RECTANGLE:
		case TOOL_CREATE_TEXT: {
			app_state->mouse_mode = MODE_INSERT;
			set_cursor_crosshair();
		} break;
	}
	app_state->mouse_tool = tool;
	app_state->scene.annotation_set.editing_annotation_index = -1;
}

bool scene_control_layers(app_state_t* app_state, scene_t* scene, input_t* input, float delta_time) {
    bool consume_directional_control = false;
    i32 image_count = arrlen(app_state->loaded_images);
	i32 enabled_count = 0;
	for (i32 i = 0; i < image_count; ++i) {
		if (app_state->loaded_images[i]->is_enabled) ++enabled_count;
	}
    if (enabled_count > 1) {
        if (was_key_pressed(input, KEY_F5) || ((!gui_want_capture_keyboard) && (was_key_pressed(input, KEY_Space)))) {
            if (scene->target_layer_time == 0.5f) {
	            scene->target_layer_time += 0.001f;
            }
	        scene->target_layer_time = roundf(1.0f - scene->target_layer_time);
        }
        if (!gui_want_capture_keyboard) {
            if (was_key_pressed(input, KEY_B)) {
	            scene->target_layer_time = 0.5f;
            }
            if (is_key_down(input, KEY_B)) {
                if (scene->control.y != 0.0f || scene->control.x != 0.0f) {
                    scene->panning_velocity = V2F(0.0f,0.0f);
	                scene->target_layer_time += delta_time * scene->control.x * 1.0f;
	                scene->target_layer_time = CLAMP(scene->target_layer_time, 0.0f, 1.0f);
                    consume_directional_control = true;
                }
            }
            if (is_key_down(input, KEY_N)) {
                if (scene->control.y != 0.0f || scene->control.x != 0.0f) {
                    i32 overlay_image_index = layers_window_selected_image_index;
                    image_t* image_to_nudge = app_state->loaded_images[overlay_image_index];
                    v2f delta = v2f_scale(delta_time * 10.0f, scene->control);
                    image_to_nudge->origin_offset = v2f_add(image_to_nudge->origin_offset, delta);
                    scene->panning_velocity = V2F(0.0f,0.0f);
                    consume_directional_control = true;
                }
            }
        }
    }

    if ((i32)roundf(scene->target_layer_time) == 0) {
		scene->active_layer = app_state->enabled_image_indices[0];
	} else {
	    scene->active_layer = app_state->enabled_image_indices[1];
	}

    {
        float adjust_speed = 8.0f * delta_time;
        if (scene->layer_time < scene->target_layer_time) {
            float delta = MIN((scene->target_layer_time - scene->layer_time), adjust_speed);
	        scene->layer_time += delta;
        } else if (scene->layer_time > scene->target_layer_time) {
            float delta = MIN((scene->layer_time - scene->target_layer_time), adjust_speed);
	        scene->layer_time -= delta;
        }
    }
    return consume_directional_control; // directional input not consumed
}

void recount_enabled_images(app_state_t* app_state) {
	i32 image_count = arrlen(app_state->loaded_images);
	app_state->enabled_image_count = 0;
	app_state->enabled_image_indices[0] = 0;
	for (i32 i = 0; i < image_count; ++i) {
		if (app_state->loaded_images[i]->is_enabled) {
			app_state->enabled_image_indices[app_state->enabled_image_count++] = i;
		}
	}
	ASSERT(image_count >= 0);
	app_state->is_any_image_loaded = (image_count > 0);
}

void viewer_update_and_render(app_state_t *app_state, input_t *input, i32 client_width, i32 client_height, float delta_time) {

	// Release the temporary memory that was allocated the previous frame.
	app_state->temp_arena.used = 0;

//	if (!app_state->initialized) init_app_state(app_state);
	// Note: the window might get resized, so need to update this every frame
	app_state->client_viewport = RECT2I(0, 0, client_width, client_height);

	scene_t* scene = &app_state->scene;
	ASSERT(app_state->initialized);
	ASSERT(scene->initialized);
	annotation_set_t* annotation_set = &scene->annotation_set;

	// Note: could be changed to allow e.g. multiple scenes side by side
	{
		rect2f old_viewport = scene->viewport;
		rect2f new_viewport = RECT2F(
				(float)app_state->client_viewport.x * app_state->display_points_per_pixel,
				(float)app_state->client_viewport.y * app_state->display_points_per_pixel,
				(float)app_state->client_viewport.w * app_state->display_points_per_pixel,
				(float)app_state->client_viewport.h * app_state->display_points_per_pixel
		);
		if (new_viewport.x != old_viewport.x || old_viewport.y != new_viewport.y || old_viewport.w != new_viewport.w || old_viewport.h != new_viewport.h) {
			scene->viewport_changed = true;
		} else {
			scene->viewport_changed = false;
		}
		scene->viewport = new_viewport;
	}

	scene->clicked = false;
	scene->right_clicked = false;
	scene->drag_started = false;
	scene->drag_ended = false;

	app_state->input = input;

	// Set up rendering state for the next frame
	renderer_clear_render_target(app_state->clear_color, client_width, client_height);

	app_state->allow_idling_next_frame = true; // but we might set it to false later

	recount_enabled_images(app_state);
	i32 image_count = arrlen(app_state->loaded_images);

	if (!app_state->is_export_in_progress) {
		viewer_process_completion_queue(app_state);
	}

	if (image_count == 0) {
		scene->can_export_region = false;
		if (app_state->is_window_title_set_for_image) {
			reset_window_title(app_state->main_window);
			app_state->is_window_title_set_for_image = false;
		}
		//load_generic_file(app_state, "test.jpeg");
		do_after_scene_render(app_state, input);
		return;
	}

	image_t* base_image = app_state->loaded_images[app_state->base_image_index];
	ASSERT(base_image->type == IMAGE_TYPE_WSI);

	// Do actions that need to performed only once, when a new image has been loaded
	if (base_image->is_freshly_loaded) {
		set_window_title(app_state->main_window, base_image->name);
		app_state->is_window_title_set_for_image = true;
		// Workaround for drag onto window being registered as a click
		input->mouse_buttons[0].down = false;
		input->mouse_buttons[0].transition_count = 0;
		base_image->is_freshly_loaded = false;
	}

	if (input) {
		// TODO: select preferred controller based on input?
		controller_input_t* controller = get_preferred_controller(input);

		if (input->are_any_buttons_down || controller->is_analog || controller->left_trigger.has_input || controller->right_trigger.has_input) {
			app_state->allow_idling_next_frame = false;
		}

		if (input->mouse_moved) {
			app_state->seconds_without_mouse_movement = 0.0f;
		} else {
			app_state->seconds_without_mouse_movement += delta_time;
		}

		if (!gui_want_capture_keyboard && input->keyboard.modifiers == KMOD_NONE) {
			i32 slide_direction = was_key_pressed(input, KEY_Space) ? 1
			                    : was_key_pressed(input, KEY_Delete) ? -1 : 0;
			if (slide_direction != 0 && gui_navigate_slide_tray(app_state, slide_direction)) {
				do_after_scene_render(app_state, input);
				return;
			}
		}

		if ((was_key_pressed(input, KEY_W) && input->keyboard.key_ctrl.down) || app_state->need_close) {
			menu_close_file(app_state);
            do_after_scene_render(app_state, input);
            app_state->need_close = false;
            return;
		}

		if (gui_want_capture_mouse) {
			// ignore mouse input
		} else {
			// TODO: fix click on another window (but inside window bounds) registering as a click
			rect2i window_rect = {0, 0, (i32)(client_width * app_state->display_scale_factor), (i32)(client_height * app_state->display_scale_factor)};
			bool mouse_inside_window = is_point_inside_rect2i(window_rect, V2I((i32)input->mouse_xy.x, (i32)input->mouse_xy.y));

			if (was_button_released(&input->mouse_buttons[0]) && !scene->suppress_next_click && mouse_inside_window) {
				float drag_distance = v2f_length(scene->cumulative_drag_vector);
				if (drag_distance < VIEWER_CLICK_DRAG_TOLERANCE) {
					scene->clicked = true;
				}
			}
			if (was_button_released(&input->mouse_buttons[1])) {
				// Right click doesn't drag the scene, so we can be a bit more tolerant without confusing drags with clicks.
				scene->right_clicked = true;
//				scene->right_clicked_xy = input->mouse_xy;
				/*float drag_distance = v2f_length(scene->cumulative_drag_vector);
				if (drag_distance < 30.0f) {

				}*/
			}

			if (input->mouse_buttons[0].down) {
				// Mouse drag.
				if (input->mouse_buttons[0].transition_count != 0) {
					// Don't start dragging if clicked outside the window
					if (mouse_inside_window) {
						scene->is_dragging = true; // drag start
						scene->drag_started = true;
						scene->cumulative_drag_vector = v2f();
						mouse_hide();
//					    console_print("Drag started: x=%d y=%d\n", input->mouse_xy.x, input->mouse_xy.y);
					}
				} else if (scene->is_dragging) {
					// already started dragging on a previous frame
					scene->raw_drag_vector = input->drag_vector;
					if (scene->rotation == 0.0f) {
						// Trivial case: no need to rotate the drag vector
						scene->drag_vector = scene->raw_drag_vector;
					} else {
						// Rotate the drag vector along with the scene (still in screen pixel units)
						float drag_length = v2f_length(input->drag_vector);
						float theta = atan2f(input->drag_vector.y, input->drag_vector.x);
						if (drag_length > 0.0f) {
							scene->drag_vector.x = cosf(theta - scene->rotation) * drag_length;
							scene->drag_vector.y = sinf( theta - scene->rotation) * drag_length;
						} else {
							scene->drag_vector = V2F(0.0f, 0.0f);
						}
					}
					// NOTE(pvalkema): this variable rotates along with the scene, should this be the case?
					scene->cumulative_drag_vector.x += scene->drag_vector.x;
					scene->cumulative_drag_vector.y += scene->drag_vector.y;

					// Hide the mouse while panning.
					// Ctrl cancels panning, so in that case the mouse cursor should be shown like normal.
					if (!input->keyboard.key_ctrl.down) {
						mouse_hide();
					} else {
						mouse_show();
					}
				}
				input->drag_vector = v2f();
				scene->is_drag_vector_within_click_tolerance = v2f_length(scene->cumulative_drag_vector) < VIEWER_CLICK_DRAG_TOLERANCE;
			} else {
				if (input->mouse_buttons[0].transition_count != 0) {
					mouse_show();
					scene->is_dragging = false;
					scene->drag_ended = true;
//			        console_print("Drag ended: dx=%d dy=%d\n", input->drag_vector.x, input->drag_vector.y);
				}
				scene->suppress_next_click = false;
			}
		}
	}

	if (base_image->type == IMAGE_TYPE_WSI) {
		if (scene->need_zoom_reset && client_width > 0 && client_height > 0) {
			float abs_cos_rotation = fabsf(cosf(base_image->initial_rotation));
			float abs_sin_rotation = fabsf(sinf(base_image->initial_rotation));
			float rotated_width = abs_cos_rotation * (float)base_image->width_in_pixels +
			                      abs_sin_rotation * (float)base_image->height_in_pixels;
			float rotated_height = abs_sin_rotation * (float)base_image->width_in_pixels +
			                       abs_cos_rotation * (float)base_image->height_in_pixels;
			float times_larger_x = rotated_width / (float)client_width;
			float times_larger_y = rotated_height / (float)client_height;
			float times_larger = MAX(times_larger_x, times_larger_y);
			float desired_zoom_pos = ceilf(log2f(times_larger * 1.1f));

			// By default, allow zooming in up to 4x native resolution
			// If the image is small, allow zooming in further
			i32 suggested_min_level = desired_zoom_pos >= 2.0f ? -2 : (i32)desired_zoom_pos - 4;
			if (viewer_min_level > suggested_min_level) {
				viewer_min_level = suggested_min_level;
			}
			// Don't set the initial zoom level past native resolution for small images
			if (desired_zoom_pos < 0.0f) {
				desired_zoom_pos = 0.0f;
			}

			init_zoom_state(&scene->zoom, desired_zoom_pos, 1.0f, base_image->mpp_x, base_image->mpp_y);
			scene->camera.x = base_image->width_in_um / 2.0f;
			scene->camera.y = base_image->height_in_um / 2.0f;
			scene->rotation = base_image->initial_rotation;
			scene->rotation_target = scene->rotation;
			scene->sin_rotation = sinf(scene->rotation);
			scene->cos_rotation = cosf(scene->rotation);
			scene->need_rotation_animation = false;

			scene->need_zoom_reset = false;
		}
		scene->is_mpp_known = base_image->is_mpp_known;

		zoom_state_t old_zoom = scene->zoom;

		scene->r_minus_l = scene->zoom.pixel_width * (float) client_width;
		scene->t_minus_b = scene->zoom.pixel_height * (float) client_height;

		scene_update_camera_bounds(scene);
		scene->mouse = scene->camera;

		if (input) {
			profiler_begin(PROFILER_SECTION_PROCESS_WSI_INPUT);

			controller_input_t* controller = get_preferred_controller(input);

			scene_update_mouse_pos(scene, input->mouse_xy);

			if (scene->right_clicked) {
				scene->right_clicked_pos = scene->mouse;
			}
            if (scene->clicked) {
                scene->left_clicked_pos = scene->mouse;
            }

			// Panning control
			scene->control = get_2d_control_from_input(input, !gui_want_capture_keyboard);
			float control_length = v2f_length(scene->control);
			if (control_length > 0.0f) {
				scene->time_since_control_start += delta_time;
			} else {
				scene->time_since_control_start = 0.0f;
			}
			scene->panning_velocity = viewer_do_2d_control(scene->panning_velocity, scene->control, delta_time, scene->time_since_control_start, input->keyboard.key_shift.down);


			// Zoom control
			float dlevel = 0.0f;
			float residual_dlevel = scene->zoom_target_state.pos - scene->zoom.pos; // how much the zoom is lagging behind
			float abs_residual_dlevel = fabs(residual_dlevel);
			bool integer_zoom = true; // can override later
			bool use_view_animation = !use_fast_rendering;
			bool32 used_mouse_to_zoom = false;

			// Zoom in or out, or rotate using the mouse wheel.
			if (!gui_want_capture_mouse && input->mouse_z != 0.0f) {
				if (input->keyboard.key_ctrl.down) {
					// Whole wheel notches snap to 5 degree increments. Repeated input while
					// animation is pending accelerates to 10 degrees per notch. SDL preserves
					// fractional trackpad deltas, which remain linear for finer control.
					const float rotation_notch = PI_F / 36.0f;
					float rounded_wheel_delta = roundf(input->mouse_z);
					bool precise_wheel_input =
						fabsf(input->mouse_z - rounded_wheel_delta) > 0.001f;
					if (!scene->need_rotation_animation) {
						scene->rotation_target = scene->rotation;
					}
					if (precise_wheel_input) {
						scene->rotation_target += input->mouse_z * (2.0f * rotation_notch);
					} else {
						float residual_rotation = scene->rotation_target - scene->rotation;
						float notch_multiplier = fabsf(residual_rotation) < 0.6f * rotation_notch
						                             ? 1.499f : 1.999f;
						float new_target = scene->rotation_target +
						                   input->mouse_z * notch_multiplier * rotation_notch;
						scene->rotation_target =
							roundf(new_target / rotation_notch) * rotation_notch;
					}
					scene->need_rotation_animation = true;

				} else if (input->keyboard.key_alt.down && arrlen(app_state->loaded_images) > 1) {
					// Quickly scroll through different layers using the mouse wheel.
					// TODO: fix alt key 'hanging' after e.g. alt+tab
					bool layer_changed = false;
					if (input->mouse_z < 0 && scene->active_layer < arrlen(app_state->loaded_images) - 1) {
						++scene->active_layer;
						layer_changed = true;
					} else if (input->mouse_z > 0 && scene->active_layer > 0) {
						--scene->active_layer;
						layer_changed = true;
					}
					if (layer_changed) {
						layers_window_selected_image_index = app_state->scene.active_layer;
						app_state->last_layer_enabled = app_state->scene.active_layer;
						scene->layer_time = 0.0f;
						scene->target_layer_time = 0.0f;
						for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
							app_state->loaded_images[i]->is_enabled = (app_state->scene.active_layer == i);
						}
						recount_enabled_images(app_state);
					}
				} else {
					if (use_view_animation) {
						// We want to snap to multiples of 0.5 levels when zooming in discrete increments using the mouse wheel.
						// Pro: gives better control than zooming in whole levels; con: makes zooming in/out rather slow.
						// Trick: advance the 'zoom target' in increments of ~0.75 or more levels, but round down to the nearest 0.5 once we are close by.
						// This allows slow zooming (0.5 per mouse wheel tick), but if you move fast you can accelerate.
						if (abs_residual_dlevel < 0.3f) {
							dlevel = -input->mouse_z * 0.7499f;
						} else {
							dlevel = -input->mouse_z * 0.9999f;
						}
					} else {
						dlevel = -input->mouse_z;
					}

					used_mouse_to_zoom = true;
				}

			}

			if (scene->need_rotation_animation) {
				float rotation_delta = scene->rotation_target - scene->rotation;
				float abs_rotation_delta = fabsf(rotation_delta);
				if (!use_view_animation || abs_rotation_delta < 1e-5f) {
					scene->rotation = scene->rotation_target;
					scene->need_rotation_animation = false;
					if (fabsf(scene->rotation) >= 2.0f * PI_F) {
						scene->rotation = fmodf(scene->rotation, 2.0f * PI_F);
						scene->rotation_target = scene->rotation;
					}
				} else {
					float sign_rotation_delta = signbit(rotation_delta) ? -1.0f : 1.0f;
					float linear_catch_up_speed = 2.0f * delta_time;
					float exponential_catch_up_speed = 16.0f * delta_time;
					if (abs_rotation_delta > linear_catch_up_speed) {
						rotation_delta =
							(linear_catch_up_speed +
							 (abs_rotation_delta - linear_catch_up_speed) *
							 exponential_catch_up_speed) * sign_rotation_delta;
					}
					scene->rotation += rotation_delta;
				}
				scene->sin_rotation = sinf(scene->rotation);
				scene->cos_rotation = cosf(scene->rotation);
				app_state->allow_idling_next_frame = false;
			}

			float key_repeat_interval = 0.15f; // in seconds

			// Zoom out using Z or / or controller button B
			bool zoom_out_button_held = input->controllers[0].button_b.down ||
			                            (!gui_want_capture_keyboard && !input->keyboard.key_ctrl.down &&
			                             (is_key_down(input, KEY_Z) || is_key_down(input, KEY_Slash)));
			// Zoom in using X or . or controller button A
			bool zoom_in_button_held = input->controllers[0].button_a.down ||
			                           (!gui_want_capture_keyboard && !input->keyboard.key_ctrl.down &&
			                           	(is_key_down(input, KEY_X) || is_key_down(input, KEY_Period)));

			if (prefer_integer_zoom) {
				if (zoom_out_button_held) {
					if (was_key_pressed(input, KEY_Z) || was_key_pressed(input, KEY_Slash) || was_button_pressed(&input->controllers[0].button_b)) {
						dlevel += 1.0f;
						zoom_in_key_hold_down_start_time = get_clock();
						zoom_in_key_times_zoomed_while_holding = 0;
					} else {
						float time_elapsed = get_seconds_elapsed(zoom_in_key_hold_down_start_time, get_clock());
						int zooms = (int) (time_elapsed / key_repeat_interval);
						if ((zooms - zoom_in_key_times_zoomed_while_holding) == 1) {
							zoom_in_key_times_zoomed_while_holding = zooms;
							dlevel += 1.0f;
						}
					}
				}

				if (zoom_in_button_held) {
					if (was_key_pressed(input, KEY_X) || was_key_pressed(input, KEY_Period) || was_button_pressed(&input->controllers[0].button_a)) {
						dlevel -= 1.0f;
						zoom_out_key_hold_down_start_time = get_clock();
						zoom_out_key_times_zoomed_while_holding = 0;
					} else {
						float time_elapsed = get_seconds_elapsed(zoom_out_key_hold_down_start_time, get_clock());
						int zooms = (int) (time_elapsed / key_repeat_interval);
						if ((zooms - zoom_out_key_times_zoomed_while_holding) == 1) {
							zoom_out_key_times_zoomed_while_holding = zooms;
							dlevel -= 1.0f;
						}
					}
				}
			} else {
				// non-integer zoom
				float zoom_speed = 6.0f;
				if (input->keyboard.key_shift.down) {
					zoom_speed *= 2.0f;
				}
				if (zoom_out_button_held) {
					dlevel += zoom_speed * delta_time;
					integer_zoom = false;
				}
				if (zoom_in_button_held) {
					dlevel -= zoom_speed * delta_time;
					integer_zoom = false;
				}
			}

			if (controller->left_trigger.has_input) {
				dlevel += controller->left_trigger.end * 7.0f * delta_time;
				integer_zoom = false;
			}
			if (controller->right_trigger.has_input) {
				dlevel -= controller->right_trigger.end * 7.0f * delta_time;
				integer_zoom = false;
			}

			bool need_set_zoom_pivot = (dlevel != 0.0f);
			if (dlevel != 0.0f || residual_dlevel != 0.0f) {
//		        console_print("mouse_z = %d\n", input->mouse_z);

				float new_level = scene->zoom.pos + dlevel;
				if (scene->need_zoom_animation) {
					new_level += residual_dlevel;
				}
				if (integer_zoom) {
					// snap to multiples of 0.5
					if (fabs(residual_dlevel) < 0.75f) {
						new_level = roundf(new_level * 2.0f) * 0.5f;
					}
				}

				new_level = CLAMP(new_level, viewer_min_level, viewer_max_level);
				zoom_state_t new_zoom = scene->zoom;
				zoom_update_pos(&new_zoom, (float) new_level);

				if (new_zoom.pos != old_zoom.pos) {
					if (need_set_zoom_pivot) {
						// TODO: fix pivot point calculation when rotated
						if (used_mouse_to_zoom) {
							scene->zoom_pivot = scene->mouse;
//							scene->zoom_pivot.x = CLAMP(scene->zoom_pivot.x, 0, base_image->width_in_um);
//							scene->zoom_pivot.y = CLAMP(scene->zoom_pivot.y, 0, base_image->height_in_um);
						} else {
							scene->zoom_pivot = scene->camera;
						}
					}
					scene->zoom_target_state = new_zoom;
					scene->need_zoom_animation = true;
				}

			}

			if (scene->need_zoom_animation) {
				if (!use_view_animation) {
					// skip animation by jumping to the target state immediately
					zoom_update_pos(&scene->zoom, scene->zoom_target_state.pos);
				}
				float d_zoom = scene->zoom_target_state.pos - scene->zoom.pos;

				// Stop the zoom animation when the remaining movement is more or less imperceptible
				float abs_d_zoom = fabsf(d_zoom);
				if (abs_d_zoom < 1e-5f) {
					scene->need_zoom_animation = false;
				}

				// Calculate the change in zoom position we want for this frame
				float sign_d_zoom = signbit(d_zoom) ? -1.0f : 1.0f;
				float linear_catch_up_speed = 2.0f * delta_time;
				float exponential_catch_up_speed = 16.0f * delta_time;
				if (abs_d_zoom > linear_catch_up_speed) {
					d_zoom = (linear_catch_up_speed + (abs_d_zoom - linear_catch_up_speed) * exponential_catch_up_speed) *
					         sign_d_zoom;
				}

				zoom_update_pos(&scene->zoom, scene->zoom.pos + d_zoom);

				// Get the position of the pivot point on the screen, relative to the width/height of the screen
				// The values are normalized between -0.5 and +0.5, where (0,0) is the center of the screen (= camera pos)
				polygon4v2f rotated_camera_rect_origin = rotated_rectangle(scene->r_minus_l, scene->t_minus_b, scene->rotation);
				v2f rotated_r_minus_l = v2f_subtract(rotated_camera_rect_origin.topright, rotated_camera_rect_origin.topleft);
				v2f rotated_t_minus_b = v2f_subtract(rotated_camera_rect_origin.bottomleft, rotated_camera_rect_origin.topleft);
				v2f pivot_relative_to_camera = v2f_subtract(scene->zoom_pivot, scene->camera);
				float pivot_along_r_minus_l = v2f_dot(rotated_r_minus_l, pivot_relative_to_camera);
				float pivot_along_t_minus_b = v2f_dot(rotated_t_minus_b, pivot_relative_to_camera);
				pivot_along_r_minus_l /= SQUARE(scene->r_minus_l);
				pivot_along_t_minus_b /= SQUARE(scene->t_minus_b);

				// Now, calculate the new camera position
				// First, get a new resized (possibly rotated) camera rect with correct the dimensions for the new zoom level
				scene->r_minus_l = scene->zoom.pixel_width * (float) client_width;
				scene->t_minus_b = scene->zoom.pixel_height * (float) client_height;
				polygon4v2f new_camera_rect = rotated_rectangle(scene->r_minus_l, scene->t_minus_b, scene->rotation);

				// Reposition the camera so that the zoom pivot point remains in the same relative position on-screen
				// (Basically we do the reverse of the steps above.)
				rotated_r_minus_l = v2f_subtract(new_camera_rect.topright, new_camera_rect.topleft);
				rotated_t_minus_b = v2f_subtract(new_camera_rect.bottomleft, new_camera_rect.topleft);
				pivot_relative_to_camera = v2f_add(v2f_scale(pivot_along_r_minus_l, rotated_r_minus_l), v2f_scale(pivot_along_t_minus_b, rotated_t_minus_b));
				scene->camera = v2f_subtract(scene->zoom_pivot, pivot_relative_to_camera);
				scene_update_camera_bounds(scene);

				// The camera was updated, so we need to update the mouse position
				scene_update_mouse_pos(scene, input->mouse_xy);

			}

			if (scene->need_zoom_animation) {
				app_state->allow_idling_next_frame = false;
			}

            bool is_directional_input_consumed = scene_control_layers(app_state, scene, input, delta_time);

			// Panning should be faster when zoomed in very far.
			float panning_multiplier = 1.0f + 3.0f * ((float) viewer_max_level - scene->zoom.pos) / (float) viewer_max_level;
            panning_multiplier *= app_state->display_scale_factor;

            if (!is_directional_input_consumed) {
                // Panning using the arrow or WASD keys.
                float panning_speed = app_state->keyboard_base_panning_speed * 100.0f * delta_time * panning_multiplier;
                v2f world_panning_velocity = v2f_rotate(scene->panning_velocity, -scene->rotation);
                bool panning = false;
                if (world_panning_velocity.y != 0.0f) {
                    scene->camera.y += scene->zoom.pixel_height * panning_speed * world_panning_velocity.y;
                    panning = true;
                }
                if (world_panning_velocity.x != 0.0f) {
                    scene->camera.x += scene->zoom.pixel_height * panning_speed * world_panning_velocity.x;
                    panning = true;
                }
                if (panning && app_state->seconds_without_mouse_movement > 0.25f) {
                    mouse_hide();
                }
            }

			// camera has been updated (now we need to recalculate some things)
			scene->r_minus_l = scene->zoom.pixel_width * (float) client_width;
			scene->t_minus_b = scene->zoom.pixel_height * (float) client_height;
			scene_update_camera_bounds(scene);
			scene_update_mouse_pos(scene, input->mouse_xy);

			if (!gui_want_capture_keyboard) {
				u32 key_modifiers_without_shift = input->keyboard.modifiers & ~KMOD_SHIFT;
				bool redo_shortcut = (was_key_pressed(input, KEY_Y) && input->keyboard.modifiers == KMOD_CTRL) ||
				                     (was_key_pressed(input, KEY_Z) && input->keyboard.modifiers == (KMOD_CTRL | KMOD_SHIFT));
				if (redo_shortcut) {
					annotation_history_redo(&scene->annotation_set);
				} else if (was_key_pressed(input, KEY_Z) && input->keyboard.modifiers == KMOD_CTRL) {
					annotation_history_undo(&scene->annotation_set);
				}

				// Toggle the grid
				if (was_key_pressed(input, KEY_G) && key_modifiers_without_shift == KMOD_CTRL) {
					scene->enable_grid = !scene->enable_grid;
				}

				// Toggle the scale bar
				if (was_key_pressed(input, KEY_B) && key_modifiers_without_shift == KMOD_CTRL) {
					scene->scale_bar.enabled = !scene->scale_bar.enabled;
				}

				// Switch to annotation creation tools
				if (was_key_pressed(input, KEY_Q) && key_modifiers_without_shift == 0) {
					viewer_switch_tool(app_state, TOOL_CREATE_POINT);
				} else if (was_key_pressed(input, KEY_M) && key_modifiers_without_shift == 0) {
					viewer_switch_tool(app_state, TOOL_CREATE_LINE);
				} else if (was_key_pressed(input, KEY_F) && key_modifiers_without_shift == 0) {
					viewer_switch_tool(app_state, TOOL_CREATE_FREEFORM);
//			    } else if (was_key_pressed(input, KEY_E) && key_modifiers_without_shift == 0) {
//				    viewer_switch_tool(app_state, TOOL_CREATE_ELLIPSE);
				} else if (was_key_pressed(input, KEY_R) && key_modifiers_without_shift == 0) {
					viewer_switch_tool(app_state, TOOL_CREATE_RECTANGLE);
//			    } else if (was_key_pressed(input, KEY_T) && key_modifiers_without_shift == 0) {
//				    viewer_switch_tool(app_state, TOOL_CREATE_TEXT);
				}

				/*if (was_key_pressed(input, KEY_O)) {
					app_state->mouse_mode = MODE_CREATE_SELECTION_BOX;
				console_print("switching to creation mode\n");
				}*/

				// Toggle image adjustments
				if (!gui_want_capture_keyboard && was_key_pressed(input, KEY_P)) {
					app_state->use_image_adjustments = !app_state->use_image_adjustments;
				}

			}

			// Debug feature: view 'frozen' outline of camera bounds
#if DO_DEBUG
			if (!gui_want_capture_keyboard && was_key_pressed(input, KEY_F8)) {
				if (scene->restrict_load_bounds) {
					scene->restrict_load_bounds = false;
				} else {
					scene->tile_load_bounds = scene->camera_bounds;
					scene->restrict_load_bounds = true;
				}
			}
			if (scene->restrict_load_bounds) {
				gui_draw_bounds_in_scene(scene->tile_load_bounds, RGBA(0,0,0,128), 2.0f, scene, NULL);
			}
#endif

			update_scale_bar(scene, &scene->scale_bar);

			if (app_state->mouse_mode == MODE_VIEW) {
				v2f mouse = input->mouse_xy;
				if (scene->drag_started && v2f_between_points(mouse, scene->scale_bar.pos, scene->scale_bar.pos_max)) {
					scene->scale_bar.drag_start_offset = V2F(mouse.x - scene->scale_bar.pos.x,
															 mouse.y - scene->scale_bar.pos.y);
					app_state->mouse_mode = MODE_DRAG_SCALE_BAR;
				}
			}

			if (app_state->mouse_mode == MODE_VIEW) {
				if (scene->is_dragging && v2f_length(scene->cumulative_drag_vector) >= VIEWER_CLICK_DRAG_TOLERANCE && !input->keyboard.key_ctrl.down) {
					float final_multiplier = panning_multiplier * app_state->mouse_sensitivity * 0.1f;
					scene->camera.x -= scene->drag_vector.x * scene->zoom.pixel_width * final_multiplier;
					scene->camera.y -= scene->drag_vector.y * scene->zoom.pixel_height * final_multiplier;

					// camera has been updated (now we need to recalculate some things)
					scene_update_camera_bounds(scene);
					scene_update_mouse_pos(scene, input->mouse_xy);
				}

				if (!gui_want_capture_mouse) {
					// try to hover over / select an annotation
					if (scene->annotation_set.active_annotation_count > 0) {
						interact_with_annotations(app_state, scene, input);
//				    	float selection_ms = get_seconds_elapsed(select_begin, get_clock()) * 1000.0f;
//			    	    console_print("Selecting took %g ms.\n", selection_ms);
					}

				}

			} else if (app_state->mouse_mode == MODE_CREATE_SELECTION_BOX) {
				if (!gui_want_capture_mouse) {
					if (scene->drag_started) {
						scene->selection_box = RECT2F(scene->mouse.x, scene->mouse.y, 0.0f, 0.0f);
						scene->has_selection_box = true;
					} else if (scene->is_dragging) {
						scene->selection_box.w = scene->mouse.x - scene->selection_box.x;
						scene->selection_box.h = scene->mouse.y - scene->selection_box.y;
					} else if (scene->drag_ended) {
						app_state->mouse_mode = MODE_VIEW;

					}

				}
			} else if (app_state->mouse_mode == MODE_INSERT) {
				if (!gui_want_capture_mouse) {
					if (app_state->mouse_tool == TOOL_CREATE_POINT) {
						if (scene->clicked) {
							create_point_annotation(&scene->annotation_set, scene->mouse);
//							console_print("Creating a point\n");
							viewer_switch_tool(app_state, TOOL_NONE);
						} else if (was_key_pressed(input, KEY_Escape)) {
							viewer_switch_tool(app_state, TOOL_NONE);
						}
					} else if (app_state->mouse_tool == TOOL_CREATE_LINE) {
						do_mouse_tool_create_line(app_state, input, scene, annotation_set);
					} else if (app_state->mouse_tool == TOOL_CREATE_FREEFORM) {
						do_mouse_tool_create_freeform(app_state, input, scene, annotation_set);
					} else if (app_state->mouse_tool == TOOL_CREATE_ELLIPSE) {
						if (scene->drag_started) {
							create_ellipse_annotation(annotation_set, scene->mouse);
						} else if (scene->is_dragging) {
							if (annotation_set->editing_annotation_index >= 0) {
								annotation_t* ellipse = get_active_annotation(annotation_set, annotation_set->editing_annotation_index);
								ellipse->p1 = scene->mouse;
							}
						} else if (scene->drag_ended) {
							// finalize ellipse
							viewer_switch_tool(app_state, TOOL_NONE);
						} else if (was_key_pressed(input, KEY_Escape)) {
							viewer_switch_tool(app_state, TOOL_NONE);
						}
					} else if (app_state->mouse_tool == TOOL_CREATE_RECTANGLE) {
						do_mouse_tool_create_rectangle(app_state, input, scene, annotation_set);
					} else if (app_state->mouse_tool == TOOL_CREATE_TEXT) {
						if (scene->clicked) {
							// create text
							viewer_switch_tool(app_state, TOOL_NONE);
						}
					}
				}
			}

			scene_determine_if_export_is_possible(scene, base_image);

			// Update dragging of objects
			if (app_state->mouse_mode == MODE_DRAG_ANNOTATION_NODE) {
				if (scene->is_dragging) {
					do_drag_annotation_node(scene);
				} else if (scene->drag_ended) {
					annotation_history_end_action(&scene->annotation_set);
					app_state->mouse_mode = MODE_VIEW;
//					scene->annotation_set.is_edit_mode = false;

				}

			} else if (app_state->mouse_mode == MODE_DRAG_SCALE_BAR) {
				scale_bar_t* scale_bar = &scene->scale_bar;
				if (scene->is_dragging && v2f_length(scene->cumulative_drag_vector) >= VIEWER_CLICK_DRAG_TOLERANCE) {
					// Update the position of the scale bar while dragging the mouse.
#if WINDOWS
					scale_bar->pos.x = input->mouse_xy.x - scale_bar->drag_start_offset.x;
					scale_bar->pos.y = input->mouse_xy.y - scale_bar->drag_start_offset.y;
#else
					// TODO: figure out why on macOS, input->mouse_xy is {0,0} while dragging
					scale_bar->pos.x += scene->raw_drag_vector.x;
					scale_bar->pos.y += scene->raw_drag_vector.y;
#endif
//					console_print_verbose("mouse = %d, %d\n", input->mouse_xy.x, input->mouse_xy.y);
					update_scale_bar(scene, scale_bar);
				} else if (scene->drag_ended) {
					app_state->mouse_mode = MODE_VIEW;
					update_scale_bar(scene, scale_bar);
				}
			}


			// Ctrl+S: save annotations manually
			if (scene->annotation_set.modified) {
				if (was_key_pressed(input, KEY_S) && input->keyboard.key_ctrl.down) {
					save_annotations(app_state, &scene->annotation_set, true, false);
				}
			}

			/*if (scene->clicked && !gui_want_capture_mouse) {
				scene->has_selection_box = false; // deselect selection box
			}*/
			profiler_end(PROFILER_SECTION_PROCESS_WSI_INPUT);
		}
	
		// Visualize the rectangles retreived from the 'valid data envelopes' (for debugging)
		if (scene->draw_envelopes) {
			image_t* image = app_state->loaded_images[0];
			for (i32 i = 0; i < image->clip_rect_count; ++i) {
				rect2f* rectangle = &image->clip_rects[i];
				temp_memory_t temp_memory = begin_temp_memory_on_local_thread();
				v2f* points = arena_push_array(temp_memory.arena, 4, v2f);
				points[0] = V2F(rectangle->x, rectangle->y);
				points[1] = V2F(rectangle->x + rectangle->w, rectangle->y);
				points[2] = V2F(rectangle->x + rectangle->w, rectangle->y + rectangle->h);
				points[3] = V2F(rectangle->x, rectangle->y + rectangle->h);
				gui_draw_polygon_outline_in_scene(points, 4, RGBA(255, 0, 0, 255), true, 3.0f, scene, NULL);
				release_temp_memory(&temp_memory);
			}
		}

		draw_grid(scene);
//		i64 start = get_clock();

		profiler_begin(PROFILER_SECTION_DRAW_ANNOTATIONS);
		draw_annotations(app_state, scene, &scene->annotation_set, scene->camera_bounds.min);
		profiler_end(PROFILER_SECTION_DRAW_ANNOTATIONS);

//		console_print_verbose("draw_annotations() took %.01f ms\n", get_seconds_elapsed(start, get_clock()) * 1000.0f);
		draw_selection_box(scene);
		draw_scale_bar(&scene->scale_bar);
	}

	if (app_state->enabled_image_count <= 1) {
		// Render everything at once
		renderer_clear_render_target(app_state->clear_color, client_width, client_height);
		image_t* image = app_state->loaded_images[app_state->enabled_image_indices[0]];
		update_and_render_image(app_state, image);
	} else {
		// We are rendering the scene in two passes.
		// 1: render to framebuffer
		// 2: blit framebuffer to screen

		renderer_ensure_layer_render_targets(app_state);

		i32 running_framebuffer_index = 0;
		// TODO: iterate over enabled image indices?
		for (i32 image_index = 0; image_index < image_count; ++image_index) {

			image_t* image = app_state->loaded_images[image_index];
			if (image->is_enabled) {
				profiler_begin(PROFILER_SECTION_RENDER_IMAGE);
				renderer_prepare_layer_render_target(running_framebuffer_index, client_width, client_height, app_state->clear_color);

				update_and_render_image(app_state, image);

				renderer_bind_screen_render_target();
				++running_framebuffer_index;
				profiler_end(PROFILER_SECTION_RENDER_IMAGE);
			}
			if (running_framebuffer_index == 2) {
				break; // only 2 images can be drawn together at the same time
			}
		}

		// Second pass
		renderer_clear_render_target(app_state->clear_color, client_width, client_height);
		renderer_final_blit_layers(scene->layer_time);
	}


	do_after_scene_render(app_state, input);
}

void do_after_scene_render(app_state_t* app_state, input_t* input) {
	// Hotkeys that never produce text (e.g. F-keys): ignore gui_want_capture_keyboard
	if (was_key_pressed(input, KEY_F1)) {
		if (input->keyboard.modifiers & KMOD_CTRL) {
			show_debugging_window = !show_debugging_window; // Ctrl+F1
		} else {
			show_demo_window = !show_demo_window; // F1
		}
	}
	if (was_key_pressed(input, KEY_F2)) {
		show_profiler_window = !show_profiler_window;
	}
	if (was_key_pressed(input, KEY_F3) || was_key_pressed(input, KEY_Grave)) {
		show_console_window = !show_console_window;
	}
	// TODO: fix 'sticky' Alt key after Alt+Enter
	if (was_key_pressed(input, KEY_F12) && input->keyboard.key_alt.down) {
		show_menu_bar = !show_menu_bar;
		viewer_save_options(app_state);
	}
	if (was_key_pressed(input, KEY_F6)) {
		// Load the next image dragged on top of the window as a new layer/overlay instead of a base image.
		if (arrlen(app_state->loaded_images) >= 1) {
			load_next_image_as_overlay = true;
			viewer_save_options(app_state);
		}
	}

	// Hotkeys for keys that could produce text: respect gui_want_capture_keyboard conditionally
	u32 key_modifiers_without_shift = input->keyboard.modifiers & ~KMOD_SHIFT;
	if (key_modifiers_without_shift == KMOD_CTRL) {
		// Hotkeys with Ctrl pressed: ignore gui_want_capture_keyboard, ignore Shift (for now)
		if (was_key_pressed(input, KEY_L)) {
			show_layers_window = !show_layers_window; // Ctrl+L (open Layers window)
		}
		if (was_key_pressed(input, KEY_U)) {
			show_open_uri_window = true; // Ctrl+U (open URI window)
		}
	} else if (key_modifiers_without_shift == 0) {
		// Hotkeys without modifiers (or only Shift, which is ignored): respect gui_want_capture_keyboard
		if (!gui_want_capture_keyboard) {
			if (was_key_pressed(input, KEY_L)) {
				show_label_image_window = !show_label_image_window; // L (label)
			}
			if (was_key_pressed(input, KEY_O)) {
				show_macro_image_window = !show_macro_image_window; // O (overview)
			}
			if (was_key_pressed(input, KEY_H)) {
				app_state->scene.enable_annotations = !app_state->scene.enable_annotations; // H (hide annotations)
			}
			if (was_key_pressed(input, KEY_G)) {
				show_annotation_group_assignment_window = !show_annotation_group_assignment_window;
			}
		}
	}

	// NOTE: some more hotkeys are handled directly in viewer_update_and_render()
	// TODO: maybe handle all hotkeys in one location?

	profiler_begin(PROFILER_SECTION_GUI_DRAW);
	gui_draw(app_state, curr_input, app_state->client_viewport.w, app_state->client_viewport.h);
	profiler_end(PROFILER_SECTION_GUI_DRAW);
//	last_section = profiler_end_section(last_section, "gui draw", 10.0f);

	autosave(app_state, false, true);
//	last_section = profiler_end_section(last_section, "autosave", 10.0f);

	if (need_quit) {
		if (!app_state->enable_autosave && app_state->scene.annotation_set.modified) {
			show_save_quit_prompt = true;
		} else {
			is_program_running = false;
		}
	}

	//renderer_finish();

	float update_and_render_time = get_seconds_elapsed(app_state->last_frame_start, get_clock());
//	console_print("Frame time: %g ms\n", update_and_render_time * 1000.0f);

	++app_state->frame_counter;
}
