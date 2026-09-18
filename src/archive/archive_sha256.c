/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_sha256.h"

#include <stdio.h>
#include <stdlib.h>

#include <limits.h>
#include <string.h>

static archive_sha256_backend_t* archive_sha256_backend(archive_sha256_t* sha) {
	return &sha->backend;
}

void archive_sha256_initialize(archive_sha256_t* sha) {
	if (!sha) return;
	memset(sha, 0, sizeof(*sha));
#if ARCHIVE_SHA256_USE_COMMON_CRYPTO
	CC_SHA256_Init(archive_sha256_backend(sha));
#else
	slidescape_sha256_init(archive_sha256_backend(sha));
#endif
}

bool archive_sha256_update(archive_sha256_t* sha, const void* data, size_t size) {
	const u8* bytes = data;
	if (!sha || (!bytes && size > 0) || sha->byte_count > UINT64_MAX / 8 ||
	    size > UINT64_MAX / 8 - sha->byte_count) return false;
#if ARCHIVE_SHA256_USE_COMMON_CRYPTO
	size_t remaining = size;
	while (remaining > 0) {
		CC_LONG chunk = remaining > UINT_MAX ? UINT_MAX : (CC_LONG)remaining;
		if (!CC_SHA256_Update(archive_sha256_backend(sha), bytes, chunk))
			return false;
		bytes += chunk;
		remaining -= chunk;
	}
#else
	slidescape_sha256_update(archive_sha256_backend(sha), bytes, size);
#endif
	sha->byte_count += size;
	return true;
}

void archive_sha256_finish(archive_sha256_t* sha, u8 digest[32]) {
	if (!sha || !digest) return;
#if ARCHIVE_SHA256_USE_COMMON_CRYPTO
	CC_SHA256_Final(digest, archive_sha256_backend(sha));
#else
	slidescape_sha256_final(archive_sha256_backend(sha), digest);
#endif
	memset(sha, 0, sizeof(*sha));
}

void archive_sha256_hex(const u8 digest[32], char hex[65]) {
	static const char digits[] = "0123456789abcdef";
	for (i32 i = 0; i < 32; ++i) {
		hex[i * 2] = digits[digest[i] >> 4];
		hex[i * 2 + 1] = digits[digest[i] & 15];
	}
	hex[64] = '\0';
}

bool archive_sha256_bytes(const void* data, size_t size, char hex[65]) {
	if ((!data && size > 0) || !hex) return false;
	archive_sha256_t sha;
	u8 digest[32];
	archive_sha256_initialize(&sha);
	if (!archive_sha256_update(&sha, data, size)) return false;
	archive_sha256_finish(&sha, digest);
	archive_sha256_hex(digest, hex);
	return true;
}

/* A fixed-size heap buffer keeps large-source hashing off worker stacks. */
bool archive_sha256_file(const char* path, char digest[65], u64* byte_size) {
	if (digest) digest[0] = '\0';
	if (byte_size) *byte_size = 0;
	if (!path || !digest || !byte_size) return false;
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	u8* buffer = malloc(MEGABYTES(1));
	archive_sha256_t sha;
	archive_sha256_initialize(&sha);
	bool success = buffer != NULL;
	while (success) {
		size_t count = fread(buffer, 1, MEGABYTES(1), file);
		if (count) success = archive_sha256_update(&sha, buffer, count);
		if (count < MEGABYTES(1)) {
			if (ferror(file)) success = false;
			break;
		}
	}
	free(buffer);
	if (fclose(file) != 0) success = false;
	if (!success) return false;
	u8 bytes[32];
	*byte_size = sha.byte_count;
	archive_sha256_finish(&sha, bytes);
	archive_sha256_hex(bytes, digest);
	return true;
}
