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

#include "replay.h"

static bool replay_read_u32(mem_t* file, u32* value) {
	u8 bytes[4];
	if (mem_read(bytes, file, sizeof(bytes)) != sizeof(bytes)) {
		return false;
	}
	*value = (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
	return true;
}

static bool replay_read_float(mem_t* file, float* value) {
	u32 bits;
	if (!replay_read_u32(file, &bits)) {
		return false;
	}
	memcpy(value, &bits, sizeof(bits));
	return isfinite(*value);
}

bool replay_init(replay_t* replay, mem_t* file) {
	memset(replay, 0, sizeof(*replay));
	replay->file = file;
	char magic[8];
	u32 version;
	if (!file || file->len > REPLAY_MAX_FILE_SIZE) {
		replay->error = "missing or oversized replay file";
		return false;
	}
	file->cursor = 0;
	if (mem_read(magic, file, sizeof(magic)) != sizeof(magic) ||
	    memcmp(magic, "SCREPLAY", sizeof(magic)) != 0 ||
	    !replay_read_u32(file, &version) || version != REPLAY_VERSION) {
		replay->error = "invalid replay header or unsupported version";
		return false;
	}
	return true;
}

void replay_destroy(replay_t* replay) {
	free(replay->file);
	memset(replay, 0, sizeof(*replay));
}

static bool replay_parse_next(replay_t* replay) {
	mem_t* file = replay->file;
	if ((u64)file->cursor == file->len) {
		replay->finished = true;
		return false;
	}
	replay_command_t command = {0};
	u32 argument_count;
	u32 kind;
	if (!replay_read_float(file, &command.timestamp) ||
	    command.timestamp < replay->previous_timestamp ||
	    !replay_read_u32(file, &kind) ||
	    !replay_read_u32(file, &argument_count)) {
		replay->error = "invalid timestamp or truncated command";
		return false;
	}
	command.kind = (replay_command_kind_t)kind;
	if (command.kind < REPLAY_OPEN || command.kind > REPLAY_EXIT) {
		replay->error = "unknown replay command";
		return false;
	}
	u32 expected_arguments = command.kind == REPLAY_OPEN ? 1 : command.kind == REPLAY_CAMERA ? 2 : 0;
	command.has_zoom = command.kind == REPLAY_CAMERA && argument_count == 3;
	if (argument_count != expected_arguments && !command.has_zoom) {
		replay->error = "incorrect argument count";
		return false;
	}
	for (u32 i = 0; i < argument_count; ++i) {
		u32 size;
		if (!replay_read_u32(file, &size) || size > file->len - (u64)file->cursor) {
			replay->error = "truncated argument";
			return false;
		}
		if (command.kind == REPLAY_OPEN) {
			if (size < 2 || size > REPLAY_MAX_STRING_SIZE) {
				replay->error = "invalid filename size";
				return false;
			}
			command.path = (char*)file->data + file->cursor;
			command.path[size - 1] = '\0'; // Re-terminate even if the writer omitted the terminator.
			file->cursor += size;
			if (!command.path[0] || memchr(command.path, '\0', size - 1)) {
				replay->error = "empty filename or embedded null";
				return false;
			}
		} else if (size != sizeof(float) || !replay_read_float(file, command.camera + i)) {
			replay->error = "invalid camera argument";
			return false;
		}
	}
	if (command.has_zoom && (command.camera[2] < -16.0f || command.camera[2] > 30.0f)) {
		replay->error = "camera zoom outside supported range [-16, 30]";
		return false;
	}
	if (command.kind == REPLAY_CAMERA && (fabsf(command.camera[0]) > 1.0e9f || fabsf(command.camera[1]) > 1.0e9f)) {
		replay->error = "camera coordinates outside supported range";
		return false;
	}
	replay->command = command;
	replay->has_command = true;
	replay->previous_timestamp = command.timestamp;
	++replay->command_index;
	return true;
}

void replay_tick(replay_t* replay, double now, replay_execute_t* execute, void* userdata) {
	if (replay->error || replay->finished) {
		return;
	}
	if (!replay->started) {
		replay->origin = now;
		replay->started = true;
	}
	// Bound work per frame, including for malformed or excessively dense recordings.
	for (i32 i = 0; i < 64; ++i) {
		if (!replay->has_command && !replay_parse_next(replay)) {
			return;
		}
		replay_command_t* command = &replay->command;
		if (now - replay->origin < command->timestamp) {
			return;
		}
		if (command->kind == REPLAY_WAIT && !replay->waiting) {
			replay->waiting = true;
			replay->wait_started = now;
		}
		replay_result_t result = execute(userdata, command);
		if (result == REPLAY_ERROR) {
			replay->error = "command failed";
			return;
		}
		if (result == REPLAY_PENDING) {
			if (command->kind != REPLAY_WAIT || now - replay->wait_started > 120.0) {
				replay->error = "synchronization timed out";
			}
			return;
		}
		replay->has_command = false;
		replay->completed_commands = replay->command_index;
		if (command->kind == REPLAY_WAIT) {
			replay->origin = now;
			replay->previous_timestamp = 0;
			replay->waiting = false;
			++replay->segment;
		}
		if (command->kind == REPLAY_EXIT) {
			replay->finished = true;
		}
		// Catch up camera commands after a slow frame; file changes get a frame boundary.
		if (command->kind != REPLAY_CAMERA) {
			return;
		}
	}
}

static bool replay_write_u32(FILE* file, u32 value) {
	u8 bytes[4] = {(u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24)};
	return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

static bool replay_write_float(FILE* file, float value) {
	u32 bits;
	memcpy(&bits, &value, sizeof(bits));
	return replay_write_u32(file, bits);
}

bool replay_write_header(FILE* file) {
	return fwrite("SCREPLAY", 1, 8, file) == 8 && replay_write_u32(file, REPLAY_VERSION);
}

bool replay_write_command(FILE* file, const replay_command_t* command) {
	u32 count = command->kind == REPLAY_OPEN ? 1 : command->kind == REPLAY_CAMERA ? (command->has_zoom ? 3 : 2) : 0;
	if (!replay_write_float(file, command->timestamp) || !replay_write_u32(file, command->kind) ||
	    !replay_write_u32(file, count)) {
		return false;
	}
	for (u32 i = 0; i < count; ++i) {
		if (command->kind == REPLAY_OPEN) {
			size_t size = strlen(command->path) + 1;
			if (size > REPLAY_MAX_STRING_SIZE || !replay_write_u32(file, (u32)size) ||
			    fwrite(command->path, 1, size, file) != size) {
				return false;
			}
		} else if (!replay_write_u32(file, sizeof(float)) || !replay_write_float(file, command->camera[i])) {
			return false;
		}
	}
	return true;
}
