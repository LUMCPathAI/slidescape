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
#include "http_parser.h"
#include "memrw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum http_response_error_t {
	HTTP_RESPONSE_ERROR_NONE = 0,
	HTTP_RESPONSE_ERROR_INVALID_REQUEST,
	HTTP_RESPONSE_ERROR_CANCELLED,
	HTTP_RESPONSE_ERROR_TOTAL_TIMEOUT,
	HTTP_RESPONSE_ERROR_WRITE_TIMEOUT,
	HTTP_RESPONSE_ERROR_IDLE_READ_TIMEOUT,
	HTTP_RESPONSE_ERROR_CONNECTION,
	HTTP_RESPONSE_ERROR_NETWORK_WRITE,
	HTTP_RESPONSE_ERROR_NETWORK_READ,
	HTTP_RESPONSE_ERROR_PARSE,
	HTTP_RESPONSE_ERROR_HEADER_TOO_LARGE,
	HTTP_RESPONSE_ERROR_BODY_TOO_LARGE,
	HTTP_RESPONSE_ERROR_TRUNCATED,
} http_response_error_t;

typedef struct http_response_t {
	memrw_t buffer;
	size_t content_length;
	i32 status_code;
	char content_type[256];
} http_response_t;

typedef struct http_response_parser_t {
	http_parser parser;
	http_response_t* response;
	size_t max_body_size;
	http_response_error_t error;
	bool message_complete;
	bool last_was_header_value;
	bool header_name_overflow;
	char header_name[128];
	size_t header_name_length;
	char header_value[1024];
	size_t header_value_length;
	size_t header_bytes;
} http_response_parser_t;

bool http_response_parser_init(http_response_parser_t* parser, size_t max_body_size);
bool http_response_parser_execute(http_response_parser_t* parser, const void* data, size_t size);
void http_response_parser_finish(http_response_parser_t* parser);
http_response_t* http_response_parser_take_response(http_response_parser_t* parser);
void http_response_parser_destroy(http_response_parser_t* parser);
const char* http_response_error_string(http_response_error_t error);

#ifdef __cplusplus
}
#endif
