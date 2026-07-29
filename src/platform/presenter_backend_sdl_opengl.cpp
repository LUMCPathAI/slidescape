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
#include "presenter_backend.h"
#include "presenter_imgui_opengl_cached.h"
#include "viewer.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"

// About Desktop OpenGL function loaders:
//  Modern desktop OpenGL doesn't have a standard portable header file to load OpenGL function pointers.
//  Helper libraries are often used for this purpose. Keep loader setup backend-private.
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
#include <GL/gl3w.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
#include <GL/glew.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
#include <glad/glad.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
#include <glad/gl.h>
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
#define GLFW_INCLUDE_NONE
#include <glbinding/Binding.h>
#include <glbinding/gl/gl.h>
using namespace gl;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
#define GLFW_INCLUDE_NONE
#include <glbinding/glbinding.h>
#include <glbinding/gl/gl.h>
using namespace gl;
#else
#include IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#endif

static SDL_GLContext sdl_opengl_context;
static const char* sdl_opengl_glsl_version;

static bool presenter_sdl_opengl_init_window(const presenter_window_desc_t* desc, window_handle_t* out_window, void** out_present_handle) {
#ifdef __APPLE__
	// GL 3.2 Core + GLSL 150
	sdl_opengl_glsl_version = "#version 150";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
	// GL 3.3 Core + GLSL 130
	sdl_opengl_glsl_version = "#version 130";
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	u32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (desc->start_maximized) {
		window_flags |= SDL_WINDOW_MAXIMIZED;
	}

	SDL_Window* window = SDL_CreateWindow(desc->title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, desc->width, desc->height, window_flags);
	if (!window) {
		console_print_error("Error creating SDL OpenGL window: %s\n", SDL_GetError());
		return false;
	}

	sdl_opengl_context = SDL_GL_CreateContext(window);
	if (!sdl_opengl_context) {
		console_print_error("Error creating SDL OpenGL context: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		return false;
	}
	SDL_GL_MakeCurrent(window, sdl_opengl_context);

	char* version_string = (char*)glGetString(GL_VERSION);
	console_print("OpenGL supported version: %s\n", version_string);

#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
	bool err = gl3wInit() != 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
	bool err = glewInit() != GLEW_OK;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
	bool err = gladLoadGL() == 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
	bool err = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) == 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
	bool err = false;
	glbinding::Binding::initialize();
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
	bool err = false;
	glbinding::initialize([](const char* name) { return (glbinding::ProcAddress)SDL_GL_GetProcAddress(name); });
#else
	bool err = false;
#endif
	if (err) {
		console_print_error("Failed to initialize OpenGL loader.\n");
		SDL_GL_DeleteContext(sdl_opengl_context);
		sdl_opengl_context = NULL;
		SDL_DestroyWindow(window);
		return false;
	}

	*out_window = window;
	if (out_present_handle) {
		*out_present_handle = NULL;
	}
	return true;
}

static void presenter_sdl_opengl_init_imgui(app_state_t* app_state, window_handle_t window) {
	(void)app_state;
	ImGui_ImplSDL2_InitForOpenGL(window, sdl_opengl_context);
	ImGui_ImplOpenGL3_Init(sdl_opengl_glsl_version);
}

static void presenter_sdl_opengl_imgui_new_frame() {
	ImGui_ImplOpenGL3_NewFrame();
}

static void presenter_sdl_opengl_render_imgui_draw_data(ImDrawData* draw_data) {
	ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

static bool presenter_sdl_opengl_render_cached_imgui_draw_data(ImDrawData* draw_data, u32 generation) {
	return presenter_imgui_opengl_render_cached_draw_data(draw_data, generation);
}

static void presenter_sdl_opengl_set_swap_interval(int interval) {
	SDL_GL_SetSwapInterval(interval);
}

static int presenter_sdl_opengl_get_refresh_rate(window_handle_t window, void* present_handle) {
	(void)present_handle;
	int refresh_rate = 0;
	int display_index = SDL_GetWindowDisplayIndex(window);
	if (display_index >= 0) {
		SDL_DisplayMode mode = {};
		if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0) {
			refresh_rate = mode.refresh_rate;
		}
	}
	return refresh_rate;
}

static void presenter_sdl_opengl_get_drawable_size(window_handle_t window, i32* out_width, i32* out_height) {
	SDL_GL_GetDrawableSize(window, out_width, out_height);
}

static void presenter_sdl_opengl_present(window_handle_t window, void* present_handle) {
	(void)present_handle;
	SDL_GL_SwapWindow(window);
}

static void presenter_sdl_opengl_shutdown(window_handle_t window, void* present_handle) {
	(void)present_handle;
	presenter_imgui_opengl_destroy_cached_draw_data();
	ImGui_ImplOpenGL3_Shutdown();
	SDL_GL_DeleteContext(sdl_opengl_context);
	sdl_opengl_context = NULL;
	if (window) {
		SDL_DestroyWindow(window);
	}
}

presenter_backend_t presenter_opengl_get_backend() {
	static const presenter_backend_t backend = {
			presenter_sdl_opengl_init_window,
			presenter_sdl_opengl_init_imgui,
			presenter_sdl_opengl_imgui_new_frame,
			presenter_sdl_opengl_render_imgui_draw_data,
			presenter_sdl_opengl_render_cached_imgui_draw_data,
			presenter_sdl_opengl_set_swap_interval,
			presenter_sdl_opengl_get_refresh_rate,
			presenter_sdl_opengl_get_drawable_size,
			presenter_sdl_opengl_present,
			presenter_sdl_opengl_shutdown,
	};
	return backend;
}
