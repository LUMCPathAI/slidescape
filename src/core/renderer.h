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
#include <linmath.h>

typedef struct app_state_t app_state_t;
typedef struct image_t image_t;
typedef struct pixel_transfer_state_t pixel_transfer_state_t;
typedef struct scene_t scene_t;

typedef u32 renderer_texture_handle_t;

typedef enum renderer_pixel_format_t {
	RENDERER_PIXEL_FORMAT_BGRA,
	RENDERER_PIXEL_FORMAT_RGBA,
} renderer_pixel_format_t;

void renderer_begin_image_render(app_state_t* app_state, scene_t* scene, mat4x4 projection_view_matrix);
void renderer_set_image_model_matrix(mat4x4 model_matrix);
void renderer_draw_textured_rect(renderer_texture_handle_t texture);
void renderer_disable_stencil_test();
void renderer_begin_stencil_write();
void renderer_end_stencil_write();
void renderer_set_tile_blend_enabled(bool enabled);
void renderer_finish_image_render();
void renderer_clear_render_target(v4f clear_color, i32 client_width, i32 client_height);
void renderer_ensure_layer_render_targets(app_state_t* app_state);
void renderer_prepare_layer_render_target(i32 target_index, i32 width, i32 height, v4f clear_color);
void renderer_bind_screen_render_target();
void renderer_final_blit_layers(float layer_time);
renderer_texture_handle_t renderer_get_dummy_texture();
renderer_texture_handle_t renderer_get_transparent_texture();
renderer_texture_handle_t renderer_create_texture(void* pixels, i32 width, i32 height, renderer_pixel_format_t pixel_format);
void renderer_destroy_texture(renderer_texture_handle_t texture);
void renderer_destroy_image_resources(app_state_t* app_state, image_t* image);
pixel_transfer_state_t* renderer_submit_texture_upload(app_state_t *app_state, i32 width, i32 height,
														i32 bytes_per_pixel, u8 *pixels, bool finalize);
void renderer_finalize_texture_upload(pixel_transfer_state_t* transfer_state);
void renderer_upload_tile_on_worker_thread(image_t* image, void* tile_pixels, i32 scale, i32 tile_index, i32 tile_width, i32 tile_height);
void renderer_init(app_state_t* app_state);
void renderer_set_viewport(i32 width, i32 height);
void renderer_finish();

extern bool finalize_textures_immediately;

#ifdef __cplusplus
}
#endif
