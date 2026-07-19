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

#include "profiler.h"
#include "timerutils.h"

#include <string.h>

profiler_state_t global_profiler;

const char* profiler_section_names[PROFILER_SECTION_COUNT] = {
#define PROFILER_SECTION_NAME_ENTRY(id, name) name,
	PROFILER_SECTION_LIST(PROFILER_SECTION_NAME_ENTRY)
#undef PROFILER_SECTION_NAME_ENTRY
};
STATIC_ASSERT(COUNT(profiler_section_names) == PROFILER_SECTION_COUNT);

void profiler_new_frame(void) {
	if (global_profiler.paused) return;

	i32 prev = global_profiler.current_frame;
	i32 next = (prev + 1) % PROFILER_FRAME_BUFFER_SIZE;

	profiler_frame_t* frame = &global_profiler.frames[next];
	memset(frame, 0, sizeof(*frame));
	frame->frame_start = get_clock();

	global_profiler.current_frame = next;
	global_profiler.current_level = 0;
	global_profiler.displayed_frame = prev;
}

void profiler_begin(i32 section) {
	if (global_profiler.paused) return;
	if ((u32)section >= (u32)PROFILER_SECTION_COUNT) return;

	profiler_frame_t* frame = &global_profiler.frames[global_profiler.current_frame];
	profiler_section_t* s = &frame->sections[section];
	s->level = global_profiler.current_level++;
	s->start = get_clock();
	s->end = s->start;
}

void profiler_end(i32 section) {
	if (global_profiler.paused) return;
	if ((u32)section >= (u32)PROFILER_SECTION_COUNT) return;

	profiler_frame_t* frame = &global_profiler.frames[global_profiler.current_frame];
	profiler_section_t* s = &frame->sections[section];
	s->end = get_clock();
	if (global_profiler.current_level > 0) --global_profiler.current_level;

	if (section == PROFILER_SECTION_FRAME) {
		frame->frame_end = s->end;
	}
}
