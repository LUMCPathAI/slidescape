/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_repository.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum archive_object_status_t {
	ARCHIVE_OBJECT_OK = 0,
	ARCHIVE_OBJECT_INVALID_ARGUMENT,
	ARCHIVE_OBJECT_SOURCE_ERROR,
	ARCHIVE_OBJECT_STORAGE_ERROR,
	ARCHIVE_OBJECT_INTEGRITY_ERROR,
	ARCHIVE_OBJECT_DATABASE_ERROR,
	ARCHIVE_OBJECT_CONFLICT,
} archive_object_status_t;

typedef struct archive_object_t {
	i64 id;
	char sha256[65];
	u64 byte_size;
	char media_type[129];
	char storage_key[257];
	bool already_present;
} archive_object_t;

/*
 * Copy one source into staging while hashing it, durably install it under its
 * SHA-256 key, then idempotently register it in SQLite. The source is never
 * referenced by path after this call.
 */
archive_object_status_t archive_object_import_file(const archive_repository_t* repository,
		const char* source_path, const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity);
/*
 * Development-only zero-copy placement. The content-addressed object path is a
 * filesystem link to `source_path`; moving, deleting, or modifying the source can
 * invalidate the archive. The source is fully hashed before registration.
 */
archive_object_status_t archive_object_reference_file(
		const archive_repository_t* repository, const char* source_path,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity);
archive_object_status_t archive_object_store_bytes(const archive_repository_t* repository,
		const void* data, size_t size, const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity);
/*
 * Store an immutable canonical descriptor, recipe, or other control object. The
 * local profile keeps control objects inline in SQLite. Placement is not part of
 * the object's identity or provenance.
 */
archive_object_status_t archive_object_store_control_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity);
/*
 * Store an immutable control object directly in SQLite.
 */
archive_object_status_t archive_object_store_inline_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* media_type, archive_object_t* object,
		char* error, size_t error_capacity);
archive_object_status_t archive_object_read_inline_bytes(
		const archive_repository_t* repository, const char* sha256,
		u64 expected_size, const char* expected_media_type, size_t maximum_size,
		u8** data, size_t* size, char* error, size_t error_capacity);
/*
 * Store a validated virtual shell in the independently placeable fast tier.
 * The returned object remains content-addressed and registered in the common
 * object table; only its storage key is rooted below shells/.
 */
archive_object_status_t archive_object_store_shell_bytes(const archive_repository_t* repository,
		const void* data, size_t size, archive_object_t* object,
		char* error, size_t error_capacity);
/*
 * Atomically restore a missing or corrupt published fast-tier object. The bytes,
 * digest, size, and content-addressed key must all match the existing database
 * record; this operation does not create or change publication metadata.
 */
archive_object_status_t archive_object_repair_shell_bytes(
		const archive_repository_t* repository, const void* data, size_t size,
		const char* expected_sha256, const char* expected_storage_key,
		char* error, size_t error_capacity);
const char* archive_object_status_string(archive_object_status_t status);

#ifdef __cplusplus
}
#endif
