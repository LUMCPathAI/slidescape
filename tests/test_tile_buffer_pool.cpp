#include "doctest.h"

#include "tile_buffer_pool.h"

#include <thread>
#include <vector>

TEST_CASE("tile buffer pool reuses an exact-size allocation") {
	tile_buffer_pool_trim();
	tile_buffer_pool_stats_t before = tile_buffer_pool_get_stats();

	constexpr size_t size = 512 * 512 * 4;
	u8* first = tile_buffer_acquire(size);
	REQUIRE(first != nullptr);
	first[0] = 0x5a;
	tile_buffer_release(first, size);

	u8* second = tile_buffer_acquire(size);
	CHECK(second == first);
	tile_buffer_pool_stats_t after = tile_buffer_pool_get_stats();
	CHECK(after.system_allocations == before.system_allocations + 1);
	CHECK(after.reuse_count == before.reuse_count + 1);
	tile_buffer_release(second, size);
	tile_buffer_pool_trim();
}

TEST_CASE("tile buffer pool separates allocation sizes") {
	tile_buffer_pool_trim();
	u8* small = tile_buffer_acquire(256 * 512 * 4);
	REQUIRE(small != nullptr);
	tile_buffer_release(small, 256 * 512 * 4);

	u8* large = tile_buffer_acquire(512 * 512 * 4);
	REQUIRE(large != nullptr);
	CHECK(large != small);
	tile_buffer_release(large, 512 * 512 * 4);
	tile_buffer_pool_trim();
}

TEST_CASE("tile buffer pool is safe under concurrent reuse") {
	tile_buffer_pool_trim();
	constexpr size_t size = 512 * 512 * 4;
	std::vector<std::thread> threads;
	for (int thread_index = 0; thread_index < 8; ++thread_index) {
		threads.emplace_back([] {
			for (int iteration = 0; iteration < 2000; ++iteration) {
				u8* buffer = tile_buffer_acquire(size);
				REQUIRE(buffer != nullptr);
				buffer[0] = (u8)iteration;
				buffer[size - 1] = (u8)(iteration >> 8);
				tile_buffer_release(buffer, size);
			}
		});
	}
	for (std::thread& thread : threads) thread.join();
	tile_buffer_pool_stats_t stats = tile_buffer_pool_get_stats();
	CHECK(stats.retained_bytes <= MEGABYTES(64));
	CHECK(stats.retained_buffer_count <= 32);
	tile_buffer_pool_trim();
}
