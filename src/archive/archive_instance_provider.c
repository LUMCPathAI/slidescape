/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_instance_provider.h"

#include "archive_object.h"
#include "archive_mrxs_adapter.h"
#include "archive_sha256.h"
#include "archive_shell.h"
#include "archive_shell_recipe.h"
#include "dicom.h"
#include "jpeg_decoder.h"
#include "jpeg2000_decoder.h"

#include <math.h>
#include <stdarg.h>

#define ARCHIVE_PROVIDER_MAX_METADATA_SIZE MEGABYTES(256)
#define ARCHIVE_PROVIDER_MAX_FRAME_SIZE MEGABYTES(64)
#define ARCHIVE_PROVIDER_WORK_QUEUE_CAPACITY 1024
#define ARCHIVE_PROVIDER_CACHE_CAPACITY 16

typedef struct archive_provider_writer_t {
	char* data;
	size_t capacity;
	size_t used;
	bool failed;
} archive_provider_writer_t;

static void archive_provider_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_provider_owns_global_thread_pool;

typedef struct archive_provider_cache_entry_t {
	bool occupied;
	i32 refcount;
	u64 last_used;
	char repository_root[ARCHIVE_PATH_CAPACITY];
	char shell_key[sizeof(((archive_instance_record_t*)0)->shell_key)];
	u32 shell_instance_index;
	u8* shell_data;
	size_t shell_size;
	archive_shell_view_t shell;
	archive_shell_instance_t instance;
	file_handle_t source_file;
	archive_mrxs_adapter_t* mrxs;
	char* metadata_json;
	size_t metadata_size;
} archive_provider_cache_entry_t;

static platform_mutex_t archive_provider_cache_mutex = PLATFORM_MUTEX_INITIALIZER;
static platform_mutex_t archive_provider_regeneration_mutex = PLATFORM_MUTEX_INITIALIZER;
static archive_provider_cache_entry_t archive_provider_cache[ARCHIVE_PROVIDER_CACHE_CAPACITY];
static u64 archive_provider_cache_clock;
static archive_instance_provider_cache_stats_t archive_provider_cache_stats;

static void archive_provider_cache_entry_destroy(archive_provider_cache_entry_t* entry) {
	if (!entry || !entry->occupied) return;
	free(entry->metadata_json);
	free(entry->shell_data);
	file_handle_close(entry->source_file);
	if (entry->mrxs) {
		archive_mrxs_adapter_close(entry->mrxs);
		free(entry->mrxs);
	}
	memset(entry, 0, sizeof(*entry));
}

static void archive_provider_cache_destroy(void) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		ASSERT(archive_provider_cache[i].refcount == 0);
		archive_provider_cache_entry_destroy(archive_provider_cache + i);
	}
	archive_provider_cache_clock = 0;
	memset(&archive_provider_cache_stats, 0, sizeof(archive_provider_cache_stats));
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

archive_instance_provider_cache_stats_t archive_instance_provider_get_cache_stats(void) {
	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_instance_provider_cache_stats_t result = archive_provider_cache_stats;
	result.capacity = ARCHIVE_PROVIDER_CACHE_CAPACITY;
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		if (archive_provider_cache[i].occupied) ++result.resident_count;
	}
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return result;
}

bool archive_instance_provider_initialize(void) {
	/*
	 * Archive parsing and native adapters share Slidescape's worker/runtime contract:
	 * every request runs on a logical thread with a private temporary arena. Keep DICOM
	 * dictionary initialization ahead of worker dispatch; dicom_init() is idempotent,
	 * but its first initialization is deliberately serialized here on the main thread.
	 */
	if (!dicom_init()) return false;
	if (!global_thread_pool.initialized) {
		init_thread_pool(&global_thread_pool, ARCHIVE_PROVIDER_WORK_QUEUE_CAPACITY,
		                 true, false, NULL);
		archive_provider_owns_global_thread_pool = true;
	}
	return global_thread_pool.initialized && threadlocal_thread_memory != NULL;
}

void archive_instance_provider_shutdown(void) {
	if (archive_provider_owns_global_thread_pool) {
		thread_pool_destroy(&global_thread_pool);
		archive_provider_owns_global_thread_pool = false;
	}
	archive_provider_cache_destroy();
}

static bool archive_provider_integer_matches(const char* published, u64 parsed) {
	if (!published[0]) return true;
	char value[32];
	i32 length = snprintf(value, sizeof(value), "%llu", (unsigned long long)parsed);
	return length > 0 && (size_t)length < sizeof(value) && strcmp(value, published) == 0;
}

static void archive_provider_write(archive_provider_writer_t* writer, const char* text) {
	if (writer->failed) return;
	size_t length = strlen(text);
	if (length >= writer->capacity - writer->used) {
		writer->failed = true;
		return;
	}
	memcpy(writer->data + writer->used, text, length);
	writer->used += length;
	writer->data[writer->used] = '\0';
}

static void archive_provider_write_format(archive_provider_writer_t* writer, const char* format, ...) {
	if (writer->failed) return;
	va_list args;
	va_start(args, format);
	i32 length = vsnprintf(writer->data + writer->used, writer->capacity - writer->used, format, args);
	va_end(args);
	if (length < 0 || (size_t)length >= writer->capacity - writer->used) {
		writer->failed = true;
		return;
	}
	writer->used += (size_t)length;
}

static void archive_provider_write_json_string(
		archive_provider_writer_t* writer, const char* value) {
	static const char hex[] = "0123456789abcdef";
	archive_provider_write(writer, "\"");
	for (const u8* cursor = (const u8*)(value ? value : ""); *cursor; ++cursor) {
		char encoded[7] = {0};
		if (*cursor == '"' || *cursor == '\\') {
			encoded[0] = '\\';
			encoded[1] = (char)*cursor;
		} else if (*cursor < 0x20) {
			memcpy(encoded, "\\u00", 4);
			encoded[4] = hex[*cursor >> 4];
			encoded[5] = hex[*cursor & 15];
		} else {
			encoded[0] = (char)*cursor;
		}
		archive_provider_write(writer, encoded);
	}
	archive_provider_write(writer, "\"");
}

static void archive_provider_write_person_name(
		archive_provider_writer_t* writer, const char* value) {
	static const char* keys[] = {"Alphabetic", "Ideographic", "Phonetic"};
	archive_provider_write(writer, "{");
	bool wrote_group = false;
	const char* group = value;
	for (i32 i = 0; i < COUNT(keys) && group; ++i) {
		const char* separator = strchr(group, '=');
		size_t length = separator ? (size_t)(separator - group) : strlen(group);
		if (length) {
			if (wrote_group) archive_provider_write(writer, ",");
			archive_provider_write_format(writer, "\"%s\":", keys[i]);
			char name[321] = {0};
			length = MIN(length, sizeof(name) - 1);
			memcpy(name, group, length);
			archive_provider_write_json_string(writer, name);
			wrote_group = true;
		}
		group = separator ? separator + 1 : NULL;
	}
	archive_provider_write(writer, "}");
}

static void archive_provider_write_base64(
		archive_provider_writer_t* writer, const u8* data, size_t size) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	archive_provider_write(writer, "\"");
	for (size_t offset = 0; offset < size; offset += 3) {
		u32 value = (u32)data[offset] << 16;
		size_t remaining = size - offset;
		if (remaining > 1) value |= (u32)data[offset + 1] << 8;
		if (remaining > 2) value |= data[offset + 2];
		char encoded[5] = {
			alphabet[(value >> 18) & 63],
			alphabet[(value >> 12) & 63],
			remaining > 1 ? alphabet[(value >> 6) & 63] : '=',
			remaining > 2 ? alphabet[value & 63] : '=',
			'\0',
		};
		archive_provider_write(writer, encoded);
	}
	archive_provider_write(writer, "\"");
}

static void archive_provider_write_code_sequence(
		archive_provider_writer_t* writer, const char* tag,
		const char* code_value, const char* coding_scheme,
		const char* code_meaning, bool* has_attribute) {
	if (!code_value || !code_value[0] || !coding_scheme || !coding_scheme[0] ||
	    !code_meaning || !code_meaning[0]) return;
	if (*has_attribute) archive_provider_write(writer, ",");
	archive_provider_write_format(writer, "\"%s\":{\"vr\":\"SQ\",\"Value\":[{", tag);
	archive_provider_write(writer, "\"00080100\":{\"vr\":\"SH\",\"Value\":[");
	archive_provider_write_json_string(writer, code_value);
	archive_provider_write(writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_provider_write_json_string(writer, coding_scheme);
	archive_provider_write(writer, "]},\"00080104\":{\"vr\":\"LO\",\"Value\":[");
	archive_provider_write_json_string(writer, code_meaning);
	archive_provider_write(writer, "]}}]}");
	*has_attribute = true;
}

static void archive_provider_write_content_code_sequence(
		archive_provider_writer_t* writer, const char* tag,
		const char* code_value, const char* coding_scheme,
		const char* coding_scheme_version, const char* code_meaning) {
	archive_provider_write_format(
		writer, "\"%s\":{\"vr\":\"SQ\",\"Value\":[{"
		"\"00080100\":{\"vr\":\"SH\",\"Value\":[", tag);
	archive_provider_write_json_string(writer, code_value);
	archive_provider_write(
		writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_provider_write_json_string(writer, coding_scheme);
	archive_provider_write(writer, "]}");
	if (coding_scheme_version && coding_scheme_version[0]) {
		archive_provider_write(
			writer, ",\"00080103\":{\"vr\":\"SH\",\"Value\":[");
		archive_provider_write_json_string(
			writer, coding_scheme_version);
		archive_provider_write(writer, "]}");
	}
	if (code_meaning && code_meaning[0]) {
		archive_provider_write(
			writer, ",\"00080104\":{\"vr\":\"LO\",\"Value\":[");
		archive_provider_write_json_string(writer, code_meaning);
		archive_provider_write(writer, "]}");
	}
	archive_provider_write(writer, "}]}");
}

static bool archive_provider_write_preparation_content_item(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 item_index,
		archive_provider_writer_t* writer) {
	archive_shell_preparation_item_t item = {0};
	if (!archive_shell_get_preparation_item(
			shell, instance, item_index, &item)) return false;
	static const char* value_types[] = {
		"", "CODE", "TEXT", "NUM", "UIDREF",
		"DATETIME", "DATE", "TIME", "PNAME",
	};
	if ((u32)item.value_kind >= COUNT(value_types)) return false;
	archive_provider_write(
		writer, "{\"0040A040\":{\"vr\":\"CS\",\"Value\":[");
	archive_provider_write_json_string(
		writer, value_types[(u32)item.value_kind]);
	archive_provider_write(writer, "]}");
	if (item.relationship_type[0]) {
		archive_provider_write(
			writer, ",\"0040A010\":{\"vr\":\"CS\",\"Value\":[");
		archive_provider_write_json_string(
			writer, item.relationship_type);
		archive_provider_write(writer, "]}");
	}
	archive_provider_write(
		writer, ",\"0040A043\":{\"vr\":\"SQ\",\"Value\":[{");
	archive_provider_write(
		writer, "\"00080100\":{\"vr\":\"SH\",\"Value\":[");
	archive_provider_write_json_string(
		writer, item.concept_name_code_value);
	archive_provider_write(
		writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_provider_write_json_string(
		writer, item.concept_name_coding_scheme);
	archive_provider_write(writer, "]}");
	if (item.concept_name_coding_scheme_version[0]) {
		archive_provider_write(
			writer, ",\"00080103\":{\"vr\":\"SH\",\"Value\":[");
		archive_provider_write_json_string(
			writer, item.concept_name_coding_scheme_version);
		archive_provider_write(writer, "]}");
	}
	if (item.concept_name_code_meaning[0]) {
		archive_provider_write(
			writer, ",\"00080104\":{\"vr\":\"LO\",\"Value\":[");
		archive_provider_write_json_string(
			writer, item.concept_name_code_meaning);
		archive_provider_write(writer, "]}");
	}
	archive_provider_write(writer, "}]}");
	switch (item.value_kind) {
		case ARCHIVE_SHELL_CONTENT_CODE:
			archive_provider_write(writer, ",");
			archive_provider_write_content_code_sequence(
				writer, "0040A168", item.code_value,
				item.code_coding_scheme,
				item.code_coding_scheme_version, item.code_meaning);
			break;
		case ARCHIVE_SHELL_CONTENT_NUM: {
			char* end = NULL;
			double numeric_value = strtod(item.scalar_value, &end);
			if (!end || *end || !isfinite(numeric_value)) return false;
			archive_provider_write_format(
				writer,
				",\"0040A300\":{\"vr\":\"SQ\",\"Value\":[{"
				"\"0040A30A\":{\"vr\":\"DS\",\"Value\":[%.17g]}",
				numeric_value);
			if (item.measurement_units_code_value[0] &&
			    item.measurement_units_coding_scheme[0]) {
				archive_provider_write(writer, ",");
				archive_provider_write_content_code_sequence(
					writer, "004008EA",
					item.measurement_units_code_value,
					item.measurement_units_coding_scheme,
					item.measurement_units_coding_scheme_version,
					item.measurement_units_code_meaning);
			}
			archive_provider_write(writer, "}]}");
		} break;
		default: {
			const char* tag = NULL;
			const char* vr = NULL;
			switch (item.value_kind) {
				case ARCHIVE_SHELL_CONTENT_TEXT:
					tag = "0040A160"; vr = "UT"; break;
				case ARCHIVE_SHELL_CONTENT_UID:
					tag = "0040A124"; vr = "UI"; break;
				case ARCHIVE_SHELL_CONTENT_DATETIME:
					tag = "0040A120"; vr = "DT"; break;
				case ARCHIVE_SHELL_CONTENT_DATE:
					tag = "0040A121"; vr = "DA"; break;
				case ARCHIVE_SHELL_CONTENT_TIME:
					tag = "0040A122"; vr = "TM"; break;
				case ARCHIVE_SHELL_CONTENT_PNAME:
					tag = "0040A123"; vr = "PN"; break;
				default:
					return false;
			}
			archive_provider_write_format(
				writer, ",\"%s\":{\"vr\":\"%s\",\"Value\":[",
				tag, vr);
			if (item.value_kind == ARCHIVE_SHELL_CONTENT_PNAME)
				archive_provider_write_person_name(
					writer, item.scalar_value);
			else
				archive_provider_write_json_string(
					writer, item.scalar_value);
			archive_provider_write(writer, "]}");
		} break;
	}
	u32 item_count =
		archive_shell_get_preparation_item_count(shell, instance);
	bool has_child = false;
	for (u32 child_index = item_index + 1;
	     child_index < item_count; ++child_index) {
		archive_shell_preparation_item_t child = {0};
		if (!archive_shell_get_preparation_item(
				shell, instance, child_index, &child)) return false;
		if (child.parent_item_index != item_index) continue;
		archive_provider_write(
			writer, has_child ? "," :
			",\"0040A730\":{\"vr\":\"SQ\",\"Value\":[");
		if (!archive_provider_write_preparation_content_item(
				shell, instance, child_index, writer)) return false;
		has_child = true;
	}
	if (has_child) archive_provider_write(writer, "]}");
	archive_provider_write(writer, "}");
	return !writer->failed;
}

static bool archive_provider_write_shell_metadata(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		char* dest, size_t capacity, size_t* size, bool array_wrapper) {
	archive_provider_writer_t writer = {dest, capacity, 0, false};
	archive_provider_write(&writer, array_wrapper ? "[{" : "{");
	u32 attribute_count = archive_shell_get_attribute_count(shell, instance);
	bool has_attribute = false;
	for (u32 i = 0; i < attribute_count; ++i) {
		archive_shell_attribute_t attribute = {0};
		if (!archive_shell_get_attribute(shell, instance, i, &attribute)) return false;
		if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_STRING &&
		    !attribute.string_value[0]) continue;
		if (has_attribute) archive_provider_write(&writer, ",");
		archive_provider_write_format(&writer, "\"%08X\":{\"vr\":\"%s\",\"Value\":[",
		                              attribute.tag, attribute.vr);
		if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_STRING) {
			if (strcmp(attribute.vr, "PN") == 0)
				archive_provider_write_person_name(&writer, attribute.string_value);
			else
				archive_provider_write_json_string(&writer, attribute.string_value);
		} else if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_SIGNED) {
			if (strcmp(attribute.vr, "IS") == 0)
				archive_provider_write_format(&writer, "\"%lld\"",
				                              (long long)attribute.signed_value);
			else
				archive_provider_write_format(&writer, "%lld",
				                              (long long)attribute.signed_value);
		} else if (strcmp(attribute.vr, "IS") == 0) {
			archive_provider_write_format(&writer, "\"%llu\"",
			                              (unsigned long long)attribute.unsigned_value);
		} else {
			archive_provider_write_format(&writer, "%llu",
			                              (unsigned long long)attribute.unsigned_value);
		}
		archive_provider_write(&writer, "]}");
		has_attribute = true;
	}
	if (instance->image_flavor[0]) {
		/*
		 * Shell v1 retains the governed flavor but not the complete source
		 * ImageType tuple. Publish the stable values required by the WSI
		 * information model; exact ORIGINAL/DERIVED and RESAMPLED provenance
		 * will become typed shell fields rather than trigger source reparsing.
		 */
		archive_provider_write_format(&writer,
			"%s\"00080008\":{\"vr\":\"CS\",\"Value\":[\"ORIGINAL\",\"PRIMARY\",\"%s\"]}",
			has_attribute ? "," : "",
			instance->image_flavor);
		has_attribute = true;
	}
	if (has_attribute) archive_provider_write(&writer, ",");
	archive_provider_write(&writer,
		"\"00480105\":{\"vr\":\"SQ\",\"Value\":[");
	for (u32 i = 0; i < instance->optical_path_count; ++i) {
		archive_shell_optical_path_t optical_path = {0};
		if (!archive_shell_get_optical_path(shell, instance, i, &optical_path))
			return false;
		if (i) archive_provider_write(&writer, ",");
		archive_provider_write(&writer,
			"{\"00480106\":{\"vr\":\"SH\",\"Value\":[");
		archive_provider_write_json_string(&writer, optical_path.identifier);
		archive_provider_write(&writer, "]}");
		bool has_attribute = true;
		if (optical_path.description && optical_path.description[0]) {
			archive_provider_write(&writer,
				",\"00480107\":{\"vr\":\"ST\",\"Value\":[");
			archive_provider_write_json_string(&writer, optical_path.description);
			archive_provider_write(&writer, "]}");
		}
		if (optical_path.color_space && optical_path.color_space[0]) {
			archive_provider_write(&writer,
				",\"00282002\":{\"vr\":\"CS\",\"Value\":[");
			archive_provider_write_json_string(&writer, optical_path.color_space);
			archive_provider_write(&writer, "]}");
		}
		if (optical_path.illumination_wavelength > 0.0f)
			archive_provider_write_format(
				&writer, ",\"00220055\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.illumination_wavelength);
		archive_provider_write_code_sequence(
			&writer, "00220016",
			optical_path.illumination_type_code_value,
			optical_path.illumination_type_coding_scheme,
			optical_path.illumination_type_code_meaning, &has_attribute);
		archive_provider_write_code_sequence(
			&writer, "00480108",
			optical_path.illumination_color_code_value,
			optical_path.illumination_color_coding_scheme,
			optical_path.illumination_color_code_meaning, &has_attribute);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_PASS)
			archive_provider_write_format(
				&writer, ",\"00220001\":{\"vr\":\"US\",\"Value\":[%u]}",
				optical_path.light_path_filter_pass_through_wavelength);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_BAND)
			archive_provider_write_format(
				&writer, ",\"00220002\":{\"vr\":\"US\",\"Value\":[%u,%u]}",
				optical_path.light_path_filter_pass_band[0],
				optical_path.light_path_filter_pass_band[1]);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_PASS)
			archive_provider_write_format(
				&writer, ",\"00220003\":{\"vr\":\"US\",\"Value\":[%u]}",
				optical_path.image_path_filter_pass_through_wavelength);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_BAND)
			archive_provider_write_format(
				&writer, ",\"00220004\":{\"vr\":\"US\",\"Value\":[%u,%u]}",
				optical_path.image_path_filter_pass_band[0],
				optical_path.image_path_filter_pass_band[1]);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_OBJECTIVE_POWER &&
		    isfinite(optical_path.objective_lens_power))
			archive_provider_write_format(
				&writer, ",\"00480112\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.objective_lens_power);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_OBJECTIVE_APERTURE &&
		    isfinite(optical_path.objective_lens_numerical_aperture))
			archive_provider_write_format(
				&writer, ",\"00480113\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.objective_lens_numerical_aperture);
		if (optical_path.icc_profile_size) {
			archive_provider_write(
				&writer, ",\"00282000\":{\"vr\":\"OB\",\"InlineBinary\":");
			archive_provider_write_base64(
				&writer, optical_path.icc_profile,
				(size_t)optical_path.icc_profile_size);
			archive_provider_write(&writer, "}");
		}
		archive_provider_write(&writer, "}");
	}
	archive_provider_write(&writer, "]}");
	const char* container_identifier =
		archive_shell_get_container_identifier(shell, instance);
	if (!container_identifier) return false;
	if (container_identifier[0]) {
		archive_provider_write(
			&writer, ",\"00400512\":{\"vr\":\"LO\",\"Value\":[");
		archive_provider_write_json_string(&writer, container_identifier);
		archive_provider_write(&writer, "]}");
	}
	u32 specimen_count = archive_shell_get_specimen_count(shell, instance);
	if (specimen_count) {
		archive_provider_write(
			&writer, ",\"00400560\":{\"vr\":\"SQ\",\"Value\":[");
		for (u32 i = 0; i < specimen_count; ++i) {
			archive_shell_specimen_t specimen = {0};
			if (!archive_shell_get_specimen(shell, instance, i, &specimen))
				return false;
			if (i) archive_provider_write(&writer, ",");
			archive_provider_write(&writer, "{");
			bool has_value = false;
			if (specimen.identifier[0]) {
				archive_provider_write(
					&writer, "\"00400551\":{\"vr\":\"LO\",\"Value\":[");
				archive_provider_write_json_string(&writer, specimen.identifier);
				archive_provider_write(&writer, "]}");
				has_value = true;
			}
			if (specimen.uid[0]) {
				if (has_value) archive_provider_write(&writer, ",");
				archive_provider_write(
					&writer, "\"00400554\":{\"vr\":\"UI\",\"Value\":[");
				archive_provider_write_json_string(&writer, specimen.uid);
				archive_provider_write(&writer, "]}");
				has_value = true;
			}
			if (specimen.short_description[0]) {
				if (has_value) archive_provider_write(&writer, ",");
				archive_provider_write(
					&writer, "\"00400600\":{\"vr\":\"LO\",\"Value\":[");
				archive_provider_write_json_string(
					&writer, specimen.short_description);
				archive_provider_write(&writer, "]}");
				has_value = true;
			}
			u32 preparation_count =
				archive_shell_get_preparation_item_count(shell, instance);
			u32 maximum_step = 0;
			bool has_preparation = false;
			for (u32 item_index = 0;
			     item_index < preparation_count; ++item_index) {
				archive_shell_preparation_item_t item = {0};
				if (!archive_shell_get_preparation_item(
						shell, instance, item_index, &item))
					return false;
				if (item.specimen_index == i) {
					has_preparation = true;
					maximum_step = MAX(maximum_step, item.step_index);
				}
			}
			if (has_preparation) {
				if (has_value) archive_provider_write(&writer, ",");
				archive_provider_write(
					&writer,
					"\"00400610\":{\"vr\":\"SQ\",\"Value\":[");
				bool wrote_step = false;
				for (u32 step_index = 0;
				     step_index <= maximum_step; ++step_index) {
					bool has_step_item = false;
					for (u32 item_index = 0;
					     item_index < preparation_count; ++item_index) {
						archive_shell_preparation_item_t item = {0};
						if (!archive_shell_get_preparation_item(
								shell, instance, item_index, &item))
							return false;
						if (item.specimen_index == i &&
						    item.step_index == step_index &&
						    item.parent_item_index == UINT32_MAX) {
							if (!has_step_item) {
								archive_provider_write(
									&writer,
									wrote_step ? ",{\"00400612\":"
									"{\"vr\":\"SQ\",\"Value\":["
									: "{\"00400612\":{\"vr\":"
									"\"SQ\",\"Value\":[");
							} else {
								archive_provider_write(&writer, ",");
							}
							if (!archive_provider_write_preparation_content_item(
									shell, instance, item_index,
									&writer))
								return false;
							has_step_item = true;
						}
					}
					if (has_step_item) {
						archive_provider_write(&writer, "]}}");
						wrote_step = true;
					}
				}
				archive_provider_write(&writer, "]}");
			}
			archive_provider_write(&writer, "}");
		}
		archive_provider_write(&writer, "]}");
	}
	if (instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID)
		archive_provider_write(&writer,
			",\"00209311\":{\"vr\":\"CS\",\"Value\":[\"TILED_FULL\"]}");
	if (instance->pixel_spacing_row_mm > 0.0f &&
	    instance->pixel_spacing_column_mm > 0.0f) {
		archive_provider_write_format(&writer,
			",\"52009229\":{\"vr\":\"SQ\",\"Value\":[{\"00289110\":{\"vr\":\"SQ\","
			"\"Value\":[{\"00280030\":{\"vr\":\"DS\",\"Value\":[%g,%g]}}]}}]}",
			instance->pixel_spacing_row_mm, instance->pixel_spacing_column_mm);
	}
	if (instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_EXPLICIT) {
		archive_provider_write(&writer, ",\"52009230\":{\"vr\":\"SQ\",\"Value\":[");
		for (u32 frame_index = 0; frame_index < instance->frame_count; ++frame_index) {
			archive_shell_frame_t frame = {0};
			if (!archive_shell_get_frame(shell, instance, frame_index, &frame)) return false;
			if (frame_index) archive_provider_write(&writer, ",");
			archive_provider_write_format(&writer,
				"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{"
				"\"0048021E\":{\"vr\":\"SL\",\"Value\":[%u]},"
				"\"0048021F\":{\"vr\":\"SL\",\"Value\":[%u]}}]}}",
				frame.column_position, frame.row_position);
		}
		archive_provider_write(&writer, "]}");
	}
	archive_provider_write(&writer, array_wrapper ? "}]\n" : "}");
	if (writer.failed) return false;
	*size = writer.used;
	return true;
}

static bool archive_provider_record_matches_shell(
		const archive_instance_record_t* record,
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		char* error, size_t error_capacity) {
	archive_shell_source_t source = {0};
	char source_sha256[65];
	char representation_fingerprint[65];
	if (!archive_shell_get_source(shell, instance->source_index, &source)) return false;
	archive_sha256_hex(source.sha256, source_sha256);
	archive_sha256_hex(shell->representation_fingerprint,
	                   representation_fingerprint);
	if (strcmp(record->study_instance_uid, instance->study_instance_uid) != 0 ||
	    strcmp(record->series_instance_uid, instance->series_instance_uid) != 0 ||
	    strcmp(record->sop_instance_uid, instance->sop_instance_uid) != 0 ||
	    strcmp(record->sop_class_uid, instance->sop_class_uid) != 0 ||
	    strcmp(record->representation_fingerprint,
	           representation_fingerprint) != 0 ||
	    strcmp(record->transfer_syntax_uid, instance->transfer_syntax_uid) != 0 ||
	    strcmp(record->image_flavor, instance->image_flavor) != 0 ||
	    strcmp(record->object_sha256, source_sha256) != 0 ||
	    record->object_byte_size != source.byte_size ||
	    !archive_provider_integer_matches(
		    record->instance_number, (u64)instance->instance_number) ||
	    !archive_provider_integer_matches(record->number_of_frames, instance->frame_count) ||
	    !archive_provider_integer_matches(record->total_pixel_matrix_columns,
	                                      instance->total_pixel_matrix_columns) ||
	    !archive_provider_integer_matches(record->total_pixel_matrix_rows,
	                                      instance->total_pixel_matrix_rows)) {
		archive_provider_set_error(error, error_capacity,
		                           "virtual DICOM shell does not match its published index record");
		return false;
	}
	return true;
}

static bool archive_provider_hash_file(const char* path, char sha256[65], u64* byte_size) {
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	archive_sha256_t sha;
	archive_sha256_initialize(&sha);
	u8 buffer[KILOBYTES(64)];
	bool success = true;
	while (true) {
		size_t read = fread(buffer, 1, sizeof(buffer), file);
		if (read && !archive_sha256_update(&sha, buffer, read)) {
			success = false;
			break;
		}
		if (read < sizeof(buffer)) {
			if (ferror(file)) success = false;
			break;
		}
	}
	if (fclose(file) != 0) success = false;
	if (!success) return false;
	u8 digest[32];
	*byte_size = sha.byte_count;
	archive_sha256_finish(&sha, digest);
	archive_sha256_hex(digest, sha256);
	return true;
}

static bool archive_provider_path_is_external_reference(const char* path) {
#if defined(_WIN32)
	DWORD attributes = GetFileAttributesA(path);
	return attributes != INVALID_FILE_ATTRIBUTES &&
	       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	struct stat info = {0};
	return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
#endif
}

static bool archive_provider_read_exact_file(const char* path, u64 expected_size,
		u64 maximum_size, u8** data) {
	*data = NULL;
	if (!path || expected_size == 0 || expected_size > maximum_size ||
	    expected_size > SIZE_MAX) return false;
	FILE* file = fopen(path, "rb");
	u8* result = malloc((size_t)expected_size);
	bool success = file && result &&
	               fread(result, 1, (size_t)expected_size, file) ==
	                   (size_t)expected_size &&
	               fgetc(file) == EOF && !ferror(file);
	if (file && fclose(file) != 0) success = false;
	if (!success) {
		free(result);
		return false;
	}
	*data = result;
	return true;
}

static bool archive_provider_get_metadata_capacity(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u64* capacity) {
	if (!shell || !instance || !capacity) return false;
	u64 result = KILOBYTES(4);
	if (instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_EXPLICIT) {
		if ((u64)instance->frame_count > UINT64_MAX / 192) return false;
		u64 frame_capacity = (u64)instance->frame_count * 192;
		if (result > UINT64_MAX - frame_capacity) return false;
		result += frame_capacity;
	}
	for (u32 i = 0; i < instance->optical_path_count; ++i) {
		archive_shell_optical_path_t optical_path = {0};
		if (!archive_shell_get_optical_path(
				shell, instance, i, &optical_path)) return false;
		if (optical_path.icc_profile_size > UINT64_MAX - 2) return false;
		u64 groups = (optical_path.icc_profile_size + 2) / 3;
		if (groups > UINT64_MAX / 4) return false;
		u64 encoded_icc_size = groups * 4;
		if (encoded_icc_size > UINT64_MAX - KILOBYTES(2) ||
		    result > UINT64_MAX - encoded_icc_size - KILOBYTES(2))
			return false;
		result += encoded_icc_size + KILOBYTES(2);
	}
	u64 specimen_count =
		archive_shell_get_specimen_count(shell, instance);
	if (specimen_count > UINT64_MAX / 512 ||
	    result > UINT64_MAX - specimen_count * 512)
		return false;
	result += specimen_count * 512;
	u32 preparation_count =
		archive_shell_get_preparation_item_count(shell, instance);
	for (u32 i = 0; i < preparation_count; ++i) {
		archive_shell_preparation_item_t item = {0};
		if (!archive_shell_get_preparation_item(
				shell, instance, i, &item)) return false;
		const char* values[] = {
			item.concept_name_code_value,
			item.concept_name_coding_scheme,
			item.concept_name_coding_scheme_version,
			item.concept_name_code_meaning,
			item.code_value,
			item.code_coding_scheme,
			item.code_coding_scheme_version,
			item.code_meaning,
			item.scalar_value,
			item.measurement_units_code_value,
			item.measurement_units_coding_scheme,
			item.measurement_units_coding_scheme_version,
			item.measurement_units_code_meaning,
			item.relationship_type,
		};
		u64 item_capacity = KILOBYTES(1);
		for (i32 field = 0; field < COUNT(values); ++field) {
			size_t length = strlen(values[field] ? values[field] : "");
			if (item_capacity > UINT64_MAX - length) return false;
			item_capacity += length;
		}
		if (result > UINT64_MAX - item_capacity) return false;
		result += item_capacity;
	}
	if (result > ARCHIVE_PROVIDER_MAX_METADATA_SIZE || result > SIZE_MAX)
		return false;
	*capacity = result;
	return true;
}

static char* archive_provider_recipe_next_line(char** cursor) {
	if (!cursor || !*cursor || !**cursor) return NULL;
	char* line = *cursor;
	char* newline = strchr(line, '\n');
	if (!newline) {
		*cursor = line + strlen(line);
		return line;
	}
	*newline = '\0';
	*cursor = newline + 1;
	return line;
}

static const char* archive_provider_recipe_value(
		char* line, const char* prefix) {
	size_t length = strlen(prefix);
	return line && strncmp(line, prefix, length) == 0 ?
	       line + length : NULL;
}

static bool archive_provider_regenerate_aggregate_shell(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		const u8* retained_recipe, size_t retained_recipe_size,
		char* error, size_t error_capacity) {
	char* text = malloc(retained_recipe_size + 1);
	if (!text) return false;
	memcpy(text, retained_recipe, retained_recipe_size);
	text[retained_recipe_size] = '\0';
	char* cursor = text;
	char* line = archive_provider_recipe_next_line(&cursor);
	bool success = line &&
		strcmp(line, "slidescape-virtual-dicom-recipe-v6") == 0;
	const char* bundle_sha256 = NULL;
	const char* study_uid = NULL;
	const char* series_uid = NULL;
	u32 instance_count = 0;
	if (success) {
		bundle_sha256 = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor), "bundle-sha256\t");
		study_uid = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor),
			"logical-slide-study-uid\t");
		series_uid = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor),
			"logical-slide-series-uid\t");
		const char* count_text = archive_provider_recipe_value(
			archive_provider_recipe_next_line(&cursor), "instance-count\t");
		char* count_end = NULL;
		unsigned long parsed_count = count_text ?
			strtoul(count_text, &count_end, 10) : 0;
		success = bundle_sha256 && study_uid && series_uid && count_text &&
		          count_end && !*count_end && parsed_count > 0 &&
		          parsed_count <= ARCHIVE_SHELL_MAX_INSTANCES;
		instance_count = success ? (u32)parsed_count : 0;
	}
	archive_shell_part10_recipe_input_t* recipe_instances =
		success ? calloc(instance_count, sizeof(*recipe_instances)) : NULL;
	dicom_series_t* dicoms =
		success ? calloc(instance_count, sizeof(*dicoms)) : NULL;
	archive_shell_build_part10_input_t* shell_instances =
		success ? calloc(instance_count, sizeof(*shell_instances)) : NULL;
	char (*source_sha256)[65] =
		success ? calloc(instance_count, sizeof(*source_sha256)) : NULL;
	char (*sop_instance_uid)[65] =
		success ? calloc(instance_count, sizeof(*sop_instance_uid)) : NULL;
	success = success && recipe_instances && dicoms && shell_instances &&
	          source_sha256 && sop_instance_uid;
	bool record_found = false;
	for (u32 i = 0; i < instance_count && success; ++i) {
		line = archive_provider_recipe_next_line(&cursor);
		unsigned int ordinal = 0;
		unsigned long long byte_size = 0;
		char extra = '\0';
		int fields = line ? sscanf(
			line, "instance\t%u\t%64[0-9.]\t%64[0-9a-f]\t%llu%c",
			&ordinal, sop_instance_uid[i], source_sha256[i],
			&byte_size, &extra) : 0;
		if (fields != 4 || ordinal != i || !byte_size) {
			success = false;
			break;
		}
		char storage_key[257];
		char source_path[ARCHIVE_PATH_CAPACITY];
		i32 key_length = snprintf(
			storage_key, sizeof(storage_key),
			"objects/sha256/%.2s/%s", source_sha256[i], source_sha256[i]);
		char actual_sha256[65];
		u64 actual_size = 0;
		success = key_length > 0 &&
		          (size_t)key_length < sizeof(storage_key) &&
		          archive_repository_make_path(
			          repository, storage_key, source_path,
			          sizeof(source_path)) &&
		          archive_provider_hash_file(
			          source_path, actual_sha256, &actual_size) &&
		          actual_size == (u64)byte_size &&
		          strcmp(actual_sha256, source_sha256[i]) == 0 &&
		          dicom_open_single_instance_from_path(
			          dicoms + i, source_path) &&
		          arrlen(dicoms[i].instances) == 1 &&
		          strcmp(dicoms[i].instances->sop_instance_uid.value,
			                 sop_instance_uid[i]) == 0;
		if (!success) break;
		recipe_instances[i] = (archive_shell_part10_recipe_input_t){
			.bundle_sha256 = bundle_sha256,
			.source_sha256 = source_sha256[i],
			.source_byte_size = (u64)byte_size,
			.sop_instance_uid = sop_instance_uid[i],
		};
		shell_instances[i] = (archive_shell_build_part10_input_t){
			.instance = dicoms[i].instances,
			.source_sha256 = source_sha256[i],
			.source_byte_size = (u64)byte_size,
		};
		if (strcmp(record->sop_instance_uid, sop_instance_uid[i]) == 0) {
			record_found = record->shell_instance_index == i &&
			               strcmp(record->object_sha256,
			                      source_sha256[i]) == 0 &&
			               record->object_byte_size == (u64)byte_size;
		}
	}
	archive_shell_part10_aggregate_recipe_input_t recipe_input = {
		.bundle_sha256 = bundle_sha256,
		.study_instance_uid = study_uid,
		.series_instance_uid = series_uid,
		.instances = recipe_instances,
		.instance_count = instance_count,
	};
	char* expected_recipe = NULL;
	size_t expected_recipe_size = 0;
	char expected_fingerprint[65];
	if (success) {
		success = record_found &&
		          strcmp(record->bundle_sha256, bundle_sha256) == 0 &&
		          strcmp(record->study_instance_uid, study_uid) == 0 &&
		          strcmp(record->series_instance_uid, series_uid) == 0 &&
		          archive_shell_build_part10_aggregate_recipe(
			          &recipe_input, &expected_recipe, &expected_recipe_size,
			          expected_fingerprint) &&
		          expected_recipe_size == retained_recipe_size &&
		          memcmp(expected_recipe, retained_recipe,
		                 retained_recipe_size) == 0 &&
		          strcmp(expected_fingerprint,
		                 record->representation_fingerprint) == 0;
	}
	archive_shell_buffer_t regenerated = {0};
	if (success) {
		archive_shell_build_part10_aggregate_input_t shell_input = {
			.instances = shell_instances,
			.instance_count = instance_count,
			.representation_fingerprint = expected_fingerprint,
		};
		success = archive_shell_build_part10_aggregate(
			&shell_input, &regenerated, error, error_capacity);
	}
	char regenerated_sha256[65];
	if (success) {
		success = regenerated.size == record->shell_byte_size &&
		          archive_sha256_bytes(
			          regenerated.data, regenerated.size,
			          regenerated_sha256) &&
		          strcmp(regenerated_sha256, record->shell_sha256) == 0 &&
		          archive_object_repair_shell_bytes(
			          repository, regenerated.data, regenerated.size,
			          record->shell_sha256, record->shell_key,
			          error, error_capacity) == ARCHIVE_OBJECT_OK;
	}
	if (!success && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not regenerate aggregate virtual DICOM shell '%s'",
			record->shell_key);
	archive_shell_buffer_destroy(&regenerated);
	free(expected_recipe);
	if (dicoms) {
		for (u32 i = 0; i < instance_count; ++i)
			dicom_destroy(dicoms + i);
	}
	free(sop_instance_uid);
	free(source_sha256);
	free(shell_instances);
	free(dicoms);
	free(recipe_instances);
	free(text);
	return success;
}

static bool archive_provider_load_published_shell(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		u8** shell_data, archive_shell_view_t* shell,
		archive_shell_instance_t* instance, char* error, size_t error_capacity) {
	char shell_path[ARCHIVE_PATH_CAPACITY];
	u8* data = NULL;
	if (!archive_repository_make_path(repository, record->shell_key,
	                                  shell_path, sizeof(shell_path)) ||
	    !archive_provider_read_exact_file(shell_path, record->shell_byte_size,
	                                      SIZE_MAX, &data)) {
		archive_provider_set_error(error, error_capacity,
		                           "could not read virtual DICOM shell '%s'",
		                           record->shell_key);
		return false;
	}
	char shell_sha256[65];
	bool success =
		archive_sha256_bytes(data, (size_t)record->shell_byte_size, shell_sha256) &&
		strcmp(shell_sha256, record->shell_sha256) == 0 &&
		archive_shell_open(shell, data, (size_t)record->shell_byte_size,
		                   error, error_capacity) &&
		archive_shell_get_instance(shell, record->shell_instance_index, instance) &&
		archive_provider_record_matches_shell(record, shell, instance,
		                                     error, error_capacity);
	if (!success) {
		free(data);
		if (!error || !error[0])
			archive_provider_set_error(error, error_capacity,
			                           "virtual DICOM shell '%s' failed validation",
			                           record->shell_key);
		return false;
	}
	*shell_data = data;
	return true;
}

static bool archive_provider_regenerate_shell(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	char source_path[ARCHIVE_PATH_CAPACITY];
	char recipe_path[ARCHIVE_PATH_CAPACITY];
	u8* retained_recipe = NULL;
	size_t retained_recipe_size = 0;
	char expected_recipe[ARCHIVE_SHELL_RECIPE_CAPACITY];
	size_t expected_recipe_size = 0;
	char expected_fingerprint[65];
	char retained_recipe_sha256[65];
	archive_shell_buffer_t regenerated = {0};
	dicom_series_t dicom = {0};
	bool success = archive_repository_make_path(
		repository, record->object_key, source_path, sizeof(source_path));
	if (success && record->recipe_key[0]) {
		success =
			archive_repository_make_path(repository, record->recipe_key,
			                             recipe_path, sizeof(recipe_path)) &&
			archive_provider_read_exact_file(recipe_path, record->recipe_byte_size,
			                                 SIZE_MAX,
			                                 &retained_recipe);
		retained_recipe_size = success ? (size_t)record->recipe_byte_size : 0;
	} else if (success) {
		success = archive_object_read_inline_bytes(
			repository, record->recipe_sha256, record->recipe_byte_size,
			ARCHIVE_SHELL_RECIPE_MEDIA_TYPE, SIZE_MAX,
			&retained_recipe, &retained_recipe_size,
			error, error_capacity) == ARCHIVE_OBJECT_OK;
	}
	success = success &&
		retained_recipe_size == record->recipe_byte_size &&
		archive_sha256_bytes(retained_recipe, (size_t)record->recipe_byte_size,
		                     retained_recipe_sha256) &&
		strcmp(retained_recipe_sha256, record->recipe_sha256) == 0;
	if (success && retained_recipe_size >=
	    strlen("slidescape-virtual-dicom-recipe-v6\n") &&
	    memcmp(retained_recipe, "slidescape-virtual-dicom-recipe-v6\n",
	           strlen("slidescape-virtual-dicom-recipe-v6\n")) == 0) {
		success = archive_provider_regenerate_aggregate_shell(
			repository, record, retained_recipe, retained_recipe_size,
			error, error_capacity);
		dicom_destroy(&dicom);
		free(retained_recipe);
		return success;
	}
	archive_shell_part10_recipe_input_t recipe_input = {
		.bundle_sha256 = record->bundle_sha256,
		.source_sha256 = record->object_sha256,
		.source_byte_size = record->object_byte_size,
		.sop_instance_uid = record->sop_instance_uid,
	};
	if (success)
		success = archive_shell_build_part10_recipe(
			          &recipe_input, expected_recipe, &expected_recipe_size,
			          expected_fingerprint) &&
		          expected_recipe_size == record->recipe_byte_size &&
		          memcmp(expected_recipe, retained_recipe, expected_recipe_size) == 0 &&
		          strcmp(expected_fingerprint,
		                 record->representation_fingerprint) == 0;
	char source_sha256[65];
	u64 source_size = 0;
	if (success)
		success = archive_provider_hash_file(source_path, source_sha256, &source_size) &&
		          source_size == record->object_byte_size &&
		          strcmp(source_sha256, record->object_sha256) == 0;
	if (success)
		success = dicom_open_single_instance_from_path(&dicom, source_path) &&
		          arrlen(dicom.instances) == 1 &&
		          strcmp(dicom.instances->sop_instance_uid.value,
		                 record->sop_instance_uid) == 0;
	if (success) {
		archive_shell_build_part10_input_t shell_input = {
			.instance = dicom.instances,
			.source_sha256 = record->object_sha256,
			.source_byte_size = record->object_byte_size,
			.representation_fingerprint = record->representation_fingerprint,
		};
		success = archive_shell_build_part10(&shell_input, &regenerated,
		                                    error, error_capacity);
	}
	char regenerated_sha256[65];
	if (success)
		success = regenerated.size == record->shell_byte_size &&
		          archive_sha256_bytes(regenerated.data, regenerated.size,
		                               regenerated_sha256) &&
		          strcmp(regenerated_sha256, record->shell_sha256) == 0;
	if (success)
		success = archive_object_repair_shell_bytes(
			          repository, regenerated.data, regenerated.size,
			          record->shell_sha256, record->shell_key,
			          error, error_capacity) == ARCHIVE_OBJECT_OK;
	if (!success && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not regenerate virtual DICOM shell '%s' from its retained recipe",
			record->shell_key);
	archive_shell_buffer_destroy(&regenerated);
	dicom_destroy(&dicom);
	free(retained_recipe);
	return success;
}

static archive_mrxs_adapter_t* archive_provider_open_mrxs(
		const archive_repository_t* repository,
		const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	u8* recipe = NULL;
	size_t recipe_size = 0;
	if (archive_object_read_inline_bytes(
			repository, record->recipe_sha256, record->recipe_byte_size,
			ARCHIVE_MRXS_RECIPE_MEDIA_TYPE, KILOBYTES(16),
			&recipe, &recipe_size, error, error_capacity) != ARCHIVE_OBJECT_OK)
		return NULL;
	char* text = malloc(recipe_size + 1);
	if (!text) {
		free(recipe);
		return NULL;
	}
	memcpy(text, recipe, recipe_size);
	text[recipe_size] = '\0';
	free(recipe);
	char descriptor_sha256[65] = {0};
	char fingerprint[65] = {0};
	char source_path[ARCHIVE_PATH_CAPACITY] = {0};
	unsigned long long descriptor_size = 0;
	char extra = '\0';
	int fields = sscanf(
		text,
		"slidescape-mrxs-native-recipe-v2\n"
		"adapter\tslidescape-mrxs\t2\n"
		"bundle-descriptor-sha256\t%64[0-9a-f]\n"
		"bundle-descriptor-size\t%llu\n"
		"representation-fingerprint\t%64[0-9a-f]\n"
		"source-path\t%1023[^\n]\n%c",
		descriptor_sha256, &descriptor_size, fingerprint,
		source_path, &extra);
	bool valid = fields == 4 &&
		descriptor_size == record->object_byte_size &&
		strcmp(descriptor_sha256, record->object_sha256) == 0 &&
		strcmp(fingerprint, record->representation_fingerprint) == 0;
	archive_mrxs_adapter_t* adapter =
		valid ? calloc(1, sizeof(*adapter)) : NULL;
	if (!adapter || !archive_mrxs_adapter_open(
			adapter, source_path, fingerprint, error, error_capacity)) {
		free(adapter);
		adapter = NULL;
	}
	free(text);
	if (!adapter && (!error || !error[0]))
		archive_provider_set_error(
			error, error_capacity,
			"could not validate retained MRXS provider recipe");
	return adapter;
}

static archive_provider_cache_entry_t* archive_provider_shell_cache_acquire(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		char* error, size_t error_capacity) {
	bool is_part10 = record && strcmp(record->provider, "part10") == 0;
	bool is_mrxs = record && strcmp(record->provider, "mrxs-native") == 0;
	if (!repository || !record || (!is_part10 && !is_mrxs) ||
	    !record->shell_key[0] || (is_part10 && !record->object_key[0]) ||
	    record->shell_byte_size == 0 || record->shell_byte_size > SIZE_MAX) {
		archive_provider_set_error(error, error_capacity,
		                           "published instance has no supported shell provider");
		return NULL;
	}
	platform_mutex_lock(&archive_provider_cache_mutex);
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		archive_provider_cache_entry_t* entry = archive_provider_cache + i;
		if (entry->occupied &&
		    strcmp(entry->repository_root, repository->root) == 0 &&
		    strcmp(entry->shell_key, record->shell_key) == 0 &&
		    entry->shell_instance_index == record->shell_instance_index) {
			++archive_provider_cache_stats.hits;
			++entry->refcount;
			entry->last_used = ++archive_provider_cache_clock;
			platform_mutex_unlock(&archive_provider_cache_mutex);
			return entry;
		}
	}
	++archive_provider_cache_stats.misses;
	platform_mutex_unlock(&archive_provider_cache_mutex);

	char source_path[ARCHIVE_PATH_CAPACITY];
	u8* shell_data = NULL;
	char* metadata = NULL;
	file_handle_t source_file = 0;
	archive_mrxs_adapter_t* mrxs = NULL;
	archive_shell_view_t shell = {0};
	archive_shell_instance_t instance = {0};
	size_t metadata_size = 0;
	bool success = (!is_part10 ||
		archive_repository_make_path(repository, record->object_key,
		                             source_path, sizeof(source_path))) &&
	               archive_provider_load_published_shell(
		               repository, record, &shell_data, &shell, &instance,
		               error, error_capacity);
	if (success && is_part10 &&
	    archive_provider_path_is_external_reference(source_path)) {
		char actual_sha256[65];
		u64 actual_size = 0;
		success = archive_provider_hash_file(
			          source_path, actual_sha256, &actual_size) &&
		          actual_size == record->object_byte_size &&
		          strcmp(actual_sha256, record->object_sha256) == 0;
		if (!success)
			archive_provider_set_error(
				error, error_capacity,
				"externally referenced Part 10 source changed or is unavailable");
	}
	if (!success && is_part10) {
		/*
		 * A single process-wide repair lock keeps regeneration rare and bounded.
		 * Recheck after acquiring it: another request may already have restored
		 * the same immutable content-addressed object.
		 */
		platform_mutex_lock(&archive_provider_regeneration_mutex);
		if (error && error_capacity) error[0] = '\0';
		success = archive_repository_make_path(repository, record->object_key,
		                                       source_path, sizeof(source_path)) &&
		          archive_provider_load_published_shell(
			          repository, record, &shell_data, &shell, &instance,
			          error, error_capacity);
		if (!success) {
			if (error && error_capacity) error[0] = '\0';
			success = archive_provider_regenerate_shell(repository, record,
			                                           error, error_capacity);
			if (success) {
				if (error && error_capacity) error[0] = '\0';
				success = archive_provider_load_published_shell(
					repository, record, &shell_data, &shell, &instance,
					error, error_capacity);
			}
		}
		platform_mutex_unlock(&archive_provider_regeneration_mutex);
	}
	u64 metadata_capacity = 0;
	success = success && archive_provider_get_metadata_capacity(
		&shell, &instance, &metadata_capacity);
	if (success) {
		metadata = malloc((size_t)metadata_capacity);
		success = metadata &&
		          archive_provider_write_shell_metadata(
			          &shell, &instance, metadata, (size_t)metadata_capacity,
			          &metadata_size, true);
	}
	if (success && is_part10) {
		source_file = open_file_handle_for_simultaneous_access(source_path);
		success = source_file != 0;
	} else if (success && is_mrxs) {
		mrxs = archive_provider_open_mrxs(
			repository, record, error, error_capacity);
		success = mrxs != NULL;
	}
	if (!success) {
		file_handle_close(source_file);
		if (mrxs) {
			archive_mrxs_adapter_close(mrxs);
			free(mrxs);
		}
		free(metadata);
		free(shell_data);
		if (!error || !error[0])
			archive_provider_set_error(error, error_capacity,
			                           "could not load or validate virtual DICOM shell '%s'",
			                           record->shell_key);
		return NULL;
	}

	platform_mutex_lock(&archive_provider_cache_mutex);
	archive_provider_cache_entry_t* available = NULL;
	for (i32 i = 0; i < COUNT(archive_provider_cache); ++i) {
		archive_provider_cache_entry_t* entry = archive_provider_cache + i;
		if (entry->occupied &&
		    strcmp(entry->repository_root, repository->root) == 0 &&
		    strcmp(entry->shell_key, record->shell_key) == 0 &&
		    entry->shell_instance_index == record->shell_instance_index) {
			++entry->refcount;
			entry->last_used = ++archive_provider_cache_clock;
			platform_mutex_unlock(&archive_provider_cache_mutex);
			file_handle_close(source_file);
			if (mrxs) {
				archive_mrxs_adapter_close(mrxs);
				free(mrxs);
			}
			free(metadata);
			free(shell_data);
			return entry;
		}
		if (!entry->occupied ||
		    (entry->refcount == 0 && (!available || entry->last_used < available->last_used)))
			available = entry;
	}
	if (!available) {
		platform_mutex_unlock(&archive_provider_cache_mutex);
		file_handle_close(source_file);
		free(metadata);
		free(shell_data);
		archive_provider_set_error(error, error_capacity,
		                           "all virtual DICOM shell cache entries are busy");
		return NULL;
	}
	if (available->occupied) ++archive_provider_cache_stats.evictions;
	archive_provider_cache_entry_destroy(available);
	available->occupied = true;
	available->refcount = 1;
	available->last_used = ++archive_provider_cache_clock;
	snprintf(available->repository_root, sizeof(available->repository_root), "%s",
	         repository->root);
	snprintf(available->shell_key, sizeof(available->shell_key), "%s", record->shell_key);
	available->shell_instance_index = record->shell_instance_index;
	available->shell_data = shell_data;
	available->shell_size = (size_t)record->shell_byte_size;
	available->shell = shell;
	available->instance = instance;
	available->source_file = source_file;
	available->mrxs = mrxs;
	available->metadata_json = metadata;
	available->metadata_size = metadata_size;
	platform_mutex_unlock(&archive_provider_cache_mutex);
	return available;
}

static void archive_provider_cache_release(archive_provider_cache_entry_t* entry) {
	if (!entry) return;
	platform_mutex_lock(&archive_provider_cache_mutex);
	ASSERT(entry->occupied && entry->refcount > 0);
	--entry->refcount;
	entry->last_used = ++archive_provider_cache_clock;
	platform_mutex_unlock(&archive_provider_cache_mutex);
}

bool archive_instance_provider_get_metadata(const archive_repository_t* repository,
		const archive_instance_record_t* record, char** json, size_t* json_size,
		char* error, size_t error_capacity) {
	if (!json || !json_size) return false;
	*json = NULL;
	*json_size = 0;
	archive_provider_cache_entry_t* entry =
		archive_provider_shell_cache_acquire(repository, record, error, error_capacity);
	if (!entry) return false;
	char* result = malloc(entry->metadata_size + 1);
	bool success = result != NULL;
	if (success) {
		memcpy(result, entry->metadata_json, entry->metadata_size + 1);
		*json = result;
		*json_size = entry->metadata_size;
	} else {
		archive_provider_set_error(error, error_capacity,
		                           "could not copy cached Part 10 metadata");
	}
	archive_provider_cache_release(entry);
	return success;
}

bool archive_instance_provider_get_series_metadata(
		const archive_repository_t* repository,
		const archive_instance_record_t* records, i32 record_count,
		char** json, size_t* json_size, char* error, size_t error_capacity) {
	if (!json || !json_size) return false;
	*json = NULL;
	*json_size = 0;
	if (!repository || !records || record_count <= 0) {
		archive_provider_set_error(error, error_capacity,
		                           "published series has no instances");
		return false;
	}
	archive_provider_cache_entry_t* entry =
		archive_provider_shell_cache_acquire(
			repository, records, error, error_capacity);
	if (!entry) return false;

	bool success = entry->shell.instance_count == (u32)record_count;
	u64 result_capacity = 4;
	for (i32 i = 0; i < record_count && success; ++i) {
		const archive_instance_record_t* record = records + i;
		archive_shell_instance_t instance = {0};
		success =
			record->shell_instance_index == (u32)i &&
			strcmp(record->provider, records->provider) == 0 &&
			strcmp(record->study_instance_uid,
			       records->study_instance_uid) == 0 &&
			strcmp(record->series_instance_uid,
			       records->series_instance_uid) == 0 &&
			strcmp(record->shell_key, records->shell_key) == 0 &&
			strcmp(record->shell_sha256, records->shell_sha256) == 0 &&
			record->shell_byte_size == records->shell_byte_size &&
			strcmp(record->bundle_sha256, records->bundle_sha256) == 0 &&
			strcmp(record->representation_fingerprint,
			       records->representation_fingerprint) == 0 &&
			strcmp(record->recipe_key, records->recipe_key) == 0 &&
			strcmp(record->recipe_sha256, records->recipe_sha256) == 0 &&
			record->recipe_byte_size == records->recipe_byte_size &&
			archive_shell_get_instance(
				&entry->shell, (u32)i, &instance) &&
			archive_provider_record_matches_shell(
				record, &entry->shell, &instance,
				error, error_capacity);
		u64 instance_capacity = 0;
		success = success && archive_provider_get_metadata_capacity(
			&entry->shell, &instance, &instance_capacity);
		if (!success ||
		    result_capacity > ARCHIVE_PROVIDER_MAX_METADATA_SIZE -
		                      instance_capacity) {
			success = false;
			break;
		}
		result_capacity += instance_capacity;
	}
	if (!success) {
		if (!error || !error[0])
			archive_provider_set_error(
				error, error_capacity,
				"published series does not match one complete aggregate shell");
		archive_provider_cache_release(entry);
		return false;
	}

	char* result = malloc((size_t)result_capacity);
	success = result != NULL;
	size_t used = 0;
	if (success) result[used++] = '[';
	for (i32 i = 0; i < record_count && success; ++i) {
		archive_shell_instance_t instance = {0};
		size_t object_size = 0;
		if (i) result[used++] = ',';
		success =
			archive_shell_get_instance(&entry->shell, (u32)i, &instance) &&
			archive_provider_write_shell_metadata(
				&entry->shell, &instance, result + used,
				(size_t)result_capacity - used, &object_size, false);
		if (success) used += object_size;
	}
	if (success) {
		result[used++] = ']';
		result[used++] = '\n';
		result[used] = '\0';
		*json = result;
		*json_size = used;
	} else if (!error || !error[0]) {
		archive_provider_set_error(error, error_capacity,
		                           "could not serialize series metadata");
	}
	if (!success) free(result);
	archive_provider_cache_release(entry);
	return success;
}

static const char* archive_provider_media_type_for_transfer_syntax(
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

i32 archive_instance_provider_get_frame_representations(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		archive_frame_representation_t* representations, i32 capacity,
		char* error, size_t error_capacity) {
	if (!repository || !record || !representations || capacity <= 0 ||
	    (strcmp(record->provider, "part10") != 0 &&
	     strcmp(record->provider, "mrxs-native") != 0)) return 0;
	if (strcmp(record->provider, "mrxs-native") == 0) {
		archive_provider_cache_entry_t* entry =
			archive_provider_shell_cache_acquire(
				repository, record, error, error_capacity);
		if (!entry) return 0;
		bool valid =
			entry->instance.provider_kind ==
				ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE &&
			entry->instance.provider_parameter_version ==
				ARCHIVE_MRXS_ADAPTER_VERSION;
		archive_provider_cache_release(entry);
		if (!valid) return 0;
		archive_frame_representation_t generated = {0};
		snprintf(generated.media_type, sizeof(generated.media_type),
		         "image/jpeg");
		snprintf(generated.transfer_syntax_uid,
		         sizeof(generated.transfer_syntax_uid),
		         "1.2.840.10008.1.2.4.50");
		generated.source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
		generated.preference = 200;
		representations[0] = generated;
		return 1;
	}
	const char* media_type =
		archive_provider_media_type_for_transfer_syntax(record->transfer_syntax_uid);
	if (!media_type) return 0;
	archive_frame_representation_t representation = {0};
	snprintf(representation.media_type, sizeof(representation.media_type), "%s", media_type);
	snprintf(representation.transfer_syntax_uid,
	         sizeof(representation.transfer_syntax_uid), "%s",
	         record->transfer_syntax_uid);
	representation.source = ARCHIVE_FRAME_REPRESENTATION_STORED;
	representation.preference = 100;
	representations[0] = representation;
	i32 count = 1;

	archive_provider_cache_entry_t* entry =
		archive_provider_shell_cache_acquire(repository, record, error, error_capacity);
	if (!entry) return 0;
	const archive_shell_instance_t* instance = &entry->instance;
	bool can_generate_rgb8 =
		instance->payload_is_encapsulated &&
		instance->bits_allocated == 8 && instance->bits_stored == 8 &&
		instance->high_bit == 7 && instance->pixel_representation == 0 &&
		instance->samples_per_pixel == 3 &&
		strcmp(instance->photometric_interpretation, "RGB") == 0;
	if (can_generate_rgb8 && count < capacity) {
		archive_frame_representation_t generated = {0};
		snprintf(generated.media_type, sizeof(generated.media_type),
		         "application/octet-stream");
		snprintf(generated.transfer_syntax_uid,
		         sizeof(generated.transfer_syntax_uid), "1.2.840.10008.1.2.1");
		generated.source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
		generated.preference = 200;
		representations[count++] = generated;
	}
	archive_provider_cache_release(entry);
	return count;
}

static bool archive_provider_read_shell_frame(
		const archive_provider_cache_entry_t* entry, u32 frame_index,
		u8** data, size_t* size) {
	if (!entry || !data || !size) return false;
	*data = NULL;
	*size = 0;
	archive_shell_payload_range_t range = {0};
	if (!archive_shell_get_payload_range(&entry->shell, &entry->instance,
	                                     frame_index, &range) ||
	    range.source_index != entry->instance.source_index ||
	    range.size > ARCHIVE_PROVIDER_MAX_FRAME_SIZE) return false;
	u8* encoded = malloc(range.size);
	if (!encoded) return false;
	if (file_handle_read_at_offset(encoded, entry->source_file, range.offset, range.size) !=
	    range.size) {
		free(encoded);
		return false;
	}
	size_t encoded_size = range.size;
	if (entry->instance.payload_is_encapsulated) {
		i64 payload_size =
			dicom_defragment_encapsulated_pixel_data_frame(encoded, (i64)encoded_size);
		if (payload_size <= 0 || payload_size > ARCHIVE_PROVIDER_MAX_FRAME_SIZE) {
			free(encoded);
			return false;
		}
		encoded_size = (size_t)payload_size;
	}
	*data = encoded;
	*size = encoded_size;
	return true;
}

bool archive_instance_provider_get_frame(const archive_repository_t* repository,
		const archive_instance_record_t* record, u32 one_based_frame,
		const archive_frame_representation_t* representation,
		archive_instance_frame_t* frame, char* error, size_t error_capacity) {
	if (!frame) return false;
	memset(frame, 0, sizeof(*frame));
	if (!record || !representation || one_based_frame == 0) {
		archive_provider_set_error(error, error_capacity,
		                           "requested frame representation is unavailable");
		return false;
	}
	archive_provider_cache_entry_t* entry =
		archive_provider_shell_cache_acquire(repository, record, error, error_capacity);
	if (!entry) return false;
	const archive_shell_instance_t* instance = &entry->instance;
	if (entry->mrxs &&
	    instance->provider_kind == ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE &&
	    representation->source == ARCHIVE_FRAME_REPRESENTATION_GENERATED &&
	    strcmp(representation->media_type, "image/jpeg") == 0 &&
	    strcmp(representation->transfer_syntax_uid,
	           "1.2.840.10008.1.2.4.50") == 0 &&
	    archive_mrxs_adapter_get_jpeg_frame(
		    entry->mrxs, record->shell_instance_index, one_based_frame,
		    &frame->data, &frame->size, error, error_capacity)) {
		snprintf(frame->media_type, sizeof(frame->media_type),
		         "image/jpeg");
		snprintf(frame->transfer_syntax_uid,
		         sizeof(frame->transfer_syntax_uid),
		         "1.2.840.10008.1.2.4.50");
		archive_provider_cache_release(entry);
		return true;
	}
	const char* media_type =
		archive_provider_media_type_for_transfer_syntax(instance->transfer_syntax_uid);
	bool stored = representation->source == ARCHIVE_FRAME_REPRESENTATION_STORED &&
	              strcmp(representation->transfer_syntax_uid,
	                     instance->transfer_syntax_uid) == 0 &&
	              media_type && strcmp(media_type, representation->media_type) == 0;
	if (stored && archive_provider_read_shell_frame(
			entry, one_based_frame - 1, &frame->data, &frame->size)) {
		snprintf(frame->media_type, sizeof(frame->media_type), "%s", media_type);
		snprintf(frame->transfer_syntax_uid, sizeof(frame->transfer_syntax_uid), "%s",
		         instance->transfer_syntax_uid);
		archive_provider_cache_release(entry);
		return true;
	}

	bool generated_rgb8 =
		representation->source == ARCHIVE_FRAME_REPRESENTATION_GENERATED &&
		strcmp(representation->media_type, "application/octet-stream") == 0 &&
		strcmp(representation->transfer_syntax_uid, "1.2.840.10008.1.2.1") == 0 &&
		media_type != NULL &&
		instance->payload_is_encapsulated &&
		instance->bits_allocated == 8 && instance->bits_stored == 8 &&
		instance->high_bit == 7 && instance->pixel_representation == 0 &&
		instance->samples_per_pixel == 3 &&
		strcmp(instance->photometric_interpretation, "RGB") == 0;
	u8* encoded = NULL;
	size_t encoded_size = 0;
	if (generated_rgb8 &&
	    archive_provider_read_shell_frame(entry, one_based_frame - 1,
	                                      &encoded, &encoded_size)) {
		i32 decoded_width = 0;
		i32 decoded_height = 0;
		u8* bgra = NULL;
		if (strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0 &&
		    encoded_size <= UINT32_MAX) {
			i32 channels = 0;
			bgra = jpeg_decode_image(encoded, (u32)encoded_size,
			                         &decoded_width, &decoded_height, &channels);
			if (bgra && channels != 4) {
				free(bgra);
				bgra = NULL;
			}
		} else if (strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.90") == 0 ||
		           strcmp(instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.91") == 0) {
			bgra = jpeg2000_decode_image_to_bgra(encoded, encoded_size,
			                                    &decoded_width, &decoded_height);
		}
		free(encoded);
		encoded = NULL;
		size_t pixel_count = (size_t)instance->rows * instance->columns;
		if (bgra && decoded_width == instance->columns &&
		    decoded_height == instance->rows &&
		    pixel_count <= ARCHIVE_PROVIDER_MAX_FRAME_SIZE / 3) {
			frame->size = pixel_count * 3;
			frame->data = malloc(frame->size);
			if (frame->data) {
				for (size_t i = 0; i < pixel_count; ++i) {
					frame->data[i * 3 + 0] = bgra[i * 4 + 2];
					frame->data[i * 3 + 1] = bgra[i * 4 + 1];
					frame->data[i * 3 + 2] = bgra[i * 4 + 0];
				}
				snprintf(frame->media_type, sizeof(frame->media_type),
				         "application/octet-stream");
				snprintf(frame->transfer_syntax_uid,
				         sizeof(frame->transfer_syntax_uid),
				         "1.2.840.10008.1.2.1");
			}
		}
		free(bgra);
		if (frame->data) {
			archive_provider_cache_release(entry);
			return true;
		}
	}
	if (encoded) free(encoded);
	if (!frame->data) {
		archive_provider_set_error(error, error_capacity,
		                           "frame is unavailable in the selected representation");
		archive_provider_cache_release(entry);
		return false;
	}
	archive_provider_cache_release(entry);
	return false;
}

void archive_instance_frame_destroy(archive_instance_frame_t* frame) {
	if (!frame) return;
	free(frame->data);
	memset(frame, 0, sizeof(*frame));
}
