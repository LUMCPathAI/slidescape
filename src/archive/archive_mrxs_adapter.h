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

#define ARCHIVE_MRXS_ZIP_ADAPTER_VERSION 7
#define ARCHIVE_MRXS_ZIP_ADAPTER_VERSION_STRING "7"
#define ARCHIVE_MRXS_ZIP_FRAME_POLICY "mrxs-zip-jpeg-v" ARCHIVE_MRXS_ZIP_ADAPTER_VERSION_STRING
#define ARCHIVE_MRXS_ADAPTER_VERSION 6
#define ARCHIVE_MRXS_ADAPTER_VERSION_STRING "6"
#define ARCHIVE_MRXS_RECIPE_MAGIC "slidescape-mrxs-native-recipe-v6"
#define ARCHIVE_MRXS_FRAME_POLICY "mrxs-jpeg-shell-payload-v6"
#define ARCHIVE_MRXS_MAX_LEVELS 16
#define ARCHIVE_MRXS_RECIPE_MEDIA_TYPE \
	"application/vnd.slidescape.mrxs-native-recipe"

/* Fixed descriptor entries precede the indexed payload data files. */
typedef enum archive_mrxs_member_index_t {
	ARCHIVE_MRXS_MEMBER_SLIDE = 0,
	ARCHIVE_MRXS_MEMBER_SLIDEDAT = 1,
	ARCHIVE_MRXS_MEMBER_INDEX = 2,
	ARCHIVE_MRXS_MEMBER_FIRST_DATA = 3,
} archive_mrxs_member_index_t;

typedef struct archive_mrxs_adapter_t {
	mrxs_t slide;
	char source_path[ARCHIVE_PATH_CAPACITY];
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uids[ARCHIVE_MRXS_MAX_LEVELS][65];
	bool is_open;
	bool is_zip;
} archive_mrxs_adapter_t;

bool archive_mrxs_adapter_open(archive_mrxs_adapter_t* adapter,
		const char* mrxs_path,
		char* error, size_t error_capacity);
bool archive_mrxs_adapter_open_zip(archive_mrxs_adapter_t* adapter, const char* path, char* error, size_t capacity);
bool archive_mrxs_zip_recipe(const char* hash, u64 size, char recipe[512], size_t* recipe_size, char fingerprint[65]);
/* Native identities depend on canonical source membership, not adapter version. */
bool archive_mrxs_adapter_set_identity(archive_mrxs_adapter_t* adapter, const char* bundle_sha256);
void archive_mrxs_adapter_close(archive_mrxs_adapter_t* adapter);
bool archive_mrxs_adapter_build_shell(archive_mrxs_adapter_t* adapter,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, archive_shell_buffer_t* shell,
		char* error, size_t error_capacity);
bool archive_mrxs_adapter_build_recipe(const char* mrxs_path,
		const char* bundle_descriptor_sha256, u64 bundle_descriptor_size,
		const char* representation_fingerprint, char** recipe,
		size_t* recipe_size);
/* Development comparison helpers: zero-based dense native grid indices. */
bool archive_mrxs_adapter_get_rgb_tile(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 tile_index, u8** data, size_t* size,
		char* error, size_t error_capacity);
bool archive_mrxs_adapter_get_jpeg_tile(archive_mrxs_adapter_t* adapter,
		u32 level_index, u32 tile_index, u8** data, size_t* size,
		char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
