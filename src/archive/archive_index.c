/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_index.h"

#include "archive_database.h"
#include "common.h"
#include "dicom_uid.h"
#include "sqlite3.h"

#include <stdarg.h>

typedef struct archive_json_writer_t {
	char* data;
	size_t capacity;
	size_t used;
	bool failed;
} archive_json_writer_t;

static void archive_index_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_study_date_is_valid(const char* date) {
	if (!date[0]) return true;
	if (strlen(date) != 8) return false;
	for (i32 i = 0; i < 8; ++i) if (date[i] < '0' || date[i] > '9') return false;
	return true;
}

static bool archive_study_record_is_valid(const archive_study_record_t* record) {
	if (!record || !dicom_uid_is_valid_string(record->study_instance_uid,
			strlen(record->study_instance_uid)) || !archive_study_date_is_valid(record->study_date)) {
		return false;
	}
	return !strchr(record->description, '\t') && !strchr(record->description, '\r') &&
	       !strchr(record->description, '\n');
}

static bool archive_index_text_is_valid(const char* value) {
	return value && !strchr(value, '\t') && !strchr(value, '\r') && !strchr(value, '\n');
}

static bool archive_index_integer_string_is_valid(const char* value, bool allow_empty) {
	if (!value || !value[0]) return allow_empty;
	for (const char* p = value; *p; ++p) if (*p < '0' || *p > '9') return false;
	return true;
}

static bool archive_index_sha256_is_valid(const char* value) {
	if (!value || strlen(value) != 64) return false;
	for (i32 i = 0; i < 64; ++i) {
		if (!((value[i] >= '0' && value[i] <= '9') ||
		      (value[i] >= 'a' && value[i] <= 'f'))) return false;
	}
	return true;
}

static bool archive_series_record_is_valid(const archive_series_record_t* record) {
	return record &&
	       dicom_uid_is_valid_string(record->study_instance_uid, strlen(record->study_instance_uid)) &&
	       dicom_uid_is_valid_string(record->series_instance_uid, strlen(record->series_instance_uid)) &&
	       archive_index_integer_string_is_valid(record->series_number, true) &&
	       archive_index_text_is_valid(record->modality) && archive_index_text_is_valid(record->description);
}

static bool archive_index_external_path_is_valid(const char* path) {
	if (!path || !path[0] ||
	    strlen(path) >= ARCHIVE_PATH_CAPACITY)
		return false;
#if defined(_WIN32)
	return ((((path[0] >= 'A' && path[0] <= 'Z') ||
	          (path[0] >= 'a' && path[0] <= 'z')) &&
	         path[1] == ':' &&
	         (path[2] == '\\' || path[2] == '/')) ||
	        (path[0] == '\\' && path[1] == '\\'));
#else
	return path[0] == '/';
#endif
}

static bool archive_instance_record_is_valid(const archive_instance_record_t* record) {
	if (!record) return false;
	bool shell_valid =
		strncmp(record->shell_key, "shells/", 7) == 0 &&
		!strstr(record->shell_key, "..") &&
		!strchr(record->shell_key, '\\') &&
		archive_index_sha256_is_valid(record->shell_sha256) &&
		record->shell_byte_size > 0 &&
		archive_index_sha256_is_valid(record->bundle_sha256) &&
		archive_index_sha256_is_valid(record->representation_fingerprint) &&
		(!record->recipe_key[0] ||
		 (strncmp(record->recipe_key, "objects/", 8) == 0 &&
		  !strstr(record->recipe_key, "..") &&
		  !strchr(record->recipe_key, '\\'))) &&
		archive_index_sha256_is_valid(record->recipe_sha256) &&
		record->recipe_byte_size > 0 &&
		dicom_uid_is_valid_string(record->transfer_syntax_uid,
		                          strlen(record->transfer_syntax_uid));
	bool provider_valid = (!record->provider[0] &&
	                       !record->object_key[0] &&
	                       !record->external_path[0]) ||
	                      (strcmp(record->provider, "part10") == 0 &&
	                       ((record->object_key[0] &&
	                         !record->external_path[0] &&
	                         strncmp(record->object_key, "objects/", 8) == 0 &&
	                         !strstr(record->object_key, "..") &&
	                         !strchr(record->object_key, '\\')) ||
	                        (!record->object_key[0] &&
	                         archive_index_external_path_is_valid(
			                         record->external_path))) &&
	                       archive_index_sha256_is_valid(record->object_sha256) &&
	                       record->object_byte_size > 0 &&
	                       shell_valid) ||
	                      (strcmp(record->provider, "mrxs-native") == 0 &&
	                       (!record->object_key[0] || (strncmp(record->object_key, "objects/sha256/", 15) == 0 &&
	                        !strstr(record->object_key, "..") && !strchr(record->object_key, '\\'))) &&
	                       !record->external_path[0] &&
	                       archive_index_sha256_is_valid(
		                       record->object_sha256) &&
	                       record->object_byte_size > 0 &&
	                       shell_valid);
	return dicom_uid_is_valid_string(record->study_instance_uid, strlen(record->study_instance_uid)) &&
	       dicom_uid_is_valid_string(record->series_instance_uid, strlen(record->series_instance_uid)) &&
	       dicom_uid_is_valid_string(record->sop_instance_uid, strlen(record->sop_instance_uid)) &&
	       dicom_uid_is_valid_string(record->sop_class_uid, strlen(record->sop_class_uid)) &&
	       archive_index_integer_string_is_valid(record->instance_number, true) &&
	       archive_index_integer_string_is_valid(record->number_of_frames, true) &&
	       archive_index_integer_string_is_valid(record->total_pixel_matrix_columns, true) &&
	       archive_index_integer_string_is_valid(record->total_pixel_matrix_rows, true) &&
	       archive_index_text_is_valid(record->image_flavor) && provider_valid;
}

static bool archive_index_append(archive_index_t* index, const archive_study_record_t* record) {
	if (index->study_count == index->study_capacity) {
		i32 capacity = index->study_capacity ? index->study_capacity * 2 : 64;
		if (capacity > ARCHIVE_INDEX_MAX_STUDIES) capacity = ARCHIVE_INDEX_MAX_STUDIES;
		if (capacity <= index->study_count) return false;
		archive_study_record_t* records = realloc(index->studies,
				(size_t)capacity * sizeof(archive_study_record_t));
		if (!records) return false;
		index->studies = records;
		index->study_capacity = capacity;
	}
	index->studies[index->study_count++] = *record;
	return true;
}

static bool archive_index_append_series(archive_index_t* index, const archive_series_record_t* record) {
	if (index->series_count == index->series_capacity) {
		i32 capacity = index->series_capacity ? index->series_capacity * 2 : 128;
		if (capacity > ARCHIVE_INDEX_MAX_SERIES) capacity = ARCHIVE_INDEX_MAX_SERIES;
		if (capacity <= index->series_count) return false;
		archive_series_record_t* records = realloc(index->series,
				(size_t)capacity * sizeof(archive_series_record_t));
		if (!records) return false;
		index->series = records;
		index->series_capacity = capacity;
	}
	index->series[index->series_count++] = *record;
	return true;
}

static bool archive_index_append_instance(archive_index_t* index, const archive_instance_record_t* record) {
	if (index->instance_count == index->instance_capacity) {
		i32 capacity = index->instance_capacity ? index->instance_capacity * 2 : 256;
		if (capacity > ARCHIVE_INDEX_MAX_INSTANCES) capacity = ARCHIVE_INDEX_MAX_INSTANCES;
		if (capacity <= index->instance_count) return false;
		archive_instance_record_t* records = realloc(index->instances,
				(size_t)capacity * sizeof(archive_instance_record_t));
		if (!records) return false;
		index->instances = records;
		index->instance_capacity = capacity;
	}
	index->instances[index->instance_count++] = *record;
	return true;
}

static bool archive_index_copy_string(char* dest, size_t capacity, const char* source) {
	i32 length = snprintf(dest, capacity, "%s", source);
	return length >= 0 && (size_t)length < capacity;
}

static bool archive_index_has_study(const archive_index_t* index, const char* study_uid) {
	i32 low = 0, high = index->study_count;
	while (low < high) {
		i32 mid = low + (high - low) / 2;
		i32 order = strcmp(index->studies[mid].study_instance_uid, study_uid);
		if (!order) return true;
		if (order < 0) low = mid + 1; else high = mid;
	}
	return false;
}

static i32 archive_index_compare_series_key(const char* study, const char* series,
		const char* requested_study, const char* requested_series) {
	i32 order = strcmp(study, requested_study);
	return order ? order : strcmp(series, requested_series);
}

static bool archive_index_has_series(const archive_index_t* index, const char* study_uid,
		const char* series_uid) {
	i32 low = 0, high = index->series_count;
	while (low < high) {
		i32 mid = low + (high - low) / 2;
		const archive_series_record_t* series = index->series + mid;
		i32 order = archive_index_compare_series_key(series->study_instance_uid,
			series->series_instance_uid, study_uid, series_uid);
		if (!order) return true;
		if (order < 0) low = mid + 1; else high = mid;
	}
	return false;
}

static int archive_index_compare_uid(const void* lhs, const void* rhs) {
	return strcmp(*(const char* const*)lhs, *(const char* const*)rhs);
}

static int archive_index_compare_instance_uid(const void* lhs, const void* rhs) {
	const archive_instance_record_t* a = *(const archive_instance_record_t* const*)lhs;
	const archive_instance_record_t* b = *(const archive_instance_record_t* const*)rhs;
	return strcmp(a->sop_instance_uid, b->sop_instance_uid);
}

static bool archive_index_build_uid_directory(archive_index_t* index) {
	/* Detect global UID reuse once, without a quadratic scan during every append. */
	if (index->series_count) {
		const char** uids = malloc((size_t)index->series_count * sizeof(*uids));
		if (!uids) return false;
		for (i32 i = 0; i < index->series_count; ++i) uids[i] = index->series[i].series_instance_uid;
		qsort(uids, index->series_count, sizeof(*uids), archive_index_compare_uid);
		bool unique = true;
		for (i32 i = 1; i < index->series_count; ++i)
			if (!strcmp(uids[i - 1], uids[i])) unique = false;
		free(uids);
		if (!unique) return false;
	}
	if (!index->instance_count) return true;
	index->instances_by_uid = malloc((size_t)index->instance_count * sizeof(*index->instances_by_uid));
	if (!index->instances_by_uid) return false;
	for (i32 i = 0; i < index->instance_count; ++i) index->instances_by_uid[i] = index->instances + i;
	qsort(index->instances_by_uid, index->instance_count, sizeof(*index->instances_by_uid),
		archive_index_compare_instance_uid);
	for (i32 i = 1; i < index->instance_count; ++i)
		if (!strcmp(index->instances_by_uid[i - 1]->sop_instance_uid,
		            index->instances_by_uid[i]->sop_instance_uid)) return false;
	return true;
}

static bool archive_index_copy_column(char* dest, size_t capacity,
		sqlite3_stmt* statement, i32 column) {
	const char* value = (const char*)sqlite3_column_text(statement, column);
	return archive_index_copy_string(dest, capacity, value ? value : "");
}

static bool archive_index_copy_integer(char* dest, size_t capacity,
		sqlite3_stmt* statement, i32 column) {
	if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
		dest[0] = '\0';
		return true;
	}
	i32 length = snprintf(dest, capacity, "%lld",
	                      (long long)sqlite3_column_int64(statement, column));
	return length >= 0 && (size_t)length < capacity;
}

static bool archive_index_prepare(sqlite3* database, const char* sql,
		sqlite3_stmt** statement, char* error, size_t error_capacity) {
	if (sqlite3_prepare_v2(database, sql, -1, statement, NULL) == SQLITE_OK) return true;
	archive_index_set_error(error, error_capacity, "could not read published archive index: %s",
	                        sqlite3_errmsg(database));
	return false;
}

bool archive_index_open(archive_index_t* index, const archive_repository_t* repository,
		char* error, size_t error_capacity) {
	if (!index || !repository) return false;
	memset(index, 0, sizeof(*index));
	if (error && error_capacity) error[0] = '\0';
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_repository_make_path(repository, ARCHIVE_DATABASE_FILENAME, path, sizeof(path))) return false;
	if (archive_database_validate(repository, error, error_capacity) != ARCHIVE_DATABASE_OK) return false;
	sqlite3* database = NULL;
	sqlite3_stmt* statement = NULL;
	if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
		archive_index_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                        path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return false;
	}
	if (sqlite3_busy_timeout(database, 5000) != SQLITE_OK ||
	    sqlite3_exec(database, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) goto sql_failure;
	/* Studies, series, and instances must come from the same publication snapshot. */
	int step_result = SQLITE_OK;
	const char* study_sql =
		"SELECT s.study_instance_uid, MIN(COALESCE(s.study_date,''))"
		" FROM study s JOIN representation r ON r.id=s.representation_id"
		" WHERE r.state='PUBLISHED' GROUP BY s.study_instance_uid"
		" ORDER BY s.study_instance_uid;";
	if (!archive_index_prepare(database, study_sql, &statement, error, error_capacity)) goto failure;
	while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
		archive_study_record_t record = {0};
		if (!archive_index_copy_column(record.study_instance_uid, sizeof(record.study_instance_uid), statement, 0) ||
		    !archive_index_copy_column(record.study_date, sizeof(record.study_date), statement, 1) ||
		    !archive_index_copy_string(record.description, sizeof(record.description), "Imported DICOM study") ||
		    !archive_study_record_is_valid(&record) || !archive_index_append(index, &record)) {
			archive_index_set_error(error, error_capacity, "published study metadata is invalid or duplicated");
			goto failure;
		}
	}
	if (step_result != SQLITE_DONE) goto sql_failure;
	sqlite3_finalize(statement);
	statement = NULL;

	const char* series_sql =
		"SELECT s.study_instance_uid, s.series_instance_uid, MIN(s.modality)"
		" FROM series s JOIN representation r ON r.id=s.representation_id"
		" WHERE r.state='PUBLISHED'"
		" GROUP BY s.study_instance_uid, s.series_instance_uid"
		" ORDER BY s.study_instance_uid, s.series_instance_uid;";
	if (!archive_index_prepare(database, series_sql, &statement, error, error_capacity)) goto failure;
	while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
		archive_series_record_t record = {0};
		if (!archive_index_copy_column(record.study_instance_uid, sizeof(record.study_instance_uid), statement, 0) ||
		    !archive_index_copy_column(record.series_instance_uid, sizeof(record.series_instance_uid), statement, 1) ||
		    !archive_index_copy_column(record.modality, sizeof(record.modality), statement, 2) ||
		    !archive_index_copy_string(record.description, sizeof(record.description), "Imported DICOM series") ||
		    !archive_series_record_is_valid(&record) ||
		    !archive_index_has_study(index, record.study_instance_uid) ||
		    !archive_index_append_series(index, &record)) {
			archive_index_set_error(error, error_capacity, "published series metadata is invalid, orphaned, or duplicated");
			goto failure;
		}
	}
	if (step_result != SQLITE_DONE) goto sql_failure;
	sqlite3_finalize(statement);
	statement = NULL;

	const char* instance_sql =
		"SELECT i.study_instance_uid, i.series_instance_uid, i.sop_instance_uid,"
		" i.sop_class_uid, i.instance_number, i.frame_count, i.image_flavor,"
		" i.total_pixel_matrix_columns, i.total_pixel_matrix_rows,"
		" COALESCE(source.storage_key,''), COALESCE(source.external_path,''),"
		" source.sha256, source.byte_size,"
		" shell.storage_key, shell.sha256, shell.byte_size,"
		" i.shell_instance_index, fr.transfer_syntax_uid, ip.provider_kind,"
		" b.bundle_digest, r.fingerprint,"
		" COALESCE(recipe.storage_key,''), recipe.sha256, recipe.byte_size"
		" FROM instance i"
		" JOIN representation r ON r.id=i.representation_id AND r.state='PUBLISHED'"
		" JOIN bundle b ON b.id=r.bundle_id"
		" JOIN instance_provider ip ON ip.instance_id=i.id"
		" JOIN object source ON source.id=ip.source_object_id"
		" JOIN object shell ON shell.id=r.shell_object_id"
		" JOIN object recipe ON recipe.id=ip.parameters_object_id"
		" JOIN frame_representation fr ON fr.instance_id=i.id "
		"   AND fr.rowid=(SELECT canonical.rowid FROM frame_representation canonical"
		"     WHERE canonical.instance_id=i.id AND canonical.source_object_id=ip.source_object_id"
		"     AND canonical.source=CASE ip.provider_kind WHEN 'part10' THEN 'STORED' ELSE 'GENERATED' END"
		"     ORDER BY canonical.rowid LIMIT 1)"
		" ORDER BY i.study_instance_uid, i.series_instance_uid,"
		" i.shell_instance_index;";
	if (!archive_index_prepare(database, instance_sql, &statement, error, error_capacity)) goto failure;
	while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
		archive_instance_record_t record = {0};
		if (!archive_index_copy_column(record.study_instance_uid, sizeof(record.study_instance_uid), statement, 0) ||
		    !archive_index_copy_column(record.series_instance_uid, sizeof(record.series_instance_uid), statement, 1) ||
		    !archive_index_copy_column(record.sop_instance_uid, sizeof(record.sop_instance_uid), statement, 2) ||
		    !archive_index_copy_column(record.sop_class_uid, sizeof(record.sop_class_uid), statement, 3) ||
		    !archive_index_copy_integer(record.instance_number, sizeof(record.instance_number), statement, 4) ||
		    !archive_index_copy_integer(record.number_of_frames, sizeof(record.number_of_frames), statement, 5) ||
		    !archive_index_copy_column(record.image_flavor, sizeof(record.image_flavor), statement, 6) ||
		    !archive_index_copy_integer(record.total_pixel_matrix_columns,
		                                sizeof(record.total_pixel_matrix_columns), statement, 7) ||
		    !archive_index_copy_integer(record.total_pixel_matrix_rows,
		                                sizeof(record.total_pixel_matrix_rows), statement, 8) ||
		    !archive_index_copy_column(record.provider, sizeof(record.provider), statement, 18) ||
		    !archive_index_copy_column(record.object_key, sizeof(record.object_key), statement, 9) ||
		    !archive_index_copy_column(record.external_path,
		                               sizeof(record.external_path), statement, 10) ||
		    !archive_index_copy_column(record.object_sha256, sizeof(record.object_sha256), statement, 11) ||
		    (record.object_byte_size = (u64)sqlite3_column_int64(statement, 12)) == 0 ||
		    !archive_index_copy_column(record.shell_key, sizeof(record.shell_key), statement, 13) ||
		    !archive_index_copy_column(record.shell_sha256, sizeof(record.shell_sha256), statement, 14) ||
		    (record.shell_byte_size = (u64)sqlite3_column_int64(statement, 15)) == 0 ||
		    (record.shell_instance_index = (u32)sqlite3_column_int64(statement, 16)) !=
		        (u64)sqlite3_column_int64(statement, 16) ||
		    !archive_index_copy_column(record.transfer_syntax_uid,
		                               sizeof(record.transfer_syntax_uid), statement, 17) ||
		    !archive_index_copy_column(record.bundle_sha256,
		                               sizeof(record.bundle_sha256), statement, 19) ||
		    !archive_index_copy_column(record.representation_fingerprint,
		                               sizeof(record.representation_fingerprint), statement, 20) ||
		    !archive_index_copy_column(record.recipe_key,
		                               sizeof(record.recipe_key), statement, 21) ||
		    !archive_index_copy_column(record.recipe_sha256,
		                               sizeof(record.recipe_sha256), statement, 22) ||
		    (record.recipe_byte_size = (u64)sqlite3_column_int64(statement, 23)) == 0 ||
		    !archive_instance_record_is_valid(&record) ||
		    !archive_index_has_series(index, record.study_instance_uid, record.series_instance_uid) ||
		    !archive_index_append_instance(index, &record)) {
			archive_index_set_error(error, error_capacity, "published instance metadata is invalid, orphaned, or duplicated");
			goto failure;
		}
	}
	if (step_result != SQLITE_DONE) goto sql_failure;
	sqlite3_finalize(statement);
	statement = NULL;
	if (!archive_index_build_uid_directory(index)) {
		archive_index_set_error(error, error_capacity, "duplicate published UID or insufficient index memory");
		goto failure;
	}
	for (i32 i = 0; i < index->series_count; ++i) {
		const archive_instance_record_t* records = NULL;
		i32 count = 0;
		if (!archive_index_find_series_instances(
				index, index->series[i].study_instance_uid,
				index->series[i].series_instance_uid,
				&records, &count)) {
			archive_index_set_error(
				error, error_capacity,
				"published series instance directory is missing or non-contiguous");
			goto failure;
		}
	}
	if (sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto sql_failure;
	sqlite3_close(database);
	return true;

sql_failure:
	archive_index_set_error(error, error_capacity, "could not read published archive index: %s",
	                        sqlite3_errmsg(database));
failure:
	sqlite3_finalize(statement);
	sqlite3_close(database);
	archive_index_destroy(index);
	return false;
}

void archive_index_destroy(archive_index_t* index) {
	if (!index) return;
	free(index->studies);
	free(index->series);
	free(index->instances_by_uid);
	free(index->instances);
	memset(index, 0, sizeof(*index));
}

const archive_instance_record_t* archive_index_find_instance(const archive_index_t* index,
		const char* study_instance_uid, const char* series_instance_uid,
		const char* sop_instance_uid) {
	if (!index || !study_instance_uid || !series_instance_uid || !sop_instance_uid) return NULL;
	i32 low = 0, high = index->instance_count;
	while (low < high && index->instances_by_uid) {
		i32 mid = low + (high - low) / 2;
		const archive_instance_record_t* record = index->instances_by_uid[mid];
		i32 order = strcmp(record->sop_instance_uid, sop_instance_uid);
		if (!order) return archive_index_compare_series_key(record->study_instance_uid,
			record->series_instance_uid, study_instance_uid, series_instance_uid) == 0 ? record : NULL;
		if (order < 0) low = mid + 1; else high = mid;
	}
	return NULL;
}

bool archive_index_find_series_instances(const archive_index_t* index,
		const char* study_instance_uid, const char* series_instance_uid,
		const archive_instance_record_t** records, i32* count) {
	if (records) *records = NULL;
	if (count) *count = 0;
	if (!index || !study_instance_uid || !series_instance_uid || !records || !count)
		return false;
	i32 low = 0, high = index->instance_count;
	while (low < high) {
		i32 mid = low + (high - low) / 2;
		const archive_instance_record_t* record = index->instances + mid;
		i32 order = archive_index_compare_series_key(record->study_instance_uid,
			record->series_instance_uid, study_instance_uid, series_instance_uid);
		if (order < 0) low = mid + 1; else high = mid;
	}
	i32 first = low;
	i32 found = 0;
	for (i32 i = first; i < index->instance_count; ++i) {
		const archive_instance_record_t* record = index->instances + i;
		bool matches =
			strcmp(record->study_instance_uid, study_instance_uid) == 0 &&
			strcmp(record->series_instance_uid, series_instance_uid) == 0;
		if (!matches) break;
		if (record->shell_instance_index != (u32)found) return false;
		++found;
	}
	if (found == 0) return false;
	*records = index->instances + first;
	*count = found;
	return true;
}

static bool archive_parse_nonnegative_i32(const char* text, size_t length, i32 maximum, i32* value) {
	if (!length || length > 10) return false;
	u64 parsed = 0;
	for (size_t i = 0; i < length; ++i) {
		if (text[i] < '0' || text[i] > '9') return false;
		parsed = parsed * 10 + (u64)(text[i] - '0');
		if (parsed > (u64)maximum) return false;
	}
	*value = (i32)parsed;
	return true;
}

bool archive_study_query_parse(const char* target, archive_study_query_t* query) {
	if (!target || !query) return false;
	memset(query, 0, sizeof(*query));
	query->limit = ARCHIVE_QIDO_DEFAULT_LIMIT;
	const char* cursor = strchr(target, '?');
	if (!cursor) return true;
	++cursor;
	while (*cursor) {
		const char* separator = strchr(cursor, '&');
		const char* end = separator ? separator : cursor + strlen(cursor);
		const char* equals = memchr(cursor, '=', (size_t)(end - cursor));
		if (!equals || equals == cursor) return false;
		size_t name_length = (size_t)(equals - cursor);
		const char* value = equals + 1;
		size_t value_length = (size_t)(end - value);
		if (name_length == 5 && memcmp(cursor, "limit", 5) == 0) {
			if (!archive_parse_nonnegative_i32(value, value_length, ARCHIVE_QIDO_MAX_LIMIT, &query->limit) ||
			    query->limit == 0) return false;
		} else if (name_length == 6 && memcmp(cursor, "offset", 6) == 0) {
			if (!archive_parse_nonnegative_i32(value, value_length, INT32_MAX, &query->offset)) return false;
		} else if (name_length == 8 && memcmp(cursor, "0020000D", 8) == 0) {
			if (value_length >= sizeof(query->study_instance_uid) ||
			    !dicom_uid_is_valid_string(value, value_length)) return false;
			memcpy(query->study_instance_uid, value, value_length);
			query->study_instance_uid[value_length] = '\0';
		}
		cursor = separator ? separator + 1 : end;
	}
	return true;
}

static bool archive_qido_query_parse_common(const char* target, i32* limit, i32* offset,
		char* uid, size_t uid_capacity, const char* uid_tag) {
	*limit = ARCHIVE_QIDO_DEFAULT_LIMIT;
	*offset = 0;
	const char* cursor = strchr(target, '?');
	if (!cursor) return true;
	++cursor;
	while (*cursor) {
		const char* separator = strchr(cursor, '&');
		const char* end = separator ? separator : cursor + strlen(cursor);
		const char* equals = memchr(cursor, '=', (size_t)(end - cursor));
		if (!equals || equals == cursor) return false;
		size_t name_length = (size_t)(equals - cursor);
		const char* value = equals + 1;
		size_t value_length = (size_t)(end - value);
		if (name_length == 5 && memcmp(cursor, "limit", 5) == 0) {
			if (!archive_parse_nonnegative_i32(value, value_length, ARCHIVE_QIDO_MAX_LIMIT, limit) ||
			    *limit == 0) return false;
		} else if (name_length == 6 && memcmp(cursor, "offset", 6) == 0) {
			if (!archive_parse_nonnegative_i32(value, value_length, INT32_MAX, offset)) return false;
		} else if (strlen(uid_tag) == name_length && memcmp(cursor, uid_tag, name_length) == 0) {
			if (value_length >= uid_capacity || !dicom_uid_is_valid_string(value, value_length)) return false;
			memcpy(uid, value, value_length);
			uid[value_length] = '\0';
		}
		cursor = separator ? separator + 1 : end;
	}
	return true;
}

bool archive_series_query_parse(const char* target, const char* study_instance_uid,
		archive_series_query_t* query) {
	if (!target || !study_instance_uid || !query ||
	    !dicom_uid_is_valid_string(study_instance_uid, strlen(study_instance_uid))) return false;
	memset(query, 0, sizeof(*query));
	if (!archive_index_copy_string(query->study_instance_uid, sizeof(query->study_instance_uid),
	                               study_instance_uid)) return false;
	return archive_qido_query_parse_common(target, &query->limit, &query->offset,
	                                       query->series_instance_uid,
	                                       sizeof(query->series_instance_uid), "0020000E");
}

bool archive_instance_query_parse(const char* target, const char* study_instance_uid,
		const char* series_instance_uid, archive_instance_query_t* query) {
	if (!target || !study_instance_uid || !series_instance_uid || !query ||
	    !dicom_uid_is_valid_string(study_instance_uid, strlen(study_instance_uid)) ||
	    !dicom_uid_is_valid_string(series_instance_uid, strlen(series_instance_uid))) return false;
	memset(query, 0, sizeof(*query));
	if (!archive_index_copy_string(query->study_instance_uid, sizeof(query->study_instance_uid), study_instance_uid) ||
	    !archive_index_copy_string(query->series_instance_uid, sizeof(query->series_instance_uid), series_instance_uid)) {
		return false;
	}
	return archive_qido_query_parse_common(target, &query->limit, &query->offset,
	                                       query->sop_instance_uid,
	                                       sizeof(query->sop_instance_uid), "00080018");
}

static void archive_json_write(archive_json_writer_t* writer, const char* text, size_t length) {
	if (writer->failed) return;
	if (length >= writer->capacity - writer->used) {
		writer->failed = true;
		return;
	}
	memcpy(writer->data + writer->used, text, length);
	writer->used += length;
	writer->data[writer->used] = '\0';
}

static void archive_json_write_literal(archive_json_writer_t* writer, const char* text) {
	archive_json_write(writer, text, strlen(text));
}

static void archive_json_write_string(archive_json_writer_t* writer, const char* value) {
	archive_json_write_literal(writer, "\"");
	for (const u8* p = (const u8*)value; *p; ++p) {
		if (*p == '"') archive_json_write_literal(writer, "\\\"");
		else if (*p == '\\') archive_json_write_literal(writer, "\\\\");
		else if (*p == '\n') archive_json_write_literal(writer, "\\n");
		else if (*p == '\r') archive_json_write_literal(writer, "\\r");
		else if (*p == '\t') archive_json_write_literal(writer, "\\t");
		else if (*p < 0x20) writer->failed = true;
		else archive_json_write(writer, (const char*)p, 1);
	}
	archive_json_write_literal(writer, "\"");
}

static void archive_json_write_tag_string(archive_json_writer_t* writer, const char* tag,
		const char* vr, const char* value, bool* first) {
	if (!value[0]) return;
	if (!*first) archive_json_write_literal(writer, ",");
	*first = false;
	archive_json_write_literal(writer, "\"");
	archive_json_write_literal(writer, tag);
	archive_json_write_literal(writer, "\":{\"vr\":\"");
	archive_json_write_literal(writer, vr);
	archive_json_write_literal(writer, "\",\"Value\":[");
	archive_json_write_string(writer, value);
	archive_json_write_literal(writer, "]}");
}

static void archive_json_write_tag_unsigned(archive_json_writer_t* writer, const char* tag,
		const char* value, bool* first) {
	if (!value[0]) return;
	if (!*first) archive_json_write_literal(writer, ",");
	*first = false;
	archive_json_write_literal(writer, "\"");
	archive_json_write_literal(writer, tag);
	archive_json_write_literal(writer, "\":{\"vr\":\"UL\",\"Value\":[");
	archive_json_write_literal(writer, value);
	archive_json_write_literal(writer, "]}");
}

static void archive_json_write_image_type(archive_json_writer_t* writer, const char* flavor,
		bool* first) {
	if (!flavor[0]) return;
	if (!*first) archive_json_write_literal(writer, ",");
	*first = false;
	archive_json_write_literal(writer,
		"\"00080008\":{\"vr\":\"CS\",\"Value\":[\"ORIGINAL\",\"PRIMARY\",");
	archive_json_write_string(writer, flavor);
	archive_json_write_literal(writer, ",\"NONE\"]}");
}

bool archive_index_write_studies_json(const archive_index_t* index,
		const archive_study_query_t* query, char* dest, size_t capacity, size_t* size) {
	if (!index || !query || !dest || capacity == 0 || !size) return false;
	archive_json_writer_t writer = {dest, capacity, 0, false};
	archive_json_write_literal(&writer, "[");
	i32 skipped = 0;
	i32 written = 0;
	for (i32 i = 0; i < index->study_count && written < query->limit; ++i) {
		const archive_study_record_t* record = index->studies + i;
		if (query->study_instance_uid[0] &&
		    strcmp(query->study_instance_uid, record->study_instance_uid) != 0) continue;
		if (skipped++ < query->offset) continue;
		if (written++) archive_json_write_literal(&writer, ",");
		archive_json_write_literal(&writer, "{\"0020000D\":{\"vr\":\"UI\",\"Value\":[");
		archive_json_write_string(&writer, record->study_instance_uid);
		archive_json_write_literal(&writer, "]}");
		if (record->study_date[0]) {
			archive_json_write_literal(&writer, ",\"00080020\":{\"vr\":\"DA\",\"Value\":[");
			archive_json_write_string(&writer, record->study_date);
			archive_json_write_literal(&writer, "]}");
		}
		if (record->description[0]) {
			archive_json_write_literal(&writer, ",\"00081030\":{\"vr\":\"LO\",\"Value\":[");
			archive_json_write_string(&writer, record->description);
			archive_json_write_literal(&writer, "]}");
		}
		archive_json_write_literal(&writer, "}");
	}
	archive_json_write_literal(&writer, "]\n");
	if (writer.failed) {
		dest[0] = '\0';
		return false;
	}
	*size = writer.used;
	return true;
}

bool archive_index_write_series_json(const archive_index_t* index,
		const archive_series_query_t* query, char* dest, size_t capacity, size_t* size) {
	if (!index || !query || !dest || capacity == 0 || !size) return false;
	archive_json_writer_t writer = {dest, capacity, 0, false};
	archive_json_write_literal(&writer, "[");
	i32 skipped = 0;
	i32 written = 0;
	for (i32 i = 0; i < index->series_count && written < query->limit; ++i) {
		const archive_series_record_t* record = index->series + i;
		if (strcmp(query->study_instance_uid, record->study_instance_uid) != 0) continue;
		if (query->series_instance_uid[0] &&
		    strcmp(query->series_instance_uid, record->series_instance_uid) != 0) continue;
		if (skipped++ < query->offset) continue;
		if (written++) archive_json_write_literal(&writer, ",");
		archive_json_write_literal(&writer, "{");
		bool first = true;
		archive_json_write_tag_string(&writer, "0020000E", "UI", record->series_instance_uid, &first);
		archive_json_write_tag_string(&writer, "00200011", "IS", record->series_number, &first);
		archive_json_write_tag_string(&writer, "00080060", "CS", record->modality, &first);
		archive_json_write_tag_string(&writer, "0008103E", "LO", record->description, &first);
		archive_json_write_literal(&writer, "}");
	}
	archive_json_write_literal(&writer, "]\n");
	if (writer.failed) {
		dest[0] = '\0';
		return false;
	}
	*size = writer.used;
	return true;
}

bool archive_index_write_instances_json(const archive_index_t* index,
		const archive_instance_query_t* query, char* dest, size_t capacity, size_t* size) {
	if (!index || !query || !dest || capacity == 0 || !size) return false;
	archive_json_writer_t writer = {dest, capacity, 0, false};
	archive_json_write_literal(&writer, "[");
	i32 skipped = 0;
	i32 written = 0;
	for (i32 i = 0; i < index->instance_count && written < query->limit; ++i) {
		const archive_instance_record_t* record = index->instances + i;
		if (strcmp(query->study_instance_uid, record->study_instance_uid) != 0 ||
		    strcmp(query->series_instance_uid, record->series_instance_uid) != 0) continue;
		if (query->sop_instance_uid[0] &&
		    strcmp(query->sop_instance_uid, record->sop_instance_uid) != 0) continue;
		if (skipped++ < query->offset) continue;
		if (written++) archive_json_write_literal(&writer, ",");
		archive_json_write_literal(&writer, "{");
		bool first = true;
		archive_json_write_tag_string(&writer, "00080016", "UI", record->sop_class_uid, &first);
		archive_json_write_tag_string(&writer, "00080018", "UI", record->sop_instance_uid, &first);
		archive_json_write_tag_string(&writer, "00200013", "IS", record->instance_number, &first);
		archive_json_write_tag_string(&writer, "00280008", "IS", record->number_of_frames, &first);
		archive_json_write_image_type(&writer, record->image_flavor, &first);
		archive_json_write_tag_unsigned(&writer, "00480006", record->total_pixel_matrix_columns, &first);
		archive_json_write_tag_unsigned(&writer, "00480007", record->total_pixel_matrix_rows, &first);
		archive_json_write_literal(&writer, "}");
	}
	archive_json_write_literal(&writer, "]\n");
	if (writer.failed) {
		dest[0] = '\0';
		return false;
	}
	*size = writer.used;
	return true;
}
