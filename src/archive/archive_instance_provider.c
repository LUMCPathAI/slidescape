/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_instance_provider.h"

#include "archive_database.h"
#include "archive_metadata.h"
#include "archive_object.h"
#include "archive_mrxs_adapter.h"
#include "archive_sha256.h"
#include "archive_shell.h"
#include "archive_shell_recipe.h"
#include "dicom.h"
#include "jpeg_decoder.h"
#include "jpeg2000_decoder.h"

#include <math.h>
#include <stdarg.h>

#define ARCHIVE_PROVIDER_MAX_FRAME_SIZE MEGABYTES(64)
#define ARCHIVE_PROVIDER_WORK_QUEUE_CAPACITY 64
#define ARCHIVE_PROVIDER_CACHE_CAPACITY 16
#define ARCHIVE_PROVIDER_SHELL_BYTE_BUDGET MEGABYTES(512)
#define ARCHIVE_PROVIDER_PAYLOAD_BYTE_BUDGET MEGABYTES(64)
#define ARCHIVE_PROVIDER_MRXS_FILE_BUDGET 256
#define ARCHIVE_PROVIDER_MRXS_DESCRIPTOR_MAX MEGABYTES(16)
/*
 * Leave room to acquire a new shell before evicting the per-instance entry that
 * may hold the final reference to an older shell.
 */
#define ARCHIVE_PROVIDER_SHELL_CACHE_CAPACITY 32

static void archive_provider_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_provider_owns_global_thread_pool;

typedef struct archive_provider_mrxs_sources_t {
	file_handle_t* files;
	u64* sizes;
	u32 count;
	bool handles_reserved;
} archive_provider_mrxs_sources_t;

typedef struct archive_provider_shell_cache_entry_t {
	bool occupied;
	i32 refcount;
	u64 last_used;
	char repository_root[ARCHIVE_PATH_CAPACITY];
	char shell_key[sizeof(((archive_instance_record_t*)0)->shell_key)];
	char shell_sha256[65];
	u8* data;
	size_t size;
	archive_shell_view_t shell;
	archive_provider_mrxs_sources_t* mrxs_sources;
} archive_provider_shell_cache_entry_t;

typedef struct archive_provider_cache_entry_t {
	bool occupied;
	i32 refcount;
	u64 last_used;
	char repository_root[ARCHIVE_PATH_CAPACITY];
	char shell_key[sizeof(((archive_instance_record_t*)0)->shell_key)];
	u32 shell_instance_index;
	archive_provider_shell_cache_entry_t* shared_shell;
	archive_shell_instance_t instance;
	file_handle_t source_file;
	archive_shell_payload_range_t* payload_ranges;
	size_t payload_bytes;
} archive_provider_cache_entry_t;

static platform_mutex_t archive_provider_cache_mutex = PLATFORM_MUTEX_INITIALIZER;
static platform_mutex_t archive_provider_regeneration_mutex = PLATFORM_MUTEX_INITIALIZER;
static archive_provider_cache_entry_t archive_provider_cache[ARCHIVE_PROVIDER_CACHE_CAPACITY];
static archive_provider_shell_cache_entry_t
	archive_provider_shell_cache[ARCHIVE_PROVIDER_SHELL_CACHE_CAPACITY];
/* Reservations include in-progress cold loads, not just resident entries. */
static size_t archive_provider_shell_bytes;
static size_t archive_provider_payload_bytes;
static platform_mutex_t archive_provider_file_budget_mutex = PLATFORM_MUTEX_INITIALIZER;
static u32 archive_provider_mrxs_files;

/* Separate striped admission locks coalesce same-key cold loads while allowing
 * unrelated shells to load concurrently. Lock order: instance, shell, cache. */
#define ARCHIVE_PROVIDER_LOAD_LOCKS { PLATFORM_MUTEX_INITIALIZER, PLATFORM_MUTEX_INITIALIZER, \
	PLATFORM_MUTEX_INITIALIZER, PLATFORM_MUTEX_INITIALIZER, PLATFORM_MUTEX_INITIALIZER, \
	PLATFORM_MUTEX_INITIALIZER, PLATFORM_MUTEX_INITIALIZER, PLATFORM_MUTEX_INITIALIZER }
static platform_mutex_t archive_provider_instance_load_mutexes[8] = ARCHIVE_PROVIDER_LOAD_LOCKS;
static platform_mutex_t archive_provider_shell_load_mutexes[8] = ARCHIVE_PROVIDER_LOAD_LOCKS;
#undef ARCHIVE_PROVIDER_LOAD_LOCKS

static u32 archive_provider_load_stripe(const archive_instance_record_t* record, bool instance) {
	u32 hash = instance ? record->shell_instance_index : 0;
	for (const u8* p = (const u8*)record->shell_key; *p; ++p) hash = hash * 33 + *p;
	return hash % COUNT(archive_provider_instance_load_mutexes);
}

static u64 archive_provider_cache_clock;
static archive_instance_provider_cache_stats_t archive_provider_cache_stats;

static void archive_provider_mrxs_sources_destroy(
		archive_provider_mrxs_sources_t* sources) {
	if (!sources) return;
	if (sources->files) for (u32 i = 0; i < sources->count; ++i)
		file_handle_close(sources->files[i]);
	if (sources->handles_reserved) {
		platform_mutex_lock(&archive_provider_file_budget_mutex);
		archive_provider_mrxs_files -= sources->count;
		platform_mutex_unlock(&archive_provider_file_budget_mutex);
	}
	free(sources->sizes);
	free(sources->files);
	free(sources);
}

static void archive_provider_shell_cache_entry_destroy(
		archive_provider_shell_cache_entry_t* entry) {
	if (!entry || !entry->occupied) return;
	ASSERT(entry->refcount == 0);
	archive_provider_shell_bytes -= entry->size;
	free(entry->data);
	archive_provider_mrxs_sources_destroy(entry->mrxs_sources);
	memset(entry, 0, sizeof(*entry));
}

static void archive_provider_cache_entry_destroy(archive_provider_cache_entry_t* entry) {
	if (!entry || !entry->occupied) return;
	archive_provider_payload_bytes -= entry->payload_bytes;
	free(entry->payload_ranges);
	file_handle_close(entry->source_file);
	if (entry->shared_shell) {
		ASSERT(entry->shared_shell->occupied &&
		       entry->shared_shell->refcount > 0);
		--entry->shared_shell->refcount;
	}
	memset(entry, 0, sizeof(*entry));
}

/* Caller holds the cache mutex. Eviction never invalidates an acquired view. */
static bool archive_provider_evict_idle_instance(void) {
	archive_provider_cache_entry_t* oldest = NULL;
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		archive_provider_cache_entry_t* entry = archive_provider_cache + i;
		if (entry->occupied && !entry->refcount && (!oldest || entry->last_used < oldest->last_used))
			oldest = entry;
	}
	if (!oldest) return false;
	++archive_provider_cache_stats.evictions;
	archive_provider_cache_entry_destroy(oldest);
	return true;
}

static bool archive_provider_evict_idle_shell(void) {
	archive_provider_shell_cache_entry_t* oldest = NULL;
	for (i32 i = 0; i < COUNT(archive_provider_shell_cache); ++i) {
		archive_provider_shell_cache_entry_t* entry = archive_provider_shell_cache + i;
		if (entry->occupied && !entry->refcount && (!oldest || entry->last_used < oldest->last_used))
			oldest = entry;
	}
	if (!oldest) return false;
	++archive_provider_cache_stats.shell_evictions;
	archive_provider_shell_cache_entry_destroy(oldest);
	return true;
}

static bool archive_provider_reserve_shell_bytes(size_t bytes) {
	if (bytes > ARCHIVE_PROVIDER_SHELL_BYTE_BUDGET) return false;
	while (archive_provider_shell_bytes > ARCHIVE_PROVIDER_SHELL_BYTE_BUDGET - bytes) {
		if (!archive_provider_evict_idle_shell() && !archive_provider_evict_idle_instance()) {
			return false;
		}
	}
	archive_provider_shell_bytes += bytes;
	return true;
}

static bool archive_provider_reserve_payload_bytes(size_t bytes) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	bool admitted = bytes <= ARCHIVE_PROVIDER_PAYLOAD_BYTE_BUDGET;
	while (admitted && archive_provider_payload_bytes > ARCHIVE_PROVIDER_PAYLOAD_BYTE_BUDGET - bytes)
		admitted = archive_provider_evict_idle_instance();
	if (admitted) archive_provider_payload_bytes += bytes;
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return admitted;
}

static bool archive_provider_reserve_mrxs_files(u32 count) {
	if (count > ARCHIVE_PROVIDER_MRXS_FILE_BUDGET) return false;
	platform_mutex_lock(&archive_provider_cache_mutex);
	bool admitted = false;
	for (;;) {
		platform_mutex_lock(&archive_provider_file_budget_mutex);
		admitted = archive_provider_mrxs_files <= ARCHIVE_PROVIDER_MRXS_FILE_BUDGET - count;
		if (admitted) archive_provider_mrxs_files += count;
		platform_mutex_unlock(&archive_provider_file_budget_mutex);
		if (admitted || (!archive_provider_evict_idle_shell() && !archive_provider_evict_idle_instance()))
			break;
	}
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return admitted;
}

static void archive_provider_release_load_bytes(size_t shell_bytes, size_t payload_bytes) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_provider_shell_bytes -= shell_bytes;
	archive_provider_payload_bytes -= payload_bytes;
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

static void archive_provider_cache_destroy(void) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		ASSERT(archive_provider_cache[i].refcount == 0);
		archive_provider_cache_entry_destroy(archive_provider_cache + i);
	}
	for (i32 i = 0; i < COUNT(archive_provider_shell_cache); ++i) {
		archive_provider_shell_cache_entry_destroy(
			archive_provider_shell_cache + i);
	}
	ASSERT(archive_provider_shell_bytes == 0 && archive_provider_payload_bytes == 0);
	archive_provider_cache_clock = 0;
	memset(&archive_provider_cache_stats, 0, sizeof(archive_provider_cache_stats));
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

archive_instance_provider_cache_stats_t archive_instance_provider_get_cache_stats(void) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_instance_provider_cache_stats_t result = archive_provider_cache_stats;
	result.shell_bytes = archive_provider_shell_bytes;
	result.payload_bytes = archive_provider_payload_bytes;
	result.capacity = ARCHIVE_PROVIDER_CACHE_CAPACITY;
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		if (archive_provider_cache[i].occupied) ++result.resident_count;
	}
	result.shell_capacity = ARCHIVE_PROVIDER_SHELL_CACHE_CAPACITY;
	for (i32 i = 0; i < COUNT(archive_provider_shell_cache); ++i) {
		if (archive_provider_shell_cache[i].occupied)
			++result.shell_resident_count;
	}
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return result;
}

bool archive_instance_provider_initialize(void) {
	/*
	 * Archive parsing and native adapters share Slidescape's worker/runtime contract:
	 * every request runs on a logical thread with a private temporary arena. Keep DICOM
	 * dictionary initialization ahead of worker dispatch; dicom_init() is idempotent,
	 * but its first initialization is deliberately serialized here on the main thread.
	 */
	if (!dicom_init()) return false;
	if (!global_thread_pool.initialized) {
		init_thread_pool(&global_thread_pool, ARCHIVE_PROVIDER_WORK_QUEUE_CAPACITY,
		                 true, false, NULL);
		archive_provider_owns_global_thread_pool = true;
	}
	return global_thread_pool.initialized && threadlocal_thread_memory != NULL;
}

void archive_instance_provider_shutdown(void) {
	if (archive_provider_owns_global_thread_pool) {
		thread_pool_destroy(&global_thread_pool);
		archive_provider_owns_global_thread_pool = false;
	}
	archive_provider_cache_destroy();
}

static bool archive_provider_integer_matches(const char* published, u64 parsed) {
	if (!published[0]) return true;
	char value[32];
	i32 length = snprintf(value, sizeof(value), "%llu", (unsigned long long)parsed);
	return length > 0 && (size_t)length < sizeof(value) && strcmp(value, published) == 0;
}

static bool archive_provider_record_matches_shell(
		const archive_instance_record_t* record,
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		char* error, size_t error_capacity) {
	archive_shell_source_t source = {0};
	char source_sha256[65];
	char representation_fingerprint[65];
	if (!archive_shell_get_source(shell, instance->source_index, &source)) return false;
	archive_sha256_hex(source.sha256, source_sha256);
	archive_sha256_hex(shell->representation_fingerprint,
	                   representation_fingerprint);
	if (strcmp(record->study_instance_uid, instance->study_instance_uid) != 0 ||
	    strcmp(record->series_instance_uid, instance->series_instance_uid) != 0 ||
	    strcmp(record->sop_instance_uid, instance->sop_instance_uid) != 0 ||
	    strcmp(record->sop_class_uid, instance->sop_class_uid) != 0 ||
	    strcmp(record->representation_fingerprint,
	           representation_fingerprint) != 0 ||
	    strcmp(record->transfer_syntax_uid, instance->transfer_syntax_uid) != 0 ||
	    strcmp(record->image_flavor, instance->image_flavor) != 0 ||
	    strcmp(record->object_sha256, source_sha256) != 0 ||
	    record->object_byte_size != source.byte_size ||
	    !archive_provider_integer_matches(
		    record->instance_number, (u64)instance->instance_number) ||
	    !archive_provider_integer_matches(record->number_of_frames, instance->frame_count) ||
	    !archive_provider_integer_matches(record->total_pixel_matrix_columns,
	                                      instance->total_pixel_matrix_columns) ||
	    !archive_provider_integer_matches(record->total_pixel_matrix_rows,
	                                      instance->total_pixel_matrix_rows)) {
		archive_provider_set_error(error, error_capacity,
		                           "virtual DICOM shell does not match its published index record");
		return false;
	}
	return true;
}



static bool archive_provider_source_path(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		char path[ARCHIVE_PATH_CAPACITY]) {
	if (!repository || !record || !path) return false;
	if (record->external_path[0]) {
		i32 length = snprintf(
			path, ARCHIVE_PATH_CAPACITY, "%s",
			record->external_path);
		return length > 0 &&
			length < ARCHIVE_PATH_CAPACITY;
	}
	return record->object_key[0] &&
		archive_repository_make_path(
			repository, record->object_key, path,
			ARCHIVE_PATH_CAPACITY);
}

static bool archive_provider_read_exact_file(const char* path, u64 expected_size,
		u64 maximum_size, u8** data) {
	*data = NULL;
	if (!path || expected_size == 0 || expected_size > maximum_size ||
	    expected_size > SIZE_MAX) return false;
	FILE* file = fopen(path, "rb");
	u8* result = malloc((size_t)expected_size);
	bool success = file && result &&
	               fread(result, 1, (size_t)expected_size, file) ==
	                   (size_t)expected_size &&
	               fgetc(file) == EOF && !ferror(file);
	if (file && fclose(file) != 0) success = false;
	if (!success) {
		free(result);
		return false;
	}
	*data = result;
	return true;
}



static char* archive_provider_recipe_next_line(char** cursor) {
	if (!cursor || !*cursor || !**cursor) return NULL;
	char* line = *cursor;
	char* newline = strchr(line, '\n');
	if (!newline) {
		*cursor = line + strlen(line);
		return line;
	}
	*newline = '\0';
	*cursor = newline + 1;
	return line;
}

static const char* archive_provider_recipe_value(
		char* line, const char* prefix) {
	size_t length = strlen(prefix);
	return line && strncmp(line, prefix, length) == 0 ?
	       line + length : NULL;
}

static bool archive_provider_regenerate_aggregate_shell(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		const u8* retained_recipe, size_t retained_recipe_size,
		char* error, size_t error_capacity) {
	char* text = malloc(retained_recipe_size + 1);
	if (!text) return false;
	memcpy(text, retained_recipe, retained_recipe_size);
	text[retained_recipe_size] = '\0';
	char* cursor = text;
	char* line = archive_provider_recipe_next_line(&cursor);
	bool success = line &&
		strcmp(line, ARCHIVE_PART10_RECIPE_MAGIC) == 0;
	const char* bundle_sha256 = NULL;
	const char* study_uid = NULL;
	const char* series_uid = NULL;
	u32 instance_count = 0;
	if (success) {
		bundle_sha256 = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor), "bundle-sha256\t");
		study_uid = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor),
			"logical-slide-study-uid\t");
		series_uid = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor),
			"logical-slide-series-uid\t");
		const char* count_text = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor), "instance-count\t");
		char* count_end = NULL;
		unsigned long parsed_count = count_text ?
			strtoul(count_text, &count_end, 10) : 0;
		success = bundle_sha256 && study_uid && series_uid && count_text &&
		          count_end && !*count_end && parsed_count > 0 &&
		          parsed_count <= ARCHIVE_SHELL_MAX_INSTANCES;
		instance_count = success ? (u32)parsed_count : 0;
	}
	archive_shell_part10_recipe_input_t* recipe_instances =
		success ? calloc(instance_count, sizeof(*recipe_instances)) : NULL;
	dicom_series_t* dicoms =
		success ? calloc(instance_count, sizeof(*dicoms)) : NULL;
	archive_shell_build_part10_input_t* shell_instances =
		success ? calloc(instance_count, sizeof(*shell_instances)) : NULL;
	char (*source_sha256)[65] =
		success ? calloc(instance_count, sizeof(*source_sha256)) : NULL;
	char (*sop_instance_uid)[65] =
		success ? calloc(instance_count, sizeof(*sop_instance_uid)) : NULL;
	success = success && recipe_instances && dicoms && shell_instances &&
	          source_sha256 && sop_instance_uid;
	bool record_found = false;
	for (u32 i = 0; i < instance_count && success; ++i) {
		line = archive_provider_recipe_next_line(&cursor);
		unsigned int ordinal = 0;
		unsigned long long byte_size = 0;
		char extra = '\0';
		int fields = line ? sscanf(
			line, "instance\t%u\t%64[0-9.]\t%64[0-9a-f]\t%llu%c",
			&ordinal, sop_instance_uid[i], source_sha256[i],
			&byte_size, &extra) : 0;
		if (fields != 4 || ordinal != i || !byte_size) {
			success = false;
			break;
		}
		archive_database_object_t source_object = {0};
		char source_path[ARCHIVE_PATH_CAPACITY];
		char actual_sha256[65];
		u64 actual_size = 0;
		success =
		          archive_database_find_object(
			          repository, source_sha256[i],
			          &source_object, error,
			          error_capacity) == ARCHIVE_DATABASE_OK &&
		          source_object.byte_size == (u64)byte_size &&
		          strcmp(source_object.media_type,
		                 "application/dicom") == 0 &&
		          ((source_object.external_path[0] &&
		            snprintf(source_path, sizeof(source_path), "%s",
		                     source_object.external_path) > 0 &&
		            strlen(source_object.external_path) <
			            sizeof(source_path)) ||
		           (source_object.storage_key[0] &&
		            archive_repository_make_path(
			            repository, source_object.storage_key,
			            source_path, sizeof(source_path)))) &&
		          archive_sha256_file(
			          source_path, actual_sha256, &actual_size) &&
		          actual_size == (u64)byte_size &&
		          strcmp(actual_sha256, source_sha256[i]) == 0 &&
		          dicom_open_single_instance_from_path(
			          dicoms + i, source_path) &&
		          arrlen(dicoms[i].instances) == 1 &&
		          strcmp(dicoms[i].instances->sop_instance_uid.value,
			                 sop_instance_uid[i]) == 0;
		if (!success) break;
		recipe_instances[i] = (archive_shell_part10_recipe_input_t){
			.bundle_sha256 = bundle_sha256,
			.source_sha256 = source_sha256[i],
			.source_byte_size = (u64)byte_size,
			.sop_instance_uid = sop_instance_uid[i],
		};
		shell_instances[i] = (archive_shell_build_part10_input_t){
			.instance = dicoms[i].instances,
			.source_sha256 = source_sha256[i],
			.source_byte_size = (u64)byte_size,
		};
		if (strcmp(record->sop_instance_uid, sop_instance_uid[i]) == 0) {
			record_found = record->shell_instance_index == i &&
			               strcmp(record->object_sha256,
			                      source_sha256[i]) == 0 &&
			               record->object_byte_size == (u64)byte_size;
		}
	}
	archive_shell_part10_aggregate_recipe_input_t recipe_input = {
		.bundle_sha256 = bundle_sha256,
		.study_instance_uid = study_uid,
		.series_instance_uid = series_uid,
		.instances = recipe_instances,
		.instance_count = instance_count,
	};
	char* expected_recipe = NULL;
	size_t expected_recipe_size = 0;
	char expected_fingerprint[65];
	if (success) {
		success = record_found &&
		          strcmp(record->bundle_sha256, bundle_sha256) == 0 &&
		          strcmp(record->study_instance_uid, study_uid) == 0 &&
		          strcmp(record->series_instance_uid, series_uid) == 0 &&
		          archive_shell_build_part10_aggregate_recipe(
			          &recipe_input, &expected_recipe, &expected_recipe_size,
			          expected_fingerprint) &&
		          expected_recipe_size == retained_recipe_size &&
		          memcmp(expected_recipe, retained_recipe,
		                 retained_recipe_size) == 0 &&
		          strcmp(expected_fingerprint,
		                 record->representation_fingerprint) == 0;
	}
	archive_shell_buffer_t regenerated = {0};
	if (success) {
		archive_shell_build_part10_aggregate_input_t shell_input = {
			.instances = shell_instances,
			.instance_count = instance_count,
			.representation_fingerprint = expected_fingerprint,
		};
		success = archive_shell_build_part10_aggregate(
			&shell_input, &regenerated, error, error_capacity);
	}
	char regenerated_sha256[65];
	if (success) {
		success = regenerated.size == record->shell_byte_size &&
		          archive_sha256_bytes(
			          regenerated.data, regenerated.size,
			          regenerated_sha256) &&
		          strcmp(regenerated_sha256, record->shell_sha256) == 0 &&
		          archive_object_repair_shell_bytes(
			          repository, regenerated.data, regenerated.size,
			          record->shell_sha256, record->shell_key,
			          error, error_capacity) == ARCHIVE_OBJECT_OK;
	}
	if (!success && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not regenerate aggregate virtual DICOM shell '%s'",
			record->shell_key);
	archive_shell_buffer_destroy(&regenerated);
	free(expected_recipe);
	if (dicoms) {
		for (u32 i = 0; i < instance_count; ++i)
			dicom_destroy(dicoms + i);
	}
	free(sop_instance_uid);
	free(source_sha256);
	free(shell_instances);
	free(dicoms);
	free(recipe_instances);
	free(text);
	return success;
}

static bool archive_provider_load_published_shell(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		u8** shell_data, archive_shell_view_t* shell,
		archive_shell_instance_t* instance, char* error, size_t error_capacity) {
	char shell_path[ARCHIVE_PATH_CAPACITY];
	u8* data = NULL;
	if (!archive_repository_make_path(repository, record->shell_key,
	                                  shell_path, sizeof(shell_path)) ||
	    !archive_provider_read_exact_file(shell_path, record->shell_byte_size,
	                                      SIZE_MAX, &data)) {
		archive_provider_set_error(error, error_capacity,
		                           "could not read virtual DICOM shell '%s'",
		                           record->shell_key);
		return false;
	}
	char shell_sha256[65];
	bool success =
		archive_sha256_bytes(data, (size_t)record->shell_byte_size, shell_sha256) &&
		strcmp(shell_sha256, record->shell_sha256) == 0 &&
		archive_shell_open(shell, data, (size_t)record->shell_byte_size,
		                   error, error_capacity) &&
		archive_shell_get_instance(shell, record->shell_instance_index, instance) &&
		archive_provider_record_matches_shell(record, shell, instance,
		                                     error, error_capacity);
	if (!success) {
		free(data);
		if (!error || !error[0])
			archive_provider_set_error(error, error_capacity,
			                           "virtual DICOM shell '%s' failed validation",
			                           record->shell_key);
		return false;
	}
	*shell_data = data;
	return true;
}

static bool archive_provider_regenerate_shell(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	char source_path[ARCHIVE_PATH_CAPACITY];
	char recipe_path[ARCHIVE_PATH_CAPACITY];
	u8* retained_recipe = NULL;
	size_t retained_recipe_size = 0;
	char expected_recipe[ARCHIVE_SHELL_RECIPE_CAPACITY];
	size_t expected_recipe_size = 0;
	char expected_fingerprint[65];
	char retained_recipe_sha256[65];
	archive_shell_buffer_t regenerated = {0};
	dicom_series_t dicom = {0};
	bool success = archive_provider_source_path(
		repository, record, source_path);
	if (success && record->recipe_key[0]) {
		success =
			archive_repository_make_path(repository, record->recipe_key,
			                             recipe_path, sizeof(recipe_path)) &&
			archive_provider_read_exact_file(recipe_path, record->recipe_byte_size,
			                                 SIZE_MAX,
			                                 &retained_recipe);
		retained_recipe_size = success ? (size_t)record->recipe_byte_size : 0;
	} else if (success) {
		success = archive_object_read_inline_bytes(
			repository, record->recipe_sha256, record->recipe_byte_size,
			ARCHIVE_SHELL_RECIPE_MEDIA_TYPE, SIZE_MAX,
			&retained_recipe, &retained_recipe_size,
			error, error_capacity) == ARCHIVE_OBJECT_OK;
	}
	success = success &&
		retained_recipe_size == record->recipe_byte_size &&
		archive_sha256_bytes(retained_recipe, (size_t)record->recipe_byte_size,
		                     retained_recipe_sha256) &&
		strcmp(retained_recipe_sha256, record->recipe_sha256) == 0;
	if (success && retained_recipe_size >=
	    strlen(ARCHIVE_PART10_RECIPE_MAGIC "\n") &&
	    memcmp(retained_recipe, ARCHIVE_PART10_RECIPE_MAGIC "\n",
	           strlen(ARCHIVE_PART10_RECIPE_MAGIC "\n")) == 0) {
		success = archive_provider_regenerate_aggregate_shell(
			repository, record, retained_recipe, retained_recipe_size,
			error, error_capacity);
		dicom_destroy(&dicom);
		free(retained_recipe);
		return success;
	}
	if (success &&
	    (retained_recipe_size <
		     strlen(ARCHIVE_PART10_RECIPE_MAGIC "-single\n") ||
	     memcmp(retained_recipe,
	            ARCHIVE_PART10_RECIPE_MAGIC "-single\n",
	            strlen(ARCHIVE_PART10_RECIPE_MAGIC "-single\n")) != 0)) {
		archive_provider_set_error(
			error, error_capacity,
			"Part 10 shell recipe predates the current shell format; "
			"reingest the source to publish a current shell in a new repository");
		dicom_destroy(&dicom);
		free(retained_recipe);
		return false;
	}
	archive_shell_part10_recipe_input_t recipe_input = {
		.bundle_sha256 = record->bundle_sha256,
		.source_sha256 = record->object_sha256,
		.source_byte_size = record->object_byte_size,
		.sop_instance_uid = record->sop_instance_uid,
	};
	if (success)
		success = archive_shell_build_part10_recipe(
			          &recipe_input, expected_recipe, &expected_recipe_size,
			          expected_fingerprint) &&
		          expected_recipe_size == record->recipe_byte_size &&
		          memcmp(expected_recipe, retained_recipe, expected_recipe_size) == 0 &&
		          strcmp(expected_fingerprint,
		                 record->representation_fingerprint) == 0;
	char source_sha256[65];
	u64 source_size = 0;
	if (success)
		success = archive_sha256_file(source_path, source_sha256, &source_size) &&
		          source_size == record->object_byte_size &&
		          strcmp(source_sha256, record->object_sha256) == 0;
	if (success)
		success = dicom_open_single_instance_from_path(&dicom, source_path) &&
		          arrlen(dicom.instances) == 1 &&
		          strcmp(dicom.instances->sop_instance_uid.value,
		                 record->sop_instance_uid) == 0;
	if (success) {
		archive_shell_build_part10_input_t shell_input = {
			.instance = dicom.instances,
			.source_sha256 = record->object_sha256,
			.source_byte_size = record->object_byte_size,
			.representation_fingerprint = record->representation_fingerprint,
		};
		success = archive_shell_build_part10(&shell_input, &regenerated,
		                                    error, error_capacity);
	}
	char regenerated_sha256[65];
	if (success)
		success = regenerated.size == record->shell_byte_size &&
		          archive_sha256_bytes(regenerated.data, regenerated.size,
		                               regenerated_sha256) &&
		          strcmp(regenerated_sha256, record->shell_sha256) == 0;
	if (success)
		success = archive_object_repair_shell_bytes(
			          repository, regenerated.data, regenerated.size,
			          record->shell_sha256, record->shell_key,
			          error, error_capacity) == ARCHIVE_OBJECT_OK;
	if (!success && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not regenerate virtual DICOM shell '%s' from its retained recipe",
			record->shell_key);
	archive_shell_buffer_destroy(&regenerated);
	dicom_destroy(&dicom);
	free(retained_recipe);
	return success;
}

static archive_provider_mrxs_sources_t*
archive_provider_open_mrxs_sources(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	u8* recipe = NULL;
	size_t recipe_size = 0;
	if (archive_object_read_inline_bytes(
			repository, record->recipe_sha256, record->recipe_byte_size,
			ARCHIVE_MRXS_RECIPE_MEDIA_TYPE, KILOBYTES(16),
			&recipe, &recipe_size, error, error_capacity) != ARCHIVE_OBJECT_OK)
		return NULL;
	char* text = malloc(recipe_size + 1);
	if (!text) {
		free(recipe);
		return NULL;
	}
	memcpy(text, recipe, recipe_size);
	text[recipe_size] = '\0';
	free(recipe);
	char descriptor_sha256[65] = {0};
	char fingerprint[65] = {0};
	char source_path[ARCHIVE_PATH_CAPACITY] = {0};
	unsigned long long descriptor_size = 0;
	char extra = '\0';
	unsigned recipe_version = 0, adapter_version = 0;
	int fields = sscanf(
		text,
		"slidescape-mrxs-native-recipe-v%u\n"
		"adapter\tslidescape-mrxs\t%u\n"
		"bundle-descriptor-sha256\t%64[0-9a-f]\n"
		"bundle-descriptor-size\t%llu\n"
		"representation-fingerprint\t%64[0-9a-f]\n"
		"source-path\t%1023[^\n]\n%c",
		&recipe_version, &adapter_version, descriptor_sha256, &descriptor_size, fingerprint,
		source_path, &extra);
	bool valid = fields == 6 && recipe_version == adapter_version &&
		(recipe_version == 5 || recipe_version == ARCHIVE_MRXS_ADAPTER_VERSION) &&
		descriptor_size == record->object_byte_size &&
		strcmp(descriptor_sha256, record->object_sha256) == 0 &&
		strcmp(fingerprint, record->representation_fingerprint) == 0;
	free(text);
	if (!valid) {
		if (!error || !error[0])
			archive_provider_set_error(
				error, error_capacity,
				"could not validate retained MRXS provider recipe");
		return NULL;
	}

	u8* descriptor = NULL;
	size_t descriptor_bytes = 0;
	if (archive_object_read_inline_bytes(
			repository, descriptor_sha256,
			(u64)descriptor_size,
			"application/vnd.slidescape.mrxs-bundle",
			ARCHIVE_PROVIDER_MRXS_DESCRIPTOR_MAX,
			&descriptor, &descriptor_bytes,
			error, error_capacity) != ARCHIVE_OBJECT_OK)
		return NULL;
	char* descriptor_text = malloc(descriptor_bytes + 1);
	if (!descriptor_text) {
		free(descriptor);
		return NULL;
	}
	memcpy(descriptor_text, descriptor, descriptor_bytes);
	descriptor_text[descriptor_bytes] = '\0';
	free(descriptor);
	const char* header = "slidescape-mrxs-bundle-v1\n";
	valid = descriptor_bytes >= strlen(header) &&
		memcmp(descriptor_text, header, strlen(header)) == 0;

	u32 dat_count = 0;
	char* cursor = valid ? descriptor_text + strlen(header) : NULL;
	while (cursor && *cursor) {
		char* newline = strchr(cursor, '\n');
		if (!newline) {
			valid = false;
			break;
		}
		*newline = '\0';
		unsigned object_index = 0;
		char logical_name[ARCHIVE_PATH_CAPACITY] = {0};
		char digest[65] = {0};
		unsigned long long byte_size = 0;
		char extra = '\0';
		int parsed = sscanf(
			cursor, "object\t%u\t%1023[^\t]\t%64[0-9a-f]\t%llu%c",
			&object_index, logical_name, digest, &byte_size, &extra);
		if (parsed != 4 || object_index < ARCHIVE_MRXS_MEMBER_FIRST_DATA) {
			if (parsed != 4) valid = false;
		} else if (object_index - ARCHIVE_MRXS_MEMBER_FIRST_DATA != dat_count) {
			valid = false;
		} else {
			++dat_count;
		}
		cursor = newline + 1;
	}
	if (!valid || !dat_count || dat_count > ARCHIVE_PROVIDER_MRXS_FILE_BUDGET) {
		free(descriptor_text);
		archive_provider_set_error(
			error, error_capacity,
			"retained MRXS bundle descriptor is invalid");
		return NULL;
	}

	archive_provider_mrxs_sources_t* sources =
		calloc(1, sizeof(*sources));
	if (sources) {
		sources->files = calloc(dat_count, sizeof(*sources->files));
		sources->sizes = calloc(dat_count, sizeof(*sources->sizes));
		sources->count = dat_count;
	}
	if (!sources || !sources->files || !sources->sizes) {
		archive_provider_mrxs_sources_destroy(sources);
		free(descriptor_text);
		return NULL;
	}
	sources->handles_reserved = archive_provider_reserve_mrxs_files(dat_count);
	if (!sources->handles_reserved) {
		archive_provider_mrxs_sources_destroy(sources);
		free(descriptor_text);
		archive_provider_set_error(error, error_capacity, "MRXS source file budget is busy");
		return NULL;
	}
	char directory[ARCHIVE_PATH_CAPACITY];
	snprintf(directory, sizeof(directory), "%s", source_path);
	char* extension = strrchr(directory, '.');
	valid = extension && strcasecmp(extension, ".mrxs") == 0;
	if (valid) *extension = '\0';

	/*
	 * Restore the line delimiters replaced during validation, then open only
	 * the data files needed for shell-native byte-range lookup. Slidedat.ini
	 * and Index.dat are intentionally not opened or parsed here.
	 */
	for (size_t i = strlen(header); i < descriptor_bytes; ++i)
		if (descriptor_text[i] == '\0')
			descriptor_text[i] = '\n';
	cursor = valid ? descriptor_text + strlen(header) : NULL;
	while (cursor && *cursor && valid) {
		char* newline = strchr(cursor, '\n');
		if (!newline) {
			valid = false;
			break;
		}
		*newline = '\0';
		unsigned object_index = 0;
		char logical_name[ARCHIVE_PATH_CAPACITY] = {0};
		char digest[65] = {0};
		unsigned long long byte_size = 0;
		int parsed = sscanf(
			cursor, "object\t%u\t%1023[^\t]\t%64[0-9a-f]\t%llu",
			&object_index, logical_name, digest, &byte_size);
		if (parsed != 4) {
			valid = false;
		} else if (object_index >= ARCHIVE_MRXS_MEMBER_FIRST_DATA) {
			const char* prefix = "companion/";
			const char* relative = logical_name + strlen(prefix);
			if (strncmp(logical_name, prefix, strlen(prefix)) != 0 ||
			    !relative[0] || strstr(relative, "..") ||
			    relative[0] == '/' || relative[0] == '\\') {
				valid = false;
			} else {
				char path[ARCHIVE_PATH_CAPACITY];
				struct stat info = {0};
				u32 file_index = object_index - ARCHIVE_MRXS_MEMBER_FIRST_DATA;
				if (snprintf(path, sizeof(path), "%s%s%s",
				             directory, PATH_SEP, relative) >=
					    (i32)sizeof(path) ||
				    platform_stat(path, &info) != 0 ||
				    !S_ISREG(info.st_mode) ||
				    (u64)info.st_size != (u64)byte_size) {
					valid = false;
				} else {
					sources->files[file_index] =
						open_file_handle_for_simultaneous_access(path);
					sources->sizes[file_index] = (u64)byte_size;
					valid =
						sources->files[file_index] != 0;
				}
			}
		}
		cursor = newline + 1;
	}
	free(descriptor_text);
	if (!valid) {
		archive_provider_mrxs_sources_destroy(sources);
		sources = NULL;
	}
	if (!sources && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not open retained MRXS payload sources");
	return sources;
}

static void archive_provider_shell_cache_release(
		archive_provider_shell_cache_entry_t* entry) {
	if (!entry) return;
	platform_mutex_lock(&archive_provider_cache_mutex);
	ASSERT(entry->occupied && entry->refcount > 0);
	--entry->refcount;
	entry->last_used = ++archive_provider_cache_clock;
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

static archive_provider_shell_cache_entry_t*
archive_provider_shared_shell_acquire_locked(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	bool is_part10 = record && strcmp(record->provider, "part10") == 0;
	bool is_mrxs = record && strcmp(record->provider, "mrxs-native") == 0;
	if (!repository || !record || (!is_part10 && !is_mrxs) ||
	    !record->shell_key[0] || !record->shell_sha256[0] ||
	    record->shell_byte_size == 0 ||
	    record->shell_byte_size > SIZE_MAX) return NULL;

	platform_mutex_lock(&archive_provider_cache_mutex);
	for (i32 i = 0; i < COUNT(archive_provider_shell_cache); ++i) {
		archive_provider_shell_cache_entry_t* entry =
			archive_provider_shell_cache + i;
		if (!entry->occupied ||
		    strcmp(entry->repository_root, repository->root) != 0 ||
		    strcmp(entry->shell_key, record->shell_key) != 0) continue;
		if (entry->size != record->shell_byte_size ||
		    strcmp(entry->shell_sha256, record->shell_sha256) != 0) {
			platform_mutex_unlock(&archive_provider_cache_mutex);
			archive_provider_set_error(
				error, error_capacity,
				"published records disagree about aggregate shell identity");
			return NULL;
		}
		++archive_provider_cache_stats.shell_hits;
		++entry->refcount;
		entry->last_used = ++archive_provider_cache_clock;
		platform_mutex_unlock(&archive_provider_cache_mutex);
		return entry;
	}
	++archive_provider_cache_stats.shell_misses;
	bool admitted = archive_provider_reserve_shell_bytes((size_t)record->shell_byte_size);
	platform_mutex_unlock(&archive_provider_cache_mutex);
	if (!admitted) {
		archive_provider_set_error(error, error_capacity, "aggregate shell byte budget is busy or exceeded");
		return NULL;
	}

	u8* shell_data = NULL;
	archive_shell_view_t shell = {0};
	archive_shell_instance_t instance = {0};
	bool success = archive_provider_load_published_shell(
		repository, record, &shell_data, &shell, &instance,
		error, error_capacity);
	if (!success && is_part10) {
		/*
		 * A single process-wide repair lock keeps regeneration rare and
		 * bounded. Recheck after acquiring it because another request may
		 * already have restored the same immutable shell.
		 */
		platform_mutex_lock(&archive_provider_regeneration_mutex);
		if (error && error_capacity) error[0] = '\0';
		success = archive_provider_load_published_shell(
			repository, record, &shell_data, &shell, &instance,
			error, error_capacity);
		if (!success) {
			if (error && error_capacity) error[0] = '\0';
			success = archive_provider_regenerate_shell(
				repository, record, error, error_capacity);
			if (success) {
				if (error && error_capacity) error[0] = '\0';
				success = archive_provider_load_published_shell(
					repository, record, &shell_data, &shell, &instance,
					error, error_capacity);
			}
		}
		platform_mutex_unlock(&archive_provider_regeneration_mutex);
	}
	archive_provider_mrxs_sources_t* mrxs_sources =
		success && is_mrxs ?
		archive_provider_open_mrxs_sources(
			repository, record, error, error_capacity) : NULL;
	if (success && is_mrxs) success = mrxs_sources != NULL;
	if (!success) {
		archive_provider_mrxs_sources_destroy(mrxs_sources);
		free(shell_data);
		archive_provider_release_load_bytes((size_t)record->shell_byte_size, 0);
		if (!error || !error[0])
			archive_provider_set_error(
				error, error_capacity,
				"could not load or validate aggregate virtual DICOM shell '%s'",
				record->shell_key);
		return NULL;
	}

	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_provider_shell_cache_entry_t* available = NULL;
	for (i32 i = 0; i < COUNT(archive_provider_shell_cache); ++i) {
		archive_provider_shell_cache_entry_t* entry =
			archive_provider_shell_cache + i;
		if (entry->occupied &&
		    strcmp(entry->repository_root, repository->root) == 0 &&
		    strcmp(entry->shell_key, record->shell_key) == 0) {
			bool matches =
				entry->size == record->shell_byte_size &&
				strcmp(entry->shell_sha256, record->shell_sha256) == 0;
			if (matches) {
				++entry->refcount;
				entry->last_used = ++archive_provider_cache_clock;
			}
			platform_mutex_unlock(&archive_provider_cache_mutex);
			archive_provider_mrxs_sources_destroy(
				mrxs_sources);
			free(shell_data);
			archive_provider_release_load_bytes((size_t)record->shell_byte_size, 0);
			if (!matches)
				archive_provider_set_error(
					error, error_capacity,
					"published records disagree about aggregate shell identity");
			return matches ? entry : NULL;
		}
		if (!entry->occupied ||
		    (entry->refcount == 0 &&
		     (!available || entry->last_used < available->last_used)))
			available = entry;
	}
	if (!available) {
		platform_mutex_unlock(&archive_provider_cache_mutex);
		archive_provider_mrxs_sources_destroy(mrxs_sources);
		free(shell_data);
		archive_provider_release_load_bytes((size_t)record->shell_byte_size, 0);
		archive_provider_set_error(
			error, error_capacity,
			"all aggregate virtual DICOM shell cache entries are busy");
		return NULL;
	}
	if (available->occupied)
		++archive_provider_cache_stats.shell_evictions;
	archive_provider_shell_cache_entry_destroy(available);
	available->occupied = true;
	available->refcount = 1;
	available->last_used = ++archive_provider_cache_clock;
	snprintf(available->repository_root, sizeof(available->repository_root),
	         "%s", repository->root);
	snprintf(available->shell_key, sizeof(available->shell_key),
	         "%s", record->shell_key);
	snprintf(available->shell_sha256, sizeof(available->shell_sha256),
	         "%s", record->shell_sha256);
	available->data = shell_data;
	available->size = (size_t)record->shell_byte_size;
	available->shell = shell;
	available->mrxs_sources = mrxs_sources;
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return available;
}

static archive_provider_shell_cache_entry_t* archive_provider_shared_shell_acquire(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	if (!record) return NULL;
	platform_mutex_t* mutex = archive_provider_shell_load_mutexes + archive_provider_load_stripe(record, false);
	platform_mutex_lock(mutex);
	archive_provider_shell_cache_entry_t* result = archive_provider_shared_shell_acquire_locked(
		repository, record, error, error_capacity);
	platform_mutex_unlock(mutex);
	return result;
}

static archive_provider_cache_entry_t* archive_provider_instance_cache_acquire_locked(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	bool is_part10 = record && strcmp(record->provider, "part10") == 0;
	bool is_mrxs = record && strcmp(record->provider, "mrxs-native") == 0;
	if (!repository || !record || (!is_part10 && !is_mrxs) ||
	    !record->shell_key[0] ||
	    (is_part10 &&
	     !record->object_key[0] &&
	     !record->external_path[0]) ||
	    record->shell_byte_size == 0 || record->shell_byte_size > SIZE_MAX) {
		archive_provider_set_error(error, error_capacity,
		                           "published instance has no supported shell provider");
		return NULL;
	}
	platform_mutex_lock(&archive_provider_cache_mutex);
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		archive_provider_cache_entry_t* entry = archive_provider_cache + i;
		if (entry->occupied &&
		    strcmp(entry->repository_root, repository->root) == 0 &&
		    strcmp(entry->shell_key, record->shell_key) == 0 &&
		    entry->shell_instance_index == record->shell_instance_index) {
			++archive_provider_cache_stats.hits;
			++entry->refcount;
			entry->last_used = ++archive_provider_cache_clock;
			platform_mutex_unlock(&archive_provider_cache_mutex);
			return entry;
		}
	}
	++archive_provider_cache_stats.misses;
	platform_mutex_unlock(&archive_provider_cache_mutex);

	char source_path[ARCHIVE_PATH_CAPACITY];
	file_handle_t source_file = 0;
	archive_shell_payload_range_t* payload_ranges = NULL;
	size_t payload_bytes = 0;
	archive_shell_instance_t instance = {0};
	archive_provider_shell_cache_entry_t* shared_shell =
		archive_provider_shared_shell_acquire(
			repository, record, error, error_capacity);
	bool success = shared_shell &&
		archive_shell_get_instance(
			&shared_shell->shell, record->shell_instance_index, &instance) &&
		archive_provider_record_matches_shell(
			record, &shared_shell->shell, &instance,
			error, error_capacity) &&
		(!is_part10 ||
		 archive_provider_source_path(
			 repository, record, source_path));
	if (success && is_part10 &&
	    record->external_path[0]) {
		struct stat info = {0};
		success = platform_stat(source_path, &info) == 0 &&
		          S_ISREG(info.st_mode) &&
		          (u64)info.st_size == record->object_byte_size;
		if (!success)
			archive_provider_set_error(
				error, error_capacity,
				"externally referenced Part 10 source size changed "
				"or is unavailable");
	}
	if (success && is_part10) {
		source_file = open_file_handle_for_simultaneous_access(source_path);
		success = source_file != 0;
	}
	/* Only delta offsets need expansion for constant-time frame access. */
	if (success && is_part10 &&
	    instance.payload_map_kind == ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS) {
		u64 required = (u64)instance.frame_count * sizeof(*payload_ranges);
		if (required <= SIZE_MAX && archive_provider_reserve_payload_bytes((size_t)required)) {
			payload_bytes = (size_t)required;
			payload_ranges = malloc(payload_bytes);
		} else {
			archive_provider_set_error(error, error_capacity, "frame directory byte budget is busy or exceeded");
		}
		success = payload_ranges &&
			archive_shell_build_payload_ranges(
				&shared_shell->shell, &instance,
				payload_ranges, instance.frame_count);
	}
	if (!success) {
		file_handle_close(source_file);
		free(payload_ranges);
		archive_provider_release_load_bytes(0, payload_bytes);
		archive_provider_shell_cache_release(shared_shell);
		if (!error || !error[0])
			archive_provider_set_error(error, error_capacity,
			                           "could not load or validate virtual DICOM shell '%s'",
			                           record->shell_key);
		return NULL;
	}

	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_provider_cache_entry_t* available = NULL;
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		archive_provider_cache_entry_t* entry = archive_provider_cache + i;
		if (entry->occupied &&
		    strcmp(entry->repository_root, repository->root) == 0 &&
		    strcmp(entry->shell_key, record->shell_key) == 0 &&
		    entry->shell_instance_index == record->shell_instance_index) {
			++entry->refcount;
			entry->last_used = ++archive_provider_cache_clock;
			platform_mutex_unlock(&archive_provider_cache_mutex);
			file_handle_close(source_file);
			free(payload_ranges);
			archive_provider_release_load_bytes(0, payload_bytes);
			archive_provider_shell_cache_release(shared_shell);
			return entry;
		}
		if (!entry->occupied ||
		    (entry->refcount == 0 && (!available || entry->last_used < available->last_used)))
			available = entry;
	}
	if (!available) {
		platform_mutex_unlock(&archive_provider_cache_mutex);
		file_handle_close(source_file);
		free(payload_ranges);
		archive_provider_release_load_bytes(0, payload_bytes);
		archive_provider_shell_cache_release(shared_shell);
		archive_provider_set_error(error, error_capacity,
		                           "all virtual DICOM shell cache entries are busy");
		return NULL;
	}
	if (available->occupied) ++archive_provider_cache_stats.evictions;
	archive_provider_cache_entry_destroy(available);
	available->occupied = true;
	available->refcount = 1;
	available->last_used = ++archive_provider_cache_clock;
	snprintf(available->repository_root, sizeof(available->repository_root), "%s",
	         repository->root);
	snprintf(available->shell_key, sizeof(available->shell_key), "%s", record->shell_key);
	available->shell_instance_index = record->shell_instance_index;
	available->shared_shell = shared_shell;
	available->instance = instance;
	available->source_file = source_file;
	available->payload_ranges = payload_ranges;
	available->payload_bytes = payload_bytes;
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return available;
}

static archive_provider_cache_entry_t* archive_provider_instance_cache_acquire(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	if (!record) return NULL;
	platform_mutex_t* mutex = archive_provider_instance_load_mutexes + archive_provider_load_stripe(record, true);
	platform_mutex_lock(mutex);
	archive_provider_cache_entry_t* result = archive_provider_instance_cache_acquire_locked(
		repository, record, error, error_capacity);
	platform_mutex_unlock(mutex);
	return result;
}

static void archive_provider_cache_release(archive_provider_cache_entry_t* entry) {
	if (!entry) return;
	platform_mutex_lock(&archive_provider_cache_mutex);
	ASSERT(entry->occupied && entry->refcount > 0);
	--entry->refcount;
	entry->last_used = ++archive_provider_cache_clock;
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

bool archive_instance_provider_get_metadata(const archive_repository_t* repository,
		const archive_instance_record_t* record, char** json, size_t* json_size,
		char* error, size_t error_capacity) {
	if (!json || !json_size) return false;
	*json = NULL;
	*json_size = 0;
	archive_provider_cache_entry_t* entry =
		archive_provider_instance_cache_acquire(
			repository, record, error, error_capacity);
	if (!entry) return false;
	u64 capacity = 0;
	bool success = archive_metadata_get_capacity(
		&entry->shared_shell->shell, &entry->instance, &capacity);
	char* result = success ? malloc((size_t)capacity) : NULL;
	success = result && archive_metadata_write_instance(
		&entry->shared_shell->shell, &entry->instance, result,
		(size_t)capacity, json_size, true);
	if (success) {
		*json = result;
	} else {
		free(result);
		archive_provider_set_error(error, error_capacity,
		                           "could not serialize instance metadata within the size limit");
	}
	archive_provider_cache_release(entry);
	return success;
}

bool archive_instance_provider_get_series_metadata(
		const archive_repository_t* repository,
		const archive_instance_record_t* records, i32 record_count,
		char** json, size_t* json_size, char* error, size_t error_capacity) {
	if (!json || !json_size) return false;
	*json = NULL;
	*json_size = 0;
	if (!repository || !records || record_count <= 0) {
		archive_provider_set_error(error, error_capacity,
		                           "published series has no instances");
		return false;
	}
	archive_provider_cache_entry_t* entry =
		archive_provider_instance_cache_acquire(
			repository, records, error, error_capacity);
	if (!entry) return false;

	const archive_shell_view_t* shell = &entry->shared_shell->shell;
	bool success = shell->instance_count == (u32)record_count;
	u64 result_capacity = 4;
	for (i32 i = 0; i < record_count && success; ++i) {
		const archive_instance_record_t* record = records + i;
		archive_shell_instance_t instance = {0};
		success =
			record->shell_instance_index == (u32)i &&
			strcmp(record->provider, records->provider) == 0 &&
			strcmp(record->study_instance_uid,
			       records->study_instance_uid) == 0 &&
			strcmp(record->series_instance_uid,
			       records->series_instance_uid) == 0 &&
			strcmp(record->shell_key, records->shell_key) == 0 &&
			strcmp(record->shell_sha256, records->shell_sha256) == 0 &&
			record->shell_byte_size == records->shell_byte_size &&
			strcmp(record->bundle_sha256, records->bundle_sha256) == 0 &&
			strcmp(record->representation_fingerprint,
			       records->representation_fingerprint) == 0 &&
			strcmp(record->recipe_key, records->recipe_key) == 0 &&
			strcmp(record->recipe_sha256, records->recipe_sha256) == 0 &&
			record->recipe_byte_size == records->recipe_byte_size &&
			archive_shell_get_instance(
				shell, (u32)i, &instance) &&
			archive_provider_record_matches_shell(
				record, shell, &instance,
				error, error_capacity);
		u64 instance_capacity = 0;
		success = success && archive_metadata_get_capacity(
			shell, &instance, &instance_capacity);
		if (!success ||
		    result_capacity > ARCHIVE_MAX_METADATA_SIZE -
		                      instance_capacity) {
			success = false;
			break;
		}
		result_capacity += instance_capacity;
	}
	if (!success) {
		if (!error || !error[0])
			archive_provider_set_error(
				error, error_capacity,
				"published series does not match one complete aggregate shell");
		archive_provider_cache_release(entry);
		return false;
	}

	char* result = malloc((size_t)result_capacity);
	success = result != NULL;
	size_t used = 0;
	if (success) result[used++] = '[';
	for (i32 i = 0; i < record_count && success; ++i) {
		archive_shell_instance_t instance = {0};
		size_t object_size = 0;
		if (i) result[used++] = ',';
		success =
			archive_shell_get_instance(shell, (u32)i, &instance) &&
			archive_metadata_write_instance(
				shell, &instance, result + used,
				(size_t)result_capacity - used, &object_size, false);
		if (success) used += object_size;
	}
	if (success) {
		result[used++] = ']';
		result[used++] = '\n';
		result[used] = '\0';
		*json = result;
		*json_size = used;
	} else if (!error || !error[0]) {
		archive_provider_set_error(error, error_capacity,
		                           "could not serialize series metadata");
	}
	if (!success) free(result);
	archive_provider_cache_release(entry);
	return success;
}


i32 archive_instance_provider_get_frame_representations(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		archive_frame_representation_t* representations, i32 capacity,
		char* error, size_t error_capacity) {
	if (!repository || !record || !representations || capacity <= 0 ||
	    (strcmp(record->provider, "part10") != 0 &&
	     strcmp(record->provider, "mrxs-native") != 0)) return 0;
	if (strcmp(record->provider, "mrxs-native") == 0) {
		archive_provider_cache_entry_t* entry =
			archive_provider_instance_cache_acquire(
				repository, record, error, error_capacity);
		if (!entry) return 0;
		bool valid =
			entry->instance.provider_kind ==
				ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE &&
			(entry->instance.provider_parameter_version == ARCHIVE_MRXS_ADAPTER_VERSION ||
			 entry->instance.provider_parameter_version == 5) &&
			entry->instance.payload_map_kind ==
				ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA;
		archive_provider_cache_release(entry);
		if (!valid) return 0;
		archive_frame_representation_t generated = {0};
		snprintf(generated.media_type, sizeof(generated.media_type),
		         "image/jpeg");
		snprintf(generated.transfer_syntax_uid,
		         sizeof(generated.transfer_syntax_uid),
		         "1.2.840.10008.1.2.4.50");
		generated.source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
		generated.preference = 200;
		representations[0] = generated;
		return 1;
	}
	const char* media_type =
		archive_frame_media_type_for_transfer_syntax(record->transfer_syntax_uid);
	if (!media_type) return 0;
	archive_frame_representation_t representation = {0};
	snprintf(representation.media_type, sizeof(representation.media_type), "%s", media_type);
	snprintf(representation.transfer_syntax_uid,
	         sizeof(representation.transfer_syntax_uid), "%s",
	         record->transfer_syntax_uid);
	representation.source = ARCHIVE_FRAME_REPRESENTATION_STORED;
	representation.preference = 100;
	representations[0] = representation;
	i32 count = 1;

	archive_provider_cache_entry_t* entry =
		archive_provider_instance_cache_acquire(
			repository, record, error, error_capacity);
	if (!entry) return 0;
	const archive_shell_instance_t* instance = &entry->instance;
	bool can_generate_rgb8 =
		instance->payload_is_encapsulated &&
		instance->bits_allocated == 8 && instance->bits_stored == 8 &&
		instance->high_bit == 7 && instance->pixel_representation == 0 &&
		instance->samples_per_pixel == 3 &&
		strcmp(instance->photometric_interpretation, "RGB") == 0;
	if (can_generate_rgb8 && count < capacity) {
		archive_frame_representation_t generated = {0};
		snprintf(generated.media_type, sizeof(generated.media_type),
		         "application/octet-stream");
		snprintf(generated.transfer_syntax_uid,
		         sizeof(generated.transfer_syntax_uid), "1.2.840.10008.1.2.1");
		generated.source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
		generated.preference = 200;
		representations[count++] = generated;
	}
	archive_provider_cache_release(entry);
	return count;
}

static bool archive_provider_read_shell_frame(
		const archive_provider_cache_entry_t* entry, u32 frame_index,
		u8** data, size_t* size) {
	if (!entry || !data || !size) return false;
	*data = NULL;
	*size = 0;
	if (frame_index >= entry->instance.frame_count)
		return false;
	archive_shell_payload_range_t range = {0};
	if (entry->payload_ranges) {
		range = entry->payload_ranges[frame_index];
	} else if (!archive_shell_get_payload_range(
			&entry->shared_shell->shell, &entry->instance, frame_index, &range)) {
		return false;
	}
	if (
	    range.source_index != entry->instance.source_index ||
	    range.size > ARCHIVE_PROVIDER_MAX_FRAME_SIZE) return false;
	u8* encoded = malloc(range.size);
	if (!encoded) return false;
	if (file_handle_read_at_offset(encoded, entry->source_file, range.offset, range.size) !=
	    range.size) {
		free(encoded);
		return false;
	}
	size_t encoded_size = range.size;
	if (entry->instance.payload_is_encapsulated) {
		i64 payload_size =
			dicom_defragment_encapsulated_pixel_data_frame(encoded, (i64)encoded_size);
		if (payload_size <= 0 || payload_size > ARCHIVE_PROVIDER_MAX_FRAME_SIZE) {
			free(encoded);
			return false;
		}
		encoded_size = (size_t)payload_size;
	}
	*data = encoded;
	*size = encoded_size;
	return true;
}

static bool archive_provider_read_mrxs_frame(
		const archive_provider_cache_entry_t* entry,
		u32 frame_index, u8** data, size_t* size,
		char* error, size_t error_capacity) {
	if (!entry || !entry->shared_shell ||
	    !entry->shared_shell->mrxs_sources ||
	    !data || !size)
		return false;
	archive_shell_native_payload_t payload = {0};
	archive_provider_mrxs_sources_t* sources =
		entry->shared_shell->mrxs_sources;
	if (!archive_shell_get_native_payload(
			&entry->shared_shell->shell, &entry->instance,
			frame_index, &payload) ||
	    payload.file_index >= sources->count ||
	    payload.size > ARCHIVE_PROVIDER_MAX_FRAME_SIZE ||
	    payload.offset > sources->sizes[payload.file_index] ||
	    payload.size >
		    sources->sizes[payload.file_index] - payload.offset) {
		archive_provider_set_error(
			error, error_capacity,
			"MRXS shell payload range is invalid");
		return false;
	}
	u8* jpeg = malloc(payload.size);
	if (!jpeg) return false;
	size_t read = file_handle_read_at_offset(
		jpeg, sources->files[payload.file_index],
		payload.offset, payload.size);
	if (read != payload.size || payload.size < 4 ||
	    jpeg[0] != 0xff || jpeg[1] != 0xd8) {
		free(jpeg);
		archive_provider_set_error(
			error, error_capacity,
			"MRXS shell payload is not a standalone JPEG frame");
		return false;
	}
	*data = jpeg;
	*size = payload.size;
	return true;
}

bool archive_instance_provider_get_frame(const archive_repository_t* repository,
		const archive_instance_record_t* record, u32 one_based_frame,
		const archive_frame_representation_t* representation,
		archive_instance_frame_t* frame, char* error, size_t error_capacity) {
	if (!frame) return false;
	memset(frame, 0, sizeof(*frame));
	if (!record || !representation || one_based_frame == 0) {
		archive_provider_set_error(error, error_capacity,
		                           "requested frame representation is unavailable");
		return false;
	}
	archive_provider_cache_entry_t* entry =
		archive_provider_instance_cache_acquire(
			repository, record, error, error_capacity);
	if (!entry) return false;
	const archive_shell_instance_t* instance = &entry->instance;
	if (entry->shared_shell->mrxs_sources &&
	    instance->provider_kind == ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE &&
	    representation->source == ARCHIVE_FRAME_REPRESENTATION_GENERATED &&
	    strcmp(representation->media_type, "image/jpeg") == 0 &&
	    strcmp(representation->transfer_syntax_uid,
	           "1.2.840.10008.1.2.4.50") == 0 &&
	    one_based_frame <= instance->frame_count &&
	    archive_provider_read_mrxs_frame(
		    entry, one_based_frame - 1,
		    &frame->data, &frame->size, error, error_capacity)) {
		snprintf(frame->media_type, sizeof(frame->media_type),
		         "image/jpeg");
		snprintf(frame->transfer_syntax_uid,
		         sizeof(frame->transfer_syntax_uid),
		         "1.2.840.10008.1.2.4.50");
		archive_provider_cache_release(entry);
		return true;
	}
	const char* media_type =
		archive_frame_media_type_for_transfer_syntax(instance->transfer_syntax_uid);
	bool stored = representation->source == ARCHIVE_FRAME_REPRESENTATION_STORED &&
	              strcmp(representation->transfer_syntax_uid,
	                     instance->transfer_syntax_uid) == 0 &&
	              media_type && strcmp(media_type, representation->media_type) == 0;
	if (stored && archive_provider_read_shell_frame(
			entry, one_based_frame - 1, &frame->data, &frame->size)) {
		snprintf(frame->media_type, sizeof(frame->media_type), "%s", media_type);
		snprintf(frame->transfer_syntax_uid, sizeof(frame->transfer_syntax_uid), "%s",
		         instance->transfer_syntax_uid);
		archive_provider_cache_release(entry);
		return true;
	}

	bool generated_rgb8 =
		representation->source == ARCHIVE_FRAME_REPRESENTATION_GENERATED &&
		strcmp(representation->media_type, "application/octet-stream") == 0 &&
		strcmp(representation->transfer_syntax_uid, "1.2.840.10008.1.2.1") == 0 &&
		media_type != NULL &&
		instance->payload_is_encapsulated &&
		instance->bits_allocated == 8 && instance->bits_stored == 8 &&
		instance->high_bit == 7 && instance->pixel_representation == 0 &&
		instance->samples_per_pixel == 3 &&
		strcmp(instance->photometric_interpretation, "RGB") == 0;
	u8* encoded = NULL;
	size_t encoded_size = 0;
	if (generated_rgb8 &&
	    archive_provider_read_shell_frame(entry, one_based_frame - 1,
	                                      &encoded, &encoded_size)) {
		i32 decoded_width = 0;
		i32 decoded_height = 0;
		u8* bgra = NULL;
		if (strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0 &&
		    encoded_size <= UINT32_MAX) {
			i32 channels = 0;
			bgra = jpeg_decode_image_bounded(encoded, (u32)encoded_size,
				(i32)instance->columns, (i32)instance->rows, ARCHIVE_PROVIDER_MAX_FRAME_SIZE,
				&decoded_width, &decoded_height, &channels);
			if (bgra && channels != 4) {
				free(bgra);
				bgra = NULL;
			}
		} else if (strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.90") == 0 ||
		           strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.91") == 0) {
			bgra = jpeg2000_decode_image_to_bgra_bounded(encoded, encoded_size,
				(i32)instance->columns, (i32)instance->rows, ARCHIVE_PROVIDER_MAX_FRAME_SIZE,
				&decoded_width, &decoded_height);
		}
		free(encoded);
		encoded = NULL;
		size_t pixel_count = (size_t)instance->rows * instance->columns;
		if (bgra && decoded_width == instance->columns &&
		    decoded_height == instance->rows &&
		    pixel_count <= ARCHIVE_PROVIDER_MAX_FRAME_SIZE / 3) {
			frame->size = pixel_count * 3;
			frame->data = malloc(frame->size);
			if (frame->data) {
				for (size_t i = 0; i < pixel_count; ++i) {
					frame->data[i * 3 + 0] = bgra[i * 4 + 2];
					frame->data[i * 3 + 1] = bgra[i * 4 + 1];
					frame->data[i * 3 + 2] = bgra[i * 4 + 0];
				}
				snprintf(frame->media_type, sizeof(frame->media_type),
				         "application/octet-stream");
				snprintf(frame->transfer_syntax_uid,
				         sizeof(frame->transfer_syntax_uid),
				         "1.2.840.10008.1.2.1");
			}
		}
		free(bgra);
		if (frame->data) {
			archive_provider_cache_release(entry);
			return true;
		}
	}
	if (encoded) free(encoded);
	if (!frame->data) {
		archive_provider_set_error(error, error_capacity,
		                           "frame is unavailable in the selected representation");
		archive_provider_cache_release(entry);
		return false;
	}
	archive_provider_cache_release(entry);
	return false;
}

void archive_instance_frame_destroy(archive_instance_frame_t* frame) {
	if (!frame) return;
	free(frame->data);
	memset(frame, 0, sizeof(*frame));
}
