/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "ssi.h"

#include <ctype.h>

#include "json.h"

static void* ssi_json_alloc(void* userdata, size_t size) {
	(void)userdata;
	/* Keep json.h allocations in the same allocator domain as this translation unit. */
	return malloc(size);
}

static struct json_value_s* ssi_object_get(struct json_object_s* object, const char* name) {
	for (struct json_object_element_s* element = object ? object->start : NULL;
	     element; element = element->next) {
		if (strcmp(element->name->string, name) == 0) return element->value;
	}
	return NULL;
}

static const char* ssi_scalar_string(struct json_value_s* value) {
	if (!value) return NULL;
	if (value->type == json_type_string) {
		return ((struct json_string_s*)value->payload)->string;
	}
	if (value->type == json_type_number) {
		return ((struct json_number_s*)value->payload)->number;
	}
	return NULL;
}

static bool ssi_read_i64(struct json_object_s* object, const char* name, i64* dest) {
	const char* text = ssi_scalar_string(ssi_object_get(object, name));
	if (!text || !text[0]) return false;
	char* end = NULL;
	long long value = strtoll(text, &end, 10);
	if (!end || *end != '\0') return false;
	*dest = (i64)value;
	return true;
}

static bool ssi_read_i32(struct json_object_s* object, const char* name, i32* dest) {
	i64 value = 0;
	if (!ssi_read_i64(object, name, &value) || value < INT32_MIN || value > INT32_MAX) {
		return false;
	}
	*dest = (i32)value;
	return true;
}

static bool ssi_read_f32(struct json_object_s* object, const char* name, float* dest) {
	const char* text = ssi_scalar_string(ssi_object_get(object, name));
	if (!text || !text[0]) return false;
	char* end = NULL;
	float value = strtof(text, &end);
	if (!end || *end != '\0' || !isfinite(value)) return false;
	*dest = value;
	return true;
}

static void ssi_copy_json_string(struct json_object_s* object, const char* name,
		char* dest, size_t dest_size) {
	const char* text = ssi_scalar_string(ssi_object_get(object, name));
	if (!dest || dest_size == 0) return;
	if (!text) {
		dest[0] = '\0';
		return;
	}
	snprintf(dest, dest_size, "%s", text);
}

static bool ssi_slide_id_is_valid(const char* id) {
	if (!id || strlen(id) != 32) return false;
	for (i32 i = 0; i < 32; ++i) {
		char c = id[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F'))) return false;
	}
	return true;
}

static bool ssi_host_is_loopback(const char* host) {
	return strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 ||
	       strcmp(host, "[::1]") == 0;
}

static bool ssi_ascii_equal_nocase_n(const char* a, size_t a_size,
		const char* b, size_t b_size) {
	if (a_size != b_size) return false;
	for (size_t i = 0; i < a_size; ++i) {
		if (tolower((u8)a[i]) != tolower((u8)b[i])) return false;
	}
	return true;
}

static bool ssi_ascii_starts_with_nocase(const char* text, const char* prefix) {
	if (!text || !prefix) return false;
	size_t prefix_size = strlen(prefix);
	return strlen(text) >= prefix_size &&
	       ssi_ascii_equal_nocase_n(text, prefix_size, prefix, prefix_size);
}

static i32 ssi_hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool ssi_query_value(const char* query, const char* wanted_name,
		char* dest, size_t dest_size) {
	if (!query || !wanted_name || !dest || dest_size == 0) return false;
	for (const char* item = query; *item;) {
		const char* item_end = strchr(item, '&');
		if (!item_end) item_end = item + strlen(item);
		const char* equals = (const char*)memchr(item, '=', (size_t)(item_end - item));
		if (equals && ssi_ascii_equal_nocase_n(item, (size_t)(equals - item),
		                                      wanted_name, strlen(wanted_name))) {
			size_t used = 0;
			for (const char* p = equals + 1; p < item_end; ++p) {
				char decoded = *p;
				if (*p == '%' && item_end - p >= 3) {
					i32 high = ssi_hex_digit(p[1]);
					i32 low = ssi_hex_digit(p[2]);
					if (high < 0 || low < 0) return false;
					decoded = (char)((high << 4) | low);
					p += 2;
				} else if (*p == '+') {
					decoded = ' ';
				}
				if (decoded == '\0' || used + 1 >= dest_size) return false;
				dest[used++] = decoded;
			}
			dest[used] = '\0';
			return used > 0;
		}
		item = *item_end ? item_end + 1 : item_end;
	}
	return false;
}

bool ssi_parse_endpoint(const char* api_url, ssi_endpoint_t* endpoint) {
	if (!api_url || !endpoint) return false;
	memset(endpoint, 0, sizeof(*endpoint));

	const char* authority = NULL;
	if (strncmp(api_url, "https://", 8) == 0) {
		endpoint->use_tls = true;
		endpoint->port = 443;
		authority = api_url + 8;
	} else if (strncmp(api_url, "http://", 7) == 0) {
		endpoint->use_tls = false;
		endpoint->port = 80;
		authority = api_url + 7;
	} else {
		return false;
	}

	const char* path = strchr(authority, '/');
	const char* authority_end = path ? path : api_url + strlen(api_url);
	if (authority_end == authority || authority_end - authority >= sizeof(endpoint->host_header)) {
		return false;
	}
	memcpy(endpoint->host_header, authority, (size_t)(authority_end - authority));
	endpoint->host_header[authority_end - authority] = '\0';

	const char* port_separator = strrchr(endpoint->host_header, ':');
	size_t hostname_length = strlen(endpoint->host_header);
	if (port_separator && endpoint->host_header[0] != '[') {
		char* end = NULL;
		long port = strtol(port_separator + 1, &end, 10);
		if (!end || *end != '\0' || port <= 0 || port > 65535) return false;
		endpoint->port = (i32)port;
		hostname_length = (size_t)(port_separator - endpoint->host_header);
	}
	if (hostname_length == 0 || hostname_length >= sizeof(endpoint->hostname)) return false;
	memcpy(endpoint->hostname, endpoint->host_header, hostname_length);
	endpoint->hostname[hostname_length] = '\0';
	for (const u8* p = (const u8*)endpoint->hostname; *p; ++p) {
		if (*p <= 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return false;
	}
	if (!endpoint->use_tls && !ssi_host_is_loopback(endpoint->hostname)) return false;

	if (path && path[0]) {
		size_t root_length = strlen(path);
		while (root_length > 1 && path[root_length - 1] == '/') --root_length;
		if (root_length >= sizeof(endpoint->root)) return false;
		memcpy(endpoint->root, path, root_length);
		endpoint->root[root_length] = '\0';
	} else {
		snprintf(endpoint->root, sizeof(endpoint->root), "/SSI");
	}
	return true;
}

bool ssi_parse_json_string(const char* json, size_t json_size, char* dest, size_t dest_size) {
	if (!json || !json_size || !dest || dest_size == 0) return false;
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                         ssi_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_string) {
		free(root);
		return false;
	}
	struct json_string_s* value = (struct json_string_s*)root->payload;
	if (value->string_size == 0 || value->string_size >= dest_size) {
		free(root);
		return false;
	}
	memcpy(dest, value->string, value->string_size);
	dest[value->string_size] = '\0';
	free(root);
	return true;
}

bool ssi_parse_slide_properties(const char* json, size_t json_size,
		ssi_slide_properties_t* properties) {
	if (!json || !json_size || !properties) return false;
	memset(properties, 0, sizeof(*properties));
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                         ssi_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_object) {
		free(root);
		return false;
	}
	struct json_object_s* object = (struct json_object_s*)root->payload;
	ssi_copy_json_string(object, "OriginalName", properties->original_name,
	                     sizeof(properties->original_name));
	ssi_copy_json_string(object, "FolderId", properties->folder_id,
	                     sizeof(properties->folder_id));
	bool valid = ssi_read_i64(object, "Width", &properties->width) &&
	             ssi_read_i64(object, "Height", &properties->height) &&
	             ssi_read_i32(object, "MagnificationLevelMax", &properties->magnification_level_max) &&
	             ssi_read_i32(object, "TileXMinNative", &properties->tile_x_min_native) &&
	             ssi_read_i32(object, "TileXMaxNative", &properties->tile_x_max_native) &&
	             ssi_read_i32(object, "TileYMinNative", &properties->tile_y_min_native) &&
	             ssi_read_i32(object, "TileYMaxNative", &properties->tile_y_max_native);
	ssi_read_f32(object, "MicronPerPixelWidth", &properties->mpp_x);
	ssi_read_f32(object, "MicronPerPixelHeight", &properties->mpp_y);
	ssi_read_i32(object, "BitPerChannel", &properties->bits_per_channel);
	ssi_read_i32(object, "ChannelCount", &properties->channel_count);
	ssi_read_i32(object, "FocusLevelMin", &properties->focus_level_min);
	ssi_read_i32(object, "FocusLevelMax", &properties->focus_level_max);
	const char* illumination = ssi_scalar_string(ssi_object_get(object, "SlideIlluminationType"));
	properties->is_brightfield = illumination && strcmp(illumination, "BRIGHTFIELD") == 0;
	free(root);
	return valid && properties->width > 0 && properties->height > 0 &&
	       properties->magnification_level_max >= 0 &&
	       properties->magnification_level_max < 10 &&
	       properties->tile_x_min_native >= 0 && properties->tile_y_min_native >= 0 &&
	       properties->tile_x_max_native >= properties->tile_x_min_native &&
	       properties->tile_y_max_native >= properties->tile_y_min_native;
}

bool ssi_parse_slide_attribute_name(const char* json, size_t json_size,
		char* name, size_t name_size) {
	if (!json || !json_size || !name || name_size == 0) return false;
	name[0] = '\0';
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                         ssi_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_object) {
		free(root);
		return false;
	}
	ssi_copy_json_string((struct json_object_s*)root->payload, "Name", name, name_size);
	free(root);
	return name[0] != '\0';
}

bool ssi_parse_folder_content(const char* json, size_t json_size,
		ssi_slide_ref_t** slides, i32* slide_count) {
	if (!json || !json_size || !slides || !slide_count) return false;
	*slides = NULL;
	*slide_count = 0;
	struct json_value_s* root = json_parse_ex(json, json_size, json_parse_flags_default,
	                                         ssi_json_alloc, NULL, NULL);
	if (!root || root->type != json_type_object) {
		free(root);
		return false;
	}
	struct json_value_s* slides_value = ssi_object_get(
		(struct json_object_s*)root->payload, "Slides");
	if (!slides_value || slides_value->type == json_type_null) {
		free(root);
		return true;
	}
	if (slides_value->type != json_type_array) {
		free(root);
		return false;
	}
	struct json_array_s* array = (struct json_array_s*)slides_value->payload;
	if (array->length > 4096 || array->length > INT32_MAX) {
		free(root);
		return false;
	}
	ssi_slide_ref_t* parsed = array->length
	                          ? (ssi_slide_ref_t*)calloc(array->length, sizeof(*parsed))
	                          : NULL;
	if (array->length && !parsed) {
		free(root);
		return false;
	}
	i32 count = 0;
	for (struct json_array_element_s* element = array->start;
	     element; element = element->next) {
		if (!element->value || element->value->type != json_type_object) continue;
		ssi_slide_ref_t candidate = {0};
		struct json_object_s* object = (struct json_object_s*)element->value->payload;
		ssi_copy_json_string(object, "Id", candidate.id, sizeof(candidate.id));
		ssi_copy_json_string(object, "Name", candidate.name, sizeof(candidate.name));
		if (!ssi_slide_id_is_valid(candidate.id)) continue;
		if (!candidate.name[0]) snprintf(candidate.name, sizeof(candidate.name),
		                                 "%s", candidate.id);
		parsed[count++] = candidate;
	}
	free(root);
	if (count == 0) {
		free(parsed);
		parsed = NULL;
	}
	*slides = parsed;
	*slide_count = count;
	return true;
}

bool ssi_get_level_geometry(const ssi_slide_properties_t* properties, i32 level,
		ssi_level_geometry_t* geometry) {
	if (!properties || !geometry || level < 0 ||
	    level > properties->magnification_level_max || level >= 10) return false;
	i32 scale = 1 << level;
	i32 tile_x_min = properties->tile_x_min_native / scale;
	i32 tile_x_max = properties->tile_x_max_native / scale;
	i32 tile_y_min = properties->tile_y_min_native / scale;
	i32 tile_y_max = properties->tile_y_max_native / scale;
	if (tile_x_max < tile_x_min || tile_y_max < tile_y_min) return false;
	i64 level_width = (properties->width + scale - 1) / scale;
	i64 level_height = (properties->height + scale - 1) / scale;
	i64 width_in_tiles = (level_width + 255) / 256;
	i64 height_in_tiles = (level_height + 255) / 256;
	if (width_in_tiles <= 0 || width_in_tiles > INT32_MAX ||
	    height_in_tiles <= 0 || height_in_tiles > INT32_MAX ||
	    tile_x_max >= width_in_tiles || tile_y_max >= height_in_tiles) return false;
	geometry->tile_x_min = tile_x_min;
	geometry->tile_x_max = tile_x_max;
	geometry->tile_y_min = tile_y_min;
	geometry->tile_y_max = tile_y_max;
	/* Retain the full common canvas; tiles outside the occupied bounds stay empty. */
	geometry->width_in_tiles = (i32)width_in_tiles;
	geometry->height_in_tiles = (i32)height_in_tiles;
	return geometry->width_in_tiles > 0 && geometry->height_in_tiles > 0;
}

bool ssi_build_tile_path(char* dest, size_t dest_size, const ssi_remote_image_t* remote,
		i32 level, i32 tile_x, i32 tile_y) {
	return ssi_build_tile_rectangle_path(dest, dest_size, remote, level,
	                                     tile_x, tile_y, tile_x, tile_y);
}

bool ssi_build_tile_rectangle_path(char* dest, size_t dest_size,
		const ssi_remote_image_t* remote, i32 level,
		i32 tile_x_min, i32 tile_y_min, i32 tile_x_max, i32 tile_y_max) {
	if (!dest || dest_size == 0 || !remote || !remote->slide_token[0] || level < 0 ||
	    level > remote->properties.magnification_level_max ||
	    tile_x_min < 0 || tile_y_min < 0 ||
	    tile_x_max < tile_x_min || tile_y_max < tile_y_min) {
		return false;
	}
	ssi_level_geometry_t geometry = {0};
	if (!ssi_get_level_geometry(&remote->properties, level, &geometry) ||
	    tile_x_min < geometry.tile_x_min || tile_x_max > geometry.tile_x_max ||
	    tile_y_min < geometry.tile_y_min || tile_y_max > geometry.tile_y_max) return false;
	i32 length = snprintf(dest, dest_size,
	                      "%s/api/simpleslideinterface/v1/slide/%s/tile"
	                      "?encoding=JPEG_LOSSY&x1=%d&y1=%d&x2=%d&y2=%d"
	                      "&magnification=%d&channel_triplet_index=0&output_quality=90",
	                      remote->endpoint.root, remote->slide_token,
	                      tile_x_min, tile_y_min, tile_x_max, tile_y_max, level);
	return length > 0 && (size_t)length < dest_size;
}

bool ssi_uri_is_supported(const char* uri) {
	return ssi_ascii_starts_with_nocase(uri, "ssi:") ||
	       ssi_ascii_starts_with_nocase(uri, "mview:");
}

bool ssi_parse_uri_slide_guid(const char* uri, char* slide_guid, size_t slide_guid_size) {
	if (!ssi_ascii_starts_with_nocase(uri, "ssi:") || !slide_guid || slide_guid_size < 33) {
		return false;
	}
	const char* guid = uri + 4;
	if (strlen(guid) != 32) return false;
	for (i32 i = 0; i < 32; ++i) {
		char c = guid[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F'))) return false;
	}
	memcpy(slide_guid, guid, 33);
	return true;
}

bool ssi_parse_mview_uri(const char* uri, ssi_uri_config_t* config) {
	if (!uri || !config || !ssi_ascii_starts_with_nocase(uri, "mview:OfflTelSlide?")) {
		return false;
	}
	memset(config, 0, sizeof(*config));
	const char* query = strchr(uri, '?');
	char host[512];
	char secure[16];
	if (!query || !ssi_query_value(query + 1, "HUBIP", host, sizeof(host)) ||
	    !ssi_query_value(query + 1, "HUBSecure", secure, sizeof(secure)) ||
	    !ssi_query_value(query + 1, "SldID", config->slide_guid,
	                     sizeof(config->slide_guid)) ||
	    !ssi_ascii_equal_nocase_n(secure, strlen(secure), "true", 4)) return false;
	for (const u8* p = (const u8*)host; *p; ++p) {
		if (*p <= 0x20 || *p == 0x7f || *p == '/' || *p == '\\' || *p == '?' || *p == '#') {
			return false;
		}
	}
	char checked_guid[64];
	char ssi_uri[80];
	i32 uri_length = snprintf(ssi_uri, sizeof(ssi_uri), "ssi:%s", config->slide_guid);
	if (uri_length <= 0 || uri_length >= sizeof(ssi_uri) ||
	    !ssi_parse_uri_slide_guid(ssi_uri, checked_guid, sizeof(checked_guid))) return false;
	ssi_query_value(query + 1, "Capt", config->caption, sizeof(config->caption));
	if (ssi_query_value(query + 1, "AccID", config->session_id,
	                    sizeof(config->session_id))) {
		for (const u8* p = (const u8*)config->session_id; *p; ++p) {
			if (!(isalnum(*p) || *p == '-' || *p == '_')) return false;
		}
	}
	i32 api_length = snprintf(config->api_url, sizeof(config->api_url),
	                          "https://%s/SSI", host);
	i32 center_length = snprintf(config->slide_center_url,
	                             sizeof(config->slide_center_url), "https://%s", host);
	ssi_endpoint_t endpoint;
	return api_length > 0 && api_length < sizeof(config->api_url) &&
	       center_length > 0 && center_length < sizeof(config->slide_center_url) &&
	       ssi_parse_endpoint(config->api_url, &endpoint);
}
