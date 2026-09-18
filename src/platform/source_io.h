/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum source_read_result_t {
	SOURCE_READ_OK,
	SOURCE_READ_OUT_OF_BOUNDS,
	SOURCE_READ_IO_ERROR,
} source_read_result_t;

/* Exact reads: a short read of an otherwise valid range is an I/O error.
   Implementations must permit concurrent reads without a shared seek position. */
typedef source_read_result_t (*source_read_fn)(void* context, u64 offset, void* dest, size_t size);
typedef struct source_blob_t {
	u64 size;
	void* context;
	source_read_fn read;
	void (*close)(void* context);
} source_blob_t;

/* A bounded, borrowed view. Keep the blob alive until all readers have finished.
   Ordinary files and ZIP members use exactly the same read path. */
typedef struct source_file_t {
	source_blob_t* blob;
	u64 offset;
	u64 size;
} source_file_t;

bool source_blob_open_local(source_blob_t* blob, const char* path);
void source_blob_close(source_blob_t* blob);
bool source_file_view(source_blob_t* blob, u64 offset, u64 size, source_file_t* file);
source_read_result_t source_file_read(const source_file_t* file, u64 offset, void* dest, size_t size);

#ifdef __cplusplus
}
#endif
