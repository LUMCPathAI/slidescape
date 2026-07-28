/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef SLIDESCAPE_ARCHIVE_ROUTES_H
#define SLIDESCAPE_ARCHIVE_ROUTES_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum archive_http_method_t {
	ARCHIVE_HTTP_METHOD_UNKNOWN = 0,
	ARCHIVE_HTTP_METHOD_GET,
	ARCHIVE_HTTP_METHOD_POST,
} archive_http_method_t;

typedef enum archive_route_kind_t {
	ARCHIVE_ROUTE_UNKNOWN = 0,
	ARCHIVE_ROUTE_HEALTH,
	ARCHIVE_ROUTE_QIDO_STUDIES,
	ARCHIVE_ROUTE_QIDO_SERIES,
	ARCHIVE_ROUTE_QIDO_INSTANCES,
	ARCHIVE_ROUTE_WADO_SERIES_METADATA,
	ARCHIVE_ROUTE_WADO_INSTANCE_METADATA,
	ARCHIVE_ROUTE_WADO_FRAMES,
	ARCHIVE_ROUTE_STOW_STUDIES,
} archive_route_kind_t;

typedef struct archive_route_match_t {
	archive_route_kind_t kind;
	char study_uid[65];
	char series_uid[65];
	char instance_uid[65];
	char frames[768];
} archive_route_match_t;

archive_http_method_t archive_http_method_parse(const char* method);
bool archive_route_match(archive_http_method_t method, const char* target,
		archive_route_match_t* match);
const char* archive_route_kind_string(archive_route_kind_t kind);
bool archive_frame_list_parse(const char* text, u32* frames, i32 capacity, i32* count);

#ifdef __cplusplus
}
#endif

#endif
