/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include "source_zip.h"
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct source_fs_t source_fs_t;
typedef struct source_entry_t {
	const char* path; /* Namespace-relative, borrowed for the callback. No trailing '/'. */
	u64 size;
	bool is_directory;
} source_entry_t;
typedef bool (*source_visit_fn)(void* context, const source_entry_t* entry);

source_fs_t* source_fs_open_directory(const char* path);
source_fs_t* source_fs_open_zip(const char* path, char* error, size_t error_capacity);
/* The namespace owns opened local handles and the ZIP blob/index. Open files on
   the loading thread, then read views concurrently. Close after workers finish. */
void source_fs_close(source_fs_t* source);
bool source_fs_open_file(source_fs_t* source, const char* path, source_file_t* file);
bool source_fs_stat(source_fs_t* source, const char* path, source_entry_t* entry);
/* Visit immediate children. Empty path denotes the namespace root. */
bool source_fs_list(source_fs_t* source, const char* directory, source_visit_fn visit, void* context);
/* Small parser inputs only; caller supplies a limit and owns the returned buffer. */
mem_t* source_fs_read_entire(source_fs_t* source, const char* path, size_t limit);

#ifdef __cplusplus
}
#endif
