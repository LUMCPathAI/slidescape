/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "ssi.h"

#include "image.h"
#include "jpeg_decoder.h"
#include "remote.h"
#include "ssi_transport.h"
#include "stringutils.h"
#include "viewer.h"

extern bool show_slide_tray_window;

static char ssi_last_status[512];

static void ssi_set_status(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(ssi_last_status, sizeof(ssi_last_status), format, args);
	va_end(args);
	ssi_last_status[sizeof(ssi_last_status) - 1] = '\0';
}

const char* ssi_get_last_status(void) {
	return ssi_last_status;
}

static void ssi_secure_zero(void* memory, size_t size) {
	volatile u8* p = (volatile u8*)memory;
	while (size--) *p++ = 0;
}

static bool ssi_identifier_is_safe(const char* identifier) {
	if (!identifier || !identifier[0]) return false;
	for (const u8* p = (const u8*)identifier; *p; ++p) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
	}
	return true;
}

static bool ssi_json_escape(char* dest, size_t dest_size, const char* source) {
	if (!dest || dest_size == 0 || !source) return false;
	size_t used = 0;
	for (const u8* p = (const u8*)source; *p; ++p) {
		const char* escape = NULL;
		char unicode_escape[7];
		switch (*p) {
			case '"': escape = "\\\""; break;
			case '\\': escape = "\\\\"; break;
			case '\b': escape = "\\b"; break;
			case '\f': escape = "\\f"; break;
			case '\n': escape = "\\n"; break;
			case '\r': escape = "\\r"; break;
			case '\t': escape = "\\t"; break;
			default: break;
		}
		if (*p < 0x20) {
			snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *p);
			escape = unicode_escape;
		}
		if (escape) {
			size_t length = strlen(escape);
			if (used + length >= dest_size) return false;
			memcpy(dest + used, escape, length);
			used += length;
		} else {
			if (used + 1 >= dest_size) return false;
			dest[used++] = (char)*p;
		}
	}
	dest[used] = '\0';
	return true;
}

static bool ssi_query_escape(char* dest, size_t dest_size, const char* source) {
	static const char hex[] = "0123456789ABCDEF";
	if (!dest || dest_size == 0 || !source) return false;
	size_t used = 0;
	for (const u8* p = (const u8*)source; *p; ++p) {
		bool unreserved = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
		                  *p == '.' || *p == '~';
		size_t needed = unreserved ? 1 : 3;
		if (used + needed >= dest_size) return false;
		if (unreserved) {
			dest[used++] = (char)*p;
		} else {
			dest[used++] = '%';
			dest[used++] = hex[*p >> 4];
			dest[used++] = hex[*p & 0x0f];
		}
	}
	dest[used] = '\0';
	return true;
}

static http_response_t* ssi_request(const ssi_endpoint_t* endpoint,
		remote_http_method_t method, const char* path, const char* content_type,
		const void* body, size_t body_size, const char* accept) {
	remote_request_options_t options = {0};
	options.read_poll_timeout_ms = 250;
	options.write_timeout_seconds = 10.0f;
	options.idle_read_timeout_seconds = 30.0f;
	options.total_timeout_seconds = 60.0f;
	options.max_response_body_size = MEGABYTES(4);
	return ssi_transport_request(endpoint, NULL, SSI_TRANSPORT_FALLBACK_IF_BUSY,
	                             method, path, accept, content_type,
	                             body, body_size, &options);
}

static i32 ssi_elapsed_milliseconds(i64 start_clock) {
	float milliseconds = get_seconds_elapsed(start_clock, get_clock()) * 1000.0f;
	return milliseconds > 0.0f ? (i32)(milliseconds + 0.5f) : 0;
}

static http_response_t* ssi_initial_request(ssi_remote_image_t* remote,
		const char* operation,
		remote_http_method_t method, const char* path,
		const char* content_type, const void* body, size_t body_size,
		const char* accept) {
	i64 start_clock = get_clock();
	http_response_t* response = ssi_request(
		&remote->endpoint, method, path, content_type, body, body_size, accept);
	i32 elapsed_ms = ssi_elapsed_milliseconds(start_clock);
	remote->initial_request_count += 1;
	remote->initial_request_time_ms += elapsed_ms;
	if (response) remote->initial_response_bytes += response->content_length;
	console_print_verbose("SSI initial request %s: HTTP %d, %d ms, %zu bytes\n",
	                      operation, response ? response->status_code : 0, elapsed_ms,
	                      response ? response->content_length : 0);
	return response;
}

static bool ssi_response_succeeded(const http_response_t* response) {
	return response && response->status_code >= 200 && response->status_code < 300;
}

static bool ssi_create_session(ssi_remote_image_t* remote,
		const char* slide_center_url, const char* username, const char* password) {
	char escaped_url[1024];
	char escaped_username[1024];
	char escaped_password[2048];
	char body[4096];
	bool encoded = ssi_json_escape(escaped_url, sizeof(escaped_url), slide_center_url) &&
	               ssi_json_escape(escaped_username, sizeof(escaped_username), username) &&
	               ssi_json_escape(escaped_password, sizeof(escaped_password), password);
	if (!encoded) {
		ssi_secure_zero(escaped_password, sizeof(escaped_password));
		return false;
	}
	i32 body_length = snprintf(body, sizeof(body),
	                           "{\"slide_center_url\":\"%s\",\"username\":\"%s\","
	                           "\"password\":\"%s\"}",
	                           escaped_url, escaped_username, escaped_password);
	ssi_secure_zero(escaped_password, sizeof(escaped_password));
	if (body_length <= 0 || body_length >= sizeof(body)) {
		ssi_secure_zero(body, sizeof(body));
		return false;
	}
	char path[768];
	i32 path_length = snprintf(path, sizeof(path),
	                           "%s/api/simpleslideinterface/v1/session/create",
	                           remote->endpoint.root);
	if (path_length <= 0 || path_length >= sizeof(path)) {
		ssi_secure_zero(body, sizeof(body));
		return false;
	}
	http_response_t* response = ssi_initial_request(remote, "session/create",
	                                                REMOTE_HTTP_POST, path,
	                                                "application/json", body,
	                                                (size_t)body_length, "application/json");
	ssi_secure_zero(body, sizeof(body));
	if (!ssi_response_succeeded(response)) {
		ssi_set_status("SSI session creation failed (HTTP %d).",
		               response ? response->status_code : 0);
		console_print_error("%s\n", ssi_last_status);
		if (response) http_response_destroy(response);
		return false;
	}
	bool parsed = ssi_parse_json_string((const char*)response->buffer.data,
	                                    response->content_length, remote->session_id,
	                                    sizeof(remote->session_id));
	http_response_destroy(response);
	remote->session_open = parsed && ssi_identifier_is_safe(remote->session_id);
	if (!remote->session_open) ssi_set_status("SSI returned an invalid session handle.");
	return remote->session_open;
}

static bool ssi_connect_session(ssi_remote_image_t* remote,
		const char* slide_center_url,
		const char* existing_session_id) {
	if (!ssi_identifier_is_safe(existing_session_id)) return false;
	char escaped_url[2304];
	char escaped_session_id[384];
	if (!ssi_query_escape(escaped_url, sizeof(escaped_url), slide_center_url) ||
	    !ssi_query_escape(escaped_session_id, sizeof(escaped_session_id),
	                      existing_session_id)) return false;
	char path[4096];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/session/connect"
	                      "?slide_center_url=%s&session_id=%s",
	                      remote->endpoint.root, escaped_url, escaped_session_id);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote,
	                                                "session/connect",
	                                                REMOTE_HTTP_POST, path,
	                                                NULL, NULL, 0, "application/json");
	if (!ssi_response_succeeded(response)) {
		ssi_set_status("SSI could not connect to the mview session (HTTP %d).",
		               response ? response->status_code : 0);
		console_print_error("%s\n", ssi_last_status);
		if (response) http_response_destroy(response);
		return false;
	}
	bool parsed = ssi_parse_json_string((const char*)response->buffer.data,
	                                    response->content_length, remote->session_id,
	                                    sizeof(remote->session_id));
	http_response_destroy(response);
	remote->session_open = parsed && ssi_identifier_is_safe(remote->session_id);
	if (!remote->session_open) ssi_set_status("SSI returned an invalid session handle.");
	return remote->session_open;
}

static bool ssi_open_slide(ssi_remote_image_t* remote) {
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/slide/open/server/%s/%s"
	                      "?readonly=true&useOriginalBitMode=false",
	                      remote->endpoint.root, remote->session_id, remote->slide_guid);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote, "slide/open",
	                                                REMOTE_HTTP_POST, path,
	                                                NULL, NULL, 0, "application/json");
	if (!ssi_response_succeeded(response)) {
		ssi_set_status("SSI could not open the requested slide (HTTP %d).",
		               response ? response->status_code : 0);
		console_print_error("%s\n", ssi_last_status);
		if (response) http_response_destroy(response);
		return false;
	}
	bool parsed = ssi_parse_json_string((const char*)response->buffer.data,
	                                    response->content_length, remote->slide_token,
	                                    sizeof(remote->slide_token));
	http_response_destroy(response);
	remote->slide_open = parsed && ssi_identifier_is_safe(remote->slide_token);
	if (!remote->slide_open) ssi_set_status("SSI returned an invalid slide handle.");
	return remote->slide_open;
}

static bool ssi_get_slide_properties(ssi_remote_image_t* remote) {
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/slide/%s/base_properties",
	                      remote->endpoint.root, remote->slide_token);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote, "base_properties",
	                                                REMOTE_HTTP_GET, path,
	                                                NULL, NULL, 0, "application/json");
	if (!ssi_response_succeeded(response)) {
		ssi_set_status("SSI base-properties request failed (HTTP %d).",
		               response ? response->status_code : 0);
		console_print_error("%s\n", ssi_last_status);
		if (response) http_response_destroy(response);
		return false;
	}
	bool parsed = ssi_parse_slide_properties((const char*)response->buffer.data,
	                                         response->content_length, &remote->properties);
	http_response_destroy(response);
	if (!parsed) ssi_set_status("SSI returned invalid or unsupported slide properties.");
	return parsed;
}

static void ssi_copy_display_name(char* dest, size_t dest_size, const char* source) {
	if (!dest || dest_size == 0) return;
	size_t used = 0;
	for (const u8* p = (const u8*)(source ? source : ""); *p && used + 1 < dest_size; ++p) {
		dest[used++] = (*p >= 0x20 && *p != 0x7f) ? (char)*p : ' ';
	}
	dest[used] = '\0';
}

static bool ssi_store_json_response(http_response_t* response, char** dest,
		size_t* dest_size) {
	if (!ssi_response_succeeded(response) || !response->buffer.data ||
	    response->content_length == 0 || !dest || !dest_size) return false;
	char* json = (char*)malloc(response->content_length + 1);
	if (!json) return false;
	memcpy(json, response->buffer.data, response->content_length);
	json[response->content_length] = '\0';
	free(*dest);
	*dest = json;
	*dest_size = response->content_length;
	return true;
}

static bool ssi_get_slide_attributes(ssi_remote_image_t* remote) {
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/server/%s/%s/slide_attributes",
	                      remote->endpoint.root, remote->session_id, remote->slide_guid);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote,
	                                                "slide_attributes",
	                                                REMOTE_HTTP_GET, path,
	                                                NULL, NULL, 0, "application/json");
	bool stored = ssi_store_json_response(response, &remote->slide_attributes_json,
	                                      &remote->slide_attributes_json_size);
	if (stored) {
		char display_name[sizeof(remote->display_name)];
		if (ssi_parse_slide_attribute_name(remote->slide_attributes_json,
		                                   remote->slide_attributes_json_size,
		                                   display_name, sizeof(display_name))) {
			ssi_copy_display_name(remote->display_name, sizeof(remote->display_name),
			                      display_name);
		} else {
			stored = false;
		}
		console_print_verbose("SSI slide attributes JSON (%zu bytes):\n%.*s\n",
		                      remote->slide_attributes_json_size,
		                      (i32)remote->slide_attributes_json_size,
		                      remote->slide_attributes_json);
	} else {
		console_print_verbose("SSI slide attributes are unavailable.\n");
	}
	if (response) http_response_destroy(response);
	if (!stored) ssi_set_status("SSI returned invalid or unavailable slide attributes.");
	return stored;
}

static void ssi_get_all_properties(ssi_remote_image_t* remote) {
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                  "%s/api/simpleslideinterface/v1/slide/%s/all_properties"
	                  "?read_hierarchical_properties=true",
	                  remote->endpoint.root, remote->slide_token);
	if (length > 0 && length < sizeof(path)) {
		http_response_t* response = ssi_initial_request(remote,
		                                                "all_properties",
		                                                REMOTE_HTTP_GET, path,
		                                                NULL, NULL, 0, "application/json");
		if (ssi_store_json_response(response, &remote->all_properties_json,
		                            &remote->all_properties_json_size)) {
			console_print_verbose("SSI all-properties JSON (%zu bytes):\n%.*s\n",
			                      remote->all_properties_json_size,
			                      (i32)remote->all_properties_json_size,
			                      remote->all_properties_json);
		} else {
			console_print_verbose("SSI all-properties metadata is unavailable.\n");
		}
		if (response) http_response_destroy(response);
	}
}

typedef struct ssi_required_attributes_task_t {
	ssi_remote_image_t remote;
	bool succeeded;
} ssi_required_attributes_task_t;

static void ssi_required_attributes_task_func(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	ssi_required_attributes_task_t* task = *(ssi_required_attributes_task_t**)userdata;
	task->succeeded = ssi_get_slide_attributes(&task->remote);
}

static bool ssi_get_required_slide_data(ssi_remote_image_t* remote) {
	i32 initial_request_count = remote->initial_request_count;
	i32 initial_request_time_ms = remote->initial_request_time_ms;
	u64 initial_response_bytes = remote->initial_response_bytes;
	ssi_required_attributes_task_t attributes = {0};
	attributes.remote = *remote;
	ssi_required_attributes_task_t* attributes_pointer = &attributes;
	task_group_t group = {0};
	bool submitted = thread_pool_submit_task_to_group(
		&global_thread_pool, &group, ssi_required_attributes_task_func,
		&attributes_pointer, sizeof(attributes_pointer));
	bool properties_ok = ssi_get_slide_properties(remote);
	if (submitted) {
		thread_pool_wait_for_group(&global_thread_pool, &group);
	} else {
		ssi_required_attributes_task_func(0, &attributes_pointer);
	}
	if (!properties_ok || !attributes.succeeded) {
		free(attributes.remote.slide_attributes_json);
		return false;
	}
	remote->slide_attributes_json = attributes.remote.slide_attributes_json;
	remote->slide_attributes_json_size = attributes.remote.slide_attributes_json_size;
	attributes.remote.slide_attributes_json = NULL;
	copy_cstring(remote->display_name, attributes.remote.display_name,
	             sizeof(remote->display_name));
	remote->initial_request_count += attributes.remote.initial_request_count -
	                                 initial_request_count;
	remote->initial_request_time_ms += attributes.remote.initial_request_time_ms -
	                                   initial_request_time_ms;
	remote->initial_response_bytes += attributes.remote.initial_response_bytes -
	                                  initial_response_bytes;
	return true;
}

static bool ssi_get_single_image(ssi_remote_image_t* remote, const char* image_type,
		const char* operation, simple_image_t* image) {
	if (!remote || !image_type || !operation || !image) return false;
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/slide/%s/single_image"
	                      "?encoding=JPEG_LOSSY&type=%s&output_quality=90",
	                      remote->endpoint.root, remote->slide_token, image_type);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote, operation,
	                                                REMOTE_HTTP_GET,
	                                                path, NULL, NULL, 0,
	                                                "application/octet-stream");
	if (!ssi_response_succeeded(response) || response->content_length == 0 ||
	    response->content_length > UINT32_MAX) {
		if (response) http_response_destroy(response);
		return false;
	}
	i32 width = 0;
	i32 height = 0;
	i32 channels = 0;
	u8* pixels = jpeg_decode_image((u8*)response->buffer.data,
	                               (u32)response->content_length,
	                               &width, &height, &channels);
	http_response_destroy(response);
	if (!pixels || width <= 0 || height <= 0 || channels != BYTES_PER_PIXEL) {
		free(pixels);
		return false;
	}
	image->pixels = pixels;
	image->width = width;
	image->height = height;
	image->channels_in_file = channels;
	image->channels = BYTES_PER_PIXEL;
	image->is_valid = true;
	console_print_verbose("SSI associated image %s: %dx%d decoded\n",
	                      image_type, width, height);
	return true;
}

static bool ssi_get_folder_content(ssi_remote_image_t* remote) {
	if (!remote->properties.folder_id[0] ||
	    !ssi_identifier_is_safe(remote->properties.folder_id)) return false;
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/server/%s/folder_content"
	                      "?folder_id=%s",
	                      remote->endpoint.root, remote->session_id,
	                      remote->properties.folder_id);
	if (length <= 0 || length >= sizeof(path)) return false;
	http_response_t* response = ssi_initial_request(remote,
	                                                "folder_content",
	                                                REMOTE_HTTP_GET, path,
	                                                NULL, NULL, 0, "application/json");
	ssi_slide_ref_t* slides = NULL;
	i32 slide_count = 0;
	bool loaded = false;
	if (ssi_response_succeeded(response) &&
	    ssi_parse_folder_content((const char*)response->buffer.data,
	                             response->content_length, &slides, &slide_count)) {
		free(remote->folder_slides);
		remote->folder_slides = slides;
		remote->folder_slide_count = slide_count;
		loaded = true;
		console_print_verbose("SSI folder content: %d slide(s)\n", slide_count);
	} else {
		free(slides);
		console_print_verbose("SSI folder content is unavailable.\n");
	}
	if (response) http_response_destroy(response);
	return loaded;
}

static void ssi_free_simple_image(simple_image_t* image) {
	if (image && image->pixels) free(image->pixels);
	if (image) memset(image, 0, sizeof(*image));
}

typedef struct ssi_optional_result_t {
	i32 resource_id;
	char display_name[512];
	char* all_properties_json;
	size_t all_properties_json_size;
	simple_image_t macro_image;
	simple_image_t label_image;
	ssi_slide_ref_t* folder_slides;
	i32 folder_slide_count;
	bool folder_content_loaded;
	i32 request_count;
	i32 request_time_ms;
	u64 response_bytes;
	i32 completed_after_ready_ms;
} ssi_optional_result_t;

typedef struct ssi_optional_task_t {
	image_t* image;
	ssi_remote_image_t remote;
	i32 resource_id;
} ssi_optional_task_t;

static void ssi_optional_result_destroy(ssi_optional_result_t* result) {
	if (!result) return;
	free(result->all_properties_json);
	ssi_free_simple_image(&result->macro_image);
	ssi_free_simple_image(&result->label_image);
	free(result->folder_slides);
	free(result);
}

static ssi_optional_result_t* ssi_load_optional_data(ssi_remote_image_t* remote,
		i32 resource_id) {
	ssi_optional_result_t* result = (ssi_optional_result_t*)calloc(1, sizeof(*result));
	if (!result) return NULL;
	result->resource_id = resource_id;
	ssi_get_all_properties(remote);
	result->folder_content_loaded = ssi_get_folder_content(remote);
	if (!ssi_get_single_image(remote, "PREVIEW", "single_image PREVIEW",
	                          &result->macro_image)) {
		console_print_verbose("SSI preview image unavailable; trying generated thumbnail.\n");
		ssi_get_single_image(remote, "THUMBNAIL", "single_image THUMBNAIL",
		                     &result->macro_image);
	}
	if (!ssi_get_single_image(remote, "LABEL", "single_image LABEL",
	                          &result->label_image)) {
		console_print_verbose("SSI label image is unavailable.\n");
	}
	copy_cstring(result->display_name, remote->display_name,
	             sizeof(result->display_name));
	result->all_properties_json = remote->all_properties_json;
	result->all_properties_json_size = remote->all_properties_json_size;
	remote->all_properties_json = NULL;
	remote->all_properties_json_size = 0;
	if (result->folder_content_loaded) {
		result->folder_slides = remote->folder_slides;
		result->folder_slide_count = remote->folder_slide_count;
		remote->folder_slides = NULL;
		remote->folder_slide_count = 0;
	}
	result->request_count = remote->initial_request_count;
	result->request_time_ms = remote->initial_request_time_ms;
	result->response_bytes = remote->initial_response_bytes;
	result->completed_after_ready_ms = ssi_elapsed_milliseconds(remote->viewer_ready_clock);
	return result;
}

static void ssi_optional_task_func(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	ssi_optional_task_t* task = *(ssi_optional_task_t**)userdata;
	ssi_optional_result_t* result = NULL;
	if (!task->image->is_deleted) {
		result = ssi_load_optional_data(&task->remote, task->resource_id);
	}
	if (result) {
		ssi_optional_result_t* result_pointer = result;
		if (!completion_queue_post(&global_completion_queue,
		                           SSI_COMPLETION_EVENT_OPTIONAL_DATA,
		                           &result_pointer, sizeof(result_pointer))) {
			ssi_optional_result_destroy(result);
		}
	}
	atomic_decrement(&task->image->refcount);
	free(task);
}

/* SlideCenter maintains the session and opened slide as separate expiring handles. */
#define SSI_KEEPALIVE_INTERVAL_SECONDS 60.0f

typedef struct ssi_keepalive_task_t {
	image_t* image;
} ssi_keepalive_task_t;

static bool ssi_keepalive_response_requires_reauthentication(
		const http_response_t* response) {
	if (!response) return false;
	return response->status_code == 400 || response->status_code == 401 ||
	       response->status_code == 403 || response->status_code == 404;
}

static http_response_t* ssi_ping_handle(ssi_remote_image_t* remote,
		const char* kind, const char* handle) {
	if (!remote || !kind || !ssi_identifier_is_safe(handle)) return NULL;
	char path[1024];
	i32 length = snprintf(path, sizeof(path),
	                      "%s/api/simpleslideinterface/v1/%s/%s/ping",
	                      remote->endpoint.root, kind, handle);
	if (length <= 0 || length >= sizeof(path)) return NULL;
	/* Bound close-time waiting if the lab network disappears during a ping. */
	remote_request_options_t options = {0};
	options.read_poll_timeout_ms = 250;
	options.write_timeout_seconds = 5.0f;
	options.idle_read_timeout_seconds = 5.0f;
	options.total_timeout_seconds = 10.0f;
	options.max_response_body_size = KILOBYTES(64);
	return ssi_transport_request(
		&remote->endpoint, NULL, SSI_TRANSPORT_FALLBACK_IF_BUSY,
		REMOTE_HTTP_POST, path, "application/json", NULL, NULL, 0, &options);
}

static void ssi_keepalive_task_func(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	ssi_keepalive_task_t* task = *(ssi_keepalive_task_t**)userdata;
	image_t* image = task->image;
	ssi_remote_image_t* remote = &image->ssi;
	bool session_ok = false;
	bool slide_ok = false;
	bool requires_reauthentication = false;
	i32 session_status = 0;
	i32 slide_status = 0;
	i64 start_clock = get_clock();
	if (!image->is_deleted && image->backend == IMAGE_BACKEND_SSI) {
		http_response_t* response = ssi_ping_handle(
			remote, "session", remote->session_id);
		session_status = response ? response->status_code : 0;
		session_ok = ssi_response_succeeded(response);
		requires_reauthentication =
			ssi_keepalive_response_requires_reauthentication(response);
		if (response) http_response_destroy(response);

		if (!image->is_deleted && !requires_reauthentication) {
			response = ssi_ping_handle(remote, "slide",
			                           remote->slide_token);
			slide_status = response ? response->status_code : 0;
			slide_ok = ssi_response_succeeded(response);
			requires_reauthentication =
				ssi_keepalive_response_requires_reauthentication(response);
			if (response) http_response_destroy(response);
		}
		if (requires_reauthentication) {
			atomic_compare_exchange(&remote->reauthentication_state, 1, 0);
		} else if (!session_ok || !slide_ok) {
			console_print_verbose(
				"SSI keepalive failed transiently (session HTTP %d, slide HTTP %d); "
				"will retry.\n", session_status, slide_status);
		}
		ssi_transport_stats_t transport_stats = {0};
		ssi_transport_get_stats(&transport_stats);
		console_print_verbose("SSI keepalive: session=%s, slide=%s, %d ms; "
		                      "control pool reuses=%d, fallback handshakes=%d\n",
		                      session_ok ? "ok" : "failed",
		                      slide_ok ? "ok" : "not pinged/failed",
		                      ssi_elapsed_milliseconds(start_clock),
		                      transport_stats.control_reuses,
		                      transport_stats.control_fallback_handshakes);
	}
	atomic_compare_exchange(&remote->keepalive_in_flight, 0, 1);
	atomic_decrement(&image->refcount);
	free(task);
}

void ssi_poll_keepalive(app_state_t* app_state) {
	if (!app_state || app_state->headless) return;
	i64 now = get_clock();
	for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
		image_t* image = app_state->loaded_images[i];
		if (!image || image->backend != IMAGE_BACKEND_SSI || image->is_deleted) continue;
		ssi_remote_image_t* remote = &image->ssi;
		if (!remote->session_open || !remote->slide_open ||
		    remote->reauthentication_state != 0 ||
		    get_seconds_elapsed(remote->keepalive_last_scheduled_clock, now) <
		        SSI_KEEPALIVE_INTERVAL_SECONDS ||
		    !atomic_compare_exchange(&remote->keepalive_in_flight, 1, 0)) continue;
		remote->keepalive_last_scheduled_clock = now;
		ssi_keepalive_task_t* task = (ssi_keepalive_task_t*)calloc(1, sizeof(*task));
		if (!task) {
			atomic_compare_exchange(&remote->keepalive_in_flight, 0, 1);
			continue;
		}
		task->image = image;
		ssi_keepalive_task_t* task_pointer = task;
		atomic_increment(&image->refcount);
		if (!thread_pool_submit_task(&global_thread_pool, ssi_keepalive_task_func,
		                             &task_pointer, sizeof(task_pointer))) {
			atomic_decrement(&image->refcount);
			atomic_compare_exchange(&remote->keepalive_in_flight, 0, 1);
			free(task);
		}
	}
}

static bool ssi_schedule_optional_data(image_t* image) {
	ssi_optional_task_t* task = (ssi_optional_task_t*)calloc(1, sizeof(*task));
	if (!task) return false;
	task->image = image;
	task->remote = image->ssi;
	/* The worker copy must not own or invalidate the list currently shown by the UI. */
	task->remote.folder_slides = NULL;
	task->remote.folder_slide_count = 0;
	task->resource_id = image->resource_id;
	ssi_optional_task_t* task_pointer = task;
	atomic_increment(&image->refcount);
	if (!thread_pool_submit_task(&global_thread_pool, ssi_optional_task_func,
	                             &task_pointer, sizeof(task_pointer))) {
		atomic_decrement(&image->refcount);
		free(task);
		return false;
	}
	return true;
}

void ssi_commit_optional_data(app_state_t* app_state, void* result_pointer) {
	ssi_optional_result_t* result = (ssi_optional_result_t*)result_pointer;
	if (!result) return;
	image_t* image = get_image_from_resource_id(app_state, result->resource_id);
	if (!image || image->backend != IMAGE_BACKEND_SSI || image->is_deleted) {
		ssi_optional_result_destroy(result);
		return;
	}
	platform_mutex_lock(&image->lock);
	free(image->ssi.all_properties_json);
	image->ssi.all_properties_json = result->all_properties_json;
	image->ssi.all_properties_json_size = result->all_properties_json_size;
	result->all_properties_json = NULL;
	image->ssi.initial_request_count = result->request_count;
	image->ssi.initial_request_time_ms = result->request_time_ms;
	image->ssi.initial_response_bytes = result->response_bytes;
	if (result->display_name[0]) {
		copy_cstring(image->ssi.display_name, result->display_name,
		             sizeof(image->ssi.display_name));
		copy_cstring(image->name, result->display_name, sizeof(image->name));
		app_state->is_window_title_set_for_image = false;
	}
	if (result->macro_image.is_valid) {
		image->macro_image = result->macro_image;
		memset(&result->macro_image, 0, sizeof(result->macro_image));
		image->macro_image.mpp = image->width_in_um /
		                         (float)MAX(1, image->macro_image.width);
		image->macro_image.world_pos = V2F(0.0f, 0.0f);
	}
	if (result->label_image.is_valid) {
		image->label_image = result->label_image;
		memset(&result->label_image, 0, sizeof(result->label_image));
		image->label_image.mpp = image->macro_image.is_valid
		                         ? image->macro_image.mpp
		                         : image->width_in_um /
		                           (float)MAX(1, image->label_image.width);
		image->label_image.world_pos = image->macro_image.is_valid
		                              ? V2F(image->macro_image.width *
		                                    image->macro_image.mpp, 0.0f)
		                              : V2F(0.0f, 0.0f);
	}
	if (result->folder_content_loaded) {
		free(image->ssi.folder_slides);
		image->ssi.folder_slides = result->folder_slides;
		image->ssi.folder_slide_count = result->folder_slide_count;
		result->folder_slides = NULL;
		result->folder_slide_count = 0;
	}
	if (image->ssi.folder_slide_count > 1) show_slide_tray_window = true;
	platform_mutex_unlock(&image->lock);
	ssi_transport_stats_t transport_stats = {0};
	ssi_transport_get_stats(&transport_stats);
	console_print_verbose(
		"SSI optional data ready: %d ms after viewer ready; cumulative requests=%d, "
		"response_bytes=%llu, request_time=%d ms; pool opens=%d, reuses=%d, "
		"control reuses=%d, fallback handshakes=%d\n",
		result->completed_after_ready_ms, result->request_count,
		(unsigned long long)result->response_bytes, result->request_time_ms,
		transport_stats.pool_opens, transport_stats.pool_reuses,
		transport_stats.control_reuses,
		transport_stats.control_fallback_handshakes);
	ssi_optional_result_destroy(result);
}

void ssi_close_remote_image(ssi_remote_image_t* remote) {
	if (!remote) return;
	char path[1024];
	if (remote->slide_open && ssi_identifier_is_safe(remote->slide_token)) {
		i32 length = snprintf(path, sizeof(path),
		                      "%s/api/simpleslideinterface/v1/slide/%s/close",
		                      remote->endpoint.root, remote->slide_token);
		if (length > 0 && length < sizeof(path)) {
			http_response_t* response = ssi_request(&remote->endpoint, REMOTE_HTTP_DELETE,
			                                        path, NULL, NULL, 0, "application/json");
			if (response) http_response_destroy(response);
		}
	}
	remote->slide_open = false;
	remote->slide_token[0] = '\0';
	if (remote->session_open && ssi_identifier_is_safe(remote->session_id)) {
		i32 length = snprintf(path, sizeof(path),
		                      "%s/api/simpleslideinterface/v1/session/%s/close",
		                      remote->endpoint.root, remote->session_id);
		if (length > 0 && length < sizeof(path)) {
			http_response_t* response = ssi_request(&remote->endpoint, REMOTE_HTTP_DELETE,
			                                        path, NULL, NULL, 0, "application/json");
			if (response) http_response_destroy(response);
		}
	}
	remote->session_open = false;
	remote->session_id[0] = '\0';
	free(remote->slide_attributes_json);
	remote->slide_attributes_json = NULL;
	remote->slide_attributes_json_size = 0;
	free(remote->all_properties_json);
	remote->all_properties_json = NULL;
	remote->all_properties_json_size = 0;
	free(remote->folder_slides);
	remote->folder_slides = NULL;
	remote->folder_slide_count = 0;
}

static image_t* ssi_create_initialized_image(ssi_remote_image_t* remote,
		bool preserve_session_on_failure) {
	image_t* image = (image_t*)calloc(1, sizeof(image_t));
	if (!image) {
		ssi_set_status("Could not allocate the SSI image.");
		return NULL;
	}
	image->resource_id = global_next_resource_id++;
	bool valid = init_image_from_ssi(image, remote, false);
	if (image->backend == IMAGE_BACKEND_SSI) {
		remote->session_open = false;
		remote->slide_open = false;
		remote->slide_attributes_json = NULL;
		remote->slide_attributes_json_size = 0;
		remote->all_properties_json = NULL;
		remote->all_properties_json_size = 0;
		remote->folder_slides = NULL;
		remote->folder_slide_count = 0;
	}
	if (valid && !image_initialize_runtime(image)) valid = false;
	if (!valid) {
		ssi_set_status("Could not initialize the SSI image pyramid.");
		if (image->backend == IMAGE_BACKEND_SSI) {
			if (preserve_session_on_failure) image->ssi.session_open = false;
			image_destroy(image);
		}
		free(image);
		return NULL;
	}
	return image;
}

static void ssi_install_image(app_state_t* app_state, image_t* image,
		i64 total_start_clock) {
	unload_all_images(app_state);
	image->ssi.viewer_ready_clock = get_clock();
	image->ssi.keepalive_last_scheduled_clock = image->ssi.viewer_ready_clock;
	add_image(app_state, image, true, false);
	console_print_verbose(
		"SSI viewer ready: requests=%d, response_bytes=%llu, request_time=%d ms, "
		"elapsed=%d ms\n",
		image->ssi.initial_request_count,
		(unsigned long long)image->ssi.initial_response_bytes,
		image->ssi.initial_request_time_ms,
		ssi_elapsed_milliseconds(total_start_clock));
	if (app_state->headless) {
		ssi_optional_result_t* result = ssi_load_optional_data(&image->ssi,
		                                                       image->resource_id);
		ssi_commit_optional_data(app_state, result);
	} else if (!ssi_schedule_optional_data(image)) {
		console_print_verbose("SSI optional data task could not be scheduled.\n");
	}
}

bool ssi_open_remote_image(app_state_t* app_state, const char* api_url,
		const char* slide_center_url, const char* username, const char* password,
		const char* existing_session_id, const char* slide_guid, const char* caption) {
	i64 total_start_clock = get_clock();
	ssi_last_status[0] = '\0';
	if (!app_state || !api_url || !slide_center_url || !slide_guid ||
	    ((!existing_session_id || !existing_session_id[0]) && (!username || !password))) {
		ssi_set_status("SSI configuration or credentials are missing.");
		return false;
	}
	ssi_remote_image_t remote = {0};
	if (!ssi_parse_endpoint(api_url, &remote.endpoint)) {
		ssi_set_status("Expected a secure SSI API URL ending in /SSI.");
		return false;
	}
	if (strlen(slide_guid) != 32) {
		ssi_set_status("SSI slide identifiers must contain 32 hexadecimal characters.");
		return false;
	}
	for (i32 i = 0; i < 32; ++i) {
		char c = slide_guid[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		      (c >= 'A' && c <= 'F'))) {
			ssi_set_status("SSI slide identifiers must contain 32 hexadecimal characters.");
			return false;
		}
	}
	memcpy(remote.slide_guid, slide_guid, 33);
	ssi_copy_display_name(remote.display_name, sizeof(remote.display_name), caption);
	copy_cstring(remote.api_url, api_url, sizeof(remote.api_url));
	copy_cstring(remote.slide_center_url, slide_center_url, sizeof(remote.slide_center_url));

	bool opened = (existing_session_id && existing_session_id[0]
	               ? ssi_connect_session(&remote,
	                                     slide_center_url, existing_session_id)
	               : ssi_create_session(&remote, slide_center_url,
	                                    username, password)) &&
	              ssi_open_slide(&remote) &&
	              ssi_get_required_slide_data(&remote);
	if (!opened) {
		ssi_close_remote_image(&remote);
		return false;
	}

	image_t* image = ssi_create_initialized_image(&remote, false);
	if (!image) {
		if (remote.session_open) ssi_close_remote_image(&remote);
		return false;
	}
	ssi_install_image(app_state, image, total_start_clock);
	ssi_set_status("Opened SSI slide '%s'.", image->name);
	console_print("%s\n", ssi_last_status);
	return true;
}

bool ssi_open_folder_slide(app_state_t* app_state, image_t* current_image,
		i32 slide_index) {
	i64 total_start_clock = get_clock();
	if (!app_state || !current_image || current_image->backend != IMAGE_BACKEND_SSI ||
	    slide_index < 0 || slide_index >= current_image->ssi.folder_slide_count) {
		ssi_set_status("The selected SSI folder slide is unavailable.");
		return false;
	}
	ssi_slide_ref_t selected = current_image->ssi.folder_slides[slide_index];
	if (strcmp(selected.id, current_image->ssi.slide_guid) == 0) return true;
	ssi_remote_image_t remote = {0};
	remote.endpoint = current_image->ssi.endpoint;
	copy_cstring(remote.api_url, current_image->ssi.api_url, sizeof(remote.api_url));
	copy_cstring(remote.slide_center_url, current_image->ssi.slide_center_url,
	             sizeof(remote.slide_center_url));
	copy_cstring(remote.session_id, current_image->ssi.session_id,
	             sizeof(remote.session_id));
	copy_cstring(remote.slide_guid, selected.id, sizeof(remote.slide_guid));
	ssi_copy_display_name(remote.display_name, sizeof(remote.display_name), selected.name);
	remote.session_open = current_image->ssi.session_open;
	if (!remote.session_open) {
		ssi_set_status("The SSI session is no longer available.");
		return false;
	}
	bool opened = ssi_open_slide(&remote) &&
	              ssi_get_required_slide_data(&remote);
	if (!opened) {
		remote.session_open = false;
		ssi_close_remote_image(&remote);
		return false;
	}
	if (current_image->ssi.folder_slide_count > 0) {
		size_t folder_slides_size = (size_t)current_image->ssi.folder_slide_count *
		                            sizeof(*remote.folder_slides);
		remote.folder_slides = (ssi_slide_ref_t*)malloc(folder_slides_size);
		if (remote.folder_slides) {
			memcpy(remote.folder_slides, current_image->ssi.folder_slides,
			       folder_slides_size);
			remote.folder_slide_count = current_image->ssi.folder_slide_count;
		}
	}
	image_t* image = ssi_create_initialized_image(&remote, true);
	if (!image) {
		remote.session_open = false;
		ssi_close_remote_image(&remote);
		return false;
	}
	/* Transfer session ownership before destroying the previous slide image. */
	current_image->ssi.session_open = false;
	ssi_install_image(app_state, image, total_start_clock);
	ssi_set_status("Opened SSI slide '%s'.", image->name);
	console_print("%s\n", ssi_last_status);
	return true;
}

bool ssi_resolve_uri(const char* uri, ssi_uri_config_t* config) {
	if (!uri || !config) return false;
	if (ssi_parse_mview_uri(uri, config)) return true;
	memset(config, 0, sizeof(*config));
	if (!ssi_parse_uri_slide_guid(uri, config->slide_guid, sizeof(config->slide_guid))) {
		return false;
	}
	const char* api_url = getenv("SLIDESCAPE_SSI_API_URL");
	const char* slide_center_url = getenv("SLIDESCAPE_SSI_SLIDECENTER_URL");
	if (!api_url || !slide_center_url) return false;
	copy_cstring(config->api_url, api_url, sizeof(config->api_url));
	copy_cstring(config->slide_center_url, slide_center_url, sizeof(config->slide_center_url));
	return true;
}

bool ssi_open_uri_with_credentials(app_state_t* app_state, const char* uri,
		const char* username, const char* password) {
	ssi_uri_config_t config;
	if (!ssi_resolve_uri(uri, &config)) {
		ssi_set_status("Could not resolve the SSI or mview URI. Plain ssi: links also require "
		               "SLIDESCAPE_SSI_API_URL and SLIDESCAPE_SSI_SLIDECENTER_URL.");
		return false;
	}
	return ssi_open_remote_image(app_state, config.api_url, config.slide_center_url,
	                             username, password, config.session_id, config.slide_guid,
	                             config.caption);
}

bool ssi_try_open_uri(app_state_t* app_state, const char* uri) {
	const char* username = getenv("SLIDESCAPE_SSI_USERNAME");
	const char* password = getenv("SLIDESCAPE_SSI_PASSWORD");
	ssi_uri_config_t config;
	if (!ssi_resolve_uri(uri, &config)) return false;
	if (!config.session_id[0] && (!username || !password)) {
		ssi_set_status("SSI credentials are required.");
		console_print_error("%s\n", ssi_last_status);
		return false;
	}
	return ssi_open_uri_with_credentials(app_state, uri, username, password);
}
