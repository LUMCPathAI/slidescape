#include "doctest.h"

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

TEST_CASE("hard demand keeps an older viewer decode alive") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	fixture.image.tile_cache->viewer_generation = 2;
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
