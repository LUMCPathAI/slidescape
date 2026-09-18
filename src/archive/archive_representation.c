/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_representation.h"

#include <stdlib.h>
#include <string.h>

static char archive_representation_ascii_lower(char value) {
	return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool archive_representation_value_equal(const char* lhs, const char* rhs) {
	if (!lhs || !rhs || strlen(lhs) != strlen(rhs)) return false;
	for (; *lhs; ++lhs, ++rhs) {
		if (archive_representation_ascii_lower(*lhs) !=
		    archive_representation_ascii_lower(*rhs)) return false;
	}
	return true;
}

static char* archive_representation_trim(char* value) {
	while (*value == ' ' || *value == '\t') ++value;
	char* end = value + strlen(value);
	while (end > value && (end[-1] == ' ' || end[-1] == '\t')) --end;
	*end = '\0';
	return value;
}

static char* archive_representation_unquote(char* value) {
	size_t length = strlen(value);
	if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
		value[length - 1] = '\0';
		return value + 1;
	}
	return value;
}

static i32 archive_representation_media_specificity(const char* requested,
		const char* available) {
	if (archive_representation_value_equal(requested, available)) return 2;
	if (archive_representation_value_equal(requested, "*/*")) return 0;
	const char* slash = strchr(requested, '/');
	const char* available_slash = strchr(available, '/');
	if (!slash || !available_slash || strcmp(slash, "/*") != 0) return -1;
	size_t prefix_length = (size_t)(slash - requested);
	if (prefix_length != (size_t)(available_slash - available)) return -1;
	for (size_t i = 0; i < prefix_length; ++i) {
		if (archive_representation_ascii_lower(requested[i]) !=
		    archive_representation_ascii_lower(available[i])) return -1;
	}
	return 1;
}

static char* archive_representation_next(char* value, char delimiter) {
	bool quoted = false;
	for (char* cursor = value; *cursor; ++cursor) {
		if (*cursor == '"') quoted = !quoted;
		if (!quoted && *cursor == delimiter) {
			*cursor = '\0';
			return cursor + 1;
		}
	}
	return NULL;
}

typedef struct archive_accept_range_t {
	char media_type[32];
	char transfer_syntax_uid[65];
	i32 quality; /* HTTP qvalue in thousandths, from 0 through 1000. */
} archive_accept_range_t;

static bool archive_representation_parse_quality(const char* value, i32* quality) {
	if (*value != '0' && *value != '1') return false;
	i32 result = (*value++ - '0') * 1000;
	if (*value == '.') {
		++value;
		for (i32 place = 100; *value && place > 0; place /= 10) {
			if (*value < '0' || *value > '9' || (result >= 1000 && *value != '0'))
				return false;
			result += (*value++ - '0') * place;
		}
	}
	if (*value) return false;
	*quality = result;
	return true;
}

archive_representation_selection_result_t archive_select_frame_representation(
		const char* accept, size_t accept_size,
		const archive_frame_representation_t* available, i32 available_count,
		archive_frame_representation_t* selected) {
	if (selected) memset(selected, 0, sizeof(*selected));
	if (!accept || accept_size == 0) return ARCHIVE_REPRESENTATION_MISSING_ACCEPT;
	if (!available || available_count <= 0 || !selected ||
	    accept_size > ARCHIVE_MAX_ACCEPT_HEADER_SIZE) {
		return ARCHIVE_REPRESENTATION_NOT_ACCEPTABLE;
	}
	char value[ARCHIVE_MAX_ACCEPT_HEADER_SIZE + 1];
	memcpy(value, accept, accept_size);
	value[accept_size] = '\0';

	archive_accept_range_t ranges[32] = {0};
	i32 range_count = 0;
	for (char* item = value; item && *item; ) {
		char* next_item = archive_representation_next(item, ',');
		char* parameter = archive_representation_next(item, ';');
		char* outer_type = archive_representation_trim(item);
		char root_type[32] = {0};
		char transfer_syntax_uid[65] = {0};
		i32 quality = 1000;
		bool malformed = !strchr(outer_type, '/');
		while (parameter && *parameter) {
			char* next_parameter = archive_representation_next(parameter, ';');
			char* field = archive_representation_trim(parameter);
			char* equals = strchr(field, '=');
			if (!equals) {
				malformed = true;
			} else {
				*equals++ = '\0';
				char* name = archive_representation_trim(field);
				char* parameter_value = archive_representation_trim(equals);
				bool quoted_value = parameter_value[0] == '"';
				parameter_value = archive_representation_unquote(parameter_value);
				if (archive_representation_value_equal(name, "type")) {
					if (strlen(parameter_value) >= sizeof(root_type)) {
						malformed = true;
					} else {
						memcpy(root_type, parameter_value, strlen(parameter_value) + 1);
					}
				} else if (archive_representation_value_equal(name, "transfer-syntax")) {
					if (strlen(parameter_value) >= sizeof(transfer_syntax_uid)) {
						malformed = true;
					} else {
						memcpy(transfer_syntax_uid, parameter_value,
						       strlen(parameter_value) + 1);
					}
				} else if (archive_representation_value_equal(name, "q")) {
					if (quoted_value || !archive_representation_parse_quality(parameter_value, &quality))
						malformed = true;
				}
			}
			parameter = next_parameter;
		}
		bool multipart = archive_representation_value_equal(outer_type, "multipart/related");
		if (multipart && !root_type[0]) malformed = true;
		const char* requested_type = multipart ? root_type : outer_type;
		if (strlen(requested_type) >= sizeof(ranges[0].media_type)) malformed = true;
		if (!malformed && range_count < COUNT(ranges)) {
			archive_accept_range_t* range = ranges + range_count++;
			memcpy(range->media_type, requested_type, strlen(requested_type) + 1);
			memcpy(range->transfer_syntax_uid, transfer_syntax_uid,
			       strlen(transfer_syntax_uid) + 1);
			range->quality = quality;
		}
		item = next_item;
	}

	bool have_selection = false;
	i32 selected_quality = 0;
	i32 selected_specificity = -1;
	i32 selected_preference = INT32_MIN;
	for (i32 candidate_index = 0; candidate_index < available_count; ++candidate_index) {
		const archive_frame_representation_t* candidate = available + candidate_index;
		i32 candidate_quality = -1;
		i32 candidate_specificity = -1;
		for (i32 range_index = 0; range_index < range_count; ++range_index) {
			const archive_accept_range_t* range = ranges + range_index;
			i32 media_specificity = archive_representation_media_specificity(
				range->media_type, candidate->media_type);
			if (media_specificity < 0) continue;
			bool syntax_matches = !range->transfer_syntax_uid[0] ||
				archive_representation_value_equal(range->transfer_syntax_uid, "*") ||
				strcmp(range->transfer_syntax_uid,
				       candidate->transfer_syntax_uid) == 0;
			if (!syntax_matches) continue;
			i32 specificity = media_specificity * 2 +
				(range->transfer_syntax_uid[0] &&
				 !archive_representation_value_equal(
					 range->transfer_syntax_uid, "*") ? 1 : 0);
			if (specificity > candidate_specificity ||
			    (specificity == candidate_specificity &&
			     range->quality > candidate_quality)) {
				candidate_specificity = specificity;
				candidate_quality = range->quality;
			}
		}
		if (candidate_quality <= 0) continue;
		if (!have_selection || candidate_quality > selected_quality ||
		    (candidate_quality == selected_quality &&
		     candidate_specificity > selected_specificity) ||
		    (candidate_quality == selected_quality &&
		     candidate_specificity == selected_specificity &&
		     candidate->preference > selected_preference)) {
			*selected = *candidate;
			selected_quality = candidate_quality;
			selected_specificity = candidate_specificity;
			selected_preference = candidate->preference;
			have_selection = true;
		}
	}
	return have_selection ? ARCHIVE_REPRESENTATION_SELECTED
	                      : ARCHIVE_REPRESENTATION_NOT_ACCEPTABLE;
}

const char* archive_frame_media_type_for_transfer_syntax(
		const char* transfer_syntax_uid) {
	if (!transfer_syntax_uid) return NULL;
	if (strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0) return "image/jpeg";
	if (strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.90") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.91") == 0) return "image/jp2";
	if (strcmp(transfer_syntax_uid, "1.2.840.10008.1.2") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.1") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.2") == 0) {
		return "application/octet-stream";
	}
	return NULL;
}
