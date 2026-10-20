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
