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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "image.h"

typedef enum viewer_file_type_enum {
	VIEWER_FILE_TYPE_UNKNOWN = 0,
	VIEWER_FILE_TYPE_SIMPLE_IMAGE,
	VIEWER_FILE_TYPE_TIFF,
	VIEWER_FILE_TYPE_NDPI,
	VIEWER_FILE_TYPE_MRXS,
	VIEWER_FILE_TYPE_DICOM,
	VIEWER_FILE_TYPE_ISYNTAX,
	VIEWER_FILE_TYPE_OPENSLIDE_COMPATIBLE,
	VIEWER_FILE_TYPE_XML,
	VIEWER_FILE_TYPE_JSON,
} viewer_file_type_enum;

typedef struct file_info_t {
	char full_filename[512];
	char filename_prefix[512]; // directory name + trailing slash
	char filename_in_directory[512];
	char ext[16];
	i64 filesize;
	viewer_file_type_enum type;
	bool is_valid;
	bool is_directory;
	bool is_regular_file;
	bool is_image;
	bool is_openslide_compatible;
	u8 header[256];
} file_info_t;

typedef struct directory_info_t {
	file_info_t* dicom_files; // array
	file_info_t* nondicom_files; // array
	directory_info_t* directories; // array
	bool contains_dicom_files;
	bool contains_nondicom_images;
	bool contains_mrxs_files;
	bool is_valid;
} directory_info_t;

typedef enum filetype_hint_enum {
	FILETYPE_HINT_NONE = 0,
	FILETYPE_HINT_ANNOTATIONS,
	FILETYPE_HINT_BASE_IMAGE,
	FILETYPE_HINT_OVERLAY,
} filetype_hint_enum;

typedef struct image_load_options_t {
	bool is_overlay;
	bool use_builtin_tiff_backend;
	bool use_native_mrxs_backend;
	bool openslide_available;
	bool openslide_loading_done;
	i32 resource_id;
	image_t* parent_image;
	thread_pool_t* thread_pool;
} image_load_options_t;

file_info_t viewer_get_file_info(const char* filename);
directory_info_t viewer_get_directory_info(const char* path);
directory_info_t viewer_get_dicom_directory_info(const char* path);
void viewer_directory_info_destroy(directory_info_t* info);
void load_openslide_wsi(wsi_t* wsi, const char* filename, bool openslide_loading_done, thread_pool_t* thread_pool);
image_t* image_load_from_file(file_info_t* file, directory_info_t* directory, image_load_options_t* options);

#ifdef __cplusplus
}
#endif
