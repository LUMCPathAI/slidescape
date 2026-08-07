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
#ifndef VIEWER_H
#define VIEWER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "platform.h"
#include "graphical_app.h"
#include "mathutils.h"
#include "arena.h"
#include "image.h"
#include "tiff.h"
#include "isyntax.h"
#include "isyntax_streamer.h"
#include "dicom.h"
#include "openslide_api.h"
#include "annotation.h"
#include "renderer.h"
#include "tile_loader.h"
#include "image_loader.h"

typedef struct scale_bar_t {
	char text[64];
	float max_width;
	float width;
	float height;
	v2f pos;
	v2f pos_max; // lower right corner
	v2f pos_center;
	v2f drag_start_offset;
	corner_enum corner;
	v2f pos_relative_to_corner;
	float text_x;
	bool enabled;
	bool initialized;
} scale_bar_t;


typedef enum entity_type_enum {
	ENTITY_SIMPLE_IMAGE = 1,
	ENTITY_TILED_IMAGE = 2,
} entity_type_enum;

typedef enum mouse_mode_enum {
	MODE_VIEW,
	MODE_INSERT,
	MODE_CREATE_SELECTION_BOX,
	MODE_DRAG_ANNOTATION_NODE,
	MODE_DRAG_SCALE_BAR,
} mouse_mode_enum;

typedef enum placement_tool_enum {
	TOOL_NONE,
	TOOL_CREATE_OUTLINE,
	TOOL_EDIT_EXISTING_COORDINATES,
	TOOL_CREATE_POINT,
	TOOL_CREATE_LINE,
	TOOL_CREATE_ARROW,
	TOOL_CREATE_FREEFORM,
	TOOL_CREATE_ELLIPSE,
	TOOL_CREATE_RECTANGLE,
	TOOL_CREATE_TEXT,
} placement_tool_enum;

typedef struct entity_t {
	u32 type;
	v2f pos;
	union {
		struct {
			image_t* image;
		} simple_image;
		struct {
			image_t* image;
		} tiled_image;
	};
} entity_t;

#define MAX_ENTITIES 1000


typedef struct zoom_state_t {
	float pos;
	i32 level;
	i32 notches;
	float notch_size;
	float pixel_width;
	float pixel_height;
	float screen_point_width;
	float downsample_factor;
	float base_pixel_width;
	float base_pixel_height;
} zoom_state_t;

typedef struct scene_t {
	rect2f viewport;
	v2f camera;
	v2f mouse;
	v2f right_clicked_pos;
    v2f left_clicked_pos;
	bounds2f camera_bounds;
	bounds2f tile_load_bounds;
	float rotation;
	float cos_rotation; // should be updated every time the rotation is updated
	float sin_rotation; // should be updated every time the rotation is updated
	bool restrict_load_bounds;
	float r_minus_l;
	float t_minus_b;
	zoom_state_t zoom;
	bool need_zoom_reset;
	bool need_zoom_animation;
	v2f control;
	float time_since_control_start;
	v2f panning_velocity;
	v2f zoom_pivot;
	zoom_state_t zoom_target_state;
	v2f level_pixel_size;
	v4f clear_color;
	u32 entity_count;
	entity_t entities[MAX_ENTITIES];
	i32 active_layer;
	float target_layer_time;
	float layer_time;
	annotation_set_t annotation_set;
    annotation_set_template_t annotation_set_template;
	bool clicked;
	bool right_clicked;
	bool drag_started;
	bool drag_ended;
	bool is_dragging; // if mouse down: is this scene being dragged?
	bool is_drag_vector_within_click_tolerance;
	bool suppress_next_click; // if mouse down: prevent release being counted as click
	bool viewport_changed;
	rect2f selection_box;
	bool has_selection_box;
	v2f raw_drag_vector;
	v2f drag_vector;
	v2f cumulative_drag_vector;
	bounds2f crop_bounds;
	bounds2i selection_pixel_bounds;
	bool can_export_region;
	bool is_cropped;
	const char* selection_description;
	v3f transparent_color;
	float transparent_tolerance;
	bool use_transparent_filter;
    bool draw_outlines;
    bool draw_envelopes;
	bool draw_wsi_image;
	bool draw_wsi_background;
	bool draw_macro_image;
	bool draw_label_image;
	scale_bar_t scale_bar;
	bool is_mpp_known;
	bool enable_grid;
	bool enable_annotations;
	bool initialized;
} scene_t;

typedef struct pixel_transfer_state_t {
	u32 pbo;
	renderer_texture_handle_t texture;
	i32 texture_width;
	i32 texture_height;
	bool need_finalization;
	void* userdata;
	image_t* image;
	i32 level;
	i32 tile_index;
	bool8 initialized;
} pixel_transfer_state_t;

typedef struct tile_streamer_t {
	image_t* image;
	scene_t* scene;
	v2f origin_offset;
	v2f camera_center;
	bounds2f camera_bounds;
	bounds2f crop_bounds;
	bool is_cropped;
	zoom_state_t zoom;
} tile_streamer_t;


typedef enum command_enum {
	COMMAND_NONE,
	COMMAND_PRINT_VERSION,
	COMMAND_EXPORT,
} command_enum;

typedef enum command_export_error_enum {
	COMMAND_EXPORT_ERROR_NONE,
	COMMAND_EXPORT_ERROR_NO_ROI,
} command_export_error_enum;

typedef struct app_command_t app_command_t;
struct app_command_t {
	bool headless;
	bool exit_immediately;
	command_enum command;
	struct app_command_export_t {
		const char* roi;
		bool use_first_roi;
		bool with_annotations;
		command_export_error_enum error;
	} export_command;
	const char* dicomweb_username;
	const char* dicomweb_password;
	const char** inputs; // array
	const char** overlay_inputs; // array
};

typedef struct app_state_t {
	app_command_t command;
	u8* temp_storage_memory; // TODO: remove, use thread local temp storage instead
	arena_t temp_arena; // TODO: remove
	rect2i client_viewport;
	float display_scale_factor;
	float display_points_per_pixel;
	scene_t scene;
	v4f clear_color;
	float black_level;
	float white_level;
	image_t** loaded_images; // array
	i32 last_layer_enabled;
	i32 enabled_image_count;
	i32 enabled_image_indices[2];
	i32 base_image_index;
	bool is_any_image_loaded;
	bool use_builtin_tiff_backend;
	bool use_image_adjustments;
	bool initialized;
	bool allow_idling_next_frame;
	mouse_mode_enum mouse_mode;
	placement_tool_enum mouse_tool;
	i64 last_frame_start;
	i64 frame_counter;
	float seconds_without_mouse_movement;
	i32 mouse_sensitivity;
	i32 keyboard_base_panning_speed;
	pixel_transfer_state_t pixel_transfer_states[32];
	u32 next_pixel_transfer_to_submit;
	window_handle_t main_window;
	bool is_window_title_set_for_image;
	input_t* input;
	i32* active_resources; // array
	bool is_export_in_progress;
	bool enable_autosave;
    bool remember_annotation_groups_as_template;
	bool headless;
    char last_active_directory[512];
    char annotation_directory[512];
    bool is_annotation_directory_set;
    bool need_close;
} app_state_t;

#define VIEWER_CLICK_DRAG_TOLERANCE 8.0f


//  prototypes
void add_image(app_state_t* app_state, image_t* image, bool need_zoom_reset, bool need_image_registration);
void unload_all_images(app_state_t* app_state);
image_t* get_image_from_resource_id(app_state_t* app_state, i32 resource_id);
bool load_generic_file(app_state_t* app_state, const char* filename, u32 filetype_hint);
bool load_dicom_file_from_directory(app_state_t* app_state, const char* filename,
		const char* directory, u32 filetype_hint);
bool app_load_input(app_state_t* app_state, const char* input, u32 filetype_hint);
bool app_get_slide_score_api_key_filename(char* buffer, size_t buffer_size);
void app_read_slide_score_api_key(char* buffer, size_t buffer_size);
bool app_write_slide_score_api_key(const char* api_key);
image_t* load_image_from_file(app_state_t* app_state, file_info_t* file, directory_info_t* directory, u32 filetype_hint);
bool was_button_pressed(button_state_t* button);
bool was_button_released(button_state_t* button);
bool was_key_pressed(input_t* input, i32 keycode);
bool is_key_down(input_t* input, i32 keycode);
void init_app_state(app_state_t* app_state, app_command_t command);
void autosave(app_state_t* app_state, bool force_ignore_delay, bool async);
void scene_update_camera_pos(scene_t* scene, v2f pos);
void viewer_switch_tool(app_state_t* app_state, placement_tool_enum tool);
void viewer_update_and_render(app_state_t* app_state, input_t* input, i32 client_width, i32 client_height, float delta_time);
void do_after_scene_render(app_state_t* app_state, input_t* input);

// viewer_io_file.cpp
const char* get_active_directory(app_state_t* app_state);
const char* get_annotation_directory(app_state_t* app_state);
void set_annotation_directory(app_state_t* app_state, const char* path);
void slide_score_load_tile_batch_func(i32 logical_thread_index, void* userdata);
void ssi_load_tile_batch_func(i32 logical_thread_index, void* userdata);

// viewer_io_remote.cpp
void tiff_load_tile_batch_func(i32 logical_thread_index, void* userdata);

// viewer_options.cpp
void viewer_init_options(app_state_t* app_state);
void viewer_save_options(app_state_t* app_state);
void viewer_save_options_sync(app_state_t* app_state);

// viewer_commandline.cpp
app_command_t app_parse_commandline(int argc, const char** argv);
void app_command_execute_immediately(app_command_t* app_command);
bool app_load_commandline_inputs(app_state_t* app_state);
int app_command_execute(app_state_t* app_state);

// isyntax_streamer.cpp
void isyntax_stream_image_tiles(isyntax_streamer_t* tile_streamer, isyntax_t* isyntax);
void isyntax_begin_stream_image_tiles(isyntax_streamer_t* tile_streamer);

// scene.cpp
v2f world_pos_to_screen_pos(scene_t* scene, v2f world_pos);
void zoom_update_pos(zoom_state_t* zoom, float pos);
void init_zoom_state(zoom_state_t* zoom, float zoom_position, float notch_size, float base_pixel_width, float base_pixel_height);
void init_scene(app_state_t *app_state, scene_t *scene);
v2f scene_mouse_pos(scene_t* scene);
void update_scale_bar(scene_t* scene, scale_bar_t* scale_bar);
void draw_scale_bar(scale_bar_t* scale_bar);
void draw_grid(scene_t* scene);
void draw_selection_box(scene_t* scene);
void scene_determine_if_export_is_possible(scene_t* scene, image_t* image);

// globals
#if defined(VIEWER_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif

extern app_state_t global_app_state;

extern i64 zoom_in_key_hold_down_start_time;
extern i64 zoom_in_key_times_zoomed_while_holding;
extern i64 zoom_out_key_hold_down_start_time;
extern i64 zoom_out_key_times_zoomed_while_holding;
extern bool prefer_integer_zoom INIT(= false);
extern bool use_fast_rendering INIT(= false); // optimize for performance for e.g. remote desktop
extern i32 global_lowest_scale_to_render INIT(= 0);
extern i32 global_highest_scale_to_render INIT(= 16);

extern bool window_start_maximized INIT(= true); // TODO: make this a configurable option in the GUI
extern i32 desired_window_width INIT(= 1280);
extern i32 desired_window_height INIT(= 720);

extern i32 global_next_resource_id INIT(= 1000);

extern float global_tiff_export_progress; // TODO: change to task-local variable?
extern i32 global_tiff_export_progress_console_dots_written; // TODO: change to task-local variable?
extern const char* global_export_region_filename_postfix INIT(= ".exported");

extern bool is_dicom_available;
extern bool is_dicom_loading_done;
extern bool is_openslide_available;
extern bool is_openslide_loading_done;

extern bool global_use_native_mrxs_backend INIT(= false);

#undef INIT
#undef extern


#ifdef __cplusplus
}
#endif

#endif //VIEWER_H
