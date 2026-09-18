/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "source_zip.h"
#include "miniz.h"

/* ZIP layout constants belong here; image backends only see source_file_t. */
enum {
	ZIP_LOCAL_HEADER_SIZE = 30,
	ZIP_FLAG_DESCRIPTOR = 1 << 3,
	ZIP_FLAG_UTF8 = 1 << 11,
	ZIP_UNIX_FILE_TYPE_MASK = 0170000,
	ZIP_UNIX_REGULAR = 0100000,
	ZIP_UNIX_DIRECTORY = 0040000,
	ZIP_VALIDATION_CHUNK = 64 * 1024,
};

struct source_zip_t {
	source_zip_member_t* members;
	u32 count;
};

static void zip_error(char* error, size_t capacity, const char* message) {
	if (error && capacity) snprintf(error, capacity, "%s", message);
}

static u16 zip_u16(const u8* p) { return (u16)(p[0] | (u16)p[1] << 8); }
static u32 zip_u32(const u8* p) { return (u32)zip_u16(p) | (u32)zip_u16(p + 2) << 16; }
static u64 zip_u64(const u8* p) { return (u64)zip_u32(p) | (u64)zip_u32(p + 4) << 32; }
static unsigned char path_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

/* ASCII case collisions are rejected, but lookup preserves the original spelling.
   Non-ASCII paths are UTF-8, not silently normalized or renamed. */
static int path_compare(const char* a, const char* b) {
	for (;;) {
		unsigned char x = path_lower((unsigned char)*a);
		unsigned char y = path_lower((unsigned char)*b);
		if (x != y) return x < y ? -1 : 1;
		if (!x) return 0;
		++a; ++b;
	}
}

static bool valid_utf8(const unsigned char* p) {
	while (*p) {
		u32 c = *p++;
		if (c < 128) continue;
		u32 value, minimum;
		int following;
		if (c >= 0xc2 && c <= 0xdf) { value = c & 31; following = 1; minimum = 0x80; }
		else if (c >= 0xe0 && c <= 0xef) { value = c & 15; following = 2; minimum = 0x800; }
		else if (c >= 0xf0 && c <= 0xf4) { value = c & 7; following = 3; minimum = 0x10000; }
		else return false;
		while (following--) {
			if ((*p & 0xc0) != 0x80) return false;
			value = value << 6 | (*p++ & 63);
		}
		if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
	}
	return true;
}

bool source_path_is_valid(const char* path, bool is_directory) {
	if (!path || !*path || strlen(path) >= SOURCE_PATH_CAPACITY || !valid_utf8((const unsigned char*)path)) return false;
	size_t length = strlen(path);
	if ((path[length - 1] == '/') != is_directory) return false;
	const char* component = path;
	for (const char* at = path;; ++at) {
		unsigned char c = (unsigned char)*at;
		if (c && (c < 32 || c == 127 || strchr("\\:<>\"|?*", c))) return false;
		if (!c || c == '/') {
			size_t n = (size_t)(at - component);
			if (!n || component[n - 1] == '.' || component[n - 1] == ' ') return false;
			/* Avoid aliases and device names when re-exported on Windows. */
			char stem[5] = {0};
			size_t s = 0;
			while (s < n && component[s] != '.' && s < 4) { stem[s] = (char)path_lower(component[s]); ++s; }
			if (s == n || component[s] == '.') {
				if (!strcmp(stem, "con") || !strcmp(stem, "prn") || !strcmp(stem, "aux") || !strcmp(stem, "nul") ||
					(s == 4 && (!memcmp(stem, "com", 3) || !memcmp(stem, "lpt", 3)) && stem[3] >= '1' && stem[3] <= '9')) return false;
			}
			if (!c || !at[1]) break;
			component = at + 1;
		}
	}
	return true;
}

static int member_compare(const void* a, const void* b) {
	return path_compare(((const source_zip_member_t*)a)->path, ((const source_zip_member_t*)b)->path);
}

const source_zip_member_t* source_zip_find(const source_zip_t* zip, const char* path) {
	if (!zip || !path) return NULL;
	u32 lo = 0, hi = zip->count;
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		int c = path_compare(zip->members[mid].path, path);
		if (c < 0) lo = mid + 1;
		else if (c > 0) hi = mid;
		else return zip->members + mid;
	}
	return NULL;
}

static bool zip_check_names(source_zip_t* zip) {
	qsort(zip->members, zip->count, sizeof(*zip->members), member_compare);
	for (u32 i = 0; i < zip->count; ++i) {
		const char* path = zip->members[i].path;
		if (i) {
			const char* previous = zip->members[i - 1].path;
			if (!path_compare(path, previous)) return false;
			/* Implicit parent directories must have one spelling as well. */
			bool spelling_differs = false;
			for (size_t at = 0; path[at] && previous[at] &&
				path_lower(path[at]) == path_lower(previous[at]); ++at) {
				if (path[at] != previous[at]) spelling_differs = true;
				if (path[at] == '/' && spelling_differs) return false;
			}
		}
		char prefix[SOURCE_PATH_CAPACITY];
		strcpy(prefix, path);
		for (char* at = prefix; *at; ++at) {
			if (*at != '/') continue;
			*at = 0;
			/* Files may not act as parents, including an explicit "file/". */
			if (source_zip_find(zip, prefix)) return false;
			*at = '/';
		}
	}
	return true;
}

/* Bound miniz's allocations before it trusts the directory's advertised size. */
typedef union zip_allocation_t { size_t size; long double alignment; void* pointer_alignment; } zip_allocation_t;
typedef struct zip_budget_t { size_t used; } zip_budget_t;
static void* zip_realloc(void* context, void* pointer, size_t items, size_t size) {
	zip_budget_t* budget = context;
	if (size && items > SOURCE_ZIP_INDEX_BUDGET / size) return NULL;
	size_t bytes = items * size;
	zip_allocation_t* old = pointer ? (zip_allocation_t*)pointer - 1 : NULL;
	size_t previous = old ? old->size : 0;
	if (bytes > SOURCE_ZIP_INDEX_BUDGET - (budget->used - previous)) return NULL;
	zip_allocation_t* next = realloc(old, sizeof(*next) + bytes);
	if (!next) return NULL;
	next->size = bytes;
	budget->used = budget->used - previous + bytes;
	return next + 1;
}
static void* zip_alloc(void* context, size_t items, size_t size) { return zip_realloc(context, NULL, items, size); }
static void zip_free(void* context, void* pointer) {
	if (!pointer) return;
	zip_budget_t* budget = context;
	zip_allocation_t* allocation = (zip_allocation_t*)pointer - 1;
	budget->used -= allocation->size;
	free(allocation);
}
static void zip_set_allocator(mz_zip_archive* archive, zip_budget_t* budget) {
	archive->m_pAlloc = zip_alloc;
	archive->m_pRealloc = zip_realloc;
	archive->m_pFree = zip_free;
	archive->m_pAlloc_opaque = budget;
}
static size_t zip_read(void* context, mz_uint64 offset, void* dest, size_t size) {
	return source_file_read(context, offset, dest, size) == SOURCE_READ_OK ? size : 0;
}

typedef struct zip_extent_t { u64 start, end; } zip_extent_t;
static int extent_compare(const void* a, const void* b) {
	u64 x = ((const zip_extent_t*)a)->start, y = ((const zip_extent_t*)b)->start;
	return x < y ? -1 : x > y;
}

/* miniz's header validator chooses descriptor width from the whole archive's
   ZIP64 flag. Mixed ZIP32/ZIP64 entries are legal, including miniz's own output.
   Validate this small local-header boundary here using the member's actual sizes. */
static bool zip_validate_local(const source_file_t* container, const u8* header,
		u64 extra_offset, u16 extra_size, u64 data_end, u64 directory_offset,
		const mz_zip_archive_file_stat* stat, u64* record_end) {
	u64 compressed = zip_u32(header + 18), size = zip_u32(header + 22);
	bool need_size = size == UINT32_MAX, need_compressed = compressed == UINT32_MAX;
	bool found_zip64 = false;
	u8* extra = extra_size ? malloc(extra_size) : NULL;
	if (extra_size && (!extra || source_file_read(container, extra_offset, extra, extra_size) != SOURCE_READ_OK)) {
		free(extra); return false;
	}
	bool valid = true;
	for (size_t at = 0; at < extra_size;) {
		if (extra_size - at < 4) { valid = false; break; }
		u16 id = zip_u16(extra + at), bytes = zip_u16(extra + at + 2);
		at += 4;
		if (bytes > extra_size - at) { valid = false; break; }
		if (id == 1) {
			if (found_zip64 || bytes < (need_size + need_compressed) * 8) { valid = false; break; }
			found_zip64 = true;
			size_t pos = at;
			if (need_size) { size = zip_u64(extra + pos); pos += 8; }
			if (need_compressed) compressed = zip_u64(extra + pos);
		}
		at += bytes;
	}
	free(extra);
	if (!valid || ((need_size || need_compressed) && !found_zip64)) return false;
	u32 crc = zip_u32(header + 14);
	*record_end = data_end;
	if (!(stat->m_bit_flag & ZIP_FLAG_DESCRIPTOR))
		return size == stat->m_uncomp_size && compressed == stat->m_comp_size && crc == stat->m_crc32;
	if ((size && size != stat->m_uncomp_size) || (compressed && compressed != stat->m_comp_size) ||
		(crc && crc != stat->m_crc32)) return false;
	u8 descriptor[24];
	size_t available = (size_t)MIN(directory_offset - data_end, sizeof(descriptor));
	if (available < 12 || source_file_read(container, data_end, descriptor, available) != SOURCE_READ_OK) return false;
	for (size_t prefix = 0; prefix <= 4; prefix += 4) {
		if (prefix && zip_u32(descriptor) != 0x08074b50) continue;
		if (available < prefix + 12 || zip_u32(descriptor + prefix) != stat->m_crc32) continue;
		const u8* sizes = descriptor + prefix + 4;
		if (stat->m_uncomp_size <= UINT32_MAX && zip_u32(sizes) == stat->m_comp_size &&
			zip_u32(sizes + 4) == stat->m_uncomp_size) {
			*record_end += prefix + 12; return true;
		}
		if (available >= prefix + 20 && zip_u64(sizes) == stat->m_comp_size && zip_u64(sizes + 8) == stat->m_uncomp_size) {
			*record_end += prefix + 20; return true;
		}
	}
	return false;
}

source_zip_t* source_zip_open(const source_file_t* container, char* error, size_t error_capacity) {
	zip_error(error, error_capacity, "Invalid or unsupported stored ZIP");
	if (!container || !container->blob) return NULL;
	mz_zip_archive reader = {0};
	zip_budget_t budget = {0};
	zip_set_allocator(&reader, &budget);
	reader.m_pRead = zip_read;
	reader.m_pIO_opaque = (void*)container;
	if (!mz_zip_reader_init(&reader, container->size, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) return NULL;
	source_zip_t* zip = calloc(1, sizeof(*zip));
	zip_extent_t* extents = NULL;
	bool success = false;
	if (!zip || reader.m_total_files > SOURCE_ZIP_MAX_MEMBERS ||
		reader.m_total_files > SOURCE_ZIP_INDEX_BUDGET / (sizeof(source_zip_member_t) + sizeof(zip_extent_t))) goto done;
	zip->members = calloc(reader.m_total_files ? reader.m_total_files : 1, sizeof(*zip->members));
	extents = calloc(reader.m_total_files ? reader.m_total_files : 1, sizeof(*extents));
	if (!zip->members || !extents) goto done;
	size_t index_bytes = (size_t)reader.m_total_files * (sizeof(*zip->members) + sizeof(*extents));
	for (u32 i = 0; i < reader.m_total_files; ++i) {
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&reader, i, &stat)) goto done;
		u32 path_size = mz_zip_reader_get_filename(&reader, i, NULL, 0);
		if (path_size < 2 || path_size > SOURCE_PATH_CAPACITY || path_size > SOURCE_ZIP_INDEX_BUDGET - index_bytes) goto done;
		index_bytes += path_size;
		source_zip_member_t* member = zip->members + i;
		zip->count = i + 1; /* Own partial allocations on failure. */
		member->path = malloc(path_size);
		if (!member->path || mz_zip_reader_get_filename(&reader, i, member->path, path_size) != path_size ||
			strlen(member->path) != path_size - 1) goto done;
		member->is_directory = stat.m_is_directory != 0;
		u32 unix_type = (stat.m_external_attr >> 16) & ZIP_UNIX_FILE_TYPE_MASK;
		if (!source_path_is_valid(member->path, member->is_directory) || stat.m_method != 0 ||
			stat.m_is_encrypted || !stat.m_is_supported || stat.m_comp_size != stat.m_uncomp_size ||
			(stat.m_bit_flag & ~(ZIP_FLAG_DESCRIPTOR | ZIP_FLAG_UTF8)) ||
			(unix_type && unix_type != (member->is_directory ? ZIP_UNIX_DIRECTORY : ZIP_UNIX_REGULAR)) ||
			(member->is_directory && stat.m_uncomp_size)) goto done;
		u8 header[ZIP_LOCAL_HEADER_SIZE];
		if (source_file_read(container, stat.m_local_header_ofs, header, sizeof(header)) != SOURCE_READ_OK ||
			zip_u32(header) != 0x04034b50 || zip_u16(header + 6) != stat.m_bit_flag ||
			zip_u16(header + 8) != 0 || zip_u16(header + 26) != path_size - 1) goto done;
		u64 start = stat.m_local_header_ofs + sizeof(header);
		u64 extra = (u64)zip_u16(header + 26) + zip_u16(header + 28);
		if (start > reader.m_central_directory_file_ofs || extra > reader.m_central_directory_file_ofs - start) goto done;
		char name[SOURCE_PATH_CAPACITY];
		if (source_file_read(container, start, name, path_size - 1) != SOURCE_READ_OK ||
			memcmp(name, member->path, path_size - 1)) goto done;
		start += extra;
		if (stat.m_uncomp_size > reader.m_central_directory_file_ofs - start ||
			!source_file_view(container->blob, container->offset + start, stat.m_uncomp_size, &member->file)) goto done;
		u64 record_end;
		if (!zip_validate_local(container, header, stat.m_local_header_ofs + sizeof(header) + path_size - 1,
			zip_u16(header + 28), start + stat.m_uncomp_size, reader.m_central_directory_file_ofs, &stat, &record_end)) {
			if (error && error_capacity) snprintf(error, error_capacity, "Inconsistent ZIP local header or descriptor: %s", member->path);
			goto done;
		}
		member->crc32 = stat.m_crc32;
		extents[i] = (zip_extent_t){stat.m_local_header_ofs, record_end};
	}
	qsort(extents, zip->count, sizeof(*extents), extent_compare);
	for (u32 i = 1; i < zip->count; ++i) if (extents[i].start < extents[i - 1].end) goto done;
	if (!zip_check_names(zip)) goto done;
	success = true;
	done:
	free(extents);
	mz_zip_reader_end(&reader);
	if (!success) { source_zip_close(zip); return NULL; }
	zip_error(error, error_capacity, "");
	return zip;
}

void source_zip_close(source_zip_t* zip) {
	if (!zip) return;
	for (u32 i = 0; i < zip->count; ++i) free(zip->members[i].path);
	free(zip->members);
	free(zip);
}
u32 source_zip_count(const source_zip_t* zip) { return zip ? zip->count : 0; }
const source_zip_member_t* source_zip_member(const source_zip_t* zip, u32 index) {
	return zip && index < zip->count ? zip->members + index : NULL;
}

bool source_zip_validate_contents(const source_zip_t* zip, char* error, size_t capacity) {
	if (!zip) return false;
	u8* buffer = malloc(ZIP_VALIDATION_CHUNK);
	if (!buffer) { zip_error(error, capacity, "Cannot allocate ZIP validation buffer"); return false; }
	bool success = true;
	for (u32 i = 0; i < zip->count && success; ++i) {
		const source_zip_member_t* member = zip->members + i;
		mz_ulong crc = MZ_CRC32_INIT;
		for (u64 offset = 0; offset < member->file.size;) {
			size_t size = (size_t)MIN(member->file.size - offset, ZIP_VALIDATION_CHUNK);
			if (source_file_read(&member->file, offset, buffer, size) != SOURCE_READ_OK) { success = false; break; }
			crc = mz_crc32(crc, buffer, size);
			offset += size;
		}
		if (crc != member->crc32) success = false;
		if (!success && error && capacity) snprintf(error, capacity, "ZIP content validation failed: %s", member->path);
	}
	free(buffer);
	if (success) zip_error(error, capacity, "");
	return success;
}

typedef struct zip_sink_t { source_zip_write_fn write; void* context; u64 offset; bool failed; } zip_sink_t;
static size_t zip_write_callback(void* context, mz_uint64 offset, const void* data, size_t size) {
	zip_sink_t* sink = context;
	if (sink->failed) return 0;
	if (offset != sink->offset || size > UINT64_MAX - offset || !sink->write(sink->context, data, size)) {
		sink->failed = true;
		return 0;
	}
	sink->offset += size;
	return size;
}

typedef struct zip_input_reader_t { const source_file_t* file; u64 read; bool failed; } zip_input_reader_t;
static size_t zip_input_read(void* context, mz_uint64 offset, void* dest, size_t size) {
	zip_input_reader_t* input = context;
	if (input->failed || offset != input->read || offset > input->file->size) { input->failed = true; return 0; }
	/* Unlike random-access ZIP metadata reads, miniz's writer asks for a full
	   buffer even at EOF and requires the final read to return a partial buffer. */
	size = (size_t)MIN((u64)size, input->file->size - offset);
	if (source_file_read(input->file, offset, dest, size) != SOURCE_READ_OK) { input->failed = true; return 0; }
	input->read += size;
	return size;
}

bool source_zip_write(const source_zip_input_t* members, u32 count,
		source_zip_write_fn write, void* context, char* error, size_t capacity) {
	zip_error(error, capacity, "Could not write stored ZIP64; discard staged output");
	if (!write || (count && !members) || count > SOURCE_ZIP_MAX_MEMBERS ||
		count > SOURCE_ZIP_INDEX_BUDGET / sizeof(source_zip_member_t)) return false;
	/* Validate the entire namespace before producing bytes. Reuse the reader's
	   collision policy without taking ownership of the caller's strings. */
	source_zip_t names = {0};
	names.count = count;
	names.members = calloc(count ? count : 1, sizeof(*names.members));
	if (!names.members) return false;
	bool valid = true;
	for (u32 i = 0; i < count; ++i) {
		names.members[i].path = (char*)members[i].path;
		if (!source_path_is_valid(members[i].path, members[i].is_directory) ||
			(!members[i].is_directory && source_file_read(&members[i].file, 0, NULL, 0) != SOURCE_READ_OK)) { valid = false; break; }
	}
	if (valid) valid = zip_check_names(&names);
	free(names.members);
	if (!valid) { zip_error(error, capacity, "Invalid or ambiguous ZIP member paths"); return false; }
	mz_zip_archive writer = {0};
	zip_budget_t budget = {0};
	zip_set_allocator(&writer, &budget);
	zip_sink_t sink = {write, context, 0, false};
	writer.m_pWrite = zip_write_callback;
	writer.m_pIO_opaque = &sink;
	if (!mz_zip_writer_init_v2(&writer, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) return false;
	bool success = true;
	/* A stable timestamp is adequate for this initial bytes-and-paths profile.
	   Filesystem attribute preservation can be added without changing source I/O. */
	struct tm date = {0};
	date.tm_year = 80; /* 1980-01-01 in local time: miniz encodes DOS timestamps through localtime(). */
	date.tm_mday = 1;
	date.tm_isdst = -1;
	MZ_TIME_T timestamp = mktime(&date);
	if (timestamp == (MZ_TIME_T)-1) { mz_zip_writer_end(&writer); return false; }
	for (u32 i = 0; i < count && success; ++i) {
		const source_zip_input_t* member = members + i;
		if (member->is_directory) {
			success = mz_zip_writer_add_mem_ex_v2(&writer, member->path, NULL, 0, NULL, 0, 0,
				0, 0, &timestamp, NULL, 0, NULL, 0) != 0;
		} else {
			zip_input_reader_t input = {&member->file, 0, false};
			success = mz_zip_writer_add_read_buf_callback(&writer, member->path, zip_input_read,
				&input, member->file.size, &timestamp, NULL, 0, MZ_NO_COMPRESSION,
				NULL, 0, NULL, 0) != 0;
			success = success && !input.failed && input.read == member->file.size;
		}
		if (sink.failed) success = false;
	}
	if (success) success = mz_zip_writer_finalize_archive(&writer) != 0;
	mz_zip_writer_end(&writer);
	if (success) zip_error(error, capacity, "");
	return success;
}
