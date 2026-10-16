/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tile_buffer_pool_stats_t {
	u64 system_allocations;
	u64 reuse_count;
	u64 release_count;
	u64 system_frees;
	u64 retained_buffer_count;
	u64 retained_bytes;
} tile_buffer_pool_stats_t;

/*
 * Buffers are grouped by exact byte size. The pool may adopt any malloc-owned
 * buffer, and acquired buffers remain ordinary malloc allocations. Calling
 * free() instead of releasing an acquired buffer is safe but misses reuse.
 * The size passed to release must be the allocation's exact byte size.
 */
u8* tile_buffer_acquire(size_t size);
void tile_buffer_release(u8* buffer, size_t size);
void tile_buffer_pool_trim(void);
tile_buffer_pool_stats_t tile_buffer_pool_get_stats(void);

#ifdef __cplusplus
}
#endif
