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

/* Opt-in viewer bootstrap representation; never a replacement for DICOM JSON. */
#define DICOMWEB_COMPACT_METADATA_MEDIA_TYPE "application/vnd.slidescape.wsi-metadata.v1+json"
#define DICOMWEB_MAX_COMPACT_METADATA_SIZE MEGABYTES(8)

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
	u32 frame_map_count;
	u32* frame_numbers;
} dicomweb_instance_metadata_t;

typedef struct dicomweb_frame_t {
	const u8* data;
	size_t size;
	char media_type[64];
	char transfer_syntax_uid[65];
	u32 frame_number;
} dicomweb_frame_t;

typedef struct dicomweb_instance_ref_t {
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
	char sop_class_uid[65];
	char image_flavor[17];
	char instance_number[17];
	char number_of_frames[17];
	char concatenation_uid[65];
	char in_concatenation_number[17];
	char in_concatenation_total_number[17];
	char total_pixel_matrix_columns[17];
	char total_pixel_matrix_rows[17];
} dicomweb_instance_ref_t;

typedef struct dicomweb_study_ref_t {
	char study_instance_uid[65];
	char study_date[17];
	char description[129];
} dicomweb_study_ref_t;

typedef struct dicomweb_study_search_t {
	char study_instance_uid[65];
	char patient_id[65];
	char accession_number[65];
	char study_date[18];
	char description[129];
} dicomweb_study_search_t;

typedef struct dicomweb_series_ref_t {
	char series_instance_uid[65];
	char series_number[17];
	char modality[17];
	char description[129];
} dicomweb_series_ref_t;

typedef struct dicomweb_remote_image_t {
	char hostname[256];
	char root[512];
	char authorization_header[6144];
	char study_instance_uid[65];
	char series_instance_uid[65];
	i32 port;
	bool use_tls;
	i32 available_series_count;
	i32 available_series_capacity;
	i32 selected_series_index;
	dicomweb_series_ref_t* available_series;
	i32 instance_count;
	i32 instance_capacity;
	dicomweb_instance_metadata_t* instances;
	i64 open_start_clock;
	bool first_tile_upload_reported; /* Main/render thread only. */
} dicomweb_remote_image_t;

bool dicomweb_parse_instance_metadata(const char* json, size_t json_size,
                                      dicomweb_instance_metadata_t* out);
bool dicomweb_parse_compact_metadata(const char* json, size_t json_size,
		dicomweb_instance_metadata_t* out);
void dicomweb_instance_metadata_destroy(dicomweb_instance_metadata_t* metadata);
void dicomweb_remote_image_destroy(dicomweb_remote_image_t* remote);
i32 dicomweb_parse_instance_refs(const char* json, size_t json_size,
                                 dicomweb_instance_ref_t* out, i32 capacity);
i32 dicomweb_count_instance_refs(const char* json, size_t json_size);
i32 dicomweb_parse_study_refs(const char* json, size_t json_size,
                              dicomweb_study_ref_t* out, i32 capacity);
i32 dicomweb_count_study_refs(const char* json, size_t json_size);
i32 dicomweb_parse_series_refs(const char* json, size_t json_size,
                               dicomweb_series_ref_t* out, i32 capacity);
i32 dicomweb_count_series_refs(const char* json, size_t json_size);
bool dicomweb_series_ref_is_supported_wsi(const dicomweb_series_ref_t* series);
bool dicomweb_instance_ref_is_supported_wsi(const dicomweb_instance_ref_t* instance);
bool dicomweb_parse_multipart_frame(const u8* body, size_t body_size,
                                    const char* response_content_type,
                                    dicomweb_frame_t* out);
i32 dicomweb_parse_multipart_frames(const u8* body, size_t body_size,
                                    const char* response_content_type,
                                    dicomweb_frame_t* out, i32 capacity);
u8* dicomweb_decode_jpeg_frame_to_bgra(const dicomweb_instance_metadata_t* instance,
		const dicomweb_frame_t* frame);
/* Decode a native DICOMweb frame response. JPEG Baseline and JPEG 2000 transfer
 * syntaxes are currently supported; the returned BGRA buffer is caller-owned. */
u8* dicomweb_decode_frame_to_bgra(const dicomweb_instance_metadata_t* instance,
		const dicomweb_frame_t* frame);
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
bool dicomweb_build_frames_path(char* dest, size_t dest_size,
                                const char* root,
                                const char* study_uid,
                                const char* series_uid,
                                const char* instance_uid,
                                const u32* one_based_frame_numbers,
                                i32 frame_count);
bool dicomweb_resolve_series_url(char* dest, size_t dest_size, const char* url);
bool dicomweb_is_valid_uid(const char* value, size_t length);
bool dicomweb_encode_query_value(char* dest, size_t dest_size,
                                 const char* value);
bool dicomweb_build_study_query_path(char* dest, size_t dest_size,
                                     const char* root,
                                     const dicomweb_study_search_t* search);

#ifdef __cplusplus
}
#endif
