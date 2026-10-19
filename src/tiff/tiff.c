/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2023  Pieter Valkema

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

#include "lz4.h"

#include "tiff.h"
#include "tile_buffer_pool.h"
#include "tif_lzw.h"
#include "remote.h"
#include "jpeg_decoder.h"

u32 get_tiff_field_size(u16 data_type) {
	u32 size = 0;
	switch(data_type) {
		default:
			console_print("Warning: encountered a TIFF field with an unrecognized data type (%d)\n", data_type);
			break;
		case TIFF_UINT8: case TIFF_INT8: case TIFF_ASCII: case TIFF_UNDEFINED: size = 1; break;
		case TIFF_UINT16: case TIFF_INT16:                                     size = 2; break;
		case TIFF_UINT32: case TIFF_INT32: case TIFF_IFD: case TIFF_FLOAT:     size = 4; break;
		case TIFF_RATIONAL:	case TIFF_SRATIONAL:                               size = 8; break; // note: actually 2x4
		case TIFF_DOUBLE: case TIFF_UINT64: case TIFF_INT64: case TIFF_IFD8:   size = 8; break;
	}
	return size;
}

static void maybe_swap_tiff_field(void* field, u16 data_type, u64 data_count, bool32 is_big_endian) {
	if (DATA_ENDIAN_DIFFERS(is_big_endian)) {
		u32 field_size = get_tiff_field_size(data_type);
		if (field_size > 1) {
			// Some fields consist of two smaller field (RATIONAL, SRATIONAL), their components need to be swapped individually
			if (data_type == TIFF_RATIONAL || data_type == TIFF_SRATIONAL) {
				data_count *= 2;
				field_size = 4;
			}
			ASSERT(data_count * field_size <= 8); // we should be able to inline it
			u8* pos = (u8*) field;
			for (i32 i = 0; i < data_count; ++i, pos += field_size) {
				switch(field_size) {
					case 2: *(u16*)pos = bswap_16(*(u16*)pos); break;
					case 4: *(u32*)pos = bswap_32(*(u32*)pos); break;
					case 8: *(u64*)pos = bswap_64(*(u64*)pos); break;
					default: ASSERT(!"This field size should not exist");
				}
			}
		}
	}
}

const char* get_tiff_tag_name(u32 tag) {
	const char* result = "unrecognized tag";
	switch(tag) {
		case TIFF_TAG_NEW_SUBFILE_TYPE: result = "NewSubfileType"; break;
		case TIFF_TAG_IMAGE_WIDTH: result = "ImageWidth"; break;
		case TIFF_TAG_IMAGE_LENGTH: result = "ImageLength"; break;
		case TIFF_TAG_BITS_PER_SAMPLE: result = "BitsPerSample"; break;
		case TIFF_TAG_COMPRESSION: result = "Compression"; break;
		case TIFF_TAG_PHOTOMETRIC_INTERPRETATION: result = "PhotometricInterpretation"; break;
		case TIFF_TAG_FILL_ORDER: result = "FillOrder"; break;
		case TIFF_TAG_IMAGE_DESCRIPTION: result = "ImageDescription"; break;
        case TIFF_TAG_MAKE: result = "Make"; break;
        case TIFF_TAG_MODEL: result = "Model"; break;
		case TIFF_TAG_STRIP_OFFSETS: result = "StripOffsets"; break;
		case TIFF_TAG_ORIENTATION: result = "Orientation"; break;
		case TIFF_TAG_SAMPLES_PER_PIXEL: result = "SamplesPerPixel"; break;
		case TIFF_TAG_ROWS_PER_STRIP: result = "RowsPerStrip"; break;
		case TIFF_TAG_STRIP_BYTE_COUNTS: result = "StripByteCounts"; break;
		case TIFF_TAG_X_RESOLUTION: result = "XResolution"; break;
		case TIFF_TAG_Y_RESOLUTION: result = "YResolution"; break;
		case TIFF_TAG_X_POSITION: result = "XPosition"; break;
		case TIFF_TAG_Y_POSITION: result = "YPosition"; break;
		case TIFF_TAG_PLANAR_CONFIGURATION: result = "PlanarConfiguration"; break;
		case TIFF_TAG_RESOLUTION_UNIT: result = "ResolutionUnit"; break;
		case TIFF_TAG_PAGE_NUMBER: result = "PageNumber"; break;
		case TIFF_TAG_SOFTWARE: result = "Software"; break;
        case TIFF_TAG_DATETIME: result = "DateTime"; break;
        case TIFF_TAG_PREDICTOR: result = "Predictor"; break;
		case TIFF_TAG_WHITE_POINT: result = "WhitePoint"; break;
		case TIFF_TAG_PRIMARY_CHROMACITIES: result = "PrimaryChromacities"; break;
		case TIFF_TAG_TILE_WIDTH: result = "TileWidth"; break;
		case TIFF_TAG_TILE_LENGTH: result = "TileLength"; break;
		case TIFF_TAG_TILE_OFFSETS: result = "TileOffsets"; break;
		case TIFF_TAG_TILE_BYTE_COUNTS: result = "TileByteCounts"; break;
		case TIFF_TAG_SAMPLE_FORMAT: result = "SampleFormat"; break;
		case TIFF_TAG_S_MIN_SAMPLE_VALUE: result = "SMinSampleValue"; break;
		case TIFF_TAG_S_MAX_SAMPLE_VALUE: result = "SMaxSampleValue"; break;
		case TIFF_TAG_JPEG_TABLES: result = "JPEGTables"; break;
		case TIFF_TAG_YCBCRSUBSAMPLING: result = "YCbCrSubSampling"; break;
		case TIFF_TAG_REFERENCEBLACKWHITE: result = "ReferenceBlackWhite"; break;
		case TIFF_TAG_XMP: result = "XML"; break;
		case TIFF_TAG_IMAGE_DEPTH: result = "ImageDepth"; break;
		case TIGG_TAG_ICC_PROFILE_OFFSET: result = "ICCProfileOffset"; break;

        // NDPI tags
        case NDPI_TAG_SOURCE_LENS: result = "[NDPI] SourceLens"; break;
        case NDPI_TAG_X_OFFSET_FROM_SLIDE_CENTRE: result = "[NDPI] XOffsetFromSlideCentre"; break;
        case NDPI_TAG_Y_OFFSET_FROM_SLIDE_CENTRE: result = "[NDPI] YOffsetFromSlideCentre"; break;
        case NDPI_TAG_OPTIMISATION_FILE: result = "[NDPI] OptimisationFile"; break;
        case NDPI_TAG_REFERENCE: result = "[NDPI] Reference"; break;
		default: break;
	}
	return result;
}

static size_t tiff_read(tiff_t* tiff, void* dest, size_t size) {
	size_t read = source_file_read_at_offset(dest, &tiff->file, tiff->read_cursor, size);
	tiff->read_cursor += read;
	return read;
}

char* tiff_read_field_ascii(tiff_t* tiff, tiff_tag_t* tag) {
	size_t description_length = tag->data_count;
	char* result = (char*) calloc(ATLEAST(8, description_length + 1), 1);
	if (tag->data_is_offset) {
		source_file_read_at_offset(result, &tiff->file, tag->offset, tag->data_count);
	} else {
		memcpy(result, tag->data, description_length);
	}
	return result;
}

static inline void* tiff_read_field_undefined(tiff_t* tiff, tiff_tag_t* tag) {
	return (void*) tiff_read_field_ascii(tiff, tag);
}

// Read integer values in a TIFF tag (either 8, 16, 32, or 64 bits wide) + convert them to little-endian u64 if needed
u64* tiff_read_field_integers(tiff_t* tiff, tiff_tag_t* tag) {
	u64* integers = NULL;

	u64 bytesize = get_tiff_field_size(tag->data_type);
	u64 read_size = tag->data_count * bytesize;
	if (tag->data_is_offset) {
		void* temp_integers = calloc(bytesize, tag->data_count);
		if (source_file_read_at_offset(temp_integers, &tiff->file, tag->offset, read_size) != read_size) {
			free(temp_integers);
			return NULL; // failed
		}

		if (bytesize == 8) {
			// the numbers are already 64-bit, no need to widen
			integers = (u64*) temp_integers;
			if (DATA_ENDIAN_DIFFERS(tiff->is_big_endian)) {
				for (i32 i = 0; i < tag->data_count; ++i) {
					integers[i] = bswap_64(integers[i]);
				}
			}
		} else {
			// offsets are 32-bit or less -> widen to 64-bit offsets
			integers = (u64*) malloc(tag->data_count * sizeof(u64));
			switch(bytesize) {
				case 4: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = maybe_swap_32(((u32*) temp_integers)[i], tiff->is_big_endian);
					}
				} break;
				case 2: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = maybe_swap_16(((u16*) temp_integers)[i], tiff->is_big_endian);
					}
				} break;
				case 1: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = ((u8*) temp_integers)[i];
					}
				} break;
				default: {
					free(temp_integers);
					free(integers);
					return NULL; // failed (other bytesizes than the above shouldn't exist)
				}
			}
			free(temp_integers);
		}
		// all done!

	} else {
		// data is inlined
		integers = (u64*) calloc(1, sizeof(u64) * tag->data_count);
		for (i32 i = 0; i < tag->data_count; ++i) {
			memcpy(integers + i, tag->data + (i * bytesize), bytesize);
		}
	}

	return integers;
}

u16* tiff_read_field_u16(tiff_t* tiff, tiff_tag_t* tag) {
	u16* integers = NULL;

	u64 bytesize = get_tiff_field_size(tag->data_type);
	if (bytesize == sizeof(u16)) {
		integers = (u16*)calloc(bytesize, tag->data_count);

		u64 read_size = tag->data_count * bytesize;
		if (tag->data_is_offset) {
			if (source_file_read_at_offset(integers, &tiff->file, tag->offset, read_size) != read_size) {
				free(integers);
				return NULL; // failed
			}
			// Endian-swap if necessary
			if (DATA_ENDIAN_DIFFERS(tiff->is_big_endian)) {
				for (i32 i = 0; i < tag->data_count; ++i) {
					integers[i] = bswap_16((integers)[i]);
				}
			}
		} else {
			// data is inlined, and already in the right endian order
			memcpy(integers, (u16*)tag->data, read_size);
		}
	}

	return integers;
}

u8* tiff_read_field_u8(tiff_t* tiff, tiff_tag_t* tag) {
	u8* integers = NULL;

	u64 bytesize = get_tiff_field_size(tag->data_type);
	if (bytesize == sizeof(u8)) {
		integers = (u8*)calloc(bytesize, tag->data_count);

		u64 read_size = tag->data_count * bytesize;
		if (tag->data_is_offset) {
			if (source_file_read_at_offset(integers, &tiff->file, tag->offset, read_size) != read_size) {
				free(integers);
				return NULL; // failed
			}
		} else {
			// data is inlined
			memcpy(integers, (u8*)tag->data, read_size);
		}
	}

	return integers;
}


tiff_rational_t* tiff_read_field_rationals(tiff_t* tiff, tiff_tag_t* tag) {
	tiff_rational_t* rationals = (tiff_rational_t*) calloc(ATLEAST(8, tag->data_count * sizeof(tiff_rational_t)), 1);

	if (tag->data_is_offset) {
		source_file_read_at_offset(rationals, &tiff->file, tag->offset, tag->data_count * sizeof(tiff_rational_t));
	} else {
		// data is inlined
		rationals[0] = *(tiff_rational_t*) &tag->data_u64;
	}

	if (DATA_ENDIAN_DIFFERS(tiff->is_big_endian)) {
		for (i32 i = 0; i < tag->data_count; ++i) {
			tiff_rational_t* rational = rationals + i;
			rational->a = bswap_32(rational->a);
			rational->b = bswap_32(rational->b);
		}
	}

	return rationals;
}

// read only one rational
tiff_rational_t tiff_read_field_rational(tiff_t* tiff, tiff_tag_t* tag) {
	tiff_rational_t result = {0};
	if (tag->data_is_offset) {
		tiff_rational_t* rational = tiff_read_field_rationals(tiff, tag);
		ASSERT(rational);
		result = *rational;
		free(rational);
	} else {
		result = *(tiff_rational_t*) &tag->data_u64;
	}
	return result;
}

double tiff_rational_to_float(tiff_rational_t rational) {
	float result = ((double)(u32)rational.a) / ((double)(u32)rational.b);
	return result;
}

tiff_rational_t float_to_tiff_rational(double x) {

	// Source: https://www.ics.uci.edu/~eppstein/numth/frap.c

	i64 maxden = INT32_MAX / 4; // to be safe, don't use all bits
	i64 startx = x;
	i64 m[2][2];
	i64 ai;

	/* initialize matrix */
	m[0][0] = m[1][1] = 1;
	m[0][1] = m[1][0] = 0;

	/* loop finding terms until denom gets too big */
	while (m[1][0] *  ( ai = (i64)x ) + m[1][1] <= maxden) {
		long t;
		t = m[0][0] * ai + m[0][1];
		m[0][1] = m[0][0];
		m[0][0] = t;
		t = m[1][0] * ai + m[1][1];
		m[1][1] = m[1][0];
		m[1][0] = t;
		if(x==(double)ai) break;     // AF: division by zero
		x = 1/(x - (double) ai);
		if(x>(double)0x7FFFFFFF) break;  // AF: representation failure
	}

	/* now remaining x is between 0 and 1/ai */
	/* approx as either 0 or 1/m where m is max that will fit in maxden */
	/* first try zero */
//	console_print_verbose("%ld/%ld, error = %e\n", m[0][0], m[1][0], startx - ((double) m[0][0] / (double) m[1][0]));

	/* now try other possibility */
	/*ai = (maxden - m[1][1]) / m[1][0];
	m[0][0] = m[0][0] * ai + m[0][1];
	m[1][0] = m[1][0] * ai + m[1][1];
	console_print_verbose("%ld/%ld, error = %e\n", m[0][0], m[1][0],
	       startx - ((double) m[0][0] / (double) m[1][0]));*/

	tiff_rational_t result = {m[0][0], m[1][0]};
	return result;
}

bool tiff_read_ifd(tiff_t* tiff, tiff_ifd_t* ifd, u64* next_ifd_offset) {
	bool is_bigtiff = tiff->is_bigtiff;
	bool is_big_endian = tiff->is_big_endian;

	// By default, assume RGB color space.
	// (although TIFF files are always required to specify this in the PhotometricInterpretation tag)
	ifd->color_space = TIFF_PHOTOMETRIC_RGB;

	// Set the file position to the start of the IFD
	if (!(next_ifd_offset != NULL && *next_ifd_offset <= tiff->file.size)) {
		return false; // failed
	}

	tiff->read_cursor = *next_ifd_offset;
	u64 tag_count = 0;
	u64 tag_count_num_bytes = is_bigtiff ? 8 : 2;
	if (tiff_read(tiff, &tag_count, tag_count_num_bytes) != tag_count_num_bytes) return false;
	tag_count = is_bigtiff ? maybe_swap_64(tag_count, is_big_endian) : maybe_swap_16(tag_count, is_big_endian);

	// Read the tags
	u64 tag_size = is_bigtiff ? 20 : 12;
	u64 bytes_to_read = tag_count * tag_size;
	if (bytes_to_read > tiff->filesize) {
		return false; // sanity check
	}
	u8* raw_tags = (u8*) malloc(bytes_to_read);
	if (tiff_read(tiff, raw_tags, bytes_to_read) != bytes_to_read) {
		free(raw_tags);
		return false; // failed
	}

	// Restructure the fields so we don't have to worry about the memory layout, endianness, etc
	tiff_tag_t* tags = (tiff_tag_t*) calloc(sizeof(tiff_tag_t) * tag_count, 1);
	for (i32 i = 0; i < tag_count; ++i) {
		tiff_tag_t* tag = tags + i;
		if (is_bigtiff) {
			raw_bigtiff_tag_t* raw = (raw_bigtiff_tag_t*)raw_tags + i;
			tag->code = maybe_swap_16(raw->code, is_big_endian);
			tag->data_type = maybe_swap_16(raw->data_type, is_big_endian);
			tag->data_count = maybe_swap_64(raw->data_count, is_big_endian);

			u32 field_size = get_tiff_field_size(tag->data_type);
			u64 data_size = field_size * tag->data_count;
			if (data_size <= 8) {
				// Data fits in the tag so it is inlined
				memcpy(tag->data, raw->data, 8);
				maybe_swap_tiff_field(tag->data, tag->data_type, tag->data_count, is_big_endian);
				tag->data_is_offset = false;
			} else {
				// Data doesn't fit in the tag itself, so it's an offset
				tag->offset = maybe_swap_64(raw->offset, is_big_endian);
				tag->data_is_offset = true;
			}
		} else {
			// Standard TIFF
			raw_tiff_tag_t* raw = (raw_tiff_tag_t*)raw_tags + i;
			tag->code = maybe_swap_16(raw->code, is_big_endian);
			tag->data_type = maybe_swap_16(raw->data_type, is_big_endian);
			tag->data_count = maybe_swap_32(raw->data_count, is_big_endian);

			u32 field_size = get_tiff_field_size(tag->data_type);
			u64 data_size = field_size * tag->data_count;
			if (data_size <= 4) {
				// Data fits in the tag so it is inlined
				memcpy(tag->data, raw->data, 4);
				maybe_swap_tiff_field(tag->data, tag->data_type, tag->data_count, is_big_endian);
				tag->data_is_offset = false;
			} else {
				// Data doesn't fit in the tag itself, so it's an offset
				tag->offset = maybe_swap_32(raw->offset, is_big_endian);
				tag->data_is_offset = true;
			}
		}
	}
	free(raw_tags);

	// Read and interpret the entries in the IFD
	for (i32 tag_index = 0; tag_index < tag_count; ++tag_index) {
		tiff_tag_t* tag = tags + tag_index;
		if (is_verbose_mode) {
			console_print_verbose("tag %2d: %30s - code=%d, data_type=%2d, count=%5llu, %s=%llu\n",
						 tag_index, get_tiff_tag_name(tag->code), tag->code, tag->data_type, tag->data_count, tag->data_is_offset ? "offset" : "  data",tag->offset);
		}
		switch(tag->code) {
			case TIFF_TAG_NEW_SUBFILE_TYPE: {
				ifd->tiff_subfiletype = tag->data_u32;
			} break;
			// Note: the data type of many tags (E.g. ImageWidth) can actually be either SHORT or LONG,
			// but because we already converted the byte order to native (=little-endian) with enough
			// padding in the tag struct, we can get away with treating them as if they are always LONG.
			case TIFF_TAG_IMAGE_WIDTH: {
				if (tag->data_type == TIFF_UINT16) {
					ifd->image_width = tag->data_u16;
				} else {
					ifd->image_width = tag->data_u32;
				}
			} break;
			case TIFF_TAG_IMAGE_LENGTH: {
				if (tag->data_type == TIFF_UINT16) {
					ifd->image_height = tag->data_u16;
				} else {
					ifd->image_height = tag->data_u32;
				}
			} break;
			case TIFF_TAG_BITS_PER_SAMPLE: {
				if (!tag->data_is_offset) {
					ifd->bits_per_sample = tag->data_u16; // assume all channels the same
					if (is_verbose_mode) {
						for (i32 i = 0; i < tag->data_count; ++i) {
							u16 bits = *(u16*)&tag->data[i*2];
							console_print_verbose("   channel %d: BitsPerSample=%d\n", i, bits); // expected to be 8
						}
					}
				} else {
					u16* integers = tiff_read_field_u16(tiff, tag);
					ifd->bits_per_sample = integers[0]; // assume all channels the same
					if (is_verbose_mode) {
						for (i32 i = 0; i < tag->data_count; ++i) {
							console_print_verbose("   channel %d: BitsPerSample=%d\n", i, integers[i]); // expected to be 8
						}
					}
					free(integers);
				}
				ifd->bytes_per_sample = ifd->bits_per_sample / 8;
			} break;
			case TIFF_TAG_COMPRESSION: {
				ifd->compression = tag->data_u16;
			} break;
			case TIFF_TAG_PHOTOMETRIC_INTERPRETATION: {
				ifd->color_space = tag->data_u16;
			} break;
			case TIFF_TAG_IMAGE_DESCRIPTION: {
				ifd->image_description = tiff_read_field_ascii(tiff, tag);
				ifd->image_description_length = tag->data_count;
				console_print_verbose("%.500s\n", ifd->image_description);
			} break;
            case TIFF_TAG_MAKE: {
                if (is_verbose_mode) {
                    char* field = tiff_read_field_ascii(tiff, tag);
                    console_print_verbose("    %.500s\n", field);
                    free(field);
                }
            } break;
            case TIFF_TAG_MODEL: {
                if (is_verbose_mode) {
                    char* field = tiff_read_field_ascii(tiff, tag);
                    console_print_verbose("    %.500s\n", field);
                    free(field);
                }
            } break;
			case TIFF_TAG_STRIP_OFFSETS: {
				ifd->strip_count = tag->data_count;
				ifd->strip_offsets = tiff_read_field_integers(tiff, tag);
				if (ifd->strip_offsets == NULL) {
					free(tags);
					return false; // failed
				}
			} break;
			case TIFF_TAG_SAMPLES_PER_PIXEL: {
				ifd->samples_per_pixel = tag->data_u16;
			} break;
			case TIFF_TAG_ROWS_PER_STRIP: {
				if (tag->data_type == TIFF_UINT16) {
					ifd->rows_per_strip = tag->data_u16;
				} else if (tag->data_type == TIFF_UINT32) {
					ifd->rows_per_strip = tag->data_u32;
				}
			} break;
			case TIFF_TAG_STRIP_BYTE_COUNTS: {
				ifd->strip_count = tag->data_count;
				ifd->strip_byte_counts = tiff_read_field_integers(tiff, tag);
				if (ifd->strip_byte_counts == NULL) {
					free(tags);
					return false; // failed
				}
			}
			case TIFF_TAG_X_RESOLUTION: {
				tiff_rational_t resolution = tiff_read_field_rational(tiff, tag);
				ifd->x_resolution = resolution;
				console_print_verbose("   %g\n", tiff_rational_to_float(resolution));
			} break;
			case TIFF_TAG_Y_RESOLUTION: {
				tiff_rational_t resolution = tiff_read_field_rational(tiff, tag);
				ifd->y_resolution = resolution;
				console_print_verbose("   %g\n", tiff_rational_to_float(resolution));
			} break;
			case TIFF_TAG_X_POSITION: {
				tiff_rational_t position = tiff_read_field_rational(tiff, tag);
				ifd->x_position = position;
				console_print_verbose("   %g\n", tiff_rational_to_float(position));
			} break;
			case TIFF_TAG_Y_POSITION: {
				tiff_rational_t position = tiff_read_field_rational(tiff, tag);
				ifd->y_position = position;
				console_print_verbose("   %g\n", tiff_rational_to_float(position));
			} break;
			case TIFF_TAG_RESOLUTION_UNIT: {
				ifd->resolution_unit = tag->data_u16;
			} break;
			case TIFF_TAG_SOFTWARE: {
				ifd->software = tiff_read_field_ascii(tiff, tag);
				ifd->software_length = tag->data_count;
				console_print_verbose("    %.500s\n", ifd->software);
				if (strncmp(ifd->software, "Philips", 7) == 0) {
					ifd->is_philips = true;
					tiff->is_philips = true;
				}
				if (strncmp(ifd->software, "ScanOutputManager", 17) == 0) {
					ifd->is_ventana = true;
					tiff->is_ventana = true;
				}
			} break;
            case TIFF_TAG_PREDICTOR: {
                ifd->predictor = tag->data_u16;
            } break;
			case TIFF_TAG_TILE_WIDTH: {
				ifd->tile_width = tag->data_u32;
			} break;
			case TIFF_TAG_TILE_LENGTH: {
				ifd->tile_height = tag->data_u32;
			} break;
			case TIFF_TAG_TILE_OFFSETS: {
				// TODO: to be sure, need check PlanarConfiguration==1 to check how to interpret the data count?
				ifd->tile_count = tag->data_count;
				ifd->tile_offsets = tiff_read_field_integers(tiff, tag);
				if (ifd->tile_offsets == NULL) {
					free(tags);
					return false; // failed
				}
			} break;
			case TIFF_TAG_TILE_BYTE_COUNTS: {
				// Note: is it OK to assume that the TileByteCounts will always come after the TileOffsets?
				if (tag->data_count != ifd->tile_count) {
					ASSERT(tag->data_count != 0);
					console_print("Error: mismatch in the TIFF tile count reported by TileByteCounts and TileOffsets tags\n");
					free(tags);
					return false; // failed;
				}
				ifd->tile_byte_counts = tiff_read_field_integers(tiff, tag);
				if (ifd->tile_byte_counts == NULL) {
					free(tags);
					return false; // failed
				}
			} break;
			case TIFF_TAG_SAMPLE_FORMAT: {
				u16* formats = tiff_read_field_u16(tiff, tag);
				if (formats) {
					ifd->sample_format = (u16)formats[0];
					if (is_verbose_mode) {
						for (i32 i = 0; i < tag->data_count; ++i) {
							console_print_verbose("   channel %d: SampleFormat=%d\n", i, formats[i]);
						}
					}
					free(formats);
				}
			} break;
			case TIFF_TAG_S_MIN_SAMPLE_VALUE: {
				// NOTE: SampleFormat: SAMPLEFORMAT_UINT = 1; SAMPLEFORMAT_INT = 2; SAMPLEFORMAT_IEEEFP = 3; ...
				if (ifd->sample_format <= 2) {
					u64 bytesize = get_tiff_field_size(tag->data_type);
					if (bytesize == 1) {
						u8* values = tiff_read_field_u8(tiff, tag);
						if (values) {
							i64 lowest = values[0];
							for (i32 i = 0; i < tag->data_count; ++i) {
								if (values[i] < lowest) lowest = values[i];
								console_print_verbose("   channel %d: SMinSampleValue=%d\n", i, values[i]);
							}
							ifd->min_sample_value = lowest;
							free(values);
						}
					}
				}
			} break;
			case TIFF_TAG_S_MAX_SAMPLE_VALUE: {
                // todo: what to do if this tag is not present?
				if (ifd->sample_format <= 2) {
					u64 bytesize = get_tiff_field_size(tag->data_type);
					if (bytesize == 1) {
						u8* values = tiff_read_field_u8(tiff, tag);
						if (values) {
							i64 highest = values[0];
							for (i32 i = 0; i < tag->data_count; ++i) {
								if (values[i] > highest) highest = values[i];
								console_print_verbose("   channel %d: SMaxSampleValue=%d\n", i, values[i]);
							}
							ifd->max_sample_value = highest;
                            ifd->has_max_sample_value = true;
							free(values);
						}
					}
				}
			} break;
			case TIFF_TAG_JPEG_TABLES: {
				ifd->jpeg_tables = (u8*) tiff_read_field_undefined(tiff, tag);
				ifd->jpeg_tables_length = tag->data_count;
			} break;
			case TIFF_TAG_YCBCRSUBSAMPLING: {
				// https://www.awaresystems.be/imaging/tiff/tifftags/ycbcrsubsampling.html
				ifd->chroma_subsampling_horizontal = *(u16*)&tag->data[0];
				ifd->chroma_subsampling_vertical = *(u16*)&tag->data[2];
				console_print_verbose("   YCbCrSubsampleHoriz = %d, YCbCrSubsampleVert = %d\n", ifd->chroma_subsampling_horizontal, ifd->chroma_subsampling_vertical);

			} break;
			case TIFF_TAG_REFERENCEBLACKWHITE: {
				ifd->reference_black_white_rational_count = tag->data_count;
				ifd->reference_black_white = tiff_read_field_rationals(tiff, tag); //TODO: add to serialized format
				if (ifd->reference_black_white == NULL) {
					free(tags);
					return false; // failed
				}
				for (i32 i = 0; i < tag->data_count; ++i) {
					tiff_rational_t* reference_black_white = ifd->reference_black_white + i;
					console_print_verbose("    [%d] = %d / %d\n", i, reference_black_white->a, reference_black_white->b);
				}
			} break;
			case TIFF_TAG_XMP: {
				ifd->software = tiff_read_field_ascii(tiff, tag);
				ifd->software_length = tag->data_count;
				console_print_verbose("    %.500s\n", ifd->software);
			} break;
			case TIFF_TAG_IMAGE_DEPTH: {
				//stub
			} break;
			case TIGG_TAG_ICC_PROFILE_OFFSET: {
				if (tag->data_is_offset) {
					ifd->icc_profile_offset = tag->offset;
					ifd->icc_profile_length = tag->data_count;
				}
			} break;
            case NDPI_TAG_ALWAYS_1: {
                ifd->is_ndpi = true;
                tiff->is_ndpi = true;
            }
            case NDPI_TAG_SOURCE_LENS: {
                // objective power
            } break;
            case NDPI_TAG_OPTIMISATION_FILE: {
                ifd->ndpi_optimization_markers = (u32*)tiff_read_field_integers(tiff, tag);
                ifd->ndpi_optimization_count = tag->data_count;
            } break;
			default: {
			} break;
		}


	}

	free(tags);

    if (ifd->is_ndpi) {
        DUMMY_STATEMENT;
    }

	if (ifd->tile_count > 0) {
		ifd->is_tiled = true;
	}

	if (ifd->tile_width > 0) {
		ifd->width_in_tiles = (ifd->image_width + ifd->tile_width - 1) / ifd->tile_width;
	}
	if (ifd->tile_height > 0) {
		ifd->height_in_tiles = (ifd->image_height + ifd->tile_height - 1) / ifd->tile_height;
	}

	// Try to deduce what type of image this is (level, macro, or label).
	// Unfortunately this does not seem to be very consistently specified in the TIFF files, so in part we have to guess.
	if (ifd->is_ventana) {
		switch (ifd->ifd_index) {
			case 0: ifd->subimage_type = TIFF_LABEL_SUBIMAGE; break;
			case 1: ifd->subimage_type = TIFF_TISSUE_PROBABILITY_SUBIMAGE; break;
			default: ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
		}
	} else {
		if (ifd->image_description) {
			if (strncmp(ifd->image_description, "Macro", 5) == 0) {
				ifd->subimage_type = TIFF_MACRO_SUBIMAGE;
				tiff->macro_image = ifd;
				tiff->macro_image_index = ifd->ifd_index;
			} else if (strncmp(ifd->image_description, "Label", 5) == 0) {
				ifd->subimage_type = TIFF_LABEL_SUBIMAGE;
				tiff->label_image = ifd;
				tiff->label_image_index = ifd->ifd_index;
			} else if (strncmp(ifd->image_description, "level", 5) == 0) {
				ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
			}
		}
	}

	// Guess that it must be a level image if it's not explicitly said to be something else
	if (ifd->subimage_type == TIFF_UNKNOWN_SUBIMAGE /*0*/ && ifd->tile_width > 0) {
		if (ifd->ifd_index == 0 /* main image */ || ifd->tiff_subfiletype & TIFF_FILETYPE_REDUCEDIMAGE) {
			ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
		} else if (ifd->tiff_subfiletype == 0) {
			// Be lenient on incorrect implementations that don't set this tag properly for level subimages
			// Example: files converted from Olympus VSI to TIFF using the ASAP converter
			ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
		}
	}



	// Read the next IFD
	if (tiff_read(tiff, next_ifd_offset, tiff->bytesize_of_offsets) != tiff->bytesize_of_offsets) return false;
	console_print_verbose("next ifd offset = %lld\n", *next_ifd_offset);
	return true; // success
}

// Calculate various derived values (better name for this procedure??)
void tiff_post_init(tiff_t* tiff) {

	// Most often the first IFD is the main image, and also level 0.
	// However, this does not hold for all TIFF variants (e.g. Roche/Ventana .bif files use IFD index 2)
	if (tiff->is_ventana && tiff->ifd_count >= 3) {
		tiff->main_image_ifd = tiff->ifds + 2;
		tiff->main_image_ifd_index = 2;
	} else {
		tiff->main_image_ifd = tiff->ifds;
		tiff->main_image_ifd_index = 0;
	}
	tiff->level_images_ifd = tiff->main_image_ifd;
	tiff->level_images_ifd_index = tiff->main_image_ifd_index;

	// Determine the resolution of the base level
	// TODO: if resolution tags are missing, try to extract this information from the iSyntax header
	tiff->mpp_x = tiff->mpp_y = 0.25f;
	tiff->is_mpp_known = true;
	tiff_ifd_t* main_image = tiff->main_image_ifd;
	if (main_image->x_resolution.b != 0 && main_image->y_resolution.b != 0) {
		if (main_image->resolution_unit == TIFF_RESUNIT_CENTIMETER) {
			float pixels_per_centimeter_x = tiff_rational_to_float(main_image->x_resolution);
			float pixels_per_centimeter_y = tiff_rational_to_float(main_image->y_resolution);
			tiff->mpp_x = 10000.0f / pixels_per_centimeter_x;
			tiff->mpp_y = 10000.0f / pixels_per_centimeter_y;
			tiff->is_mpp_known = true;
		}
	}

	float main_image_width = (float) main_image->image_width;
	float main_image_height = (float) main_image->image_height;

	if (main_image->is_tiled) {
		tiff->max_downsample_level = 0;
		i32 last_downsample_level = 0;
		tiff->level_image_ifd_count = 0;

		// TODO: handle images containing multiple pyramids
		for (u64 ifd_index = tiff->level_images_ifd_index; ifd_index < tiff->ifd_count; ++ifd_index) {
			tiff_ifd_t* ifd = tiff->ifds + ifd_index;
			if (ifd->tile_count == 0) {
				break; // not a tiled image, so cannot be part of the pyramid (could be macro or label image)
			}
			if (ifd_index == tiff->level_images_ifd_index || ifd->subimage_type == TIFF_LEVEL_SUBIMAGE) {
				++tiff->level_image_ifd_count;
			}

			float level_width = (float)ifd->image_width;
			float raw_downsample_factor = main_image_width / level_width;
			float raw_downsample_level = log2f(raw_downsample_factor);
			i32 downsample_level = (i32) roundf(raw_downsample_level);

			// Some TIFF files have the width/height set to an integer multiple of the tile size.
			// For the most zoomed out levels, this makes it harder to calculate the actual downsampling level
			// (because we might underestimate it). So we need to do extra work to deduce the downsampling
			// level in these corner cases.
			if (ifd->image_width % ifd->tile_width == 0) {
				if (ifd->width_in_tiles >= 1 && ifd->height_in_tiles >= 1) {
					u32 min_possible_width = ifd->tile_width * (ifd->width_in_tiles-1) + 1;
					u32 max_possible_width = ifd->tile_width * (ifd->width_in_tiles) ;
					float downsample_factor_upper_bound = main_image_width / (float)min_possible_width;
					float downsample_factor_lower_bound = main_image_width / (float)max_possible_width;

					if (ifd->image_height % ifd->tile_height == 0) {
						// constrain further based on the vertical tile count
						u32 min_possible_height = ifd->tile_height * (ifd->height_in_tiles-1) + 1;
						u32 max_possible_height = ifd->tile_height * (ifd->height_in_tiles);

						float downsample_factor_y_upper_bound = main_image_height / (float)min_possible_height;
						float downsample_factor_y_lower_bound = main_image_height / (float)max_possible_height;

						downsample_factor_upper_bound = MIN(downsample_factor_upper_bound, downsample_factor_y_upper_bound);
						downsample_factor_lower_bound = MAX(downsample_factor_lower_bound, downsample_factor_y_lower_bound);
					}

					float level_lower_bound = log2f(downsample_factor_lower_bound);
					float level_upper_bound = log2f(downsample_factor_upper_bound);

					i32 discrete_level_lower_bound = (i32) ceilf(level_lower_bound);
					i32 discrete_level_upper_bound = (i32) floorf(level_upper_bound);

					if (discrete_level_lower_bound == discrete_level_upper_bound) {
						downsample_level = discrete_level_lower_bound;
					} else {
						// ambiguity could not be resolved. Use last resort.
						downsample_level = MIN(discrete_level_lower_bound, last_downsample_level + 1);
					}
					DUMMY_STATEMENT;
				}
			}

			ifd->downsample_level = last_downsample_level = downsample_level;
			ifd->downsample_factor = exp2f((float)ifd->downsample_level);
			tiff->max_downsample_level = MAX(ifd->downsample_level, tiff->max_downsample_level);
			ifd->um_per_pixel_x = tiff->mpp_x * ifd->downsample_factor;
			ifd->um_per_pixel_y = tiff->mpp_y * ifd->downsample_factor;
			ifd->x_tile_side_in_um = ifd->um_per_pixel_x * (float)ifd->tile_width;
			ifd->y_tile_side_in_um = ifd->um_per_pixel_y * (float)ifd->tile_height;
			DUMMY_STATEMENT;
		}
		DUMMY_STATEMENT;
		// Some TIFF files may have the XResolution and YResolution tags coded incorrectly
		// In the case of TIFF files converted from Olympus VSI using the ASAP converter, the X- and YResolution
		// tags will contain the microns per pixel (mpp) instead of pixels per centimeter as the TIFF standard requires.
		// Also, the order may be backward compared to what is expected (i.e. the lowest mpp value will be found in the last image)

		// As a first screening check: bogus resolution tags tend to result in mpp being way higher than expected
		if (tiff->mpp_x > 10.0f ) {

			float highest_x = 0.0f;
			float highest_y = 0.0f;
			float lowest_x = 1e10f;
			float lowest_y = 1e10f;

			for (u64 ifd_index = tiff->level_images_ifd_index; ifd_index < tiff->ifd_count; ++ifd_index) {
				tiff_ifd_t* ifd = tiff->ifds + ifd_index;
				if (ifd->x_resolution.b != 0 && ifd->y_resolution.b != 0) {
					float res_x = tiff_rational_to_float(ifd->x_resolution);
					float res_y = tiff_rational_to_float(ifd->y_resolution);
					if (res_x > highest_x) highest_x = res_x;
					if (res_y > highest_y) highest_y = res_y;
					if (res_x < lowest_x) lowest_x = res_x;
					if (res_y < lowest_y) lowest_y = res_y;
				}
			}

			if (lowest_x < highest_x && lowest_y < highest_y) {
				if (lowest_x < 100.0f && lowest_y < 100.0f) {
					tiff->mpp_x = lowest_x;
					tiff->mpp_y = lowest_y;
					tiff->is_mpp_known = true;

					// Update pixel and tile dimension information
					for (u64 ifd_index = tiff->level_images_ifd_index; ifd_index < tiff->ifd_count; ++ifd_index) {
						tiff_ifd_t* ifd = tiff->ifds + ifd_index;
						ifd->um_per_pixel_x = tiff->mpp_x * ifd->downsample_factor;
						ifd->um_per_pixel_y = tiff->mpp_y * ifd->downsample_factor;
						ifd->x_tile_side_in_um = ifd->um_per_pixel_x * (float)ifd->tile_width;
						ifd->y_tile_side_in_um = ifd->um_per_pixel_y * (float)ifd->tile_height;
					}
				}
			}


		}
	} else {
		// In this case the main image is a regular image consisting of strips, not tiles.
		tiff->level_image_ifd_count = 1;
		tiff->max_downsample_level = 0;
		main_image->downsample_level = 0;
		main_image->downsample_factor = 1.0f;
		main_image->um_per_pixel_x = tiff->mpp_x;
		main_image->um_per_pixel_y = tiff->mpp_y;
		main_image->tile_width = main_image->image_width;
		main_image->tile_height = main_image->image_height;
		main_image->x_tile_side_in_um = main_image->um_per_pixel_x * (float)main_image->tile_width;
		main_image->y_tile_side_in_um = main_image->um_per_pixel_y * (float)main_image->tile_height;
	}

}

static bool32 tiff_open_view(tiff_t* tiff) {
	if (!tiff->file.blob || tiff->file.size > INT64_MAX) return false;
	tiff->filesize = (i64)tiff->file.size;
	int ret;
	bool32 success = false;
	if (tiff->filesize > 8) {
		// read the 8-byte TIFF header / 16-byte BigTIFF header
		tiff_header_t tiff_header = {0};
		ret = tiff_read(tiff, &tiff_header, sizeof(tiff_header_t));
		if (ret != sizeof(tiff_header_t)) goto fail;
		bool32 is_big_endian;
		switch(tiff_header.byte_order_indication) {
			case TIFF_BIG_ENDIAN: is_big_endian = true; break;
			case TIFF_LITTLE_ENDIAN: is_big_endian = false; break;
			default: goto fail;
		}
		tiff->is_big_endian = is_big_endian;
		u16 filetype = maybe_swap_16(tiff_header.filetype, is_big_endian);
		bool32 is_bigtiff;
		switch(filetype) {
			case 0x2A: is_bigtiff = false; break;
			case 0x2B: is_bigtiff = true; break;
			default: goto fail;
		}
		tiff->is_bigtiff = is_bigtiff;
		u32 bytesize_of_offsets;
		u64 next_ifd_offset = 0;
		if (is_bigtiff) {
			console_print_verbose("TIFF variant is BigTIFF\n");
			bytesize_of_offsets = maybe_swap_16(tiff_header.bigtiff.offset_size, is_big_endian);
			if (bytesize_of_offsets != 8) goto fail;
			if (tiff_header.bigtiff.always_zero != 0) goto fail;
			next_ifd_offset = maybe_swap_64(tiff_header.bigtiff.first_ifd_offset, is_big_endian);
		} else {
			console_print_verbose("TIFF variant is standard TIFF\n");
			bytesize_of_offsets = 4;
			next_ifd_offset = maybe_swap_32(tiff_header.tiff.first_ifd_offset, is_big_endian);
		}
		ASSERT((bytesize_of_offsets == 4 && !is_bigtiff) || (bytesize_of_offsets == 8 && is_bigtiff));
		tiff->bytesize_of_offsets = bytesize_of_offsets;

		// Read and process the IFDs
		tiff_ifd_t last_ifd = {0};
		while (next_ifd_offset != 0) {
			console_print_verbose("Reading IFD #%llu\n", tiff->ifd_count);
			tiff_ifd_t ifd = { .ifd_index = tiff->ifd_count };

			// Apply default values
			ifd.compression = TIFF_COMPRESSION_NONE;
			ifd.samples_per_pixel = 1; // usually 3 for RGB

			// Apply some values from the last IFD that might not be repeated.
			ifd.min_sample_value = last_ifd.min_sample_value;
			ifd.max_sample_value = last_ifd.max_sample_value;

			if (!tiff_read_ifd(tiff, &ifd, &next_ifd_offset)) goto fail;
			arrput(tiff->ifds, ifd);
			tiff->ifd_count += 1;
			last_ifd = ifd;
		}

		if (!tiff->ifd_count) goto fail;
		tiff_post_init(tiff);

		success = true;

	}
fail:
	return success;
}

bool32 open_tiff_file(tiff_t* tiff, const char* filename) {
	tiff->owned_blob = calloc(1, sizeof(*tiff->owned_blob));
	return tiff->owned_blob && source_blob_open_local(tiff->owned_blob, filename) &&
		source_file_view(tiff->owned_blob, 0, tiff->owned_blob->size, &tiff->file) && tiff_open_view(tiff);
}

bool32 open_tiff_from_source(tiff_t* tiff, source_fs_t* source, const char* member) {
	tiff->source = source;
	return source && source_fs_open_file(source, member, &tiff->file) && tiff_open_view(tiff);
}


void memrw_push_tiff_block(memrw_t* buffer, u32 block_type, u32 index, u64 block_length) {
	serial_block_t block = { .block_type = block_type, .index = index, .length = block_length };
	memrw_push_back(buffer, (u8*) &block, sizeof(block));
}

#define INCLUDE_IMAGE_DESCRIPTION 1

memrw_t* tiff_serialize(tiff_t* tiff, memrw_t* buffer) {

	u64 uncompressed_size = 0;

	// TODO: is_mpp_known
	// block: general TIFF header / meta
	uncompressed_size += sizeof(serial_block_t);
	tiff_serial_header_t serial_header = (tiff_serial_header_t){
			.filesize = tiff->filesize,
			.ifd_count = tiff->ifd_count,
			.main_image_index = tiff->main_image_ifd_index,
			.macro_image_index = tiff->macro_image_index,
			.label_image_index = tiff->label_image_index,
			.level_image_ifd_count = tiff->level_image_ifd_count,
			.level_image_index = tiff->level_images_ifd_index,
			.bytesize_of_offsets = tiff->bytesize_of_offsets,
			.is_bigtiff = tiff->is_bigtiff,
			.is_big_endian = tiff->is_big_endian,
			.mpp_x = tiff->mpp_x,
			.mpp_y = tiff->mpp_y,
	};
	uncompressed_size += sizeof(serial_header);

	// block: IFD's
	uncompressed_size += sizeof(serial_block_t);
	u64 serial_ifds_block_size = tiff->ifd_count * sizeof(tiff_serial_ifd_t);
	tiff_serial_ifd_t* serial_ifds = (tiff_serial_ifd_t*) alloca(serial_ifds_block_size);
	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		tiff_serial_ifd_t* serial_ifd = serial_ifds + i;
		*serial_ifd = (tiff_serial_ifd_t) {
			.image_width = ifd->image_width,
			.image_height = ifd->image_height,
			.tile_width = ifd->tile_width,
			.tile_height = ifd->tile_height,
			.tile_count = ifd->tile_count,
			.image_description_length = ifd->image_description_length,
			.jpeg_tables_length = ifd->jpeg_tables_length,
			.compression = ifd->compression,
			.color_space = ifd->color_space,
			.level_magnification = ifd->level_magnification,
			.width_in_tiles = ifd->width_in_tiles,
			.height_in_tiles = ifd->height_in_tiles,
			.um_per_pixel_x = ifd->um_per_pixel_x,
			.um_per_pixel_y = ifd->um_per_pixel_y,
			.x_tile_side_in_um = ifd->x_tile_side_in_um,
			.y_tile_side_in_um = ifd->y_tile_side_in_um,
			.chroma_subsampling_horizontal = ifd->chroma_subsampling_horizontal,
			.chroma_subsampling_vertical = ifd->chroma_subsampling_vertical,
			.subimage_type = ifd->subimage_type,
		};
#if INCLUDE_IMAGE_DESCRIPTION
		uncompressed_size += ifd->image_description_length;
#endif
		uncompressed_size += ifd->jpeg_tables_length;
		uncompressed_size += ifd->tile_count * sizeof(ifd->tile_offsets[0]);
		uncompressed_size += ifd->tile_count * sizeof(ifd->tile_byte_counts[0]);
	}
	uncompressed_size += tiff->ifd_count * sizeof(tiff_serial_ifd_t);

	// blocks: need separate blocks for each IFD's image descriptions, tile offsets, tile byte counts, jpeg tables
#if INCLUDE_IMAGE_DESCRIPTION
	uncompressed_size += tiff->ifd_count * sizeof(serial_block_t);
#endif
	uncompressed_size += tiff->ifd_count * sizeof(serial_block_t);
	uncompressed_size += tiff->ifd_count * sizeof(serial_block_t);
	uncompressed_size += tiff->ifd_count * sizeof(serial_block_t);

	// block: terminator (end of stream marker)
	uncompressed_size += sizeof(serial_block_t);

	// Allocate space, and start pushing the data onto the buffer
	memrw_maybe_grow(buffer, uncompressed_size);

	memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_HEADER_AND_META, 0, sizeof(serial_block_t));
	memrw_push_back(buffer, &serial_header, sizeof(serial_header));

	memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_IFDS, 0, serial_ifds_block_size);
	memrw_push_back(buffer, serial_ifds, serial_ifds_block_size);

	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
#if INCLUDE_IMAGE_DESCRIPTION
		memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_IMAGE_DESCRIPTION, i, ifd->image_description_length);
		memrw_push_back(buffer, ifd->image_description, ifd->image_description_length);
#endif
		u64 tile_offsets_size = ifd->tile_count * sizeof(ifd->tile_offsets[0]);
		memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_TILE_OFFSETS, i, tile_offsets_size);
		memrw_push_back(buffer, ifd->tile_offsets, tile_offsets_size);

		u64 tile_byte_counts_size = ifd->tile_count * sizeof(ifd->tile_byte_counts[0]);
		memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_TILE_BYTE_COUNTS, i, tile_byte_counts_size);
		memrw_push_back(buffer, ifd->tile_byte_counts, tile_byte_counts_size);

		memrw_push_tiff_block(buffer, SERIAL_BLOCK_TIFF_JPEG_TABLES, i, ifd->jpeg_tables_length);
		memrw_push_back(buffer, ifd->jpeg_tables, ifd->jpeg_tables_length);

	}

	memrw_push_tiff_block(buffer, SERIAL_BLOCK_TERMINATOR, 0, 0);

//	console_print("buffer has %llu used bytes, out of %llu capacity\n", buffer->used_size, buffer->capacity);

	// Additional compression step
#if 1
	ASSERT(buffer->used_size == uncompressed_size);
	i32 compression_size_bound = LZ4_COMPRESSBOUND(buffer->used_size);
	u8* compression_buffer = (u8*) malloc(compression_size_bound);
	i32 compressed_size = LZ4_compress_default((char*)buffer->data, (char*)compression_buffer,
	                                           buffer->used_size, compression_size_bound);
	if (compressed_size > 0) {
		// success! We can replace the buffer contents with the compressed data
		memrw_rewind(buffer);
		memrw_push_tiff_block(buffer, SERIAL_BLOCK_LZ4_COMPRESSED_DATA, uncompressed_size, compressed_size);
		memrw_push_back(buffer, compression_buffer, compressed_size);
	} else {
		console_print_error("Warning: tiff_serialize(): payload LZ4 compression failed\n");
	}

	free(compression_buffer);
#endif

	return buffer;

}

u8* pop_from_buffer(u8** pos, i64 size, i64* bytes_left) {
	if (size > *bytes_left) {
		console_print_error("pop_from_buffer(): buffer empty\n");
		return NULL;
	}
	u8* old_pos = *pos;
	*pos += size;
	*bytes_left -= size;
	return old_pos;
}

serial_block_t* pop_block_from_buffer(u8** pos, i64* bytes_left) {
	return (serial_block_t*) pop_from_buffer(pos, sizeof(serial_block_t), bytes_left);
}

i64 find_end_of_http_headers(u8* str, u64 len) {
	static const char crlfcrlf[] = "\r\n\r\n";
	u32 search_key = *(u32*)crlfcrlf;
	i64 result = 0;
	for (i64 offset = 0; offset < len - 4; ++offset) {
		u8* pos = str + offset;
		u32 check = *(u32*)pos;
		if (check == search_key) {
			result = offset + 4;
			break;
		}
	}
	return result;
}



bool32 tiff_deserialize(tiff_t* tiff, u8* buffer, u64 buffer_size) {
	i64 bytes_left = (i64)buffer_size;
	u8* pos = buffer;
	u8* data = NULL;
	serial_block_t* block = NULL;
	bool32 success = false;
	u8* decompressed_buffer = NULL;

	if (0) {
		failed:
		if (decompressed_buffer) free(decompressed_buffer);
		return false;
	}


#define POP_DATA(size) do {if (!(data = pop_from_buffer(&pos, (size), &bytes_left))) goto failed; } while(0)
#define POP_BLOCK() do{if (!(block = pop_block_from_buffer(&pos, &bytes_left))) goto failed; } while(0)


	i64 content_offset = find_end_of_http_headers(buffer, buffer_size);
	i64 content_length = buffer_size - content_offset;
	POP_DATA(content_offset);

	// block: general TIFF header / meta
	POP_BLOCK();

	if (block->block_type == SERIAL_BLOCK_LZ4_COMPRESSED_DATA) {
		// compressed LZ4 stream
		i32 compressed_size = (i32)block->length;
		i32 decompressed_size = (i32)block->index; // used as general purpose field here..
		ASSERT(block->length < INT32_MAX);
		ASSERT(block->index < INT32_MAX);
		POP_DATA(compressed_size);
		decompressed_buffer = (u8*) malloc(decompressed_size);
		i32 bytes_decompressed = LZ4_decompress_safe((char*)data, (char*)decompressed_buffer, compressed_size, decompressed_size);
		if (bytes_decompressed > 0) {
			if (bytes_decompressed != decompressed_size) {
				console_print_error("LZ4_decompress_safe() decompressed %d bytes, however the expected size was %d\n", bytes_decompressed, decompressed_size);
			} else {
				// success, switch over to the uncompressed buffer!
				pos = decompressed_buffer;
				bytes_left = bytes_decompressed;
				POP_BLOCK(); // now pointing to the uncompressed data stream
			}

		} else {
			console_print_error("LZ4_decompress_safe() failed (return value %d)\n", bytes_decompressed);
		}
	}

	if (block->block_type != SERIAL_BLOCK_TIFF_HEADER_AND_META) goto failed;

	POP_DATA(sizeof(tiff_serial_header_t));
	tiff_serial_header_t* serial_header = (tiff_serial_header_t*) data;
	*tiff = (tiff_t) {0};
	tiff->is_remote = 1; // Deserialized IFDs use calloc, including on partial failure.
	tiff->location = (network_location_t){0}; // set later
	tiff->filesize = serial_header->filesize;
	tiff->bytesize_of_offsets = serial_header->bytesize_of_offsets;
	tiff->ifd_count = serial_header->ifd_count;
	tiff->ifds = NULL; // set later
	tiff->main_image_ifd = NULL; // set later
	tiff->main_image_ifd_index = serial_header->main_image_index;
	tiff->macro_image = NULL; // set later
	tiff->macro_image_index = serial_header->macro_image_index;
	tiff->label_image = NULL; // set later
	tiff->label_image_index = serial_header->label_image_index;
	tiff->level_image_ifd_count = serial_header->level_image_ifd_count;
	tiff->level_images_ifd = NULL; // set later
	tiff->level_images_ifd_index = 0; // set later
	tiff->is_bigtiff = serial_header->is_bigtiff;
	tiff->is_big_endian = serial_header->is_big_endian;
	tiff->mpp_x = serial_header->mpp_x;
	tiff->mpp_y = serial_header->mpp_y;

	// block: IFD's
	POP_BLOCK();
	if (block->block_type != SERIAL_BLOCK_TIFF_IFDS) goto failed;
	u64 serial_ifds_block_size = tiff->ifd_count * sizeof(tiff_serial_ifd_t);
	if (block->length != serial_ifds_block_size) goto failed;

	POP_DATA(serial_ifds_block_size);
	tiff_serial_ifd_t* serial_ifds = (tiff_serial_ifd_t*) data;

	// TODO: maybe not use a stretchy_buffer here?
	tiff->ifds = (tiff_ifd_t*) calloc(1, sizeof(tiff_ifd_t) * tiff->ifd_count); // allocate space for the IFD's
	memset(tiff->ifds, 0, tiff->ifd_count * sizeof(tiff_ifd_t));

	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		tiff_serial_ifd_t* serial_ifd = serial_ifds + i;
		*ifd = (tiff_ifd_t) {0};
		ifd->ifd_index = i;
		ifd->image_width = serial_ifd->image_width;
		ifd->image_height = serial_ifd->image_height;
		ifd->tile_width = serial_ifd->tile_width;
		ifd->tile_height = serial_ifd->tile_height;
		ifd->tile_count = serial_ifd->tile_count;
		ifd->tile_offsets = NULL; // set later
		ifd->tile_byte_counts = NULL; // set later
		ifd->image_description = NULL; // set later
		ifd->image_description_length = serial_ifd->image_description_length;
		ifd->jpeg_tables = NULL; // set later
		ifd->jpeg_tables_length = serial_ifd->jpeg_tables_length;
		ifd->compression = serial_ifd->compression;
		ifd->color_space = serial_ifd->color_space;
		ifd->subimage_type = serial_ifd->subimage_type;
		ifd->level_magnification = serial_ifd->level_magnification; // TODO: delete this?
		ifd->width_in_tiles = serial_ifd->width_in_tiles;
		ifd->height_in_tiles = serial_ifd->height_in_tiles;
		ifd->um_per_pixel_x = serial_ifd->um_per_pixel_x;
		ifd->um_per_pixel_y = serial_ifd->um_per_pixel_y;
		ifd->x_tile_side_in_um = serial_ifd->x_tile_side_in_um;
		ifd->y_tile_side_in_um = serial_ifd->y_tile_side_in_um;
		ifd->chroma_subsampling_horizontal = serial_ifd->chroma_subsampling_horizontal;
		ifd->chroma_subsampling_vertical = serial_ifd->chroma_subsampling_vertical;
		ifd->reference_black_white_rational_count = 0; // unused for now
		ifd->reference_black_white = NULL; // unused for now
	}

	DUMMY_STATEMENT; // for placing a debug breakpoint

	// The number of remaining blocks is unspecified.
	// We are expecting at least byte offsets, tile offsets and JPEG tables for each IFD
	bool32 reached_end = false;
	while (!reached_end) {

		POP_BLOCK();
		if (block->length > 0) {
			POP_DATA(block->length);
		}
		u8* block_content = data;
		// TODO: Need to think about this: are block index's (if present) always referring an IFD index, though? Or are there other use cases?
		if (block->index >= tiff->ifd_count) {
			console_print_error("tiff_deserialize(): found block referencing a non-existent IFD\n");
			goto failed;
		}
		tiff_ifd_t* referenced_ifd = tiff->ifds + block->index;


		switch (block->block_type) {
			case SERIAL_BLOCK_TIFF_IMAGE_DESCRIPTION: {
				if (referenced_ifd->image_description) {
					console_print_error("tiff_deserialize(): IFD %u already has an image description\n", block->index);
					goto failed;
				}
				referenced_ifd->image_description = (char*) malloc(block->length + 1);
				memcpy(referenced_ifd->image_description, block_content, block->length);
				referenced_ifd->image_description[block->length] = '\0';
				referenced_ifd->image_description_length = block->length;
			} break;
			case SERIAL_BLOCK_TIFF_TILE_OFFSETS: {
				if (referenced_ifd->tile_offsets) {
					console_print_error("tiff_deserialize(): IFD %u already has tile offsets\n", block->index);
					goto failed;
				}
				referenced_ifd->tile_offsets = (u64*) malloc(block->length);
				memcpy(referenced_ifd->tile_offsets, block_content, block->length);
			} break;
			case SERIAL_BLOCK_TIFF_TILE_BYTE_COUNTS: {
				if (referenced_ifd->tile_byte_counts) {
					console_print_error("tiff_deserialize(): IFD %u already has tile byte counts\n", block->index);
					goto failed;
				}
				referenced_ifd->tile_byte_counts = (u64*) malloc(block->length);
				memcpy(referenced_ifd->tile_byte_counts, block_content, block->length);
			} break;
			case SERIAL_BLOCK_TIFF_JPEG_TABLES: {
				if (referenced_ifd->jpeg_tables) {
					console_print_error("tiff_deserialize(): IFD %u already has JPEG tables\n", block->index);
					goto failed;
				}
				referenced_ifd->jpeg_tables = (u8*) malloc(block->length);
				if (!referenced_ifd->jpeg_tables) goto failed;
				memcpy(referenced_ifd->jpeg_tables, block_content, block->length);
				referenced_ifd->jpeg_tables_length = block->length;
			} break;
			case SERIAL_BLOCK_TERMINATOR: {
				// Reached the end
#if REMOTE_CLIENT_VERBOSE
				console_print("tiff_deserialize(): found a terminator block\n");
#endif
				reached_end = true;
				break;
			} break;
		}
	}
	if (reached_end) {
		success = true;
#if REMOTE_CLIENT_VERBOSE
		console_print("tiff_deserialize(): bytes_left = %lld, content length = %lld, buffer size = %llu\n", bytes_left, content_length, buffer_size);
#endif
	}

	if (decompressed_buffer != NULL) free(decompressed_buffer);

	tiff_post_init(tiff);

	return success;


#undef POP_DATA
#undef POP_BLOCK

}

void tiff_destroy(tiff_t* tiff) {
	if (tiff->owned_blob) {
		source_blob_close(tiff->owned_blob);
		free(tiff->owned_blob); tiff->owned_blob = NULL;
	}
	source_fs_close(tiff->source); tiff->source = NULL;
	tiff->file = (source_file_t){0};

	for (i32 i = 0; tiff->ifds && i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		if (ifd->tile_offsets) free(ifd->tile_offsets);
		if (ifd->tile_byte_counts) free(ifd->tile_byte_counts);
		if (ifd->image_description) free(ifd->image_description);
		if (ifd->xmp) free(ifd->xmp);
		if (ifd->software) free(ifd->software);
		if (ifd->jpeg_tables) free(ifd->jpeg_tables);
		if (ifd->reference_black_white) free(ifd->reference_black_white);
        if (ifd->ndpi_optimization_markers) free(ifd->ndpi_optimization_markers);
	}
	// TODO: fix this, choose either stretchy_buffer or regular malloc, not both...
	if (tiff->is_remote) {
		free(tiff->ifds);
	} else {
		arrfree(tiff->ifds);
	}
	memset(tiff, 0, sizeof(*tiff));
}

// Adapted from ASAP: see core/PathologyEnums.cpp
u32 lut[] = {
	MAKE_BGRA(0, 0, 0, 0),
	MAKE_BGRA(0, 224, 249, 255),
	MAKE_BGRA(0, 249, 50, 255),
	MAKE_BGRA(174, 249, 0, 255),
	MAKE_BGRA(249, 100, 0, 255),
	MAKE_BGRA(249, 0, 125, 255),
	MAKE_BGRA(149, 0, 249, 255),
	MAKE_BGRA(0, 0, 206, 255),
	MAKE_BGRA(0, 185, 206, 255),
	MAKE_BGRA(0, 206, 41, 255),
	MAKE_BGRA(143, 206, 0, 255),
	MAKE_BGRA(206, 82, 0, 255),
	MAKE_BGRA(206, 0, 103, 255),
	MAKE_BGRA(124, 0, 206, 255),
	MAKE_BGRA(0, 0, 162, 255),
	MAKE_BGRA(0, 145, 162, 255),
	MAKE_BGRA(0, 162, 32, 255),
	MAKE_BGRA(114, 162, 0, 255),
	MAKE_BGRA(162, 65, 0, 255),
	MAKE_BGRA(162, 0, 81, 255),
	MAKE_BGRA(97, 0, 162, 255),
	MAKE_BGRA(0, 0, 119, 255),
	MAKE_BGRA(0, 107, 119, 255),
	MAKE_BGRA(0, 119, 23, 255),
	MAKE_BGRA(83, 119, 0, 255),
	MAKE_BGRA(119, 47, 0, 255),
	MAKE_BGRA(119, 0, 59, 255),
	MAKE_BGRA(71, 0, 119, 255),
	MAKE_BGRA(100, 100, 249, 255),
	MAKE_BGRA(100, 234, 249, 255)
};

// TODO: move this somewhere appropriate?
static inline u32 lookup_color_from_lut(u8 index) {
	if (index >= COUNT(lut)) {
		index = 0;
	}
	u32 color = lut[index];
	return color;
}

// from libtiff: horizontal predictor decoder
static int horAcc8(u32 stride, uint8* cp0, u32 cc) {
    unsigned char* cp = (unsigned char*) cp0;
    if((cc%stride)!=0) {
//        TIFFErrorExt(tif->tif_clientdata, "horAcc8",
//                     "%s", "(cc%stride)!=0");
        return 0;
    }
    if (cc > stride) {
        /*
         * Pipeline the most common cases.
         */
        if (stride == 3)  {
            unsigned int cr = cp[0];
            unsigned int cg = cp[1];
            unsigned int cb = cp[2];
            cc -= 3;
            cp += 3;
            while (cc>0) {
                cp[0] = (unsigned char) ((cr += cp[0]) & 0xff);
                cp[1] = (unsigned char) ((cg += cp[1]) & 0xff);
                cp[2] = (unsigned char) ((cb += cp[2]) & 0xff);
                cc -= 3;
                cp += 3;
            }
        } else if (stride == 4)  {
            unsigned int cr = cp[0];
            unsigned int cg = cp[1];
            unsigned int cb = cp[2];
            unsigned int ca = cp[3];
            cc -= 4;
            cp += 4;
            while (cc>0) {
                cp[0] = (unsigned char) ((cr += cp[0]) & 0xff);
                cp[1] = (unsigned char) ((cg += cp[1]) & 0xff);
                cp[2] = (unsigned char) ((cb += cp[2]) & 0xff);
                cp[3] = (unsigned char) ((ca += cp[3]) & 0xff);
                cc -= 4;
                cp += 4;
            }
        } else  {
            cc -= stride;
            do {
                for (i32 i = 0; i < stride; ++i) {
                    cp[stride] = (unsigned char) ((cp[stride] + *cp) & 0xff);
                    cp++;
                }
                cc -= stride;
            } while (cc>0);
        }
    }
    return 1;
}


u8* tiff_decode_tile(i32 logical_thread_index, tiff_t* tiff, tiff_ifd_t* level_ifd, i32 tile_index, i32 level, i32 tile_x, i32 tile_y) {

	u16 compression = level_ifd->compression;
	u8* jpeg_tables = level_ifd->jpeg_tables;
	u64 jpeg_tables_length = level_ifd->jpeg_tables_length;

	u64 tile_offset = 0;
	u64 compressed_tile_size_in_bytes = 0;
	u8* compressed_tile_data = NULL;
	u8** compressed_strip_data = NULL;
	bool failed = false;

	if (level_ifd->is_tiled) {
		tile_offset = level_ifd->tile_offsets[tile_index];
		compressed_tile_size_in_bytes = level_ifd->tile_byte_counts[tile_index];

		// Some tiles apparently contain no data (not even an empty/dummy JPEG stream like some other tiles have).
		// We need to check for this situation and chicken out if this is the case.
		if (tile_offset == 0 || compressed_tile_size_in_bytes == 0) {
#if DO_DEBUG
			console_print("thread %d: tile level %d, tile %d (%d, %d) appears to be empty\n", logical_thread_index, level, tile_index, tile_x, tile_y);
#endif
			return NULL;
		}

		// TODO: optimize allocation
		compressed_tile_data = (u8*)malloc(compressed_tile_size_in_bytes);

		if (!tiff->is_remote) {
			if (source_file_read(&tiff->file, tile_offset, compressed_tile_data, compressed_tile_size_in_bytes) != SOURCE_READ_OK) {
				free(compressed_tile_data);
				return NULL;
			}
		} else {
			console_print_verbose("[thread %d] remote tile requested: level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);

			i32 bytes_read = 0;
			u8* read_buffer = download_remote_chunk(tiff->location.hostname, tiff->location.portno, tiff->location.filename,
			                                        tile_offset, compressed_tile_size_in_bytes, &bytes_read, logical_thread_index);
			if (read_buffer && bytes_read > 0) {
				i64 content_offset = find_end_of_http_headers(read_buffer, bytes_read);
				i64 content_length = bytes_read - content_offset;
				u8* content = read_buffer + content_offset;

				if (content_length >= compressed_tile_size_in_bytes) {
					memcpy(compressed_tile_data, content, compressed_tile_size_in_bytes);
				} else {
					failed = true;
				}

			} else {
				failed = true;
			}
			if (read_buffer) {
				free(read_buffer);
			}
			if (failed) {
				console_print_error("[thread %d] failed to read from remote level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);
                free(compressed_tile_data);
				return NULL;
			}
		}

	} else {
		// image is not tiled


		if (!tiff->is_remote) {

			if (level_ifd->strip_count > 0) {
				ASSERT(level_ifd->strip_offsets);
				ASSERT(level_ifd->strip_byte_counts);
				if (level_ifd->strip_count == 1) {
					compressed_tile_size_in_bytes = level_ifd->strip_byte_counts[0];
					compressed_tile_data = (u8*)malloc(compressed_tile_size_in_bytes);
					size_t bytes_read = source_file_read_at_offset(compressed_tile_data, &tiff->file, level_ifd->strip_offsets[0], compressed_tile_size_in_bytes);
					if (bytes_read != compressed_tile_size_in_bytes) {
						failed = true;
					}
				} else {
					compressed_strip_data = (u8**)alloca(level_ifd->strip_count * sizeof(u8**));
					memset(compressed_strip_data, 0, level_ifd->strip_count * sizeof(u8**));
					for (i32 i = 0; i < level_ifd->strip_count; ++i) {
						u64 strip_offset = level_ifd->strip_offsets[i];
						u64 strip_byte_count = level_ifd->strip_byte_counts[i];
						compressed_strip_data[i] = (u8*)malloc(strip_byte_count);
						size_t bytes_read = source_file_read_at_offset(compressed_strip_data[i], &tiff->file, strip_offset, strip_byte_count);
						if (bytes_read != strip_byte_count) {
							failed = true;
						}
					}
				}

			} else {
				failed = true; // no tiles and no strips?
			}

		} else {
			failed = true;
		}

		if (failed) {
			if (compressed_tile_data) {
				free(compressed_tile_data);
				compressed_tile_data = NULL;
			}
			if (compressed_strip_data) {
				for (i32 i = 0; i < level_ifd->strip_count; ++i) {
					free(compressed_strip_data[i]);
					compressed_strip_data[i] = NULL;
				}
			}
			return NULL;
		}
	}
	// We should have either a compressed tile, or one or more compressed strips
	ASSERT(compressed_tile_data != NULL || compressed_strip_data[0] != NULL);

	// Now decompress
	if (compressed_tile_data || compressed_strip_data)  {
//		console_print_verbose("[thread %d] loading tile: level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);

		size_t pixel_memory_size = level_ifd->tile_width * level_ifd->tile_height * BYTES_PER_PIXEL;
		u8* pixel_memory = tile_buffer_acquire(pixel_memory_size);
		u8* decompressed = NULL; // allocated later if needed

		// Take into account either tiled or multi-strip TIFF files
		u8** compressed_streams;
		u64* compressed_stream_sizes;
		i32 compressed_stream_count;
		if (compressed_tile_data) {
			compressed_streams = &compressed_tile_data;
			compressed_stream_sizes = &compressed_tile_size_in_bytes;
			compressed_stream_count = 1;
		} else if (level_ifd->strip_count > 0 && compressed_strip_data) {
			compressed_streams = compressed_strip_data;
			compressed_stream_sizes = level_ifd->strip_byte_counts;
			compressed_stream_count = level_ifd->strip_count;
		} else {
			goto decompression_failed;
		}

		for (i32 compressed_stream_index = 0; compressed_stream_index < compressed_stream_count; ++compressed_stream_index) {

			u8* compressed_stream = compressed_streams[compressed_stream_index];
			u64 compressed_stream_size = compressed_stream_sizes[compressed_stream_index];
			ASSERT(compressed_stream_size >= 2);

			u8* pixel_memory_dest = pixel_memory;
			if (compressed_stream_index > 0 && level_ifd->strip_count > 0) {
				u64 row_offset = (u64)level_ifd->rows_per_strip * compressed_stream_index;
				if (row_offset >= level_ifd->tile_height) {
					goto decompression_failed; // out of bounds
				}
				pixel_memory_dest += row_offset * level_ifd->tile_width * BYTES_PER_PIXEL;
			}

			u32 decompressed_height;
			if (level_ifd->strip_count > 0) {
				decompressed_height = level_ifd->rows_per_strip;
				if ((compressed_stream_index + 1) * decompressed_height > level_ifd->tile_height) {
					// last strip height may be less, if the total height is not a multiple of rows_per_strip
					decompressed_height = level_ifd->tile_height % level_ifd->rows_per_strip;
				}
				if (decompressed_height == 0 || decompressed_height > level_ifd->tile_height) {
					goto decompression_failed;
				}
			} else {
				decompressed_height = level_ifd->tile_height;
			}

			if (compression == TIFF_COMPRESSION_JPEG) {
				if (compressed_stream[0] == 0xFF && compressed_stream[1] == 0xD9) {
					// JPEG stream is empty
					memset(pixel_memory_dest, 0xFF, level_ifd->tile_width * decompressed_height * sizeof(u32));
				} else {
					bool success = false;
					if (level_ifd->is_ndpi) {
						success = jpeg_decode_ndpi_image(compressed_stream, compressed_stream_size, level_ifd->image_width, level_ifd->image_height, NULL);
					} else {
						success = jpeg_decode_tile(jpeg_tables, jpeg_tables_length, compressed_stream,
						                           compressed_stream_size,
						                           pixel_memory_dest, (level_ifd->color_space == TIFF_PHOTOMETRIC_YCBCR));
					}
					if (success) {
//		                console_print_verbose("thread %d: successfully decoded level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);
						continue; // success
					} else {
						console_print_error("thread %d: failed to decode level %d, tile %d (%d, %d)\n", logical_thread_index, level, tile_index, tile_x, tile_y);
						goto decompression_failed;
					}
				}
			} else if (level_ifd->compression == TIFF_COMPRESSION_LZW) {

//			    i64 start = get_clock();

				size_t decompressed_size = level_ifd->tile_width * decompressed_height * level_ifd->samples_per_pixel * level_ifd->bytes_per_sample;
				decompressed = (u8*)malloc(decompressed_size);

				PseudoTIFF tif = {};
				tif.tif_rawdata = compressed_stream;
				tif.tif_rawcp = compressed_stream;
				tif.tif_rawdatasize = compressed_stream_size;
				tif.tif_rawcc = compressed_stream_size;
				LZWSetupDecode(&tif);
				LZWPreDecode(&tif, 0);
				// Check for old bit-reversed codes.
				int decode_success = 0;
				if (tif.tif_rawcc >= 2 && tif.tif_rawdata[0] == 0 && (tif.tif_rawdata[1] & 0x1)) {
					decode_success = LZWDecodeCompat(&tif, decompressed, decompressed_size, 0);
				} else {
					decode_success = LZWDecode(&tif, decompressed, decompressed_size, 0);
				}
				if (!decode_success) {
					console_print_error("LZW decompression failed\n");
					goto decompression_failed;
				}

				if (level_ifd->predictor > 1) {
					if (level_ifd->predictor == 2 /* PREDICTOR_HORIZONTAL */ && level_ifd->samples_per_pixel <= 8) {
						// horizontal differencing
						u32 samples = level_ifd->samples_per_pixel;
						u32 subpixels_per_scanline = level_ifd->tile_width * samples;
						for (u32 y = 0; y < decompressed_height; ++y) {
							u8* scanline = decompressed + y * subpixels_per_scanline;
							horAcc8(samples, scanline, subpixels_per_scanline);
						}
					} else {
						console_print_error("LZW decoding failed: unsupported predictor operator (%d)\n", level_ifd->predictor);
						goto decompression_failed;
					}
				}

//			    i64 decode_end = get_clock();
//			    console_print_verbose("[thread %d] decode  level %d, tile %d (%d, %d) took %g ms\n", logical_thread_index, level, tile_index, tile_x, tile_y, 1000.0f * get_seconds_elapsed(start, decode_end));

				// Convert RGB to BGRA
				if (level_ifd->samples_per_pixel == 4) {
					// TODO: convert RGBA to BGRA
					console_print_error("LZW decompression: RGBA to BGRA conversion not implemented, assuming already in BGRA\n");
					memcpy(pixel_memory_dest, decompressed, decompressed_size);
					free(decompressed);
					decompressed = NULL;
					continue; // success
				} else if (level_ifd->samples_per_pixel == 3) {

					// NOTE: Some TIFFs should actually be treated as palettized, but still set PhotometricInterpretation to TIFF_PHOTOMETRIC_RGB.
					// (as an example, the TIFF masks from the Kaggle challenge do this)
					// However, in that case they will still probably have set SMaxSampleValue to a low value (the number of colors/categories used).
					// We can use this fact to guess that we still want to treat the image as palettized / using a color lookup table.
					bool palettized = level_ifd->color_space == TIFF_PHOTOMETRIC_PALETTE || (level_ifd->max_sample_value > 0 && level_ifd->max_sample_value < 64);

					u64 pixel_count = level_ifd->tile_width * decompressed_height;
					i32 source_pos = 0;
					u32* pixels = (u32*)pixel_memory_dest;

					// TODO: vectorize: https://stackoverflow.com/questions/7194452/fast-vectorized-conversion-from-rgb-to-bgra
					if (palettized) {
						for (u64 i = 0; i < pixel_count; ++i) {
							u8 r = decompressed[source_pos]; // only the red channel is being used
//						    u8 g = decompressed[source_pos+1];
//					    	u8 b = decompressed[source_pos+2];
//					    	pixels[i]=(r<<16) | (g<<8) | b | (0xff << 24);
							u32 color = lookup_color_from_lut(r);
							color = BGRA_SET_ALPHA(color, 128); // TODO: make color lookup tables configurable
							pixels[i] = color;
							source_pos+=3;
						}
					} else {
						for (u64 i = 0; i < pixel_count; ++i) {
							// TODO: optimize?
							u8 r = decompressed[source_pos];
							u8 g = decompressed[source_pos+1];
							u8 b = decompressed[source_pos+2];
							pixels[i] = MAKE_BGRA(r, g, b, 255);
							source_pos+=3;
						}
					}

					free(decompressed);
					decompressed = NULL;
//				    console_print_verbose("[thread %d] swizzle level %d, tile %d (%d, %d) took %g ms\n", logical_thread_index, level, tile_index, tile_x, tile_y, 1000.0f * get_seconds_elapsed(decode_end, get_clock()));
					continue; // success
				} else if (level_ifd->samples_per_pixel == 1) {

					// Grayscale image
					u64 pixel_count = level_ifd->tile_width * decompressed_height;
					u32* pixels = (u32*)pixel_memory_dest;
					if (level_ifd->bits_per_sample == 8) {
						i32 source_pos = 0;

						// assume palettized
						// TODO: how to decide if it's a palettized image or not?
						for (u64 i = 0; i < pixel_count; ++i) {
							u8 r = decompressed[source_pos]; // only the red channel is being used
//						    u8 g = decompressed[source_pos+1];
//					        u8 b = decompressed[source_pos+2];
//					        pixels[i]=(r<<16) | (g<<8) | b | (0xff << 24);
							u32 color = lookup_color_from_lut(r);
//                          color = BGRA_SET_ALPHA(color, 128); // TODO: make color lookup tables configurable
							pixels[i] = color;
							source_pos+=1;
						}
					} else if (level_ifd->bits_per_sample == 32) {
						u32* source_pixels = (u32*)decompressed;
						i32 source_pos = 0;
						for (u64 i = 0; i < pixel_count; ++i) {
							u32 as_u32 = source_pixels[source_pos];
							float f = *(float*)(source_pixels + source_pos);
							float normalized = f / 16000.0f;
							u8 c = FLOAT_TO_BYTE(normalized);
							u8 r = source_pixels[source_pos]; // only the red channel is being used
//						    u8 g = source_pixels[source_pos+1];
//					        u8 b = source_pixels[source_pos+2];
//					        pixels[i]=(r<<16) | (g<<8) | b | (0xff << 24);
//							u32 color = lookup_color_from_lut(r);
                            u32 color = MAKE_BGRA(c, c, c, 255);
							pixels[i] = color;
							source_pos+=1;
						}
					}



					/*u8 output_for_min_value = 0;
					u8 output_for_max_value = 255;
					if (level_ifd->color_space == TIFF_PHOTOMETRIC_MINISBLACK) {
						// no action
					} else if (level_ifd->color_space == TIFF_PHOTOMETRIC_MINISWHITE) {
						output_for_min_value = 255;
						output_for_max_value = 0;
					} else {
						// Issue warning? PhotometricInterpretation missing
					}

					bool is_bilevel = level_ifd->max_sample_value == 1 && level_ifd->min_sample_value == 0;

					if (is_bilevel) {
						for (u64 i = 0; i < pixel_count; ++i) {
							u8 r = decompressed[source_pos];
							r = r ? output_for_max_value : output_for_min_value;
							u32 color = MAKE_BGRA(r, r, r, 255);
							pixels[i] = color;
							source_pos+=1;
						}
					} else {
						if (level_ifd->max_sample_value == 0 *//*assume not set*//* || level_ifd->max_sample_value == 255) {
						// output raw value as RGB value
						for (u64 i = 0; i < pixel_count; ++i) {
							u8 r = decompressed[source_pos];
							u32 color = MAKE_BGRA(r, r, r, 255);
							pixels[i] = color;
							source_pos+=1;
						}
					} else {
						// resample
						float convert_factor = (1.0f / (float)level_ifd->max_sample_value) * 255.0f;
						for (u64 i = 0; i < pixel_count; ++i) {
							u8 r = decompressed[source_pos];
							r = (u8)((float)r * convert_factor);
							u32 color = MAKE_BGRA(r, r, r, 255);
							pixels[i] = color;
							source_pos+=1;
						}
					}

				}*/

					free(decompressed);
					decompressed = NULL;
					continue; // success
				} else {
					console_print_error("LZW decompression: unexpected number of samples per pixel (%d)\n", level_ifd->samples_per_pixel);
					goto decompression_failed;
				}

			} else if (level_ifd->compression == TIFF_COMPRESSION_NONE) {
				u8* uncompressed = (u8*)compressed_stream;
				u64 pixel_count = level_ifd->tile_width * decompressed_height;
				i32 source_pos = 0;
				if (level_ifd->samples_per_pixel == 4) {
					u32* pixels = (u32*)pixel_memory_dest;
					for (u64 i = 0; i < pixel_count; ++i) {
						u8 r = uncompressed[source_pos];
						u8 g = uncompressed[source_pos+1];
						u8 b = uncompressed[source_pos+2];
						u8 a = uncompressed[source_pos+3];
						pixels[i] = MAKE_BGRA(r, g, b, a);
						source_pos+=4;
					}
					// memcpy(pixel_memory_dest, uncompressed, pixel_count * 4);
				} else if (level_ifd->samples_per_pixel == 3) {
					u32* pixels = (u32*)pixel_memory_dest;
					for (u64 i = 0; i < pixel_count; ++i) {
						u8 r = uncompressed[source_pos]; // only the red channel is being used
						u8 g = uncompressed[source_pos+1];
						u8 b = uncompressed[source_pos+2];
						pixels[i] = MAKE_BGRA(r, g, b, 255);
						source_pos+=3;
					}
					continue;
				} else {
					console_print_error("Uncompressed TIFF: unexpected number of samples per pixel (%d)\n", level_ifd->samples_per_pixel);
					goto decompression_failed;
				}
			} else {
				console_print_error("\"thread %d: failed to decode level %d, tile %d (%d, %d): unsupported TIFF compression method (compression=%d)\n", logical_thread_index, level, tile_index, tile_x, tile_y, compression);
				goto decompression_failed;
			}
		}

		ASSERT(pixel_memory);

		if (false) { decompression_failed:
			// We'll return NULL in case of failure
			if (pixel_memory) {
				tile_buffer_release(pixel_memory, pixel_memory_size);
				pixel_memory = NULL;
			}
		}

		// Cleanup
		if (compressed_tile_data) {
			free(compressed_tile_data);
		}
		if (compressed_strip_data) {
			for (i32 i = 0; i < level_ifd->strip_count; ++i) {
				free(compressed_strip_data[i]);
			}
		}
		if (decompressed) {
			free(decompressed);
		}

		return pixel_memory;

	}
	ASSERT(!"should not reach this code");
	return NULL;

	// Trim the tile (replace with transparent color) if it extends beyond the image size
	// TODO: anti-alias edge?
/*	i32 new_tile_height = level_image->tile_height;
	i32 pitch = level_image->tile_width * BYTES_PER_PIXEL;
	if (tile_y_excess > 0) {
		i32 excess_rows = (int)((tile_y_excess / level_image->y_tile_side_in_um) * level_image->tile_height);
		ASSERT(excess_rows >= 0);
		new_tile_height = level_image->tile_height - excess_rows;
		memset(pixel_memory + (new_tile_height * pitch), 0, excess_rows * pitch);
	}
	if (tile_x_excess > 0) {
		i32 excess_pixels = (int)((tile_x_excess / level_image->x_tile_side_in_um) * level_image->tile_width);
		ASSERT(excess_pixels >= 0);
		i32 new_tile_width = level_image->tile_width - excess_pixels;
		for (i32 row = 0; row < new_tile_height; ++row) {
			u8* write_pos = pixel_memory + (row * pitch) + (new_tile_width * BYTES_PER_PIXEL);
			memset(write_pos, 0, excess_pixels * BYTES_PER_PIXEL);
		}
	}*/
}
