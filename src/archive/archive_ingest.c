/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_ingest.h"
#include "archive_zip.h"
#include "archive_dicom_zip.h"

#include "archive_database.h"
#include "archive_representation.h"
#include "archive_mrxs_adapter.h"
#include "archive_object.h"
#include "archive_sha256.h"
#include "archive_shell.h"
#include "archive_shell_recipe.h"
#include "dicom.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef struct archive_ingest_paths_t {
	char** values;
	i32 count;
	i32 capacity;
} archive_ingest_paths_t;

typedef struct archive_ingest_member_t {
	archive_database_instance_t database;
	archive_object_t object;
} archive_ingest_member_t;

static void archive_ingest_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_ingest_object_path(
		const archive_repository_t* repository,
		const archive_object_t* object, char* path,
		size_t path_capacity) {
	if (!repository || !object || !path || !path_capacity)
		return false;
	if (object->external_path[0]) {
		i32 length = snprintf(
			path, path_capacity, "%s",
			object->external_path);
		return length > 0 && (size_t)length < path_capacity;
	}
	return object->storage_key[0] &&
		archive_repository_make_path(
			repository, object->storage_key,
			path, path_capacity);
}

static bool archive_ingest_has_dcm_extension(const char* name) {
	size_t length = strlen(name);
	if (length < 4) return false;
	const char* extension = name + length - 4;
	return extension[0] == '.' &&
	       (extension[1] == 'd' || extension[1] == 'D') &&
	       (extension[2] == 'c' || extension[2] == 'C') &&
	       (extension[3] == 'm' || extension[3] == 'M');
}

static bool archive_ingest_has_mrxs_extension(const char* name) {
	size_t length = strlen(name);
	if (length < 5) return false;
	const char* extension = name + length - 5;
	return extension[0] == '.' &&
	       (extension[1] == 'm' || extension[1] == 'M') &&
	       (extension[2] == 'r' || extension[2] == 'R') &&
	       (extension[3] == 'x' || extension[3] == 'X') &&
	       (extension[4] == 's' || extension[4] == 'S');
}

static bool archive_ingest_append_path(archive_ingest_paths_t* paths, const char* path) {
	if (strlen(path) >= ARCHIVE_PATH_CAPACITY) return false;
	if (paths->count == paths->capacity) {
		i32 capacity = paths->capacity ? paths->capacity * 2 : 16;
		char** values = realloc(paths->values, (size_t)capacity * sizeof(*values));
		if (!values) return false;
		paths->values = values;
		paths->capacity = capacity;
	}
	size_t length = strlen(path) + 1;
	paths->values[paths->count] = malloc(length);
	if (!paths->values[paths->count]) return false;
	memcpy(paths->values[paths->count++], path, length);
	return true;
}

static void archive_ingest_destroy_paths(archive_ingest_paths_t* paths) {
	for (i32 i = 0; i < paths->count; ++i) free(paths->values[i]);
	free(paths->values);
	memset(paths, 0, sizeof(*paths));
}

static bool archive_ingest_join(char* dest, size_t capacity, const char* parent, const char* name) {
	i32 length = snprintf(dest, capacity, "%s/%s", parent, name);
	return length > 0 && (size_t)length < capacity;
}

#if defined(_WIN32)
static bool archive_ingest_discover_directory(const char* directory,
		archive_ingest_paths_t* paths,
		char* error, size_t error_capacity) {
	char pattern[ARCHIVE_PATH_CAPACITY];
	i32 pattern_length = 0;
	if ((pattern_length = snprintf(pattern, sizeof(pattern), "%s\\*", directory)) <= 0 ||
	    (size_t)pattern_length >= sizeof(pattern)) {
		archive_ingest_set_error(error, error_capacity, "source path is too long");
		return false;
	}
	WIN32_FIND_DATAA entry;
	HANDLE find = FindFirstFileA(pattern, &entry);
	if (find == INVALID_HANDLE_VALUE) {
		archive_ingest_set_error(error, error_capacity, "could not scan '%s'", directory);
		return false;
	}
	bool success = true;
	do {
		if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) continue;
		char source_path[ARCHIVE_PATH_CAPACITY];
		if (!archive_ingest_join(source_path, sizeof(source_path), directory, entry.cFileName)) {
			success = false;
			break;
		}
		if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			success = archive_ingest_discover_directory(
				source_path, paths, error, error_capacity);
		} else if (archive_ingest_has_dcm_extension(entry.cFileName)) {
			success = archive_ingest_append_path(paths, source_path);
		}
	} while (success && FindNextFileA(find, &entry));
	FindClose(find);
	if (!success && (!error || !error[0]))
		archive_ingest_set_error(error, error_capacity, "could not record discovered object path");
	return success;
}
#else
static bool archive_ingest_discover_directory(const char* directory,
		archive_ingest_paths_t* paths,
		char* error, size_t error_capacity) {
	DIR* stream = opendir(directory);
	if (!stream) {
		archive_ingest_set_error(error, error_capacity, "could not scan '%s': %s",
		                         directory, strerror(errno));
		return false;
	}
	bool success = true;
	struct dirent* entry;
	while (success && (entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		char source_path[ARCHIVE_PATH_CAPACITY];
		if (!archive_ingest_join(
				source_path, sizeof(source_path), directory, entry->d_name)) {
			success = false;
			break;
		}
		struct stat info;
		if (stat(source_path, &info) != 0) {
			archive_ingest_set_error(error, error_capacity, "could not inspect '%s': %s",
			                         source_path, strerror(errno));
			success = false;
		} else if (S_ISDIR(info.st_mode)) {
			success = archive_ingest_discover_directory(
				source_path, paths, error, error_capacity);
		} else if (S_ISREG(info.st_mode) && archive_ingest_has_dcm_extension(entry->d_name)) {
			success = archive_ingest_append_path(paths, source_path);
		}
	}
	closedir(stream);
	if (!success && (!error || !error[0]))
		archive_ingest_set_error(error, error_capacity, "could not record discovered object path");
	return success;
}
#endif

static int archive_ingest_compare_paths(const void* a, const void* b) {
	return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static const char* archive_ingest_image_flavor(const dicom_instance_t* instance) {
	switch (instance->image_flavor) {
		case DICOM_IMAGE_FLAVOR_VOLUME: return "VOLUME";
		case DICOM_IMAGE_FLAVOR_LABEL: return "LABEL";
		case DICOM_IMAGE_FLAVOR_OVERVIEW: return "OVERVIEW";
		case DICOM_IMAGE_FLAVOR_THUMBNAIL: return "THUMBNAIL";
		default: return "";
	}
}

static bool archive_ingest_copy(char* dest, size_t capacity, const char* value) {
	i32 length = snprintf(dest, capacity, "%s", value ? value : "");
	return length >= 0 && (size_t)length < capacity;
}

static int archive_ingest_compare_members(const void* a, const void* b) {
	const archive_ingest_member_t* left = a;
	const archive_ingest_member_t* right = b;
	return strcmp(left->database.sop_instance_uid, right->database.sop_instance_uid);
}

bool archive_ingest_build_part10_record(archive_database_instance_t* record,
		i64 object_id, const dicom_instance_t* instance) {
	if (!record || !instance || !instance->has_valid_wsi_image_type ||
		!instance->general_study.StudyInstanceUID.len || !instance->general_series.SeriesInstanceUID.len ||
		!instance->sop_instance_uid.len || !instance->sop_class_uid.len ||
		!archive_frame_media_type_for_transfer_syntax(instance->transfer_syntax_uid.value) ||
		instance->number_of_frames <= 0 || !instance->rows || !instance->columns ||
		!instance->total_pixel_matrix_columns || !instance->total_pixel_matrix_rows) return false;
	memset(record, 0, sizeof(*record));
	record->object_id = object_id;
	if (!archive_ingest_copy(record->sop_instance_uid, sizeof(record->sop_instance_uid),
	                         instance->sop_instance_uid.value) ||
	    !archive_ingest_copy(record->sop_class_uid, sizeof(record->sop_class_uid),
	                         instance->sop_class_uid.value) ||
	    !archive_ingest_copy(record->study_instance_uid, sizeof(record->study_instance_uid),
	                         instance->general_study.StudyInstanceUID.value) ||
	    !archive_ingest_copy(record->series_instance_uid, sizeof(record->series_instance_uid),
	                         instance->general_series.SeriesInstanceUID.value) ||
	    !archive_ingest_copy(record->transfer_syntax_uid, sizeof(record->transfer_syntax_uid),
	                         instance->transfer_syntax_uid.value) ||
	    !archive_ingest_copy(record->modality, sizeof(record->modality),
	                         instance->general_series.Modality.value) ||
	    !archive_ingest_copy(record->image_flavor, sizeof(record->image_flavor),
	                         archive_ingest_image_flavor(instance))) return false;
	i32 logical_length = snprintf(record->logical_name, sizeof(record->logical_name),
	                              "%s.dcm", record->sop_instance_uid);
	if (logical_length < 0 || (size_t)logical_length >= sizeof(record->logical_name)) return false;
	if (instance->general_study.StudyDate.present && instance->general_study.StudyDate.year > 0) {
		snprintf(record->study_date, sizeof(record->study_date), "%04u%02u%02u",
		         instance->general_study.StudyDate.year, instance->general_study.StudyDate.month,
		         instance->general_study.StudyDate.day);
	}
	record->has_instance_number = instance->instance_number >= 0;
	record->instance_number = instance->instance_number;
	record->frame_count = instance->number_of_frames;
	record->rows = instance->rows;
	record->columns = instance->columns;
	record->total_pixel_matrix_rows = instance->total_pixel_matrix_rows;
	record->total_pixel_matrix_columns = instance->total_pixel_matrix_columns;
	return true;
}

static bool archive_ingest_build_member(archive_ingest_member_t* member,
		const archive_object_t* object, const dicom_instance_t* instance) {
	memset(member, 0, sizeof(*member));
	member->object = *object;
	return archive_ingest_build_part10_record(&member->database, object->id, instance);
}

static bool archive_ingest_build_descriptor(const archive_ingest_member_t* members, i32 count,
		char** descriptor, size_t* descriptor_size) {
	size_t capacity = 32 + (size_t)count * 256;
	char* data = malloc(capacity);
	if (!data) return false;
	size_t used = (size_t)snprintf(data, capacity, "slidescape-part10-bundle-v1\n");
	for (i32 i = 0; i < count; ++i) {
		const archive_ingest_member_t* member = members + i;
		i32 length = snprintf(data + used, capacity - used,
			"object\t%d\tpart10-instance\t%s\t%s\t%llu\n", i,
			member->database.logical_name, member->object.sha256,
			(unsigned long long)member->object.byte_size);
		if (length < 0 || (size_t)length >= capacity - used) {
			free(data);
			return false;
		}
		used += (size_t)length;
	}
	*descriptor = data;
	*descriptor_size = used;
	return true;
}



static bool archive_ingest_append(char** data, size_t* capacity, size_t* used,
		const char* format, ...) {
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	i32 needed = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (needed < 0) {
		va_end(args);
		return false;
	}
	size_t required = *used + (size_t)needed + 1;
	if (required > *capacity) {
		size_t new_capacity = *capacity ? *capacity : 4096;
		while (new_capacity < required) new_capacity *= 2;
		char* replacement = realloc(*data, new_capacity);
		if (!replacement) {
			va_end(args);
			return false;
		}
		*data = replacement;
		*capacity = new_capacity;
	}
	vsnprintf(*data + *used, *capacity - *used, format, args);
	va_end(args);
	*used += (size_t)needed;
	return true;
}

static bool archive_ingest_mrxs_descriptor(const archive_mrxs_adapter_t* adapter,
		const char* mrxs_path, char** descriptor, size_t* descriptor_size,
		char* error, size_t error_capacity) {
	*descriptor = NULL;
	*descriptor_size = 0;
	char directory[ARCHIVE_PATH_CAPACITY];
	snprintf(directory, sizeof(directory), "%s", mrxs_path);
	char* extension = strrchr(directory, '.');
	if (!extension) return false;
	*extension = '\0';
	const char* source_name = strrchr(mrxs_path, '/');
	const char* windows_name = strrchr(mrxs_path, '\\');
	if (!source_name || (windows_name && windows_name > source_name))
		source_name = windows_name;
	source_name = source_name ? source_name + 1 : mrxs_path;
	size_t capacity = 0;
	size_t used = 0;
	char* data = NULL;
	if (!archive_ingest_append(&data, &capacity, &used,
	                           "slidescape-mrxs-bundle-v1\n")) return false;
	i32 file_count = adapter->slide.dat_count + ARCHIVE_MRXS_MEMBER_FIRST_DATA;
	for (i32 i = 0; i < file_count; ++i) {
		char full_path[ARCHIVE_PATH_CAPACITY];
		char logical_name[ARCHIVE_PATH_CAPACITY];
		if (i == ARCHIVE_MRXS_MEMBER_SLIDE) {
			snprintf(full_path, sizeof(full_path), "%s", mrxs_path);
			snprintf(logical_name, sizeof(logical_name), "%s", source_name);
		} else {
			const char* name = i == ARCHIVE_MRXS_MEMBER_SLIDEDAT ? "Slidedat.ini" :
				i == ARCHIVE_MRXS_MEMBER_INDEX ? adapter->slide.index_dat_filename :
				adapter->slide.dat_filenames[i - ARCHIVE_MRXS_MEMBER_FIRST_DATA];
			if (!name || strchr(name, '\n') || strchr(name, '\t') ||
			    snprintf(full_path, sizeof(full_path), "%s%s%s", directory,
			             PATH_SEP, name) >= (i32)sizeof(full_path) ||
			    snprintf(logical_name, sizeof(logical_name), "companion/%s",
			             name) >= (i32)sizeof(logical_name)) {
				free(data);
				return false;
			}
		}
		char digest[65];
		u64 byte_size = 0;
		if (!archive_sha256_file(full_path, digest, &byte_size) ||
		    !archive_ingest_append(
			    &data, &capacity, &used, "object\t%d\t%s\t%s\t%llu\n",
			    i, logical_name, digest, (unsigned long long)byte_size)) {
			archive_ingest_set_error(error, error_capacity,
			                         "could not hash MRXS source '%s'", full_path);
			free(data);
			return false;
		}
	}
	*descriptor = data;
	*descriptor_size = used;
	return true;
}

bool archive_ingest_part10_path(const archive_repository_t* repository,
		const char* input_path, bool reference_source,
		archive_ingest_result_t* result, char* error, size_t error_capacity) {
	if (result) memset(result, 0, sizeof(*result));
	if (error && error_capacity) error[0] = '\0';
	if (!repository || !input_path || !input_path[0] || !dicom_init()) {
		archive_ingest_set_error(error, error_capacity, "could not initialize DICOM ingestion");
		return false;
	}
	char source_root[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	DWORD source_length = GetFullPathNameA(
		input_path, (DWORD)sizeof(source_root), source_root, NULL);
	if (!source_length || source_length >= sizeof(source_root)) {
#else
	if (!realpath(input_path, source_root)) {
#endif
		archive_ingest_set_error(error, error_capacity,
		                         "could not resolve Part 10 source '%s'",
		                         input_path);
		return false;
	}
	archive_ingest_paths_t paths = {0};
	struct stat source_info = {0};
	bool discovered = platform_stat(source_root, &source_info) == 0;
	if (discovered && S_ISDIR(source_info.st_mode)) {
		discovered = archive_ingest_discover_directory(
			source_root, &paths, error, error_capacity);
	} else if (discovered && S_ISREG(source_info.st_mode)) {
		discovered = archive_ingest_append_path(&paths, source_root);
	} else {
		discovered = false;
	}
	if (!discovered) {
		if (error && !error[0])
			archive_ingest_set_error(
				error, error_capacity,
				"Part 10 source is not a regular file or directory");
		archive_ingest_destroy_paths(&paths);
		return false;
	}
	qsort(paths.values, (size_t)paths.count, sizeof(*paths.values), archive_ingest_compare_paths);
	if (paths.count == 0) {
		archive_ingest_set_error(error, error_capacity,
		                         "no .dcm objects were found below '%s'",
		                         source_root);
		archive_ingest_destroy_paths(&paths);
		return false;
	}

	archive_ingest_member_t* members = calloc((size_t)paths.count, sizeof(*members));
	if (!members) {
		archive_ingest_set_error(error, error_capacity, "out of memory while preparing ingestion");
		archive_ingest_destroy_paths(&paths);
		return false;
	}
	i32 member_count = 0;
	bool success = true;
	for (i32 i = 0; i < paths.count && success; ++i) {
		char full_path[ARCHIVE_PATH_CAPACITY];
		archive_object_t object = {0};
		dicom_series_t dicom = {0};
		archive_object_status_t object_status = reference_source ?
			archive_object_reference_file(
				repository, paths.values[i], "application/dicom",
				&object, error, error_capacity) :
			archive_object_import_file(
				repository, paths.values[i], "application/dicom",
				&object, error, error_capacity);
		if (object_status != ARCHIVE_OBJECT_OK) {
			success = false;
		} else if (!archive_ingest_object_path(
				repository, &object,
				full_path, sizeof(full_path)) ||
		    !dicom_open_single_instance_from_path(&dicom, full_path) || arrlen(dicom.instances) != 1) {
			archive_ingest_set_error(error, error_capacity, "could not parse Part 10 object '%s'", paths.values[i]);
			success = false;
		} else {
			const dicom_instance_t* instance = dicom.instances;
			if (!archive_frame_media_type_for_transfer_syntax(instance->transfer_syntax_uid.value)) {
				archive_ingest_set_error(error, error_capacity, "unsupported archive transfer syntax: %s",
					instance->transfer_syntax_uid.value);
				success = false;
			} else if (!instance->has_valid_wsi_image_type ||
			    !instance->general_study.StudyInstanceUID.len ||
			    !instance->general_series.SeriesInstanceUID.len ||
			    !instance->sop_instance_uid.len || !instance->sop_class_uid.len ||
			    !instance->transfer_syntax_uid.len || instance->number_of_frames <= 0 ||
			    !instance->rows || !instance->columns ||
			    !instance->total_pixel_matrix_columns || !instance->total_pixel_matrix_rows) {
				archive_ingest_set_error(error, error_capacity,
				                         "Part 10 object lacks required WSI metadata: '%s'", paths.values[i]);
				success = false;
			} else {
				success = archive_ingest_build_member(members + member_count, &object, instance);
				if (success) ++member_count;
				else if (error && !error[0])
					archive_ingest_set_error(error, error_capacity, "out of memory while indexing '%s'", paths.values[i]);
			}
		}
		dicom_destroy(&dicom);
	}
	if (success) {
		qsort(members, (size_t)member_count, sizeof(*members), archive_ingest_compare_members);
		for (i32 i = 1; i < member_count; ++i) {
			if (strcmp(members[i - 1].database.sop_instance_uid,
			           members[i].database.sop_instance_uid) != 0) continue;
			if (strcmp(members[i - 1].object.sha256, members[i].object.sha256) != 0) {
				archive_ingest_set_error(error, error_capacity,
					"duplicate SOP Instance UID '%s' has different content",
					members[i].database.sop_instance_uid);
				success = false;
				break;
			}
			memmove(members + i, members + i + 1,
			        (size_t)(member_count - i - 1) * sizeof(*members));
			--member_count;
			--i;
		}
	}
	if (success) {
		for (i32 i = 0; i < member_count && success; ++i) {
			for (i32 j = i + 1; j < member_count; ++j) {
				if (strcmp(members[i].database.series_instance_uid,
				           members[j].database.series_instance_uid) == 0 &&
				    strcmp(members[i].database.study_instance_uid,
				           members[j].database.study_instance_uid) != 0) {
					archive_ingest_set_error(
						error, error_capacity,
						"Series Instance UID '%s' is reused across studies",
						members[i].database.series_instance_uid);
					success = false;
					break;
				}
			}
		}
	}
	archive_database_shell_publication_t* publications = NULL;
	i32 publication_count = 0;
	if (success) {
		publications = calloc((size_t)member_count, sizeof(*publications));
		if (!publications) {
			archive_ingest_set_error(error, error_capacity,
			                         "out of memory while preparing virtual DICOM shells");
			success = false;
		}
	}
	for (i32 i = 0; i < member_count && success; ++i) {
		bool already_grouped = false;
		for (i32 group = 0; group < publication_count; ++group) {
			if (strcmp(publications[group].representation_id,
			           members[i].database.series_instance_uid) == 0) {
				already_grouped = true;
				break;
			}
		}
		if (already_grouped) continue;
		i32 group_count = 0;
		for (i32 j = 0; j < member_count; ++j) {
			if (strcmp(members[i].database.study_instance_uid,
			           members[j].database.study_instance_uid) == 0 &&
			    strcmp(members[i].database.series_instance_uid,
			           members[j].database.series_instance_uid) == 0) {
				++group_count;
			}
		}
		/* Bundle identity belongs to one logical slide, independent of the batch
		 * used to discover it. All prepared publications still commit together. */
		archive_ingest_member_t* group_members = calloc((size_t)group_count, sizeof(*group_members));
		archive_database_instance_t* database_members =
			calloc((size_t)group_count, sizeof(*database_members));
		char* descriptor = NULL;
		size_t descriptor_size = 0;
		archive_object_t descriptor_object = {0};
		archive_database_bundle_t bundle = {0};
		i32 grouped = 0;
		success = group_members && database_members;
		for (i32 j = 0; j < member_count && success; ++j) {
			if (strcmp(members[i].database.series_instance_uid,
			           members[j].database.series_instance_uid) != 0) continue;
			group_members[grouped] = members[j];
			database_members[grouped++] = members[j].database;
		}
		success = success && archive_ingest_build_descriptor(
			group_members, group_count, &descriptor, &descriptor_size);
		if (success) success = archive_object_store_control_bytes(
			repository, descriptor, descriptor_size,
			"application/vnd.slidescape.part10-bundle", &descriptor_object,
			error, error_capacity) == ARCHIVE_OBJECT_OK;
		if (success) success = archive_database_register_part10_bundle(
			repository, descriptor_object.sha256, descriptor_object.id,
			database_members, group_count, &bundle,
			error, error_capacity) == ARCHIVE_DATABASE_OK;
		free(descriptor);
		free(database_members);
		free(group_members);
		if (!success) {
			if (error && !error[0]) archive_ingest_set_error(
				error, error_capacity, "could not prepare logical slide bundle");
			break;
		}
		archive_database_shell_publication_t* publication =
			publications + publication_count;
		publication->bundle_id = bundle.id;
		archive_database_shell_instance_publication_t* published_instances =
			calloc((size_t)group_count, sizeof(*published_instances));
		dicom_series_t* dicoms = calloc((size_t)group_count, sizeof(*dicoms));
		archive_shell_build_part10_input_t* shell_instances =
			calloc((size_t)group_count, sizeof(*shell_instances));
		archive_shell_part10_recipe_input_t* recipe_instances =
			calloc((size_t)group_count, sizeof(*recipe_instances));
		i32 group_index = 0;
		char* recipe = NULL;
		size_t recipe_size = 0;
		char fingerprint[65];
		archive_object_t recipe_object = {0};
		archive_object_t shell_object = {0};
		archive_shell_buffer_t shell = {0};
		if (!published_instances || !dicoms || !shell_instances ||
		    !recipe_instances ||
		    !archive_ingest_copy(
			    publication->representation_id,
			    sizeof(publication->representation_id),
			    members[i].database.series_instance_uid)) {
			archive_ingest_set_error(
				error, error_capacity,
				"out of memory while grouping logical slide instances");
			success = false;
		}
		for (i32 j = 0; j < member_count && success; ++j) {
			if (strcmp(members[i].database.study_instance_uid,
			           members[j].database.study_instance_uid) != 0 ||
			    strcmp(members[i].database.series_instance_uid,
			           members[j].database.series_instance_uid) != 0) continue;
			char source_path[ARCHIVE_PATH_CAPACITY];
			if (!archive_ingest_object_path(
					repository, &members[j].object,
					source_path, sizeof(source_path)) ||
			    !dicom_open_single_instance_from_path(
				    dicoms + group_index, source_path) ||
			    arrlen(dicoms[group_index].instances) != 1) {
				archive_ingest_set_error(
					error, error_capacity,
					"could not reopen immutable Part 10 object for "
					"aggregate shell generation");
				success = false;
				break;
			}
			published_instances[group_index].instance = members[j].database;
			published_instances[group_index].shell_instance_index =
				(u32)group_index;
			recipe_instances[group_index] =
				(archive_shell_part10_recipe_input_t){
					.bundle_sha256 = bundle.bundle_digest,
					.source_sha256 = members[j].object.sha256,
					.source_byte_size = members[j].object.byte_size,
					.sop_instance_uid =
						members[j].database.sop_instance_uid,
				};
			shell_instances[group_index] =
				(archive_shell_build_part10_input_t){
					.instance = dicoms[group_index].instances,
					.source_sha256 = members[j].object.sha256,
					.source_byte_size = members[j].object.byte_size,
				};
			++group_index;
		}
		archive_shell_part10_aggregate_recipe_input_t recipe_input = {
			.bundle_sha256 = bundle.bundle_digest,
			.study_instance_uid = members[i].database.study_instance_uid,
			.series_instance_uid = members[i].database.series_instance_uid,
			.instances = recipe_instances,
			.instance_count = (u32)group_count,
		};
		if (success && group_index != group_count) {
			archive_ingest_set_error(error, error_capacity,
			                         "logical slide grouping is inconsistent");
			success = false;
		} else if (success && !archive_shell_build_part10_aggregate_recipe(
				&recipe_input, &recipe, &recipe_size, fingerprint)) {
			archive_ingest_set_error(error, error_capacity,
			                         "could not build canonical aggregate shell recipe");
			success = false;
		} else if (success && archive_object_store_control_bytes(
				repository, recipe, recipe_size, ARCHIVE_SHELL_RECIPE_MEDIA_TYPE,
				&recipe_object, error, error_capacity) != ARCHIVE_OBJECT_OK) {
			success = false;
		} else if (success) {
			archive_shell_build_part10_aggregate_input_t shell_input = {
				.instances = shell_instances,
				.instance_count = (u32)group_count,
				.representation_fingerprint = fingerprint,
			};
			if (!archive_shell_build_part10_aggregate(
				    &shell_input, &shell, error, error_capacity) ||
			    archive_object_store_shell_bytes(repository, shell.data, shell.size,
			                                     &shell_object, error, error_capacity) !=
			    ARCHIVE_OBJECT_OK) {
				success = false;
			} else {
				publication->shell_object_id = shell_object.id;
				publication->recipe_object_id = recipe_object.id;
				memcpy(publication->representation_fingerprint, fingerprint, 65);
				publication->instances = published_instances;
				publication->instance_count = group_count;
				++publication_count;
				published_instances = NULL;
			}
		}
		archive_shell_buffer_destroy(&shell);
		free(recipe);
		if (dicoms) for (i32 j = 0; j < group_count; ++j)
			dicom_destroy(dicoms + j);
		free(recipe_instances);
		free(shell_instances);
		free(dicoms);
		free(published_instances);
	}
	if (success && archive_database_publish_shells(
			repository, publications, publication_count,
			error, error_capacity) != ARCHIVE_DATABASE_OK) success = false;
	if (success && result) {
		for (i32 i = 0; i < member_count; ++i) {
			bool new_study = true;
			bool new_series = true;
			for (i32 j = 0; j < i; ++j) {
				if (strcmp(members[j].database.study_instance_uid,
				           members[i].database.study_instance_uid) == 0) new_study = false;
				if (strcmp(members[j].database.series_instance_uid,
				           members[i].database.series_instance_uid) == 0) new_series = false;
			}
			result->study_count += new_study;
			result->series_count += new_series;
		}
		result->instance_count = member_count;
	}
	for (i32 i = 0; i < publication_count; ++i)
		free((void*)publications[i].instances);
	free(publications);
	free(members);
	archive_ingest_destroy_paths(&paths);
	return success;
}

bool archive_ingest_part10_objects(const archive_repository_t* repository,
		archive_ingest_result_t* result, char* error, size_t error_capacity) {
	char source_path[ARCHIVE_PATH_CAPACITY];
	if (!repository ||
	    !archive_repository_make_path(
		    repository, "objects", source_path, sizeof(source_path)))
		return false;
	return archive_ingest_part10_path(
		repository, source_path, false, result, error, error_capacity);
}

static bool archive_ingest_mrxs(const archive_repository_t* repository,
		const char* mrxs_path, const archive_object_t* zip_object, archive_ingest_result_t* result,
		char* error, size_t error_capacity) {
	if (result) memset(result, 0, sizeof(*result));
	if (error && error_capacity) error[0] = '\0';
	if (!repository || !mrxs_path || !mrxs_path[0]) return false;
	char source_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	DWORD length = GetFullPathNameA(mrxs_path, (DWORD)sizeof(source_path),
	                                source_path, NULL);
	if (!length || length >= sizeof(source_path)) {
#else
	if (!realpath(mrxs_path, source_path)) {
#endif
		archive_ingest_set_error(error, error_capacity,
		                         "could not resolve MRXS source path '%s'",
		                         mrxs_path);
		return false;
	}

	archive_mrxs_adapter_t adapter = {0};
	if (!(zip_object ? archive_mrxs_adapter_open_zip(&adapter, source_path, error, error_capacity) :
		archive_mrxs_adapter_open(&adapter, source_path, error, error_capacity)))
		return false;

	char* descriptor = NULL;
	size_t descriptor_size = 0;
	archive_object_t descriptor_object = {0};
	archive_object_t recipe_object = {0};
	archive_object_t shell_object = {0};
	archive_database_bundle_t bundle = {0};
	archive_shell_buffer_t shell = {0};
	char* recipe = NULL;
	size_t recipe_size = 0;
	archive_database_shell_instance_publication_t* instances = NULL;
	bool success = zip_object || archive_ingest_mrxs_descriptor(
		&adapter, source_path, &descriptor, &descriptor_size,
		error, error_capacity);
	if (zip_object) descriptor_object = *zip_object;

	if (success && !zip_object && archive_object_store_control_bytes(
			repository, descriptor, descriptor_size,
			"application/vnd.slidescape.mrxs-bundle",
			&descriptor_object, error, error_capacity) != ARCHIVE_OBJECT_OK)
		success = false;
	char fingerprint_input[160];
	char fingerprint[65] = {0};
	if (success && zip_object) {
		char canonical[512];
		success = archive_mrxs_zip_recipe(zip_object->sha256, zip_object->byte_size, canonical, &recipe_size, fingerprint);
		if (success) { recipe = malloc(recipe_size + 1); success = recipe != NULL; }
		if (success) memcpy(recipe, canonical, recipe_size + 1);
	} else if (success) {
		i32 length = snprintf(fingerprint_input, sizeof(fingerprint_input),
			"slidescape-mrxs-virtual-dicom-v" ARCHIVE_MRXS_ADAPTER_VERSION_STRING "\n%s\n", descriptor_object.sha256);
		success = length > 0 && (size_t)length < sizeof(fingerprint_input) &&
			archive_sha256_bytes(fingerprint_input, length, fingerprint) &&
			archive_mrxs_adapter_build_recipe(source_path, descriptor_object.sha256, descriptor_object.byte_size,
				fingerprint, &recipe, &recipe_size);
	}
	if (success) success = archive_mrxs_adapter_set_identity(&adapter, descriptor_object.sha256);
	if (success) success = (zip_object ? archive_database_register_mrxs_zip_bundle(repository,
		descriptor_object.sha256, descriptor_object.id, &bundle, error, error_capacity) :
		archive_database_register_mrxs_bundle(repository, descriptor_object.sha256, descriptor_object.id,
			&bundle, error, error_capacity)) == ARCHIVE_DATABASE_OK;
	if (success && archive_object_store_control_bytes(
			repository, recipe, recipe_size, ARCHIVE_MRXS_RECIPE_MEDIA_TYPE,
			&recipe_object, error, error_capacity) != ARCHIVE_OBJECT_OK)
		success = false;
	if (success && (!archive_mrxs_adapter_build_shell(
			&adapter, descriptor_object.sha256,
			descriptor_object.byte_size, fingerprint,
			&shell, error, error_capacity) ||
	    archive_object_store_shell_bytes(
		    repository, shell.data, shell.size, &shell_object,
		    error, error_capacity) != ARCHIVE_OBJECT_OK))
		success = false;

	i32 instance_count = success ? adapter.slide.level_count : 0;
	if (success) {
		instances = calloc((size_t)instance_count, sizeof(*instances));
		if (!instances) {
			archive_ingest_set_error(error, error_capacity,
			                         "out of memory while publishing MRXS pyramid");
			success = false;
		}
	}
	for (i32 i = 0; i < instance_count && success; ++i) {
		const mrxs_level_t* level = adapter.slide.levels + i;
		archive_database_shell_instance_publication_t* published =
			instances + i;
		archive_database_instance_t* instance = &published->instance;
		u32 scale = 1u << i;
		instance->object_id = descriptor_object.id;
		snprintf(instance->logical_name, sizeof(instance->logical_name),
		         "level-%02d", i);
		success =
			archive_ingest_copy(instance->study_instance_uid,
			                    sizeof(instance->study_instance_uid),
			                    adapter.study_instance_uid) &&
			archive_ingest_copy(instance->series_instance_uid,
			                    sizeof(instance->series_instance_uid),
			                    adapter.series_instance_uid) &&
			archive_ingest_copy(instance->sop_instance_uid,
			                    sizeof(instance->sop_instance_uid),
			                    adapter.sop_instance_uids[i]) &&
			archive_ingest_copy(instance->sop_class_uid,
			                    sizeof(instance->sop_class_uid),
			                    "1.2.840.10008.5.1.4.1.1.77.1.6") &&
			archive_ingest_copy(instance->transfer_syntax_uid,
			                    sizeof(instance->transfer_syntax_uid),
			                    "1.2.840.10008.1.2.4.50") &&
			archive_ingest_copy(instance->modality,
			                    sizeof(instance->modality), "SM") &&
			archive_ingest_copy(instance->image_flavor,
			                    sizeof(instance->image_flavor), "VOLUME");
		instance->has_instance_number = true;
		instance->instance_number = i + 1;
		instance->frame_count = 0;
		for (i32 tile_index = 0;
		     tile_index < level->width_in_tiles * level->height_in_tiles;
		     ++tile_index)
			if (level->tiles[tile_index].hier_entry.length)
				++instance->frame_count;
		instance->rows = level->tile_height;
		instance->columns = level->tile_width;
		instance->total_pixel_matrix_rows =
			(adapter.slide.base_height_in_pixels + scale - 1) / scale;
		instance->total_pixel_matrix_columns =
			(adapter.slide.base_width_in_pixels + scale - 1) / scale;
		published->shell_instance_index = (u32)i;
	}

	archive_database_shell_publication_t publication = {0};
	publication.bundle_id = bundle.id;
	if (success) {
		success =
			archive_ingest_copy(publication.representation_id,
			                    sizeof(publication.representation_id),
			                    adapter.series_instance_uid) &&
			archive_ingest_copy(publication.representation_fingerprint,
			                    sizeof(publication.representation_fingerprint),
			                    fingerprint) &&
			archive_ingest_copy(publication.provider_kind,
			                    sizeof(publication.provider_kind),
			                    "mrxs-native") &&
			archive_ingest_copy(publication.adapter_version,
			                    sizeof(publication.adapter_version), zip_object ? ARCHIVE_MRXS_ZIP_ADAPTER_VERSION_STRING : ARCHIVE_MRXS_ADAPTER_VERSION_STRING) &&
			archive_ingest_copy(publication.frame_media_type,
			                    sizeof(publication.frame_media_type),
			                    "image/jpeg") &&
			archive_ingest_copy(publication.frame_source,
			                    sizeof(publication.frame_source),
			                    "GENERATED") &&
			archive_ingest_copy(publication.frame_policy_version,
			                    sizeof(publication.frame_policy_version),
			                    zip_object ? ARCHIVE_MRXS_ZIP_FRAME_POLICY : ARCHIVE_MRXS_FRAME_POLICY);
		publication.shell_object_id = shell_object.id;
		publication.recipe_object_id = recipe_object.id;
		publication.instances = instances;
		publication.instance_count = instance_count;
	}
	if (success && archive_database_publish_shells(
			repository, &publication, 1,
			error, error_capacity) != ARCHIVE_DATABASE_OK)
		success = false;
	if (success && result) {
		result->study_count = 1;
		result->series_count = 1;
		result->instance_count = instance_count;
	}
	if (!success && error && !error[0])
		archive_ingest_set_error(error, error_capacity,
		                         "could not ingest MRXS source");
	free(instances);
	archive_shell_buffer_destroy(&shell);
	free(recipe);
	free(descriptor);
	archive_mrxs_adapter_close(&adapter);
	return success;
}

bool archive_ingest_path(const archive_repository_t* repository,
		const char* source_path, bool reference_source,
		archive_ingest_result_t* result, char* error,
		size_t error_capacity) {
	if (result) memset(result, 0, sizeof(*result));
	if (error && error_capacity) error[0] = '\0';
	if (!repository || !source_path || !source_path[0]) {
		archive_ingest_set_error(
			error, error_capacity, "source path is required");
		return false;
	}
	const char* extension = strrchr(source_path, '.');
	bool is_zip = extension && !strcasecmp(extension, ".zip");
	char slidedat[ARCHIVE_PATH_CAPACITY];
	bool mrxs_directory = is_directory(source_path) &&
		archive_ingest_join(slidedat, sizeof(slidedat), source_path, "Slidedat.ini") && file_exists(slidedat);
	if (is_zip || mrxs_directory || archive_ingest_has_mrxs_extension(source_path)) {
		if (reference_source && !is_zip && !mrxs_directory)
			return archive_ingest_mrxs(repository, source_path, NULL, result, error, error_capacity);
		if (reference_source) {
			archive_ingest_set_error(error, error_capacity, "ZIP/directory ingestion retains an owned bundle; omit --reference");
			return false;
		}
		archive_object_t object = {0};
		bool success = is_zip ? archive_object_import_file(repository, source_path, "application/zip", &object,
			error, error_capacity) == ARCHIVE_OBJECT_OK :
			archive_zip_capture_mrxs(repository, source_path, &object, error, error_capacity);
		char owned_path[ARCHIVE_PATH_CAPACITY];
		if (success) success = archive_ingest_object_path(repository, &object, owned_path, sizeof(owned_path));
		archive_zip_kind_t kind = ARCHIVE_ZIP_MRXS;
		if (success && is_zip) success = archive_zip_validate(owned_path, &kind, error, error_capacity);
		if (success) success = kind == ARCHIVE_ZIP_MRXS ?
			archive_ingest_mrxs(repository, owned_path, &object, result, error, error_capacity) :
			archive_dicom_zip_ingest(repository, &object, owned_path, result, error, error_capacity);
		return success;
	}
	return archive_ingest_part10_path(repository, source_path, reference_source, result, error, error_capacity);
}
