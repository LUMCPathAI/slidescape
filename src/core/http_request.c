/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "http_request.h"

static bool http_header_value_is_safe(const char* value) {
	if (!value) return true;
	for (const u8* p = (const u8*)value; *p; ++p) {
		if (*p == '\r' || *p == '\n' || *p == 0x7f) return false;
	}
	return true;
}

static bool http_request_target_is_safe(const char* value) {
	if (!value || !value[0]) return false;
	for (const u8* p = (const u8*)value; *p; ++p) {
		if (*p <= 0x20 || *p == 0x7f) return false;
	}
	return true;
}

static bool http_extra_headers_are_safe(const char* headers) {
	if (!headers) return true;
	if (strstr(headers, "\r\n\r\n")) return false;
	for (const u8* p = (const u8*)headers; *p; ++p) {
		if (*p == 0x7f || (*p < 0x20 && *p != '\r' && *p != '\n' && *p != '\t')) {
			return false;
		}
		if (*p == '\r' && p[1] != '\n') return false;
		if (*p == '\n' && (p == (const u8*)headers || p[-1] != '\r')) return false;
	}
	return true;
}

i32 remote_build_http_request_headers(char* request, size_t request_size,
		remote_http_method_t method, const char* host, const char* path,
		const char* accept, const char* content_type, size_t content_length,
		const char* extra_headers, bool close_after_request) {
	const char* method_name = NULL;
	switch (method) {
		case REMOTE_HTTP_GET: method_name = "GET"; break;
		case REMOTE_HTTP_POST: method_name = "POST"; break;
		case REMOTE_HTTP_DELETE: method_name = "DELETE"; break;
		default: return -1;
	}
	if (!request || request_size == 0 || !host || !host[0] || !path || path[0] != '/') {
		return -1;
	}
	if (!http_request_target_is_safe(host) || !http_request_target_is_safe(path) ||
	    !http_header_value_is_safe(accept) || !http_header_value_is_safe(content_type) ||
	    !http_extra_headers_are_safe(extra_headers)) {
		return -1;
	}

	i32 length = snprintf(request, request_size,
	                      "%s %s HTTP/1.1\r\n"
	                      "Accept: %s\r\n"
	                      "%s"
	                      "%s%s%s"
	                      "Content-Length: %llu\r\n"
	                      "Connection: %s\r\n"
	                      "Host: %s\r\n\r\n",
	                      method_name, path, accept ? accept : "*/*",
	                      extra_headers ? extra_headers : "",
	                      content_type ? "Content-Type: " : "",
	                      content_type ? content_type : "",
	                      content_type ? "\r\n" : "",
	                      (unsigned long long)content_length,
	                      close_after_request ? "close" : "keep-alive", host);
	return length;
}
