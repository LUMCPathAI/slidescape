/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "source_fs.h"
#if !WINDOWS
#include <dirent.h>
#include <errno.h>
#endif

typedef struct source_open_file_t {
	source_blob_t blob;
	struct source_open_file_t* next;
} source_open_file_t;

struct source_fs_t {
	char* root;
	source_blob_t container;
	source_zip_t* zip;
	source_open_file_t* opened;
};

source_fs_t* source_fs_open_directory(const char* path) {
	if (!path || !is_directory(path)) return NULL;
	source_fs_t* source = calloc(1, sizeof(*source));
	if (!source) return NULL;
	source->root = strdup(path);
	if (!source->root) { free(source); return NULL; }
	return source;
}

source_fs_t* source_fs_open_zip(const char* path, char* error, size_t capacity) {
	source_fs_t* source = calloc(1, sizeof(*source));
	if (!source) return NULL;
	if (!source_blob_open_local(&source->container, path)) {
		if (error && capacity) snprintf(error, capacity, "Cannot open ZIP file");
		source_fs_close(source); return NULL;
	}
	source_file_t container;
	source_file_view(&source->container, 0, source->container.size, &container);
	source->zip = source_zip_open(&container, error, capacity);
	if (!source->zip) { source_fs_close(source); return NULL; }
	return source;
}

void source_fs_close(source_fs_t* source) {
	if (!source) return;
	source_open_file_t* opened = source->opened;
	while (opened) {
		source_open_file_t* next = opened->next;
		source_blob_close(&opened->blob);
		free(opened);
		opened = next;
	}
	source_zip_close(source->zip);
	source_blob_close(&source->container);
	libc_free(source->root); // NOTE: created by strdup()
	free(source);
}

static char* local_path(source_fs_t* source, const char* path) {
	size_t root_size = strlen(source->root), path_size = strlen(path);
	char* joined = malloc(root_size + path_size + 2);
	if (joined) snprintf(joined, root_size + path_size + 2, "%s/%s", source->root, path);
	return joined;
}

bool source_fs_open_file(source_fs_t* source, const char* path, source_file_t* file) {
	if (!file) return false;
	*file = (source_file_t){0};
	if (!source || !source_path_is_valid(path, false)) return false;
	if (source->zip) {
		const source_zip_member_t* member = source_zip_find(source->zip, path);
		if (!member || member->is_directory) return false;
		*file = member->file;
		return true;
	}
	char* joined = local_path(source, path);
	if (!joined) return false;
	source_open_file_t* opened = calloc(1, sizeof(*opened));
	bool success = opened && source_blob_open_local(&opened->blob, joined);
	free(joined);
	if (!success) { free(opened); return false; }
	opened->next = source->opened;
	source->opened = opened;
	return source_file_view(&opened->blob, 0, opened->blob.size, file);
}

bool source_fs_stat(source_fs_t* source, const char* path, source_entry_t* entry) {
	if (!source || !path || !entry || (*path && !source_path_is_valid(path, false))) return false;
	*entry = (source_entry_t){path, 0, false};
	if (!*path) { entry->is_directory = true; return true; }
	if (source->zip) {
		const source_zip_member_t* member = source_zip_find(source->zip, path);
		if (member) { entry->size = member->file.size; return true; }
		size_t length = strlen(path);
		for (u32 i = 0; i < source_zip_count(source->zip); ++i) {
			const char* candidate = source_zip_member(source->zip, i)->path;
			if (!strncmp(candidate, path, length) && candidate[length] == '/') {
				entry->is_directory = true;
				return true;
			}
		}
		return false;
	}
	char* joined = local_path(source, path);
	if (!joined) return false;
#if WINDOWS
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, joined, -1, NULL, 0);
	wchar_t* wide = count > 0 ? malloc((size_t)count * sizeof(*wide)) : NULL;
	if (wide) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, joined, -1, wide, count);
	WIN32_FILE_ATTRIBUTE_DATA info;
	bool success = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &info);
	free(wide);
	if (success) {
		entry->is_directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		entry->is_link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		entry->size = entry->is_directory ? 0 : ((u64)info.nFileSizeHigh << 32) | info.nFileSizeLow;
	}
#else
	struct stat info;
	bool success = lstat(joined, &info) == 0 && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode));
	entry->is_link = success && S_ISLNK(info.st_mode);
	if (success) { entry->is_directory = S_ISDIR(info.st_mode); entry->size = entry->is_directory ? 0 : (u64)info.st_size; }
#endif
	free(joined);
	return success;
}

bool source_fs_list(source_fs_t* source, const char* directory, source_visit_fn visit, void* context) {
	source_entry_t info;
	if (!visit || !source_fs_stat(source, directory, &info) || !info.is_directory) return false;
	char prefix[SOURCE_PATH_CAPACITY];
	int prefix_length = snprintf(prefix, sizeof(prefix), "%s%s", directory, *directory ? "/" : "");
	if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) return false;
	if (source->zip) {
		char previous[SOURCE_PATH_CAPACITY] = "";
		for (u32 i = 0; i < source_zip_count(source->zip); ++i) {
			const source_zip_member_t* member = source_zip_member(source->zip, i);
			if (strncmp(member->path, prefix, (size_t)prefix_length)) continue;
			const char* name = member->path + prefix_length;
			if (!*name) continue;
			const char* slash = strchr(name, '/');
			size_t length = slash ? (size_t)(slash - member->path) : strlen(member->path);
			char path[SOURCE_PATH_CAPACITY];
			memcpy(path, member->path, length); path[length] = 0;
			if (!strcmp(previous, path)) continue;
			strcpy(previous, path);
			source_entry_t entry = {path, slash ? 0 : member->file.size, slash != NULL};
			if (!visit(context, &entry)) return false;
		}
		return true;
	}
	char* joined = local_path(source, directory);
	if (!joined) return false;
	bool success = true;
#if WINDOWS
	size_t size = strlen(joined) + 3;
	char* pattern = malloc(size);
	if (!pattern) { free(joined); return false; }
	snprintf(pattern, size, "%s/*", joined);
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, NULL, 0);
	wchar_t* wide = count > 0 ? malloc((size_t)count * sizeof(*wide)) : NULL;
	if (wide) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wide, count);
	free(pattern); free(joined);
	if (!wide) return false;
	WIN32_FIND_DATAW data;
	HANDLE listing = FindFirstFileW(wide, &data);
	free(wide);
	if (listing == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
	do {
		char name[SOURCE_PATH_CAPACITY];
		if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data.cFileName, -1, name, sizeof(name), NULL, NULL)) {
			success = false; break;
		}
#else
	DIR* listing = opendir(joined);
	free(joined);
	if (!listing) return false;
	for (;;) {
		errno = 0;
		struct dirent* data = readdir(listing);
		if (!data) { if (errno) success = false; break; }
		const char* name = data->d_name;
#endif
		if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
		char path[SOURCE_PATH_CAPACITY];
		int length = snprintf(path, sizeof(path), "%s%s", prefix, name);
		if (length < 0 || (size_t)length >= sizeof(path) || !source_fs_stat(source, path, &info) || !visit(context, &info)) {
			success = false; break;
		}
#if WINDOWS
	} while (FindNextFileW(listing, &data));
	if (success && GetLastError() != ERROR_NO_MORE_FILES) success = false;
	FindClose(listing);
#else
	}
	closedir(listing);
#endif
	return success;
}

mem_t* source_fs_read_entire(source_fs_t* source, const char* path, size_t limit) {
	source_file_t file;
	if (!source_fs_open_file(source, path, &file) || file.size > limit || file.size > SIZE_MAX - sizeof(mem_t) - 1) return NULL;
	mem_t* memory = malloc(sizeof(*memory) + (size_t)file.size + 1);
	if (!memory) return NULL;
	memory->len = memory->capacity = (size_t)file.size;
	memory->cursor = 0;
	memory->data[file.size] = 0;
	if (source_file_read(&file, 0, memory->data, memory->len) != SOURCE_READ_OK) { free(memory); return NULL; }
	return memory;
}
