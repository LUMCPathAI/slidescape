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
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILER_FRAME_BUFFER_SIZE 256

#define PROFILER_SECTION_LIST(X) \
	X(FRAME, "Frame") \
	X(INPUT, "Input") \
	X(PLATFORM_EVENTS, "Platform Events") \
	X(INPUT_STATE, "Input State") \
	X(INPUT_WAIT, "Input Wait") \
	X(GUI_NEW_FRAME, "GUI New Frame") \
	X(VIEWER, "Viewer") \
	X(PROCESS_WSI_INPUT, "Process WSI input") \
	X(DRAW_ANNOTATIONS, "Draw Annotations") \
	X(RENDER_IMAGE, "Render Image") \
	X(MEMORY_SAMPLE, "Memory sample") \
	X(COMPLETION_QUEUE, "Completion Queue") \
	X(TEXTURE_UPLOAD, "Texture Upload") \
	X(TEXTURE_FINALIZE, "Texture Finalize") \
	X(TILE_LOADING, "Tile Loading") \
	X(SCENE_RENDER, "Scene Render") \
	X(GUI_DRAW, "GUI Draw") \
	X(IMGUI_RENDER, "ImGui Render") \
	X(IMGUI_BUILD, "ImGui Build") \
	X(IMGUI_EXTRA_RENDER, "ImGui Extra Render") \
	X(IMGUI_MAIN_RENDER, "ImGui Main Render") \
	X(PRESENT, "Present")

typedef enum profiler_section_enum {
#define PROFILER_SECTION_ENUM_ENTRY(id, name) PROFILER_SECTION_##id,
	PROFILER_SECTION_LIST(PROFILER_SECTION_ENUM_ENTRY)
#undef PROFILER_SECTION_ENUM_ENTRY
	PROFILER_SECTION_COUNT,
} profiler_section_enum;

typedef struct {
	i64 start;
	i64 end;
	i64 active_start;
	float elapsed_ms; // Sum of repeated invocations; start/end retain the last span for the UI.
	u8 level;
} profiler_section_t;

typedef struct profiler_metrics_t {
	u64 resident_bytes;
	u64 texture_bytes; // Estimated pixel storage, excluding driver overhead.
	u64 cpu_tile_bytes;
	u64 cache_table_bytes;
	u64 cache_entries;
	u64 tile_metadata_bytes;
	u64 mrxs_index_bytes;
	u64 pooled_bytes;
	u32 image_count;
	u32 replay_segment;
	u32 replay_command;
	float camera_x;
	float camera_y;
	float zoom;
	i32 viewport_width;
	i32 viewport_height;
	float display_scale;
} profiler_metrics_t;

typedef enum profiler_visible_tile_state_t {
	PROFILER_TILE_READY,
	PROFILER_TILE_ADJACENT_PLANE,
	PROFILER_TILE_MISSING,
	PROFILER_TILE_FAILED,
	PROFILER_TILE_EMPTY,
	PROFILER_TILE_STATE_COUNT,
} profiler_visible_tile_state_t;

typedef struct profiler_worker_metrics_t {
	u64 jobs;
	u64 queued_jobs;
	u64 io_bytes;
	double load_ms;
	double queue_ms;
	double io_ms;
	double decode_ms;
} profiler_worker_metrics_t;

typedef struct profiler_tile_metrics_t {
	u64 visible[PROFILER_TILE_STATE_COUNT];
	u64 unready_levels;
	u64 ready_tiles;
	double request_to_ready_ms;
	float request_to_ready_max_ms;
	u64 first_draws;
	double request_to_draw_ms;
	float request_to_draw_max_ms;
} profiler_tile_metrics_t;

typedef struct {
	i64 frame_start;
	i64 frame_end;
	profiler_section_t sections[PROFILER_SECTION_COUNT];
	// Immutable completed-frame snapshots shared by CSV and the GUI.
	bool metrics_valid;
	i64 memory_sample_clock;
	profiler_metrics_t metrics;
	profiler_worker_metrics_t workers;
	profiler_tile_metrics_t tiles;
} profiler_frame_t;

typedef struct {
	profiler_frame_t frames[PROFILER_FRAME_BUFFER_SIZE];
	i32 current_frame;
	bool paused;
	i32 displayed_frame;
	u8 current_level;
} profiler_state_t;

extern profiler_state_t global_profiler;
extern const char* profiler_section_names[];

// These counters belong to the current frame and are independent of sampled memory.
void profiler_record_visible_tile(profiler_visible_tile_state_t state);
void profiler_record_unready_level(void);
void profiler_record_tile_draw(u64 capture_id, i64 request_clock);
void profiler_record_tile_ready(u64 capture_id, i64 request_clock);

// Thread-safe capture generation. Zero disables instrumentation; old jobs cannot
// contribute to a later capture. Worker measurements are thread-local until end.
u64 profiler_capture_id(void);
void profiler_worker_begin(u64 capture_id, i64 queued_clock);
void profiler_worker_end(void);
i64 profiler_worker_decode_begin(void);
void profiler_worker_decode_end(i64 start);
i64 profiler_worker_io_begin(void);
void profiler_worker_io_end(i64 start, u64 bytes);

// Main-thread API. Live collection is enabled only while the GUI requests it.
// CSV capture continues even when the profiler UI is closed or paused.
void profiler_set_live_enabled(bool enabled);
bool profiler_is_collecting(void);
const char* profiler_build_configuration(void);
bool profiler_start_capture(const char* path);
bool profiler_stop_capture(void);
bool profiler_is_capturing(void);
bool profiler_memory_sample_due(void);
u64 profiler_resident_bytes(void);
void profiler_set_metrics(const profiler_metrics_t* metrics);
void profiler_new_frame(void);
void profiler_begin(i32 section);
void profiler_end(i32 section);

#ifdef __cplusplus
}
#endif
