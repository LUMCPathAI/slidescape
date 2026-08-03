/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

static const char* slidescape_test_temporary_root(void) {
	const char* configured = getenv("SLIDESCAPE_TEST_TMPDIR");
	if (configured && configured[0]) return configured;
	configured = getenv("TEMP");
	if (configured && configured[0]) return configured;
	configured = getenv("TMP");
	return configured && configured[0] ? configured : ".";
}

static bool slidescape_test_make_temp_path(
		char* path, size_t capacity, const char* stem) {
	char generated[L_tmpnam] = {};
	if (tmpnam_s(generated, sizeof(generated)) != 0) return false;
	const char* leaf = generated;
	for (const char* at = generated; *at; ++at) {
		if (*at == '/' || *at == '\\') leaf = at + 1;
	}
	int length = snprintf(path, capacity, "%s/%s-%s",
		slidescape_test_temporary_root(), stem, leaf);
	return length > 0 && (size_t)length < capacity;
}

static bool slidescape_test_make_temp_directory(
		char* path, size_t capacity, const char* stem) {
	return slidescape_test_make_temp_path(path, capacity, stem) &&
	       _mkdir(path) == 0;
}

static int slidescape_test_make_temp_file(
		char* path, size_t capacity, const char* stem) {
	if (!slidescape_test_make_temp_path(path, capacity, stem)) return -1;
	return _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
	             _S_IREAD | _S_IWRITE);
}
#else
#include <unistd.h>

static const char* slidescape_test_temporary_root(void) {
	const char* configured = getenv("SLIDESCAPE_TEST_TMPDIR");
	if (configured && configured[0]) return configured;
	if (access("/dev/shm", W_OK) == 0) return "/dev/shm";
	configured = getenv("TMPDIR");
	return configured && configured[0] ? configured : "/tmp";
}

static bool slidescape_test_make_temp_directory(
		char* path, size_t capacity, const char* stem) {
	int length = snprintf(
		path, capacity, "%s/%s-XXXXXX",
		slidescape_test_temporary_root(), stem);
	return length > 0 && (size_t)length < capacity &&
	       mkdtemp(path) != NULL;
}

static int slidescape_test_make_temp_file(
		char* path, size_t capacity, const char* stem) {
	int length = snprintf(
		path, capacity, "%s/%s-XXXXXX",
		slidescape_test_temporary_root(), stem);
	return length > 0 && (size_t)length < capacity ?
	       mkstemp(path) : -1;
}
#endif
