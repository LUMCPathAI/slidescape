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
#include "jpeg_decoder.h"
#include "remote.h"
#include "stringutils.h"
#include "tile_cache.h"
#include "tile_loader.h"
#include "viewer.h"

extern bool show_dicom_slides_window;

static char dicomweb_last_status[512];

typedef struct dicomweb_worker_connection_t {
	tls_connection_t* connection;
	char hostname[256];
	i32 port;
	bool use_tls;
	i64 last_used_clock;
} dicomweb_worker_connection_t;

static THREAD_LOCAL dicomweb_worker_connection_t dicomweb_worker_connection;

#define DICOMWEB_ASYNC_COMPLETION_RESULT 0x4449434fu
#define DICOMWEB_INSTANCE_INCLUDE_FIELDS \
	"?includefield=00080016&includefield=00080008&includefield=00200013" \
	"&includefield=00280008&includefield=00209161&includefield=00209162" \
	"&includefield=00209163&includefield=00480006&includefield=00480007"

typedef struct dicomweb_async_task_t {
	dicomweb_async_context_t* context;
	dicomweb_async_query_kind_t kind;
	i32 generation;
	char archive_url[1024];
	char username[256];
	char password[256];
	char study_uid[65];
	char series_uid[65];
} dicomweb_async_task_t;

static image_t* dicomweb_prepare_configured_series(
	dicomweb_remote_image_t remote, const remote_request_options_t* request_options,
	char* message, size_t message_size);

const char* dicomweb_get_last_status(void) {
	return dicomweb_last_status;
}

static void set_status(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(dicomweb_last_status, sizeof(dicomweb_last_status), format, args);
	va_end(args);
	console_print("%s\n", dicomweb_last_status);
}

static bool is_loopback_host(const char* hostname) {
	return strcmp(hostname, "127.0.0.1") == 0 ||
	       strcmp(hostname, "localhost") == 0 ||
	       strcmp(hostname, "::1") == 0;
}

static bool parse_archive_url(const char* url, dicomweb_remote_image_t* remote,
                              const char** out_path) {
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
	const char* colon = NULL;
	for (const char* p = authority; p < path; ++p) {
		if (*p == ':') colon = p;
	}
	const char* host_end = colon ? colon : path;
	size_t host_length = (size_t)(host_end - authority);
	if (!host_length || host_length >= sizeof(remote->hostname)) return false;
	memcpy(remote->hostname, authority, host_length);
	remote->hostname[host_length] = '\0';
	if (colon) {
		char port_string[16];
		size_t port_length = (size_t)(path - colon - 1);
		if (!port_length || port_length >= sizeof(port_string)) return false;
		memcpy(port_string, colon + 1, port_length);
		port_string[port_length] = '\0';
		remote->port = atoi(port_string);
		if (remote->port <= 0 || remote->port > 65535) return false;
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
	    !series_length || series_length >= sizeof(remote->series_instance_uid)) return false;
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
	if (reuse_worker_connection) {
		for (i32 attempt = 0; attempt < 2; ++attempt) {
			tls_connection_t* connection = get_worker_connection(remote);
			if (!connection) return NULL;
			http_response_t* response = remote_connection_request_with_headers_and_options(
				connection, remote->hostname, path, accept,
				remote->authorization_header, false, options);
			if (response) {
				dicomweb_worker_connection.last_used_clock = get_clock();
				return response;
			}
			drop_worker_connection();
		}
		return NULL;
	}
	tls_connection_t* connection = remote_connection_open_http(remote->hostname, remote->port,
	                                                           remote->use_tls);
	if (!connection) return NULL;
	http_response_t* response = remote_connection_request_with_headers_and_options(
		connection, remote->hostname, path, accept, remote->authorization_header, true, options);
	remote_connection_close_and_free(connection);
	return response;
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
			remote, &request_options, result->message, sizeof(result->message));
		if (task->generation != task->context->generation) {
			result->cancelled = true;
			copy_cstring(result->message, "Open cancelled.", sizeof(result->message));
		} else {
			result->succeeded = result->image != NULL;
		}
		goto complete;
	}

	char path[1400];
	i32 path_length = 0;
	switch (task->kind) {
		case DICOMWEB_ASYNC_QUERY_STUDIES:
			path_length = snprintf(path, sizeof(path), "%s/studies", remote.root);
			break;
		case DICOMWEB_ASYNC_QUERY_SERIES:
			path_length = snprintf(path, sizeof(path), "%s/studies/%s/series",
			                       remote.root, task->study_uid);
			break;
		case DICOMWEB_ASYNC_QUERY_INSTANCES:
			path_length = snprintf(path, sizeof(path), "%s/studies/%s/series/%s/instances%s",
			                       remote.root, task->study_uid, task->series_uid,
			                       DICOMWEB_INSTANCE_INCLUDE_FIELDS);
			break;
		default:
			copy_cstring(result->message, "Invalid DICOMweb query.", sizeof(result->message));
			goto complete;
	}
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
		snprintf(result->message, sizeof(result->message),
		         "DICOMweb query failed (HTTP %d).", result->http_status);
		if (response) http_response_destroy(response);
		goto complete;
	}

	const char* json = (const char*)response->buffer.data;
	size_t json_size = response->content_length;
	if (task->kind == DICOMWEB_ASYNC_QUERY_STUDIES) {
		result->count = dicomweb_count_study_refs(json, json_size);
		if (result->count > 0) {
			result->studies = (dicomweb_study_ref_t*)calloc(
				(size_t)result->count, sizeof(*result->studies));
			if (!result->studies ||
			    dicomweb_parse_study_refs(json, json_size, result->studies,
			                              result->count) != result->count) {
				copy_cstring(result->message, "Could not parse DICOMweb study results.",
				             sizeof(result->message));
				http_response_destroy(response);
				goto complete;
			}
		}
	} else if (task->kind == DICOMWEB_ASYNC_QUERY_SERIES) {
		result->count = dicomweb_count_series_refs(json, json_size);
		if (result->count > 0) {
			result->series = (dicomweb_series_ref_t*)calloc(
				(size_t)result->count, sizeof(*result->series));
			if (!result->series ||
			    dicomweb_parse_series_refs(json, json_size, result->series,
			                               result->count) != result->count) {
				copy_cstring(result->message, "Could not parse DICOMweb series results.",
				             sizeof(result->message));
				http_response_destroy(response);
				goto complete;
			}
		}
	} else {
		result->count = dicomweb_count_instance_refs(json, json_size);
		if (result->count > 0) {
			result->instances = (dicomweb_instance_ref_t*)calloc(
				(size_t)result->count, sizeof(*result->instances));
			if (!result->instances ||
			    dicomweb_parse_instance_refs(json, json_size, result->instances,
			                                  result->count) != result->count) {
				copy_cstring(result->message, "Could not parse DICOMweb instance results.",
				             sizeof(result->message));
				http_response_destroy(response);
				goto complete;
			}
		}
	}
	http_response_destroy(response);
	result->succeeded = true;
	snprintf(result->message, sizeof(result->message),
	         "DICOMweb query returned %d result%s.",
	         result->count, result->count == 1 ? "" : "s");

complete:
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

i32 dicomweb_async_query(dicomweb_async_context_t* context,
                         dicomweb_async_query_kind_t kind,
                         const char* archive_url,
                         const char* username,
                         const char* password,
                         const char* study_uid,
                         const char* series_uid) {
	if (!context || !context->initialized || !archive_url ||
	    kind <= DICOMWEB_ASYNC_QUERY_NONE || kind > DICOMWEB_ASYNC_OPEN_SERIES) {
		return 0;
	}
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
	if (image->dicomweb.available_series_count > 1) show_dicom_slides_window = true;
	set_status("Opened DICOMweb series with %d pyramid instances.",
	           image->dicomweb.instance_count);
	return true;
}

i32 dicomweb_query_studies(const char* archive_url, const char* username, const char* password,
                           dicomweb_study_ref_t* out, i32 capacity) {
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) return 0;
	char path[1024];
	i32 length = snprintf(path, sizeof(path), "%s/studies", remote.root);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb study query failed (HTTP %d).", response ? response->status_code : 0);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_study_refs((const char*)response->buffer.data,
	                                     response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb archive returned %d studies.", count);
	return count;
}

i32 dicomweb_query_series(const char* archive_url, const char* username, const char* password,
                          const char* study_uid, dicomweb_series_ref_t* out, i32 capacity) {
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) return 0;
	char path[1200];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series", remote.root, study_uid);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb series query failed (HTTP %d).", response ? response->status_code : 0);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_series_refs((const char*)response->buffer.data,
	                                      response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb study returned %d series.", count);
	return count;
}

i32 dicomweb_query_instances(const char* archive_url, const char* username, const char* password,
                             const char* study_uid, const char* series_uid,
                             dicomweb_instance_ref_t* out, i32 capacity) {
	dicomweb_remote_image_t remote = {0};
	if (!configure_archive(&remote, archive_url, username, password)) return 0;
	char path[1400];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series/%s/instances%s",
	                      remote.root, study_uid, series_uid,
	                      DICOMWEB_INSTANCE_INCLUDE_FIELDS);
	if (length <= 0 || length >= sizeof(path)) return 0;
	http_response_t* response = dicomweb_get(&remote, path, "application/dicom+json", false);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		set_status("DICOMweb instance query failed (HTTP %d).", response ? response->status_code : 0);
		if (response) http_response_destroy(response);
		return 0;
	}
	i32 count = dicomweb_parse_instance_refs((const char*)response->buffer.data,
	                                        response->content_length, out, capacity);
	http_response_destroy(response);
	set_status("DICOMweb series returned %d instances.", count);
	return count;
}

bool dicomweb_build_series_url(char* dest, size_t dest_size, const char* archive_url,
                               const char* study_uid, const char* series_uid) {
	dicomweb_remote_image_t remote = {0};
	if (!parse_archive_url(archive_url, &remote, NULL)) return false;
	i32 length = snprintf(dest, dest_size, "%s://%s:%d%s/studies/%s/series/%s",
	                      remote.use_tls ? "https" : "http", remote.hostname, remote.port,
	                      remote.root, study_uid, series_uid);
	return length > 0 && (size_t)length < dest_size;
}

static void load_available_series(dicomweb_remote_image_t* remote,
                                  const remote_request_options_t* request_options) {
	char path[1200];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series",
	                      remote->root, remote->study_instance_uid);
	if (length <= 0 || length >= sizeof(path)) return;
	http_response_t* response = dicomweb_get_with_options(
		remote, path, "application/dicom+json", false, request_options);
	if (!response || response->status_code < 200 || response->status_code >= 300) {
		if (response) http_response_destroy(response);
		return;
	}
	remote->available_series_count = dicomweb_parse_series_refs(
		(const char*)response->buffer.data, response->content_length,
		remote->available_series, COUNT(remote->available_series));
	http_response_destroy(response);
	remote->selected_series_index = -1;
	for (i32 i = 0; i < remote->available_series_count; ++i) {
		if (strcmp(remote->available_series[i].series_instance_uid,
		           remote->series_instance_uid) == 0) {
			remote->selected_series_index = i;
			break;
		}
	}
}

static int compare_instance_width_descending(const void* a, const void* b) {
	const dicomweb_instance_metadata_t* left = (const dicomweb_instance_metadata_t*)a;
	const dicomweb_instance_metadata_t* right = (const dicomweb_instance_metadata_t*)b;
	if (left->total_pixel_matrix_columns < right->total_pixel_matrix_columns) return 1;
	if (left->total_pixel_matrix_columns > right->total_pixel_matrix_columns) return -1;
	return 0;
}

static image_t* dicomweb_prepare_configured_series(
	dicomweb_remote_image_t remote, const remote_request_options_t* request_options,
	char* message, size_t message_size) {
	load_available_series(&remote, request_options);
	char path[1400];
	i32 length = snprintf(path, sizeof(path), "%s/studies/%s/series/%s/instances%s",
	                      remote.root, remote.study_instance_uid, remote.series_instance_uid,
	                      DICOMWEB_INSTANCE_INCLUDE_FIELDS);
	if (length <= 0 || length >= sizeof(path)) {
		copy_cstring(message, "DICOMweb series path is too long.", message_size);
		return NULL;
	}
	http_response_t* qido = dicomweb_get_with_options(
		&remote, path, "application/dicom+json", false, request_options);
	if (!qido || qido->status_code < 200 || qido->status_code >= 300) {
		snprintf(message, message_size, "DICOMweb QIDO-RS request failed (HTTP %d).",
		         qido ? qido->status_code : 0);
		if (qido) http_response_destroy(qido);
		return NULL;
	}
	dicomweb_instance_ref_t refs[DICOMWEB_MAX_INSTANCES];
	i32 ref_count = dicomweb_parse_instance_refs((const char*)qido->buffer.data,
	                                             qido->content_length, refs, COUNT(refs));
	http_response_destroy(qido);
	for (i32 i = 0; i < ref_count && remote.instance_count < DICOMWEB_MAX_INSTANCES; ++i) {
		if (request_options && request_options->cancellation_generation &&
		    *request_options->cancellation_generation != request_options->expected_generation) {
			break;
		}
		const char* study_uid = refs[i].study_instance_uid[0]
		                        ? refs[i].study_instance_uid : remote.study_instance_uid;
		const char* series_uid = refs[i].series_instance_uid[0]
		                         ? refs[i].series_instance_uid : remote.series_instance_uid;
		if (!dicomweb_build_instance_metadata_path(path, sizeof(path), remote.root,
		                                           study_uid, series_uid,
		                                           refs[i].sop_instance_uid)) continue;
		http_response_t* response = dicomweb_get_with_options(
			&remote, path, "application/dicom+json", false, request_options);
		if (!response || response->status_code < 200 || response->status_code >= 300) {
			if (response) http_response_destroy(response);
			continue;
		}
		dicomweb_instance_metadata_t metadata = {0};
		if (dicomweb_parse_instance_metadata((const char*)response->buffer.data,
		                                     response->content_length, &metadata) &&
		    strcmp(metadata.image_flavor, "VOLUME") == 0 && metadata.is_tiled_full) {
			remote.instances[remote.instance_count++] = metadata;
		} else {
			dicomweb_instance_metadata_destroy(&metadata);
		}
		http_response_destroy(response);
	}
	if (remote.instance_count == 0) {
		copy_cstring(message,
		             "DICOMweb series has no supported TILED_FULL VOLUME instances.",
		             message_size);
		return NULL;
	}
	qsort(remote.instances, remote.instance_count, sizeof(remote.instances[0]),
	      compare_instance_width_descending);

	image_t* image = (image_t*)calloc(1, sizeof(image_t));
	if (!image || !init_image_from_dicomweb(image, &remote, false)) {
		for (i32 i = 0; i < remote.instance_count; ++i) {
			dicomweb_instance_metadata_destroy(remote.instances + i);
		}
		if (image) {
			for (i32 i = 0; i < image->level_count; ++i) free(image->level_images[i].tiles);
			free(image);
		}
		copy_cstring(message, "DICOMweb pyramid metadata is unsupported or inconsistent.",
		             message_size);
		return NULL;
	}
	platform_mutex_init(&image->lock);
	image->lock_initialized = true;
	snprintf(message, message_size, "Opened DICOMweb series with %d pyramid instances.",
	         remote.instance_count);
	return image;
}

static bool dicomweb_open_configured_series(app_state_t* app_state,
                                            dicomweb_remote_image_t remote) {
	char message[512];
	image_t* image = dicomweb_prepare_configured_series(
		remote, NULL, message, sizeof(message));
	if (!image) {
		set_status("%s", message);
		return false;
	}
	image->resource_id = global_next_resource_id++;
	unload_all_images(app_state);
	add_image(app_state, image, true, false);
	if (image->dicomweb.available_series_count > 1) show_dicom_slides_window = true;
	set_status("%s", message);
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
		return false;
	}
	if (!set_basic_authorization(&remote, username, password)) {
		set_status("DICOMweb credentials are too long.");
		return false;
	}
	return dicomweb_open_configured_series(app_state, remote);
}

bool dicomweb_open_related_series(app_state_t* app_state, const image_t* current_image,
                                  i32 series_index) {
	if (!current_image || current_image->backend != IMAGE_BACKEND_DICOMWEB) return false;
	dicomweb_remote_image_t remote = current_image->dicomweb;
	if (series_index < 0 || series_index >= remote.available_series_count) return false;
	remote.instance_count = 0;
	remote.selected_series_index = series_index;
	copy_cstring(remote.series_instance_uid,
	             remote.available_series[series_index].series_instance_uid,
	             sizeof(remote.series_instance_uid));
	return dicomweb_open_configured_series(app_state, remote);
}

static u8* retrieve_frame_pixels(const dicomweb_remote_image_t* remote,
                                 i32 instance_index, u32 frame_number) {
	if (instance_index < 0 || instance_index >= remote->instance_count) return NULL;
	const dicomweb_instance_metadata_t* instance = remote->instances + instance_index;
	char path[1400];
	if (!dicomweb_build_frame_path(path, sizeof(path), remote->root,
	                               instance->study_instance_uid,
	                               instance->series_instance_uid,
	                               instance->sop_instance_uid, frame_number)) return NULL;
	http_response_t* response = dicomweb_get(
		remote, path,
		"multipart/related; type=\"image/jpeg\"; transfer-syntax=1.2.840.10008.1.2.4.50",
		true);
	if (!response || response->status_code != 200) {
		if (response) http_response_destroy(response);
		return NULL;
	}
	dicomweb_frame_t frame = {0};
	u8* pixels = NULL;
	if (dicomweb_parse_multipart_frame(response->buffer.data, response->content_length,
	                                   response->content_type, &frame) &&
	    strcmp(frame.media_type, "image/jpeg") == 0 &&
	    strcmp(frame.transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0) {
		i32 width = 0;
		i32 height = 0;
		i32 channels = 0;
		if (strcmp(instance->photometric_interpretation, "RGB") == 0) {
			pixels = jpeg_decode_rgb_image((u8*)frame.data, (u32)frame.size, &width, &height, &channels);
		} else {
			pixels = jpeg_decode_image((u8*)frame.data, (u32)frame.size, &width, &height, &channels);
		}
		if (pixels && (width != instance->columns || height != instance->rows || channels != 4)) {
			free(pixels);
			pixels = NULL;
		}
	}
	http_response_destroy(response);
	return pixels;
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
		u8* pixels = frame_number ? retrieve_frame_pixels(
			&image->dicomweb, level->pyramid_image_index, frame_number) : NULL;
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
	atomic_subtract(&image->refcount, refcount_decrement);
}
