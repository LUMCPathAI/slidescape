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
#include "presenter.h"

typedef struct app_state_t app_state_t;
struct ImDrawData;

typedef struct presenter_backend_t {
	bool (*init_window)(const presenter_window_desc_t* desc, window_handle_t* out_window, void** out_present_handle);
	void (*init_imgui)(app_state_t* app_state, window_handle_t window);
	void (*imgui_new_frame)();
	void (*render_imgui_draw_data)(ImDrawData* draw_data);
	bool (*render_cached_imgui_draw_data)(ImDrawData* draw_data, u32 generation);
	void (*set_swap_interval)(int interval);
	int (*get_refresh_rate)(window_handle_t window, void* present_handle);
	void (*get_drawable_size)(window_handle_t window, i32* out_width, i32* out_height);
	void (*present)(window_handle_t window, void* present_handle);
	void (*shutdown)(window_handle_t window, void* present_handle);
} presenter_backend_t;

presenter_backend_t presenter_opengl_get_backend();
