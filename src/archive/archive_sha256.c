/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_sha256.h"

#include <string.h>

static u32 archive_sha256_rotr(u32 value, u32 count) {
	return (value >> count) | (value << (32 - count));
}

static u32 archive_sha256_load_be(const u8* value) {
	return ((u32)value[0] << 24) | ((u32)value[1] << 16) |
	       ((u32)value[2] << 8) | (u32)value[3];
}

static void archive_sha256_store_be(u8* dest, u32 value) {
	dest[0] = (u8)(value >> 24);
	dest[1] = (u8)(value >> 16);
	dest[2] = (u8)(value >> 8);
	dest[3] = (u8)value;
}

static void archive_sha256_transform(archive_sha256_t* sha, const u8 block[64]) {
	static const u32 constants[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	u32 words[64];
	for (i32 i = 0; i < 16; ++i) words[i] = archive_sha256_load_be(block + i * 4);
	for (i32 i = 16; i < 64; ++i) {
		u32 s0 = archive_sha256_rotr(words[i - 15], 7) ^
		         archive_sha256_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
		u32 s1 = archive_sha256_rotr(words[i - 2], 17) ^
		         archive_sha256_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}
	u32 a = sha->state[0];
	u32 b = sha->state[1];
	u32 c = sha->state[2];
	u32 d = sha->state[3];
	u32 e = sha->state[4];
	u32 f = sha->state[5];
	u32 g = sha->state[6];
	u32 h = sha->state[7];
	for (i32 i = 0; i < 64; ++i) {
		u32 sum1 = archive_sha256_rotr(e, 6) ^ archive_sha256_rotr(e, 11) ^
		           archive_sha256_rotr(e, 25);
		u32 choice = (e & f) ^ (~e & g);
		u32 temporary1 = h + sum1 + choice + constants[i] + words[i];
		u32 sum0 = archive_sha256_rotr(a, 2) ^ archive_sha256_rotr(a, 13) ^
		           archive_sha256_rotr(a, 22);
		u32 majority = (a & b) ^ (a & c) ^ (b & c);
		u32 temporary2 = sum0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	sha->state[0] += a;
	sha->state[1] += b;
	sha->state[2] += c;
	sha->state[3] += d;
	sha->state[4] += e;
	sha->state[5] += f;
	sha->state[6] += g;
	sha->state[7] += h;
}

void archive_sha256_initialize(archive_sha256_t* sha) {
	memset(sha, 0, sizeof(*sha));
	sha->state[0] = 0x6a09e667;
	sha->state[1] = 0xbb67ae85;
	sha->state[2] = 0x3c6ef372;
	sha->state[3] = 0xa54ff53a;
	sha->state[4] = 0x510e527f;
	sha->state[5] = 0x9b05688c;
	sha->state[6] = 0x1f83d9ab;
	sha->state[7] = 0x5be0cd19;
}

bool archive_sha256_update(archive_sha256_t* sha, const void* data, size_t size) {
	const u8* bytes = data;
	if (!sha || (!bytes && size > 0) || sha->byte_count > UINT64_MAX / 8 ||
	    size > UINT64_MAX / 8 - sha->byte_count) return false;
	sha->byte_count += size;
	while (size > 0) {
		size_t available = sizeof(sha->buffer) - sha->buffer_size;
		size_t copied = size < available ? size : available;
		memcpy(sha->buffer + sha->buffer_size, bytes, copied);
		sha->buffer_size += copied;
		bytes += copied;
		size -= copied;
		if (sha->buffer_size == sizeof(sha->buffer)) {
			archive_sha256_transform(sha, sha->buffer);
			sha->buffer_size = 0;
		}
	}
	return true;
}

void archive_sha256_finish(archive_sha256_t* sha, u8 digest[32]) {
	u64 bit_count = sha->byte_count * 8;
	sha->buffer[sha->buffer_size++] = 0x80;
	if (sha->buffer_size > 56) {
		memset(sha->buffer + sha->buffer_size, 0, sizeof(sha->buffer) - sha->buffer_size);
		archive_sha256_transform(sha, sha->buffer);
		sha->buffer_size = 0;
	}
	memset(sha->buffer + sha->buffer_size, 0, 56 - sha->buffer_size);
	for (i32 i = 0; i < 8; ++i) sha->buffer[63 - i] = (u8)(bit_count >> (i * 8));
	archive_sha256_transform(sha, sha->buffer);
	for (i32 i = 0; i < 8; ++i) archive_sha256_store_be(digest + i * 4, sha->state[i]);
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
