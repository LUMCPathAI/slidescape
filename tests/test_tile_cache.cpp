#include "doctest.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include "common.h"
#include "tile_cache.h"

typedef struct tile_cache_test_image_t {
	image_t image;
	tile_t geometry;
} tile_cache_test_image_t;

static void tile_cache_test_image_init(tile_cache_test_image_t* fixture) {
	memset(fixture, 0, sizeof(*fixture));
	fixture->image.type = IMAGE_TYPE_WSI;
	fixture->image.backend = IMAGE_BACKEND_DICOM;
	fixture->image.level_count = 1;

	level_image_t* level = fixture->image.level_images;
	level->exists = true;
	level->tile_count = 1;
	level->width_in_tiles = 1;
	level->height_in_tiles = 1;
	level->tile_width = 1;
	level->tile_height = 1;
	level->tiles = &fixture->geometry;

	fixture->image.tile_cache = tile_cache_create(&fixture->image);
}

static void tile_cache_test_image_destroy(tile_cache_test_image_t* fixture) {
	tile_cache_destroy(fixture->image.tile_cache);
	fixture->image.tile_cache = nullptr;
}

TEST_CASE("tile cache suppresses duplicate decode requests") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                        TILE_CACHE_DEMAND_CPU_RESIDENCY, 20, 1));

	tile_cache_cancel_decode(&fixture.image, 0, 0);
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_CPU_RESIDENCY, 20, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("tile cache grants one decode reservation to competing callers") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	std::atomic<bool> start = false;
	std::atomic<i32> reservations = 0;
	std::vector<std::thread> callers;
	for (i32 i = 0; i < 16; ++i) {
		callers.emplace_back([&]() {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			if (tile_cache_try_begin_decode(&fixture.image, 0, 0,
			                                TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1)) {
				reservations.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	start.store(true, std::memory_order_release);
	for (std::thread& caller : callers) {
		caller.join();
	}

	CHECK(reservations.load(std::memory_order_relaxed) == 1);
	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("hard demand keeps an older viewer decode alive") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 1));

	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_task_is_stale(&fixture.image, 0, 0, 1));
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                        TILE_CACHE_DEMAND_READ_REGION, 20, 0));

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("CPU tile pixels live until the final pin is released") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	pixels[0] = 0x5a;
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, pixels);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	u8* pinned_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0);
	REQUIRE(pinned_pixels != nullptr);
	CHECK(pinned_pixels[0] == 0x5a);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK(tile_cache_get_cpu_pixels(&fixture.image, 0, 0) == nullptr);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("hard demand bits follow overlapping pin types") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_EXPORT);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_task_is_stale(&fixture.image, 0, 0, 1));
	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_EXPORT);
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("cached upload reservation pins CPU pixels") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	pixels[0] = 0x7c;
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, pixels);

	REQUIRE(tile_cache_try_begin_upload(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	u8* upload_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0);
	REQUIRE(upload_pixels != nullptr);
	CHECK(upload_pixels[0] == 0x7c);

	tile_cache_release_upload_cpu_pin(&fixture.image, 0, 0);
	CHECK(tile_cache_get_cpu_pixels(&fixture.image, 0, 0) == nullptr);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("tile cache drains posted results in order") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	tile_cache_result_t first = {};
	first.level = 0;
	first.tile_index = 0;
	first.resource_id = 11;
	tile_cache_result_t second = {};
	second.level = 0;
	second.tile_index = 0;
	second.resource_id = 12;
	second.stale = true;

	REQUIRE(tile_cache_post_load_result(&fixture.image, &first));
	REQUIRE(tile_cache_post_load_result(&fixture.image, &second));

	tile_cache_result_t result = {};
	REQUIRE(tile_cache_poll_load_result(&fixture.image, &result));
	CHECK(result.resource_id == 11);
	CHECK_FALSE(result.stale);
	REQUIRE(tile_cache_poll_load_result(&fixture.image, &result));
	CHECK(result.resource_id == 12);
	CHECK(result.stale);
	CHECK_FALSE(tile_cache_poll_load_result(&fixture.image, &result));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("tile cache completion polling is exact once across consumers") {
	constexpr i32 result_count = 256;
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	for (i32 i = 0; i < result_count; ++i) {
		tile_cache_result_t result = {};
		result.resource_id = i;
		REQUIRE(tile_cache_post_load_result(&fixture.image, &result));
	}

	std::array<std::atomic<i32>, result_count> seen = {};
	std::atomic<i32> consumed = 0;
	std::vector<std::thread> consumers;
	for (i32 consumer_index = 0; consumer_index < 4; ++consumer_index) {
		consumers.emplace_back([&]() {
			for (i32 attempt = 0;
			     attempt < 100000 && consumed.load(std::memory_order_relaxed) < result_count;
			     ++attempt) {
				tile_cache_result_t result = {};
				if (tile_cache_poll_load_result(&fixture.image, &result)) {
					if (result.resource_id >= 0 && result.resource_id < result_count) {
						seen[result.resource_id].fetch_add(1, std::memory_order_relaxed);
					}
					consumed.fetch_add(1, std::memory_order_relaxed);
				} else {
					std::this_thread::yield();
				}
			}
		});
	}
	for (std::thread& consumer : consumers) {
		consumer.join();
	}

	CHECK(consumed.load(std::memory_order_relaxed) == result_count);
	for (i32 i = 0; i < result_count; ++i) {
		CHECK(seen[i].load(std::memory_order_relaxed) == 1);
	}

	tile_cache_result_t result = {};
	CHECK_FALSE(tile_cache_poll_load_result(&fixture.image, &result));
	tile_cache_test_image_destroy(&fixture);
}
