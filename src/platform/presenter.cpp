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
#include "presenter.h"
#include "presenter_backend.h"
#include "renderer.h"

typedef struct presenter_t {
	presenter_api_t api;
	presenter_backend_t backend;
	window_handle_t window;
	void* present_handle;
	bool initialized;
} presenter_t;

static presenter_t presenter;

static presenter_backend_t presenter_get_backend(presenter_api_t api) {
	presenter_backend_t result = {};
	switch (api) {
		case PRESENTER_API_OPENGL: {
			result = presenter_opengl_get_backend();
		} break;
	}
	return result;
}

bool presenter_init_window(const presenter_window_desc_t* desc, presenter_api_t api) {
	presenter.api = api;
	presenter.backend = presenter_get_backend(api);
	presenter.window = NULL;
	presenter.present_handle = NULL;
	presenter.initialized = false;

	presenter.initialized = presenter.backend.init_window(desc, &presenter.window, &presenter.present_handle);

	return presenter.initialized;
}

window_handle_t presenter_get_window() {
	return presenter.window;
}

void presenter_init_viewer(app_state_t* app_state) {
	renderer_init(app_state);
}

void presenter_init_imgui(app_state_t* app_state) {
	presenter.backend.init_imgui(app_state, presenter.window);
}

void presenter_imgui_new_frame() {
	presenter.backend.imgui_new_frame();
}

void presenter_render_imgui_draw_data(struct ImDrawData *draw_data) {
	presenter.backend.render_imgui_draw_data(draw_data);
}

void presenter_render_cached_imgui_draw_data(struct ImDrawData *draw_data, u32 generation) {
	if (presenter.backend.render_cached_imgui_draw_data) {
		if (presenter.backend.render_cached_imgui_draw_data(draw_data, generation)) {
			return;
		}
	}
	presenter.backend.render_imgui_draw_data(draw_data);
}

void presenter_set_swap_interval(int interval) {
	presenter.backend.set_swap_interval(interval);
}

int presenter_get_refresh_rate() {
	return presenter.backend.get_refresh_rate(presenter.window, presenter.present_handle);
}

void presenter_set_viewport(i32 width, i32 height) {
	renderer_set_viewport(width, height);
}

void presenter_get_drawable_size(i32* out_width, i32* out_height) {
	presenter.backend.get_drawable_size(presenter.window, out_width, out_height);
}

void presenter_present() {
	presenter.backend.present(presenter.window, presenter.present_handle);
}

void presenter_shutdown() {
	if (presenter.initialized) {
		presenter.backend.shutdown(presenter.window, presenter.present_handle);
	}
	presenter.window = NULL;
	presenter.present_handle = NULL;
	presenter.initialized = false;
}

bool presenter_can_present() {
	return presenter.initialized && presenter.window;
}
