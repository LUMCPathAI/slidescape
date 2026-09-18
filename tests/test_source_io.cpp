#include "doctest.h"
#include "source_fs.h"
#include "miniz.h"
#include "mrxs.h"
#include "image_loader.h"
#include "test_temp.h"
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

static std::vector<u8> tiny_mrxs_zip(bool duplicate = false, bool omit_data = false) {
	const char* ini = "[GENERAL]\nIMAGENUMBER_X=1\nIMAGENUMBER_Y=1\nCameraImageDivisionsPerSide=1\n"
		"[HIERARCHICAL]\nINDEXFILE=Index.dat\nHIER_COUNT=1\nNONHIER_COUNT=0\n"
		"HIER_0_NAME=Slide zoom level\nHIER_0_COUNT=1\nHIER_0_VAL_0=ZoomLevel_0\nHIER_0_VAL_0_SECTION=ZOOM\n"
		"[DATAFILE]\nFILE_COUNT=1\nFILE_0=Data0000.dat\n"
		"[ZOOM]\nDIGITIZER_WIDTH=2\nDIGITIZER_HEIGHT=2\nIMAGE_FORMAT=BMP24\n"
		"MICROMETER_PER_PIXEL_X=0.25\nMICROMETER_PER_PIXEL_Y=0.25\n";
	std::vector<u8> ini_bytes(ini, ini+strlen(ini));
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
	CHECK(slide.level_count == 1);
	CHECK(slide.base_width_in_pixels == 2);
	CHECK(slide.base_height_in_pixels == 2);
	CHECK(slide.mpp_x == doctest::Approx(0.25));
	CHECK(slide.dat_files[0].size == 73);
	CHECK(slide.dat_files[0].offset > 0);
	u8* pixels = mrxs_decode_tile_to_bgra(&slide, 0, 0);
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
