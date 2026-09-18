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

#ifndef ARCHIVE_SHA256_USE_COMMON_CRYPTO
#if defined(__APPLE__)
#define ARCHIVE_SHA256_USE_COMMON_CRYPTO 1
#else
#define ARCHIVE_SHA256_USE_COMMON_CRYPTO 0
#endif
#endif

#if ARCHIVE_SHA256_USE_COMMON_CRYPTO
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX archive_sha256_backend_t;
#else
#include "sha256.h"
typedef slidescape_sha256_context_t archive_sha256_backend_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct archive_sha256_t {
	u64 byte_count;
	archive_sha256_backend_t backend;
} archive_sha256_t;

void archive_sha256_initialize(archive_sha256_t* sha);
bool archive_sha256_update(archive_sha256_t* sha, const void* data, size_t size);
void archive_sha256_finish(archive_sha256_t* sha, u8 digest[32]);
void archive_sha256_hex(const u8 digest[32], char hex[65]);
bool archive_sha256_bytes(const void* data, size_t size, char hex[65]);

/* Hash an entire local file; outputs are cleared on failure. */
bool archive_sha256_file(const char* path, char digest[65], u64* byte_size);

#ifdef __cplusplus
}
#endif
