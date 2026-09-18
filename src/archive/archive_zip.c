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

static void archive_zip_trim_trailing_separators(char* path) {
	size_t length = strlen(path);
	while (length > 1 && (path[length-1] == '/' || path[length-1] == '\\') &&
		path[length-2] != ':') path[--length] = 0;
}

/* Canonical paths also make the repository-containment check work through parent aliases. */
static bool archive_zip_resolve_path(const char* path, char resolved[ARCHIVE_PATH_CAPACITY],
		bool* is_directory) {
	if (!path || !*path || strlen(path) >= ARCHIVE_PATH_CAPACITY) return false;
	char input[ARCHIVE_PATH_CAPACITY];
	strcpy(input, path);
	/* lstat("link/") follows the link; inspect the selected entry itself. */
	archive_zip_trim_trailing_separators(input);
	path = input;
#if WINDOWS
	wchar_t wide[ARCHIVE_PATH_CAPACITY], absolute[ARCHIVE_PATH_CAPACITY];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (sizeof(wide) / sizeof(*wide)))) return false;
	DWORD attributes = GetFileAttributesW(wide);
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	HANDLE handle = CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (handle == INVALID_HANDLE_VALUE) return false;
	DWORD length = GetFinalPathNameByHandleW(handle, absolute, (sizeof(absolute) / sizeof(*absolute)), FILE_NAME_NORMALIZED);
	CloseHandle(handle);
	if (!length || length >= (sizeof(absolute) / sizeof(*absolute))) return false;
	/* The namespace uses ordinary DOS/UNC paths and may join with forward slashes;
	   those joins are not valid under the extended-path prefix returned here. */
	wchar_t* normalized = absolute;
	if (!wcsncmp(absolute, L"\\\\?\\UNC\\", 8)) {
		absolute[6] = L'\\';
		normalized = absolute + 6;
	} else if (!wcsncmp(absolute, L"\\\\?\\", 4)) normalized = absolute + 4;
	if (!WideCharToMultiByte(CP_UTF8, 0, normalized, -1, resolved, ARCHIVE_PATH_CAPACITY, NULL, NULL)) return false;
	*is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
	struct stat info;
	if (lstat(path, &info) != 0 || S_ISLNK(info.st_mode)) return false;
	char* absolute = realpath(path, NULL);
	if (!absolute) return false;
	bool fits = strlen(absolute) < ARCHIVE_PATH_CAPACITY;
	if (fits) strcpy(resolved, absolute);
	free(absolute);
	if (!fits) return false;
	*is_directory = S_ISDIR(info.st_mode);
#endif
	archive_zip_trim_trailing_separators(resolved);
	return true;
}

typedef enum archive_zip_capture_kind_t {
	ARCHIVE_ZIP_CAPTURE_MRXS,
	ARCHIVE_ZIP_CAPTURE_DIRECTORY,
} archive_zip_capture_kind_t;

static bool archive_zip_capture_internal(const archive_repository_t* repository, const char* path,
		archive_zip_capture_kind_t kind, archive_object_t* object, char* error, size_t capacity) {
	char resolved[ARCHIVE_PATH_CAPACITY], repository_path[ARCHIVE_PATH_CAPACITY];
	bool is_directory = false, repository_directory = false;
	if (!repository || !object || !archive_zip_resolve_path(path, resolved, &is_directory)) return false;
	if (kind == ARCHIVE_ZIP_CAPTURE_DIRECTORY && !is_directory) {
		if (error && capacity) snprintf(error, capacity, "--zip-directory requires a directory source");
		return false;
	}
	if (kind == ARCHIVE_ZIP_CAPTURE_MRXS && !is_directory) {
		char* extension = strrchr(resolved, '.');
		if (extension && !strcasecmp(extension, ".mrxs")) *extension = 0;
	}
	if (!archive_zip_resolve_path(repository->root, repository_path, &repository_directory) || !repository_directory) return false;
	/* Do not capture the database and staging files that this operation will modify. */
	size_t root_length = strlen(resolved);
#if WINDOWS
	bool prefix = !strncasecmp(repository_path, resolved, root_length);
#else
	bool prefix = !strncmp(repository_path, resolved, root_length);
#endif
	if (prefix && (!repository_path[root_length] || repository_path[root_length] == '/' ||
		repository_path[root_length] == '\\' || resolved[root_length-1] == '/' || resolved[root_length-1] == '\\')) {
		if (error && capacity) snprintf(error, capacity, "Capture source must not contain the destination repository");
		return false;
	}
	char directory[SOURCE_PATH_CAPACITY] = "";
	if (kind == ARCHIVE_ZIP_CAPTURE_MRXS) {
		char* name = resolved;
		for (char* at = resolved; *at; ++at) if (*at == '/' || *at == '\\') name = at + 1;
		if (name == resolved || !*name || strlen(name) >= sizeof(directory)) return false;
		strcpy(directory, name);
		/* Preserve the separator for filesystem roots ("/" and "C:\\"). */
		if (name == resolved + 1 || (name > resolved + 1 && name[-2] == ':')) *name = 0;
		else name[-1] = 0;
	}
	archive_zip_capture_t capture = {0};
	capture.root = resolved;
	capture.error = error; capture.error_capacity = capacity;
	capture.source = source_fs_open_directory(resolved);
	bool success = capture.source != NULL;
	if (success && kind == ARCHIVE_ZIP_CAPTURE_DIRECTORY) {
		success = source_fs_list(capture.source, "", archive_zip_capture_entry, &capture);
	} else if (success) {
		source_entry_t entry;
		success = source_fs_stat(capture.source, directory, &entry) && entry.is_directory &&
			archive_zip_capture_entry(&capture, &entry);
		char thumbnail[SOURCE_PATH_CAPACITY];
		int thumbnail_length = snprintf(thumbnail, sizeof(thumbnail), "%s.mrxs", directory);
		if (success && thumbnail_length > 0 && (size_t)thumbnail_length < sizeof(thumbnail) &&
			source_fs_stat(capture.source, thumbnail, &entry))
			success = !entry.is_directory && archive_zip_capture_entry(&capture, &entry);
	}
	if (success && !capture.count) {
		if (error && capacity) snprintf(error, capacity, "Capture source directory is empty");
		success = false;
	}
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
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not preserve the complete source tree");
	return success;
}

bool archive_zip_capture_mrxs(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity) {
	if (error && capacity) error[0] = 0;
	bool success = archive_zip_capture_internal(repository, path, ARCHIVE_ZIP_CAPTURE_MRXS, object, error, capacity);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not capture MRXS source directory");
	return success;
}

bool archive_zip_capture_directory(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity) {
	if (error && capacity) error[0] = 0;
	bool success = archive_zip_capture_internal(repository, path, ARCHIVE_ZIP_CAPTURE_DIRECTORY, object, error, capacity);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not capture source directory");
	return success;
}

bool archive_zip_inspect(const char* path, bool validate_contents, archive_zip_kind_t* kind,
		char* error, size_t capacity) {
	source_blob_t blob = {0}; source_file_t file = {0}; source_zip_t* zip = NULL;
	bool success = source_blob_open_local(&blob, path) && source_file_view(&blob, 0, blob.size, &file) &&
		(zip = source_zip_open(&file, error, capacity)) &&
		(!validate_contents || source_zip_validate_contents(zip, error, capacity));
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
