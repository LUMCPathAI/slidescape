/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "remote_url.h"

#include <ctype.h>

static bool copy_url_component(char* destination, size_t destination_size, const char* start, size_t length) {
	if (!destination || destination_size == 0 || !start || length >= destination_size) return false;
	memcpy(destination, start, length);
	destination[length] = '\0';
	return true;
}

static bool url_component_has_unsafe_http_chars(const char* start, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		unsigned char c = (unsigned char)start[i];
		if (c <= 0x20 || c == 0x7f) return true;
	}
	return false;
}

bool remote_url_protocol_equals(const char* protocol, const char* expected) {
	if (!protocol || !expected) return false;
	while (*protocol && *expected) {
		if (tolower((unsigned char)*protocol++) != tolower((unsigned char)*expected++)) return false;
	}
	return *protocol == '\0' && *expected == '\0';
}

bool remote_url_parse(remote_url_t* result, const char* url) {
	if (!result || !url || !url[0]) return false;
	memset(result, 0, sizeof(*result));

	const char* scheme_end = strstr(url, "://");
	if (!scheme_end || url_component_has_unsafe_http_chars(url, (size_t)(scheme_end - url)) ||
	    !copy_url_component(result->protocol, sizeof(result->protocol), url, (size_t)(scheme_end - url))) {
		return false;
	}
	if (!remote_url_protocol_equals(result->protocol, "http") &&
	    !remote_url_protocol_equals(result->protocol, "https")) {
		return false;
	}

	const char* authority_start = scheme_end + 3;
	const char* authority_end = strpbrk(authority_start, "/?#");
	if (!authority_end) authority_end = url + strlen(url);
	size_t authority_length = (size_t)(authority_end - authority_start);
	if (authority_length == 0 || url_component_has_unsafe_http_chars(authority_start, authority_length) ||
	    memchr(authority_start, '@', authority_length) != NULL ||
	    !copy_url_component(result->host_header, sizeof(result->host_header), authority_start, authority_length)) {
		return false;
	}

	const char* host_start = authority_start;
	const char* host_end = authority_end;
	const char* port_start = NULL;
	if (*host_start == '[') {
		const char* closing_bracket = strchr(host_start + 1, ']');
		if (!closing_bracket || closing_bracket >= authority_end) return false;
		host_start++;
		host_end = closing_bracket;
		if (closing_bracket + 1 < authority_end) {
			if (closing_bracket[1] != ':') return false;
			port_start = closing_bracket + 2;
		}
	} else {
		const char* first_colon = (const char*)memchr(host_start, ':', authority_length);
		if (first_colon) {
			const char* last_colon = first_colon;
			for (const char* cursor = first_colon + 1; cursor < authority_end; ++cursor) {
				if (*cursor == ':') last_colon = cursor;
			}
			if (first_colon != last_colon) return false; // IPv6 literals must use brackets.
			host_end = first_colon;
			port_start = first_colon + 1;
		}
	}

	if (host_end == host_start ||
	    !copy_url_component(result->site, sizeof(result->site), host_start, (size_t)(host_end - host_start))) {
		return false;
	}

	if (port_start) {
		size_t port_length = (size_t)(authority_end - port_start);
		if (port_length == 0 || port_length >= sizeof(result->port)) return false;
		for (const char* cursor = port_start; cursor < authority_end; ++cursor) {
			if (!isdigit((unsigned char)*cursor)) return false;
		}
		if (!copy_url_component(result->port, sizeof(result->port), port_start, port_length)) return false;
		long port_number = strtol(result->port, NULL, 10);
		if (port_number <= 0 || port_number > 65535) return false;
	} else {
		const char* default_port = remote_url_protocol_equals(result->protocol, "https") ? "443" : "80";
		if (!copy_url_component(result->port, sizeof(result->port), default_port, strlen(default_port))) return false;
	}

	if (*authority_end == '\0' || *authority_end == '#') {
		return copy_url_component(result->path, sizeof(result->path), "/", 1);
	}

	const char* fragment = strchr(authority_end, '#');
	const char* path_end = fragment ? fragment : url + strlen(url);
	size_t path_length = (size_t)(path_end - authority_end);
	if (url_component_has_unsafe_http_chars(authority_end, path_length)) return false;
	if (*authority_end == '?') {
		if (path_length + 1 >= sizeof(result->path)) return false;
		result->path[0] = '/';
		return copy_url_component(result->path + 1, sizeof(result->path) - 1, authority_end, path_length);
	}
	return copy_url_component(result->path, sizeof(result->path), authority_end, path_length);
}
