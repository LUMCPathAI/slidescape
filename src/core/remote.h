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

typedef struct http_response_t {
    memrw_t buffer;
    size_t content_length;
    i32 status_code;
    char content_type[256];
} http_response_t;

typedef struct tls_connection_t tls_connection_t;

#ifdef __cplusplus
extern "C" {
#endif


// prototypes
void init_networking();
u8 *do_http_request(const char *hostname, i32 portno, const char *uri, i32 *bytes_read, i32 thread_id);
u8 *download_remote_chunk(const char *hostname, i32 portno, const char *filename, i64 chunk_offset, i64 chunk_size,
                          i32 *bytes_read, i32 thread_id);
u8 *download_remote_batch(const char *hostname, i32 portno, const char *filename, i64 *chunk_offsets, i64 *chunk_sizes,
                          i32 batch_size, i32 *bytes_read, i32 thread_id);
bool open_remote_slide(app_state_t *app_state, const char *hostname, i32 portno, const char *filename);
http_response_t* open_remote_uri(app_state_t *app_state, const char *uri, const char* api_token);
http_response_t* open_remote_uri_with_extra_headers(const char *uri, const char* api_token, const char* cookie_header);
tls_connection_t* remote_connection_open(const char* hostname, i32 portno);
tls_connection_t* remote_connection_open_http(const char* hostname, i32 portno, bool use_tls);
void remote_connection_close_and_free(tls_connection_t* connection);
http_response_t* remote_connection_request(tls_connection_t* connection, const char* host, const char* path, const char* api_token, const char* cookie_header, bool close_after_request);
http_response_t* remote_connection_request_with_headers(tls_connection_t* connection, const char* host,
                                                        const char* path, const char* accept,
                                                        const char* extra_headers,
                                                        bool close_after_request);
void http_response_destroy(http_response_t* response);

#if DO_DEBUG
void do_remote_connection_test();
#endif

// globals
#if defined(TLSCLIENT_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif



#undef INIT
#undef extern


#ifdef __cplusplus
}
#endif
