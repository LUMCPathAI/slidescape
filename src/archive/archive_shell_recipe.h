/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#define ARCHIVE_SHELL_RECIPE_CAPACITY 1024
#define ARCHIVE_SHELL_RECIPE_MEDIA_TYPE \
	"application/vnd.slidescape.virtual-dicom-recipe"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct archive_shell_part10_recipe_input_t {
	const char* bundle_sha256;
	const char* source_sha256;
	u64 source_byte_size;
	const char* sop_instance_uid;
} archive_shell_part10_recipe_input_t;

typedef struct archive_shell_part10_aggregate_recipe_input_t {
	const char* bundle_sha256;
	const char* study_instance_uid;
	const char* series_instance_uid;
	const archive_shell_part10_recipe_input_t* instances;
	u32 instance_count;
} archive_shell_part10_aggregate_recipe_input_t;

/*
 * Emit the exact canonical generation recipe retained as provenance and hash
 * those bytes into the representation fingerprint.
 */
bool archive_shell_build_part10_recipe(
		const archive_shell_part10_recipe_input_t* input,
		char recipe[ARCHIVE_SHELL_RECIPE_CAPACITY], size_t* recipe_size,
		char fingerprint[65]);
bool archive_shell_build_part10_aggregate_recipe(
		const archive_shell_part10_aggregate_recipe_input_t* input,
		char** recipe, size_t* recipe_size, char fingerprint[65]);

#ifdef __cplusplus
}
#endif
