/*
  BSD 2-Clause License

  Copyright (c) 2019-2026, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "platform_mutex.h"

// See:
// https://github.com/SasLuca/rayfork/blob/rayfork-0.9/source/core/rayfork-core.c

enum allocator_mode {
	ALLOCATOR_MODE_UNKNOWN = 0,
	ALLOCATOR_MODE_ALLOC,
	ALLOCATOR_MODE_REALLOC,
	ALLOCATOR_MODE_FREE,
};

typedef struct allocator_t allocator_t;
struct allocator_t {
	void* userdata;
	void* (*proc)(allocator_t* this_allocator, size_t size_to_allocate, u32 mode, void* ptr_to_free_or_realloc);
};


typedef struct block_allocator_item_t block_allocator_item_t;
struct block_allocator_item_t {
	size_t chunk_index;
	size_t block_index;
	block_allocator_item_t* next;
};

typedef struct block_allocator_chunk_t {
	size_t used_blocks;
	u8* memory;
} block_allocator_chunk_t;

typedef struct block_allocator_t {
	size_t block_size;
	size_t max_capacity_in_blocks;
	size_t chunk_capacity_in_blocks;
	size_t chunk_size;
	size_t chunk_count;
	size_t used_chunks;
	size_t next_unused_block_index;
	block_allocator_chunk_t* chunks;
	block_allocator_item_t* free_list_storage;
	block_allocator_item_t* free_list;
	size_t free_list_length;
	u8* allocation_state;
	platform_mutex_t lock;
	bool is_valid;
} block_allocator_t;

void block_allocator_init(block_allocator_t* allocator, size_t block_size, size_t max_capacity_in_blocks, size_t chunk_size);
// The allocator object itself must be heap-allocated; destroy releases it too.
void block_allocator_destroy(block_allocator_t* allocator);
void* block_alloc(block_allocator_t* allocator);
void block_free(block_allocator_t* allocator, void* ptr_to_free);

#ifdef __cplusplus
};
#endif
