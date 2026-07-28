/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_repository.h"

#include "archive_database.h"
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define archive_mkdir(path) _mkdir(path)
#else
#define archive_mkdir(path) mkdir(path, 0700)
#endif

static const char* archive_repository_directories[] = {
	"objects",
	"shells",
	"bundles",
	"representations",
	"derived",
	"staging",
};

static void archive_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_path_join(char* dest, size_t capacity, const char* root, const char* leaf) {
	size_t root_length = strlen(root);
	bool needs_separator = root_length > 0 && root[root_length - 1] != '/' && root[root_length - 1] != '\\';
	int length = snprintf(dest, capacity, "%s%s%s", root, needs_separator ? "/" : "", leaf);
	return length >= 0 && (size_t)length < capacity;
}

static bool archive_is_directory(const char* path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool archive_descriptor_has_schema_version(const char* descriptor, long expected_version) {
	const char* key = strstr(descriptor, "\"schemaVersion\"");
	if (!key) return false;
	const char* value = strchr(key, ':');
	if (!value) return false;
	++value;
	while (isspace((unsigned char)*value)) ++value;
	char* end = NULL;
	errno = 0;
	long parsed = strtol(value, &end, 10);
	if (errno || end == value) return false;
	while (isspace((unsigned char)*end)) ++end;
	return parsed == expected_version && (*end == ',' || *end == '}');
}

static archive_repository_status_t archive_ensure_directory(const char* path, char* error,
		size_t error_capacity) {
	if (archive_is_directory(path)) return ARCHIVE_REPOSITORY_OK;
	if (archive_mkdir(path) == 0) return ARCHIVE_REPOSITORY_OK;
	archive_set_error(error, error_capacity, "could not create directory '%s': %s", path,
	                  strerror(errno));
	return ARCHIVE_REPOSITORY_IO_ERROR;
}

static archive_repository_status_t archive_validate_root(archive_repository_t* repository,
		const char* root, char* error, size_t error_capacity) {
	if (!repository || !root || !root[0]) {
		archive_set_error(error, error_capacity, "repository root is required");
		return ARCHIVE_REPOSITORY_INVALID_ARGUMENT;
	}
	int length = snprintf(repository->root, sizeof(repository->root), "%s", root);
	if (length < 0 || (size_t)length >= sizeof(repository->root)) {
		archive_set_error(error, error_capacity, "repository path is too long");
		return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
	}
	return ARCHIVE_REPOSITORY_OK;
}

static archive_repository_status_t archive_repository_database_status(
		archive_database_status_t status) {
	switch (status) {
		case ARCHIVE_DATABASE_OK: return ARCHIVE_REPOSITORY_OK;
		case ARCHIVE_DATABASE_INVALID_ARGUMENT: return ARCHIVE_REPOSITORY_INVALID_ARGUMENT;
		case ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA: return ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA;
		case ARCHIVE_DATABASE_IO_ERROR:
		default: return ARCHIVE_REPOSITORY_IO_ERROR;
	}
}

static archive_repository_status_t archive_validate_layout(const archive_repository_t* repository,
		bool validate_database, char* error, size_t error_capacity) {
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_is_directory(repository->root)) {
		archive_set_error(error, error_capacity, "repository root '%s' is not a directory", repository->root);
		return ARCHIVE_REPOSITORY_INVALID_LAYOUT;
	}

	if (!archive_path_join(path, sizeof(path), repository->root, "repository.json")) {
		return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
	}
	FILE* file = fopen(path, "rb");
	if (!file) {
		archive_set_error(error, error_capacity, "repository descriptor '%s' is missing", path);
		return ARCHIVE_REPOSITORY_INVALID_LAYOUT;
	}
	char descriptor[1024] = {0};
	size_t bytes_read = fread(descriptor, 1, sizeof(descriptor) - 1, file);
	bool read_failed = ferror(file) != 0;
	fclose(file);
	if (read_failed || bytes_read == sizeof(descriptor) - 1) {
		archive_set_error(error, error_capacity, "repository descriptor could not be read safely");
		return ARCHIVE_REPOSITORY_IO_ERROR;
	}
	if (!strstr(descriptor, "\"kind\": \"slidescape-pathology-archive\"") ||
	    !archive_descriptor_has_schema_version(descriptor, ARCHIVE_REPOSITORY_SCHEMA_VERSION)) {
		archive_set_error(error, error_capacity, "repository schema is not supported");
		return ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA;
	}

	for (i32 i = 0; i < COUNT(archive_repository_directories); ++i) {
		if (!archive_path_join(path, sizeof(path), repository->root, archive_repository_directories[i])) {
			return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
		}
		if (!archive_is_directory(path)) {
			archive_set_error(error, error_capacity, "repository directory '%s' is missing", path);
			return ARCHIVE_REPOSITORY_INVALID_LAYOUT;
		}
	}
	if (validate_database) {
		return archive_repository_database_status(
			archive_database_validate(repository, error, error_capacity));
	}
	return ARCHIVE_REPOSITORY_OK;
}

archive_repository_status_t archive_repository_initialize(archive_repository_t* repository,
		const char* root, char* error, size_t error_capacity) {
	archive_repository_status_t status = archive_validate_root(repository, root, error, error_capacity);
	if (status != ARCHIVE_REPOSITORY_OK) return status;
	status = archive_ensure_directory(repository->root, error, error_capacity);
	if (status != ARCHIVE_REPOSITORY_OK) return status;

	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_path_join(path, sizeof(path), repository->root, "repository.json")) {
		return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
	}
	FILE* existing = fopen(path, "rb");
	if (existing) {
		fclose(existing);
		/* Never add directories to a repository whose schema we do not understand. */
		status = archive_validate_layout(repository, false, error, error_capacity);
		if (status != ARCHIVE_REPOSITORY_OK) return status;
		status = archive_repository_database_status(
			archive_database_initialize(repository, error, error_capacity));
		return status == ARCHIVE_REPOSITORY_OK ?
		       archive_validate_layout(repository, true, error, error_capacity) : status;
	}

	for (i32 i = 0; i < COUNT(archive_repository_directories); ++i) {
		if (!archive_path_join(path, sizeof(path), repository->root, archive_repository_directories[i])) {
			return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
		}
		status = archive_ensure_directory(path, error, error_capacity);
		if (status != ARCHIVE_REPOSITORY_OK) return status;
	}

	if (!archive_path_join(path, sizeof(path), repository->root, "repository.json")) {
		return ARCHIVE_REPOSITORY_PATH_TOO_LONG;
	}
	FILE* descriptor = fopen(path, "wb");
	if (!descriptor) {
		archive_set_error(error, error_capacity, "could not create repository descriptor '%s': %s",
		                  path, strerror(errno));
		return ARCHIVE_REPOSITORY_IO_ERROR;
	}
	const char contents[] =
		"{\n"
		"  \"kind\": \"slidescape-pathology-archive\",\n"
		"  \"schemaVersion\": 2,\n"
		"  \"publicationModel\": \"sqlite-transaction\"\n"
		"}\n";
	bool write_failed = fwrite(contents, 1, sizeof(contents) - 1, descriptor) != sizeof(contents) - 1;
	if (fclose(descriptor) != 0) write_failed = true;
	if (write_failed) {
		archive_set_error(error, error_capacity, "could not write repository descriptor '%s'", path);
		return ARCHIVE_REPOSITORY_IO_ERROR;
	}
	status = archive_repository_database_status(
		archive_database_initialize(repository, error, error_capacity));
	return status == ARCHIVE_REPOSITORY_OK ?
	       archive_validate_layout(repository, true, error, error_capacity) : status;
}

archive_repository_status_t archive_repository_open(archive_repository_t* repository,
		const char* root, char* error, size_t error_capacity) {
	archive_repository_status_t status = archive_validate_root(repository, root, error, error_capacity);
	return status == ARCHIVE_REPOSITORY_OK ?
	       archive_validate_layout(repository, true, error, error_capacity) : status;
}

const char* archive_repository_status_string(archive_repository_status_t status) {
	switch (status) {
		case ARCHIVE_REPOSITORY_OK: return "ok";
		case ARCHIVE_REPOSITORY_INVALID_ARGUMENT: return "invalid argument";
		case ARCHIVE_REPOSITORY_PATH_TOO_LONG: return "path too long";
		case ARCHIVE_REPOSITORY_IO_ERROR: return "I/O error";
		case ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA: return "incompatible schema";
		case ARCHIVE_REPOSITORY_INVALID_LAYOUT: return "invalid layout";
		default: return "unknown repository error";
	}
}

bool archive_repository_make_path(const archive_repository_t* repository, const char* relative_path,
		char* dest, size_t capacity) {
	if (!repository || !repository->root[0] || !relative_path || !relative_path[0] ||
	    relative_path[0] == '/' || relative_path[0] == '\\' || strstr(relative_path, "..")) {
		return false;
	}
	return archive_path_join(dest, capacity, repository->root, relative_path);
}
