/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "tile_buffer_pool.h"

#include "platform_mutex.h"

#define TILE_BUFFER_POOL_BUCKET_COUNT 16
#define TILE_BUFFER_POOL_MAX_BUFFERS_PER_BUCKET 32
#define TILE_BUFFER_POOL_MAX_RETAINED_BYTES MEGABYTES(64)
#define TILE_BUFFER_POOL_MAX_BUFFER_SIZE MEGABYTES(16)
#if SLIDESCAPE_USE_MIMALLOC
/* mimalloc's cross-thread path is faster than the shared pool for small tiles. */
#define TILE_BUFFER_POOL_MIN_BUFFER_SIZE KILOBYTES(512)
#else
#define TILE_BUFFER_POOL_MIN_BUFFER_SIZE 0
#endif

typedef struct tile_buffer_free_node_t {
	struct tile_buffer_free_node_t* next;
} tile_buffer_free_node_t;

typedef struct tile_buffer_bucket_t {
	size_t size;
	size_t count;
	tile_buffer_free_node_t* head;
} tile_buffer_bucket_t;

static platform_mutex_t tile_buffer_pool_lock = PLATFORM_MUTEX_INITIALIZER;
static tile_buffer_bucket_t tile_buffer_buckets[TILE_BUFFER_POOL_BUCKET_COUNT];
static tile_buffer_pool_stats_t tile_buffer_pool_stats;

static tile_buffer_bucket_t* tile_buffer_find_bucket(size_t size, bool create) {
	tile_buffer_bucket_t* empty = NULL;
	for (i32 i = 0; i < COUNT(tile_buffer_buckets); ++i) {
		tile_buffer_bucket_t* bucket = tile_buffer_buckets + i;
		if (bucket->size == size) return bucket;
		if (!empty && bucket->size == 0) empty = bucket;
	}
	if (create && empty) {
		empty->size = size;
		return empty;
	}
	return NULL;
}

u8* tile_buffer_acquire(size_t size) {
	if (size == 0) return NULL;
	if (size < TILE_BUFFER_POOL_MIN_BUFFER_SIZE) return (u8*)malloc(size);

	platform_mutex_lock(&tile_buffer_pool_lock);
	tile_buffer_bucket_t* bucket = tile_buffer_find_bucket(size, false);
	if (bucket && bucket->head) {
		tile_buffer_free_node_t* node = bucket->head;
		bucket->head = node->next;
		--bucket->count;
		++tile_buffer_pool_stats.reuse_count;
		--tile_buffer_pool_stats.retained_buffer_count;
		tile_buffer_pool_stats.retained_bytes -= size;
		platform_mutex_unlock(&tile_buffer_pool_lock);
		return (u8*)node;
	}
	++tile_buffer_pool_stats.system_allocations;
	platform_mutex_unlock(&tile_buffer_pool_lock);

	return (u8*)malloc(size);
}

void tile_buffer_release(u8* buffer, size_t size) {
	if (!buffer) return;
	if (size < TILE_BUFFER_POOL_MIN_BUFFER_SIZE || size < sizeof(tile_buffer_free_node_t) ||
	    size > TILE_BUFFER_POOL_MAX_BUFFER_SIZE) {
		free(buffer);
		return;
	}

	bool retained = false;
	platform_mutex_lock(&tile_buffer_pool_lock);
	++tile_buffer_pool_stats.release_count;
	tile_buffer_bucket_t* bucket = tile_buffer_find_bucket(size, true);
	if (bucket && bucket->count < TILE_BUFFER_POOL_MAX_BUFFERS_PER_BUCKET &&
	    tile_buffer_pool_stats.retained_bytes <= TILE_BUFFER_POOL_MAX_RETAINED_BYTES - size) {
		tile_buffer_free_node_t* node = (tile_buffer_free_node_t*)buffer;
		node->next = bucket->head;
		bucket->head = node;
		++bucket->count;
		++tile_buffer_pool_stats.retained_buffer_count;
		tile_buffer_pool_stats.retained_bytes += size;
		retained = true;
	} else {
		++tile_buffer_pool_stats.system_frees;
	}
	platform_mutex_unlock(&tile_buffer_pool_lock);

	if (!retained) free(buffer);
}

void tile_buffer_pool_trim(void) {
	platform_mutex_lock(&tile_buffer_pool_lock);
	for (i32 i = 0; i < COUNT(tile_buffer_buckets); ++i) {
		tile_buffer_bucket_t* bucket = tile_buffer_buckets + i;
		tile_buffer_free_node_t* node = bucket->head;
		while (node) {
			tile_buffer_free_node_t* next = node->next;
			free(node);
			node = next;
			++tile_buffer_pool_stats.system_frees;
		}
		*bucket = (tile_buffer_bucket_t){0};
	}
	tile_buffer_pool_stats.retained_buffer_count = 0;
	tile_buffer_pool_stats.retained_bytes = 0;
	platform_mutex_unlock(&tile_buffer_pool_lock);
}

tile_buffer_pool_stats_t tile_buffer_pool_get_stats(void) {
	platform_mutex_lock(&tile_buffer_pool_lock);
	tile_buffer_pool_stats_t result = tile_buffer_pool_stats;
	platform_mutex_unlock(&tile_buffer_pool_lock);
	return result;
}
