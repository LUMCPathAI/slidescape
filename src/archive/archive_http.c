/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_http.h"
#include "dicomweb.h"

#include "common.h"
#include "archive_index.h"
#include "archive_instance_provider.h"
#include "archive_representation.h"
#include "archive_routes.h"
#include "memrw.h"
#include "platform.h"
#include "work_queue.h"
#include "timerutils.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET archive_socket_t;
#define ARCHIVE_INVALID_SOCKET INVALID_SOCKET
#define archive_socket_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int archive_socket_t;
#define ARCHIVE_INVALID_SOCKET (-1)
#define archive_socket_close close
#endif

#define ARCHIVE_REQUEST_HEADER_LIMIT (16 * 1024)
#define ARCHIVE_MAX_FRAMES_PER_REQUEST 64
#define ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE MEGABYTES(256)

#define ARCHIVE_REQUEST_DEADLINE_MS 30000
#define ARCHIVE_SOCKET_IDLE_TIMEOUT_MS 5000
#define ARCHIVE_RESPONSE_MEMORY_BUDGET MEGABYTES(512)

/* Opt-in stage timings, with no request URLs or patient metadata in the log. */
static bool archive_profile_requests;
static THREAD_LOCAL double archive_profile_send_ms;
static THREAD_LOCAL double archive_profile_read_ms;
static THREAD_LOCAL const char* archive_profile_route;

typedef struct archive_connection_t {
	archive_socket_t socket;
	i64 accepted_at;
} archive_connection_t;

typedef struct archive_client_task_t {
	archive_connection_t connection;
	const archive_repository_t* repository;
	const archive_index_t* index;
} archive_client_task_t;

static void archive_memrw_init_fixed(memrw_t* buffer, void* data, size_t capacity) {
	*buffer = (memrw_t){0};
	buffer->data = data;
	buffer->capacity = capacity;
	buffer->is_growing_disallowed = true;
}

static void archive_memrw_write_u64(memrw_t* buffer, u64 value) {
	char digits[32];
	size_t count = 0;
	do {
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	while (count) memrw_write(digits + --count, buffer, 1);
}

static bool archive_socket_startup(void) {
#if defined(_WIN32)
	WSADATA data;
	return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
	signal(SIGPIPE, SIG_IGN);
	return true;
#endif
}

static void archive_socket_shutdown(void) {
#if defined(_WIN32)
	WSACleanup();
#endif
}

static archive_socket_t archive_create_loopback_listener(i32 family, u16 port) {
	archive_socket_t server = socket(family, SOCK_STREAM, IPPROTO_TCP);
	if (server == ARCHIVE_INVALID_SOCKET) return ARCHIVE_INVALID_SOCKET;
	int reuse = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	int result;
	if (family == AF_INET6) {
		/* Keep the listeners independent: this socket is for ::1 only. */
		int ipv6_only = 1;
		setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY,
		           (const char*)&ipv6_only, sizeof(ipv6_only));
		struct sockaddr_in6 address = {0};
		address.sin6_family = AF_INET6;
		address.sin6_addr = in6addr_loopback;
		address.sin6_port = htons(port);
		result = bind(server, (struct sockaddr*)&address, sizeof(address));
	} else {
		struct sockaddr_in address = {0};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		result = bind(server, (struct sockaddr*)&address, sizeof(address));
	}
	if (result != 0 || listen(server, 16) != 0) {
		archive_socket_close(server);
		return ARCHIVE_INVALID_SOCKET;
	}
	return server;
}

static archive_socket_t archive_accept_loopback(archive_socket_t ipv4_server,
		archive_socket_t ipv6_server) {
	if (ipv6_server == ARCHIVE_INVALID_SOCKET) return accept(ipv4_server, NULL, NULL);
	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(ipv4_server, &read_fds);
	FD_SET(ipv6_server, &read_fds);
#if defined(_WIN32)
	i32 ready = select(0, &read_fds, NULL, NULL, NULL);
#else
	i32 ready = select(MAX(ipv4_server, ipv6_server) + 1, &read_fds, NULL, NULL, NULL);
#endif
	if (ready <= 0) return ARCHIVE_INVALID_SOCKET;
	if (FD_ISSET(ipv6_server, &read_fds)) return accept(ipv6_server, NULL, NULL);
	return accept(ipv4_server, NULL, NULL);
}

/* Apply the remaining absolute deadline to every blocking operation. A client
 * making tiny periodic progress cannot occupy a worker indefinitely. Queue time
 * counts too, since accepted_at is recorded by the listener. */
static bool archive_set_client_timeout(const archive_connection_t* connection) {
	i32 remaining_ms = ARCHIVE_REQUEST_DEADLINE_MS -
		(i32)(get_seconds_elapsed(connection->accepted_at, get_clock()) * 1000.0f);
	if (remaining_ms <= 0) return false;
	i32 timeout_ms = MIN(remaining_ms, ARCHIVE_SOCKET_IDLE_TIMEOUT_MS);
#if defined(_WIN32)
	DWORD timeout = (DWORD)timeout_ms;
#else
	struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
	return setsockopt(connection->socket, SOL_SOCKET, SO_RCVTIMEO,
		(const char*)&timeout, sizeof(timeout)) == 0 &&
		setsockopt(connection->socket, SOL_SOCKET, SO_SNDTIMEO,
		(const char*)&timeout, sizeof(timeout)) == 0;
}

static bool archive_send_all_impl(const archive_connection_t* socket, const char* data, size_t size) {
	while (size > 0) {
		if (!archive_set_client_timeout(socket)) return false;
#if defined(_WIN32)
		i32 sent = send(socket->socket, data, size > INT32_MAX ? INT32_MAX : (i32)size, 0);
#else
		ssize_t sent = send(socket->socket, data, size, 0);
#endif
		if (sent <= 0) return false;
		data += sent;
		size -= (size_t)sent;
	}
	return true;
}

static bool archive_send_all(const archive_connection_t* socket, const char* data, size_t size) {
	if (!archive_profile_requests) return archive_send_all_impl(socket, data, size);
	i64 start = get_clock();
	bool success = archive_send_all_impl(socket, data, size);
	archive_profile_send_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
	return success;
}

static bool archive_send_headers(const archive_connection_t* socket, i32 status, const char* reason,
		const char* content_type, size_t body_size) {
	char header[512];
	memrw_t builder;
	archive_memrw_init_fixed(&builder, header, sizeof(header));
	memrw_write_literal("HTTP/1.1 ", &builder);
	archive_memrw_write_u64(&builder, (u64)status);
	memrw_write_literal(" ", &builder);
	memrw_write_string(reason, &builder);
	memrw_write_literal("\r\nContent-Type: ", &builder);
	memrw_write_string(content_type, &builder);
	memrw_write_literal("\r\nContent-Length: ", &builder);
	archive_memrw_write_u64(&builder, (u64)body_size);
	memrw_write_literal("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", &builder);
	return archive_send_all(socket, header, builder.used_size);
}

static bool archive_send_body(const archive_connection_t* socket, i32 status, const char* reason,
		const char* content_type, const char* body, size_t body_size) {
	return archive_send_headers(socket, status, reason, content_type, body_size) &&
		archive_send_all(socket, body, body_size);
}

static bool archive_send_json(const archive_connection_t* socket, i32 status, const char* reason,
		const char* json) {
	return archive_send_body(socket, status, reason, "application/json", json, strlen(json));
}

static bool archive_read_request_headers_impl(const archive_connection_t* socket, char* request, size_t capacity) {
	size_t used = 0;
	while (used + 1 < capacity) {
		if (!archive_set_client_timeout(socket)) return false;
#if defined(_WIN32)
		int received = recv(socket->socket, request + used, (int)(capacity - used - 1), 0);
#else
		ssize_t received = recv(socket->socket, request + used, capacity - used - 1, 0);
#endif
		if (received <= 0) return false;
		used += (size_t)received;
		request[used] = '\0';
		if (strstr(request, "\r\n\r\n")) return true;
	}
	return false;
}


static bool archive_read_request_headers(const archive_connection_t* socket, char* request, size_t capacity) {
	if (!archive_profile_requests) return archive_read_request_headers_impl(socket, request, capacity);
	i64 start = get_clock();
	bool success = archive_read_request_headers_impl(socket, request, capacity);
	archive_profile_read_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
	return success;
}

static void archive_send_qido_response(const archive_connection_t* client, const archive_index_t* index,
		const archive_route_match_t* route, const char* target) {
	size_t capacity = MEGABYTES(1);
	char* body = malloc(capacity);
	size_t body_size = 0;
	bool parsed = false;
	bool written = false;
	if (!body) {
		archive_send_json(client, 500, "Internal Server Error", "{\"error\":\"qido_response_failed\"}\n");
		return;
	}
	if (route->kind == ARCHIVE_ROUTE_QIDO_STUDIES) {
		archive_study_query_t query;
		parsed = archive_study_query_parse(target, &query);
		written = parsed && archive_index_write_studies_json(index, &query, body, capacity, &body_size);
	} else if (route->kind == ARCHIVE_ROUTE_QIDO_SERIES) {
		archive_series_query_t query;
		parsed = archive_series_query_parse(target, route->study_uid, &query);
		written = parsed && archive_index_write_series_json(index, &query, body, capacity, &body_size);
	} else if (route->kind == ARCHIVE_ROUTE_QIDO_INSTANCES) {
		archive_instance_query_t query;
		parsed = archive_instance_query_parse(target, route->study_uid, route->series_uid, &query);
		written = parsed && archive_index_write_instances_json(index, &query, body, capacity, &body_size);
	}
	if (!parsed) {
		archive_send_json(client, 400, "Bad Request", "{\"error\":\"invalid_qido_query\"}\n");
	} else if (!written) {
		archive_send_json(client, 500, "Internal Server Error", "{\"error\":\"qido_response_failed\"}\n");
	} else {
		archive_send_body(client, 200, "OK", "application/dicom+json", body, body_size);
	}
	free(body);
}

static char archive_ascii_lower(char value) {
	return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static const char* archive_find_header(const char* headers, const char* name) {
	size_t name_length = strlen(name);
	for (const char* line = headers; line && *line; ) {
		const char* end = strstr(line, "\r\n");
		if (!end) break;
		if ((size_t)(end - line) > name_length && line[name_length] == ':') {
			size_t i = 0;
			while (i < name_length && archive_ascii_lower(line[i]) == archive_ascii_lower(name[i])) ++i;
			if (i == name_length) return line + name_length + 1;
		}
		line = end + 2;
	}
	return NULL;
}

/* The compact bootstrap is an explicit single-media-type opt-in. Ordinary
 * DICOMweb requests continue to receive the full standards representation. */
static bool archive_wants_compact_metadata(const char* headers) {
	const char* value = archive_find_header(headers, "Accept");
	if (!value) return false;
	while (*value == ' ' || *value == '\t') ++value;
	size_t length = strlen(DICOMWEB_COMPACT_METADATA_MEDIA_TYPE);
	if (strncasecmp(value, DICOMWEB_COMPACT_METADATA_MEDIA_TYPE, length) != 0) return false;
	value += length;
	while (*value == ' ' || *value == '\t') ++value;
	return value[0] == '\r' && value[1] == '\n';
}

static void archive_destroy_frames(archive_instance_frame_t* frames, i32 count) {
	for (i32 i = 0; i < count; ++i) archive_instance_frame_destroy(frames + i);
}

static void archive_write_frame_part_header(memrw_t* builder, bool include_separator,
		const char* boundary, const archive_instance_frame_t* frame,
		const archive_route_match_t* route, u32 frame_number) {
	if (include_separator) memrw_write_literal("\r\n", builder);
	memrw_write_literal("--", builder);
	memrw_write_string(boundary, builder);
	memrw_write_literal("\r\nContent-Type: ", builder);
	memrw_write_string(frame->media_type, builder);
	memrw_write_literal("; transfer-syntax=", builder);
	memrw_write_string(frame->transfer_syntax_uid, builder);
	memrw_write_literal("\r\nContent-Location: /dicom-web/studies/", builder);
	memrw_write_string(route->study_uid, builder);
	memrw_write_literal("/series/", builder);
	memrw_write_string(route->series_uid, builder);
	memrw_write_literal("/instances/", builder);
	memrw_write_string(route->instance_uid, builder);
	memrw_write_literal("/frames/", builder);
	archive_memrw_write_u64(builder, frame_number);
	memrw_write_literal("\r\n\r\n", builder);
}

static platform_mutex_t archive_response_budget_mutex = PLATFORM_MUTEX_INITIALIZER;
static size_t archive_response_bytes_reserved;

static void archive_send_wado_response_admitted(const archive_connection_t* client,
		const archive_repository_t* repository, const archive_index_t* index,
		const archive_route_match_t* route, const char* request_headers, size_t* reservation) {
	char error[512] = {0};
	if (route->kind == ARCHIVE_ROUTE_WADO_SERIES_METADATA) {
		const archive_instance_record_t* records = NULL;
		i32 record_count = 0;
		if (!archive_index_find_series_instances(
				index, route->study_uid, route->series_uid,
				&records, &record_count)) {
			archive_send_json(client, 404, "Not Found",
			                  "{\"error\":\"series_not_found\"}\n");
			return;
		}
		char* json = NULL;
		size_t json_size = 0;
		if (!archive_instance_provider_get_series_metadata(
				repository, records, record_count, &json, &json_size,
				error, sizeof(error))) {
			fprintf(stderr, "archive metadata retrieval failed: %s\n", error);
			archive_send_json(client, 409, "Conflict",
			                  "{\"error\":\"representation_unavailable\"}\n");
			return;
		}
		archive_send_body(client, 200, "OK", "application/dicom+json",
		                  json, json_size);
		free(json);
		return;
	}
	const archive_instance_record_t* record = archive_index_find_instance(index,
		route->study_uid, route->series_uid, route->instance_uid);
	if (!record) {
		archive_send_json(client, 404, "Not Found", "{\"error\":\"instance_not_found\"}\n");
		return;
	}
	if (route->kind == ARCHIVE_ROUTE_WADO_INSTANCE_METADATA) {
		if (archive_wants_compact_metadata(request_headers)) {
			char* compact = NULL;
			size_t compact_size = 0;
			if (archive_instance_provider_get_compact_metadata(repository, record,
					&compact, &compact_size, error, sizeof(error))) {
				archive_send_body(client, 200, "OK", DICOMWEB_COMPACT_METADATA_MEDIA_TYPE,
					compact, compact_size);
			} else {
				archive_send_json(client, 406, "Not Acceptable", "{\"error\":\"compact_metadata_unavailable\"}\n");
			}
			free(compact);
			return;
		}
		char* json = NULL;
		size_t json_size = 0;
		if (!archive_instance_provider_get_metadata(repository, record, &json, &json_size,
				error, sizeof(error))) {
			fprintf(stderr, "archive metadata retrieval failed: %s\n", error);
			archive_send_json(client, 409, "Conflict", "{\"error\":\"representation_unavailable\"}\n");
			return;
		}
		archive_send_body(client, 200, "OK", "application/dicom+json", json, json_size);
		free(json);
		return;
	}

	u32 frame_numbers[ARCHIVE_MAX_FRAMES_PER_REQUEST];
	i32 frame_count = 0;
	if (!archive_frame_list_parse(route->frames, frame_numbers, COUNT(frame_numbers), &frame_count)) {
		archive_send_json(client, 400, "Bad Request", "{\"error\":\"invalid_or_excessive_frame_list\"}\n");
		return;
	}

	const char* accept = archive_find_header(request_headers, "Accept");
	const char* accept_end = accept ? strstr(accept, "\r\n") : NULL;
	size_t accept_size = accept && accept_end ? (size_t)(accept_end - accept) : 0;
	if (!accept_size) {
		archive_send_json(client, 406, "Not Acceptable",
		                  "{\"error\":\"accept_header_required\"}\n");
		return;
	}
	archive_frame_representation_t available[ARCHIVE_MAX_FRAME_REPRESENTATIONS] = {0};
	i32 available_count = archive_instance_provider_get_frame_representations(
		repository, record, available, COUNT(available), error, sizeof(error));
	if (available_count <= 0) {
		fprintf(stderr, "archive representation lookup failed: %s\n", error);
		archive_send_json(client, 409, "Conflict",
		                  "{\"error\":\"representation_unavailable\"}\n");
		return;
	}
	archive_frame_representation_t selected = {0};
	archive_representation_selection_result_t selection =
		archive_select_frame_representation(accept, accept_size, available,
		                                    available_count, &selected);
	if (selection != ARCHIVE_REPRESENTATION_SELECTED) {
		archive_send_json(client, 406, "Not Acceptable",
		                  "{\"error\":\"representation_not_acceptable\"}\n");
		return;
	}

	size_t frame_bytes = 0;
	if (!archive_instance_provider_get_frame_buffer_size(repository, record, frame_numbers,
			frame_count, &selected, &frame_bytes)) {
		archive_send_json(client, 409, "Conflict", "{\"error\":\"frame_unavailable\"}\n");
		return;
	}
	/* Reserve actual shell-derived bounds, not 256 MiB for a few KiB of JPEG.
	 * The old fixed reservation effectively limited the server to two requests. */
	size_t overhead = (size_t)frame_count * 768 + 64;
	if (frame_bytes > ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE - overhead) {
		archive_send_json(client, 413, "Content Too Large", "{\"error\":\"frame_response_too_large\"}\n");
		return;
	}
	platform_mutex_lock(&archive_response_budget_mutex);
	bool admitted = frame_bytes + overhead <= ARCHIVE_RESPONSE_MEMORY_BUDGET - archive_response_bytes_reserved;
	if (admitted) {
		*reservation = frame_bytes + overhead;
		archive_response_bytes_reserved += *reservation;
	}
	platform_mutex_unlock(&archive_response_budget_mutex);
	if (!admitted) {
		archive_send_json(client, 503, "Service Unavailable", "{\"error\":\"response_budget_busy\"}\n");
		return;
	}

	archive_instance_frame_t* frames = arena_push_array(
		&threadlocal_thread_memory->temp_arena,
		ARCHIVE_MAX_FRAMES_PER_REQUEST, archive_instance_frame_t);
	memset(frames, 0, ARCHIVE_MAX_FRAMES_PER_REQUEST * sizeof(*frames));
	static const char boundary[] = "slidescape-frame-boundary";
	size_t body_size = 0;
	for (i32 i = 0; i < frame_count; ++i) {
		if (!archive_set_client_timeout(client)) {
			archive_destroy_frames(frames, i);
			return;
		}
		if (!archive_instance_provider_get_frame(repository, record, frame_numbers[i],
				&selected, frames + i, error, sizeof(error))) {
			fprintf(stderr, "archive frame retrieval failed: %s\n", error);
			archive_destroy_frames(frames, i);
			archive_send_json(client, 409, "Conflict", "{\"error\":\"frame_unavailable\"}\n");
			return;
		}
		char part_header[768];
		memrw_t part_header_builder;
		archive_memrw_init_fixed(&part_header_builder, part_header, sizeof(part_header));
		archive_write_frame_part_header(&part_header_builder, i != 0, boundary,
		                                frames + i, route, frame_numbers[i]);
		size_t part_header_size = part_header_builder.used_size;
		if (frames[i].size > ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE - body_size ||
		    part_header_size > ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE -
		                               body_size - frames[i].size) {
			archive_destroy_frames(frames, i + 1);
			archive_send_json(client, 413, "Content Too Large",
			                  "{\"error\":\"frame_response_too_large\"}\n");
			return;
		}
		body_size += part_header_size + frames[i].size;
	}
	char closing[64];
	memrw_t closing_builder;
	archive_memrw_init_fixed(&closing_builder, closing, sizeof(closing));
	memrw_write_literal("\r\n--", &closing_builder);
	memrw_write_string(boundary, &closing_builder);
	memrw_write_literal("--\r\n", &closing_builder);
	size_t closing_size = closing_builder.used_size;
	if (closing_size > ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE - body_size) {
		archive_destroy_frames(frames, frame_count);
		archive_send_json(client, 500, "Internal Server Error", "{\"error\":\"frame_response_failed\"}\n");
		return;
	}
	body_size += closing_size;
	char content_type[160];
	memrw_t content_type_builder;
	archive_memrw_init_fixed(&content_type_builder, content_type, sizeof(content_type));
	memrw_write_literal("multipart/related; type=\"", &content_type_builder);
	memrw_write_string(frames[0].media_type, &content_type_builder);
	memrw_write_literal("\"; transfer-syntax=", &content_type_builder);
	memrw_write_string(frames[0].transfer_syntax_uid, &content_type_builder);
	memrw_write_literal("; boundary=", &content_type_builder);
	memrw_write_string(boundary, &content_type_builder);
	memrw_putc('\0', &content_type_builder);
	/* Fetch and validate all frames before committing the response headers, then
	 * send them directly. There is no second allocation/copy of the whole body. */
	bool sent = archive_send_headers(client, 200, "OK", content_type, body_size);
	for (i32 i = 0; i < frame_count; ++i) {
		char part_header[768];
		memrw_t part_header_builder;
		archive_memrw_init_fixed(&part_header_builder, part_header, sizeof(part_header));
		archive_write_frame_part_header(&part_header_builder, i != 0, boundary,
			frames + i, route, frame_numbers[i]);
		sent = sent && archive_send_all(client, part_header, part_header_builder.used_size) &&
			archive_send_all(client, (const char*)frames[i].data, frames[i].size);
		archive_instance_frame_destroy(frames + i);
	}
	if (sent) archive_send_all(client, closing, closing_size);
}

static void archive_send_wado_response(const archive_connection_t* client,
		const archive_repository_t* repository, const archive_index_t* index,
		const archive_route_match_t* route, const char* request_headers) {
	size_t reservation = route->kind == ARCHIVE_ROUTE_WADO_FRAMES ? 0 : ARCHIVE_MAX_MULTIPART_RESPONSE_SIZE;
	if (route->kind == ARCHIVE_ROUTE_WADO_INSTANCE_METADATA && archive_wants_compact_metadata(request_headers))
		reservation = DICOMWEB_MAX_COMPACT_METADATA_SIZE;
	platform_mutex_lock(&archive_response_budget_mutex);
	bool admitted = reservation <= ARCHIVE_RESPONSE_MEMORY_BUDGET - archive_response_bytes_reserved;
	if (admitted) archive_response_bytes_reserved += reservation;
	platform_mutex_unlock(&archive_response_budget_mutex);
	if (!admitted) {
		archive_send_json(client, 503, "Service Unavailable", "{\"error\":\"response_budget_busy\"}\n");
		return;
	}
	archive_send_wado_response_admitted(client, repository, index, route, request_headers, &reservation);
	platform_mutex_lock(&archive_response_budget_mutex);
	archive_response_bytes_reserved -= reservation;
	platform_mutex_unlock(&archive_response_budget_mutex);
}

static void archive_handle_request_with_buffer(const archive_connection_t* client,
		const archive_repository_t* repository, const archive_index_t* index,
		char* request) {
	if (!archive_read_request_headers(client, request, ARCHIVE_REQUEST_HEADER_LIMIT + 1)) {
		archive_send_json(client, 400, "Bad Request", "{\"error\":\"invalid_request_headers\"}\n");
		return;
	}
	char method_text[16] = {0};
	char target[2048] = {0};
	char version[16] = {0};
	char* request_line_end = strstr(request, "\r\n");
	if (!request_line_end) {
		archive_send_json(client, 400, "Bad Request", "{\"error\":\"invalid_request_line\"}\n");
		return;
	}
	*request_line_end = '\0';
	const char* request_headers = request_line_end + 2;
	i32 consumed = 0;
	if (sscanf(request, "%15s %2047s %15s%n", method_text, target, version, &consumed) != 3 ||
	    request[consumed] != '\0' || strcmp(version, "HTTP/1.1") != 0) {
		archive_send_json(client, 400, "Bad Request", "{\"error\":\"invalid_request_line\"}\n");
		return;
	}
	archive_http_method_t method = archive_http_method_parse(method_text);
	if (method == ARCHIVE_HTTP_METHOD_UNKNOWN) {
		archive_send_json(client, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}\n");
		return;
	}
	archive_route_match_t route;
	if (!archive_route_match(method, target, &route)) {
		archive_send_json(client, 404, "Not Found", "{\"error\":\"route_not_found\"}\n");
		return;
	}
	archive_profile_route = archive_route_kind_string(route.kind);
	if (route.kind == ARCHIVE_ROUTE_HEALTH) {
		archive_instance_provider_cache_stats_t stats =
			archive_instance_provider_get_cache_stats();
		char body[1024];
		memrw_t builder;
		archive_memrw_init_fixed(&builder, body, sizeof(body));
		memrw_write_literal("{\"status\":\"ok\",\"service\":\"slidescape-pathology-archive\","
			"\"repositorySchemaVersion\":", &builder);
		archive_memrw_write_u64(&builder, ARCHIVE_REPOSITORY_SCHEMA_VERSION);
		memrw_write_literal(",\"part10Cache\":{\"resident\":", &builder);
		archive_memrw_write_u64(&builder, (u64)stats.resident_count);
		memrw_write_literal(",\"capacity\":", &builder);
		archive_memrw_write_u64(&builder, (u64)stats.capacity);
		memrw_write_literal(",\"hits\":", &builder);
		archive_memrw_write_u64(&builder, stats.hits);
		memrw_write_literal(",\"misses\":", &builder);
		archive_memrw_write_u64(&builder, stats.misses);
		memrw_write_literal(",\"evictions\":", &builder);
		archive_memrw_write_u64(&builder, stats.evictions);
		memrw_write_literal(",\"payloadBytes\":", &builder);
		archive_memrw_write_u64(&builder, stats.payload_bytes);
		memrw_write_literal("},\"shellCache\":{\"resident\":", &builder);
		archive_memrw_write_u64(&builder, (u64)stats.shell_resident_count);
		memrw_write_literal(",\"capacity\":", &builder);
		archive_memrw_write_u64(&builder, (u64)stats.shell_capacity);
		memrw_write_literal(",\"hits\":", &builder);
		archive_memrw_write_u64(&builder, stats.shell_hits);
		memrw_write_literal(",\"misses\":", &builder);
		archive_memrw_write_u64(&builder, stats.shell_misses);
		memrw_write_literal(",\"evictions\":", &builder);
		archive_memrw_write_u64(&builder, stats.shell_evictions);
		memrw_write_literal(",\"bytes\":", &builder);
		archive_memrw_write_u64(&builder, stats.shell_bytes);
		memrw_write_literal("}}\n", &builder);
		memrw_putc('\0', &builder);
		archive_send_json(client, 200, "OK", body);
		return;
	}
	if (route.kind == ARCHIVE_ROUTE_QIDO_STUDIES || route.kind == ARCHIVE_ROUTE_QIDO_SERIES ||
	    route.kind == ARCHIVE_ROUTE_QIDO_INSTANCES) {
		archive_send_qido_response(client, index, &route, target);
		return;
	}
	if (route.kind == ARCHIVE_ROUTE_WADO_SERIES_METADATA ||
	    route.kind == ARCHIVE_ROUTE_WADO_INSTANCE_METADATA ||
	    route.kind == ARCHIVE_ROUTE_WADO_FRAMES) {
		archive_send_wado_response(client, repository, index, &route, request_headers);
		return;
	}
	char body[256];
	memrw_t builder;
	archive_memrw_init_fixed(&builder, body, sizeof(body));
	memrw_write_literal("{\"error\":\"not_implemented\",\"route\":\"", &builder);
	memrw_write_string(archive_route_kind_string(route.kind), &builder);
	memrw_write_literal("\"}\n", &builder);
	memrw_putc('\0', &builder);
	archive_send_json(client, 501, "Not Implemented", body);
}

static void archive_handle_request(const archive_connection_t* client, const archive_repository_t* repository,
		const archive_index_t* index) {
	temp_memory_t temp = begin_temp_memory_on_local_thread();
	char* request = arena_push_array(&threadlocal_thread_memory->temp_arena,
	                                 ARCHIVE_REQUEST_HEADER_LIMIT + 1, char);
	memset(request, 0, ARCHIVE_REQUEST_HEADER_LIMIT + 1);
	archive_handle_request_with_buffer(client, repository, index, request);
	release_temp_memory(&temp);
}

static void archive_handle_client_task(i32 logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	archive_client_task_t* task = (archive_client_task_t*)userdata;
	i64 start = archive_profile_requests ? get_clock() : 0;
	archive_profile_send_ms = archive_profile_read_ms = 0;
	archive_profile_route = "invalid";
	if (archive_set_client_timeout(&task->connection))
		archive_handle_request(&task->connection, task->repository, task->index);
	archive_socket_close(task->connection.socket);
	if (archive_profile_requests) {
		double elapsed = get_seconds_elapsed(start, get_clock()) * 1000.0;
		fprintf(stderr, "archive timing: route=%s queue=%.3f ms receive=%.3f ms work=%.3f ms send=%.3f ms total=%.3f ms\n",
			archive_profile_route,
			get_seconds_elapsed(task->connection.accepted_at, start) * 1000.0,
			archive_profile_read_ms, MAX(0.0, elapsed - archive_profile_read_ms - archive_profile_send_ms),
			archive_profile_send_ms, get_seconds_elapsed(task->connection.accepted_at, get_clock()) * 1000.0);
	}
}

int archive_http_serve(const archive_repository_t* repository, u16 port,
		size_t max_requests) {
	if (!repository || !repository->root[0] || !archive_socket_startup()) return 1;
	const char* profile = getenv("SLIDESCAPE_ARCHIVE_PROFILE");
	archive_profile_requests = profile && strcmp(profile, "1") == 0;
	if (!archive_instance_provider_initialize()) {
		fprintf(stderr, "Could not initialize archive instance providers\n");
		archive_socket_shutdown();
		return 1;
	}
	archive_index_t index = {0};
	char index_error[512] = {0};
	if (!archive_index_open(&index, repository, index_error, sizeof(index_error))) {
		fprintf(stderr, "Could not open published archive index: %s\n", index_error);
		archive_instance_provider_shutdown();
		archive_socket_shutdown();
		return 1;
	}
	archive_socket_t ipv4_server = archive_create_loopback_listener(AF_INET, port);
	if (ipv4_server == ARCHIVE_INVALID_SOCKET) {
		fprintf(stderr, "Could not bind archive to 127.0.0.1:%u\n", (unsigned)port);
		archive_index_destroy(&index);
		archive_instance_provider_shutdown();
		archive_socket_shutdown();
		return 1;
	}
	archive_socket_t ipv6_server = archive_create_loopback_listener(AF_INET6, port);
	if (ipv6_server == ARCHIVE_INVALID_SOCKET) {
		fprintf(stderr, "Warning: could not bind archive to [::1]:%u; IPv4 remains available\n",
		        (unsigned)port);
		printf("Slidescape pathology archive listening on http://127.0.0.1:%u\n", (unsigned)port);
	} else {
		printf("Slidescape pathology archive listening on http://localhost:%u (127.0.0.1 and ::1)\n",
		       (unsigned)port);
	}
	printf("Repository: %s\n", repository->root);
	printf("QIDO and Part 10 instance metadata/negotiated frame WADO are enabled on %d worker threads; other routes return 501.\n",
	       thread_pool_get_active_worker_thread_count(&global_thread_pool));
	fflush(stdout);

	size_t handled = 0;
	while (max_requests == 0 || handled < max_requests) {
		archive_socket_t client = archive_accept_loopback(ipv4_server, ipv6_server);
		if (client == ARCHIVE_INVALID_SOCKET) {
			fprintf(stderr, "Archive accept failed\n");
			break;
		}
		archive_client_task_t task = {{client, get_clock()}, repository, &index};
		if (!thread_pool_submit_task(&global_thread_pool, archive_handle_client_task,
		                             &task, sizeof(task))) {
			archive_send_json(&task.connection, 503, "Service Unavailable", "{\"error\":\"server_busy\"}\n");
			archive_socket_close(client);
		} else if (thread_pool_get_active_worker_thread_count(&global_thread_pool) == 0) {
			/* A single-core host has no background worker; thread 0 remains a valid
			 * logical worker with the same temporary-arena contract. */
			thread_pool_do_work(&global_thread_pool);
		}
		++handled;
	}
	archive_socket_close(ipv4_server);
	if (ipv6_server != ARCHIVE_INVALID_SOCKET) archive_socket_close(ipv6_server);
	thread_pool_wait_for_completion(&global_thread_pool);
	archive_index_destroy(&index);
	archive_instance_provider_shutdown();
	archive_socket_shutdown();
	return max_requests != 0 && handled == max_requests ? 0 : 1;
}
