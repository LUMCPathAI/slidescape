/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

// NOTE: we need to guarantee Winsock 2 is included 
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

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
typedef SOCKET archive_socket_t;
#define ARCHIVE_INVALID_SOCKET INVALID_SOCKET
#define archive_socket_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#define ARCHIVE_MAX_IDLE_CONNECTIONS 32
#define ARCHIVE_MAX_REQUESTS_PER_CONNECTION 256
#define ARCHIVE_LISTENER_POLL_MS 5
#define ARCHIVE_RESPONSE_MEMORY_BUDGET MEGABYTES(512)

/* Opt-in stage timings, with no request URLs or patient metadata in the log. */
static bool archive_profile_requests;
static THREAD_LOCAL double archive_profile_send_ms;
static THREAD_LOCAL double archive_profile_read_ms;
static THREAD_LOCAL const char* archive_profile_route;

typedef struct archive_connection_t {
	archive_socket_t socket;
	i64 accepted_at;
	i64 idle_since;
	i32 request_count;
	bool keep_alive;
	bool sent_headers;
	bool send_failed;
	size_t body_remaining;
} archive_connection_t;

typedef struct archive_client_task_t {
	archive_connection_t* connection;
	const archive_repository_t* repository;
	const archive_index_t* index;
} archive_client_task_t;

/* Workers hand completed connections back to the listener. Only the listener
 * waits for the next request, so idle clients do not occupy its worker pool. */
static platform_mutex_t archive_returned_mutex = PLATFORM_MUTEX_INITIALIZER;
static archive_connection_t* archive_returned[ARCHIVE_MAX_IDLE_CONNECTIONS];
static i32 archive_returned_count;

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

static void archive_close_connection(archive_connection_t* connection) {
	if (!connection) return;
	archive_socket_close(connection->socket);
	free(connection);
}

static void archive_add_idle_connection(archive_connection_t** idle, i32* count,
		archive_connection_t* connection) {
#if !defined(_WIN32)
	if (connection->socket >= FD_SETSIZE) {
		archive_close_connection(connection);
		return;
	}
#endif
	if (*count >= ARCHIVE_MAX_IDLE_CONNECTIONS) {
		archive_close_connection(connection);
		return;
	}
	idle[(*count)++] = connection;
}

static void archive_collect_returned_connections(archive_connection_t** idle, i32* idle_count) {
	archive_connection_t* returned[ARCHIVE_MAX_IDLE_CONNECTIONS];
	platform_mutex_lock(&archive_returned_mutex);
	i32 count = archive_returned_count;
	memcpy(returned, archive_returned, count * sizeof(*returned));
	archive_returned_count = 0;
	platform_mutex_unlock(&archive_returned_mutex);
	for (i32 i = 0; i < count; ++i)
		archive_add_idle_connection(idle, idle_count, returned[i]);
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

static bool archive_send_all_impl(archive_connection_t* socket, const char* data, size_t size) {
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

static bool archive_send_all(archive_connection_t* socket, const char* data, size_t size) {
	i64 start = archive_profile_requests ? get_clock() : 0;
	bool success = archive_send_all_impl(socket, data, size);
	if (archive_profile_requests)
		archive_profile_send_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
	if (!success || (socket->sent_headers && size > socket->body_remaining)) {
		socket->send_failed = true;
	} else if (socket->sent_headers) {
		socket->body_remaining -= size;
	}
	return success;
}

static bool archive_send_headers(archive_connection_t* socket, i32 status, const char* reason,
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
	memrw_write_literal("\r\nCache-Control: no-store\r\nConnection: ", &builder);
	if (socket->keep_alive) memrw_write_literal("keep-alive\r\n\r\n", &builder);
	else memrw_write_literal("close\r\n\r\n", &builder);
	socket->sent_headers = false;
	if (!archive_send_all(socket, header, builder.used_size)) return false;
	socket->sent_headers = true;
	socket->body_remaining = body_size;
	return true;
}

static bool archive_send_body(archive_connection_t* socket, i32 status, const char* reason,
		const char* content_type, const char* body, size_t body_size) {
	return archive_send_headers(socket, status, reason, content_type, body_size) &&
		archive_send_all(socket, body, body_size);
}

static bool archive_send_json(archive_connection_t* socket, i32 status, const char* reason,
		const char* json) {
	return archive_send_body(socket, status, reason, "application/json", json, strlen(json));
}

static bool archive_read_request_headers_impl(archive_connection_t* socket, char* request, size_t capacity) {
	size_t used = 0;
	while (used + 1 < capacity) {
		if (!archive_set_client_timeout(socket)) return false;
#if defined(_WIN32)
		int available = recv(socket->socket, request + used, (int)(capacity - used - 1), MSG_PEEK);
#else
		ssize_t available = recv(socket->socket, request + used, capacity - used - 1, MSG_PEEK);
#endif
		if (available <= 0) return false;
		request[used + (size_t)available] = '\0';
		char* end = strstr(request, "\r\n\r\n");
		size_t count = end ? (size_t)(end + 4 - request) - used : (size_t)available;
		size_t consumed = 0;
		while (consumed < count) {
			if (!archive_set_client_timeout(socket)) return false;
#if defined(_WIN32)
			int received = recv(socket->socket, request + used + consumed, (int)(count - consumed), 0);
#else
			ssize_t received = recv(socket->socket, request + used + consumed, count - consumed, 0);
#endif
			if (received <= 0) return false;
			consumed += (size_t)received;
		}
		used += count;
		request[used] = '\0';
		if (end) return true;
	}
	return false;
}


static bool archive_read_request_headers(archive_connection_t* socket, char* request, size_t capacity) {
	if (!archive_profile_requests) return archive_read_request_headers_impl(socket, request, capacity);
	i64 start = get_clock();
	bool success = archive_read_request_headers_impl(socket, request, capacity);
	archive_profile_read_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
	return success;
}

static void archive_send_qido_response(archive_connection_t* client, const archive_index_t* index,
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

static bool archive_header_has_token(const char* headers, const char* name, const char* token) {
	size_t name_size = strlen(name);
	size_t token_size = strlen(token);
	for (const char* line = headers; line && *line; ) {
		const char* end = strstr(line, "\r\n");
		if (!end) break;
		if ((size_t)(end - line) > name_size && line[name_size] == ':') {
			size_t i = 0;
			while (i < name_size && archive_ascii_lower(line[i]) == archive_ascii_lower(name[i])) ++i;
			if (i == name_size) {
				const char* value = line + name_size + 1;
				while (value < end) {
					while (value < end && (*value == ' ' || *value == '\t' || *value == ',')) ++value;
					const char* next = value;
					while (next < end && *next != ',') ++next;
					const char* last = next;
					while (last > value && (last[-1] == ' ' || last[-1] == '\t')) --last;
					if ((size_t)(last - value) == token_size) {
						i = 0;
						while (i < token_size && archive_ascii_lower(value[i]) == archive_ascii_lower(token[i])) ++i;
						if (i == token_size) return true;
					}
					value = next < end ? next + 1 : end;
				}
			}
		}
		line = end + 2;
	}
	return false;
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

static void archive_send_wado_response_admitted(archive_connection_t* client,
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

static void archive_send_wado_response(archive_connection_t* client,
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

static void archive_handle_request_with_buffer(archive_connection_t* client,
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
	/* Request bodies are not consumed by this read-only server. Never reuse a
	 * connection if one could be waiting after the header block. */
	client->keep_alive = client->request_count < ARCHIVE_MAX_REQUESTS_PER_CONNECTION &&
	                     !archive_header_has_token(request_headers, "Connection", "close") &&
	                     !archive_find_header(request_headers, "Content-Length") &&
	                     !archive_find_header(request_headers, "Transfer-Encoding");
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

static void archive_handle_request(archive_connection_t* client, const archive_repository_t* repository,
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
	archive_connection_t* connection = task->connection;
	i64 start = archive_profile_requests ? get_clock() : 0;
	archive_profile_send_ms = archive_profile_read_ms = 0;
	archive_profile_route = "invalid";
	connection->keep_alive = false;
	connection->sent_headers = false;
	connection->send_failed = false;
	connection->body_remaining = 0;
	if (archive_set_client_timeout(connection))
		archive_handle_request(connection, task->repository, task->index);
	if (archive_profile_requests) {
		double elapsed = get_seconds_elapsed(start, get_clock()) * 1000.0;
		fprintf(stderr, "archive timing: route=%s queue=%.3f ms receive=%.3f ms work=%.3f ms send=%.3f ms total=%.3f ms\n",
			archive_profile_route,
			get_seconds_elapsed(connection->accepted_at, start) * 1000.0,
			archive_profile_read_ms, MAX(0.0, elapsed - archive_profile_read_ms - archive_profile_send_ms),
			archive_profile_send_ms, get_seconds_elapsed(connection->accepted_at, get_clock()) * 1000.0);
	}
	if (connection->keep_alive && connection->sent_headers && !connection->send_failed &&
	    connection->body_remaining == 0) {
		connection->idle_since = get_clock();
		platform_mutex_lock(&archive_returned_mutex);
		if (archive_returned_count < ARCHIVE_MAX_IDLE_CONNECTIONS) {
			archive_returned[archive_returned_count++] = connection;
			connection = NULL;
		}
		platform_mutex_unlock(&archive_returned_mutex);
	}
	if (connection) {
		archive_socket_close(connection->socket);
		free(connection);
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

	archive_connection_t* idle[ARCHIVE_MAX_IDLE_CONNECTIONS] = {0};
	i32 idle_count = 0;
	size_t handled = 0;
	while (max_requests == 0 || handled < max_requests) {
		archive_collect_returned_connections(idle, &idle_count);
		i64 now = get_clock();
		for (i32 i = idle_count - 1; i >= 0; --i) {
			if (get_seconds_elapsed(idle[i]->idle_since, now) * 1000.0 >=
			    ARCHIVE_SOCKET_IDLE_TIMEOUT_MS) {
				archive_close_connection(idle[i]);
				idle[i] = idle[--idle_count];
			}
		}
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(ipv4_server, &read_fds);
		archive_socket_t max_socket = ipv4_server;
		if (ipv6_server != ARCHIVE_INVALID_SOCKET) {
			FD_SET(ipv6_server, &read_fds);
			max_socket = MAX(max_socket, ipv6_server);
		}
		for (i32 i = 0; i < idle_count; ++i) {
			FD_SET(idle[i]->socket, &read_fds);
			max_socket = MAX(max_socket, idle[i]->socket);
		}
		struct timeval timeout = {0, ARCHIVE_LISTENER_POLL_MS * 1000};
#if defined(_WIN32)
		i32 ready = select(0, &read_fds, NULL, NULL, &timeout);
#else
		i32 ready = select(max_socket + 1, &read_fds, NULL, NULL, &timeout);
		if (ready < 0 && errno == EINTR) continue;
#endif
		if (ready < 0) {
			fprintf(stderr, "Archive listener select failed\n");
			break;
		}
		if (ready == 0) continue;
		/* Reused connections are ready to request work. Waiting for them happens
		 * here, outside the worker pool. */
		for (i32 i = idle_count - 1; i >= 0 && (max_requests == 0 || handled < max_requests); --i) {
			archive_connection_t* connection = idle[i];
			if (!FD_ISSET(connection->socket, &read_fds)) continue;
			idle[i] = idle[--idle_count];
			connection->accepted_at = get_clock();
			++connection->request_count;
			archive_client_task_t task = {connection, repository, &index};
			if (!thread_pool_submit_task(&global_thread_pool, archive_handle_client_task,
			                             &task, sizeof(task))) {
				connection->keep_alive = false;
				archive_send_json(connection, 503, "Service Unavailable", "{\"error\":\"server_busy\"}\n");
				archive_close_connection(connection);
			} else if (thread_pool_get_active_worker_thread_count(&global_thread_pool) == 0) {
				/* On a single-core host the listener is also logical worker 0. */
				thread_pool_do_work(&global_thread_pool);
			}
			++handled;
		}
		archive_socket_t listeners[2] = {ipv4_server, ipv6_server};
		for (i32 i = 0; i < 2; ++i) {
			if (listeners[i] == ARCHIVE_INVALID_SOCKET ||
			    !FD_ISSET(listeners[i], &read_fds)) continue;
			archive_socket_t socket = accept(listeners[i], NULL, NULL);
			if (socket == ARCHIVE_INVALID_SOCKET) continue;
			/* Multipart responses are written in small header/body pieces. Disable
			 * Nagle so a persistent connection does not wait for delayed ACKs. */
			int no_delay = 1;
			setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
			           (const char*)&no_delay, sizeof(no_delay));
			archive_connection_t* connection = calloc(1, sizeof(*connection));
			if (!connection) {
				archive_socket_close(socket);
				continue;
			}
			connection->socket = socket;
			connection->idle_since = get_clock();
			archive_add_idle_connection(idle, &idle_count, connection);
		}
	}
	for (i32 i = 0; i < idle_count; ++i) archive_close_connection(idle[i]);
	archive_socket_close(ipv4_server);
	if (ipv6_server != ARCHIVE_INVALID_SOCKET) archive_socket_close(ipv6_server);
	thread_pool_wait_for_completion(&global_thread_pool);
	idle_count = 0;
	archive_collect_returned_connections(idle, &idle_count);
	for (i32 i = 0; i < idle_count; ++i) archive_close_connection(idle[i]);
	archive_index_destroy(&index);
	archive_instance_provider_shutdown();
	archive_socket_shutdown();
	return max_requests != 0 && handled == max_requests ? 0 : 1;
}
