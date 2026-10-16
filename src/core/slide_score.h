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

#pragma once

#include "common.h"
#include "memrw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum field_type_enum {
    FIELD_TYPE_UNKNOWN = 0,
    FIELD_TYPE_I32,
    FIELD_TYPE_I64,
    FIELD_TYPE_FLOAT,
    FIELD_TYPE_STRING_256CHARS,
    FIELD_TYPE_STRING_512CHARS,
} field_type_enum;

typedef enum slide_score_api_enum {
    SLIDE_SCORE_API_UNKNOWN = 0,
    SLIDE_SCORE_API_SCORES,
    SLIDE_SCORE_API_GET_STUDIES_UPDATED,
    SLIDE_SCORE_API_STUDIES,
    SLIDE_SCORE_API_IMAGES,
    SLIDE_SCORE_API_CASES,
    SLIDE_SCORE_API_QUESTIONS,
    SLIDE_SCORE_API_GET_SLIDE_PATH,
    SLIDE_SCORE_API_GET_SLIDE_DESCRIPTION,
    SLIDE_SCORE_API_GET_SLIDE_DETAILS,
    SLIDE_SCORE_API_GET_CASE_DESCRIPTION,
    SLIDE_SCORE_API_PUBLISH,
    SLIDE_SCORE_API_UNPUBLISH,
    SLIDE_SCORE_API_DOWNLOAD_SLIDE,
    SLIDE_SCORE_API_IS_SLIDE_OUT_OF_FOCUS,
    SLIDE_SCORE_API_UPSERT_DOMAIN,
    SLIDE_SCORE_API_GET_DOMAIN_FOR_STUDY,
    SLIDE_SCORE_API_SET_DOMAIN_FOR_STUDY,
    SLIDE_SCORE_API_GENERATE_STUDENT_ACCOUNT,
    SLIDE_SCORE_API_GENERATE_LOGIN_LINK,
    SLIDE_SCORE_API_GENERATE_SLIDE_FILE_URL,
    SLIDE_SCORE_API_CREATE_STUDY,
    SLIDE_SCORE_API_UPDATE_STUDY,
    SLIDE_SCORE_API_REQUEST_UPLOAD,
    SLIDE_SCORE_API_FINISH_UPLOAD,
    SLIDE_SCORE_API_ADD_SLIDE,
    SLIDE_SCORE_API_UPLOAD_RESULTS,
    SLIDE_SCORE_API_CONVERT_SCORE_VALUE_TO_ANNO2,
    SLIDE_SCORE_API_CREATE_ANNO2,
    SLIDE_SCORE_API_SET_SLIDE_RESOLUTION,
    SLIDE_SCORE_API_SET_SLIDE_DESCRIPTION,
    SLIDE_SCORE_API_UPDATE_SLIDE_NAME,
    SLIDE_SCORE_API_UPDATE_CASE_NAME,
    SLIDE_SCORE_API_UPDATE_SLIDE_PATH,
    SLIDE_SCORE_API_SET_IS_ARCHIVED,
    SLIDE_SCORE_API_UPDATE_TMA_CORE_SIZE,
    SLIDE_SCORE_API_DELETE_SLIDE,
    SLIDE_SCORE_API_GET_QUPATH_TOKENS_FOR_STUDY,
    SLIDE_SCORE_API_ADD_CASE,
    SLIDE_SCORE_API_SET_IMAGE_CASE,
    SLIDE_SCORE_API_REMOVE_CASE,
    SLIDE_SCORE_API_DELETE_STUDY,
    SLIDE_SCORE_API_UNDELETE_STUDY,
    SLIDE_SCORE_API_EXPORT_ASAP_ANNOTATIONS,
    SLIDE_SCORE_API_UPLOAD_ASAP_ANNOTATIONS,
    SLIDE_SCORE_API_GET_SCREENSHOT,
    SLIDE_SCORE_API_SEND_EMAIL,
    SLIDE_SCORE_API_ADD_USER,
    SLIDE_SCORE_API_REMOVE_USER,
    SLIDE_SCORE_API_CREATE_TMA_MAP,
    SLIDE_SCORE_API_SET_SLIDE_TMA_MAP,
    SLIDE_SCORE_API_ADD_QUESTION,
    SLIDE_SCORE_API_UPDATE_QUESTION,
    SLIDE_SCORE_API_REMOVE_QUESTION,
    SLIDE_SCORE_API_REIMPORT,
    SLIDE_SCORE_API_GET_IMAGE_METADATA,
    SLIDE_SCORE_API_GET_TILE_SERVER,
    SLIDE_SCORE_API_GET_TOKEN_EXPIRY,
    SLIDE_SCORE_API_I_ENDPOINT,
    SLIDE_SCORE_API_GET_RAW_TILE,
    SLIDE_SCORE_API_LAST,
} slide_score_api_enum;

typedef enum slide_score_failure_kind_t {
	SLIDE_SCORE_FAILURE_NONE = 0,
	SLIDE_SCORE_FAILURE_CREDENTIALS_REQUIRED,
	SLIDE_SCORE_FAILURE_AUTHENTICATION,
	SLIDE_SCORE_FAILURE_TRANSPORT,
	SLIDE_SCORE_FAILURE_HTTP,
	SLIDE_SCORE_FAILURE_INVALID_RESPONSE,
} slide_score_failure_kind_t;

typedef struct slide_score_get_image_metadata_result_t {
    i32 tile_width;
    i32 tile_height;
    i32 osd_tile_size;
    float mpp_x;
    float mpp_y;
    float objective_power;
    char background_color[256];
    i32 level_count;
    i32 z_layer_count;
    i64 level_0_width;
    i64 level_0_height;
    double downsamples[32];
    i32 downsample_count;
    i64 bounds_x;
    i64 bounds_y;
    i64 bounds_width;
    i64 bounds_height;
    char filename[256];
} slide_score_get_image_metadata_result_t;

typedef struct slide_score_get_tile_server_result_t {
    char cookie_part[256];
    char url_part[256];
    char expires_on[256];
} slide_score_get_tile_server_result_t;

typedef struct slide_score_get_slide_details_result_t {
	i32 rotation_degrees_clockwise;
} slide_score_get_slide_details_result_t;

typedef struct slide_score_api_result_t {
    slide_score_api_enum api;
    bool success;
    union {
        void* result_generic;
        slide_score_get_image_metadata_result_t get_image_metadata;
        slide_score_get_tile_server_result_t get_tile_server;
		slide_score_get_slide_details_result_t get_slide_details;
    };
} slide_score_api_result_t;

typedef struct slide_score_client_t {
    char server_name[256];
    char api_key[4096];
} slide_score_client_t;

typedef struct slide_score_slide_ref_t {
	i32 image_id;
	char name[256];
} slide_score_slide_ref_t;

typedef struct slide_score_remote_image_t {
    slide_score_client_t client;
    i32 image_id;
	i32 study_id;
	i32 case_id;
	i32 selected_slide_index;
	i32 rotation_degrees_clockwise;
	/* Owned by the remote image; ownership transfers into image_t during initialization. */
	slide_score_slide_ref_t* case_slides;
	i32 case_slide_count;
	char case_name[256];
    i32 max_deepzoom_level;
    bool use_qupath_tile_endpoint;
    char qupath_base_path[512];
    slide_score_get_image_metadata_result_t metadata;
    slide_score_get_tile_server_result_t tile_server;
} slide_score_remote_image_t;



typedef struct web_api_binding_t {
    const char* name;
    i64 offset;
    field_type_enum field_type;
    bool already_filled;
} web_api_binding_t;

typedef struct web_api_result_descriptor_t {
    web_api_binding_t* bindings_template;
    i32 binding_count;
} web_api_result_descriptor_t;

typedef struct web_api_call_t {
    bool is_valid;
    memrw_t url;
    memrw_t request;
    web_api_binding_t* bindings;
} web_api_call_t;

typedef struct app_state_t app_state_t;
typedef struct image_t image_t;

void slide_score_client_init(slide_score_client_t* ss, const char* server_url_or_hostname, const char* api_key);
slide_score_api_result_t debug_slide_score_api_handle_response(const char* json, size_t json_length, slide_score_api_enum api);
bool slide_score_request_api(slide_score_client_t* ss, slide_score_api_enum api, const char** par_names, const char** par_values, i32 par_count, slide_score_api_result_t* out_result);
bool slide_score_refresh_tile_server(slide_score_remote_image_t* remote);
bool slide_score_open_remote_image(app_state_t* app_state, const char* server_url_or_hostname, const char* api_token, i32 image_id);
bool slide_score_open_case_slide(app_state_t* app_state, image_t* current_image, i32 slide_index);
bool slide_score_open_qupath_metadata_url(app_state_t* app_state, const char* uri);
bool slide_score_uri_is_supported(const char* uri);
bool slide_score_try_open_uri(app_state_t* app_state, const char* uri, const char* api_token);
bool slide_score_last_open_succeeded(void);
slide_score_failure_kind_t slide_score_get_last_failure_kind(void);
slide_score_failure_kind_t slide_score_classify_http_failure(i32 status_code);
bool slide_score_failure_requires_api_token(slide_score_failure_kind_t failure_kind);
bool slide_score_parse_case_images(const char* json, size_t json_length, i32 case_id,
		slide_score_slide_ref_t** out_slides, i32* out_slide_count,
		char* out_case_name, size_t out_case_name_size);
void slide_score_remote_image_destroy(slide_score_remote_image_t* remote);
bool init_image_from_slide_score(image_t* image, slide_score_remote_image_t* remote, bool is_overlay);
char* slide_score_build_tile_url(char* buffer, size_t buffer_size, slide_score_remote_image_t* remote, i32 level, i32 tile_x, i32 tile_y);
char* slide_score_build_qupath_tile_path(char* buffer, size_t buffer_size, slide_score_remote_image_t* remote, i32 level, i32 tile_x, i32 tile_y, i32 tile_width, i32 tile_height, i64 level_width, i64 level_height);
const char* slide_score_get_last_status(void);


#ifdef __cplusplus
}
#endif
