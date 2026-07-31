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

#define ARCHIVE_INDEX_MAX_STUDIES 100000
#define ARCHIVE_INDEX_MAX_SERIES 1000000
#define ARCHIVE_INDEX_MAX_INSTANCES 10000000
#define ARCHIVE_QIDO_DEFAULT_LIMIT 100
#define ARCHIVE_QIDO_MAX_LIMIT 1000

typedef struct archive_study_record_t {
	char study_instance_uid[65];
	char study_date[9];
	char description[129];
} archive_study_record_t;

typedef struct archive_series_record_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char series_number[17];
	char modality[17];
	char description[129];
} archive_series_record_t;

typedef struct archive_instance_record_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
	char sop_class_uid[65];
	char instance_number[17];
	char number_of_frames[17];
	char image_flavor[17];
	char total_pixel_matrix_columns[17];
	char total_pixel_matrix_rows[17];
	/*
	 * These two fields select the byte provider; they are not part of QIDO output.
	 * Owned `object_key` values are repository-relative. Development references
	 * instead use an explicit absolute `external_path`.
	 */
	char provider[17];
	char object_key[257];
	char external_path[ARCHIVE_PATH_CAPACITY];
	char object_sha256[65];
	u64 object_byte_size;
	char shell_key[257];
	char shell_sha256[65];
	u64 shell_byte_size;
	u32 shell_instance_index;
	char bundle_sha256[65];
	char representation_fingerprint[65];
	char recipe_key[257];
	char recipe_sha256[65];
	u64 recipe_byte_size;
	char transfer_syntax_uid[65];
} archive_instance_record_t;

typedef struct archive_index_t {
	archive_study_record_t* studies;
	i32 study_count;
	i32 study_capacity;
	archive_series_record_t* series;
	i32 series_count;
	i32 series_capacity;
	archive_instance_record_t* instances;
	i32 instance_count;
	i32 instance_capacity;
} archive_index_t;

typedef struct archive_study_query_t {
	char study_instance_uid[65];
	i32 limit;
	i32 offset;
} archive_study_query_t;

typedef struct archive_series_query_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	i32 limit;
	i32 offset;
} archive_series_query_t;

typedef struct archive_instance_query_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
	i32 limit;
	i32 offset;
} archive_instance_query_t;

bool archive_index_open(archive_index_t* index, const archive_repository_t* repository,
		char* error, size_t error_capacity);
void archive_index_destroy(archive_index_t* index);
const archive_instance_record_t* archive_index_find_instance(const archive_index_t* index,
		const char* study_instance_uid, const char* series_instance_uid,
		const char* sop_instance_uid);
bool archive_index_find_series_instances(const archive_index_t* index,
		const char* study_instance_uid, const char* series_instance_uid,
		const archive_instance_record_t** records, i32* count);
bool archive_study_query_parse(const char* target, archive_study_query_t* query);
bool archive_series_query_parse(const char* target, const char* study_instance_uid,
		archive_series_query_t* query);
bool archive_instance_query_parse(const char* target, const char* study_instance_uid,
		const char* series_instance_uid, archive_instance_query_t* query);
bool archive_index_write_studies_json(const archive_index_t* index,
		const archive_study_query_t* query, char* dest, size_t capacity, size_t* size);
bool archive_index_write_series_json(const archive_index_t* index,
		const archive_series_query_t* query, char* dest, size_t capacity, size_t* size);
bool archive_index_write_instances_json(const archive_index_t* index,
		const archive_instance_query_t* query, char* dest, size_t capacity, size_t* size);

#ifdef __cplusplus
}
#endif
