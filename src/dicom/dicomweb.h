/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dicomweb_instance_metadata_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
	char image_flavor[17];
	char photometric_interpretation[17];
	i64 number_of_frames;
	u32 rows;
	u32 columns;
	u32 total_pixel_matrix_columns;
	u32 total_pixel_matrix_rows;
	u16 samples_per_pixel;
	float mpp_x;
	float mpp_y;
	bool is_mpp_known;
	bool is_tiled_full;
} dicomweb_instance_metadata_t;

typedef struct dicomweb_frame_t {
	const u8* data;
	size_t size;
	char media_type[64];
	char transfer_syntax_uid[65];
} dicomweb_frame_t;

#define DICOMWEB_MAX_INSTANCES 16

typedef struct dicomweb_instance_ref_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
} dicomweb_instance_ref_t;

typedef struct dicomweb_remote_image_t {
	char hostname[256];
	char root[512];
	char authorization_header[6144];
	char study_instance_uid[65];
	char series_instance_uid[65];
	i32 port;
	bool use_tls;
	i32 instance_count;
	dicomweb_instance_metadata_t instances[DICOMWEB_MAX_INSTANCES];
} dicomweb_remote_image_t;

bool dicomweb_parse_instance_metadata(const char* json, size_t json_size,
                                      dicomweb_instance_metadata_t* out);
i32 dicomweb_parse_instance_refs(const char* json, size_t json_size,
                                 dicomweb_instance_ref_t* out, i32 capacity);
bool dicomweb_parse_multipart_frame(const u8* body, size_t body_size,
                                    const char* response_content_type,
                                    dicomweb_frame_t* out);
bool dicomweb_build_instance_metadata_path(char* dest, size_t dest_size,
                                           const char* root,
                                           const char* study_uid,
                                           const char* series_uid,
                                           const char* instance_uid);
bool dicomweb_build_frame_path(char* dest, size_t dest_size,
                               const char* root,
                               const char* study_uid,
                               const char* series_uid,
                               const char* instance_uid,
                               u32 one_based_frame_number);

#ifdef __cplusplus
}
#endif
