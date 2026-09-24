/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "dicomweb_client.h"

#include "dicomweb.h"
#include "image.h"
#include "remote.h"
#include "stringutils.h"
#include "tile_cache.h"
#include "tile_loader.h"
#include "viewer.h"

extern bool show_slide_tray_window;

static char dicomweb_last_status[512];
static THREAD_LOCAL http_response_error_t dicomweb_last_transport_error;

typedef struct dicomweb_worker_connection_t {
	tls_connection_t* connection;
	char hostname[256];
	i32 port;
	bool use_tls;
	i64 last_used_clock;
} dicomweb_worker_connection_t;

static THREAD_LOCAL dicomweb_worker_connection_t dicomweb_worker_connection;

typedef struct dicomweb_tile_stats_t {
	volatile i32 request_count;
	volatile i32 single_request_count;
	volatile i32 multi_request_count;
	volatile i32 batch_size_2_count;
	volatile i32 batch_size_3_count;
	volatile i32 batch_size_4_count;
	volatile i32 requested_frame_count;
	volatile i32 decoded_frame_count;
	volatile i32 failed_request_count;
	volatile i32 fallback_request_count;
	volatile i32 fallback_multipart_count;
	volatile i32 fallback_mapping_count;
	volatile i32 fallback_decode_count;
} dicomweb_tile_stats_t;

static dicomweb_tile_stats_t dicomweb_tile_stats;

#define DICOMWEB_REQUEST_WINDOW 100

typedef struct dicomweb_request_timing_t {
	i64 start_clock;
	double http_ms;
	double decode_ms;
} dicomweb_request_timing_t;

typedef struct dicomweb_latency_sample_t {
	double total_ms;
	double http_ms;
	double decode_ms;
} dicomweb_latency_sample_t;

/* Workers complete out of order. Protect the short ring update and snapshot;
 * never hold this lock during I/O, decoding, or console output. */
static platform_mutex_t dicomweb_latency_mutex = PLATFORM_MUTEX_INITIALIZER;
static dicomweb_latency_sample_t dicomweb_latency_samples[DICOMWEB_REQUEST_WINDOW];
static dicomweb_latency_sample_t dicomweb_latency_sum;
static i32 dicomweb_latency_count;
static i32 dicomweb_latency_next;

#define DICOMWEB_ASYNC_COMPLETION_RESULT 0x4449434fu
#define DICOMWEB_MAX_JSON_RESPONSE_SIZE MEGABYTES(256)
#define DICOMWEB_MAX_FRAME_RESPONSE_SIZE MEGABYTES(64)
#define DICOMWEB_QIDO_PAGE_SIZE 100
#define DICOMWEB_QIDO_BROWSER_RESULT_LIMIT 1000
#define DICOMWEB_QIDO_OPEN_INSTANCE_LIMIT 10000
#define DICOMWEB_INSTANCE_INCLUDE_FIELDS \
	"?includefield=00080016&includefield=00080008&includefield=00200013" \
	"&includefield=00280008&includefield=00209161&includefield=00209162" \
	"&includefield=00209163&includefield=00480006&includefield=00480007"

void dicomweb_print_tile_stats(void) {
	i32 request_count = dicomweb_tile_stats.request_count;
	i32 fallback_count = dicomweb_tile_stats.fallback_request_count +
	                     dicomweb_tile_stats.fallback_multipart_count +
	                     dicomweb_tile_stats.fallback_mapping_count +
	                     dicomweb_tile_stats.fallback_decode_count;
	platform_mutex_lock(&dicomweb_latency_mutex);
	i32 sample_count = dicomweb_latency_count;
	dicomweb_latency_sample_t average = dicomweb_latency_sum;
	platform_mutex_unlock(&dicomweb_latency_mutex);
	if (sample_count) {
		average.total_ms /= sample_count;
		average.http_ms /= sample_count;
		average.decode_ms /= sample_count;
	}
	console_print_verbose(
		"DICOMweb tile stats: requests=%d (single=%d, multi=%d; batch 2/3/4=%d/%d/%d), "
		"frame requests=%d, decoded=%d, failed requests=%d, "
		"fallbacks=%d (request=%d, multipart=%d, mapping=%d, decode=%d), "
		"average request=%.2f ms (last %d; HTTP=%.2f ms, decode=%.2f ms, other=%.2f ms)\n",
		request_count, dicomweb_tile_stats.single_request_count,
		dicomweb_tile_stats.multi_request_count,
		dicomweb_tile_stats.batch_size_2_count,
		dicomweb_tile_stats.batch_size_3_count,
		dicomweb_tile_stats.batch_size_4_count,
		dicomweb_tile_stats.requested_frame_count,
		dicomweb_tile_stats.decoded_frame_count,
		dicomweb_tile_stats.failed_request_count, fallback_count,
		dicomweb_tile_stats.fallback_request_count,
		dicomweb_tile_stats.fallback_multipart_count,
		dicomweb_tile_stats.fallback_mapping_count,
		dicomweb_tile_stats.fallback_decode_count, average.total_ms, sample_count,
		average.http_ms, average.decode_ms,
		MAX(0.0, average.total_ms - average.http_ms - average.decode_ms));
}

static void dicomweb_record_tile_request(i32 frame_count, i32 decoded_count,
		bool request_failed, const dicomweb_request_timing_t* timing, i32 fallback_request_count,
		i32 fallback_multipart_count, i32 fallback_mapping_count,
		i32 fallback_decode_count) {
	bool first_multi_request = false;
	if (frame_count > 1) {
		first_multi_request = atomic_increment(&dicomweb_tile_stats.multi_request_count) == 1;
		switch (frame_count) {
			case 2: atomic_increment(&dicomweb_tile_stats.batch_size_2_count); break;
			case 3: atomic_increment(&dicomweb_tile_stats.batch_size_3_count); break;
			case 4: atomic_increment(&dicomweb_tile_stats.batch_size_4_count); break;
		}
	} else {
		atomic_increment(&dicomweb_tile_stats.single_request_count);
	}
	atomic_add(&dicomweb_tile_stats.requested_frame_count, frame_count);
	atomic_add(&dicomweb_tile_stats.decoded_frame_count, decoded_count);
	if (request_failed) atomic_increment(&dicomweb_tile_stats.failed_request_count);
	atomic_add(&dicomweb_tile_stats.fallback_request_count, fallback_request_count);
	atomic_add(&dicomweb_tile_stats.fallback_multipart_count, fallback_multipart_count);
	atomic_add(&dicomweb_tile_stats.fallback_mapping_count, fallback_mapping_count);
	atomic_add(&dicomweb_tile_stats.fallback_decode_count, fallback_decode_count);
	dicomweb_latency_sample_t sample = {
		get_seconds_elapsed(timing->start_clock, get_clock()) * 1000.0,
		timing->http_ms, timing->decode_ms,
	};
	platform_mutex_lock(&dicomweb_latency_mutex);
	dicomweb_latency_sample_t previous = dicomweb_latency_samples[dicomweb_latency_next];
	dicomweb_latency_sum.total_ms += sample.total_ms - previous.total_ms;
	dicomweb_latency_sum.http_ms += sample.http_ms - previous.http_ms;
	dicomweb_latency_sum.decode_ms += sample.decode_ms - previous.decode_ms;
	dicomweb_latency_samples[dicomweb_latency_next] = sample;
	dicomweb_latency_next = (dicomweb_latency_next + 1) % DICOMWEB_REQUEST_WINDOW;
	if (dicomweb_latency_count < DICOMWEB_REQUEST_WINDOW) ++dicomweb_latency_count;
	platform_mutex_unlock(&dicomweb_latency_mutex);
	i32 request_count = atomic_increment(&dicomweb_tile_stats.request_count);
	if (request_count == 1 || first_multi_request || request_count % 16 == 0 ||
	    fallback_request_count || fallback_multipart_count ||
	    fallback_mapping_count || fallback_decode_count) {
		dicomweb_print_tile_stats();
	}
}

typedef struct dicomweb_async_task_t {
	dicomweb_async_context_t* context;
	dicomweb_async_query_kind_t kind;
	i32 generation;
	char archive_url[1024];
	char username[256];
	char password[256];
	char study_uid[65];
	char series_uid[65];
	dicomweb_study_search_t study_search;
} dicomweb_async_task_t;

static image_t* dicomweb_prepare_configured_series(
	dicomweb_remote_image_t remote, const remote_request_options_t* request_options,
	i32* http_status, char* message, size_t message_size);

static size_t dicomweb_async_item_size(dicomweb_async_query_kind_t kind) {
	switch (kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES: return sizeof(dicomweb_study_ref_t);
		case DICOMWEB_ASYNC_QUERY_SERIES: return sizeof(dicomweb_series_ref_t);
		case DICOMWEB_ASYNC_QUERY_INSTANCES: return sizeof(dicomweb_instance_ref_t);
		default: return 0;
	}
}

static i32 dicomweb_async_count_items(dicomweb_async_query_kind_t kind,
	                                  const char* json, size_t json_size) {
	switch (kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES:
			return dicomweb_count_study_refs(json, json_size);
		case DICOMWEB_ASYNC_QUERY_SERIES:
			return dicomweb_count_series_refs(json, json_size);
		case DICOMWEB_ASYNC_QUERY_INSTANCES:
			return dicomweb_count_instance_refs(json, json_size);
		default: return 0;
	}
}

static i32 dicomweb_async_parse_items(dicomweb_async_query_kind_t kind,
	                                  const char* json, size_t json_size,
	                                  void* out, i32 capacity) {
	switch (kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES:
			return dicomweb_parse_study_refs(json, json_size,
			                                 (dicomweb_study_ref_t*)out, capacity);
		case DICOMWEB_ASYNC_QUERY_SERIES:
			return dicomweb_parse_series_refs(json, json_size,
			                                  (dicomweb_series_ref_t*)out, capacity);
		case DICOMWEB_ASYNC_QUERY_INSTANCES:
			return dicomweb_parse_instance_refs(json, json_size,
			                                    (dicomweb_instance_ref_t*)out, capacity);
		default: return 0;
	}
}

static const char* dicomweb_async_item_uid(dicomweb_async_query_kind_t kind,
	                                       const void* item) {
	switch (kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES:
			return ((const dicomweb_study_ref_t*)item)->study_instance_uid;
		case DICOMWEB_ASYNC_QUERY_SERIES:
			return ((const dicomweb_series_ref_t*)item)->series_instance_uid;
		case DICOMWEB_ASYNC_QUERY_INSTANCES:
			return ((const dicomweb_instance_ref_t*)item)->sop_instance_uid;
		default: return "";
	}
}

const char* dicomweb_get_last_status(void) {
	return dicomweb_last_status;
}

/* Store status without logging; the caller reports the result once. */
static void set_status(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(dicomweb_last_status, sizeof(dicomweb_last_status), format, args);
	va_end(args);
}

static bool is_loopback_host(const char* hostname) {
	return strcmp(hostname, "127.0.0.1") == 0 ||
	       strcmp(hostname, "localhost") == 0;
}

static bool url_text_is_safe(const char* text) {
	if (!text || !text[0]) return false;
	for (const u8* p = (const u8*)text; *p; ++p) {
		if (*p <= 0x20 || *p == 0x7f || *p == '\\') return false;
	}
	return true;
}

static bool hostname_is_safe(const char* hostname) {
	if (!hostname || !hostname[0]) return false;
	for (const u8* p = (const u8*)hostname; *p; ++p) {
		if ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') &&
		    (*p < '0' || *p > '9') && *p != '.' && *p != '-') return false;
	}
	return true;
}

static bool parse_archive_url(const char* url, dicomweb_remote_image_t* remote,
                              const char** out_path) {
	if (!url_text_is_safe(url)) return false;
	const char* authority = NULL;
	if (strncmp(url, "https://", 8) == 0) {
		remote->use_tls = true;
		remote->port = 443;
		authority = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		remote->use_tls = false;
		remote->port = 80;
		authority = url + 7;
	} else {
		return false;
	}
	const char* path = strchr(authority, '/');
	if (!path) return false;
	if (strchr(path, '?') || strchr(path, '#')) return false;
	const char* colon = NULL;
	for (const char* p = authority; p < path; ++p) {
		if (*p == ':') colon = p;
	}
	const char* host_end = colon ? colon : path;
	size_t host_length = (size_t)(host_end - authority);
	if (!host_length || host_length >= sizeof(remote->hostname)) return false;
	memcpy(remote->hostname, authority, host_length);
	remote->hostname[host_length] = '\0';
	if (!hostname_is_safe(remote->hostname)) return false;
	if (colon) {
		char port_string[16];
		size_t port_length = (size_t)(path - colon - 1);
		if (!port_length || port_length >= sizeof(port_string)) return false;
		memcpy(port_string, colon + 1, port_length);
		port_string[port_length] = '\0';
		char* port_end = NULL;
		long parsed_port = strtol(port_string, &port_end, 10);
		if (port_end == port_string || *port_end != '\0' ||
		    parsed_port <= 0 || parsed_port > 65535) return false;
		remote->port = (i32)parsed_port;
	}
	if (!remote->use_tls && !is_loopback_host(remote->hostname)) {
		set_status("DICOMweb refuses unencrypted HTTP to non-loopback host '%s'.", remote->hostname);
		return false;
	}
	const char* studies = strstr(path, "/studies/");
	const char* root_end = studies ? studies : path + strlen(path);
	while (root_end > path && root_end[-1] == '/') --root_end;
	size_t root_length = (size_t)(root_end - path);
	if (root_length >= sizeof(remote->root)) return false;
	memcpy(remote->root, path, root_length);
	remote->root[root_length] = '\0';
	if (out_path) *out_path = path;
	return true;
}

static bool parse_series_url(const char* url, dicomweb_remote_image_t* remote) {
	char resolved_url[2048];
	if (!dicomweb_resolve_series_url(resolved_url, sizeof(resolved_url), url)) return false;
	url = resolved_url;
	const char* path = NULL;
	if (!parse_archive_url(url, remote, &path)) return false;
	const char* studies = strstr(path, "/studies/");
	if (!studies) return false;
	const char* study_start = studies + strlen("/studies/");
	const char* series_marker = strstr(study_start, "/series/");
	if (!series_marker) return false;
	size_t study_length = (size_t)(series_marker - study_start);
	const char* series_start = series_marker + strlen("/series/");
	const char* series_end = strchr(series_start, '/');
	size_t series_length = series_end ? (size_t)(series_end - series_start) : strlen(series_start);
	if (!study_length || study_length >= sizeof(remote->study_instance_uid) ||
	    !series_length || series_length >= sizeof(remote->series_instance_uid) ||
	    !dicomweb_is_valid_uid(study_start, study_length) ||
	    !dicomweb_is_valid_uid(series_start, series_length)) return false;
	memcpy(remote->study_instance_uid, study_start, study_length);
	remote->study_instance_uid[study_length] = '\0';
	memcpy(remote->series_instance_uid, series_start, series_length);
	remote->series_instance_uid[series_length] = '\0';
	return true;
}

static const char base64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool set_basic_authorization(dicomweb_remote_image_t* remote,
                                    const char* username, const char* password) {
	if ((!username || !username[0]) && (!password || !password[0])) return true;
	char credentials[4096];
	i32 credential_length = snprintf(credentials, sizeof(credentials), "%s:%s",
	                                 username ? username : "", password ? password : "");
	if (credential_length < 0 || credential_length >= sizeof(credentials)) return false;
	char encoded[5500];
	size_t source = 0;
	size_t dest = 0;
	while (source < (size_t)credential_length) {
		u32 value = (u8)credentials[source++] << 16;
		bool have_second = source < (size_t)credential_length;
		if (have_second) value |= (u8)credentials[source++] << 8;
		bool have_third = source < (size_t)credential_length;
		if (have_third) value |= (u8)credentials[source++];
		encoded[dest++] = base64_alphabet[(value >> 18) & 63];
		encoded[dest++] = base64_alphabet[(value >> 12) & 63];
		encoded[dest++] = have_second ? base64_alphabet[(value >> 6) & 63] : '=';
		encoded[dest++] = have_third ? base64_alphabet[value & 63] : '=';
	}
	encoded[dest] = '\0';
	i32 header_length = snprintf(remote->authorization_header,
	                             sizeof(remote->authorization_header),
	                             "Authorization: Basic %s\r\n", encoded);
	memset(credentials, 0, sizeof(credentials));
	return header_length > 0 && header_length < sizeof(remote->authorization_header);
}

static void drop_worker_connection(void) {
	if (dicomweb_worker_connection.connection) {
		remote_connection_close_and_free(dicomweb_worker_connection.connection);
		dicomweb_worker_connection = (dicomweb_worker_connection_t){0};
	}
}

static bool worker_connection_matches(const dicomweb_remote_image_t* remote) {
	return dicomweb_worker_connection.connection &&
	       dicomweb_worker_connection.port == remote->port &&
	       dicomweb_worker_connection.use_tls == remote->use_tls &&
	       strcmp(dicomweb_worker_connection.hostname, remote->hostname) == 0;
}

static tls_connection_t* get_worker_connection(const dicomweb_remote_image_t* remote) {
	if (dicomweb_worker_connection.connection &&
	    get_seconds_elapsed(dicomweb_worker_connection.last_used_clock, get_clock()) > 0.75f) {
		drop_worker_connection();
	}
	if (dicomweb_worker_connection.connection && !worker_connection_matches(remote)) {
		drop_worker_connection();
	}
	if (!dicomweb_worker_connection.connection) {
		dicomweb_worker_connection.connection = remote_connection_open_http(
			remote->hostname, remote->port, remote->use_tls);
		if (dicomweb_worker_connection.connection) {
			copy_cstring(dicomweb_worker_connection.hostname, remote->hostname,
			             sizeof(dicomweb_worker_connection.hostname));
			dicomweb_worker_connection.port = remote->port;
			dicomweb_worker_connection.use_tls = remote->use_tls;
			dicomweb_worker_connection.last_used_clock = get_clock();
		}
	}
	return dicomweb_worker_connection.connection;
}

static http_response_t* dicomweb_get_with_options(
	const dicomweb_remote_image_t* remote, const char* path, const char* accept,
	bool reuse_worker_connection, const remote_request_options_t* options) {
	remote_request_options_t effective_options = options ? *options : (remote_request_options_t){0};
	if (!effective_options.read_poll_timeout_ms) effective_options.read_poll_timeout_ms = 250;
	if (effective_options.write_timeout_seconds <= 0.0f) {
		effective_options.write_timeout_seconds = 10.0f;
	}
	if (effective_options.idle_read_timeout_seconds <= 0.0f) {
		effective_options.idle_read_timeout_seconds = 10.0f;
	}
	if (effective_options.total_timeout_seconds <= 0.0f) effective_options.total_timeout_seconds = 30.0f;
	if (!effective_options.max_response_body_size) {
		effective_options.max_response_body_size = strstr(accept, "image/jpeg")
		                                           ? DICOMWEB_MAX_FRAME_RESPONSE_SIZE
		                                           : DICOMWEB_MAX_JSON_RESPONSE_SIZE;
	}
	http_response_error_t* caller_error = effective_options.out_error;
	dicomweb_last_transport_error = HTTP_RESPONSE_ERROR_NONE;
	effective_options.out_error = &dicomweb_last_transport_error;
	if (reuse_worker_connection) {
		for (i32 attempt = 0; attempt < 2; ++attempt) {
			tls_connection_t* connection = get_worker_connection(remote);
			if (!connection) {
				dicomweb_last_transport_error = HTTP_RESPONSE_ERROR_CONNECTION;
				if (caller_error) *caller_error = dicomweb_last_transport_error;
				return NULL;
			}
			http_response_t* response = remote_connection_request_with_headers_and_options(
				connection, remote->hostname, path, accept,
				remote->authorization_header, false, &effective_options);
			if (response) {
				if (response->connection_reusable) {
					dicomweb_worker_connection.last_used_clock = get_clock();
				} else {
					/*
					 * The peer may legally close an HTTP/1.1 connection even though
					 * this request asked for keep-alive. Do not leave that socket in
					 * the worker cache for the next tile request.
					 */
					drop_worker_connection();
				}
				if (caller_error) *caller_error = dicomweb_last_transport_error;
				return response;
			}
			drop_worker_connection();
		}
		if (caller_error) *caller_error = dicomweb_last_transport_error;
		return NULL;
	}
	tls_connection_t* connection = remote_connection_open_http(remote->hostname, remote->port,
	                                                           remote->use_tls);
	if (!connection) {
		dicomweb_last_transport_error = HTTP_RESPONSE_ERROR_CONNECTION;
		if (caller_error) *caller_error = dicomweb_last_transport_error;
		return NULL;
	}
	http_response_t* response = remote_connection_request_with_headers_and_options(
		connection, remote->hostname, path, accept, remote->authorization_header, true,
		&effective_options);
	remote_connection_close_and_free(connection);
	if (caller_error) *caller_error = dicomweb_last_transport_error;
	return response;
}

static const char* dicomweb_transport_error(void) {
	return http_response_error_string(dicomweb_last_transport_error);
}

static http_response_t* dicomweb_get(const dicomweb_remote_image_t* remote,
                                     const char* path, const char* accept,
                                     bool reuse_worker_connection) {
	return dicomweb_get_with_options(remote, path, accept, reuse_worker_connection, NULL);
}

static bool configure_archive(dicomweb_remote_image_t* remote, const char* archive_url,
                              const char* username, const char* password) {
	if (!parse_archive_url(archive_url, remote, NULL)) return false;
	if (!set_basic_authorization(remote, username, password)) {
		set_status("DICOMweb credentials are too long.");
		return false;
	}
	return true;
}

static bool dicomweb_study_date_query_is_valid(const char* value) {
	if (!value || !value[0]) return true;
	size_t length = strlen(value);
	if (length != 8 && length != 9 && length != 17) return false;
	for (size_t i = 0; i < length; ++i) {
		bool is_range_separator = value[i] == '-' &&
		                          ((length == 9 && (i == 0 || i == 8)) ||
		                           (length == 17 && i == 8));
		if (!is_range_separator && (value[i] < '0' || value[i] > '9')) return false;
	}
	return true;
}

static bool dicomweb_search_text_is_safe(const char* value, size_t capacity) {
	if (!value || !memchr(value, '\0', capacity)) return false;
	for (const u8* p = (const u8*)value; *p; ++p) {
		if (*p < 0x20 || *p == 0x7f) return false;
	}
	return true;
}

static bool dicomweb_study_search_is_valid(const dicomweb_study_search_t* search) {
	if (!search) return true;
	if (!dicomweb_search_text_is_safe(search->study_instance_uid,
	                                  sizeof(search->study_instance_uid)) ||
	    !dicomweb_search_text_is_safe(search->patient_id, sizeof(search->patient_id)) ||
	    !dicomweb_search_text_is_safe(search->accession_number,
	                                  sizeof(search->accession_number)) ||
	    !dicomweb_search_text_is_safe(search->study_date, sizeof(search->study_date)) ||
	    !dicomweb_search_text_is_safe(search->description, sizeof(search->description))) {
		return false;
	}
	if (search->study_instance_uid[0] &&
	    !dicomweb_is_valid_uid(search->study_instance_uid,
	                           strlen(search->study_instance_uid))) return false;
	return dicomweb_study_date_query_is_valid(search->study_date);
}

static void dicomweb_async_task_func(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	dicomweb_async_task_t* task = *(dicomweb_async_task_t**)userdata;
	dicomweb_async_result_t* result = (dicomweb_async_result_t*)calloc(1, sizeof(*result));
	if (!result) {
		memset(task->password, 0, sizeof(task->password));
		atomic_decrement(&task->context->active_tasks);
		free(task);
		return;
	}
	result->kind = task->kind;
	result->generation = task->generation;
	if (task->generation != task->context->generation) {
		result->cancelled = true;
		copy_cstring(result->message, "Request cancelled.", sizeof(result->message));
		goto complete;
	}

	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, task->archive_url, task->username, task->password)) {
		copy_cstring(result->message,
		             dicomweb_last_status[0] ? dicomweb_last_status
		                                    : "Invalid DICOMweb archive configuration.",
		             sizeof(result->message));
		goto complete;
	}

	remote_request_options_t request_options = {
		.read_poll_timeout_ms = 250,
		.total_timeout_seconds = 30.0f,
		.cancellation_generation = &task->context->generation,
		.expected_generation = task->generation,
	};
	if (task->kind == DICOMWEB_ASYNC_OPEN_SERIES) {
		copy_cstring(remote.study_instance_uid, task->study_uid,
		             sizeof(remote.study_instance_uid));
		copy_cstring(remote.series_instance_uid, task->series_uid,
		             sizeof(remote.series_instance_uid));
		result->image = dicomweb_prepare_configured_series(
			remote, &request_options, &result->http_status,
			result->message, sizeof(result->message));
		if (task->generation != task->context->generation) {
			result->cancelled = true;
			copy_cstring(result->message, "Open cancelled.", sizeof(result->message));
		} else {
			result->succeeded = result->image != NULL;
		}
		goto complete;
	}

	char base_path[1400];
	i32 path_length = 0;
	switch (task->kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES:
			if (!dicomweb_build_study_query_path(base_path, sizeof(base_path), remote.root,
			                                      &task->study_search)) {
				copy_cstring(result->message, "Invalid or excessive DICOMweb study search.",
				             sizeof(result->message));
				goto complete;
			}
			path_length = (i32)strlen(base_path);
			break;
		case DICOMWEB_ASYNC_QUERY_SERIES:
			path_length = snprintf(base_path, sizeof(base_path), "%s/studies/%s/series",
			                       remote.root, task->study_uid);
			break;
		case DICOMWEB_ASYNC_QUERY_INSTANCES:
			path_length = snprintf(base_path, sizeof(base_path), "%s/studies/%s/series/%s/instances%s",
			                       remote.root, task->study_uid, task->series_uid,
			                       DICOMWEB_INSTANCE_INCLUDE_FIELDS);
			break;
		default:
			copy_cstring(result->message, "Invalid DICOMweb query.", sizeof(result->message));
			goto complete;
	}
	if (path_length <= 0 || path_length >= sizeof(base_path)) {
		copy_cstring(result->message, "DICOMweb query path is too long.",
		             sizeof(result->message));
		goto complete;
	}

	size_t item_size = dicomweb_async_item_size(task->kind);
	char previous_page_first_uid[65] = {0};
	for (i32 offset = 0; offset < DICOMWEB_QIDO_BROWSER_RESULT_LIMIT;) {
		char path[1500];
		const char separator = strchr(base_path, '?') ? '&' : '?';
		path_length = snprintf(path, sizeof(path), "%s%climit=%d&offset=%d",
		                       base_path, separator, DICOMWEB_QIDO_PAGE_SIZE, offset);
		if (path_length <= 0 || path_length >= sizeof(path)) {
			copy_cstring(result->message, "DICOMweb query path is too long.",
			             sizeof(result->message));
			goto complete;
		}
		http_response_t* response = dicomweb_get_with_options(
			&remote, path, "application/dicom+json", false, &request_options);
		if (task->generation != task->context->generation) {
			if (response) http_response_destroy(response);
			result->cancelled = true;
			copy_cstring(result->message, "Request cancelled.", sizeof(result->message));
			goto complete;
		}
		result->http_status = response ? response->status_code : 0;
		if (!response || response->status_code < 200 || response->status_code >= 300) {
			if (response) {
				snprintf(result->message, sizeof(result->message),
				         "DICOMweb query failed (HTTP %d).", result->http_status);
				http_response_destroy(response);
			} else {
				snprintf(result->message, sizeof(result->message),
				         "DICOMweb query failed: %s.", dicomweb_transport_error());
			}
			goto complete;
		}

		const char* json = (const char*)response->buffer.data;
		i32 page_count = dicomweb_async_count_items(task->kind, json,
		                                               response->content_length);
		if (page_count == 0) {
			http_response_destroy(response);
			break;
		}
		i32 remaining = DICOMWEB_QIDO_BROWSER_RESULT_LIMIT - result->count;
		i32 append_count = MIN(page_count, remaining);
		void* page_items = calloc((size_t)append_count, item_size);
		if (!page_items || dicomweb_async_parse_items(task->kind, json,
		                                                response->content_length,
		                                                page_items, append_count) != append_count) {
			free(page_items);
			http_response_destroy(response);
			copy_cstring(result->message, "Could not parse DICOMweb query results.",
			             sizeof(result->message));
			goto complete;
		}
		const char* page_first_uid = dicomweb_async_item_uid(task->kind, page_items);
		if (offset > 0 && strcmp(page_first_uid, previous_page_first_uid) == 0) {
			free(page_items);
			http_response_destroy(response);
			result->truncated = true;
			break;
		}
		copy_cstring(previous_page_first_uid, page_first_uid,
		             sizeof(previous_page_first_uid));
		void* items = realloc(result->items, (size_t)(result->count + append_count) * item_size);
		if (!items) {
			free(page_items);
			http_response_destroy(response);
			copy_cstring(result->message, "Out of memory while storing DICOMweb results.",
			             sizeof(result->message));
			goto complete;
		}
		result->items = items;
		memcpy((u8*)result->items + (size_t)result->count * item_size,
		       page_items, (size_t)append_count * item_size);
		free(page_items);
		result->count += append_count;
		http_response_destroy(response);
		if (append_count < page_count || result->count >= DICOMWEB_QIDO_BROWSER_RESULT_LIMIT) {
			result->truncated = true;
			break;
		}
		if (page_count > DICOMWEB_QIDO_PAGE_SIZE) break;
		if (page_count < DICOMWEB_QIDO_PAGE_SIZE) break;
		offset += page_count;
	}
	result->succeeded = true;
	if (result->truncated) {
		snprintf(result->message, sizeof(result->message),
		         "DICOMweb query returned the first %d results; refine the search to see more.",
		         result->count);
	} else {
		snprintf(result->message, sizeof(result->message),
		         "DICOMweb query returned %d result%s.",
		         result->count, result->count == 1 ? "" : "s");
	}

complete:
	/* The GUI logs accepted completions; stale results should stay silent. */
	memset(task->password, 0, sizeof(task->password));
	if (!completion_queue_post(&task->context->completions,
	                           DICOMWEB_ASYNC_COMPLETION_RESULT,
	                           &result, sizeof(result))) {
		dicomweb_async_result_destroy(result);
	}
	atomic_decrement(&task->context->active_tasks);
	free(task);
}

void dicomweb_async_context_init(dicomweb_async_context_t* context) {
	if (!context || context->initialized) return;
	memset(context, 0, sizeof(*context));
	context->completions = completion_queue_create(16);
	context->initialized = context->completions.entries != NULL;
}

void dicomweb_async_context_destroy(dicomweb_async_context_t* context) {
	if (!context || !context->initialized) return;
	dicomweb_async_cancel(context);
	while (context->active_tasks > 0) {
		if (!thread_pool_do_work(&global_thread_pool)) platform_sleep(1);
	}
	dicomweb_async_result_t* result = NULL;
	while (dicomweb_async_poll(context, &result)) {
		dicomweb_async_result_destroy(result);
	}
	completion_queue_destroy(&context->completions);
	memset(context, 0, sizeof(*context));
}

static i32 dicomweb_async_query_internal(dicomweb_async_context_t* context,
	                                     dicomweb_async_query_kind_t kind,
	                                     const char* archive_url,
	                                     const char* username,
	                                     const char* password,
	                                     const char* study_uid,
	                                     const char* series_uid,
	                                     const dicomweb_study_search_t* search) {
	if (!context || !context->initialized || !archive_url ||
	    kind <= DICOMWEB_ASYNC_QUERY_NONE || kind > DICOMWEB_ASYNC_OPEN_SERIES) {
		return 0;
	}
	if ((kind == DICOMWEB_ASYNC_QUERY_SERIES ||
	     kind == DICOMWEB_ASYNC_QUERY_INSTANCES || kind == DICOMWEB_ASYNC_OPEN_SERIES) &&
	    (!study_uid || !dicomweb_is_valid_uid(study_uid, strlen(study_uid)))) return 0;
	if ((kind == DICOMWEB_ASYNC_QUERY_INSTANCES || kind == DICOMWEB_ASYNC_OPEN_SERIES) &&
	    (!series_uid || !dicomweb_is_valid_uid(series_uid, strlen(series_uid)))) return 0;
	if (kind != DICOMWEB_ASYNC_QUERY_STUDIES && search) return 0;
	if (!dicomweb_study_search_is_valid(search)) return 0;
	dicomweb_async_task_t* task = (dicomweb_async_task_t*)calloc(1, sizeof(*task));
	if (!task) return 0;
	task->context = context;
	task->kind = kind;
	task->generation = atomic_increment(&context->generation);
	copy_cstring(task->archive_url, archive_url, sizeof(task->archive_url));
	if (username) copy_cstring(task->username, username, sizeof(task->username));
	if (password) copy_cstring(task->password, password, sizeof(task->password));
	if (study_uid) copy_cstring(task->study_uid, study_uid, sizeof(task->study_uid));
	if (series_uid) copy_cstring(task->series_uid, series_uid, sizeof(task->series_uid));
	if (search) task->study_search = *search;
	dicomweb_async_task_t* task_pointer = task;
	atomic_increment(&context->active_tasks);
	if (!thread_pool_submit_task(&global_thread_pool, dicomweb_async_task_func,
	                             &task_pointer, sizeof(task_pointer))) {
		atomic_decrement(&context->active_tasks);
		memset(task->password, 0, sizeof(task->password));
		free(task);
		return 0;
	}
	return task->generation;
}

i32 dicomweb_async_query(dicomweb_async_context_t* context,
	                     dicomweb_async_query_kind_t kind,
	                     const char* archive_url,
	                     const char* username,
	                     const char* password,
	                     const char* study_uid,
	                     const char* series_uid) {
	return dicomweb_async_query_internal(context, kind, archive_url, username, password,
	                                     study_uid, series_uid, NULL);
}

i32 dicomweb_async_query_studies(dicomweb_async_context_t* context,
	                             const char* archive_url,
	                             const char* username,
	                             const char* password,
	                             const dicomweb_study_search_t* search) {
	return dicomweb_async_query_internal(context, DICOMWEB_ASYNC_QUERY_STUDIES,
	                                     archive_url, username, password,
	                                     NULL, NULL, search);
}

i32 dicomweb_async_open_series_url(dicomweb_async_context_t* context,
                                   const char* series_url,
                                   const char* username,
                                   const char* password) {
	char resolved_url[2048];
	if (!dicomweb_resolve_series_url(resolved_url, sizeof(resolved_url), series_url)) return 0;
	dicomweb_remote_image_t remote = {0};
	if (!parse_series_url(resolved_url, &remote)) return 0;
	return dicomweb_async_query(context, DICOMWEB_ASYNC_OPEN_SERIES,
	                            resolved_url, username, password,
	                            remote.study_instance_uid, remote.series_instance_uid);
}

void dicomweb_async_cancel(dicomweb_async_context_t* context) {
	if (!context || !context->initialized) return;
	atomic_increment(&context->generation);
}

bool dicomweb_async_poll(dicomweb_async_context_t* context,
                         dicomweb_async_result_t** out_result) {
	if (!context || !context->initialized || !out_result) return false;
	completion_event_t event = {0};
	if (!completion_queue_poll(&context->completions, &event)) return false;
	if (event.kind != DICOMWEB_ASYNC_COMPLETION_RESULT) return false;
	memcpy(out_result, event.userdata, sizeof(*out_result));
	return true;
}

void dicomweb_async_result_destroy(dicomweb_async_result_t* result) {
	if (!result) return;
	free(result->items);
	if (result->image) {
		image_destroy(result->image);
		free(result->image);
	}
	free(result);
}

bool dicomweb_async_commit_open(app_state_t* app_state,
                                dicomweb_async_result_t* result) {
	if (!app_state || !result || result->kind != DICOMWEB_ASYNC_OPEN_SERIES ||
	    !result->succeeded || !result->image) return false;
	image_t* image = result->image;
	result->image = NULL;
	image->resource_id = global_next_resource_id++;
	unload_all_images(app_state);
	add_image(app_state, image, true, false);
	if (image->dicomweb.available_series_count > 1) show_slide_tray_window = true;
	set_status("Opened DICOMweb series with %d pyramid instances.",
	           image->dicomweb.instance_count);
	console_print("%s\n", dicomweb_last_status);
	return true;
}

i32 dicomweb_query_studies(const char* archive_url, const char* username, const char* password,
                           dicomweb_study_ref_t* out, i32 capacity) {
	dicomweb_last_status[0] = '\0';
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) {
		if (dicomweb_last_status[0]) console_print_error("%s\n", dicomweb_last_status);
		return 0;
	}
	char path[1024];
	i32 length = snprintf(path, sizeof(path), "%s/studies", remote.root);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb study query failed (HTTP %d).", response ? response->status_code : 0);
		console_print_error("%s\n", dicomweb_last_status);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_study_refs((const char*)response->buffer.data,
	                                     response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb archive returned %d studies.", count);
	console_print("%s\n", dicomweb_last_status);
	return count;
}

i32 dicomweb_query_series(const char* archive_url, const char* username, const char* password,
                          const char* study_uid, dicomweb_series_ref_t* out, i32 capacity) {
	if (!study_uid || !dicomweb_is_valid_uid(study_uid, strlen(study_uid))) return 0;
	dicomweb_last_status[0] = '\0';
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) {
		if (dicomweb_last_status[0]) console_print_error("%s\n", dicomweb_last_status);
		return 0;
	}
	char path[1200];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series", remote.root, study_uid);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb series query failed (HTTP %d).", response ? response->status_code : 0);
		console_print_error("%s\n", dicomweb_last_status);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_series_refs((const char*)response->buffer.data,
	                                      response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb study returned %d series.", count);
	console_print("%s\n", dicomweb_last_status);
	return count;
}

i32 dicomweb_query_instances(const char* archive_url, const char* username, const char* password,
                             const char* study_uid, const char* series_uid,
                             dicomweb_instance_ref_t* out, i32 capacity) {
	if (!study_uid || !series_uid ||
	    !dicomweb_is_valid_uid(study_uid, strlen(study_uid)) ||
	    !dicomweb_is_valid_uid(series_uid, strlen(series_uid))) return 0;
	dicomweb_last_status[0] = '\0';
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) {
		if (dicomweb_last_status[0]) console_print_error("%s\n", dicomweb_last_status);
		return 0;
	}
	char path[1400];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series/%s/instances%s",
	                      remote.root, study_uid, series_uid,
	                      DICOMWEB_INSTANCE_INCLUDE_FIELDS);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb instance query failed (HTTP %d).", response ? response->status_code : 0);
		console_print_error("%s\n", dicomweb_last_status);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_instance_refs((const char*)response->buffer.data,
	                                        response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb series returned %d instances.", count);
	console_print("%s\n", dicomweb_last_status);
	return count;
}

bool dicomweb_build_series_url(char* dest, size_t dest_size, const char* archive_url,
                               const char* study_uid, const char* series_uid) {
	if (!study_uid || !series_uid ||
	    !dicomweb_is_valid_uid(study_uid, strlen(study_uid)) ||
	    !dicomweb_is_valid_uid(series_uid, strlen(series_uid))) return false;
	dicomweb_remote_image_t remote = {0};
	if (!parse_archive_url(archive_url, &remote, NULL)) return false;
	i32 length = snprintf(dest, dest_size, "%s://%s:%d%s/studies/%s/series/%s",
	                      remote.use_tls ? "https" : "http", remote.hostname, remote.port,
	                      remote.root, study_uid, series_uid);
	return length > 0 && (size_t)length < dest_size;
}

static void load_available_series(dicomweb_remote_image_t* remote,
                                  const remote_request_options_t* request_options) {
	char base_path[1200];
	i32 length = snprintf(base_path, sizeof(base_path), "%s/studies/%s/series",
	                      remote->root, remote->study_instance_uid);
	if (length <= 0 || length >= sizeof(base_path)) return;
	dicomweb_series_ref_t* series = NULL;
	i32 count = 0;
	char previous_page_first_uid[65] = {0};
	for (i32 offset = 0; offset < DICOMWEB_QIDO_BROWSER_RESULT_LIMIT;) {
		char path[1300];
		length = snprintf(path, sizeof(path), "%s?limit=%d&offset=%d",
		                  base_path, DICOMWEB_QIDO_PAGE_SIZE, offset);
		if (length <= 0 || length >= sizeof(path)) goto fail;
		http_response_t* response = dicomweb_get_with_options(
			remote, path, "application/dicom+json", false, request_options);
		if (!response || response->status_code < 200 || response->status_code >= 300) {
			if (response) http_response_destroy(response);
			goto fail;
		}
		const char* json = (const char*)response->buffer.data;
		i32 page_count = dicomweb_count_series_refs(json, response->content_length);
		if (page_count == 0) {
			http_response_destroy(response);
			break;
		}
		if (page_count > DICOMWEB_QIDO_BROWSER_RESULT_LIMIT - count) {
			http_response_destroy(response);
			goto fail;
		}
		dicomweb_series_ref_t* resized = (dicomweb_series_ref_t*)realloc(
			series, (size_t)(count + page_count) * sizeof(*series));
		if (!resized) {
			http_response_destroy(response);
			goto fail;
		}
		series = resized;
		dicomweb_series_ref_t* page = series + count;
		if (dicomweb_parse_series_refs(json, response->content_length,
		                               page, page_count) != page_count) {
			http_response_destroy(response);
			goto fail;
		}
		if (offset > 0 && strcmp(page[0].series_instance_uid,
		                         previous_page_first_uid) == 0) {
			http_response_destroy(response);
			goto fail;
		}
		copy_cstring(previous_page_first_uid, page[0].series_instance_uid,
		             sizeof(previous_page_first_uid));
		count += page_count;
		http_response_destroy(response);
		if (count >= DICOMWEB_QIDO_BROWSER_RESULT_LIMIT &&
		    page_count == DICOMWEB_QIDO_PAGE_SIZE) goto fail;
		if (page_count != DICOMWEB_QIDO_PAGE_SIZE) break;
		offset += page_count;
	}
	free(remote->available_series);
	remote->available_series = series;
	remote->available_series_count = count;
	remote->available_series_capacity = count;
	remote->selected_series_index = -1;
	for (i32 i = 0; i < remote->available_series_count; ++i) {
		if (strcmp(remote->available_series[i].series_instance_uid,
		           remote->series_instance_uid) == 0) {
			remote->selected_series_index = i;
			break;
		}
	}
	return;

fail:
	free(series);
}

static int compare_instance_width_descending(const void* a, const void* b) {
	const dicomweb_instance_metadata_t* left = (const dicomweb_instance_metadata_t*)a;
	const dicomweb_instance_metadata_t* right = (const dicomweb_instance_metadata_t*)b;
	if (left->total_pixel_matrix_columns < right->total_pixel_matrix_columns) return 1;
	if (left->total_pixel_matrix_columns > right->total_pixel_matrix_columns) return -1;
	return 0;
}

static bool dicomweb_remote_reserve_instances(dicomweb_remote_image_t* remote, i32 capacity) {
	if (capacity <= remote->instance_capacity) return true;
	i32 new_capacity = MAX(capacity, MAX(8, remote->instance_capacity * 2));
	dicomweb_instance_metadata_t* instances = (dicomweb_instance_metadata_t*)realloc(
		remote->instances, (size_t)new_capacity * sizeof(*instances));
	if (!instances) return false;
	memset(instances + remote->instance_capacity, 0,
	       (size_t)(new_capacity - remote->instance_capacity) * sizeof(*instances));
	remote->instances = instances;
	remote->instance_capacity = new_capacity;
	return true;
}

static bool dicomweb_query_instance_refs_all(
	const dicomweb_remote_image_t* remote, const char* base_path,
	const remote_request_options_t* request_options,
	dicomweb_instance_ref_t** out_refs, i32* out_count,
	i32* http_status, char* message, size_t message_size) {
	*out_refs = NULL;
	*out_count = 0;
	if (http_status) *http_status = 0;
	char previous_page_first_uid[65] = {0};
	for (i32 offset = 0; offset < DICOMWEB_QIDO_OPEN_INSTANCE_LIMIT;) {
		char path[1500];
		const char separator = strchr(base_path, '?') ? '&' : '?';
		i32 length = snprintf(path, sizeof(path), "%s%climit=%d&offset=%d",
		                      base_path, separator, DICOMWEB_QIDO_PAGE_SIZE, offset);
		if (length <= 0 || length >= sizeof(path)) {
			copy_cstring(message, "DICOMweb series path is too long.", message_size);
			goto fail;
		}
		http_response_t* response = dicomweb_get_with_options(
			remote, path, "application/dicom+json", false, request_options);
		if (!response || response->status_code < 200 || response->status_code >= 300) {
			if (response) {
				if (http_status) *http_status = response->status_code;
				snprintf(message, message_size,
				         "DICOMweb QIDO-RS request failed (HTTP %d).",
				         response->status_code);
				http_response_destroy(response);
			} else {
				snprintf(message, message_size, "DICOMweb QIDO-RS request failed: %s.",
				         dicomweb_transport_error());
			}
			goto fail;
		}
		const char* json = (const char*)response->buffer.data;
		i32 page_count = dicomweb_count_instance_refs(json, response->content_length);
		if (page_count == 0) {
			http_response_destroy(response);
			return true;
		}
		if (page_count > DICOMWEB_QIDO_OPEN_INSTANCE_LIMIT - *out_count) {
			http_response_destroy(response);
			copy_cstring(message,
			             "DICOMweb series exceeds the 10000-instance safety limit.",
			             message_size);
			goto fail;
		}
		dicomweb_instance_ref_t* refs = (dicomweb_instance_ref_t*)realloc(
			*out_refs, (size_t)(*out_count + page_count) * sizeof(**out_refs));
		if (!refs) {
			http_response_destroy(response);
			copy_cstring(message, "Out of memory while storing DICOMweb instance results.",
			             message_size);
			goto fail;
		}
		*out_refs = refs;
		dicomweb_instance_ref_t* page = refs + *out_count;
		if (dicomweb_parse_instance_refs(json, response->content_length,
		                                 page, page_count) != page_count) {
			http_response_destroy(response);
			copy_cstring(message, "Could not parse DICOMweb instance results.", message_size);
			goto fail;
		}
		if (offset > 0 && strcmp(page[0].sop_instance_uid,
		                         previous_page_first_uid) == 0) {
			http_response_destroy(response);
			copy_cstring(message,
			             "DICOMweb server repeated a QIDO page; safe pagination is unavailable.",
			             message_size);
			goto fail;
		}
		copy_cstring(previous_page_first_uid, page[0].sop_instance_uid,
		             sizeof(previous_page_first_uid));
		*out_count += page_count;
		http_response_destroy(response);
		if (page_count < DICOMWEB_QIDO_PAGE_SIZE) return true;
		if (page_count > DICOMWEB_QIDO_PAGE_SIZE) return true;
		offset += page_count;
	}
	copy_cstring(message, "DICOMweb series exceeds the 10000-instance safety limit.",
	             message_size);

fail:
	free(*out_refs);
	*out_refs = NULL;
	*out_count = 0;
	return false;
}

static image_t* dicomweb_prepare_configured_series(
	dicomweb_remote_image_t remote, const remote_request_options_t* request_options,
	i32* http_status, char* message, size_t message_size) {
	if (http_status) *http_status = 0;
	i64 open_start = get_clock();
	remote.open_start_clock = open_start;
	remote.first_tile_upload_reported = false;
	load_available_series(&remote, request_options);
	char path[1400];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series/%s/instances%s",
	                      remote.root, remote.study_instance_uid, remote.series_instance_uid,
	                      DICOMWEB_INSTANCE_INCLUDE_FIELDS);
	if (length <= 0 || length >= sizeof(path)) {
		copy_cstring(message, "DICOMweb series path is too long.", message_size);
		dicomweb_remote_image_destroy(&remote);
		return NULL;
	}
	dicomweb_instance_ref_t* refs = NULL;
	i32 ref_count = 0;
	if (!dicomweb_query_instance_refs_all(&remote, path, request_options,
	                                      &refs, &ref_count, http_status,
	                                      message, message_size)) {
		dicomweb_remote_image_destroy(&remote);
		return NULL;
	}
	double discovery_ms = get_seconds_elapsed(open_start, get_clock()) * 1000.0;
	double metadata_http_ms = 0, metadata_parse_ms = 0;
	u64 metadata_bytes = 0;
	i32 metadata_request_failures = 0;
	i32 last_metadata_http_status = 0;
	http_response_error_t last_metadata_transport_error = HTTP_RESPONSE_ERROR_NONE;
	i32 metadata_parse_failures = 0;
	i32 unsupported_instances = 0;
	bool try_compact_metadata = true;
	for (i32 i = 0; i < ref_count; ++i) {
		if (request_options && request_options->cancellation_generation &&
		    *request_options->cancellation_generation != request_options->expected_generation) {
			break;
		}
		/* QIDO already identifies label/overview instances. They are not pyramid
		 * inputs, so avoid downloading their full metadata and ICC profiles. */
		if (refs[i].image_flavor[0] && strcmp(refs[i].image_flavor, "VOLUME") != 0) {
			++unsupported_instances;
			continue;
		}
		const char* study_uid = refs[i].study_instance_uid[0]
		                        ? refs[i].study_instance_uid : remote.study_instance_uid;
		const char* series_uid = refs[i].series_instance_uid[0]
		                         ? refs[i].series_instance_uid : remote.series_instance_uid;
		if (!dicomweb_build_instance_metadata_path(path, sizeof(path), remote.root,
		                                           study_uid, series_uid,
		                                           refs[i].sop_instance_uid)) continue;
		i64 metadata_start = get_clock();
		http_response_t* response = dicomweb_get_with_options(
			&remote, path, try_compact_metadata ? DICOMWEB_COMPACT_METADATA_MEDIA_TYPE : "application/dicom+json",
			false, request_options);
		/* Archives may reject the extension or ignore Accept and return DICOM
		 * JSON. IDC returns 500 for this unsupported media type. Retry once with
		 * standard metadata, then stop probing the extension for this series. */
		if (try_compact_metadata && response &&
		    (response->status_code == 400 || response->status_code == 406 ||
		     response->status_code == 415 || response->status_code == 500 ||
		     response->status_code == 501)) {
			http_response_destroy(response);
			try_compact_metadata = false;
			response = dicomweb_get_with_options(&remote, path, "application/dicom+json", false, request_options);
		}
		metadata_http_ms += get_seconds_elapsed(metadata_start, get_clock()) * 1000.0;
		if (!response || response->status_code < 200 || response->status_code >= 300) {
			++metadata_request_failures;
			if (response) {
				last_metadata_http_status = response->status_code;
				http_response_destroy(response);
			} else {
				last_metadata_transport_error = dicomweb_last_transport_error;
			}
			continue;
		}
		metadata_bytes += response->content_length;
		i64 parse_start = get_clock();
		dicomweb_instance_metadata_t metadata = {0};
		bool compact = strcmp(response->content_type, DICOMWEB_COMPACT_METADATA_MEDIA_TYPE) == 0;
		if (!compact) try_compact_metadata = false;
		bool parsed = compact ? dicomweb_parse_compact_metadata((const char*)response->buffer.data,
			response->content_length, &metadata) :
			dicomweb_parse_instance_metadata((const char*)response->buffer.data, response->content_length, &metadata);
		if (parsed && (strcmp(metadata.study_instance_uid, study_uid) != 0 ||
		    strcmp(metadata.series_instance_uid, series_uid) != 0 ||
		    strcmp(metadata.sop_instance_uid, refs[i].sop_instance_uid) != 0)) parsed = false;
		metadata_parse_ms += get_seconds_elapsed(parse_start, get_clock()) * 1000.0;
		if (parsed && strcmp(metadata.image_flavor, "VOLUME") == 0 &&
		    metadata.frame_numbers && metadata.frame_map_count) {
			if (dicomweb_remote_reserve_instances(&remote, remote.instance_count + 1)) {
				remote.instances[remote.instance_count++] = metadata;
			} else {
				dicomweb_instance_metadata_destroy(&metadata);
				free(refs);
				http_response_destroy(response);
				dicomweb_remote_image_destroy(&remote);
				copy_cstring(message, "Out of memory while opening DICOMweb metadata.", message_size);
				return NULL;
			}
		} else {
			if (parsed) ++unsupported_instances;
			else ++metadata_parse_failures;
			dicomweb_instance_metadata_destroy(&metadata);
		}
		http_response_destroy(response);
	}
	free(refs);
	if (remote.instance_count == 0) {
		if (metadata_request_failures > 0) {
			if (last_metadata_http_status) {
				if (http_status) *http_status = last_metadata_http_status;
				snprintf(message, message_size,
				         "DICOMweb could not retrieve metadata for %d instance%s (last HTTP status: %d).",
				         metadata_request_failures, metadata_request_failures == 1 ? "" : "s",
				         last_metadata_http_status);
			} else {
				snprintf(message, message_size,
				         "DICOMweb could not retrieve metadata for %d instance%s (last error: %s).",
				         metadata_request_failures, metadata_request_failures == 1 ? "" : "s",
				         http_response_error_string(last_metadata_transport_error));
			}
		} else if (metadata_parse_failures > 0) {
			snprintf(message, message_size,
			         "DICOMweb returned invalid or unsupported metadata for %d instance%s.",
			         metadata_parse_failures, metadata_parse_failures == 1 ? "" : "s");
		} else {
			snprintf(message, message_size,
			         "DICOMweb series has no supported tiled VOLUME instances (%d inspected).",
			         unsupported_instances);
		}
		dicomweb_remote_image_destroy(&remote);
		return NULL;
	}
	qsort(remote.instances, remote.instance_count, sizeof(remote.instances[0]),
	      compare_instance_width_descending);

	i64 image_start = get_clock();
	image_t* image = (image_t*)calloc(1, sizeof(image_t));
	if (!image || !init_image_from_dicomweb(image, &remote, false) ||
	    !image_initialize_runtime(image)) {
		if (image) {
			if (image->backend == IMAGE_BACKEND_DICOMWEB) image_destroy(image);
			else dicomweb_remote_image_destroy(&remote);
			free(image);
		} else {
			dicomweb_remote_image_destroy(&remote);
		}
		copy_cstring(message, "DICOMweb pyramid metadata is unsupported or inconsistent.",
		             message_size);
		return NULL;
	}
	console_print_verbose("DICOMweb open: total=%.2f ms, discovery=%.2f ms, metadata HTTP=%.2f ms (%llu bytes), metadata parse=%.2f ms, image setup=%.2f ms\n",
		get_seconds_elapsed(open_start, get_clock()) * 1000.0, discovery_ms,
		metadata_http_ms, (unsigned long long)metadata_bytes, metadata_parse_ms,
		get_seconds_elapsed(image_start, get_clock()) * 1000.0);
	snprintf(message, message_size, "Opened DICOMweb series with %d pyramid instances.",
	         remote.instance_count);
	return image;
}

static bool dicomweb_open_configured_series(app_state_t* app_state,
                                            dicomweb_remote_image_t remote) {
	char message[512];
	image_t* image = dicomweb_prepare_configured_series(
		remote, NULL, NULL, message, sizeof(message));
	if (!image) {
		set_status("%s", message);
		console_print_error("%s\n", message);
		return false;
	}
	image->resource_id = global_next_resource_id++;
	unload_all_images(app_state);
	add_image(app_state, image, true, false);
	if (image->dicomweb.available_series_count > 1) show_slide_tray_window = true;
	set_status("%s", message);
	console_print("%s\n", message);
	return true;
}

bool dicomweb_open_series(app_state_t* app_state, const char* series_url,
                          const char* username, const char* password) {
	dicomweb_last_status[0] = '\0';
	dicomweb_remote_image_t remote = {0};
	if (!parse_series_url(series_url, &remote)) {
		if (!dicomweb_last_status[0]) {
			set_status("Expected a DICOMweb series URL ending in /studies/{uid}/series/{uid}.");
		}
		console_print_error("%s\n", dicomweb_last_status);
		return false;
	}
	if (!set_basic_authorization(&remote, username, password)) {
		set_status("DICOMweb credentials are too long.");
		console_print_error("%s\n", dicomweb_last_status);
		return false;
	}
	return dicomweb_open_configured_series(app_state, remote);
}

bool dicomweb_open_related_series(app_state_t* app_state, const image_t* current_image,
                                  i32 series_index) {
	if (!current_image || current_image->backend != IMAGE_BACKEND_DICOMWEB) return false;
	dicomweb_remote_image_t remote = current_image->dicomweb;
	if (series_index < 0 || series_index >= remote.available_series_count) return false;
	remote.available_series = NULL;
	remote.available_series_count = 0;
	remote.available_series_capacity = 0;
	remote.instances = NULL;
	remote.instance_count = 0;
	remote.instance_capacity = 0;
	remote.selected_series_index = series_index;
	copy_cstring(remote.series_instance_uid,
	             current_image->dicomweb.available_series[series_index].series_instance_uid,
	             sizeof(remote.series_instance_uid));
	return dicomweb_open_configured_series(app_state, remote);
}

static u8* retrieve_frame_pixels(const dicomweb_remote_image_t* remote,
                                 i32 instance_index, u32 frame_number) {
	if (instance_index < 0 || instance_index >= remote->instance_count) return NULL;
	dicomweb_request_timing_t timing = {.start_clock = get_clock()};
	const dicomweb_instance_metadata_t* instance = remote->instances + instance_index;
	char path[1400];
	if (!dicomweb_build_frame_path(path, sizeof(path), remote->root,
	                               instance->study_instance_uid,
	                               instance->series_instance_uid,
	                               instance->sop_instance_uid, frame_number)) {
		dicomweb_record_tile_request(1, 0, true,
		                             &timing,
		                             0, 0, 0, 0);
		return NULL;
	}
	/* Advertise only compressed transfer syntaxes the viewer can decode locally. The
	 * Slidescape archive selects the published/stored syntax and never transcodes. */
	i64 http_start = get_clock();
	http_response_t* response = dicomweb_get(
		remote, path,
		"multipart/related; type=\"image/jpeg\"; transfer-syntax=1.2.840.10008.1.2.4.50, "
		"multipart/related; type=\"image/jp2\"; transfer-syntax=1.2.840.10008.1.2.4.90, "
		"multipart/related; type=\"image/jp2\"; transfer-syntax=1.2.840.10008.1.2.4.91",
		true);
	timing.http_ms = get_seconds_elapsed(http_start, get_clock()) * 1000.0;
	if (!response || response->status_code != 200) {
		if (response) http_response_destroy(response);
		dicomweb_record_tile_request(1, 0, true,
		                             &timing,
		                             0, 0, 0, 0);
		return NULL;
	}
	dicomweb_frame_t frame = {0};
	u8* pixels = NULL;
	if (dicomweb_parse_multipart_frame(response->buffer.data, response->content_length,
	                                   response->content_type, &frame)) {
		i64 decode_start = get_clock();
		pixels = dicomweb_decode_frame_to_bgra(instance, &frame);
		timing.decode_ms += get_seconds_elapsed(decode_start, get_clock()) * 1000.0;
	}
	http_response_destroy(response);
	dicomweb_record_tile_request(1, pixels ? 1 : 0, false,
	                             &timing,
	                             0, 0, 0, 0);
	return pixels;
}

static i32 retrieve_frame_pixels_batch(const dicomweb_remote_image_t* remote,
		i32 instance_index, const u32* frame_numbers, i32 frame_count, u8** pixels) {
	if (!remote || !frame_numbers || !pixels || frame_count <= 1 ||
	    frame_count > DICOMWEB_FRAME_BATCH_MAX ||
	    instance_index < 0 || instance_index >= remote->instance_count) return 0;
	dicomweb_request_timing_t timing = {.start_clock = get_clock()};
	const dicomweb_instance_metadata_t* instance = remote->instances + instance_index;
	char path[1600];
	if (!dicomweb_build_frames_path(path, sizeof(path), remote->root,
	                                instance->study_instance_uid,
	                                instance->series_instance_uid,
	                                instance->sop_instance_uid,
	                                frame_numbers, frame_count)) {
		dicomweb_record_tile_request(frame_count, 0, true,
		                             &timing,
		                             frame_count, 0, 0, 0);
		return 0;
	}
	i64 http_start = get_clock();
	http_response_t* response = dicomweb_get(
		remote, path,
		"multipart/related; type=\"image/jpeg\"; transfer-syntax=1.2.840.10008.1.2.4.50, "
		"multipart/related; type=\"image/jp2\"; transfer-syntax=1.2.840.10008.1.2.4.90, "
		"multipart/related; type=\"image/jp2\"; transfer-syntax=1.2.840.10008.1.2.4.91",
		true);
	timing.http_ms = get_seconds_elapsed(http_start, get_clock()) * 1000.0;
	if (!response || response->status_code != 200) {
		if (response) http_response_destroy(response);
		dicomweb_record_tile_request(frame_count, 0, true,
		                             &timing,
		                             frame_count, 0, 0, 0);
		return 0;
	}
	dicomweb_frame_t frames[DICOMWEB_FRAME_BATCH_MAX] = {0};
	i32 parsed_count = dicomweb_parse_multipart_frames(
		response->buffer.data, response->content_length, response->content_type,
		frames, frame_count);
	i32 decoded_count = 0;
	i32 fallback_multipart_count = 0;
	i32 fallback_mapping_count = 0;
	i32 fallback_decode_count = 0;
	if (parsed_count == frame_count) {
		bool consumed[DICOMWEB_FRAME_BATCH_MAX] = {0};
		bool has_frame_locations = false;
		for (i32 part_index = 0; part_index < parsed_count; ++part_index) {
			if (frames[part_index].frame_number) {
				has_frame_locations = true;
				break;
			}
		}
		for (i32 requested_index = 0; requested_index < frame_count; ++requested_index) {
			i32 match = -1;
			for (i32 part_index = 0; part_index < parsed_count; ++part_index) {
				if (!consumed[part_index] &&
				    frames[part_index].frame_number == frame_numbers[requested_index]) {
					match = part_index;
					break;
				}
			}
			/* Content-Location is mandatory for these parts, but preserve response-order
			 * compatibility with older servers that omit it. */
			if (match < 0 && !has_frame_locations &&
			    !consumed[requested_index]) match = requested_index;
			if (match >= 0) {
				consumed[match] = true;
				i64 decode_start = get_clock();
				pixels[requested_index] = dicomweb_decode_frame_to_bgra(instance, frames + match);
				timing.decode_ms += get_seconds_elapsed(decode_start, get_clock()) * 1000.0;
				if (pixels[requested_index]) {
					++decoded_count;
				} else {
					++fallback_decode_count;
				}
			} else {
				++fallback_mapping_count;
			}
		}
	} else {
		fallback_multipart_count = frame_count;
	}
	http_response_destroy(response);
	dicomweb_record_tile_request(frame_count, decoded_count, false,
	                             &timing,
	                             0, fallback_multipart_count,
	                             fallback_mapping_count, fallback_decode_count);
	return decoded_count;
}

static void post_dicomweb_tile_result(image_t* image, const load_tile_task_t* task,
		const level_image_t* level, u32 frame_number, u8* pixels) {
	tile_cache_result_t result = {0};
	result.resource_id = task->resource_id;
	result.pixel_memory = pixels;
	result.tile_width = level->tile_width;
	result.tile_height = level->tile_height;
	result.level = task->level;
	result.tile_index = task->tile_index;
	result.want_gpu_residency = task->need_gpu_residency;
	result.want_cpu_residency = task->need_cpu_residency;
	result.failed = frame_number != 0 && pixels == NULL;
	result.is_empty = frame_number == 0;
	if (!tile_cache_post_load_result(image, &result) && pixels) free(pixels);
}

void dicomweb_load_tile_batch_func(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	load_tile_task_batch_t* batch = (load_tile_task_batch_t*)userdata;
	image_t* image = batch->tile_tasks[0].image;
	i32 refcount_decrement = 0;
	for (i32 i = 0; i < batch->task_count; ++i) {
		refcount_decrement += batch->tile_tasks[i].refcount_to_decrement;
	}
	if (image->is_deleted) {
		atomic_subtract(&image->refcount, refcount_decrement);
		return;
	}
	for (i32 i = 0; i < batch->task_count; ++i) {
		load_tile_task_t* task = batch->tile_tasks + i;
		if (task->may_discard_if_stale &&
		    tile_cache_task_is_stale(image, task->level, task->tile_index, task->generation)) {
			if (!tile_cache_post_stale_result(image, task->resource_id, task->level, task->tile_index)) {
				tile_cache_cancel_decode(image, task->level, task->tile_index);
			}
			continue;
		}
		level_image_t* level = image->level_images + task->level;
		const dicomweb_instance_metadata_t* instance =
			image->dicomweb.instances + level->pyramid_image_index;
		u32 frame_number = task->tile_index < instance->frame_map_count
		                   ? instance->frame_numbers[task->tile_index] : 0;
		if (!frame_number) {
			post_dicomweb_tile_result(image, task, level, 0, NULL);
			continue;
		}

		load_tile_task_t* grouped_tasks[DICOMWEB_FRAME_BATCH_MAX] = {task};
		level_image_t* grouped_levels[DICOMWEB_FRAME_BATCH_MAX] = {level};
		u32 grouped_frames[DICOMWEB_FRAME_BATCH_MAX] = {frame_number};
		u8* grouped_pixels[DICOMWEB_FRAME_BATCH_MAX] = {0};
		i32 group_count = 1;
		for (i32 candidate_index = i + 1;
		     candidate_index < batch->task_count && group_count < DICOMWEB_FRAME_BATCH_MAX;
		     ++candidate_index) {
			load_tile_task_t* candidate = batch->tile_tasks + candidate_index;
			if (candidate->may_discard_if_stale &&
			    tile_cache_task_is_stale(image, candidate->level, candidate->tile_index,
			                             candidate->generation)) break;
			level_image_t* candidate_level = image->level_images + candidate->level;
			if (candidate_level->pyramid_image_index != level->pyramid_image_index) break;
			const dicomweb_instance_metadata_t* candidate_instance =
				image->dicomweb.instances + candidate_level->pyramid_image_index;
			u32 candidate_frame =
				candidate->tile_index < candidate_instance->frame_map_count
				? candidate_instance->frame_numbers[candidate->tile_index] : 0;
			if (!candidate_frame) break;
			grouped_tasks[group_count] = candidate;
			grouped_levels[group_count] = candidate_level;
			grouped_frames[group_count++] = candidate_frame;
		}

		if (group_count > 1) {
			retrieve_frame_pixels_batch(&image->dicomweb, level->pyramid_image_index,
			                            grouped_frames, group_count, grouped_pixels);
		}
		for (i32 group_index = 0; group_index < group_count; ++group_index) {
			if (!grouped_pixels[group_index]) {
				grouped_pixels[group_index] = retrieve_frame_pixels(
					&image->dicomweb, level->pyramid_image_index,
					grouped_frames[group_index]);
			}
			post_dicomweb_tile_result(image, grouped_tasks[group_index],
			                          grouped_levels[group_index],
			                          grouped_frames[group_index],
			                          grouped_pixels[group_index]);
		}
		i += group_count - 1;
	}
	atomic_subtract(&image->refcount, refcount_decrement);
}
