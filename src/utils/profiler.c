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
#if WINDOWS
#include <psapi.h>
#elif APPLE
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

static FILE* capture_file;
static bool capture_failed;
static i64 capture_start;
static i64 memory_sample_clock;
static u64 capture_frames;
static double capture_total_ms;
static double capture_max_ms;
static profiler_metrics_t capture_metrics;

bool profiler_is_capturing(void) {
	return capture_file != NULL;
}

bool profiler_start_capture(const char* path) {
	if (capture_file) {
		return false;
	}
	capture_file = fopen(path, "wb");
	if (!capture_file) {
		return false;
	}
	capture_failed = false;
	capture_start = get_clock();
	memory_sample_clock = 0;
	capture_frames = 0;
	capture_total_ms = 0;
	capture_max_ms = 0;
	memset(&capture_metrics, 0, sizeof(capture_metrics));
	fprintf(capture_file, "seconds,frame,frame_ms,frame_interval_ms,resident_bytes,texture_bytes,cpu_tile_bytes,cache_table_bytes,cache_entries,tile_metadata_bytes,mrxs_index_bytes,pooled_bytes,image_count,replay_segment,replay_command,camera_x,camera_y,zoom,viewport_width,viewport_height,display_scale");
	for (i32 i = 0; i < PROFILER_SECTION_COUNT; ++i) {
		fprintf(capture_file, ",%s_ms", profiler_section_names[i]);
	}
	fprintf(capture_file, "\n");
	if (ferror(capture_file)) {
		capture_failed = true;
		profiler_stop_capture();
		return false;
	}
	return true;
}

bool profiler_stop_capture(void) {
	if (capture_file) {
		if (ferror(capture_file) || fflush(capture_file) != 0) {
			capture_failed = true;
		}
		if (fclose(capture_file) != 0) {
			capture_failed = true;
		}
		capture_file = NULL;
		console_print("Profile: %llu frames, mean %.3f ms, maximum %.3f ms%s.\n",
		              (unsigned long long)capture_frames,
		              capture_frames ? capture_total_ms / capture_frames : 0.0,
		              capture_max_ms, capture_failed ? " (write failed)" : "");
	}
	return !capture_failed;
}

bool profiler_memory_sample_due(void) {
	if (!capture_file) {
		return false;
	}
	i64 now = get_clock();
	if (!memory_sample_clock || get_seconds_elapsed(memory_sample_clock, now) >= 1.0f) {
		memory_sample_clock = now;
		return true;
	}
	return false;
}

u64 profiler_resident_bytes(void) {
#if WINDOWS
	PROCESS_MEMORY_COUNTERS counters = {0};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
		return counters.WorkingSetSize;
	}
#elif APPLE
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
		return info.resident_size;
	}
#else
	FILE* file = fopen("/proc/self/statm", "r");
	if (file) {
		unsigned long long total_pages = 0;
		unsigned long long resident_pages = 0;
		int fields = fscanf(file, "%llu %llu", &total_pages, &resident_pages);
		fclose(file);
		long page_size = sysconf(_SC_PAGESIZE);
		if (fields == 2 && page_size > 0) {
			return resident_pages * (u64)page_size;
		}
	}
#endif
	return 0;
}

void profiler_set_metrics(const profiler_metrics_t* metrics) {
	capture_metrics = *metrics;
}

static void profiler_capture_frame(const profiler_frame_t* frame) {
	if (!capture_file || !frame->frame_start) {
		return;
	}
	static i64 previous_start;
	double milliseconds = get_seconds_elapsed(frame->frame_start, frame->frame_end) * 1000.0;
	double interval = capture_frames ? get_seconds_elapsed(previous_start, frame->frame_start) * 1000.0 : 0.0;
	previous_start = frame->frame_start;
	profiler_metrics_t* m = &capture_metrics;
	fprintf(capture_file, "%.6f,%llu,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%.9g,%.9g,%.9g,%d,%d,%.9g",
	        get_seconds_elapsed(capture_start, frame->frame_end), (unsigned long long)capture_frames,
	        milliseconds, interval, (unsigned long long)m->resident_bytes,
	        (unsigned long long)m->texture_bytes, (unsigned long long)m->cpu_tile_bytes,
	        (unsigned long long)m->cache_table_bytes, (unsigned long long)m->cache_entries,
	        (unsigned long long)m->tile_metadata_bytes, (unsigned long long)m->mrxs_index_bytes,
	        (unsigned long long)m->pooled_bytes, m->image_count, m->replay_segment, m->replay_command,
	        m->camera_x, m->camera_y, m->zoom, m->viewport_width, m->viewport_height, m->display_scale);
	for (i32 i = 0; i < PROFILER_SECTION_COUNT; ++i) {
		const profiler_section_t* section = frame->sections + i;
		fprintf(capture_file, ",%.6f", get_seconds_elapsed(section->start, section->end) * 1000.0);
	}
	fprintf(capture_file, "\n");
	++capture_frames;
	capture_total_ms += milliseconds;
	capture_max_ms = MAX(capture_max_ms, milliseconds);
	if (ferror(capture_file)) {
		capture_failed = true;
		profiler_stop_capture();
	}
}

profiler_state_t global_profiler;

const char* profiler_section_names[PROFILER_SECTION_COUNT] = {
#define PROFILER_SECTION_NAME_ENTRY(id, name) name,
	PROFILER_SECTION_LIST(PROFILER_SECTION_NAME_ENTRY)
#undef PROFILER_SECTION_NAME_ENTRY
};
STATIC_ASSERT(COUNT(profiler_section_names) == PROFILER_SECTION_COUNT);

void profiler_new_frame(void) {
	if (global_profiler.paused && !capture_file) {
		return;
	}

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
	if (global_profiler.paused && !capture_file) {
		return;
	}
	if ((u32)section >= (u32)PROFILER_SECTION_COUNT) {
		return;
	}

	profiler_frame_t* frame = &global_profiler.frames[global_profiler.current_frame];
	profiler_section_t* s = &frame->sections[section];
	s->level = global_profiler.current_level++;
	s->start = get_clock();
	s->end = s->start;
}

void profiler_end(i32 section) {
	if (global_profiler.paused && !capture_file) {
		return;
	}
	if ((u32)section >= (u32)PROFILER_SECTION_COUNT) {
		return;
	}

	profiler_frame_t* frame = &global_profiler.frames[global_profiler.current_frame];
	profiler_section_t* s = &frame->sections[section];
	s->end = get_clock();
	if (global_profiler.current_level > 0) {
		--global_profiler.current_level;
	}

	if (section == PROFILER_SECTION_FRAME) {
		frame->frame_end = s->end;
		profiler_capture_frame(frame);
	}
}
