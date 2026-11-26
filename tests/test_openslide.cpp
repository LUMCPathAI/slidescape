#include "common.h"
#include "doctest.h"
#include "image.h"

struct openslide_test_handle_t {
	i64 width[2] = {-1, -1};
	i64 height[2] = {-1, -1};
	i32 reads[2] = {};
	i32 fail_read = -1;
	bool fail_dimensions = false;
	bool no_names = false;
	const char* error = NULL;
	i32 closes = 0;
	i32 region_level = -1;
	i64 region_x = 0;
	i64 region_y = 0;
	i64 region_w = 0;
	i64 region_h = 0;
};

static i32 openslide_test_associated_index(const char* name) {
	CHECK((strcmp(name, "label") == 0 || strcmp(name, "macro") == 0));
	return strcmp(name, "label") == 0 ? 0 : 1;
}

static const char* const* WINAPI openslide_test_names(openslide_t* osr) {
	openslide_test_handle_t* handle = (openslide_test_handle_t*)osr;
	static const char* names[] = {"label", "macro", "thumbnail", NULL};
	return handle->no_names ? NULL : names;
}

static void WINAPI openslide_test_dimensions(openslide_t* osr, const char* name, i64* width, i64* height) {
	openslide_test_handle_t* handle = (openslide_test_handle_t*)osr;
	i32 index = openslide_test_associated_index(name);
	*width = handle->width[index];
	*height = handle->height[index];
	if (handle->fail_dimensions) {
		handle->error = "associated image dimensions failed";
	}
}

static void WINAPI openslide_test_read(openslide_t* osr, const char* name, u32* pixels) {
	openslide_test_handle_t* handle = (openslide_test_handle_t*)osr;
	i32 index = openslide_test_associated_index(name);
	++handle->reads[index];
	if (handle->fail_read == index) {
		handle->error = "associated image read failed";
		return;
	}
	const u32 argb[] = {0xFFFF0000, 0xFF0000FF, 0x80402010, 0};
	size_t pixel_count = (size_t)handle->width[index] * (size_t)handle->height[index];
	REQUIRE(pixel_count <= COUNT(argb));
	memcpy(pixels, argb, pixel_count * sizeof(u32));
}

static const char* WINAPI openslide_test_error(openslide_t* osr) {
	return ((openslide_test_handle_t*)osr)->error;
}

static void WINAPI openslide_test_close(openslide_t* osr) {
	++((openslide_test_handle_t*)osr)->closes;
}

static void WINAPI openslide_test_region(openslide_t* osr, u32* pixels, i64 x, i64 y, i32 level, i64 w, i64 h) {
	openslide_test_handle_t* handle = (openslide_test_handle_t*)osr;
	handle->region_level = level;
	handle->region_x = x;
	handle->region_y = y;
	handle->region_w = w;
	handle->region_h = h;
	for (i64 i = 0; i < w * h; ++i) {
		pixels[i] = 0xff112233;
	}
}

struct openslide_test_image_t {
	openslide_api saved_api = openslide;
	openslide_test_handle_t handle;
	image_t image = {};

	openslide_test_image_t() {
		openslide = {};
		openslide.get_associated_image_names = openslide_test_names;
		openslide.get_associated_image_dimensions = openslide_test_dimensions;
		openslide.read_associated_image = openslide_test_read;
		openslide.get_error = openslide_test_error;
		openslide.close = openslide_test_close;
		openslide.read_region = openslide_test_region;
		wsi_t* wsi = &image.openslide_wsi;
		wsi->osr = (openslide_t*)&handle;
		wsi->width = 512;
		wsi->height = 512;
		wsi->mpp_x = 0.5f;
		wsi->mpp_y = 0.5f;
		wsi->tile_width = 512;
		wsi->tile_height = 512;
		wsi->level_count = 1;
		wsi_level_t* level = wsi->levels;
		level->width = 512;
		level->height = 512;
		level->width_in_tiles = 1;
		level->height_in_tiles = 1;
		level->tile_width = 512;
		level->tile_height = 512;
		level->tile_count = 1;
		level->downsample_factor = 1.0f;
		level->um_per_pixel_x = 0.5f;
		level->um_per_pixel_y = 0.5f;
		level->x_tile_side_in_um = 256.0f;
		level->y_tile_side_in_um = 256.0f;
	}

	~openslide_test_image_t() {
		image_destroy(&image);
		openslide = saved_api;
	}
};

TEST_CASE_FIXTURE(openslide_test_image_t, "OpenSlide translates a region origin using the cached file downsample") {
	handle.no_names = true;
	wsi_t* wsi = &image.openslide_wsi;
	wsi->level_count = 2;
	wsi->max_downsample_level = 2;
	wsi->levels[1] = wsi->levels[0];
	wsi->levels[1].downsample_level = 2;
	constexpr double downsample = 3.25000001;
	wsi->levels[1].downsample_factor = downsample;
	init_image_from_openslide(&image, false);
	REQUIRE(image_initialize_runtime(&image));

	const i32 level = 2;
	CHECK(image.level_images[level].downsample_factor == downsample);
	u32 pixels[6] = {};
	REQUIRE(image_read_region(&image, 0, level, 7, -1, 3, 2, pixels, PIXEL_FORMAT_U8_BGRA));
	CHECK(handle.region_level == 1);
	CHECK(handle.region_x == 23);
	CHECK(handle.region_y == -4);
	CHECK(handle.region_w == 3);
	CHECK(handle.region_h == 2);
	CHECK(pixels[5] == 0xff112233);

	REQUIRE(image_read_region(&image, 0, level, 1500000001, 0, 1, 1, pixels, PIXEL_FORMAT_U8_BGRA));
	CHECK(handle.region_x == INT64_C(4875000018));
}

TEST_CASE_FIXTURE(openslide_test_image_t, "OpenSlide label and macro images use the viewer's RGBA buffers") {
	handle.width[0] = 4;
	handle.height[0] = 1;
	handle.width[1] = 2;
	handle.height[1] = 2;
	init_image_from_openslide(&image, false);
	REQUIRE(image.is_valid);
	const u8 expected[] = {255, 0, 0, 255, 0, 0, 255, 255, 128, 64, 32, 128, 0, 0, 0, 0};
	simple_image_t* associated_images[] = {&image.label_image, &image.macro_image};
	for (simple_image_t* associated : associated_images) {
		REQUIRE(associated->is_valid);
		REQUIRE(associated->pixels != NULL);
		CHECK(associated->channels == 4);
		CHECK(memcmp(associated->pixels, expected, sizeof(expected)) == 0);
	}
	CHECK(image.label_image.width == 4);
	CHECK(image.label_image.height == 1);
	CHECK(image.macro_image.width == 2);
	CHECK(image.macro_image.height == 2);
	CHECK(handle.reads[0] == 1);
	CHECK(handle.reads[1] == 1);
	image_destroy(&image);
	CHECK(handle.closes == 1);
	CHECK(image.label_image.pixels == NULL);
	CHECK(image.macro_image.pixels == NULL);
	CHECK_FALSE(image.label_image.is_valid);
	CHECK_FALSE(image.macro_image.is_valid);
}

TEST_CASE_FIXTURE(openslide_test_image_t, "OpenSlide associated images are independently optional") {
	SUBCASE("no associated image list") {
		handle.no_names = true;
	}
	SUBCASE("listed associated images are unavailable") {
	}
	SUBCASE("only the label is available") {
		handle.width[0] = 1;
		handle.height[0] = 1;
	}
	SUBCASE("only the macro is available") {
		handle.width[1] = 1;
		handle.height[1] = 1;
	}
	init_image_from_openslide(&image, false);
	CHECK(image.is_valid);
	CHECK(image.label_image.is_valid == (handle.width[0] > 0));
	CHECK(image.macro_image.is_valid == (handle.width[1] > 0));
	CHECK(handle.reads[0] == (handle.width[0] > 0));
	CHECK(handle.reads[1] == (handle.width[1] > 0));
}

TEST_CASE_FIXTURE(openslide_test_image_t, "OpenSlide rejects invalid associated image dimensions before reading") {
	handle.height[0] = 1;
	SUBCASE("zero width") {
		handle.width[0] = 0;
	}
	SUBCASE("width exceeds viewer dimensions") {
		handle.width[0] = (i64)INT32_MAX + 1;
	}
	SUBCASE("height exceeds viewer dimensions") {
		handle.width[0] = 1;
		handle.height[0] = (i64)INT32_MAX + 1;
	}
	init_image_from_openslide(&image, false);
	CHECK(image.is_valid);
	CHECK_FALSE(image.label_image.is_valid);
	CHECK(image.label_image.pixels == NULL);
	CHECK(handle.reads[0] == 0);
}

TEST_CASE_FIXTURE(openslide_test_image_t, "OpenSlide associated image errors discard failed buffers and stop loading") {
	handle.width[0] = handle.width[1] = 1;
	handle.height[0] = handle.height[1] = 1;
	SUBCASE("dimension lookup fails") {
		handle.fail_dimensions = true;
	}
	SUBCASE("label read fails") {
		handle.fail_read = 0;
	}
	SUBCASE("macro read fails after the label was loaded") {
		handle.fail_read = 1;
	}
	init_image_from_openslide(&image, false);
	CHECK_FALSE(image.is_valid);
	if (handle.fail_read == 1) {
		CHECK(image.label_image.is_valid);
		CHECK_FALSE(image.macro_image.is_valid);
		CHECK(image.macro_image.pixels == NULL);
	} else {
		CHECK_FALSE(image.label_image.is_valid);
		CHECK(image.label_image.pixels == NULL);
		CHECK(handle.reads[1] == 0);
	}
}
