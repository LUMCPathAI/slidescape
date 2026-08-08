/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2023  Pieter Valkema

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
extern "C" {
#endif

void win32_gui_new_frame(app_state_t* app_state);
void win32_init_gui(app_state_t* app_state);
void win32_update_gui_dpi(app_state_t* app_state, bool force);

typedef enum win32_mview_handler_state_t {
	WIN32_MVIEW_HANDLER_DISABLED = 0,
	WIN32_MVIEW_HANDLER_ENABLED,
	WIN32_MVIEW_HANDLER_CONFLICT,
} win32_mview_handler_state_t;

win32_mview_handler_state_t win32_query_mview_handler(char* status, size_t status_size);
bool win32_set_mview_handler(bool enable, char* status, size_t status_size);

#ifdef __cplusplus
}
#endif
