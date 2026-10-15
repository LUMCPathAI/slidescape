/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

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

#include "common.h"
#include "mathutils.h"

#include <math.h>
/*
 * MSVC's math.h defines "complex" as the legacy struct _complex.
 * That is not C99 complex arithmetic. Force minfft's portable
 * [real, imaginary] representation, which this file uses.
 */
#ifdef complex
#undef complex
#endif
#include "minfft.h"

#include "phasecorrelate.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"


void copy_make_border(buffer2d_t* src, buffer2d_t* dst, i32 top, i32 bottom, i32 left, i32 right, float fill) {
	i32 dst_h = src->h + top + bottom;
	i32 dst_w = src->w + left + right;
	size_t total_size = dst_h * dst_w * sizeof(real_t);

	if (dst->data) {
		// already allocated memory, need to that sizes match!
		if (!(dst->h == dst_h && dst->w == dst_w)) {
			fatal_error("size mismatch");
		}
//		memset(dst->data, 0, total_size);
	} else {
		dst->h = dst_h;
		dst->w = dst_w;
		dst->data = malloc(total_size);
	}

	for (i32 y = 0; y < dst_h; ++y) {
		float* row = dst->data + y * dst_w;
		for (i32 x = 0; x < dst_w; ++x) {
			row[x] = fill;
		}
	}

	size_t src_row_size = src->w * sizeof(real_t);
	for (i32 y = 0; y < src->h; ++y) {
		memcpy(dst->data + (y + top) * dst_w + left, src->data + y * src->w, src_row_size);
	}
}

void debug_create_magnitude_plot(minfft_cmpl* src, i32 w, i32 h, real_t scale, const char* filename) {

	u8* magnitudes = malloc(w * h * sizeof(u8));

	for (i32 y = 0; y < h; ++y) {
		minfft_cmpl* src_row = src + y * w ;
		u8* dst_row = magnitudes + y * w;
		for (i32 x = 0; x < w; ++x) {
			real_t re = src_row[x][0];
			real_t im = src_row[x][1];
			real_t mag = sqrtf(re * re + im * im);
			mag *= scale;
			dst_row[x] = (u8)ATMOST(255, (mag));
		}
	}

	stbi_write_png(filename, w, h, 1, magnitudes, w);
	free(magnitudes);
}


void debug_create_luminance_png(real_t* src, i32 w, i32 h, real_t scale, const char* filename) {

	u8* magnitudes = malloc(w * h * sizeof(u8));


	for (i32 y = 0; y < h; ++y) {
		real_t* src_row = src + y * w;
		u8* dst_row = magnitudes + y * w;
		for (i32 x = 0; x < w; ++x) {
			real_t c = src_row[x];
			c = c * scale * 255.0f;
			dst_row[x] = (u8)CLAMP(c, 0, 255.0f);
		}
	}

	stbi_write_png(filename, w, h, 1, magnitudes, w);
	free(magnitudes);
}

void debug_create_luminance_png_complex(minfft_cmpl* src, i32 w, i32 h, real_t scale, const char* filename) {

	u8* magnitudes = malloc(w * h * sizeof(u8));


	for (i32 y = 0; y < h; ++y) {
		minfft_cmpl* src_row = src + y * w;
		u8* dst_row = magnitudes + y * w;
		for (i32 x = 0; x < w; ++x) {
			real_t c = src_row[x][0];
			dst_row[x] = (u8)ATMOST(255, (c * scale * 255.0f));
		}
	}

	stbi_write_png(filename, w, h, 1, magnitudes, w);
	free(magnitudes);
}

static v2i find_highest_peak(real_t* src, i32 w, i32 h, float* out_value) {
	v2i point = {};
	real_t highest = 0.0f;
	for (i32 y = 0; y < h; ++y) {
		real_t* row = src + y * w;
		for (i32 x = 0; x < w; ++x) {
			real_t value = row[x];
			if (value > highest) {
				highest = value;
				point = (v2i){x, y};
			}
		}
	}
	if (out_value) *out_value = highest;
	return point;
}

static void fftshift_f(real_t* restrict src, real_t* restrict dst, i32 w, i32 h) {
	// swap quadrants
	ASSERT(w % 2 == 0);
	ASSERT(h % 2 == 0);
	i32 h_half = h / 2;
	i32 w_half = w / 2;
	size_t row_stride = w * sizeof(real_t);
	size_t half_row_size = w_half * sizeof(real_t);
	for (i32 y = 0; y < h_half; ++y) {
		memcpy(dst + (h_half + y) * w + w_half, src + y * w,          half_row_size); // copy from top-left to bottom-right
		memcpy(dst + (h_half + y) * w,          src + y * w + w_half, half_row_size); // copy from top-right to bottom-left
	}
	for (i32 y = 0; y < h_half; ++y) {
		memcpy(dst + y * w + w_half, src + (h_half + y)*w,          half_row_size); // copy from bottom-left to top-right
		memcpy(dst + y * w,          src + (h_half + y)*w + w_half, half_row_size); // copy from bottom-right to top-left
	}
}

// background:
// https://en.wikipedia.org/wiki/Phase_correlation
// https://sthoduka.github.io/imreg_fmt/docs/phase-correlation/

v2f phase_correlate(buffer2d_t* src1, buffer2d_t* src2, buffer2d_t* window, float background, float* response, i32 offset_limit) {

//	debug_create_luminance_png(src1->data, src1->w, src1->h, 1.0f, "patch1_Y.png");
//	debug_create_luminance_png(src2->data, src2->w, src2->h, 1.0f, "patch2_Y.png");

	// Add padding
	i32 smallest_h = MIN(src1->h, src2->h);
	i32 smallest_w = MIN(src1->w, src2->w);
	i32 largest_h = MAX(src1->h, src2->h);
	i32 largest_w = MAX(src1->w, src2->w);
	i32 h = (i32)next_pow2(largest_h /*+ smallest_h*/);
	i32 w = (i32)next_pow2(largest_w /*+ smallest_w*/);

	buffer2d_t padded1 = {};
	buffer2d_t padded2 = {};
	buffer2d_t padded_win = {};

	if(h != src1->h || w != src1->w) {
        // TODO: fix memory leak
		copy_make_border(src1, &padded1, 0, h - src1->h, 0, w - src1->w, background);
		copy_make_border(src2, &padded2, 0, h - src2->h, 0, w - src2->w, background);

		if(window) {
			copy_make_border(window, &padded_win, 0, h - window->h, 0, w - window->w, background);
		}
	} else {
		padded1 = *src1;
		padded2 = *src2;
		if (window) {
			padded_win = *window;
		}
	}

	ASSERT(padded1.w == w && padded1.h == h);
	ASSERT(padded2.w == w && padded2.h == h);

	// perform window multiplication if available
	if (window) {
//		buffer2d_multiply() // TODO: not implemented
	}

	// execute phase correlation equation
	// Reference: http://en.wikipedia.org/wiki/Phase_correlation

	size_t size = padded1.h * padded1.w * sizeof(minfft_cmpl);
	minfft_cmpl* FFT1 = calloc(1, size);
	minfft_cmpl* FFT2 = calloc(1, size);
	minfft_cmpl* P = calloc(1, size);
	real_t* C = calloc(1, w * h * sizeof(real_t));
	real_t* C_shifted = calloc(1, w * h * sizeof(real_t));

	bool check = false;
	bool create_debug_pngs = false;

	if (create_debug_pngs) {
		debug_create_luminance_png(padded1.data, padded1.w, padded1.h, 1.0f, "patch1_Y.png");
		debug_create_luminance_png(padded2.data, padded2.w, padded2.h, 1.0f, "patch2_Y.png");
	}

	minfft_aux* a = minfft_mkaux_realdft_2d(h, w); // prepare aux data
	minfft_realdft(padded1.data, FFT1, a);
	minfft_realdft(padded2.data, FFT2, a);
	i32 fft_w = w / 2 + 1;

	real_t scale = 1.0f / (real_t)(w * h);

	if (create_debug_pngs) {
		debug_create_magnitude_plot(FFT1, fft_w, h, 1.0f, "dft1.png");
		debug_create_magnitude_plot(FFT2, fft_w, h, 1.0f, "dft2.png");
	}

	// calculate cross-power spectrum

	for (i32 y = 0; y < h; ++y) {
		minfft_cmpl* src_row1 = FFT1 + y * fft_w;
		minfft_cmpl* src_row2 = FFT2 + y * fft_w;
		minfft_cmpl* dst_row = P + y * fft_w;
		for (i32 x = 0; x < fft_w; ++x) {
			real_t s1_re = src_row1[x][0];
			real_t s1_im = src_row1[x][1];
			real_t s2_re = src_row2[x][0];
			real_t s2_im = src_row2[x][1];

			/* s1 * conjugate(s2) */
			real_t p_re = s1_re * s2_re + s1_im * s2_im;
			real_t p_im = s1_im * s2_re - s1_re * s2_im;
			real_t magnitude = sqrtf(p_re * p_re + p_im * p_im) + 0.0001f;

			dst_row[x][0] = p_re / magnitude;
			dst_row[x][1] = p_im / magnitude;
		}
	}

	minfft_invrealdft(P, C, a);
	fftshift_f(C, C_shifted, w, h);
	if (create_debug_pngs) debug_create_luminance_png(C, w, h, 10.0f * scale, "phasecorr_real.png");
	if (create_debug_pngs) debug_create_luminance_png(C_shifted, w, h, 10.0f * scale, "phasecorr_real_shifted.png");

    // find highest and second highest peak
	real_t highest = 0.0f;
    real_t lowest = -0.01f;
	v2i peak = {};//find_highest_peak(C_shifted, w, h, &highest);
    real_t second_highest = 0.0f;
    v2i second_highest_peak = {};

    bounds2i search_bounds = {0, 0, w, h};
    if (offset_limit > 0) {
        search_bounds.min.x = ATLEAST(0, w/2 - offset_limit);
        search_bounds.min.y = ATLEAST(0, h/2 - offset_limit);
        search_bounds.max.x = ATMOST(w, w/2 + offset_limit);
        search_bounds.max.y = ATMOST(h, h/2 + offset_limit);
    }

    for (i32 y = search_bounds.min.y; y < search_bounds.max.y; ++y) {
        real_t* row = C_shifted + y * w;
        for (i32 x = search_bounds.min.x; x < search_bounds.max.x; ++x) {
            real_t value = row[x];
            if (value > highest) {
                // NOTE: we require the second highest peak to be separated by some distance from the first peak
                i32 manhattan_distance_from_last_highest = abs(peak.x - x) + abs(peak.y - y);
                if (manhattan_distance_from_last_highest > 4) {
                    second_highest = highest;
                    second_highest_peak = peak;
                }
                highest = value;
                peak = (v2i){x, y};
            } else if (value > second_highest) {
                i32 manhattan_distance_from_last_highest = abs(peak.x - x) + abs(peak.y - y);
                if (manhattan_distance_from_last_highest > 4) {
                    second_highest = value;
                    second_highest_peak = (v2i){x, y};
                }
            }
            if (value < lowest) {
                lowest = value;
            }
        }
    }
    float ratio_between_peaks = (highest - lowest) / (second_highest - lowest);
    float dist_sq = SQUARE(peak.x - second_highest_peak.x) + SQUARE(peak.y - second_highest_peak.y);
    float distance_between_peaks = sqrtf(dist_sq);

    // Subpixel shifting using method by Foroosh et al., see:
    // https://en.wikipedia.org/wiki/Phase_correlation#cite_note-1
    // https://www.cs.ucf.edu/~foroosh/subreg.pdf

    float dx = 0.0f;
    float dy = 0.0f;
    if (peak.x > 0 && peak.x < w - 1 && peak.y > 0 && peak.y < h - 1) {
        i32 center_pixel_index = w * peak.y + peak.x;

        // horizontal subpixel shift
        {
            float dx_sign = 0.0f;
            float second_highest = 0.0f;
            if (C_shifted[center_pixel_index + 1] > C_shifted[center_pixel_index - 1]) {
                second_highest = C_shifted[center_pixel_index + 1];
                dx_sign = 1.0f;
            } else {
                second_highest = C_shifted[center_pixel_index - 1];
                dx_sign = -1.0f;
            }
            float dx_first_solution = second_highest / (second_highest + highest);
            float dx_second_solution = second_highest / (second_highest - highest);
            if (dx_first_solution >= 0.0f && dx_first_solution < 1.0f) {
                dx = dx_sign * dx_first_solution;
            } else if (dx_second_solution >= 0.0f && dx_second_solution < 1.0f) {
                dx = dx_sign * dx_second_solution;
            }
        }

        // vertical subpixel shift
        {
            float dy_sign = 0.0f;
            float second_highest = 0.0f;
            if (C_shifted[center_pixel_index + w] > C_shifted[center_pixel_index - w]) {
                second_highest = C_shifted[center_pixel_index + w];
                dy_sign = 1.0f;
            } else {
                second_highest = C_shifted[center_pixel_index - w];
                dy_sign = -1.0f;
            }
            float dy_first_solution = second_highest / (second_highest + highest);
            float dy_second_solution = second_highest / (second_highest - highest);
            if (dy_first_solution >= 0.0f && dy_first_solution < 1.0f) {
                dy = dy_sign * dy_first_solution;
            } else if (dy_second_solution >= 0.0f && dy_second_solution < 1.0f) {
                dy = dy_sign * dy_second_solution;
            }
        }
    }

	peak.x -= (w/2);
	peak.y -= (h/2);
    v2f peak_exact = {(float)peak.x + dx, (float)peak.y + dy};
    highest *= scale;
    if (response) *response = highest;
	console_print("Phase correlation: highest peak (%d, %d), value = %g, ratio between peaks = %g; dist between peaks = %g; subpixel shift (%g, %g)\n", peak.x, peak.y, highest, ratio_between_peaks, distance_between_peaks, peak_exact.x, peak_exact.y);

	if (check) {
        // NOTE: the inverse DFT should give the original grayscale image back (sanity check)
		minfft_invrealdft(FFT1, padded1.data, a);
		minfft_invrealdft(FFT2, padded2.data, a);
		if (create_debug_pngs) debug_create_luminance_png(padded1.data, w, h, scale, "patch1_Y_check.png");
		if (create_debug_pngs) debug_create_luminance_png(padded2.data, w, h, scale, "patch2_Y_check.png");
	}

    libc_free(a);
    free(FFT1);
    free(FFT2);
    free(P);
    free(C);
    free(C_shifted);

	return peak_exact;
}
