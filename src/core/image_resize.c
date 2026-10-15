/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2025  Pieter Valkema

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


// NOTE: The resampling code is adapted from the Pillow library
// License information below:

/*
The Python Imaging Library (PIL) is

    Copyright © 1997-2011 by Secret Labs AB
    Copyright © 1995-2011 by Fredrik Lundh and contributors

Pillow is the friendly PIL fork. It is

    Copyright © 2010 by Jeffrey A. Clark and contributors

Like PIL, Pillow is licensed under the open source MIT-CMU License:

By obtaining, using, and/or copying this software and/or its associated
documentation, you agree that you have read, understood, and will comply
with the following terms and conditions:

Permission to use, copy, modify and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appears in all copies, and that
both that copyright notice and this permission notice appear in supporting
documentation, and that the name of Secret Labs AB or the author not be
used in advertising or publicity pertaining to distribution of the software
without specific, written prior permission.

SECRET LABS AB AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL SECRET LABS AB OR THE AUTHOR BE LIABLE FOR ANY SPECIAL,
INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
*/

#include "common.h"
#include "image.h"
#include "image_resize.h"

#include <math.h>

image_buffer_t create_bgra_image_buffer(i32 width, i32 height) {
    image_buffer_t result = {};
    result.width = width;
    result.height = height;
    result.channels = 4;
    result.stride_in_pixels = width;
    result.stride_in_bytes = width * 4;
	result.pixel_format = PIXEL_FORMAT_U8_BGRA;
	result.pixels = calloc(1, width * height * 4);
	result.is_valid = true;
	result.owns_pixels = true;
	return result;
}

image_buffer_t create_bgra_image_buffer_using_arena(arena_t* arena, i32 width, i32 height) {
    image_buffer_t result = {};
    result.width = width;
    result.height = height;
    result.channels = 4;
    result.stride_in_pixels = width;
    result.stride_in_bytes = width * 4;
    result.pixel_format = PIXEL_FORMAT_U8_BGRA;
    size_t size = width * height * 4;
	if ((arena->used + size) <= arena->size) {
		result.pixels = arena_push_size(arena, width * height * 4);
		result.is_valid = true;
		result.owns_pixels = false;
	}
	return result;
}

void destroy_image_buffer(image_buffer_t* image_buffer) {
	if (image_buffer->pixels && image_buffer->owns_pixels) {
		free(image_buffer->pixels);
	}
	image_buffer->pixels = NULL;
	image_buffer->is_valid = false;
	image_buffer->owns_pixels = false;
}


static float sinc_filter(float x) {
    if (x == 0.0f) {
        return 1.0f;
    } else {
        x *= PI_F;
        return sinf(x) / x;
    }
}

static float lanczos3_filter(float x) {
    if (x >= -3.0f && x <= 3.0f) {
        return sinc_filter(x) * sinc_filter(x / 3.0f);
    } else {
        return 0.0f;
    }
}


/* 8 bits for result. Filter can have negative areas.
   In one cases the sum of the coefficients will be negative,
   in the other it will be more than 1.0. That is why we need
   two extra bits for overflow and int type. */
#define PRECISION_BITS (32 - 8 - 2)

/* Handles values form -640 to 639. */
u8 _clip8_lookups[1280] = {
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,   2,   3,   4,   5,
        6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,
        23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,
        40,  41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,
        57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,
        74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,
        91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107,
        108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124,
        125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141,
        142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
        159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
        176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192,
        193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
        210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226,
        227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243,
        244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255,
};

u8 *clip8_lookups = &_clip8_lookups[640];

static inline u8 clip8(int in) {
    return clip8_lookups[in >> PRECISION_BITS];
}

static i32 precompute_coeffs(i32 in_size, float in0, float in1, i32 out_size, filter_t *filter, i32 **boundsp, float **kkp) {
    /* prepare for horizontal stretch */
    float filterscale = (in1 - in0) / out_size;
    float scale = filterscale;
    if (filterscale < 1.0) {
        filterscale = 1.0;
    }

    /* determine support size (length of resampling filter) */
    float support = filter->support * filterscale;

    /* maximum number of coeffs */
    i32 ksize = (i32)ceil(support) * 2 + 1;

    // check for overflow
    if (out_size > INT32_MAX / (ksize * (int)sizeof(float))) {
        fatal_error();
        return 0;
    }

    /* coefficient buffer */
    /* malloc check ok, overflow checked above */
    float* kk = malloc(out_size * ksize * sizeof(float));
    if (!kk) {
        fatal_error();
        return 0;
    }

    /* malloc check ok, ksize*sizeof(double) > 2*sizeof(int) */
    i32* bounds = malloc(out_size * 2 * sizeof(i32));
    if (!bounds) {
        free(kk);
        fatal_error();
        return 0;
    }

    for (i32 xx = 0; xx < out_size; xx++) {
        float center = in0 + (xx + 0.5f) * scale;
        float ww = 0.0f;
        float ss = 1.0f / filterscale;
        // Round the value
        i32 xmin = (i32)(center - support + 0.5f);
        if (xmin < 0) {
            xmin = 0;
        }
        // Round the value
        i32 xmax = (i32)(center + support + 0.5f);
        if (xmax > in_size) {
            xmax = in_size;
        }
        xmax -= xmin;
        float* k = &kk[xx * ksize];
        i32 x;
        for (x = 0; x < xmax; x++) {
            float w = filter->filter((x + xmin - center + 0.5f) * ss);
            k[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; x++) {
            if (ww != 0.0) {
                k[x] /= ww;
            }
        }
        // Remaining values should stay empty if they are used despite of xmax.
        for (; x < ksize; x++) {
            k[x] = 0;
        }
        bounds[xx * 2 + 0] = xmin;
        bounds[xx * 2 + 1] = xmax;
    }
    *boundsp = bounds;
    *kkp = kk;
    return ksize;
}

void normalize_coeffs_8bpc(int out_size, int ksize, float *prekk) {
    i32 x;
    i32 *kk;

    // use the same buffer for normalized coefficients
    kk = (i32 *)prekk;

    for (x = 0; x < out_size * ksize; x++) {
        if (prekk[x] < 0) {
            kk[x] = (int)(-0.5 + prekk[x] * (1 << PRECISION_BITS));
        } else {
            kk[x] = (int)(0.5 + prekk[x] * (1 << PRECISION_BITS));
        }
    }
}

void image_resample_horizontal_8bit(image_buffer_t* out, image_buffer_t* in, i32 offset, i32 ksize, i32 *bounds, float *prekk) {
    int ss0, ss1, ss2, ss3;
    int xx, yy, x, xmin, xmax;
    i32 *k, *kk;

    // use the same buffer for normalized coefficients
    kk = (i32 *)prekk;
    normalize_coeffs_8bpc(out->width, ksize, prekk);


    if (in->channels == 1) {

    } else if (in->channels == 2) {
        for (yy = 0; yy < out->height; yy++) {
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                xmin = bounds[xx * 2 + 0];
                xmax = bounds[xx * 2 + 1];
                k = &kk[xx * ksize];
                ss0 = ss3 = 1 << (PRECISION_BITS - 1);
                for (x = 0; x < xmax; x++) {
                    ss0 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 0) * k[x];
                    ss3 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 3) * k[x];
                }
                v = MAKE_UINT32(clip8(ss0), 0, 0, clip8(ss3));
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    } else if (in->channels == 3) {
        for (yy = 0; yy < out->height; yy++) {
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                xmin = bounds[xx * 2 + 0];
                xmax = bounds[xx * 2 + 1];
                k = &kk[xx * ksize];
                ss0 = ss1 = ss2 = 1 << (PRECISION_BITS - 1);
                for (x = 0; x < xmax; x++) {
                    ss0 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 0) * k[x];
                    ss1 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 1) * k[x];
                    ss2 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 2) * k[x];
                }
                v = MAKE_UINT32(clip8(ss0), clip8(ss1), clip8(ss2), 0);
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    } else {
        for (yy = 0; yy < out->height; yy++) {
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                xmin = bounds[xx * 2 + 0];
                xmax = bounds[xx * 2 + 1];
                k = &kk[xx * ksize];
                ss0 = ss1 = ss2 = ss3 = 1 << (PRECISION_BITS - 1);
                for (x = 0; x < xmax; x++) {
                    ss0 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 0) * k[x];
                    ss1 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 1) * k[x];
                    ss2 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 2) * k[x];
                    ss3 += *(in->pixels + ((yy + offset) * in->width + (x + xmin)) * 4 + 3) * k[x];
                }
                v = MAKE_UINT32(clip8(ss0), clip8(ss1), clip8(ss2), clip8(ss3));
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    }

}

void image_resample_vertical_8bit(image_buffer_t* out, image_buffer_t* in, i32 offset, i32 ksize, i32 *bounds, float *prekk) {
    int ss0, ss1, ss2, ss3;
    int xx, yy, y, ymin, ymax;
    i32 *k, *kk;

    // use the same buffer for normalized coefficients
    kk = (i32 *)prekk;
    normalize_coeffs_8bpc(out->height, ksize, prekk);

    if (in->channels == 1) {
        /*for (yy = 0; yy < out->height; yy++) {
            k = &kk[yy * ksize];
            ymin = bounds[yy * 2 + 0];
            ymax = bounds[yy * 2 + 1];
            for (xx = 0; xx < out->width; xx++) {
                ss0 = 1 << (PRECISION_BITS - 1);
                for (y = 0; y < ymax; y++) {
                    ss0 += ((u8)in->pixels[y + ymin][xx]) * k[y];
                }
                out->pixels[yy][xx] = clip8(ss0);
            }
        }*/
    } else if (in->channels == 2) {
        for (yy = 0; yy < out->height; yy++) {
            k = &kk[yy * ksize];
            ymin = bounds[yy * 2 + 0];
            ymax = bounds[yy * 2 + 1];
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                ss0 = ss3 = 1 << (PRECISION_BITS - 1);
                for (y = 0; y < ymax; y++) {
                    ss0 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 0) * k[y];
                    ss3 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 3) * k[y];
                }
                v = MAKE_UINT32(clip8(ss0), 0, 0, clip8(ss3));
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    } else if (in->channels == 3) {
        for (yy = 0; yy < out->height; yy++) {
            k = &kk[yy * ksize];
            ymin = bounds[yy * 2 + 0];
            ymax = bounds[yy * 2 + 1];
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                ss0 = ss1 = ss2 = 1 << (PRECISION_BITS - 1);
                for (y = 0; y < ymax; y++) {
                    ss0 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 0) * k[y];
                    ss1 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 1) * k[y];
                    ss2 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 2) * k[y];
                }
                v = MAKE_UINT32(clip8(ss0), clip8(ss1), clip8(ss2), 0);
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    } else {
        for (yy = 0; yy < out->height; yy++) {
            k = &kk[yy * ksize];
            ymin = bounds[yy * 2 + 0];
            ymax = bounds[yy * 2 + 1];
            for (xx = 0; xx < out->width; xx++) {
                u32 v;
                ss0 = ss1 = ss2 = ss3 = 1 << (PRECISION_BITS - 1);
                for (y = 0; y < ymax; y++) {
                    ss0 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 0) * k[y];
                    ss1 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 1) * k[y];
                    ss2 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 2) * k[y];
                    ss3 += *(in->pixels + ((y + ymin) * in->width + xx) * 4 + 3) * k[y];
                }
                v = MAKE_UINT32(clip8(ss0), clip8(ss1), clip8(ss2), clip8(ss3));
                memcpy(out->pixels + (yy * out->width + xx) * sizeof(v), &v, sizeof(v));
            }
        }
    }

}

bool image_resample_lanczos3(image_buffer_t* in, image_buffer_t* out, rect2f box) {
    // resample a subset of an input image (with offset/width/height specified by the box) into an output image
    if (in->pixel_format != out->pixel_format) {
        return false;
    }

    filter_t filter = {lanczos3_filter, 3.0f};

    if (in->pixel_format == PIXEL_FORMAT_U8_BGRA || in->pixel_format == PIXEL_FORMAT_U8_RGBA) {
        if (in->channels != 4 || out->channels != 4) {
            return false;
        }

        // precompute coefficients

        bool need_horizontal_pass = out->width != in->width || box.x != 0.0f || roundf(box.w) != out->width;
        bool need_vertical_pass = out->height != in->height || box.y != 0.0f || roundf(box.h) != out->height;

        i32 *bounds_horiz;
        i32 *bounds_vert;
        float* kk_horiz;
        float* kk_vert;
        i32 ksize_horiz = precompute_coeffs(in->width, box.x, box.x + box.w, out->width, &filter, &bounds_horiz, &kk_horiz);
        i32 ksize_vert = precompute_coeffs(in->height, box.y, box.y + box.h, out->height, &filter, &bounds_vert, &kk_vert);

        // First used row in the source image
        i32 ybox_first = bounds_vert[0];
        // Last used row in the source image
        i32 ybox_last = bounds_vert[out->height * 2 - 2] + bounds_vert[out->height * 2 - 1];

        /* two-pass resize, horizontal pass */
        image_buffer_t* input_image_for_vertical_pass = in;
        image_buffer_t temp = {};
        if (need_horizontal_pass) {
            // Shift bounds for vertical pass
            for (i32 i = 0; i < out->height; i++) {
                bounds_vert[i * 2] -= ybox_first;
            }

            temp = *out;
            temp.height = ybox_last - ybox_first;
            temp.pixels = calloc(1,temp.width * temp.height * temp.channels);
            if (temp.pixels) {
                image_resample_horizontal_8bit(&temp, in, ybox_first, ksize_horiz, bounds_horiz, kk_horiz);
            }
            free(bounds_horiz);
            free(kk_horiz);
            if (!temp.pixels) {
                free(bounds_vert);
                free(kk_vert);
                return NULL;
            }
            input_image_for_vertical_pass = &temp;
        } else {
            // Free in any case
            free(bounds_horiz);
            free(kk_horiz);
        }

        /* vertical pass */
        if (need_vertical_pass) {
            if (out) {
                /* imIn can be the original image or horizontally resampled one */
                image_resample_vertical_8bit(out, input_image_for_vertical_pass, 0, ksize_vert, bounds_vert, kk_vert);
            }
            free(bounds_vert);
            free(kk_vert);
            if (!out) {
                return false;
            }
        } else {
            // Free in any case
            free(bounds_vert);
            free(kk_vert);
        }

        if (temp.pixels) {
            free(temp.pixels);
        }

        return true;
    } else {
        return false; // unknown or unsupported pixel format
    }
}

bool image_shrink_2x2(image_buffer_t* in, image_buffer_t* out, rect2i box) {
    if (in->pixel_format != out->pixel_format) {
        return false;
    }

    int xscale = 2, yscale = 2;
    int x, y;
    u32 ss0, ss1, ss2, ss3;
    u32 amend = yscale * xscale / 2;
    for (y = 0; y < box.h / yscale; y++) {
        int yy = box.y + y * yscale;
        u8 *line0 = (u8 *)in->pixels + (yy + 0) * in->stride_in_bytes;
        u8 *line1 = (u8 *)in->pixels + (yy + 1) * in->stride_in_bytes;
        if (in->channels == 2) {
            for (x = 0; x < box.w / xscale; x++) {
                int xx = box.x + x * xscale;
                u32 v;
                ss0 = line0[xx * 4 + 0] + line0[xx * 4 + 4] + line1[xx * 4 + 0] + line1[xx * 4 + 4];
                ss3 = line0[xx * 4 + 3] + line0[xx * 4 + 7] + line1[xx * 4 + 3] + line1[xx * 4 + 7];
                v = MAKE_UINT32((ss0 + amend) >> 2, 0, 0, (ss3 + amend) >> 2);
                memcpy(out->pixels + y * out->stride_in_bytes + x * sizeof(v), &v, sizeof(v));
            }
        } else if (in->channels == 3) {
            for (x = 0; x < box.w / xscale; x++) {
                int xx = box.x + x * xscale;
                u32 v;
                ss0 = line0[xx * 4 + 0] + line0[xx * 4 + 4] + line1[xx * 4 + 0] + line1[xx * 4 + 4];
                ss1 = line0[xx * 4 + 1] + line0[xx * 4 + 5] + line1[xx * 4 + 1] + line1[xx * 4 + 5];
                ss2 = line0[xx * 4 + 2] + line0[xx * 4 + 6] + line1[xx * 4 + 2] + line1[xx * 4 + 6];
                v = MAKE_UINT32((ss0 + amend) >> 2, (ss1 + amend) >> 2, (ss2 + amend) >> 2, 0);
                memcpy(out->pixels + y * out->stride_in_bytes + x * sizeof(v), &v, sizeof(v));
            }
        } else {  // bands == 4
            for (x = 0; x < box.w / xscale; x++) {
                int xx = box.x + x * xscale;
                u32 v;
                ss0 = line0[xx * 4 + 0] + line0[xx * 4 + 4] + line1[xx * 4 + 0] + line1[xx * 4 + 4];
                ss1 = line0[xx * 4 + 1] + line0[xx * 4 + 5] + line1[xx * 4 + 1] + line1[xx * 4 + 5];
                ss2 = line0[xx * 4 + 2] + line0[xx * 4 + 6] + line1[xx * 4 + 2] + line1[xx * 4 + 6];
                ss3 = line0[xx * 4 + 3] + line0[xx * 4 + 7] + line1[xx * 4 + 3] + line1[xx * 4 + 7];
                v = MAKE_UINT32(
                        (ss0 + amend) >> 2,
                        (ss1 + amend) >> 2,
                        (ss2 + amend) >> 2,
                        (ss3 + amend) >> 2
                );
                memcpy(out->pixels + y * out->stride_in_bytes + x * sizeof(v), &v, sizeof(v));
            }
        }
    }
    return true;
}

#if 0
#include "stb_image.h"
#include "stb_image_write.h"

void debug_test_resample() {
    i32 x = 0;
    i32 y = 0;
    i32 channels_in_file = 0;
    image_buffer_t buffer = {};
    buffer.pixels = stbi_load("../../../Bodleian.jpg", &x, &y, &channels_in_file, 4);
    if (buffer.pixels) {

        buffer.width = x;
        buffer.height = y;
        buffer.channels = 4;
        buffer.stride_in_bytes = x * 4;
        buffer.stride_in_pixels = x;
        buffer.pixel_format = PIXEL_FORMAT_U8_RGBA;

        image_buffer_t out = {};
        out.width = 256;
        out.height = 256;
        out.channels = 4;
        out.stride_in_bytes = 256 * 4;
        out.stride_in_pixels = 256;
        out.pixel_format = PIXEL_FORMAT_U8_RGBA;
        out.pixels = calloc(1, out.width * out.height * out.channels);

        rect2f box = {500.0f, 500.0f, 512.0f, 512.0f};
        i64 start = get_clock();
        if (image_resample_lanczos3(&buffer, &out, box)) {
            console_print("Debug image resample test: resampling took %g s.\n", get_seconds_elapsed(start, get_clock()));
            stbi_write_png("debug_resample_result.png", out.width, out.height, 4, out.pixels, out.width * out.channels);
        }

    }

}

void debug_test_shrink2x2() {
    i32 x = 0;
    i32 y = 0;
    i32 channels_in_file = 0;
    image_buffer_t buffer = {};
    buffer.pixels = stbi_load("../../../Bodleian.jpg", &x, &y, &channels_in_file, 4);
    if (buffer.pixels) {

        buffer.width = x;
        buffer.height = y;
        buffer.channels = 4;
        buffer.stride_in_bytes = x * 4;
        buffer.stride_in_pixels = x;
        buffer.pixel_format = PIXEL_FORMAT_U8_RGBA;

        image_buffer_t out = {};
        out.width = 256;
        out.height = 256;
        out.channels = 4;
        out.stride_in_bytes = 256 * 4;
        out.stride_in_pixels = 256;
        out.pixel_format = PIXEL_FORMAT_U8_RGBA;
        out.pixels = calloc(1, out.width * out.height * out.channels);

        rect2i box = {500, 500, 512, 512};
        i64 start = get_clock();
        if (image_shrink_2x2(&buffer, &out, box)) {
            console_print("Debug image shrink test: 2x2 shrink took %g s.\n", get_seconds_elapsed(start, get_clock()));
            stbi_write_png("debug_shrink_result.png", out.width, out.height, 4, out.pixels, out.width * out.channels);
        }

    }

}

#endif
