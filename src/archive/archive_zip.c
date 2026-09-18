/* Slidescape. Copyright (C) 2019-2026 Pieter Valkema. SPDX-License-Identifier: GPL-3.0-or-later */
#include "archive_zip.h"
#include "source_fs.h"

typedef struct archive_zip_member_t {
	char* path;
	char* local_path;
	source_blob_t blob;
	source_blob_t opened;
	bool directory;
} archive_zip_member_t;
typedef struct archive_zip_capture_t {
	source_fs_t* source;
	const char* root;
	archive_zip_member_t** members;
	u32 count, capacity;
	size_t memory_used;
	char* error;
	size_t error_capacity;
} archive_zip_capture_t;

static source_read_result_t archive_zip_read_member(void* context, u64 offset, void* dest, size_t size) {
	archive_zip_member_t* member = context;
	if (!member->opened.read && (!source_blob_open_local(&member->opened, member->local_path) ||
		member->opened.size != member->blob.size)) return SOURCE_READ_IO_ERROR;
	source_file_t file;
	if (!source_file_view(&member->opened, 0, member->opened.size, &file)) return SOURCE_READ_IO_ERROR;
	source_read_result_t result = source_file_read(&file, offset, dest, size);
	/* The sequential ZIP writer visits one member at a time; keep FD use bounded. */
	if (result != SOURCE_READ_OK || offset + size == file.size) source_blob_close(&member->opened);
	return result;
}

static bool archive_zip_capture_entry(void* context, const source_entry_t* entry) {
	archive_zip_capture_t* capture = context;
	if (entry->is_link) {
		if (capture->error && capture->error_capacity) snprintf(capture->error, capture->error_capacity,
			"ZIP preservation does not support source links: %s", entry->path);
		return false;
	}
	size_t bytes = sizeof(archive_zip_member_t) + sizeof(void*) + sizeof(source_zip_input_t) +
		2 * strlen(entry->path) + strlen(capture->root) + 4;
	if (capture->count >= SOURCE_ZIP_MAX_MEMBERS || bytes > SOURCE_ZIP_INDEX_BUDGET - capture->memory_used) {
		if (capture->error && capture->error_capacity) snprintf(capture->error, capture->error_capacity,
			"ZIP source enumeration exceeds its member or memory budget");
		return false;
	}
	capture->memory_used += bytes;
	if (capture->count == capture->capacity) {
		u32 capacity = capture->capacity ? capture->capacity * 2 : 64;
		archive_zip_member_t** grown = realloc(capture->members, (size_t)capacity * sizeof(*grown));
		if (!grown) return false;
		capture->members = grown; capture->capacity = capacity;
	}
	archive_zip_member_t* member = calloc(1, sizeof(*member));
	if (!member) return false;
	capture->members[capture->count++] = member;
	size_t path_size = strlen(entry->path) + 2;
	member->path = malloc(path_size);
	size_t local_size = strlen(capture->root) + strlen(entry->path) + 2;
	member->local_path = malloc(local_size);
	if (!member->path || !member->local_path) return false;
	snprintf(member->path, path_size, "%s%s", entry->path, entry->is_directory ? "/" : "");
	snprintf(member->local_path, local_size, "%s/%s", capture->root, entry->path);
	if (!source_path_is_valid(member->path, entry->is_directory)) return false;
	member->directory = entry->is_directory;
	member->blob = (source_blob_t){entry->size, member, archive_zip_read_member, NULL};
	return !entry->is_directory || source_fs_list(capture->source, entry->path, archive_zip_capture_entry, capture);
}
static int archive_zip_compare(const void* a, const void* b) {
	return strcmp((*(archive_zip_member_t* const*)a)->path, (*(archive_zip_member_t* const*)b)->path);
}

static bool archive_zip_capture_mrxs_internal(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity) {
	char resolved[ARCHIVE_PATH_CAPACITY];
	bool is_directory = false;
#if WINDOWS
	wchar_t wide[ARCHIVE_PATH_CAPACITY], absolute[ARCHIVE_PATH_CAPACITY];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (sizeof(wide) / sizeof(*wide)))) return false;
	DWORD attributes = GetFileAttributesW(wide);
	DWORD length = GetFullPathNameW(wide, (sizeof(absolute) / sizeof(*absolute)), absolute, NULL);
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !length || length >= (sizeof(absolute) / sizeof(*absolute)) ||
		!WideCharToMultiByte(CP_UTF8, 0, absolute, -1, resolved, sizeof(resolved), NULL, NULL)) return false;
	is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat info;
	if (lstat(path, &info) != 0 || S_ISLNK(info.st_mode)) return false;
	char* absolute = realpath(path, NULL);
	if (!absolute) return false;
	bool fits = strlen(absolute) < sizeof(resolved);
	if (fits) strcpy(resolved, absolute);
	free(absolute);
	if (!fits) return false;
	is_directory = S_ISDIR(info.st_mode);
#endif
	size_t resolved_length = strlen(resolved);
	while (resolved_length > 1 && (resolved[resolved_length-1] == '/' || resolved[resolved_length-1] == '\\') &&
		!(resolved_length == 3 && resolved[1] == ':')) resolved[--resolved_length] = 0;
	char* extension = strrchr(resolved, '.');
	if (!is_directory && extension && !strcasecmp(extension, ".mrxs")) *extension = 0;
	char* name = resolved;
	for (char* at = resolved; *at; ++at) if (*at == '/' || *at == '\\') name = at + 1;
	if (name == resolved || !*name) return false;
	char directory[SOURCE_PATH_CAPACITY];
	if (strlen(name) >= sizeof(directory)) return false;
	strcpy(directory, name);
	/* Preserve the separator for filesystem roots ("/" and "C:\\"). */
	if (name == resolved + 1 || (name == resolved + 3 && resolved[1] == ':')) *name = 0;
	else name[-1] = 0;
	archive_zip_capture_t capture = {0};
	capture.root = resolved;
	capture.error = error; capture.error_capacity = capacity;
	capture.source = source_fs_open_directory(resolved);
	source_entry_t entry;
	bool success = capture.source && source_fs_stat(capture.source, directory, &entry) && entry.is_directory &&
		archive_zip_capture_entry(&capture, &entry);
	char thumbnail[SOURCE_PATH_CAPACITY];
	int thumbnail_length = snprintf(thumbnail, sizeof(thumbnail), "%s.mrxs", directory);
	if (success && thumbnail_length > 0 && (size_t)thumbnail_length < sizeof(thumbnail) &&
		source_fs_stat(capture.source, thumbnail, &entry))
		success = !entry.is_directory && archive_zip_capture_entry(&capture, &entry);
	source_zip_input_t* inputs = NULL;
	if (success) {
		qsort(capture.members, capture.count, sizeof(*capture.members), archive_zip_compare);
		inputs = calloc(capture.count, sizeof(*inputs));
		success = inputs != NULL;
	}
	for (u32 i = 0; success && i < capture.count; ++i) {
		archive_zip_member_t* member = capture.members[i];
		inputs[i].path = member->path;
		inputs[i].is_directory = member->directory;
		success = source_file_view(&member->blob, 0, member->blob.size, &inputs[i].file);
	}
	if (success) success = archive_object_store_zip(repository, inputs, capture.count, object, error, capacity) == ARCHIVE_OBJECT_OK;
	for (u32 i = 0; i < capture.count; ++i) {
		archive_zip_member_t* member = capture.members[i];
		source_blob_close(&member->opened);
		free(member->path); free(member->local_path); free(member);
	}
	free(inputs); free(capture.members); source_fs_close(capture.source);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not preserve the complete MRXS bundle");
	return success;
}

bool archive_zip_capture_mrxs(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity) {
	if (error && capacity) error[0] = 0;
	bool success = archive_zip_capture_mrxs_internal(repository, path, object, error, capacity);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not capture MRXS source directory");
	return success;
}

bool archive_zip_validate(const char* path, archive_zip_kind_t* kind, char* error, size_t capacity) {
	source_blob_t blob = {0}; source_file_t file = {0}; source_zip_t* zip = NULL;
	bool success = source_blob_open_local(&blob, path) && source_file_view(&blob, 0, blob.size, &file) &&
		(zip = source_zip_open(&file, error, capacity)) && source_zip_validate_contents(zip, error, capacity);
	if (success && kind) {
		*kind = ARCHIVE_ZIP_DICOM;
		for (u32 i = 0; i < source_zip_count(zip); ++i) {
			const source_zip_member_t* member = source_zip_member(zip, i);
			const char* leaf = strrchr(member->path, '/');
			if (!member->is_directory && !strcasecmp(leaf ? leaf + 1 : member->path, "Slidedat.ini"))
				*kind = ARCHIVE_ZIP_MRXS;
		}
	}
	source_zip_close(zip); source_blob_close(&blob);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not validate ZIP source");
	return success;
}
