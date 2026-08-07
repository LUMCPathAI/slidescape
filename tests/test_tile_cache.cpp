#include "doctest.h"

#include "common.h"
#include "intrinsics.h"
#include "tile_cache.h"
#include "work_queue.h"

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

static void tile_cache_test_ensure_thread_memory(void) {
	if (!threadlocal_thread_memory) {
		init_global_system_info(false);
		init_thread_memory(&global_system_info);
	}
}

static void tile_cache_test_ensure_global_thread_pool(void) {
	tile_cache_test_ensure_thread_memory();
	if (!global_thread_pool.initialized) {
		if (global_system_info.suggested_total_thread_count < 2) {
			global_system_info.suggested_total_thread_count = 2;
		}
		init_thread_pool(&global_thread_pool, 128, true, false, NULL);
	}
}

typedef struct tile_cache_reserve_task_t {
	image_t* image;
	i32 volatile* reservations;
} tile_cache_reserve_task_t;

static void tile_cache_reserve_decode_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	tile_cache_reserve_task_t* task = (tile_cache_reserve_task_t*)userdata;
	if (tile_cache_try_begin_decode(task->image, 0, 0,
	                                TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1)) {
		atomic_increment(task->reservations);
	}
}

typedef struct tile_cache_consume_task_t {
	image_t* image;
	i32 volatile* seen;
	i32 volatile* consumed;
	i32 result_count;
} tile_cache_consume_task_t;

static void tile_cache_consume_results_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	tile_cache_consume_task_t* task = (tile_cache_consume_task_t*)userdata;
	for (i32 attempt = 0; attempt < 100000 && *task->consumed < task->result_count; ++attempt) {
		tile_cache_result_t result = {};
		if (tile_cache_poll_load_result(task->image, &result)) {
			if (result.resource_id >= 0 && result.resource_id < task->result_count) {
				atomic_increment(task->seen + result.resource_id);
			}
			atomic_increment(task->consumed);
		} else {
			platform_sleep(0);
		}
	}
}

typedef struct tile_cache_apply_task_t {
	image_t* image;
	tile_cache_result_t result;
} tile_cache_apply_task_t;

static void tile_cache_apply_result_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	tile_cache_apply_task_t* task = (tile_cache_apply_task_t*)userdata;
	tile_cache_result_action_t action = {0};
	tile_cache_apply_result(task->image, &task->result, false, &action);
	free(action.pixels_to_free);
}

typedef struct tile_cache_pin_task_t {
	image_t* image;
} tile_cache_pin_task_t;

static void tile_cache_pin_read_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	tile_cache_pin_task_t* task = (tile_cache_pin_task_t*)userdata;
	tile_cache_pin_cpu_tile(task->image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
}

typedef struct tile_cache_read_region_state_t {
	image_t* image;
	u32 output_pixel;
	bool8 succeeded;
} tile_cache_read_region_state_t;

typedef struct tile_cache_read_region_task_t {
	tile_cache_read_region_state_t* state;
} tile_cache_read_region_task_t;

static void tile_cache_read_region_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	tile_cache_read_region_task_t* task = (tile_cache_read_region_task_t*)userdata;
	tile_cache_read_region_state_t* state = task->state;
	state->succeeded = image_read_region(state->image, 0, 0, 0, 1, 1,
	                                    &state->output_pixel, PIXEL_FORMAT_U8_BGRA);
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

TEST_CASE("GPU-resident tile can be decoded for a later CPU request") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	tile_cache_store_gpu_texture(&fixture.image, 0, 0, 42);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                        TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_CPU_RESIDENCY, 10, 1));
	tile_cache_cancel_decode(&fixture.image, 0, 0);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("tile cache grants one decode reservation to competing callers") {
	tile_cache_test_ensure_thread_memory();
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 4;
	thread_pool_t pool = {};
	init_thread_pool(&pool, 32, false, false, NULL);

	i32 volatile reservations = 0;
	tile_cache_reserve_task_t task = {&fixture.image, &reservations};
	for (i32 i = 0; i < 16; ++i) {
		REQUIRE(thread_pool_submit_task(&pool, tile_cache_reserve_decode_task,
		                                &task, sizeof(task)));
	}
	thread_pool_wait_for_completion(&pool);

	CHECK(reservations == 1);
	thread_pool_destroy(&pool);
	global_system_info = old_system_info;
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

TEST_CASE("retryable tile failure releases its reservation without marking empty") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE |
	                                    TILE_CACHE_DEMAND_GPU_RESIDENCY,
	                                    10, 0));
	tile_cache_result_t result = {};
	result.level = 0;
	result.tile_index = 0;
	result.failed = true;
	result.retryable_failure = true;
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(&fixture.image, &result, true, &action));
	CHECK_FALSE(action.mark_empty);
	CHECK_FALSE(tile_cache_tile_is_busy(&fixture.image, 0, 0));
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_VIEWER_VISIBLE |
	                                  TILE_CACHE_DEMAND_GPU_RESIDENCY,
	                                  10, 0));
	tile_cache_cancel_decode(&fixture.image, 0, 0);
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

TEST_CASE("decode completion atomically adopts pixels before releasing the reservation") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_CPU_RESIDENCY, 10, 1));

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	pixels[0] = 0x39;
	tile_cache_result_t result = {};
	result.level = 0;
	result.tile_index = 0;
	result.pixel_memory = pixels;
	result.want_cpu_residency = true;
	result.want_gpu_residency = true;

	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(&fixture.image, &result, true, &action));
	CHECK(action.pixels_to_free == nullptr);
	CHECK(action.should_upload);
	CHECK(action.upload_pixels == pixels);
	CHECK_FALSE(action.free_upload_pixels);
	CHECK(action.release_upload_cpu_pin);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 10, 0));
	tile_cache_store_gpu_texture(&fixture.image, 0, 0, 42);
	tile_cache_release_upload_cpu_pin(&fixture.image, 0, 0);

	u8* cached_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0);
	REQUIRE(cached_pixels != nullptr);
	CHECK(cached_pixels[0] == 0x39);
	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("read-region consumption does not wedge a cached upload") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, pixels);
	REQUIRE(tile_cache_try_begin_upload(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 0));

	tile_cache_result_t result = {};
	result.level = 0;
	result.tile_index = 0;
	result.want_gpu_residency = true;
	result.upload_from_cached_pixels = true;
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(&fixture.image, &result, false, &action));
	CHECK_FALSE(action.should_upload);
	CHECK(action.pixels_to_free == nullptr);

	CHECK(tile_cache_try_begin_upload(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 0));
	tile_cache_cancel_upload(&fixture.image, 0, 0);
	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("generation completion and read pin race preserves a valid cache state") {
	tile_cache_test_ensure_thread_memory();
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 2;
	thread_pool_t pool = {};
	init_thread_pool(&pool, 8, false, false, NULL);

	for (i32 iteration = 0; iteration < 64; ++iteration) {
		tile_cache_test_image_t fixture = {};
		tile_cache_test_image_init(&fixture);
		REQUIRE(fixture.image.tile_cache != nullptr);
		REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
		                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));

		u8* pixels = (u8*)malloc(4);
		REQUIRE(pixels != nullptr);
		pixels[0] = (u8)iteration;
		tile_cache_apply_task_t apply_task = {};
		apply_task.image = &fixture.image;
		apply_task.result.level = 0;
		apply_task.result.tile_index = 0;
		apply_task.result.pixel_memory = pixels;
		apply_task.result.want_cpu_residency = true;
		tile_cache_pin_task_t pin_task = {&fixture.image};
		task_group_t group = {};

		REQUIRE(thread_pool_submit_task_to_group(&pool, &group, tile_cache_apply_result_task,
		                                         &apply_task, sizeof(apply_task)));
		REQUIRE(thread_pool_submit_task_to_group(&pool, &group, tile_cache_pin_read_task,
		                                         &pin_task, sizeof(pin_task)));
		thread_pool_wait_for_group(&pool, &group);

		if (tile_cache_tile_has_cpu_pixels(&fixture.image, 0, 0)) {
			u8* cached_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0);
			REQUIRE(cached_pixels != nullptr);
			CHECK(cached_pixels[0] == (u8)iteration);
			CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 10, 0));
		} else {
			REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
			                                    TILE_CACHE_DEMAND_CPU_RESIDENCY, 10, 0));
			tile_cache_cancel_decode(&fixture.image, 0, 0);
		}
		tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
		tile_cache_test_image_destroy(&fixture);
	}

	thread_pool_destroy(&pool);
	global_system_info = old_system_info;
}

TEST_CASE("image_read_region joins a viewer tile already in flight") {
	tile_cache_test_ensure_global_thread_pool();
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);
	fixture.image.backend = IMAGE_BACKEND_STBI;
	fixture.image.lock_initialized = true;
	platform_mutex_init(&fixture.image.lock);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE |
	                                    TILE_CACHE_DEMAND_GPU_RESIDENCY,
	                                    10, 1));

	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 2;
	thread_pool_t read_pool = {};
	init_thread_pool(&read_pool, 8, false, false, NULL);
	task_group_t read_group = {};
	tile_cache_read_region_state_t read_state = {&fixture.image};
	tile_cache_read_region_task_t read_task = {&read_state};
	REQUIRE(thread_pool_submit_task_to_group(&read_pool, &read_group,
	                                         tile_cache_read_region_task,
	                                         &read_task, sizeof(read_task)));

	bool read_pin_acquired = false;
	for (i32 attempt = 0; attempt < 1000; ++attempt) {
		if (tile_cache_tile_is_cpu_pinned(&fixture.image, 0, 0)) {
			read_pin_acquired = true;
			break;
		}
		platform_sleep(1);
	}

	u32 expected_pixel = 0xff3366aa;
	u32* decoded_pixel = (u32*)malloc(sizeof(*decoded_pixel));
	REQUIRE(decoded_pixel != nullptr);
	*decoded_pixel = expected_pixel;
	tile_cache_result_t viewer_result = {};
	viewer_result.pixel_memory = (u8*)decoded_pixel;
	viewer_result.level = 0;
	viewer_result.tile_index = 0;
	viewer_result.tile_width = 1;
	viewer_result.tile_height = 1;
	viewer_result.want_gpu_residency = true;
	REQUIRE(tile_cache_post_load_result(&fixture.image, &viewer_result));

	thread_pool_wait_for_group(&read_pool, &read_group);
	CHECK(read_pin_acquired);
	CHECK(read_state.succeeded);
	CHECK(read_state.output_pixel == expected_pixel);
	CHECK_FALSE(tile_cache_tile_is_busy(&fixture.image, 0, 0));

	thread_pool_destroy(&read_pool);
	global_system_info = old_system_info;
	platform_mutex_destroy(&fixture.image.lock);
	fixture.image.lock_initialized = false;
	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("tile cache shutdown rejects new work and drains queued pixels") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);
	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));

	u8* queued_pixels = (u8*)malloc(4);
	REQUIRE(queued_pixels != nullptr);
	tile_cache_result_t queued_result = {};
	queued_result.pixel_memory = queued_pixels;
	queued_result.level = 0;
	queued_result.tile_index = 0;
	REQUIRE(tile_cache_post_load_result(&fixture.image, &queued_result));

	tile_cache_begin_shutdown(&fixture.image);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 10, 0));
	CHECK_FALSE(tile_cache_begin_read(&fixture.image));
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_tile_is_cpu_pinned(&fixture.image, 0, 0));

	tile_cache_result_t polled_result = {};
	REQUIRE(tile_cache_poll_load_result(&fixture.image, &polled_result));
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(&fixture.image, &polled_result, false, &action));
	CHECK(action.pixels_to_free == queued_pixels);
	free(action.pixels_to_free);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("active read can finish after cache shutdown begins") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);
	REQUIRE(tile_cache_begin_read(&fixture.image));

	tile_cache_begin_shutdown(&fixture.image);
	CHECK(tile_cache_get_active_read_count(&fixture.image) == 1);
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                  TILE_CACHE_DEMAND_READ_REGION, 10, 0));
	tile_cache_cancel_decode(&fixture.image, 0, 0);
	tile_cache_end_read(&fixture.image);

	CHECK(tile_cache_get_active_read_count(&fixture.image) == 0);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0,
	                                        TILE_CACHE_DEMAND_READ_REGION, 10, 0));
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
	tile_cache_test_ensure_thread_memory();
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	for (i32 i = 0; i < result_count; ++i) {
		tile_cache_result_t result = {};
		result.resource_id = i;
		REQUIRE(tile_cache_post_load_result(&fixture.image, &result));
	}

	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 4;
	thread_pool_t pool = {};
	init_thread_pool(&pool, 16, false, false, NULL);

	i32 volatile seen[result_count] = {};
	i32 volatile consumed = 0;
	tile_cache_consume_task_t task = {&fixture.image, seen, &consumed, result_count};
	for (i32 consumer_index = 0; consumer_index < 4; ++consumer_index) {
		REQUIRE(thread_pool_submit_task(&pool, tile_cache_consume_results_task,
		                                &task, sizeof(task)));
	}
	thread_pool_wait_for_completion(&pool);

	CHECK(consumed == result_count);
	for (i32 i = 0; i < result_count; ++i) {
		CHECK(seen[i] == 1);
	}

	tile_cache_result_t result = {};
	CHECK_FALSE(tile_cache_poll_load_result(&fixture.image, &result));
	thread_pool_destroy(&pool);
	global_system_info = old_system_info;
	tile_cache_test_image_destroy(&fixture);
}
