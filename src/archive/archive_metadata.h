/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_shell.h"

#define ARCHIVE_MAX_METADATA_SIZE MEGABYTES(256)

#ifdef __cplusplus
extern "C" {
#endif

/* A NULL destination counts the exact encoded size, bounded by capacity.
 * The reported size excludes the terminating NUL; capacity must include it. */
bool archive_metadata_write_instance(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, char* dest, size_t capacity,
		size_t* size, bool array_wrapper);
bool archive_metadata_get_capacity(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u64* capacity);

#ifdef __cplusplus
}
#endif
