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

#define ARCHIVE_SHELL_FORMAT_VERSION 3
#define ARCHIVE_SHELL_HEADER_SIZE 128
#define ARCHIVE_SHELL_SECTION_ENTRY_SIZE 40
#define ARCHIVE_SHELL_INSTANCE_RECORD_SIZE 192
#define ARCHIVE_SHELL_MAX_INSTANCES 65535
#define ARCHIVE_SHELL_SHARED_SECTION UINT32_MAX

typedef struct dicom_instance_t dicom_instance_t;

typedef enum archive_shell_section_kind_t {
	ARCHIVE_SHELL_SECTION_SOURCE_TABLE = 1,
	ARCHIVE_SHELL_SECTION_STRING_TABLE = 2,
	ARCHIVE_SHELL_SECTION_INSTANCE_TABLE = 3,
	ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE = 4,
	ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE = 5,
	ARCHIVE_SHELL_SECTION_FRAME_MAP = 6,
	ARCHIVE_SHELL_SECTION_PAYLOAD_MAP = 7,
	ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE = 8,
	ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION = 9,
	ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE = 10,
} archive_shell_section_kind_t;

typedef enum archive_shell_frame_map_kind_t {
	ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID = 1,
	ARCHIVE_SHELL_FRAME_MAP_EXPLICIT = 2,
	ARCHIVE_SHELL_FRAME_MAP_SPARSE_RUNS = 3,
} archive_shell_frame_map_kind_t;

typedef enum archive_shell_payload_map_kind_t {
	ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE = 1,
	ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA = 2,
	ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS = 3,
	ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA = 4,
} archive_shell_payload_map_kind_t;

typedef enum archive_shell_provider_kind_t {
	ARCHIVE_SHELL_PROVIDER_PART10 = 1,
	ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE = 2,
} archive_shell_provider_kind_t;

typedef enum archive_shell_attribute_value_kind_t {
	ARCHIVE_SHELL_ATTRIBUTE_STRING = 1,
	ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED = 2,
	ARCHIVE_SHELL_ATTRIBUTE_SIGNED = 3,
} archive_shell_attribute_value_kind_t;

typedef enum archive_shell_optical_field_t {
	ARCHIVE_SHELL_OPTICAL_IDENTIFIER = 1 << 0,
	ARCHIVE_SHELL_OPTICAL_DESCRIPTION = 1 << 1,
	ARCHIVE_SHELL_OPTICAL_COLOR_SPACE = 1 << 2,
	ARCHIVE_SHELL_OPTICAL_ILLUMINATION_WAVELENGTH = 1 << 3,
	ARCHIVE_SHELL_OPTICAL_ILLUMINATION_TYPE = 1 << 4,
	ARCHIVE_SHELL_OPTICAL_ILLUMINATION_COLOR = 1 << 5,
	ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_PASS = 1 << 6,
	ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_BAND = 1 << 7,
	ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_PASS = 1 << 8,
	ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_BAND = 1 << 9,
	ARCHIVE_SHELL_OPTICAL_OBJECTIVE_POWER = 1 << 10,
	ARCHIVE_SHELL_OPTICAL_OBJECTIVE_APERTURE = 1 << 11,
	ARCHIVE_SHELL_OPTICAL_ICC_PROFILE = 1 << 12,
} archive_shell_optical_field_t;

typedef struct archive_shell_build_part10_input_t {
	dicom_instance_t* instance;
	const char* source_sha256;
	u64 source_byte_size;
	const char* representation_fingerprint;
	archive_shell_provider_kind_t provider_kind;
	u32 provider_parameter_version;
	const struct archive_shell_native_payload_t* native_payloads;
	u32 native_payload_count;
} archive_shell_build_part10_input_t;

typedef struct archive_shell_build_part10_aggregate_input_t {
	const archive_shell_build_part10_input_t* instances;
	u32 instance_count;
	const char* representation_fingerprint;
} archive_shell_build_part10_aggregate_input_t;

typedef struct archive_shell_buffer_t {
	u8* data;
	size_t size;
} archive_shell_buffer_t;

typedef struct archive_shell_view_t {
	const u8* data;
	size_t size;
	const u8* section_directory;
	const u8* sources;
	const u8* strings;
	const u8* instances;
	u8 representation_fingerprint[32];
	u32 section_count;
	u32 source_count;
	u32 instance_count;
	u64 strings_size;
} archive_shell_view_t;

typedef struct archive_shell_instance_t {
	u32 instance_index;
	u32 source_index;
	archive_shell_provider_kind_t provider_kind;
	u32 provider_parameter_version;
	archive_shell_payload_map_kind_t payload_map_kind;
	archive_shell_frame_map_kind_t frame_map_kind;
	u32 frame_count;
	u32 rows;
	u32 columns;
	u32 total_pixel_matrix_rows;
	u32 total_pixel_matrix_columns;
	u32 tiles_per_row;
	u32 tiles_per_column;
	u32 optical_path_count;
	u32 focal_plane_count;
	u16 samples_per_pixel;
	u16 planar_configuration;
	u16 bits_allocated;
	u16 bits_stored;
	u16 high_bit;
	u16 pixel_representation;
	i64 instance_number;
	u32 concatenation_frame_offset;
	u32 in_concatenation_number;
	const char* study_instance_uid;
	const char* series_instance_uid;
	const char* sop_instance_uid;
	const char* sop_class_uid;
	const char* transfer_syntax_uid;
	const char* modality;
	const char* image_flavor;
	const char* photometric_interpretation;
	const char* concatenation_uid;
	float pixel_spacing_row_mm;
	float pixel_spacing_column_mm;
	bool has_slide_coordinate_origin;
	float slide_coordinate_origin_x_mm;
	float slide_coordinate_origin_y_mm;
	float slide_coordinate_origin_z_um;
	bool has_image_orientation_slide;
	float image_orientation_slide[6];
	u32 attribute_section_index;
	u32 optical_path_section_index;
	u32 frame_map_section_index;
	u32 payload_map_section_index;
	bool payload_is_encapsulated;
	bool has_image_type;
	bool is_image_original;
	bool is_image_resampled;
	bool is_tiled_full;
} archive_shell_instance_t;

typedef enum archive_shell_source_role_t {
	ARCHIVE_SHELL_SOURCE_ROLE_PART10 = 1,
	ARCHIVE_SHELL_SOURCE_ROLE_NATIVE_BUNDLE = 2,
} archive_shell_source_role_t;

typedef struct archive_shell_source_t {
	u8 sha256[32];
	u64 byte_size;
	archive_shell_source_role_t role;
	u32 flags;
} archive_shell_source_t;

typedef struct archive_shell_payload_range_t {
	u64 offset;
	u32 size;
	u32 source_index;
} archive_shell_payload_range_t;

typedef struct archive_shell_native_payload_t {
	u32 file_index;
	u64 offset;
	u32 size;
} archive_shell_native_payload_t;

typedef struct archive_shell_frame_t {
	u32 row_position;
	u32 column_position;
	float x_offset_mm;
	float y_offset_mm;
	float z_offset_um;
	u32 optical_path_index;
	u32 focal_plane_index;
	u32 flags;
} archive_shell_frame_t;

typedef struct archive_shell_optical_path_t {
	const char* identifier;
	const char* description;
	const char* color_space;
	float illumination_wavelength;
	const char* illumination_type_code_value;
	const char* illumination_type_coding_scheme;
	const char* illumination_type_code_meaning;
	const char* illumination_color_code_value;
	const char* illumination_color_coding_scheme;
	const char* illumination_color_code_meaning;
	const u8* icc_profile;
	u64 icc_profile_size;
	u16 light_path_filter_pass_through_wavelength;
	u16 light_path_filter_pass_band[2];
	u16 image_path_filter_pass_through_wavelength;
	u16 image_path_filter_pass_band[2];
	float objective_lens_power;
	float objective_lens_numerical_aperture;
	u32 source_preserved_fields;
	u32 inferred_fields;
} archive_shell_optical_path_t;

typedef struct archive_shell_specimen_t {
	const char* identifier;
	const char* uid;
	const char* short_description;
} archive_shell_specimen_t;

typedef enum archive_shell_content_value_kind_t {
	ARCHIVE_SHELL_CONTENT_CODE = 1,
	ARCHIVE_SHELL_CONTENT_TEXT = 2,
	ARCHIVE_SHELL_CONTENT_NUM = 3,
	ARCHIVE_SHELL_CONTENT_UID = 4,
	ARCHIVE_SHELL_CONTENT_DATETIME = 5,
	ARCHIVE_SHELL_CONTENT_DATE = 6,
	ARCHIVE_SHELL_CONTENT_TIME = 7,
	ARCHIVE_SHELL_CONTENT_PNAME = 8,
} archive_shell_content_value_kind_t;

typedef struct archive_shell_preparation_item_t {
	u32 specimen_index;
	u32 step_index;
	u32 parent_item_index;
	archive_shell_content_value_kind_t value_kind;
	const char* concept_name_code_value;
	const char* concept_name_coding_scheme;
	const char* concept_name_coding_scheme_version;
	const char* concept_name_code_meaning;
	const char* code_value;
	const char* code_coding_scheme;
	const char* code_coding_scheme_version;
	const char* code_meaning;
	const char* scalar_value;
	const char* measurement_units_code_value;
	const char* measurement_units_coding_scheme;
	const char* measurement_units_coding_scheme_version;
	const char* measurement_units_code_meaning;
	const char* relationship_type;
} archive_shell_preparation_item_t;

typedef struct archive_shell_attribute_t {
	u32 tag;
	char vr[3];
	archive_shell_attribute_value_kind_t value_kind;
	const char* string_value;
	u64 unsigned_value;
	i64 signed_value;
} archive_shell_attribute_t;

bool archive_shell_build_part10(const archive_shell_build_part10_input_t* input,
		archive_shell_buffer_t* shell, char* error, size_t error_capacity);
bool archive_shell_build_part10_aggregate(
		const archive_shell_build_part10_aggregate_input_t* input,
		archive_shell_buffer_t* shell, char* error, size_t error_capacity);
bool archive_shell_open(archive_shell_view_t* shell, const void* data, size_t size,
		char* error, size_t error_capacity);
bool archive_shell_get_source(const archive_shell_view_t* shell, u32 source_index,
		archive_shell_source_t* source);
bool archive_shell_get_instance(const archive_shell_view_t* shell, u32 instance_index,
		archive_shell_instance_t* instance);
bool archive_shell_get_payload_range(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_payload_range_t* range);
bool archive_shell_build_payload_ranges(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		archive_shell_payload_range_t* ranges, u32 range_count);
bool archive_shell_get_native_payload(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_native_payload_t* payload);
bool archive_shell_get_frame(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_frame_t* frame);
bool archive_shell_get_optical_path(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 optical_path_index,
		archive_shell_optical_path_t* optical_path);
const char* archive_shell_get_container_identifier(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance);
u32 archive_shell_get_specimen_count(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance);
bool archive_shell_get_specimen(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 specimen_index,
		archive_shell_specimen_t* specimen);
bool archive_shell_specimen_metadata_is_shared(
		const archive_shell_view_t* shell);
bool archive_shell_preparation_metadata_is_shared(
		const archive_shell_view_t* shell);
u32 archive_shell_get_preparation_item_count(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance);
bool archive_shell_get_preparation_item(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 item_index,
		archive_shell_preparation_item_t* item);
u32 archive_shell_get_attribute_count(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance);
u32 archive_shell_get_governed_attribute_count(
		const archive_shell_view_t* shell);
u32 archive_shell_get_attribute_override_count(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance);
bool archive_shell_get_attribute(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 attribute_index,
		archive_shell_attribute_t* attribute);
void archive_shell_buffer_destroy(archive_shell_buffer_t* shell);

#ifdef __cplusplus
}
#endif
