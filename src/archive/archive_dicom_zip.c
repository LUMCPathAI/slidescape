/* Slidescape. Copyright (C) 2019-2026 Pieter Valkema. SPDX-License-Identifier: GPL-3.0-or-later */
#include "archive_dicom_zip.h"
#include "archive_sha256.h"
#include "archive_shell_recipe.h"
#include "dicom_uid.h"

static bool archive_dicom_zip_entry(void* context, const source_entry_t* entry) {
	archive_dicom_zip_t* zip = context;
	if (entry->is_directory) return source_fs_list(zip->source, entry->path, archive_dicom_zip_entry, zip);
	if (entry->size < sizeof(dicom_header_t)) return true;
	source_file_t file;
	u8 header[sizeof(dicom_header_t)];
	if (!source_fs_open_file(zip->source, entry->path, &file) ||
		source_file_read(&file, 0, header, sizeof(header)) != SOURCE_READ_OK) return false;
	if (memcmp(header + 128, "DICM", 4)) return true; // Preserve ancillary members without publishing them.
	if (arrlen(zip->dicom.instances) >= ARCHIVE_DICOM_ZIP_MAX_INSTANCES) {
		if (zip->error && zip->error_capacity) snprintf(zip->error, zip->error_capacity,
			"DICOM ZIP exceeds the %u-instance limit", (unsigned)ARCHIVE_DICOM_ZIP_MAX_INSTANCES);
		return false;
	}
	if (!dicom_open_single_instance_from_source(&zip->dicom, &file, entry->path)) {
		if (zip->error && zip->error_capacity) snprintf(zip->error, zip->error_capacity,
			"Could not parse ZIP Part 10 member: %s", entry->path);
		return false;
	}
	archive_database_instance_t record;
	if (!archive_ingest_build_part10_record(&record, 0, zip->dicom.instances + arrlen(zip->dicom.instances) - 1)) {
		if (zip->error && zip->error_capacity) snprintf(zip->error, zip->error_capacity,
			"ZIP Part 10 member lacks supported WSI metadata: %s", entry->path);
		return false;
	}
	return true;
}

static int archive_dicom_zip_compare(const void* a, const void* b) {
	const dicom_instance_t* left = a;
	const dicom_instance_t* right = b;
	int order = strcmp(left->general_study.StudyInstanceUID.value, right->general_study.StudyInstanceUID.value);
	if (!order) order = strcmp(left->general_series.SeriesInstanceUID.value, right->general_series.SeriesInstanceUID.value);
	return order ? order : strcmp(left->sop_instance_uid.value, right->sop_instance_uid.value);
}

void archive_dicom_zip_close(archive_dicom_zip_t* zip) {
	if (!zip) return;
	dicom_destroy(&zip->dicom);
	source_fs_close(zip->source);
	memset(zip, 0, sizeof(*zip));
}

bool archive_dicom_zip_open(archive_dicom_zip_t* zip, const char* path, char* error, size_t capacity) {
	if (error && capacity) error[0] = 0;
	memset(zip, 0, sizeof(*zip));
	zip->error = error; zip->error_capacity = capacity;
	zip->source = source_fs_open_zip(path, error, capacity);
	bool success = zip->source && dicom_init() && source_fs_list(zip->source, "", archive_dicom_zip_entry, zip) &&
		arrlen(zip->dicom.instances) > 0;
	if (success) {
		qsort(zip->dicom.instances, arrlen(zip->dicom.instances), sizeof(*zip->dicom.instances), archive_dicom_zip_compare);
		/* No member hashes: duplicate SOP identities are ambiguous, even within one blob.
		   The bounded count keeps this cross-series uniqueness check small. */
		for (i32 i = 0; i < arrlen(zip->dicom.instances) && success; ++i) {
			dicom_instance_t* a = zip->dicom.instances + i;
			for (i32 j = 0; j < i; ++j) {
				dicom_instance_t* b = zip->dicom.instances + j;
				if (!strcmp(a->sop_instance_uid.value, b->sop_instance_uid.value) ||
					(!strcmp(a->general_series.SeriesInstanceUID.value, b->general_series.SeriesInstanceUID.value) &&
					strcmp(a->general_study.StudyInstanceUID.value, b->general_study.StudyInstanceUID.value))) {
					if (error && capacity) snprintf(error, capacity, "ZIP has duplicate SOP identities or a series reused across studies");
					success = false; break;
				}
			}
		}
	}
	if (!success) {
		if (error && capacity && !error[0]) snprintf(error, capacity, "Could not discover supported DICOM WSI instances in ZIP");
		archive_dicom_zip_close(zip);
	}
	return success;
}

bool archive_dicom_zip_recipe(const char* hash, u64 size, const char* study, const char* series,
		char recipe[512], size_t* recipe_size, char fingerprint[65]) {
	if (!hash || strlen(hash) != 64 || !size || !study || !series ||
		!dicom_uid_is_valid_string(study, strlen(study)) || !dicom_uid_is_valid_string(series, strlen(series))) return false;
	int length = snprintf(recipe, 512, ARCHIVE_DICOM_ZIP_RECIPE_MAGIC "\nblob-sha256\t%s\nblob-size\t%llu\nstudy-uid\t%s\nseries-uid\t%s\n",
		hash, (unsigned long long)size, study, series);
	if (length <= 0 || length >= 512) return false;
	*recipe_size = (size_t)length;
	return archive_sha256_bytes(recipe, *recipe_size, fingerprint);
}

static bool archive_dicom_zip_in_series(const dicom_instance_t* instance, const char* study, const char* series) {
	return !strcmp(instance->general_study.StudyInstanceUID.value, study) &&
		!strcmp(instance->general_series.SeriesInstanceUID.value, series);
}

bool archive_dicom_zip_build_series(archive_dicom_zip_t* zip, const char* study, const char* series,
		const char* hash, u64 size, const char* fingerprint, archive_shell_buffer_t* shell, char* error, size_t capacity) {
	u32 count = 0;
	archive_shell_build_part10_input_t* inputs = calloc(arrlen(zip->dicom.instances), sizeof(*inputs));
	if (!inputs) return false;
	for (i32 i = 0; i < arrlen(zip->dicom.instances); ++i) {
		dicom_instance_t* instance = zip->dicom.instances + i;
		if (!archive_dicom_zip_in_series(instance, study, series)) continue;
		inputs[count++] = (archive_shell_build_part10_input_t){
			.instance = instance, .source_sha256 = hash, .source_byte_size = size,
			.member_offset = instance->file.offset, .member_size = instance->file.size,
			.provider_parameter_version = ARCHIVE_DICOM_ZIP_PARAMETER_VERSION,
		};
	}
	archive_shell_build_part10_aggregate_input_t input = {inputs, count, fingerprint};
	bool success = count && archive_shell_build_part10_aggregate(&input, shell, error, capacity);
	free(inputs);
	return success;
}

bool archive_dicom_zip_ingest(const archive_repository_t* repository, const archive_object_t* object,
		const char* path, archive_ingest_result_t* result, char* error, size_t capacity) {
	archive_dicom_zip_t zip;
	if (!archive_dicom_zip_open(&zip, path, error, capacity)) return false;
	i32 count = (i32)arrlen(zip.dicom.instances), publication_count = 0;
	archive_database_bundle_t bundle = {0};
	archive_database_shell_publication_t* publications = calloc(count, sizeof(*publications));
	bool success = publications && archive_database_register_dicom_zip_bundle(repository, object->sha256,
		object->id, &bundle, error, capacity) == ARCHIVE_DATABASE_OK;
	for (i32 start = 0; start < count && success;) {
		const dicom_instance_t* first = zip.dicom.instances + start;
		const char* study = first->general_study.StudyInstanceUID.value;
		const char* series = first->general_series.SeriesInstanceUID.value;
		i32 end = start + 1;
		while (end < count && archive_dicom_zip_in_series(zip.dicom.instances + end, study, series)) ++end;
		archive_database_shell_publication_t* publication = publications + publication_count++;
		publication->bundle_id = bundle.id;
		snprintf(publication->representation_id, sizeof(publication->representation_id), "%s", series);
		snprintf(publication->adapter_version, sizeof(publication->adapter_version), "%s", ARCHIVE_DICOM_ZIP_ADAPTER_VERSION);
		snprintf(publication->frame_policy_version, sizeof(publication->frame_policy_version), "%s", ARCHIVE_DICOM_ZIP_FRAME_POLICY);
		publication->instance_count = end - start;
		archive_database_shell_instance_publication_t* instances = calloc(end - start, sizeof(*instances));
		publication->instances = instances;
		success = instances != NULL;
		for (i32 i = start; i < end && success; ++i) {
			instances[i-start].shell_instance_index = (u32)(i-start);
			success = archive_ingest_build_part10_record(&instances[i-start].instance, object->id, zip.dicom.instances + i);
		}
		char recipe[512]; size_t recipe_size = 0;
		archive_object_t recipe_object = {0}, shell_object = {0};
		archive_shell_buffer_t shell = {0};
		if (success) success = archive_dicom_zip_recipe(object->sha256, object->byte_size, study, series,
			recipe, &recipe_size, publication->representation_fingerprint) &&
			archive_dicom_zip_build_series(&zip, study, series, object->sha256, object->byte_size,
				publication->representation_fingerprint, &shell, error, capacity) &&
			archive_object_store_control_bytes(repository, recipe, recipe_size, ARCHIVE_SHELL_RECIPE_MEDIA_TYPE,
				&recipe_object, error, capacity) == ARCHIVE_OBJECT_OK &&
			archive_object_store_shell_bytes(repository, shell.data, shell.size, &shell_object, error, capacity) == ARCHIVE_OBJECT_OK;
		publication->recipe_object_id = recipe_object.id;
		publication->shell_object_id = shell_object.id;
		archive_shell_buffer_destroy(&shell);
		start = end;
	}
	if (success) success = archive_database_publish_shells(repository, publications, publication_count, error, capacity) == ARCHIVE_DATABASE_OK;
	if (success && result) {
		memset(result, 0, sizeof(*result));
		result->instance_count = count;
		result->series_count = publication_count;
		for (i32 i = 0; i < count; ++i)
			if (!i || strcmp(zip.dicom.instances[i-1].general_study.StudyInstanceUID.value,
				zip.dicom.instances[i].general_study.StudyInstanceUID.value)) ++result->study_count;
	}
	for (i32 i = 0; i < publication_count; ++i) free((void*)publications[i].instances);
	free(publications);
	archive_dicom_zip_close(&zip);
	if (!success && error && capacity && !error[0]) snprintf(error, capacity, "Could not publish DICOM ZIP bundle");
	return success;
}
