/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_repository.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct archive_ingest_result_t {
	i32 study_count;
	i32 series_count;
	i32 instance_count;
} archive_ingest_result_t;

/* Discover Part 10 .dcm sources recursively below objects/, import immutable
 * content-addressed copies, and atomically publish their metadata in SQLite. */
bool archive_ingest_part10_objects(const archive_repository_t* repository,
		archive_ingest_result_t* result, char* error, size_t error_capacity);
bool archive_ingest_part10_path(const archive_repository_t* repository,
		const char* source_path, bool reference_source,
		archive_ingest_result_t* result, char* error, size_t error_capacity);

/*
 * Dispatch an explicit source path to the supported adapter. MRXS currently
 * requires reference_source because owned multi-file bundle import is not yet
 * implemented.
 */
bool archive_ingest_path(const archive_repository_t* repository,
		const char* source_path, bool reference_source,
		archive_ingest_result_t* result, char* error,
		size_t error_capacity);

#ifdef __cplusplus
}
#endif
