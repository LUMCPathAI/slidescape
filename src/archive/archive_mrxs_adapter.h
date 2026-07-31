/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_shell.h"
#include "archive_repository.h"
#include "common.h"
#include "mrxs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHIVE_MRXS_ADAPTER_VERSION 3
#define ARCHIVE_MRXS_MAX_LEVELS 16
#define ARCHIVE_MRXS_RECIPE_MEDIA_TYPE \
	"application/vnd.slidescape.mrxs-native-recipe"

typedef struct archive_mrxs_adapter_t {
	mrxs_t slide;
	char source_path[ARCHIVE_PATH_CAPACITY];
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uids[ARCHIVE_MRXS_MAX_LEVELS][65];
	bool is_open;
} archive_mrxs_adapter_t;

bool archive_mrxs_adapter_open(archive_mrxs_adapter_t* adapter,
		const char* mrxs_path, const char* representation_fingerprint,
		char* error, size_t error_capacity);
void archive_mrxs_adapter_close(archive_mrxs_adapter_t* adapter);
bool archive_mrxs_adapter_build_shell(archive_mrxs_adapter_t* adapter,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, archive_shell_buffer_t* shell,
		char* error, size_t error_capacity);
bool archive_mrxs_adapter_build_recipe(const char* mrxs_path,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, char** recipe,
		size_t* recipe_size);
bool archive_mrxs_adapter_get_rgb_frame(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 one_based_frame, u8** data, size_t* size,
		char* error, size_t error_capacity);
bool archive_mrxs_adapter_get_jpeg_frame(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 one_based_frame, u8** data, size_t* size,
		char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
