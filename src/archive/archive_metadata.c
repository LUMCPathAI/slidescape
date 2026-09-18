/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_metadata.h"
#include "archive_frame_map.h"

#include <stdarg.h>

typedef struct archive_metadata_writer_t {
	char* data;
	size_t capacity;
	size_t used;
	bool failed;
} archive_metadata_writer_t;

static void archive_metadata_write(archive_metadata_writer_t* writer, const char* text) {
	if (writer->failed) return;
	size_t length = strlen(text);
	if (length >= writer->capacity - writer->used) {
		writer->failed = true;
		return;
	}
	if (writer->data) memcpy(writer->data + writer->used, text, length);
	writer->used += length;
	if (writer->data) writer->data[writer->used] = '\0';
}

static void archive_metadata_write_format(archive_metadata_writer_t* writer, const char* format, ...) {
	if (writer->failed) return;
	va_list args;
	va_start(args, format);
	i32 length = vsnprintf(writer->data ? writer->data + writer->used : NULL,
		writer->data ? writer->capacity - writer->used : 0, format, args);
	va_end(args);
	if (length < 0 || (size_t)length >= writer->capacity - writer->used) {
		writer->failed = true;
		return;
	}
	writer->used += (size_t)length;
}

static void archive_metadata_write_json_string(
		archive_metadata_writer_t* writer, const char* value) {
	static const char hex[] = "0123456789abcdef";
	archive_metadata_write(writer, "\"");
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
		archive_metadata_write(writer, encoded);
	}
	archive_metadata_write(writer, "\"");
}

static void archive_metadata_write_person_name(
		archive_metadata_writer_t* writer, const char* value) {
	static const char* keys[] = {"Alphabetic", "Ideographic", "Phonetic"};
	archive_metadata_write(writer, "{");
	bool wrote_group = false;
	const char* group = value;
	for (i32 i = 0; i < COUNT(keys) && group; ++i) {
		const char* separator = strchr(group, '=');
		size_t length = separator ? (size_t)(separator - group) : strlen(group);
		if (length) {
			if (wrote_group) archive_metadata_write(writer, ",");
			archive_metadata_write_format(writer, "\"%s\":", keys[i]);
			char name[321] = {0};
			length = MIN(length, sizeof(name) - 1);
			memcpy(name, group, length);
			archive_metadata_write_json_string(writer, name);
			wrote_group = true;
		}
		group = separator ? separator + 1 : NULL;
	}
	archive_metadata_write(writer, "}");
}

static void archive_metadata_write_base64(
		archive_metadata_writer_t* writer, const u8* data, size_t size) {
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	archive_metadata_write(writer, "\"");
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
		archive_metadata_write(writer, encoded);
	}
	archive_metadata_write(writer, "\"");
}

static void archive_metadata_write_code_sequence(
		archive_metadata_writer_t* writer, const char* tag,
		const char* code_value, const char* coding_scheme,
		const char* code_meaning, bool* has_attribute) {
	if (!code_value || !code_value[0] || !coding_scheme || !coding_scheme[0] ||
	    !code_meaning || !code_meaning[0]) return;
	if (*has_attribute) archive_metadata_write(writer, ",");
	archive_metadata_write_format(writer, "\"%s\":{\"vr\":\"SQ\",\"Value\":[{", tag);
	archive_metadata_write(writer, "\"00080100\":{\"vr\":\"SH\",\"Value\":[");
	archive_metadata_write_json_string(writer, code_value);
	archive_metadata_write(writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_metadata_write_json_string(writer, coding_scheme);
	archive_metadata_write(writer, "]},\"00080104\":{\"vr\":\"LO\",\"Value\":[");
	archive_metadata_write_json_string(writer, code_meaning);
	archive_metadata_write(writer, "]}}]}");
	*has_attribute = true;
}

static void archive_metadata_write_content_code_sequence(
		archive_metadata_writer_t* writer, const char* tag,
		const char* code_value, const char* coding_scheme,
		const char* coding_scheme_version, const char* code_meaning) {
	archive_metadata_write_format(
		writer, "\"%s\":{\"vr\":\"SQ\",\"Value\":[{"
		"\"00080100\":{\"vr\":\"SH\",\"Value\":[", tag);
	archive_metadata_write_json_string(writer, code_value);
	archive_metadata_write(
		writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_metadata_write_json_string(writer, coding_scheme);
	archive_metadata_write(writer, "]}");
	if (coding_scheme_version && coding_scheme_version[0]) {
		archive_metadata_write(
			writer, ",\"00080103\":{\"vr\":\"SH\",\"Value\":[");
		archive_metadata_write_json_string(
			writer, coding_scheme_version);
		archive_metadata_write(writer, "]}");
	}
	if (code_meaning && code_meaning[0]) {
		archive_metadata_write(
			writer, ",\"00080104\":{\"vr\":\"LO\",\"Value\":[");
		archive_metadata_write_json_string(writer, code_meaning);
		archive_metadata_write(writer, "]}");
	}
	archive_metadata_write(writer, "}]}");
}

static bool archive_metadata_write_preparation_content_item(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 item_index,
		archive_metadata_writer_t* writer) {
	archive_shell_preparation_item_t item = {0};
	if (!archive_shell_get_preparation_item(
			shell, instance, item_index, &item)) return false;
	static const char* value_types[] = {
		"", "CODE", "TEXT", "NUM", "UIDREF",
		"DATETIME", "DATE", "TIME", "PNAME",
	};
	if ((u32)item.value_kind >= COUNT(value_types)) return false;
	archive_metadata_write(
		writer, "{\"0040A040\":{\"vr\":\"CS\",\"Value\":[");
	archive_metadata_write_json_string(
		writer, value_types[(u32)item.value_kind]);
	archive_metadata_write(writer, "]}");
	if (item.relationship_type[0]) {
		archive_metadata_write(
			writer, ",\"0040A010\":{\"vr\":\"CS\",\"Value\":[");
		archive_metadata_write_json_string(
			writer, item.relationship_type);
		archive_metadata_write(writer, "]}");
	}
	archive_metadata_write(
		writer, ",\"0040A043\":{\"vr\":\"SQ\",\"Value\":[{");
	archive_metadata_write(
		writer, "\"00080100\":{\"vr\":\"SH\",\"Value\":[");
	archive_metadata_write_json_string(
		writer, item.concept_name_code_value);
	archive_metadata_write(
		writer, "]},\"00080102\":{\"vr\":\"SH\",\"Value\":[");
	archive_metadata_write_json_string(
		writer, item.concept_name_coding_scheme);
	archive_metadata_write(writer, "]}");
	if (item.concept_name_coding_scheme_version[0]) {
		archive_metadata_write(
			writer, ",\"00080103\":{\"vr\":\"SH\",\"Value\":[");
		archive_metadata_write_json_string(
			writer, item.concept_name_coding_scheme_version);
		archive_metadata_write(writer, "]}");
	}
	if (item.concept_name_code_meaning[0]) {
		archive_metadata_write(
			writer, ",\"00080104\":{\"vr\":\"LO\",\"Value\":[");
		archive_metadata_write_json_string(
			writer, item.concept_name_code_meaning);
		archive_metadata_write(writer, "]}");
	}
	archive_metadata_write(writer, "}]}");
	switch (item.value_kind) {
		case ARCHIVE_SHELL_CONTENT_CODE:
			archive_metadata_write(writer, ",");
			archive_metadata_write_content_code_sequence(
				writer, "0040A168", item.code_value,
				item.code_coding_scheme,
				item.code_coding_scheme_version, item.code_meaning);
			break;
		case ARCHIVE_SHELL_CONTENT_NUM: {
			char* end = NULL;
			double numeric_value = strtod(item.scalar_value, &end);
			if (!end || *end || !isfinite(numeric_value)) return false;
			archive_metadata_write_format(
				writer,
				",\"0040A300\":{\"vr\":\"SQ\",\"Value\":[{"
				"\"0040A30A\":{\"vr\":\"DS\",\"Value\":[%.17g]}",
				numeric_value);
			if (item.measurement_units_code_value[0] &&
			    item.measurement_units_coding_scheme[0]) {
				archive_metadata_write(writer, ",");
				archive_metadata_write_content_code_sequence(
					writer, "004008EA",
					item.measurement_units_code_value,
					item.measurement_units_coding_scheme,
					item.measurement_units_coding_scheme_version,
					item.measurement_units_code_meaning);
			}
			archive_metadata_write(writer, "}]}");
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
			archive_metadata_write_format(
				writer, ",\"%s\":{\"vr\":\"%s\",\"Value\":[",
				tag, vr);
			if (item.value_kind == ARCHIVE_SHELL_CONTENT_PNAME)
				archive_metadata_write_person_name(
					writer, item.scalar_value);
			else
				archive_metadata_write_json_string(
					writer, item.scalar_value);
			archive_metadata_write(writer, "]}");
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
		archive_metadata_write(
			writer, has_child ? "," :
			",\"0040A730\":{\"vr\":\"SQ\",\"Value\":[");
		if (!archive_metadata_write_preparation_content_item(
				shell, instance, child_index, writer)) return false;
		has_child = true;
	}
	if (has_child) archive_metadata_write(writer, "]}");
	archive_metadata_write(writer, "}");
	return !writer->failed;
}

bool archive_metadata_write_instance(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		char* dest, size_t capacity, size_t* size, bool array_wrapper) {
	if (!shell || !instance || !size || !capacity) return false;
	*size = 0;
	archive_metadata_writer_t writer = {dest, capacity, 0, false};
	archive_metadata_write(&writer, array_wrapper ? "[{" : "{");
	u32 attribute_count = archive_shell_get_attribute_count(shell, instance);
	bool has_attribute = false;
	for (u32 i = 0; i < attribute_count; ++i) {
		archive_shell_attribute_t attribute = {0};
		if (!archive_shell_get_attribute(shell, instance, i, &attribute)) return false;
		if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_STRING &&
		    !attribute.string_value[0]) continue;
		if (has_attribute) archive_metadata_write(&writer, ",");
		archive_metadata_write_format(&writer, "\"%08X\":{\"vr\":\"%s\",\"Value\":[",
		                              attribute.tag, attribute.vr);
		if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_STRING) {
			if (strcmp(attribute.vr, "PN") == 0)
				archive_metadata_write_person_name(&writer, attribute.string_value);
			else
				archive_metadata_write_json_string(&writer, attribute.string_value);
		} else if (attribute.value_kind == ARCHIVE_SHELL_ATTRIBUTE_SIGNED) {
			if (strcmp(attribute.vr, "IS") == 0)
				archive_metadata_write_format(&writer, "\"%lld\"",
				                              (long long)attribute.signed_value);
			else
				archive_metadata_write_format(&writer, "%lld",
				                              (long long)attribute.signed_value);
		} else if (strcmp(attribute.vr, "IS") == 0) {
			archive_metadata_write_format(&writer, "\"%llu\"",
			                              (unsigned long long)attribute.unsigned_value);
		} else {
			archive_metadata_write_format(&writer, "%llu",
			                              (unsigned long long)attribute.unsigned_value);
		}
		archive_metadata_write(&writer, "]}");
		has_attribute = true;
	}
	/* Pixel interpretation is present in every shell, including the v2 layout. */
	archive_metadata_write_format(&writer,
		"%s\"00280102\":{\"vr\":\"US\",\"Value\":[%u]},"
		"\"00280103\":{\"vr\":\"US\",\"Value\":[%u]}",
		has_attribute ? "," : "", instance->high_bit, instance->pixel_representation);
	has_attribute = true;
	if (instance->samples_per_pixel > 1)
		archive_metadata_write_format(&writer,
			",\"00280006\":{\"vr\":\"US\",\"Value\":[%u]}", instance->planar_configuration);
	/* Legacy shells lack this provenance. Omit it instead of inventing ORIGINAL. */
	if (instance->has_image_type && instance->image_flavor[0]) {
		archive_metadata_write_format(&writer,
			",\"00080008\":{\"vr\":\"CS\",\"Value\":[\"%s\",\"PRIMARY\",",
			instance->is_image_original ? "ORIGINAL" : "DERIVED");
		archive_metadata_write_json_string(&writer, instance->image_flavor);
		archive_metadata_write_format(&writer, ",\"%s\"]}",
			instance->is_image_resampled ? "RESAMPLED" : "NONE");
	}
	if (instance->concatenation_uid[0]) {
		archive_metadata_write(&writer, ",\"00209161\":{\"vr\":\"UI\",\"Value\":[");
		archive_metadata_write_json_string(&writer, instance->concatenation_uid);
		archive_metadata_write_format(&writer,
			"]},\"00209162\":{\"vr\":\"US\",\"Value\":[%u]},"
			"\"00209228\":{\"vr\":\"UL\",\"Value\":[%u]}",
			instance->in_concatenation_number, instance->concatenation_frame_offset);
	}
	archive_metadata_write_format(&writer,
		",\"00480302\":{\"vr\":\"UL\",\"Value\":[%u]},"
		"\"00480303\":{\"vr\":\"UL\",\"Value\":[%u]}",
		instance->optical_path_count, instance->focal_plane_count);
	if (instance->has_slide_coordinate_origin) {
		archive_metadata_write_format(&writer,
			",\"00480008\":{\"vr\":\"SQ\",\"Value\":[{"
			"\"0040072A\":{\"vr\":\"DS\",\"Value\":[%g]},"
			"\"0040073A\":{\"vr\":\"DS\",\"Value\":[%g]},"
			"\"0040074A\":{\"vr\":\"DS\",\"Value\":[%g]}}]}",
			instance->slide_coordinate_origin_x_mm, instance->slide_coordinate_origin_y_mm,
			instance->slide_coordinate_origin_z_um);
	}
	if (instance->has_image_orientation_slide) {
		const float* orientation = instance->image_orientation_slide;
		archive_metadata_write_format(&writer,
			",\"00480102\":{\"vr\":\"DS\",\"Value\":[%g,%g,%g,%g,%g,%g]}",
			orientation[0], orientation[1], orientation[2],
			orientation[3], orientation[4], orientation[5]);
	}
	if (has_attribute) archive_metadata_write(&writer, ",");
	archive_metadata_write(&writer,
		"\"00480105\":{\"vr\":\"SQ\",\"Value\":[");
	for (u32 i = 0; i < instance->optical_path_count; ++i) {
		archive_shell_optical_path_t optical_path = {0};
		if (!archive_shell_get_optical_path(shell, instance, i, &optical_path))
			return false;
		if (i) archive_metadata_write(&writer, ",");
		archive_metadata_write(&writer,
			"{\"00480106\":{\"vr\":\"SH\",\"Value\":[");
		archive_metadata_write_json_string(&writer, optical_path.identifier);
		archive_metadata_write(&writer, "]}");
		bool has_attribute = true;
		if (optical_path.description && optical_path.description[0]) {
			archive_metadata_write(&writer,
				",\"00480107\":{\"vr\":\"ST\",\"Value\":[");
			archive_metadata_write_json_string(&writer, optical_path.description);
			archive_metadata_write(&writer, "]}");
		}
		if (optical_path.color_space && optical_path.color_space[0]) {
			archive_metadata_write(&writer,
				",\"00282002\":{\"vr\":\"CS\",\"Value\":[");
			archive_metadata_write_json_string(&writer, optical_path.color_space);
			archive_metadata_write(&writer, "]}");
		}
		if (optical_path.illumination_wavelength > 0.0f)
			archive_metadata_write_format(
				&writer, ",\"00220055\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.illumination_wavelength);
		archive_metadata_write_code_sequence(
			&writer, "00220016",
			optical_path.illumination_type_code_value,
			optical_path.illumination_type_coding_scheme,
			optical_path.illumination_type_code_meaning, &has_attribute);
		archive_metadata_write_code_sequence(
			&writer, "00480108",
			optical_path.illumination_color_code_value,
			optical_path.illumination_color_coding_scheme,
			optical_path.illumination_color_code_meaning, &has_attribute);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_PASS)
			archive_metadata_write_format(
				&writer, ",\"00220001\":{\"vr\":\"US\",\"Value\":[%u]}",
				optical_path.light_path_filter_pass_through_wavelength);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_BAND)
			archive_metadata_write_format(
				&writer, ",\"00220002\":{\"vr\":\"US\",\"Value\":[%u,%u]}",
				optical_path.light_path_filter_pass_band[0],
				optical_path.light_path_filter_pass_band[1]);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_PASS)
			archive_metadata_write_format(
				&writer, ",\"00220003\":{\"vr\":\"US\",\"Value\":[%u]}",
				optical_path.image_path_filter_pass_through_wavelength);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_BAND)
			archive_metadata_write_format(
				&writer, ",\"00220004\":{\"vr\":\"US\",\"Value\":[%u,%u]}",
				optical_path.image_path_filter_pass_band[0],
				optical_path.image_path_filter_pass_band[1]);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_OBJECTIVE_POWER &&
		    isfinite(optical_path.objective_lens_power))
			archive_metadata_write_format(
				&writer, ",\"00480112\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.objective_lens_power);
		if (optical_path.source_preserved_fields &
		    ARCHIVE_SHELL_OPTICAL_OBJECTIVE_APERTURE &&
		    isfinite(optical_path.objective_lens_numerical_aperture))
			archive_metadata_write_format(
				&writer, ",\"00480113\":{\"vr\":\"FL\",\"Value\":[%g]}",
				optical_path.objective_lens_numerical_aperture);
		if (optical_path.icc_profile_size) {
			archive_metadata_write(
				&writer, ",\"00282000\":{\"vr\":\"OB\",\"InlineBinary\":");
			archive_metadata_write_base64(
				&writer, optical_path.icc_profile,
				(size_t)optical_path.icc_profile_size);
			archive_metadata_write(&writer, "}");
		}
		archive_metadata_write(&writer, "}");
	}
	archive_metadata_write(&writer, "]}");
	const char* container_identifier =
		archive_shell_get_container_identifier(shell, instance);
	if (!container_identifier) return false;
	if (container_identifier[0]) {
		archive_metadata_write(
			&writer, ",\"00400512\":{\"vr\":\"LO\",\"Value\":[");
		archive_metadata_write_json_string(&writer, container_identifier);
		archive_metadata_write(&writer, "]}");
	}
	u32 specimen_count = archive_shell_get_specimen_count(shell, instance);
	if (specimen_count) {
		archive_metadata_write(
			&writer, ",\"00400560\":{\"vr\":\"SQ\",\"Value\":[");
		for (u32 i = 0; i < specimen_count; ++i) {
			archive_shell_specimen_t specimen = {0};
			if (!archive_shell_get_specimen(shell, instance, i, &specimen))
				return false;
			if (i) archive_metadata_write(&writer, ",");
			archive_metadata_write(&writer, "{");
			bool has_value = false;
			if (specimen.identifier[0]) {
				archive_metadata_write(
					&writer, "\"00400551\":{\"vr\":\"LO\",\"Value\":[");
				archive_metadata_write_json_string(&writer, specimen.identifier);
				archive_metadata_write(&writer, "]}");
				has_value = true;
			}
			if (specimen.uid[0]) {
				if (has_value) archive_metadata_write(&writer, ",");
				archive_metadata_write(
					&writer, "\"00400554\":{\"vr\":\"UI\",\"Value\":[");
				archive_metadata_write_json_string(&writer, specimen.uid);
				archive_metadata_write(&writer, "]}");
				has_value = true;
			}
			if (specimen.short_description[0]) {
				if (has_value) archive_metadata_write(&writer, ",");
				archive_metadata_write(
					&writer, "\"00400600\":{\"vr\":\"LO\",\"Value\":[");
				archive_metadata_write_json_string(
					&writer, specimen.short_description);
				archive_metadata_write(&writer, "]}");
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
				if (has_value) archive_metadata_write(&writer, ",");
				archive_metadata_write(
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
								archive_metadata_write(
									&writer,
									wrote_step ? ",{\"00400612\":"
									"{\"vr\":\"SQ\",\"Value\":["
									: "{\"00400612\":{\"vr\":"
									"\"SQ\",\"Value\":[");
							} else {
								archive_metadata_write(&writer, ",");
							}
							if (!archive_metadata_write_preparation_content_item(
									shell, instance, item_index,
									&writer))
								return false;
							has_step_item = true;
						}
					}
					if (has_step_item) {
						archive_metadata_write(&writer, "]}}");
						wrote_step = true;
					}
				}
				archive_metadata_write(&writer, "]}");
			}
			archive_metadata_write(&writer, "}");
		}
		archive_metadata_write(&writer, "]}");
	}
	archive_metadata_write(
		&writer,
		instance->is_tiled_full ?
			",\"00209311\":{\"vr\":\"CS\",\"Value\":[\"TILED_FULL\"]}" :
			",\"00209311\":{\"vr\":\"CS\",\"Value\":[\"TILED_SPARSE\"]}");
	if (instance->pixel_spacing_row_mm > 0.0f &&
	    instance->pixel_spacing_column_mm > 0.0f) {
		archive_metadata_write_format(&writer,
			",\"52009229\":{\"vr\":\"SQ\",\"Value\":[{\"00289110\":{\"vr\":\"SQ\","
			"\"Value\":[{\"00280030\":{\"vr\":\"DS\",\"Value\":[%g,%g]}}]}}]}",
			instance->pixel_spacing_row_mm, instance->pixel_spacing_column_mm);
	}
	if (!instance->is_tiled_full) {
		archive_metadata_write(&writer, ",\"52009230\":{\"vr\":\"SQ\",\"Value\":[");
		for (u32 frame_index = 0; frame_index < instance->frame_count && !writer.failed; ++frame_index) {
			archive_shell_frame_t frame = {0};
			if (!archive_shell_get_frame(shell, instance, frame_index, &frame)) return false;
			if (instance->focal_plane_count > 1 && !(frame.flags & ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN))
				return false;
			if (frame_index) archive_metadata_write(&writer, ",");
			archive_metadata_write_format(&writer,
				"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{"
				"\"0048021E\":{\"vr\":\"SL\",\"Value\":[%u]},"
				"\"0048021F\":{\"vr\":\"SL\",\"Value\":[%u]}",
				frame.column_position, frame.row_position);
			if (frame.flags & ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN)
				archive_metadata_write_format(&writer,
					",\"0040072A\":{\"vr\":\"DS\",\"Value\":[%g]},"
					"\"0040073A\":{\"vr\":\"DS\",\"Value\":[%g]}",
					frame.x_offset_mm, frame.y_offset_mm);
			if (frame.flags & ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN)
				archive_metadata_write_format(&writer,
					",\"0040074A\":{\"vr\":\"DS\",\"Value\":[%g]}", frame.z_offset_um);
			archive_metadata_write(&writer, "}]}");
			if (frame.flags & ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN) {
				archive_shell_optical_path_t optical = {0};
				if (!archive_shell_get_optical_path(shell, instance, frame.optical_path_index, &optical))
					return false;
				archive_metadata_write(&writer,
					",\"00480207\":{\"vr\":\"SQ\",\"Value\":[{\"00480106\":{\"vr\":\"SH\",\"Value\":[");
				archive_metadata_write_json_string(&writer, optical.identifier);
				archive_metadata_write(&writer, "]}}]}");
			}
			archive_metadata_write(&writer, "}");
		}
		archive_metadata_write(&writer, "]}");
	}
	archive_metadata_write(&writer, array_wrapper ? "}]\n" : "}");
	if (writer.failed) return false;
	*size = writer.used;
	return true;
}

bool archive_metadata_get_capacity(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u64* capacity) {
	size_t size = 0;
	if (!capacity || !archive_metadata_write_instance(
			shell, instance, NULL, ARCHIVE_MAX_METADATA_SIZE, &size, true))
		return false;
	*capacity = size + 1;
	return true;
}
