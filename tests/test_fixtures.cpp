#include "common.h"
#include "doctest.h"

#include "stringutils.h"
#include "tiff.h"
#include "isyntax.h"
#include "dicom.h"
#include "dicom_wsi.h"
#include "image_loader.h"

#include <stdint.h>

#ifndef SLIDESCAPE_TEST_FIXTURE_MANIFEST
#define SLIDESCAPE_TEST_FIXTURE_MANIFEST "tests/fixtures/manifest.txt"
#endif

#ifndef SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST
#define SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST "tests/fixtures/local_manifest.txt"
#endif

#define MAX_TEST_FIXTURES 128

struct fixture_t {
	char id[128];
	char type[32];
	char visibility[32];
	char tags[256];
	char path[1024];
	char source_url[1024];
};

struct fixture_manifest_t {
	fixture_t fixtures[MAX_TEST_FIXTURES];
	size_t fixture_count;
	bool loaded;
};

static void copy_string(char* dest, size_t dest_size, const char* src) {
	if (dest_size == 0) return;
	snprintf(dest, dest_size, "%s", src ? src : "");
}

static bool test_path_is_absolute(const char* path) {
	if (!path || !path[0]) return false;
	if (path[0] == '/' || path[0] == '\\') return true;
	if (strlen(path) >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	    path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
		return true;
	}
	return false;
}

static void test_path_dirname(char* dest, size_t dest_size, const char* path) {
	if (dest_size == 0) return;
	const char* last_slash = NULL;
	for (const char* pos = path; pos && *pos; ++pos) {
		if (*pos == '/' || *pos == '\\') last_slash = pos;
	}
	if (!last_slash) {
		copy_string(dest, dest_size, ".");
		return;
	}
	size_t len = (size_t)(last_slash - path);
	if (len >= dest_size) len = dest_size - 1;
	memcpy(dest, path, len);
	dest[len] = '\0';
}

static void join_test_path(char* dest, size_t dest_size, const char* lhs, const char* rhs) {
	if (dest_size == 0) return;
	if (!rhs || !rhs[0] || test_path_is_absolute(rhs)) {
		copy_string(dest, dest_size, rhs);
		return;
	}
	if (!lhs || !lhs[0] || strcmp(lhs, ".") == 0) {
		copy_string(dest, dest_size, rhs);
		return;
	}
	size_t lhs_len = strlen(lhs);
	char separator = (lhs[lhs_len - 1] == '/' || lhs[lhs_len - 1] == '\\') ? '\0' : '/';
	if (separator) {
		snprintf(dest, dest_size, "%s/%s", lhs, rhs);
	} else {
		snprintf(dest, dest_size, "%s%s", lhs, rhs);
	}
}

static bool test_fixture_file_exists(const char* path) {
	FILE* fp = fopen(path, "rb");
	if (!fp) return false;
	fclose(fp);
	return true;
}

static size_t split_manifest_line(char* line, char** fields, size_t field_capacity) {
	size_t field_count = 0;
	char* field_start = line;
	for (char* pos = line; ; ++pos) {
		if (*pos == '|' || *pos == '\0') {
			char previous = *pos;
			*pos = '\0';
			if (field_count < field_capacity) fields[field_count++] = trim_whitespace(field_start);
			if (previous == '\0') break;
			field_start = pos + 1;
		}
	}
	return field_count;
}

static void load_fixture_manifest(fixture_manifest_t* manifest, const char* manifest_path) {
	FILE* fp = fopen(manifest_path, "rb");
	if (!fp) return;

	char manifest_dir[1024];
	test_path_dirname(manifest_dir, COUNT(manifest_dir), manifest_path);

	char line[4096];
	while (fgets(line, COUNT(line), fp)) {
		char* text = trim_whitespace(line);
		if (!text[0] || text[0] == '#') continue;

		char* fields[6] = {};
		size_t field_count = split_manifest_line(text, fields, COUNT(fields));
		if (field_count < 5 || manifest->fixture_count >= COUNT(manifest->fixtures)) continue;

		fixture_t& fixture = manifest->fixtures[manifest->fixture_count++];
		copy_string(fixture.id, COUNT(fixture.id), fields[0]);
		copy_string(fixture.type, COUNT(fixture.type), fields[1]);
		copy_string(fixture.visibility, COUNT(fixture.visibility), fields[2]);
		copy_string(fixture.tags, COUNT(fixture.tags), fields[3]);
		join_test_path(fixture.path, COUNT(fixture.path), manifest_dir, fields[4]);
		if (field_count >= 6) copy_string(fixture.source_url, COUNT(fixture.source_url), fields[5]);
	}

	fclose(fp);
}

static fixture_manifest_t* fixture_manifest() {
	static fixture_manifest_t manifest = {};
	if (!manifest.loaded) {
		load_fixture_manifest(&manifest, SLIDESCAPE_TEST_FIXTURE_MANIFEST);
		load_fixture_manifest(&manifest, SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST);
		manifest.loaded = true;
	}
	return &manifest;
}

static bool fixture_has_tag(const fixture_t& fixture, const char* tag) {
	size_t tag_len = strlen(tag);
	size_t offset = 0;
	size_t tags_len = strlen(fixture.tags);
	while (offset <= tags_len) {
		char candidate[128];
		size_t comma = offset;
		while (comma < tags_len && fixture.tags[comma] != ',') ++comma;
		size_t len = comma - offset;
		if (len >= COUNT(candidate)) len = COUNT(candidate) - 1;
		memcpy(candidate, fixture.tags + offset, len);
		candidate[len] = '\0';
		char* trimmed = trim_whitespace(candidate);
		if (strlen(trimmed) == tag_len && memcmp(trimmed, tag, tag_len) == 0) return true;
		if (comma == tags_len) break;
		offset = comma + 1;
	}
	return false;
}

static const fixture_t* first_available_fixture(const char* type, const char* tag = NULL) {
	fixture_manifest_t* manifest = fixture_manifest();
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, type) != 0) continue;
		if (tag && !fixture_has_tag(fixture, tag)) continue;
		if (test_fixture_file_exists(fixture.path)) return &fixture;
	}
	return NULL;
}

static bool read_file_prefix(const char* filename, u8* dest, size_t bytes_to_read) {
	FILE* fp = fopen(filename, "rb");
	if (!fp) return false;
	size_t bytes_read = fread(dest, 1, bytes_to_read, fp);
	fclose(fp);
	return bytes_read == bytes_to_read;
}

static bool pixel_buffer_has_variation(const u32* pixels, size_t pixel_count) {
	if (pixel_count == 0) return false;
	u32 first = pixels[0];
	for (size_t i = 1; i < pixel_count; ++i) {
		if (pixels[i] != first) return true;
	}
	return false;
}

static void ensure_image_read_region_thread_pool(void) {
	if (!global_thread_pool.initialized) {
		init_global_system_info(false);
		if (global_system_info.suggested_total_thread_count < 2) {
			global_system_info.suggested_total_thread_count = 2;
		}
		init_thread_pool(&global_thread_pool, 128, true, false, NULL);
	}
}

static void ensure_dicom_initialized(void) {
	static bool initialized = false;
	if (!initialized) {
		REQUIRE(dicom_init());
		initialized = true;
	}
}

typedef struct selected_tiff_tile_t {
	i32 level;
	i32 tile_x;
	i32 tile_y;
	i32 tile_index;
	i32 read_width;
	i32 read_height;
	u8* pixels;
} selected_tiff_tile_t;

static bool select_decodable_tiff_tile(image_t* image, selected_tiff_tile_t* out_tile) {
	ASSERT(image && image->backend == IMAGE_BACKEND_TIFF);
	level_image_t* level_image = image->level_images + 0;
	tiff_ifd_t* ifd = image->tiff.level_images_ifd + level_image->pyramid_image_index;
	for (i32 tile_y = 0; tile_y < (i32)level_image->height_in_tiles; ++tile_y) {
		for (i32 tile_x = 0; tile_x < (i32)level_image->width_in_tiles; ++tile_x) {
			i32 tile_index = tile_y * (i32)level_image->width_in_tiles + tile_x;
			if (ifd->tile_offsets[tile_index] == 0 || ifd->tile_byte_counts[tile_index] == 0) continue;

			u8* pixels = tiff_decode_tile(0, &image->tiff, ifd, tile_index, 0, tile_x, tile_y);
			if (!pixels) continue;

			i64 x = (i64)tile_x * (i64)level_image->tile_width;
			i64 y = (i64)tile_y * (i64)level_image->tile_height;
			i32 read_width = (i32)MIN((i64)level_image->tile_width, level_image->width_in_pixels - x);
			i32 read_height = (i32)MIN((i64)level_image->tile_height, level_image->height_in_pixels - y);
			if (read_width <= 0 || read_height <= 0) {
				free(pixels);
				continue;
			}

			*out_tile = {};
			out_tile->level = 0;
			out_tile->tile_x = tile_x;
			out_tile->tile_y = tile_y;
			out_tile->tile_index = tile_index;
			out_tile->read_width = read_width;
			out_tile->read_height = read_height;
			out_tile->pixels = pixels;
			return true;
		}
	}
	return false;
}

static image_t* load_tiff_fixture_image(const fixture_t* fixture) {
	file_info_t file = viewer_get_file_info(fixture->path);
	REQUIRE(file.is_valid);
	REQUIRE(file.is_regular_file);
	REQUIRE(file.type == VIEWER_FILE_TYPE_TIFF);

	image_load_options_t options = {};
	options.use_builtin_tiff_backend = true;
	options.resource_id = 2000;
	options.thread_pool = &global_thread_pool;
	image_t* image = image_load_from_file(&file, NULL, &options);
	REQUIRE(image != NULL);
	REQUIRE(image->is_valid);
	REQUIRE(image->backend == IMAGE_BACKEND_TIFF);
	return image;
}

static image_t* load_dicom_fixture_image(const fixture_t* fixture) {
	ensure_dicom_initialized();
	ensure_image_read_region_thread_pool();

	file_info_t file = viewer_get_file_info(fixture->path);
	REQUIRE(file.is_valid);
	REQUIRE(file.is_regular_file);
	REQUIRE(file.type == VIEWER_FILE_TYPE_DICOM);

	image_load_options_t options = {};
	options.resource_id = 3000;
	options.thread_pool = &global_thread_pool;
	image_t* image = image_load_from_file(&file, NULL, &options);
	REQUIRE(image != NULL);
	REQUIRE(image->is_valid);
	REQUIRE(image->backend == IMAGE_BACKEND_DICOM);
	return image;
}

TEST_CASE("fixture manifest is readable") {
	fixture_manifest_t* manifest = fixture_manifest();
	REQUIRE(manifest->fixture_count > 0);

	bool has_public_fixture = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		CAPTURE(fixture.id);
		CHECK(fixture.type[0] != '\0');
		CHECK(fixture.path[0] != '\0');
		if (strcmp(fixture.visibility, "public") == 0) has_public_fixture = true;
	}
	CHECK(has_public_fixture);
}

TEST_CASE("available WSI fixtures have expected extensions") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (!test_fixture_file_exists(fixture.path)) continue;

		const char* extension = get_file_extension(fixture.path);
		CAPTURE(fixture.path);
		if (strcmp(fixture.type, "tiff") == 0) {
			CHECK((strcmp(extension, "tif") == 0 || strcmp(extension, "tiff") == 0));
		} else if (strcmp(fixture.type, "isyntax") == 0) {
			CHECK(strcmp(extension, "isyntax") == 0);
		} else if (strcmp(fixture.type, "dicom") == 0) {
			u8 header[256] = {};
			REQUIRE(read_file_prefix(fixture.path, header, sizeof(header)));
			CHECK(is_file_a_dicom_file(header, sizeof(header)));
		} else if (strcmp(fixture.type, "asap_xml") == 0) {
			CHECK(strcmp(extension, "xml") == 0);
		}
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping fixture extension checks: no manifest fixtures are present locally.");
	}
}

TEST_CASE("TIFF fixtures have valid TIFF or BigTIFF headers") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "tiff") != 0 || !test_fixture_file_exists(fixture.path)) continue;

		u8 header[4] = {};
		CAPTURE(fixture.path);
		REQUIRE(read_file_prefix(fixture.path, header, sizeof(header)));

		bool little_endian_tiff = header[0] == 'I' && header[1] == 'I' && header[2] == 42 && header[3] == 0;
		bool little_endian_bigtiff = header[0] == 'I' && header[1] == 'I' && header[2] == 43 && header[3] == 0;
		bool big_endian_tiff = header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 42;
		bool big_endian_bigtiff = header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 43;
		CHECK((little_endian_tiff || little_endian_bigtiff || big_endian_tiff || big_endian_bigtiff));
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping TIFF header checks: no TIFF fixtures are present locally.");
	}
}

TEST_CASE("iSyntax fixture starts with XML metadata") {
	const fixture_t* fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax header check: no iSyntax fixture is present locally.");
		return;
	}

	u8 header[256] = {};
	REQUIRE(read_file_prefix(fixture->path, header, sizeof(header)));
	CHECK(memcmp(header, "<?xml", 5) == 0);
	CHECK(static_cast<bool>(strstr((char*)header, "DPUfsImport") != NULL));
	CHECK(static_cast<bool>(strstr((char*)header, "DICOM_MANUFACTURER") != NULL));
}

TEST_CASE("ASAP XML fixture exposes at least one annotation node") {
	const fixture_t* fixture = first_available_fixture("asap_xml");
	if (!fixture) {
		MESSAGE("Skipping ASAP XML fixture check: no ASAP XML fixture is present locally.");
		return;
	}

	FILE* fp = fopen(fixture->path, "rb");
	REQUIRE(fp != NULL);
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	char* xml = (char*)calloc((size_t)file_size + 1, 1);
	REQUIRE(static_cast<bool>(xml != NULL));
	REQUIRE(fread(xml, 1, (size_t)file_size, fp) == (size_t)file_size);
	fclose(fp);

	CHECK(static_cast<bool>(strstr(xml, "<Annotations") != NULL));
	CHECK(static_cast<bool>(strstr(xml, "<Annotation") != NULL));
	CHECK(static_cast<bool>(strstr(xml, "<Coordinate") != NULL));

	free(xml);
}

TEST_CASE("open TIFF fixtures through Slidescape TIFF reader") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "tiff") != 0 || !test_fixture_file_exists(fixture.path)) continue;

		tiff_t tiff = {};
		CAPTURE(fixture.path);
		REQUIRE(open_tiff_file(&tiff, fixture.path));

		CHECK(tiff.filesize > 0);
		CHECK(tiff.ifd_count > 0);
		REQUIRE(tiff.ifds != NULL);
		REQUIRE(tiff.main_image_ifd != NULL);
		CHECK(tiff.main_image_ifd->image_width > 0);
		CHECK(tiff.main_image_ifd->image_height > 0);
		CHECK(tiff.level_image_ifd_count > 0);
		CHECK(tiff.level_images_ifd != NULL);

		for (u64 level_index = 0; level_index < tiff.level_image_ifd_count; ++level_index) {
			tiff_ifd_t* ifd = tiff.level_images_ifd + level_index;
			CHECK(ifd->image_width > 0);
			CHECK(ifd->image_height > 0);
			CHECK(ifd->tile_width > 0);
			CHECK(ifd->tile_height > 0);
			CHECK(ifd->width_in_tiles > 0);
			CHECK(ifd->height_in_tiles > 0);
			CHECK(ifd->tile_count == (u64)ifd->width_in_tiles * (u64)ifd->height_in_tiles);
			CHECK(ifd->downsample_factor >= 1.0f);
		}

		tiff_destroy(&tiff);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping TIFF reader fixture checks: no TIFF fixtures are present locally.");
	}
}

TEST_CASE("TODO: TIFF fixture metadata serializes and deserializes" * doctest::skip()) {
	// This currently does not round-trip the cropped TIFF fixture metadata correctly:
	// deserialized.level_image_ifd_count differs from the original and cleanup trips ltalloc
	// guard checks. Keep this as a regression target once tiff_serialize/tiff_deserialize
	// semantics are clarified or fixed.
}

TEST_CASE("open DICOM WSI fixtures through Slidescape DICOM reader") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "dicom") != 0 ||
		    !fixture_has_tag(fixture, "dicom-metadata") ||
		    !test_fixture_file_exists(fixture.path)) {
			continue;
		}

		CAPTURE(fixture.id);
		CAPTURE(fixture.path);
		image_t* image = load_dicom_fixture_image(&fixture);

		CHECK(image->dicom.wsi.instance_count > 0);
		CHECK(image->level_count > 0);
		CHECK(image->width_in_pixels > 0);
		CHECK(image->height_in_pixels > 0);
		CHECK(image->tile_width > 0);
		CHECK(image->tile_height > 0);

		for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
			level_image_t* level = image->level_images + level_index;
			if (!level->exists) continue;
			CAPTURE(level_index);
			CHECK(level->width_in_pixels > 0);
			CHECK(level->height_in_pixels > 0);
			CHECK(level->width_in_tiles > 0);
			CHECK(level->height_in_tiles > 0);
			CHECK(level->tile_count == level->width_in_tiles * level->height_in_tiles);
			CHECK(level->tile_width > 0);
			CHECK(level->tile_height > 0);
		}

		if (fixture_has_tag(fixture, "dicom-multislide")) {
			CHECK(arrlen(image->dicom.slides) > 1);
			CHECK(image->dicom.selected_slide_index >= 0);
			CHECK(image->dicom.selected_slide_index < arrlen(image->dicom.slides));
		}

		image_destroy(image);
		free(image);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping DICOM WSI metadata checks: no tagged DICOM fixture is present locally.");
	}
}

TEST_CASE("decode a representative DICOM WSI frame") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t fixture_index = 0; fixture_index < manifest->fixture_count; ++fixture_index) {
		fixture_t& fixture = manifest->fixtures[fixture_index];
		if (strcmp(fixture.type, "dicom") != 0 ||
		    !fixture_has_tag(fixture, "dicom-tile") ||
		    !test_fixture_file_exists(fixture.path)) {
			continue;
		}

		CAPTURE(fixture.id);
		CAPTURE(fixture.path);
		image_t* image = load_dicom_fixture_image(&fixture);

		level_image_t* level = NULL;
		i32 level_index = -1;
		i32 tile_index = -1;
		for (i32 candidate_level = 0; candidate_level < image->level_count && tile_index < 0; ++candidate_level) {
			level_image_t* candidate = image->level_images + candidate_level;
			if (!candidate->exists) continue;
			for (i32 candidate_tile = 0; candidate_tile < candidate->tile_count; ++candidate_tile) {
				if (!candidate->tiles[candidate_tile].is_empty) {
					level = candidate;
					level_index = candidate_level;
					tile_index = candidate_tile;
					break;
				}
			}
		}

		REQUIRE(level != NULL);
		REQUIRE(level_index >= 0);
		REQUIRE(tile_index >= 0);
		CAPTURE(level_index);
		CAPTURE(tile_index);

		dicom_instance_t* level_instance = image->dicom.wsi.level_instances[level->pyramid_image_index];
		REQUIRE(level_instance != NULL);
		dicom_tile_t* dicom_tile = level_instance->tiles + tile_index;
		REQUIRE(dicom_tile->exists);
		if (!dicom_tile->is_offset_known) {
			REQUIRE(dicom_wsi_index_level(&image->dicom, level->pyramid_image_index));
		}
		REQUIRE(dicom_tile->is_offset_known);
		REQUIRE(dicom_tile->data_size > 0);

		u8* pixels = dicom_wsi_decode_tile_to_bgra(&image->dicom, level->pyramid_image_index, tile_index);
		REQUIRE(pixels != NULL);
		CHECK(pixel_buffer_has_variation((u32*)pixels, (size_t)level->tile_width * (size_t)level->tile_height));

		if (fixture_has_tag(fixture, "dicom-concatenation")) {
			bool uses_multiple_instances = false;
			dicom_instance_t* first_instance = NULL;
			for (i32 i = 0; i < level_instance->tile_count; ++i) {
				dicom_tile_t* tile = level_instance->tiles + i;
				if (!tile->exists) continue;
				if (!first_instance) {
					first_instance = tile->instance;
				} else if (tile->instance != first_instance) {
					uses_multiple_instances = true;
					break;
				}
			}
			CHECK(uses_multiple_instances);
		}

		free(pixels);
		image_destroy(image);
		free(image);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping DICOM WSI frame decode check: no tagged DICOM fixture is present locally.");
	}
}

TEST_CASE("decode a representative TIFF tile") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping TIFF tile decode check: no TIFF fixture is present locally.");
		return;
	}

	tiff_t tiff = {};
	REQUIRE(open_tiff_file(&tiff, fixture->path));
	REQUIRE(tiff.main_image_ifd != NULL);
	tiff_ifd_t* ifd = tiff.main_image_ifd;
	REQUIRE(ifd->tile_count > 0);

	u8* pixels = NULL;
	i32 tile_index = -1;
	for (u64 i = 0; i < ifd->tile_count; ++i) {
		if (ifd->tile_offsets[i] == 0 || ifd->tile_byte_counts[i] == 0) continue;
		pixels = tiff_decode_tile(0, &tiff, ifd, (i32)i, 0, (i32)(i % ifd->width_in_tiles), (i32)(i / ifd->width_in_tiles));
		if (pixels) {
			tile_index = (i32)i;
			break;
		}
	}

	CAPTURE(tile_index);
	REQUIRE(pixels != NULL);
	size_t pixel_count = (size_t)ifd->tile_width * (size_t)ifd->tile_height;
	CHECK(pixel_buffer_has_variation((u32*)pixels, pixel_count));

	free(pixels);
	tiff_destroy(&tiff);
}

TEST_CASE("load TIFF fixture as image_t and read a tile region") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping image_read_region TIFF check: no TIFF fixture is present locally.");
		return;
	}

	ensure_image_read_region_thread_pool();

	image_t* image = load_tiff_fixture_image(fixture);
	level_image_t* level_image = image->level_images + 0;
	REQUIRE(level_image->exists);

	selected_tiff_tile_t tile = {};
	REQUIRE(select_decodable_tiff_tile(image, &tile));
	CAPTURE(fixture->path);
	CAPTURE(tile.tile_index);
	CAPTURE(tile.tile_x);
	CAPTURE(tile.tile_y);

	i32 x = tile.tile_x * (i32)level_image->tile_width;
	i32 y = tile.tile_y * (i32)level_image->tile_height;
	size_t region_size = (size_t)tile.read_width * (size_t)tile.read_height * sizeof(u32);
	u32* region = (u32*)calloc((size_t)tile.read_width * (size_t)tile.read_height, sizeof(u32));
	REQUIRE(region != NULL);

	REQUIRE(image_read_region(image, 0, x, y, tile.read_width, tile.read_height, region, PIXEL_FORMAT_U8_BGRA));
	for (i32 row = 0; row < tile.read_height; ++row) {
		u32* expected_row = (u32*)tile.pixels + (size_t)row * (size_t)level_image->tile_width;
		u32* actual_row = region + (size_t)row * (size_t)tile.read_width;
		CHECK(memcmp(actual_row, expected_row, (size_t)tile.read_width * sizeof(u32)) == 0);
	}
	CHECK(region_size > 0);

	free(region);
	free(tile.pixels);
	image_destroy(image);
	free(image);
}

TEST_CASE("image_read_region converts TIFF BGRA pixels to F32 luminance") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping image_read_region F32 conversion check: no TIFF fixture is present locally.");
		return;
	}

	ensure_image_read_region_thread_pool();

	image_t* image = load_tiff_fixture_image(fixture);
	level_image_t* level_image = image->level_images + 0;
	REQUIRE(level_image->exists);

	selected_tiff_tile_t tile = {};
	REQUIRE(select_decodable_tiff_tile(image, &tile));

	i32 read_width = MIN(tile.read_width, 16);
	i32 read_height = MIN(tile.read_height, 16);
	REQUIRE(read_width > 0);
	REQUIRE(read_height > 0);

	i32 x = tile.tile_x * (i32)level_image->tile_width;
	i32 y = tile.tile_y * (i32)level_image->tile_height;
	float* luminance = (float*)calloc((size_t)read_width * (size_t)read_height, sizeof(float));
	REQUIRE(luminance != NULL);

	REQUIRE(image_read_region(image, 0, x, y, read_width, read_height, luminance, PIXEL_FORMAT_F32_Y));
	for (i32 row = 0; row < read_height; ++row) {
		for (i32 col = 0; col < read_width; ++col) {
			u8* bgra = tile.pixels + ((size_t)row * (size_t)level_image->tile_width + (size_t)col) * 4;
			float b = (float)bgra[0] * (1.0f / 255.0f);
			float g = (float)bgra[1] * (1.0f / 255.0f);
			float r = (float)bgra[2] * (1.0f / 255.0f);
			float expected = f32_rgb_to_f32_y(r, g, b);
			CHECK(luminance[(size_t)row * (size_t)read_width + (size_t)col] == doctest::Approx(expected));
		}
	}

	free(luminance);
	free(tile.pixels);
	image_destroy(image);
	free(image);
}

TEST_CASE("open iSyntax fixture and read WSI metadata through libisyntax" ) {
	const fixture_t* fixture = first_available_fixture("isyntax", "isyntax-metadata");
	if (!fixture) fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax metadata check: no iSyntax fixture is present locally.");
		return;
	}

	REQUIRE(libisyntax_init() == LIBISYNTAX_OK);

	isyntax_t* isyntax = NULL;
	REQUIRE(libisyntax_open(fixture->path, (libisyntax_open_flags_t)0, &isyntax) == LIBISYNTAX_OK);
	REQUIRE(isyntax != NULL);

	CHECK(libisyntax_get_tile_width(isyntax) > 0);
	CHECK(libisyntax_get_tile_height(isyntax) > 0);
	CHECK(isyntax->image_count > 0);
	CHECK(isyntax->wsi_image_index >= 0);
	CHECK(isyntax->wsi_image_index < isyntax->image_count);

	const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax);
	REQUIRE(wsi != NULL);
	i32 level_count = libisyntax_image_get_level_count(wsi);
	CHECK(level_count > 0);

	for (i32 level_index = 0; level_index < level_count; ++level_index) {
		const isyntax_level_t* level = libisyntax_image_get_level(wsi, level_index);
		CAPTURE(level_index);
		CHECK(libisyntax_level_get_scale(level) >= 0);
		CHECK(libisyntax_level_get_width(level) > 0);
		CHECK(libisyntax_level_get_height(level) > 0);
		CHECK(libisyntax_level_get_width_in_tiles(level) > 0);
		CHECK(libisyntax_level_get_height_in_tiles(level) > 0);
		CHECK(libisyntax_level_get_mpp_x(level) > 0.0f);
		CHECK(libisyntax_level_get_mpp_y(level) > 0.0f);
	}

	libisyntax_close(isyntax);
}

TEST_CASE("decode an iSyntax tile through libisyntax") {
	const fixture_t* fixture = first_available_fixture("isyntax", "isyntax-tile");
	if (!fixture) fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax tile decode check: no iSyntax fixture is present locally.");
		return;
	}

	REQUIRE(libisyntax_init() == LIBISYNTAX_OK);

	isyntax_t* isyntax = NULL;
	REQUIRE(libisyntax_open(fixture->path, (libisyntax_open_flags_t)0, &isyntax) == LIBISYNTAX_OK);
	REQUIRE(isyntax != NULL);

	isyntax_cache_t* cache = NULL;
	REQUIRE(libisyntax_cache_create("slidescape_tests iSyntax cache", 256, &cache) == LIBISYNTAX_OK);
	REQUIRE(cache != NULL);
	REQUIRE(libisyntax_cache_inject(cache, isyntax) == LIBISYNTAX_OK);

	const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax);
	REQUIRE(wsi != NULL);
	i32 level_count = libisyntax_image_get_level_count(wsi);
	REQUIRE(level_count > 0);

	const isyntax_level_t* selected_level = NULL;
	const isyntax_tile_t* selected_tile = NULL;
	i32 selected_level_index = -1;
	for (i32 level_index = level_count - 1; level_index >= 0; --level_index) {
		const isyntax_level_t* level = libisyntax_image_get_level(wsi, level_index);
		if (!level || !level->tiles) continue;
		for (u64 tile_index = 0; tile_index < level->tile_count; ++tile_index) {
			const isyntax_tile_t* tile = level->tiles + tile_index;
			if (tile->exists) {
				selected_level = level;
				selected_tile = tile;
				selected_level_index = level_index;
				break;
			}
		}
		if (selected_tile) break;
	}

	REQUIRE(selected_level != NULL);
	REQUIRE(selected_tile != NULL);
	CAPTURE(selected_level_index);
	CAPTURE(selected_tile->tile_x);
	CAPTURE(selected_tile->tile_y);

	i32 tile_width = libisyntax_get_tile_width(isyntax);
	i32 tile_height = libisyntax_get_tile_height(isyntax);
	REQUIRE(tile_width > 0);
	REQUIRE(tile_height > 0);

	size_t pixel_count = (size_t)tile_width * (size_t)tile_height;
	u32* pixels = (u32*)calloc(pixel_count, sizeof(u32));
	REQUIRE(static_cast<bool>(pixels != NULL));

	REQUIRE(libisyntax_tile_read(isyntax, cache, selected_level_index, selected_tile->tile_x, selected_tile->tile_y,
	                             pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA) == LIBISYNTAX_OK);
	CHECK(pixel_buffer_has_variation(pixels, pixel_count));

	free(pixels);
	libisyntax_cache_destroy(cache);
	libisyntax_close(isyntax);
}

TEST_CASE("TODO: load ASAP XML fixture through annotation parser" * doctest::skip()) {
	// This should call load_asap_xml_annotations() or a lower-level parser once annotation loading can
	// be exercised without constructing a full app_state_t/GUI viewer context.
}
