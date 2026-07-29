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

#include "common.h"
#include "graphical_app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum presenter_api_t {
	PRESENTER_API_OPENGL = 0,
} presenter_api_t;

typedef struct presenter_window_desc_t {
	window_handle_t existing_window;
	const char* title;
	i32 width;
	i32 height;
	bool start_maximized;
	void* native_instance;
	const char* native_window_class_name;
} presenter_window_desc_t;

typedef struct app_state_t app_state_t;
struct ImDrawData;

bool presenter_init_window(const presenter_window_desc_t* desc, presenter_api_t api);
window_handle_t presenter_get_window();
void presenter_init_viewer(app_state_t* app_state);
void presenter_init_imgui(app_state_t* app_state);
void presenter_imgui_new_frame();
void presenter_render_imgui_draw_data(struct ImDrawData *draw_data);
void presenter_render_cached_imgui_draw_data(struct ImDrawData *draw_data, u32 generation);
void presenter_set_swap_interval(int interval);
int presenter_get_refresh_rate();
void presenter_set_viewport(i32 width, i32 height);
void presenter_get_drawable_size(i32* out_width, i32* out_height);
void presenter_present();
void presenter_shutdown();
bool presenter_can_present();

#ifdef __cplusplus
}
#endif
