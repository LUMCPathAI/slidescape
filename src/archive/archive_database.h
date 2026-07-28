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

#define ARCHIVE_DATABASE_FILENAME "archive.sqlite3"
#define ARCHIVE_DATABASE_SCHEMA_VERSION 4
#define ARCHIVE_DATABASE_APPLICATION_ID 0x534C5041 /* "SLPA" */

typedef enum archive_database_status_t {
	ARCHIVE_DATABASE_OK = 0,
	ARCHIVE_DATABASE_INVALID_ARGUMENT,
	ARCHIVE_DATABASE_IO_ERROR,
	ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA,
	ARCHIVE_DATABASE_CONFLICT,
} archive_database_status_t;

typedef struct archive_database_object_t {
	i64 id;
	char sha256[65];
	u64 byte_size;
	char media_type[129];
	char storage_key[257];
	bool already_registered;
} archive_database_object_t;

typedef struct archive_database_part10_member_t {
	i64 object_id;
	char logical_name[81];
	char sop_instance_uid[65];
	char sop_class_uid[65];
	char study_instance_uid[65];
	char series_instance_uid[65];
	char transfer_syntax_uid[65];
	char modality[17];
	char study_date[9];
	bool has_instance_number;
	i64 instance_number;
	i64 frame_count;
	i64 rows;
	i64 columns;
	i64 total_pixel_matrix_rows;
	i64 total_pixel_matrix_columns;
	char image_flavor[17];
} archive_database_part10_member_t;

typedef struct archive_database_bundle_t {
	i64 id;
	char bundle_digest[65];
	i64 descriptor_object_id;
	bool already_registered;
} archive_database_bundle_t;

typedef struct archive_database_shell_instance_publication_t {
	archive_database_part10_member_t instance;
	u32 shell_instance_index;
} archive_database_shell_instance_publication_t;

typedef struct archive_database_shell_publication_t {
	char representation_id[65];
	i64 shell_object_id;
	i64 recipe_object_id;
	char representation_fingerprint[65];
	const archive_database_shell_instance_publication_t* instances;
	i32 instance_count;
	char provider_kind[17];
	char adapter_version[17];
	char frame_media_type[33];
	char frame_source[17];
	char frame_policy_version[33];
} archive_database_shell_publication_t;

/*
 * Initialization is the only operation that creates the database.
 * Validation is read-only so `check` and `serve` cannot silently repair a
 * partially initialized repository.
 */
archive_database_status_t archive_database_initialize(const archive_repository_t* repository,
		char* error, size_t error_capacity);
archive_database_status_t archive_database_validate(const archive_repository_t* repository,
		char* error, size_t error_capacity);
archive_database_status_t archive_database_register_object(
		const archive_repository_t* repository, const char* sha256, u64 byte_size,
		const char* media_type, const char* storage_key,
		archive_database_object_t* object, char* error, size_t error_capacity);
archive_database_status_t archive_database_register_inline_object(
		const archive_repository_t* repository, const char* sha256,
		const void* data, size_t size, const char* media_type,
		archive_database_object_t* object, char* error, size_t error_capacity);
archive_database_status_t archive_database_read_inline_object(
		const archive_repository_t* repository, const char* sha256,
		u64 expected_size, const char* expected_media_type, size_t maximum_size,
		u8** data, size_t* size, char* error, size_t error_capacity);
archive_database_status_t archive_database_register_part10_bundle(
		const archive_repository_t* repository, const char* bundle_digest,
		i64 descriptor_object_id, const archive_database_part10_member_t* members,
		i32 member_count, archive_database_bundle_t* bundle,
		char* error, size_t error_capacity);
archive_database_status_t archive_database_register_mrxs_bundle(
		const archive_repository_t* repository, const char* bundle_digest,
		i64 descriptor_object_id, archive_database_bundle_t* bundle,
		char* error, size_t error_capacity);
archive_database_status_t archive_database_publish_shells(
		const archive_repository_t* repository, i64 bundle_id,
		const archive_database_shell_publication_t* publications,
		i32 publication_count, char* error, size_t error_capacity);
archive_database_status_t archive_database_publish_part10_shells(
		const archive_repository_t* repository, i64 bundle_id,
		const archive_database_shell_publication_t* publications,
		i32 publication_count, char* error, size_t error_capacity);
const char* archive_database_status_string(archive_database_status_t status);

#ifdef __cplusplus
}
#endif
