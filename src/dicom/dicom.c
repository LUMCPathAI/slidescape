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

#define DICOM_IMPL

#include "common.h"
#include "platform.h"
#include "mathutils.h"
#include "listing.h"

#include "viewer.h"
#include "stringutils.h"

#include "dicom.h"
#include "dicom_wsi.h"

#include "lz4.h"

#include <ctype.h>

#define LISTING_IMPLEMENTATION
#include "listing.h"


static void dicom_switch_data_encoding(dicom_instance_t* instance, dicom_data_element_t *transfer_syntax_uid) {

	// TODO: what if the transfer UID only applies to the interpretation of e.g. pixel data?
	// list of UIDs: https://dicom.nema.org/medical/dicom/current/output/chtml/part06/chapter_A.html

	instance->transfer_syntax_uid = dicom_parse_uid((str_t) {
		(const char*)instance->data + transfer_syntax_uid->data_offset,
		transfer_syntax_uid->length
	});

	// The first 18 chars do not discriminate (all have the prefix "1.2.840.10008.1.2")
	const char* suffix = (const char*) &(instance->data + transfer_syntax_uid->data_offset)[17];
	u32 uid_length = transfer_syntax_uid->length;

	if (uid_length == 20 && suffix[0] == '.') {
		if (suffix[1] == '1') {
			instance->encoding = DICOM_TRANSFER_SYNTAX_EXPLICIT_VR_LITTLE_ENDIAN; // 1.2.840.10008.1.2.1
		} else if (suffix[1] == '2') {
			instance->encoding = DICOM_TRANSFER_SYNTAX_EXPLICIT_VR_BIG_ENDIAN_RETIRED; // 1.2.840.10008.1.2.2
		}
	} else if (uid_length == 23 && strncmp(suffix, ".1.99", 5) == 0) {
		instance->encoding = DICOM_TRANSFER_SYNTAX_DEFLATED_EXPLICIT_VR_LITTLE_ENDIAN; // 1.2.840.10008.1.2.1.99
	}
}

static inline bool32 need_alternate_element_layout(u16 vr) {
	int sum = (vr == DICOM_VR_OB) +
	          (vr == DICOM_VR_OD) +
	          (vr == DICOM_VR_OF) +
	          (vr == DICOM_VR_OL) +
	          (vr == DICOM_VR_OV) +
	          (vr == DICOM_VR_OW) +
	          (vr == DICOM_VR_SQ) +
	          (vr == DICOM_VR_UC) +
	          (vr == DICOM_VR_UR) +
	          (vr == DICOM_VR_UT) +
	          (vr == DICOM_VR_UN);
	return (sum != 0);
}

dicom_dict_entry_t* dicom_dict_entries;
const char* dicom_dict_string_pool;

dicom_dict_entry_t* dicom_dict_hash_table;
u32 dicom_dict_hash_table_size;

// Hash function adapted from https://github.com/skeeto/hash-prospector
u32 lowbias32(u32 x) {
	x ^= x >> 16;
	x *= 0x21f0aaad;
	x ^= x >> 15;
	x *= 0x735a2d97;
	x ^= x >> 15;
	return x;
}

// Note: Unfortunately it looks like lookup using a hash table is only marginally faster than linear lookup
static dicom_dict_entry_t* dicom_dict_lookup(u32 tag) {
	u32 hash = lowbias32(tag);
	u32 index = hash & (dicom_dict_hash_table_size-1);
	dicom_dict_entry_t* slot = dicom_dict_hash_table + index;
	i32 j = 0;
	u32 first_checked_index = index;
	while (slot->tag != tag) {
		// linear probing
		++j;
		index = (first_checked_index + j) & (dicom_dict_hash_table_size-1);
		slot = dicom_dict_hash_table + index;
		if (slot->tag == 0) {
			// empty slot -> not found
			return NULL;
		}
	}
	return slot;
}

static u16 get_dicom_tag_vr(u32 tag) {
	dicom_dict_entry_t* entry = dicom_dict_lookup(tag);
	if (entry) {
		return entry->vr;
	} else {
		return DICOM_VR_UN;
	}
}


static const char* get_dicom_tag_name(u32 tag) {
	dicom_dict_entry_t* entry = dicom_dict_lookup(tag);
	if (entry) {
		return dicom_dict_string_pool + entry->name_offset;
	} else {
		return NULL;
	}
}

static const char* get_dicom_tag_keyword(u32 tag) {
	dicom_dict_entry_t* entry = dicom_dict_lookup(tag);
	if (entry) {
		return dicom_dict_string_pool + entry->keyword_offset;
	} else {
		return NULL;
	}
}

#if 0
static u16 get_dicom_tag_vr_linear(u32 tag) {
	u32 tag_count = COUNT(dicom_dict_packed_entries);

	// TODO: hash table
	for (u32 tag_index = 0; tag_index < tag_count; ++tag_index) {
		if (dicom_dict_entries[tag_index].tag == tag) {
			return dicom_dict_entries[tag_index].vr;
		}
	}
	return DICOM_VR_UN;
}

static u16 test_vr;

void lookup_tester() {
	i64 start = get_clock();

	i32 lookup_count = 1000000;

	srand(46458);
	for (i32 i = 0; i < lookup_count; ++i) {
		i32 entry_index = rand() % COUNT(dicom_dict_packed_entries);
		dicom_dict_entry_t* entry_to_lookup = dicom_dict_entries + entry_index;
		test_vr = get_dicom_tag_vr(entry_to_lookup->tag);
	}
	console_print("Lookup using hash table (%dx) took %g seconds\n", lookup_count, get_seconds_elapsed(start, get_clock()));

	start = get_clock();

	srand(46458);
	for (i32 i = 0; i < lookup_count; ++i) {
		i32 entry_index = rand() % COUNT(dicom_dict_packed_entries);
		dicom_dict_entry_t* entry_to_lookup = dicom_dict_entries + entry_index;
		test_vr = get_dicom_tag_vr_linear(entry_to_lookup->tag);
	}
	console_print("Lookup using linear method (%dx) took %g seconds\n", lookup_count, get_seconds_elapsed(start, get_clock()));

}
#endif


static dicom_uid_enum dicom_uid_lookup(const char* uid_str, u32 len) {
	// TODO: hash table
	if (len > 14 && strncmp(uid_str, "1.2.840.10008.", 14) == 0) {
		for (i32 i = 1; i < COUNT(dicom_dict_uid_entries); ++i) {
			dicom_dict_uid_entry_t* entry = dicom_dict_uid_entries + i;
			if (strncmp(uid_str + 14, entry->uid_last_part, len - 14) == 0) {
				return i;
			}
		}
		console_print("DICOM UID not found: %s\n", uid_str);
	}
	return 0;
}

dicom_ui_t dicom_parse_uid(str_t s) {
    dicom_ui_t ui = {};
    i32 len = ATMOST(s.len, sizeof(ui.value));
    memcpy(ui.value, s.s, len);
    ui.value[len] = '\0';
    ui.len = strlen(ui.value);
    ui.as_enum = dicom_uid_lookup(ui.value, ui.len);
    return ui;
}

static dicom_dict_uid_entry_t* dicom_uid_get_entry(const dicom_ui_t* uid) {
	dicom_uid_enum uid_index = uid->as_enum;
	if (uid_index > 0) {
		return dicom_dict_uid_entries + uid_index;
	} else {
		return NULL;
	}
}

dicom_data_element_t dicom_read_data_element(u8* data_start, i64 data_offset, dicom_transfer_syntax_enum encoding, i64 bytes_available) {
	dicom_data_element_t result = {};
	u8* pos = data_start + data_offset;
	if (bytes_available >= 8) {
		result.is_valid = true; // may override with false later
		result.tag = *(dicom_tag_t*) pos;

		if (result.tag.group == 0xFFFE &&
				(result.tag.as_u32 == DICOM_Item ||
				result.tag.as_u32 == DICOM_ItemDelimitationItem ||
				result.tag.as_u32 == DICOM_SequenceDelimitationItem))
		{
			// special cases: DICOM_Item, DICOM_ItemDelimitationItem, DICOM_SequenceDelimitationItem
			dicom_implicit_data_element_header_t* element_header = (dicom_implicit_data_element_header_t*) pos;
			result.tag = element_header->tag;
			result.length = element_header->value_length;
//			result.data = element_header->data;
			result.data_offset = data_offset + sizeof(*element_header);
			result.vr = 0; // undefined
		} else if (encoding == DICOM_TRANSFER_SYNTAX_EXPLICIT_VR_LITTLE_ENDIAN || result.tag.group == 2 /*File Meta info*/ ) {
			// Data element is Explicit VR.
			dicom_explicit_data_element_header_t* element_header = (dicom_explicit_data_element_header_t*) pos;
			result.vr = element_header->vr;

			// Some VRs have the value length field stored differently...
			if (need_alternate_element_layout(result.vr)) {
				if (bytes_available >= 12) {
					result.length = *(u32*) &element_header->variable_part[+2];
//					result.data = pos + 12; // Advance to value field
					result.data_offset = data_offset + 12; // Advance to value field
				} else {
					result.is_valid = false;
				}
			} else {
				result.length = *(u16*) &element_header->variable_part[0];
//				result.data = pos + 8;
				result.data_offset = data_offset + 8;
			}
		} else {
			// Data element is Implicit VR.
			dicom_implicit_data_element_header_t* element_header = (dicom_implicit_data_element_header_t*) pos;
			result.tag = element_header->tag;
			result.length = element_header->value_length;
//			result.data = element_header->data;
			result.data_offset = data_offset + sizeof(*element_header);
			result.vr = get_dicom_tag_vr(result.tag.as_u32); // look up the VR from the data dictionary.
		}
	}

	return result;
}

static inline u32 dicom_get_element_length_without_trailing_whitespace(u8* element_data, u32 element_length) {
	u32 length = element_length;
	if (length != 0 && length % 2 == 0 && element_data[length-1] == ' ') {
		--length; // ignore trailing space character
	}
	return length;
}

// Print information about a data element
static void debug_print_dicom_element(dicom_instance_t* instance, dicom_data_element_t element, FILE* out, i32 nesting_level, u32 item_number) {
	memrw_t string_builder = memrw_create(512);
	if (nesting_level > 0) {
		for (i32 i = 1; i < nesting_level; ++i) {
			memrw_write_literal("  ", &string_builder); // extra indentation
		}
		memrw_printf(&string_builder, "  %u: ", item_number);
	}

	char vr_text[4] = {}; // convert 2-byte VR to printable form
	*(u16*) vr_text = element.vr;
	const char* keyword = get_dicom_tag_keyword(element.tag.as_u32);
	memrw_printf(&string_builder, "(%04x,%04x) - %s - length: %d - %s",
	             element.tag.group, element.tag.element, vr_text, element.length,
	             keyword);

	u8* element_data = instance->data + element.data_offset;

	// Try to print out the value of the tag.
	switch(element.vr) {
		default: break;
		case DICOM_VR_UI:
		case DICOM_VR_SH:
		case DICOM_VR_LO:
		case DICOM_VR_AE:
		case DICOM_VR_AS:
		case DICOM_VR_CS:
		case DICOM_VR_DS:
		case DICOM_VR_PN:
		case DICOM_VR_IS:
		case DICOM_VR_DA:
		case DICOM_VR_LT:
		case DICOM_VR_UT:
		case DICOM_VR_TM: {
			// print identifier
			char identifier[65];
			u32 length = MIN(64, dicom_get_element_length_without_trailing_whitespace(element_data, element.length));
			if (length > 0) {
				memcpy(identifier, element_data, length);
				identifier[length] = '\0';
				memrw_printf(&string_builder, " - \"%s\"", identifier);
				if (element.vr == DICOM_VR_UI) {
                    dicom_ui_t ui = dicom_parse_uid((str_t){(const char*) element_data, length});
					dicom_dict_uid_entry_t* uid_entry = dicom_uid_get_entry(&ui);
					if (uid_entry) {
						const char* uid_keyword = dicom_dict_string_pool + uid_entry->keyword_offset;
						memrw_printf(&string_builder, " - %s", uid_keyword);
					}
				}
			}
		} break;
		case DICOM_VR_UL: {
			u32 value = *(u32*) element_data;
			memrw_printf(&string_builder, " - %u", value);
		} break;
		case DICOM_VR_SL: {
			i32 value = *(i32*) element_data;
			memrw_printf(&string_builder, " - %d", value);
		} break;
		case DICOM_VR_US: {
			u32 value = *(u16*) element_data;
			memrw_printf(&string_builder, " - %u", value);
		} break;
		case DICOM_VR_SS: {
			i32 value = *(i16*) element_data;
			memrw_printf(&string_builder, " - %d", value);
		} break;
		case DICOM_VR_FL: {
			float value = *(float*) element_data;
			memrw_printf(&string_builder, " - %g", value);
		} break;
	}
	memrw_putc('\n', &string_builder);

	memrw_putc('\0', &string_builder);
	console_print_verbose("%s", string_builder.data);
	if (out) {
		fprintf(out, "%s", string_builder.data);
	}
	memrw_destroy(&string_builder);
}

static void handle_dicom_tag_for_tag_dumping(dicom_series_t* series, dicom_instance_t* instance, dicom_data_element_t element) {
	u32 current_item_number = instance->pos_stack[instance->nesting_level].item_number;
	debug_print_dicom_element(instance, element, series->debug_output_file, instance->nesting_level,
	                          current_item_number);
}


i64 dicom_parse_integer_string(str_t s, str_t* next) {
	// From the standard:
	// A string of characters representing an Integer in base-10 (decimal), shall contain only the characters 0 - 9,
    // with an optional leading "+" or "-". It may be padded with leading and/or trailing spaces. Embedded spaces are not allowed.

	// TODO: handle multiple values

	// Notes:
	// * the string will generally NOT be zero-terminated
	// * only base 10
	// * leading and trailing spaces are not significant
	i64 result = 0;
	bool positive = true;
	bool in_leading_section = true;
	for (i32 i = 0; i < s.len; ++i) {
		char c = s.s[i];
		if (c == '\0') break;
		if (in_leading_section) {
			if (c == ' ') continue;
			else if (c == '-') {
				positive = false;
				in_leading_section = false;
				continue;
			} else if (c == '+') {
				in_leading_section = false;
				continue;
			}
		}
		in_leading_section = false;
		if (c >= '0' && c <= '9') {
			result = (result * 10) + (c - '0');
		} else {
			break; // any other character other than a decimal terminates the number
		}
	}
	if (!positive) {
		result = -result;
	}
	return result;
}


float dicom_parse_decimal_string(str_t s, str_t* next) {
	// From the standard:
	// A string of characters representing either a fixed point number or a floating point number.
	// A fixed point number shall contain only the characters 0-9 with an optional leading "+" or "-" and an optional "." to mark the decimal point.
	// A floating point number shall be conveyed as defined in ANSI X3.9, with an "E" or "e" to indicate the start of the exponent.
	// Decimal Strings may be padded with leading or trailing spaces. Embedded spaces are not allowed.

	// Basically, we are parsing a normal floating point value, and we can use strtof() or strtod() to do this.
	float result = 0.0f;
	char s_copy[17] = {};

	// skip leading spaces
	i32 start = 0;
	for (i32 i = 0; i < s.len; ++i) {
		char c = s.s[i];
		if (c == ' ') {
			++start;
			continue;
		}
		break;
	}

	// check if there is another value
	i32 value_end = s.len;
	for (i32 i = start; i < s.len; ++i) {
		char c = s.s[i];
		if (c == '\\') {
			value_end = i;
			if (next) *next = (str_t){s.s + i + 1, s.len - i - 1};
			break;
		}
	}
	i32 value_len = value_end - start;
	if (value_len > 16) value_len = 16;
	memcpy(s_copy, s.s + start, value_len);

	result = strtof(s_copy, NULL);
	return result;
}


dicom_cs_t dicom_parse_code_string(str_t s, str_t* next) {
	dicom_cs_t result = {};
	i32 bytes_written = 0;
	bool in_leading_section = true;
	if (next) *next = (str_t){}; // set to position after \ separator if there is another value
	for (i32 i = 0; i < s.len; ++i) {
		char c = s.s[i];
		if (c == '\0') break;
		if (in_leading_section && c == ' ') continue;
		in_leading_section = false;
		if (c == '\\') {
			if (next) {
				*next = (str_t) {s.s + i + 1, s.len - i - 1};
			}
			break;
		}
		result.value[bytes_written++] = c;
		if (bytes_written >= COUNT(result.value)-1) break;
	}
	// Strip trailing whitespace
	i32 last_char = ATLEAST(0, bytes_written - 1);
	while (result.value[last_char] == ' ') {
		result.value[last_char--] = '\0';
		if (last_char <= 0) break;
	}
    result.present = true;
	return result;
}

dicom_sh_t dicom_parse_short_string(str_t s) {
    dicom_sh_t result = {};
    i32 bytes_written = 0;
    bool in_leading_section = true;
    for (i32 i = 0; i < s.len; ++i) {
        char c = s.s[i];
        if (c == '\0') break;
        if (in_leading_section && c == ' ') continue;
        in_leading_section = false;
        if (c == '\\') {
            break;
        }
        result.value[bytes_written++] = c;
        if (bytes_written >= COUNT(result.value)-1) break;
    }
    // Strip trailing whitespace
    i32 last_char = ATLEAST(0, bytes_written - 1);
    while (result.value[last_char] == ' ') {
        result.value[last_char--] = '\0';
        if (last_char <= 0) break;
    }
    result.present = true;
    return result;
}

// similar to atoi(), but for non-zero-terminated strings
static i32 antoi(const char* str, i32 len) {
    i32 num = 0;
    bool neg = false;
    while (isspace(*str)) {
        ++str;
        --len;
    }
    if (*str == '-') {
        neg = true;
        ++str;
        --len;
    }
    while (len > 0 && isdigit(*str)) {
        num = 10*num + (*str - '0');
        ++str;
        --len;
    }
    if (neg) num = -num;
    return num;
}

dicom_da_t dicom_parse_date(str_t s) {
    dicom_da_t result = {};
    if (s.len == 8) {
        // YYYYMMDD
        result.day = antoi(s.s+6, 2);
        result.month = antoi(s.s+4, 2);
        result.year = antoi(s.s, 4);
    }
    result.present = true;
    return result;
}

dicom_tm_t dicom_parse_time(str_t s) {
    dicom_tm_t result = {};
    u32 len = dicom_get_element_length_without_trailing_whitespace((u8*)s.s, s.len);
    if (len >= 2) {
        result.hours = antoi(s.s, 2);
    }
    if (len >= 4) {
        result.minutes = antoi(s.s+2, 2);
    }
    if (len >= 6) {
        result.seconds = antoi(s.s+4, 2);
    }
    if (len >= 8 && len <= 14) {
        result.fractional_part = antoi(s.s+7, (i32)len - 7);
    }
    result.present = true;
    return result;
}

static void dicom_interpret_top_level_data_element(dicom_instance_t* instance, dicom_data_element_t element) {
	u8* data = instance->data + element.data_offset;
	char* data_str = (char*)data;
    str_t str = {(char*)data, element.length};

	switch(element.tag.group) {
		default: break;
		case 0x0002: {
			if (element.vr == DICOM_VR_UI) {
				if (element.tag.as_enum == DICOM_MediaStorageSOPClassUID) {
                    instance->sop_class_uid = dicom_parse_uid(str);
					if (instance->sop_class_uid.as_enum == DICOM_VLWholeSlideMicroscopyImageStorage) {
                        //?
					}
				} else if (element.tag.as_enum == DICOM_MediaStorageSOPInstanceUID) {
                    instance->sop_instance_uid = dicom_parse_uid(str);
                }
			}
		} break;
		case 0x0008: {
			switch(element.tag.as_u32) {
				default: break;
				case DICOM_ImageType: {
					// Handled by SOP class
				} break;
				case DICOM_SOPClassUID: {
                    // NOTE: should be identical to DICOM_MediaStorageSOPClassUID, ignore if already present
                    if (instance->sop_class_uid.len == 0) {
                        instance->sop_class_uid = dicom_parse_uid(str);
                    }
				} break;
                case DICOM_SOPInstanceUID: {
                    // NOTE: should be identical to DICOM_MediaStorageSOPInstanceUID, ignore if already present
                    if (instance->sop_instance_uid.len == 0) {
                        instance->sop_instance_uid = dicom_parse_uid(str);
                    }
                } break;

                    // Module: General Study
                case DICOM_StudyDate: {
                    instance->general_study.StudyDate = dicom_parse_date(str);
                } break;
                case DICOM_StudyTime: {
                    instance->general_study.StudyTime = dicom_parse_time(str);
                } break;
                case DICOM_AccessionNumber: {
                    instance->general_study.AccessionNumber = dicom_parse_short_string(str);
                } break;
                case DICOM_ReferringPhysicianName: {
//                    instance->general_study.ReferringPhysicianName.value = dicom_parse_person_name(str);
                    instance->general_study.ReferringPhysicianName.present = true;
                } break;

                // Module: General Series
                case DICOM_Modality: {
                    instance->general_series.Modality = dicom_parse_code_string(str, NULL);
                } break;

                case DICOM_ContentDate: {
                    // not handled right now
                } break;
                case DICOM_ContentTime: {
                    // not handled right now
                } break;
			}
		} break;
		case 0x0020: {
			switch(element.tag.as_u32) {
				default: break;

                // Module: General Study
                case DICOM_StudyInstanceUID: {
                    instance->general_study.StudyInstanceUID = dicom_parse_uid(str);
                } break;
                case DICOM_StudyID: {
                    instance->general_study.StudyID = dicom_parse_short_string(str);
                } break;

                case DICOM_SeriesInstanceUID: {
                    instance->general_series.SeriesInstanceUID = dicom_parse_uid(str);
                } break;
				case DICOM_SeriesNumber: {
					// TODO
				} break;
				case DICOM_InstanceNumber: {
					instance->instance_number = dicom_parse_integer_string(str, NULL);
				} break;
				case DICOM_PatientOrientation: {
					// TODO
				} break;
				case DICOM_FrameOfReferenceUID: {
					// TODO
				} break;
				case DICOM_PositionReferenceIndicator: {
					// TODO
				} break;
				case DICOM_SOPInstanceUIDOfConcatenationSource: {
					// not handled for now
				} break;
				case DICOM_ConcatenationUID: {
					// NOTE: if this is present, we are dealing with a concatenation
					instance->concatenation_uid = dicom_parse_uid(str);
				} break;
				case DICOM_InConcatenationNumber: {
					instance->in_concatenation_number = *(u16*)data;
				} break;
				case DICOM_InConcatenationTotalNumber: {
					// value is optional; don't assume present
				} break;
				case DICOM_ConcatenationFrameOffsetNumber: {
					// NOTE: If this is part of a concatenation, add this to the frame number in the file to get the actual frame number
					instance->concatenation_frame_offset_number = *(u32*)data;
				} break;
			}
		} break;
		case 0x0028: {
			switch(element.tag.as_u32) {
				default: break;
				case DICOM_SamplesPerPixel: {
					instance->samples_per_pixel = *(u16*)data;
				} break;
				case DICOM_PhotometricInterpretation: {
					dicom_photometric_interpretation_enum photometric_interpretation = DICOM_PHOTOMETRIC_INTERPRETATION_UNKNOWN;
					static const char* possible_values[] = {"MONOCHROME1", "MONOCHROME2", "PALETTE COLOR", "RGB", "HSV",
															"ARGB", "CMYK", "YBR_FULL", "YBR_FULL_422", "YBR_PARTIAL_422",
															"YBR_PARTIAL_420", "YBR_ICT", "YBR_RCT"};
					ASSERT(COUNT(possible_values) == DICOM_PHOTOMETRIC_INTERPRETATION_YBR_RCT); // enum value equals index+1
					size_t length = dicom_get_element_length_without_trailing_whitespace(data, element.length);
					for (i32 i = 0; i < COUNT(possible_values); ++i) {
						if (strncmp(data_str, possible_values[i], length) == 0) {
							photometric_interpretation = i + 1;
							break;
						}
					}
					if (photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_UNKNOWN) {
						// TODO: decide error checking here or at the end?
						console_print("DICOM: unknown Photometric Interpretation '%s'\n", data_str);
						instance->is_image_invalid = true;
					}
					instance->photometric_interpretation = photometric_interpretation;
				} break;
				case DICOM_PlanarConfiguration: { instance->planar_configuration = *(u16*)data; } break;
				case DICOM_NumberOfFrames: {
					instance->number_of_frames = dicom_parse_integer_string(str, NULL);
				} break;
				case DICOM_Rows:                instance->rows = *(u16*)data; break;
				case DICOM_Columns:             instance->columns = *(u16*)data; break;
				case DICOM_BitsAllocated:       instance->bits_allocated = *(u16*)data; break;
				case DICOM_BitsStored:          instance->bits_stored = *(u16*)data; break;
				case DICOM_HighBit:             instance->high_bit = *(u16*)data; break;
				case DICOM_PixelRepresentation: instance->pixel_representation = *(u16*)data; break;
				case DICOM_BurnedInAnnotation: {

				} break;
				case DICOM_LossyImageCompression: {
					dicom_cs_t cs = dicom_parse_code_string(str, NULL);
					if (strcmp(cs.value, "00") == 0) {
						instance->uncompressed = true;
					}
					// assume compressed otherwise
				} break;
				case DICOM_LossyImageCompressionRatio: {

				} break;
				case DICOM_LossyImageCompressionMethod: {
					dicom_lossy_image_compression_method_enum method = DICOM_LOSSY_IMAGE_COMPRESSION_METHOD_UNKNOWN;
					ASSERT(COUNT(dicom_lossy_image_compression_method_strings) == DICOM_LOSSY_IMAGE_COMPRESSION_METHOD_ISO_23008_2); // enum value equals index+1
					dicom_cs_t cs = dicom_parse_code_string(str, NULL);
					for (i32 i = 0; i < COUNT(dicom_lossy_image_compression_method_strings); ++i) {
						if (strcmp(cs.value, dicom_lossy_image_compression_method_strings[i]) == 0) {
							method = i + 1;
							break;
						}
					}
					instance->lossy_image_compression_method = method;
				} break;
			}
		} break;
	}

	if (instance->sop_class_uid.as_enum == DICOM_VLWholeSlideMicroscopyImageStorage) {
		dicom_wsi_interpret_top_level_data_element(instance, element);
	}
}

static void dicom_interpret_nested_data_element(dicom_instance_t* instance, dicom_data_element_t element) {

	if (instance->sop_class_uid.as_enum == DICOM_VLWholeSlideMicroscopyImageStorage) {
		dicom_wsi_interpret_nested_data_element(instance, element);
	}
}

static void dicom_parser_pop_nesting_level(dicom_instance_t* instance, dicom_parser_pos_t** current_position, dicom_data_element_t* parent_element) {
	// Pop
	--instance->nesting_level;
	--(*current_position);

	instance->nested_sequences[(instance->nesting_level + 1)/ 2].as_u32 = 0;
	// TODO: finalize sequence?

	if (instance->nesting_level == 0) {
		instance->nested_sequences[0].as_u32 = 0;
		instance->nested_item_numbers[0] = 0;
	} else if (instance->nesting_level >= 2 && parent_element->vr == DICOM_VR_SQ) {
		// NOTE: Sequence data elements are always 2 nesting levels apart, in between them will be Items.
//		ASSERT((instance->pos_stack + instance->nesting_level - 1)->element.tag.as_enum == DICOM_Item);
		dicom_data_element_t* parent_sequence = &(instance->pos_stack + instance->nesting_level - 2)->element;

	}
}

bool dicom_read_encapsulated_pixel_data_item(dicom_instance_t* instance, dicom_data_element_t element, dicom_parser_pos_t* current_position, bool known_enough_bytes_left) {
	ASSERT(instance->is_pixel_data_encapsulated);
	ASSERT(instance->number_of_frames > 0);
	if (current_position->item_number == 0) {
		console_print_verbose("Found Basic Offset Table at offset=%d\n", element.data_offset);
		if (known_enough_bytes_left && element.length % 4 == 0) {
			instance->found_pixel_data = true;

			u32 offset_count = element.length / 4;
			if (offset_count > 0) {
				instance->has_basic_offset_table = true;
				instance->are_all_offsets_read = true;
				instance->need_parse_abort = true; // no need to read further elements for now

				// TODO: is it always true that the number of frames equals the number of items in the sequence?
				// https://dicom.nema.org/dicom/2013/output/chtml/part05/sect_A.4.html
				if (offset_count != instance->number_of_frames) {
					console_print_error("DICOM: number of offsets in the basic offset table (%d) does not match number of frames (%d)\n", offset_count, instance->number_of_frames);
					instance->is_image_invalid = true;
					return false;
				}

				instance->pixel_data_offset_count = offset_count;
				instance->pixel_data_start_offset = element.data_offset + element.length;
				ASSERT(instance->pixel_data_offsets == NULL);
				instance->pixel_data_offsets = malloc(offset_count * sizeof(u32));
				ASSERT(offset_count == instance->number_of_frames);
				ASSERT(instance->pixel_data_offsets != NULL);
				ASSERT(offset_count * sizeof(u32) == element.length);
				memcpy(instance->pixel_data_offsets, instance->data + element.data_offset, offset_count * sizeof(u32));
				ASSERT(instance->pixel_data_sizes == NULL);
				instance->pixel_data_sizes = malloc(offset_count * sizeof(u32));
				for (i32 i = 0; i < offset_count - 1; ++i) {
					i64 size = instance->pixel_data_offsets[i + 1] - instance->pixel_data_offsets[i];
					if (size > 0) {
						instance->pixel_data_sizes[i] = size;
					} else {
						instance->pixel_data_sizes[i] = DICOM_UNDEFINED_LENGTH;
						instance->is_image_invalid = true;
					}
				}
				// the last frame size cannot be known, unfortunately, because we have no guarantee
				// that there aren't additional data elements at the end of the file.
				// We can however check that the amount of data is reasonable enough that we could
				// easily read it in one go if we want.
				i64 data_left = instance->total_bytes_in_stream - instance->pixel_data_offsets[offset_count-1];
				if (data_left < MEGABYTES(2)) {
					instance->pixel_data_sizes[offset_count-1] = data_left;
				} else {
					instance->pixel_data_sizes[offset_count-1] = DICOM_UNDEFINED_LENGTH;
				}

			} else {
				// Basic offset table with length 0 means we unfortunately have to parse each item to get the offsets.
				instance->has_basic_offset_table = false;

				// TODO: is it always true that the number of frames equals the number of items in the sequence?
				// https://dicom.nema.org/dicom/2013/output/chtml/part05/sect_A.4.html
				instance->pixel_data_offset_count = instance->number_of_frames;
				instance->pixel_data_start_offset = element.data_offset + element.length;
				ASSERT(instance->pixel_data_offsets == NULL);
				ASSERT(instance->pixel_data_sizes == NULL);
				instance->pixel_data_offsets = calloc(1, instance->pixel_data_offset_count * sizeof(u32));
				instance->pixel_data_sizes = calloc(1, instance->pixel_data_offset_count * sizeof(u32));
				if (!instance->pixel_data_offsets || !instance->pixel_data_sizes) {
					console_print_error("DICOM: could not allocate Pixel Data frame offsets\n");
					instance->is_image_invalid = true;
					return false;
				}

				// We might be lucky if we have already read to the end of the file.
				// In that case, we can continue parsing the rest of the items to fill out the offset table.
				// Otherwise, abort reading further elements (we'll need to read them later).
				if (instance->bytes_read_from_file < instance->total_bytes_in_stream) {
					instance->need_parse_abort = true;
				}
			}

		} else {
			// TODO: handle error condition (malformed DICOM file?)
			console_print_error("DICOM: basic offset table cannot be read or has unexpected length (%u)\n", element.length);
			instance->is_image_invalid = true;
		}
	} else {
		// Pixel Data Item: record offset
		i64 frame_index = current_position->item_number - 1;
//		console_print("DICOM: pixel data for frame %d\n", frame_index);
		ASSERT(instance->pixel_data_offsets);
		ASSERT(frame_index < instance->pixel_data_offset_count);
		if (frame_index < instance->pixel_data_offset_count) {
			// For a DICOM 'Basic Offset Table', the offset is expected point to the first byte of the Item Tag
			// (counting from the start of the Pixel Data data start offset)
			instance->pixel_data_offsets[frame_index] = current_position->offset - instance->pixel_data_start_offset;
			instance->pixel_data_sizes[frame_index] = element.length + (element.data_offset - current_position->offset);
		}
		if (frame_index == instance->number_of_frames - 1) {
			// This is the last frame (we're done).
			instance->are_all_offsets_read = true;
		}
	}
	return true;
}

bool dicom_read_chunk(dicom_instance_t* instance) {

	for (;;) {
		dicom_parser_pos_t* current_position = instance->pos_stack + instance->nesting_level;

		// Check if we reached the end of a sequence or item
		if (instance->nesting_level > 0) {
			dicom_parser_pos_t* parent_position = instance->pos_stack + instance->nesting_level - 1;
			dicom_data_element_t* parent_element = &parent_position->element;
			if (parent_element->length != DICOM_UNDEFINED_LENGTH) {
				if (current_position->offset >= parent_position->offset + parent_element->length) {
					dicom_parser_pop_nesting_level(instance, &current_position, parent_element);

					// Advance position
					current_position->offset = parent_element->data_offset + parent_element->length;
					if (parent_element->tag.as_u32 == DICOM_Item) {

						++current_position->item_number;
						instance->nested_item_numbers[(instance->nesting_level + 1) / 2] = current_position->item_number;
						// TODO: fix hack, do we actually want to know the top level item number??
					}
					continue;
				}
			}
		}

		// instance->data only contains the bytes loaded so far, not the entire logical stream.
		i64 bytes_left = instance->bytes_read_from_file - current_position->offset;
		dicom_data_element_t element = dicom_read_data_element(instance->data, current_position->offset,
		                                                       instance->encoding, bytes_left);

		// Perform checks to prevent going out of bounds
		if (!element.is_valid) {
			return false; // not enough bytes left for the tag/header of this element
		}

		// If the element has a defined length --> guarantee that the data is fully read from file, then interpret the element.
		// If the length is undefined --> we cannot guarantee that the data is fully read, but proceed anyway (cautiously).
		i64 data_bytes_left = instance->bytes_read_from_file - element.data_offset;
		bool known_enough_bytes_left = false;
		if (element.length != DICOM_UNDEFINED_LENGTH) {

			// Special case: start of unencapsulated Pixel Data
			if (instance->nesting_level == 0 && element.tag.as_u32 == DICOM_PixelData) {
				instance->found_pixel_data = true;
				instance->is_pixel_data_encapsulated = false;
				instance->pixel_data = element;
			}

			if (data_bytes_left >= element.length) {
				known_enough_bytes_left = true;
			} else {
				return false; // not enough bytes left to read the data of this element
			}
		}

		current_position->element = element;
		// Switch to a different encoding scheme (Explicit or Implicit VR), if specified in the File Meta info.
		if (element.tag.as_u32 == DICOM_TransferSyntaxUID && known_enough_bytes_left) {
			dicom_switch_data_encoding(instance, &element);
		}

		// Do actions for specific tags
//		ASSERT(dicom_state->tag_handler_func != NULL);
		dicom_series_t* series = instance->series;
		if (series) {
			if (series->tag_handler_func != NULL) {
				// Dump all tags (except Item and ItemDelimitationItem to reduce spam)
				bool want_dump = true;
				if (element.tag.as_u32 == DICOM_ItemDelimitationItem) {
					want_dump = false;
				} else if (element.tag.as_u32 == DICOM_Item || element.tag.as_u32 == DICOM_SequenceDelimitationItem) {
					if (instance->nesting_level > 0) {
						dicom_parser_pos_t* parent_position = instance->pos_stack + instance->nesting_level - 1;
						dicom_data_element_t* parent_element = &parent_position->element;
						if (parent_element->vr == DICOM_VR_SQ) {
							want_dump = false;
						}
					}
				}

				if (want_dump) {
					series->tag_handler_func(series, instance, element);
				}
			}
		}

		if (known_enough_bytes_left && element.vr != DICOM_VR_SQ) {
			if (instance->nesting_level == 0) {
				dicom_interpret_top_level_data_element(instance, element);
			} else {
				dicom_interpret_nested_data_element(instance, element);
			}
		}


		bool need_pop = false;
		bool need_push = false;
		bool need_increment_item_number = false; // for items that are not part of a SQ, e.g. PixelData items

		if (element.tag.as_u32 == DICOM_ItemDelimitationItem || element.tag.as_u32 == DICOM_SequenceDelimitationItem) {
			if (element.tag.as_u32 == DICOM_ItemDelimitationItem) {
				need_increment_item_number = true;
			}
			need_pop = true;
		}

		if (need_pop) {
			//return element.data - start;
			if (instance->nesting_level > 0) {
				dicom_parser_pos_t* parent_position = instance->pos_stack + instance->nesting_level - 1;
				dicom_data_element_t* parent_element = &parent_position->element;

				dicom_parser_pop_nesting_level(instance, &current_position, parent_element);


				// End of sequence was reached
				if (parent_element->length == DICOM_UNDEFINED_LENGTH) {
					// Hack: advance correctly to the next element by retroactively telling what the length was.
					parent_element->length = element.data_offset - parent_element->data_offset;
				}
			} else {
				// TODO: handle error condition
				fatal_error();
			}
		} else {

			if (element.vr == DICOM_VR_SQ) {
				// start of a new sequence
				need_push = true;
				instance->nested_sequences[instance->nesting_level / 2] = element.tag;
			} else if (element.tag.as_u32 == DICOM_Item) {
				if (instance->nesting_level > 0) {
					dicom_parser_pos_t* parent_position = instance->pos_stack + instance->nesting_level - 1;
					dicom_data_element_t* parent_element = &parent_position->element;
					if (parent_element->vr == DICOM_VR_SQ) {
						// This is an item in a sequence -> data contains new data elements -> push is needed
						need_push = true;
					} else {
						// Maybe this is encapsulated pixel data -> no pushing needed
						need_increment_item_number = true;
						if (instance->nesting_level == 1 && parent_element->tag.as_u32 == DICOM_PixelData) {
							dicom_read_encapsulated_pixel_data_item(instance, element, current_position,
							                                        known_enough_bytes_left);

						}
					}
				} else {
					// unknown, does this ever happen?
				}
			} else if (element.tag.as_u32 == DICOM_PixelData) {
				if (element.length == DICOM_UNDEFINED_LENGTH) {
					need_push = true;
					if (instance->nesting_level == 0) {
						instance->is_pixel_data_encapsulated = true;
					}
				}
			}

			if (need_push) {

				dicom_parser_pos_t new_position = {};
				new_position.element_index = 0;
				new_position.offset = element.data_offset;
				if (element.tag.as_u32 == DICOM_Item) {
					new_position.item_number = current_position->item_number;
				} else {
					// Start of a new sequence, so reset the item number
					new_position.item_number = 0;
				}
				instance->pos_stack[instance->nesting_level] = *current_position;
				++instance->nesting_level; // TODO: bounds check
				++current_position;
				*current_position = new_position;
				instance->nested_item_numbers[(instance->nesting_level) / 2] = new_position.item_number;

				continue;
			}
		}

		element = current_position->element;
		if (element.length == DICOM_UNDEFINED_LENGTH) {
			current_position->offset = element.data_offset;
		} else {
			current_position->offset = element.data_offset + element.length;
		}

		++current_position->element_index;
		if (need_increment_item_number) {
			++current_position->item_number;
		}

		bytes_left = instance->total_bytes_in_stream - current_position->offset;
		if (bytes_left < 8) {
			DUMMY_STATEMENT;
			break; // reached end of file
		}

		if (instance->need_parse_abort) {
			instance->need_parse_abort = false;
			return false;
		}
	}
	return true;

}

bool dicom_instance_index_pixel_data(dicom_instance_t* instance) {
    bool success = false;
	if (!instance->is_pixel_data_encapsulated && instance->found_pixel_data) {
		u64 bytes_per_sample = (instance->bits_allocated + 7) / 8;
		u64 frame_size = (u64)instance->columns * instance->rows * instance->samples_per_pixel * bytes_per_sample;
		u64 required_size = frame_size * instance->number_of_frames;
		if (frame_size > 0 && frame_size <= UINT32_MAX && required_size <= instance->pixel_data.length) {
			for (i32 i = 0; i < instance->tile_count; ++i) {
				dicom_tile_t* tile = instance->tiles + i;
				u64 frame_offset = frame_size * tile->frame_index;
				u64 file_offset = sizeof(dicom_header_t) + instance->pixel_data.data_offset + frame_offset;
				if (file_offset > UINT32_MAX) {
					console_print_error("dicom_instance_index_pixel_data(): native Pixel Data offset is too large\n");
					return false;
				}
				tile->data_offset_in_file = (u32)file_offset;
				tile->data_size = (u32)frame_size;
				tile->is_offset_known = true;
			}
			instance->are_all_offsets_read = true;
			success = true;
		} else {
			console_print_error("dicom_instance_index_pixel_data(): invalid native Pixel Data frame size\n");
		}
	} else if (instance->is_pixel_data_encapsulated && !instance->are_all_offsets_read) {

        size_t chunk_size = MEGABYTES(4);
        temp_memory_t temp = begin_temp_memory_on_local_thread();
        void* chunk = arena_push_size(temp.arena, chunk_size);
        size_t filesize = instance->total_bytes_in_stream  + (i64)sizeof(dicom_header_t);

        bool finished = false;
        do {
            dicom_parser_pos_t* current_position = instance->pos_stack + instance->nesting_level;
            i64 file_offset = current_position->element.data_offset + (i64)sizeof(dicom_header_t);
            size_t bytes_left_in_file = filesize - file_offset;
            ASSERT(bytes_left_in_file >= 0);
            size_t bytes_to_read = MIN(chunk_size, bytes_left_in_file);

            size_t bytes_read = file_handle_read_at_offset(chunk, instance->file_handle, file_offset, bytes_to_read);

            if (bytes_read == bytes_to_read) {
                // Hack: trick the parser into thinking the data buffer actually starts at the start of the DICOM stream,
                // even though we are skipping that and really starting where we left off previously
                // NOTE: this means that instance->data now points to an invalid memory location (caution!)
                instance->data = (u8*)chunk - current_position->element.data_offset;
                instance->bytes_read_from_file = current_position->element.data_offset + (i64)bytes_to_read;

                finished = dicom_read_chunk(instance);

            } else {
                console_print_error("dicom_instance_index_pixel_data(): error reading DICOM instance from file\n");
                break;
            }
        } while (!finished);
        success = finished;

        release_temp_memory(&temp);
    } else if (instance->is_pixel_data_encapsulated && instance->are_all_offsets_read) {
		success = true;
	}

    if (!instance->are_all_offsets_read) {
        console_print_error("dicom_instance_index_pixel_data(): frame offsets could not be read\n");
        success = false;
    }

    // update tiles
    if (success) {
        for (i32 i = 0; i < instance->tile_count; ++i) {
            dicom_tile_t* tile = instance->tiles + i;
            tile->data_offset_in_file = sizeof(dicom_header_t) + instance->pixel_data_start_offset + instance->pixel_data_offsets[tile->frame_index];
            tile->data_size = instance->pixel_data_sizes[tile->frame_index];
            tile->is_offset_known = true;
        }
    }

    return success;
}

dicom_instance_t dicom_load_file(dicom_series_t* dicom_series, file_info_t* file) {

	dicom_instance_t instance = {};
	instance.series = dicom_series;

	// TODO: pipe all debug output to private memrw_t for dumping it onto the console later (enable concurrent loading?)

	file_stream_t fp = file_stream_open_for_reading(file->full_filename);
	if (fp) {
		copy_cstring(instance.filename, file->full_filename, sizeof(instance.filename));
		size_t chunk_size = KILOBYTES(64);
		size_t bytes_to_read = MIN(chunk_size, file->filesize);

		if (file->filesize > sizeof(dicom_header_t)) {
			memrw_t buffer = memrw_create(bytes_to_read);
			i64 bytes_read = file_stream_read(buffer.data, (size_t)bytes_to_read, fp);
			buffer.used_size = bytes_read;

			if (bytes_read == bytes_to_read) {
				if (is_file_a_dicom_file(buffer.data, bytes_read)) {
					console_print_verbose("Found DICOM file: '%s'\n", file->full_filename);
					i64 payload_offset = sizeof(dicom_header_t);
					if (dicom_series->debug_output_file) {
						fprintf(dicom_series->debug_output_file, "\nFile: %s\n\n", file->full_filename);
					}
					i64 payload_bytes = ((i64)buffer.used_size - payload_offset);
					ASSERT(payload_bytes > 0);
					i64 total_bytes_in_stream = file->filesize - payload_offset;
					ASSERT(total_bytes_in_stream >= payload_bytes);

					instance.encoding = DICOM_TRANSFER_SYNTAX_EXPLICIT_VR_LITTLE_ENDIAN;
					instance.data = buffer.data + payload_offset;
					instance.bytes_read_from_file = bytes_read - payload_offset;
					instance.total_bytes_in_stream = file->filesize - payload_offset;

					// Read the DICOM file in chunks, until we hit the PixelData tag (which should hopefully have a
					// basic offset table as well for seeking/random access).
					// This way we don't need to read the whole file initially.
					for(;;) {
						bool finished = dicom_read_chunk(&instance);

						bool stop_reading = finished;
						if (!finished) {
							if (instance.found_pixel_data) {
								if (!instance.is_pixel_data_encapsulated) {
									stop_reading = true;
								} else {
									if (instance.are_all_offsets_read) {
										stop_reading = true;
									} else {
										ASSERT(!instance.has_basic_offset_table);
										// There is no offset table, and we haven't read enough data yet to know all of the frame offsets.
										// We can choose to read to the end of the file immediately if it isn't too large.
										i64 bytes_left_to_read = instance.total_bytes_in_stream - instance.bytes_read_from_file;
										i64 reasonable_size_to_read_immediately = MEGABYTES(4);
										if (bytes_left_to_read < reasonable_size_to_read_immediately) {
											chunk_size = bytes_left_to_read;
											// continue reading
										} else {
											stop_reading = true;
										}
									}
								}
							}
						}

						if (stop_reading) {
							break; // done
						}

						// Read an additional chunk
						i64 bytes_left_in_file = instance.total_bytes_in_stream - instance.bytes_read_from_file;
						if (bytes_left_in_file <= 0) {
							// TODO: handle error condition
						}
						ASSERT(bytes_left_in_file > 0 && bytes_left_in_file < instance.total_bytes_in_stream);
						bytes_to_read = MIN(chunk_size, bytes_left_in_file);
						memrw_maybe_grow(&buffer, buffer.used_size + bytes_to_read);
						bytes_read = file_stream_read(buffer.data + buffer.used_size, (size_t)bytes_to_read, fp);
						buffer.used_size += bytes_read;
						buffer.cursor += bytes_read;
						instance.data = buffer.data + payload_offset; // need to update the pointer, maybe it moved due to resizing
						instance.bytes_read_from_file += bytes_read;

						bytes_left_in_file = instance.total_bytes_in_stream - instance.bytes_read_from_file;
						if (bytes_left_in_file <= 0) {
							DUMMY_STATEMENT;
						}

					}
					instance.is_valid = !instance.is_image_invalid; // TODO: what to do here?


				}
			}

			memrw_destroy(&buffer);
			instance.data = NULL;
		}
	}
	file_stream_close(fp);
	return instance;
}

typedef struct indexed_value_t {
	i64 value;
	i32 index;
} indexed_value_t;

// for qsort
static int compare_indexed_value (const void* a, const void* b) {
	return (int)( ((indexed_value_t*)b)->value - ((indexed_value_t*)a)->value );
}

static bool dicom_uids_equal(const dicom_ui_t* a, const dicom_ui_t* b) {
	return a->len == b->len && a->len > 0 && memcmp(a->value, b->value, a->len) == 0;
}

static bool dicom_instance_is_in_selected_slide(const dicom_series_t* dicom, const dicom_instance_t* instance) {
	if (dicom->selected_slide_index < 0 || dicom->selected_slide_index >= arrlen(dicom->slides)) return false;
	return dicom_uids_equal(&instance->general_series.SeriesInstanceUID,
	                        &dicom->slides[dicom->selected_slide_index].series_instance_uid);
}

static bool dicom_instances_are_same_level(const dicom_instance_t* a, const dicom_instance_t* b) {
	if (a->total_pixel_matrix_columns != b->total_pixel_matrix_columns ||
	    a->total_pixel_matrix_rows != b->total_pixel_matrix_rows) {
		return false;
	}
	if (a == b) return true;
	return dicom_uids_equal(&a->concatenation_uid, &b->concatenation_uid);
}

static int compare_dicom_slides(const void* a, const void* b) {
	const dicom_slide_t* slide_a = (const dicom_slide_t*)a;
	const dicom_slide_t* slide_b = (const dicom_slide_t*)b;
	return strcmp(slide_a->representative_filename, slide_b->representative_filename);
}

static bool dicom_paths_equal(const char* a, const char* b) {
	while (*a || *b) {
		bool a_is_separator = *a == '/' || *a == '\\';
		bool b_is_separator = *b == '/' || *b == '\\';
		if (a_is_separator || b_is_separator) {
			if (!a_is_separator || !b_is_separator) return false;
			while (*a == '/' || *a == '\\') ++a;
			while (*b == '/' || *b == '\\') ++b;
			continue;
		}
		#if WINDOWS
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
		#else
		if (*a != *b) return false;
		#endif
		++a;
		++b;
	}
	return true;
}

static void dicom_load_instances_from_directory_tree(dicom_series_t* dicom, directory_info_t* directory) {
	for (i32 i = 0; i < arrlen(directory->dicom_files); ++i) {
		dicom_instance_t instance = dicom_load_file(dicom, directory->dicom_files + i);
		if (instance.is_valid) {
			arrput(dicom->instances, instance);
		}
	}
	for (i32 i = 0; i < arrlen(directory->directories); ++i) {
		dicom_load_instances_from_directory_tree(dicom, directory->directories + i);
	}
}

bool dicom_open_from_directory(dicom_series_t* dicom, directory_info_t* directory, const file_info_t* selected_file) {
	i64 start = get_clock();

	#if DO_DEBUG
	if (is_verbose_mode) {
		dicom->debug_output_file = fopen("dicom_dump.txt", "wb");
	}
	#endif
	dicom->tag_handler_func = is_verbose_mode ? handle_dicom_tag_for_tag_dumping : NULL;

	bool success = true;
	dicom_load_instances_from_directory_tree(dicom, directory);

	if (dicom->debug_output_file) {
		fclose(dicom->debug_output_file);
	}

	console_print("DICOM: series has %d instances\n", arrlen(dicom->instances));

	// TODO: move much of this to dicom_wsi.c

	dicom->selected_slide_index = -1;
	dicom_ui_t selected_series_instance_uid = {};
	for (i32 i = 0; i < arrlen(dicom->instances); ++i) {
		dicom_instance_t* instance = dicom->instances + i;
		if (selected_file && dicom_paths_equal(instance->filename, selected_file->full_filename)) {
			selected_series_instance_uid = instance->general_series.SeriesInstanceUID;
		}
		if (instance->image_flavor == DICOM_IMAGE_FLAVOR_VOLUME) {
			i32 slide_index = -1;
			for (i32 j = 0; j < arrlen(dicom->slides); ++j) {
				if (dicom_uids_equal(&dicom->slides[j].series_instance_uid,
				                    &instance->general_series.SeriesInstanceUID)) {
					slide_index = j;
					break;
				}
			}
			if (slide_index < 0) {
				dicom_slide_t slide = {};
				slide.series_instance_uid = instance->general_series.SeriesInstanceUID;
				copy_cstring(slide.representative_filename, instance->filename, sizeof(slide.representative_filename));
				slide.representative_width = instance->total_pixel_matrix_columns;
				arrput(dicom->slides, slide);
				slide_index = arrlen(dicom->slides) - 1;
			} else if (instance->total_pixel_matrix_columns > dicom->slides[slide_index].representative_width) {
				copy_cstring(dicom->slides[slide_index].representative_filename, instance->filename,
				             sizeof(dicom->slides[slide_index].representative_filename));
				dicom->slides[slide_index].representative_width = instance->total_pixel_matrix_columns;
			}
		}

		console_print_verbose("%d: #=%d flavor=%s w=%u h=%u\n", i, instance->instance_number, instance->image_flavor_cs.value,
		              instance->total_pixel_matrix_columns, instance->total_pixel_matrix_rows);
	}
	qsort(dicom->slides, arrlen(dicom->slides), sizeof(dicom_slide_t), compare_dicom_slides);
	for (i32 i = 0; i < arrlen(dicom->slides); ++i) {
		if (dicom_uids_equal(&dicom->slides[i].series_instance_uid, &selected_series_instance_uid)) {
			dicom->selected_slide_index = i;
			break;
		}
	}
	if (dicom->selected_slide_index < 0 && arrlen(dicom->slides) > 0) {
		dicom->selected_slide_index = 0;
	}
	console_print("DICOM: directory contains %d slide(s), selecting slide %d\n",
	              arrlen(dicom->slides), dicom->selected_slide_index + 1);

	// Sort levels (volumes) by descending image width to get the
	indexed_value_t* volume_image_widths = alloca(arrlen(dicom->instances) * sizeof(indexed_value_t));
	i32 running_volume_index = 0;
	for (i32 i = 0; i < arrlen(dicom->instances); ++i) {
		dicom_instance_t* instance = dicom->instances + i;
		if (instance->image_flavor == DICOM_IMAGE_FLAVOR_VOLUME &&
		    dicom_instance_is_in_selected_slide(dicom, instance)) {
			i64 pixel_count = instance->total_pixel_matrix_columns;
			volume_image_widths[running_volume_index++] = (indexed_value_t){pixel_count, i};
		}
	}
	i32 volume_count = running_volume_index;
	qsort(volume_image_widths, volume_count, sizeof(indexed_value_t), compare_indexed_value);

	// Fold concatenation parts at the same matrix dimensions into one logical level.
	indexed_value_t unique_volume_image_widths[16] = {};
	i32 unique_volume_count = 0;
	for (i32 i = 0; i < volume_count; ++i) {
		dicom_instance_t* instance = dicom->instances + volume_image_widths[i].index;
		bool already_present = false;
		for (i32 j = 0; j < unique_volume_count; ++j) {
			dicom_instance_t* existing = dicom->instances + unique_volume_image_widths[j].index;
			if (existing->total_pixel_matrix_columns == instance->total_pixel_matrix_columns &&
			    existing->total_pixel_matrix_rows == instance->total_pixel_matrix_rows) {
				if (!dicom_instances_are_same_level(existing, instance)) {
					console_print_error("DICOM: multiple non-concatenated volume instances have the same dimensions\n");
					return false;
				}
				already_present = true;
				break;
			}
		}
		if (!already_present) {
			if (unique_volume_count >= COUNT(unique_volume_image_widths)) {
				console_print_error("DICOM: slide has too many pyramid levels\n");
				return false;
			}
			unique_volume_image_widths[unique_volume_count++] = volume_image_widths[i];
		}
	}
	dicom->wsi.instance_count = unique_volume_count;
	for (i32 i = 0; i < unique_volume_count; ++i) {
		i32 instance_index = unique_volume_image_widths[i].index;
		dicom_instance_t* instance = dicom->instances + instance_index;
		dicom->wsi.level_instances[i] = instance;
		console_print("level %d: #=%d w=%u h=%u\n", i, instance_index, instance->total_pixel_matrix_columns, instance->total_pixel_matrix_rows);
	}

	if (dicom->wsi.instance_count <= 0) {
		console_print_error("DICOM: selected slide contains no volume instances\n");
		return false;
	}
	dicom_instance_t* base_level_instance = dicom->wsi.level_instances[0];
	ASSERT(base_level_instance);
	if (base_level_instance) {
		if (base_level_instance->pixel_spacing.x > 0 && base_level_instance->pixel_spacing.y > 0) {
			dicom->wsi.is_mpp_known = true;
			dicom->wsi.mpp_x = base_level_instance->pixel_spacing.x * 1000.0f;
			dicom->wsi.mpp_y = base_level_instance->pixel_spacing.y * 1000.0f;
		}
	} else {
		// TODO: handle error condition
	}


	// Set up tiles
	for (i32 i = 0; i < dicom->wsi.instance_count; ++i) {
		dicom_instance_t* instance = dicom->wsi.level_instances[i];

        ASSERT(instance->tiles == NULL);
		instance->width_in_tiles = (instance->total_pixel_matrix_columns + instance->columns - 1) / instance->columns;
		instance->height_in_tiles = (instance->total_pixel_matrix_rows + instance->rows - 1) / instance->rows;
		instance->tile_count = instance->width_in_tiles * instance->height_in_tiles;
		instance->tiles = calloc(instance->tile_count, sizeof(dicom_tile_t));

		for (i32 part_index = 0; part_index < arrlen(dicom->instances); ++part_index) {
			dicom_instance_t* part = dicom->instances + part_index;
			if (!dicom_instance_is_in_selected_slide(dicom, part) ||
			    part->image_flavor != DICOM_IMAGE_FLAVOR_VOLUME ||
			    !dicom_instances_are_same_level(instance, part)) {
				continue;
			}

			if(arrlen(part->per_frame_plane_position_slide) > 0) {
				for (i32 frame_index = 0; frame_index < arrlen(part->per_frame_plane_position_slide); ++frame_index) {
					dicom_plane_position_slide_t* plane_position = part->per_frame_plane_position_slide + frame_index;
					i32 tile_x = (plane_position->column_position_in_total_image_pixel_matrix - 1) / part->columns;
					i32 tile_y = (plane_position->row_position_in_total_image_pixel_matrix - 1) / part->rows;
					if (tile_x < 0 || tile_x >= instance->width_in_tiles ||
					    tile_y < 0 || tile_y >= instance->height_in_tiles) continue;
					dicom_tile_t* tile = instance->tiles + tile_y * instance->width_in_tiles + tile_x;
					tile->exists = true;
					tile->instance = part;
					tile->frame_index = frame_index;
					if (part->pixel_data_offsets && part->pixel_data_sizes && part->are_all_offsets_read) {
						// TODO: bounds check
						tile->data_offset_in_file = sizeof(dicom_header_t) + part->pixel_data_start_offset + part->pixel_data_offsets[frame_index];
						tile->data_size = part->pixel_data_sizes[frame_index];
						tile->is_offset_known = true;
					}
				}
			} else {
				// We don't have tile position information -> guess that all tiles are present in the logical order
				for (i32 frame_index = 0; frame_index < part->number_of_frames; ++frame_index) {
					i32 logical_frame_index = part->concatenation_frame_offset_number + frame_index;
					if (logical_frame_index < 0 || logical_frame_index >= instance->tile_count) continue;
					dicom_tile_t* tile = instance->tiles + logical_frame_index;
					tile->exists = true;
					tile->instance = part;
					tile->frame_index = frame_index;
					if (part->pixel_data_offsets && part->pixel_data_sizes && part->are_all_offsets_read) {
						// TODO: bounds check
						tile->data_offset_in_file = sizeof(dicom_header_t) + part->pixel_data_start_offset + part->pixel_data_offsets[frame_index];
						tile->data_size = part->pixel_data_sizes[frame_index];
						tile->is_offset_known = true;
					}
				}
			}
		}
	}

	// Deduce downsample factors from the level dimensions.
	// NOTE: also see tiff_post_init()
	i32 last_downsample_level = 0;
	for (i32 i = 0; i < dicom->wsi.instance_count; ++i) {
		dicom_instance_t* instance = dicom->wsi.level_instances[i];
		if (instance->tile_count == 0) {
			ASSERT(!"level has no tiles"); // Sanity check: there should always be at least one tile
			break;
		}

		float level_width = (float)instance->total_pixel_matrix_columns;
		float raw_downsample_factor = base_level_instance->total_pixel_matrix_columns / level_width;
		float raw_downsample_level = log2f(raw_downsample_factor);
		i32 downsample_level = (i32) roundf(raw_downsample_level);

		// Some TIFF files have the width/height set to an integer multiple of the tile size.
		// For the most zoomed out levels, this makes it harder to calculate the actual downsampling level
		// (because we might underestimate it). So we need to do extra work to deduce the downsampling
		// level in these corner cases.
		if (instance->total_pixel_matrix_columns % instance->columns == 0) {
			if (instance->width_in_tiles >= 1 && instance->height_in_tiles >= 1) {
				u32 min_possible_width = instance->columns * (instance->width_in_tiles-1) + 1;
				u32 max_possible_width = instance->columns * (instance->width_in_tiles) ;
				float downsample_factor_upper_bound = base_level_instance->total_pixel_matrix_columns / (float)min_possible_width;
				float downsample_factor_lower_bound = base_level_instance->total_pixel_matrix_columns / (float)max_possible_width;

				if (instance->total_pixel_matrix_rows % instance->rows == 0) {
					// constrain further based on the vertical tile count
					u32 min_possible_height = instance->rows * (instance->height_in_tiles-1) + 1;
					u32 max_possible_height = instance->rows * (instance->height_in_tiles);

					float downsample_factor_y_upper_bound = base_level_instance->total_pixel_matrix_rows / (float)min_possible_height;
					float downsample_factor_y_lower_bound = base_level_instance->total_pixel_matrix_rows / (float)max_possible_height;

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

		instance->downsample_level = last_downsample_level = downsample_level;
		instance->downsample_factor = exp2f((float)instance->downsample_level);
		dicom->wsi.max_downsample_level = MAX(instance->downsample_level, dicom->wsi.max_downsample_level);
	}

	// Reopen files for simultaneous access
	for (i32 i = 0; i < arrlen(dicom->instances); ++i) {
		dicom_instance_t* instance = dicom->instances + i;
		instance->file_handle = open_file_handle_for_simultaneous_access(instance->filename);
		if (!instance->file_handle) {
			console_print_error("Error: Could not reopen file for asynchronous I/O: '%s'\n", instance->filename);
			success = false;
		}
	}

	console_print("DICOM parsing took %g seconds\n", get_seconds_elapsed(start, get_clock()));
	return success;
}

bool dicom_open_from_file(dicom_series_t* dicom, file_info_t* file) {
	i64 start = get_clock();

	#if DO_DEBUG
	if (is_verbose_mode) {
		dicom->debug_output_file = fopen("dicom_dump.txt", "wb");
	}
	#endif
	dicom->tag_handler_func = is_verbose_mode ? handle_dicom_tag_for_tag_dumping : NULL;

	dicom_load_file(dicom, file);

	if (dicom->debug_output_file) {
		fclose(dicom->debug_output_file);
	}

	console_print("DICOM parsing took %g seconds\n", get_seconds_elapsed(start, get_clock()));
	return true;
}

bool is_file_a_dicom_file(u8* file_header_data, size_t file_header_data_len) {
	if (file_header_data_len > sizeof(dicom_header_t)) {
		dicom_header_t* dicom_header = (dicom_header_t*) file_header_data;
		u32 prefix = *(u32*) dicom_header->prefix;
		if (prefix == LE_4CHARS('D','I','C','M')) {
			return true;
		}
	}
	return false;
}


static u16 dicom_vr_tbl[] = {
	0, // undefined
	DICOM_VR_AE,
	DICOM_VR_AS,
	DICOM_VR_AT,
	DICOM_VR_CS,
	DICOM_VR_DA,
	DICOM_VR_DS,
	DICOM_VR_DT,
	DICOM_VR_FD,
	DICOM_VR_FL,
	DICOM_VR_IS,
	DICOM_VR_LO,
	DICOM_VR_LT,
	DICOM_VR_OB,
	DICOM_VR_OD,
	DICOM_VR_OF,
	DICOM_VR_OL,
	DICOM_VR_OV,
	DICOM_VR_OW,
	DICOM_VR_PN,
	DICOM_VR_SH,
	DICOM_VR_SL,
	DICOM_VR_SQ,
	DICOM_VR_SS,
	DICOM_VR_ST,
	DICOM_VR_SV,
	DICOM_VR_TM,
	DICOM_VR_UC,
	DICOM_VR_UI,
	DICOM_VR_UL,
	DICOM_VR_UN,
	DICOM_VR_UR,
	DICOM_VR_US,
	DICOM_VR_UT,
	DICOM_VR_UV,
};

void dicom_dict_init_hash_table() {
	ASSERT(dicom_dict_entries);
	dicom_dict_hash_table_size = next_pow2(COUNT(dicom_dict_packed_entries) * 4);
	dicom_dict_hash_table = (dicom_dict_entry_t*)calloc(1, dicom_dict_hash_table_size * sizeof(dicom_dict_entry_t));
	i32 collision_count = 0;
	i32 extra_lookup_count = 0;
	for (i32 i = 0; i < COUNT(dicom_dict_packed_entries); ++i) {
		dicom_dict_entry_t entry = dicom_dict_entries[i];
		u32 hash = lowbias32(entry.tag);
		u32 index = hash % dicom_dict_hash_table_size;
		dicom_dict_entry_t* slot = dicom_dict_hash_table + index;
		if (slot->tag == 0) {
			// empty slot!
			*slot = entry;
		} else {
			++collision_count;
			i32 cluster_size = 1;
			bool resolved = false;
			for (i32 j = 1; j < dicom_dict_hash_table_size; ++j) {
				// Use linear probing for collision resolution
				i32 new_index = (index + (j)) % dicom_dict_hash_table_size;
				slot = dicom_dict_hash_table + new_index;
				if (slot->tag == 0) {
					*slot = entry;
					resolved = true;
					break; // resolved
				} else {
					++cluster_size;
//					console_print("Hash table collision: extra lookups %d\n", cluster_size);
				}
			}
			if (!resolved) {
				fatal_error();
			}
			extra_lookup_count += cluster_size;
		}
	}
	console_print_verbose("Hash table size: %d entries: %d (load factor %.2f) collisions: %d extra lookups: %d", dicom_dict_hash_table_size,
				  COUNT(dicom_dict_packed_entries),
				  (float)COUNT(dicom_dict_packed_entries)/(float)dicom_dict_hash_table_size,
				  collision_count, extra_lookup_count);
}

bool dicom_unpack_and_decompress_dictionary() {
	// Unpack dictionary entries, these have been packed to take up ~0.5x space
	// - convert 1-byte name/keyword lengths back into 4-byte offsets into the string pool
	// - convert 1-byte VR lookup indices back into 2-byte VR codes as used in DICOM streams
	// (this packed data is not much further compressible using LZ4, and is therefore left uncompressed)
	dicom_dict_entry_t* unpacked_entries = (dicom_dict_entry_t*) malloc(COUNT(dicom_dict_packed_entries) * sizeof(dicom_dict_entry_t));
	u32 running_offset = 1; // the first byte is a '\0' character (for the null / empty string), so start at offset=1
	for (i32 i = 0; i < COUNT(dicom_dict_packed_entries); ++i) {
		dicom_dict_packed_entry_t packed_entry = dicom_dict_packed_entries[i];
		dicom_dict_entry_t entry = {};
		entry.tag = packed_entry.tag;
		entry.name_offset = running_offset;
		running_offset += packed_entry.name_len + 1; // one extra byte for the '\0'
		entry.keyword_offset = running_offset;
		running_offset += packed_entry.keyword_len + 1;
		ASSERT(running_offset <= DICOM_DICT_STRING_POOL_UNCOMPRESSED_SIZE);
		entry.vr = dicom_vr_tbl[packed_entry.vr_index];
		unpacked_entries[i] = entry;
	}
	dicom_dict_entries = unpacked_entries;

	// LZ4-decompress string pool, which contains the DICOM tag names and keywords
	i32 decompressed_size = DICOM_DICT_STRING_POOL_UNCOMPRESSED_SIZE;
	i32 compressed_size = DICOM_DICT_STRING_POOL_COMPRESSED_SIZE;
	u8* decompressed = (u8*) malloc(decompressed_size);
	i32 bytes_decompressed = LZ4_decompress_safe((char*)dicom_dict_string_pool_lz4_compressed, (char*)decompressed, compressed_size, decompressed_size);
	if (bytes_decompressed <= 0) {
		console_print_error("LZ4_decompress_safe() failed (return value %d)\n", bytes_decompressed);
		free(decompressed);
		return false;
	} else {
		if (bytes_decompressed != decompressed_size) {
			console_print_error("LZ4_decompress_safe() decompressed %d bytes, however the expected size was %d\n", bytes_decompressed, decompressed_size);
			free(decompressed);
			return false;
		} else {
			dicom_dict_string_pool = (const char*)decompressed;
			return true;
		}
	}
}

bool dicom_init() {
	i64 start = get_clock();
	bool success = dicom_unpack_and_decompress_dictionary();
	if (success) {
		dicom_dict_init_hash_table();
		console_print_verbose("Initialized DICOM dictionary in %g seconds.\n", get_seconds_elapsed(start, get_clock()));
	}
	return success;
}

void dicom_instance_destroy(dicom_instance_t* instance) {
	// TODO: refcount? how to do this?
	while (instance->refcount > 0) {
//		console_print_error("refcount = %d\n", isyntax->refcount);
		platform_sleep(1);
		thread_pool_do_work(&global_thread_pool);
	}
    // TODO: use pool/arena allocator so we can free everything in one go?
	if (instance->pixel_data_offsets) free(instance->pixel_data_offsets);
	if (instance->pixel_data_sizes) free(instance->pixel_data_sizes);
	if (instance->tiles) free(instance->tiles);
	arrfree(instance->per_frame_plane_position_slide);
    for (i32 i = 0; i < arrlen(instance->optical_paths); ++i) {
        dicom_optical_path_t* optical_path = instance->optical_paths + i;
        if (optical_path->icc_profile) free(optical_path->icc_profile);
    }
    arrfree(instance->optical_paths);
	if (instance->file_handle) file_handle_close(instance->file_handle);
}

void dicom_destroy(dicom_series_t* dicom_series) {
	for (i32 i = 0; i < arrlen(dicom_series->instances); ++i) {
		dicom_instance_t* instance = dicom_series->instances + i;
		dicom_instance_destroy(instance);
	}
    arrfree(dicom_series->instances);
    arrfree(dicom_series->slides);
}

// Undo the encapsulation of encoded pixel data
// See: https://dicom.nema.org/medical/dicom/current/output/html/part05.html#sect_A.4

i64 dicom_defragment_encapsulated_pixel_data_frame(u8* data, i64 len) {
	i64 current_src_offset = 0;
	i64 current_dest_offset = 0;
	for(;;) {
		i64 bytes_available = len - current_src_offset;
		dicom_data_element_t element = dicom_read_data_element(data, current_src_offset, DICOM_TRANSFER_SYNTAX_IMPLICIT_VR_LITTLE_ENDIAN, bytes_available);
		if (element.tag.as_u32 != DICOM_Item) {
			if (element.tag.as_u32 == DICOM_SequenceDelimitationItem) {
				break; // end reached
			} else {
				return -1; // invalid tag
			}
		}
		i64 data_offset = current_src_offset + element.data_offset;
		i64 data_bytes_available = len - data_offset;
		if (element.length > data_bytes_available) {
			return -1;
		}
		// Strip element header from the start, move data from this fragment back
		memmove(data + current_dest_offset, data + element.data_offset, element.length);

		current_src_offset = data_offset + element.length;
		current_dest_offset += element.length;

		if (current_src_offset >= len) {
			break; // end reached
		}
	}
	return current_dest_offset; // return size of defragmented data
}

#if 0
void dicom_read_pixel_data_offsets(dicom_instance_t* instance) {
	if (instance->is_pixel_data_encapsulated && instance->found_pixel_data) {

		if (instance->file_handle) {
			i64 chunk_size = MEGABYTES(4);
			temp_memory_t temp = begin_temp_memory_on_local_thread();

			size_t bytes_to_read = MIN(bytes_left_in_file, chunk_size);
			file_handle_read_at_offset(chunk, instance->file_handle, offset, bytes_to_read);

			release_temp_memory(&temp);
		}
	}
}
#endif
