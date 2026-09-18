/* Slidescape. Copyright (C) 2019-2026 Pieter Valkema. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "archive_ingest.h"
#include "archive_object.h"
#include "archive_shell.h"
#include "dicom.h"
#ifdef __cplusplus
extern "C" {
#endif

#define ARCHIVE_DICOM_ZIP_ADAPTER_VERSION "1"
#define ARCHIVE_DICOM_ZIP_RECIPE_MAGIC "slidescape-dicom-zip-v" ARCHIVE_DICOM_ZIP_ADAPTER_VERSION
#define ARCHIVE_DICOM_ZIP_FRAME_POLICY "dicom-zip-ranges-v" ARCHIVE_DICOM_ZIP_ADAPTER_VERSION
/* Part 10 shell parameter contract: 1 = standalone file; 2 = resolved ZIP ranges. */
enum { ARCHIVE_DICOM_ZIP_PARAMETER_VERSION = 2, ARCHIVE_DICOM_ZIP_MAX_INSTANCES = 4096 };

/* Owns the namespace and parsed instances. No viewer pyramid or per-member hashes. */
typedef struct archive_dicom_zip_t {
	source_fs_t* source;
	dicom_series_t dicom;
	char* error;
	size_t error_capacity;
} archive_dicom_zip_t;
bool archive_dicom_zip_open(archive_dicom_zip_t* zip, const char* path, char* error, size_t capacity);
void archive_dicom_zip_close(archive_dicom_zip_t* zip);
bool archive_dicom_zip_recipe(const char* hash, u64 size, const char* study, const char* series,
	char recipe[512], size_t* recipe_size, char fingerprint[65]);
bool archive_dicom_zip_build_series(archive_dicom_zip_t* zip, const char* study, const char* series,
	const char* hash, u64 size, const char* fingerprint, archive_shell_buffer_t* shell, char* error, size_t capacity);
bool archive_dicom_zip_ingest(const archive_repository_t* repository, const archive_object_t* object,
	const char* path, archive_ingest_result_t* result, char* error, size_t capacity);
#ifdef __cplusplus
}
#endif
