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
#include "platform.h"
#ifdef __cplusplus
extern "C" {
#endif
bool viewer_replay_init(app_state_t* app);
void viewer_replay_update(app_state_t* app);
void viewer_replay_apply_camera(app_state_t* app);
void viewer_replay_after_navigation(app_state_t* app);
bool viewer_replay_is_playing(void);
void viewer_replay_record_open(const char* path);
void viewer_replay_record_close(void);
int viewer_replay_shutdown(void);
#ifdef __cplusplus
}
#endif
