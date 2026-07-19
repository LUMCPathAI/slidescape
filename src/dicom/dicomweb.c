/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "dicomweb.h"

#include "json.h"

static void* dicomweb_json_alloc(void* userdata, size_t size) {
	(void)userdata;
	return malloc(size);
}

static struct json_value_s* object_get(struct json_object_s* object, const char* name) {
	if (!object) return NULL;
	for (struct json_object_element_s* element = object->start; element; element = element->next) {
		if (strcmp(element->name->string, name) == 0) return element->value;
	}
	return NULL;
}

static struct json_value_s* first_value(struct json_object_s* dataset, const char* tag) {
	struct json_value_s* attribute_value = object_get(dataset, tag);
	if (!attribute_value || attribute_value->type != json_type_object) return NULL;
	struct json_value_s* values_value = object_get((struct json_object_s*)attribute_value->payload, "Value");
	if (!values_value || values_value->type != json_type_array) return NULL;
	struct json_array_s* values = (struct json_array_s*)values_value->payload;
	return values->start ? values->start->value : NULL;
}

static struct json_array_s* attribute_values(struct json_object_s* dataset, const char* tag) {
	struct json_value_s* attribute_value = object_get(dataset, tag);
	if (!attribute_value || attribute_value->type != json_type_object) return NULL;
	struct json_value_s* values_value = object_get((struct json_object_s*)attribute_value->payload, "Value");
	if (!values_value || values_value->type != json_type_array) return NULL;
	return (struct json_array_s*)values_value->payload;
}

static struct json_value_s* value_at(struct json_object_s* dataset, const char* tag, i32 index) {
	struct json_array_s* values = attribute_values(dataset, tag);
	if (!values || index < 0) return NULL;
	struct json_array_element_s* element = values->start;
	for (i32 i = 0; element && i < index; ++i) element = element->next;
	return element ? element->value : NULL;
}

static const char* scalar_string(struct json_value_s* value) {
	if (!value) return NULL;
	if (value->type == json_type_string) return ((struct json_string_s*)value->payload)->string;
	if (value->type == json_type_number) return ((struct json_number_s*)value->payload)->number;
	return NULL;
}

static bool copy_tag_string(struct json_object_s* dataset, const char* tag, char* dest, size_t dest_size) {
	const char* value = scalar_string(first_value(dataset, tag));
	if (!value || strlen(value) >= dest_size) return false;
	memcpy(dest, value, strlen(value) + 1);
	return true;
}

static bool copy_tag_string_at(struct json_object_s* dataset, const char* tag, i32 index,
                               char* dest, size_t dest_size) {
	const char* value = scalar_string(value_at(dataset, tag, index));
	if (!value || strlen(value) >= dest_size) return false;
	memcpy(dest, value, strlen(value) + 1);
	return true;
}

static bool read_tag_i64(struct json_object_s* dataset, const char* tag, i64* dest) {
	const char* value = scalar_string(first_value(dataset, tag));
	if (!value) return false;
	char* end = NULL;
	long long parsed = strtoll(value, &end, 10);
	if (end == value || *end != '\0') return false;
	*dest = (i64)parsed;
	return true;
}

static struct json_value_s* find_attribute_recursive(struct json_value_s* value, const char* tag, i32 depth) {
	if (!value || depth > 12) return NULL;
	if (value->type == json_type_object) {
		struct json_object_s* object = (struct json_object_s*)value->payload;
		struct json_value_s* match = object_get(object, tag);
		if (match) return match;
		for (struct json_object_element_s* element = object->start; element; element = element->next) {
			match = find_attribute_recursive(element->value, tag, depth + 1);
			if (match) return match;
		}
	} else if (value->type == json_type_array) {
		struct json_array_s* array = (struct json_array_s*)value->payload;
		for (struct json_array_element_s* element = array->start; element; element = element->next) {
			struct json_value_s* match = find_attribute_recursive(element->value, tag, depth + 1);
			if (match) return match;
		}
	}
	return NULL;
}

static bool read_pixel_spacing(struct json_value_s* dataset_value, float* mpp_x, float* mpp_y) {
	struct json_value_s* attribute = find_attribute_recursive(dataset_value, "00280030", 0);
	if (!attribute || attribute->type != json_type_object) return false;
	struct json_value_s* values_value = object_get((struct json_object_s*)attribute->payload, "Value");
	if (!values_value || values_value->type != json_type_array) return false;
	struct json_array_element_s* element = ((struct json_array_s*)values_value->payload)->start;
	if (!element) return false;
	const char* row_spacing = scalar_string(element->value);
	element = element->next;
	if (!element) return false;
	const char* column_spacing = scalar_string(element->value);
	if (!row_spacing || !column_spacing) return false;
	*mpp_y = (float)(atof(row_spacing) * 1000.0);
	*mpp_x = (float)(atof(column_spacing) * 1000.0);
	return *mpp_x > 0.0f && *mpp_y > 0.0f;
}

static bool read_position_value(struct json_object_s* frame, const char* tag, i64* value) {
	struct json_value_s* plane_position_attribute = object_get(frame, "0048021A");
	if (!plane_position_attribute || plane_position_attribute->type != json_type_object) return false;
	struct json_value_s* plane_position_values = object_get(
		(struct json_object_s*)plane_position_attribute->payload, "Value");
	if (!plane_position_values || plane_position_values->type != json_type_array) return false;
	struct json_array_element_s* first = ((struct json_array_s*)plane_position_values->payload)->start;
	if (!first || first->value->type != json_type_object) return false;
	return read_tag_i64((struct json_object_s*)first->value->payload, tag, value);
}

static bool build_frame_map(struct json_object_s* dataset, dicomweb_instance_metadata_t* out) {
	u64 width_in_tiles = ((u64)out->total_pixel_matrix_columns + out->columns - 1) / out->columns;
	u64 height_in_tiles = ((u64)out->total_pixel_matrix_rows + out->rows - 1) / out->rows;
	u64 tile_count = width_in_tiles * height_in_tiles;
	if (!tile_count || tile_count > UINT32_MAX) return false;
	out->frame_numbers = (u32*)calloc((size_t)tile_count, sizeof(u32));
	if (!out->frame_numbers) return false;
	out->frame_map_count = (u32)tile_count;

	struct json_array_s* per_frame = attribute_values(dataset, "52009230");
	if (!per_frame || !per_frame->start) {
		if ((u64)out->number_of_frames > tile_count) return false;
		for (u32 i = 0; i < (u32)out->number_of_frames; ++i) {
			out->frame_numbers[i] = i + 1;
		}
		return true;
	}

	u32 one_based_frame = 1;
	for (struct json_array_element_s* element = per_frame->start;
	     element; element = element->next, ++one_based_frame) {
		if (element->value->type != json_type_object) continue;
		i64 column = 0;
		i64 row = 0;
		struct json_object_s* frame = (struct json_object_s*)element->value->payload;
		if (!read_position_value(frame, "0048021E", &column) ||
		    !read_position_value(frame, "0048021F", &row) ||
		    column <= 0 || row <= 0) continue;
		u64 tile_x = (u64)(column - 1) / out->columns;
		u64 tile_y = (u64)(row - 1) / out->rows;
		if (tile_x >= width_in_tiles || tile_y >= height_in_tiles) continue;
		out->frame_numbers[tile_y * width_in_tiles + tile_x] = one_based_frame;
	}
	return one_based_frame - 1 == (u32)out->number_of_frames;
}

bool dicomweb_parse_instance_metadata(const char* json, size_t json_size,
                                      dicomweb_instance_metadata_t* out) {
	if (!json || !json_size || !out) return false;
	memset(out, 0, sizeof(*out));
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                          dicomweb_json_alloc, NULL, NULL);
	if (!root) return false;

	struct json_value_s* dataset_value = root;
	if (root->type == json_type_array) {
		struct json_array_s* array = (struct json_array_s*)root->payload;
		dataset_value = array->start ? array->start->value : NULL;
	}
	if (!dataset_value || dataset_value->type != json_type_object) {
		free(root);
		return false;
	}
	struct json_object_s* dataset = (struct json_object_s*)dataset_value->payload;
	i64 value = 0;
	bool valid = copy_tag_string(dataset, "0020000D", out->study_instance_uid, sizeof(out->study_instance_uid)) &&
	             copy_tag_string(dataset, "0020000E", out->series_instance_uid, sizeof(out->series_instance_uid)) &&
	             copy_tag_string(dataset, "00080018", out->sop_instance_uid, sizeof(out->sop_instance_uid)) &&
	             read_tag_i64(dataset, "00280008", &out->number_of_frames) &&
	             read_tag_i64(dataset, "00280010", &value);
	out->rows = (u32)value;
	valid = valid && read_tag_i64(dataset, "00280011", &value);
	out->columns = (u32)value;
	valid = valid && read_tag_i64(dataset, "00480006", &value);
	out->total_pixel_matrix_columns = (u32)value;
	valid = valid && read_tag_i64(dataset, "00480007", &value);
	out->total_pixel_matrix_rows = (u32)value;
	if (read_tag_i64(dataset, "00280002", &value)) out->samples_per_pixel = (u16)value;
	copy_tag_string(dataset, "00280004", out->photometric_interpretation,
	                sizeof(out->photometric_interpretation));

	struct json_value_s* image_type_attribute = object_get(dataset, "00080008");
	if (image_type_attribute && image_type_attribute->type == json_type_object) {
		struct json_value_s* values_value = object_get((struct json_object_s*)image_type_attribute->payload, "Value");
		if (values_value && values_value->type == json_type_array) {
			struct json_array_element_s* item = ((struct json_array_s*)values_value->payload)->start;
			for (i32 i = 0; item && i < 2; ++i) item = item->next;
			const char* flavor = item ? scalar_string(item->value) : NULL;
			if (flavor && strlen(flavor) < sizeof(out->image_flavor)) {
				memcpy(out->image_flavor, flavor, strlen(flavor) + 1);
			}
		}
	}
	const char* organization = scalar_string(first_value(dataset, "00209311"));
	u64 expected_frames = ((u64)out->total_pixel_matrix_columns + out->columns - 1) / out->columns;
	expected_frames *= ((u64)out->total_pixel_matrix_rows + out->rows - 1) / out->rows;
	out->is_tiled_full = organization ? strcmp(organization, "TILED_FULL") == 0
	                                  : expected_frames == (u64)out->number_of_frames;
	out->is_mpp_known = read_pixel_spacing(dataset_value, &out->mpp_x, &out->mpp_y);
	if (out->rows == 0 || out->columns == 0 || out->number_of_frames <= 0 ||
	    out->total_pixel_matrix_columns == 0 || out->total_pixel_matrix_rows == 0) {
		valid = false;
	}
	if (valid && !build_frame_map(dataset, out)) valid = false;
	free(root);
	if (!valid) dicomweb_instance_metadata_destroy(out);
	return valid;
}

void dicomweb_instance_metadata_destroy(dicomweb_instance_metadata_t* metadata) {
	if (!metadata) return;
	free(metadata->frame_numbers);
	metadata->frame_numbers = NULL;
	metadata->frame_map_count = 0;
}

i32 dicomweb_parse_instance_refs(const char* json, size_t json_size,
                                 dicomweb_instance_ref_t* out, i32 capacity) {
	if (!json || !json_size || (capacity > 0 && !out) || capacity < 0) return 0;
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                          dicomweb_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_array) {
		if (root) free(root);
		return 0;
	}
	i32 count = 0;
	struct json_array_s* array = (struct json_array_s*)root->payload;
	for (struct json_array_element_s* element = array->start;
	     element; element = element->next) {
		if (element->value->type != json_type_object) continue;
		struct json_object_s* dataset = (struct json_object_s*)element->value->payload;
		dicomweb_instance_ref_t ref = {0};
		if (copy_tag_string(dataset, "00080018", ref.sop_instance_uid,
		                    sizeof(ref.sop_instance_uid))) {
			copy_tag_string(dataset, "0020000D", ref.study_instance_uid,
			                sizeof(ref.study_instance_uid));
			copy_tag_string(dataset, "0020000E", ref.series_instance_uid,
			                sizeof(ref.series_instance_uid));
			copy_tag_string(dataset, "00080016", ref.sop_class_uid,
			                sizeof(ref.sop_class_uid));
			copy_tag_string_at(dataset, "00080008", 2, ref.image_flavor,
			                   sizeof(ref.image_flavor));
			copy_tag_string(dataset, "00200013", ref.instance_number,
			                sizeof(ref.instance_number));
			copy_tag_string(dataset, "00280008", ref.number_of_frames,
			                sizeof(ref.number_of_frames));
			copy_tag_string(dataset, "00209161", ref.concatenation_uid,
			                sizeof(ref.concatenation_uid));
			copy_tag_string(dataset, "00209162", ref.in_concatenation_number,
			                sizeof(ref.in_concatenation_number));
			copy_tag_string(dataset, "00209163", ref.in_concatenation_total_number,
			                sizeof(ref.in_concatenation_total_number));
			copy_tag_string(dataset, "00480006", ref.total_pixel_matrix_columns,
			                sizeof(ref.total_pixel_matrix_columns));
			copy_tag_string(dataset, "00480007", ref.total_pixel_matrix_rows,
			                sizeof(ref.total_pixel_matrix_rows));
			if (count < capacity) out[count] = ref;
			++count;
		}
	}
	free(root);
	return out ? MIN(count, capacity) : count;
}

i32 dicomweb_count_instance_refs(const char* json, size_t json_size) {
	if (!json || !json_size) return 0;
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                          dicomweb_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_array) {
		if (root) free(root);
		return 0;
	}
	i32 count = 0;
	struct json_array_s* array = (struct json_array_s*)root->payload;
	for (struct json_array_element_s* element = array->start;
	     element; element = element->next) {
		if (element->value->type != json_type_object) continue;
		struct json_object_s* dataset = (struct json_object_s*)element->value->payload;
		char instance_uid[65];
		if (copy_tag_string(dataset, "00080018", instance_uid, sizeof(instance_uid))) {
			++count;
		}
	}
	free(root);
	return count;
}

static i32 parse_qido_refs(const char* json, size_t json_size, void* out, i32 capacity,
                           bool studies) {
	if (!json || !json_size || (capacity > 0 && !out) || capacity < 0) return 0;
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                          dicomweb_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_array) {
		if (root) free(root);
		return 0;
	}
	i32 count = 0;
	struct json_array_s* array = (struct json_array_s*)root->payload;
	for (struct json_array_element_s* element = array->start;
	     element; element = element->next) {
		if (element->value->type != json_type_object) continue;
		struct json_object_s* dataset = (struct json_object_s*)element->value->payload;
		if (studies) {
			dicomweb_study_ref_t ref = {0};
			if (!copy_tag_string(dataset, "0020000D", ref.study_instance_uid,
			                     sizeof(ref.study_instance_uid))) continue;
			copy_tag_string(dataset, "00080020", ref.study_date, sizeof(ref.study_date));
			copy_tag_string(dataset, "00081030", ref.description, sizeof(ref.description));
			if (count < capacity) ((dicomweb_study_ref_t*)out)[count] = ref;
			++count;
		} else {
			dicomweb_series_ref_t ref = {0};
			if (!copy_tag_string(dataset, "0020000E", ref.series_instance_uid,
			                     sizeof(ref.series_instance_uid))) continue;
			copy_tag_string(dataset, "00200011", ref.series_number, sizeof(ref.series_number));
			copy_tag_string(dataset, "00080060", ref.modality, sizeof(ref.modality));
			copy_tag_string(dataset, "0008103E", ref.description, sizeof(ref.description));
			if (count < capacity) ((dicomweb_series_ref_t*)out)[count] = ref;
			++count;
		}
	}
	free(root);
	return out ? MIN(count, capacity) : count;
}

i32 dicomweb_parse_study_refs(const char* json, size_t json_size,
                              dicomweb_study_ref_t* out, i32 capacity) {
	return parse_qido_refs(json, json_size, out, capacity, true);
}

i32 dicomweb_parse_series_refs(const char* json, size_t json_size,
                               dicomweb_series_ref_t* out, i32 capacity) {
	return parse_qido_refs(json, json_size, out, capacity, false);
}

i32 dicomweb_count_study_refs(const char* json, size_t json_size) {
	return parse_qido_refs(json, json_size, NULL, 0, true);
}

i32 dicomweb_count_series_refs(const char* json, size_t json_size) {
	return parse_qido_refs(json, json_size, NULL, 0, false);
}

static const u8* find_bytes(const u8* haystack, size_t haystack_size,
                            const char* needle, size_t needle_size) {
	if (!needle_size || needle_size > haystack_size) return NULL;
	for (size_t i = 0; i <= haystack_size - needle_size; ++i) {
		if (memcmp(haystack + i, needle, needle_size) == 0) return haystack + i;
	}
	return NULL;
}

static bool copy_parameter(const char* text, const char* name, char* dest, size_t dest_size) {
	const char* start = strstr(text, name);
	if (!start) return false;
	start += strlen(name);
	if (*start == '"') ++start;
	const char* end = start;
	while (*end && *end != '"' && *end != ';' && *end != '\r' && *end != '\n') ++end;
	size_t length = (size_t)(end - start);
	if (!length || length >= dest_size) return false;
	memcpy(dest, start, length);
	dest[length] = '\0';
	return true;
}

bool dicomweb_parse_multipart_frame(const u8* body, size_t body_size,
                                    const char* response_content_type,
                                    dicomweb_frame_t* out) {
	if (!body || !body_size || !response_content_type || !out) return false;
	memset(out, 0, sizeof(*out));
	char boundary[256];
	if (!copy_parameter(response_content_type, "boundary=", boundary, sizeof(boundary))) return false;
	char marker[260];
	i32 marker_length = snprintf(marker, sizeof(marker), "--%s", boundary);
	if (marker_length <= 2 || marker_length >= sizeof(marker)) return false;
	const u8* part = find_bytes(body, body_size, marker, (size_t)marker_length);
	if (!part) return false;
	const u8* headers = part + marker_length;
	size_t remaining = body_size - (size_t)(headers - body);
	const u8* payload = find_bytes(headers, remaining, "\r\n\r\n", 4);
	if (!payload) return false;
	payload += 4;
	remaining = body_size - (size_t)(payload - body);
	char end_marker[264];
	i32 end_marker_length = snprintf(end_marker, sizeof(end_marker), "\r\n--%s", boundary);
	const u8* end = find_bytes(payload, remaining, end_marker, (size_t)end_marker_length);
	if (!end || end <= payload) return false;

	size_t header_size = (size_t)(payload - 4 - headers);
	char* header_text = (char*)malloc(header_size + 1);
	if (!header_text) return false;
	memcpy(header_text, headers, header_size);
	header_text[header_size] = '\0';
	const char* content_type = strstr(header_text, "Content-Type:");
	if (!content_type) content_type = strstr(header_text, "content-type:");
	if (content_type) {
		content_type += strlen("Content-Type:");
		while (*content_type == ' ') ++content_type;
		const char* media_end = content_type;
		while (*media_end && *media_end != ';' && *media_end != '\r') ++media_end;
		size_t length = (size_t)(media_end - content_type);
		if (length < sizeof(out->media_type)) {
			memcpy(out->media_type, content_type, length);
			out->media_type[length] = '\0';
		}
		copy_parameter(content_type, "transfer-syntax=", out->transfer_syntax_uid,
		               sizeof(out->transfer_syntax_uid));
	}
	free(header_text);
	out->data = payload;
	out->size = (size_t)(end - payload);
	return out->size > 0;
}

static bool build_path(char* dest, size_t dest_size, const char* root,
                       const char* study_uid, const char* series_uid,
                       const char* instance_uid, const char* suffix) {
	if (!dest || !dest_size || !root || !study_uid || !series_uid || !instance_uid) return false;
	size_t root_length = strlen(root);
	const char* separator = root_length && root[root_length - 1] == '/' ? "" : "/";
	i32 length = snprintf(dest, dest_size,
	                      "%s%sstudies/%s/series/%s/instances/%s/%s",
	                      root, separator, study_uid, series_uid, instance_uid, suffix);
	return length > 0 && (size_t)length < dest_size;
}

bool dicomweb_build_instance_metadata_path(char* dest, size_t dest_size,
                                           const char* root, const char* study_uid,
                                           const char* series_uid, const char* instance_uid) {
	return build_path(dest, dest_size, root, study_uid, series_uid, instance_uid, "metadata");
}

bool dicomweb_build_frame_path(char* dest, size_t dest_size,
                               const char* root, const char* study_uid,
                               const char* series_uid, const char* instance_uid,
                               u32 one_based_frame_number) {
	if (!one_based_frame_number) return false;
	char suffix[64];
	snprintf(suffix, sizeof(suffix), "frames/%u", one_based_frame_number);
	return build_path(dest, dest_size, root, study_uid, series_uid, instance_uid, suffix);
}

static bool is_dicom_uid(const char* value, size_t length) {
	if (!value || !length || length > 64 || value[0] == '.' || value[length - 1] == '.') return false;
	for (size_t i = 0; i < length; ++i) {
		if ((value[i] < '0' || value[i] > '9') && value[i] != '.') return false;
		if (value[i] == '.' && i > 0 && value[i - 1] == '.') return false;
	}
	return true;
}

bool dicomweb_resolve_series_url(char* dest, size_t dest_size, const char* url) {
	if (!dest || !dest_size || !url) return false;
	static const char idc_viewer_prefix[] =
		"https://viewer.imaging.datacommons.cancer.gov/slim/studies/";
	if (strncmp(url, idc_viewer_prefix, sizeof(idc_viewer_prefix) - 1) != 0) {
		size_t length = strlen(url);
		if (!length || length >= dest_size) return false;
		memcpy(dest, url, length + 1);
		return true;
	}

	const char* study = url + sizeof(idc_viewer_prefix) - 1;
	const char* series_marker = strstr(study, "/series/");
	if (!series_marker) return false;
	const char* series = series_marker + strlen("/series/");
	const char* series_end = strpbrk(series, "/?#");
	if (!series_end) series_end = series + strlen(series);
	size_t study_length = (size_t)(series_marker - study);
	size_t series_length = (size_t)(series_end - series);
	if (!is_dicom_uid(study, study_length) || !is_dicom_uid(series, series_length)) return false;

	static const char idc_dicomweb_base[] =
		"https://proxy.imaging.datacommons.cancer.gov/current/"
		"viewer-only-no-downloads-see-tinyurl-dot-com-slash-3j3d9jyp/dicomWeb";
	i32 length = snprintf(dest, dest_size, "%s/studies/%.*s/series/%.*s",
	                      idc_dicomweb_base, (i32)study_length, study,
	                      (i32)series_length, series);
	return length > 0 && (size_t)length < dest_size;
}
