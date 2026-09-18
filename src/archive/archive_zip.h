/* Slidescape. Copyright (C) 2019-2026 Pieter Valkema. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "archive_object.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Capture the complete MRXS companion tree and optional sibling thumbnail. */
bool archive_zip_capture_mrxs(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity);
typedef enum archive_zip_kind_t { ARCHIVE_ZIP_MRXS, ARCHIVE_ZIP_DICOM } archive_zip_kind_t;
/* Full CRC validation, plus MRXS-first discovery. Other ZIPs go to DICOM admission. */
bool archive_zip_validate(const char* path, archive_zip_kind_t* kind, char* error, size_t capacity);
#ifdef __cplusplus
}
#endif
