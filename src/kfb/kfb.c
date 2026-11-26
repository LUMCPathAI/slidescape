/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "kfb.h"
#include "jpeg_decoder.h"
#include "tile_buffer_pool.h"

#define KFB_HEADER_SIZE 92
#define KFB_TILE_RECORD_SIZE 64
#define KFB_TILE_RECORDS_PER_READ 1024
#define KFB_MAX_TILE_SIZE 4096
#define KFB_MAX_JPEG_SIZE MEGABYTES(64)

static u32 kfb_read_u32(const u8* data) {
	return (u32)data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u64 kfb_read_u64(const u8* data) {
	return (u64)kfb_read_u32(data) | ((u64)kfb_read_u32(data + 4) << 32);
}

static float kfb_read_float(const u8* data) {
	u32 bits = kfb_read_u32(data);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

bool kfb_has_signature(const u8* data, size_t size) {
	return size >= 8 && memcmp(data, "\xf1\x01\xee\xee" "KFB\0", 8) == 0;
}

static u32 kfb_level_dimension(u32 base_dimension, i32 level, bool round_up) {
	u64 downsample = UINT64_C(1) << level;
	u64 rounding = round_up ? downsample - 1 : 0;
	return (u32)MAX(1, (base_dimension + rounding) >> level);
}

static i32 kfb_get_level_index(u32 scan_scale, float scale) {
	if (!isfinite(scale) || scale <= 0.0f) {
		return -1;
	}
	i32 exponent = 0;
	double fraction = frexp((double)scan_scale / scale, &exponent);
	return fraction == 0.5 && exponent > 0 ? exponent - 1 : -1;
}

static bool kfb_get_tile_data_offset(const kfb_t* kfb, u64 index_start, u64 index_end,
		u64 relative_offset, u32 length, u64* offset) {
	// Each record stores a signed 64-bit offset from the start of the index.
	if (relative_offset >> 63) {
		u64 distance = ~relative_offset + 1;
		if (distance > index_start) {
			return false;
		}
		*offset = index_start - distance;
	} else {
		if (relative_offset > kfb->file.size - index_start) {
			return false;
		}
		*offset = index_start + relative_offset;
	}
	if (*offset < KFB_HEADER_SIZE || length > kfb->file.size - *offset) {
		return false;
	}
	return (*offset < index_start && length <= index_start - *offset) || *offset >= index_end;
}

static bool kfb_read_index(kfb_t* kfb, u64 index_start, u32 tile_count, u32 scan_scale,
		bool populate_tiles) {
	u8 records[KFB_TILE_RECORD_SIZE * KFB_TILE_RECORDS_PER_READ];
	u64 index_end = index_start + (u64)tile_count * KFB_TILE_RECORD_SIZE;
	for (u32 first = 0; first < tile_count;) {
		u32 count = MIN(tile_count - first, KFB_TILE_RECORDS_PER_READ);
		u64 position = index_start + (u64)first * KFB_TILE_RECORD_SIZE;
		if (source_file_read(&kfb->file, position, records, count * KFB_TILE_RECORD_SIZE) != SOURCE_READ_OK) {
			return false;
		}
		for (u32 i = 0; i < count; ++i) {
			const u8* record = records + i * KFB_TILE_RECORD_SIZE;
			u32 length = kfb_read_u32(record + 32);
			if (length == 0) {
				continue;
			}
			i32 level_index = kfb_get_level_index(scan_scale, kfb_read_float(record + 20));
			if (level_index < 0) {
				return false;
			}
			if (level_index >= kfb->level_count) {
				continue;
			}
			kfb_level_t* level = kfb->levels + level_index;
			u32 x = kfb_read_u32(record + 4);
			u32 y = kfb_read_u32(record + 8);
			u32 width = kfb_read_u32(record + 12);
			u32 height = kfb_read_u32(record + 16);
			u32 max_width = kfb_level_dimension(kfb->width, level_index, true);
			u32 max_height = kfb_level_dimension(kfb->height, level_index, true);
			if (memcmp(record, "\xf1\x04\xee\xee", 4) != 0 ||
			    x >= max_width || y >= max_height || x % kfb->tile_size || y % kfb->tile_size ||
			    !width || width > kfb->tile_size || !height || height > kfb->tile_size ||
			    length > KFB_MAX_JPEG_SIZE) {
				return false;
			}
			if (!populate_tiles) {
				// The stored edges tell us whether the scanner rounded each dimension up or down.
				level->width = MAX(level->width, MIN(max_width, x + width));
				level->height = MAX(level->height, MIN(max_height, y + height));
				continue;
			}
			if (!level->tiles || x >= level->width || y >= level->height) {
				return false;
			}
			u32 index = (y / kfb->tile_size) * level->columns + x / kfb->tile_size;
			kfb_jpeg_t* tile = level->tiles + index;
			u64 offset;
			if (tile->length ||
			    !kfb_get_tile_data_offset(kfb, index_start, index_end, kfb_read_u64(record + 36), length, &offset)) {
				return false;
			}
			*tile = (kfb_jpeg_t){offset, length, (u16)width, (u16)height};
		}
		first += count;
	}
	return true;
}

static bool kfb_allocate_tile_grids(kfb_t* kfb) {
	i32 last_level = -1;
	for (i32 i = 0; i < kfb->level_count; ++i) {
		kfb_level_t* level = kfb->levels + i;
		if (!level->width) {
			continue;
		}
		level->width = MAX(level->width, kfb_level_dimension(kfb->width, i, false));
		level->height = MAX(level->height, kfb_level_dimension(kfb->height, i, false));
		level->columns = (level->width + kfb->tile_size - 1) / kfb->tile_size;
		level->rows = (level->height + kfb->tile_size - 1) / kfb->tile_size;
		u64 count = (u64)level->columns * level->rows;
		if (count > INT32_MAX || count > SIZE_MAX / sizeof(*level->tiles)) {
			return false;
		}
		level->tiles = calloc((size_t)count, sizeof(*level->tiles));
		if (!level->tiles) {
			return false;
		}
		last_level = i;
		if (count == 1) {
			break;
		}
	}
	kfb->level_count = last_level + 1;
	return kfb->levels[0].tiles != NULL;
}

static void kfb_read_associated_image(const kfb_t* kfb, u64 section_offset, u8 section_type, kfb_jpeg_t* image) {
	u8 section[28];
	if (section_offset < KFB_HEADER_SIZE ||
	    source_file_read(&kfb->file, section_offset, section, sizeof(section)) != SOURCE_READ_OK ||
	    section[0] != 0xf1 || section[1] != section_type || section[2] != 0xee || section[3] != 0xee) {
		return;
	}
	u32 height = kfb_read_u32(section + 8);
	u32 width = kfb_read_u32(section + 12);
	u32 length = kfb_read_u32(section + 20);
	u32 relative_offset = kfb_read_u32(section + 24);
	if (relative_offset < sizeof(section) || relative_offset > kfb->file.size - section_offset ||
	    !width || width > UINT16_MAX || !height || height > UINT16_MAX ||
	    (u64)width * height > KFB_MAX_JPEG_SIZE / 4 || !length || length > KFB_MAX_JPEG_SIZE) {
		return;
	}
	u64 offset = section_offset + relative_offset;
	if (length <= kfb->file.size - offset) {
		*image = (kfb_jpeg_t){offset, length, (u16)width, (u16)height};
	}
}

bool kfb_open(kfb_t* kfb, const char* filename, i32 max_levels) {
	if (max_levels <= 0 || max_levels > 32 || !source_blob_open_local(&kfb->blob, filename)) {
		return false;
	}
	u8 header[KFB_HEADER_SIZE];
	if (!source_file_view(&kfb->blob, 0, kfb->blob.size, &kfb->file) ||
	    source_file_read(&kfb->file, 0, header, sizeof(header)) != SOURCE_READ_OK ||
	    !kfb_has_signature(header, sizeof(header)) || memcmp(header + 32, "JPEG", 4) != 0) {
		goto failed;
	}
	u32 tile_count = kfb_read_u32(header + 16);
	kfb->height = kfb_read_u32(header + 20);
	kfb->width = kfb_read_u32(header + 24);
	u32 scan_scale = kfb_read_u32(header + 28);
	u64 index_start = kfb_read_u64(header + 68);
	kfb->mpp = kfb_read_float(header + 76);
	kfb->tile_size = kfb_read_u32(header + 88);
	if (!kfb->width || kfb->width > INT32_MAX || !kfb->height || kfb->height > INT32_MAX ||
	    !kfb->tile_size || kfb->tile_size > KFB_MAX_TILE_SIZE || !scan_scale || scan_scale > INT32_MAX ||
	    !tile_count || index_start < KFB_HEADER_SIZE || index_start > kfb->file.size ||
	    tile_count > (kfb->file.size - index_start) / KFB_TILE_RECORD_SIZE) {
		goto failed;
	}
	kfb->level_count = max_levels;
	kfb->levels = calloc((size_t)max_levels, sizeof(*kfb->levels));
	if (!kfb->levels || !kfb_read_index(kfb, index_start, tile_count, scan_scale, false) ||
	    !kfb_allocate_tile_grids(kfb) || !kfb_read_index(kfb, index_start, tile_count, scan_scale, true)) {
		goto failed;
	}
	kfb_read_associated_image(kfb, kfb_read_u32(header + 52), 2, &kfb->macro_image);
	kfb_read_associated_image(kfb, kfb_read_u32(header + 56), 3, &kfb->label_image);
	kfb_read_associated_image(kfb, kfb_read_u64(header + 60), 2, &kfb->thumbnail_image);
	return true;

failed:
	kfb_destroy(kfb);
	return false;
}

void kfb_destroy(kfb_t* kfb) {
	if (kfb->levels) {
		for (i32 i = 0; i < kfb->level_count; ++i) {
			free(kfb->levels[i].tiles);
		}
		free(kfb->levels);
	}
	source_blob_close(&kfb->blob);
	memset(kfb, 0, sizeof(*kfb));
}

static u8* kfb_decode_jpeg(const kfb_t* kfb, const kfb_jpeg_t* image) {
	if (!image->length) {
		return NULL;
	}
	u8* compressed = malloc(image->length);
	if (!compressed) {
		return NULL;
	}
	u8* pixels = NULL;
	if (source_file_read(&kfb->file, image->offset, compressed, image->length) == SOURCE_READ_OK) {
		size_t size = (size_t)image->width * image->height * 4;
		pixels = jpeg_decode_image_bounded(compressed, image->length, image->width, image->height,
		                                   size, NULL, NULL, NULL);
	}
	free(compressed);
	return pixels;
}

u8* kfb_decode_tile_to_bgra(const kfb_t* kfb, i32 level_index, u32 tile_index) {
	if (level_index < 0 || level_index >= kfb->level_count) {
		return NULL;
	}
	const kfb_level_t* level = kfb->levels + level_index;
	if (!level->tiles || tile_index >= level->columns * level->rows) {
		return NULL;
	}
	const kfb_jpeg_t* tile = level->tiles + tile_index;
	u8* pixels = kfb_decode_jpeg(kfb, tile);
	if (!pixels) {
		return NULL;
	}
	u32 tile_size = kfb->tile_size;
	u32 x = (tile_index % level->columns) * tile_size;
	u32 y = (tile_index / level->columns) * tile_size;
	u32 width = MIN(tile->width, level->width - x);
	u32 height = MIN(tile->height, level->height - y);
	if (width == tile_size && height == tile_size) {
		return pixels;
	}
	size_t stride = (size_t)tile_size * 4;
	size_t source_stride = (size_t)tile->width * 4;
	u8* padded = tile_buffer_acquire(stride * tile_size);
	if (padded) {
		memset(padded, 0xff, stride * tile_size);
		for (u32 row = 0; row < height; ++row) {
			memcpy(padded + row * stride, pixels + row * source_stride, (size_t)width * 4);
		}
	}
	tile_buffer_release(pixels, source_stride * tile->height);
	return padded;
}

u8* kfb_decode_associated_image_to_rgba(const kfb_t* kfb, const kfb_jpeg_t* image) {
	u8* pixels = kfb_decode_jpeg(kfb, image);
	if (pixels) {
		size_t pixel_count = (size_t)image->width * image->height;
		for (size_t i = 0; i < pixel_count; ++i) {
			u8* pixel = pixels + i * 4;
			u8 blue = pixel[0];
			pixel[0] = pixel[2];
			pixel[2] = blue;
		}
	}
	return pixels;
}
