/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_repository.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHIVE_MANIFEST_SCHEMA_VERSION 2

typedef struct dicom_instance_t dicom_instance_t;

/*
 * These structs are intentionally small staging contracts, not the final database
 * schema and not a complete DICOM WSI information model. They make identity,
 * provenance, and representation versioning explicit while the publication/provider
 * boundaries are developed. A production representation also needs governed
 * clinical/specimen metadata, complete optical-path definitions, color policy, and
 * durable publication; see pathology_archive_architecture.md.
 */

typedef enum archive_source_format_t {
	ARCHIVE_SOURCE_FORMAT_UNKNOWN = 0,
	ARCHIVE_SOURCE_FORMAT_DICOM,
	ARCHIVE_SOURCE_FORMAT_MRXS,
	ARCHIVE_SOURCE_FORMAT_ISYNTAX,
	ARCHIVE_SOURCE_FORMAT_TIFF,
} archive_source_format_t;

typedef struct archive_source_bundle_manifest_t {
	char bundle_id[65];
	archive_source_format_t source_format;
	char bundle_digest_sha256[65];
	char adapter_name[65];
	char adapter_version[33];
	u32 object_count;
	u64 total_bytes;
} archive_source_bundle_manifest_t;

typedef enum archive_manifest_provider_kind_t {
	ARCHIVE_MANIFEST_PROVIDER_UNKNOWN = 0,
	ARCHIVE_MANIFEST_PROVIDER_PART10,
	ARCHIVE_MANIFEST_PROVIDER_NATIVE,
} archive_manifest_provider_kind_t;

typedef enum archive_manifest_representation_source_t {
	ARCHIVE_MANIFEST_REPRESENTATION_UNKNOWN = 0,
	ARCHIVE_MANIFEST_REPRESENTATION_PASSTHROUGH,
	ARCHIVE_MANIFEST_REPRESENTATION_GENERATED,
} archive_manifest_representation_source_t;

typedef enum archive_manifest_frame_map_kind_t {
	ARCHIVE_MANIFEST_FRAME_MAP_UNKNOWN = 0,
	ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR,
	ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT,
} archive_manifest_frame_map_kind_t;

typedef struct archive_virtual_instance_manifest_t {
	char representation_id[65];
	char bundle_id[65];
	u32 representation_version;
	char study_instance_uid[65];
	char series_instance_uid[65];
	char sop_instance_uid[65];
	char sop_class_uid[65];
	char transfer_syntax_uid[65];
	char image_flavor[17];
	char photometric_interpretation[17];
	u32 instance_number;
	u32 frame_count;
	u32 rows;
	u32 columns;
	u32 total_pixel_matrix_rows;
	u32 total_pixel_matrix_columns;
	archive_manifest_provider_kind_t provider_kind;
	char provider_locator[257];
	archive_manifest_representation_source_t representation_source;
	archive_manifest_frame_map_kind_t frame_map_kind;
	char frame_map_object_key[257];
	u32 tiles_per_row;
	u32 tiles_per_column;
	u32 optical_path_count;
	u32 focal_plane_count;
	char default_optical_path_identifier[17];
	bool has_default_focal_plane_z_offset;
	float default_focal_plane_z_offset_um;
	char frame_dimension_map_object_key[257];
	float pixel_spacing_row_mm;
	float pixel_spacing_column_mm;
	bool has_slide_coordinate_origin;
	float slide_coordinate_origin_x_mm;
	float slide_coordinate_origin_y_mm;
	float slide_coordinate_origin_z_mm;
	bool has_image_orientation_slide;
	float image_orientation_slide[6];
	char icc_profile_digest_sha256[65];
} archive_virtual_instance_manifest_t;

bool archive_source_bundle_manifest_is_valid(const archive_source_bundle_manifest_t* manifest);
bool archive_virtual_instance_manifest_is_valid(const archive_virtual_instance_manifest_t* manifest);
/*
 * Populate fields that are deterministic properties of one parsed Part 10 instance.
 * The caller supplies representation/bundle identity and any explicit map object keys.
 * The result is intentionally not guaranteed to validate when source metadata requires
 * an explicit spatial or dimension map that has not been materialized yet.
 */
bool archive_manifest_populate_part10_instance(archive_virtual_instance_manifest_t* manifest,
		const dicom_instance_t* instance);
bool archive_manifest_materialize_part10_frame_map(const archive_repository_t* repository,
		archive_virtual_instance_manifest_t* manifest, const dicom_instance_t* instance,
		char* error, size_t error_capacity);
bool archive_manifest_stage_source_bundle(const archive_repository_t* repository,
		const archive_source_bundle_manifest_t* manifest, char* error, size_t error_capacity);
bool archive_manifest_stage_virtual_instance(const archive_repository_t* repository,
		const archive_virtual_instance_manifest_t* manifest, char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
