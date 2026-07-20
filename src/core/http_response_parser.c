/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "http_response_parser.h"

#include "stringutils.h"

#define HTTP_RESPONSE_DEFAULT_MAX_BODY_SIZE MEGABYTES(64)
#define HTTP_RESPONSE_MAX_HEADER_SIZE KILOBYTES(64)

static bool add_header_bytes(http_response_parser_t* context, size_t size) {
	if (context->header_bytes > HTTP_RESPONSE_MAX_HEADER_SIZE ||
	    size > HTTP_RESPONSE_MAX_HEADER_SIZE - context->header_bytes) {
		context->error = HTTP_RESPONSE_ERROR_HEADER_TOO_LARGE;
		return false;
	}
	context->header_bytes += size;
	return true;
}

static void commit_header(http_response_parser_t* context) {
	context->header_name[context->header_name_length] = '\0';
	context->header_value[context->header_value_length] = '\0';
	if (!context->header_name_overflow &&
	    strcasecmp(context->header_name, "Content-Type") == 0) {
		copy_cstring(context->response->content_type, context->header_value,
		             sizeof(context->response->content_type));
	}
	context->header_name_length = 0;
	context->header_value_length = 0;
	context->header_name_overflow = false;
	context->last_was_header_value = false;
}

static i32 on_header_field(http_parser* parser, const char* data, size_t size) {
	http_response_parser_t* context = (http_response_parser_t*)parser->data;
	if (!add_header_bytes(context, size)) return 1;
	if (context->last_was_header_value) commit_header(context);
	size_t available = sizeof(context->header_name) - 1 - context->header_name_length;
	if (size > available) {
		context->header_name_overflow = true;
		size = available;
	}
	memcpy(context->header_name + context->header_name_length, data, size);
	context->header_name_length += size;
	return 0;
}

static i32 on_header_value(http_parser* parser, const char* data, size_t size) {
	http_response_parser_t* context = (http_response_parser_t*)parser->data;
	if (!add_header_bytes(context, size)) return 1;
	context->header_name[context->header_name_length] = '\0';
	if (!context->header_name_overflow &&
	    strcasecmp(context->header_name, "Content-Type") == 0) {
		size_t available = sizeof(context->header_value) - 1 - context->header_value_length;
		if (size > available) {
			context->error = HTTP_RESPONSE_ERROR_HEADER_TOO_LARGE;
			return 1;
		}
		memcpy(context->header_value + context->header_value_length, data, size);
		context->header_value_length += size;
	}
	context->last_was_header_value = true;
	return 0;
}

static i32 on_body(http_parser* parser, const char* data, size_t size) {
	http_response_parser_t* context = (http_response_parser_t*)parser->data;
	if (context->last_was_header_value) commit_header(context);
	if (context->response->content_length > context->max_body_size ||
	    size > context->max_body_size - context->response->content_length) {
		context->error = HTTP_RESPONSE_ERROR_BODY_TOO_LARGE;
		return 1;
	}
	memrw_push_back(&context->response->buffer, (void*)data, size);
	context->response->content_length += size;
	return 0;
}

static i32 on_message_complete(http_parser* parser) {
	http_response_parser_t* context = (http_response_parser_t*)parser->data;
	if (context->last_was_header_value) commit_header(context);
	context->message_complete = true;
	return 0;
}

bool http_response_parser_init(http_response_parser_t* context, size_t max_body_size) {
	if (!context) return false;
	memset(context, 0, sizeof(*context));
	context->max_body_size = max_body_size ? max_body_size : HTTP_RESPONSE_DEFAULT_MAX_BODY_SIZE;
	context->response = (http_response_t*)calloc(1, sizeof(*context->response));
	if (!context->response) return false;
	u64 initial_capacity = MIN((u64)MEGABYTES(1), (u64)context->max_body_size + 1);
	context->response->buffer = memrw_create(MAX(initial_capacity, 1));
	http_parser_init(&context->parser, HTTP_RESPONSE);
	context->parser.data = context;
	return true;
}

bool http_response_parser_execute(http_response_parser_t* context, const void* data, size_t size) {
	if (!context || !context->response || context->error != HTTP_RESPONSE_ERROR_NONE ||
	    (!data && size)) return false;
	http_parser_settings settings = {0};
	settings.on_header_field = on_header_field;
	settings.on_header_value = on_header_value;
	settings.on_body = on_body;
	settings.on_message_complete = on_message_complete;
	size_t parsed = http_parser_execute(&context->parser, &settings, (const char*)data, size);
	if (parsed != size) {
		if (context->error == HTTP_RESPONSE_ERROR_NONE) {
			context->error = HTTP_RESPONSE_ERROR_PARSE;
		}
		return false;
	}
	return context->error == HTTP_RESPONSE_ERROR_NONE;
}

void http_response_parser_finish(http_response_parser_t* context) {
	if (!context || context->error != HTTP_RESPONSE_ERROR_NONE) return;
	if (!context->message_complete) context->error = HTTP_RESPONSE_ERROR_TRUNCATED;
}

http_response_t* http_response_parser_take_response(http_response_parser_t* context) {
	if (!context || !context->response || context->error != HTTP_RESPONSE_ERROR_NONE ||
	    !context->message_complete) return NULL;
	context->response->status_code = context->parser.status_code;
	memrw_putc(0, &context->response->buffer);
	http_response_t* response = context->response;
	context->response = NULL;
	return response;
}

void http_response_parser_destroy(http_response_parser_t* context) {
	if (!context) return;
	if (context->response) {
		memrw_destroy(&context->response->buffer);
		free(context->response);
	}
	memset(context, 0, sizeof(*context));
}

const char* http_response_error_string(http_response_error_t error) {
	switch (error) {
		case HTTP_RESPONSE_ERROR_NONE: return "no error";
		case HTTP_RESPONSE_ERROR_INVALID_REQUEST: return "invalid HTTP request";
		case HTTP_RESPONSE_ERROR_CANCELLED: return "request cancelled";
		case HTTP_RESPONSE_ERROR_TOTAL_TIMEOUT: return "total request deadline exceeded";
		case HTTP_RESPONSE_ERROR_WRITE_TIMEOUT: return "HTTP request write deadline exceeded";
		case HTTP_RESPONSE_ERROR_IDLE_READ_TIMEOUT: return "HTTP response idle-read deadline exceeded";
		case HTTP_RESPONSE_ERROR_CONNECTION: return "connection failed";
		case HTTP_RESPONSE_ERROR_NETWORK_WRITE: return "network write failed";
		case HTTP_RESPONSE_ERROR_NETWORK_READ: return "network read failed";
		case HTTP_RESPONSE_ERROR_PARSE: return "malformed HTTP response";
		case HTTP_RESPONSE_ERROR_HEADER_TOO_LARGE: return "HTTP response header is too large";
		case HTTP_RESPONSE_ERROR_BODY_TOO_LARGE: return "HTTP response body is too large";
		case HTTP_RESPONSE_ERROR_TRUNCATED: return "truncated HTTP response";
	}
	return "unknown HTTP response error";
}
