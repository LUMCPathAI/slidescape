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

#if !defined(_WIN32)
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
