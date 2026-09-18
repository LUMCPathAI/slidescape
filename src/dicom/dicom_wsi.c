/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2025  Pieter Valkema

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

#include "common.h"
#include "dicom.h"
#include "dicom_wsi.h"

#include "jpeg_decoder.h"
#include "jpeg2000_decoder.h"

// Returns either &array[index] if it already exists, or a newly added and zeroed element at the end of the array
#define array_last_maybe_expand(array, index) \
  (((arrlen((array)) <= (index))              \
       ? (arraddn((array), 1), memset((array) + (index), 0, sizeof((array)[0])), 0)         \
       : 0), \
   (array) + (index))

static void dicom_wsi_parse_code_element(dicom_code_t* code,
		dicom_data_element_t element, str_t value) {
	if (!code) return;
	switch (element.tag.as_u32) {
		case DICOM_CodeValue:
			code->CodeValue = dicom_parse_short_string(value);
			break;
		case DICOM_CodingSchemeDesignator:
			code->CodingSchemeDesignator = dicom_parse_short_string(value);
			break;
		case DICOM_CodeMeaning:
			code->CodeMeaning = dicom_parse_long_string(value);
			break;
		default:
			break;
	}
}

static bool dicom_wsi_content_sequence_tag(u32 tag) {
	return tag == DICOM_SpecimenPreparationStepContentItemSequence ||
	       tag == DICOM_ContentSequence;
}

static char* dicom_wsi_copy_content_string(const u8* data, u32 size) {
	while (size && (data[size - 1] == ' ' || data[size - 1] == '\0')) --size;
	if ((u64)size + 1 > SIZE_MAX) return NULL;
	char* result = malloc((size_t)size + 1);
	if (!result) return NULL;
	memcpy(result, data, size);
	result[size] = '\0';
	return result;
}

static void dicom_wsi_replace_content_string(
		char** destination, const u8* data, u32 size) {
	char* replacement = dicom_wsi_copy_content_string(data, size);
	if (!replacement) return;
	free(*destination);
	*destination = replacement;
}

static i32 dicom_wsi_find_sequence_level(
		const dicom_instance_t* instance, u32 tag, i32 before_level) {
	i32 start = MIN(before_level, instance->nesting_level - 1);
	for (i32 level = start; level >= 0; --level) {
		if (instance->pos_stack[level].element.vr == DICOM_VR_SQ &&
		    instance->pos_stack[level].element.tag.as_u32 == tag) return level;
	}
	return -1;
}

static dicom_specimen_preparation_item_t*
dicom_wsi_get_specimen_preparation_item(dicom_instance_t* instance) {
	i32 specimen_level = dicom_wsi_find_sequence_level(
		instance, DICOM_SpecimenDescriptionSequence, instance->nesting_level);
	i32 preparation_level = dicom_wsi_find_sequence_level(
		instance, DICOM_SpecimenPreparationSequence, instance->nesting_level);
	if (specimen_level < 0 || preparation_level <= specimen_level ||
	    specimen_level + 1 >= instance->nesting_level ||
	    preparation_level + 1 >= instance->nesting_level) return NULL;
	u32 specimen_index =
		(u32)instance->pos_stack[specimen_level + 1].item_number;
	u32 step_index =
		(u32)instance->pos_stack[preparation_level + 1].item_number;
	dicom_specimen_t* specimen =
		array_last_maybe_expand(instance->specimens, specimen_index);
	dicom_specimen_preparation_step_t* step =
		array_last_maybe_expand(specimen->preparation_steps, step_index);
	u32 parent_index = UINT32_MAX;
	dicom_specimen_preparation_item_t* current = NULL;
	for (i32 level = preparation_level + 2;
	     level < instance->nesting_level; ++level) {
		if (instance->pos_stack[level].element.vr != DICOM_VR_SQ ||
		    !dicom_wsi_content_sequence_tag(
			    instance->pos_stack[level].element.tag.as_u32) ||
		    level + 1 >= instance->nesting_level) continue;
		u32 item_number =
			(u32)instance->pos_stack[level + 1].item_number;
		current = NULL;
		for (i32 i = 0; i < arrlen(step->items); ++i) {
			dicom_specimen_preparation_item_t* candidate = step->items + i;
			if (candidate->parent_index == parent_index &&
			    candidate->parser_item_number == item_number &&
			    candidate->parser_sequence_level == (u16)level) {
				current = candidate;
				break;
			}
		}
		if (!current) {
			dicom_specimen_preparation_item_t item = {0};
			item.parent_index = parent_index;
			item.parser_item_number = item_number;
			item.parser_sequence_level = (u16)level;
			arrput(step->items, item);
			current = step->items + arrlen(step->items) - 1;
		}
		parent_index = (u32)(current - step->items);
	}
	return current;
}

static dicom_content_code_t* dicom_wsi_content_code_for_element(
		dicom_instance_t* instance,
		dicom_specimen_preparation_item_t* item) {
	for (i32 level = instance->nesting_level - 1; level >= 0; --level) {
		if (instance->pos_stack[level].element.vr != DICOM_VR_SQ) continue;
		switch (instance->pos_stack[level].element.tag.as_u32) {
			case DICOM_ConceptNameCodeSequence:
				return &item->concept_name;
			case DICOM_ConceptCodeSequence:
				return &item->code_value;
			case DICOM_MeasurementUnitsCodeSequence:
				return &item->measurement_units;
			default:
				break;
		}
	}
	return NULL;
}

static void dicom_wsi_parse_specimen_preparation_element(
		dicom_instance_t* instance, dicom_data_element_t element) {
	dicom_specimen_preparation_item_t* item =
		dicom_wsi_get_specimen_preparation_item(instance);
	if (!item) return;
	u8* data = instance->data + element.data_offset;
	if (element.tag.as_u32 == DICOM_ValueType) {
		char value_type[17] = {0};
		u32 size = MIN(element.length, (u32)sizeof(value_type) - 1);
		memcpy(value_type, data, size);
		while (size && (value_type[size - 1] == ' ' ||
		                value_type[size - 1] == '\0'))
			value_type[--size] = '\0';
		if (strcmp(value_type, "CODE") == 0)
			item->value_type = DICOM_CONTENT_VALUE_CODE;
		else if (strcmp(value_type, "TEXT") == 0)
			item->value_type = DICOM_CONTENT_VALUE_TEXT;
		else if (strcmp(value_type, "NUM") == 0)
			item->value_type = DICOM_CONTENT_VALUE_NUM;
		else if (strcmp(value_type, "UIDREF") == 0)
			item->value_type = DICOM_CONTENT_VALUE_UID;
		else if (strcmp(value_type, "DATETIME") == 0)
			item->value_type = DICOM_CONTENT_VALUE_DATETIME;
		else if (strcmp(value_type, "DATE") == 0)
			item->value_type = DICOM_CONTENT_VALUE_DATE;
		else if (strcmp(value_type, "TIME") == 0)
			item->value_type = DICOM_CONTENT_VALUE_TIME;
		else if (strcmp(value_type, "PNAME") == 0)
			item->value_type = DICOM_CONTENT_VALUE_PNAME;
		return;
	}
	if (element.tag.as_u32 == DICOM_RelationshipType) {
		dicom_wsi_replace_content_string(
			&item->relationship_type, data, element.length);
		return;
	}
	dicom_content_code_t* code =
		dicom_wsi_content_code_for_element(instance, item);
	if (code) {
		switch (element.tag.as_u32) {
			case DICOM_CodeValue:
			case DICOM_LongCodeValue:
			case DICOM_URNCodeValue:
				dicom_wsi_replace_content_string(
					&code->value, data, element.length);
				return;
			case DICOM_CodingSchemeDesignator:
				dicom_wsi_replace_content_string(
					&code->coding_scheme, data, element.length);
				return;
			case DICOM_CodingSchemeVersion:
				dicom_wsi_replace_content_string(
					&code->coding_scheme_version, data, element.length);
				return;
			case DICOM_CodeMeaning:
				dicom_wsi_replace_content_string(
					&code->meaning, data, element.length);
				return;
			default:
				break;
		}
	}
	switch (element.tag.as_u32) {
		case DICOM_TextValue:
		case DICOM_NumericValue:
		case DICOM_UID:
		case DICOM_DateTime:
		case DICOM_Date:
		case DICOM_Time:
		case DICOM_PersonName:
			dicom_wsi_replace_content_string(
				&item->scalar_value, data, element.length);
			break;
		default:
			break;
	}
}

// C.8.12.6.1 Plane Position (Slide) Macro
// https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.8.12.6.html#sect_C.8.12.6.1


void dicom_interpret_data_element_in_plane_position_slide_sq_item(dicom_instance_t* instance, dicom_data_element_t element) {
	dicom_plane_position_slide_t* plane_position_slide = array_last_maybe_expand(instance->per_frame_plane_position_slide, instance->pos_stack[2].item_number);
	u8* data = instance->data + element.data_offset;
	const char* data_str = (const char*)data;

	switch(element.tag.as_u32) {
		default: break;
		case DICOM_ColumnPositionInTotalImagePixelMatrix: {
			// Type 1
			// The column position of the top left hand pixel of the frame in the Total Pixel Matrix (see Section C.8.12.4.1.1).
			// The column position of the top left pixel of the Total Pixel Matrix is 1.
			plane_position_slide->column_position_in_total_image_pixel_matrix = *(i32*)data;
		} break;
		case DICOM_RowPositionInTotalImagePixelMatrix: {
			// Type 1
			// The row position of the top left hand pixel of the frame in the Total Pixel Matrix (see Section C.8.12.4.1.1).
			// The row position of the top left pixel of the Total Pixel Matrix is 1.
			plane_position_slide->row_position_in_total_image_pixel_matrix = *(i32*)data;
		} break;
		case DICOM_XOffsetInSlideCoordinateSystem: {
			// Type 1
			// The X offset in mm from the Origin of the Slide Coordinate System.
			plane_position_slide->offset_in_slide_coordinate_system.x = dicom_parse_decimal_string((str_t){data_str, element.length}, NULL);
		} break;
		case DICOM_YOffsetInSlideCoordinateSystem: {
			// Type 1
			// The Y offset in mm from the Origin of the Slide Coordinate System.
			plane_position_slide->offset_in_slide_coordinate_system.y = dicom_parse_decimal_string((str_t){data_str, element.length}, NULL);
		} break;
		case DICOM_ZOffsetInSlideCoordinateSystem: {
			// Type 1
			// The Z offset in µm from the Origin of the Slide Coordinate System, nominally the surface of the glass slide substrate.
			plane_position_slide->z_offset_in_slide_coordinate_system = dicom_parse_decimal_string((str_t){data_str, element.length}, NULL);
		} break;
	}
}

void dicom_interpret_data_element_in_shared_functional_groups(dicom_instance_t* instance, dicom_data_element_t element) {
	u8* data = instance->data + element.data_offset;
	const char* data_str = (const char*)data;

	switch(element.tag.as_u32) {
		default: break;

	}
}


// Attributes that describe the Whole Slide Microscopy Image Module:
// https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.8.12.4.html#sect_C.8.12.4.1.1

void dicom_wsi_interpret_top_level_data_element(dicom_instance_t* instance, dicom_data_element_t element) {
	u8* data = instance->data + element.data_offset;
	const char* data_str = (const char*)data;

	switch(element.tag.as_u32) {
		case DICOM_ContainerIdentifier: {
			instance->ContainerIdentifier =
				dicom_parse_long_string((str_t){data_str, element.length});
		} break;
		case DICOM_ImageType: {
			// Type 1
			// Image identification characteristics.
			str_t next = {data_str, element.length};
			bool valid = true;
			i32 value_count = 0;
			for (i32 i = 0; i < 4; ++i) {
				dicom_cs_t cs = dicom_parse_code_string(next, &next);
				++value_count;
				if (i == 0) {
					// Value 1 shall have a value of ORIGINAL or DERIVED
					if (strcmp(cs.value, "ORIGINAL") == 0) {
						instance->is_image_original = true;
					} else if (strcmp(cs.value, "DERIVED") == 0) {
						instance->is_image_original = false;
					} else { valid = false; }
				} else if (i == 1) {
					// Value 2 shall have a value of PRIMARY
					valid = valid && strcmp(cs.value, "PRIMARY") == 0;
				} else if (i == 2) {
					// Value 3 (Image Flavor)
					if (strcmp(cs.value, "VOLUME") == 0) {
						instance->image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
					} else if (strcmp(cs.value, "LABEL") == 0) {
						instance->image_flavor = DICOM_IMAGE_FLAVOR_LABEL;
					} else if (strcmp(cs.value, "OVERVIEW") == 0) {
						instance->image_flavor = DICOM_IMAGE_FLAVOR_OVERVIEW;
					} else if (strcmp(cs.value, "THUMBNAIL") == 0) {
						instance->image_flavor = DICOM_IMAGE_FLAVOR_THUMBNAIL;
					} else {
						instance->image_flavor = DICOM_IMAGE_FLAVOR_UNKNOWN;
						valid = false;
					}
					instance->image_flavor_cs = cs;
				} else if (i == 3) {
					// Value 4 (Derived Pixel)
					if (strcmp(cs.value, "NONE") == 0) {
						instance->is_image_resampled = false;
					} else if (strcmp(cs.value, "RESAMPLED") == 0) {
						instance->is_image_resampled = true;
					} else { valid = false; }
				}
				if (!next.s) {
					break;
				}
			}
			instance->has_valid_wsi_image_type = valid && value_count == 4 && (!next.s || !next.len);

		} break;
		case DICOM_ImagedVolumeWidth: {
			// Type 1C
			// Width of total imaged volume (distance in the direction of rows in each frame) in mm.
			// Required if Image Type (0008,0008) Value 3 is VOLUME. May be present otherwise.
			instance->imaged_volume_width = *(float*)data;
		} break;
		case DICOM_ImagedVolumeHeight: {
			// Type 1C
			// Height of total imaged volume (distance in the direction of columns in each frame) in mm.
			// Required if Image Type (0008,0008) Value 3 is VOLUME. May be present otherwise.
			instance->imaged_volume_height = *(float*)data;
		} break;
		case DICOM_ImagedVolumeDepth: {
			// Type 1C
			// Depth of total imaged volume (distance in the Z direction of focal planes) in µm.
			// Required if Image Type (0008,0008) Value 3 is VOLUME. May be present otherwise.
			instance->imaged_volume_depth = *(float*)data;
		} break;
		case DICOM_SamplesPerPixel: {
			// Type 1
			// Number of samples (color planes) per frame in this image.
			// Enumerated Values: 3 1
		} break;
		case DICOM_PhotometricInterpretation: {
			// Type 1
			// Specifies the intended interpretation of the pixel data.
			// Enumerated Values:  MONOCHROME2 RGB YBR_FULL_422 YBR_ICT YBR_RCT
			// https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.8.12.4.html#sect_C.8.12.4.1.5
		} break;
		case DICOM_PlanarConfiguration:  {
			// Type 1C
			// Indicates whether the pixel data are encoded color-by-plane or color-by-pixel. Required if Samples per Pixel (0028,0002) has a value greater than 1.
			// Enumerated Values: 0 = color-by-pixel
		} break;
		case DICOM_NumberOfFrames: {
			// Type 1
			// Number of frames in a multi-frame image.
			// Enumerated Values if Image Type (0008,0008) Value 3 is THUMBNAIL, LABEL or OVERVIEW: 1
		} break;
		case DICOM_BitsAllocated: {
			// Type 1
			// Number of bits allocated for each pixel sample.
			// Enumerated Values: 8 16
		} break;
		case DICOM_BitsStored: {
			// Type 1
			// Number of bits stored for each pixel sample. Shall be equal to Bits Allocated (0028,0100).
		} break;
		case DICOM_HighBit: {
			// Type 1
			// Most significant bit for pixel sample data. High Bit (0028,0102) shall be one less than Bits Stored (0028,0101).
		} break;
		case DICOM_PixelRepresentation: {
			// Type 1
			// Data representation of pixel samples.
			// Enumerated Values: 0 = unsigned integer
		} break;
		case DICOM_AcquisitionDateTime: {
			// Type 1
			// The date and time that the acquisition of data that resulted in this image started.
		} break;
		case DICOM_AcquisitionDuration: {
			// Type 1
			// Duration of the image acquisition in seconds.
		} break;
		case DICOM_LossyImageCompression: {
			// Type 1
			// Specifies whether an Image has undergone lossy compression (at a point in its lifetime).
			// Enumerated Values:
			//   0 = Image has NOT been subjected to lossy compression.
			//   1 = Image has been subjected to lossy compression.
		} break;
		case DICOM_LossyImageCompressionRatio: {
			// Type 1C
			// Describes the approximate lossy compression ratio(s) that have been applied to this image.
			// Required if Lossy Image Compression (0028,2110) is "01".
		} break;
		case DICOM_LossyImageCompressionMethod: {
			// Type 1C
			// A label for the lossy compression method(s) that have been applied to this image.
			// Required if Lossy Image Compression (0028,2110) is "01".
		} break;
		case DICOM_PresentationLUTShape: {
			// Type 1C
			// Specifies an identity transformation for the Presentation LUT, such that the output of all grayscale transformations defined in the IOD containing this Module are defined to be P-Values.
			// Enumerated Values: IDENTITY = output is in P-Values.
			// Required if Photometric Interpretation (0028,0004) is MONOCHROME2.
		} break;
		case DICOM_RescaleIntercept: {
			// Type 1C
			// The value b in relationship between stored values (SV) and the output units.
			// Output units = m*SV + b.
			// Required if Photometric Interpretation (0028,0004) is MONOCHROME2.
			// Enumerated Values: 0
		} break;
		case DICOM_RescaleSlope: {
			// Type 1C
			// m in the equation specified by Rescale Intercept (0028,1052).
			// Required if Photometric Interpretation (0028,0004) is MONOCHROME2.
			// Enumerated Values: 1
		} break;
		case DICOM_VolumetricProperties: {
			// Type 1
			// Indication if geometric manipulations are possible with frames in the SOP Instance.
			// Enumerated Values: VOLUME = pixels represent the volume specified for the image, and may be geometrically manipulated
		} break;
		case DICOM_SpecimenLabelInImage: {
			// Type 1
			// Indicates whether the specimen label is captured in the image.
			// Enumerated Values: YES NO
			// Shall be YES if Image Type (0008,0008) Value 3 is OVERVIEW or LABEL.
			// Shall be NO if Image Type (0008,0008) Value 3 is THUMBNAIL or VOLUME.
		} break;
		case DICOM_BurnedInAnnotation: {
			// Type 1
			// Indicates whether or not image contains sufficient burned in annotation to identify the patient.
			// Enumerated Values: YES NO
		} break;
		case DICOM_FocusMethod: {
			// Type 1
			// Method of focusing image.
			// Enumerated Values: AUTO = autofocus, MANUAL = includes any human adjustment or verification of autofocus
		} break;
		case DICOM_ExtendedDepthOfField: {
			// Type 1
			// Image pixels were created through combining of image acquisition at multiple focal planes (focus stacking).
			// Enumerated Values: YES NO
		} break;
		case DICOM_NumberOfFocalPlanes: {
			// Type 1C
			// Number of acquisition focal planes used for extended depth of field.
			// Required if Extended Depth of Field (0048,0012) value is YES
			instance->number_of_focal_planes = *(u16*)data;
		} break;
		case DICOM_DistanceBetweenFocalPlanes: {
			// Type 1C
			// Distance between acquisition focal planes used for extended depth of field, in µm.
			// Required if Extended Depth of Field (0048,0012) value is YES
			instance->distance_between_focal_planes_um = *(float*)data;
		} break;
		case DICOM_AcquisitionDeviceProcessingDescription: {
			// Type 3
			// Description of visual processing performed on the image prior to exchange. Examples of this processing are: edge enhanced, gamma corrected, convolved (spatially filtered)
		} break;
		case DICOM_ConvolutionKernel: {
			// Type 3
			// Label for convolution kernel used in acquisition device visual processing
		} break;
		case DICOM_RecommendedAbsentPixelCIELabValue: {
			// Type 3
			// A color value with which it is recommended to display the pixels of the Total Pixel Matrix that are not encoded. The units are specified in PCS-Values, and the value is encoded as CIELab.
		} break;

		// C.8.12.14 Microscope Slide Layer Tile Organization Module
		// https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.8.12.14.html#sect_C.8.12.14.1.1
		// Table C.8.12.14-1 specifies the Attributes that describe the logical and physical organization of the tiles within a single resolution layer encoded as a tiled Image, such as that of a Multi-Resolution Pyramid.
		case DICOM_TotalPixelMatrixColumns: {
			// Type 1
			// Total number of columns in pixel matrix; i.e., width of total imaged volume in pixels.
			instance->total_pixel_matrix_columns = *(u32*)data;
		} break;
		case DICOM_TotalPixelMatrixRows: {
			// Type 1
			// Total number of rows in pixel matrix; i.e., height of total imaged volume in pixels.
			instance->total_pixel_matrix_rows = *(u32*)data;
		} break;
		case DICOM_DimensionOrganizationType: {
			dicom_cs_t organization = dicom_parse_code_string(
				(str_t){data_str, element.length}, NULL);
			instance->is_tiled_full = strcmp(organization.value, "TILED_FULL") == 0;
		} break;
		case DICOM_NumberOfOpticalPaths: {
			instance->number_of_optical_paths = *(u32*)data;
		} break;
		case DICOM_TotalPixelMatrixFocalPlanes: {
			instance->total_pixel_matrix_focal_planes = *(u32*)data;
		} break;
		case DICOM_ImageOrientationSlide: {
			// Type 1
			// Direction cosines of the first row and first column with respect to the Slide Coordinate System.
			str_t next = {data_str, element.length};
			bool complete = true;
			for (i32 i = 0; i < 6; ++i) {
				if (!next.s) {
					complete = false;
					break;
				}
				instance->image_orientation_slide[i] =
					dicom_parse_decimal_string(next, &next);
			}
			instance->is_image_orientation_slide_known = complete;
		} break;
	}
}


void dicom_wsi_interpret_nested_data_element(dicom_instance_t* instance, dicom_data_element_t element) {
	u8* data = instance->data + element.data_offset;
	switch(instance->nested_sequences[0].as_u32) {
		default: break;
		case DICOM_TotalPixelMatrixOriginSequence: {
			switch(element.tag.as_u32) {
				default: break;
				case DICOM_XOffsetInSlideCoordinateSystem: {
					instance->total_pixel_matrix_origin_x_mm =
						dicom_parse_decimal_string((str_t){(char*)data, element.length}, NULL);
					instance->total_pixel_matrix_origin_component_mask |= 1;
				} break;
				case DICOM_YOffsetInSlideCoordinateSystem: {
					instance->total_pixel_matrix_origin_y_mm =
						dicom_parse_decimal_string((str_t){(char*)data, element.length}, NULL);
					instance->total_pixel_matrix_origin_component_mask |= 2;
				} break;
				case DICOM_ZOffsetInSlideCoordinateSystem: {
					instance->total_pixel_matrix_origin_z_um =
						dicom_parse_decimal_string((str_t){(char*)data, element.length}, NULL);
					instance->total_pixel_matrix_origin_component_mask |= 4;
				} break;
			}
			instance->is_total_pixel_matrix_origin_known =
				(instance->total_pixel_matrix_origin_component_mask & 3) == 3;
		} break;
		case DICOM_OpticalPathSequence: {
			str_t value = {(char*)data, element.length};
			switch(instance->pos_stack[2].element.tag.as_enum) {
				default: break;
				case DICOM_IlluminationTypeCodeSequence: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					dicom_wsi_parse_code_element(
						&optical_path->illumination_type, element, value);
				} break;
					// Either IlluminationWaveLength or IlluminationColorCodeSequence is required to be present
				case DICOM_IlluminationWaveLength: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					if (element.length >= sizeof(float)) {
						memcpy(&optical_path->illumination_wavelength, data, sizeof(float));
						optical_path->illumination_wavelength_present = true;
					}
				} break;
				case DICOM_IlluminationColorCodeSequence: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					dicom_wsi_parse_code_element(
						&optical_path->illumination_color, element, value);
				} break;
				case DICOM_ICCProfile: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					optical_path->icc_profile = malloc(element.length);
					optical_path->icc_profile_size = element.length;
					memcpy(optical_path->icc_profile, data, element.length);
				} break;
				case DICOM_OpticalPathIdentifier: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					optical_path->OpticalPathIdentifier = dicom_parse_short_string((str_t){(char*)data, element.length});
				} break;
				case DICOM_OpticalPathDescription: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					u32 length = element.length;
					while (length && (data[length - 1] == ' ' || data[length - 1] == '\0'))
						--length;
					length = MIN(length, sizeof(optical_path->OpticalPathDescription.value) - 1);
					memcpy(optical_path->OpticalPathDescription.value, data, length);
					optical_path->OpticalPathDescription.value[length] = '\0';
					optical_path->OpticalPathDescription.present = true;
				} break;
				case DICOM_ColorSpace: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					optical_path->ColorSpace =
						dicom_parse_code_string(value, NULL);
				} break;
				case DICOM_LightPathFilterPassThroughWavelength: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					if (element.length >= sizeof(u16)) {
						memcpy(&optical_path->light_path_filter_pass_through_wavelength,
						       data, sizeof(u16));
						optical_path->present_fields |= DICOM_OPTICAL_LIGHT_FILTER_PASS;
					}
				} break;
				case DICOM_LightPathFilterPassBand: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					if (element.length >= sizeof(optical_path->light_path_filter_pass_band)) {
						memcpy(optical_path->light_path_filter_pass_band, data,
						       sizeof(optical_path->light_path_filter_pass_band));
						optical_path->present_fields |= DICOM_OPTICAL_LIGHT_FILTER_BAND;
					}
				} break;
				case DICOM_ImagePathFilterPassThroughWavelength: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					if (element.length >= sizeof(u16)) {
						memcpy(&optical_path->image_path_filter_pass_through_wavelength,
						       data, sizeof(u16));
						optical_path->present_fields |= DICOM_OPTICAL_IMAGE_FILTER_PASS;
					}
				} break;
				case DICOM_ImagePathFilterPassBand: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					if (element.length >= sizeof(optical_path->image_path_filter_pass_band)) {
						memcpy(optical_path->image_path_filter_pass_band, data,
						       sizeof(optical_path->image_path_filter_pass_band));
						optical_path->present_fields |= DICOM_OPTICAL_IMAGE_FILTER_BAND;
					}
				} break;
				case DICOM_ObjectiveLensPower: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					optical_path->objective_lens_power =
						dicom_parse_decimal_string(value, NULL);
					optical_path->present_fields |= DICOM_OPTICAL_OBJECTIVE_POWER;
				} break;
				case DICOM_ObjectiveLensNumericalAperture: {
					dicom_optical_path_t* optical_path = array_last_maybe_expand(
						instance->optical_paths, instance->pos_stack[1].item_number);
					optical_path->objective_lens_numerical_aperture =
						dicom_parse_decimal_string(value, NULL);
					optical_path->present_fields |= DICOM_OPTICAL_OBJECTIVE_APERTURE;
				} break;
			}
		} break;
		case DICOM_SpecimenDescriptionSequence: {
			dicom_wsi_parse_specimen_preparation_element(instance, element);
			str_t value = {(char*)data, element.length};
			switch (instance->pos_stack[2].element.tag.as_enum) {
				case DICOM_SpecimenIdentifier: {
					dicom_specimen_t* specimen = array_last_maybe_expand(
						instance->specimens, instance->pos_stack[1].item_number);
					specimen->SpecimenIdentifier = dicom_parse_long_string(value);
				} break;
				case DICOM_SpecimenUID: {
					dicom_specimen_t* specimen = array_last_maybe_expand(
						instance->specimens, instance->pos_stack[1].item_number);
					specimen->SpecimenUID = dicom_parse_uid(value);
				} break;
				case DICOM_SpecimenShortDescription: {
					dicom_specimen_t* specimen = array_last_maybe_expand(
						instance->specimens, instance->pos_stack[1].item_number);
					specimen->SpecimenShortDescription =
						dicom_parse_long_string(value);
				} break;
				default:
					break;
			}
		} break;
		case DICOM_PerFrameFunctionalGroupsSequence: {
			switch(instance->nested_sequences[1].as_u32) {
				default: break;
				case DICOM_PlanePositionSlideSequence: {
					dicom_interpret_data_element_in_plane_position_slide_sq_item(instance, element);
				} break;
                case DICOM_OpticalPathIdentificationSequence: {
					if (element.tag.as_u32 == DICOM_OpticalPathIdentifier) {
						dicom_sh_t* identifier = array_last_maybe_expand(
							instance->per_frame_optical_path_identifiers,
							instance->pos_stack[2].item_number);
						*identifier = dicom_parse_short_string(
							(str_t){(char*)data, element.length});
					}
                } break;
			}
		} break;
		case DICOM_SharedFunctionalGroupsSequence: {
			switch(instance->nested_sequences[1].as_u32) {
				default: break;
				case DICOM_PixelMeasuresSequence: {
					switch(element.tag.as_u32) {
						case DICOM_SliceThickness: {

						} break;
						case DICOM_SpacingBetweenSlices: {

						} break;
						case DICOM_PixelSpacing: {
							str_t next = {(const char*)data, element.length};
							v2f pixel_spacing = {};
							pixel_spacing.x = dicom_parse_decimal_string(next, &next);
							if (next.s) {
								pixel_spacing.y = dicom_parse_decimal_string(next, NULL);
							} else {
								pixel_spacing.y = pixel_spacing.x; // this should not happen
							}
							instance->pixel_spacing = pixel_spacing; // TODO: error if pixel spacing not under shared functional groups?
						} break;
					}
				} break;
			}
		} break;
	}
}

static u8* dicom_wsi_decode_native_tile_to_bgra(dicom_instance_t* instance, const u8* data, size_t data_size) {
	if (instance->bits_allocated != 8 || instance->bits_stored != 8 || instance->high_bit != 7 ||
	    instance->pixel_representation != 0) {
		console_print_error("DICOM tile decode: unsupported native pixel sample format\n");
		return NULL;
	}

	size_t pixel_count = (size_t)instance->columns * instance->rows;
	size_t expected_size = pixel_count * instance->samples_per_pixel;
	if (data_size < expected_size) {
		console_print_error("DICOM tile decode: native Pixel Data frame is too small\n");
		return NULL;
	}

	u8* result = malloc(pixel_count * 4);
	if (!result) return NULL;

	if (instance->photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_RGB &&
	    instance->samples_per_pixel == 3) {
		for (size_t i = 0; i < pixel_count; ++i) {
			size_t r_offset;
			size_t g_offset;
			size_t b_offset;
			if (instance->planar_configuration == 0) {
				r_offset = i * 3;
				g_offset = r_offset + 1;
				b_offset = r_offset + 2;
			} else if (instance->planar_configuration == 1) {
				r_offset = i;
				g_offset = pixel_count + i;
				b_offset = pixel_count * 2 + i;
			} else {
				console_print_error("DICOM tile decode: invalid Planar Configuration (%u)\n",
				                    instance->planar_configuration);
				free(result);
				return NULL;
			}
			result[i * 4 + 0] = data[b_offset];
			result[i * 4 + 1] = data[g_offset];
			result[i * 4 + 2] = data[r_offset];
			result[i * 4 + 3] = 255;
		}
	} else if (instance->photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_MONOCHROME2 &&
	           instance->samples_per_pixel == 1) {
		for (size_t i = 0; i < pixel_count; ++i) {
			result[i * 4 + 0] = data[i];
			result[i * 4 + 1] = data[i];
			result[i * 4 + 2] = data[i];
			result[i * 4 + 3] = 255;
		}
	} else {
		console_print_error("DICOM tile decode: unsupported native Photometric Interpretation/sample count\n");
		free(result);
		return NULL;
	}

	return result;
}

u8* dicom_wsi_decode_tile_to_bgra(dicom_series_t* dicom_series, i32 instance_index, i32 tile_index) {
	dicom_instance_t* level_instance = dicom_series->wsi.level_instances[instance_index];
	ASSERT(level_instance);
	if (!level_instance) return NULL;
	dicom_tile_t* dicom_tile = level_instance->tiles + tile_index;
	dicom_instance_t* instance = dicom_tile->instance;
	if (!dicom_tile->exists || !instance) return NULL;
	if (!dicom_tile->is_offset_known) {
		if (!dicom_instance_index_pixel_data(instance)) return NULL;
		// Concatenation tiles can borrow a different instance's offset table.
		if (!dicom_tile->is_offset_known) {
			if (!instance->pixel_data_offsets || !instance->pixel_data_sizes ||
				dicom_tile->frame_index >= instance->pixel_data_offset_count) return NULL;
			dicom_tile->data_offset_in_file = sizeof(dicom_header_t) + instance->pixel_data_start_offset +
				instance->pixel_data_offsets[dicom_tile->frame_index];
			dicom_tile->data_size = instance->pixel_data_sizes[dicom_tile->frame_index];
			dicom_tile->is_offset_known = true;
		}
	}
	size_t read_size = dicom_tile->data_size;
	if (dicom_tile->data_size == DICOM_UNDEFINED_LENGTH) {
		u8 temp[12];
		size_t bytes_read = source_file_read_at_offset(temp, &instance->file, dicom_tile->data_offset_in_file, 12);
		dicom_data_element_t element = dicom_read_data_element(temp, 0, instance->encoding, bytes_read);
		if (element.tag.as_u32 == DICOM_Item) {
			read_size = (u64)element.length + 8; // Include the encapsulation item header.
		} else {
			ASSERT(!"could not read a valid Item");
			return NULL;
		}
	}
	if (!read_size || dicom_tile->data_offset_in_file > instance->file.size) return NULL;
	// The last Basic Offset Table entry can conservatively extend through EOF.
	u64 available = instance->file.size - dicom_tile->data_offset_in_file;
	if (read_size > available) read_size = available;
	if (!read_size || read_size > MEGABYTES(64)) return NULL;
    temp_memory_t temp = begin_temp_memory_on_local_thread();
	u8* tile_data = (u8*)arena_push_size(temp.arena, read_size);
	size_t bytes_read = source_file_read_at_offset(tile_data, &instance->file,
	                                              dicom_tile->data_offset_in_file, read_size);
	if (bytes_read != read_size) {
		release_temp_memory(&temp);
		return NULL;
	}

    u8* result = NULL;
	if (!instance->is_pixel_data_encapsulated) {
		result = dicom_wsi_decode_native_tile_to_bgra(instance, tile_data, bytes_read);
	} else {
		i64 data_size = dicom_defragment_encapsulated_pixel_data_frame(tile_data, bytes_read);
		bool is_jpeg_baseline = instance->transfer_syntax_uid.as_enum == DICOM_JPEGBaseline8Bit;
		bool is_jpeg2000 = instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000Lossless ||
		                   instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000 ||
		                   instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000MCLossless ||
		                   instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000MC;
		if (data_size > 0 && is_jpeg_baseline) {
			// JPEG compression
			i32 width = 0;
			i32 height = 0;
			i32 channels_in_file = 0;
			bool is_ycbcr = instance->photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_YBR_FULL ||
			                instance->photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_YBR_FULL_422;
			bool is_rgb = instance->photometric_interpretation == DICOM_PHOTOMETRIC_INTERPRETATION_RGB;
			u8* pixels = NULL;
			if (is_rgb) {
				// Some RGB DICOM JPEGs have ambiguous component identifiers, causing libjpeg to guess YCbCr.
				pixels = jpeg_decode_rgb_image(tile_data, data_size, &width, &height, &channels_in_file);
			} else if (is_ycbcr) {
				// Prefer JPEG marker detection here. Some converted slides are tagged YBR_FULL_422 even
				// though their Adobe APP14 transform and component sampling identify RGB JPEG data.
				pixels = jpeg_decode_image(tile_data, data_size, &width, &height, &channels_in_file);
			} else {
				console_print_error("DICOM tile decode: unsupported JPEG Photometric Interpretation (%d)\n",
				                    instance->photometric_interpretation);
			}
			if (pixels && width == instance->columns && height == instance->rows && channels_in_file == 4) {
				// success
				result = pixels;
			} else {
				if (pixels) free(pixels);
				result = NULL;
			}
		} else if (data_size > 0 && is_jpeg2000) {
			i32 width = 0;
			i32 height = 0;
			result = jpeg2000_decode_image_to_bgra(tile_data, data_size, &width, &height);
			if (result && (width != instance->columns || height != instance->rows)) {
				console_print_error("DICOM tile decode: JPEG 2000 dimensions do not match DICOM frame\n");
				free(result);
				result = NULL;
			}
			if (!result) {
				console_print_error("DICOM tile decode: JPEG 2000 decoding failed\n");
			}
		} else if (data_size <= 0) {
			console_print_error("DICOM tile decode: invalid encapsulated Pixel Data frame\n");
		} else {
			console_print_error("DICOM tile decode: unsupported Transfer Syntax (%s)\n",
			                    instance->transfer_syntax_uid.value);
		}
	}
    release_temp_memory(&temp);
	return result;
}

bool dicom_wsi_index_level(dicom_series_t* dicom_series, i32 instance_index) {
	dicom_instance_t* level_instance = dicom_series->wsi.level_instances[instance_index];
	if (!level_instance) return false;
	bool success = true;
	for (i32 tile_index = 0; tile_index < level_instance->tile_count; ++tile_index) {
		dicom_tile_t* tile = level_instance->tiles + tile_index;
		if (!tile->exists || tile->is_offset_known) continue;
		dicom_instance_t* instance = tile->instance;
		if (!instance || !dicom_instance_index_pixel_data(instance) ||
		    !instance->pixel_data_offsets || !instance->pixel_data_sizes ||
		    tile->frame_index >= instance->pixel_data_offset_count) {
			success = false;
			continue;
		}
		tile->data_offset_in_file = sizeof(dicom_header_t) + instance->pixel_data_start_offset +
		                            instance->pixel_data_offsets[tile->frame_index];
		tile->data_size = instance->pixel_data_sizes[tile->frame_index];
		tile->is_offset_known = true;
	}
	return success;
}
