#include "doctest.h"

#include "common.h"
#include "block_allocator.h"

#include <cstdlib>
#include <set>

TEST_CASE("block allocator supports capacities smaller than one chunk") {
	block_allocator_t* allocator = (block_allocator_t*)malloc(sizeof(*allocator));
	REQUIRE(allocator != nullptr);
	block_allocator_init(allocator, 16, 2, 128);

	void* first = block_alloc(allocator);
	void* second = block_alloc(allocator);
	CHECK(first != nullptr);
	CHECK(second != nullptr);
	CHECK(first != second);

	block_free(allocator, first);
	CHECK(block_alloc(allocator) == first);
	block_allocator_destroy(allocator);
}

TEST_CASE("block allocator reuses freed blocks without exceeding its configured capacity") {
	block_allocator_t* allocator = (block_allocator_t*)malloc(sizeof(*allocator));
	REQUIRE(allocator != nullptr);
	block_allocator_init(allocator, 16, 10, 48);

	void* blocks[10] = {};
	std::set<void*> unique_blocks;
	for (size_t i = 0; i < COUNT(blocks); ++i) {
		blocks[i] = block_alloc(allocator);
		CHECK(unique_blocks.insert(blocks[i]).second);
	}

	std::set<void*> freed_blocks;
	for (size_t i = 1; i < COUNT(blocks); i += 2) {
		freed_blocks.insert(blocks[i]);
		block_free(allocator, blocks[i]);
	}
	for (size_t i = 0; i < freed_blocks.size(); ++i) {
		CHECK(freed_blocks.count(block_alloc(allocator)) == 1);
	}

	block_allocator_destroy(allocator);
}
