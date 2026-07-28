/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_routes.h"

#include "common.h"
#include "dicom_uid.h"

#include <ctype.h>

#define ARCHIVE_ROUTE_SEGMENT_COUNT 12
#define ARCHIVE_ROUTE_TARGET_CAPACITY 2048

static bool archive_copy_value(char* dest, size_t capacity, const char* value) {
	int length = snprintf(dest, capacity, "%s", value);
	return length >= 0 && (size_t)length < capacity;
}

static bool archive_is_frame_list(const char* value) {
	if (!value[0]) return false;
	bool need_digit = true;
	for (const char* p = value; *p; ++p) {
		if (isdigit((unsigned char)*p)) {
			need_digit = false;
		} else if (*p == ',' && !need_digit) {
			need_digit = true;
		} else {
			return false;
		}
	}
	return !need_digit;
}

bool archive_frame_list_parse(const char* text, u32* frames, i32 capacity, i32* count) {
	if (count) *count = 0;
	if (!text || !text[0] || !frames || capacity <= 0 || !count) return false;
	i32 parsed_count = 0;
	const char* cursor = text;
	while (*cursor) {
		if (parsed_count == capacity) return false;
		u64 value = 0;
		const char* start = cursor;
		while (*cursor >= '0' && *cursor <= '9') {
			value = value * 10 + (u64)(*cursor - '0');
			if (value > UINT32_MAX) return false;
			++cursor;
		}
		if (cursor == start || value == 0 || (*cursor && *cursor != ',')) return false;
		frames[parsed_count++] = (u32)value;
		if (*cursor == ',') {
			++cursor;
			if (!*cursor) return false;
		}
	}
	*count = parsed_count;
	return true;
}

archive_http_method_t archive_http_method_parse(const char* method) {
	if (method && strcmp(method, "GET") == 0) return ARCHIVE_HTTP_METHOD_GET;
	if (method && strcmp(method, "POST") == 0) return ARCHIVE_HTTP_METHOD_POST;
	return ARCHIVE_HTTP_METHOD_UNKNOWN;
}

bool archive_route_match(archive_http_method_t method, const char* target,
		archive_route_match_t* match) {
	if (!target || !match || target[0] != '/') return false;
	memset(match, 0, sizeof(*match));
	if (method == ARCHIVE_HTTP_METHOD_GET && strcmp(target, "/health") == 0) {
		match->kind = ARCHIVE_ROUTE_HEALTH;
		return true;
	}

	char path[ARCHIVE_ROUTE_TARGET_CAPACITY];
	const char* query = strchr(target, '?');
	size_t path_length = query ? (size_t)(query - target) : strlen(target);
	if (path_length == 0 || path_length >= sizeof(path)) return false;
	memcpy(path, target, path_length);
	path[path_length] = '\0';

	char* segments[ARCHIVE_ROUTE_SEGMENT_COUNT] = {0};
	i32 segment_count = 0;
	char* cursor = path + 1;
	while (*cursor && segment_count < ARCHIVE_ROUTE_SEGMENT_COUNT) {
		segments[segment_count++] = cursor;
		char* slash = strchr(cursor, '/');
		if (!slash) break;
		*slash = '\0';
		cursor = slash + 1;
		if (!*cursor) return false;
	}
	if (*cursor && segment_count == ARCHIVE_ROUTE_SEGMENT_COUNT) return false;

	i32 offset = segment_count > 0 && strcmp(segments[0], "dicom-web") == 0 ? 1 : 0;
	if (segment_count <= offset || strcmp(segments[offset], "studies") != 0) return false;
	char** route = &segments[offset];
	i32 count = segment_count - offset;

	if (count == 1) {
		if (method == ARCHIVE_HTTP_METHOD_GET) match->kind = ARCHIVE_ROUTE_QIDO_STUDIES;
		else if (method == ARCHIVE_HTTP_METHOD_POST) match->kind = ARCHIVE_ROUTE_STOW_STUDIES;
		else return false;
		return true;
	}
	if (method != ARCHIVE_HTTP_METHOD_GET || count < 3 ||
	    !dicom_uid_is_valid_string(route[1], strlen(route[1])) ||
	    strcmp(route[2], "series") != 0) return false;
	if (!archive_copy_value(match->study_uid, sizeof(match->study_uid), route[1])) return false;
	if (count == 3) {
		match->kind = ARCHIVE_ROUTE_QIDO_SERIES;
		return true;
	}
	if (!dicom_uid_is_valid_string(route[3], strlen(route[3])) || !archive_copy_value(match->series_uid,
			sizeof(match->series_uid), route[3])) return false;
	if (count == 5 && strcmp(route[4], "metadata") == 0) {
		match->kind = ARCHIVE_ROUTE_WADO_SERIES_METADATA;
		return true;
	}
	if (count < 5 || strcmp(route[4], "instances") != 0) return false;
	if (count == 5) {
		match->kind = ARCHIVE_ROUTE_QIDO_INSTANCES;
		return true;
	}
	if (!dicom_uid_is_valid_string(route[5], strlen(route[5])) || !archive_copy_value(match->instance_uid,
			sizeof(match->instance_uid), route[5])) return false;
	if (count == 7 && strcmp(route[6], "metadata") == 0) {
		match->kind = ARCHIVE_ROUTE_WADO_INSTANCE_METADATA;
		return true;
	}
	if (count == 8 && strcmp(route[6], "frames") == 0 && archive_is_frame_list(route[7]) &&
	    archive_copy_value(match->frames, sizeof(match->frames), route[7])) {
		match->kind = ARCHIVE_ROUTE_WADO_FRAMES;
		return true;
	}
	return false;
}

const char* archive_route_kind_string(archive_route_kind_t kind) {
	switch (kind) {
		case ARCHIVE_ROUTE_HEALTH: return "health";
		case ARCHIVE_ROUTE_QIDO_STUDIES: return "qido-studies";
		case ARCHIVE_ROUTE_QIDO_SERIES: return "qido-series";
		case ARCHIVE_ROUTE_QIDO_INSTANCES: return "qido-instances";
		case ARCHIVE_ROUTE_WADO_SERIES_METADATA: return "wado-series-metadata";
		case ARCHIVE_ROUTE_WADO_INSTANCE_METADATA: return "wado-instance-metadata";
		case ARCHIVE_ROUTE_WADO_FRAMES: return "wado-frames";
		case ARCHIVE_ROUTE_STOW_STUDIES: return "stow-studies";
		default: return "unknown";
	}
}
