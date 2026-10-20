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
#include "platform.h"
#include "intrinsics.h"

#include <string.h>
#if WINDOWS
#include <psapi.h>
#elif APPLE
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

#ifndef SLIDESCAPE_BUILD_CONFIGURATION
#define SLIDESCAPE_BUILD_CONFIGURATION "unknown"
#endif

static FILE* capture_file;
static bool capture_failed;
static i64 capture_start;
static i64 memory_sample_clock;
static u64 capture_frames;
static double capture_total_ms;
static float capture_max_ms;
static double capture_pre_swap_total_ms;
static float capture_pre_swap_max_ms;
static u64 capture_swaps;
static profiler_metrics_t capture_metrics;
static volatile u64 active_capture_id;
static u64 next_capture_id;
static platform_mutex_t worker_metrics_lock = PLATFORM_MUTEX_INITIALIZER;

static profiler_worker_metrics_t worker_metrics;
static THREAD_LOCAL struct {
	u64 capture_id;
	i64 start;
	i32 io_depth;
	i32 decode_depth;
	double decode_io_start_ms;
	profiler_worker_metrics_t metrics;
} worker;
static THREAD_LOCAL i32 worker_scope_depth;

static profiler_tile_metrics_t frame_tiles;
static bool live_enabled;

u64 profiler_capture_id(void) {
	return atomic_load_u64_acquire(&active_capture_id);
}

void profiler_record_visible_tile(profiler_visible_tile_state_t state) {
	if (profiler_is_collecting() && (u32)state < PROFILER_TILE_STATE_COUNT) {
		++frame_tiles.visible[state];
	}
}

void profiler_record_unready_level(void) {
	if (profiler_is_collecting()) {
		++frame_tiles.unready_levels;
	}
}

void profiler_record_tile_draw(u64 capture_id, i64 request_clock) {
	if (profiler_is_collecting() && capture_id == profiler_capture_id() && request_clock) {
		float milliseconds = get_seconds_elapsed(request_clock, get_clock()) * 1000.0f;
		++frame_tiles.first_draws;
		frame_tiles.request_to_draw_ms += milliseconds;
		frame_tiles.request_to_draw_max_ms = MAX(frame_tiles.request_to_draw_max_ms, milliseconds);
	}
}

void profiler_record_tile_ready(u64 capture_id, i64 request_clock) {
	if (profiler_is_collecting() && capture_id == profiler_capture_id() && request_clock) {
		float milliseconds = get_seconds_elapsed(request_clock, get_clock()) * 1000.0f;
		++frame_tiles.ready_tiles;
		frame_tiles.request_to_ready_ms += milliseconds;
		frame_tiles.request_to_ready_max_ms = MAX(frame_tiles.request_to_ready_max_ms, milliseconds);
	}
}

void profiler_worker_begin(u64 capture_id, i64 queued_clock) {
	if (worker_scope_depth++ > 0) {
		return;
	}
	memset(&worker, 0, sizeof(worker));
	if (capture_id && capture_id == profiler_capture_id()) {
		worker.capture_id = capture_id;
		worker.start = get_clock();
		worker.metrics.jobs = 1;
		if (queued_clock) {
			worker.metrics.queued_jobs = 1;
			worker.metrics.queue_ms = get_seconds_elapsed(queued_clock, worker.start) * 1000.0f;
		}
	}
}

void profiler_worker_end(void) {
	ASSERT(worker_scope_depth > 0);
	if (--worker_scope_depth > 0) {
		return;
	}
	if (!worker.capture_id) {
		return;
	}
	worker.metrics.load_ms = get_seconds_elapsed(worker.start, get_clock()) * 1000.0f;
	platform_mutex_lock(&worker_metrics_lock);
	if (worker.capture_id == profiler_capture_id()) {
		worker_metrics.jobs += worker.metrics.jobs;
		worker_metrics.queued_jobs += worker.metrics.queued_jobs;
		worker_metrics.io_bytes += worker.metrics.io_bytes;
		worker_metrics.load_ms += worker.metrics.load_ms;
		worker_metrics.queue_ms += worker.metrics.queue_ms;
		worker_metrics.io_ms += worker.metrics.io_ms;
		worker_metrics.decode_ms += worker.metrics.decode_ms;
	}
	platform_mutex_unlock(&worker_metrics_lock);
	memset(&worker, 0, sizeof(worker));
}

i64 profiler_worker_decode_begin(void) {
	if (!worker.capture_id || worker.decode_depth++ > 0) {
		return 0;
	}
	worker.decode_io_start_ms = worker.metrics.io_ms;
	return get_clock();
}

void profiler_worker_decode_end(i64 start) {
	if (!worker.capture_id) {
		return;
	}
	--worker.decode_depth;
	if (start) {
		float elapsed_ms = get_seconds_elapsed(start, get_clock()) * 1000.0f;
		worker.metrics.decode_ms += MAX(0.0, elapsed_ms - (worker.metrics.io_ms - worker.decode_io_start_ms));
	}
}

i64 profiler_worker_io_begin(void) {
	if (!worker.capture_id) {
		return 0;
	}
	// Source adapters can recursively read another source. Count only the outer read.
	return worker.io_depth++ == 0 ? get_clock() : 0;
}

void profiler_worker_io_end(i64 start, u64 bytes) {
	if (!worker.capture_id) {
		return;
	}
	--worker.io_depth;
	if (start) {
		worker.metrics.io_ms += get_seconds_elapsed(start, get_clock()) * 1000.0f;
		worker.metrics.io_bytes += bytes;
	}
}


const char* profiler_build_configuration(void) {
	return SLIDESCAPE_BUILD_CONFIGURATION;
}

bool profiler_is_collecting(void) {
	return profiler_capture_id() != 0;
}

void profiler_set_live_enabled(bool enabled) {
	live_enabled = enabled;
	if (enabled && !profiler_is_collecting()) {
		platform_mutex_lock(&worker_metrics_lock);
		memset(&worker_metrics, 0, sizeof(worker_metrics));
		atomic_store_u64_release(&active_capture_id, ++next_capture_id);
		platform_mutex_unlock(&worker_metrics_lock);
		memory_sample_clock = 0;
	} else if (!enabled && !capture_file) {
		atomic_store_u64_release(&active_capture_id, 0);
	}
}

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
	platform_mutex_lock(&worker_metrics_lock);
	memset(&worker_metrics, 0, sizeof(worker_metrics));
	atomic_store_u64_release(&active_capture_id, ++next_capture_id);
	platform_mutex_unlock(&worker_metrics_lock);
	capture_failed = false;
	capture_start = get_clock();
	memory_sample_clock = 0;
	capture_frames = 0;
	capture_total_ms = 0;
	capture_max_ms = 0;
	capture_pre_swap_total_ms = 0;
	capture_pre_swap_max_ms = 0;
	capture_swaps = 0;
	memset(&capture_metrics, 0, sizeof(capture_metrics));
	fprintf(capture_file, "seconds,frame,frame_ms,frame_interval_ms,resident_bytes,texture_bytes,cpu_tile_bytes,cache_table_bytes,cache_entries,tile_metadata_bytes,mrxs_index_bytes,pooled_bytes,image_count,replay_segment,replay_command,camera_x,camera_y,zoom,viewport_width,viewport_height,display_scale");
	fprintf(capture_file, ",pre_swap_ms,visible_ready,visible_adjacent_plane,visible_missing,visible_failed,visible_empty,visible_unready_levels,first_draws,request_to_draw_sum_ms,request_to_draw_max_ms,worker_jobs,worker_queued_jobs,worker_queue_sum_ms,worker_load_sum_ms,worker_io_sum_ms,worker_decode_sum_ms,worker_io_bytes,ready_tiles,request_to_ready_sum_ms,request_to_ready_max_ms,build_configuration");
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
	if (!live_enabled) {
		atomic_store_u64_release(&active_capture_id, 0);
	}
	if (capture_file) {
		if (ferror(capture_file) || fflush(capture_file) != 0) {
			capture_failed = true;
		}
		if (fclose(capture_file) != 0) {
			capture_failed = true;
		}
		capture_file = NULL;
		console_print("Profile (%s): %llu frames, mean %.3f ms, maximum %.3f ms%s.\n",
		              SLIDESCAPE_BUILD_CONFIGURATION,
		              (unsigned long long)capture_frames,
		              capture_frames ? capture_total_ms / capture_frames : 0.0,
		              capture_max_ms, capture_failed ? " (write failed)" : "");
		if (capture_swaps) {
			console_print("Profile pre-swap: mean %.3f ms, maximum %.3f ms (%llu swaps).\n",
			              capture_pre_swap_total_ms / capture_swaps, capture_pre_swap_max_ms,
			              (unsigned long long)capture_swaps);
		}
	}
	return !capture_failed;
}

bool profiler_memory_sample_due(void) {
	if (!profiler_is_collecting()) {
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
	float milliseconds = get_seconds_elapsed(frame->frame_start, frame->frame_end) * 1000.0f;
	float interval = capture_frames ? get_seconds_elapsed(previous_start, frame->frame_start) * 1000.0f : 0.0f;
	previous_start = frame->frame_start;
	const profiler_metrics_t* m = &frame->metrics;
	fprintf(capture_file, "%.6f,%llu,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%.9g,%.9g,%.9g,%d,%d,%.9g",
	        get_seconds_elapsed(capture_start, frame->frame_end), (unsigned long long)capture_frames,
	        milliseconds, interval, (unsigned long long)m->resident_bytes,
	        (unsigned long long)m->texture_bytes, (unsigned long long)m->cpu_tile_bytes,
	        (unsigned long long)m->cache_table_bytes, (unsigned long long)m->cache_entries,
	        (unsigned long long)m->tile_metadata_bytes, (unsigned long long)m->mrxs_index_bytes,
	        (unsigned long long)m->pooled_bytes, m->image_count, m->replay_segment, m->replay_command,
	        m->camera_x, m->camera_y, m->zoom, m->viewport_width, m->viewport_height, m->display_scale);
	const profiler_section_t* present = &frame->sections[PROFILER_SECTION_PRESENT];
	// Blank when no swap occurred (e.g. a minimized/early-return frame).
	if (present->start >= frame->frame_start && present->end >= present->start) {
		float pre_swap_ms = get_seconds_elapsed(frame->frame_start, present->start) * 1000.0f;
		fprintf(capture_file, ",%.6f", pre_swap_ms);
		capture_pre_swap_total_ms += pre_swap_ms;
		capture_pre_swap_max_ms = MAX(capture_pre_swap_max_ms, pre_swap_ms);
		++capture_swaps;
	} else {
		fprintf(capture_file, ",");
	}
	for (i32 i = 0; i < PROFILER_TILE_STATE_COUNT; ++i) {
		fprintf(capture_file, ",%llu", (unsigned long long)frame->tiles.visible[i]);
	}
	const profiler_worker_metrics_t* completed = &frame->workers;
	fprintf(capture_file, ",%llu,%llu,%.6f,%.6f,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%llu",
	        (unsigned long long)frame->tiles.unready_levels, (unsigned long long)frame->tiles.first_draws,
	        frame->tiles.request_to_draw_ms, frame->tiles.request_to_draw_max_ms,
	        (unsigned long long)completed->jobs, (unsigned long long)completed->queued_jobs,
	        completed->queue_ms, completed->load_ms, completed->io_ms, completed->decode_ms,
	        (unsigned long long)completed->io_bytes);
	fprintf(capture_file, ",%llu,%.6f,%.6f", (unsigned long long)frame->tiles.ready_tiles,
	        frame->tiles.request_to_ready_ms, frame->tiles.request_to_ready_max_ms);
	fprintf(capture_file, ",%s", SLIDESCAPE_BUILD_CONFIGURATION);
	for (i32 i = 0; i < PROFILER_SECTION_COUNT; ++i) {
		const profiler_section_t* section = frame->sections + i;
		fprintf(capture_file, ",%.6f", section->elapsed_ms);
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

	memset(&frame_tiles, 0, sizeof(frame_tiles));
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
	s->active_start = s->start;
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
	if (s->active_start) {
		s->elapsed_ms += get_seconds_elapsed(s->active_start, s->end) * 1000.0f;
		s->active_start = 0;
	}
	if (global_profiler.current_level > 0) {
		--global_profiler.current_level;
	}

	if (section == PROFILER_SECTION_FRAME) {
		frame->frame_end = s->end;
		if (profiler_is_collecting()) {
			frame->metrics_valid = true;
			frame->metrics = capture_metrics;
			frame->memory_sample_clock = memory_sample_clock;
			frame->tiles = frame_tiles;
			platform_mutex_lock(&worker_metrics_lock);
			frame->workers = worker_metrics;
			memset(&worker_metrics, 0, sizeof(worker_metrics));
			platform_mutex_unlock(&worker_metrics_lock);
		}
		profiler_capture_frame(frame);
	}
}
