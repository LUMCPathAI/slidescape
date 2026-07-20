/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "jpeg2000_decoder.h"

#include <openjpeg.h>

typedef struct jpeg2000_memory_stream_t {
	const u8* data;
	size_t size;
	size_t offset;
} jpeg2000_memory_stream_t;

static OPJ_SIZE_T jpeg2000_stream_read(void* dest, OPJ_SIZE_T bytes, void* user_data) {
	jpeg2000_memory_stream_t* stream = user_data;
	size_t remaining = stream->size - stream->offset;
	size_t read_size = MIN((size_t)bytes, remaining);
	if (read_size == 0) return (OPJ_SIZE_T)-1;
	memcpy(dest, stream->data + stream->offset, read_size);
	stream->offset += read_size;
	return read_size;
}

static OPJ_OFF_T jpeg2000_stream_skip(OPJ_OFF_T bytes, void* user_data) {
	jpeg2000_memory_stream_t* stream = user_data;
	if (bytes < 0 || (u64)bytes > stream->size - stream->offset) return -1;
	stream->offset += (size_t)bytes;
	return bytes;
}

static OPJ_BOOL jpeg2000_stream_seek(OPJ_OFF_T offset, void* user_data) {
	jpeg2000_memory_stream_t* stream = user_data;
	if (offset < 0 || (u64)offset > stream->size) return OPJ_FALSE;
	stream->offset = (size_t)offset;
	return OPJ_TRUE;
}

static u8 jpeg2000_sample_to_u8(const opj_image_comp_t* component, size_t index) {
	i64 sample = component->data[index];
	if (component->sgnd && component->prec > 0 && component->prec < 32) {
		sample += (i64)1 << (component->prec - 1);
	}
	if (sample < 0) sample = 0;
	u64 max_sample = component->prec >= 32 ? 0xffffffffu : (((u64)1 << component->prec) - 1);
	if ((u64)sample > max_sample) sample = (i64)max_sample;
	if (max_sample == 0) return 0;
	return (u8)(((u64)sample * 255 + max_sample / 2) / max_sample);
}

u8* jpeg2000_decode_image_to_bgra(const u8* input, size_t input_size,
		i32* width, i32* height) {
	if (width) *width = 0;
	if (height) *height = 0;
	if (!input || input_size == 0) return NULL;

	jpeg2000_memory_stream_t memory_stream = {input, input_size, 0};
	opj_stream_t* stream = opj_stream_default_create(OPJ_TRUE);
	opj_codec_t* codec = NULL;
	opj_image_t* image = NULL;
	u8* result = NULL;
	if (!stream) goto cleanup;
	opj_stream_set_user_data(stream, &memory_stream, NULL);
	opj_stream_set_user_data_length(stream, input_size);
	opj_stream_set_read_function(stream, jpeg2000_stream_read);
	opj_stream_set_skip_function(stream, jpeg2000_stream_skip);
	opj_stream_set_seek_function(stream, jpeg2000_stream_seek);

	OPJ_CODEC_FORMAT format = input_size >= 12 &&
		input[4] == 'j' && input[5] == 'P' && input[6] == ' ' && input[7] == ' ' ?
		OPJ_CODEC_JP2 : OPJ_CODEC_J2K;
	codec = opj_create_decompress(format);
	if (!codec) goto cleanup;

	opj_dparameters_t parameters;
	opj_set_default_decoder_parameters(&parameters);
	if (!opj_setup_decoder(codec, &parameters) ||
		!opj_read_header(stream, codec, &image) ||
		!opj_decode(codec, stream, image) ||
		!opj_end_decompress(codec, stream)) {
		goto cleanup;
	}

	if (!image || (image->numcomps != 1 && image->numcomps < 3)) goto cleanup;
	opj_image_comp_t* first = image->comps;
	if (!first->data || first->w == 0 || first->h == 0 || first->prec == 0 || first->prec > 32 ||
		first->w > INT32_MAX || first->h > INT32_MAX) goto cleanup;
	for (OPJ_UINT32 component_index = 1; component_index < MIN(image->numcomps, 3); ++component_index) {
		opj_image_comp_t* component = image->comps + component_index;
		if (!component->data || component->w != first->w || component->h != first->h ||
			component->prec == 0 || component->prec > 32) {
			goto cleanup;
		}
	}

	size_t pixel_count = (size_t)first->w * first->h;
	if (pixel_count > SIZE_MAX / 4) goto cleanup;
	result = malloc(pixel_count * 4);
	if (!result) goto cleanup;
	for (size_t i = 0; i < pixel_count; ++i) {
		u8 red;
		u8 green;
		u8 blue;
		if (image->numcomps == 1) {
			red = green = blue = jpeg2000_sample_to_u8(first, i);
		} else {
			red = jpeg2000_sample_to_u8(image->comps + 0, i);
			green = jpeg2000_sample_to_u8(image->comps + 1, i);
			blue = jpeg2000_sample_to_u8(image->comps + 2, i);
		}
		result[i * 4 + 0] = blue;
		result[i * 4 + 1] = green;
		result[i * 4 + 2] = red;
		result[i * 4 + 3] = 255;
	}
	if (width) *width = (i32)first->w;
	if (height) *height = (i32)first->h;

cleanup:
	if (image) opj_image_destroy(image);
	if (codec) opj_destroy_codec(codec);
	if (stream) opj_stream_destroy(stream);
	return result;
}
