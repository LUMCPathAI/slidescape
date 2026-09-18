/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_shell.h"

#include "archive_frame_map.h"
#include "dicom.h"

#include <math.h>
#include <stdarg.h>

#define ARCHIVE_SHELL_SOURCE_RECORD_SIZE 48
#define ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE 16
#define ARCHIVE_SHELL_OPTICAL_HEADER_SIZE 16
#define ARCHIVE_SHELL_OPTICAL_RECORD_SIZE 96
#define ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE 16
#define ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE 16
#define ARCHIVE_SHELL_PREPARATION_HEADER_SIZE 16
#define ARCHIVE_SHELL_PREPARATION_RECORD_SIZE 72
#define ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE 32
#define ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE 4
#define ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE 4
#define ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE 8
#define ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE 4
#define ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE 4
#define ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE 32
#define ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE 16
#define ARCHIVE_SHELL_SPARSE_RUN_VERSION 1
#define ARCHIVE_SHELL_SECTION_CRITICAL 1
typedef enum archive_shell_instance_flags_t {
	ARCHIVE_SHELL_INSTANCE_HAS_ORIGIN = 1 << 0,
	ARCHIVE_SHELL_INSTANCE_HAS_ORIENTATION = 1 << 1,
	ARCHIVE_SHELL_INSTANCE_HAS_IMAGE_TYPE = 1 << 2,
	ARCHIVE_SHELL_INSTANCE_IMAGE_ORIGINAL = 1 << 3,
	ARCHIVE_SHELL_INSTANCE_IMAGE_RESAMPLED = 1 << 4,
	ARCHIVE_SHELL_INSTANCE_TILED_FULL = 1 << 5,
} archive_shell_instance_flags_t;
#define ARCHIVE_SHELL_PAYLOAD_ENCAPSULATED 1
#define ARCHIVE_SHELL_NO_SECTION UINT32_MAX

/* The first nine slots are stored in the fixed instance record, in this order. */
typedef enum archive_shell_string_slot_t {
	ARCHIVE_SHELL_STRING_STUDY_UID = 0,
	ARCHIVE_SHELL_STRING_SERIES_UID = 1,
	ARCHIVE_SHELL_STRING_SOP_INSTANCE_UID = 2,
	ARCHIVE_SHELL_STRING_SOP_CLASS_UID = 3,
	ARCHIVE_SHELL_STRING_TRANSFER_SYNTAX_UID = 4,
	ARCHIVE_SHELL_STRING_MODALITY = 5,
	ARCHIVE_SHELL_STRING_IMAGE_FLAVOR = 6,
	ARCHIVE_SHELL_STRING_PHOTOMETRIC = 7,
	ARCHIVE_SHELL_STRING_CONCATENATION_UID = 8,
	ARCHIVE_SHELL_STRING_PATIENT_ID = 9,
	ARCHIVE_SHELL_STRING_PATIENT_BIRTH_DATE = 10,
	ARCHIVE_SHELL_STRING_PATIENT_SEX = 11,
	ARCHIVE_SHELL_STRING_STUDY_DATE = 12,
	ARCHIVE_SHELL_STRING_ACCESSION_NUMBER = 13,
	ARCHIVE_SHELL_STRING_STUDY_ID = 14,
	ARCHIVE_SHELL_STRING_PATIENT_NAME = 15,
	ARCHIVE_SHELL_STRING_COUNT,
} archive_shell_string_slot_t;

static const u8 archive_shell_magic[8] = {'S', 'S', 'H', 'E', 'L', 'L', '1', '\0'};
static const u8 archive_shell_frame_map_magic[8] = {'S', 'S', 'F', 'M', 'A', 'P', '1', '\0'};
static const u8 archive_shell_sparse_run_magic[8] =
	{'S', 'S', 'R', 'U', 'N', '1', '\0', '\0'};

typedef struct archive_shell_section_t {
	u32 kind;
	u32 instance_index;
	u32 flags;
	u8* data;
	size_t size;
} archive_shell_section_t;

typedef struct archive_shell_strings_t {
	u8* data;
	size_t size;
	size_t capacity;
} archive_shell_strings_t;

static u32 archive_shell_attribute_table_count(
		const u8* section, u64 size);

static void archive_shell_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || !capacity) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static size_t archive_shell_align8(size_t value) {
	return (value + 7) & ~(size_t)7;
}

static u32 archive_shell_read_be_u32(const u8* source) {
	return ((u32)source[0] << 24) | ((u32)source[1] << 16) |
	       ((u32)source[2] << 8) | source[3];
}

static bool archive_shell_icc_profile_is_valid(const u8* profile, u32 size) {
	if (!profile || size < 128 || memcmp(profile + 36, "acsp", 4) != 0) return false;
	u32 declared_size = archive_shell_read_be_u32(profile);
	return declared_size >= 128 && declared_size <= size &&
	       size - declared_size <= 1;
}

static void archive_shell_write_u16(u8* dest, u16 value) {
	dest[0] = (u8)value;
	dest[1] = (u8)(value >> 8);
}

static void archive_shell_write_u32(u8* dest, u32 value) {
	dest[0] = (u8)value;
	dest[1] = (u8)(value >> 8);
	dest[2] = (u8)(value >> 16);
	dest[3] = (u8)(value >> 24);
}

static void archive_shell_write_u64(u8* dest, u64 value) {
	for (i32 i = 0; i < 8; ++i) dest[i] = (u8)(value >> (i * 8));
}

static void archive_shell_write_i64(u8* dest, i64 value) {
	archive_shell_write_u64(dest, (u64)value);
}

static void archive_shell_write_float(u8* dest, float value) {
	u32 bits;
	memcpy(&bits, &value, sizeof(bits));
	archive_shell_write_u32(dest, bits);
}

static u16 archive_shell_read_u16(const u8* source) {
	return (u16)source[0] | (u16)((u16)source[1] << 8);
}

static u32 archive_shell_read_u32(const u8* source) {
	return (u32)source[0] | ((u32)source[1] << 8) |
	       ((u32)source[2] << 16) | ((u32)source[3] << 24);
}

static u64 archive_shell_read_u64(const u8* source) {
	u64 value = 0;
	for (i32 i = 0; i < 8; ++i) value |= (u64)source[i] << (i * 8);
	return value;
}

static i64 archive_shell_read_i64(const u8* source) {
	return (i64)archive_shell_read_u64(source);
}

static float archive_shell_read_float(const u8* source) {
	u32 bits = archive_shell_read_u32(source);
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static bool archive_shell_hex_digest(const char* text, u8 digest[32]) {
	if (!text || strlen(text) != 64) return false;
	for (i32 i = 0; i < 32; ++i) {
		i32 high = text[i * 2];
		i32 low = text[i * 2 + 1];
		high = high >= '0' && high <= '9' ? high - '0' :
		       high >= 'a' && high <= 'f' ? high - 'a' + 10 :
		       high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
		low = low >= '0' && low <= '9' ? low - '0' :
		      low >= 'a' && low <= 'f' ? low - 'a' + 10 :
		      low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;
		if (high < 0 || low < 0) return false;
		digest[i] = (u8)((high << 4) | low);
	}
	return true;
}

static bool archive_shell_strings_reserve(archive_shell_strings_t* strings, size_t required) {
	if (required <= strings->capacity) return true;
	size_t capacity = strings->capacity ? strings->capacity * 2 : 256;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2) return false;
		capacity *= 2;
	}
	u8* data = realloc(strings->data, capacity);
	if (!data) return false;
	strings->data = data;
	strings->capacity = capacity;
	return true;
}

static bool archive_shell_strings_initialize(archive_shell_strings_t* strings) {
	if (!archive_shell_strings_reserve(strings, 1)) return false;
	strings->data[0] = '\0';
	strings->size = 1;
	return true;
}

static bool archive_shell_strings_add(archive_shell_strings_t* strings,
		const char* value, u32* offset) {
	if (!strings || !offset) return false;
	if (!value || !value[0]) {
		*offset = 0;
		return true;
	}
	size_t length = strlen(value) + 1;
	if (strings->size > UINT32_MAX || length > UINT32_MAX - strings->size ||
	    !archive_shell_strings_reserve(strings, strings->size + length)) return false;
	*offset = (u32)strings->size;
	memcpy(strings->data + strings->size, value, length);
	strings->size += length;
	return true;
}

static const char* archive_shell_image_flavor(const dicom_instance_t* instance) {
	switch (instance->image_flavor) {
		case DICOM_IMAGE_FLAVOR_VOLUME: return "VOLUME";
		case DICOM_IMAGE_FLAVOR_LABEL: return "LABEL";
		case DICOM_IMAGE_FLAVOR_OVERVIEW: return "OVERVIEW";
		case DICOM_IMAGE_FLAVOR_THUMBNAIL: return "THUMBNAIL";
		default: return "";
	}
}

static const char* archive_shell_photometric(const dicom_instance_t* instance) {
	static const char* values[] = {
		"", "MONOCHROME1", "MONOCHROME2", "PALETTE COLOR", "RGB", "HSV", "ARGB", "CMYK",
		"YBR_FULL", "YBR_FULL_422", "YBR_PARTIAL_422", "YBR_PARTIAL_420", "YBR_ICT", "YBR_RCT"
	};
	return instance->photometric_interpretation > 0 &&
	       instance->photometric_interpretation < COUNT(values)
	       ? values[instance->photometric_interpretation] : "";
}

static void archive_shell_write_attribute(u8* record, u32 tag, char vr0, char vr1,
		u16 type, u64 value) {
	archive_shell_write_u32(record, tag);
	archive_shell_write_u16(record + 4, (u16)(u8)vr0 | ((u16)(u8)vr1 << 8));
	archive_shell_write_u16(record + 6, type);
	archive_shell_write_u64(record + 8, value);
}

static bool archive_shell_build_attributes(const dicom_instance_t* instance,
		const u32 string_offsets[ARCHIVE_SHELL_STRING_COUNT], archive_shell_section_t* section) {
	enum { ATTRIBUTE_COUNT = 22 };
	section->size = 16 + ATTRIBUTE_COUNT * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
	section->data = calloc(1, section->size);
	if (!section->data) return false;
	archive_shell_write_u32(section->data, ATTRIBUTE_COUNT);
	archive_shell_write_u32(section->data + 4, ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE);
	u8* record = section->data + 16;
#define WRITE_ATTRIBUTE(tag, a, b, type, value) \
	do { archive_shell_write_attribute(record, tag, a, b, type, (u64)(value)); \
	     record += ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE; } while (0)
	WRITE_ATTRIBUTE(0x00080016, 'U', 'I', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_SOP_CLASS_UID]);
	WRITE_ATTRIBUTE(0x00080018, 'U', 'I', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_SOP_INSTANCE_UID]);
	WRITE_ATTRIBUTE(0x00080020, 'D', 'A', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_STUDY_DATE]);
	WRITE_ATTRIBUTE(0x00080050, 'S', 'H', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_ACCESSION_NUMBER]);
	WRITE_ATTRIBUTE(0x00080060, 'C', 'S', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_MODALITY]);
	WRITE_ATTRIBUTE(0x00100010, 'P', 'N', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_PATIENT_NAME]);
	WRITE_ATTRIBUTE(0x00100020, 'L', 'O', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_PATIENT_ID]);
	WRITE_ATTRIBUTE(0x00100030, 'D', 'A', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_PATIENT_BIRTH_DATE]);
	WRITE_ATTRIBUTE(0x00100040, 'C', 'S', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_PATIENT_SEX]);
	WRITE_ATTRIBUTE(0x0020000D, 'U', 'I', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_STUDY_UID]);
	WRITE_ATTRIBUTE(0x0020000E, 'U', 'I', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_SERIES_UID]);
	WRITE_ATTRIBUTE(0x00200010, 'S', 'H', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_STUDY_ID]);
	WRITE_ATTRIBUTE(0x00200013, 'I', 'S', ARCHIVE_SHELL_ATTRIBUTE_SIGNED, instance->instance_number);
	WRITE_ATTRIBUTE(0x00280002, 'U', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->samples_per_pixel);
	WRITE_ATTRIBUTE(0x00280004, 'C', 'S', ARCHIVE_SHELL_ATTRIBUTE_STRING, string_offsets[ARCHIVE_SHELL_STRING_PHOTOMETRIC]);
	WRITE_ATTRIBUTE(0x00280008, 'I', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->number_of_frames);
	WRITE_ATTRIBUTE(0x00280010, 'U', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->rows);
	WRITE_ATTRIBUTE(0x00280011, 'U', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->columns);
	WRITE_ATTRIBUTE(0x00280100, 'U', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->bits_allocated);
	WRITE_ATTRIBUTE(0x00280101, 'U', 'S', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->bits_stored);
	WRITE_ATTRIBUTE(0x00480006, 'U', 'L', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->total_pixel_matrix_columns);
	WRITE_ATTRIBUTE(0x00480007, 'U', 'L', ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED, instance->total_pixel_matrix_rows);
#undef WRITE_ATTRIBUTE
	return true;
}

static bool archive_shell_build_optical_paths(const dicom_instance_t* instance,
		archive_shell_strings_t* strings, archive_shell_section_t* section) {
	i32 count = arrlen(instance->optical_paths);
	if (count <= 0) count = 1;
	u64 records_size = (u64)count * ARCHIVE_SHELL_OPTICAL_RECORD_SIZE;
	u64 size = ARCHIVE_SHELL_OPTICAL_HEADER_SIZE + records_size;
	for (i32 i = 0; i < count; ++i) {
		if (size > SIZE_MAX - 7) return false;
		size = (size + 7) & ~(u64)7;
		u64 profile_size = arrlen(instance->optical_paths) > 0 ?
			instance->optical_paths[i].icc_profile_size : 0;
		if (profile_size > SIZE_MAX - size) return false;
		size += profile_size;
	}
	section->data = calloc(1, (size_t)size);
	if (!section->data) return false;
	section->size = (size_t)size;
	archive_shell_write_u32(section->data, (u32)count);
	archive_shell_write_u32(section->data + 4, ARCHIVE_SHELL_OPTICAL_RECORD_SIZE);
	size_t blob_offset = ARCHIVE_SHELL_OPTICAL_HEADER_SIZE + (size_t)records_size;
	for (i32 i = 0; i < count; ++i) {
		const dicom_optical_path_t* source =
			arrlen(instance->optical_paths) > 0 ? instance->optical_paths + i : NULL;
		u8* record = section->data + ARCHIVE_SHELL_OPTICAL_HEADER_SIZE +
		             (size_t)i * ARCHIVE_SHELL_OPTICAL_RECORD_SIZE;
		u32 identifier_offset = 0;
		u32 description_offset = 0;
		u32 color_space_offset = 0;
		u32 illumination_type_offsets[3] = {0};
		u32 illumination_color_offsets[3] = {0};
		if (!archive_shell_strings_add(strings,
				source && source->OpticalPathIdentifier.present ?
				source->OpticalPathIdentifier.value : "0", &identifier_offset) ||
		    !archive_shell_strings_add(strings,
				source && source->OpticalPathDescription.present ?
				source->OpticalPathDescription.value : "", &description_offset) ||
		    !archive_shell_strings_add(strings,
				source && source->ColorSpace.present ?
				source->ColorSpace.value : "", &color_space_offset) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_type.CodeValue.present ?
				source->illumination_type.CodeValue.value : "",
				illumination_type_offsets) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_type.CodingSchemeDesignator.present ?
				source->illumination_type.CodingSchemeDesignator.value : "",
				illumination_type_offsets + 1) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_type.CodeMeaning.present ?
				source->illumination_type.CodeMeaning.value : "",
				illumination_type_offsets + 2) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_color.CodeValue.present ?
				source->illumination_color.CodeValue.value : "",
				illumination_color_offsets) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_color.CodingSchemeDesignator.present ?
				source->illumination_color.CodingSchemeDesignator.value : "",
				illumination_color_offsets + 1) ||
		    !archive_shell_strings_add(strings,
				source && source->illumination_color.CodeMeaning.present ?
				source->illumination_color.CodeMeaning.value : "",
				illumination_color_offsets + 2)) return false;
		archive_shell_write_u32(record, identifier_offset);
		archive_shell_write_u32(record + 4, color_space_offset);
		archive_shell_write_float(record + 8, source ? source->illumination_wavelength : 0.0f);
		for (i32 field = 0; field < 3; ++field) {
			archive_shell_write_u32(record + 32 + field * 4,
			                        illumination_type_offsets[field]);
			archive_shell_write_u32(record + 44 + field * 4,
			                        illumination_color_offsets[field]);
		}
		archive_shell_write_u32(record + 56, description_offset);
		u32 source_fields = 0;
		u32 inferred_fields = 0;
		if (source) {
			if (source->OpticalPathIdentifier.present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_IDENTIFIER;
			else
				inferred_fields |= ARCHIVE_SHELL_OPTICAL_IDENTIFIER;
			if (source->OpticalPathDescription.present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_DESCRIPTION;
			if (source->ColorSpace.present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_COLOR_SPACE;
			if (source->illumination_wavelength_present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_ILLUMINATION_WAVELENGTH;
			if (source->illumination_type.CodeValue.present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_ILLUMINATION_TYPE;
			if (source->illumination_color.CodeValue.present)
				source_fields |= ARCHIVE_SHELL_OPTICAL_ILLUMINATION_COLOR;
			if (source->present_fields & DICOM_OPTICAL_LIGHT_FILTER_PASS)
				source_fields |= ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_PASS;
			if (source->present_fields & DICOM_OPTICAL_LIGHT_FILTER_BAND)
				source_fields |= ARCHIVE_SHELL_OPTICAL_LIGHT_FILTER_BAND;
			if (source->present_fields & DICOM_OPTICAL_IMAGE_FILTER_PASS)
				source_fields |= ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_PASS;
			if (source->present_fields & DICOM_OPTICAL_IMAGE_FILTER_BAND)
				source_fields |= ARCHIVE_SHELL_OPTICAL_IMAGE_FILTER_BAND;
			if (source->present_fields & DICOM_OPTICAL_OBJECTIVE_POWER)
				source_fields |= ARCHIVE_SHELL_OPTICAL_OBJECTIVE_POWER;
			if (source->present_fields & DICOM_OPTICAL_OBJECTIVE_APERTURE)
				source_fields |= ARCHIVE_SHELL_OPTICAL_OBJECTIVE_APERTURE;
			if (source->icc_profile_size)
				source_fields |= ARCHIVE_SHELL_OPTICAL_ICC_PROFILE;
		} else {
			inferred_fields |= ARCHIVE_SHELL_OPTICAL_IDENTIFIER;
		}
		archive_shell_write_u32(record + 60, source_fields);
		archive_shell_write_u32(record + 64, inferred_fields);
		if (source) {
			archive_shell_write_u16(
				record + 68, source->light_path_filter_pass_through_wavelength);
			archive_shell_write_u16(record + 70, source->light_path_filter_pass_band[0]);
			archive_shell_write_u16(record + 72, source->light_path_filter_pass_band[1]);
			archive_shell_write_u16(
				record + 74, source->image_path_filter_pass_through_wavelength);
			archive_shell_write_u16(record + 76, source->image_path_filter_pass_band[0]);
			archive_shell_write_u16(record + 78, source->image_path_filter_pass_band[1]);
			archive_shell_write_float(record + 80, source->objective_lens_power);
			archive_shell_write_float(
				record + 84, source->objective_lens_numerical_aperture);
		}
		blob_offset = archive_shell_align8(blob_offset);
		if (source && source->icc_profile_size) {
			archive_shell_write_u64(record + 16, blob_offset);
			archive_shell_write_u64(record + 24, source->icc_profile_size);
			memcpy(section->data + blob_offset, source->icc_profile, source->icc_profile_size);
			blob_offset += source->icc_profile_size;
		}
	}
	return true;
}

static bool archive_shell_build_specimens(const dicom_instance_t* instance,
		archive_shell_strings_t* strings, archive_shell_section_t* section) {
	i32 count = arrlen(instance->specimens);
	if (count < 0 || (u64)count >
	    (SIZE_MAX - ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE) /
	    ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE) return false;
	section->size = ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE +
	                (size_t)count * ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE;
	section->data = calloc(1, section->size);
	if (!section->data) return false;
	archive_shell_write_u32(section->data, (u32)count);
	archive_shell_write_u32(section->data + 4, ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE);
	u32 container_offset = 0;
	if (!archive_shell_strings_add(
			strings, instance->ContainerIdentifier.present ?
			instance->ContainerIdentifier.value : "", &container_offset)) return false;
	archive_shell_write_u32(section->data + 8, container_offset);
	for (i32 i = 0; i < count; ++i) {
		const dicom_specimen_t* source = instance->specimens + i;
		u32 offsets[3] = {0};
		if (!archive_shell_strings_add(
				strings, source->SpecimenIdentifier.present ?
				source->SpecimenIdentifier.value : "", offsets) ||
		    !archive_shell_strings_add(
				strings, source->SpecimenUID.value[0] ?
				source->SpecimenUID.value : "", offsets + 1) ||
		    !archive_shell_strings_add(
				strings, source->SpecimenShortDescription.present ?
				source->SpecimenShortDescription.value : "", offsets + 2)) return false;
		u8* record = section->data + ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE +
		             (size_t)i * ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE;
		for (i32 field = 0; field < 3; ++field)
			archive_shell_write_u32(record + field * 4, offsets[field]);
	}
	return true;
}

static bool archive_shell_build_preparation_items(
		const dicom_instance_t* instance,
		archive_shell_strings_t* strings, archive_shell_section_t* section,
		char* error, size_t error_capacity) {
	u64 item_count64 = 0;
	for (i32 specimen_index = 0;
	     specimen_index < arrlen(instance->specimens); ++specimen_index) {
		const dicom_specimen_t* specimen =
			instance->specimens + specimen_index;
		for (i32 step_index = 0;
		     step_index < arrlen(specimen->preparation_steps); ++step_index)
			item_count64 +=
				(u64)arrlen(specimen->preparation_steps[step_index].items);
	}
	if (item_count64 > UINT32_MAX ||
	    item_count64 >
	    (SIZE_MAX - ARCHIVE_SHELL_PREPARATION_HEADER_SIZE) /
	    ARCHIVE_SHELL_PREPARATION_RECORD_SIZE) return false;
	u32 item_count = (u32)item_count64;
	section->size = ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
	                (size_t)item_count * ARCHIVE_SHELL_PREPARATION_RECORD_SIZE;
	section->data = calloc(1, section->size);
	if (!section->data) return false;
	archive_shell_write_u32(section->data, item_count);
	archive_shell_write_u32(
		section->data + 4, ARCHIVE_SHELL_PREPARATION_RECORD_SIZE);
	u32 output_index = 0;
	for (i32 specimen_index = 0;
	     specimen_index < arrlen(instance->specimens); ++specimen_index) {
		const dicom_specimen_t* specimen =
			instance->specimens + specimen_index;
		for (i32 step_index = 0;
		     step_index < arrlen(specimen->preparation_steps); ++step_index) {
			const dicom_specimen_preparation_step_t* step =
				specimen->preparation_steps + step_index;
			u32 step_base = output_index;
			for (i32 item_index = 0;
			     item_index < arrlen(step->items); ++item_index) {
				const dicom_specimen_preparation_item_t* source =
					step->items + item_index;
				if (source->value_type < DICOM_CONTENT_VALUE_CODE ||
				    source->value_type > DICOM_CONTENT_VALUE_PNAME) {
					archive_shell_set_error(
						error, error_capacity,
						"specimen %d preparation step %d item %d has "
						"an unsupported or missing Value Type",
						specimen_index, step_index, item_index);
					return false;
				}
				if (source->parent_index != UINT32_MAX &&
				    source->parent_index >= (u32)item_index) {
					archive_shell_set_error(
						error, error_capacity,
						"specimen preparation item nesting is invalid");
					return false;
				}
				const char* values[13] = {
					source->concept_name.value,
					source->concept_name.coding_scheme,
					source->concept_name.coding_scheme_version,
					source->concept_name.meaning,
					source->value_type == DICOM_CONTENT_VALUE_CODE ?
						source->code_value.value : source->scalar_value,
					source->code_value.coding_scheme,
					source->code_value.coding_scheme_version,
					source->code_value.meaning,
					source->measurement_units.value,
					source->measurement_units.coding_scheme,
					source->measurement_units.coding_scheme_version,
					source->measurement_units.meaning,
					source->relationship_type,
				};
				u32 offsets[13] = {0};
				bool success = true;
				for (i32 field = 0; field < COUNT(values) && success; ++field)
					success = archive_shell_strings_add(
						strings, values[field] ? values[field] : "",
						offsets + field);
				if (!success) return false;
				u8* record =
					section->data + ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
					(size_t)output_index *
						ARCHIVE_SHELL_PREPARATION_RECORD_SIZE;
				archive_shell_write_u32(record, (u32)specimen_index);
				archive_shell_write_u32(record + 4, (u32)step_index);
				archive_shell_write_u32(
					record + 8,
					source->parent_index == UINT32_MAX ?
						UINT32_MAX : step_base + source->parent_index);
				archive_shell_write_u32(
					record + 12, (u32)source->value_type);
				for (i32 field = 0; field < 13; ++field)
					archive_shell_write_u32(
						record + 16 + (size_t)field * 4,
						offsets[field]);
				++output_index;
			}
		}
	}
	return output_index == item_count;
}

typedef struct archive_shell_sparse_run_t {
	u32 first_frame;
	u32 tile_row;
	u32 first_tile_column;
	u32 tile_count;
} archive_shell_sparse_run_t;

static bool archive_shell_frame_offset_matches(
		float actual, float expected) {
	float scale = MAX(fabsf(actual), fabsf(expected));
	float tolerance = 0.0001f + scale * 0.000001f;
	return isfinite(actual) && isfinite(expected) &&
	       fabsf(actual - expected) <= tolerance;
}

static bool archive_shell_try_build_sparse_runs(
		const dicom_instance_t* instance, const archive_frame_map_t* map,
		archive_shell_section_t* section) {
	if (!instance || !map || !section || map->optical_path_count != 1 ||
	    map->focal_plane_count != 1 || !instance->rows || !instance->columns)
		return false;
	archive_shell_sparse_run_t* runs = calloc(
		map->frame_count, sizeof(*runs));
	if (!runs) return false;
	u32 run_count = 0;
	bool success = true;
	for (u32 frame_index = 0; frame_index < map->frame_count; ++frame_index) {
		const archive_frame_map_entry_t* frame =
			map->entries + frame_index;
		if (!frame->row_position || !frame->column_position ||
		    (frame->row_position - 1) % instance->rows != 0 ||
		    (frame->column_position - 1) % instance->columns != 0 ||
		    frame->optical_path_index != 0 ||
		    frame->focal_plane_index != 0) {
			success = false;
			break;
		}
		u32 tile_row = (frame->row_position - 1) / instance->rows;
		u32 tile_column =
			(frame->column_position - 1) / instance->columns;
		if (frame->flags & ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN) {
			if (!instance->is_total_pixel_matrix_origin_known ||
			    !instance->is_image_orientation_slide_known ||
			    instance->pixel_spacing.x <= 0.0f ||
			    instance->pixel_spacing.y <= 0.0f) {
				success = false;
				break;
			}
			float row_distance =
				(float)(frame->row_position - 1) *
				instance->pixel_spacing.x;
			float column_distance =
				(float)(frame->column_position - 1) *
				instance->pixel_spacing.y;
			float expected_x =
				instance->total_pixel_matrix_origin_x_mm +
				column_distance *
					instance->image_orientation_slide[0] +
				row_distance *
					instance->image_orientation_slide[3];
			float expected_y =
				instance->total_pixel_matrix_origin_y_mm +
				column_distance *
					instance->image_orientation_slide[1] +
				row_distance *
					instance->image_orientation_slide[4];
			if (!archive_shell_frame_offset_matches(
					frame->x_offset_mm, expected_x) ||
			    !archive_shell_frame_offset_matches(
					frame->y_offset_mm, expected_y)) {
				success = false;
				break;
			}
		}
		if ((frame->flags & ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN) &&
		    (!instance->is_total_pixel_matrix_origin_known ||
		     !archive_shell_frame_offset_matches(
			     frame->z_offset_um,
			     instance->total_pixel_matrix_origin_z_um))) {
			success = false;
			break;
		}
		archive_shell_sparse_run_t* previous =
			run_count ? runs + run_count - 1 : NULL;
		if (previous && previous->tile_row == tile_row &&
		    previous->first_tile_column + previous->tile_count ==
			    tile_column) {
			++previous->tile_count;
		} else {
			if (previous &&
			    (tile_row < previous->tile_row ||
			     (tile_row == previous->tile_row &&
			      tile_column <=
				      previous->first_tile_column +
					      previous->tile_count - 1))) {
				success = false;
				break;
			}
			runs[run_count++] = (archive_shell_sparse_run_t){
				.first_frame = frame_index,
				.tile_row = tile_row,
				.first_tile_column = tile_column,
				.tile_count = 1,
			};
		}
	}
	u64 required = ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
		(u64)run_count * ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
	if (!success || !run_count || required > SIZE_MAX) {
		free(runs);
		return false;
	}
	section->size = (size_t)required;
	section->data = calloc(1, section->size);
	if (!section->data) {
		free(runs);
		return false;
	}
	memcpy(section->data, archive_shell_sparse_run_magic,
	       sizeof(archive_shell_sparse_run_magic));
	archive_shell_write_u32(
		section->data + 8, ARCHIVE_SHELL_SPARSE_RUN_VERSION);
	archive_shell_write_u32(
		section->data + 12, ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE);
	archive_shell_write_u32(
		section->data + 16, ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE);
	archive_shell_write_u32(section->data + 20, run_count);
	archive_shell_write_u32(section->data + 24, map->frame_count);
	for (u32 i = 0; i < run_count; ++i) {
		u8* record =
			section->data + ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
			(size_t)i * ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
		archive_shell_write_u32(record, runs[i].first_frame);
		archive_shell_write_u32(record + 4, runs[i].tile_row);
		archive_shell_write_u32(
			record + 8, runs[i].first_tile_column);
		archive_shell_write_u32(record + 12, runs[i].tile_count);
	}
	free(runs);
	return true;
}

static bool archive_shell_build_frame_map(const dicom_instance_t* instance,
		archive_shell_section_t* section,
		archive_shell_frame_map_kind_t* kind,
		char* error, size_t error_capacity) {
	archive_frame_map_t map = {0};
	if (!archive_frame_map_build_part10(&map, instance, error, error_capacity)) return false;
	if (archive_shell_try_build_sparse_runs(instance, &map, section)) {
		*kind = ARCHIVE_SHELL_FRAME_MAP_SPARSE_RUNS;
		archive_frame_map_destroy(&map);
		return true;
	}
	size_t required = 0;
	archive_frame_map_serialize(&map, NULL, 0, &required, NULL, 0);
	section->data = malloc(required);
	bool success = section->data &&
		archive_frame_map_serialize(&map, section->data, required, &section->size,
		                            error, error_capacity);
	if (success) *kind = ARCHIVE_SHELL_FRAME_MAP_EXPLICIT;
	archive_frame_map_destroy(&map);
	if (!success) {
		free(section->data);
		section->data = NULL;
		section->size = 0;
	}
	return success;
}

static bool archive_shell_find_last_encapsulated_frame_size(
		const dicom_instance_t* instance, u64 file_offset, u64 source_size, u32* size) {
	if (!instance || !instance->file.blob || !size || file_offset >= source_size) return false;
	u64 cursor = file_offset;
	while (cursor <= source_size && source_size - cursor >= 8) {
		u8 header[8];
		if (source_file_read_at_offset(header, &instance->file, cursor, sizeof(header)) !=
		    sizeof(header)) return false;
		bool is_item = header[0] == 0xfe && header[1] == 0xff &&
		               header[2] == 0x00 && header[3] == 0xe0;
		bool is_sequence_delimiter = header[0] == 0xfe && header[1] == 0xff &&
		                             header[2] == 0xdd && header[3] == 0xe0;
		if (is_sequence_delimiter) {
			u64 length = cursor - file_offset;
			if (!length || length > UINT32_MAX) return false;
			*size = (u32)length;
			return true;
		}
		if (!is_item) return false;
		u32 item_size = archive_shell_read_u32(header + 4);
		if (item_size == DICOM_UNDEFINED_LENGTH ||
		    item_size > source_size - cursor - 8) return false;
		cursor += 8 + item_size;
	}
	return false;
}

static bool archive_shell_build_native_payload_map(
		const archive_shell_native_payload_t* payloads,
		u32 payload_count, archive_shell_section_t* section,
		archive_shell_payload_map_kind_t* kind) {
	if (!payloads || !payload_count || !section || !kind)
		return false;
	bool common_file = true;
	for (u32 frame = 1; frame < payload_count; ++frame)
		if (payloads[frame].file_index !=
		    payloads[0].file_index) {
			common_file = false;
			break;
		}
	u64 per_frame_size =
		ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE +
		ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE +
		(common_file ? 0 :
		 ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE);
	u64 required = ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
		(u64)payload_count * per_frame_size;
	if (required > SIZE_MAX) return false;
	section->size = (size_t)required;
	section->data = calloc(1, section->size);
	if (!section->data) return false;
	*kind = ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA;
	archive_shell_write_u32(section->data, *kind);
	archive_shell_write_u32(section->data + 4, payload_count);
	if (common_file)
		archive_shell_write_u32(
			section->data + 8,
			payloads[0].file_index);
	archive_shell_write_u32(
		section->data + 12,
		ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE);
	archive_shell_write_u32(
		section->data + 24,
		common_file ? 0 :
			ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE);
	archive_shell_write_u32(
		section->data + 28,
		ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE);
	u8* files = section->data + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
	u8* offsets = files + (common_file ? 0 :
		(size_t)payload_count *
			ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE);
	u8* sizes = offsets +
		(size_t)payload_count *
			ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE;
	for (u32 frame = 0; frame < payload_count; ++frame) {
		if (!payloads[frame].size) {
			free(section->data);
			memset(section, 0, sizeof(*section));
			return false;
		}
		if (!common_file)
			archive_shell_write_u32(
				files + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE,
				payloads[frame].file_index);
		archive_shell_write_u64(
			offsets + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE,
			payloads[frame].offset);
		archive_shell_write_u32(
			sizes + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE,
			payloads[frame].size);
	}
	return true;
}

static bool archive_shell_build_payload_map(dicom_instance_t* instance, u64 source_size, u64 source_offset,
		archive_shell_section_t* section, archive_shell_payload_map_kind_t* kind,
		char* error, size_t error_capacity) {
	if (!dicom_instance_index_pixel_data(instance) ||
	    instance->number_of_frames <= 0 || instance->number_of_frames > UINT32_MAX) {
		archive_shell_set_error(error, error_capacity, "could not index Part 10 frame payloads");
		return false;
	}
	u32 frame_count = (u32)instance->number_of_frames;
	if (!instance->is_pixel_data_encapsulated) {
		if (!instance->bits_allocated || instance->bits_allocated % 8 || !instance->samples_per_pixel) {
			archive_shell_set_error(error, error_capacity, "unsupported native Part 10 pixel packing");
			return false;
		}
		u64 frame_size = (u64)instance->rows * instance->columns *
			instance->samples_per_pixel * instance->bits_allocated / 8;
		u64 base = sizeof(dicom_header_t) + (u64)instance->pixel_data.data_offset;
		if (!frame_size || base > source_size ||
		    frame_size > source_size - base ||
		    (u64)frame_count > (source_size - base) / frame_size ||
		    (u64)frame_count > instance->pixel_data.length / frame_size) {
			archive_shell_set_error(error, error_capacity, "native Part 10 frame payload is out of bounds");
			return false;
		}
		section->size = ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
		section->data = calloc(1, section->size);
		if (!section->data) return false;
		*kind = ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE;
		archive_shell_write_u32(section->data, *kind);
		archive_shell_write_u32(section->data + 4, frame_count);
		archive_shell_write_u64(section->data + 16, source_offset + base);
		archive_shell_write_u64(section->data + 24, frame_size);
		return true;
	}
	if (!instance->pixel_data_offsets || !instance->pixel_data_sizes ||
	    instance->pixel_data_offset_count != frame_count) {
		archive_shell_set_error(error, error_capacity, "encapsulated Part 10 frame index is incomplete");
		return false;
	}
	u64* offsets = calloc(frame_count, sizeof(*offsets));
	u32* sizes = calloc(frame_count, sizeof(*sizes));
	if (!offsets || !sizes) {
		free(sizes);
		free(offsets);
		return false;
	}
	bool valid = true;
	for (u32 frame = 0; frame < frame_count; ++frame) {
		u64 offset = sizeof(dicom_header_t) + (u64)instance->pixel_data_start_offset +
		             instance->pixel_data_offsets[frame];
		u32 size = instance->pixel_data_sizes[frame];
		if (frame + 1 == frame_count) {
			u32 exact_size = 0;
			if (archive_shell_find_last_encapsulated_frame_size(
					instance, offset, source_size, &exact_size)) {
				size = exact_size;
			} else if (size == DICOM_UNDEFINED_LENGTH) {
				archive_shell_set_error(error, error_capacity,
				                        "encapsulated Part 10 frame %u has no bounded byte range",
				                        frame + 1);
				valid = false;
				break;
			}
		} else if (size == DICOM_UNDEFINED_LENGTH) {
			archive_shell_set_error(error, error_capacity,
			                        "encapsulated Part 10 frame %u has no bounded byte range",
			                        frame + 1);
			valid = false;
			break;
		}
		if (offset > source_size ||
		    size > source_size - offset) {
			archive_shell_set_error(error, error_capacity,
			                        "encapsulated Part 10 frame %u has no bounded byte range "
			                        "(offset=%llu, size=%u, source=%llu)",
			                        frame + 1, (unsigned long long)offset, size,
			                        (unsigned long long)source_size);
			valid = false;
			break;
		}
		offsets[frame] = source_offset + offset;
		sizes[frame] = size;
	}
	if (!valid) {
		free(sizes);
		free(offsets);
		return false;
	}

	/*
	 * Prefer one 64-bit base plus parallel 32-bit frame-start delta and gap
	 * arrays. If source order, overlap, or delta width prevents that exact
	 * representation, retain parallel absolute-offset and size arrays instead.
	 */
	bool use_deltas = true;
	for (u32 frame = 0; frame < frame_count; ++frame) {
		if (!sizes[frame]) {
			archive_shell_set_error(
				error, error_capacity,
				"encapsulated Part 10 frame %u has an empty byte range",
				frame + 1);
			free(sizes);
			free(offsets);
			return false;
		}
		if (frame + 1 == frame_count) continue;
		if (offsets[frame + 1] <= offsets[frame] ||
		    sizes[frame] > offsets[frame + 1] - offsets[frame]) {
			use_deltas = false;
			continue;
		}
		u64 delta = offsets[frame + 1] - offsets[frame];
		u64 correction = delta - sizes[frame];
		if (delta > UINT32_MAX || correction > UINT32_MAX)
			use_deltas = false;
	}
	u64 per_frame_size = use_deltas ?
		ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE +
			ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE :
		ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE +
			ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE;
	u64 required = ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
		(u64)frame_count * per_frame_size;
	if (required > SIZE_MAX) {
		free(sizes);
		free(offsets);
		return false;
	}
	section->size = (size_t)required;
	section->data = calloc(1, section->size);
	if (!section->data) {
		free(sizes);
		free(offsets);
		return false;
	}
	*kind = use_deltas ? ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS :
		ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA;
	archive_shell_write_u32(section->data, *kind);
	archive_shell_write_u32(section->data + 4, frame_count);
	archive_shell_write_u32(
		section->data + 8, ARCHIVE_SHELL_PAYLOAD_ENCAPSULATED);
	if (use_deltas) {
		archive_shell_write_u32(
			section->data + 12,
			ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE);
		archive_shell_write_u64(section->data + 16, offsets[0]);
		archive_shell_write_u32(
			section->data + 24, sizes[frame_count - 1]);
		archive_shell_write_u32(
			section->data + 28, ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
		u8* deltas = section->data + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
		u8* corrections = deltas +
			(size_t)frame_count *
				ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE;
		for (u32 frame = 0; frame < frame_count; ++frame) {
			u32 delta = frame ?
				(u32)(offsets[frame] - offsets[frame - 1]) : 0;
			u32 correction = frame + 1 < frame_count ?
				(u32)(offsets[frame + 1] - offsets[frame] -
				      sizes[frame]) : 0;
			archive_shell_write_u32(
				deltas + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE,
				delta);
			archive_shell_write_u32(
				corrections + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE,
				correction);
		}
	} else {
		archive_shell_write_u32(
			section->data + 12, ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE);
		archive_shell_write_u32(
			section->data + 28, ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE);
		u8* stored_offsets =
			section->data + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
		u8* stored_sizes = stored_offsets +
			(size_t)frame_count *
				ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE;
		for (u32 frame = 0; frame < frame_count; ++frame) {
			archive_shell_write_u64(
				stored_offsets + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE,
				offsets[frame]);
			archive_shell_write_u32(
				stored_sizes + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE,
				sizes[frame]);
		}
	}
	free(sizes);
	free(offsets);
	return true;
}

static bool archive_shell_section_kind_is_known(u32 kind) {
	return kind >= ARCHIVE_SHELL_SECTION_SOURCE_TABLE &&
	       kind <= ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE;
}

static const u8* archive_shell_section_entry(const archive_shell_view_t* shell, u32 index) {
	return shell && index < shell->section_count ?
		shell->section_directory + (size_t)index * ARCHIVE_SHELL_SECTION_ENTRY_SIZE : NULL;
}

static const u8* archive_shell_section_data(const archive_shell_view_t* shell,
		u32 index, u32 expected_kind, u32 expected_instance, u64* size) {
	const u8* entry = archive_shell_section_entry(shell, index);
	if (!entry || archive_shell_read_u32(entry) != expected_kind ||
	    archive_shell_read_u32(entry + 8) != expected_instance) return NULL;
	u64 offset = archive_shell_read_u64(entry + 16);
	u64 encoded_size = archive_shell_read_u64(entry + 24);
	if (offset > shell->size || encoded_size > shell->size - offset) return NULL;
	if (size) *size = encoded_size;
	return shell->data + offset;
}

static const u8* archive_shell_scoped_section_data(
		const archive_shell_view_t* shell, u32 index,
		u32 expected_kind, u32 instance_index, u64* size) {
	const u8* entry = archive_shell_section_entry(shell, index);
	if (!entry || archive_shell_read_u32(entry) != expected_kind)
		return NULL;
	u32 owner = archive_shell_read_u32(entry + 8);
	if (owner != instance_index &&
	    owner != ARCHIVE_SHELL_SHARED_SECTION)
		return NULL;
	u64 offset = archive_shell_read_u64(entry + 16);
	u64 encoded_size = archive_shell_read_u64(entry + 24);
	if (offset > shell->size ||
	    encoded_size > shell->size - offset)
		return NULL;
	if (size) *size = encoded_size;
	return shell->data + offset;
}

static const u8* archive_shell_find_section(const archive_shell_view_t* shell,
		u32 expected_kind, u32 expected_instance, u64* size) {
	if (!shell) return NULL;
	for (u32 i = 0; i < shell->section_count; ++i) {
		const u8* section = archive_shell_section_data(
			shell, i, expected_kind, expected_instance, size);
		if (section) return section;
	}
	return NULL;
}

static const u8* archive_shell_find_scoped_section(
		const archive_shell_view_t* shell, u32 expected_kind,
		u32 instance_index, u64* size) {
	const u8* section = archive_shell_find_section(
		shell, expected_kind, instance_index, size);
	return section ? section : archive_shell_find_section(
		shell, expected_kind, ARCHIVE_SHELL_SHARED_SECTION, size);
}

static u32 archive_shell_count_sections(
		const archive_shell_view_t* shell, u32 expected_kind,
		u32 expected_instance) {
	u32 count = 0;
	if (!shell) return 0;
	for (u32 i = 0; i < shell->section_count; ++i) {
		const u8* entry = archive_shell_section_entry(shell, i);
		if (archive_shell_read_u32(entry) == expected_kind &&
		    archive_shell_read_u32(entry + 8) == expected_instance)
			++count;
	}
	return count;
}

static const char* archive_shell_string(const archive_shell_view_t* shell, u32 offset) {
	if (!shell || offset >= shell->strings_size) return NULL;
	const char* value = (const char*)shell->strings + offset;
	if (!memchr(value, '\0', (size_t)(shell->strings_size - offset))) return NULL;
	return value;
}

bool archive_shell_build_part10(const archive_shell_build_part10_input_t* input,
		archive_shell_buffer_t* shell, char* error, size_t error_capacity) {
	if (shell) memset(shell, 0, sizeof(*shell));
	if (error && error_capacity) error[0] = '\0';
	u8 source_digest[32];
	u8 fingerprint[32];
	if (!input || !input->instance || !shell || !input->source_byte_size ||
	    !archive_shell_hex_digest(input->source_sha256, source_digest) ||
	    !archive_shell_hex_digest(input->representation_fingerprint, fingerprint)) {
		return false;
	}
	u64 member_size = input->member_size ? input->member_size : input->source_byte_size;
	if (input->member_offset > input->source_byte_size || member_size > input->source_byte_size - input->member_offset) {
		archive_shell_set_error(error, error_capacity, "Part 10 member placement is outside its source blob");
		return false;
	}
	dicom_instance_t* instance = input->instance;
	if (instance->number_of_frames <= 0 || instance->number_of_frames > UINT32_MAX ||
	    !instance->rows || !instance->columns ||
	    !instance->total_pixel_matrix_rows || !instance->total_pixel_matrix_columns) {
		archive_shell_set_error(error, error_capacity,
			"Part 10 instance has invalid shell dimensions (%lld frames, %ux%u tiles, %ux%u matrix)",
			(long long)instance->number_of_frames, instance->rows, instance->columns,
			instance->total_pixel_matrix_rows, instance->total_pixel_matrix_columns);
		return false;
	}
	if (!instance->general_study.StudyInstanceUID.value[0] ||
	    !instance->general_series.SeriesInstanceUID.value[0] ||
	    !instance->sop_instance_uid.value[0] || !instance->sop_class_uid.value[0] ||
	    !instance->transfer_syntax_uid.value[0]) {
		archive_shell_set_error(error, error_capacity, "Part 10 instance lacks required shell identity");
		return false;
	}
	/* The parser does not yet retain spacing between focal planes. Until it
	 * does, a full multi-plane grid cannot be projected faithfully. */
	if (instance->is_tiled_full && instance->total_pixel_matrix_focal_planes > 1) {
		archive_shell_set_error(error, error_capacity,
			"TILED_FULL multi-focal-plane publication requires retained focal spacing");
		return false;
	}
	if (!isfinite(instance->pixel_spacing.x) || !isfinite(instance->pixel_spacing.y) ||
	    (instance->is_total_pixel_matrix_origin_known &&
	     (!isfinite(instance->total_pixel_matrix_origin_x_mm) ||
	      !isfinite(instance->total_pixel_matrix_origin_y_mm) ||
	      !isfinite(instance->total_pixel_matrix_origin_z_um)))) {
		archive_shell_set_error(error, error_capacity, "non-finite slide geometry");
		return false;
	}
	if (instance->is_image_orientation_slide_known) for (i32 i = 0; i < 6; ++i) {
		if (!isfinite(instance->image_orientation_slide[i])) {
			archive_shell_set_error(error, error_capacity, "non-finite slide orientation");
			return false;
		}
	}
	for (i32 i = 0; i < arrlen(instance->optical_paths); ++i) {
		const dicom_optical_path_t* optical_path = instance->optical_paths + i;
		if (optical_path->icc_profile_size &&
		    !archive_shell_icc_profile_is_valid(
			    optical_path->icc_profile, optical_path->icc_profile_size)) {
			archive_shell_set_error(error, error_capacity,
				"Part 10 optical path %d contains an invalid ICC profile", i);
			return false;
		}
	}

	archive_shell_strings_t strings = {0};
	archive_shell_section_t sections[9] = {0};
	i32 section_count = 0;
	bool success = archive_shell_strings_initialize(&strings);
	char patient_birth_date[9] = {0};
	char study_date[9] = {0};
	if (instance->patient.PatientBirthDate.present)
		snprintf(patient_birth_date, sizeof(patient_birth_date), "%04u%02u%02u",
		         instance->patient.PatientBirthDate.year,
		         instance->patient.PatientBirthDate.month,
		         instance->patient.PatientBirthDate.day);
	if (instance->general_study.StudyDate.present)
		snprintf(study_date, sizeof(study_date), "%04u%02u%02u",
		         instance->general_study.StudyDate.year,
		         instance->general_study.StudyDate.month,
		         instance->general_study.StudyDate.day);
	u32 string_offsets[ARCHIVE_SHELL_STRING_COUNT] = {0};
	const char* string_values[ARCHIVE_SHELL_STRING_COUNT] = {
		instance->general_study.StudyInstanceUID.value,
		instance->general_series.SeriesInstanceUID.value,
		instance->sop_instance_uid.value,
		instance->sop_class_uid.value,
		instance->transfer_syntax_uid.value,
		instance->general_series.Modality.value,
		archive_shell_image_flavor(instance),
		archive_shell_photometric(instance),
		instance->concatenation_uid.value,
		instance->patient.PatientID.value,
		patient_birth_date,
		instance->patient.PatientSex.value,
		study_date,
		instance->general_study.AccessionNumber.value,
		instance->general_study.StudyID.value,
		instance->patient.PatientName.value,
	};
	for (i32 i = 0; i < COUNT(string_values) && success; ++i)
		success = archive_shell_strings_add(&strings, string_values[i], string_offsets + i);

	archive_shell_section_t* source_section = sections + section_count++;
	source_section->kind = ARCHIVE_SHELL_SECTION_SOURCE_TABLE;
	source_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
	source_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	source_section->size = ARCHIVE_SHELL_SOURCE_RECORD_SIZE;
	source_section->data = calloc(1, source_section->size);
	if (!source_section->data) success = false;
	if (success) {
		memcpy(source_section->data, source_digest, sizeof(source_digest));
		archive_shell_write_u64(source_section->data + 32, input->source_byte_size);
		archive_shell_write_u32(
			source_section->data + 40,
			input->provider_kind == ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE ?
				ARCHIVE_SHELL_SOURCE_ROLE_NATIVE_BUNDLE :
				ARCHIVE_SHELL_SOURCE_ROLE_PART10);
	}

	archive_shell_section_t* string_section = sections + section_count++;
	string_section->kind = ARCHIVE_SHELL_SECTION_STRING_TABLE;
	string_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
	string_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;

	archive_shell_section_t* instance_section = sections + section_count++;
	instance_section->kind = ARCHIVE_SHELL_SECTION_INSTANCE_TABLE;
	instance_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
	instance_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	instance_section->size = ARCHIVE_SHELL_INSTANCE_RECORD_SIZE;
	instance_section->data = calloc(1, instance_section->size);
	if (!instance_section->data) success = false;

	u32 attribute_section_index = (u32)section_count;
	archive_shell_section_t* attribute_section = sections + section_count++;
	attribute_section->kind = ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE;
	attribute_section->instance_index = 0;
	attribute_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	if (success && !archive_shell_build_attributes(instance, string_offsets, attribute_section))
		success = false;

	u32 optical_section_index = (u32)section_count;
	archive_shell_section_t* optical_section = sections + section_count++;
	optical_section->kind = ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE;
	optical_section->instance_index = 0;
	optical_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	if (success && !archive_shell_build_optical_paths(instance, &strings, optical_section))
		success = false;

	archive_shell_section_t* specimen_section = sections + section_count++;
	specimen_section->kind = ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE;
	specimen_section->instance_index = 0;
	specimen_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	if (success && !archive_shell_build_specimens(instance, &strings, specimen_section))
		success = false;

	archive_shell_section_t* preparation_section = sections + section_count++;
	preparation_section->kind = ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION;
	preparation_section->instance_index = 0;
	preparation_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	if (success && !archive_shell_build_preparation_items(
			instance, &strings, preparation_section,
			error, error_capacity)) success = false;

	u64 tiles_per_row = ((u64)instance->total_pixel_matrix_columns + instance->columns - 1) /
	                    instance->columns;
	u64 tiles_per_column = ((u64)instance->total_pixel_matrix_rows + instance->rows - 1) /
	                       instance->rows;
	u32 focal_plane_count = instance->total_pixel_matrix_focal_planes ?
		instance->total_pixel_matrix_focal_planes : 1;
	u32 optical_path_count = arrlen(instance->optical_paths) > 0 ?
		(u32)arrlen(instance->optical_paths) : 1;
	bool regular = optical_path_count == 1 && focal_plane_count == 1 &&
		tiles_per_row * tiles_per_column == (u64)instance->number_of_frames &&
		(instance->is_tiled_full || instance->number_of_frames == 1);
	archive_shell_frame_map_kind_t frame_map_kind =
		regular ? ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID :
			ARCHIVE_SHELL_FRAME_MAP_EXPLICIT;
	u32 frame_section_index = ARCHIVE_SHELL_NO_SECTION;
	if (!regular) {
		frame_section_index = (u32)section_count;
		archive_shell_section_t* frame_section = sections + section_count++;
		frame_section->kind = ARCHIVE_SHELL_SECTION_FRAME_MAP;
		frame_section->instance_index = 0;
		frame_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
		if (success && !archive_shell_build_frame_map(
				instance, frame_section, &frame_map_kind,
				error, error_capacity)) success = false;
	}

	u32 payload_section_index = (u32)section_count;
	archive_shell_section_t* payload_section = sections + section_count++;
	payload_section->kind = ARCHIVE_SHELL_SECTION_PAYLOAD_MAP;
	payload_section->instance_index = 0;
	payload_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	archive_shell_payload_map_kind_t payload_kind = 0;
	if (success && input->native_payloads) {
		if (input->native_payload_count !=
			    (u32)instance->number_of_frames ||
		    !archive_shell_build_native_payload_map(
			    input->native_payloads,
			    input->native_payload_count,
			    payload_section, &payload_kind))
			success = false;
	} else if (success && !archive_shell_build_payload_map(
			instance, member_size, input->member_offset, payload_section,
			&payload_kind, error, error_capacity)) {
		success = false;
	}

	if (success) {
		string_section->data = strings.data;
		string_section->size = strings.size;
		strings.data = NULL;
		u8* record = instance_section->data;
		archive_shell_write_u32(record, 0);
		archive_shell_write_u32(
			record + 4,
			input->provider_parameter_version ?
				input->provider_parameter_version : 1);
		archive_shell_write_u32(record + 8, payload_kind);
		archive_shell_write_u32(record + 12, frame_map_kind);
		archive_shell_write_u32(record + 16, (u32)instance->number_of_frames);
		archive_shell_write_u32(record + 20, instance->rows);
		archive_shell_write_u32(record + 24, instance->columns);
		archive_shell_write_u32(record + 28, instance->total_pixel_matrix_rows);
		archive_shell_write_u32(record + 32, instance->total_pixel_matrix_columns);
		archive_shell_write_u32(record + 36, (u32)tiles_per_row);
		archive_shell_write_u32(record + 40, (u32)tiles_per_column);
		archive_shell_write_u32(record + 44, optical_path_count);
		archive_shell_write_u32(record + 48, focal_plane_count);
		archive_shell_write_u16(record + 52, instance->samples_per_pixel);
		archive_shell_write_u16(record + 54, instance->planar_configuration);
		archive_shell_write_u16(record + 56, instance->bits_allocated);
		archive_shell_write_u16(record + 58, instance->bits_stored);
		archive_shell_write_u16(record + 60, instance->high_bit);
		archive_shell_write_u16(record + 62, instance->pixel_representation);
		archive_shell_write_i64(record + 64, instance->instance_number);
		archive_shell_write_u32(record + 72, instance->concatenation_frame_offset_number);
		archive_shell_write_u32(record + 76, instance->in_concatenation_number);
		for (i32 i = 0; i <= ARCHIVE_SHELL_STRING_CONCATENATION_UID; ++i)
			archive_shell_write_u32(record + 80 + i * 4, string_offsets[i]);
		u32 instance_flags = (instance->is_total_pixel_matrix_origin_known ?
			ARCHIVE_SHELL_INSTANCE_HAS_ORIGIN : 0) |
			(instance->is_image_orientation_slide_known ?
			ARCHIVE_SHELL_INSTANCE_HAS_ORIENTATION : 0) |
			(instance->has_valid_wsi_image_type ? ARCHIVE_SHELL_INSTANCE_HAS_IMAGE_TYPE : 0) |
			(instance->is_image_original ? ARCHIVE_SHELL_INSTANCE_IMAGE_ORIGINAL : 0) |
			(instance->is_image_resampled ? ARCHIVE_SHELL_INSTANCE_IMAGE_RESAMPLED : 0) |
			(instance->is_tiled_full ? ARCHIVE_SHELL_INSTANCE_TILED_FULL : 0);
		archive_shell_write_u32(record + 116, instance_flags);
		archive_shell_write_float(record + 120, instance->pixel_spacing.x);
		archive_shell_write_float(record + 124, instance->pixel_spacing.y);
		archive_shell_write_float(record + 128, instance->total_pixel_matrix_origin_x_mm);
		archive_shell_write_float(record + 132, instance->total_pixel_matrix_origin_y_mm);
		archive_shell_write_float(record + 136, instance->total_pixel_matrix_origin_z_um);
		for (i32 i = 0; i < 6; ++i)
			archive_shell_write_float(record + 140 + i * 4, instance->image_orientation_slide[i]);
		archive_shell_write_u32(record + 168, attribute_section_index);
		archive_shell_write_u32(record + 172, optical_section_index);
		archive_shell_write_u32(record + 176, frame_section_index);
		archive_shell_write_u32(record + 180, payload_section_index);
		archive_shell_write_u32(
			record + 184,
			input->provider_kind ? input->provider_kind :
				ARCHIVE_SHELL_PROVIDER_PART10);
		archive_shell_write_u32(record + 188,
			instance->is_pixel_data_encapsulated ?
				ARCHIVE_SHELL_PAYLOAD_ENCAPSULATED : 0);
	}

	size_t total_size = ARCHIVE_SHELL_HEADER_SIZE +
		(size_t)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE;
	total_size = archive_shell_align8(total_size);
	for (i32 i = 0; i < section_count && success; ++i) {
		total_size = archive_shell_align8(total_size);
		if (sections[i].size > SIZE_MAX - total_size) success = false;
		else total_size += sections[i].size;
	}
	if (success) {
		shell->data = calloc(1, total_size);
		shell->size = total_size;
		if (!shell->data) success = false;
	}
	if (success) {
		memcpy(shell->data, archive_shell_magic, sizeof(archive_shell_magic));
		archive_shell_write_u32(shell->data + 8, ARCHIVE_SHELL_FORMAT_VERSION);
		archive_shell_write_u32(shell->data + 12, ARCHIVE_SHELL_HEADER_SIZE);
		archive_shell_write_u64(shell->data + 16, total_size);
		archive_shell_write_u64(shell->data + 24, ARCHIVE_SHELL_HEADER_SIZE);
		archive_shell_write_u32(shell->data + 32, (u32)section_count);
		archive_shell_write_u32(shell->data + 36, 1);
		archive_shell_write_u32(shell->data + 40, 1);
		memcpy(shell->data + 48, fingerprint, sizeof(fingerprint));
		size_t offset = archive_shell_align8(ARCHIVE_SHELL_HEADER_SIZE +
			(size_t)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE);
		for (i32 i = 0; i < section_count; ++i) {
			offset = archive_shell_align8(offset);
			u8* entry = shell->data + ARCHIVE_SHELL_HEADER_SIZE +
			            (size_t)i * ARCHIVE_SHELL_SECTION_ENTRY_SIZE;
			archive_shell_write_u32(entry, sections[i].kind);
			archive_shell_write_u16(entry + 4, 1);
			archive_shell_write_u16(entry + 6, (u16)sections[i].flags);
			archive_shell_write_u32(entry + 8, sections[i].instance_index);
			archive_shell_write_u64(entry + 16, offset);
			archive_shell_write_u64(entry + 24, sections[i].size);
			archive_shell_write_u64(entry + 32, sections[i].size);
			memcpy(shell->data + offset, sections[i].data, sections[i].size);
			offset += sections[i].size;
		}
		archive_shell_view_t validation = {0};
		if (!archive_shell_open(&validation, shell->data, shell->size,
				error, error_capacity)) success = false;
	}
	for (i32 i = 0; i < section_count; ++i) free(sections[i].data);
	free(strings.data);
	if (!success) {
		archive_shell_buffer_destroy(shell);
		if (error && error_capacity && !error[0])
			archive_shell_set_error(error, error_capacity, "could not build virtual DICOM shell");
	}
	return success;
}

static bool archive_shell_adjust_string_offset(u8* data, size_t offset, u32 delta) {
	u32 value = archive_shell_read_u32(data + offset);
	if (!value) return true;
	if (value > UINT32_MAX - delta) return false;
	archive_shell_write_u32(data + offset, value + delta);
	return true;
}

static bool archive_shell_adjust_section_strings(
		archive_shell_section_t* section, u32 delta) {
	if (!delta) return true;
	switch (section->kind) {
		case ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE:
		case ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE: {
			if (section->size < 16) return false;
			u32 count = archive_shell_read_u32(section->data);
			u32 stride = archive_shell_read_u32(section->data + 4);
			if (stride != ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE ||
			    (u64)count * stride > section->size - 16) return false;
			for (u32 i = 0; i < count; ++i) {
				u8* record = section->data + 16 + (size_t)i * stride;
				if (archive_shell_read_u16(record + 6) ==
				    ARCHIVE_SHELL_ATTRIBUTE_STRING) {
					u64 value = archive_shell_read_u64(record + 8);
					if (value && (value > UINT32_MAX ||
					    (u32)value > UINT32_MAX - delta)) return false;
					if (value) archive_shell_write_u64(record + 8, value + delta);
				}
			}
			return true;
		}
		case ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE: {
			if (section->size < ARCHIVE_SHELL_OPTICAL_HEADER_SIZE) return false;
			u32 count = archive_shell_read_u32(section->data);
			u32 stride = archive_shell_read_u32(section->data + 4);
			if (stride != ARCHIVE_SHELL_OPTICAL_RECORD_SIZE ||
			    (u64)count * stride >
			    section->size - ARCHIVE_SHELL_OPTICAL_HEADER_SIZE) return false;
			static const size_t offsets[] = {
				0, 4, 32, 36, 40, 44, 48, 52, 56,
			};
			for (u32 i = 0; i < count; ++i) {
				u8* record = section->data + ARCHIVE_SHELL_OPTICAL_HEADER_SIZE +
				             (size_t)i * stride;
				for (i32 field = 0; field < COUNT(offsets); ++field) {
					if (!archive_shell_adjust_string_offset(
							record, offsets[field], delta)) return false;
				}
			}
			return true;
		}
		case ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE: {
			if (section->size < ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE ||
			    !archive_shell_adjust_string_offset(section->data, 8, delta))
				return false;
			u32 count = archive_shell_read_u32(section->data);
			u32 stride = archive_shell_read_u32(section->data + 4);
			if (stride != ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE ||
			    (u64)count * stride >
			    section->size - ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE) return false;
			for (u32 i = 0; i < count; ++i) {
				u8* record = section->data + ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE +
				             (size_t)i * stride;
				for (i32 field = 0; field < 3; ++field) {
					if (!archive_shell_adjust_string_offset(
							record, (size_t)field * 4, delta)) return false;
				}
			}
			return true;
		}
		case ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION: {
			if (section->size < ARCHIVE_SHELL_PREPARATION_HEADER_SIZE)
				return false;
			u32 count = archive_shell_read_u32(section->data);
			u32 stride = archive_shell_read_u32(section->data + 4);
			if (stride != ARCHIVE_SHELL_PREPARATION_RECORD_SIZE ||
			    (u64)count * stride >
			    section->size - ARCHIVE_SHELL_PREPARATION_HEADER_SIZE)
				return false;
			for (u32 i = 0; i < count; ++i) {
				u8* record =
					section->data + ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
					(size_t)i * stride;
				for (i32 field = 0; field < 13; ++field) {
					if (!archive_shell_adjust_string_offset(
							record, 16 + (size_t)field * 4, delta))
						return false;
				}
			}
			return true;
		}
		default: return true;
	}
}

static bool archive_shell_payload_kind_is_known(
		const archive_shell_section_t* section) {
	if (!section || section->kind != ARCHIVE_SHELL_SECTION_PAYLOAD_MAP ||
	    section->size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE) return false;
	u32 kind = archive_shell_read_u32(section->data);
	if (kind == ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE ||
	    kind == ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA ||
	    kind == ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS ||
	    kind == ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA)
		return true;
	return false;
}

static const u32 archive_shell_governed_attribute_tags[] = {
	0x00080020, /* Study Date */
	0x00080050, /* Accession Number */
	0x00080060, /* Modality */
	0x00100010, /* Patient Name */
	0x00100020, /* Patient ID */
	0x00100030, /* Patient Birth Date */
	0x00100040, /* Patient Sex */
	0x0020000D, /* Study Instance UID */
	0x0020000E, /* Series Instance UID */
	0x00200010, /* Study ID */
};

static bool archive_shell_attribute_is_governed(u32 tag) {
	for (i32 i = 0; i < COUNT(archive_shell_governed_attribute_tags); ++i) {
		if (archive_shell_governed_attribute_tags[i] == tag) return true;
	}
	return false;
}

static bool archive_shell_find_attribute_by_tag(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 tag,
		archive_shell_attribute_t* attribute) {
	u32 count = archive_shell_get_attribute_count(shell, instance);
	for (u32 i = 0; i < count; ++i) {
		archive_shell_attribute_t candidate = {0};
		if (!archive_shell_get_attribute(
				shell, instance, i, &candidate)) return false;
		if (candidate.tag == tag) {
			*attribute = candidate;
			return true;
		}
	}
	return false;
}

static bool archive_shell_attributes_are_equal(
		const archive_shell_attribute_t* a,
		const archive_shell_attribute_t* b) {
	if (!a || !b || a->tag != b->tag ||
	    strcmp(a->vr, b->vr) != 0 ||
	    a->value_kind != b->value_kind) return false;
	switch (a->value_kind) {
		case ARCHIVE_SHELL_ATTRIBUTE_STRING:
			return a->string_value && b->string_value &&
			       strcmp(a->string_value, b->string_value) == 0;
		case ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED:
			return a->unsigned_value == b->unsigned_value;
		case ARCHIVE_SHELL_ATTRIBUTE_SIGNED:
			return a->signed_value == b->signed_value;
		default:
			return false;
	}
}

static bool archive_shell_build_governed_attributes(
		const archive_shell_view_t* views, u32 instance_count,
		archive_shell_section_t* section,
		char* error, size_t error_capacity) {
	if (!views || !instance_count || !section) return false;
	u32 count = COUNT(archive_shell_governed_attribute_tags);
	section->kind = ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE;
	section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
	section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
	section->size = 16 + (size_t)count * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
	section->data = calloc(1, section->size);
	if (!section->data) return false;
	archive_shell_write_u32(section->data, count);
	archive_shell_write_u32(
		section->data + 4, ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE);
	archive_shell_instance_t first_instance = {0};
	if (!archive_shell_get_instance(views, 0, &first_instance)) return false;
	for (u32 tag_index = 0; tag_index < count; ++tag_index) {
		archive_shell_attribute_t first = {0};
		if (!archive_shell_find_attribute_by_tag(
				views, &first_instance,
				archive_shell_governed_attribute_tags[tag_index],
				&first)) return false;
		for (u32 instance_index = 1;
		     instance_index < instance_count; ++instance_index) {
			archive_shell_instance_t candidate_instance = {0};
			archive_shell_attribute_t candidate = {0};
			if (!archive_shell_get_instance(
					views + instance_index, 0,
					&candidate_instance) ||
			    !archive_shell_find_attribute_by_tag(
					views + instance_index, &candidate_instance,
					first.tag, &candidate) ||
			    !archive_shell_attributes_are_equal(
					&first, &candidate)) {
				archive_shell_set_error(
					error, error_capacity,
					"governed DICOM attribute %08X conflicts "
					"within the logical slide",
					first.tag);
				return false;
			}
		}
		u64 value = 0;
		if (first.value_kind == ARCHIVE_SHELL_ATTRIBUTE_STRING) {
			const u8* string = (const u8*)first.string_value;
			if (string < views[0].strings ||
			    (u64)(string - views[0].strings) >=
				    views[0].strings_size)
				return false;
			value = (u64)(string - views[0].strings);
		} else if (first.value_kind ==
		           ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED) {
			value = first.unsigned_value;
		} else if (first.value_kind ==
		           ARCHIVE_SHELL_ATTRIBUTE_SIGNED) {
			value = (u64)first.signed_value;
		} else {
			return false;
		}
		archive_shell_write_attribute(
			section->data + 16 +
				(size_t)tag_index * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE,
			first.tag, first.vr[0], first.vr[1],
			(u16)first.value_kind, value);
	}
	return true;
}

static bool archive_shell_remove_governed_attributes(
		archive_shell_section_t* section) {
	if (!section ||
	    section->kind != ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE ||
	    section->size < 16 ||
	    archive_shell_read_u32(section->data + 4) !=
		    ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE)
		return false;
	u32 count = archive_shell_read_u32(section->data);
	if (count > (section->size - 16) /
	            ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE) return false;
	u32 retained = 0;
	for (u32 i = 0; i < count; ++i) {
		u8* source = section->data + 16 +
		             (size_t)i * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
		if (archive_shell_attribute_is_governed(
				archive_shell_read_u32(source))) continue;
		u8* destination = section->data + 16 +
		                  (size_t)retained *
			                  ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
		if (destination != source)
			memmove(destination, source,
			        ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE);
		++retained;
	}
	archive_shell_write_u32(section->data, retained);
	section->size =
		16 + (size_t)retained * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
	return true;
}

static bool archive_shell_specimen_metadata_is_equal(
		const archive_shell_view_t* a, const archive_shell_view_t* b) {
	archive_shell_instance_t a_instance = {0};
	archive_shell_instance_t b_instance = {0};
	if (!archive_shell_get_instance(a, 0, &a_instance) ||
	    !archive_shell_get_instance(b, 0, &b_instance)) return false;
	const char* a_container =
		archive_shell_get_container_identifier(a, &a_instance);
	const char* b_container =
		archive_shell_get_container_identifier(b, &b_instance);
	if (!a_container || !b_container ||
	    strcmp(a_container, b_container) != 0) return false;
	u32 count = archive_shell_get_specimen_count(a, &a_instance);
	if (archive_shell_get_specimen_count(b, &b_instance) != count)
		return false;
	for (u32 i = 0; i < count; ++i) {
		archive_shell_specimen_t a_specimen = {0};
		archive_shell_specimen_t b_specimen = {0};
		if (!archive_shell_get_specimen(
				a, &a_instance, i, &a_specimen) ||
		    !archive_shell_get_specimen(
				b, &b_instance, i, &b_specimen) ||
		    strcmp(a_specimen.identifier, b_specimen.identifier) != 0 ||
		    strcmp(a_specimen.uid, b_specimen.uid) != 0 ||
		    strcmp(a_specimen.short_description,
		           b_specimen.short_description) != 0)
			return false;
	}
	return true;
}

static bool archive_shell_preparation_items_are_equal(
		const archive_shell_preparation_item_t* a,
		const archive_shell_preparation_item_t* b) {
	if (!a || !b || a->specimen_index != b->specimen_index ||
	    a->step_index != b->step_index ||
	    a->parent_item_index != b->parent_item_index ||
	    a->value_kind != b->value_kind) return false;
	const char* a_values[] = {
		a->concept_name_code_value,
		a->concept_name_coding_scheme,
		a->concept_name_coding_scheme_version,
		a->concept_name_code_meaning,
		a->code_value,
		a->code_coding_scheme,
		a->code_coding_scheme_version,
		a->code_meaning,
		a->scalar_value,
		a->measurement_units_code_value,
		a->measurement_units_coding_scheme,
		a->measurement_units_coding_scheme_version,
		a->measurement_units_code_meaning,
		a->relationship_type,
	};
	const char* b_values[] = {
		b->concept_name_code_value,
		b->concept_name_coding_scheme,
		b->concept_name_coding_scheme_version,
		b->concept_name_code_meaning,
		b->code_value,
		b->code_coding_scheme,
		b->code_coding_scheme_version,
		b->code_meaning,
		b->scalar_value,
		b->measurement_units_code_value,
		b->measurement_units_coding_scheme,
		b->measurement_units_coding_scheme_version,
		b->measurement_units_code_meaning,
		b->relationship_type,
	};
	for (i32 i = 0; i < COUNT(a_values); ++i) {
		if (strcmp(a_values[i], b_values[i]) != 0) return false;
	}
	return true;
}

static bool archive_shell_preparation_metadata_is_equal(
		const archive_shell_view_t* a, const archive_shell_view_t* b) {
	archive_shell_instance_t a_instance = {0};
	archive_shell_instance_t b_instance = {0};
	if (!archive_shell_get_instance(a, 0, &a_instance) ||
	    !archive_shell_get_instance(b, 0, &b_instance)) return false;
	u32 count =
		archive_shell_get_preparation_item_count(a, &a_instance);
	if (archive_shell_get_preparation_item_count(
			b, &b_instance) != count) return false;
	for (u32 i = 0; i < count; ++i) {
		archive_shell_preparation_item_t a_item = {0};
		archive_shell_preparation_item_t b_item = {0};
		if (!archive_shell_get_preparation_item(
				a, &a_instance, i, &a_item) ||
		    !archive_shell_get_preparation_item(
				b, &b_instance, i, &b_item) ||
		    !archive_shell_preparation_items_are_equal(
			    &a_item, &b_item))
			return false;
	}
	return true;
}

static bool archive_shell_optical_paths_are_equal(
		const archive_shell_view_t* a, const archive_shell_view_t* b) {
	archive_shell_instance_t a_instance = {0};
	archive_shell_instance_t b_instance = {0};
	if (!archive_shell_get_instance(a, 0, &a_instance) ||
	    !archive_shell_get_instance(b, 0, &b_instance) ||
	    a_instance.optical_path_count != b_instance.optical_path_count)
		return false;
	for (u32 i = 0; i < a_instance.optical_path_count; ++i) {
		archive_shell_optical_path_t ap = {0};
		archive_shell_optical_path_t bp = {0};
		if (!archive_shell_get_optical_path(a, &a_instance, i, &ap) ||
		    !archive_shell_get_optical_path(b, &b_instance, i, &bp))
			return false;
		const char* a_values[] = {
			ap.identifier, ap.description, ap.color_space,
			ap.illumination_type_code_value,
			ap.illumination_type_coding_scheme,
			ap.illumination_type_code_meaning,
			ap.illumination_color_code_value,
			ap.illumination_color_coding_scheme,
			ap.illumination_color_code_meaning,
		};
		const char* b_values[] = {
			bp.identifier, bp.description, bp.color_space,
			bp.illumination_type_code_value,
			bp.illumination_type_coding_scheme,
			bp.illumination_type_code_meaning,
			bp.illumination_color_code_value,
			bp.illumination_color_coding_scheme,
			bp.illumination_color_code_meaning,
		};
		for (i32 field = 0; field < COUNT(a_values); ++field)
			if (strcmp(a_values[field], b_values[field]) != 0)
				return false;
		if (ap.illumination_wavelength != bp.illumination_wavelength ||
		    ap.source_preserved_fields != bp.source_preserved_fields ||
		    ap.inferred_fields != bp.inferred_fields ||
		    ap.light_path_filter_pass_through_wavelength !=
			    bp.light_path_filter_pass_through_wavelength ||
		    memcmp(ap.light_path_filter_pass_band,
		           bp.light_path_filter_pass_band,
		           sizeof(ap.light_path_filter_pass_band)) != 0 ||
		    ap.image_path_filter_pass_through_wavelength !=
			    bp.image_path_filter_pass_through_wavelength ||
		    memcmp(ap.image_path_filter_pass_band,
		           bp.image_path_filter_pass_band,
		           sizeof(ap.image_path_filter_pass_band)) != 0 ||
		    ap.objective_lens_power != bp.objective_lens_power ||
		    ap.objective_lens_numerical_aperture !=
			    bp.objective_lens_numerical_aperture ||
		    ap.icc_profile_size != bp.icc_profile_size ||
		    (ap.icc_profile_size &&
		     memcmp(ap.icc_profile, bp.icc_profile,
		            (size_t)ap.icc_profile_size) != 0))
			return false;
	}
	return true;
}

bool archive_shell_build_part10_aggregate(
		const archive_shell_build_part10_aggregate_input_t* input,
		archive_shell_buffer_t* shell, char* error, size_t error_capacity) {
	if (shell) memset(shell, 0, sizeof(*shell));
	if (error && error_capacity) error[0] = '\0';
	u8 fingerprint[32];
	if (!input || !input->instances || !input->instance_count ||
	    input->instance_count > ARCHIVE_SHELL_MAX_INSTANCES || !shell ||
	    !archive_shell_hex_digest(input->representation_fingerprint, fingerprint)) {
		return false;
	}
	archive_shell_buffer_t* parts =
		calloc(input->instance_count, sizeof(*parts));
	archive_shell_view_t* views =
		calloc(input->instance_count, sizeof(*views));
	archive_shell_section_t* sections = NULL;
	bool success = parts && views;
	u64 section_count64 = 4;
	u64 strings_size64 = 1;
	for (u32 i = 0; i < input->instance_count && success; ++i) {
		archive_shell_build_part10_input_t part_input = input->instances[i];
		part_input.representation_fingerprint = input->representation_fingerprint;
		success = archive_shell_build_part10(
			          &part_input, parts + i, error, error_capacity) &&
		          archive_shell_open(views + i, parts[i].data, parts[i].size,
		                             error, error_capacity) &&
		          views[i].instance_count == 1 && views[i].source_count == 1 &&
		          views[i].strings_size >= 1;
		if (success) {
			section_count64 += views[i].section_count - 3;
			strings_size64 += views[i].strings_size - 1;
			success = section_count64 <= UINT32_MAX &&
			          strings_size64 <= UINT32_MAX &&
			          strings_size64 <= SIZE_MAX;
		}
	}
	bool share_optical_paths = success;
	for (u32 i = 1;
	     i < input->instance_count && share_optical_paths; ++i)
		share_optical_paths =
			archive_shell_optical_paths_are_equal(
				views, views + i);
	for (u32 i = 1; i < input->instance_count && success; ++i) {
		if (!archive_shell_specimen_metadata_is_equal(
				views, views + i)) {
			archive_shell_set_error(
				error, error_capacity,
				"governed container or specimen metadata conflicts "
				"within the logical slide");
			success = false;
		} else if (!archive_shell_preparation_metadata_is_equal(
				views, views + i)) {
			archive_shell_set_error(
				error, error_capacity,
				"governed specimen-preparation metadata conflicts "
				"within the logical slide");
			success = false;
		}
	}
	if (success) {
		u64 duplicate_sections =
			(u64)(input->instance_count - 1) * 2;
		if (share_optical_paths)
			duplicate_sections += input->instance_count - 1;
		success = section_count64 >= duplicate_sections;
		if (success) section_count64 -= duplicate_sections;
	}
	u32 section_count = success ? (u32)section_count64 : 0;
	if (success) {
		sections = calloc(section_count, sizeof(*sections));
		success = sections != NULL;
	}
	u32 output_section = 0;
	archive_shell_section_t* source_section = NULL;
	archive_shell_section_t* string_section = NULL;
	archive_shell_section_t* instance_section = NULL;
	archive_shell_section_t* governed_section = NULL;
	if (success) {
		source_section = sections + output_section++;
		source_section->kind = ARCHIVE_SHELL_SECTION_SOURCE_TABLE;
		source_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
		source_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
		source_section->size =
			(size_t)input->instance_count * ARCHIVE_SHELL_SOURCE_RECORD_SIZE;
		source_section->data = calloc(1, source_section->size);

		string_section = sections + output_section++;
		string_section->kind = ARCHIVE_SHELL_SECTION_STRING_TABLE;
		string_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
		string_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
		string_section->size = (size_t)strings_size64;
		string_section->data = calloc(1, string_section->size);

		instance_section = sections + output_section++;
		instance_section->kind = ARCHIVE_SHELL_SECTION_INSTANCE_TABLE;
		instance_section->instance_index = ARCHIVE_SHELL_SHARED_SECTION;
		instance_section->flags = ARCHIVE_SHELL_SECTION_CRITICAL;
		instance_section->size =
			(size_t)input->instance_count * ARCHIVE_SHELL_INSTANCE_RECORD_SIZE;
		instance_section->data = calloc(1, instance_section->size);
		success = source_section->data && string_section->data &&
		          instance_section->data;
		if (success) {
			governed_section = sections + output_section++;
			success = archive_shell_build_governed_attributes(
				views, input->instance_count, governed_section,
				error, error_capacity);
		}
	}
	u32 string_cursor = 1;
	u32 shared_optical_section_index = ARCHIVE_SHELL_NO_SECTION;
	for (u32 i = 0; i < input->instance_count && success; ++i) {
		const archive_shell_view_t* view = views + i;
		archive_shell_source_t source = {0};
		archive_shell_instance_t part_instance = {0};
		success = archive_shell_get_source(view, 0, &source) &&
		          archive_shell_get_instance(view, 0, &part_instance);
		if (!success) break;
		u8* source_record = source_section->data +
		                    (size_t)i * ARCHIVE_SHELL_SOURCE_RECORD_SIZE;
		memcpy(source_record, source.sha256, sizeof(source.sha256));
		archive_shell_write_u64(source_record + 32, source.byte_size);
		archive_shell_write_u32(source_record + 40, source.role);
		archive_shell_write_u32(source_record + 44, source.flags);

		u32 string_delta = string_cursor - 1;
		memcpy(string_section->data + string_cursor, view->strings + 1,
		       (size_t)view->strings_size - 1);
		string_cursor += (u32)view->strings_size - 1;

		u8* instance_record = instance_section->data +
		                      (size_t)i * ARCHIVE_SHELL_INSTANCE_RECORD_SIZE;
		memcpy(instance_record, view->instances, ARCHIVE_SHELL_INSTANCE_RECORD_SIZE);
		archive_shell_write_u32(instance_record, i);
		for (i32 field = 0; field < 9; ++field) {
			if (!archive_shell_adjust_string_offset(
					instance_record, 80 + (size_t)field * 4, string_delta)) {
				success = false;
				break;
			}
		}
		if (!success) break;
		archive_shell_write_u32(instance_record + 168, ARCHIVE_SHELL_NO_SECTION);
		archive_shell_write_u32(instance_record + 172, ARCHIVE_SHELL_NO_SECTION);
		archive_shell_write_u32(instance_record + 176, ARCHIVE_SHELL_NO_SECTION);
		archive_shell_write_u32(instance_record + 180, ARCHIVE_SHELL_NO_SECTION);
		if (i > 0 && share_optical_paths)
			archive_shell_write_u32(
				instance_record + 172,
				shared_optical_section_index);

		for (u32 source_index = 0;
		     source_index < view->section_count && success; ++source_index) {
			const u8* entry = archive_shell_section_entry(view, source_index);
			u32 kind = archive_shell_read_u32(entry);
			u32 owner = archive_shell_read_u32(entry + 8);
			if (owner == ARCHIVE_SHELL_SHARED_SECTION) continue;
			if (i > 0 &&
			    (kind == ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE ||
			     kind == ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION ||
			     (share_optical_paths &&
			      kind == ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE)))
				continue;
			if (output_section >= section_count) {
				success = false;
				break;
			}
			archive_shell_section_t* destination = sections + output_section;
			destination->kind = kind;
			destination->instance_index =
				(kind == ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE ||
				 kind == ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION ||
				 (share_optical_paths &&
				  kind == ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE)) ?
				ARCHIVE_SHELL_SHARED_SECTION : i;
			destination->flags = archive_shell_read_u16(entry + 6);
			u64 source_size = archive_shell_read_u64(entry + 24);
			u64 source_offset = archive_shell_read_u64(entry + 16);
			if (source_size > SIZE_MAX ||
			    source_offset > view->size ||
			    source_size > view->size - source_offset) {
				success = false;
				break;
			}
			destination->size = (size_t)source_size;
			destination->data = malloc(destination->size);
			if (!destination->data) {
				success = false;
				break;
			}
			memcpy(destination->data, view->data + source_offset,
			       destination->size);
			success = archive_shell_adjust_section_strings(
				destination, string_delta);
			if (success &&
			    kind == ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE)
				success = archive_shell_remove_governed_attributes(
					destination);
			if (success &&
			    kind == ARCHIVE_SHELL_SECTION_PAYLOAD_MAP) {
				success =
					archive_shell_payload_kind_is_known(destination);
			}
			if (!success) break;
			switch (kind) {
				case ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE:
					archive_shell_write_u32(instance_record + 168, output_section);
					break;
				case ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE:
					archive_shell_write_u32(instance_record + 172, output_section);
					if (share_optical_paths)
						shared_optical_section_index =
							output_section;
					break;
				case ARCHIVE_SHELL_SECTION_FRAME_MAP:
					archive_shell_write_u32(instance_record + 176, output_section);
					break;
				case ARCHIVE_SHELL_SECTION_PAYLOAD_MAP:
					archive_shell_write_u32(instance_record + 180, output_section);
					break;
				default: break;
			}
			++output_section;
		}
	}
	if (success)
		success = output_section == section_count &&
		          string_cursor == string_section->size;

	size_t total_size = ARCHIVE_SHELL_HEADER_SIZE +
		(size_t)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE;
	total_size = archive_shell_align8(total_size);
	for (u32 i = 0; i < section_count && success; ++i) {
		total_size = archive_shell_align8(total_size);
		if (sections[i].size > SIZE_MAX - total_size) success = false;
		else total_size += sections[i].size;
	}
	if (success) {
		shell->data = calloc(1, total_size);
		shell->size = total_size;
		success = shell->data != NULL;
	}
	if (success) {
		memcpy(shell->data, archive_shell_magic, sizeof(archive_shell_magic));
		archive_shell_write_u32(shell->data + 8, ARCHIVE_SHELL_FORMAT_VERSION);
		archive_shell_write_u32(shell->data + 12, ARCHIVE_SHELL_HEADER_SIZE);
		archive_shell_write_u64(shell->data + 16, total_size);
		archive_shell_write_u64(shell->data + 24, ARCHIVE_SHELL_HEADER_SIZE);
		archive_shell_write_u32(shell->data + 32, section_count);
		archive_shell_write_u32(shell->data + 36, input->instance_count);
		archive_shell_write_u32(shell->data + 40, input->instance_count);
		memcpy(shell->data + 48, fingerprint, sizeof(fingerprint));
		size_t offset = archive_shell_align8(
			ARCHIVE_SHELL_HEADER_SIZE +
			(size_t)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE);
		for (u32 i = 0; i < section_count; ++i) {
			offset = archive_shell_align8(offset);
			u8* entry = shell->data + ARCHIVE_SHELL_HEADER_SIZE +
			            (size_t)i * ARCHIVE_SHELL_SECTION_ENTRY_SIZE;
			archive_shell_write_u32(entry, sections[i].kind);
			archive_shell_write_u16(entry + 4, 1);
			archive_shell_write_u16(entry + 6, (u16)sections[i].flags);
			archive_shell_write_u32(entry + 8, sections[i].instance_index);
			archive_shell_write_u64(entry + 16, offset);
			archive_shell_write_u64(entry + 24, sections[i].size);
			archive_shell_write_u64(entry + 32, sections[i].size);
			memcpy(shell->data + offset, sections[i].data, sections[i].size);
			offset += sections[i].size;
		}
		archive_shell_view_t validation = {0};
		success = archive_shell_open(
			&validation, shell->data, shell->size, error, error_capacity);
	}
	if (sections) {
		for (u32 i = 0; i < section_count; ++i) free(sections[i].data);
	}
	free(sections);
	if (parts) {
		for (u32 i = 0; i < input->instance_count; ++i)
			archive_shell_buffer_destroy(parts + i);
	}
	free(parts);
	free(views);
	if (!success) {
		archive_shell_buffer_destroy(shell);
		if (error && error_capacity && !error[0])
			archive_shell_set_error(
				error, error_capacity,
				"could not build aggregate virtual DICOM shell");
	}
	return success;
}

bool archive_shell_open(archive_shell_view_t* shell, const void* data, size_t size,
		char* error, size_t error_capacity) {
	if (shell) memset(shell, 0, sizeof(*shell));
	if (!shell || !data || size < ARCHIVE_SHELL_HEADER_SIZE) return false;
	const u8* bytes = data;
	u64 encoded_size = archive_shell_read_u64(bytes + 16);
	u64 directory_offset = archive_shell_read_u64(bytes + 24);
	u32 section_count = archive_shell_read_u32(bytes + 32);
	u32 instance_count = archive_shell_read_u32(bytes + 36);
	u32 source_count = archive_shell_read_u32(bytes + 40);
	if (memcmp(bytes, archive_shell_magic, sizeof(archive_shell_magic)) != 0 ||
	    (archive_shell_read_u32(bytes + 8) != ARCHIVE_SHELL_FORMAT_VERSION &&
	     archive_shell_read_u32(bytes + 8) != 2) ||
	    archive_shell_read_u32(bytes + 12) != ARCHIVE_SHELL_HEADER_SIZE ||
	    encoded_size != size || directory_offset != ARCHIVE_SHELL_HEADER_SIZE ||
	    !section_count || !instance_count || instance_count > ARCHIVE_SHELL_MAX_INSTANCES ||
	    !source_count || (u64)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE >
	    size - directory_offset) {
		archive_shell_set_error(error, error_capacity, "virtual DICOM shell header is invalid");
		return false;
	}
	shell->data = bytes;
	shell->size = size;
	shell->section_directory = bytes + directory_offset;
	shell->section_count = section_count;
	shell->source_count = source_count;
	shell->instance_count = instance_count;
	memcpy(shell->representation_fingerprint, bytes + 48,
	       sizeof(shell->representation_fingerprint));
	u64 previous_end = archive_shell_align8((size_t)directory_offset +
		(size_t)section_count * ARCHIVE_SHELL_SECTION_ENTRY_SIZE);
	u32 source_sections = 0;
	u32 string_sections = 0;
	u32 instance_sections = 0;
	u32 governed_attribute_sections = 0;
	for (u32 i = 0; i < section_count; ++i) {
		const u8* entry = archive_shell_section_entry(shell, i);
		u32 kind = archive_shell_read_u32(entry);
		u16 version = archive_shell_read_u16(entry + 4);
		u16 flags = archive_shell_read_u16(entry + 6);
		u32 owner = archive_shell_read_u32(entry + 8);
		u64 offset = archive_shell_read_u64(entry + 16);
		u64 section_size = archive_shell_read_u64(entry + 24);
		u64 decoded_size = archive_shell_read_u64(entry + 32);
		if (offset % 8 || offset < previous_end || offset > size ||
		    section_size > size - offset ||
		    (owner != ARCHIVE_SHELL_SHARED_SECTION && owner >= instance_count)) {
			archive_shell_set_error(error, error_capacity, "virtual DICOM shell section is invalid");
			return false;
		}
		previous_end = offset + section_size;
		if (!archive_shell_section_kind_is_known(kind)) {
			if (flags & ARCHIVE_SHELL_SECTION_CRITICAL) {
				archive_shell_set_error(error, error_capacity,
				                        "virtual DICOM shell has an unknown critical section");
				return false;
			}
			continue;
		}
		if (version != 1 || decoded_size != section_size) {
			archive_shell_set_error(error, error_capacity,
			                        "virtual DICOM shell section version or encoding is unsupported");
			return false;
		}
		if (kind == ARCHIVE_SHELL_SECTION_SOURCE_TABLE) {
			++source_sections;
			if (owner != ARCHIVE_SHELL_SHARED_SECTION ||
			    section_size != (u64)source_count * ARCHIVE_SHELL_SOURCE_RECORD_SIZE) return false;
			shell->sources = bytes + offset;
		} else if (kind == ARCHIVE_SHELL_SECTION_STRING_TABLE) {
			++string_sections;
			if (owner != ARCHIVE_SHELL_SHARED_SECTION || !section_size ||
			    bytes[offset] != '\0') return false;
			shell->strings = bytes + offset;
			shell->strings_size = section_size;
		} else if (kind == ARCHIVE_SHELL_SECTION_INSTANCE_TABLE) {
			++instance_sections;
			if (owner != ARCHIVE_SHELL_SHARED_SECTION ||
			    section_size != (u64)instance_count * ARCHIVE_SHELL_INSTANCE_RECORD_SIZE) return false;
			shell->instances = bytes + offset;
		} else if (kind ==
		           ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE) {
			++governed_attribute_sections;
			if (owner != ARCHIVE_SHELL_SHARED_SECTION ||
			    archive_shell_attribute_table_count(
				    bytes + offset, section_size) == 0)
				return false;
		}
	}
	if (source_sections != 1 || string_sections != 1 || instance_sections != 1) {
		archive_shell_set_error(error, error_capacity, "virtual DICOM shell lacks a required shared section");
		return false;
	}
	if (governed_attribute_sections > 1) return false;
	for (u32 i = 0; i < instance_count; ++i) {
		archive_shell_instance_t instance;
		if (!archive_shell_get_instance(shell, i, &instance)) {
			archive_shell_set_error(error, error_capacity, "virtual DICOM shell instance is invalid");
			memset(shell, 0, sizeof(*shell));
			return false;
		}
	}
	return true;
}

bool archive_shell_get_source(const archive_shell_view_t* shell, u32 source_index,
		archive_shell_source_t* source) {
	if (!shell || !source || source_index >= shell->source_count || !shell->sources) return false;
	const u8* record = shell->sources + (size_t)source_index * ARCHIVE_SHELL_SOURCE_RECORD_SIZE;
	memset(source, 0, sizeof(*source));
	memcpy(source->sha256, record, 32);
	source->byte_size = archive_shell_read_u64(record + 32);
	source->role = (archive_shell_source_role_t)archive_shell_read_u32(record + 40);
	source->flags = archive_shell_read_u32(record + 44);
	return source->byte_size > 0;
}

static bool archive_shell_delta_payload_is_valid(
		const u8* payload, u64 payload_size, u32 frame_count) {
	if (!payload || payload_size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    !frame_count ||
	    archive_shell_read_u32(payload + 12) !=
		    ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE ||
	    archive_shell_read_u32(payload + 28) !=
		    ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE)
		return false;
	u64 delta_size =
		(u64)frame_count * ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE;
	u64 correction_size =
		(u64)frame_count * ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE;
	if (delta_size > UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    correction_size > UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE -
		                      delta_size ||
	    payload_size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
		                    delta_size + correction_size ||
	    archive_shell_read_u32(payload + 24) == 0)
		return false;
	const u8* deltas = payload + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
	const u8* corrections = deltas + delta_size;
	if (archive_shell_read_u32(deltas) != 0) return false;
	u64 cumulative = 0;
	for (u32 frame = 1; frame < frame_count; ++frame) {
		u32 delta = archive_shell_read_u32(
			deltas + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
		if (!delta || cumulative > UINT64_MAX - delta) return false;
		cumulative += delta;
	}
	for (u32 frame = 0; frame + 1 < frame_count; ++frame) {
		u32 correction = archive_shell_read_u32(
			corrections + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE);
		u32 next_delta = archive_shell_read_u32(
			deltas + (size_t)(frame + 1) *
				ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
		if (correction >= next_delta) return false;
	}
	if (archive_shell_read_u32(
			corrections + (size_t)(frame_count - 1) *
				ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE) != 0)
		return false;
	u64 base = archive_shell_read_u64(payload + 16);
	return base <= UINT64_MAX - cumulative &&
	       base + cumulative <=
		       UINT64_MAX - archive_shell_read_u32(payload + 24);
}

static bool archive_shell_explicit_payload_is_valid(
		const u8* payload, u64 payload_size, u32 frame_count) {
	if (!payload || payload_size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    !frame_count ||
	    archive_shell_read_u32(payload + 12) !=
		    ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE ||
	    archive_shell_read_u32(payload + 28) !=
		    ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE)
		return false;
	u64 offset_size =
		(u64)frame_count * ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE;
	u64 sizes_size =
		(u64)frame_count * ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE;
	if (offset_size > UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    sizes_size > UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE -
		                 offset_size ||
	    payload_size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
		                    offset_size + sizes_size)
		return false;
	const u8* offsets = payload + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
	const u8* sizes = offsets + offset_size;
	for (u32 frame = 0; frame < frame_count; ++frame) {
		u64 offset = archive_shell_read_u64(
			offsets + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE);
		u32 size = archive_shell_read_u32(
			sizes + (size_t)frame *
				ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE);
		if (!size || offset > UINT64_MAX - size) return false;
	}
	return true;
}

static bool archive_shell_native_payload_layout_is_valid(
		const u8* payload, u64 payload_size, u32 frame_count) {
	if (!payload || payload_size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    !frame_count ||
	    archive_shell_read_u32(payload + 12) !=
		    ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE ||
	    archive_shell_read_u32(payload + 28) !=
		    ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE)
		return false;
	u32 kind = archive_shell_read_u32(payload);
	if (kind != ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA)
		return false;
	u32 file_record_size = archive_shell_read_u32(payload + 24);
	if (file_record_size != 0 &&
	    file_record_size != ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE)
		return false;
	u64 records_size =
		(u64)frame_count *
		(ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE +
		 ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE +
		 file_record_size);
	if (records_size >
		    UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    payload_size !=
		    ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE + records_size)
		return false;
	return true;
}

static bool archive_shell_native_payload_is_valid(
		const u8* payload, u64 payload_size, u32 frame_count) {
	if (!archive_shell_native_payload_layout_is_valid(payload, payload_size, frame_count))
		return false;
	u32 file_record_size = archive_shell_read_u32(payload + 24);
	const u8* sizes =
		payload + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
		(u64)frame_count *
			(ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE +
			 file_record_size);
	for (u32 frame = 0; frame < frame_count; ++frame)
		if (!archive_shell_read_u32(
				sizes + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE))
			return false;
	return true;
}

static bool archive_shell_sparse_runs_are_valid(
		const u8* data, u64 size,
		const archive_shell_instance_t* instance) {
	if (!data || !instance ||
	    size < ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE ||
	    memcmp(data, archive_shell_sparse_run_magic,
	           sizeof(archive_shell_sparse_run_magic)) != 0 ||
	    archive_shell_read_u32(data + 8) !=
		    ARCHIVE_SHELL_SPARSE_RUN_VERSION ||
	    archive_shell_read_u32(data + 12) !=
		    ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE ||
	    archive_shell_read_u32(data + 16) !=
		    ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE ||
	    archive_shell_read_u32(data + 24) != instance->frame_count)
		return false;
	u32 run_count = archive_shell_read_u32(data + 20);
	u64 required = ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
		(u64)run_count * ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
	if (!run_count || required != size ||
	    !instance->tiles_per_row || !instance->tiles_per_column)
		return false;
	u32 expected_frame = 0;
	u32 previous_row = 0;
	u32 previous_end_column = 0;
	for (u32 i = 0; i < run_count; ++i) {
		const u8* record =
			data + ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
			(size_t)i * ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
		u32 first_frame = archive_shell_read_u32(record);
		u32 tile_row = archive_shell_read_u32(record + 4);
		u32 first_column = archive_shell_read_u32(record + 8);
		u32 tile_count = archive_shell_read_u32(record + 12);
		if (first_frame != expected_frame || !tile_count ||
		    tile_row >= instance->tiles_per_column ||
		    first_column >= instance->tiles_per_row ||
		    tile_count > instance->tiles_per_row - first_column ||
		    tile_count > instance->frame_count ||
		    expected_frame > instance->frame_count - tile_count)
			return false;
		if (i && (tile_row < previous_row ||
		          (tile_row == previous_row &&
		           first_column <= previous_end_column)))
			return false;
		expected_frame += tile_count;
		previous_row = tile_row;
		previous_end_column = first_column + tile_count;
	}
	return expected_frame == instance->frame_count;
}

bool archive_shell_get_instance(const archive_shell_view_t* shell, u32 instance_index,
		archive_shell_instance_t* instance) {
	if (!shell || !instance || instance_index >= shell->instance_count || !shell->instances) return false;
	const u8* record = shell->instances +
		(size_t)instance_index * ARCHIVE_SHELL_INSTANCE_RECORD_SIZE;
	memset(instance, 0, sizeof(*instance));
	instance->instance_index = instance_index;
	instance->source_index = archive_shell_read_u32(record);
	instance->provider_kind =
		(archive_shell_provider_kind_t)archive_shell_read_u32(record + 184);
	instance->provider_parameter_version = archive_shell_read_u32(record + 4);
	instance->payload_map_kind = (archive_shell_payload_map_kind_t)archive_shell_read_u32(record + 8);
	instance->frame_map_kind = (archive_shell_frame_map_kind_t)archive_shell_read_u32(record + 12);
	instance->frame_count = archive_shell_read_u32(record + 16);
	instance->rows = archive_shell_read_u32(record + 20);
	instance->columns = archive_shell_read_u32(record + 24);
	instance->total_pixel_matrix_rows = archive_shell_read_u32(record + 28);
	instance->total_pixel_matrix_columns = archive_shell_read_u32(record + 32);
	instance->tiles_per_row = archive_shell_read_u32(record + 36);
	instance->tiles_per_column = archive_shell_read_u32(record + 40);
	instance->optical_path_count = archive_shell_read_u32(record + 44);
	instance->focal_plane_count = archive_shell_read_u32(record + 48);
	instance->samples_per_pixel = archive_shell_read_u16(record + 52);
	instance->planar_configuration = archive_shell_read_u16(record + 54);
	instance->bits_allocated = archive_shell_read_u16(record + 56);
	instance->bits_stored = archive_shell_read_u16(record + 58);
	instance->high_bit = archive_shell_read_u16(record + 60);
	instance->pixel_representation = archive_shell_read_u16(record + 62);
	instance->instance_number = archive_shell_read_i64(record + 64);
	instance->concatenation_frame_offset = archive_shell_read_u32(record + 72);
	instance->in_concatenation_number = archive_shell_read_u32(record + 76);
	const char** values[] = {
		&instance->study_instance_uid, &instance->series_instance_uid,
		&instance->sop_instance_uid, &instance->sop_class_uid,
		&instance->transfer_syntax_uid, &instance->modality,
		&instance->image_flavor, &instance->photometric_interpretation,
		&instance->concatenation_uid,
	};
	for (i32 i = 0; i < COUNT(values); ++i) {
		*values[i] = archive_shell_string(shell, archive_shell_read_u32(record + 80 + i * 4));
		if (!*values[i]) return false;
	}
	u32 flags = archive_shell_read_u32(record + 116);
	instance->has_image_type = (flags & ARCHIVE_SHELL_INSTANCE_HAS_IMAGE_TYPE) != 0;
	instance->is_image_original = (flags & ARCHIVE_SHELL_INSTANCE_IMAGE_ORIGINAL) != 0;
	instance->is_image_resampled = (flags & ARCHIVE_SHELL_INSTANCE_IMAGE_RESAMPLED) != 0;
	instance->is_tiled_full = (flags & ARCHIVE_SHELL_INSTANCE_TILED_FULL) != 0 ||
		instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID;
	instance->pixel_spacing_row_mm = archive_shell_read_float(record + 120);
	instance->pixel_spacing_column_mm = archive_shell_read_float(record + 124);
	instance->has_slide_coordinate_origin = (flags & ARCHIVE_SHELL_INSTANCE_HAS_ORIGIN) != 0;
	instance->slide_coordinate_origin_x_mm = archive_shell_read_float(record + 128);
	instance->slide_coordinate_origin_y_mm = archive_shell_read_float(record + 132);
	instance->slide_coordinate_origin_z_um = archive_shell_read_float(record + 136);
	instance->has_image_orientation_slide = (flags & ARCHIVE_SHELL_INSTANCE_HAS_ORIENTATION) != 0;
	for (i32 i = 0; i < 6; ++i)
		instance->image_orientation_slide[i] = archive_shell_read_float(record + 140 + i * 4);
	instance->attribute_section_index = archive_shell_read_u32(record + 168);
	instance->optical_path_section_index = archive_shell_read_u32(record + 172);
	instance->frame_map_section_index = archive_shell_read_u32(record + 176);
	instance->payload_map_section_index = archive_shell_read_u32(record + 180);
	instance->payload_is_encapsulated =
		(archive_shell_read_u32(record + 188) & ARCHIVE_SHELL_PAYLOAD_ENCAPSULATED) != 0;
	if (instance->source_index >= shell->source_count ||
	    (instance->provider_kind != ARCHIVE_SHELL_PROVIDER_PART10 &&
	     instance->provider_kind != ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE) ||
	    !instance->provider_parameter_version || !instance->frame_count ||
	    !instance->rows || !instance->columns || !instance->total_pixel_matrix_rows ||
	    !instance->total_pixel_matrix_columns || !instance->optical_path_count ||
	    !instance->focal_plane_count || !instance->study_instance_uid[0] ||
	    !instance->series_instance_uid[0] || !instance->sop_instance_uid[0] ||
	    (instance->frame_map_kind != ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID &&
	     instance->frame_map_kind != ARCHIVE_SHELL_FRAME_MAP_EXPLICIT &&
	     instance->frame_map_kind != ARCHIVE_SHELL_FRAME_MAP_SPARSE_RUNS) ||
	    (instance->payload_map_kind != ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE &&
	     instance->payload_map_kind != ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA &&
	     instance->payload_map_kind !=
		     ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS &&
	     instance->payload_map_kind !=
		     ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA))
		return false;
	u64 attribute_size = 0;
	const u8* attributes = archive_shell_section_data(shell, instance->attribute_section_index,
		ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE, instance_index, &attribute_size);
	u64 optical_size = 0;
	const u8* optical_paths = archive_shell_scoped_section_data(
		shell, instance->optical_path_section_index,
		ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE,
		instance_index, &optical_size);
	u64 payload_size = 0;
	const u8* payload = archive_shell_section_data(shell, instance->payload_map_section_index,
		ARCHIVE_SHELL_SECTION_PAYLOAD_MAP, instance_index, &payload_size);
	u32 optical_record_size = optical_paths &&
		optical_size >= ARCHIVE_SHELL_OPTICAL_HEADER_SIZE ?
		archive_shell_read_u32(optical_paths + 4) : 0;
	if (!attributes || attribute_size < 16 ||
	    archive_shell_read_u32(attributes + 4) != ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE ||
	    archive_shell_read_u32(attributes) >
	    (attribute_size - 16) / ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE ||
	    !optical_paths || optical_size < ARCHIVE_SHELL_OPTICAL_HEADER_SIZE ||
	    archive_shell_read_u32(optical_paths) != instance->optical_path_count ||
	    optical_record_size != ARCHIVE_SHELL_OPTICAL_RECORD_SIZE ||
	    optical_size < ARCHIVE_SHELL_OPTICAL_HEADER_SIZE +
	                   (u64)instance->optical_path_count * optical_record_size ||
	    !payload || payload_size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    archive_shell_read_u32(payload) != (u32)instance->payload_map_kind ||
	    archive_shell_read_u32(payload + 4) != instance->frame_count) return false;
	if (instance->payload_map_kind == ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE &&
	    payload_size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE) return false;
	if (instance->payload_map_kind ==
		    ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA &&
	    !archive_shell_explicit_payload_is_valid(
		    payload, payload_size, instance->frame_count))
		return false;
	if (instance->payload_map_kind ==
		    ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS &&
	    !archive_shell_delta_payload_is_valid(
		    payload, payload_size, instance->frame_count))
		return false;
	if (instance->payload_map_kind ==
		    ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA) {
		if (!archive_shell_native_payload_is_valid(
				payload, payload_size,
				instance->frame_count))
			return false;
	}
	if (instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID) {
		if (instance->frame_map_section_index != ARCHIVE_SHELL_NO_SECTION ||
		    !instance->tiles_per_row || !instance->tiles_per_column ||
		    (u64)instance->tiles_per_row * instance->tiles_per_column !=
		    instance->frame_count) return false;
	} else if (instance->frame_map_kind ==
		   ARCHIVE_SHELL_FRAME_MAP_EXPLICIT) {
		u64 frame_map_size = 0;
		const u8* frame_map = archive_shell_section_data(shell, instance->frame_map_section_index,
			ARCHIVE_SHELL_SECTION_FRAME_MAP, instance_index, &frame_map_size);
		if (!frame_map || frame_map_size != ARCHIVE_FRAME_MAP_HEADER_SIZE +
		    (u64)instance->frame_count * ARCHIVE_FRAME_MAP_ENTRY_SIZE ||
		    memcmp(frame_map, archive_shell_frame_map_magic,
		           sizeof(archive_shell_frame_map_magic)) != 0 ||
		    archive_shell_read_u32(frame_map + 8) != ARCHIVE_FRAME_MAP_SCHEMA_VERSION ||
		    archive_shell_read_u32(frame_map + 24) != instance->frame_count) return false;
	} else {
		u64 frame_map_size = 0;
		const u8* frame_map = archive_shell_section_data(
			shell, instance->frame_map_section_index,
			ARCHIVE_SHELL_SECTION_FRAME_MAP, instance_index,
			&frame_map_size);
		if (!archive_shell_sparse_runs_are_valid(
				frame_map, frame_map_size, instance))
			return false;
	}
	if (!archive_shell_get_container_identifier(shell, instance)) return false;
	if (archive_shell_count_sections(
			shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
			instance->instance_index) +
	    archive_shell_count_sections(
			shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
			ARCHIVE_SHELL_SHARED_SECTION) != 1 ||
	    archive_shell_count_sections(
			shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
			instance->instance_index) +
	    archive_shell_count_sections(
			shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
			ARCHIVE_SHELL_SHARED_SECTION) != 1)
		return false;
	u32 specimen_count = archive_shell_get_specimen_count(shell, instance);
	for (u32 i = 0; i < specimen_count; ++i) {
		archive_shell_specimen_t specimen = {0};
		if (!archive_shell_get_specimen(shell, instance, i, &specimen)) return false;
	}
	u64 preparation_size = 0;
	const u8* preparation = archive_shell_find_scoped_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
		instance->instance_index, &preparation_size);
	if (!preparation ||
	    preparation_size < ARCHIVE_SHELL_PREPARATION_HEADER_SIZE ||
	    archive_shell_read_u32(preparation + 4) !=
		    ARCHIVE_SHELL_PREPARATION_RECORD_SIZE ||
	    archive_shell_read_u32(preparation) >
		    (preparation_size - ARCHIVE_SHELL_PREPARATION_HEADER_SIZE) /
		    ARCHIVE_SHELL_PREPARATION_RECORD_SIZE) return false;
	u32 preparation_count =
		archive_shell_get_preparation_item_count(shell, instance);
	for (u32 i = 0; i < preparation_count; ++i) {
		archive_shell_preparation_item_t item = {0};
		if (!archive_shell_get_preparation_item(
				shell, instance, i, &item)) return false;
	}
	u32 attribute_count =
		archive_shell_get_attribute_count(shell, instance);
	for (u32 i = 0; i < attribute_count; ++i) {
		archive_shell_attribute_t attribute = {0};
		if (!archive_shell_get_attribute(
				shell, instance, i, &attribute)) return false;
	}
	return true;
}

bool archive_shell_get_payload_range(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_payload_range_t* range) {
	if (!shell || !instance || !range || frame_index >= instance->frame_count) return false;
	u64 size = 0;
	const u8* section = archive_shell_section_data(shell, instance->payload_map_section_index,
		ARCHIVE_SHELL_SECTION_PAYLOAD_MAP, instance->instance_index, &size);
	if (!section || size < ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
	    archive_shell_read_u32(section + 4) != instance->frame_count) return false;
	memset(range, 0, sizeof(*range));
	if (instance->payload_map_kind == ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE) {
		u64 base = archive_shell_read_u64(section + 16);
		u64 stride = archive_shell_read_u64(section + 24);
		if (!stride || frame_index > (UINT64_MAX - base) / stride || stride > UINT32_MAX) return false;
		range->source_index = instance->source_index;
		range->offset = base + (u64)frame_index * stride;
		range->size = (u32)stride;
		archive_shell_source_t source;
		return archive_shell_get_source(shell, range->source_index, &source) &&
		       range->offset <= source.byte_size &&
		       range->size <= source.byte_size - range->offset;
	}
	if (instance->payload_map_kind ==
	    ARCHIVE_SHELL_PAYLOAD_OFFSET_SIZE_SOA) {
		u64 offset_array_size =
			(u64)instance->frame_count *
			ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE;
		u64 size_array_size =
			(u64)instance->frame_count *
			ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE;
		if (archive_shell_read_u32(section + 12) !=
			    ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE ||
		    archive_shell_read_u32(section + 28) !=
			    ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE ||
		    offset_array_size >
			    UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
		    size_array_size >
			    UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE -
				    offset_array_size ||
		    size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
			    offset_array_size + size_array_size)
			return false;
		const u8* offsets =
			section + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
		const u8* sizes = offsets + offset_array_size;
		range->source_index = instance->source_index;
		range->offset = archive_shell_read_u64(
			offsets + (size_t)frame_index *
				ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE);
		range->size = archive_shell_read_u32(
			sizes + (size_t)frame_index *
				ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE);
		archive_shell_source_t source;
		return range->size > 0 &&
		       archive_shell_get_source(
			       shell, range->source_index, &source) &&
		       range->offset <= source.byte_size &&
		       range->size <= source.byte_size - range->offset;
	}
	if (instance->payload_map_kind ==
	    ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS) {
		u64 delta_size =
			(u64)instance->frame_count *
			ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE;
		u64 correction_size =
			(u64)instance->frame_count *
			ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE;
		if (archive_shell_read_u32(section + 12) !=
			    ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE ||
		    archive_shell_read_u32(section + 28) !=
			    ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE ||
		    delta_size >
			    UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE ||
		    correction_size >
			    UINT64_MAX - ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE -
				    delta_size ||
		    size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE +
			    delta_size + correction_size)
			return false;
		const u8* deltas =
			section + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
		const u8* corrections = deltas + delta_size;
		u64 start = 0;
		for (u32 frame = 1; frame <= frame_index; ++frame) {
			u32 delta = archive_shell_read_u32(
				deltas + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
			if (start > UINT64_MAX - delta) return false;
			start += delta;
		}
		u32 frame_size = archive_shell_read_u32(section + 24);
		if (frame_index + 1 < instance->frame_count) {
			u32 next_delta = archive_shell_read_u32(
				deltas + (size_t)(frame_index + 1) *
					ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
			u32 correction = archive_shell_read_u32(
				corrections + (size_t)frame_index *
					ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE);
			if (correction >= next_delta) return false;
			frame_size = next_delta - correction;
		}
		u64 base = archive_shell_read_u64(section + 16);
		if (base > UINT64_MAX - start) return false;
		range->source_index = instance->source_index;
		range->offset = base + start;
		range->size = frame_size;
		archive_shell_source_t source;
		return frame_size > 0 &&
		       archive_shell_get_source(shell, range->source_index, &source) &&
		       range->offset <= source.byte_size &&
		       range->size <= source.byte_size - range->offset;
	}
	return false;
}

bool archive_shell_get_native_payload(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_native_payload_t* payload) {
	if (!shell || !instance || !payload ||
	    frame_index >= instance->frame_count ||
	    instance->payload_map_kind !=
		    ARCHIVE_SHELL_PAYLOAD_NATIVE_OFFSET_SIZE_SOA)
		return false;
	u64 size = 0;
	const u8* section = archive_shell_section_data(
		shell, instance->payload_map_section_index,
		ARCHIVE_SHELL_SECTION_PAYLOAD_MAP,
		instance->instance_index, &size);
	/* Full validation happens when the shell opens; lookups check only layout
	 * and the requested record, so tile access stays independent of slide size. */
	if (!archive_shell_native_payload_layout_is_valid(
			section, size, instance->frame_count))
		return false;
	u32 file_record_size = archive_shell_read_u32(section + 24);
	bool multiple_sources =
		file_record_size == ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE;
	u64 files_size = multiple_sources ?
		(u64)instance->frame_count *
			file_record_size : 0;
	u64 offsets_size =
		(u64)instance->frame_count *
		ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE;
	const u8* files =
		section + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
	const u8* offsets = files + files_size;
	const u8* sizes = offsets + offsets_size;
	payload->file_index = multiple_sources ?
		archive_shell_read_u32(
			files + (size_t)frame_index *
				ARCHIVE_SHELL_PAYLOAD_FILE_RECORD_SIZE) :
		archive_shell_read_u32(section + 8);
	payload->offset = archive_shell_read_u64(
		offsets + (size_t)frame_index *
			ARCHIVE_SHELL_PAYLOAD_OFFSET_RECORD_SIZE);
	payload->size = archive_shell_read_u32(
		sizes + (size_t)frame_index *
			ARCHIVE_SHELL_PAYLOAD_SIZE_RECORD_SIZE);
	return payload->size > 0;
}

bool archive_shell_build_payload_ranges(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance,
		archive_shell_payload_range_t* ranges, u32 range_count) {
	if (!shell || !instance || !ranges ||
	    range_count != instance->frame_count)
		return false;
	if (instance->payload_map_kind !=
	    ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS) {
		for (u32 frame = 0; frame < range_count; ++frame)
			if (!archive_shell_get_payload_range(
					shell, instance, frame, ranges + frame))
				return false;
		return true;
	}

	u64 size = 0;
	const u8* section = archive_shell_section_data(
		shell, instance->payload_map_section_index,
		ARCHIVE_SHELL_SECTION_PAYLOAD_MAP,
		instance->instance_index, &size);
	u64 array_size =
		(u64)instance->frame_count *
		ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE;
	if (!section ||
	    size != ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE + array_size * 2 ||
	    archive_shell_read_u32(section) !=
		    ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS ||
	    archive_shell_read_u32(section + 4) != instance->frame_count)
		return false;
	const u8* deltas =
		section + ARCHIVE_SHELL_PAYLOAD_HEADER_SIZE;
	const u8* corrections = deltas + array_size;
	archive_shell_source_t source = {0};
	if (!archive_shell_get_source(
			shell, instance->source_index, &source))
		return false;
	u64 offset = archive_shell_read_u64(section + 16);
	for (u32 frame = 0; frame < range_count; ++frame) {
		u32 frame_size = archive_shell_read_u32(section + 24);
		if (frame + 1 < range_count) {
			u32 next_delta = archive_shell_read_u32(
				deltas + (size_t)(frame + 1) *
					ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
			u32 correction = archive_shell_read_u32(
				corrections + (size_t)frame *
					ARCHIVE_SHELL_PAYLOAD_CORRECTION_RECORD_SIZE);
			if (!next_delta || correction >= next_delta)
				return false;
			frame_size = next_delta - correction;
		}
		if (!frame_size || offset > source.byte_size ||
		    frame_size > source.byte_size - offset)
			return false;
		ranges[frame] = (archive_shell_payload_range_t){
			.source_index = instance->source_index,
			.offset = offset,
			.size = frame_size,
		};
		if (frame + 1 < range_count) {
			u32 next_delta = archive_shell_read_u32(
				deltas + (size_t)(frame + 1) *
					ARCHIVE_SHELL_PAYLOAD_DELTA_RECORD_SIZE);
			if (offset > UINT64_MAX - next_delta)
				return false;
			offset += next_delta;
		}
	}
	return true;
}

bool archive_shell_get_frame(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 frame_index,
		archive_shell_frame_t* frame) {
	if (!shell || !instance || !frame || frame_index >= instance->frame_count) return false;
	memset(frame, 0, sizeof(*frame));
	if (instance->frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID) {
		if (!instance->tiles_per_row || !instance->tiles_per_column) return false;
		frame->row_position = (frame_index / instance->tiles_per_row) * instance->rows + 1;
		frame->column_position = (frame_index % instance->tiles_per_row) * instance->columns + 1;
		return true;
	}
	u64 size = 0;
	const u8* section = archive_shell_section_data(shell, instance->frame_map_section_index,
		ARCHIVE_SHELL_SECTION_FRAME_MAP, instance->instance_index, &size);
	if (instance->frame_map_kind ==
	    ARCHIVE_SHELL_FRAME_MAP_SPARSE_RUNS) {
		if (!section || size < ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE ||
		    memcmp(section, archive_shell_sparse_run_magic,
		           sizeof(archive_shell_sparse_run_magic)) != 0 ||
		    archive_shell_read_u32(section + 8) !=
			    ARCHIVE_SHELL_SPARSE_RUN_VERSION ||
		    archive_shell_read_u32(section + 12) !=
			    ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE ||
		    archive_shell_read_u32(section + 16) !=
			    ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE)
			return false;
		u32 run_count = archive_shell_read_u32(section + 20);
		if (size != ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
			    (u64)run_count *
				    ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE)
			return false;
		u32 low = 0;
		u32 high = run_count;
		while (low < high) {
			u32 middle = low + (high - low) / 2;
			const u8* record =
				section + ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
				(size_t)middle *
					ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
			u32 first_frame = archive_shell_read_u32(record);
			if (first_frame <= frame_index) low = middle + 1;
			else high = middle;
		}
		if (!low) return false;
		const u8* record =
			section + ARCHIVE_SHELL_SPARSE_RUN_HEADER_SIZE +
			(size_t)(low - 1) *
				ARCHIVE_SHELL_SPARSE_RUN_RECORD_SIZE;
		u32 first_frame = archive_shell_read_u32(record);
		u32 tile_row = archive_shell_read_u32(record + 4);
		u32 first_column = archive_shell_read_u32(record + 8);
		u32 tile_count = archive_shell_read_u32(record + 12);
		if (frame_index - first_frame >= tile_count) return false;
		u32 tile_column =
			first_column + frame_index - first_frame;
		frame->row_position =
			tile_row * instance->rows + 1;
		frame->column_position =
			tile_column * instance->columns + 1;
		frame->flags = ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN;
		if (instance->has_slide_coordinate_origin &&
		    instance->has_image_orientation_slide &&
		    instance->pixel_spacing_row_mm > 0.0f &&
		    instance->pixel_spacing_column_mm > 0.0f) {
			float row_distance =
				(float)(frame->row_position - 1) *
				instance->pixel_spacing_row_mm;
			float column_distance =
				(float)(frame->column_position - 1) *
				instance->pixel_spacing_column_mm;
			frame->x_offset_mm =
				instance->slide_coordinate_origin_x_mm +
				column_distance *
					instance->image_orientation_slide[0] +
				row_distance *
					instance->image_orientation_slide[3];
			frame->y_offset_mm =
				instance->slide_coordinate_origin_y_mm +
				column_distance *
					instance->image_orientation_slide[1] +
				row_distance *
					instance->image_orientation_slide[4];
			frame->flags |=
				ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN;
			frame->z_offset_um =
				instance->slide_coordinate_origin_z_um;
			frame->flags |=
				ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN;
		}
		return true;
	}
	if (!section || size < ARCHIVE_FRAME_MAP_HEADER_SIZE ||
	    frame_index >= archive_shell_read_u32(section + 24)) return false;
	const u8* record = section + ARCHIVE_FRAME_MAP_HEADER_SIZE +
		(size_t)frame_index * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
	if ((u64)(record - section) + ARCHIVE_FRAME_MAP_ENTRY_SIZE > size) return false;
	frame->row_position = archive_shell_read_u32(record);
	frame->column_position = archive_shell_read_u32(record + 4);
	frame->x_offset_mm = archive_shell_read_float(record + 8);
	frame->y_offset_mm = archive_shell_read_float(record + 12);
	frame->z_offset_um = archive_shell_read_float(record + 16);
	frame->optical_path_index = archive_shell_read_u32(record + 20);
	frame->focal_plane_index = archive_shell_read_u32(record + 24);
	frame->flags = archive_shell_read_u32(record + 28);
	return frame->optical_path_index < instance->optical_path_count &&
	       frame->focal_plane_index < instance->focal_plane_count;
}

bool archive_shell_get_optical_path(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 optical_path_index,
		archive_shell_optical_path_t* optical_path) {
	if (!shell || !instance || !optical_path ||
	    optical_path_index >= instance->optical_path_count) return false;
	u64 size = 0;
	const u8* section = archive_shell_scoped_section_data(
		shell, instance->optical_path_section_index,
		ARCHIVE_SHELL_SECTION_OPTICAL_PATH_TABLE,
		instance->instance_index, &size);
	if (!section || size < ARCHIVE_SHELL_OPTICAL_HEADER_SIZE ||
	    archive_shell_read_u32(section) != instance->optical_path_count ||
	    archive_shell_read_u32(section + 4) != ARCHIVE_SHELL_OPTICAL_RECORD_SIZE) return false;
	u32 record_size = archive_shell_read_u32(section + 4);
	if (size < ARCHIVE_SHELL_OPTICAL_HEADER_SIZE +
	           (u64)instance->optical_path_count * record_size) return false;
	const u8* record = section + ARCHIVE_SHELL_OPTICAL_HEADER_SIZE +
		(size_t)optical_path_index * record_size;
	memset(optical_path, 0, sizeof(*optical_path));
	optical_path->identifier = archive_shell_string(shell, archive_shell_read_u32(record));
	optical_path->illumination_wavelength = archive_shell_read_float(record + 8);
	const char** values[] = {
		&optical_path->description,
		&optical_path->color_space,
		&optical_path->illumination_type_code_value,
		&optical_path->illumination_type_coding_scheme,
		&optical_path->illumination_type_code_meaning,
		&optical_path->illumination_color_code_value,
		&optical_path->illumination_color_coding_scheme,
		&optical_path->illumination_color_code_meaning,
	};
	u32 offsets[] = {56, 4, 32, 36, 40, 44, 48, 52};
	for (i32 i = 0; i < COUNT(values); ++i) {
		*values[i] = archive_shell_string(
			shell, archive_shell_read_u32(record + offsets[i]));
		if (!*values[i]) return false;
	}
	u64 offset = archive_shell_read_u64(record + 16);
	optical_path->icc_profile_size = archive_shell_read_u64(record + 24);
	optical_path->source_preserved_fields = archive_shell_read_u32(record + 60);
	optical_path->inferred_fields = archive_shell_read_u32(record + 64);
	optical_path->light_path_filter_pass_through_wavelength =
		archive_shell_read_u16(record + 68);
	optical_path->light_path_filter_pass_band[0] = archive_shell_read_u16(record + 70);
	optical_path->light_path_filter_pass_band[1] = archive_shell_read_u16(record + 72);
	optical_path->image_path_filter_pass_through_wavelength =
		archive_shell_read_u16(record + 74);
	optical_path->image_path_filter_pass_band[0] = archive_shell_read_u16(record + 76);
	optical_path->image_path_filter_pass_band[1] = archive_shell_read_u16(record + 78);
	optical_path->objective_lens_power = archive_shell_read_float(record + 80);
	optical_path->objective_lens_numerical_aperture =
		archive_shell_read_float(record + 84);
	if (!optical_path->identifier || offset > size ||
	    optical_path->icc_profile_size > size - offset) return false;
	if (((optical_path->source_preserved_fields &
	      ARCHIVE_SHELL_OPTICAL_OBJECTIVE_POWER) &&
	     !isfinite(optical_path->objective_lens_power)) ||
	    ((optical_path->source_preserved_fields &
	      ARCHIVE_SHELL_OPTICAL_OBJECTIVE_APERTURE) &&
	     !isfinite(optical_path->objective_lens_numerical_aperture))) return false;
	if (optical_path->icc_profile_size) {
		optical_path->icc_profile = section + offset;
		if (optical_path->icc_profile_size > UINT32_MAX ||
		    !archive_shell_icc_profile_is_valid(
			    optical_path->icc_profile, (u32)optical_path->icc_profile_size)) return false;
	}
	return true;
}

const char* archive_shell_get_container_identifier(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance) {
	if (!shell || !instance) return NULL;
	u64 size = 0;
	const u8* section = archive_shell_find_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
		instance->instance_index, &size);
	if (!section)
		section = archive_shell_find_section(
			shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
			ARCHIVE_SHELL_SHARED_SECTION, &size);
	if (!section || size < ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE ||
	    archive_shell_read_u32(section + 4) != ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE ||
	    archive_shell_read_u32(section) >
		    (size - ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE) /
		    ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE) return NULL;
	return archive_shell_string(shell, archive_shell_read_u32(section + 8));
}

u32 archive_shell_get_specimen_count(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance) {
	if (!shell || !instance) return 0;
	u64 size = 0;
	const u8* section = archive_shell_find_scoped_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
		instance->instance_index, &size);
	if (!section) return 0;
	u32 count = size >= ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE ?
		archive_shell_read_u32(section) : 0;
	return size >= ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE &&
	       archive_shell_read_u32(section + 4) == ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE &&
	       count <= (size - ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE) /
	                ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE ? count : 0;
}

bool archive_shell_get_specimen(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 specimen_index,
		archive_shell_specimen_t* specimen) {
	u32 count = archive_shell_get_specimen_count(shell, instance);
	if (!specimen || specimen_index >= count) return false;
	u64 size = 0;
	const u8* section = archive_shell_find_scoped_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
		instance->instance_index, &size);
	const u8* record = section + ARCHIVE_SHELL_SPECIMEN_HEADER_SIZE +
	                   (size_t)specimen_index * ARCHIVE_SHELL_SPECIMEN_RECORD_SIZE;
	memset(specimen, 0, sizeof(*specimen));
	const char** values[] = {
		&specimen->identifier, &specimen->uid, &specimen->short_description,
	};
	for (i32 i = 0; i < COUNT(values); ++i) {
		*values[i] = archive_shell_string(
			shell, archive_shell_read_u32(record + i * 4));
		if (!*values[i]) return false;
	}
	return true;
}

bool archive_shell_specimen_metadata_is_shared(
		const archive_shell_view_t* shell) {
	return archive_shell_find_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_TABLE,
		ARCHIVE_SHELL_SHARED_SECTION, NULL) != NULL;
}

bool archive_shell_preparation_metadata_is_shared(
		const archive_shell_view_t* shell) {
	return archive_shell_find_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
		ARCHIVE_SHELL_SHARED_SECTION, NULL) != NULL;
}

u32 archive_shell_get_preparation_item_count(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance) {
	if (!shell || !instance) return 0;
	u64 size = 0;
	const u8* section = archive_shell_find_scoped_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
		instance->instance_index, &size);
	if (!section || size < ARCHIVE_SHELL_PREPARATION_HEADER_SIZE ||
	    archive_shell_read_u32(section + 4) !=
		    ARCHIVE_SHELL_PREPARATION_RECORD_SIZE) return 0;
	u32 count = archive_shell_read_u32(section);
	return count <=
		(size - ARCHIVE_SHELL_PREPARATION_HEADER_SIZE) /
		ARCHIVE_SHELL_PREPARATION_RECORD_SIZE ? count : 0;
}

bool archive_shell_get_preparation_item(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 item_index,
		archive_shell_preparation_item_t* item) {
	u32 count = archive_shell_get_preparation_item_count(shell, instance);
	if (!item || item_index >= count) return false;
	u64 size = 0;
	const u8* section = archive_shell_find_scoped_section(
		shell, ARCHIVE_SHELL_SECTION_SPECIMEN_PREPARATION,
		instance->instance_index, &size);
	const u8* record =
		section + ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
		(size_t)item_index * ARCHIVE_SHELL_PREPARATION_RECORD_SIZE;
	memset(item, 0, sizeof(*item));
	item->specimen_index = archive_shell_read_u32(record);
	item->step_index = archive_shell_read_u32(record + 4);
	item->parent_item_index = archive_shell_read_u32(record + 8);
	item->value_kind =
		(archive_shell_content_value_kind_t)archive_shell_read_u32(record + 12);
	const char** values[] = {
		&item->concept_name_code_value,
		&item->concept_name_coding_scheme,
		&item->concept_name_coding_scheme_version,
		&item->concept_name_code_meaning,
		&item->scalar_value,
		&item->code_coding_scheme,
		&item->code_coding_scheme_version,
		&item->code_meaning,
		&item->measurement_units_code_value,
		&item->measurement_units_coding_scheme,
		&item->measurement_units_coding_scheme_version,
		&item->measurement_units_code_meaning,
		&item->relationship_type,
	};
	for (i32 field = 0; field < COUNT(values); ++field) {
		*values[field] = archive_shell_string(
			shell,
			archive_shell_read_u32(record + 16 + (size_t)field * 4));
		if (!*values[field]) return false;
	}
	item->code_value = item->value_kind == ARCHIVE_SHELL_CONTENT_CODE ?
		item->scalar_value : "";
	if (item->specimen_index >= archive_shell_get_specimen_count(
			shell, instance) ||
	    item->value_kind < ARCHIVE_SHELL_CONTENT_CODE ||
	    item->value_kind > ARCHIVE_SHELL_CONTENT_PNAME ||
	    (item->parent_item_index != UINT32_MAX &&
	     item->parent_item_index >= item_index))
		return false;
	if (item->parent_item_index != UINT32_MAX) {
		const u8* parent =
			section + ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
			(size_t)item->parent_item_index *
				ARCHIVE_SHELL_PREPARATION_RECORD_SIZE;
		if (archive_shell_read_u32(parent) != item->specimen_index ||
		    archive_shell_read_u32(parent + 4) != item->step_index)
			return false;
	}
	u32 depth = 0;
	u32 parent_index = item->parent_item_index;
	while (parent_index != UINT32_MAX) {
		if (++depth > 16 || parent_index >= item_index) return false;
		const u8* parent =
			section + ARCHIVE_SHELL_PREPARATION_HEADER_SIZE +
			(size_t)parent_index * ARCHIVE_SHELL_PREPARATION_RECORD_SIZE;
		parent_index = archive_shell_read_u32(parent + 8);
	}
	return true;
}

static u32 archive_shell_attribute_table_count(
		const u8* section, u64 size) {
	if (!section || size < 16 ||
	    archive_shell_read_u32(section + 4) != ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE) return 0;
	u32 count = archive_shell_read_u32(section);
	return count <= (size - 16) / ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE ? count : 0;
}

static bool archive_shell_decode_attribute(
		const archive_shell_view_t* shell, const u8* section,
		u32 attribute_index, archive_shell_attribute_t* attribute) {
	const u8* record = section + 16 +
		(size_t)attribute_index * ARCHIVE_SHELL_ATTRIBUTE_RECORD_SIZE;
	memset(attribute, 0, sizeof(*attribute));
	attribute->tag = archive_shell_read_u32(record);
	u16 vr = archive_shell_read_u16(record + 4);
	attribute->vr[0] = (char)(u8)vr;
	attribute->vr[1] = (char)(u8)(vr >> 8);
	attribute->value_kind =
		(archive_shell_attribute_value_kind_t)archive_shell_read_u16(record + 6);
	u64 value = archive_shell_read_u64(record + 8);
	switch (attribute->value_kind) {
		case ARCHIVE_SHELL_ATTRIBUTE_STRING:
			attribute->string_value = archive_shell_string(shell, (u32)value);
			return value <= UINT32_MAX && attribute->string_value != NULL;
		case ARCHIVE_SHELL_ATTRIBUTE_UNSIGNED:
			attribute->unsigned_value = value;
			return true;
		case ARCHIVE_SHELL_ATTRIBUTE_SIGNED:
			attribute->signed_value = (i64)value;
			return true;
		default: return false;
	}
}

u32 archive_shell_get_attribute_count(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance) {
	if (!shell || !instance) return 0;
	return archive_shell_get_governed_attribute_count(shell) +
	       archive_shell_get_attribute_override_count(shell, instance);
}

u32 archive_shell_get_governed_attribute_count(
		const archive_shell_view_t* shell) {
	if (!shell) return 0;
	u64 governed_size = 0;
	const u8* governed = archive_shell_find_section(
		shell, ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE,
		ARCHIVE_SHELL_SHARED_SECTION, &governed_size);
	return archive_shell_attribute_table_count(governed, governed_size);
}

u32 archive_shell_get_attribute_override_count(
		const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance) {
	if (!shell || !instance) return 0;
	u64 local_size = 0;
	const u8* local = archive_shell_section_data(
		shell, instance->attribute_section_index,
		ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE,
		instance->instance_index, &local_size);
	return archive_shell_attribute_table_count(local, local_size);
}

bool archive_shell_get_attribute(const archive_shell_view_t* shell,
		const archive_shell_instance_t* instance, u32 attribute_index,
		archive_shell_attribute_t* attribute) {
	if (!shell || !instance || !attribute) return false;
	u64 governed_size = 0;
	const u8* governed = archive_shell_find_section(
		shell, ARCHIVE_SHELL_SECTION_GOVERNED_ATTRIBUTE_TABLE,
		ARCHIVE_SHELL_SHARED_SECTION, &governed_size);
	u32 governed_count =
		archive_shell_attribute_table_count(governed, governed_size);
	if (attribute_index < governed_count)
		return archive_shell_decode_attribute(
			shell, governed, attribute_index, attribute);
	attribute_index -= governed_count;
	u64 local_size = 0;
	const u8* local = archive_shell_section_data(
		shell, instance->attribute_section_index,
		ARCHIVE_SHELL_SECTION_ATTRIBUTE_TABLE,
		instance->instance_index, &local_size);
	u32 local_count =
		archive_shell_attribute_table_count(local, local_size);
	return attribute_index < local_count &&
	       archive_shell_decode_attribute(
		       shell, local, attribute_index, attribute);
}

void archive_shell_buffer_destroy(archive_shell_buffer_t* shell) {
	if (!shell) return;
	free(shell->data);
	memset(shell, 0, sizeof(*shell));
}
