/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

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

#include <windows.h>
#include "platform.h"
#include "presenter.h"

#include "imgui.h"
#include "misc/freetype/imgui_freetype.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"

#include "win32_gui.h"
#include "gui.h"
#include "font_definitions.h"

static float win32_current_gui_dpi_scale;

void win32_gui_new_frame(app_state_t* app_state) {
	// Init for the frame
	presenter_imgui_new_frame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
}

static void win32_load_imgui_fonts() {
	ImGuiIO& io = ImGui::GetIO();

	ImFontConfig font_config = ImFontConfig();
//	font_config.OversampleH = 3;
//	font_config.OversampleV = 2;
//	font_config.RasterizerMultiply = 1.2f;
	static const ImWchar ranges[] =
	{
			0x0020, 0x00FF, // Basic Latin + Latin Supplement
			0x0370, 0x03FF, // Greek
			0,
	};

	float system_font_size = 18.0f;

	const char* main_ui_font_filename = "c:\\Windows\\Fonts\\segoeui.ttf";
	if (file_exists(main_ui_font_filename)) {
		global_main_font = io.Fonts->AddFontFromFileTTF(main_ui_font_filename, system_font_size, &font_config, ranges);
	}
	if (!global_main_font) {
		console_print_error("Main UI font '%s' could not be loaded", main_ui_font_filename);
	}

	const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
	const char* icon_font_filename = "resources/FontAwesome4/font.ttf";
	if (file_exists(icon_font_filename)) {
		global_icon_font = io.Fonts->AddFontFromFileTTF(icon_font_filename, 40.0f, &font_config, icon_ranges);
	}
	if (!global_icon_font) {
//		console_print_error("Icon font could not be loaded");
	}

	const char* fixed_width_font_filename = "c:\\Windows\\Fonts\\consola.ttf";
	if (file_exists(fixed_width_font_filename)) {
		global_fixed_width_font = io.Fonts->AddFontFromFileTTF(fixed_width_font_filename, 14.0f, &font_config, ranges);
	}
	if (!global_fixed_width_font) {
		console_print_error("Fixed width font '%s' could not be loaded", fixed_width_font_filename);
	}

	ImFont* font_default = io.Fonts->AddFontDefault();
	if (!global_main_font) {
		global_main_font = font_default;
	}
	if (!global_fixed_width_font) {
		global_fixed_width_font = font_default;
	}
//	IM_ASSERT(font != NULL);

	// The base font size is small; per-monitor DPI scaling is applied dynamically through FontScaleDpi.
	io.Fonts->FontLoaderFlags = ImGuiFreeTypeLoaderFlags_MonoHinting;
}

void win32_update_gui_dpi(app_state_t* app_state, bool force) {
	if (!app_state || !app_state->main_window || !ImGui::GetCurrentContext()) return;

	float dpi_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(app_state->main_window);
	if (dpi_scale <= 0.0f) dpi_scale = 1.0f;

	if (!force && fabsf(dpi_scale - win32_current_gui_dpi_scale) < 0.01f) return;

	win32_current_gui_dpi_scale = dpi_scale;
	ImGui::GetStyle().FontScaleDpi = dpi_scale;
}


void win32_init_gui(app_state_t* app_state) {

	imgui_create_context();
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
//	ImGui::StyleColorsLight();
//	ImGui::StyleColorsClassic();

	ImGuiStyle& style = ImGui::GetStyle();
	style.Alpha = 0.95f;
	style.DisplaySafeAreaPadding = ImVec2(0.0f, 0.0f);
	style.TouchExtraPadding = ImVec2(0.0f, 1.0f);

	// Setup Platform/Renderer bindings
	ImGui_ImplWin32_Init(app_state->main_window);
	presenter_init_imgui(app_state);

	win32_load_imgui_fonts();
	win32_update_gui_dpi(app_state, true);


	// TODO: Windows high DPI code

	is_fullscreen = check_fullscreen(app_state->main_window);

}
