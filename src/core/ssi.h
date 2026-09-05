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

typedef struct app_state_t app_state_t;
typedef struct image_t image_t;

typedef enum ssi_failure_kind_t {
	SSI_FAILURE_NONE = 0,
	SSI_FAILURE_CREDENTIALS_REQUIRED,
	SSI_FAILURE_AUTHENTICATION,
	SSI_FAILURE_SESSION_EXPIRED,
	SSI_FAILURE_TRANSIENT,
	SSI_FAILURE_OTHER,
} ssi_failure_kind_t;

typedef struct ssi_endpoint_t {
	char hostname[256];
	char host_header[320];
	char root[512];
	i32 port;
	bool use_tls;
} ssi_endpoint_t;

typedef struct ssi_slide_properties_t {
	char original_name[512];
	char folder_id[64];
	i64 width;
	i64 height;
	float mpp_x;
	float mpp_y;
	i32 magnification_level_max;
	i32 bits_per_channel;
	i32 channel_count;
	i32 focus_level_min;
	i32 focus_level_max;
	i32 tile_x_min_native;
	i32 tile_x_max_native;
	i32 tile_y_min_native;
	i32 tile_y_max_native;
	bool is_brightfield;
} ssi_slide_properties_t;

typedef struct ssi_slide_ref_t {
	char id[64];
	char name[512];
	char compact_name[512];
	char barcode[256];
} ssi_slide_ref_t;

typedef struct ssi_remote_image_t {
	ssi_endpoint_t endpoint;
	ssi_slide_properties_t properties;
	char display_name[512];
	char api_url[768];
	char slide_center_url[768];
	char session_id[128];
	char slide_token[128];
	char slide_guid[64];
	char folder_name[512];
	char* slide_attributes_json;
	size_t slide_attributes_json_size;
	char* all_properties_json;
	size_t all_properties_json_size;
	ssi_slide_ref_t* folder_slides;
	i32 folder_slide_count;
	bool folder_slides_compact;
	i32 initial_request_count;
	i32 initial_request_time_ms;
	u64 initial_response_bytes;
	i64 viewer_ready_clock;
	volatile i32 first_tile_decoded_reported;
	volatile i32 first_tile_displayed_reported;
	volatile i32 reauthentication_state;
	volatile i32 keepalive_in_flight;
	i64 keepalive_last_scheduled_clock;
	i64 reauthentication_last_attempt_clock;
	i32 reauthentication_attempt_count;
	ssi_failure_kind_t failure_kind;
	bool session_open;
	bool slide_open;
} ssi_remote_image_t;

typedef struct ssi_uri_config_t {
	char api_url[768];
	char slide_center_url[768];
	char slide_guid[64];
	char session_id[128];
	char caption[512];
} ssi_uri_config_t;

typedef struct ssi_level_geometry_t {
	i32 tile_x_min;
	i32 tile_x_max;
	i32 tile_y_min;
	i32 tile_y_max;
	i32 width_in_tiles;
	i32 height_in_tiles;
} ssi_level_geometry_t;

bool ssi_parse_endpoint(const char* api_url, ssi_endpoint_t* endpoint);
bool ssi_parse_json_string(const char* json, size_t json_size, char* dest, size_t dest_size);
bool ssi_parse_slide_properties(const char* json, size_t json_size,
		ssi_slide_properties_t* properties);
bool ssi_parse_slide_attribute_name(const char* json, size_t json_size,
		char* name, size_t name_size);
bool ssi_parse_folder_content(const char* json, size_t json_size,
		ssi_slide_ref_t** slides, i32* slide_count, char* folder_name,
		size_t folder_name_size, bool* compact_names);
bool ssi_get_level_geometry(const ssi_slide_properties_t* properties, i32 level,
		ssi_level_geometry_t* geometry);
bool ssi_build_tile_path(char* dest, size_t dest_size, const ssi_remote_image_t* remote,
		i32 level, i32 tile_x, i32 tile_y);
bool ssi_build_tile_rectangle_path(char* dest, size_t dest_size,
		const ssi_remote_image_t* remote, i32 level,
		i32 tile_x_min, i32 tile_y_min, i32 tile_x_max, i32 tile_y_max);
bool ssi_uri_is_supported(const char* uri);
bool ssi_parse_uri_slide_guid(const char* uri, char* slide_guid, size_t slide_guid_size);
bool ssi_parse_mview_uri(const char* uri, ssi_uri_config_t* config);
bool ssi_resolve_uri(const char* uri, ssi_uri_config_t* config);

bool ssi_open_remote_image(app_state_t* app_state, const char* api_url,
		const char* slide_center_url, const char* username, const char* password,
		const char* existing_session_id, const char* slide_guid, const char* caption);
bool ssi_open_folder_slide(app_state_t* app_state, image_t* current_image,
		i32 slide_index);
bool ssi_open_uri_with_credentials(app_state_t* app_state, const char* uri,
		const char* username, const char* password);
bool ssi_try_open_uri(app_state_t* app_state, const char* uri);
const char* ssi_get_last_status(void);
ssi_failure_kind_t ssi_get_last_failure_kind(void);
ssi_failure_kind_t ssi_classify_http_failure(i32 status_code,
		bool credential_request, bool expiring_handle_request);
bool ssi_failure_requires_credentials(ssi_failure_kind_t failure_kind);
void ssi_poll_keepalive(app_state_t* app_state);
void ssi_close_remote_image(ssi_remote_image_t* remote);
void ssi_commit_optional_data(app_state_t* app_state, void* result_pointer);
void ssi_load_tile_batch_func(i32 logical_thread_index, void* userdata);

#define SSI_COMPLETION_EVENT_OPTIONAL_DATA 0x5353494fu

#ifdef __cplusplus
}
#endif
