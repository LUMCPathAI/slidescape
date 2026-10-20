#include "doctest.h"
#include "profiler.h"
#include "test_temp.h"
#include <string>

TEST_CASE("profiler CSV streams frames while the UI is paused and can restart") {
	char directory[512];
	REQUIRE(slidescape_test_make_temp_directory(directory, sizeof(directory), "slidescape-profile"));
	char path[1024];
	snprintf(path, sizeof(path), "%s/profile.csv", directory);
	for (i32 run = 0; run < 2; ++run) {
		REQUIRE(profiler_start_capture(path));
		CHECK_FALSE(profiler_start_capture(path));
		CHECK(profiler_memory_sample_due());
		CHECK_FALSE(profiler_memory_sample_due());
		global_profiler.paused = true;
		profiler_metrics_t metrics = {};
		metrics.resident_bytes = 123456;
		metrics.replay_segment = 2;
		profiler_set_metrics(&metrics);
		for (i32 frame = 0; frame < 2; ++frame) {
			profiler_new_frame();
			profiler_begin(PROFILER_SECTION_FRAME);
			profiler_begin(PROFILER_SECTION_VIEWER);
			profiler_end(PROFILER_SECTION_VIEWER);
			profiler_end(PROFILER_SECTION_FRAME);
		}
		CHECK(profiler_stop_capture());
		CHECK_FALSE(profiler_is_capturing());
		global_profiler.paused = false;
		FILE* file = fopen(path, "rb");
		REQUIRE(file);
		char line[8192];
		REQUIRE(fgets(line, sizeof(line), file));
		CHECK(std::string(line).find("resident_bytes") != std::string::npos);
		CHECK(std::string(line).find("Viewer_ms") != std::string::npos);
		REQUIRE(fgets(line, sizeof(line), file));
		CHECK(std::string(line).find(",0,") != std::string::npos);
		CHECK(std::string(line).find(",123456,") != std::string::npos);
		REQUIRE(fgets(line, sizeof(line), file));
		CHECK_FALSE(fgets(line, sizeof(line), file));
		fclose(file);
	}
	remove(path);
#if WINDOWS
	_rmdir(directory);
#else
	rmdir(directory);
#endif
}

#include "timerutils.h"
#include "tile_cache.h"
#include <map>
#include <thread>
#include <vector>

static std::vector<std::string> profiler_test_fields(char* line) {
	std::vector<std::string> fields;
	char* start = line;
	for (char* p = line;; ++p) {
		if (*p == ',' || *p == '\n' || *p == '\0') {
			fields.emplace_back(start, p - start);
			if (*p != ',') {
				break;
			}
			start = p + 1;
		}
	}
	return fields;
}

static std::map<std::string, std::string> profiler_test_read_row(const char* path) {
	FILE* file = fopen(path, "rb");
	REQUIRE(file);
	char line[8192];
	REQUIRE(fgets(line, sizeof(line), file));
	auto names = profiler_test_fields(line);
	REQUIRE(fgets(line, sizeof(line), file));
	auto values = profiler_test_fields(line);
	fclose(file);
	REQUIRE(names.size() == values.size());
	std::map<std::string, std::string> result;
	for (size_t i = 0; i < names.size(); ++i) {
		result[names[i]] = values[i];
	}
	return result;
}

TEST_CASE("profiler accumulates repeated sections and concurrent workers without nested double counting") {
	char directory[512];
	REQUIRE(slidescape_test_make_temp_directory(directory, sizeof(directory), "slidescape-profile"));
	std::string path = std::string(directory) + "/workers.csv";
	REQUIRE(profiler_start_capture(path.c_str()));
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	float section_sum = 0;
	for (int i = 0; i < 4; ++i) {
		profiler_begin(PROFILER_SECTION_TEXTURE_UPLOAD);
		profiler_end(PROFILER_SECTION_TEXTURE_UPLOAD);
		auto* section = &global_profiler.frames[global_profiler.current_frame].sections[PROFILER_SECTION_TEXTURE_UPLOAD];
		section_sum += get_seconds_elapsed(section->start, section->end) * 1000.0f;
		CHECK(section->elapsed_ms == doctest::Approx(section_sum));
	}
	u64 capture_id = profiler_capture_id();
	std::vector<std::thread> workers;
	for (int i = 0; i < 4; ++i) {
		workers.emplace_back([capture_id]() {
			for (int job = 0; job < 32; ++job) {
				profiler_worker_begin(capture_id, get_clock());
				profiler_worker_begin(capture_id, 0);
				i64 io_start = profiler_worker_io_begin();
				i64 nested_io_start = profiler_worker_io_begin();
				profiler_worker_io_end(nested_io_start, 1000);
				profiler_worker_io_end(io_start, 7);
				i64 decode_start = profiler_worker_decode_begin();
				i64 nested_decode_start = profiler_worker_decode_begin();
				profiler_worker_decode_end(nested_decode_start);
				profiler_worker_decode_end(decode_start);
				profiler_worker_end();
				profiler_worker_end();
			}
		});
	}
	for (auto& worker : workers) {
		worker.join();
	}
	profiler_begin(PROFILER_SECTION_PRESENT);
	profiler_end(PROFILER_SECTION_PRESENT);
	profiler_end(PROFILER_SECTION_FRAME);
	REQUIRE(profiler_stop_capture());
	auto row = profiler_test_read_row(path.c_str());
	CHECK(row["worker_jobs"] == "128");
	CHECK(row["worker_queued_jobs"] == "128");
	CHECK(row["worker_io_bytes"] == "896");
	CHECK(std::stof(row["worker_load_sum_ms"]) >= std::stof(row["worker_decode_sum_ms"]));
	CHECK(std::stof(row["pre_swap_ms"]) <= std::stof(row["frame_ms"]));

	// A job from the previous capture must not leak into a restarted capture.
	REQUIRE(profiler_start_capture(path.c_str()));
	profiler_worker_begin(profiler_capture_id(), 0);
	REQUIRE(profiler_stop_capture());
	REQUIRE(profiler_start_capture(path.c_str()));
	profiler_worker_end();
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_end(PROFILER_SECTION_FRAME);
	REQUIRE(profiler_stop_capture());
	row = profiler_test_read_row(path.c_str());
	CHECK(row["worker_jobs"] == "0");
	CHECK(row["pre_swap_ms"].empty());
	remove(path.c_str());
#if WINDOWS
	_rmdir(directory);
#else
	rmdir(directory);
#endif
}

TEST_CASE("profiler distinguishes requested-plane readiness from fallback and counts first draw once") {
	char directory[512];
	REQUIRE(slidescape_test_make_temp_directory(directory, sizeof(directory), "slidescape-profile"));
	std::string path = std::string(directory) + "/tiles.csv";
	image_t image = {};
	tile_t geometry = {};
	image.type = IMAGE_TYPE_WSI;
	image.backend = IMAGE_BACKEND_MRXS;
	image.level_count = 1;
	image.plane_count = 3;
	image.selected_plane = 2;
	mrxs_plane_t planes[3] = {};
	planes[0].is_extended_focus = true;
	image.mrxs.planes = planes;
	image.mrxs.plane_count = 3;
	image.level_images[0].exists = true;
	image.level_images[0].width_in_tiles = 1;
	image.level_images[0].height_in_tiles = 1;
	image.level_images[0].tile_count = 1;
	image.level_images[0].indexed_planes = UINT64_MAX;
	image.level_images[0].tiles = &geometry;
	image.tile_cache = tile_cache_create(&image);
	REQUIRE(image.tile_cache);
	REQUIRE(profiler_start_capture(path.c_str()));
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 0);
	tile_cache_store_gpu_texture(&image, 1, 0, 0, 101);
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 101);
	REQUIRE(tile_cache_try_begin_decode(&image, 2, 0, 0, TILE_CACHE_DEMAND_GPU_RESIDENCY, 1, 0));
	tile_cache_store_gpu_texture(&image, 2, 0, 0, 102);
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 102);
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 102);
	CHECK(tile_cache_take_gpu_texture(&image, 2, 0, 0) == 102);
	tile_cache_result_t failure = {};
	failure.plane = 2;
	failure.failed = true;
	tile_cache_result_action_t action = {};
	REQUIRE(tile_cache_apply_result(&image, &failure, true, &action));
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 0);
	geometry.is_empty = true;
	CHECK(tile_cache_get_focus_texture(&image, 0, 0, true) == 0);
	profiler_end(PROFILER_SECTION_FRAME);
	REQUIRE(profiler_stop_capture());
	auto row = profiler_test_read_row(path.c_str());
	CHECK(row["visible_missing"] == "1");
	CHECK(row["visible_adjacent_plane"] == "1");
	CHECK(row["visible_ready"] == "2");
	CHECK(row["visible_empty"] == "1");
	CHECK(row["visible_failed"] == "1");
	CHECK(row["first_draws"] == "1");
	CHECK(row["ready_tiles"] == "1");
	CHECK(std::stof(row["request_to_ready_sum_ms"]) <= std::stof(row["request_to_draw_sum_ms"]));
	tile_cache_destroy(image.tile_cache);
	remove(path.c_str());
#if WINDOWS
	_rmdir(directory);
#else
	rmdir(directory);
#endif
}

TEST_CASE("profiler live collection snapshots workers and tiles without a CSV") {
	global_profiler.paused = false;
	profiler_set_live_enabled(true);
	CHECK(profiler_is_collecting());
	CHECK_FALSE(profiler_is_capturing());
	CHECK(profiler_memory_sample_due());
	u64 live_id = profiler_capture_id();
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_metrics_t metrics = {};
	metrics.resident_bytes = 987654;
	profiler_set_metrics(&metrics);
	profiler_record_visible_tile(PROFILER_TILE_READY);
	profiler_record_visible_tile(PROFILER_TILE_MISSING);
	profiler_record_tile_ready(live_id, get_clock());
	profiler_worker_begin(live_id, get_clock());
	i64 io_start = profiler_worker_io_begin();
	profiler_worker_io_end(io_start, 42);
	profiler_worker_end();
	profiler_end(PROFILER_SECTION_FRAME);
	profiler_frame_t saved = global_profiler.frames[global_profiler.current_frame];
	CHECK(saved.metrics_valid);
	CHECK(saved.memory_sample_clock > 0);
	CHECK(saved.metrics.resident_bytes == 987654);
	CHECK(saved.tiles.visible[PROFILER_TILE_READY] == 1);
	CHECK(saved.tiles.visible[PROFILER_TILE_MISSING] == 1);
	CHECK(saved.tiles.ready_tiles == 1);
	CHECK(saved.workers.jobs == 1);
	CHECK(saved.workers.io_bytes == 42);
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_end(PROFILER_SECTION_FRAME);
	const profiler_frame_t* next = &global_profiler.frames[global_profiler.current_frame];
	CHECK(next->metrics_valid);
	CHECK(next->workers.jobs == 0);
	CHECK(next->tiles.ready_tiles == 0);
	CHECK(next->tiles.visible[PROFILER_TILE_READY] == 0);
	CHECK(saved.workers.jobs == 1);

	// Work from an earlier live session must not appear after closing/reopening.
	profiler_worker_begin(live_id, 0);
	profiler_set_live_enabled(false);
	CHECK_FALSE(profiler_is_collecting());
	CHECK_FALSE(profiler_memory_sample_due());
	profiler_set_live_enabled(true);
	CHECK(profiler_capture_id() != live_id);
	profiler_worker_end();
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_end(PROFILER_SECTION_FRAME);
	CHECK(global_profiler.frames[global_profiler.current_frame].workers.jobs == 0);
	profiler_set_live_enabled(false);
}

TEST_CASE("profiler live window and CSV share one snapshot without draining each other") {
	char directory[512];
	REQUIRE(slidescape_test_make_temp_directory(directory, sizeof(directory), "slidescape-profile"));
	std::string path = std::string(directory) + "/live.csv";
	profiler_set_live_enabled(true);
	REQUIRE(profiler_start_capture(path.c_str()));
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_worker_begin(profiler_capture_id(), 0);
	profiler_worker_end();
	profiler_record_visible_tile(PROFILER_TILE_ADJACENT_PLANE);
	profiler_end(PROFILER_SECTION_FRAME);
	profiler_frame_t snapshot = global_profiler.frames[global_profiler.current_frame];
	CHECK(snapshot.workers.jobs == 1);
	CHECK(snapshot.tiles.visible[PROFILER_TILE_ADJACENT_PLANE] == 1);
	profiler_set_live_enabled(false);
	CHECK(profiler_is_collecting()); // Closing the window must not stop CSV collection.
	global_profiler.paused = true;
	profiler_new_frame();
	profiler_begin(PROFILER_SECTION_FRAME);
	profiler_end(PROFILER_SECTION_FRAME);
	CHECK(global_profiler.frames[global_profiler.current_frame].workers.jobs == 0);
	CHECK(snapshot.workers.jobs == 1);
	profiler_set_live_enabled(true);
	REQUIRE(profiler_stop_capture());
	CHECK(profiler_is_collecting()); // Stopping CSV must not stop an open live window.
	auto row = profiler_test_read_row(path.c_str());
	CHECK(row["worker_jobs"] == "1");
	CHECK(row["visible_adjacent_plane"] == "1");
	profiler_set_live_enabled(false);
	global_profiler.paused = false;
	remove(path.c_str());
#if WINDOWS
	_rmdir(directory);
#else
	rmdir(directory);
#endif
}
