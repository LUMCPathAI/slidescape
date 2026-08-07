/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum remote_http_method_t {
	REMOTE_HTTP_GET = 0,
	REMOTE_HTTP_POST,
	REMOTE_HTTP_DELETE,
} remote_http_method_t;

i32 remote_build_http_request_headers(char* request, size_t request_size,
		remote_http_method_t method, const char* host, const char* path,
		const char* accept, const char* content_type, size_t content_length,
		const char* extra_headers, bool close_after_request);

#ifdef __cplusplus
}
#endif

