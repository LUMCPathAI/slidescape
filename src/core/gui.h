/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2025  Pieter Valkema

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
#include "viewer.h"
#include "platform.h"

#ifdef __cplusplus
#include "imgui.h"
#include "imgui_internal.h"
extern "C" {
#else
// Allow for function prototypes that can use ImGui structures even from C
typedef struct ImDrawList ImDrawList;
#endif

#define MAX_EXTRA_DRAWLISTS 64

enum gui_modal_type_enum {
	GUI_MODAL_NONE = 0,
	GUI_MODAL_MESSAGE = 1,
	GUI_MODAL_PROGRESS_BAR = 2,
};

typedef struct gui_modal_popup_t gui_modal_popup_t;
struct gui_modal_popup_t {
	gui_modal_type_enum type;
	char message[4096];
	const char* title;
	bool need_open;
	bool allow_cancel;
	float* progress; // for progress bars
	float visual_progress; // value that 'lags behind' for smooth visual updates
};

#if WINDOWS
void win32_init_gui(app_state_t* app_state);
#endif

void imgui_create_context();
void gui_drawlist_reset_for_new_frame(ImDrawList* drawlist);
void gui_reset_all_extra_drawlists();
void gui_destroy_all_extra_drawlists();
void gui_mark_extra_drawlists_modified();
ImDrawList* gui_get_extra_drawlist(i32 drawlist_index);
ImDrawData* gui_get_merged_extra_drawlists_data(ImGuiViewport* viewport, ImVec2 framebuffer_scale);
void gui_make_next_window_appear_in_center_of_screen();
void gui_draw_open_file_dialog(app_state_t* app_state);
void menu_close_file(app_state_t* app_state);
void gui_draw_polygon_outline(v2f* points, i32 count, rgba_t rgba, bool closed, float thickness, ImDrawList* drawlist);
void gui_draw_polygon_outline_in_scene(v2f* points, i32 count, rgba_t color, bool closed, float thickness, scene_t* scene, ImDrawList* drawlist);
void gui_draw_bounds_in_scene(bounds2f bounds, rgba_t color, float thickness, scene_t* scene, ImDrawList* drawlist);
bool gui_draw_selected_annotation_submenu_section(app_state_t* app_state, scene_t* scene, annotation_set_t* annotation_set);
void gui_draw_insert_annotation_submenu(app_state_t* app_state);
simple_image_t* gui_find_associated_image(app_state_t* app_state, bool find_label);
bool gui_navigate_slide_tray(app_state_t* app_state, i32 direction);
void gui_draw(app_state_t* app_state, input_t* input, i32 client_width, i32 client_height);
void gui_do_modal_popups();
void draw_profiler_window(app_state_t* app_state);
void gui_add_modal_message_popup(const char* title, const char* message, ...);
void gui_prompt_for_ssi_credentials(const char* uri, const char* status);
void gui_add_modal_progress_bar_popup(const char* title, float* progress, bool allow_cancel);
void draw_export_region_dialog(app_state_t* app_state);

// console.cpp
void draw_console_window(app_state_t* app_state, const char* window_title, bool* p_open);



// globals
#if defined(GUI_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif


extern bool show_demo_window;
extern bool show_debugging_window;
extern bool show_image_options_window;
extern bool show_focus_window INIT(= true);
extern bool show_open_remote_window;
extern bool show_open_uri_window;
extern bool show_dicomweb_archive_window;
extern bool show_layers_window;
extern bool show_label_image_window;
extern bool show_macro_image_window;
extern bool show_slide_tray_window;
extern bool show_annotations_window;
extern bool show_annotation_group_assignment_window;
extern bool show_annotation_group_filter_window;
extern bool show_general_options_window;
extern bool show_about_window;
extern bool show_export_region_dialog;
extern bool show_mouse_pos_overlay;
extern bool show_console_window INIT(= DO_DEBUG);
extern bool show_profiler_window;
extern bool show_menu_bar INIT(= true);
extern bool load_next_image_as_overlay;
extern i32 layers_window_selected_image_index; // TODO: make this a property of scene_t?
extern gui_modal_popup_t* gui_modal_stack;
extern float global_progress_bar_test_progress;
extern char remote_hostname[64] INIT(= "localhost");
extern char remote_port[64] INIT(= "2000");
extern char remote_filename[128] INIT(= "sample.tiff");
extern char remote_uri[1024];
#ifdef __cplusplus
extern ImFont* global_main_font;
extern ImFont* global_fixed_width_font;
extern ImFont* global_icon_font;
#endif
extern bool enable_multithreaded_annotation_drawing INIT(= true);
extern bool enable_annotation_drawlist_cache INIT(= true);
extern bool enable_merged_extra_drawlists INIT(= true);
extern bool enable_persistent_extra_drawlist_buffers INIT(= true);
extern ImDrawList* global_extra_drawlists[MAX_EXTRA_DRAWLISTS];
extern ImDrawListSharedData global_extra_drawlist_shared_datas[MAX_EXTRA_DRAWLISTS];
extern i32 global_active_extra_drawlists INIT(= 0);
extern u32 global_extra_drawlists_generation INIT(= 1);

extern i32 viewer_min_level INIT(= -2);
extern i32 viewer_max_level INIT(= 10);

// annotation.cpp
extern bool auto_assign_last_group;
extern bool annotation_show_polygon_nodes_outside_edit_mode INIT(= false);
extern float annotation_normal_line_thickness INIT(= 2.0f);
extern float annotation_selected_line_thickness INIT(= 8.0f);
extern float annotation_node_size INIT(= 6.0f);
extern float annotation_opacity INIT(= 1.0f);
extern float annotation_hover_distance INIT(= 20.0f);
extern float annotation_insert_hover_distance INIT(= 30.0f);
extern float annotation_freeform_insert_interval_distance INIT(= 35.0f);
extern bool annotation_highlight_inside_of_polygons INIT(=true);
extern float annotation_highlight_opacity INIT(=0.1f);
extern bool annotation_enable_mip_coordinates INIT(=true);
extern i32 annotation_mip_current_level INIT(=-1);
extern i32 annotation_mip_min_level INIT(=-1);
extern i32 annotation_mip_max_level INIT(=3);
extern bool show_delete_annotation_prompt;
extern bool show_save_quit_prompt;
extern bool dont_ask_to_delete_annotations;
extern bool show_annotation_palette_window INIT(=true);
extern bool show_annotation_inspector_window INIT(=false);

extern i32 desired_region_export_format;
extern u16 tiff_export_desired_color_space INIT(= TIFF_PHOTOMETRIC_YCBCR);//TIFF_PHOTOMETRIC_RGB;
extern bool tiff_export_match_input_resolution INIT(= true);
extern float tiff_export_mpp INIT(= 0.25f);
extern i32 tiff_export_tile_width INIT(= 512);
extern i32 tiff_export_jpeg_quality INIT(= 90);

#undef INIT
#undef extern

#ifdef __cplusplus
}
#endif
