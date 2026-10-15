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

typedef struct remote_url_t {
	char protocol[16];
	char site[256];
	char host_header[320];
	char port[6];
	char path[2048];
} remote_url_t;

#ifdef __cplusplus
extern "C" {
#endif

bool remote_url_parse(remote_url_t* result, const char* url);
bool remote_url_protocol_equals(const char* protocol, const char* expected);

#ifdef __cplusplus
}
#endif
