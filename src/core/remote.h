/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once
#include "common.h"
#include "viewer.h"
#include "http_response_parser.h"
#include "http_request.h"

typedef struct tls_connection_t tls_connection_t;

typedef struct remote_request_options_t {
	u32 read_poll_timeout_ms;
	float write_timeout_seconds;
	float idle_read_timeout_seconds;
	float total_timeout_seconds;
	size_t max_response_body_size;
	volatile i32* cancellation_generation;
	i32 expected_generation;
	http_response_error_t* out_error;
} remote_request_options_t;

#ifdef __cplusplus
extern "C" {
#endif


// prototypes
void init_networking();
http_response_t* open_remote_uri(app_state_t *app_state, const char *uri, const char* api_token);
http_response_t* open_remote_uri_with_extra_headers(const char *uri, const char* api_token, const char* cookie_header);
http_response_t* open_remote_uri_with_extra_headers_and_options(
	const char* uri, const char* api_token, const char* cookie_header,
	const remote_request_options_t* options);
tls_connection_t* remote_connection_open(const char* hostname, i32 portno);
tls_connection_t* remote_connection_open_http(const char* hostname, i32 portno, bool use_tls);
void remote_connection_close_and_free(tls_connection_t* connection);
http_response_t* remote_connection_request(tls_connection_t* connection, const char* host, const char* path, const char* api_token, const char* cookie_header, bool close_after_request);
http_response_t* remote_connection_request_with_headers(tls_connection_t* connection, const char* host,
                                                        const char* path, const char* accept,
                                                        const char* extra_headers,
                                                        bool close_after_request);
http_response_t* remote_connection_request_with_headers_and_options(
	tls_connection_t* connection, const char* host, const char* path, const char* accept,
	const char* extra_headers, bool close_after_request,
	const remote_request_options_t* options);
http_response_t* remote_connection_request_http(
	tls_connection_t* connection, const char* host, remote_http_method_t method,
	const char* path, const char* accept, const char* content_type,
	const void* body, size_t body_size, const char* extra_headers,
	bool close_after_request, const remote_request_options_t* options);
void http_response_destroy(http_response_t* response);

#if DO_DEBUG
void do_remote_connection_test();
#endif

#ifdef __cplusplus
}
#endif
