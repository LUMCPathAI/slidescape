/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "source_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kfb_jpeg_t {
	u64 offset;
	u32 length;
	u16 width;
	u16 height;
} kfb_jpeg_t;

typedef struct kfb_level_t {
	u32 width;
	u32 height;
	u32 columns;
	u32 rows;
	kfb_jpeg_t* tiles; // Row-major. A zero length denotes an absent tile.
} kfb_level_t;

typedef struct kfb_t {
	source_blob_t blob;
	source_file_t file; // Borrows blob. Do not move an opened reader.
	kfb_level_t* levels;
	i32 level_count;
	u32 width;
	u32 height;
	u32 tile_size;
	float mpp;
	kfb_jpeg_t macro_image;
	kfb_jpeg_t label_image;
	kfb_jpeg_t thumbnail_image;
} kfb_t;

bool kfb_has_signature(const u8* data, size_t size);
// Open into zero-initialized storage. A failed open releases all acquired resources.
bool kfb_open(kfb_t* kfb, const char* filename, i32 max_levels);
void kfb_destroy(kfb_t* kfb);
// Returns a full tile, padded white at slide edges. Release with tile_buffer_release().
u8* kfb_decode_tile_to_bgra(const kfb_t* kfb, i32 level_index, u32 tile_index);
// Returns malloc-owned RGBA pixels.
u8* kfb_decode_associated_image_to_rgba(const kfb_t* kfb, const kfb_jpeg_t* image);

#ifdef __cplusplus
}
#endif
