/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_object.h"

#include "archive_database.h"
#include "archive_sha256.h"
#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#define archive_close _close
#define archive_commit _commit
#define archive_fdopen _fdopen
#define archive_fileno _fileno
#define archive_mkdir(path) _mkdir(path)
#define archive_open _open
#define ARCHIVE_OPEN_BINARY _O_BINARY
#else
#include <unistd.h>
#define archive_close close
#define archive_commit fsync
#define archive_fdopen fdopen
#define archive_fileno fileno
#define archive_mkdir(path) mkdir(path, 0700)
#define archive_open open
#define ARCHIVE_OPEN_BINARY 0
#endif

#define ARCHIVE_OBJECT_COPY_BUFFER_SIZE (64 * 1024)

static void archive_object_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_object_media_type_is_valid(const char* media_type) {
	if (!media_type || !media_type[0] || strlen(media_type) >= 129 ||
	    !strchr(media_type, '/')) return false;
	for (const char* cursor = media_type; *cursor; ++cursor) {
		if ((unsigned char)*cursor <= 0x20 || (unsigned char)*cursor >= 0x7f) return false;
	}
	return true;
}

static bool archive_object_absolute_source_path(
		const char* source_path, char path[ARCHIVE_PATH_CAPACITY]) {
#if defined(_WIN32)
	DWORD length = GetFullPathNameA(
		source_path, ARCHIVE_PATH_CAPACITY, path, NULL);
	return length > 0 && length < ARCHIVE_PATH_CAPACITY;
#else
	return realpath(source_path, path) != NULL;
#endif
}

static bool archive_object_is_directory(const char* path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

#if !defined(_WIN32)
static bool archive_object_sync_directory(const char* path) {
	int descriptor = archive_open(path, O_RDONLY);
	if (descriptor < 0) return false;
	bool success = archive_commit(descriptor) == 0;
	archive_close(descriptor);
	return success;
}
#endif

static bool archive_object_ensure_directory(const char* path, const char* parent,
		char* error, size_t error_capacity) {
	if (archive_object_is_directory(path)) return true;
	if (archive_mkdir(path) != 0) {
		if (errno == EEXIST && archive_object_is_directory(path)) return true;
		archive_object_set_error(error, error_capacity, "could not create object directory '%s': %s",
		                         path, strerror(errno));
		return false;
	}
#if !defined(_WIN32)
	if (!archive_object_sync_directory(parent)) {
		archive_object_set_error(error, error_capacity, "could not make object directory durable: %s",
		                         strerror(errno));
		return false;
	}
#else
	(void)parent;
#endif
	return true;
}

static bool archive_object_create_staging_file(const archive_repository_t* repository,
		const char* storage_root, char* path, size_t path_capacity, FILE** file,
		char* error, size_t error_capacity) {
	const char* staging_directory = "staging";
	if (strcmp(storage_root, "objects") == 0) {
		/* The object staging directory can be placed on the objects filesystem. */
		char parent[ARCHIVE_PATH_CAPACITY];
		char object_staging[ARCHIVE_PATH_CAPACITY];
		if (!archive_repository_make_path(repository, "staging", parent, sizeof(parent)) ||
		    !archive_repository_make_path(repository, "staging/objects", object_staging,
		                                  sizeof(object_staging)) ||
		    !archive_object_ensure_directory(object_staging, parent, error, error_capacity)) {
			return false;
		}
		staging_directory = "staging/objects";
	} else if (strcmp(storage_root, "shells") != 0) {
		return false;
	}
	for (i32 attempt = 0; attempt < 16; ++attempt) {
		u8 random[16];
		char random_hex[33];
		sqlite3_randomness(sizeof(random), random);
		static const char digits[] = "0123456789abcdef";
		for (i32 i = 0; i < 16; ++i) {
			random_hex[i * 2] = digits[random[i] >> 4];
			random_hex[i * 2 + 1] = digits[random[i] & 15];
		}
		random_hex[32] = '\0';
		char relative[80];
		int relative_length = snprintf(relative, sizeof(relative),
		                               "%s/object-%s.tmp", staging_directory, random_hex);
		if (relative_length < 0 || (size_t)relative_length >= sizeof(relative) ||
		    !archive_repository_make_path(repository, relative, path, path_capacity)) {
			return false;
		}
		int descriptor = archive_open(path,
			O_WRONLY | O_CREAT | O_EXCL | ARCHIVE_OPEN_BINARY,
#if defined(_WIN32)
			_S_IREAD | _S_IWRITE
#else
			0600
#endif
		);
		if (descriptor >= 0) {
			*file = archive_fdopen(descriptor, "wb");
			if (*file) return true;
			archive_close(descriptor);
			remove(path);
			break;
		}
		if (errno != EEXIST) break;
	}
	archive_object_set_error(error, error_capacity, "could not create an exclusive staging object: %s",
	                         strerror(errno));
	return false;
}

static bool archive_object_finish_staging(FILE* file, const char* path,
		char* error, size_t error_capacity) {
	bool success = fflush(file) == 0 && archive_commit(archive_fileno(file)) == 0;
	if (fclose(file) != 0) success = false;
	if (!success) {
		archive_object_set_error(error, error_capacity, "could not make staged object '%s' durable: %s",
		                         path, strerror(errno));
	}
	return success;
}



static archive_object_status_t archive_object_install(const archive_repository_t* repository,
		const char* storage_root, const char* staging_path, const char* sha256, u64 byte_size,
		char storage_key[257], bool* already_present, char* error, size_t error_capacity) {
	char objects_path[ARCHIVE_PATH_CAPACITY];
	char sha_path[ARCHIVE_PATH_CAPACITY];
	char prefix_path[ARCHIVE_PATH_CAPACITY];
	char sha_relative[64];
	if (!storage_root || (strcmp(storage_root, "objects") != 0 &&
	                     strcmp(storage_root, "shells") != 0) ||
	    snprintf(sha_relative, sizeof(sha_relative), "%s/sha256", storage_root) < 0 ||
	    !archive_repository_make_path(repository, storage_root, objects_path, sizeof(objects_path)) ||
	    !archive_repository_make_path(repository, sha_relative, sha_path, sizeof(sha_path))) {
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	int key_length = snprintf(storage_key, 257, "%s/sha256/%c%c/%s",
	                          storage_root, sha256[0], sha256[1], sha256);
	if (key_length < 0 || key_length >= 257 ||
	    !archive_repository_make_path(repository, storage_key,
	                                  prefix_path, sizeof(prefix_path))) {
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	char prefix_directory[ARCHIVE_PATH_CAPACITY];
	int prefix_length = snprintf(prefix_directory, sizeof(prefix_directory),
	                             "%s/%c%c", sha_path, sha256[0], sha256[1]);
	if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix_directory) ||
	    !archive_object_ensure_directory(sha_path, objects_path, error, error_capacity) ||
	    !archive_object_ensure_directory(prefix_directory, sha_path, error, error_capacity)) {
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}

	*already_present = false;
#if defined(_WIN32)
	if (!MoveFileExA(staging_path, prefix_path, MOVEFILE_WRITE_THROUGH)) {
		DWORD move_error = GetLastError();
		if (move_error != ERROR_ALREADY_EXISTS && move_error != ERROR_FILE_EXISTS) {
			archive_object_set_error(error, error_capacity, "could not install immutable object: %lu",
			                         (unsigned long)move_error);
			return ARCHIVE_OBJECT_STORAGE_ERROR;
		}
		*already_present = true;
	}
#else
	if (link(staging_path, prefix_path) != 0) {
		if (errno != EEXIST) {
			archive_object_set_error(error, error_capacity, "could not install immutable object: %s",
			                         strerror(errno));
			return ARCHIVE_OBJECT_STORAGE_ERROR;
		}
		*already_present = true;
	} else if (!archive_object_sync_directory(prefix_directory)) {
		archive_object_set_error(error, error_capacity, "could not make immutable object durable: %s",
		                         strerror(errno));
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	if (remove(staging_path) != 0) {
		archive_object_set_error(error, error_capacity, "could not remove installed staging object: %s",
		                         strerror(errno));
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
#endif

	if (*already_present) {
		char stored_sha256[65];
		u64 stored_size = 0;
		if (!archive_sha256_file(prefix_path, stored_sha256, &stored_size)) {
			remove(staging_path);
			archive_object_set_error(error, error_capacity,
			                         "could not verify existing immutable object");
			return ARCHIVE_OBJECT_STORAGE_ERROR;
		}
		if (stored_size != byte_size || strcmp(stored_sha256, sha256) != 0) {
			remove(staging_path);
			archive_object_set_error(error, error_capacity,
			                         "content-addressed object '%s' failed integrity verification",
			                         storage_key);
			return ARCHIVE_OBJECT_INTEGRITY_ERROR;
		}
#if defined(_WIN32)
		if (!DeleteFileA(staging_path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
			archive_object_set_error(error, error_capacity, "could not remove duplicate staging object");
			return ARCHIVE_OBJECT_STORAGE_ERROR;
		}
#endif
	}
	return ARCHIVE_OBJECT_OK;
}

static archive_object_status_t archive_object_register(const archive_repository_t* repository,
		const char* sha256, u64 byte_size, const char* media_type, const char* storage_key,
		bool file_already_present, archive_object_t* object,
		char* error, size_t error_capacity) {
	archive_database_object_t registered;
	archive_database_status_t status = archive_database_register_object(
		repository, sha256, byte_size, media_type, storage_key,
		&registered, error, error_capacity);
	if (status != ARCHIVE_DATABASE_OK) {
		return status == ARCHIVE_DATABASE_CONFLICT ?
		       ARCHIVE_OBJECT_CONFLICT : ARCHIVE_OBJECT_DATABASE_ERROR;
	}
	object->id = registered.id;
	memcpy(object->sha256, registered.sha256, sizeof(object->sha256));
	object->byte_size = registered.byte_size;
	memcpy(object->media_type, registered.media_type, sizeof(object->media_type));
	memcpy(object->storage_key, registered.storage_key, sizeof(object->storage_key));
	object->already_present = file_already_present || registered.already_registered;
	return ARCHIVE_OBJECT_OK;
}

archive_object_status_t archive_object_import_file(const archive_repository_t* repository,
		const char* source_path, const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (!repository || !repository->root[0] || !source_path || !source_path[0] ||
	    !archive_object_media_type_is_valid(media_type) || !object) {
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	FILE* source = fopen(source_path, "rb");
	if (!source) {
		archive_object_set_error(error, error_capacity, "could not open source object '%s': %s",
		                         source_path, strerror(errno));
		return ARCHIVE_OBJECT_SOURCE_ERROR;
	}
	char staging_path[ARCHIVE_PATH_CAPACITY];
	FILE* staging = NULL;
	if (!archive_object_create_staging_file(repository, "objects", staging_path, sizeof(staging_path),
	                                        &staging, error, error_capacity)) {
		fclose(source);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}

	archive_sha256_t sha;
	archive_sha256_initialize(&sha);
	u8 buffer[ARCHIVE_OBJECT_COPY_BUFFER_SIZE];
	bool source_failed = false;
	bool storage_failed = false;
	while (true) {
		size_t read = fread(buffer, 1, sizeof(buffer), source);
		if (read > 0) {
			if (!archive_sha256_update(&sha, buffer, read)) {
				source_failed = true;
				break;
			}
			if (fwrite(buffer, 1, read, staging) != read) {
				storage_failed = true;
				break;
			}
		}
		if (read < sizeof(buffer)) {
			if (ferror(source)) source_failed = true;
			break;
		}
	}
	if (fclose(source) != 0) source_failed = true;
	if (source_failed || storage_failed) {
		fclose(staging);
		remove(staging_path);
		if (storage_failed) {
			archive_object_set_error(error, error_capacity,
			                         "could not write staged source object");
		} else {
			archive_object_set_error(error, error_capacity,
			                         "could not read source object '%s'", source_path);
		}
		return storage_failed ? ARCHIVE_OBJECT_STORAGE_ERROR : ARCHIVE_OBJECT_SOURCE_ERROR;
	}
	if (!archive_object_finish_staging(staging, staging_path, error, error_capacity)) {
		remove(staging_path);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	u8 digest[32];
	u64 byte_size = sha.byte_count;
	char sha256[65];
	archive_sha256_finish(&sha, digest);
	archive_sha256_hex(digest, sha256);
	char storage_key[257];
	bool already_present = false;
	archive_object_status_t status = archive_object_install(
		repository, "objects", staging_path, sha256, byte_size, storage_key,
		&already_present, error, error_capacity);
	if (status != ARCHIVE_OBJECT_OK) {
		remove(staging_path);
		return status;
	}
	return archive_object_register(repository, sha256, byte_size, media_type, storage_key,
	                               already_present, object, error, error_capacity);
}

archive_object_status_t archive_object_reference_file(
		const archive_repository_t* repository, const char* source_path,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (error && error_capacity) error[0] = '\0';
	if (!repository || !source_path || !source_path[0] ||
	    !archive_object_media_type_is_valid(media_type) || !object)
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	char absolute_source_path[ARCHIVE_PATH_CAPACITY];
	if (!archive_object_absolute_source_path(
			source_path, absolute_source_path)) {
		archive_object_set_error(
			error, error_capacity,
			"could not resolve referenced source '%s': %s",
			source_path, strerror(errno));
		return ARCHIVE_OBJECT_SOURCE_ERROR;
	}
	char sha256[65];
	u64 byte_size = 0;
	if (!archive_sha256_file(
			absolute_source_path, sha256, &byte_size)) {
		archive_object_set_error(error, error_capacity,
		                         "could not hash referenced source '%s'",
		                         absolute_source_path);
		return ARCHIVE_OBJECT_SOURCE_ERROR;
	}
	archive_database_object_t registered = {0};
	archive_database_status_t status =
		archive_database_register_external_object(
			repository, sha256, byte_size, media_type,
			absolute_source_path, &registered, error, error_capacity);
	if (status != ARCHIVE_DATABASE_OK)
		return status == ARCHIVE_DATABASE_CONFLICT ?
		       ARCHIVE_OBJECT_CONFLICT :
		       ARCHIVE_OBJECT_DATABASE_ERROR;
	object->id = registered.id;
	memcpy(object->sha256, registered.sha256,
	       sizeof(object->sha256));
	object->byte_size = registered.byte_size;
	memcpy(object->media_type, registered.media_type,
	       sizeof(object->media_type));
	memcpy(object->external_path, registered.external_path,
	       sizeof(object->external_path));
	object->already_present = registered.already_registered;
	return ARCHIVE_OBJECT_OK;
}

static archive_object_status_t archive_object_store_bytes_in_tier(
		const archive_repository_t* repository, const char* storage_root,
		const void* data, size_t size, const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (!repository || !repository->root[0] || (!data && size > 0) ||
	    !archive_object_media_type_is_valid(media_type) || !object) {
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	char staging_path[ARCHIVE_PATH_CAPACITY];
	FILE* staging = NULL;
	if (!archive_object_create_staging_file(repository, storage_root, staging_path, sizeof(staging_path),
	                                        &staging, error, error_capacity)) {
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	if (size > 0 && fwrite(data, 1, size, staging) != size) {
		fclose(staging);
		remove(staging_path);
		archive_object_set_error(error, error_capacity, "could not write staged object");
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	if (!archive_object_finish_staging(staging, staging_path, error, error_capacity)) {
		remove(staging_path);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	archive_sha256_t sha;
	archive_sha256_initialize(&sha);
	if (!archive_sha256_update(&sha, data, size)) {
		remove(staging_path);
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	u8 digest[32];
	char sha256[65];
	archive_sha256_finish(&sha, digest);
	archive_sha256_hex(digest, sha256);
	char storage_key[257];
	bool already_present = false;
	archive_object_status_t status = archive_object_install(
		repository, storage_root, staging_path, sha256, size, storage_key,
		&already_present, error, error_capacity);
	if (status != ARCHIVE_OBJECT_OK) {
		remove(staging_path);
		return status;
	}
	return archive_object_register(repository, sha256, size, media_type, storage_key,
	                               already_present, object, error, error_capacity);
}

archive_object_status_t archive_object_store_bytes(const archive_repository_t* repository,
		const void* data, size_t size, const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	return archive_object_store_bytes_in_tier(repository, "objects", data, size, media_type,
	                                          object, error, error_capacity);
}

archive_object_status_t archive_object_store_control_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	return archive_object_store_inline_bytes(
		repository, data, size, media_type, object, error, error_capacity);
}

archive_object_status_t archive_object_store_inline_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (!repository || !repository->root[0] || !data || !size ||
	    !archive_object_media_type_is_valid(media_type) || !object) {
		return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	}
	char sha256[65];
	if (!archive_sha256_bytes(data, size, sha256))
		return ARCHIVE_OBJECT_INTEGRITY_ERROR;
	archive_database_object_t registered = {0};
	archive_database_status_t status = archive_database_register_inline_object(
		repository, sha256, data, size, media_type, &registered,
		error, error_capacity);
	if (status != ARCHIVE_DATABASE_OK)
		return status == ARCHIVE_DATABASE_CONFLICT ?
		       ARCHIVE_OBJECT_CONFLICT : ARCHIVE_OBJECT_DATABASE_ERROR;
	object->id = registered.id;
	memcpy(object->sha256, registered.sha256, sizeof(object->sha256));
	object->byte_size = registered.byte_size;
	memcpy(object->media_type, registered.media_type, sizeof(object->media_type));
	object->already_present = registered.already_registered;
	return ARCHIVE_OBJECT_OK;
}

archive_object_status_t archive_object_read_inline_bytes(
		const archive_repository_t* repository, const char* sha256,
		u64 expected_size, const char* expected_media_type, size_t maximum_size,
		u8** data, size_t* size, char* error, size_t error_capacity) {
	archive_database_status_t status = archive_database_read_inline_object(
		repository, sha256, expected_size, expected_media_type, maximum_size,
		data, size, error, error_capacity);
	if (status != ARCHIVE_DATABASE_OK)
		return status == ARCHIVE_DATABASE_CONFLICT ?
		       ARCHIVE_OBJECT_CONFLICT : ARCHIVE_OBJECT_DATABASE_ERROR;
	char actual_sha256[65];
	if (!archive_sha256_bytes(*data, *size, actual_sha256) ||
	    strcmp(actual_sha256, sha256) != 0) {
		free(*data);
		*data = NULL;
		*size = 0;
		archive_object_set_error(error, error_capacity,
			"inline object failed SHA-256 validation");
		return ARCHIVE_OBJECT_INTEGRITY_ERROR;
	}
	return ARCHIVE_OBJECT_OK;
}

archive_object_status_t archive_object_store_shell_bytes(const archive_repository_t* repository,
		const void* data, size_t size, archive_object_t* object,
		char* error, size_t error_capacity) {
	return archive_object_store_bytes_in_tier(
		repository, "shells", data, size,
		"application/vnd.slidescape.virtual-dicom-shell", object, error, error_capacity);
}

archive_object_status_t archive_object_repair_shell_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* expected_sha256, const char* expected_storage_key,
		char* error, size_t error_capacity) {
	char actual_sha256[65];
	char expected_key[257];
	char shells_path[ARCHIVE_PATH_CAPACITY];
	char sha_path[ARCHIVE_PATH_CAPACITY];
	char prefix_path[ARCHIVE_PATH_CAPACITY];
	char target_path[ARCHIVE_PATH_CAPACITY];
	if (!repository || !repository->root[0] || !data || size == 0 ||
	    !expected_sha256 || strlen(expected_sha256) != 64 ||
	    !expected_storage_key ||
	    !archive_sha256_bytes(data, size, actual_sha256) ||
	    strcmp(actual_sha256, expected_sha256) != 0 ||
	    snprintf(expected_key, sizeof(expected_key), "shells/sha256/%c%c/%s",
	             expected_sha256[0], expected_sha256[1], expected_sha256) < 0 ||
	    strcmp(expected_key, expected_storage_key) != 0 ||
	    !archive_repository_make_path(repository, "shells", shells_path,
	                                  sizeof(shells_path)) ||
	    !archive_repository_make_path(repository, "shells/sha256", sha_path,
	                                  sizeof(sha_path)) ||
	    !archive_repository_make_path(repository, expected_storage_key,
	                                  target_path, sizeof(target_path))) {
		archive_object_set_error(error, error_capacity,
		                         "regenerated shell does not match its published object identity");
		return ARCHIVE_OBJECT_INTEGRITY_ERROR;
	}
	int prefix_length = snprintf(prefix_path, sizeof(prefix_path), "%s/%c%c",
	                             sha_path, expected_sha256[0], expected_sha256[1]);
	if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix_path) ||
	    !archive_object_ensure_directory(sha_path, shells_path, error, error_capacity) ||
	    !archive_object_ensure_directory(prefix_path, sha_path, error, error_capacity)) {
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}

	char staging_path[ARCHIVE_PATH_CAPACITY];
	FILE* staging = NULL;
	if (!archive_object_create_staging_file(repository, "shells", staging_path,
	                                        sizeof(staging_path), &staging,
	                                        error, error_capacity)) {
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
	bool staged = fwrite(data, 1, size, staging) == size;
	if (!staged) {
		fclose(staging);
		archive_object_set_error(error, error_capacity,
		                         "could not write regenerated fast-tier shell");
	} else {
		staged = archive_object_finish_staging(staging, staging_path,
		                                      error, error_capacity);
	}
	if (!staged) {
		remove(staging_path);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
#if defined(_WIN32)
	if (!MoveFileExA(staging_path, target_path,
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		archive_object_set_error(error, error_capacity,
		                         "could not atomically restore fast-tier shell: %lu",
		                         (unsigned long)GetLastError());
		remove(staging_path);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
#else
	if (rename(staging_path, target_path) != 0 ||
	    !archive_object_sync_directory(prefix_path)) {
		archive_object_set_error(error, error_capacity,
		                         "could not atomically restore fast-tier shell: %s",
		                         strerror(errno));
		remove(staging_path);
		return ARCHIVE_OBJECT_STORAGE_ERROR;
	}
#endif
	return ARCHIVE_OBJECT_OK;
}

const char* archive_object_status_string(archive_object_status_t status) {
	switch (status) {
		case ARCHIVE_OBJECT_OK: return "ok";
		case ARCHIVE_OBJECT_INVALID_ARGUMENT: return "invalid argument";
		case ARCHIVE_OBJECT_SOURCE_ERROR: return "source error";
		case ARCHIVE_OBJECT_STORAGE_ERROR: return "storage error";
		case ARCHIVE_OBJECT_INTEGRITY_ERROR: return "integrity error";
		case ARCHIVE_OBJECT_DATABASE_ERROR: return "database error";
		case ARCHIVE_OBJECT_CONFLICT: return "conflict";
		default: return "unknown object error";
	}
}


typedef struct archive_zip_sink_t {
	FILE* file;
	archive_sha256_t sha;
} archive_zip_sink_t;

static bool archive_zip_write(void* context, const void* data, size_t size) {
	archive_zip_sink_t* sink = context;
	return fwrite(data, 1, size, sink->file) == size && archive_sha256_update(&sink->sha, data, size);
}

archive_object_status_t archive_object_store_zip(const archive_repository_t* repository,
		const source_zip_input_t* members, u32 count, archive_object_t* object,
		char* error, size_t error_capacity) {
	if (!repository || !object || !members || !count) return ARCHIVE_OBJECT_INVALID_ARGUMENT;
	memset(object, 0, sizeof(*object));
	char staging_path[ARCHIVE_PATH_CAPACITY];
	archive_zip_sink_t sink = {0};
	if (!archive_object_create_staging_file(repository, "objects", staging_path, sizeof(staging_path),
		&sink.file, error, error_capacity)) return ARCHIVE_OBJECT_STORAGE_ERROR;
	archive_sha256_initialize(&sink.sha);
	bool success = source_zip_write(members, count, archive_zip_write, &sink, error, error_capacity);
	if (success) success = archive_object_finish_staging(sink.file, staging_path, error, error_capacity);
	else fclose(sink.file);
	/* Validate the captured bytes, not a second reading of the mutable inputs. */
	source_blob_t blob = {0};
	source_file_t file = {0};
	source_zip_t* zip = NULL;
	if (success) success = source_blob_open_local(&blob, staging_path) &&
		source_file_view(&blob, 0, blob.size, &file) &&
		(zip = source_zip_open(&file, error, error_capacity)) &&
		source_zip_validate_contents(zip, error, error_capacity);
	source_zip_close(zip);
	source_blob_close(&blob);
	if (!success) { remove(staging_path); return ARCHIVE_OBJECT_INTEGRITY_ERROR; }
	u64 size = sink.sha.byte_count;
	u8 digest[32];
	char hash[65], key[257];
	archive_sha256_finish(&sink.sha, digest);
	archive_sha256_hex(digest, hash);
	bool present = false;
	archive_object_status_t status = archive_object_install(repository, "objects", staging_path,
		hash, size, key, &present, error, error_capacity);
	if (status != ARCHIVE_OBJECT_OK) { remove(staging_path); return status; }
	return archive_object_register(repository, hash, size, "application/zip", key, present,
		object, error, error_capacity);
}
