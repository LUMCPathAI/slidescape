#include "doctest.h"

#include "common.h"
#include "intrinsics.h"
#include "tile_cache.h"
#include "tile_loader.h"
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
	if (tile_cache_try_begin_decode(task->image, 0, 0, 0,
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
	tile_cache_pin_cpu_tile(task->image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
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
	state->succeeded = image_read_region(state->image, 0, 0, 0, 0, 1, 1,
	                                    &state->output_pixel, PIXEL_FORMAT_U8_BGRA);
}

TEST_CASE("tile cache suppresses duplicate decode requests") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                  TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                        TILE_CACHE_DEMAND_CPU_RESIDENCY, 20, 1));

	tile_cache_cancel_decode(&fixture.image, 0, 0, 0);
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                  TILE_CACHE_DEMAND_CPU_RESIDENCY, 20, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("GPU-resident tile can be decoded for a later CPU request") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	tile_cache_store_gpu_texture(&fixture.image, 0, 0, 0, 42);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                        TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                  TILE_CACHE_DEMAND_CPU_RESIDENCY, 10, 1));
	tile_cache_cancel_decode(&fixture.image, 0, 0, 0);

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

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 0, 1));

	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_task_is_stale(&fixture.image, 0, 0, 0, 1));
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                        TILE_CACHE_DEMAND_READ_REGION, 20, 0));

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 0, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("retryable tile failure releases its reservation without marking empty") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
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
	CHECK_FALSE(tile_cache_tile_is_busy(&fixture.image, 0, 0, 0));
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                        TILE_CACHE_DEMAND_VIEWER_VISIBLE |
	                                        TILE_CACHE_DEMAND_GPU_RESIDENCY,
	                                        10, 0));
	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("retryable tile failure cooldown grows exponentially and is capped") {
	CHECK(tile_cache_retry_delay_seconds(0) == doctest::Approx(0.0f));
	CHECK(tile_cache_retry_delay_seconds(1) == doctest::Approx(0.5f));
	CHECK(tile_cache_retry_delay_seconds(2) == doctest::Approx(1.0f));
	CHECK(tile_cache_retry_delay_seconds(6) == doctest::Approx(16.0f));
	CHECK(tile_cache_retry_delay_seconds(7) == doctest::Approx(30.0f));
	CHECK(tile_cache_retry_delay_seconds(100) == doctest::Approx(30.0f));
}

TEST_CASE("CPU tile pixels live until the final pin is released") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	pixels[0] = 0x5a;
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, 0, pixels);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	u8* pinned_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0);
	REQUIRE(pinned_pixels != nullptr);
	CHECK(pinned_pixels[0] == 0x5a);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK(tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0) == nullptr);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("hard demand bits follow overlapping pin types") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_EXPORT);

	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_task_is_stale(&fixture.image, 0, 0, 0, 1));
	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_EXPORT);
	CHECK(tile_cache_task_is_stale(&fixture.image, 0, 0, 0, 1));

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("cached upload reservation pins CPU pixels") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	pixels[0] = 0x7c;
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, 0, pixels);

	REQUIRE(tile_cache_try_begin_upload(&fixture.image, 0, 0, 0,
	                                    TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 1));
	u8* upload_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0);
	REQUIRE(upload_pixels != nullptr);
	CHECK(upload_pixels[0] == 0x7c);

	tile_cache_release_upload_cpu_pin(&fixture.image, 0, 0, 0);
	CHECK(tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0) == nullptr);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("decode completion atomically adopts pixels before releasing the reservation") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
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
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 0, 10, 0));
	tile_cache_store_gpu_texture(&fixture.image, 0, 0, 0, 42);
	tile_cache_release_upload_cpu_pin(&fixture.image, 0, 0, 0);

	u8* cached_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0);
	REQUIRE(cached_pixels != nullptr);
	CHECK(cached_pixels[0] == 0x39);
	tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);

	tile_cache_test_image_destroy(&fixture);
}

TEST_CASE("read-region consumption does not wedge a cached upload") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_image_init(&fixture);
	REQUIRE(fixture.image.tile_cache != nullptr);

	u8* pixels = (u8*)malloc(4);
	REQUIRE(pixels != nullptr);
	tile_cache_store_cpu_pixels(&fixture.image, 0, 0, 0, pixels);
	REQUIRE(tile_cache_try_begin_upload(&fixture.image, 0, 0, 0,
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

	CHECK(tile_cache_try_begin_upload(&fixture.image, 0, 0, 0,
	                                  TILE_CACHE_DEMAND_GPU_RESIDENCY, 10, 0));
	tile_cache_cancel_upload(&fixture.image, 0, 0, 0);
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
		REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
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

		if (tile_cache_tile_has_cpu_pixels(&fixture.image, 0, 0, 0)) {
			u8* cached_pixels = tile_cache_get_cpu_pixels(&fixture.image, 0, 0, 0);
			REQUIRE(cached_pixels != nullptr);
			CHECK(cached_pixels[0] == (u8)iteration);
			CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 0, 10, 0));
		} else {
			REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
			                                    TILE_CACHE_DEMAND_CPU_RESIDENCY, 10, 0));
			tile_cache_cancel_decode(&fixture.image, 0, 0, 0);
		}
		tile_cache_unpin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
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

	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
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
		if (tile_cache_tile_is_cpu_pinned(&fixture.image, 0, 0, 0)) {
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
	CHECK_FALSE(tile_cache_tile_is_busy(&fixture.image, 0, 0, 0));

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
	REQUIRE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                    TILE_CACHE_DEMAND_VIEWER_VISIBLE, 10, 1));

	u8* queued_pixels = (u8*)malloc(4);
	REQUIRE(queued_pixels != nullptr);
	tile_cache_result_t queued_result = {};
	queued_result.pixel_memory = queued_pixels;
	queued_result.level = 0;
	queued_result.tile_index = 0;
	REQUIRE(tile_cache_post_load_result(&fixture.image, &queued_result));

	tile_cache_begin_shutdown(&fixture.image);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, 0, 10, 0));
	CHECK_FALSE(tile_cache_begin_read(&fixture.image));
	tile_cache_pin_cpu_tile(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK_FALSE(tile_cache_tile_is_cpu_pinned(&fixture.image, 0, 0, 0));

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
	CHECK(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
	                                  TILE_CACHE_DEMAND_READ_REGION, 10, 0));
	tile_cache_cancel_decode(&fixture.image, 0, 0, 0);
	tile_cache_end_read(&fixture.image);

	CHECK(tile_cache_get_active_read_count(&fixture.image) == 0);
	CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0,
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

static void tile_cache_test_stack_init(tile_cache_test_image_t* fixture) {
	tile_cache_test_image_init(fixture);
	tile_cache_destroy(fixture->image.tile_cache);
	image_t* image = &fixture->image;
	image->backend = IMAGE_BACKEND_MRXS;
	image->level_images[0].indexed_planes = UINT64_MAX;
	image->plane_count = image->mrxs.plane_count = 4;
	image->mrxs.level_count = 1;
	image->mrxs.levels[0].width_in_tiles = image->mrxs.levels[0].height_in_tiles = 1;
	image->mrxs.planes = (mrxs_plane_t*)calloc(4, sizeof(mrxs_plane_t));
	for (int p = 0; p < 4; ++p) {
		mrxs_plane_t* plane = image->mrxs.planes + p;
		plane->offset_um = p == 0 ? 0 : (p-2)*0.5f;
		plane->offset_known = true;
		plane->is_extended_focus = p == 0;
		plane->level_available[0] = true;
		plane->tiles[0] = (mrxs_tile_t*)calloc(1, sizeof(mrxs_tile_t));
		plane->tiles[0]->hier_entry.length = 1;
	}
	image->zstack_plane = image->mrxs.zero_plane = 2;
	image->plane_approach_direction = -1;
	image->tile_cache = tile_cache_create(image);
}

static void tile_cache_test_stack_destroy(tile_cache_test_image_t* fixture) {
	for (int p = 0; p < 4; ++p) free(fixture->image.mrxs.planes[p].tiles[0]);
	free(fixture->image.mrxs.planes);
	tile_cache_test_image_destroy(fixture);
}

TEST_CASE("z-stack fallback uses nearest physical plane and arrival direction on ties") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_stack_init(&fixture);
	image_t* image = &fixture.image;
	tile_cache_store_gpu_texture(image, 0, 0, 0, 100);
	tile_cache_store_gpu_texture(image, 1, 0, 0, 101);
	tile_cache_store_gpu_texture(image, 3, 0, 0, 103);
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 100);
	image_select_plane(image, 1); image_select_plane(image, 2);
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 101);
	image_select_plane(image, 3); image_select_plane(image, 2);
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 103);
	image->mrxs.planes[1].offset_um = -0.1f; // Distance wins over the arrival side.
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 101);
	tile_cache_store_gpu_texture(image, 2, 0, 0, 102);
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 102);
	CHECK(tile_cache_take_gpu_texture(image, 2, 0, 0) == 102);
	image->mrxs.planes[2].tiles[0]->hier_entry.length = 0;
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 101); // Missing plane data is discovered on load.
	tile_cache_result_t missing = {}; missing.plane = 2; missing.failed = true;
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(image, &missing, false, &action));
	CHECK_FALSE(fixture.geometry.is_empty); // The shared coverage must not change.
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 0); // A completed failure is not a loading gap.
	tile_cache_take_gpu_texture(image, 1, 0, 0);
	tile_cache_take_gpu_texture(image, 3, 0, 0);
	image_select_plane(image, 1); // An unfailed physical plane with no cached texture.
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 0); // Never use the composite as a physical plane.
	image_select_plane(image, 0);
	CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 100);
	tile_cache_test_stack_destroy(&fixture);
}

TEST_CASE("late z-stack results uploads failures and pins stay on their originating plane") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_stack_init(&fixture);
	image_t* image = &fixture.image;
	for (int p = 1; p <= 2; ++p) {
		tile_cache_pin_cpu_tile(image, p, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
		CHECK(tile_cache_try_begin_decode(image, p, 0, 0, TILE_CACHE_DEMAND_CPU_RESIDENCY, 0, 0));
	}
	image_select_plane(image, 2);
	tile_cache_result_t result = {};
	result.plane = 1; result.tile_width = result.tile_height = 1;
	result.pixel_memory = (u8*)malloc(4); memset(result.pixel_memory, 11, 4);
	result.want_cpu_residency = result.want_gpu_residency = true;
	REQUIRE(tile_cache_post_load_result(image, &result));
	REQUIRE(tile_cache_poll_load_result(image, &result));
	CHECK(result.plane == 1);
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(image, &result, true, &action));
	CHECK(action.should_upload);
	CHECK(tile_cache_get_cpu_pixels(image, 1, 0, 0)[0] == 11);
	CHECK(tile_cache_get_cpu_pixels(image, 2, 0, 0) == nullptr);
	CHECK(tile_cache_tile_is_busy(image, 2, 0, 0));
	// Mimic a deferred GPU transfer finishing after the selection changed.
	tile_cache_store_gpu_texture(image, result.plane, 0, 0, 101);
	tile_cache_release_upload_cpu_pin(image, result.plane, 0, 0);
	CHECK(tile_cache_get_gpu_texture(image, 2, 0, 0) == 0);
	CHECK(tile_cache_get_gpu_texture(image, 1, 0, 0) == 101);
	tile_cache_unpin_cpu_tile(image, 1, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	CHECK(tile_cache_tile_is_cpu_pinned(image, 2, 0, 0));
	result = {}; result.plane = 2; result.failed = true;
	REQUIRE(tile_cache_apply_result(image, &result, false, &action));
	CHECK((tile_cache_get_tile_status(image, 2, 0, 0) & TILE_CACHE_STATUS_FAILED));
	CHECK_FALSE((tile_cache_get_tile_status(image, 1, 0, 0) & TILE_CACHE_STATUS_FAILED));
	tile_cache_unpin_cpu_tile(image, 2, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	tile_cache_test_stack_destroy(&fixture);
}

static u32 focus_destroyed_textures;
static void focus_test_destroy_texture(renderer_texture_handle_t texture) {
	CHECK(texture >= 100);
	focus_destroyed_textures |= 1u << (texture - 100);
}

TEST_CASE("z-stack stale completion cannot cancel a newer plane and teardown releases every plane") {
	tile_cache_test_ensure_global_thread_pool();
	tile_cache_test_image_t fixture = {};
	tile_cache_test_stack_init(&fixture);
	image_t* image = &fixture.image;
	image->level_images[0].x_tile_side_in_um = image->level_images[0].y_tile_side_in_um = 1;
	tile_cache_viewer_request_t request = {};
	request.plane = 1; request.client_width = request.client_height = 1;
	request.camera_bounds = BOUNDS2F(10, 10, 11, 11);
	CHECK(tile_cache_request_viewer_tiles(image, &request) == 0); // Empty viewport; generation one.
	REQUIRE(tile_cache_try_begin_decode(image, 1, 0, 0, TILE_CACHE_DEMAND_GPU_RESIDENCY, 0, 1));
	request.plane = 2;
	CHECK(tile_cache_request_viewer_tiles(image, &request) == 0);
	REQUIRE(tile_cache_try_begin_decode(image, 2, 0, 0, TILE_CACHE_DEMAND_GPU_RESIDENCY, 0, 2));
	CHECK(tile_cache_task_is_stale(image, 1, 0, 0, 1));
	CHECK_FALSE(tile_cache_task_is_stale(image, 2, 0, 0, 2));
	tile_cache_result_t result = {}; result.plane = 1; result.failed = true;
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(image, &result, false, &action));
	CHECK_FALSE(action.mark_empty);
	CHECK_FALSE(tile_cache_tile_is_busy(image, 1, 0, 0));
	CHECK(tile_cache_tile_is_busy(image, 2, 0, 0));
	tile_cache_cancel_decode(image, 2, 0, 0);
	for (int p = 0; p < 4; ++p) tile_cache_store_gpu_texture(image, p, 0, 0, 100+p);
	focus_destroyed_textures = 0;
	tile_cache_begin_shutdown(image);
	tile_cache_destroy_gpu_textures(image, focus_test_destroy_texture);
	CHECK(focus_destroyed_textures == 15);
	for (int p = 0; p < 4; ++p) CHECK(tile_cache_get_gpu_texture(image, p, 0, 0) == 0);
	CHECK_FALSE(tile_cache_try_begin_decode(image, 1, 0, 0, TILE_CACHE_DEMAND_GPU_RESIDENCY, 0, 0));
	tile_cache_test_stack_destroy(&fixture);
}

TEST_CASE("cache hash growth preserves keys pins and pending work for single and multiple planes") {
	for (int planes : {1, 4}) {
		tile_cache_test_image_t fixture = {};
		tile_cache_test_image_init(&fixture);
		image_t* image = &fixture.image;
		image->plane_count = planes;
		image->level_count = 2;
		image->level_images[0].tile_count = 5000;
		image->level_images[1] = image->level_images[0];
		tile_cache_pin_cpu_tile(image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
		u8* pixels = (u8*)malloc(4); memset(pixels, 42, 4);
		tile_cache_store_cpu_pixels(image, 0, 0, 0, pixels);
		REQUIRE(tile_cache_try_begin_decode(image, 0, 1, 0, TILE_CACHE_DEMAND_READ_REGION, 0, 0));
		for (int p = 0; p < planes; ++p) for (int l = 0; l < 2; ++l) {
			for (int i = 1; i < 5000; ++i)
				tile_cache_store_gpu_texture(image, p, l, i, 1 + p*10000 + l*5000 + i);
		}
		CHECK(tile_cache_get_cpu_pixels(image, 0, 0, 0) == pixels);
		CHECK(pixels[0] == 42);
		CHECK(tile_cache_tile_is_cpu_pinned(image, 0, 0, 0));
		CHECK(tile_cache_tile_is_busy(image, 0, 1, 0));
		for (int p = 0; p < planes; ++p) for (int l = 0; l < 2; ++l) {
			for (int i = 1; i < 5000; ++i)
				CHECK(tile_cache_take_gpu_texture(image, p, l, i) == (u32)(1 + p*10000 + l*5000 + i));
		}
		tile_cache_cancel_decode(image, 0, 1, 0);
		CHECK_FALSE(tile_cache_tile_is_busy(image, 0, 1, 0));
		tile_cache_unpin_cpu_tile(image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
		tile_cache_test_image_destroy(&fixture);
	}
}

TEST_CASE("explicit empty results and permanent failures have separate cache outcomes") {
	for (bool empty : {false, true}) {
		tile_cache_test_image_t fixture = {};
		tile_cache_test_image_init(&fixture);
		fixture.image.backend = IMAGE_BACKEND_OPENSLIDE; // Always single-plane.
		tile_cache_result_t result = {};
		result.is_empty = empty;
		result.failed = !empty;
		result.pixel_memory = (u8*)malloc(4); // Reject even an allocated error/blank buffer.
		u8* original_pixels = result.pixel_memory;
		tile_cache_result_action_t action = {};
		REQUIRE(tile_cache_apply_result(&fixture.image, &result, true, &action));
		CHECK(action.mark_empty == empty);
		CHECK_FALSE(action.should_upload);
		CHECK(action.pixels_to_free == original_pixels);
		free(action.pixels_to_free);
		CHECK(tile_cache_get_tile_status(&fixture.image, 0, 0, 0) ==
		      (u32)(empty ? TILE_CACHE_STATUS_EMPTY : TILE_CACHE_STATUS_FAILED));
		CHECK_FALSE(tile_cache_try_begin_decode(&fixture.image, 0, 0, 0, TILE_CACHE_DEMAND_READ_REGION, 0, 0));
		tile_cache_cancel_decode(&fixture.image, 0, 0, 0); // Cancellation must not erase a terminal outcome.
		CHECK(tile_cache_get_tile_status(&fixture.image, 0, 0, 0) ==
		      (u32)(empty ? TILE_CACHE_STATUS_EMPTY : TILE_CACHE_STATUS_FAILED));
		tile_cache_test_image_destroy(&fixture);
	}
}

TEST_CASE("empty reports cannot mark shared z-stack coverage and successful results clear failure") {
	tile_cache_test_image_t fixture = {};
	tile_cache_test_stack_init(&fixture);
	for (int plane : {0, 2}) {
		tile_cache_result_t result = {};
		result.plane = plane; result.is_empty = true;
		tile_cache_result_action_t action = {};
		REQUIRE(tile_cache_apply_result(&fixture.image, &result, false, &action));
		CHECK_FALSE(action.mark_empty);
		CHECK(tile_cache_get_tile_status(&fixture.image, plane, 0, 0) == TILE_CACHE_STATUS_FAILED);
		tile_cache_pin_cpu_tile(&fixture.image, plane, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
		result.is_empty = false;
		result.pixel_memory = (u8*)malloc(4); memset(result.pixel_memory, 8, 4);
		REQUIRE(tile_cache_apply_result(&fixture.image, &result, false, &action));
		CHECK(tile_cache_get_tile_status(&fixture.image, plane, 0, 0) == TILE_CACHE_STATUS_CPU_READY);
		tile_cache_unpin_cpu_tile(&fixture.image, plane, 0, 0, TILE_CACHE_DEMAND_READ_REGION);
	}
	tile_cache_test_stack_destroy(&fixture);
}

static void WINAPI cache_test_openslide_read(openslide_t*, u32* dest, i64, i64, i32, i64 w, i64 h) {
	memset(dest, 0, (size_t)w*h*sizeof(u32));
}

static const char* WINAPI cache_test_openslide_error(openslide_t* osr) {
	return (const char*)osr;
}

TEST_CASE("transparent OpenSlide reads are empty only when the decoder reports no error") {
	tile_cache_test_ensure_thread_memory();
	for (bool error : {false, true}) {
		tile_cache_test_image_t fixture = {};
		tile_cache_test_image_init(&fixture);
		fixture.image.backend = IMAGE_BACKEND_OPENSLIDE;
		fixture.image.plane_count = 1;
		fixture.image.level_images[0].x_tile_side_in_um = 1;
		fixture.image.level_images[0].y_tile_side_in_um = 1;
		fixture.image.openslide_wsi.osr = (openslide_t*)(error ? "synthetic read failure" : nullptr);
		load_tile_task_t task = {}; task.image = &fixture.image;
		openslide_api saved = openslide;
		openslide.read_region = cache_test_openslide_read;
		openslide.get_error = cache_test_openslide_error;
		load_tile_func(0, &task);
		openslide = saved;
		tile_cache_result_t result = {};
		REQUIRE(tile_cache_poll_load_result(&fixture.image, &result));
		CHECK(result.failed == error);
		CHECK(result.is_empty == !error);
		CHECK(result.pixel_memory == nullptr);
		tile_cache_result_action_t action = {};
		REQUIRE(tile_cache_apply_result(&fixture.image, &result, false, &action));
		CHECK(action.mark_empty == !error);
		tile_cache_test_image_destroy(&fixture);
	}
}
