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

#include "block_allocator.h"

/*
void* block_allocator_proc(allocator_t* this_allocator, size_t size_to_allocate, u32 mode, void* ptr_to_free_or_realloc) {
	ASSERT(this_allocator);
	void* result = NULL;
	switch(mode) {
		default: break;
		case ALLOCATOR_MODE_ALLOC: {

		} break;
		case ALLOCATOR_MODE_REALLOC: {

		} break;
		case ALLOCATOR_MODE_FREE: {

		} break;
	}
}
*/

void block_allocator_init(block_allocator_t* allocator, size_t block_size, size_t max_capacity_in_blocks, size_t chunk_size) {
	if (!allocator || block_size == 0 || max_capacity_in_blocks == 0 || chunk_size < block_size) {
		console_print_error("block_allocator_init(): invalid allocator dimensions\n");
		fatal_error();
	}

	size_t chunk_capacity_in_blocks = chunk_size / block_size;
	size_t chunk_count = 1 + (max_capacity_in_blocks - 1) / chunk_capacity_in_blocks;
	if (chunk_count > SIZE_MAX / sizeof(block_allocator_chunk_t) ||
	    max_capacity_in_blocks > SIZE_MAX / sizeof(block_allocator_item_t)) {
		console_print_error("block_allocator_init(): bookkeeping size overflow\n");
		fatal_error();
	}
	memset(allocator, 0, sizeof(*allocator));
	allocator->block_size = block_size;
	allocator->max_capacity_in_blocks = max_capacity_in_blocks;
	allocator->chunk_capacity_in_blocks = chunk_capacity_in_blocks;
	allocator->chunk_size = chunk_size;
	allocator->chunk_count = chunk_count;
	allocator->chunks = calloc(1, chunk_count * sizeof(block_allocator_chunk_t));
	allocator->free_list_storage = calloc(1, max_capacity_in_blocks * sizeof(block_allocator_item_t));
	allocator->allocation_state = calloc(1, max_capacity_in_blocks);
	if (!allocator->chunks || !allocator->free_list_storage || !allocator->allocation_state) {
		free(allocator->chunks);
		free(allocator->free_list_storage);
		free(allocator->allocation_state);
		memset(allocator, 0, sizeof(*allocator));
		console_print_error("block_allocator_init(): failed to allocate bookkeeping storage\n");
		fatal_error();
	}
    platform_mutex_init(&allocator->lock);
	allocator->is_valid = true;
}

void block_allocator_destroy(block_allocator_t* allocator) {
	if (!allocator) return;
	for (size_t i = 0; i < allocator->used_chunks; ++i) {
		block_allocator_chunk_t* chunk = allocator->chunks + i;
		if (chunk->memory) free(chunk->memory);
	}
	if (allocator->chunks) free(allocator->chunks);
	if (allocator->free_list_storage) free(allocator->free_list_storage);
	if (allocator->allocation_state) free(allocator->allocation_state);
    platform_mutex_destroy(&allocator->lock);
	free(allocator);
}

void* block_alloc(block_allocator_t* allocator) {
	void* result = NULL;
    platform_mutex_lock(&allocator->lock);
	if (allocator->free_list != NULL) {
		// Grab a block from the free list
		block_allocator_item_t* free_item = allocator->free_list;
		result = allocator->chunks[free_item->chunk_index].memory + free_item->block_index * allocator->block_size;
		size_t global_index = free_item->chunk_index * allocator->chunk_capacity_in_blocks + free_item->block_index;
		ASSERT(global_index < allocator->max_capacity_in_blocks);
		ASSERT(allocator->allocation_state[global_index] == 0);
		allocator->allocation_state[global_index] = 1;
		allocator->free_list = free_item->next;
		--allocator->free_list_length;
	} else {
		if (allocator->next_unused_block_index >= allocator->max_capacity_in_blocks) {
			platform_mutex_unlock(&allocator->lock);
			console_print_error("block_alloc(): capacity exhausted\n");
			fatal_error();
		}

		size_t global_index = allocator->next_unused_block_index++;
		size_t chunk_index = global_index / allocator->chunk_capacity_in_blocks;
		size_t block_index = global_index % allocator->chunk_capacity_in_blocks;
		ASSERT(chunk_index < allocator->chunk_count);
		block_allocator_chunk_t* current_chunk = allocator->chunks + chunk_index;
		if (!current_chunk->memory) {
			current_chunk->memory = (u8*)malloc(allocator->chunk_size);
			if (!current_chunk->memory) {
				platform_mutex_unlock(&allocator->lock);
				console_print_error("block_alloc(): failed to allocate a chunk\n");
				fatal_error();
			}
			allocator->used_chunks = MAX(allocator->used_chunks, chunk_index + 1);
		}
		current_chunk->used_blocks = MAX(current_chunk->used_blocks, block_index + 1);
		allocator->allocation_state[global_index] = 1;
		result = current_chunk->memory + block_index * allocator->block_size;
	}
    platform_mutex_unlock(&allocator->lock);
	return result;
}

void block_free(block_allocator_t* allocator, void* ptr_to_free) {
    platform_mutex_lock(&allocator->lock);
	block_allocator_item_t free_item = {0};
	// Find the right chunk
	size_t chunk_index = SIZE_MAX;
	for (size_t i = 0; i < allocator->used_chunks; ++i) {
		block_allocator_chunk_t* chunk = allocator->chunks + i;
		if (!chunk->memory) continue;
		uintptr_t ptr_value = (uintptr_t)ptr_to_free;
		uintptr_t chunk_start = (uintptr_t)chunk->memory;
		bool match = ptr_value >= chunk_start && ptr_value - chunk_start < allocator->chunk_size;
		if (match) {
			chunk_index = i;
			break;
		}
	}
	if (chunk_index != SIZE_MAX) {
		block_allocator_chunk_t* chunk = allocator->chunks + chunk_index;
		size_t offset = (size_t)((u8*)ptr_to_free - chunk->memory);
		size_t block_index = offset / allocator->block_size;
		size_t global_index = chunk_index * allocator->chunk_capacity_in_blocks + block_index;
		bool valid = (offset % allocator->block_size) == 0 &&
		             block_index < chunk->used_blocks &&
		             global_index < allocator->max_capacity_in_blocks &&
		             allocator->allocation_state[global_index] != 0 &&
		             allocator->free_list_length < allocator->max_capacity_in_blocks;
		if (!valid) {
			platform_mutex_unlock(&allocator->lock);
			console_print_error("block_free(): invalid or duplicate pointer\n");
			fatal_error();
		}
		allocator->allocation_state[global_index] = 0;
		free_item.next = allocator->free_list;
		free_item.chunk_index = chunk_index;
		free_item.block_index = block_index;
		size_t free_index = allocator->free_list_length++;
		allocator->free_list_storage[free_index] = free_item;
		allocator->free_list = allocator->free_list_storage + free_index;
        platform_mutex_unlock(&allocator->lock);
	} else {
		console_print_error("block_free(): invalid pointer!\n");
		fatal_error();
	}

}
