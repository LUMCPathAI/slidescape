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
	X(GUI_NEW_FRAME, "GUI New Frame") \
	X(VIEWER, "Viewer") \
	X(PROCESS_WSI_INPUT, "Process WSI input") \
	X(DRAW_ANNOTATIONS, "Draw Annotations") \
	X(RENDER_IMAGE, "Render Image") \
	X(MEMORY_SAMPLE, "Memory sample") \
	X(COMPLETION_QUEUE, "Completion Queue") \
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
	u8 level;
} profiler_section_t;

typedef struct {
	i64 frame_start;
	i64 frame_end;
	profiler_section_t sections[PROFILER_SECTION_COUNT];
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

// Main-thread API. CSV capture continues even when the profiler UI is paused.
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
