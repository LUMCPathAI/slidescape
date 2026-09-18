/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "source_io.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SOURCE_PATH_CAPACITY = 512,
	SOURCE_ZIP_MAX_MEMBERS = 1000000,
	SOURCE_ZIP_INDEX_BUDGET = 64 * 1024 * 1024,
};

typedef struct source_zip_member_t {
	char* path; /* Original UTF-8 relative path; directories end in '/'. */
	source_file_t file;
	u32 crc32;
	bool is_directory;
} source_zip_member_t;

typedef struct source_zip_t source_zip_t;

/* Mount performs bounded structural validation only, never a content CRC scan.
   The container is borrowed and must outlive the index and all member views. */
source_zip_t* source_zip_open(const source_file_t* container, char* error, size_t error_capacity);
void source_zip_close(source_zip_t* zip);
u32 source_zip_count(const source_zip_t* zip);
const source_zip_member_t* source_zip_member(const source_zip_t* zip, u32 index);
const source_zip_member_t* source_zip_find(const source_zip_t* zip, const char* path);
/* Explicit ingest/deep-check operation, not needed for viewer opening. */
bool source_zip_validate_contents(const source_zip_t* zip, char* error, size_t error_capacity);

bool source_path_is_valid(const char* path, bool is_directory);

typedef struct source_zip_input_t {
	const char* path;
	source_file_t file; /* Ignored for directories. */
	bool is_directory;
} source_zip_input_t;
typedef bool (*source_zip_write_fn)(void* context, const void* data, size_t size);

/* Writes a complete stored ZIP64 sequentially. The sink can hash the entire blob
   while writing. On any failure, discard the output; publishing/staging belongs
   to the caller. No per-member SHA identities or filesystem writes are hidden here. */
bool source_zip_write(const source_zip_input_t* members, u32 count,
		source_zip_write_fn write, void* context, char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
