/* Slidescape. Copyright (C) 2019-2026 Pieter Valkema. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "archive_object.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Capture the complete MRXS companion tree and optional sibling thumbnail. */
bool archive_zip_capture_mrxs(const archive_repository_t* repository, const char* path,
		archive_object_t* object, char* error, size_t capacity);
bool archive_zip_validate(const char* path, char* error, size_t capacity);
#ifdef __cplusplus
}
#endif
