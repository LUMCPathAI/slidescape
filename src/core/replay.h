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

#define REPLAY_VERSION 1
#define REPLAY_MAX_FILE_SIZE MEGABYTES(64)
#define REPLAY_MAX_STRING_SIZE KILOBYTES(64)

typedef enum replay_command_kind_t {
	REPLAY_OPEN = 1,
	REPLAY_WAIT,
	REPLAY_CAMERA,
	REPLAY_CLOSE,
	REPLAY_EXIT,
} replay_command_kind_t;

typedef struct replay_command_t {
	float timestamp;
	replay_command_kind_t kind;
	char* path; // Points into the owned file buffer; valid until replay_destroy().
	float camera[3];
	bool has_zoom;
} replay_command_t;

typedef enum replay_result_t {
	REPLAY_ERROR = -1,
	REPLAY_PENDING = 0,
	REPLAY_DONE = 1,
} replay_result_t;

typedef struct replay_t {
	mem_t* file;
	replay_command_t command;
	double origin;
	double wait_started;
	float previous_timestamp;
	u32 command_index;
	u32 completed_commands;
	u32 segment;
	bool started;
	bool has_command;
	bool waiting;
	bool finished;
	const char* error;
} replay_t;

typedef replay_result_t replay_execute_t(void* userdata, const replay_command_t* command);

// Takes ownership of file even on failure. Commands are parsed incrementally.
bool replay_init(replay_t* replay, mem_t* file);
void replay_destroy(replay_t* replay);
void replay_tick(replay_t* replay, double now, replay_execute_t* execute, void* userdata);
// Writer uses the same little-endian wire format as the parser.
bool replay_write_header(FILE* file);
bool replay_write_command(FILE* file, const replay_command_t* command);

#ifdef __cplusplus
}
#endif
