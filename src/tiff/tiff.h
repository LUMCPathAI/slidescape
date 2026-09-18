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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include "common.h"
#include "platform.h"
#include "source_fs.h"
#include "intrinsics.h"

#define TIFF_LITTLE_ENDIAN 0x4949
#define TIFF_BIG_ENDIAN 0x4D4D

// Documentation for TIFF tags: https://www.awaresystems.be/imaging/tiff/tifftags/search.html

enum tiff_tag_code_enum {
	TIFF_TAG_NEW_SUBFILE_TYPE = 254,
	TIFF_TAG_IMAGE_WIDTH = 256,
	TIFF_TAG_IMAGE_LENGTH = 257,
	TIFF_TAG_BITS_PER_SAMPLE = 258,
	TIFF_TAG_COMPRESSION = 259,
	TIFF_TAG_PHOTOMETRIC_INTERPRETATION = 262,
	TIFF_TAG_FILL_ORDER = 266,
	TIFF_TAG_IMAGE_DESCRIPTION = 270,
    TIFF_TAG_MAKE = 271,
    TIFF_TAG_MODEL = 272,
	TIFF_TAG_STRIP_OFFSETS = 273,
	TIFF_TAG_ORIENTATION = 274,
	TIFF_TAG_SAMPLES_PER_PIXEL = 277,
	TIFF_TAG_ROWS_PER_STRIP = 278,
	TIFF_TAG_STRIP_BYTE_COUNTS = 279,
	TIFF_TAG_X_RESOLUTION = 282,
	TIFF_TAG_Y_RESOLUTION = 283,
	TIFF_TAG_PLANAR_CONFIGURATION = 284,
	TIFF_TAG_X_POSITION = 286,
	TIFF_TAG_Y_POSITION = 287,
	TIFF_TAG_RESOLUTION_UNIT = 296,
	TIFF_TAG_PAGE_NUMBER = 297,
	TIFF_TAG_SOFTWARE = 305,
    TIFF_TAG_DATETIME = 306,
    TIFF_TAG_PREDICTOR = 317,
	TIFF_TAG_WHITE_POINT = 318,
	TIFF_TAG_PRIMARY_CHROMACITIES = 319,
	TIFF_TAG_TILE_WIDTH = 322,
	TIFF_TAG_TILE_LENGTH = 323,
	TIFF_TAG_TILE_OFFSETS = 324,
	TIFF_TAG_TILE_BYTE_COUNTS = 325,
	TIFF_TAG_SAMPLE_FORMAT = 339,
	TIFF_TAG_S_MIN_SAMPLE_VALUE = 340,
	TIFF_TAG_S_MAX_SAMPLE_VALUE = 341,
	TIFF_TAG_JPEG_TABLES = 347,
	TIFF_TAG_YCBCRSUBSAMPLING = 530,
	TIFF_TAG_REFERENCEBLACKWHITE = 532,
	TIFF_TAG_XMP = 700,
	TIFF_TAG_IMAGE_DEPTH = 32997,
	TIGG_TAG_ICC_PROFILE_OFFSET = 34675,

    NDPI_TAG_ALWAYS_1 = 65420,
    NDPI_TAG_SOURCE_LENS = 65421,
    NDPI_TAG_X_OFFSET_FROM_SLIDE_CENTRE = 65422,
    NDPI_TAG_Y_OFFSET_FROM_SLIDE_CENTRE = 65423,
    NDPI_TAG_OPTIMISATION_FILE = 65426,
    NDPI_TAG_REFERENCE = 65427,
};

enum tiff_data_type_enum {
	TIFF_UINT8 = 1,
	TIFF_ASCII = 2,
	TIFF_UINT16 = 3, // SHORT
	TIFF_UINT32 = 4, // LONG
	TIFF_RATIONAL = 5,
	TIFF_INT8 = 6, // SBYTE
	TIFF_UNDEFINED = 7,
	TIFF_INT16 = 8, // SSHORT
	TIFF_INT32 = 9, // SLONG
	TIFF_SRATIONAL = 10,
	TIFF_FLOAT = 11,
	TIFF_DOUBLE = 12,
	TIFF_IFD = 13, // equal to LONG
	TIFF_UINT64 = 16, // LONG8
	TIFF_INT64 = 17, // SLONG8
	TIFF_IFD8 = 18,
};

enum tiff_subfiletype_enum {
	TIFF_FILETYPE_REDUCEDIMAGE = 1,
	TIFF_FILETYPE_PAGE = 2,
	TIFF_FILETYPE_MASK = 4,
};

typedef enum tiff_resunit_enum {
	TIFF_RESUNIT_NONE = 1,
	TIFF_RESUNIT_INCH = 2,
	TIFF_RESUNIT_CENTIMETER = 3,
} tiff_resunit_enum;

#pragma pack(push, 1)

typedef struct tiff_rational_t {
	i32 a, b;
} tiff_rational_t;

typedef struct {
	u16 byte_order_indication; // either 0x4949 for little-endian or 0x4D4D for big-endian
	u16 filetype; // must be 0x002A for standard TIFF or 0x002B for BigTIFF
	union {
		struct {
			u32 first_ifd_offset;
		} tiff;
		struct {
			u16 offset_size; // expected to be 0x0008 for 64-bit
			u16 always_zero;
			u64 first_ifd_offset;
		} bigtiff;
	};
} tiff_header_t;

// Packed standard TIFF and BigTIFF tag structs, with layout matching the file structure
typedef struct {
	u16 code;
	u16 data_type;
	u32 data_count;
	union {
		u8 data[4];
		u32 offset;
	};
} raw_tiff_tag_t;

typedef struct {
	u16 code;
	u16 data_type;
	u64 data_count;
	union {
		u8 data[8];
		u64 offset;
		u8 data_u8;
		u16 data_u16;
		u32 data_u32;
		u64 data_u64;
	};
} raw_bigtiff_tag_t;

#pragma pack(pop)

// Converted TIFF tag, with native byte order, for internal use
typedef struct {
	u16 code;
	u16 data_type;
	u64 data_count;
	union {
		u8 data[8];
		u64 offset;
		u8 data_u8;
		u16 data_u16;
		u32 data_u32;
		u64 data_u64;
	};
	bool8 data_is_offset;
} tiff_tag_t;

// https://www.awaresystems.be/imaging/tiff/tifftags/compression.html
enum tiff_compression_enum {
	TIFF_COMPRESSION_NONE = 1,
	TIFF_COMPRESSION_CCCITTRLE = 2,
	TIFF_COMPRESSION_CCITTFAX3 = 3,
	TIFF_COMPRESSION_CCITTFAX4 = 4,
	TIFF_COMPRESSION_LZW = 5,
	TIFF_COMPRESSION_OJPEG = 6, // old-style JPEG -> ignore
	TIFF_COMPRESSION_JPEG = 7,
	TIFF_COMPRESSION_ADOBE_DEFLATE = 8,
	TIFF_COMPRESSION_JP2000 = 34712,
};

// https://www.awaresystems.be/imaging/tiff/tifftags/photometricinterpretation.html
enum tiff_photometric_interpretation_enum {
	TIFF_PHOTOMETRIC_MINISWHITE = 0,
	TIFF_PHOTOMETRIC_MINISBLACK = 1,
	TIFF_PHOTOMETRIC_RGB = 2,
	TIFF_PHOTOMETRIC_PALETTE = 3,
	TIFF_PHOTOMETRIC_MASK = 4,
	TIFF_PHOTOMETRIC_SEPARATED = 5,
	TIFF_PHOTOMETRIC_YCBCR = 6,
	TIFF_PHOTOMETRIC_CIELAB = 8,
	TIFF_PHOTOMETRIC_ICCLAB = 9,
	TIFF_PHOTOMETRIC_ITULAB = 10,
	TIFF_PHOTOMETRIC_LOGL = 32844,
	TIFF_PHOTOMETRIC_LOGLUV = 32845,
};

enum tiff_orientation_enum {
	TIFF_ORIENTATION_TOPLEFT = 1,
	TIFF_ORIENTATION_TOPRIGHT = 2,
	TIFF_ORIENTATION_BOTRIGHT = 3,
	TIFF_ORIENTATION_BOTLEFT = 4,
	TIFF_ORIENTATION_LEFTTOP = 5,
	TIFF_ORIENTATION_RIGHTTOP = 6,
	TIFF_ORIENTATION_RIGHTBOT = 7,
	TIFF_ORIENTATION_LEFTBOT = 8,
};

enum tiff_planar_configuration_enum {
	TIFF_PLANARCONFIG_CONTIG = 1,
	TIFF_PLANARCONFIG_SEPARATE = 2,
};

enum subimage_type_enum {
	TIFF_UNKNOWN_SUBIMAGE = 0,
	TIFF_LEVEL_SUBIMAGE = 1,
	TIFF_MACRO_SUBIMAGE = 2,
	TIFF_LABEL_SUBIMAGE = 3,
	TIFF_TISSUE_PROBABILITY_SUBIMAGE = 4,
};

typedef struct tiff_t tiff_t;


typedef struct tiff_ifd_t {
	u64 ifd_index;
	u32 image_width;
	u32 image_height;
	bool is_tiled;
	u32 rows_per_strip;
	u64 strip_count;
	u64* strip_offsets;
	u64* strip_byte_counts;
	u32 tile_width;
	u32 tile_height;
	u64 tile_count;
	u64* tile_offsets;
	u64* tile_byte_counts;
	u16 samples_per_pixel;
	u16 bits_per_sample;
	u16 bytes_per_sample;
	u16 sample_format;
	i64 min_sample_value;
	i64 max_sample_value; // TODO: use this field in a sensible way without requiring it being present
    bool has_max_sample_value;
	char* software;
	u64 software_length;
	char* image_description;
	u64 image_description_length;
	char* xmp;
	u64 xmp_length;
	u8* jpeg_tables;
	u64 jpeg_tables_length;
    u16 predictor;
	u16 compression; // 7 = JPEG
	u16 color_space;
	u32 tiff_subfiletype;
	u32 subimage_type;
	float level_magnification;
	u32 width_in_tiles;
	u32 height_in_tiles;
	float um_per_pixel_x;
	float um_per_pixel_y;
	float x_tile_side_in_um;
	float y_tile_side_in_um;
	float downsample_factor;
	i32 downsample_level;
	u16 chroma_subsampling_horizontal;
	u16 chroma_subsampling_vertical;
	u64 reference_black_white_rational_count;
	tiff_rational_t* reference_black_white;
	tiff_rational_t x_resolution;
	tiff_rational_t y_resolution;
	tiff_rational_t x_position;
	tiff_rational_t y_position;
	tiff_resunit_enum resolution_unit;
    u32* ndpi_optimization_markers;
    u32 ndpi_optimization_count;
	u64 icc_profile_offset;
	u64 icc_profile_length;
    bool is_ndpi;
	bool is_philips;
	bool is_ventana;
} tiff_ifd_t;


typedef struct network_location_t {
	i32 portno;
	const char* hostname;
	const char* filename;
} network_location_t;

struct tiff_t {
	bool32 is_remote;
	network_location_t location;
	source_file_t file;
	source_blob_t* owned_blob; /* Local-file owner; heap stable when tiff_t is moved. */
	source_fs_t* source; /* ZIP namespace owner, if present. */
	u64 read_cursor; /* Metadata parser only; tile reads are positional. */
	i64 filesize;
	u32 bytesize_of_offsets;
	u64 ifd_count;
	tiff_ifd_t* ifds; // sb
	tiff_ifd_t* main_image_ifd; // level 0 of the WSI; in Philips TIFF it's typically the first IFD
	u64 main_image_ifd_index;
	tiff_ifd_t* macro_image; // in Philips TIFF: typically the second-to-last IFD
	u64 macro_image_index;
	tiff_ifd_t* label_image; // in Philips TIFF: typically the last IFD
	u64 label_image_index;
	u64 level_image_ifd_count;
	tiff_ifd_t* level_images_ifd;
	u64 level_images_ifd_index;
	bool is_bigtiff;
	bool is_big_endian;
	bool is_philips;
    bool is_ndpi;
	bool is_ventana;
	bool is_mpp_known;
	float mpp_x;
	float mpp_y;
	i32 max_downsample_level;
};

#pragma pack(push, 1)
typedef struct {
	i64 filesize;
	u64 ifd_count;
	u64 main_image_index; // level 0 of the WSI; in Philips TIFF it's typically the first IFD
	u64 macro_image_index; // in Philips TIFF: typically the second-to-last IFD
	u64 label_image_index; // in Philips TIFF: typically the last IFD
	u64 level_image_ifd_count;
	u64 level_image_index;
	u32 bytesize_of_offsets;
	bool8 is_bigtiff;
	bool8 is_big_endian;
	float mpp_x;
	float mpp_y;
} tiff_serial_header_t;

typedef struct {
	u32 image_width;
	u32 image_height;
	u32 tile_width;
	u32 tile_height;
//	u64* tile_offsets;
	u64 tile_count;
//	u64* tile_byte_counts;
//	char* image_description;
	u64 image_description_length;
//	u8* jpeg_tables;
	u64 jpeg_tables_length;
	u16 compression; // 7 = JPEG
	u16 color_space;
	float level_magnification;
	u32 width_in_tiles;
	u32 height_in_tiles;
	float um_per_pixel_x;
	float um_per_pixel_y;
	float x_tile_side_in_um;
	float y_tile_side_in_um;
	u16 chroma_subsampling_horizontal;
	u16 chroma_subsampling_vertical;
	u32 subimage_type;
//	tiff_tile_t* tiles;
} tiff_serial_ifd_t;

enum serial_block_type_enum {
	SERIAL_BLOCK_LZ4_COMPRESSED_DATA = 4444,
	SERIAL_BLOCK_TIFF_HEADER_AND_META = 9001, // using ridiculous numbers to make invalid file structure easier to detect
	SERIAL_BLOCK_TIFF_IFDS = 9002,
	SERIAL_BLOCK_TIFF_IMAGE_DESCRIPTION = 9003,
	SERIAL_BLOCK_TIFF_TILE_OFFSETS = 9004,
	SERIAL_BLOCK_TIFF_TILE_BYTE_COUNTS = 9005,
	SERIAL_BLOCK_TIFF_JPEG_TABLES = 9006,
	SERIAL_BLOCK_TERMINATOR = 800,
};

typedef struct {
	u32 block_type;
	u32 index; // e.g. which IFD this data block belongs to
	u64 length;
} serial_block_t;

#pragma pack(pop)

u32 get_tiff_field_size(u16 data_type);
bool32 open_tiff_file(tiff_t* tiff, const char* filename);
/* Takes namespace ownership, including on failure. Destroy with tiff_destroy(). */
bool32 open_tiff_from_source(tiff_t* tiff, source_fs_t* source, const char* member);
memrw_t* tiff_serialize(tiff_t* tiff, memrw_t* buffer);
i64 find_end_of_http_headers(u8* str, u64 len);
bool32 tiff_deserialize(tiff_t* tiff, u8* buffer, u64 buffer_size);
void tiff_destroy(tiff_t* tiff);
u8* tiff_decode_tile(i32 logical_thread_index, tiff_t* tiff, tiff_ifd_t* level_ifd, i32 tile_index, i32 level, i32 tile_x, i32 tile_y);
double tiff_rational_to_float(tiff_rational_t rational);
tiff_rational_t float_to_tiff_rational(double x);

#ifdef __cplusplus
}
#endif

