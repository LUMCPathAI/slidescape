/*
  BSD 2-Clause License

  Copyright (c) 2019-2026, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
  Decoder for whole-slide image files in iSyntax format.

  This implementation is based on the documentation on the iSyntax format released by Philips:
  https://www.openpathology.philips.com/isyntax/

  See the following documents, and the accompanying source code samples:
  - "Fast Compression Method for Medical Images on the Web", by Bas Hulsken
    https://arxiv.org/abs/2005.08713
  - The description of the iSyntax image files:
    https://www.openpathology.philips.com/wp-content/uploads/isyntax/4522%20207%2043941_2020_04_24%20Pathology%20iSyntax%20image%20format.pdf

  This implementation does not require the Philips iSyntax SDK.

  NOTE (2023-03-29):
  Unfortunately, Philips' original OpenPathology website is no longer accessible.
  The documentation on the file format is still available from Philips, but has moved to another location:
  https://share.philips.com/sites/OpenPathologyPortal
  However, you have to contact Philips directly to get access there.

*/

#include "common.h"
#include "work_queue.h"
#include "intrinsics.h"

#include "isyntax.h"

// XML library for parsing the header
#include "yxml.h"

// JPEG decoding library for macro/label images
#ifdef ISYNTAX_JPEG_DECODER_USE_LIBJPEG
#include "jpeg_decoder.h"
#else
#include "stb_image.h"
#endif

// Enable/disable debug routines for creating PNGs of IDWT steps
#define ISYNTAX_WANT_DEBUG_OUTPUT_PNG 0
#if ISYNTAX_WANT_DEBUG_OUTPUT_PNG
//#define STB_IMAGE_WRITE_IMPLEMENTATION
// stb_image_write.h provides its own crc32() implementation, so we need to prevent a conflict if we already have it
#define STBIW_CRC32 crc32
#include <stb_image_write.h>
#endif

#include <ctype.h>

#define PER_LEVEL_PADDING 3

#include "isyntax_dwt.c"

static inline isyntax_dicom_tag_header_t isyntax_read_dicom_tag_header(const void* src) {
	const u8* data = (const u8*)src;
	isyntax_dicom_tag_header_t result = {};
	result.group = read_u16_le(data + 0);
	result.element = read_u16_le(data + 2);
	result.size = read_u32_le(data + 4);
	return result;
}

static inline u32 isyntax_read_u32_le(const void* src) {
	return read_u32_le(src);
}

static inline u64 isyntax_read_u64_le(const void* src) {
	return read_u64_le(src);
}

// NOTE: this serves as an alternative to strncpy: copies up to dest_size-1 and always null-terminates
static size_t isyntax_copy_cstring(char* dest, const char* src, size_t dest_size) {
	size_t len = 0;

	if (!src) {
		if (dest_size > 0) dest[0] = '\0';
		return 0;
	}

	if (dest_size > 0) {
		for (; len + 1 < dest_size && src[len]; ++len) {
			dest[len] = src[len];
		}
		dest[len] = '\0';
	}

	while (src[len]) {
		++len;
	}

	return len; // source length; truncation if return >= dest_size
}

// Base64 decoder by Jouni Malinen, original:
// http://web.mit.edu/freebsd/head/contrib/wpa/src/utils/base64.c
// Performance comparison of base64 encoders/decoders:
// https://github.com/gaspardpetit/base64/

/*
 * Base64 encoding/decoding (RFC1341)
 * Copyright (c) 2005-2011, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */
static const unsigned char base64_table[65] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned char * base64_decode(const unsigned char *src, size_t len,
                              size_t *out_len)
{
	unsigned char dtable[256], *out, *pos, block[4], tmp;
	size_t i, count, olen;
	int pad = 0;

	memset(dtable, 0x80, 256);
	for (i = 0; i < sizeof(base64_table) - 1; i++)
		dtable[base64_table[i]] = (unsigned char) i;
	dtable['='] = 0;

	count = 0;
	for (i = 0; i < len; i++) {
		if (dtable[src[i]] != 0x80)
			count++;
	}

	if (count == 0 || count % 4)
		return NULL;

	olen = count / 4 * 3;
	pos = out = malloc(olen);
	if (out == NULL)
		return NULL;

	count = 0;
	for (i = 0; i < len; i++) {
		tmp = dtable[src[i]];
		if (tmp == 0x80)
			continue;

		if (src[i] == '=')
			pad++;
		block[count] = tmp;
		count++;
		if (count == 4) {
			*pos++ = (block[0] << 2) | (block[1] >> 4);
			*pos++ = (block[1] << 4) | (block[2] >> 2);
			*pos++ = (block[2] << 6) | block[3];
			count = 0;
			if (pad) {
				if (pad == 1)
					pos--;
				else if (pad == 2)
					pos -= 2;
				else {
					/* Invalid padding */
					free(out);
					return NULL;
				}
				break;
			}
		}
	}

	*out_len = pos - out;
	return out;
}
// end of base64 decoder.

// Wrapper for base64_decode, taking into account possible extra (invalid) characters at the end
u8* isyntax_base64_decode(const char* src, size_t len, size_t *out_len) {
	if (len > 0) {
		char last_char = src[len-1];
		if (last_char == '/') {
			len--; // The last character may cause the base64 decoding to fail if invalid
			last_char = src[len-1];
		}
		while (last_char == '\n' || last_char == '\r' || last_char == ' ') {
			--len;
			last_char = src[len-1];
		}
	}
	u8* decoded = base64_decode((u8*)src, len, out_len);
	return decoded;
}

// similar to atoi(), but also returning the string position so we can chain calls one after another.
static const char* atoi_and_advance(const char* str, i32* dest) {
	i32 num = 0;
	bool neg = false;
	while (isspace(*str)) ++str;
	if (*str == '-') {
		neg = true;
		++str;
	}
	while (isdigit(*str)) {
		num = 10*num + (*str - '0');
		++str;
	}
	if (neg) num = -num;
	*dest = num;
	return str;
}

static void parse_three_integers(const char* str, i32* first, i32* second, i32* third) {
	str = atoi_and_advance(str, first);
	str = atoi_and_advance(str, second);
	atoi_and_advance(str, third);
}

static i32 parse_up_to_five_integers(const char* str, i32* array_of_five_integers) {
	for (i32 i = 0; i < 5; ++i) {
		if (*str == '\0') {
			for (i32 j = i; i < 5; ++j) {
				array_of_five_integers[j] = 0;
				return i; // return number of valid integers
			}
		} else {
			str = atoi_and_advance(str, array_of_five_integers + i);
		}
	}
	return 5;
}

static void bgra_to_rgba(uint32_t *pixels, int width, int height) {
    int num_pixels = width * height;
    int num_pixels_aligned = (num_pixels / 4) * 4;

#if defined(__ARM_NEON)
    for (int i = 0; i < num_pixels_aligned; i += 4) {
        uint32x4_t bgra = vld1q_u32(pixels + i);
        uint32x4_t b_mask = vdupq_n_u32(0x000000FF);
        uint32x4_t r_mask = vdupq_n_u32(0x00FF0000);
        uint32x4_t b = vandq_u32(bgra, b_mask);
        uint32x4_t r = vandq_u32(bgra, r_mask);
        uint32x4_t br_swapped = vorrq_u32(vshlq_n_u32(b, 16), vshrq_n_u32(r, 16));
        uint32x4_t ga_alpha_mask = vdupq_n_u32(0xFF00FF00);
        uint32x4_t ga_alpha = vandq_u32(bgra, ga_alpha_mask);
        uint32x4_t rgba = vorrq_u32(ga_alpha, br_swapped);
        vst1q_u32(pixels + i, rgba);
    }
#elif defined(__SSE2__)
    for (int i = 0; i < num_pixels_aligned; i += 4) {
        __m128i bgra = _mm_loadu_si128((__m128i*)(pixels + i));
        __m128i b_mask = _mm_set1_epi32(0x000000FF);
        __m128i r_mask = _mm_set1_epi32(0x00FF0000);
        __m128i b = _mm_and_si128(bgra, b_mask);
        __m128i r = _mm_and_si128(bgra, r_mask);
        __m128i br_swapped = _mm_or_si128(_mm_slli_epi32(b, 16), _mm_srli_epi32(r, 16));
        __m128i ga_alpha_mask = _mm_set1_epi32(0xFF00FF00);
        __m128i ga_alpha = _mm_and_si128(bgra, ga_alpha_mask);
        __m128i rgba = _mm_or_si128(ga_alpha, br_swapped);
        _mm_storeu_si128((__m128i*)(pixels + i), rgba);
    }
#else
    for (int i = num_pixels_aligned; i < num_pixels; ++i) {
        uint32_t val = pixels[i];
        pixels[i] = ((val & 0xff) << 16) | (val & 0x00ff00) | ((val & 0xff0000) >> 16) | (val & 0xff000000);
    }
#endif
}

static u8* isyntax_decode_jpeg_stream(u8* compressed, size_t compressed_len, i32* width, i32* height, i32* channels_in_file,
                                      enum isyntax_pixel_format_t pixel_format) {
    u8* pixels = NULL;
    i32 w = 0;
    i32 h = 0;

#ifdef ISYNTAX_JPEG_DECODER_USE_LIBJPEG
    pixels = jpeg_decode_image(compressed, compressed_len, &w, &h, channels_in_file);
    if (pixel_format == LIBISYNTAX_PIXEL_FORMAT_BGRA) {
        DUMMY_STATEMENT; // no action needed
    } else if (pixel_format == LIBISYNTAX_PIXEL_FORMAT_RGBA) {
        bgra_to_rgba((uint32_t*)pixels, w, h);
    }
#else
    // stb_image.h
    pixels = stbi_load_from_memory(compressed, (int)compressed_len, &w, &h, channels_in_file, 4);
    if (pixel_format == LIBISYNTAX_PIXEL_FORMAT_RGBA) {
        DUMMY_STATEMENT; // no action needed
    } else if (pixel_format == LIBISYNTAX_PIXEL_FORMAT_BGRA) {
        bgra_to_rgba((uint32_t*)pixels, w, h);
    }
#endif
    if (width) *width = w;
    if (height) *height = h;
    return pixels;
}

// Read base64-encoded label or macro image from file and return decompressed pixels.
// TODO(pvalkema): remove this / only support returning compressed JPEG buffer and leave decompression to caller?
u8* isyntax_get_associated_image_pixels(isyntax_t* isyntax, isyntax_image_t* image, enum isyntax_pixel_format_t pixel_format) {
    u8* decompressed = NULL;
    i32 channels_in_file = 0;
    u32 jpeg_compressed_len = 0;
    u8* jpeg_compressed = isyntax_get_associated_image_jpeg(isyntax, image, &jpeg_compressed_len);
    if (jpeg_compressed) {
        decompressed = isyntax_decode_jpeg_stream(jpeg_compressed, jpeg_compressed_len,
                                                  &image->width, &image->height, &channels_in_file, pixel_format);
        free(jpeg_compressed);
    }
    return decompressed;
}

// Read base64-encoded label or macro image from file and return the decoded (still JPEG-compressed) image,
// writing the length of the JPEG buffer into jpeg_size.
u8* isyntax_get_associated_image_jpeg(isyntax_t* isyntax, isyntax_image_t* image, u32* jpeg_size) {
    if (jpeg_size == NULL) {
        return NULL;
    }
    i64 read_offset = image->base64_encoded_jpg_file_offset;
    size_t read_size = image->base64_encoded_jpg_len;
    u8* decoded = NULL;
    if (read_offset > 0 && read_size > 0) {
        u8* encoded = malloc(read_size);
        size_t bytes_read = file_handle_read_at_offset(encoded, isyntax->file_handle, read_offset, read_size);
        if (bytes_read == read_size) {
            size_t len = 0;
            decoded = base64_decode((u8*)encoded, read_size, &len);
            if (decoded) {
                *jpeg_size = len;
            }
        }
        free(encoded);
    }
    return decoded;
}

u8* isyntax_get_icc_profile(isyntax_t* isyntax, isyntax_image_t* image, u32* icc_profile_size) {
	if (icc_profile_size == NULL) {
		return NULL;
	}
	i64 read_offset = image->base64_encoded_icc_profile_file_offset;
	size_t read_size = image->base64_encoded_icc_profile_len;
	u8* decoded = NULL;
	if (read_offset > 0 && read_size > 0) {
		u8* encoded = malloc(read_size);
		size_t bytes_read = file_handle_read_at_offset(encoded, isyntax->file_handle, read_offset, read_size);
		if (bytes_read == read_size) {
			size_t len = 0;
			decoded = base64_decode((u8*)encoded, read_size, &len);
			if (decoded) {
				*icc_profile_size = len;
			}
		}
		free(encoded);
	}
	return decoded;
}

static void isyntax_parse_ufsimport_child_node(isyntax_t* isyntax, u32 group, u32 element, char* value, u64 value_len) {

	switch(group) {
		default: {
			// unknown group
			console_print_verbose("Unknown group 0x%04x\n", group);
		} break;
		case 0x0008: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x002A: /*DICOM_ACQUISITION_DATETIME*/     { // "20210101103030.000000"
					isyntax_copy_cstring(isyntax->dicom_acquisition_datetime, value, MIN(value_len, sizeof(isyntax->dicom_acquisition_datetime)));
				} break;
				case 0x0070: /*DICOM_MANUFACTURER*/             { // "PHILIPS"
					isyntax_copy_cstring(isyntax->dicom_manufacturer, value, MIN(value_len, sizeof(isyntax->dicom_manufacturer)));
				} break;
				case 0x1090: /*DICOM_MANUFACTURERS_MODEL_NAME*/ { // "UFS Scanner"
					isyntax_copy_cstring(isyntax->dicom_manufacturers_model_name, value, MIN(value_len, sizeof(isyntax->dicom_manufacturers_model_name)));
				} break;
			}
		}; break;
		case 0x0018: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x1000: /*DICOM_DEVICE_SERIAL_NUMBER*/     { // "FMT<4-digit number>"
					isyntax_copy_cstring(isyntax->dicom_device_serial_number, value, MIN(value_len, sizeof(isyntax->dicom_device_serial_number)));
				} break;
				case 0x1020: /*DICOM_SOFTWARE_VERSIONS*/        {  // "<versionnumber>" "<versionnumber>"
					if (value_len < 2) {
						// Each version is wrapped in quotes, so at least 2 characters are needed.
						isyntax->dicom_software_versions = NULL;
						isyntax->dicom_software_versions_count = 0;
						break;
					}
					// Count number of versions by counting number of '" "'
					i32 version_count = 1;
					for (i32 i = 0; i + 2 < value_len; ++i) {
						if (value[i] == '"' && value[i + 1] == ' ' && value[i + 2] == '"') {
							version_count++;
						}
					}
					isyntax->dicom_software_versions = malloc(version_count * sizeof(*isyntax->dicom_software_versions));
					if (!isyntax->dicom_software_versions) {
						isyntax->dicom_software_versions_count = 0;
						break;
					}
					i32 version_index = 0;
					const char* version_start = value + 1;
					for (i32 i = 0; i < value_len; ++i) {
						if (version_index >= version_count) {
							break; // no more versions to parse
						}
						// Check if either '" "' or '"' at end of string
						if (value[i] == '"' && ((i + 2 < value_len && value[i + 1] == ' ' && value[i + 2] == '"') || i + 1 == value_len)) {
							size_t version_len = value + i - version_start;
							isyntax_copy_cstring(isyntax->dicom_software_versions[version_index], version_start, MIN(version_len, sizeof(isyntax->dicom_software_versions[version_index])));
							version_index++;
							version_start = value + i + 3;
						}
					}
					isyntax->dicom_software_versions_count = version_count;

				} break;
				case 0x1200: /*DICOM_DATE_OF_LAST_CALIBRATION*/ { // "20210101"
					if (value_len < 10) {
						isyntax->dicom_date_of_last_calibration = NULL;
						isyntax->dicom_date_of_last_calibration_count = 0;
						break;
					}
					i32 number_of_values = 1 + (value_len - 10) / 11;
					isyntax->dicom_date_of_last_calibration = malloc(number_of_values * sizeof(*isyntax->dicom_date_of_last_calibration));
					if (!isyntax->dicom_date_of_last_calibration) {
						isyntax->dicom_date_of_last_calibration_count = 0;
						break;
					};
					for (i32 i = 0; i < number_of_values; ++i) {
						isyntax_copy_cstring(isyntax->dicom_date_of_last_calibration[i], value + 1 + i * 11, MIN(8, sizeof(isyntax->dicom_date_of_last_calibration[i])));
					}
					isyntax->dicom_date_of_last_calibration_count = number_of_values;


				} break;
				case 0x1201: /*DICOM_TIME_OF_LAST_CALIBRATION*/ { // "100730"
					if (value_len < 8) {
						isyntax->dicom_time_of_last_calibration = NULL;
						isyntax->dicom_time_of_last_calibration_count = 0;
						break;
					}
					i32 number_of_values = 1 + (value_len - 8) / 9;
					isyntax->dicom_time_of_last_calibration = malloc(number_of_values * sizeof(*isyntax->dicom_time_of_last_calibration));
					if (!isyntax->dicom_time_of_last_calibration) {
						isyntax->dicom_time_of_last_calibration_count = 0;
						break;
					};
					for (i32 i = 0; i < number_of_values; ++i) {
						isyntax_copy_cstring(isyntax->dicom_time_of_last_calibration[i], value + 1 + i * 9, MIN(6, sizeof(isyntax->dicom_time_of_last_calibration[i])));
					}
					isyntax->dicom_time_of_last_calibration_count = number_of_values;

				} break;
			}
		} break;
		case 0x101D: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x1007: /*PIIM_DP_SCANNER_RACK_NUMBER*/        {} break; // "[1..15]"
				case 0x1008: /*PIIM_DP_SCANNER_SLOT_NUMBER*/        {} break; // "[1..15]"
				case 0x1009: /*PIIM_DP_SCANNER_OPERATOR_ID*/        {} break; // "<Operator ID>"
				case 0x100A: /*PIIM_DP_SCANNER_CALIBRATION_STATUS*/ {} break; // "OK" or "NOT OK"

			}
		} break;
		case 0x301D: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x1001: /*PIM_DP_UFS_INTERFACE_VERSION*/          {
					// Value will likely be "5.0" for v1 iSyntax files, "100.5" for v2 iSyntax files
					if (sscanf(value, "%" SCNd32 ".%" SCNd32, &isyntax->data_model_major_version, &isyntax->data_model_minor_version) != 2)
						isyntax->data_model_minor_version = 0;
				} break;
				case 0x1002: /*PIM_DP_UFS_BARCODE*/ {
					// "<base64-encoded barcode value>"
					if (value_len > 0) {
						size_t decoded_len = 0;
						char* decoded = (char*) isyntax_base64_decode(value, value_len, &decoded_len);
						if (decoded) {
							memcpy(isyntax->barcode, decoded, MIN(decoded_len, sizeof(isyntax->barcode)-1));
							free(decoded);
						}
					}
					isyntax->is_barcode_read = true;
				} break;
				case 0x1003: /*PIM_DP_SCANNED_IMAGES*/                 {} break;
				case 0x1010: /*PIM_DP_SCANNER_RACK_PRIORITY*/          {} break; // "<u16>"
			}
		} break;
	}
}

static bool isyntax_parse_scannedimage_child_node(isyntax_t* isyntax, u32 group, u32 element, char* value, u64 value_len) {

	// Parse metadata belong to one of the images in the file (either a WSI, LABELIMAGE or MACROIMAGE)

	isyntax_image_t* image = isyntax->parser.current_image;
	if (!image) {
		image = isyntax->parser.current_image = &isyntax->images[0];
	}

	bool success = true;

	switch(group) {
		default: {
			// unknown group
			console_print_verbose("Unknown group 0x%04x\n", group);
		} break;
		case 0x0008: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x2111: /*DICOM_DERIVATION_DESCRIPTION*/   {         // "PHILIPS UFS V%s | Quality=%d | DWT=%d | Compressor=%d"
					isyntax_copy_cstring(isyntax->dicom_derivation_description, value, MIN(value_len, sizeof(isyntax->dicom_derivation_description)));
				} break;
			}
		}; break;
		case 0x0028: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x0002: /*DICOM_SAMPLES_PER_PIXEL*/                {} break;
				case 0x0100: /*DICOM_BITS_ALLOCATED*/                   {} break;
				case 0x0101: /*DICOM_BITS_STORED*/ {
					if (isyntax->parser.data_object_flags & ISYNTAX_OBJECT_UFSImageBlockHeaderTemplate) {
						isyntax_block_header_template_t* template = isyntax->block_header_templates + isyntax->parser.block_header_template_index;
						template->bits_stored = atoi(value);
					}
				} break;
				case 0x0102: /*DICOM_HIGH_BIT*/                         {} break;
				case 0x0103: /*DICOM_PIXEL_REPRESENTATION*/             {} break;
				case 0x2000: /*DICOM_ICCPROFILE*/                       {
					char last_char = value[value_len-1];
					if (last_char == '/') {
						value_len--; // The last character may cause the base64 decoding to fail if invalid
					}
					image->base64_encoded_icc_profile_file_offset = isyntax->parser.content_file_offset;
					image->base64_encoded_icc_profile_len = value_len;
				} break;
				case 0x2110: /*DICOM_LOSSY_IMAGE_COMPRESSION*/          {
					isyntax->dicom_lossy_image_compression = (strcmp(value, "01") == 0);
				} break;
				case 0x2112: /*DICOM_LOSSY_IMAGE_COMPRESSION_RATIO*/    {
					// Assumes only one value is present.
					isyntax->dicom_lossy_image_compression_ratio = atof(value);
				} break;
				case 0x2114: /*DICOM_LOSSY_IMAGE_COMPRESSION_METHOD*/   { // "PHILIPS_DP_1_0"
					// Assumes only one value is present.
					// Strip starting and ending " if present
					size_t start = 0, end = value_len;
					if (value_len > 0 && value[0] == '"') {
						start = 1;
					}
					if (end > start && value[end - 1] == '"') {
						end--;
					}
					size_t copy_len = MIN(end - start, sizeof(isyntax->dicom_lossy_image_compression_method) - 1);
					isyntax_copy_cstring(isyntax->dicom_lossy_image_compression_method, value + start, copy_len);
				} break;
			}
		} break;
		case 0x301D: {
			switch(element) {
				default: {
					console_print_verbose("Unknown element (0x%04x, 0x%04x)\n", group, element);
				} break;
				case 0x1004: /*PIM_DP_IMAGE_TYPE*/         {         // "MACROIMAGE" or "LABELIMAGE" or "WSI"
					if ((strcmp(value, "MACROIMAGE") == 0)) {
						isyntax->macro_image_index = isyntax->parser.running_image_index;
						isyntax->parser.current_image_type = ISYNTAX_IMAGE_TYPE_MACROIMAGE;
						image->image_type = ISYNTAX_IMAGE_TYPE_MACROIMAGE;
					} else if ((strcmp(value, "LABELIMAGE") == 0)) {
						isyntax->label_image_index = isyntax->parser.running_image_index;
						isyntax->parser.current_image_type = ISYNTAX_IMAGE_TYPE_LABELIMAGE;
						image->image_type = ISYNTAX_IMAGE_TYPE_LABELIMAGE;
					} else if ((strcmp(value, "WSI") == 0)) {
						isyntax->wsi_image_index = isyntax->parser.running_image_index;
						isyntax->parser.current_image_type = ISYNTAX_IMAGE_TYPE_WSI;
						image->image_type = ISYNTAX_IMAGE_TYPE_WSI;
					}
				} break;
				case 0x1005: { /*PIM_DP_IMAGE_DATA*/
					char last_char = value[value_len-1];
					if (last_char == '/') {
						value_len--; // The last character may cause the base64 decoding to fail if invalid
					}
                    image->base64_encoded_jpg_file_offset = isyntax->parser.content_file_offset;
                    image->base64_encoded_jpg_len = value_len;
				} break;
				case 0x1013: /*DP_COLOR_MANAGEMENT*/                        {} break;
				case 0x1014: /*DP_IMAGE_POST_PROCESSING*/                   {} break;
				case 0x1015: /*DP_SHARPNESS_GAIN_RGB24*/                    {} break;
				case 0x1016: /*DP_CLAHE_CLIP_LIMIT_Y16*/                    {} break;
				case 0x1017: /*DP_CLAHE_NR_BINS_Y16*/                       {} break;
				case 0x1018: /*DP_CLAHE_CONTEXT_DIMENSION_Y16*/             {} break;
				case 0x1019: /*DP_WAVELET_QUANTIZER_SETTINGS_PER_COLOR*/    {} break;
				case 0x101A: /*DP_WAVELET_QUANTIZER_SETTINGS_PER_LEVEL*/    {} break;
				case 0x101B: /*DP_WAVELET_QUANTIZER*/                       {} break;
				case 0x101C: /*DP_WAVELET_DEADZONE*/                        {} break;
				case 0x1025: /*UFS_IMAGE_OPP_EXTREME_VERTEX*/               {} break; // data model >= 100
				case 0x2000: /*UFS_IMAGE_GENERAL_HEADERS*/                  {} break;
				case 0x2001: /*UFS_IMAGE_NUMBER_OF_BLOCKS*/                 {
					// NOTE: the actual number of stored codeblocks may be lower than this number.
					image->number_of_blocks = atoi(value);
				} break;
				case 0x2002: /*UFS_IMAGE_DIMENSIONS_OVER_BLOCK*/            {} break;
				case 0x2003: /*UFS_IMAGE_DIMENSIONS*/                       {} break;
				case 0x2004: /*UFS_IMAGE_DIMENSION_NAME*/                   {} break;
				case 0x2005: /*UFS_IMAGE_DIMENSION_TYPE*/                   {} break;
				case 0x2006: /*UFS_IMAGE_DIMENSION_UNIT*/                   {
					isyntax_copy_cstring(isyntax->image_dimension_unit, value, MIN(value_len, sizeof(isyntax->image_dimension_unit)));
				} break;
				case 0x2007: /*UFS_IMAGE_DIMENSION_SCALE_FACTOR*/           {
					// in data model >= 100 the macro and label images have their own scaling factor
					float mpp = atof(value);
					if (isyntax->parser.dimension_index == 0 /*x*/) {
						image->mpp_x = mpp;
						image->is_mpp_known = true;
					} else if (isyntax->parser.dimension_index == 1 /*y*/) {
						image->mpp_y = mpp;
						image->is_mpp_known = true;
					}
				} break;
				case 0x2008: /*UFS_IMAGE_DIMENSION_DISCRETE_VALUES_STRING*/ {} break;
				case 0x2009: /*UFS_IMAGE_BLOCK_HEADER_TEMPLATES*/           {} break;
				case 0x200A: /*UFS_IMAGE_DIMENSION_RANGES*/                 {} break;
				case 0x200B: /*UFS_IMAGE_DIMENSION_RANGE*/                  {
					isyntax_image_dimension_range_t range = {0};
					parse_three_integers(value, &range.start, &range.step, &range.end);
					i32 step_nonzero = (range.step != 0) ? range.step : 1;
					range.numsteps = ((range.end + range.step) - range.start) / step_nonzero;
					if (isyntax->parser.data_object_flags & ISYNTAX_OBJECT_UFSImageBlockHeaderTemplate) {
						isyntax_block_header_template_t* template = isyntax->block_header_templates + isyntax->parser.block_header_template_index;
						switch(isyntax->parser.dimension_index) {
							default: break;
							case 0: template->block_width = range.numsteps; break;
							case 1: template->block_height = range.numsteps; break;
							case 2: template->color_component = range.start; break;
							case 3: template->scale = range.start; break;
							case 4: template->waveletcoeff = (range.start == 0) ? 1 : 3; break;
						}
						DUMMY_STATEMENT;
					} else if (isyntax->parser.data_object_flags & ISYNTAX_OBJECT_UFSImageGeneralHeader) {
						switch(isyntax->parser.dimension_index) {
							default: break;
							case 0: {
								image->offset_x = range.start;
								image->width_including_padding = range.numsteps;
							} break;
							case 1: {
								image->offset_y = range.start;
								image->height_including_padding = range.numsteps;
							} break;
							case 2: break; // always 3 color channels ("Y" "Co" "Cg"), no need to check
							case 3: {
								image->level_count = range.numsteps;
								image->max_scale = range.numsteps - 1;
                                image->level0_padding = (PER_LEVEL_PADDING << range.numsteps) - PER_LEVEL_PADDING;
                                image->width = image->width_including_padding - 2 * image->level0_padding;
                                image->height = image->height_including_padding - 2 * image->level0_padding;
							} break;
							case 4: break; // always 4 wavelet coefficients ("LL" "LH" "HL" "HH"), no need to check
						}
						DUMMY_STATEMENT;
					} else if (isyntax->parser.data_object_flags & ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate) {
						// The dimension range information within a cluster header template (specifically, the range.start)
						// seems to provide the 'base values' for the dimension offsets for each of the codeblocks in the cluster template.
						// To get the correct coordinate info for each codeblock, we need to add these 'base values'
						// to the codeblock-specific coordinate values from the UFS_IMAGE_BLOCK_COORDINATE tags.
						isyntax_cluster_header_template_t* template = isyntax->cluster_header_templates + isyntax->parser.cluster_header_template_index;
						switch(isyntax->parser.dimension_index) {
							default: break;
							case 0: template->base_x = range.start; break;
							case 1: template->base_y = range.start; break;
							case 2: template->base_color_component = range.start; break;
							case 3: template->base_scale = range.start; break;
							case 4: template->base_waveletcoeff = range.start; break;
						}

					}
					DUMMY_STATEMENT;
				} break;
				case 0x200C: /*UFS_IMAGE_DIMENSION_IN_BLOCK*/               {} break;
				case 0x200D: /*UFS_IMAGE_BLOCK_HEADERS*/                    {} break;
				case 0x200E: /*UFS_IMAGE_BLOCK_COORDINATE*/                 {
					if (isyntax->parser.data_object_flags & (ISYNTAX_OBJECT_UFSImageBlockHeader | ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate)) {
						isyntax_cluster_header_template_t* template = isyntax->cluster_header_templates + isyntax->parser.cluster_header_template_index;
						// NOTE: the order of the coordinates stored in UFS_IMAGE_BLOCK_COORDINATE is variable and depends on UFS_IMAGE_DIMENSIONS_IN_CLUSTER (which might be stored later in the file)
						// So, all we can do at this point is read them as 'raw coordinates' and link them with the actual coordinate once we have the complete UFS_IMAGE_DIMENSIONS_IN_CLUSTER info
						parse_up_to_five_integers(value, (i32*)template->relative_coords_for_codeblock_in_cluster[isyntax->parser.block_header_index_for_cluster].raw_coords);
					}
				} break;
				case 0x200F: /*UFS_IMAGE_BLOCK_COMPRESSION_METHOD*/         {
					i32 compression_method = atoi(value);
					if (compression_method == 16) {
						image->compressor_version = 1;
					} else if (compression_method == 19) {
						image->compressor_version = 2;
					} else {
						// unknown compressor version
						success = false;
					}
				} break;
				case 0x2012: /*UFS_IMAGE_BLOCK_HEADER_TEMPLATE_ID*/         {
					if (isyntax->parser.data_object_flags & (ISYNTAX_OBJECT_UFSImageBlockHeader | ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate)) {
						isyntax_cluster_header_template_t* template = isyntax->cluster_header_templates + isyntax->parser.cluster_header_template_index;
						isyntax_cluster_relative_coords_t* relative_coords_for_codeblock_in_cluster = template->relative_coords_for_codeblock_in_cluster + isyntax->parser.block_header_index_for_cluster;
						relative_coords_for_codeblock_in_cluster->block_header_template_id = atoi(value);
					}
				} break;
				case 0x2013: /*UFS_IMAGE_PIXEL_TRANSFORMATION_METHOD*/      {} break;
				case 0x2014: { /*UFS_IMAGE_BLOCK_HEADER_TABLE*/      // data model <100
					// NOTE: mutually exclusive with UFS_IMAGE_BLOCK_HEADERS (either one or the other must be present)
/*
					FILE* test_out = file_stream_open_for_writing("test_b64.out");
					file_stream_write(value, value_len, test_out);
					file_stream_close(test_out);
*/
					size_t decoded_len = 0;
					u8* decoded = isyntax_base64_decode(value, value_len, &decoded_len);
					if (decoded) {

						u32 header_size = isyntax_read_u32_le(decoded);
						u8* block_header_start = decoded + 4;
						isyntax_dicom_tag_header_t sequence_element = isyntax_read_dicom_tag_header(block_header_start);
						if (sequence_element.size == 40) {
							// We have a partial header structure, with 'Block Data Offset' and 'Block Size' missing (stored in Seektable)
							// Full block header size (including the sequence element) is 48 bytes
							u32 block_count = header_size / 48;
							u32 should_be_zero = header_size % 48;
							if (should_be_zero != 0) {
								success = false;
							}

							image->codeblock_count = block_count;
							image->codeblocks = calloc(1, block_count * sizeof(isyntax_codeblock_t));
							image->header_codeblocks_are_partial = true;

							for (i32 i = 0; i < block_count; ++i) {
								u8* header = block_header_start + i * sizeof(isyntax_partial_block_header_t);
								isyntax_codeblock_t* codeblock = image->codeblocks + i;
								codeblock->x_coordinate = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, x_coordinate));
								codeblock->y_coordinate = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, y_coordinate));
								codeblock->color_component = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, color_component));
								codeblock->scale = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, scale));
								codeblock->coefficient = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, coefficient));
								codeblock->block_header_template_id = isyntax_read_u32_le(header + offsetof(isyntax_partial_block_header_t, block_header_template_id));
								DUMMY_STATEMENT;
							}

						} else if (sequence_element.size == 72) {
							// We have the complete header structure. (Nothing stored in Seektable)
							u32 block_count = header_size / 80;
							u32 should_be_zero = header_size % 80;
							if (should_be_zero != 0) {
								success = false;
							}

							image->codeblock_count = block_count;
							image->codeblocks = calloc(1, block_count * sizeof(isyntax_codeblock_t));
							image->header_codeblocks_are_partial = false;

							for (i32 i = 0; i < block_count; ++i) {
								u8* header = block_header_start + i * sizeof(isyntax_full_block_header_t);
								isyntax_codeblock_t* codeblock = image->codeblocks + i;
								codeblock->x_coordinate = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, x_coordinate));
								codeblock->y_coordinate = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, y_coordinate));
								codeblock->color_component = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, color_component));
								codeblock->scale = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, scale));
								codeblock->coefficient = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, coefficient));
								codeblock->block_data_offset = isyntax_read_u64_le(header + offsetof(isyntax_full_block_header_t, block_data_offset)); // extra
								codeblock->block_size = isyntax_read_u64_le(header + offsetof(isyntax_full_block_header_t, block_size)); // extra
								codeblock->block_header_template_id = isyntax_read_u32_le(header + offsetof(isyntax_full_block_header_t, block_header_template_id));
								DUMMY_STATEMENT;
							}
						} else {
							success = false;
						}

						free(decoded);
					} else {
						success = false;
					}
				} break;
				case 0x2016: /*UFS_IMAGE_CLUSTER_HEADER_TEMPLATES*/ {} break; // data model >= 100
				case 0x2017: /*UFS_IMAGE_DIMENSIONS_OVER_CLUSTER*/  {} break;
				case 0x201F: /*UFS_IMAGE_CLUSTER_HEADER_TABLE*/ { // data model >= 100
/*
					FILE* test_out = file_stream_open_for_writing("test_b64.out");
					file_stream_write(value, value_len, test_out);
					file_stream_close(test_out);
*/
					size_t decoded_len = 0;
					u8* decoded = isyntax_base64_decode(value, value_len, &decoded_len);
					if (decoded) {
						u8* decoded_end = decoded + decoded_len;
						u32 header_size = isyntax_read_u32_le(decoded);
						u8* block_header_start = decoded + 4;
						u8* pos = block_header_start;
						isyntax_dicom_tag_header_t sequence_element = isyntax_read_dicom_tag_header(pos);

						// first element should be a sequence tag
						if (!(sequence_element.group == 0xfffe && sequence_element.element == 0xe000)) {
							goto decoding_cluster_header_table_failed;
						}
						// pass 1: check how many clusters there are
						i32 cluster_count = 1;
						for (;;) {
							isyntax_dicom_tag_header_t current_sequence_element = isyntax_read_dicom_tag_header(pos);
							u8* next_sequence_element_pos = pos + sizeof(isyntax_dicom_tag_header_t) + current_sequence_element.size;
							if (next_sequence_element_pos >= decoded_end) {
								break;
							}
							isyntax_dicom_tag_header_t next_sequence_element = isyntax_read_dicom_tag_header(next_sequence_element_pos);
							if (next_sequence_element.element != 0xe000) {
								break;
							}
							++cluster_count;
							pos = next_sequence_element_pos;
						}

						// preallocate memory for codeblocks and clusters
						if (image->data_chunks == NULL) {
							image->data_chunk_count = cluster_count;
							image->data_chunks = calloc(1, cluster_count * sizeof(isyntax_data_chunk_t));
						}
						if (image->number_of_blocks <= 0) { // sanity check
							goto decoding_cluster_header_table_failed;
						}
						if (image->codeblocks == NULL) {
							// NOTE: this value seems to be much larger than the actual number of codeblocks present in the file
							image->codeblock_count = image->number_of_blocks; // from UFS_IMAGE_NUMBER_OF_BLOCKS attribute
							image->codeblocks = calloc(1, image->codeblock_count * sizeof(isyntax_codeblock_t));
						}

						// pass 2: fill in all the information for each cluster
						pos = block_header_start;
						i32 running_codeblock_index = 0;
						for (i32 i = 0; i < cluster_count; ++i) {

							sequence_element = isyntax_read_dicom_tag_header(pos);
							u8* next_sequence_element_pos = pos + sizeof(isyntax_dicom_tag_header_t) + sequence_element.size;
//							dicom_tag_header_t* next_sequence_element = (dicom_tag_header_t*)next_sequence_element_pos;

							u32 cluster_block_size = sequence_element.size;
							u8* cluster_block_end = pos + sizeof(isyntax_dicom_tag_header_t) + cluster_block_size;
							if (cluster_block_end > decoded_end) {
								goto decoding_cluster_header_table_failed; // prevent out-of-bounds reading
							}

							// advance to cluster coordinates
							pos += sizeof(isyntax_dicom_tag_header_t);
							isyntax_dicom_tag_header_t element = isyntax_read_dicom_tag_header(pos);
							u8* next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end) {
								goto decoding_cluster_header_table_failed;
							}

							i32 cluster_coordinate_count = element.size / 4;
							if (cluster_coordinate_count < 2) {
								// Expect only X and Y coordinates (provides the X/Y coordinates of the cluster)
								// Scale, coefficient and color component are not needed, these can be derived from the cluster header templates
								// (even so, scale seems to be included: in the example files, cluster_coordinate_count is 3)
								goto decoding_cluster_header_table_failed;
							}
							u8* coordinates = pos + sizeof(isyntax_dicom_tag_header_t);
							i32 cluster_x = (i32)isyntax_read_u32_le(coordinates + 0);
							i32 cluster_y = (i32)isyntax_read_u32_le(coordinates + 4);

							// read cluster header template ID
							pos = next_element;
							element = isyntax_read_dicom_tag_header(pos);
							next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end || element.size != 4) {
								goto decoding_cluster_header_table_failed;
							}
							u32 cluster_header_template_id = isyntax_read_u32_le(pos + sizeof(isyntax_dicom_tag_header_t));
							if (cluster_header_template_id >= isyntax->cluster_header_template_count) {
								goto decoding_cluster_header_table_failed;
							}
							isyntax_cluster_header_template_t* cluster_header_template = isyntax->cluster_header_templates + cluster_header_template_id;
							if (cluster_coordinate_count >= 3) {
								ASSERT(cluster_header_template->base_scale == (i32)isyntax_read_u32_le(coordinates + 8));
							}

							// read cluster data offset
							pos = next_element;
							element = isyntax_read_dicom_tag_header(pos);
							next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end || element.size != 8) {
								goto decoding_cluster_header_table_failed;
							}
							u64 cluster_data_offset = isyntax_read_u64_le(pos + sizeof(isyntax_dicom_tag_header_t));

							// read cluster size
							pos = next_element;
							element = isyntax_read_dicom_tag_header(pos);
							next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end || element.size != 8) {
								goto decoding_cluster_header_table_failed;
							}
							u64 cluster_size = isyntax_read_u64_le(pos + sizeof(isyntax_dicom_tag_header_t));

							// read cluster block data offsets
							pos = next_element;
							element = isyntax_read_dicom_tag_header(pos);
							next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end) {
								goto decoding_cluster_header_table_failed;
							}
							u32 block_count = element.size / 4;
							u8* cluster_block_data_offsets = pos + sizeof(isyntax_dicom_tag_header_t);

							// read cluster block sizes
							pos = next_element;
							element = isyntax_read_dicom_tag_header(pos);
							next_element = pos + sizeof(isyntax_dicom_tag_header_t) + element.size;
							if (next_element > cluster_block_end || element.size / 4 != block_count) {
								goto decoding_cluster_header_table_failed;
							}
							u8* cluster_block_sizes = pos + sizeof(isyntax_dicom_tag_header_t);

							i32 top_codeblock_index = running_codeblock_index;
							bool has_ll = false;
							i32 highest_scale = 0;
							ASSERT(running_codeblock_index + block_count <= image->codeblock_count);
							for (i32 j = 0; j < block_count; ++j) {
								isyntax_codeblock_t* codeblock = image->codeblocks + running_codeblock_index;
								isyntax_cluster_relative_coords_t* relative_codeblock_in_cluster_info = cluster_header_template->relative_coords_for_codeblock_in_cluster + j;
								codeblock->x_coordinate = cluster_x + relative_codeblock_in_cluster_info->x;
								codeblock->y_coordinate = cluster_y + relative_codeblock_in_cluster_info->y;
								codeblock->color_component = relative_codeblock_in_cluster_info->color_component;
								codeblock->scale = relative_codeblock_in_cluster_info->scale;
								if (codeblock->scale > highest_scale) highest_scale = codeblock->scale;
								// account for different wavelet coefficient encoding in iSyntax v2 / data model >= 100
								codeblock->coefficient = (relative_codeblock_in_cluster_info->waveletcoeff == 3) ? 0 : 1;
								if (codeblock->coefficient == 0) has_ll = true;
								codeblock->block_data_offset = cluster_data_offset + isyntax_read_u32_le(cluster_block_data_offsets + j * sizeof(u32));
								codeblock->block_size = isyntax_read_u32_le(cluster_block_sizes + j * sizeof(u32));
								codeblock->block_header_template_id = relative_codeblock_in_cluster_info->block_header_template_id;
								++running_codeblock_index;
							}

							isyntax_data_chunk_t* cluster = image->data_chunks + i;
							cluster->offset = cluster_data_offset + isyntax_read_u32_le(cluster_block_data_offsets);
							cluster->size = cluster_size;
							cluster->top_codeblock_index = top_codeblock_index;
							cluster->codeblock_count_per_color = block_count / 3;
							cluster->scale = highest_scale;
							ASSERT(cluster->codeblock_count_per_color == isyntax_get_chunk_codeblocks_per_color_for_level(highest_scale, has_ll));

							pos = next_sequence_element_pos;
						}

						// TODO: prevent allocating too much memory in the first place?
						if (running_codeblock_index < image->codeblock_count) {
							// release excess allocated memory (there are fewer codeblocks present in the file than was anticipated)
							image->codeblock_count = running_codeblock_index;
							isyntax_codeblock_t* shrunk = realloc(image->codeblocks, image->codeblock_count * sizeof(isyntax_codeblock_t));
							ASSERT(shrunk);
							image->codeblocks = shrunk;
						}

						if (false) { decoding_cluster_header_table_failed:
							success = false;
						}
						free(decoded);

					} else {
						// base64 decoding failed
						success = false;
					}
				} break;
				case 0x2021: /*UFS_IMAGE_DIMENSIONS_IN_CLUSTER*/            {
					// NOTE: Philips' documentation says this should have element tag 1021, but the example files have 2021!
					isyntax_cluster_header_template_t* template = isyntax->cluster_header_templates + isyntax->parser.cluster_header_template_index;
					template->dimension_count = parse_up_to_five_integers(value, template->dimension_order);
				} break; // data model >= 100
				case 0x2023: /*UFS_IMAGE_VALID_DATA_ENVELOPES*/ {} break; // data model >= 100
				case 0x2024: /*UFS_IMAGE_OPP_EXTREME_VERTICES*/ { // data model >= 100
					// increment envelope
				} break;
				case 0x2025: /*UFS_IMAGE_OPP_EXTREME_VERTEX*/ { // data model >= 100
					if (isyntax->parser.data_object_flags & (ISYNTAX_OBJECT_UFSImageValidDataEnvelope)) {
						isyntax_valid_data_envelope_t* envelope = isyntax->valid_data_envelopes + isyntax->parser.valid_data_envelope_index;
						if (envelope->vertex_count < COUNT(envelope->vertices)) {
							v2i vertex = {};
							atoi_and_advance(atoi_and_advance(value, &vertex.x), &vertex.y); // parse two integers
							envelope->vertices[envelope->vertex_count++] = vertex;
						} else {
							success = false; // out of bounds
							console_print("Warning: cannot read all UFS_IMAGE_OPP_EXTREME_VERTEX (out of bounds)\n");
						}
					}
				} break; // data model >= 100
				case 0x2026: /*UFS_IMAGE_VALID_ENVELOPE_DIMENSIONS*/ {} break; // data model >= 100
				case 0x2027: /*UFS_IMAGE_DIMENSION_ORIGIN*/ {  // data model >= 100
					int origin = atoi(value);
					if (isyntax->parser.dimension_index == 0 /*x*/) {
						image->origin_x = origin;
					} else if (isyntax->parser.dimension_index == 1 /*y*/) {
						image->origin_y = origin;
					}
				} break;

				case 0x2029: /*UFS_IMAGE_PIXEL_TRANSFORM_METHOD*/ {} break; // data model >= 100
			}
		} break;
	}
	return success;
}

static bool validate_dicom_attr(const char* expected, const char* observed) {
	bool ok = (strcmp(expected, observed) == 0);
	if (!ok) {
		console_print("iSyntax validation error: while reading DICOM metadata, expected '%s' but found '%s'\n", expected, observed);
	}
	return ok;
}

static void isyntax_xml_parser_init(isyntax_xml_parser_t* parser) {

	parser->initialized = true;

	parser->attrbuf_capacity = KILOBYTES(32);
	parser->contentbuf_capacity = MEGABYTES(8);

	parser->attrbuf = malloc(parser->attrbuf_capacity);
	parser->attrbuf_end = parser->attrbuf + parser->attrbuf_capacity;
	parser->attrcur = NULL;
	parser->attrlen = 0;
	parser->contentbuf = malloc(parser->contentbuf_capacity);
	parser->contentcur = NULL;
	parser->contentlen = 0;
    parser->content_file_offset = 0;

	parser->current_dicom_attribute_name[0] = '\0';
	parser->current_dicom_group_tag = 0;
	parser->current_dicom_element_tag = 0;
	parser->attribute_index = 0;
	parser->current_node_type = ISYNTAX_NODE_NONE;

	// XML parsing using the yxml library.
	// https://dev.yorhel.nl/yxml/man
	size_t yxml_stack_buffer_size = KILOBYTES(32);
	parser->x = (yxml_t*) malloc(sizeof(yxml_t) + yxml_stack_buffer_size);

	yxml_init(parser->x, parser->x + 1, yxml_stack_buffer_size);
}

static const char* get_spaces(i32 length) {
	ASSERT(length >= 0);
	static const char spaces[] = "                                  ";
	i32 spaces_len = COUNT(spaces) - 1;
	i32 offset_from_end = MIN(spaces_len, length);
	i32 offset = spaces_len - offset_from_end;
	return spaces + offset;
}

static void push_to_buffer_maybe_grow(u8** restrict dest, size_t* restrict dest_len, size_t* restrict dest_capacity, void* restrict src, size_t src_len) {
	ASSERT(dest && dest_len && dest_capacity && src);
	size_t old_len = *dest_len;
	size_t new_len = old_len + src_len;
	size_t capacity = *dest_capacity;
	if (new_len > capacity) {
		capacity = next_pow2(new_len);
		u8* new_ptr = (u8*)realloc(*dest, capacity);
		if (!new_ptr) fatal_error();
		*dest = new_ptr;
		*dest_capacity = capacity;
	}
	memcpy(*dest + old_len, src, src_len);
	*dest_len = new_len;
}

static bool isyntax_parse_xml_header_cleanup(isyntax_t* isyntax, bool success) {
	isyntax_xml_parser_t* parser = &isyntax->parser;
	if (parser->x) {
		free(parser->x);
		parser->x = NULL;
	}
	if (parser->attrbuf) {
		free(parser->attrbuf);
		parser->attrbuf = NULL;
	}
	if (parser->contentbuf) {
		free(parser->contentbuf);
		parser->contentbuf = NULL;
	}
	return success;
}

static inline bool isyntax_parse_xml_header_failure(isyntax_t* isyntax) {
	return isyntax_parse_xml_header_cleanup(isyntax, false);
}

static bool isyntax_parse_xml_header(isyntax_t* isyntax, char* xml_header, i64 chunk_offset, i64 chunk_length, bool is_last_chunk) {

	yxml_t* x = NULL;
	bool success = false;

	static bool paranoid_mode = true;

	isyntax_xml_parser_t* parser = &isyntax->parser;

	if (!parser->initialized) {
		isyntax_xml_parser_init(parser);
	}
	x = parser->x;

	// parse XML byte for byte
	char* doc = xml_header;
	yxml_ret_t prev_r = YXML_OK;
	for (i64 remaining_length = chunk_length; remaining_length > 0; --remaining_length, ++doc) {
		int c = *doc;
		if (isyntax->open_flags & LIBISYNTAX_OPEN_FLAG_READ_BARCODE_ONLY && isyntax->is_barcode_read) {
			return isyntax_parse_xml_header_failure(isyntax); // abort early, return as if failed
		}
		yxml_ret_t r = yxml_parse(x, c);
		if (r == YXML_OK) {
			continue; // nothing worthy of note has happened -> continue
		} else if (r < 0) {
			// Syntax error; iSyntax file might be corrupt?
			if (r == YXML_ESYN && c == '\0') {
				// Zeroes are not expected to occur and would tend to suggest a corrupted file.
				// However, some iSyntax files apparently have a sequence of zeroes between the end of the XML header
				// and the End of Table (EOT) marker (3 bytes "\r\n\x04"). We need to allow for this situation.
				if (prev_r == YXML_ELEMEND && parser->node_stack_index == 0) {
					success = true;
					return isyntax_parse_xml_header_cleanup(isyntax, success);
				}
			}
			return isyntax_parse_xml_header_failure(isyntax);
		} else if (r > 0) {
			// token
			switch(r) {
				case YXML_ELEMSTART: {
					// start of an element: '<Tag ..'
					isyntax_parser_node_t* parent_node = parser->node_stack + parser->node_stack_index;
					++parser->node_stack_index;
					isyntax_parser_node_t* node = parser->node_stack + parser->node_stack_index;
					memset(node, 0, sizeof(isyntax_parser_node_t));
					// Inherit group and element of parent node
					node->group = parent_node->group;
					node->element = parent_node->element;

					parser->contentcur = parser->contentbuf;
					*parser->contentcur = '\0';
					parser->contentlen = 0;
                    parser->content_file_offset = 0;
					parser->attribute_index = 0;
					if (strcmp(x->elem, "Attribute") == 0) {
						node->node_type = ISYNTAX_NODE_LEAF;
					} else if (strcmp(x->elem, "DataObject") == 0) {
						node->node_type = ISYNTAX_NODE_BRANCH;
						// push into the data object stack, to keep track of which type of DataObject we are parsing
						// (we need this information to restore state when the XML element ends)
						++parser->data_object_stack_index;
						parser->data_object_stack[parser->data_object_stack_index] = *parent_node;
						// set relevant flag for which data object type we are now parsing
						// NOTE: the data objects can have different DICOM groups, but currently there are no element
						// ID collisions so we can switch on only the element ID. This may change in the future.
						u32 flags = parser->data_object_flags;
						switch(parent_node->element) {
							default: break;
							case 0:                                       flags |= ISYNTAX_OBJECT_DPUfsImport; break;
							case PIM_DP_SCANNED_IMAGES:                   flags |= ISYNTAX_OBJECT_DPScannedImage; break;
							case UFS_IMAGE_GENERAL_HEADERS:               flags |= ISYNTAX_OBJECT_UFSImageGeneralHeader; break;
							case UFS_IMAGE_BLOCK_HEADER_TEMPLATES: {
								flags |= ISYNTAX_OBJECT_UFSImageBlockHeaderTemplate;
								if (isyntax->parser.block_header_template_index >= COUNT(isyntax->block_header_templates)) { // bounds check
									return isyntax_parse_xml_header_failure(isyntax);
								}
							} break;
							case UFS_IMAGE_DIMENSIONS:                    flags |= ISYNTAX_OBJECT_UFSImageDimension; break;
							case UFS_IMAGE_DIMENSION_RANGES:              flags |= ISYNTAX_OBJECT_UFSImageDimensionRange; break;
							case DP_COLOR_MANAGEMENT:                     flags |= ISYNTAX_OBJECT_DPColorManagement; break;
							case DP_IMAGE_POST_PROCESSING:                flags |= ISYNTAX_OBJECT_DPImagePostProcessing; break;
							case DP_WAVELET_QUANTIZER_SETTINGS_PER_COLOR: flags |= ISYNTAX_OBJECT_DPWaveletQuantizerSeetingsPerColor; break;
							case DP_WAVELET_QUANTIZER_SETTINGS_PER_LEVEL: flags |= ISYNTAX_OBJECT_DPWaveletQuantizerSeetingsPerLevel; break;
							case PIIM_PIXEL_DATA_REPRESENTATION_SEQUENCE: flags |= ISYNTAX_OBJECT_PixelDataRepresentation; break;
							case UFS_IMAGE_BLOCK_HEADERS:                 flags |= ISYNTAX_OBJECT_UFSImageBlockHeader; break;
							case UFS_IMAGE_CLUSTER_HEADER_TEMPLATES: {
								flags |= ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate;
								if (isyntax->parser.cluster_header_template_index >= COUNT(isyntax->cluster_header_templates)) { // bounds check
									return isyntax_parse_xml_header_failure(isyntax);
								}
							} break;
							case UFS_IMAGE_VALID_DATA_ENVELOPES: {
								// 'soft' bounds check: parsing these is not strictly needed so we don't need to fail if we don't get them all.
								if (isyntax->parser.valid_data_envelope_index < COUNT(isyntax->valid_data_envelopes)) {
									flags |= ISYNTAX_OBJECT_UFSImageValidDataEnvelope;
								} else {
									console_print("Warning: cannot read all UFSImageValidDataEnvelopes (out of bounds)\n");
								}
							} break;
							case UFS_IMAGE_OPP_EXTREME_VERTICES:          flags |= ISYNTAX_OBJECT_UFSImageOppExtremeVertex; break;
						}
						parser->data_object_flags = flags;
					} else if (strcmp(x->elem, "Array") == 0) {
						node->node_type = ISYNTAX_NODE_ARRAY;
						console_print_verbose("%sArray\n", get_spaces(parser->node_stack_index));
					} else {
						node->node_type = ISYNTAX_NODE_NONE;
						console_print_verbose("%selement start: %s\n", get_spaces(parser->node_stack_index), x->elem);
					}
					parser->current_node_type = node->node_type;
					parser->current_node_has_children = false;

				} break;

				case YXML_CONTENT: {
					// element content
					if (!parser->contentcur) break;

                    // Remember the file offset of the element content
                    if (parser->content_file_offset == 0) {
                        parser->content_file_offset = chunk_offset + (doc - xml_header);
                    }

					// Load iSyntax block header table (and other large XML tags) greedily and bypass yxml parsing overhead
					if (parser->current_node_type == ISYNTAX_NODE_LEAF) {
						u32 group = parser->current_dicom_group_tag;
						u32 element = parser->current_dicom_element_tag;
						isyntax_parser_node_t* node = parser->node_stack + parser->node_stack_index;
						node->group = group;
						node->element = element;
						bool need_skip = (group == 0x301D && element == 0x2014) || // UFS_IMAGE_BLOCK_HEADER_TABLE
								         (group == 0x301D && element == 0x1005) || // PIM_DP_IMAGE_DATA
										 (group == 0x0028 && element == 0x2000);   // DICOM_ICCPROFILE

					    if (need_skip) {
					    	parser->node_stack[parser->node_stack_index].has_base64_content = true;
							char* content_start = doc;
							char* pos = (char*)memchr(content_start, '<', remaining_length);
							if (pos) {
								i64 size = pos - content_start;
								push_to_buffer_maybe_grow((u8**)&parser->contentbuf, &parser->contentlen, &parser->contentbuf_capacity, content_start, size);
								parser->contentcur = parser->contentbuf + parser->contentlen;
//								console_print("iSyntax: skipped tag (0x%04x, 0x%04x) content length = %d\n", group, element, size);
								doc += (size-1); // skip to the next tag
								remaining_length -= (size-1);
								break;
							} else {
//								console_print("iSyntax: skipped tag (0x%04x, 0x%04x) content length = %d\n", group, element, remaining_length);
								push_to_buffer_maybe_grow((u8**)&parser->contentbuf, &parser->contentlen, &parser->contentbuf_capacity, content_start, remaining_length);
								parser->contentcur = parser->contentbuf + parser->contentlen;
								remaining_length = 0; // skip to the next chunk
								break;
							}
						}
					}

					char* tmp = x->data;
					while (*tmp && parser->contentlen < parser->contentbuf_capacity) {
						*(parser->contentcur++) = *(tmp++);
						++parser->contentlen;
						// too long content -> resize buffer
						if (parser->contentlen == parser->contentbuf_capacity) {
							size_t new_capacity = parser->contentbuf_capacity * 2;
							char* new_ptr = (char*)realloc(parser->contentbuf, new_capacity);
							if (!new_ptr) fatal_error();
							parser->contentbuf = new_ptr;
							parser->contentcur = parser->contentbuf + parser->contentlen;
							parser->contentbuf_capacity = new_capacity;
//							console_print("isyntax_parse_xml_header(): XML content buffer overflow (resized buffer to %u)\n", new_capacity);
						}
					}

					*parser->contentcur = '\0';
				} break;

				case YXML_ELEMEND: {
					// end of an element: '.. />' or '</Tag>'

					if (parser->current_node_type == ISYNTAX_NODE_LEAF && !parser->current_node_has_children) {
						// Leaf node WITHOUT children.
						// In this case we didn't already parse the attributes at the YXML_ATTREND stage.
						// Now at the YXML_ELEMEND stage we can parse the complete tag at once (attributes + content).
						console_print_verbose("%sDICOM: %-40s (0x%04x, 0x%04x), size:%-8zu = %s\n", get_spaces(parser->node_stack_index),
						                      parser->current_dicom_attribute_name,
						                      parser->current_dicom_group_tag, parser->current_dicom_element_tag,
											  parser->contentlen, parser->contentbuf);

//						if (parser->node_stack[parser->node_stack_index].group == 0) {
//							DUMMY_STATEMENT; // probably the group is 0 because this is a top level node.
//						}

						if (parser->node_stack_index == 2) {
							isyntax_parse_ufsimport_child_node(isyntax, parser->current_dicom_group_tag,
															   parser->current_dicom_element_tag,
															   parser->contentbuf, parser->contentlen);
						} else {
							isyntax_parse_scannedimage_child_node(isyntax, parser->current_dicom_group_tag,
																  parser->current_dicom_element_tag,
																  parser->contentbuf, parser->contentlen);
						}
					} else {
						// We have reached the end of a branch or array node, or a leaf node WITH children.
						// In this case, the attributes have already been parsed at the YXML_ATTREND stage.
						// Because their content is not text but child nodes, we do not need to touch the content buffer.
						const char* elem_name = NULL;
						if (parser->current_node_type == ISYNTAX_NODE_LEAF) {
							// End of a leaf node WITH children.
							elem_name = "Attribute";
						} else if (parser->current_node_type == ISYNTAX_NODE_BRANCH) {
							elem_name = "DataObject";
							// pop data object stack
							isyntax_parser_node_t data_object = parser->data_object_stack[parser->data_object_stack_index];
							--parser->data_object_stack_index;
							// reset relevant data for the data object type we are now no longer parsing
							u32 flags = parser->data_object_flags;
							switch(data_object.element) {
								default: break;
								case 0:                                       flags &= ~ISYNTAX_OBJECT_DPUfsImport; break;
								case PIM_DP_SCANNED_IMAGES: {
									flags &= ~ISYNTAX_OBJECT_DPScannedImage;
								} break;
								case UFS_IMAGE_GENERAL_HEADERS: {
									flags &= ~ISYNTAX_OBJECT_UFSImageGeneralHeader;
									parser->dimension_index = 0;
								} break;
								case UFS_IMAGE_BLOCK_HEADER_TEMPLATES: {
									flags &= ~ISYNTAX_OBJECT_UFSImageBlockHeaderTemplate;
									++parser->block_header_template_index;
									if (isyntax->block_header_template_count < COUNT(isyntax->block_header_templates)) {
										++isyntax->block_header_template_count;
									}
									parser->dimension_index = 0;
								} break;
								case UFS_IMAGE_DIMENSIONS: {
									flags &= ~ISYNTAX_OBJECT_UFSImageDimension;
									++parser->dimension_index;
								} break;
								case UFS_IMAGE_DIMENSION_RANGES: {
									flags &= ~ISYNTAX_OBJECT_UFSImageDimensionRange;
									++parser->dimension_index;
								} break;
								case DP_COLOR_MANAGEMENT:                     flags &= ~ISYNTAX_OBJECT_DPColorManagement; break;
								case DP_IMAGE_POST_PROCESSING:                flags &= ~ISYNTAX_OBJECT_DPImagePostProcessing; break;
								case DP_WAVELET_QUANTIZER_SETTINGS_PER_COLOR: flags &= ~ISYNTAX_OBJECT_DPWaveletQuantizerSeetingsPerColor; break;
								case DP_WAVELET_QUANTIZER_SETTINGS_PER_LEVEL: flags &= ~ISYNTAX_OBJECT_DPWaveletQuantizerSeetingsPerLevel; break;
								case UFS_IMAGE_BLOCK_HEADERS: {
									flags &= ~ISYNTAX_OBJECT_UFSImageBlockHeader;
									if (flags & ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate) {
										// NOTE: Within a UFSImageClusterHeaderTemplate, the UFSImageBlockHeader objects contain coordinate offset values that apply to that cluster template
										// (Each UFSImageBlockHeader object corresponds to a codeblock in the cluster)
										++parser->block_header_index_for_cluster;
										if (parser->block_header_index_for_cluster >= MAX_CODEBLOCKS_PER_CLUSTER) { // bounds check
											return isyntax_parse_xml_header_failure(isyntax);
										}
									}
								} break;
								case UFS_IMAGE_CLUSTER_HEADER_TEMPLATES: {
									// Finalize cluster header template object: fix up relative codeblock coordinates within cluster
									// using the information from UFS_IMAGE_DIMENSION_RANGES (=base value)
									// combined with the UFS_IMAGE_BLOCK_COORDINATE values (=offsets) using the dimension ordering info provided by UFS_IMAGE_DIMENSIONS_IN_CLUSTER
									isyntax_cluster_header_template_t* template = isyntax->cluster_header_templates + parser->cluster_header_template_index;
									template->codeblock_in_cluster_count = parser->block_header_index_for_cluster;
									for (i32 i = 0; i < template->codeblock_in_cluster_count; ++i) {
										isyntax_cluster_relative_coords_t* relative = template->relative_coords_for_codeblock_in_cluster + i;
										// apply base values from UFS_IMAGE_DIMENSION_RANGES
										relative->x = template->base_x;
										relative->y = template->base_y;
										relative->color_component = template->base_color_component;
										relative->scale = template->base_scale;
										relative->waveletcoeff = template->base_waveletcoeff;
										// apply offset from UFS_IMAGE_BLOCK_COORDINATE
										u32* dimensions_to_fix[5] = {&relative->x, &relative->y, &relative->color_component, &relative->scale, &relative->waveletcoeff};
										for (i32 dimension_index = 0; dimension_index < template->dimension_count; ++dimension_index) {
											u32* to_fix = dimensions_to_fix[template->dimension_order[dimension_index]]; // get correct dimension from UFS_IMAGE_DIMENSIONS_IN_CLUSTER
											*to_fix += relative->raw_coords[dimension_index]; // apply offset
										}
									}
									// Pop flags and reset indices
									flags &= ~ISYNTAX_OBJECT_UFSImageClusterHeaderTemplate;
									++parser->cluster_header_template_index;
									if (isyntax->cluster_header_template_count < COUNT(isyntax->cluster_header_templates)) {
										++isyntax->cluster_header_template_count;
									}
									parser->dimension_index = 0;
									parser->block_header_index_for_cluster = 0;
								} break;
								case UFS_IMAGE_VALID_DATA_ENVELOPES: {
									flags &= ~ISYNTAX_OBJECT_UFSImageValidDataEnvelope;
									++parser->valid_data_envelope_index;
									if (isyntax->valid_data_envelope_count < COUNT(isyntax->valid_data_envelopes)) {
										++isyntax->valid_data_envelope_count;
									}
								} break;
								case UFS_IMAGE_OPP_EXTREME_VERTICES:          flags &= ~ISYNTAX_OBJECT_UFSImageOppExtremeVertex; break;
							}
							parser->data_object_flags = flags;
						} else if (parser->current_node_type == ISYNTAX_NODE_ARRAY) {
							parser->dimension_index = 0;
							elem_name = "Array";
						}

						console_print_verbose("%selement end: %s\n", get_spaces(parser->node_stack_index), elem_name);
					}

					// 'Pop' context back to parent node
					if (parser->node_stack_index > 0) {
						--parser->node_stack_index;
						parser->current_node_type = parser->node_stack[parser->node_stack_index].node_type;
						parser->current_node_has_children = parser->node_stack[parser->node_stack_index].has_children;
					} else {
						console_print_error("iSyntax XML error: closing element without matching start\n");
						return isyntax_parse_xml_header_failure(isyntax);
					}

				} break;

				case YXML_ATTRSTART: {
					// attribute: 'Name=..'
//				    console_print_verbose("attr start: %s\n", x->attr);
					parser->attrcur = parser->attrbuf;
					*parser->attrcur = '\0';
					parser->attrlen = 0;
				} break;

				case YXML_ATTRVAL: {
					// attribute value
				    //console_print_verbose("   attr val: %s\n", x->attr);
					if (!parser->attrcur) break;
					char* tmp = x->data;
					while (*tmp && parser->attrbuf < parser->attrbuf_end) {
						*(parser->attrcur++) = *(tmp++);
						++parser->attrlen;
						// too long content -> resize buffer
						if (parser->attrlen == parser->attrbuf_capacity) {
							size_t new_capacity = parser->attrbuf_capacity * 2;
							char* new_ptr = (char*)realloc(parser->attrbuf, new_capacity);
							if (!new_ptr) fatal_error();
							parser->attrbuf = new_ptr;
							parser->attrcur = parser->attrbuf + parser->attrlen;
							parser->attrbuf_capacity = new_capacity;
//							console_print("isyntax_parse_xml_header(): XML attribute buffer overflow (resized buffer to %u)\n", new_capacity);
						}
					}
					*parser->attrcur = '\0';
				} break;

				case YXML_ATTREND: {
					// end of attribute '.."'
					if (parser->attrcur) {
						ASSERT(strlen(parser->attrbuf) == parser->attrlen);

						if (parser->current_node_type == ISYNTAX_NODE_LEAF) {
							if (parser->attribute_index == 0 /* Name="..." */) {
								if (paranoid_mode) validate_dicom_attr(x->attr, "Name");
								size_t copy_size = MIN(parser->attrlen, sizeof(parser->current_dicom_attribute_name));
								memcpy(parser->current_dicom_attribute_name, parser->attrbuf, copy_size);
								i32 one_past_last_char = MIN(parser->attrlen, sizeof(parser->current_dicom_attribute_name)-1);
								parser->current_dicom_attribute_name[one_past_last_char] = '\0';
							} else if (parser->attribute_index == 1 /* Group="0x...." */) {
								if (paranoid_mode) validate_dicom_attr(x->attr, "Group");
								parser->current_dicom_group_tag = strtoul(parser->attrbuf, NULL, 0);
							} else if (parser->attribute_index == 2 /* Element="0x...." */) {
								if (paranoid_mode) validate_dicom_attr(x->attr, "Element");
								parser->current_dicom_element_tag = strtoul(parser->attrbuf, NULL, 0);
							} else if (parser->attribute_index == 3 /* PMSVR="..." */) {
								if (paranoid_mode) validate_dicom_attr(x->attr, "PMSVR");
								if (strcmp(parser->attrbuf, "IDataObjectArray") == 0) {
									// Leaf node WITH children.
									// Don't wait until YXML_ELEMEND to parse the attributes (this is the only opportunity we have!)
									parser->current_node_has_children = true;
									parser->node_stack[parser->node_stack_index].has_children = true;
									console_print_verbose("%sDICOM: %-40s (0x%04x, 0x%04x), array\n", get_spaces(parser->node_stack_index),
							                              parser->current_dicom_attribute_name,
									                      parser->current_dicom_group_tag, parser->current_dicom_element_tag);
									if (parser->node_stack_index == 2) { // At level of UfsImport
										isyntax_parse_ufsimport_child_node(isyntax, parser->current_dicom_group_tag,
																		   parser->current_dicom_element_tag,
																		   parser->contentbuf, parser->contentlen);
									} else {
										bool parse_ok = isyntax_parse_scannedimage_child_node(isyntax,
																			  parser->current_dicom_group_tag,
																			  parser->current_dicom_element_tag,
																			  parser->contentbuf, parser->contentlen);
									}

								}
							}
						} else if (parser->current_node_type == ISYNTAX_NODE_BRANCH) {
							// A DataObject node is supposed to have one attribute "ObjectType"
							ASSERT(parser->attribute_index == 0);
							ASSERT(strcmp(x->attr, "ObjectType") == 0);
							console_print_verbose("%sDataObject %s = %s\n", get_spaces(parser->node_stack_index), x->attr, parser->attrbuf);
							if (strcmp(parser->attrbuf, "DPScannedImage") == 0) {
								// We started parsing a new image (which will be either a WSI, LABELIMAGE or MACROIMAGE).
								parser->current_image = isyntax->images + isyntax->image_count;
								parser->running_image_index = isyntax->image_count++;
							}
						} else {
							console_print_verbose("%sattr %s = %s\n", get_spaces(parser->node_stack_index), x->attr, parser->attrbuf);
						}
						++parser->attribute_index;

					}
				} break;

				case YXML_PISTART:
				case YXML_PICONTENT:
				case YXML_PIEND:
					break; // processing instructions (uninteresting, skip)
				default: {
					console_print_error("yxml_parse(): unrecognized token (%d)\n", r);
					return isyntax_parse_xml_header_failure(isyntax);
				}
			}
		}
		prev_r = r;
	}

	success = true;
	if (is_last_chunk) {
		return isyntax_parse_xml_header_cleanup(isyntax, success);
	} else {
		return success; // no cleanup yet, we will still need resources until the last header chunk is reached.
	}
}


// Convert between signed magnitude and two's complement
// https://stackoverflow.com/questions/21837008/how-to-convert-from-sign-magnitude-to-twos-complement
// N.B. This function is its own inverse (conversion works the other way as well)
static inline i16 signed_magnitude_to_twos_complement_16(u16 x) {
	u16 m = -(x >> 15);
	i16 result = (~m & x) | (((x & (u16)0x8000) - x) & m);
	return result;
}

static inline i32 twos_complement_to_signed_magnitude(u32 x) {
	u32 m = -(x >> 31);
	i32 result = (~m & x) | (((x & 0x80000000) - x) & m);
	return result;
}

static void signed_magnitude_to_twos_complement_16_block(u16* data, u32 len) {
    u32 aligned_len = (len / 8) * 8;
    u32 i = 0;
#if defined(__SSE2__)
	// Fast x86 SIMD version
	for (; i < aligned_len; i += 8) {
		__m128i x = _mm_loadu_si128((__m128i*)(data + i));
		__m128i sign_masks = _mm_srai_epi16(x, 15); // 0x0000 if positive, 0xFFFF if negative
		__m128i maybe_positive = _mm_andnot_si128(sign_masks, x); // (~m & x)
		__m128i value_if_negative = _mm_sub_epi16(_mm_and_si128(x, _mm_set1_epi16(0x8000)), x); // (x & 0x8000) - x
		__m128i maybe_negative = _mm_and_si128(sign_masks, value_if_negative);
		__m128i result = _mm_or_si128(maybe_positive, maybe_negative);
		_mm_storeu_si128((__m128i*)(data + i), result);
	}
#elif defined(__ARM_NEON)
    // NEON version for ARM processors
    for (; i < aligned_len; i += 8) {
        uint16x8_t x = vld1q_u16(data + i);
        int16x8_t sign_masks = vshrq_n_s16((int16x8_t)x, 15);
        uint16x8_t maybe_positive = vbicq_u16(x, (uint16x8_t)sign_masks);
        uint16x8_t value_if_negative = vsubq_u16(vandq_u16(x, vdupq_n_u16(0x8000)), x);
        uint16x8_t maybe_negative = vandq_u16((uint16x8_t)sign_masks, value_if_negative);
        uint16x8_t result = vorrq_u16(maybe_positive, maybe_negative);
        vst1q_u16(data + i, result);
    }
#endif
    // Slow version, for last unaligned elements or in case SIMD isn't available
	for (; i < len; ++i) {
		data[i] = signed_magnitude_to_twos_complement_16(data[i]);
	}
	ASSERT(i == len);
}

// Convert a block of 16-bit signed integers to their absolute value
// Almost the same as the signed magnitude <-> twos complement conversion, except the sign bit is cleared at the end
static void signed_magnitude_to_absolute_value_16_block(i16* data, u32 len) {
    u32 aligned_len = (len / 8) * 8;
    u32 i = 0;
#if defined(__SSE2__)
	// Fast x86 SIMD version
	for (; i < aligned_len; i += 8) {
		__m128i x = _mm_loadu_si128((__m128i*)(data + i));
		__m128i sign_masks = _mm_srai_epi16(x, 15); // 0x0000 if positive, 0xFFFF if negative
		__m128i maybe_positive = _mm_andnot_si128(sign_masks, x); // (~m & x)
		__m128i value_if_negative = _mm_sub_epi16(_mm_and_si128(x, _mm_set1_epi16(0x8000)), x); // (x & 0x8000) - x
		__m128i maybe_negative = _mm_and_si128(sign_masks, value_if_negative);
		__m128i result = _mm_or_si128(maybe_positive, maybe_negative);
		result = _mm_and_si128(result, _mm_set1_epi16(0x7FFF)); // x &= 0x7FFF (clear sign bit)
		_mm_storeu_si128((__m128i*)(data + i), result);
	}
#elif defined(__ARM_NEON)
    // NEON version for ARM processors
    for (; i < aligned_len; i += 8) {
        uint16x8_t x = vld1q_u16((u16*)data + i);
        int16x8_t sign_masks = vshrq_n_s16((int16x8_t)x, 15);
        uint16x8_t maybe_positive = vbicq_u16(x, (uint16x8_t)sign_masks);
        uint16x8_t value_if_negative = vsubq_u16(vandq_u16(x, vdupq_n_u16(0x8000)), x);
        uint16x8_t maybe_negative = vandq_u16((uint16x8_t)sign_masks, value_if_negative);
        uint16x8_t result = vorrq_u16(maybe_positive, maybe_negative);
        result = vbicq_u16(result, vdupq_n_u16(0x8000)); // clear sign bit
        vst1q_u16((u16*)data + i, result);
    }
#endif
    // Slow version, for last unaligned elements or in case SIMD isn't available
	for (; i < len; ++i) {
		data[i] = (i16)(signed_magnitude_to_twos_complement_16((u16)data[i]) & 0x7FFF);
	}
	ASSERT(i == len);
}


#if ISYNTAX_WANT_DEBUG_OUTPUT_PNG
void debug_convert_wavelet_coefficients_to_image2(icoeff_t* coefficients, i32 width, i32 height, const char* filename) {
	if (coefficients) {
		u8* decoded_8bit = (u8*)malloc(width*height);
		for (i32 i = 0; i < width * height; ++i) {
			u16 magnitude = 0x7FFF & (u16)twos_complement_to_signed_magnitude(coefficients[i]);
			decoded_8bit[i] = ATMOST(255, magnitude);
		}

		stbi_write_png(filename, width, height, 1, decoded_8bit, width);
		free(decoded_8bit);
	}
}
#endif

#if (DWT_COEFF_BITS==16)
static u32 wavelet_coefficient_to_color_value(icoeff_t coefficient) {
	u32 magnitude = ((u32)signed_magnitude_to_twos_complement_16(coefficient) & ~0x8000);
	return magnitude;
}
#else
static u32 wavelet_coefficient_to_color_value(icoeff_t coefficient) {
	u32 magnitude = ((u32)twos_complement_to_signed_magnitude(coefficient) & ~0x80000000);
	return magnitude;
}
#endif

static rgba_t ycocg_to_rgb_8bit(icoeff_t Y, icoeff_t Co, icoeff_t Cg) {
    icoeff_t tmp = Y - Cg/2;
    icoeff_t G = tmp + Cg;
    icoeff_t B = tmp - Co/2;
    icoeff_t R = B + Co;

	B = (B < 0 ? 0 : B);
	G = (G < 0 ? 0 : G);
	R = (R < 0 ? 0 : R);

    return (rgba_t){{{ATMOST(255, R), ATMOST(255, G), ATMOST(255, B), 255}}};
}

static rgba_t ycocg_to_bgr_8bit(icoeff_t Y, icoeff_t Co, icoeff_t Cg) {
    icoeff_t tmp = Y - Cg/2;
    icoeff_t G = tmp + Cg;
    icoeff_t B = tmp - Co/2;
    icoeff_t R = B + Co;

	B = (B < 0 ? 0 : B);
	G = (G < 0 ? 0 : G);
	R = (R < 0 ? 0 : R);

    return (rgba_t){{{ATMOST(255, B), ATMOST(255, G), ATMOST(255, R), 255}}};
}

static rgba_t ycocg_to_rgb_9bit(icoeff_t Y, icoeff_t Co, icoeff_t Cg) {
    icoeff_t tmp = Y - Cg/2;
    icoeff_t G = tmp + Cg;
    icoeff_t B = tmp - Co/2;
    icoeff_t R = B + Co;

	G = G >> 1;
	B = B >> 1;
	R = R >> 1;

	B = (B < 0 ? 0 : B);
	G = (G < 0 ? 0 : G);
	R = (R < 0 ? 0 : R);

    return (rgba_t){{{ATMOST(255, R), ATMOST(255, G), ATMOST(255, B), 255}}};
}

static rgba_t ycocg_to_bgr_9bit(icoeff_t Y, icoeff_t Co, icoeff_t Cg) {
    icoeff_t tmp = Y - Cg/2;
    icoeff_t G = tmp + Cg;
    icoeff_t B = tmp - Co/2;
    icoeff_t R = B + Co;
	
	// iSyntax v2 measures the YCoCg data in a 9 bit resolution, compared to iSyntax v1 8 bit resolution (both stored as 16 bit integers)
	// so we need to halve the resolution by bitshifting
	B = B >> 1;
	G = G >> 1;
	R = R >> 1;

	// sometimes, especially in lower magnifications the YCoCg -> RGB conversion leads to invalid negative color values (I guess the wavelet transform is not 100% correct)
	// these values are small but when casting to u8 become significant (-2 -> 255 + -2 = 253)
	// the SIMD _mm_packus_epi16 uses a unsignedsaturate that doesnt have this behavior (<0 -> 0), copy this behavior:
	B = (B < 0 ? 0 : B);
	G = (G < 0 ? 0 : G);
	R = (R < 0 ? 0 : R);
	
    return (rgba_t){{{ATMOST(255, B), ATMOST(255, G), ATMOST(255, R), 255}}};
}

static void convert_ycocg_to_bgra_block_8bit(icoeff_t* Y, icoeff_t* Co, icoeff_t* Cg, i32 width, i32 height, i32 stride, u32* out_bgra) {
    i32 aligned_width = (width / 8) * 8;

	for (i32 y = 0; y < height; ++y) {
		u32* dest = out_bgra + (y * width);
        i32 i = 0;
#if defined(__SSE2__) && defined(__SSSE3__)
		// Fast SIMD version (~2x faster on my system)
		for (; i < aligned_width; i += 8) {
			// Do the color space conversion
			__m128i Y_ = _mm_loadu_si128((__m128i*)(Y + i));
			__m128i Co_ = _mm_loadu_si128((__m128i*)(Co + i));
			__m128i Cg_ = _mm_loadu_si128((__m128i*)(Cg + i));
			__m128i tmp = _mm_sub_epi16(Y_, _mm_srai_epi16(Cg_, 1)); // tmp = Y - Cg/2
			__m128i G = _mm_add_epi16(tmp, Cg_);                     // G = tmp + Cg
			__m128i B = _mm_sub_epi16(tmp, _mm_srai_epi16(Co_, 1));  // B = tmp - Co/2
			__m128i R = _mm_add_epi16(B, Co_);                       // R = B + Co

			// Clamp range to 0..255
			__m128i zero = _mm_set1_epi16(0);
			R = _mm_packus_epi16(R, zero); // -R-R-R-R -> RRRR----
			G = _mm_packus_epi16(zero, G); // -G-G-G-G -> ----GGGG
			B = _mm_packus_epi16(B, zero); // -B-B-B-B -> BBBB----

			__m128i A = _mm_setr_epi32(0, 0, 0xffffffff, 0xffffffff); // ----AAAA

			// Shuffle into the right order -> BGRA
			__m128i BG = _mm_or_si128(B, G);
			__m128i RA = _mm_or_si128(R, A);

			__m128i v_perm = _mm_setr_epi8(0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15);
			BG = _mm_shuffle_epi8(BG, v_perm); // BGBGBGBG
			RA = _mm_shuffle_epi8(RA, v_perm); // RARARARA
			__m128i lo = _mm_unpacklo_epi16(BG, RA); // BGRA
			__m128i hi = _mm_unpackhi_epi16(BG, RA);

			_mm_storeu_si128((__m128i*)(dest + i), lo);
			_mm_storeu_si128((__m128i*)(dest + i + 4), hi);
		}
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
        // Fast SIMD version for ARM NEON
        for (; i < aligned_width; i += 8) {
            int16x8_t Y_ = vld1q_s16(Y + i);
            int16x8_t Co_ = vld1q_s16(Co + i);
            int16x8_t Cg_ = vld1q_s16(Cg + i);
            int16x8_t tmp = vsubq_s16(Y_, vshrq_n_s16(Cg_, 1));
            int16x8_t G = vaddq_s16(tmp, Cg_);
            int16x8_t B = vsubq_s16(tmp, vshrq_n_s16(Co_, 1));
            int16x8_t R = vaddq_s16(B, Co_);

            uint8x8x4_t bgra_vec;
            bgra_vec.val[2] = vqmovun_s16(R);
            bgra_vec.val[1] = vqmovun_s16(G);
            bgra_vec.val[0] = vqmovun_s16(B);
            bgra_vec.val[3] = vdup_n_u8(0xFF);

            vst4_u8((uint8_t*)(dest + i), bgra_vec);
        }
#endif
        // Slow version, for last unaligned elements or in case SIMD isn't available
		for (; i < width; ++i) {
			((rgba_t*)dest)[i] = ycocg_to_bgr_8bit(Y[i], Co[i], Cg[i]);
		}

		Y += stride;
		Co += stride;
		Cg += stride;
	}
}

static void convert_ycocg_to_rgba_block_8bit(icoeff_t* Y, icoeff_t* Co, icoeff_t* Cg, i32 width, i32 height, i32 stride, u32* out_rgba) {
    i32 aligned_width = (width / 8) * 8;

    for (i32 y = 0; y < height; ++y) {
        u32* dest = out_rgba + (y * width);
        i32 i = 0;
#if defined(__SSE2__) && defined(__SSSE3__)
        // Fast SIMD version (~2x faster on my system)
		for (; i < aligned_width; i += 8) {
			// Do the color space conversion
			__m128i Y_ = _mm_loadu_si128((__m128i*)(Y + i));
			__m128i Co_ = _mm_loadu_si128((__m128i*)(Co + i));
			__m128i Cg_ = _mm_loadu_si128((__m128i*)(Cg + i));
			__m128i tmp = _mm_sub_epi16(Y_, _mm_srai_epi16(Cg_, 1)); // tmp = Y - Cg/2
			__m128i G = _mm_add_epi16(tmp, Cg_);                     // G = tmp + Cg
			__m128i B = _mm_sub_epi16(tmp, _mm_srai_epi16(Co_, 1));  // B = tmp - Co/2
			__m128i R = _mm_add_epi16(B, Co_);                       // R = B + Co

			// Clamp range to 0..255
			__m128i zero = _mm_set1_epi16(0);
			R = _mm_packus_epi16(R, zero); // -R-R-R-R -> RRRR----
			G = _mm_packus_epi16(zero, G); // -G-G-G-G -> ----GGGG
			B = _mm_packus_epi16(B, zero); // -B-B-B-B -> BBBB----

			__m128i A = _mm_setr_epi32(0, 0, 0xffffffff, 0xffffffff); // ----AAAA

			// Shuffle into the right order -> RGBA
			__m128i RG = _mm_or_si128(R, G);
			__m128i BA = _mm_or_si128(B, A);

			__m128i v_perm = _mm_setr_epi8(0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15);
			RG = _mm_shuffle_epi8(RG, v_perm); // RGRGRGRG
			BA = _mm_shuffle_epi8(BA, v_perm); // BABABABA
			__m128i lo = _mm_unpacklo_epi16(RG, BA); // RGBA
			__m128i hi = _mm_unpackhi_epi16(RG, BA);

			_mm_storeu_si128((__m128i*)(dest + i), lo);
			_mm_storeu_si128((__m128i*)(dest + i + 4), hi);
		}
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
        // Fast SIMD version for ARM NEON
        for (; i < aligned_width; i += 8) {
            int16x8_t Y_ = vld1q_s16(Y + i);
            int16x8_t Co_ = vld1q_s16(Co + i);
            int16x8_t Cg_ = vld1q_s16(Cg + i);
            int16x8_t tmp = vsubq_s16(Y_, vshrq_n_s16(Cg_, 1));
            int16x8_t G = vaddq_s16(tmp, Cg_);
            int16x8_t B = vsubq_s16(tmp, vshrq_n_s16(Co_, 1));
            int16x8_t R = vaddq_s16(B, Co_);

            uint8x8x4_t rgba_vec;
            rgba_vec.val[0] = vqmovun_s16(R);
            rgba_vec.val[1] = vqmovun_s16(G);
            rgba_vec.val[2] = vqmovun_s16(B);
            rgba_vec.val[3] = vdup_n_u8(0xFF);

            vst4_u8((uint8_t*)(dest + i), rgba_vec);
        }
#endif
        // Slow version, for last unaligned elements or in case SIMD isn't available
        for (; i < width; ++i) {
            ((rgba_t*)dest)[i] = ycocg_to_rgb_8bit(Y[i], Co[i], Cg[i]);
        }

        Y += stride;
        Co += stride;
        Cg += stride;
    }
}

static void convert_ycocg_to_bgra_block_9bit(icoeff_t* Y, icoeff_t* Co, icoeff_t* Cg, i32 width, i32 height, i32 stride, u32* out_bgra) {
    i32 aligned_width = (width / 8) * 8;

	for (i32 y = 0; y < height; ++y) {
		u32* dest = out_bgra + (y * width);
        i32 i = 0;
#if defined(__SSE2__) && defined(__SSSE3__)
		//console_print_error("v2 - SIMD");
		// Fast SIMD version (2x faster on my system)
		for (; i < aligned_width; i += 8) {
			// Do the color space conversion
			__m128i Y_ = _mm_loadu_si128((__m128i*)(Y + i));
			__m128i Co_ = _mm_loadu_si128((__m128i*)(Co + i));
			__m128i Cg_ = _mm_loadu_si128((__m128i*)(Cg + i));
			__m128i tmp = _mm_sub_epi16(Y_, _mm_srai_epi16(Cg_, 1)); // tmp = Y - Cg/2
			__m128i G = _mm_add_epi16(tmp, Cg_);                     // G = tmp + Cg
			__m128i B = _mm_sub_epi16(tmp, _mm_srai_epi16(Co_, 1));  // B = tmp - Co/2
			__m128i R = _mm_add_epi16(B, Co_);                       // R = B + Co

			// Bit shift from 9bit to 8bit
			R = _mm_srai_epi16(R, 1);
			G = _mm_srai_epi16(G, 1);
			B = _mm_srai_epi16(B, 1);

			// Shuffle and clamp
			__m128i A = _mm_set1_epi16(0x7fff);
			__m128i BG = _mm_packus_epi16(B, G); //BBBBBBBB + GGGGGGGG -> BBBBGGGG
			__m128i RA = _mm_packus_epi16(R, A); //RRRRRRRR + AAAAAAAA -> RRRRAAAA

			__m128i v_perm = _mm_setr_epi8(0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15);
			BG = _mm_shuffle_epi8(BG, v_perm); // BGBGBGBG
			RA = _mm_shuffle_epi8(RA, v_perm); // RARARARA
			__m128i lo = _mm_unpacklo_epi16(BG, RA); // BGRA
			__m128i hi = _mm_unpackhi_epi16(BG, RA);

			_mm_storeu_si128((__m128i*)(dest + i), lo);
			_mm_storeu_si128((__m128i*)(dest + i + 4), hi);
		}
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
        // Fast SIMD version for ARM NEON
        for (; i < aligned_width; i += 8) {
            int16x8_t Y_ = vld1q_s16(Y + i);
            int16x8_t Co_ = vld1q_s16(Co + i);
            int16x8_t Cg_ = vld1q_s16(Cg + i);
            int16x8_t tmp = vsubq_s16(Y_, vshrq_n_s16(Cg_, 1));
            int16x8_t G = vaddq_s16(tmp, Cg_);
            int16x8_t B = vsubq_s16(tmp, vshrq_n_s16(Co_, 1));
            int16x8_t R = vaddq_s16(B, Co_);

			// Please check this, i am not in the capacity to debug arm neon.
			G = vshrq_n_s16(G, 1);
			B = vshrq_n_s16(B, 1);
			R = vshrq_n_s16(R, 1);

            uint8x8x4_t bgra_vec;
            bgra_vec.val[2] = vqmovun_s16(R);
            bgra_vec.val[1] = vqmovun_s16(G);
            bgra_vec.val[0] = vqmovun_s16(B);
            bgra_vec.val[3] = vdup_n_u8(0xFF);

            vst4_u8((uint8_t*)(dest + i), bgra_vec);
        }
#endif
        // Slow version, for last unaligned elements or in case SIMD isn't available
		for (; i < width; ++i) {
			((rgba_t*)dest)[i] = ycocg_to_bgr_9bit(Y[i], Co[i], Cg[i]);
		}

		Y += stride;
		Co += stride;
		Cg += stride;
	}

	//console_print("%d %d %d %d", out_bgra[0], out_bgra[1], out_bgra[2], out_bgra[3]);
}

static void convert_ycocg_to_rgba_block_9bit(icoeff_t* Y, icoeff_t* Co, icoeff_t* Cg, i32 width, i32 height, i32 stride, u32* out_rgba) {
    i32 aligned_width = (width / 8) * 8;

    for (i32 y = 0; y < height; ++y) {
        u32* dest = out_rgba + (y * width);
        i32 i = 0;
#if defined(__SSE2__) && defined(__SSSE3__)
        // Fast SIMD version (~2x faster on my system)
		for (; i < aligned_width; i += 8) {
			// Do the color space conversion
			__m128i Y_ = _mm_loadu_si128((__m128i*)(Y + i));
			__m128i Co_ = _mm_loadu_si128((__m128i*)(Co + i));
			__m128i Cg_ = _mm_loadu_si128((__m128i*)(Cg + i));
			__m128i tmp = _mm_sub_epi16(Y_, _mm_srai_epi16(Cg_, 1)); // tmp = Y - Cg/2
			__m128i G = _mm_add_epi16(tmp, Cg_);                     // G = tmp + Cg
			__m128i B = _mm_sub_epi16(tmp, _mm_srai_epi16(Co_, 1));  // B = tmp - Co/2
			__m128i R = _mm_add_epi16(B, Co_);                       // R = B + Co

			// Bit shift from 9bit to 8bit
			R = _mm_srai_epi16(R, 1);
			G = _mm_srai_epi16(G, 1);
			B = _mm_srai_epi16(B, 1);

			// Shuffle and clamp
			__m128i A = _mm_set1_epi16(0x7fff);
			__m128i RG = _mm_packus_epi16(R, G); //RRRRRRRR + GGGGGGGG -> RRRRGGGG
			__m128i BA = _mm_packus_epi16(B, A); //BBBBBBBB + AAAAAAAA -> BBBBAAAA

			__m128i v_perm = _mm_setr_epi8(0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15);
			RG = _mm_shuffle_epi8(RG, v_perm); // RGRGRGRG
			BA = _mm_shuffle_epi8(BA, v_perm); // BABABABA
			__m128i lo = _mm_unpacklo_epi16(RG, BA); // RGBA
			__m128i hi = _mm_unpackhi_epi16(RG, BA);

			_mm_storeu_si128((__m128i*)(dest + i), lo);
			_mm_storeu_si128((__m128i*)(dest + i + 4), hi);
		}
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
        // Fast SIMD version for ARM NEON
        for (; i < aligned_width; i += 8) {
            int16x8_t Y_ = vld1q_s16(Y + i);
            int16x8_t Co_ = vld1q_s16(Co + i);
            int16x8_t Cg_ = vld1q_s16(Cg + i);
            int16x8_t tmp = vsubq_s16(Y_, vshrq_n_s16(Cg_, 1));
            int16x8_t G = vaddq_s16(tmp, Cg_);
            int16x8_t B = vsubq_s16(tmp, vshrq_n_s16(Co_, 1));
            int16x8_t R = vaddq_s16(B, Co_);

			// Please check this, i am not in the capacity to debug arm neon.
			R = vshrq_n_s16(R, 1);
			G = vshrq_n_s16(G, 1);
			B = vshrq_n_s16(B, 1);

            uint8x8x4_t rgba_vec;
            rgba_vec.val[0] = vqmovun_s16(R);
            rgba_vec.val[1] = vqmovun_s16(G);
            rgba_vec.val[2] = vqmovun_s16(B);
            rgba_vec.val[3] = vdup_n_u8(0xFF);

            vst4_u8((uint8_t*)(dest + i), rgba_vec);
        }
#endif
        // Slow version, for last unaligned elements or in case SIMD isn't available
        for (; i < width; ++i) {
            ((rgba_t*)dest)[i] = ycocg_to_rgb_9bit(Y[i], Co[i], Cg[i]);
        }

        Y += stride;
        Co += stride;
        Cg += stride;
    }
}

#define DEBUG_OUTPUT_IDWT_STEPS_AS_PNG 0

void isyntax_idwt(icoeff_t* idwt, i32 quadrant_width, i32 quadrant_height, bool output_steps_as_png, const char* png_name) {
	i32 full_width = quadrant_width * 2;
	i32 full_height= quadrant_height * 2;
	i32 idwt_stride = full_width;

#if ISYNTAX_WANT_DEBUG_OUTPUT_PNG
	if (output_steps_as_png) {
		char filename[512];
		snprintf(filename, sizeof(filename), "%s_step0.png", png_name);
		debug_convert_wavelet_coefficients_to_image2(idwt, full_width, full_height, filename);
	}
#endif

	// Horizontal pass
	opj_dwt_t h = {0};
	size_t dwt_mem_size = (MAX(quadrant_width, quadrant_height)*2) * PARALLEL_COLS_53 * sizeof(icoeff_t);

	h.mem = (icoeff_t*)alloca(dwt_mem_size); // TODO: need aligned memory?
	h.sn = quadrant_width; // number of elements in low pass band
	h.dn = quadrant_width; // number of elements in high pass band
	h.cas = 1;

	for (i32 y = 0; y < full_height; ++y) {
		icoeff_t* input_row = idwt + y * idwt_stride;
		opj_idwt53_h(&h, input_row);
	}

#if ISYNTAX_WANT_DEBUG_OUTPUT_PNG
	if (output_steps_as_png) {
		char filename[512];
		snprintf(filename, sizeof(filename), "%s_step1.png", png_name);
		debug_convert_wavelet_coefficients_to_image2(idwt, full_width, full_height, filename);
	}
#endif

	// Vertical pass
	opj_dwt_t v = {0};
	v.mem = h.mem;
	v.sn = quadrant_height; // number of elements in low pass band
	v.dn = quadrant_height; // number of elements in high pass band
	v.cas = 1;

	i32 x;
	i32 last_x = full_width;
	for (x = 0; x + PARALLEL_COLS_53 <= last_x; x += PARALLEL_COLS_53) {
		opj_idwt53_v(&v, idwt + x, idwt_stride, PARALLEL_COLS_53);
	}
	if (x < last_x) {
		opj_idwt53_v(&v, idwt + x, idwt_stride, (last_x - x));
	}

#if ISYNTAX_WANT_DEBUG_OUTPUT_PNG
	if (output_steps_as_png) {
		char filename[512];
		snprintf(filename, sizeof(filename), "%s_step2.png", png_name);
		debug_convert_wavelet_coefficients_to_image2(idwt, full_width, full_height, filename);
	}
#endif

}

static inline void get_offsetted_coeff_blocks(icoeff_t** ll_hl_lh_hh, i32 offset, isyntax_tile_channel_t* color_channel, i32 block_stride, icoeff_t* black_dummy_coeff, icoeff_t* white_dummy_coeff) {
	if (color_channel->coeff_ll) {
		ll_hl_lh_hh[0] = color_channel->coeff_ll + offset; //ll
	} else {
		ll_hl_lh_hh[0] = white_dummy_coeff;
	}
	if (color_channel->coeff_h) {
		ll_hl_lh_hh[1] = color_channel->coeff_h + offset; //hl
		ll_hl_lh_hh[2] = color_channel->coeff_h + block_stride + offset; //lh
		ll_hl_lh_hh[3] = color_channel->coeff_h + 2*block_stride + offset; //hh
	} else {
		ll_hl_lh_hh[1] = black_dummy_coeff;
		ll_hl_lh_hh[2] = black_dummy_coeff;
		ll_hl_lh_hh[3] = black_dummy_coeff;
	}

}

u32 isyntax_get_adjacent_tiles_mask(isyntax_level_t* level, i32 tile_x, i32 tile_y) {
	ASSERT(tile_x >= 0 && tile_y >= 0);
	ASSERT(tile_x < level->width_in_tiles && tile_y < level->height_in_tiles);
	// 9 bits, corresponding to the surrounding tiles:
	// 0x100 | 0x80 | 0x40
	// 0x20  | 0x10 | 8
	// 4     | 2    | 1
	u32 adj_tiles = 0x1FF; // all bits set
	if (tile_y == 0)                        adj_tiles &= ~(ISYNTAX_ADJ_TILE_TOP_LEFT | ISYNTAX_ADJ_TILE_TOP_CENTER | ISYNTAX_ADJ_TILE_TOP_RIGHT);
	if (tile_y == level->height_in_tiles-1) adj_tiles &= ~(ISYNTAX_ADJ_TILE_BOTTOM_LEFT | ISYNTAX_ADJ_TILE_BOTTOM_CENTER | ISYNTAX_ADJ_TILE_BOTTOM_RIGHT);
	if (tile_x == 0)                        adj_tiles &= ~(ISYNTAX_ADJ_TILE_TOP_LEFT | ISYNTAX_ADJ_TILE_CENTER_LEFT | ISYNTAX_ADJ_TILE_BOTTOM_LEFT);
	if (tile_x == level->width_in_tiles-1)  adj_tiles &= ~(ISYNTAX_ADJ_TILE_TOP_RIGHT | ISYNTAX_ADJ_TILE_CENTER_RIGHT | ISYNTAX_ADJ_TILE_BOTTOM_RIGHT);
	return adj_tiles;
}

u32 isyntax_get_adjacent_tiles_mask_only_existing(isyntax_level_t* level, i32 tile_x, i32 tile_y) {
	u32 adjacent = isyntax_get_adjacent_tiles_mask(level, tile_x, tile_y);
	u32 mask = 0;
	if (adjacent & ISYNTAX_ADJ_TILE_TOP_LEFT) {
		isyntax_tile_t* tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x-1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_TOP_LEFT;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_TOP_CENTER) {
		isyntax_tile_t* tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_TOP_CENTER;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_TOP_RIGHT) {
		isyntax_tile_t* tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x+1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_TOP_RIGHT;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER_LEFT) {
		isyntax_tile_t* tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x-1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_CENTER_LEFT;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER) {
		isyntax_tile_t* tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_CENTER;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER_RIGHT) {
		isyntax_tile_t* tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x+1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_CENTER_RIGHT;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_LEFT) {
		isyntax_tile_t* tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x-1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_BOTTOM_LEFT;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_CENTER) {
		isyntax_tile_t* tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_BOTTOM_CENTER;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_RIGHT) {
		isyntax_tile_t* tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x+1);
		if (tile->exists) mask |= ISYNTAX_ADJ_TILE_BOTTOM_RIGHT;
	}
	return mask;
}

bool isyntax_is_parent_tile_missing(isyntax_image_t* wsi, i32 scale, i32 tile_x, i32 tile_y) {
	// Some tiles may have a missing parent tile (from scale + 1).
	// In this case the LL coefficients are missing, but dummy coefficients can be safely used instead.
	if (scale < wsi->max_scale) {
		isyntax_level_t* parent_level = wsi->levels + scale + 1;
		isyntax_tile_t* parent_tile = parent_level->tiles + (tile_y/2) * parent_level->width_in_tiles + (tile_x/2);
		return !parent_tile->exists;
	} else {
		return false;
	}
}

u32 isyntax_idwt_tile_for_color_channel(isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale, i32 tile_x, i32 tile_y, i32 color, icoeff_t* dest_buffer) {
	isyntax_level_t* level = wsi->levels + scale;
	ASSERT(tile_x >= 0 && tile_x < level->width_in_tiles);
	ASSERT(tile_y >= 0 && tile_y < level->height_in_tiles);
	isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
	isyntax_tile_channel_t* channel = tile->color_channels + color;

	u32 adj_tiles = isyntax_get_adjacent_tiles_mask(level, tile_x, tile_y);

//	ASSERT(channel->neighbors_loaded == adj_tiles);

	// Prepare for stitching together the input image, with margins sampled from adjacent tiles for each quadrant
	i32 pad_l = ISYNTAX_IDWT_PAD_L;
	i32 pad_r = ISYNTAX_IDWT_PAD_R;
	i32 pad_l_plus_r = pad_l + pad_r;
//	ASSERT(sizeof(icoeff_t) * pad_amount == sizeof(u64)); // blit 64 bits == 4 pixels
	i32 block_width = isyntax->block_width;
	i32 block_height = isyntax->block_height;
	i32 quadrant_width = block_width + pad_l_plus_r;
	i32 quadrant_height = block_height + pad_l_plus_r;
	i32 full_width = 2 * quadrant_width;
	i32 full_height = 2 * quadrant_height;
	icoeff_t* idwt = dest_buffer; // allocated/given by the caller ahead of time

	i32 dest_stride = full_width;

	// fill upper left quadrant with white
	if (color == 0) {
		for (i32 x = 0; x < quadrant_width; ++x) {
			idwt[x] = 255;
		}
		for (i32 y = 1; y < quadrant_width; ++y) {
			memcpy(idwt + y * dest_stride, idwt, quadrant_width * sizeof(icoeff_t));
		}
	}
	icoeff_t* h_dummy_coeff = isyntax->black_dummy_coeff;
	icoeff_t* ll_dummy_coeff = (color == 0) ? isyntax->white_dummy_coeff : isyntax->black_dummy_coeff;


	i32 source_stride = block_width;
	i32 left_margin_source_x = block_width - pad_r;
	i32 top_margin_source_y = block_height - pad_r;
	size_t row_copy_size = block_width * sizeof(icoeff_t);
	size_t pad_l_copy_size = pad_l * sizeof(icoeff_t);
	size_t pad_r_copy_size = pad_r * sizeof(icoeff_t);

	i32 block_stride = block_width * block_height;
	i32 quadrant_offsets[4] = {0, quadrant_width, full_width * quadrant_height, full_width * quadrant_height + quadrant_width};
	icoeff_t* quadrants[4] = {0};
	for (i32 i = 0; i < 4; ++i) {
		quadrants[i] = idwt + quadrant_offsets[i];
	}

	icoeff_t* ll_hl_lh_hh[4] = {0};

	u32 invalid_neighbors_ll = 0;
	u32 invalid_neighbors_h = 0;

	// Now do the stitching, with margins sampled from adjacent tiles for each quadrant
	// LL | HL
	// LH | HH

	// top left corner
	if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_LEFT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x-1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y-1, tile_x-1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_TOP_LEFT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_TOP_LEFT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, (top_margin_source_y * source_stride) + left_margin_source_x,
									   color_channel, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t* source = ll_hl_lh_hh[i];
				icoeff_t* dest = quadrants[i];
				for (i32 y = 0; y < pad_l; ++y) {
					memcpy(dest, source, pad_l_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}
	// top center
	if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_CENTER) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + tile_x;
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y-1, tile_x)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_TOP_CENTER;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_TOP_CENTER;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, (top_margin_source_y * source_stride),
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + pad_l;
				for (i32 y = 0; y < pad_l; ++y) {
					memcpy(dest, source, row_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}
	// top right corner
	if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_RIGHT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x+1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y-1, tile_x+1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_TOP_RIGHT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_TOP_RIGHT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, (top_margin_source_y * source_stride),
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + pad_l + block_width;
				for (i32 y = 0; y < pad_l; ++y) {
					memcpy(dest, source, pad_r_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}
	// center left
	if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER_LEFT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x-1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y, tile_x-1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_CENTER_LEFT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_CENTER_LEFT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, left_margin_source_x,
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + (pad_l * dest_stride);
				for (i32 y = 0; y < block_height; ++y) {
					memcpy(dest, source, pad_l_copy_size);
					dest += dest_stride;
					source += source_stride;
				}
			}
		}
	}
	// center (main tile)
	if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER) {
		get_offsetted_coeff_blocks(ll_hl_lh_hh, 0,
		                           channel, block_stride, h_dummy_coeff, ll_dummy_coeff);
		for (i32 i = 0; i < 4; ++i) {
			icoeff_t *source = ll_hl_lh_hh[i];
			icoeff_t *dest = quadrants[i] + (pad_l * dest_stride) + pad_l;
			for (i32 y = 0; y < block_height; ++y) {
				memcpy(dest, source, row_copy_size);
				dest += dest_stride;
				source += source_stride;
			}
		}

	}
	// center right
	if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER_RIGHT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x+1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y, tile_x+1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_CENTER_RIGHT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_CENTER_RIGHT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, 0,
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + (pad_l * dest_stride) + pad_l + block_width;
				for (i32 y = 0; y < block_height; ++y) {
					memcpy(dest, source, pad_r_copy_size);
					dest += dest_stride;
					source += source_stride;
				}
			}
		}
	}
	// bottom left corner
	if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_LEFT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x-1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y+1, tile_x-1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_BOTTOM_LEFT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_BOTTOM_LEFT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, left_margin_source_x,
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + ((pad_l + block_height) * dest_stride);
				for (i32 y = 0; y < pad_r; ++y) {
					memcpy(dest, source, pad_l_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}
	// bottom center
	if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_CENTER) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + tile_x;
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y+1, tile_x)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_BOTTOM_CENTER;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_BOTTOM_CENTER;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, 0,
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + ((pad_l + block_height) * dest_stride) + pad_l;
				for (i32 y = 0; y < pad_r; ++y) {
					memcpy(dest, source, row_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}
	// bottom right corner
	if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_RIGHT) {
		isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x+1);
		if (source_tile->exists) {
			isyntax_tile_channel_t* color_channel = source_tile->color_channels + color;
			if (!color_channel->coeff_ll && !isyntax_is_parent_tile_missing(wsi, scale, tile_y+1, tile_x+1)) {
				invalid_neighbors_ll |= ISYNTAX_ADJ_TILE_BOTTOM_RIGHT;
			}
			if (!color_channel->coeff_h) {
				invalid_neighbors_h |= ISYNTAX_ADJ_TILE_BOTTOM_RIGHT;
			}
			get_offsetted_coeff_blocks(ll_hl_lh_hh, 0,
									   source_tile->color_channels + color, block_stride, h_dummy_coeff, ll_dummy_coeff);
			for (i32 i = 0; i < 4; ++i) {
				icoeff_t *source = ll_hl_lh_hh[i];
				icoeff_t *dest = quadrants[i] + ((pad_l + block_height) * dest_stride) + pad_l + block_width;
				for (i32 y = 0; y < pad_r; ++y) {
					memcpy(dest, source, pad_r_copy_size);
					source += source_stride;
					dest += dest_stride;
				}
			}
		}
	}

	bool output_pngs = false;
	const char* debug_png = "debug_idwt_";
	/*
	if (scale == wsi->max_scale && tile_x == 1 && tile_y == 1 && color == 0) {
		output_pngs = true;
	}*/
	isyntax_idwt(idwt, quadrant_width, quadrant_height, output_pngs, debug_png);

	u32 invalid_edges = invalid_neighbors_h | invalid_neighbors_ll;
	return invalid_edges;
}

void isyntax_load_tile(isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale, i32 tile_x, i32 tile_y,
                       block_allocator_t* ll_coeff_block_allocator,
                       u32* out_buffer_or_null, enum isyntax_pixel_format_t pixel_format) {
	// printf("@@@ isyntax_load_tile scale=%d tile_x=%d tile_y=%d\n", scale, tile_x, tile_y);
	isyntax_level_t* level = wsi->levels + scale;
	ASSERT(tile_x >= 0 && tile_x < level->width_in_tiles);
	ASSERT(tile_y >= 0 && tile_y < level->height_in_tiles);
	isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
	i32 block_width = isyntax->block_width;
	i32 block_height = isyntax->block_height;
	size_t block_size = block_width * block_height * sizeof(icoeff_t);
	i32 first_valid_pixel = ISYNTAX_IDWT_FIRST_VALID_PIXEL;
	i32 idwt_width = 2 * (block_width + ISYNTAX_IDWT_PAD_L + ISYNTAX_IDWT_PAD_R);
	i32 idwt_height = 2 * (block_height + ISYNTAX_IDWT_PAD_L + ISYNTAX_IDWT_PAD_R);
	i32 idwt_stride = idwt_width;
	size_t row_copy_size = block_width * sizeof(icoeff_t);

	temp_memory_t temp_memory = begin_temp_memory_on_local_thread();

	icoeff_t* Y = NULL;
	icoeff_t* Co = NULL;
	icoeff_t* Cg = NULL;

	float elapsed_idwt = 0.0f;
	float elapsed_malloc = 0.0f;

	u32 invalid_edges = 0;

	for (i32 color = 0; color < 3; ++color) {
		i64 start_idwt = get_clock();
		// idwt will be allocated in temporary memory (only needed for the duration of this function)
		size_t idwt_buffer_size = idwt_width * idwt_height * sizeof(icoeff_t);
		icoeff_t* idwt = arena_push_size(temp_memory.arena, idwt_buffer_size);
		memset(idwt, 0, idwt_buffer_size);
		invalid_edges |= isyntax_idwt_tile_for_color_channel(isyntax, wsi, scale, tile_x, tile_y, color, idwt);
		elapsed_idwt += get_seconds_elapsed(start_idwt, get_clock());
		ASSERT(idwt);
		switch(color) {
			case 0: Y = idwt; break;
			case 1: Co = idwt; break;
			case 2: Cg = idwt; break;
			default: {
				ASSERT(!"invalid code path");
			} break;
		}

		if (scale == 0) {
			// No children to take care of at level 0.
			continue;
		}

		// Distribute result to child tiles if it was not distributed already.
		isyntax_level_t* next_level = wsi->levels + (scale - 1);
		isyntax_tile_t* child_top_left = next_level->tiles + (tile_y*2) * next_level->width_in_tiles + (tile_x*2);
		isyntax_tile_t* child_top_right = child_top_left + 1;
		isyntax_tile_t* child_bottom_left = child_top_left + next_level->width_in_tiles;
		isyntax_tile_t* child_bottom_right = child_bottom_left + 1;

		// TODO(avirodov): instead of releasing here, skip copy if still allocated.
		if (child_top_left->color_channels[color].coeff_ll) {
			block_free(ll_coeff_block_allocator, child_top_left->color_channels[color].coeff_ll);
		}
		if (child_top_right->color_channels[color].coeff_ll) {
			block_free(ll_coeff_block_allocator, child_top_right->color_channels[color].coeff_ll);
		}
		if (child_bottom_left->color_channels[color].coeff_ll) {
			block_free(ll_coeff_block_allocator, child_bottom_left->color_channels[color].coeff_ll);
		}
		if (child_bottom_right->color_channels[color].coeff_ll) {
			block_free(ll_coeff_block_allocator, child_bottom_right->color_channels[color].coeff_ll);
		}

		// NOTE: malloc() and free() can become a bottleneck, they don't scale well especially across many threads.
		// We use a custom block allocator to address this.
		i64 start_malloc = get_clock();
		child_top_left->color_channels[color].coeff_ll = (icoeff_t*)block_alloc(ll_coeff_block_allocator);
		child_top_right->color_channels[color].coeff_ll = (icoeff_t*)block_alloc(ll_coeff_block_allocator);
		child_bottom_left->color_channels[color].coeff_ll = (icoeff_t*)block_alloc(ll_coeff_block_allocator);
		child_bottom_right->color_channels[color].coeff_ll = (icoeff_t*)block_alloc(ll_coeff_block_allocator);
		elapsed_malloc += get_seconds_elapsed(start_malloc, get_clock());
		i32 dest_stride = block_width;
		// Blit top left child LL block
		{
			icoeff_t* dest = child_top_left->color_channels[color].coeff_ll;
			icoeff_t* source = idwt + (first_valid_pixel * idwt_stride) + first_valid_pixel;
			for (i32 y = 0; y < block_height; ++y) {
				memcpy(dest, source, row_copy_size);
				dest += dest_stride;
				source += idwt_stride;
			}
		}
		// Blit top right child LL block
		{
			icoeff_t* dest = child_top_right->color_channels[color].coeff_ll;
			icoeff_t* source = idwt + (first_valid_pixel * idwt_stride) + first_valid_pixel + block_width;
			for (i32 y = 0; y < block_height; ++y) {
				memcpy(dest, source, row_copy_size);
				dest += dest_stride;
				source += idwt_stride;
			}
		}
		// Blit bottom left child LL block
		{
			icoeff_t* dest = child_bottom_left->color_channels[color].coeff_ll;
			icoeff_t* source = idwt + ((first_valid_pixel + block_height) * idwt_stride) + first_valid_pixel;
			for (i32 y = 0; y < block_height; ++y) {
				memcpy(dest, source, row_copy_size);
				dest += dest_stride;
				source += idwt_stride;
			}
		}
		// Blit bottom right child LL block
		{
			icoeff_t* dest = child_bottom_right->color_channels[color].coeff_ll;
			icoeff_t* source = idwt + ((first_valid_pixel + block_height) * idwt_stride) + first_valid_pixel + block_width;
			for (i32 y = 0; y < block_height; ++y) {
				memcpy(dest, source, row_copy_size);
				dest += dest_stride;
				source += idwt_stride;
			}
		}

		// After the last color channel, we can report that the children now have their LL blocks available.
		if (color == 2) {
			child_top_left->has_ll = true;
			child_top_right->has_ll = true;
			child_bottom_left->has_ll = true;
			child_bottom_right->has_ll = true;

			if (invalid_edges != 0) {
				console_print_error("load: scale=%d x=%d y=%d  idwt time =%g  invalid edges=%x\n", scale, tile_x, tile_y, elapsed_idwt, invalid_edges);
				// early out
				release_temp_memory(&temp_memory);
				return;
			}
		}
	}

	tile->is_loaded = true; // Meaning: it is now safe to start loading 'child' tiles of the next level
	if (out_buffer_or_null == NULL) {
		release_temp_memory(&temp_memory); // free Y, Co and Cg
		return;
	}

	// For the Y (luminance) color channel, we actually need the absolute value of the Y-channel wavelet coefficient.
	// (This doesn't hold for Co and Cg, those are are used directly as signed integers)
    signed_magnitude_to_absolute_value_16_block(Y, idwt_width * idwt_height);

	// Reconstruct RGB image from separate color channels while cutting off margins
	i64 start = get_clock();
	i32 tile_width = block_width * 2;
	i32 tile_height = block_height * 2;

	i32 valid_offset = (first_valid_pixel * idwt_stride) + first_valid_pixel;

	// NOTE: in iSyntax v1, bits_stored is usually 16 (but the values will actually cleanly decode to 8-bit colors).
	// In iSyntax V2, bits_stored can be either 8 (this tends to be the case for v100.5 files), or they can be 9 (for v100.4 files).
	// If bits_stored == 9, the coefficients no longer map to 0..255; a correction is therefore needed.
	if(isyntax->bits_stored == 9){
		switch (pixel_format) {
			case LIBISYNTAX_PIXEL_FORMAT_BGRA:
				convert_ycocg_to_bgra_block_9bit(Y + valid_offset, Co + valid_offset, Cg + valid_offset, tile_width, tile_height,
											idwt_stride, out_buffer_or_null);
				break;

			case LIBISYNTAX_PIXEL_FORMAT_RGBA:
				convert_ycocg_to_rgba_block_9bit(Y + valid_offset, Co + valid_offset, Cg + valid_offset, tile_width, tile_height,
											idwt_stride, out_buffer_or_null);
				break;

			default:
				ASSERT(!"unknown pixel format!");
				break;
		}
	} else {
		switch (pixel_format) {
			case LIBISYNTAX_PIXEL_FORMAT_BGRA:
				convert_ycocg_to_bgra_block_8bit(Y + valid_offset, Co + valid_offset, Cg + valid_offset, tile_width, tile_height,
											idwt_stride, out_buffer_or_null);
				break;

			case LIBISYNTAX_PIXEL_FORMAT_RGBA:
				convert_ycocg_to_rgba_block_8bit(Y + valid_offset, Co + valid_offset, Cg + valid_offset, tile_width, tile_height,
											idwt_stride, out_buffer_or_null);
				break;

			default:
				ASSERT(!"unknown pixel format!");
				break;
		}
	}
	isyntax->total_rgb_transform_time += get_seconds_elapsed(start, get_clock());

	//		float elapsed_rgb = get_seconds_elapsed(start, get_clock());
	//	console_print_verbose("load: scale=%d x=%d y=%d  idwt time =%g  rgb transform time=%g  malloc time=%g\n", scale, tile_x, tile_y, elapsed_idwt, elapsed_rgb, elapsed_malloc);

//	if (scale == wsi->max_scale-1 && tile_x == 5 && tile_y == 0) {
//		stbi_write_png("debug_dwt_output.png", tile_width, tile_height, 4, out_buffer_or_null, tile_width * 4);
//	}

	release_temp_memory(&temp_memory); // free Y, Co and Cg
}


// Example codeblock order for a 'chunk' in the file:
// x        y       color   scale   coeff   offset      size    header_template_id
// 66302	66302	0	    8	    1	    850048253	8270	18
// 65918	65918	0	    7	    1	    850056531	17301	19
// 98686	65918	0	    7	    1	    850073840	14503	19
// 65918	98686	0	    7	    1	    850088351	8	    19
// 98686	98686	0	    7	    1	    850088367	8	    19
// 65726	65726	0	    6	    1	    850088383	26838	20
// 82110	65726	0	    6	    1	    850115229	11215	20
// 98494	65726	0	    6	    1	    850126452	6764	20
// 114878	65726	0	    6	    1	    850133224	25409	20
// 65726	82110	0	    6	    1	    850158641	21369	20
// 82110	82110	0	    6	    1	    850180018	8146	20
// 98494	82110	0	    6	    1	    850188172	4919	20
// 114878	82110	0	    6	    1	    850193099	19908	20
// 65726	98494	0	    6	    1	    850213015	8	    20
// 82110	98494	0	    6	    1	    850213031	8	    20
// 98494	98494	0	    6	    1	    850213047	8	    20
// 114878	98494	0	    6	    1	    850213063	8	    20
// 65726	114878	0	    6	    1	    850213079	8	    20
// 82110	114878	0	    6	    1	    850213095	8	    20
// 98494	114878	0	    6	    1	    850213111	8	    20
// 114878	114878	0	    6	    1	    850213127	8	    20
// 66558	66558	0	    8	    0	    850213143	5558	21    <- LL codeblock

// The above pattern repeats for the other 2 color channels (1 and 2).
// The LL codeblock is only present at the highest scales.

void isyntax_decompress_codeblock_in_chunk(isyntax_codeblock_t* codeblock, i32 block_width, i32 block_height, u8* chunk, u64 chunk_base_offset, i32 compressor_version, i16* out_buffer) {
	i64 offset_in_chunk = codeblock->block_data_offset - chunk_base_offset;
	ASSERT(offset_in_chunk >= 0);
	isyntax_hulsken_decompress(chunk + offset_in_chunk, codeblock->block_size,
							   block_width, block_height, codeblock->coefficient, compressor_version, out_buffer);
}

// Read between 57 and 64 bits (7 bytes + 1-8 bits) from a bitstream (least significant bit first).
// Requires that at least 7 safety bytes are present at the end of the stream (don't trigger a segmentation fault)!
static inline u64 bitstream_lsb_read(u8* buffer, u32 pos) {
	u64 raw = read_u64_le(buffer + pos / 8);
	raw >>= pos % 8;
	return raw;
}

static inline u64 bitstream_lsb_read_advance(u8* buffer, i32* bits_read, i32 bits_to_read) {
	u64 raw = read_u64_le(buffer + (*bits_read / 8));
	raw >>= (*bits_read % 8);
	*bits_read += bits_to_read;
	return raw;
}

// partly adapted from stb_image.h
#define HUFFMAN_FAST_BITS 11   // optimal value may depend on various factors, CPU cache etc.

// Lookup table for (1 << n) - 1
static const u16 size_bitmasks[17]={0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535};

typedef struct huffman_t {
	u16 fast[1 << HUFFMAN_FAST_BITS];
	u16 code[256];
	u8  size[256];
	u16 nonfast_symbols[256];
	u16 nonfast_code[256+7]; // extra safety bytes for SIMD vector operations
	u16 nonfast_size[256];
	u16 nonfast_size_masks[256+7]; // extra safety bytes for SIMD vector operations
} huffman_t;

void save_code_in_huffman_fast_lookup_table(huffman_t* h, u32 code, u32 code_width, u8 symbol) {
	ASSERT(code_width <= HUFFMAN_FAST_BITS);
	i32 duplicate_bits = HUFFMAN_FAST_BITS - code_width;
	for (u32 i = 0; i < (1 << duplicate_bits); ++i) {
		u32 address = (i << code_width) | code;
		h->fast[address] = symbol;
	}
}

//static u32 max_code_size;
//static u32 symbol_counts[256];
//static u64 fast_count;
//static u64 nonfast_count;

/*
static void dump_block(u8* compressed, size_t compressed_size) {
#if 1 && DO_DEBUG
	FILE* fp = fopen("dumped_codeblock.dat", "wb");
	fwrite(compressed, compressed_size, 1, fp);
	fclose(fp);
#endif
}
*/

/*
void isyntax_test_decompress_block(const char* filename) {
#if 1 && DO_DEBUG
	mem_t* file = platform_read_entire_file(filename);
	if (file) {
		i32 block_width = 128;
		i32 block_height = 128;
		i32 coefficient = 1;
		i32 coeff_count = (coefficient == 1) ? 3 : 1;
		size_t coeff_buffer_size = coeff_count * block_width * block_height * sizeof(i16);
		i16* coeff_buffer = calloc(1, coeff_buffer_size);
		isyntax_hulsken_decompress(file->data, file->len, 128, 128, coefficient, 2, coeff_buffer);

		char out_filename[256] = {};
		snprintf(out_filename, sizeof(out_filename), "%s.raw", filename);
		FILE* fp = fopen(out_filename, "wb");
		fwrite(coeff_buffer, coeff_buffer_size, 1, fp);
		fclose(fp);

		free(coeff_buffer);
		free(file);
	}
#endif
}
 */

bool isyntax_hulsken_decompress(u8* compressed, size_t compressed_size, i32 block_width, i32 block_height,
								i32 coefficient, i32 compressor_version, i16* out_buffer) {
	ASSERT(compressor_version == 1 || compressor_version == 2);

	// Read the header information stored in the codeblock.
	// The layout varies depending on the version of the compressor used (version 1 or 2).
	// All integers are stored little-endian, least-significant bit first.
	//
	// Version 1 layout:
	//   uint32 : serialized length (in bytes)
	//   uint8 : zero run symbol
	//   uint8 : zero run counter size (in bits)
	// Version 2 layout:
	//   coeff_count (== 1 or 3) * coeff_bit_depth bits : 1 or 3 bitmasks, indicating which bitplanes are present
	//   uint8 : zero run symbol
	//   uint8 : zero run counter size (in bits)
	//   (variable length) : bitplane seektable (contains offsets to each of the bitplanes)

	// After the header section, the rest of the codeblock contains a Huffman tree, followed by a Huffman-coded
	// message of 8-bit Huffman symbols, interspersed with 'zero run' symbols (for run-length encoding of zeroes).

	i32 coeff_count = (coefficient == 1) ? 3 : 1;
	i32 coeff_bit_depth = 16; // fixed value for iSyntax
	size_t coeff_buffer_size = coeff_count * block_width * block_height * sizeof(i16);

	// Early out if dummy/empty block
	if (compressed_size <= 8) {
		memset(out_buffer, 0, coeff_buffer_size);
		return true;
	}

	temp_memory_t temp_memory = begin_temp_memory_on_local_thread();

	i32 bits_read = 0;
	i32 block_size_in_bits = compressed_size * 8;
	i64 serialized_length = 0; // In v1: stored in the first 4 bytes. In v2: derived calculation.
	u32 bitmasks[3] = { 0x000FFFF, 0x000FFFF, 0x000FFFF }; // default in v1: all ones, can be overridden later
	i32 total_mask_bits = coeff_bit_depth * coeff_count;
	u8* byte_pos = compressed;
	if (compressor_version == 1) {
		serialized_length = read_u32_le(byte_pos);
		byte_pos += 4;
		bits_read += 4*8;
	} else {
		if (coeff_count == 1) {
			bitmasks[0] = read_u16_le(byte_pos);
			byte_pos += 2;
			bits_read += 2*8;
			total_mask_bits = popcount(bitmasks[0]);
		} else if (coeff_count == 3) {
			bitmasks[0] = read_u16_le(byte_pos);
			bitmasks[1] = read_u16_le(byte_pos+2);
			bitmasks[2] = read_u16_le(byte_pos+4);
			byte_pos += 6;
			bits_read += 6*8;
			total_mask_bits = popcount(bitmasks[0]) + popcount(bitmasks[1]) + popcount(bitmasks[2]);
		} else {
			fatal_error("invalid coeff_count");
		}
		serialized_length = total_mask_bits * (block_width * block_height / 8);
	}

	// Check that the serialized length is sane
	if (serialized_length > 2 * coeff_buffer_size) {
//		dump_block(compressed, compressed_size);
		console_print_error("Error: isyntax_hulsken_decompress(): invalid codeblock, serialized_length too large (%lld)\n", serialized_length);
		ASSERT(!"serialized_length too large");
		memset(out_buffer, 0, coeff_buffer_size);
		release_temp_memory(&temp_memory);
		return false;
	}

	u8 zerorun_symbol = *(u8*)byte_pos++;
	bits_read += 8;
	u8 zero_counter_size = *(u8*)byte_pos++;
	bits_read += 8;

	u32 bitplane_offsets[16] = {0};
	if (compressor_version >= 2) {
		// Read bitplane seektable: a pointer is stored for a bit if it's represented in at least one of the bitmasks
		u32 bitmasks_aggregate = 0;
		for (i32 i = 0; i < coeff_count; ++i) {
			bitmasks_aggregate |= bitmasks[i];
		}
		i32 bitplane_ptr_count = popcount(bitmasks_aggregate);
		i32 bitplane_ptr_bits = (i32)(log2f(serialized_length)) + 5;
		u32 bitplane_ptr_mask = (1u << (bitplane_ptr_bits)) - 1;
		for (i32 i = 0; i < bitplane_ptr_count - 1; ++i) {
			u64 blob = bitstream_lsb_read(compressed, bits_read);
			bitplane_offsets[i] = blob & bitplane_ptr_mask;
			bits_read += bitplane_ptr_bits;
		}
	}

	// Read Huffman table
	huffman_t huffman = {0};
	memset(huffman.fast, 0x80, sizeof(huffman.fast));
	memset(huffman.nonfast_size_masks, 0xFF, sizeof(huffman.nonfast_size_masks));
	u32 fast_mask = (1 << HUFFMAN_FAST_BITS) - 1;
	{
		i32 code_size = 0;
		u32 code = 0;
		i32 nonfast_symbol_index = 0;
		do {
			if (bits_read >= block_size_in_bits) {
//				dump_block(compressed, compressed_size);
				console_print_error("Error: isyntax_hulsken_decompress(): invalid codeblock, Huffman table extends out of bounds (compressed_size=%zu)\n", compressed_size);
				ASSERT(!"out of bounds");
				memset(out_buffer, 0, coeff_buffer_size);
				release_temp_memory(&temp_memory);
				return false;
			}
			// Read a chunk of bits large enough to 'always' have the whole Huffman code, followed by the 8-bit symbol.
			// A blob of 57-64 bits is more than sufficient for a Huffman code of at most 16 bits.
			// The bitstream is organized least significant bit first (treat as one giant little-endian integer).
			// To read bits in the stream, look at the lowest bit positions. To advance the stream, shift right.
			i32 bits_to_advance = 1;
			u64 blob = bitstream_lsb_read(compressed, bits_read); // gives back between 57 and 64 bits.

			// 'Descend' into the tree until we hit a leaf node.
			bool is_leaf = blob & 1;
			// TODO: intrinsic?
			while (!is_leaf) {
				++bits_to_advance;
				blob >>= 1;
				is_leaf = (blob & 1);
				++code_size;
			}
			blob >>= 1;

			// Read 8-bit Huffman symbol
			u8 symbol = (u8)(blob);
			huffman.code[symbol] = code;
			huffman.size[symbol] = code_size;

			if (code_size <= HUFFMAN_FAST_BITS) {
				// We can accelerate decoding of small Huffman codes by storing them in a lookup table.
				// However, for the longer codes this becomes inefficient so in those cases we need another method.
				save_code_in_huffman_fast_lookup_table(&huffman, code, code_size, symbol);
//				++fast_count;
			} else {
				// Prepare the slow method for decoding Huffman codes that are too large to fit in the fast lookup table.
				// (Slow method = iterating over possible symbols and checking if they match)
				// Value being >= 256 in the 'fast' lookup table is the hint that we need the slow method.
				u32 prefix = code & fast_mask;
				u16 old_fast_data = huffman.fast[prefix];
				u8 old_lowest_symbol_index = old_fast_data & 0xFF;
				u8 new_lowest_symbol_index = MIN(old_lowest_symbol_index, nonfast_symbol_index);
				huffman.fast[prefix] = 256 + new_lowest_symbol_index;
				huffman.nonfast_symbols[nonfast_symbol_index] = symbol;
				huffman.nonfast_code[nonfast_symbol_index] = code;
				huffman.nonfast_size[nonfast_symbol_index] = code_size;
				huffman.nonfast_size_masks[nonfast_symbol_index] = size_bitmasks[code_size];
				++nonfast_symbol_index;
//				++nonfast_count;
			}
			/*if (code_size > max_code_size) {
				max_code_size = code_size;
//			    console_print("found the biggest code size: %d\n", code_size);
			}*/
//			symbol_counts[symbol]++;

			bits_to_advance += 8;
			bits_read += bits_to_advance;

			// traverse back up the tree: find last zero -> flip to one
			if (code_size == 0) {
				break; // already done; this happens if there is only a root node, no leaves
			}
			u32 code_high_bit = (1 << (code_size - 1));
			bool found_zero = (~code) & code_high_bit;
			while (!found_zero) {
				--code_size;
				if (code_size == 0) break;
				code &= code_high_bit - 1;
				code_high_bit >>= 1;
				found_zero = (~code) & code_high_bit;
			}
			code |= code_high_bit;
		} while(code_size > 0);
	}

	// Decode the message
	u8* decompressed_buffer = (u8*)arena_push_size(temp_memory.arena, serialized_length);

	u32 zerorun_code = huffman.code[zerorun_symbol];
	u32 zerorun_code_size = huffman.size[zerorun_symbol];
	if (zerorun_code_size == 0) zerorun_code_size = 1; // handle special case of the 'empty' Huffman tree (root node is leaf node)
	u32 zerorun_code_mask = (1 << zerorun_code_size) - 1;

	u32 zero_counter_mask = (1 << zero_counter_size) - 1;
	i32 decompressed_length = 0;
	while (bits_read < block_size_in_bits) {
		if (decompressed_length >= serialized_length || bits_read >= block_size_in_bits) {
			break; // done
		}
		i32 symbol = 0;
		i32 code_size = 1;
		u64 blob = bitstream_lsb_read(compressed, bits_read);
		u32 fast_index = blob & fast_mask;
		u16 c = huffman.fast[fast_index];
		if (c <= 255) {
			// Lookup the symbol directly.
			symbol = c;
			code_size = huffman.size[symbol];
		} else {
			bool match = false;
			u8 lowest_possible_symbol_index = c & 0xFF;

#if  !((defined(__SSE2__) && defined(__AVX__)))
			for (i32 i = lowest_possible_symbol_index; i < 256; ++i) {
				u8 test_size = huffman.nonfast_size[i];
				u16 test_code = huffman.nonfast_code[i];
				if ((blob & size_bitmasks[test_size]) == test_code) {
					// match
					code_size = test_size;
					symbol = huffman.nonfast_symbols[i];
					match = true;
					break;
				}
			}
#else
			// SIMD version using SSE2, very slightly faster than the version above
			// (Need to compile with AVX enabled, otherwise unaligned loads will make it slower)
			// NOTE: Probably not a bottleneck, the loop below (nearly) always finishes after one iteration.
			for (i32 i = lowest_possible_symbol_index; i < 256; i += 8) {
				__m128i size_mask = _mm_loadu_si128((__m128i*)(huffman.nonfast_size_masks + i));
				__m128i code = _mm_loadu_si128((__m128i*)(huffman.nonfast_code + i));
				__m128i test = _mm_set1_epi16((u16)blob);
				test = _mm_and_si128(test, size_mask);
				__m128i hit = _mm_cmpeq_epi16(test, code);
				u32 hit_mask = _mm_movemask_epi8(hit);
				if (hit_mask) {
					u32 first_bit = bit_scan_forward(hit_mask);
					i32 symbol_index = i + first_bit / 2;
					symbol = huffman.nonfast_symbols[symbol_index];
					code_size = huffman.nonfast_size[symbol_index];
					match = true;
//					if (symbol_index - i > 1) console_print_verbose("diff=%d, i=%d, symbol_index=%d\n", symbol_index - i, i, symbol_index);
					break;
				}
			}
			DUMMY_STATEMENT;
#endif
			if (!match) {
//				dump_block(compressed, compressed_size);
				console_print_error("Error: isyntax_hulsken_decompress(): error decoding Huffman message (unknown symbol)\n");
				ASSERT(!"unknown symbol");
				memset(out_buffer, 0, coeff_buffer_size);
				release_temp_memory(&temp_memory);
				return false;
			}
		}

		if (code_size == 0) code_size = 1; // handle special case of the 'empty' Huffman tree (root node is leaf node)

		blob >>= code_size;
		bits_read += code_size;

		// Handle run-length encoding of zeroes
		if (symbol == zerorun_symbol) {
			u32 numzeroes = blob & zero_counter_mask;
			bits_read += zero_counter_size;
			// A 'zero run' with length of zero means that this is not a zero run after all, but rather
			// the 'escaped' zero run symbol itself which should be outputted.
			if (numzeroes > 0) {
				u32 actual_numzeroes = (compressor_version == 2) ? numzeroes + 1 : numzeroes; // v2 stores actual count minus one
				if (decompressed_length + actual_numzeroes >= serialized_length || bits_read >= block_size_in_bits) {
					// Reached the end, terminate
					memset(decompressed_buffer + decompressed_length, 0, MIN(serialized_length - decompressed_length, actual_numzeroes));
					decompressed_length += actual_numzeroes;
					break;
				}
				// If the next Huffman symbol is also the zero run symbol, then their counters actually refer to the same zero run.
				// Basically, each extra zero run symbol expands the 'zero counter' bit depth, i.e.:
				//   n zero symbols -> depth becomes n * counter_bits
				u32 total_zero_counter_size = zero_counter_size;
				for(;;) {
					// Peek ahead in the bitstream, grab any additional zero run symbols, and recalculate numzeroes.
					blob = bitstream_lsb_read(compressed, bits_read);
					u32 next_code = (blob & zerorun_code_mask);
					if (next_code == zerorun_code) {
						// The zero run continues
						blob >>= zerorun_code_size;
						u32 counter_extra_bits = blob & zero_counter_mask;
						numzeroes <<= zero_counter_size;
						numzeroes |= (counter_extra_bits);
						total_zero_counter_size += zero_counter_size;
						bits_read += zerorun_code_size + zero_counter_size;
						actual_numzeroes = (compressor_version == 2) ? numzeroes + 1 : numzeroes; // v2 stores actual count minus one
						if (decompressed_length + actual_numzeroes >= serialized_length || bits_read >= block_size_in_bits) {
							break; // Reached the end, terminate
						}
					} else {
						actual_numzeroes = (compressor_version == 2) ? numzeroes + 1 : numzeroes; // v2 stores actual count minus one
						break; // no next zero run symbol, the zero run is finished
					}
				}

				i32 bytes_to_write = MIN(serialized_length - decompressed_length, actual_numzeroes);
				ASSERT(bytes_to_write > 0);
				memset(decompressed_buffer + decompressed_length, 0, bytes_to_write);
				decompressed_length += actual_numzeroes;
			} else {
				// This is not a 'zero run' after all, but an escaped symbol. So output the symbol.
				decompressed_buffer[decompressed_length++] = symbol;
			}
		} else {
			decompressed_buffer[decompressed_length++] = symbol;
		}

	}

	if (serialized_length != decompressed_length) {
//		dump_block(compressed, compressed_size);
		console_print("iSyntax: decompressed size mismatch (size=%zu): expected %lld observed %d\n",
				 compressed_size, serialized_length, decompressed_length);
		ASSERT(!"size mismatch");
	}

	i32 bytes_per_bitplane = (block_width * block_height) / 8;
	if (compressor_version == 1) {

		i32 bytes_per_sample = 2; // ((coeff_bit_depth+7)/8);
		i32 expected_bitmask_bits = (decompressed_length*8) / (block_width * block_height);

		// Try to deduce the number of coefficients without knowing the header information
		// NOTE: This is actually not necessary, because we do know coeff_count from the header.
		i32 extra_bits = (decompressed_length*8) % (block_width * block_height);
		if (extra_bits > 0) {
			if (coeff_count != 1 && extra_bits == 1*16) {
				coeff_count = 1;
			} else if (coeff_count != 3 && extra_bits == 3*16) {
				coeff_count = 3;
			}
			total_mask_bits = coeff_bit_depth * coeff_count;
		}

		// If there are empty bitplanes: bitmasks stored at end of data
		u64 expected_length = total_mask_bits * bytes_per_bitplane;
		if (decompressed_length < expected_length) {
			if (coeff_count == 1) {
				bitmasks[0] = read_u16_le(decompressed_buffer + decompressed_length - 2);
				total_mask_bits = popcount(bitmasks[0]);
			} else if (coeff_count == 3) {
				byte_pos = decompressed_buffer + decompressed_length - 6;
				bitmasks[0] = read_u16_le(byte_pos);
				bitmasks[1] = read_u16_le(byte_pos+2);
				bitmasks[2] = read_u16_le(byte_pos+4);
				total_mask_bits = popcount(bitmasks[0]) + popcount(bitmasks[1]) + popcount(bitmasks[2]);
			} else {
				fatal_error("invalid coeff_count");
			}
			expected_length = (total_mask_bits * block_width * block_height) / 8 + (coeff_count * 2);
			ASSERT(decompressed_length == expected_length);
		}
	}

	// unpack bitplanes
	i32 compressed_bitplane_index = 0;
	arena_align(temp_memory.arena, 32);
	u16* coeff_buffer = (u16*)arena_push_size(temp_memory.arena, coeff_buffer_size);
	memset(coeff_buffer, 0, coeff_buffer_size);
	memset(out_buffer, 0, coeff_buffer_size);

	{
		u32 running_bit_index = 0;
		i32 running_coeff_index = 0;
		u32 bitmasks_copy[3];
		memcpy(bitmasks_copy, bitmasks, sizeof(bitmasks));
		for (i32 bitplane_index = 0; bitplane_index < total_mask_bits; ++bitplane_index) {
			u8* bitplane = decompressed_buffer + (bitplane_index * bytes_per_bitplane);

			// horribly complicated 'for loop'-style iteration, needed because v1 and v2 store bitplanes in a different order
			// v1: iterate over the bitplanes (each coefficient separately)
			// v2: for each bitplane, alternate the coefficients (striped)
			// TODO: refactor?
			if (compressor_version == 1) {
				// find next bit for current coeff
				for (;;) {
					if (running_coeff_index >= coeff_count) {
						fatal_error("too many bitplanes");
					}
					u16 bitmask = bitmasks_copy[running_coeff_index];
					if (bitmask) {
						running_bit_index = bit_scan_forward(bitmask);
						ASSERT(running_bit_index < 16);
						bitmasks_copy[running_coeff_index] &= ~(1 << running_bit_index);
						break; // success
					} else {
						++running_coeff_index;
					}
				}
			} else {
				// compressor version 2: alternating coefficients
				for (;;) {
					if (running_bit_index >= 16) {
						fatal_error("too many bitplanes");
					}
					if (running_coeff_index < coeff_count) {
						u16 bitmask = bitmasks_copy[running_coeff_index];
						if (bitmask & (1 << running_bit_index)) {
							bitmasks_copy[running_coeff_index] &= ~(1 << running_bit_index);
							break; // success
						} else {
							++running_coeff_index; // keep looking
						}
					} else {
						running_coeff_index = 0;
						++running_bit_index; // keep looking
					}
				}
			}
			// Now we figured out which coeff and bit number this bitplane belongs to

			u16* current_coeff_buffer = coeff_buffer + (running_coeff_index * (block_width * block_height));
			u16* current_out_buffer = (u16*)out_buffer + (running_coeff_index * (block_width * block_height));

			// Do the bitplane unpacking
			for (i32 i = 0; i < block_width * block_height; i += 8) {
				i32 j = i/8;
				// The order bitplanes are stored in depends on the compressor version
				i32 shift_amount;
				if (compressor_version == 1) {
					shift_amount = (running_bit_index == 0) ? 15 : running_bit_index - 1; // bitplanes are stored sign, lsb ... msb
				} else {
					shift_amount = 15 - running_bit_index; // bitplanes are stored sign, msb ... lsb
				}
				u8 b = bitplane[j];
				if (b == 0) continue;
#if !defined(__SSE2__)
				// Non-SIMD version
					current_coeff_buffer[i+0] |= ((b >> 0) & 1) << shift_amount;
					current_coeff_buffer[i+1] |= ((b >> 1) & 1) << shift_amount;
					current_coeff_buffer[i+2] |= ((b >> 2) & 1) << shift_amount;
					current_coeff_buffer[i+3] |= ((b >> 3) & 1) << shift_amount;
					current_coeff_buffer[i+4] |= ((b >> 4) & 1) << shift_amount;
					current_coeff_buffer[i+5] |= ((b >> 5) & 1) << shift_amount;
					current_coeff_buffer[i+6] |= ((b >> 6) & 1) << shift_amount;
					current_coeff_buffer[i+7] |= ((b >> 7) & 1) << shift_amount;
#else
				// This SIMD implementation is ~20% faster compared to the simple version above.
				// Can it be made faster?
				__m128i* dst = (__m128i*) (current_coeff_buffer+i);
				uint64_t t = bswap_64(((0x8040201008040201ULL*b) & 0x8080808080808080ULL) >> 7);
				__m128i v_t = _mm_set_epi64x(0, t);
				__m128i array_of_bools = _mm_unpacklo_epi8(v_t, _mm_setzero_si128());
				__m128i masks = _mm_slli_epi16(array_of_bools, shift_amount);
				__m128i result = _mm_or_si128(*dst, masks);
				*dst = result;
#endif
				DUMMY_STATEMENT;
			}

			// finish iterating: basically, this is the '++i' part of the 'for loop' that got complicated because
			// the v1 and v2 compressors store bitplanes in a different order
			// TODO: refactor?
			if (compressor_version == 2) {
				++running_coeff_index;
			}
		}
	}

	// Reshuffle 4x2 snake-order and convert signed magnitude to twos complement
	for (i32 coeff_index = 0; coeff_index < coeff_count; ++coeff_index) {
		u16 bitmask = bitmasks[coeff_index];
		u16* current_coeff_buffer = coeff_buffer + (coeff_index * (block_width * block_height));
		u16* current_out_buffer = (u16*)out_buffer + (coeff_index * (block_width * block_height));
		if (bitmask > 0) {
			// Reshuffle snake-order
			i32 area_stride_x = block_width / 4;
			for (i32 area4x4_index = 0; area4x4_index < ((block_width * block_height) / 16); ++area4x4_index) {
				i32 area_base_index = area4x4_index * 16;
				i32 area_x = (area4x4_index % area_stride_x) * 4;
				i32 area_y = (area4x4_index / area_stride_x) * 4;

				u64 area_y0 = *(u64*)&current_coeff_buffer[area_base_index];
				u64 area_y1 = *(u64*)&current_coeff_buffer[area_base_index+4];
				u64 area_y2 = *(u64*)&current_coeff_buffer[area_base_index+8];
				u64 area_y3 = *(u64*)&current_coeff_buffer[area_base_index+12];

				*(u64*)(current_out_buffer + (area_y + 0) * block_width + area_x) = area_y0;
				*(u64*)(current_out_buffer + (area_y + 1) * block_width + area_x) = area_y1;
				*(u64*)(current_out_buffer + (area_y + 2) * block_width + area_x) = area_y2;
				*(u64*)(current_out_buffer + (area_y + 3) * block_width + area_x) = area_y3;
			}

			// Convert signed magnitude to twos complement (ex. 0x8002 becomes -2)
			signed_magnitude_to_twos_complement_16_block(current_out_buffer, block_width * block_height);
		}
	}

	release_temp_memory(&temp_memory); // frees coeff_buffer and decompressed_buffer
	return true;
}

static inline i32 get_first_valid_coef_pixel(i32 scale) {
	i32 result = (PER_LEVEL_PADDING << scale) - (PER_LEVEL_PADDING - 1);
	return result;
}

static inline i32 get_first_valid_ll_pixel(i32 scale) {
	i32 result = get_first_valid_coef_pixel(scale) + (1 << scale);
	return result;
}

i32 isyntax_get_chunk_codeblocks_per_color_for_level(i32 level, bool has_ll) {
	i32 rel_level = level % 3;
	i32 codeblock_count;
	if (rel_level == 0) {
		codeblock_count = 1;
	} else if (rel_level == 1) {
		codeblock_count = 1 + 4;
	} else {
		codeblock_count = 1 + 4 + 16;
	}
	if (has_ll) ++codeblock_count;
	return codeblock_count;
}

// Dump codeblock info from block header to a .csv file
static void isyntax_dump_block_header(isyntax_image_t* wsi_image, const char* filename) {
	if (filename == NULL) {
		filename = "test_block_header.csv";
	}
	FILE* test_block_header_fp = fopen(filename, "wb");
	if (test_block_header_fp) {
		fprintf(test_block_header_fp, "x_coordinate,y_coordinate,color_component,scale,coefficient,block_data_offset,block_data_size,block_header_template_id\n");

		for (i32 i = 0; i < wsi_image->codeblock_count; i += 1/*21*3*/) {
			isyntax_codeblock_t* codeblock = wsi_image->codeblocks + i;
			fprintf(test_block_header_fp, "%d,%d,%d,%d,%d,%lld,%lld,%d\n",
//			        codeblock->x_adjusted,
//			        codeblock->y_adjusted,
			        codeblock->x_coordinate - wsi_image->offset_x,
			        codeblock->y_coordinate - wsi_image->offset_y,
			        codeblock->color_component,
			        codeblock->scale,
			        codeblock->coefficient,
			        codeblock->block_data_offset,
			        codeblock->block_size,
			        codeblock->block_header_template_id);

//			if (codeblock->scale == wsi_image->level_count-1) {
//				i+=3; // skip extra LL blocks;
//			}
		}

		fclose(test_block_header_fp);
	}
}

// Set the thread pool to submit parallel jobs to
// TODO(pvalkema): remove this? needs rethinking
void isyntax_set_thread_pool(isyntax_t* isyntax, thread_pool_t* thread_pool) {
	isyntax->work_submission_pool = thread_pool;
}


bool isyntax_open(isyntax_t* isyntax, const char* filename, enum libisyntax_open_flags_t flags) {

	console_print_verbose("Attempting to open iSyntax: %s\n", filename);
	ASSERT(isyntax);

	isyntax->open_flags = flags;

	int ret = 0; (void)ret;
	file_stream_t fp = file_stream_open_for_reading(filename);
	bool success = false;

	char* read_buffer = NULL;
	isyntax_seektable_codeblock_header_t* seektable = NULL;
	isyntax_data_chunk_t* data_chunks_memory = NULL;

	if (0) { failed:
		if (fp) file_stream_close(fp);
		if (read_buffer != NULL) free(read_buffer);
		if (seektable != NULL) free(seektable);
		isyntax_image_t* wsi_image = isyntax->images + isyntax->wsi_image_index;
		if (wsi_image->image_type == ISYNTAX_IMAGE_TYPE_WSI) {
			free(wsi_image->data_chunks);
			wsi_image->data_chunks = NULL;
			for (i32 i = 0; i < wsi_image->level_count; ++i) {
				isyntax_level_t* level = wsi_image->levels + i;
				free(level->tiles);
				level->tiles = NULL;
			}
		}
		if (isyntax->open_flags & LIBISYNTAX_OPEN_FLAG_READ_BARCODE_ONLY && isyntax->is_barcode_read) {
			success = true; // Aborted early on purpose for fast barcode reading.
		}
		return success;
	}

	if (fp) {
		i64 filesize = file_stream_get_filesize(fp);
		if (filesize > 0) {
			isyntax->filesize = filesize;

			// https://www.openpathology.philips.com/wp-content/uploads/isyntax/4522%20207%2043941_2020_04_24%20Pathology%20iSyntax%20image%20format.pdf
			// Layout of an iSyntax file:
			// XML Header | End of Table (EOT) marker, 3 bytes "\r\n\x04" | Seektable (optional) | Codeblocks

			// Read the XML header
			// We don't know the length of the XML header, so we just read 'enough' data in a chunk and hope that we get it
			// (and if not, read some more data until we get it)

			init_timer(); // just in case this hasn't already been called once already
			i64 load_begin = get_clock();
			i64 io_begin = get_clock();
			i64 io_ticks_elapsed = 0;
			i64 parse_begin = get_clock();
			i64 parse_ticks_elapsed = 0;

			size_t read_size = MEGABYTES(1);
			read_buffer = malloc(read_size);
			size_t bytes_read = file_stream_read(read_buffer, read_size, fp);
			io_ticks_elapsed += (get_clock() - io_begin);

			if (bytes_read < 3) {
				goto failed;
			}
			bool are_there_bytes_left = (bytes_read == read_size);

			// find EOT candidates, 3 bytes "\r\n\x04"
			i64 header_length = 0;
			i64 isyntax_data_offset = 0; // Offset of either the Seektable, or the Codeblocks segment of the iSyntax file.
			i64 bytes_read_from_data_offset_in_last_chunk = 0;

			i32 chunk_index = 0;
			for (;; ++chunk_index) {
//				console_print_verbose("iSyntax: reading XML header chunk %d\n", chunk_index);
                i64 chunk_offset = chunk_index * (i64)read_size;
                i64 chunk_length = 0;
				bool match = false;
				char* pos = read_buffer;
				i64 marker_offset = 0;
				char* marker = (char*)memchr(read_buffer, '\x04', bytes_read);
				if (marker) {
                    marker_offset = marker - read_buffer;
					match = true;
					chunk_length = marker_offset;
					header_length += chunk_length;
					isyntax_data_offset = header_length + 1;
					i64 data_offset_in_last_chunk = marker_offset + 1;
					bytes_read_from_data_offset_in_last_chunk = (i64)bytes_read - data_offset_in_last_chunk;
				}
				if (match) {
					// We found the end of the XML header. This is the last chunk to process.
					if (!(header_length > 0 && header_length < isyntax->filesize)) {
						goto failed;
					}

					parse_begin = get_clock();
					if (!isyntax_parse_xml_header(isyntax, read_buffer, chunk_offset, chunk_length, true)) {
						goto failed;
					}
					parse_ticks_elapsed += (get_clock() - parse_begin);

//					console_print("iSyntax: the XML header is %u bytes, or %g%% of the total file size\n", header_length, (float)((float)header_length * 100.0f) / isyntax->filesize);
//					console_print("   I/O time: %g seconds\n", get_seconds_elapsed(0, io_ticks_elapsed));
//					console_print("   Parsing time: %g seconds\n", get_seconds_elapsed(0, parse_ticks_elapsed));
//					console_print("   Total loading time: %g seconds\n", get_seconds_elapsed(load_begin, get_clock()));
					break;
				} else {
					// We didn't find the end of the XML header. We need to read more chunks to find it.
					// (Or, we reached the end of the file unexpectedly, which is an error.)
					chunk_length = read_size;
					header_length += chunk_length;
					if (are_there_bytes_left) {

						parse_begin = get_clock();
						if (!isyntax_parse_xml_header(isyntax, read_buffer, chunk_offset, chunk_length, false)) {
							goto failed;
						}
						parse_ticks_elapsed += (get_clock() - parse_begin);

						io_begin = get_clock();
						bytes_read = file_stream_read(read_buffer, read_size, fp); // read the next chunk
						io_ticks_elapsed += (get_clock() - io_begin);

						are_there_bytes_left = (bytes_read == read_size);
						continue;
					} else {
						console_print_error("iSyntax parsing error: didn't find the end of the XML header (unexpected end of file)\n");
						goto failed;
					}
				}
			}
			
			isyntax_image_t* wsi_image = isyntax->images + isyntax->wsi_image_index;
			if (wsi_image->mpp_x <= 0.0f || wsi_image->mpp_y <= 0.0f) {
				isyntax->mpp_x = 1.0f; // should usually be 0.25; zero or below can never be right
				isyntax->mpp_y = 1.0f;
				isyntax->is_mpp_known = false;
			}else{
				isyntax->mpp_x = wsi_image->mpp_x;
				isyntax->mpp_y = wsi_image->mpp_y;
				isyntax->is_mpp_known = true;
			}

			isyntax->bits_stored = isyntax->block_header_templates[0].bits_stored;
			isyntax->block_width = isyntax->block_header_templates[0].block_width;
			isyntax->block_height = isyntax->block_header_templates[0].block_height;
			isyntax->tile_width = isyntax->block_width * 2; // tile dimension AFTER inverse wavelet transform
			isyntax->tile_height = isyntax->block_height * 2;

			if (wsi_image->image_type == ISYNTAX_IMAGE_TYPE_WSI) {

				i32 block_width = isyntax->block_width;
				i32 block_height = isyntax->block_height;
				i32 tile_width = isyntax->tile_width;
				i32 tile_height = isyntax->tile_height;

				i32 num_levels = wsi_image->level_count;
				ASSERT(num_levels >= 1);
				i32 grid_width = ((wsi_image->width_including_padding + (block_width << num_levels) - 1) / (block_width << num_levels)) << (num_levels - 1);
				i32 grid_height = ((wsi_image->height_including_padding + (block_height << num_levels) - 1) / (block_height << num_levels)) << (num_levels - 1);

				u64 h_coeff_tile_count = 0; // number of tiles with LH/HL/HH coefficients
				i32 base_level_tile_count = grid_height * grid_width;
				for (i32 scale = 0; scale < wsi_image->level_count; ++scale) {
					isyntax_level_t* level = wsi_image->levels + scale;
					level->tile_count = base_level_tile_count >> (scale * 2);
					h_coeff_tile_count += level->tile_count;
					level->scale = scale;
					level->width_in_tiles = grid_width >> scale;
					level->height_in_tiles = grid_height >> scale;
					level->width = wsi_image->width >> scale;
					level->height = wsi_image->height >> scale;
					level->downsample_factor = (float)(1 << scale);
					level->um_per_pixel_x = isyntax->mpp_x * level->downsample_factor;
					level->um_per_pixel_y = isyntax->mpp_y * level->downsample_factor;
					level->x_tile_side_in_um = (float)tile_width * level->um_per_pixel_x;
					level->y_tile_side_in_um = (float)tile_height * level->um_per_pixel_y;
				}

				// When recursively decoding the tiles, at each iteration the image is slightly offset
				// to the top left.
				// The shift corresponds to the per level padding added for the wavelet transform:
				// ((3 << (scale-1)) - 2)
				// Put another way: the highest (zoomed out levels) are shifted the to the bottom right
				// (this is also reflected in the x and y coordinates of the codeblocks in the iSyntax header).
				// Level 0 has no
				for (i32 scale = 1; scale < wsi_image->level_count; ++scale) {
					isyntax_level_t* level = wsi_image->levels + scale;
					level->origin_offset_in_pixels = get_first_valid_coef_pixel(scale - 1);
					float offset_in_um_x = (float)level->origin_offset_in_pixels * wsi_image->levels[0].um_per_pixel_x;
					float offset_in_um_y = (float)level->origin_offset_in_pixels * wsi_image->levels[0].um_per_pixel_y;
					level->origin_offset = (v2f){offset_in_um_x, offset_in_um_y};
				}

				// The highest level has LL tiles in addition to LH/HL/HH tiles
				i64 ll_coeff_tile_count = base_level_tile_count >> ((num_levels - 1) * 2);
				i64 total_coeff_tile_count = h_coeff_tile_count + ll_coeff_tile_count;
				i64 total_codeblock_count = total_coeff_tile_count * 3; // for 3 color channels

				for (i32 i = 0; i < wsi_image->codeblock_count; ++i) {
					isyntax_codeblock_t* codeblock = wsi_image->codeblocks + i;

					// Calculate adjusted codeblock coordinates so that they fit the origin of the image
					codeblock->x_adjusted = (i32)codeblock->x_coordinate - wsi_image->offset_x;
					codeblock->y_adjusted = (i32)codeblock->y_coordinate - wsi_image->offset_y;

					// Calculate the block ID (= index into the seektable)
					// adapted from extract_block_header.py
					bool is_ll = codeblock->coefficient == 0;
					u32 block_id = 0;
					i32 maxscale = is_ll ? codeblock->scale + 1 : codeblock->scale;
					for (i32 scale = 0; scale < maxscale; ++scale) {
						block_id += wsi_image->levels[scale].tile_count;
					}

					i32 offset;
					if (is_ll) {
						offset = get_first_valid_ll_pixel(codeblock->scale);
					} else {
						offset = get_first_valid_coef_pixel(codeblock->scale);
					}
					i32 x = codeblock->x_adjusted - offset;
					i32 y = codeblock->y_adjusted - offset;
//					codeblock->x_adjusted = x;
//					codeblock->y_adjusted = y;
					codeblock->block_x = x / (tile_width << codeblock->scale);
					codeblock->block_y = y / (tile_height << codeblock->scale);

					i32 grid_stride = grid_width >> codeblock->scale;
					block_id += codeblock->block_y * grid_stride + codeblock->block_x;

					i32 tiles_per_color = total_coeff_tile_count;
					block_id += codeblock->color_component * tiles_per_color;
					codeblock->block_id = block_id;
				}

				io_begin = get_clock(); // for performance measurement
				file_stream_set_pos(fp, isyntax_data_offset);
				if (wsi_image->header_codeblocks_are_partial) {
					// The seektable is required to be present, because the block header table did not contain all information.
					u8 seektable_header_bytes[sizeof(isyntax_dicom_tag_header_t)] = {0};
					file_stream_read(seektable_header_bytes, sizeof(seektable_header_bytes), fp);
					isyntax_dicom_tag_header_t seektable_header_tag = isyntax_read_dicom_tag_header(seektable_header_bytes);

					io_ticks_elapsed += (get_clock() - io_begin);
					parse_begin = get_clock();

					if (seektable_header_tag.group == 0x301D && seektable_header_tag.element == 0x2015) {
						i32 seektable_size = seektable_header_tag.size;
						if (seektable_size < 0) {
							// We need to guess the size...
							ASSERT(wsi_image->codeblock_count > 0);
							seektable_size = sizeof(isyntax_seektable_codeblock_header_t) * wsi_image->codeblock_count;
						}
						seektable = (isyntax_seektable_codeblock_header_t*) malloc(seektable_size);
						file_stream_read(seektable, seektable_size, fp);

						// Now fill in the missing data.
						// NOTE: The number of codeblock entries in the seektable is much greater than the number of
						// codeblocks that *actually* exist in the file. This means that we have to discard many of
						// the seektable entries.
						// Luckily, we can easily identify the entries that need to be discarded.
						// (They have the data offset (and data size) set to 0.)
						i32 seektable_entry_count = seektable_size / sizeof(isyntax_seektable_codeblock_header_t);

						for (i32 i = 0; i < wsi_image->codeblock_count; ++i) {
							isyntax_codeblock_t* codeblock = wsi_image->codeblocks + i;
							if (codeblock->block_id > seektable_entry_count) {
								ASSERT(!"block ID out of bounds");
								goto failed;
							}
							u8* seektable_entry = (u8*)(seektable + codeblock->block_id);
							isyntax_dicom_tag_header_t block_data_offset_header = isyntax_read_dicom_tag_header(seektable_entry + offsetof(isyntax_seektable_codeblock_header_t, block_data_offset_header));
							ASSERT(block_data_offset_header.group == 0x301D);
							ASSERT(block_data_offset_header.element == 0x2010);
							codeblock->block_data_offset = isyntax_read_u64_le(seektable_entry + offsetof(isyntax_seektable_codeblock_header_t, block_data_offset));
							codeblock->block_size = isyntax_read_u64_le(seektable_entry + offsetof(isyntax_seektable_codeblock_header_t, block_size));

#if 0
							// Debug test:
							// Decompress codeblocks in the seektable.
							if (1 || i == wsi_image->codeblock_count_per_color-1) {
								// Only parse 'non-empty'/'background' codeblocks
								if (codeblock->block_size > 8 /*&& codeblock->block_data_offset == 129572464*/) {
									debug_read_codeblock_from_file(codeblock, fp);
									isyntax_header_template_t* template = isyntax->header_templates + codeblock->block_header_template_id;
									if (template->waveletcoeff != 3) {
										DUMMY_STATEMENT;
									}
									if (i % 1000 == 0) {
										console_print_verbose("reading codeblock %d\n", i);
									}
									i16* decompressed = isyntax_hulsken_decompress(codeblock, isyntax->block_width, isyntax->block_height, 1);
									if (decompressed) {
#if 0
										FILE* out = fopen("hulskendecompressed4.raw", "wb");
										if(out) {
											file_stream_write(decompressed, codeblock->decompressed_size, out);
											file_stream_close(out);
										}
#endif
										_aligned_free(decompressed);
									}
								}
							}
#endif

						}
						free(seektable);
						seektable = NULL;

//						isyntax_dump_block_header(wsi_image, "test_block_header.csv");

						// Allocate enough space for the maximum number of codeblock 'chunks' we can expect
						// (the actual number of chunks may be lower, because some tiles might not exist)
						i32 max_possible_chunk_count = 0;
						for (i32 scale = 0; scale <= wsi_image->max_scale; ++scale) {
							if ((scale + 1) % 3 == 0 || scale == wsi_image->max_scale) {
								isyntax_level_t* level = wsi_image->levels + scale;
								max_possible_chunk_count += level->tile_count;
							}
						}
						wsi_image->data_chunks = (isyntax_data_chunk_t*) calloc(1, max_possible_chunk_count * sizeof(isyntax_data_chunk_t));

						// Create tables for spatial lookup of codeblocks and codeblock chunks from tile coordinates
						for (i32 i = 0; i < wsi_image->level_count; ++i) {
							isyntax_level_t* level = wsi_image->levels + i;
							// NOTE: Tile entry with codeblock_index == 0 will mean there is no codeblock for this tile (empty/background)
							level->tiles = (isyntax_tile_t*) calloc(1, level->tile_count * sizeof(isyntax_tile_t));
						}
						i32 current_chunk_codeblock_index = 0;
						i32 next_chunk_codeblock_index = 0;
						i32 current_data_chunk_index = 0;
						i32 next_data_chunk_index = 0;
						for (i32 i = 0; i < wsi_image->codeblock_count; ++i) {
							isyntax_codeblock_t* codeblock = wsi_image->codeblocks + i;
							if (codeblock->color_component != 0) {
								// Don't let color channels 1 and 2 overwrite what was already set
								i = next_chunk_codeblock_index; // skip ahead
								codeblock = wsi_image->codeblocks + i;
								if (i >= wsi_image->codeblock_count) break;
							}
							// Keep track of where we are in the 'chunk' of codeblocks
							if (i == next_chunk_codeblock_index) {
								// This codeblock is the top of a new chunk
								i32 chunk_codeblock_count_per_color;
								if (codeblock->scale == wsi_image->max_scale) {
									chunk_codeblock_count_per_color = isyntax_get_chunk_codeblocks_per_color_for_level(codeblock->scale, true);
								} else {
									chunk_codeblock_count_per_color = 21;
								}
								current_chunk_codeblock_index = i;
								next_chunk_codeblock_index = i + (chunk_codeblock_count_per_color * 3);
								current_data_chunk_index = next_data_chunk_index;
								if (current_data_chunk_index >= max_possible_chunk_count) {
									console_print_error("iSyntax: encountered too many data chunks\n");
									fatal_error();
								}

								isyntax_data_chunk_t* chunk = wsi_image->data_chunks + current_data_chunk_index;
								chunk->offset = codeblock->block_data_offset;
								// TODO: record cluster size here?
								chunk->top_codeblock_index = current_chunk_codeblock_index;
								chunk->codeblock_count_per_color = chunk_codeblock_count_per_color;
								chunk->scale = codeblock->scale;
								++wsi_image->data_chunk_count;
								++next_data_chunk_index;
							}
							isyntax_level_t* level = wsi_image->levels + codeblock->scale;
							i32 tile_index = codeblock->block_y * level->width_in_tiles + codeblock->block_x;
							ASSERT(tile_index < level->tile_count);
							level->tiles[tile_index].exists = true;
							level->tiles[tile_index].codeblock_index = i;
							level->tiles[tile_index].codeblock_chunk_index = current_chunk_codeblock_index;
							level->tiles[tile_index].data_chunk_index = current_data_chunk_index;

						}

						parse_ticks_elapsed += (get_clock() - parse_begin);
//						console_print("iSyntax: the seektable is %u bytes, or %g%% of the total file size\n", seektable_size, (float)((float)seektable_size * 100.0f) / isyntax->filesize);
//						console_print("   I/O time: %g seconds\n", get_seconds_elapsed(0, io_ticks_elapsed));
//						console_print("   Parsing time: %g seconds\n", get_seconds_elapsed(0, parse_ticks_elapsed));
						isyntax->loading_time = get_seconds_elapsed(load_begin, get_clock());
					} else {
						// seektable invalid
						goto failed;
					}
				} else if (isyntax->data_model_major_version >= 100) {
					// Create tables for spatial lookup of codeblocks and codeblock chunks from tile coordinates
					for (i32 i = 0; i < wsi_image->level_count; ++i) {
						isyntax_level_t* level = wsi_image->levels + i;
						// NOTE: Tile entry with codeblock_index == 0 will mean there is no codeblock for this tile (empty/background)
						level->tiles = (isyntax_tile_t*) calloc(1, level->tile_count * sizeof(isyntax_tile_t));
					}

					// TODO: refactor code duplication: this is (probably) mostly wrong!
					i32 current_chunk_codeblock_index = 0;
					i32 next_chunk_codeblock_index = 0;
					i32 current_data_chunk_index = 0;
					i32 next_data_chunk_index = 0;
					for (i32 i = 0; i < wsi_image->codeblock_count; ++i) {
						isyntax_codeblock_t* codeblock = wsi_image->codeblocks + i;
						if (codeblock->color_component != 0) {
							// Don't let color channels 1 and 2 overwrite what was already set
							i = next_chunk_codeblock_index; // skip ahead
							codeblock = wsi_image->codeblocks + i;
							if (i >= wsi_image->codeblock_count) break;
						}
						// Keep track of where we are in the 'chunk' of codeblocks
						if (i == next_chunk_codeblock_index) {
							// This codeblock is the top of a new chunk
							i32 chunk_codeblock_count_per_color;
							if (codeblock->scale == wsi_image->max_scale) {
								chunk_codeblock_count_per_color = isyntax_get_chunk_codeblocks_per_color_for_level(codeblock->scale, true);
							} else {
								chunk_codeblock_count_per_color = 21;
							}
							current_chunk_codeblock_index = i;
							next_chunk_codeblock_index = i + (chunk_codeblock_count_per_color * 3);
							current_data_chunk_index = next_data_chunk_index;
							if (current_data_chunk_index >= wsi_image->data_chunk_count) {
								console_print_error("iSyntax: encountered too many data chunks\n");
								fatal_error();
							}

							if (isyntax->data_model_major_version < 100) {
								isyntax_data_chunk_t* chunk = wsi_image->data_chunks + current_data_chunk_index;
								chunk->offset = codeblock->block_data_offset;
								// TODO: record cluster size here
								chunk->top_codeblock_index = current_chunk_codeblock_index;
								chunk->codeblock_count_per_color = chunk_codeblock_count_per_color;
								chunk->scale = codeblock->scale;
								++wsi_image->data_chunk_count;
							}

							++next_data_chunk_index;
						}
						isyntax_level_t* level = wsi_image->levels + codeblock->scale;
						i32 tile_index = codeblock->block_y * level->width_in_tiles + codeblock->block_x;
						ASSERT(tile_index < level->tile_count);
						level->tiles[tile_index].exists = true;
						level->tiles[tile_index].codeblock_index = i;
						level->tiles[tile_index].codeblock_chunk_index = current_chunk_codeblock_index;
						level->tiles[tile_index].data_chunk_index = current_data_chunk_index;

					}

//					isyntax_dump_block_header(wsi_image, "test_block_header.csv");

					parse_ticks_elapsed += (get_clock() - parse_begin);
//				    console_print("iSyntax: the seektable is %u bytes, or %g%% of the total file size\n", seektable_size, (float)((float)seektable_size * 100.0f) / isyntax->filesize);
//					console_print("   I/O time: %g seconds\n", get_seconds_elapsed(0, io_ticks_elapsed));
//					console_print("   Parsing time: %g seconds\n", get_seconds_elapsed(0, parse_ticks_elapsed));
					isyntax->loading_time = get_seconds_elapsed(load_begin, get_clock());
				} else {
					// non-partial header blocks are not supported
					goto failed;
				};
			} else {
				// non-WSI images are not supported
				goto failed;
			}

			size_t ll_coeff_block_size = isyntax->block_width * isyntax->block_height * sizeof(icoeff_t);
			size_t block_allocator_maximum_capacity_in_blocks = GIGABYTES(32) / ll_coeff_block_size;
			size_t ll_coeff_block_allocator_capacity_in_blocks = block_allocator_maximum_capacity_in_blocks / 4;
			size_t h_coeff_block_size = ll_coeff_block_size * 3;
			size_t h_coeff_block_allocator_capacity_in_blocks = ll_coeff_block_allocator_capacity_in_blocks * 3;
			if (flags & LIBISYNTAX_OPEN_FLAG_INIT_ALLOCATORS) {
				isyntax->ll_coeff_block_allocator = malloc(sizeof(block_allocator_t));
				isyntax->h_coeff_block_allocator = malloc(sizeof(block_allocator_t));
				block_allocator_init(isyntax->ll_coeff_block_allocator, ll_coeff_block_size, ll_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
				block_allocator_init(isyntax->h_coeff_block_allocator, h_coeff_block_size, h_coeff_block_allocator_capacity_in_blocks, MEGABYTES(256));
				isyntax->is_block_allocator_owned = true;
			} else {
				// The caller must inject the allocators after return of isyntax_open().
				isyntax->ll_coeff_block_allocator = NULL;
				isyntax->h_coeff_block_allocator = NULL;
				isyntax->is_block_allocator_owned = false;
			}

			// Initialize dummy blocks with 'background' coefficients, to use for filling in margins at the edges (in case the neighboring codeblock doesn't exist)
			if (!isyntax->black_dummy_coeff) {
				isyntax->black_dummy_coeff = (icoeff_t*)calloc(1, isyntax->block_width * isyntax->block_height * sizeof(icoeff_t));
			}
			if (!isyntax->white_dummy_coeff) {
				isyntax->white_dummy_coeff = (icoeff_t*)malloc(isyntax->block_width * isyntax->block_height * sizeof(icoeff_t));
				for (i32 i = 0; i < isyntax->block_width * isyntax->block_height; ++i) {
					isyntax->white_dummy_coeff[i] = 255;
				}
			}

			success = true;

			free(read_buffer);

			// Populate debug info.
			for (int scale = 0; scale < wsi_image->level_count; ++scale) {
				isyntax_level_t* level = &wsi_image->levels[scale];
				for (int tile_y = 0; tile_y < level->height_in_tiles; ++tile_y) {
					for (int tile_x = 0; tile_x < level->width_in_tiles; ++tile_x) {
						isyntax_tile_t* tile = &level->tiles[level->width_in_tiles * tile_y + tile_x];
						tile->tile_scale = scale;
						tile->tile_x = tile_x;
						tile->tile_y = tile_y;
					}
				}
			}
		}
		file_stream_close(fp);

		if (success) {
			isyntax->file_handle = open_file_handle_for_simultaneous_access(filename);
			if (!isyntax->file_handle) {
				console_print_error("Error: Could not reopen file for asynchronous I/O\n");
				success = false;
			}
		}

		if (success) {
			isyntax_envelopes_to_rect(isyntax);
		}
	}
	return success;
}

void isyntax_destroy(isyntax_t* isyntax) {
    // TODO(pvalkema): review synchronization needed to safely destroy the isyntax_t
    // NOTE: in isyntax_streamer.c, the refcount can be incremented in various places (while threaded jobs are running)
	while (isyntax->refcount > 0) {
		platform_sleep(1);
		if (isyntax->work_submission_pool) {
            thread_pool_do_work(isyntax->work_submission_pool);
		} else {
			static bool already_printed = false;
			if (!already_printed) {
				console_print_error("isyntax_destroy(): work_submission_pool not set; refcount = %d, waiting to reach 0\n", isyntax->refcount);
				already_printed = true;
			}
		}
	}
    if (isyntax->is_block_allocator_owned) {
        if (isyntax->ll_coeff_block_allocator->is_valid) {
            block_allocator_destroy(isyntax->ll_coeff_block_allocator);
        }
        if (isyntax->h_coeff_block_allocator->is_valid) {
            block_allocator_destroy(isyntax->h_coeff_block_allocator);
        }
    }
	if (isyntax->black_dummy_coeff) {
		free(isyntax->black_dummy_coeff);
		isyntax->black_dummy_coeff = NULL;
	}
	if (isyntax->white_dummy_coeff) {
		free(isyntax->white_dummy_coeff);
		isyntax->white_dummy_coeff = NULL;
	}
	if (isyntax->dicom_software_versions) {
		free(isyntax->dicom_software_versions);
		isyntax->dicom_software_versions = NULL;
	}
	if (isyntax->dicom_date_of_last_calibration) {
		free(isyntax->dicom_date_of_last_calibration);
		isyntax->dicom_date_of_last_calibration = NULL;
	}
	if (isyntax->dicom_time_of_last_calibration) {
		free(isyntax->dicom_time_of_last_calibration);
		isyntax->dicom_time_of_last_calibration = NULL;
	}
	for (i32 image_index = 0; image_index < isyntax->image_count; ++image_index) {
		isyntax_image_t* image = isyntax->images + image_index;
		if (image->image_type == ISYNTAX_IMAGE_TYPE_WSI) {
			if (image->codeblocks) {
				free(image->codeblocks);
				image->codeblocks = NULL;
			}
			if (image->data_chunks) {
				for (i32 i = 0; i < image->data_chunk_count; ++i) {
					isyntax_data_chunk_t* chunk = image->data_chunks + i;
					if (chunk->data) {
						free(chunk->data);
					}
				}
				free(image->data_chunks);
				image->data_chunks = NULL;
			}
			for (i32 i = 0; i < image->level_count; ++i) {
				isyntax_level_t* level = image->levels + i;
				if (level->tiles) {
#if 0
					for (i32 j = 0; j < level->tile_count; ++j) {
						isyntax_tile_t* tile = level->tiles + j;
						for (i32 color = 0; color < 3; ++color) {
							isyntax_tile_channel_t* channel = tile->color_channels + color;
							if (channel->coeff_ll) free(channel->coeff_ll);
							if (channel->coeff_h) free(channel->coeff_h);
						}
					}
#endif
					free(level->tiles);
					level->tiles = NULL;
				}
			}
		}
	}
	if (isyntax->cache) {
		libisyntax_cache_destroy(isyntax->cache);
	}
	file_handle_close(isyntax->file_handle);
}

// for qsort
static int compare_vector_y (const void* a, const void* b) {
	return (int)( ((v2i*)a)->y - ((v2i*)b)->y );
}

void isyntax_envelopes_to_rect(isyntax_t* isyntax){
	// converts the valid data envelopes into an array of simple polygons

	// iSyntax v1 does not contain envelopes, construct dummy envelope
	if(isyntax->valid_data_envelope_count == 0 && isyntax->image_count > 0){
		// Can i assume a WSI image exists? Even if not, I think it defaults iniatializes to 0...
		isyntax_image_t* wsi_image = isyntax->images + isyntax->wsi_image_index;

		rect2i rectangle;
		rectangle.x = 0;
		rectangle.y = 0;
		rectangle.h = wsi_image->height;
		rectangle.w = wsi_image->width;
		
		isyntax->valid_data_envelopes_rectangles[isyntax->valid_data_envelope_rectangle_count] = rectangle;
		isyntax->valid_data_envelope_rectangle_count += 1;
		return;
	}

	bool error = false;
	for (i32 i=0; i < isyntax->valid_data_envelope_count; ++i) {
		isyntax_valid_data_envelope_t* envelope = isyntax->valid_data_envelopes + i;
		// invalid/empty envelope
		if(envelope->vertex_count <= 1){continue;};

		// a rectangle contains 4 vertices, so worst case scenario you need 4x as much memory as rectangles.
		// I think this is so unlikely we can reasonably keep a smaller footprint
		v2i previous_vertices[COUNT(isyntax->valid_data_envelopes_rectangles)]; 
		v2i next_vertices[COUNT(isyntax->valid_data_envelopes_rectangles)];
		i32 previous_vertices_count = 0;
		i32 next_vertices_count = 0;

		i32 current_x = envelope->vertices[0].x;
		i32 next_x = 0;
		i32 iter_v = 0;
		bool last_vertex = false;

		while (true) {
			// get all vectors at specific x coordinate
			next_vertices_count = 0;
			while (true) {
				if (iter_v >= envelope->vertex_count){
					last_vertex = true;
					break;
				} else if (envelope->vertices[iter_v].x == current_x){
					if (next_vertices_count >= COUNT(next_vertices)){
						console_print("iSyntax: insufficient memory space to store all vertices of the valid data envelopes.");
						error=true;
						break;
					}
					next_vertices[next_vertices_count] = envelope->vertices[iter_v];
					next_vertices_count += 1;
					iter_v += 1;
				} else {
					next_x = envelope->vertices[iter_v].x;
					break;
				}
			}

			
			// no vertices to combine
			if (previous_vertices_count == 0){
				memcpy(previous_vertices, next_vertices, sizeof(previous_vertices));
				previous_vertices_count = next_vertices_count;
				current_x = next_x;
				continue;
			}

			// build rectangles
			if (previous_vertices_count % 2 != 0){
				console_print("iSyntax: uneven vertices in envelope. Is this even possible?");
				error=true;
				break;
			}

			for (i32 j=0; j < previous_vertices_count; j += 2) {
				if (isyntax->valid_data_envelope_rectangle_count >= COUNT(isyntax->valid_data_envelopes_rectangles)){
					console_print("iSyntax: insufficient memory space to store all necessary rectangles of the valid data envelopes.");
					error=true;
					break;
				}

				rect2i rectangle;
				rectangle.x = previous_vertices[j].x;
				rectangle.y = previous_vertices[j].y;
				rectangle.h = previous_vertices[j+1].y - previous_vertices[j].y;
				rectangle.w = current_x - previous_vertices[j].x;

				isyntax->valid_data_envelopes_rectangles[isyntax->valid_data_envelope_rectangle_count] = rectangle;
				isyntax->valid_data_envelope_rectangle_count += 1;
			}

			// identify and remove the overlapping vertexes, effectively: previous_vertices XOR next_vertices. 
			// note: repeating vertices are theorically possible and shoulnt be removed for just being a duplicate
			v2i temp_vertices[COUNT(isyntax->valid_data_envelopes_rectangles)];
			i32 temp_vertices_count = 0;
			bool passed_vertices[COUNT(isyntax->valid_data_envelopes_rectangles)] = {false};
			for (i32 j=0; j < next_vertices_count; ++j) {
				bool identified = false;
				for (i32 k=0; k < previous_vertices_count; ++k) {
					if ((previous_vertices[k].y == next_vertices[j].y) && (!passed_vertices[k])){
						identified = true;
						passed_vertices[k] = true;
						break;
					}
				}
				if (!identified){
					temp_vertices[temp_vertices_count] = next_vertices[j];
					temp_vertices_count += 1;
				}
			}
			for (i32 j=0; j < previous_vertices_count; ++j) {
				if(!passed_vertices[j]){
					v2i temp_vertex;
					temp_vertex.x = current_x;
					temp_vertex.y = previous_vertices[j].y;
					temp_vertices[temp_vertices_count] = temp_vertex;
					temp_vertices_count += 1;
				}
			}
			
			// sort from low-to-highest y value
			// when sorted two sequential vertices will form one edge of a rectangle
			qsort(temp_vertices, temp_vertices_count, sizeof(temp_vertices[0]), compare_vector_y);
			memcpy(previous_vertices, temp_vertices, sizeof(previous_vertices));
			previous_vertices_count = temp_vertices_count;

			// nothing left to parse
			if (last_vertex || error) {
				break;
			}

			// instantiate next step
			current_x = next_x;
		}
	}
}
