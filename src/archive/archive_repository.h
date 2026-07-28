/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef SLIDESCAPE_ARCHIVE_REPOSITORY_H
#define SLIDESCAPE_ARCHIVE_REPOSITORY_H

#include "common.h"

#define ARCHIVE_REPOSITORY_SCHEMA_VERSION 2
#define ARCHIVE_PATH_CAPACITY 4096

#ifdef __cplusplus
extern "C" {
#endif

typedef enum archive_repository_status_t {
	ARCHIVE_REPOSITORY_OK = 0,
	ARCHIVE_REPOSITORY_INVALID_ARGUMENT,
	ARCHIVE_REPOSITORY_PATH_TOO_LONG,
	ARCHIVE_REPOSITORY_IO_ERROR,
	ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA,
	ARCHIVE_REPOSITORY_INVALID_LAYOUT,
} archive_repository_status_t;

typedef struct archive_repository_t {
	char root[ARCHIVE_PATH_CAPACITY];
} archive_repository_t;

archive_repository_status_t archive_repository_initialize(archive_repository_t* repository,
		const char* root, char* error, size_t error_capacity);
archive_repository_status_t archive_repository_open(archive_repository_t* repository,
		const char* root, char* error, size_t error_capacity);
const char* archive_repository_status_string(archive_repository_status_t status);
bool archive_repository_make_path(const archive_repository_t* repository, const char* relative_path,
		char* dest, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
