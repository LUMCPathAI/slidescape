#include "doctest.h"
#include "source_fs.h"
#include "miniz.h"
#include "mrxs.h"
#include "image_loader.h"
#include "tile_cache.h"
#include "test_temp.h"
#include "archive_ingest.h"
#include "archive_instance_provider.h"
#include "archive_object.h"
#include "archive_dicom_zip.h"
#include "jpeg_decoder.h"
#include "dicom_wsi.h"

#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

static source_read_result_t memory_read(void* context, u64 offset, void* data, size_t size) {
	auto& bytes = *(std::vector<u8>*)context;
	if (offset > bytes.size() || size > bytes.size() - offset) return SOURCE_READ_IO_ERROR;
	memcpy(data, bytes.data() + offset, size);
	return SOURCE_READ_OK;
}
static bool memory_write(void* context, const void* data, size_t size) {
	auto& bytes = *(std::vector<u8>*)context;
	const u8* at = (const u8*)data;
	bytes.insert(bytes.end(), at, at + size);
	return true;
}
static source_blob_t memory_blob(std::vector<u8>& bytes) { return {(u64)bytes.size(), &bytes, memory_read, nullptr}; }
static source_file_t entire(source_blob_t& blob) {
	source_file_t file = {};
	REQUIRE(source_file_view(&blob, 0, blob.size, &file));
	return file;
}
static std::vector<u8> make_zip(const char* path = "nested/slide/Data0000.dat", int level = 0) {
	mz_zip_archive writer = {};
	REQUIRE(mz_zip_writer_init_heap(&writer, 0, 0));
	REQUIRE(mz_zip_writer_add_mem(&writer, path, "tile bytes", 10, level));
	void* buffer = nullptr;
	size_t size = 0;
	REQUIRE(mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size));
	std::vector<u8> bytes((u8*)buffer, (u8*)buffer + size);
	mz_free(buffer);
	mz_zip_writer_end(&writer);
	return bytes;
}
static size_t signature(const std::vector<u8>& bytes, u32 value) {
	for (size_t i = 0; i + 4 <= bytes.size(); ++i)
		if (bytes[i] == (u8)value && bytes[i+1] == (u8)(value >> 8) && bytes[i+2] == (u8)(value >> 16) && bytes[i+3] == (u8)(value >> 24)) return i;
	return bytes.size();
}
static void put(std::vector<u8>& bytes, size_t offset, u64 value, int count) {
	if (offset + count > bytes.size()) bytes.resize(offset + count);
	for (int i = 0; i < count; ++i) bytes[offset+i] = (u8)(value >> (8*i));
}

TEST_CASE("source views bound reads without touching the underlying source on invalid ranges") {
	std::vector<u8> data = {1,2,3,4,5};
	auto blob = memory_blob(data);
	source_file_t file;
	REQUIRE(source_file_view(&blob, 1, 3, &file));
	u8 out[3] = {};
	CHECK(source_file_read(&file, 0, out, 3) == SOURCE_READ_OK);
	CHECK(out[0] == 2);
	CHECK(out[2] == 4);
	CHECK(source_file_read(&file, 3, nullptr, 0) == SOURCE_READ_OK);
	CHECK(source_file_read(&file, 3, out, 1) == SOURCE_READ_OUT_OF_BOUNDS);
	CHECK(source_file_read(&file, UINT64_MAX, out, 1) == SOURCE_READ_OUT_OF_BOUNDS);
	CHECK_FALSE(source_file_view(&blob, UINT64_MAX, 2, &file));
	REQUIRE(source_file_view(&blob, 0, blob.size, &file));
	data.resize(2); // Source was truncated after opening.
	CHECK(source_file_read(&file, 0, out, 3) == SOURCE_READ_IO_ERROR);
}

TEST_CASE("stored ZIP mount skips CRC scans but explicit validation detects changed payloads") {
	auto bytes = make_zip();
	auto blob = memory_blob(bytes);
	auto file = entire(blob);
	char error[256];
	auto zip = source_zip_open(&file, error, sizeof(error));
	REQUIRE_MESSAGE(zip, error);
	CHECK(source_zip_count(zip) == 1);
	auto member = source_zip_find(zip, "nested/slide/Data0000.dat");
	REQUIRE(member);
	CHECK(source_zip_validate_contents(zip, error, sizeof(error)));
	u8 result[10];
	REQUIRE(source_file_read(&member->file, 0, result, 10) == SOURCE_READ_OK);
	CHECK(memcmp(result, "tile bytes", 10) == 0);
	bytes[member->file.offset] ^= 1;
	source_zip_close(zip);
	zip = source_zip_open(&file, error, sizeof(error));
	REQUIRE_MESSAGE(zip, error);
	CHECK_FALSE(source_zip_validate_contents(zip, error, sizeof(error)));
	source_zip_close(zip);
}

TEST_CASE("stored ZIP profile rejects ambiguous paths compression links and inconsistent headers") {
	const char* invalid[] = {"../tile", "/tile", "C:/tile", "a\\tile", "a//tile", "a/./tile", "a/../tile", "a./tile", "AUX.dat", "a /tile", "tile:", "a/\xff"};
	for (auto path : invalid) {
		CAPTURE(path);
		CHECK_FALSE(source_path_is_valid(path, false));
	}
	for (int mutation = 0; mutation < 8; ++mutation) {
		CAPTURE(mutation);
		auto bytes = make_zip(mutation == 0 ? "../tile" : "tile", mutation == 1 ? 6 : 0);
		size_t central = signature(bytes, 0x02014b50);
		REQUIRE(central < bytes.size());
		if (mutation == 2) { put(bytes, 6, 1, 2); put(bytes, central+8, 1, 2); }
		if (mutation == 3) { put(bytes, central+4, (3 << 8) | 20, 2); put(bytes, central+38, (u64)0120777 << 16, 4); }
		if (mutation == 4) put(bytes, 8, 8, 2);
		if (mutation == 5) bytes[30] = 'X';
		if (mutation == 6) bytes[central+46+1] = 0;
		if (mutation == 7) put(bytes, central+42, UINT32_MAX - 4, 4);
		auto blob = memory_blob(bytes);
		auto file = entire(blob);
		auto zip = source_zip_open(&file, nullptr, 0);
		CHECK(zip == nullptr);
		source_zip_close(zip);
	}
}

TEST_CASE("ZIP64 writer streams stored members empty directories and UTF8 without member hashes") {
	std::vector<u8> payload(130000);
	for (size_t i = 0; i < payload.size(); ++i) payload[i] = (u8)i;
	auto blob = memory_blob(payload);
	source_zip_input_t members[] = {
		{"case/", {}, true}, {"case/.sidecar", entire(blob), false},
		{"case/échantillon.dat", entire(blob), false}, {"empty/", {}, true},
		{"empty-file", {&blob, 0, 0}, false},
	};
	std::vector<u8> bytes;
	char error[256];
	REQUIRE_MESSAGE(source_zip_write(members, 5, memory_write, &bytes, error, sizeof(error)), error);
	CHECK(signature(bytes, 0x06064b50) < bytes.size());
	auto zipped_blob = memory_blob(bytes);
	auto file = entire(zipped_blob);
	auto zip = source_zip_open(&file, error, sizeof(error));
	REQUIRE_MESSAGE(zip, error);
	CHECK(source_zip_count(zip) == 5);
	CHECK(source_zip_validate_contents(zip, error, sizeof(error)));
	auto member = source_zip_find(zip, "case/échantillon.dat");
	REQUIRE(member);
	std::atomic<bool> ok(true);
	std::vector<std::thread> threads;
	for (int n = 0; n < 4; ++n) threads.emplace_back([&] {
		u8 tile[257];
		for (size_t at = 0; at + sizeof(tile) < payload.size(); at += 193)
			if (source_file_read(&member->file, at, tile, sizeof(tile)) != SOURCE_READ_OK ||
				memcmp(tile, payload.data() + at, sizeof(tile))) ok = false;
	});
	for (auto& thread : threads) thread.join();
	CHECK(ok.load());
	source_zip_close(zip);
	/* Independent from our member-view code: miniz can extract the stream. */
	mz_zip_archive reader = {};
	REQUIRE(mz_zip_reader_init_mem(&reader, bytes.data(), bytes.size(), 0));
	std::vector<u8> restored(payload.size());
	CHECK(mz_zip_reader_extract_file_to_mem(&reader, "case/échantillon.dat", restored.data(), restored.size(), 0));
	CHECK(restored == payload);
	mz_zip_reader_end(&reader);
}

TEST_CASE("ZIP writer rejects namespace collisions before writing and propagates sink failures") {
	std::vector<u8> payload = {42};
	auto blob = memory_blob(payload);
	const char* pairs[][2] = {{"file", "file"}, {"file", "FILE"}, {"a", "a/b"}, {"a", "a/"}, {"Folder/a", "folder/b"}};
	for (auto& pair : pairs) {
		source_zip_input_t members[] = {{pair[0], entire(blob), false}, {pair[1], entire(blob), pair[1][strlen(pair[1])-1] == '/'}};
		std::vector<u8> output;
		CHECK_FALSE(source_zip_write(members, 2, memory_write, &output, nullptr, 0));
		CHECK(output.empty());
	}
	source_zip_input_t member = {"data", entire(blob), false};
	int calls = 0;
	auto fail_sink = [](void* context, const void*, size_t) -> bool { return ++*(int*)context < 3; };
	CHECK_FALSE(source_zip_write(&member, 1, fail_sink, &calls, nullptr, 0));
	CHECK(calls == 3);
}

/* A >4 GiB member followed by another member beyond the 32-bit boundary. Only
   headers and a few payload bytes exist in memory; no huge allocations or writes. */
struct sparse_region { u64 offset; std::vector<u8> data; };
struct sparse_archive { std::vector<sparse_region> regions; size_t bytes_read = 0; };
static source_read_result_t sparse_read(void* context, u64 offset, void* dest, size_t size) {
	auto& sparse = *(sparse_archive*)context;
	sparse.bytes_read += size;
	memset(dest, 0, size);
	for (auto& region : sparse.regions) {
		u64 first = std::max(offset, region.offset);
		u64 last = std::min(offset + size, region.offset + region.data.size());
		if (first < last) memcpy((u8*)dest + (first-offset), region.data.data() + (first-region.offset), (size_t)(last-first));
	}
	return SOURCE_READ_OK;
}
static std::vector<u8> local_header(const char* name, u64 size, u32 crc) {
	std::vector<u8> data(30);
	put(data, 0, 0x04034b50, 4); put(data, 4, 45, 2); put(data, 14, crc, 4);
	put(data, 18, UINT32_MAX, 4); put(data, 22, UINT32_MAX, 4);
	put(data, 26, strlen(name), 2); put(data, 28, 20, 2);
	data.insert(data.end(), name, name+strlen(name));
	size_t p = data.size();
	put(data, p, 1, 2); put(data, p+2, 16, 2); put(data, p+4, size, 8); put(data, p+12, size, 8);
	return data;
}
static void central_header(std::vector<u8>& data, const char* name, u64 size, u64 offset, u32 crc) {
	size_t p = data.size(); data.resize(p+46);
	put(data, p, 0x02014b50, 4); put(data, p+4, 45, 2); put(data, p+6, 45, 2);
	put(data, p+16, crc, 4); put(data, p+20, UINT32_MAX, 4); put(data, p+24, UINT32_MAX, 4);
	put(data, p+28, strlen(name), 2); put(data, p+30, 28, 2); put(data, p+42, UINT32_MAX, 4);
	data.insert(data.end(), name, name+strlen(name)); p = data.size();
	put(data, p, 1, 2); put(data, p+2, 24, 2); put(data, p+4, size, 8); put(data, p+12, size, 8); put(data, p+20, offset, 8);
}
TEST_CASE("ZIP64 members and offsets cross 4 GiB with no SSD writes or payload scan") {
	u64 huge_size = (1ULL << 32) + 17;
	auto first = local_header("huge.dat", huge_size, 0);
	u64 next = first.size() + huge_size;
	u32 crc = (u32)mz_crc32(0, (const u8*)"tile", 4);
	auto second = local_header("tile.dat", 4, crc);
	u64 tile_offset = next + second.size();
	second.insert(second.end(), {'t','i','l','e'});
	u64 central_offset = next + second.size();
	std::vector<u8> end;
	central_header(end, "huge.dat", huge_size, 0, 0);
	central_header(end, "tile.dat", 4, next, crc);
	size_t central_size = end.size(), p = end.size();
	end.resize(p+56);
	put(end, p, 0x06064b50, 4); put(end, p+4, 44, 8); put(end, p+12, 45, 2); put(end, p+14, 45, 2);
	put(end, p+24, 2, 8); put(end, p+32, 2, 8); put(end, p+40, central_size, 8); put(end, p+48, central_offset, 8);
	p = end.size(); end.resize(p+20);
	put(end, p, 0x07064b50, 4); put(end, p+8, central_offset+central_size, 8); put(end, p+16, 1, 4);
	p = end.size(); end.resize(p+22);
	put(end, p, 0x06054b50, 4); put(end, p+8, 65535, 2); put(end, p+10, 65535, 2);
	put(end, p+12, UINT32_MAX, 4); put(end, p+16, UINT32_MAX, 4);
	sparse_archive sparse = {{{0, first}, {next, second}, {central_offset, end}}, 0};
	source_blob_t blob = {central_offset + end.size(), &sparse, sparse_read, nullptr};
	auto file = entire(blob);
	char error[256];
	auto zip = source_zip_open(&file, error, sizeof(error));
	REQUIRE_MESSAGE(zip, error);
	CHECK(source_zip_find(zip, "huge.dat")->file.size == huge_size);
	auto tile = source_zip_find(zip, "tile.dat");
	REQUIRE(tile);
	CHECK(tile->file.offset == tile_offset);
	char data[4];
	REQUIRE(source_file_read(&tile->file, 0, data, 4) == SOURCE_READ_OK);
	CHECK(memcmp(data, "tile", 4) == 0);
	CHECK(sparse.bytes_read < 128 * 1024);
	source_zip_close(zip);
}

struct source_test_file {
	char path[1024] = {};
	explicit source_test_file(const std::vector<u8>& bytes) {
		int fd = slidescape_test_make_temp_file(path, sizeof(path), "slidescape-zip");
		REQUIRE(fd >= 0);
#if defined(_WIN32)
		_close(fd);
#else
		close(fd);
#endif
		FILE* output = fopen(path, "wb");
		REQUIRE(output);
		REQUIRE(fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size());
		REQUIRE(fclose(output) == 0);
	}
	~source_test_file() { if (*path) remove(path); }
};

static bool collect_entry(void* context, const source_entry_t* entry) {
	((std::vector<std::string>*)context)->push_back(std::string(entry->path) + (entry->is_directory ? "/" : ""));
	return true;
}
TEST_CASE("ZIP namespace discovers nested implicit directories and keeps one shared blob") {
	auto bytes = make_zip();
	source_test_file temp(bytes);
	char error[256];
	auto source = source_fs_open_zip(temp.path, error, sizeof(error));
	REQUIRE_MESSAGE(source, error);
	std::vector<std::string> paths;
	REQUIRE(source_fs_list(source, "", collect_entry, &paths));
	CHECK(paths == std::vector<std::string>{"nested/"});
	paths.clear();
	REQUIRE(source_fs_list(source, "nested", collect_entry, &paths));
	CHECK(paths == std::vector<std::string>{"nested/slide/"});
	source_entry_t info;
	CHECK(source_fs_stat(source, "nested/slide", &info));
	CHECK(info.is_directory);
	source_file_t first, second;
	REQUIRE(source_fs_open_file(source, "nested/slide/Data0000.dat", &first));
	REQUIRE(source_fs_open_file(source, "nested/slide/Data0000.dat", &second));
	CHECK(first.blob == second.blob);
	CHECK(first.size == 10);
	CHECK_FALSE(source_fs_open_file(source, "../escape", &second));
	std::atomic<bool> ok(true);
	std::thread worker([&] {
		for (int n = 0; n < 100; ++n) {
			char tile[10];
			if (source_file_read(&first, 0, tile, 10) != SOURCE_READ_OK || memcmp(tile, "tile bytes", 10)) ok = false;
		}
	});
	for (int n = 0; n < 100; ++n) {
		char tile[10];
		if (source_file_read(&first, 0, tile, 10) != SOURCE_READ_OK || memcmp(tile, "tile bytes", 10)) ok = false;
	}
	worker.join();
	CHECK(ok.load());
	source_fs_close(source);
}

static std::vector<u8> tiny_mrxs_zip(bool duplicate = false, bool omit_data = false, bool jpeg = false) {
	const char* ini = "[GENERAL]\nIMAGENUMBER_X=1\nIMAGENUMBER_Y=1\nCameraImageDivisionsPerSide=1\n"
		"[HIERARCHICAL]\nINDEXFILE=Index.dat\nHIER_COUNT=1\nNONHIER_COUNT=0\n"
		"HIER_0_NAME=Slide zoom level\nHIER_0_COUNT=1\nHIER_0_VAL_0=ZoomLevel_0\nHIER_0_VAL_0_SECTION=ZOOM\n"
		"[DATAFILE]\nFILE_COUNT=1\nFILE_0=Data0000.dat\n"
		"[ZOOM]\nDIGITIZER_WIDTH=2\nDIGITIZER_HEIGHT=2\nIMAGE_FORMAT=BMP24\n"
		"MICROMETER_PER_PIXEL_X=0.25\nMICROMETER_PER_PIXEL_Y=0.25\n";
	std::string ini_text(ini);
	if (jpeg) ini_text.replace(ini_text.find("BMP24"), 5, "JPEG");
	std::vector<u8> ini_bytes(ini_text.begin(), ini_text.end());
	std::vector<u8> index(77);
	memcpy(index.data(), "01.02", 5);
	put(index, 37, 45, 4); put(index, 41, 49, 4); put(index, 45, 53, 4);
	put(index, 53, 1, 4); // One occupied tile in this page.
	put(index, 65, 3, 4); put(index, 69, 70, 4); // Offset and length within Data0000.dat.
	std::vector<u8> bmp(70);
	put(bmp, 0, 0x4d42, 2); put(bmp, 2, 70, 4); put(bmp, 10, 54, 4); put(bmp, 14, 40, 4);
	put(bmp, 18, 2, 4); put(bmp, 22, 2, 4); put(bmp, 26, 1, 2); put(bmp, 28, 24, 2);
	for (size_t row = 0; row < 2; ++row)
		for (size_t x = 0; x < 2; ++x) { bmp[54+row*8+x*3] = 30; bmp[55+row*8+x*3] = 20; bmp[56+row*8+x*3] = 10; }
	if (jpeg) {
		u8 pixels[16] = {30,20,10,255,30,20,10,255,30,20,10,255,30,20,10,255};
		u8* encoded = nullptr; u64 size = 0;
		jpeg_encode_image(pixels, 2, 2, 90, &encoded, &size);
		REQUIRE(encoded);
		bmp.assign(encoded, encoded + size);
		libc_free(encoded);
		put(index, 69, size, 4);
	}
	std::vector<u8> data = {99,98,97}; data.insert(data.end(), bmp.begin(), bmp.end());
	auto ini_blob = memory_blob(ini_bytes), index_blob = memory_blob(index), data_blob = memory_blob(data);
	source_zip_input_t members[] = {
		{"wrapper/slide/Slidedat.ini", entire(ini_blob), false},
		{"wrapper/slide/Index.dat", entire(index_blob), false},
		{"wrapper/slide/Data0000.dat", entire(data_blob), false},
		{"another/Slidedat.ini", entire(ini_blob), false},
	};
	std::vector<u8> output;
	REQUIRE(source_zip_write(members, duplicate ? 4 : omit_data ? 2 : 3, memory_write, &output, nullptr, 0));
	return output;
}
TEST_CASE("native MRXS opens and decodes a nested stored ZIP without an mrxs thumbnail file") {
	init_global_system_info(false);
	if (!threadlocal_thread_memory) init_thread_memory(&global_system_info);
	auto bytes = tiny_mrxs_zip();
	source_test_file temp(bytes);
	mrxs_t slide = {};
	char error[256];
	REQUIRE_MESSAGE(mrxs_open_from_zip(&slide, temp.path, error, sizeof(error)), error);
	CHECK(slide.levels[0].tiles == nullptr);
	REQUIRE(mrxs_index_all(&slide));
	CHECK(slide.level_count == 1);
	CHECK(slide.base_width_in_pixels == 2);
	CHECK(slide.base_height_in_pixels == 2);
	CHECK(slide.mpp_x == doctest::Approx(0.25));
	CHECK(slide.dat_files[0].size == 73);
	CHECK(slide.dat_files[0].offset > 0);
	u8* pixels = mrxs_decode_tile_to_bgra(&slide, 0, 0, 0);
	REQUIRE(pixels);
	for (int i = 0; i < 4; ++i) {
		CHECK(pixels[i*4] == 30);
		CHECK(pixels[i*4+1] == 20);
		CHECK(pixels[i*4+2] == 10);
		CHECK(pixels[i*4+3] == 255);
	}
	free(pixels);
	mrxs_destroy(&slide);
	/* Exercise the same dispatch used by the viewer, without requiring a GL context. */
	file_info_t info = viewer_get_file_info(temp.path);
	info.type = VIEWER_FILE_TYPE_ZIP; // Temporary file deliberately has no extension.
	image_load_options_t options = {};
	image_t* image = image_load_from_file(&info, nullptr, &options);
	REQUIRE(image);
	CHECK(image->is_valid);
	CHECK(image->backend == IMAGE_BACKEND_MRXS);
	image_destroy(image);
	free(image);
}
TEST_CASE("MRXS ZIP discovery rejects multiple slides and cleans up partial opens") {
	for (int scenario = 0; scenario < 2; ++scenario) {
		auto bytes = tiny_mrxs_zip(scenario == 0, scenario == 1);
		source_test_file temp(bytes);
		mrxs_t slide = {};
		char error[256] = {};
		CHECK_FALSE(mrxs_open_from_zip(&slide, temp.path, error, sizeof(error)));
		CHECK(error[0] != 0);
		if (scenario == 0) CHECK(strstr(error, "multiple") != nullptr);
		mrxs_destroy(&slide);
	}
}

TEST_CASE("source directory enumeration includes hidden members and empty directories") {
	char root[1024];
	REQUIRE(slidescape_test_make_temp_directory(root, sizeof(root), "slidescape-source"));
	std::string hidden = std::string(root) + "/.sidecar";
	std::string empty = std::string(root) + "/empty";
	FILE* file = fopen(hidden.c_str(), "wb"); REQUIRE(file);
	REQUIRE(fwrite("metadata", 1, 8, file) == 8); REQUIRE(fclose(file) == 0);
#if defined(_WIN32)
	REQUIRE(_mkdir(empty.c_str()) == 0);
#else
	REQUIRE(mkdir(empty.c_str(), 0700) == 0);
#endif
	auto source = source_fs_open_directory(root); REQUIRE(source);
	std::vector<std::string> paths;
	REQUIRE(source_fs_list(source, "", collect_entry, &paths));
	std::sort(paths.begin(), paths.end());
	CHECK(paths == std::vector<std::string>{".sidecar", "empty/"});
	paths.clear();
	CHECK(source_fs_list(source, "empty", collect_entry, &paths));
	CHECK(paths.empty());
	mem_t* data = source_fs_read_entire(source, ".sidecar", 8);
	REQUIRE(data);
	CHECK(data->len == 8);
	CHECK(memcmp(data->data, "metadata", 9) == 0); // Includes the parser terminator.
	free(data);
	CHECK(source_fs_read_entire(source, ".sidecar", 7) == nullptr);
	source_fs_close(source);
	CHECK(remove(hidden.c_str()) == 0);
#if defined(_WIN32)
	CHECK(_rmdir(empty.c_str()) == 0); CHECK(_rmdir(root) == 0);
#else
	CHECK(rmdir(empty.c_str()) == 0); CHECK(rmdir(root) == 0);
#endif
}

TEST_CASE("ZIP writer fails if a source becomes short rather than publishing a truncated member") {
	std::vector<u8> bytes(100000, 42);
	auto blob = memory_blob(bytes);
	source_zip_input_t member = {"data", entire(blob), false};
	bytes.resize(70000);
	std::vector<u8> output;
	CHECK_FALSE(source_zip_write(&member, 1, memory_write, &output, nullptr, 0));
	CHECK(signature(output, 0x06054b50) == output.size());
}

TEST_CASE("ZIP reader accepts signed and unsigned descriptors with per-member 32 or 64 bit sizes") {
	for (bool wide : {false, true}) for (bool signed_descriptor : {false, true}) {
		CAPTURE(wide);
		CAPTURE(signed_descriptor);
		auto bytes = make_zip("tile");
		size_t central = signature(bytes, 0x02014b50);
		u32 crc = (u32)mz_crc32(0, (const u8*)"tile bytes", 10);
		std::vector<u8> descriptor;
		if (signed_descriptor) put(descriptor, 0, 0x08074b50, 4);
		size_t p = descriptor.size();
		put(descriptor, p, crc, 4);
		put(descriptor, p+4, 10, wide ? 8 : 4);
		put(descriptor, p+4+(wide ? 8 : 4), 10, wide ? 8 : 4);
		size_t data_end = 30 + (bytes[26] | (u16)bytes[27] << 8) + (bytes[28] | (u16)bytes[29] << 8) + 10;
		bytes.erase(bytes.begin()+data_end, bytes.begin()+central); // Replace any existing descriptor.
		bytes.insert(bytes.begin()+data_end, descriptor.begin(), descriptor.end());
		central = data_end + descriptor.size();
		put(bytes, 6, 8, 2); put(bytes, central+8, 8, 2);
		put(bytes, 14, 0, 4); put(bytes, 18, 0, 4); put(bytes, 22, 0, 4);
		size_t end = signature(bytes, 0x06054b50);
		put(bytes, end+16, central, 4);
		auto blob = memory_blob(bytes);
		auto file = entire(blob);
		char error[256];
		auto zip = source_zip_open(&file, error, sizeof(error));
		REQUIRE_MESSAGE(zip, error);
		CHECK(source_zip_validate_contents(zip, error, sizeof(error)));
		source_zip_close(zip);
		// Corrupt descriptor CRC: structural mount must fail without scanning the payload.
		bytes[central-descriptor.size()+(signed_descriptor ? 4 : 0)] ^= 1;
		zip = source_zip_open(&file, error, sizeof(error));
		CHECK(zip == nullptr);
		source_zip_close(zip);
	}
}

TEST_CASE("ZIP64 entry count crosses 65535 entirely in memory") {
	const u32 count = 65536;
	std::vector<u8> empty;
	auto blob = memory_blob(empty);
	auto file = entire(blob);
	std::vector<std::string> paths(count);
	std::vector<source_zip_input_t> members(count);
	for (u32 i = 0; i < count; ++i) {
		char path[32]; snprintf(path, sizeof(path), "members/%05u", i);
		paths[i] = path;
		members[i] = {paths[i].c_str(), file, false};
	}
	std::vector<u8> output;
	char error[256];
	REQUIRE_MESSAGE(source_zip_write(members.data(), count, memory_write, &output, error, sizeof(error)), error);
	CHECK(output.size() < 16 * 1024 * 1024);
	auto archive_blob = memory_blob(output);
	auto archive_file = entire(archive_blob);
	auto zip = source_zip_open(&archive_file, error, sizeof(error));
	REQUIRE_MESSAGE(zip, error);
	CHECK(source_zip_count(zip) == count);
	CHECK(source_zip_find(zip, "members/65535") != nullptr);
	source_zip_close(zip);
}

struct source_test_cleanup { source_fs_t* source; const char* root; };
static bool source_test_remove(void* context, const source_entry_t* entry) {
	auto* cleanup = (source_test_cleanup*)context;
	std::string path = std::string(cleanup->root) + "/" + entry->path;
	if (!entry->is_directory) return remove(path.c_str()) == 0;
	bool success = source_fs_list(cleanup->source, entry->path, source_test_remove, cleanup);
#if defined(_WIN32)
	return _rmdir(path.c_str()) == 0 && success;
#else
	return rmdir(path.c_str()) == 0 && success;
#endif
}
struct source_test_directory {
	char path[ARCHIVE_PATH_CAPACITY];
	source_test_directory() { REQUIRE(slidescape_test_make_temp_directory(path, sizeof(path), "slidescape-zip-ingest")); }
	~source_test_directory() {
		source_test_cleanup cleanup = {source_fs_open_directory(path), path};
		if (cleanup.source) source_fs_list(cleanup.source, "", source_test_remove, &cleanup);
		source_fs_close(cleanup.source);
#if defined(_WIN32)
		_rmdir(path);
#else
		rmdir(path);
#endif
	}
};

static int loader_openslide_handle;
static int loader_openslide_closes;
static bool loader_openslide_succeeds;
static openslide_t* WINAPI loader_openslide_open(const char*) {
	return loader_openslide_succeeds ? (openslide_t*)&loader_openslide_handle : nullptr;
}
static void WINAPI loader_openslide_close(openslide_t* handle) {
	CHECK(handle == (openslide_t*)&loader_openslide_handle);
	++loader_openslide_closes;
}
static const char* WINAPI loader_openslide_error(openslide_t*) { return nullptr; }
static i32 WINAPI loader_openslide_levels(openslide_t*) { return 1; }
static void WINAPI loader_openslide_dimensions(openslide_t*, i64* w, i64* h) { *w = *h = 2; }
static void WINAPI loader_openslide_level_dimensions(openslide_t* handle, i32, i64* w, i64* h) {
	loader_openslide_dimensions(handle, w, h);
}
static double WINAPI loader_openslide_downsample(openslide_t*, i32) { return 1.0; }
static const char* const* WINAPI loader_openslide_names(openslide_t*) { return nullptr; }
static const char* WINAPI loader_openslide_property(openslide_t*, const char*) { return nullptr; }

TEST_CASE("partial native MRXS failure releases backend storage before OpenSlide fallback") {
	source_test_directory root;
	source_test_file zip(tiny_mrxs_zip());
	source_fs_t* source = source_fs_open_zip(zip.path, nullptr, 0);
	REQUIRE(source);
	mem_t* ini = source_fs_read_entire(source, "wrapper/slide/Slidedat.ini", MEGABYTES(1));
	REQUIRE(ini);
	std::string ini_path = std::string(root.path) + "/Slidedat.ini";
	FILE* fp = fopen(ini_path.c_str(), "wb"); REQUIRE(fp);
	REQUIRE(fwrite(ini->data, 1, ini->len, fp) == ini->len);
	REQUIRE(fclose(fp) == 0);
	free(ini);
	source_fs_close(source);
	// Metadata allocates backend state, but the missing Index.dat prevents opening.
	file_info_t file = viewer_get_file_info(root.path);
	file.type = VIEWER_FILE_TYPE_MRXS;
	directory_info_t directory = {};

	struct restore_openslide_t {
		openslide_api saved = openslide;
		~restore_openslide_t() { openslide = saved; }
	} restore;
	openslide = {};
	openslide.open = loader_openslide_open;
	openslide.close = loader_openslide_close;
	openslide.get_error = loader_openslide_error;
	openslide.get_level_count = loader_openslide_levels;
	openslide.get_level0_dimensions = loader_openslide_dimensions;
	openslide.get_level_dimensions = loader_openslide_level_dimensions;
	openslide.get_level_downsample = loader_openslide_downsample;
	openslide.get_property_names = loader_openslide_names;
	openslide.get_property_value = loader_openslide_property;
	openslide.get_associated_image_names = loader_openslide_names;
	for (bool succeeds : {false, true}) {
		loader_openslide_succeeds = succeeds;
		loader_openslide_closes = 0;
		image_load_options_t options = {};
		options.use_native_mrxs_backend = true;
		options.openslide_available = true;
		options.openslide_loading_done = true;
		image_t* image = image_load_from_file(&file, &directory, &options);
		REQUIRE(image);
		CHECK(image->backend == IMAGE_BACKEND_OPENSLIDE);
		CHECK(image->is_valid == succeeds);
		CHECK(loader_openslide_closes == 0);
		if (succeeds) {
			CHECK(image->plane_count == 1);
			CHECK(image->level_count == 1);
			CHECK(image->tile_cache != nullptr);
			image_destroy(image);
			CHECK(loader_openslide_closes == 1);
		} else {
			CHECK(image->openslide_wsi.osr == nullptr);
			CHECK(image->tile_cache == nullptr);
		}
		free(image);
	}
}

TEST_CASE("archive owns MRXS ZIP bytes and serves metadata and frames without mounting the bundle") {
	source_test_directory root;
	auto bytes = tiny_mrxs_zip(false, false, true);
	std::string original = std::string(root.path) + "/original.zip";
	FILE* fp = fopen(original.c_str(), "wb"); REQUIRE(fp);
	REQUIRE(fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size()); REQUIRE(fclose(fp) == 0);
	archive_repository_t repository = {};
	char error[512] = {};
	std::string repo_path = std::string(root.path) + "/repo";
	REQUIRE(archive_repository_initialize(&repository, repo_path.c_str(), error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	REQUIRE(archive_instance_provider_initialize());
	archive_ingest_result_t result = {};
	REQUIRE_MESSAGE(archive_ingest_path(&repository, original.c_str(), false, &result, error, sizeof(error)), error);
	CHECK(result.instance_count == 1);
	// Replaying the same immutable bundle is idempotent.
	REQUIRE_MESSAGE(archive_ingest_path(&repository, original.c_str(), false, &result, error, sizeof(error)), error);
	REQUIRE(remove(original.c_str()) == 0);
	archive_index_t index = {};
	REQUIRE_MESSAGE(archive_index_open(&index, &repository, error, sizeof(error)), error);
	REQUIRE(index.instance_count == 1);
	const auto& record = index.instances[0];
	CHECK(strcmp(record.object_sha256, record.bundle_sha256) == 0);
	char blob_path[ARCHIVE_PATH_CAPACITY], shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, record.object_key, blob_path, sizeof(blob_path)));
	REQUIRE(archive_repository_make_path(&repository, record.shell_key, shell_path, sizeof(shell_path)));
	std::string hidden = std::string(blob_path) + ".hidden";
	REQUIRE(rename(blob_path, hidden.c_str()) == 0);
	char* json = nullptr; size_t json_size = 0;
	REQUIRE_MESSAGE(archive_instance_provider_get_metadata(&repository, &record, &json, &json_size, error, sizeof(error)), error);
	CHECK(json_size > 0); free(json);
	REQUIRE(rename(hidden.c_str(), blob_path) == 0);
	// Destroy the central-directory signature: frame access must use shell offsets only.
	const size_t central = signature(bytes, 0x02014b50);
	REQUIRE(central < bytes.size());
	fp = fopen(blob_path, "r+b"); REQUIRE(fp);
	REQUIRE(fseek(fp, (long)central, SEEK_SET) == 0); REQUIRE(fputc(0, fp) != EOF); REQUIRE(fclose(fp) == 0);
	archive_frame_representation_t representation = {};
	REQUIRE_MESSAGE(archive_instance_provider_get_frame_representations(&repository, &record, &representation, 1, error, sizeof(error)) == 1, error);
	std::atomic<int> completed(0);
	std::vector<std::thread> readers;
	for (int i = 0; i < 8; ++i) readers.emplace_back([&] {
		for (int j = 0; j < 4; ++j) {
			archive_instance_frame_t concurrent_frame = {};
			char concurrent_error[256] = {};
			if (archive_instance_provider_get_frame(&repository, &record, 1, &representation,
				&concurrent_frame, concurrent_error, sizeof(concurrent_error)) && concurrent_frame.size > 4 &&
				concurrent_frame.data[0] == 0xff && concurrent_frame.data[1] == 0xd8) ++completed;
			archive_instance_frame_destroy(&concurrent_frame);
		}
	});
	for (auto& reader : readers) reader.join();
	CHECK(completed.load() == 32);
	archive_instance_frame_t frame = {};
	REQUIRE_MESSAGE(archive_instance_provider_get_frame(&repository, &record, 1, &representation, &frame, error, sizeof(error)), error);
	CHECK(frame.size > 4); CHECK(frame.data[0] == 0xff); CHECK(frame.data[1] == 0xd8);
	archive_instance_frame_destroy(&frame);
	archive_instance_provider_shutdown();
	fp = fopen(blob_path, "r+b"); REQUIRE(fp);
	REQUIRE(fseek(fp, (long)central, SEEK_SET) == 0); REQUIRE(fputc(bytes[central], fp) != EOF); REQUIRE(fclose(fp) == 0);
	// Missing derived shells rebuild from the immutable bundle and canonical recipe.
	REQUIRE(remove(shell_path) == 0);
	REQUIRE(archive_instance_provider_initialize());
	REQUIRE_MESSAGE(archive_instance_provider_get_metadata(&repository, &record, &json, &json_size, error, sizeof(error)), error);
	free(json);
	fp = fopen(shell_path, "rb"); CHECK(fp); if (fp) fclose(fp);
	archive_instance_provider_shutdown();
	archive_index_destroy(&index);
}

static void source_test_mkdir(const std::string& path) {
#if defined(_WIN32)
	REQUIRE(_mkdir(path.c_str()) == 0);
#else
	REQUIRE(mkdir(path.c_str(), 0700) == 0);
#endif
}
static void source_test_write(const std::string& path, const void* data, size_t size) {
	FILE* fp = fopen(path.c_str(), "wb"); REQUIRE(fp);
	REQUIRE(fwrite(data, 1, size, fp) == size); REQUIRE(fclose(fp) == 0);
}

TEST_CASE("archive packages the complete MRXS directory including sidecars and optional thumbnail") {
	source_test_directory root;
	std::string folder = std::string(root.path) + "/slide";
	source_test_mkdir(folder); source_test_mkdir(folder + "/empty");
	auto bytes = tiny_mrxs_zip(false, false, true);
	auto blob = memory_blob(bytes); auto view = entire(blob);
	auto zip = source_zip_open(&view, nullptr, 0); REQUIRE(zip);
	for (const char* name : {"Slidedat.ini", "Index.dat", "Data0000.dat"}) {
		std::string member_path = std::string("wrapper/slide/") + name;
		auto member = source_zip_find(zip, member_path.c_str()); REQUIRE(member);
		std::vector<u8> data(member->file.size);
		REQUIRE(source_file_read(&member->file, 0, data.data(), data.size()) == SOURCE_READ_OK);
		source_test_write(folder + "/" + name, data.data(), data.size());
	}
	source_zip_close(zip);
	source_test_write(folder + "/.sidecar", "original", 8);
	source_test_write(folder + ".mrxs", "thumbnail", 9);
	archive_repository_t repository = {};
	char error[512] = {};
	std::string repo_path = std::string(root.path) + "/repo";
	REQUIRE(archive_repository_initialize(&repository, repo_path.c_str(), error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	archive_ingest_result_t result = {};
	REQUIRE_MESSAGE(archive_ingest_path(&repository, folder.c_str(), false, &result, error, sizeof(error)), error);
	archive_index_t index = {};
	REQUIRE(archive_index_open(&index, &repository, error, sizeof(error)));
	REQUIRE(index.instance_count == 1);
	char owned[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, index.instances[0].object_key, owned, sizeof(owned)));
	auto source = source_fs_open_zip(owned, error, sizeof(error)); REQUIRE_MESSAGE(source, error);
	source_entry_t entry;
	REQUIRE(source_fs_stat(source, "slide/empty", &entry)); CHECK(entry.is_directory);
	for (const char* name : {"slide/.sidecar", "slide.mrxs"}) {
		source_file_t file; REQUIRE(source_fs_open_file(source, name, &file));
		char value[16] = {}; REQUIRE(source_file_read(&file, 0, value, (size_t)file.size) == SOURCE_READ_OK);
		CHECK(strcmp(value, !strcmp(name, "slide.mrxs") ? "thumbnail" : "original") == 0);
	}
	source_fs_close(source); archive_index_destroy(&index);
	// Explicit tree capture uses the selected directory as the ZIP root and does
	// not bring in its sibling thumbnail. The same MRXS adapter admits this layout.
	REQUIRE_MESSAGE(archive_ingest_capture_directory(&repository, folder.c_str(), &result, error, sizeof(error)), error);
	REQUIRE(archive_index_open(&index, &repository, error, sizeof(error)));
	REQUIRE(index.instance_count == 2);
	bool found_root_capture = false;
	for (i32 i = 0; i < index.instance_count; ++i) {
		REQUIRE(archive_repository_make_path(&repository, index.instances[i].object_key, owned, sizeof(owned)));
		source = source_fs_open_zip(owned, error, sizeof(error)); REQUIRE_MESSAGE(source, error);
		if (source_fs_stat(source, "Slidedat.ini", &entry)) {
			found_root_capture = true;
			CHECK(source_fs_stat(source, "empty", &entry)); CHECK(entry.is_directory);
			CHECK(source_fs_stat(source, ".sidecar", &entry));
			CHECK_FALSE(source_fs_stat(source, "slide.mrxs", &entry));
		}
		source_fs_close(source);
	}
	CHECK(found_root_capture);
	archive_index_destroy(&index);
	// Removing the optional thumbnail leaves a valid directory-only bundle.
	REQUIRE(remove((folder + ".mrxs").c_str()) == 0);
	REQUIRE_MESSAGE(archive_ingest_path(&repository, folder.c_str(), false, &result, error, sizeof(error)), error);
#if !defined(_WIN32)
	// Link targets must not escape the bundle or silently disappear from preservation.
	REQUIRE(symlink("Slidedat.ini", (folder + "/linked.ini").c_str()) == 0);
	CHECK_FALSE(archive_ingest_path(&repository, folder.c_str(), false, &result, error, sizeof(error)));
	CHECK(strstr(error, "links") != nullptr);
#endif
}

static std::vector<u8> source_test_tiff() {
	// One uncompressed 2x2 RGB tile; extra tag values and pixels are out of line.
	std::vector<u8> bytes(192);
	put(bytes, 0, 0x4949, 2); put(bytes, 2, 42, 2); put(bytes, 4, 8, 4);
	put(bytes, 8, 10, 2);
	size_t at = 10;
	auto tag = [&](u16 id, u16 type, u32 count, u32 value) {
		put(bytes, at, id, 2); put(bytes, at+2, type, 2); put(bytes, at+4, count, 4); put(bytes, at+8, value, 4); at += 12;
	};
	tag(256, 4, 1, 2); tag(257, 4, 1, 2); tag(258, 3, 3, 140);
	tag(259, 3, 1, 1); tag(262, 3, 1, 2); tag(277, 3, 1, 3);
	tag(322, 4, 1, 2); tag(323, 4, 1, 2); tag(324, 4, 1, 180); tag(325, 4, 1, 12);
	put(bytes, 140, 8, 2); put(bytes, 142, 8, 2); put(bytes, 144, 8, 2);
	for (size_t i = 180; i < bytes.size(); i += 3) { bytes[i] = 10; bytes[i+1] = 20; bytes[i+2] = 30; }
	return bytes;
}

static std::vector<u8> source_test_dicom(int number, bool compressed = false, std::vector<u8>* encoded_frame = nullptr) {
	std::vector<u8> bytes(132); memcpy(bytes.data()+128, "DICM", 4);
	auto element = [&](u16 group, u16 tag, const char* vr, std::vector<u8> value) {
		if (value.size() & 1) value.push_back(!strcmp(vr, "UI") || !strcmp(vr, "OB") ? 0 : ' ');
		size_t at = bytes.size(); put(bytes, at, group, 2); put(bytes, at+2, tag, 2);
		bytes.push_back(vr[0]); bytes.push_back(vr[1]);
		if (!strcmp(vr, "OB")) { put(bytes, at+6, 0, 2); put(bytes, at+8, value.size(), 4); }
		else put(bytes, at+6, value.size(), 2);
		bytes.insert(bytes.end(), value.begin(), value.end());
	};
	auto text = [&](u16 group, u16 tag, const char* vr, std::string value) { element(group, tag, vr, {value.begin(), value.end()}); };
	auto us = [&](u16 tag, u16 value) { element(0x28, tag, "US", {(u8)value, (u8)(value >> 8)}); };
	std::string sop = "2.25." + std::to_string(300+number), series = "2.25." + std::to_string(200+number);
	const char* sop_class = "1.2.840.10008.5.1.4.1.1.77.1.6";
	text(2, 2, "UI", sop_class); text(2, 3, "UI", sop); text(2, 0x10, "UI", compressed ? "1.2.840.10008.1.2.4.50" : "1.2.840.10008.1.2.1");
	text(8, 8, "CS", "DERIVED\\PRIMARY\\VOLUME\\RESAMPLED");
	text(8, 0x16, "UI", sop_class); text(8, 0x18, "UI", sop); text(8, 0x60, "CS", "SM");
	text(0x20, 0x0d, "UI", "2.25.100"); text(0x20, 0x0e, "UI", series);
	text(0x20, 0x13, "IS", "1"); text(0x20, 0x9311, "CS", "TILED_FULL");
	us(2, 3); text(0x28, 4, "CS", "RGB"); us(6, 0); text(0x28, 8, "IS", "1");
	us(0x10, 2); us(0x11, 2); us(0x100, 8); us(0x101, 8); us(0x102, 7); us(0x103, 0);
	element(0x48, 6, "UL", {2,0,0,0}); element(0x48, 7, "UL", {2,0,0,0});
	auto sequence_begin = [&](u16 group, u16 tag) {
		size_t at = bytes.size(); put(bytes, at, group, 2); put(bytes, at+2, tag, 2);
		bytes.push_back('S'); bytes.push_back('Q'); put(bytes, at+6, 0, 6);
		put(bytes, at+12, 0xe000fffe, 4); put(bytes, at+16, 0, 4); return at;
	};
	auto sequence_end = [&](size_t at) { put(bytes, at+8, bytes.size()-at-12, 4); put(bytes, at+16, bytes.size()-at-20, 4); };
	size_t shared = sequence_begin(0x5200, 0x9229), measures = sequence_begin(0x28, 0x9110);
	text(0x28, 0x30, "DS", "0.00025\\0.00025"); sequence_end(measures); sequence_end(shared);
	if (!compressed) element(0x7fe0, 0x10, "OB", {10,20,30,10,20,30,10,20,30,10,20,30});
	else {
		u8 pixels[16] = {30,20,10,255,30,20,10,255,30,20,10,255,30,20,10,255};
		u8* jpeg = nullptr; u64 jpeg_size = 0;
		jpeg_encode_image(pixels, 2, 2, 90, &jpeg, &jpeg_size); REQUIRE(jpeg);
		std::vector<u8> payload(jpeg, jpeg + jpeg_size); free(jpeg);
		if (payload.size() & 1) payload.push_back(0);
		if (encoded_frame) *encoded_frame = payload;
		size_t at = bytes.size();
		put(bytes, at, 0x00107fe0, 4); put(bytes, at+4, 'O' | ('B' << 8), 4); put(bytes, at+8, UINT32_MAX, 4);
		put(bytes, at+12, 0xe000fffe, 4); put(bytes, at+16, 4, 4); put(bytes, at+20, 0, 4); // BOT
		size_t split = (payload.size() / 2) & ~(size_t)1;
		for (int fragment = 0; fragment < 2; ++fragment) {
			size_t start = fragment ? split : 0, end = fragment ? payload.size() : split;
			at = bytes.size(); put(bytes, at, 0xe000fffe, 4); put(bytes, at+4, end-start, 4);
			bytes.insert(bytes.end(), payload.begin()+start, payload.begin()+end);
		}
		at = bytes.size(); put(bytes, at, 0xe0ddfffe, 4); put(bytes, at+4, 0, 4);
	}
	return bytes;
}

TEST_CASE("viewer opens nested TIFF and multi-slide DICOM ZIP members through native bounded reads") {
	init_global_system_info(false);
	if (!threadlocal_thread_memory) init_thread_memory(&global_system_info);
	for (bool is_dicom : {false, true}) {
		CAPTURE(is_dicom);
		auto first = is_dicom ? source_test_dicom(1) : source_test_tiff();
		auto second = source_test_dicom(2);
		auto first_blob = memory_blob(first), second_blob = memory_blob(second);
		source_zip_input_t members[] = {{"nested/a/slide", entire(first_blob), false}, {"nested/b/slide", entire(second_blob), false}};
		std::vector<u8> bytes;
		REQUIRE(source_zip_write(members, is_dicom ? 2 : 1, memory_write, &bytes, nullptr, 0));
		source_test_file zip(bytes);
		file_info_t file = viewer_get_file_info(zip.path); file.type = VIEWER_FILE_TYPE_ZIP;
		for (int selected = 0; selected < (is_dicom ? 2 : 1); ++selected) {
			if (selected) strcpy(file.zip_member, "nested/b/slide");
			image_t* image = image_load_from_file(&file, nullptr, nullptr);
			REQUIRE(image); REQUIRE(image->is_valid);
			u8* pixels = nullptr;
			if (is_dicom) {
				CHECK(image->backend == IMAGE_BACKEND_DICOM);
				CHECK(arrlen(image->dicom.slides) == 2);
				CHECK(image->dicom.instances[0].series == &image->dicom);
				CHECK(image->dicom.selected_slide_index == selected);
				CHECK(strstr(image->name, selected ? "series-2.25.202.zip" : "series-2.25.201.zip") != nullptr);
				pixels = dicom_wsi_decode_tile_to_bgra(&image->dicom, 0, 0);
			} else {
				CHECK(image->backend == IMAGE_BACKEND_TIFF);
				pixels = tiff_decode_tile(0, &image->tiff, image->tiff.main_image_ifd, 0, 0, 0, 0);
			}
			REQUIRE(pixels);
			for (int i = 0; i < 4; ++i) {
				CHECK(pixels[i*4] == 30); CHECK(pixels[i*4+1] == 20); CHECK(pixels[i*4+2] == 10); CHECK(pixels[i*4+3] == 255);
			}
			free(pixels); image_destroy(image); free(image);
		}
	}
}

TEST_CASE("viewer rejects ambiguous and invalid TIFF ZIP members without OpenSlide fallback") {
	for (bool ambiguous : {false, true}) {
		auto tiff = source_test_tiff();
		if (!ambiguous) put(tiff, 4, 0, 4); // Header but no image directory.
		auto blob = memory_blob(tiff);
		source_zip_input_t members[] = {{"a.tif", entire(blob), false}, {"b.tif", entire(blob), false}};
		std::vector<u8> bytes;
		REQUIRE(source_zip_write(members, ambiguous ? 2 : 1, memory_write, &bytes, nullptr, 0));
		source_test_file temp(bytes);
		file_info_t file = viewer_get_file_info(temp.path); file.type = VIEWER_FILE_TYPE_ZIP;
		image_load_options_t options = {}; options.openslide_available = true;
		image_t* image = image_load_from_file(&file, nullptr, &options);
		REQUIRE(image); CHECK_FALSE(image->is_valid);
		free(image); // Failed loaders already released their backend/namespace.
	}
}

TEST_CASE("archive rejects damaged ZIP payloads before publishing a bundle") {
	source_test_directory root;
	auto bytes = tiny_mrxs_zip(false, false, true);
	auto blob = memory_blob(bytes); auto view = entire(blob);
	auto zip = source_zip_open(&view, nullptr, 0); REQUIRE(zip);
	auto member = source_zip_find(zip, "wrapper/slide/Data0000.dat"); REQUIRE(member);
	bytes[(size_t)member->file.offset + 3] ^= 1;
	source_zip_close(zip);
	std::string original = std::string(root.path) + "/damaged.zip";
	source_test_write(original, bytes.data(), bytes.size());
	archive_repository_t repository = {}; char error[512] = {};
	std::string repo_path = std::string(root.path) + "/repo";
	REQUIRE(archive_repository_initialize(&repository, repo_path.c_str(), error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	archive_ingest_result_t result = {};
	CHECK_FALSE(archive_ingest_path(&repository, original.c_str(), false, &result, error, sizeof(error)));
	CHECK(error[0] != 0);
	archive_index_t index = {};
	REQUIRE(archive_index_open(&index, &repository, error, sizeof(error)));
	CHECK(index.instance_count == 0);
	archive_index_destroy(&index);
}

TEST_CASE("DICOM ZIP archive publishes series atomically and reads shell ranges without a ZIP directory") {
	source_test_directory root;
	std::vector<u8> jpeg;
	auto first = source_test_dicom(1), second = source_test_dicom(2, true, &jpeg), third = source_test_dicom(3);
	// Two instances in one series; the third represents a second slide in another study.
	std::string series_old = "2.25.202", series_new = "2.25.201";
	auto found = std::search(second.begin(), second.end(), series_old.begin(), series_old.end());
	REQUIRE(found != second.end()); std::copy(series_new.begin(), series_new.end(), found);
	std::string study_old = "2.25.100", study_new = "2.25.101";
	found = std::search(third.begin(), third.end(), study_old.begin(), study_old.end());
	REQUIRE(found != third.end()); std::copy(study_new.begin(), study_new.end(), found);
	auto a = memory_blob(first), b = memory_blob(second), c = memory_blob(third);
	source_zip_input_t members[] = {{"nested/one/a", entire(a), false}, {"nested/one/b", entire(b), false}, {"two/c", entire(c), false}};
	std::vector<u8> bytes;
	REQUIRE(source_zip_write(members, 3, memory_write, &bytes, nullptr, 0));
	std::string original = std::string(root.path) + "/study.zip";
	source_test_write(original, bytes.data(), bytes.size());
	archive_repository_t repository = {}; char error[512] = {};
	std::string repo_path = std::string(root.path) + "/repo";
	REQUIRE(archive_repository_initialize(&repository, repo_path.c_str(), error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	archive_ingest_result_t result = {};
	REQUIRE_MESSAGE(archive_ingest_path(&repository, original.c_str(), false, &result, error, sizeof(error)), error);
	CHECK(result.instance_count == 3); CHECK(result.series_count == 2); CHECK(result.study_count == 2);
	REQUIRE_MESSAGE(archive_ingest_path(&repository, original.c_str(), false, &result, error, sizeof(error)), error);
	REQUIRE(remove(original.c_str()) == 0);
	archive_index_t index = {};
	REQUIRE_MESSAGE(archive_index_open(&index, &repository, error, sizeof(error)), error);
	REQUIRE(index.instance_count == 3);
	char owned[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, index.instances[0].object_key, owned, sizeof(owned)));
	std::string hidden = std::string(owned) + ".hidden";
	REQUIRE(rename(owned, hidden.c_str()) == 0);
	REQUIRE(archive_instance_provider_initialize());
	for (i32 i = 0; i < index.instance_count; ++i) {
		const auto& record = index.instances[i];
		CHECK(strcmp(record.object_sha256, index.instances[0].object_sha256) == 0);
		CHECK(strcmp(record.object_sha256, record.bundle_sha256) == 0);
		char* json = nullptr; size_t size = 0;
		REQUIRE_MESSAGE(archive_instance_provider_get_metadata(&repository, &record, &json, &size, error, sizeof(error)), error);
		CHECK(size > 0); free(json);
	}
	REQUIRE(rename(hidden.c_str(), owned) == 0);
	size_t central = signature(bytes, 0x02014b50); REQUIRE(central < bytes.size());
	FILE* fp = fopen(owned, "r+b"); REQUIRE(fp);
	REQUIRE(fseek(fp, (long)central, SEEK_SET) == 0); REQUIRE(fputc(0, fp) != EOF); REQUIRE(fclose(fp) == 0);
	for (i32 i = 0; i < index.instance_count; ++i) {
		const auto& record = index.instances[i];
		CAPTURE(record.sop_instance_uid);
		archive_frame_representation_t representations[2] = {};
		i32 count = archive_instance_provider_get_frame_representations(&repository, &record, representations, 2, error, sizeof(error));
		REQUIRE(count >= 1);
		archive_instance_frame_t frame = {};
		REQUIRE_MESSAGE(archive_instance_provider_get_frame(&repository, &record, 1, &representations[0], &frame, error, sizeof(error)), error);
		if (!strcmp(record.sop_instance_uid, "2.25.302")) {
			CHECK(frame.size == jpeg.size()); CHECK(memcmp(frame.data, jpeg.data(), jpeg.size()) == 0);
		} else {
			REQUIRE(frame.size == 12); CHECK(frame.data[0] == 10); CHECK(frame.data[11] == 30);
		}
		archive_instance_frame_destroy(&frame);
	}
	archive_instance_provider_shutdown();
	fp = fopen(owned, "r+b"); REQUIRE(fp);
	REQUIRE(fseek(fp, (long)central, SEEK_SET) == 0); REQUIRE(fputc(bytes[central], fp) != EOF); REQUIRE(fclose(fp) == 0);
	// Rebuild every series from the same blob; no original paths or member hashes are needed.
	for (i32 i = 0; i < index.instance_count; ++i) {
		char shell_path[ARCHIVE_PATH_CAPACITY];
		REQUIRE(archive_repository_make_path(&repository, index.instances[i].shell_key, shell_path, sizeof(shell_path)));
		remove(shell_path); // Two records intentionally share a shell.
	}
	REQUIRE(archive_instance_provider_initialize());
	for (i32 i = 0; i < index.instance_count; ++i) {
		char* json = nullptr; size_t size = 0;
		REQUIRE_MESSAGE(archive_instance_provider_get_metadata(&repository, &index.instances[i], &json, &size, error, sizeof(error)), error);
		free(json);
	}
	archive_instance_provider_shutdown(); archive_index_destroy(&index);
}

TEST_CASE("Part 10 shell translates 64-bit member offsets only after member-bounded validation") {
	auto bytes = source_test_dicom(1);
	auto blob = memory_blob(bytes); auto file = entire(blob);
	dicom_series_t dicom = {};
	REQUIRE(dicom_open_single_instance_from_source(&dicom, &file, "member"));
	const u64 offset = (u64)UINT32_MAX + 4096;
	archive_shell_build_part10_input_t input = {};
	input.instance = dicom.instances;
	input.source_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	input.representation_fingerprint = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	input.source_byte_size = offset + bytes.size() + 4096;
	input.member_offset = offset; input.member_size = bytes.size();
	archive_shell_buffer_t output = {}; char error[512] = {};
	REQUIRE_MESSAGE(archive_shell_build_part10(&input, &output, error, sizeof(error)), error);
	archive_shell_view_t shell = {}; archive_shell_instance_t instance = {}; archive_shell_payload_range_t range = {};
	REQUIRE(archive_shell_open(&shell, output.data, output.size, error, sizeof(error)));
	REQUIRE(archive_shell_get_instance(&shell, 0, &instance));
	REQUIRE(archive_shell_get_payload_range(&shell, &instance, 0, &range));
	CHECK(range.offset == offset + bytes.size() - 12); CHECK(range.size == 12);
	archive_shell_buffer_destroy(&output);
	// Adjacent ZIP bytes cannot make a truncated member look like a complete frame.
	input.member_size -= 1;
	CHECK_FALSE(archive_shell_build_part10(&input, &output, error, sizeof(error)));
	archive_shell_buffer_destroy(&output);
	input.member_offset = UINT64_MAX;
	CHECK_FALSE(archive_shell_build_part10(&input, &output, error, sizeof(error)));
	archive_shell_buffer_destroy(&output); dicom_destroy(&dicom);
}

// Four focus values, two zooms and nontrivial mask/filter dimensions. Stored focus
// values are deliberately out of Z order, and the declared default is physical zero.
static std::vector<u8> tiny_mrxs_focus_zip(int damage = 0, bool extra_coverage = false) {
	std::string ini = "[GENERAL]\nIMAGENUMBER_X=2\nIMAGENUMBER_Y=2\nCameraImageDivisionsPerSide=1\n"
		"[HIERARCHICAL]\nINDEXFILE=Index.dat\nHIER_COUNT=4\nNONHIER_COUNT=0\n"
		"HIER_0_NAME=Slide zoom level\nHIER_0_COUNT=2\n"
		"HIER_0_VAL_0=ZoomLevel_0\nHIER_0_VAL_0_SECTION=ZOOM0\n"
		"HIER_0_VAL_1=ZoomLevel_1\nHIER_0_VAL_1_SECTION=ZOOM1\n"
		"HIER_1_NAME=Slide zoom mask level2\nHIER_1_COUNT=2\n"
		"HIER_2_NAME=Slide filter level\nHIER_2_COUNT=3\n"
		"HIER_3_NAME=Microscope focus level\nHIER_3_COUNT=4\nHIER_3_DEFAULT=2\nHIER_3_SECTION=FOCUS\n"
		"HIER_3_VAL_0=ExtFocusLevel\nHIER_3_VAL_0_SECTION=F0\n"
		"HIER_3_VAL_1=ZStackLevel_(+1)\nHIER_3_VAL_1_SECTION=F1\n"
		"HIER_3_VAL_2=ZStackLevel_(0)\nHIER_3_VAL_2_SECTION=F2\n"
		"HIER_3_VAL_3=ZStackLevel_(-1)\nHIER_3_VAL_3_SECTION=F3\n"
		"[DATAFILE]\nFILE_COUNT=1\nFILE_0=Data0000.dat\n";
	for (int l = 0; l < 2; ++l) ini += "[ZOOM" + std::to_string(l) + "]\nDIGITIZER_WIDTH=2\nDIGITIZER_HEIGHT=2\nIMAGE_FORMAT=BMP24\n"
		"MICROMETER_PER_PIXEL_X=0.25\nMICROMETER_PER_PIXEL_Y=0.25\n";
	ini += "[FOCUS]\nEXTENDED_FOCUS_LEVEL=0\nZERO_FOCUS_LEVEL=2\n"
		"[F0]\nOFFSET_IN_MICROMETERS=0\n[F1]\nOFFSET_IN_MICROMETERS=0.5\n"
		"[F2]\nOFFSET_IN_MICROMETERS=0\n[F3]\nOFFSET_IN_MICROMETERS=-0.5\n";
	std::vector<u8> data;
	for (int focus = 0; focus < 4; ++focus) {
		std::vector<u8> bmp(70);
		put(bmp, 0, 0x4d42, 2); put(bmp, 2, 70, 4); put(bmp, 10, 54, 4); put(bmp, 14, 40, 4);
		put(bmp, 18, 2, 4); put(bmp, 22, 2, 4); put(bmp, 26, 1, 2); put(bmp, 28, 24, 2);
		for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
			bmp[54+y*8+x*3] = 30 + focus; bmp[55+y*8+x*3] = 20; bmp[56+y*8+x*3] = 10;
		}
		data.insert(data.end(), bmp.begin(), bmp.end());
	}
	std::vector<u8> index(45 + 48 * 4);
	memcpy(index.data(), "01.02", 5); put(index, 37, 45, 4); put(index, 41, 45, 4);
	for (int f = 0; f < 4; ++f) for (int l = 0; l < 2; ++l) {
		if (f == 3 && l == 1) continue; // Missing coarse plane, not shared extended-focus data.
		size_t page = index.size();
		put(index, 45 + (12*f+l)*4, page, 4);
		int n = l == 0 ? (extra_coverage && f == 0 ? 1 : f == 3 ? 3 : 4) : 1;
		put(index, page, n, 4); put(index, page+4, damage == 1 ? page : 0, 4);
		for (int i = 0; i < n; ++i) {
			size_t at = page + 8 + i*16;
			put(index, at, damage == 2 ? 99 : i, 4); put(index, at+4, f*70, 4);
			put(index, at+8, 70, 4); put(index, at+12, 0, 4);
		}
	}
	if (damage == 3) put(index, 45, index.size()-2, 4);
	std::vector<u8> text(ini.begin(), ini.end());
	auto ini_blob = memory_blob(text), index_blob = memory_blob(index), data_blob = memory_blob(data);
	source_zip_input_t members[] = {{"slide/Slidedat.ini", entire(ini_blob), false},
		{"slide/Index.dat", entire(index_blob), false}, {"slide/Data0000.dat", entire(data_blob), false}};
	std::vector<u8> output;
	REQUIRE(source_zip_write(members, 3, memory_write, &output, nullptr, 0));
	return output;
}

TEST_CASE("MRXS focus pyramids use mixed radix addressing and retain extended focus as default") {
	init_global_system_info(false);
	if (!threadlocal_thread_memory) init_thread_memory(&global_system_info);
	source_test_file temp(tiny_mrxs_focus_zip());
	mrxs_t slide = {};
	char error[256] = {};
	REQUIRE_MESSAGE(mrxs_open_from_zip(&slide, temp.path, error, sizeof(error)), error);
	CHECK(slide.levels[0].tiles == nullptr);
	REQUIRE(mrxs_index_all(&slide));
	CHECK(slide.plane_count == 4);
	CHECK(slide.zero_plane == 2);
	CHECK(slide.planes[0].is_extended_focus);
	CHECK(slide.planes[1].offset_um == doctest::Approx(-0.5));
	CHECK(slide.planes[2].offset_um == 0);
	CHECK(slide.planes[3].offset_um == doctest::Approx(0.5));
	const int expected[] = {30,33,32,31};
	for (int p = 0; p < 4; ++p) {
		u8* pixels = mrxs_decode_tile_to_bgra(&slide, p, 0, 0);
		REQUIRE(pixels); CHECK(pixels[0] == expected[p]); free(pixels);
	}
	CHECK(slide.extra_tile_warning_count == 0);
	for (int i = 0; i < 10; ++i) CHECK(mrxs_decode_tile_to_bgra(&slide, 1, 0, 3) == nullptr);
	CHECK(slide.missing_tile_warning_count == 5); // Warning budget saturates, even on repeated reads.
	CHECK(mrxs_tile_is_empty(&slide, 1, 0, 3));
	CHECK_FALSE(mrxs_tile_is_empty(&slide, 2, 0, 3));
	CHECK_FALSE(slide.planes[1].level_available[1]);
	CHECK(mrxs_decode_tile_to_bgra(&slide, 1, 1, 0) == nullptr);
	CHECK(mrxs_decode_tile_to_bgra(&slide, 4, 0, 0) == nullptr);
	mrxs_destroy(&slide);
	file_info_t info = viewer_get_file_info(temp.path); info.type = VIEWER_FILE_TYPE_ZIP;
	image_load_options_t options = {};
	image_t* image = image_load_from_file(&info, nullptr, &options);
	REQUIRE(image);
	CHECK(image->selected_plane == 0);
	CHECK(image->zstack_plane == 2);
	CHECK(image->level_images[0].missing_planes == 0);
	CHECK(image->level_images[1].missing_planes == (UINT64_C(1) << 1));
	image_select_plane(image, image->zstack_plane);
	CHECK(image->selected_plane == 2);
	image_select_plane(image, 3); CHECK(image->plane_approach_direction == -1);
	image_select_plane(image, 2); CHECK(image->plane_approach_direction == 1);
	image_select_plane(image, 0); CHECK(image->zstack_plane == 2);
	REQUIRE(archive_instance_provider_initialize());
	for (int p = 0; p < 4; ++p) {
		image_select_plane(image, (p+1)%4);
		u8 pixels[16] = {};
		REQUIRE(image_read_region(image, p, 0, 0, 0, 2, 2, pixels, PIXEL_FORMAT_U8_BGRA));
		CHECK(pixels[0] == expected[p]);
	}
	u8 empty[16] = {};
	REQUIRE(image_read_region(image, 1, 0, 2, 2, 2, 2, empty, PIXEL_FORMAT_U8_BGRA));
	CHECK(empty[0] == 255);
	CHECK(image->mrxs.missing_tile_warning_count == 1);
	CHECK_FALSE(image->level_images[0].tiles[3].is_empty);
	REQUIRE(image_read_region(image, 2, 0, 2, 2, 2, 2, empty, PIXEL_FORMAT_U8_BGRA));
	CHECK(empty[0] == 32); // Missing data in one plane does not suppress another plane.

	image_destroy(image); free(image);
}

TEST_CASE("MRXS shared coverage ignores extra physical-plane tiles and caps warnings") {
	init_global_system_info(false);
	if (!threadlocal_thread_memory) init_thread_memory(&global_system_info);
	source_test_file temp(tiny_mrxs_focus_zip(0, true));
	file_info_t info = viewer_get_file_info(temp.path); info.type = VIEWER_FILE_TYPE_ZIP;
	image_load_options_t options = {};
	image_t* image = image_load_from_file(&info, nullptr, &options);
	REQUIRE(image);
	REQUIRE(image_wait_for_level_index(image, image->plane_count - 1, 0));
	CHECK(image->mrxs.extra_tile_warning_count == 5); // Eight extra entries, only five warnings.
	CHECK(image->level_images[0].tiles[1].is_empty);
	u8* decoded = mrxs_decode_tile_to_bgra(&image->mrxs, 2, 0, 1);
	REQUIRE(decoded); CHECK(decoded[0] == 32); free(decoded);
	REQUIRE(archive_instance_provider_initialize());
	u8 pixels[16] = {};
	REQUIRE(image_read_region(image, 2, 0, 2, 0, 2, 2, pixels, PIXEL_FORMAT_U8_BGRA));
	CHECK(pixels[0] == 255); // Deliberately skip data outside plane-zero coverage.
	CHECK(image->level_images[0].tiles[1].is_empty);
	image_destroy(image); free(image);
}

TEST_CASE("MRXS focus indexes reject cycles out of range tiles and truncated pages") {
	for (int damage = 1; damage <= 3; ++damage) {
		source_test_file temp(tiny_mrxs_focus_zip(damage));
		mrxs_t slide = {}; char error[256] = {};
		REQUIRE(mrxs_open_from_zip(&slide, temp.path, error, sizeof(error)));
		CHECK_FALSE(mrxs_index_all(&slide));
		mrxs_destroy(&slide);
	}
}

// No automatic consumers: tests can inspect the queued job before letting it run.
struct mrxs_test_pool {
	thread_pool_t pool = {};
	mrxs_test_pool() {
		init_global_system_info(false);
		if (!threadlocal_thread_memory) {
			init_thread_memory(&global_system_info);
		}
		i32 count = global_system_info.suggested_total_thread_count;
		global_system_info.suggested_total_thread_count = 1;
		init_thread_pool(&pool, 8, true, false, nullptr);
		global_system_info.suggested_total_thread_count = count;
	}
	~mrxs_test_pool() {
		thread_pool_destroy(&pool);
	}
};

TEST_CASE("MRXS indexing is one high priority job and region reads wait for it") {
	REQUIRE(archive_instance_provider_initialize());
	for (bool focus : {false, true}) {
		mrxs_test_pool worker;
		source_test_file temp(focus ? tiny_mrxs_focus_zip() : tiny_mrxs_zip());
		file_info_t info = viewer_get_file_info(temp.path);
		info.type = VIEWER_FILE_TYPE_ZIP;
		image_load_options_t options = {};
		options.thread_pool = &worker.pool;
		CHECK(thread_pool_submit_task(&worker.pool, dummy_work_queue_callback, nullptr, 0));
		image_t* image = image_load_from_file(&info, nullptr, &options);
		REQUIRE(image);
		CHECK(work_queue_get_entry_count(worker.pool.high_priority_queue) == 1);
		CHECK_FALSE(task_group_is_complete(&image->indexing_task));
		CHECK_FALSE(atomic_load_acquire(&image->indexing_finished));
		CHECK(image->level_images[0].tiles == nullptr);
		CHECK_FALSE(tile_cache_try_begin_decode(image, 0, 0, 0, 0, 0, 0));
		CHECK(tile_cache_get_focus_texture(image, 0, 0, false) == 0);
		u8 pixels[16] = {};
		CHECK(image_read_region(image, 0, 0, 0, 0, 2, 2, pixels, PIXEL_FORMAT_U8_BGRA));
		CHECK(atomic_load_acquire(&image->indexing_finished));
		CHECK_FALSE(atomic_load_acquire(&image->indexing_failed));
		CHECK(task_group_is_complete(&image->indexing_task));
		CHECK(work_queue_get_entry_count(worker.pool.queue) == 1); // Indexing overtook normal work.
		for (int level = 0; level < image->level_count; ++level) {
			for (int plane = 0; plane < image->plane_count; ++plane) {
				CHECK(image_level_index_ready(image, plane, level));
			}
		}
		image_destroy(image);
		free(image);
	}
}

struct mrxs_index_gate {
	source_file_t file;
	u64 offset;
	std::atomic<bool> entered{false};
	std::atomic<bool> release{false};
};

static source_read_result_t gated_mrxs_index_read(void* context, u64 offset, void* data, size_t size) {
	auto* gate = (mrxs_index_gate*)context;
	if (offset == gate->offset) {
		gate->entered.store(true);
		while (!gate->release.load()) {
			platform_sleep(1);
		}
	}
	return source_file_read(&gate->file, offset, data, size);
}

TEST_CASE("MRXS publishes coarse levels before fine indexes finish") {
	mrxs_test_pool worker;
	source_test_file temp(tiny_mrxs_focus_zip());
	file_info_t info = viewer_get_file_info(temp.path);
	info.type = VIEWER_FILE_TYPE_ZIP;
	image_load_options_t options = {};
	options.thread_pool = &worker.pool;
	image_t* image = image_load_from_file(&info, nullptr, &options);
	REQUIRE(image);
	mrxs_index_gate gate;
	gate.file = image->mrxs.index_file;
	gate.offset = image->mrxs.planes[0].index_pages[0];
	source_blob_t blob = {gate.file.size, &gate, gated_mrxs_index_read, nullptr};
	CHECK(source_file_view(&blob, 0, blob.size, &image->mrxs.index_file));
	std::thread indexing([&] {
		init_thread_memory(&global_system_info);
		thread_pool_do_work(&worker.pool);
		destroy_thread_memory();
	});
	i64 start = get_clock();
	while (!gate.entered.load() && get_seconds_elapsed(start, get_clock()) < 5) {
		platform_sleep(1);
	}
	CHECK(gate.entered.load());
	for (int plane = 0; plane < image->plane_count; ++plane) {
		CHECK(image_level_index_ready(image, plane, 1));
		CHECK_FALSE(image_level_index_ready(image, plane, 0));
	}
	CHECK_FALSE(atomic_load_acquire(&image->indexing_finished));
	gate.release.store(true);
	indexing.join();
	CHECK(image_wait_for_level_index(image, 3, 0));
	image_destroy(image);
	free(image);
}

TEST_CASE("MRXS pending indexing can be cancelled and malformed indexes terminate waiting reads") {
	for (int damage = 0; damage <= 3; ++damage) {
		mrxs_test_pool worker;
		source_test_file temp(tiny_mrxs_focus_zip(damage));
		file_info_t info = viewer_get_file_info(temp.path);
		info.type = VIEWER_FILE_TYPE_ZIP;
		image_load_options_t options = {};
		options.thread_pool = &worker.pool;
		image_t* image = image_load_from_file(&info, nullptr, &options);
		REQUIRE(image);
		if (damage) {
			u8 pixels[16] = {};
			CHECK_FALSE(image_read_region(image, 0, 0, 0, 0, 2, 2, pixels, PIXEL_FORMAT_U8_BGRA));
			CHECK(atomic_load_acquire(&image->indexing_finished));
			CHECK(atomic_load_acquire(&image->indexing_failed));
			CHECK_FALSE(image_level_index_ready(image, 0, 0));
			CHECK(image->level_images[0].tiles == nullptr);
			if (damage == 3) {
				CHECK(image_level_index_ready(image, 0, 1));
			}
		}
		image_destroy(image);
		free(image);
		CHECK_FALSE(thread_pool_is_work_in_progress(&worker.pool));
	}
}
