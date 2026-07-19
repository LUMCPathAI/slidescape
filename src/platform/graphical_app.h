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
#include "platform.h"

#include "keycode.h"
#include "keytable.h"

#if APPLE
#include <stddef.h> // for offsetof()
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#elif LINUX
#include <SDL2/SDL.h>
#else
// Key modifiers from SDL2
typedef enum
{
	KMOD_NONE = 0x0000,
	KMOD_LSHIFT = 0x0001,
	KMOD_RSHIFT = 0x0002,
	KMOD_LCTRL = 0x0040,
	KMOD_RCTRL = 0x0080,
	KMOD_LALT = 0x0100,
	KMOD_RALT = 0x0200,
	KMOD_LGUI = 0x0400,
	KMOD_RGUI = 0x0800,
	KMOD_NUM = 0x1000,
	KMOD_CAPS = 0x2000,
	KMOD_MODE = 0x4000,
	KMOD_RESERVED = 0x8000
} SDL_Keymod;
#define KMOD_CTRL   (KMOD_LCTRL|KMOD_RCTRL)
#define KMOD_SHIFT  (KMOD_LSHIFT|KMOD_RSHIFT)
#define KMOD_ALT    (KMOD_LALT|KMOD_RALT)
#define KMOD_GUI    (KMOD_LGUI|KMOD_RGUI)
#endif


#if WINDOWS
typedef HWND window_handle_t;
#elif APPLE
typedef SDL_Window* window_handle_t;
#else
typedef SDL_Window* window_handle_t;
#endif


typedef enum open_file_dialog_action_enum {
	OPEN_FILE_DIALOG_LOAD_GENERIC_FILE = 0,
	OPEN_FILE_DIALOG_CHOOSE_DIRECTORY,
} open_file_dialog_action_enum;


typedef struct button_state_t {
	bool8 down;
	u8 transition_count;
} button_state_t;

typedef struct analog_stick_t {
	v2f start;
	v2f end;
	bool has_input;
} analog_stick_t;

typedef struct analog_trigger_t {
	float start;
	float end;
	bool has_input;
} analog_trigger_t;

typedef struct controller_input_t {
	bool32 is_connected;
	bool32 is_analog;
	analog_stick_t left_stick;
	analog_stick_t right_stick;
	analog_trigger_t left_trigger;
	analog_trigger_t right_trigger;
	u32 modifiers;
	union {
		button_state_t buttons[533];
		struct {
			button_state_t move_up;
			button_state_t move_down;
			button_state_t move_left;
			button_state_t move_right;
			button_state_t action_up;
			button_state_t action_down;
			button_state_t action_left;
			button_state_t action_right;
			button_state_t left_shoulder;
			button_state_t right_shoulder;
			button_state_t start;
			button_state_t back;
			button_state_t button_a;
			button_state_t button_b;
			button_state_t button_x;
			button_state_t button_y;

			button_state_t keys[512];
			button_state_t key_shift;
			button_state_t key_ctrl;
			button_state_t key_alt;
			button_state_t key_super;

			// NOTE: add buttons above this line
			// cl complains about zero-sized arrays, so this the terminator is a full blown button now :(
			button_state_t terminator;
		};
	};

} controller_input_t;
// Does the count of the controller_input_t.buttons[] array add up?
STATIC_ASSERT(sizeof(((controller_input_t*)0)->buttons) == (offsetof(controller_input_t, terminator) - offsetof(controller_input_t, buttons) + sizeof(button_state_t)));

typedef struct input_t {
	button_state_t mouse_buttons[5];
	float mouse_z_start;
	float mouse_z;
	v2f drag_start_xy;
	v2f drag_vector;
	v2f mouse_xy;
	bool mouse_moved;
	float delta_t;
	union {
		controller_input_t abstract_controllers[5];
		struct {
			controller_input_t keyboard;
			controller_input_t controllers[4];
		};
	};
	u8 preferred_controller_index;
	bool are_any_buttons_down;

} input_t;

// Platform specific function prototypes
void set_swap_interval(int interval);
void mouse_show();
void mouse_hide();
void update_cursor();
void set_cursor_default();
void set_cursor_crosshair();
const char* get_default_save_directory();
void open_file_dialog(app_state_t* app_state, u32 action, u32 filetype_hint);
bool save_file_dialog(app_state_t* app_state, char* path_buffer, i32 path_buffer_size, const char* filter_string, const char* filename_hint);
void toggle_fullscreen(window_handle_t window);
bool check_fullscreen(window_handle_t window);
void set_window_title(window_handle_t window, const char* title);
void reset_window_title(window_handle_t window);
void message_box(window_handle_t window, const char* message);

#if LINUX || APPLE
void determine_if_relative_mouse_mode_can_be_safely_enabled(SDL_version version);
#endif

// globals
#if defined(GRAPHICAL_APP_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif

extern bool is_fullscreen;
extern bool is_program_running;
extern bool need_quit;
extern bool is_vsync_enabled;
extern bool is_nvidia_gpu;
extern input_t inputs[2];
extern input_t *old_input;
extern input_t *curr_input;
extern platform_mutex_t console_printer_mutex INIT(=PLATFORM_MUTEX_INITIALIZER);
extern bool cursor_hidden;
extern const char* global_settings_dir;
extern char global_export_save_as_filename[512];
extern bool save_file_dialog_open;
extern bool gui_want_capture_mouse;
extern bool gui_user_can_resize_at_window_edge;
extern bool gui_want_capture_keyboard;

#undef INIT
#undef extern

#ifdef __cplusplus
}
#endif


