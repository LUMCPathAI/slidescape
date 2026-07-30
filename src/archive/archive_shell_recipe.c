/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_shell_recipe.h"

#include "archive_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool archive_shell_recipe_digest_is_valid(const char* digest) {
	if (!digest || strlen(digest) != 64) return false;
	for (i32 i = 0; i < 64; ++i) {
		char c = digest[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

static bool archive_shell_recipe_uid_is_valid(const char* uid) {
	if (!uid || !uid[0] || strlen(uid) > 64) return false;
	for (const char* cursor = uid; *cursor; ++cursor) {
		if ((*cursor < '0' || *cursor > '9') && *cursor != '.') return false;
	}
	return true;
}

bool archive_shell_build_part10_recipe(
		const archive_shell_part10_recipe_input_t* input,
		char recipe[ARCHIVE_SHELL_RECIPE_CAPACITY], size_t* recipe_size,
		char fingerprint[65]) {
	if (recipe) recipe[0] = '\0';
	if (fingerprint) fingerprint[0] = '\0';
	if (!input || !recipe || !recipe_size || !fingerprint ||
	    !archive_shell_recipe_digest_is_valid(input->bundle_sha256) ||
	    !archive_shell_recipe_digest_is_valid(input->source_sha256) ||
	    !archive_shell_recipe_uid_is_valid(input->sop_instance_uid)) return false;

	int length = snprintf(recipe, ARCHIVE_SHELL_RECIPE_CAPACITY,
		"slidescape-virtual-dicom-recipe-v7-single\n"
		"bundle-sha256\t%s\n"
		"source-sha256\t%s\n"
		"source-byte-size\t%llu\n"
		"logical-role\tpart10-instance\n"
		"sop-instance-uid\t%s\n"
		"adapter\tslidescape-dicom-shell\n"
		"adapter-version\t7\n"
		"adapter-policy\tpart10-delta-payload-v7\n"
		"configuration\tbuiltin-default-v1\n"
		"shell-schema-version\t2\n",
		input->bundle_sha256, input->source_sha256,
		(unsigned long long)input->source_byte_size, input->sop_instance_uid);
	if (length < 0 || length >= ARCHIVE_SHELL_RECIPE_CAPACITY) return false;
	*recipe_size = (size_t)length;
	return archive_sha256_bytes(recipe, *recipe_size, fingerprint);
}

bool archive_shell_build_part10_aggregate_recipe(
		const archive_shell_part10_aggregate_recipe_input_t* input,
		char** recipe, size_t* recipe_size, char fingerprint[65]) {
	if (recipe) *recipe = NULL;
	if (recipe_size) *recipe_size = 0;
	if (fingerprint) fingerprint[0] = '\0';
	if (!input || !recipe || !recipe_size || !fingerprint ||
	    !archive_shell_recipe_digest_is_valid(input->bundle_sha256) ||
	    !archive_shell_recipe_uid_is_valid(input->study_instance_uid) ||
	    !archive_shell_recipe_uid_is_valid(input->series_instance_uid) ||
	    !input->instances || !input->instance_count) return false;
	size_t capacity = 512;
	if (input->instance_count > (SIZE_MAX - capacity) / 256) return false;
	capacity += (size_t)input->instance_count * 256;
	char* data = malloc(capacity);
	if (!data) return false;
	i32 length = snprintf(
		data, capacity,
		"slidescape-virtual-dicom-recipe-v7\n"
		"bundle-sha256\t%s\n"
		"logical-slide-study-uid\t%s\n"
		"logical-slide-series-uid\t%s\n"
		"instance-count\t%u\n",
		input->bundle_sha256, input->study_instance_uid,
		input->series_instance_uid, input->instance_count);
	bool success = length > 0 && (size_t)length < capacity;
	size_t used = success ? (size_t)length : 0;
	for (u32 i = 0; i < input->instance_count && success; ++i) {
		const archive_shell_part10_recipe_input_t* instance =
			input->instances + i;
		if (!archive_shell_recipe_digest_is_valid(instance->source_sha256) ||
		    !instance->source_byte_size ||
		    !archive_shell_recipe_uid_is_valid(instance->sop_instance_uid)) {
			success = false;
			break;
		}
		length = snprintf(
			data + used, capacity - used,
			"instance\t%u\t%s\t%s\t%llu\n",
			i, instance->sop_instance_uid, instance->source_sha256,
			(unsigned long long)instance->source_byte_size);
		if (length < 0 || (size_t)length >= capacity - used) {
			success = false;
			break;
		}
		used += (size_t)length;
	}
	if (success) {
		length = snprintf(
			data + used, capacity - used,
			"logical-role\tpart10-logical-slide\n"
			"grouping-policy\tpart10-study-series-v1\n"
			"adapter\tslidescape-dicom-shell\n"
			"adapter-version\t7\n"
			"adapter-policy\tpart10-delta-payload-v7\n"
			"configuration\tbuiltin-default-v1\n"
			"shell-schema-version\t2\n");
		if (length < 0 || (size_t)length >= capacity - used)
			success = false;
		else
			used += (size_t)length;
	}
	if (success)
		success = archive_sha256_bytes(data, used, fingerprint);
	if (!success) {
		free(data);
		return false;
	}
	*recipe = data;
	*recipe_size = used;
	return true;
}
