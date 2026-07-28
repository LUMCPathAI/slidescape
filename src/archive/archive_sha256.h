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

typedef struct archive_sha256_t {
	u32 state[8];
	u64 byte_count;
	u8 buffer[64];
	size_t buffer_size;
} archive_sha256_t;

void archive_sha256_initialize(archive_sha256_t* sha);
bool archive_sha256_update(archive_sha256_t* sha, const void* data, size_t size);
void archive_sha256_finish(archive_sha256_t* sha, u8 digest[32]);
void archive_sha256_hex(const u8 digest[32], char hex[65]);
bool archive_sha256_bytes(const void* data, size_t size, char hex[65]);

#ifdef __cplusplus
}
#endif
