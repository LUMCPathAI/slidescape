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

// Annotation save/load procedures.

#include "common.h"
#include "annotation.h"
#include "viewer.h"
#include "platform.h"
#include "stringutils.h"
#include "yxml.h"

// XML parsing using the yxml library.
// https://dev.yorhel.nl/yxml/man
#define YXML_STACK_BUFFER_SIZE KILOBYTES(32)

enum {
	ASAP_XML_PARSE_GROUPS_AND_FEATURES = 0,
	ASAP_XML_PARSE_ANNOTATIONS = 1,
};

typedef enum asap_xml_element_enum {
	ASAP_XML_ELEMENT_NONE = 0,               // for unhandled elements
	ASAP_XML_ELEMENT_ANNOTATION = 1,         // <Annotation>
	ASAP_XML_ELEMENT_COORDINATE = 2,         // <Coordinate>
	ASAP_XML_ELEMENT_GROUP = 3,              // <Group>
	ASAP_XML_ELEMENT_FEATURE = 4,            // <Feature>
	ASAP_XML_ELEMENT_ATTRIBUTES = 5,         // <Attributes>
	ASAP_XML_ELEMENT_ASAP_ANNOTATIONS = 6,   // <ASAP_Annotations>
	ASAP_XML_ELEMENT_ANNOTATIONGROUPS = 7,   // <AnnotationGroups>
	ASAP_XML_ELEMENT_ANNOTATIONFEATURES = 8, // <AnnotationFeatures>
	ASAP_XML_ELEMENT_ANNOTATIONS = 9,        // <Annotations>
} asap_xml_element_enum;

typedef enum asap_xml_attribute_enum {
	ASAP_XML_ATTRIBUTE_NONE = 0,             // for unhandled attributes
	ASAP_XML_ATTRIBUTE_COLOR = 1,            // Color="#000000"
	ASAP_XML_ATTRIBUTE_NAME = 2,             // Name="group_name"
	ASAP_XML_ATTRIBUTE_PARTOFGROUP = 3,      // PartOfGroup="group_name"
	ASAP_XML_ATTRIBUTE_TYPE = 4,             // Type="Polygon"
	ASAP_XML_ATTRIBUTE_X = 5,                // X="12345" (for <Coordinate>)
	ASAP_XML_ATTRIBUTE_Y = 6,                // Y="12345" (for <Coordinate>)
} asap_xml_attribute_enum;

static rgba_t asap_xml_parse_color(const char* value) {
	rgba_t rgba = {0, 0, 0, 255};
	if (strlen(value) != 7 || value[0] != '#') {
		console_print("annotation_set_attribute(): Color attribute \"%s\" not in form #rrggbb\n", value);
	} else {
		char temp[3] = {};
		temp[0] = value[1];
		temp[1] = value[2];
		rgba.r = (u8)strtoul(temp, NULL, 16);
		temp[0] = value[3];
		temp[1] = value[4];
		rgba.g = (u8)strtoul(temp, NULL, 16);
		temp[0] = value[5];
		temp[1] = value[6];
		rgba.b = (u8)strtoul(temp, NULL, 16);
	}
	return rgba;
}

static void annotation_set_attribute(annotation_set_t* annotation_set, annotation_t* annotation, const char* attr,
                              const char* value) {
	if (strcmp(attr, "Color") == 0) {
		annotation->color = asap_xml_parse_color(value);
	} else if (strcmp(attr, "Name") == 0) {
		copy_cstring(annotation->name, value, sizeof(annotation->name));
	} else if (strcmp(attr, "PartOfGroup") == 0) {
		annotation->group_id = find_annotation_group_or_create_if_not_found(annotation_set, value);
	} else if (strcmp(attr, "Type") == 0) {
		if (strcmp(value, "Rectangle") == 0) {
			annotation->type = ANNOTATION_RECTANGLE;
		} else if (strcmp(value, "Polygon") == 0) {
			annotation->type = ANNOTATION_POLYGON;
		} else if (strcmp(value, "Spline") == 0) {
			annotation->type = ANNOTATION_SPLINE;
		} else if (strcmp(value, "Dot") == 0) {
			annotation->type = ANNOTATION_POINT;
		} else {
			console_print("Warning: annotation '%s' with unrecognized type '%s', defaulting to 'Polygon'.\n", annotation->name, value);
			annotation->type = ANNOTATION_POLYGON;
		}
	}
}

static void coordinate_set_attribute(annotation_set_t* annotation_set, annotation_t* annotation, v2f* coordinate, const char* attr,
                              const char* value) {
	if (strcmp(attr, "Order") == 0) {
		// ignored
//		coordinate->order = atoi(value);
	} else if (strcmp(attr, "X") == 0) {
		coordinate->x = (float)atof(value) * annotation_set->mpp.x;
	} else if (strcmp(attr, "Y") == 0) {
		coordinate->y = (float)atof(value) * annotation_set->mpp.y;
	}
}

static void group_set_attribute(annotation_group_t* group, const char* attr, const char* value) {
	if (strcmp(attr, "Color") == 0) {
		group->color = asap_xml_parse_color(value);
	} else if (strcmp(attr, "Name") == 0) {
		copy_cstring(group->name, value, sizeof(group->name));
	} else if (strcmp(attr, "PartOfGroup") == 0) {
		// TODO: allow nested groups?
	}
}

static void feature_set_attribute(annotation_set_t* annotation_set, annotation_feature_t* feature, const char* attr, const char* value) {
	if (strcmp(attr, "Value") == 0) {
		feature->value = (float)atof(value);
	} else if (strcmp(attr, "Name") == 0) {
		copy_cstring(feature->name, value, sizeof(feature->name));
	} else if (strcmp(attr, "RestrictToGroup") == 0) {
		feature->group_id = find_annotation_group_or_create_if_not_found(annotation_set, value);
		feature->restrict_to_group = true;
		// TODO: allow restrict to multiple groups? nested groups?
	} else if (strcmp(attr, "Color") == 0) {
		feature->color = asap_xml_parse_color(value);
	}
}

#define ASAP_XML_PARSER_MAX_STACK 16

bool load_asap_xml_annotations(app_state_t* app_state, const char* filename) {

	annotation_set_t* live_annotation_set = &app_state->scene.annotation_set;
	annotation_set_t* annotation_set = duplicate_annotation_set(live_annotation_set);
	if (!annotation_set) return false;
	annotation_group_t current_group = {};
	annotation_feature_t current_feature = {};
	asap_xml_element_enum element_stack[ASAP_XML_PARSER_MAX_STACK] = {};
	i32 element_stack_index = 0;
	bool is_within_annotationfeatures_tag = false;


	mem_t* file = platform_read_entire_file(filename);
	yxml_t* x = NULL;
	bool32 success = false;
	i64 start = get_clock();

	if (0) { failed: cleanup:
		if (x) free(x);
		if (file) free(file);
		if (success) {
			// Parsing is transactional: publish the new state only after both passes
			// and the final XML validity check have completed successfully.
			destroy_annotation_set(live_annotation_set);
			*live_annotation_set = *annotation_set;
			free(annotation_set);
			annotation_history_reset(live_annotation_set);
		} else {
			destroy_annotation_set(annotation_set);
			free(annotation_set);
		}
		return success;
	}

	if (file) {
		// hack: merge memory for yxml_t struct and stack buffer
		// Note: what is a good stack buffer size?
		x = (yxml_t*) malloc(sizeof(yxml_t) + YXML_STACK_BUFFER_SIZE);
		if (!x) goto failed;

		// ASAP puts all of the group definitions at the end of the file, instead of the beginning.
		// To preserve the order of the groups, we need to load the XML in two passes:
		// pass 0: read annotation groups only
		// pass 1: read annotations and coordinates
		for (i32 pass = 0; pass < 2; ++pass) {
			yxml_init(x, x + 1, YXML_STACK_BUFFER_SIZE);
			element_stack_index = 0;
			is_within_annotationfeatures_tag = false;

			// parse XML byte for byte
			char attrbuf[128];
			char* attrbuf_end = attrbuf + sizeof(attrbuf);
			char* attrcur = NULL;
			char contentbuf[128];
			char* contentbuf_end = contentbuf + sizeof(contentbuf);
			char* contentcur = NULL;

			char* doc = (char*) file->data;
			for (; *doc; doc++) {
				yxml_ret_t r = yxml_parse(x, *doc);
				if (r == YXML_OK) {
					continue; // nothing worthy of note has happened -> continue
				} else if (r < 0) {
					goto failed;
				} else if (r > 0) {
					// token
					switch(r) {
						case YXML_ELEMSTART: {
							// start of an element: '<Tag ..'
//						    console_print("element start: %s\n", x->elem);
							contentcur = contentbuf;
							*contentcur = '\0';
							++element_stack_index;
							if (element_stack_index >= ASAP_XML_PARSER_MAX_STACK) {
								console_print_error("load_asap_xml_annotations(): element stack overflow (too many nested elements)\n");
								goto failed;
							}

							asap_xml_element_enum element_type = ASAP_XML_ELEMENT_NONE;
							if (strcmp(x->elem, "Coordinate") == 0) {
								element_type = ASAP_XML_ELEMENT_COORDINATE;
							} else if (strcmp(x->elem, "Annotation") == 0) {
								element_type = ASAP_XML_ELEMENT_ANNOTATION;
							} else if (strcmp(x->elem, "Feature") == 0) {
								element_type = ASAP_XML_ELEMENT_FEATURE;
							} else if (strcmp(x->elem, "Group") == 0) {
								element_type = ASAP_XML_ELEMENT_GROUP;
							} else if (strcmp(x->elem, "Attributes") == 0) {
								element_type = ASAP_XML_ELEMENT_ATTRIBUTES;
							} else if (strcmp(x->elem, "ASAP_Annotations") == 0) {
								element_type = ASAP_XML_ELEMENT_ASAP_ANNOTATIONS;
							} else if (strcmp(x->elem, "AnnotationGroups") == 0) {
								element_type = ASAP_XML_ELEMENT_ANNOTATIONGROUPS;
							} else if (strcmp(x->elem, "AnnotationFeatures") == 0) {
								element_type = ASAP_XML_ELEMENT_ANNOTATIONFEATURES;
							} else if (strcmp(x->elem, "Annotations") == 0) {
								element_type = ASAP_XML_ELEMENT_ANNOTATIONS;
							}

							if (element_type == ASAP_XML_ELEMENT_ANNOTATIONFEATURES) {
								// We need to track this in order to disambiguate feature definitions from feature values
								// Feature definitions are stored separately within an <AnnotationFeatures> tag
								// Feature values are stored within an <Annotation>
								// (We are letting both situations use the <Feature> tag, but in a different way)
								is_within_annotationfeatures_tag = true;
							} else if (pass == ASAP_XML_PARSE_GROUPS_AND_FEATURES) {
								if (element_type == ASAP_XML_ELEMENT_GROUP) {
									// reset the state (start parsing a new group)
									memset(&current_group, 0, sizeof(current_group));
									current_group.is_explicitly_defined = true; // (because this group has an XML tag)
								} else if (element_type == ASAP_XML_ELEMENT_FEATURE && is_within_annotationfeatures_tag) {
									// reset the state (start parsing a new feature)
									memset(&current_feature, 0, sizeof(current_feature));
									current_feature.is_explicitly_defined = true; // (because this feature has an XML tag)
								}
							} else if (pass == ASAP_XML_PARSE_ANNOTATIONS) {
								if (element_type == ASAP_XML_ELEMENT_ANNOTATION) {
									annotation_t new_annotation = {};
									arrput(annotation_set->stored_annotations, new_annotation);
									++annotation_set->stored_annotation_count;
								} else if (element_type == ASAP_XML_ELEMENT_COORDINATE) {
									ASSERT(annotation_set->stored_annotation_count == arrlen(annotation_set->stored_annotations));
									ASSERT(annotation_set->stored_annotation_count > 0);
									if (annotation_set->stored_annotations != NULL && annotation_set->stored_annotation_count > 0) {
										annotation_t* current_annotation = arrlastptr(annotation_set->stored_annotations);
										v2f new_coordinate = {};
										ASSERT(current_annotation);
										arrput(current_annotation->coordinates, new_coordinate);
										++current_annotation->coordinate_count;
									}
								} else if (element_type == ASAP_XML_ELEMENT_FEATURE && !is_within_annotationfeatures_tag) {
									element_type = ASAP_XML_ELEMENT_FEATURE;
									memset(&current_feature, 0, sizeof(current_feature));
								}
							}
							element_stack[element_stack_index] = element_type;

						} break;
						case YXML_CONTENT: {
							// element content
//						    console_print("   element content: %s\n", x->elem);
							if (!contentcur) break;
							char* tmp = x->data;
							while (*tmp && contentcur < contentbuf_end - 1) {
								*(contentcur++) = *(tmp++);
							}
							if (*tmp) {
								// too long content
								console_print_error("load_asap_xml_annotations(): encountered a too long XML element content\n");
								goto failed;
							}
							*contentcur = '\0';
						} break;

						case YXML_ELEMEND: {
							// end of an element: '.. />' or '</Tag>'
							asap_xml_element_enum current_element_type = element_stack[element_stack_index];
//						    console_print("element end: %s\n", current_element_type);
							if (contentcur) {
								// NOTE: usually only whitespace (newlines and such)
//							    console_print("elem content: %s\n", contentbuf);
							}

							if (pass == ASAP_XML_PARSE_GROUPS_AND_FEATURES) {
								if (current_element_type == ASAP_XML_ELEMENT_GROUP) {
									annotation_group_t* parsed_group = &current_group;
									// Check if a group already exists with this name, if not create it
									i32 group_index = find_annotation_group_or_create_if_not_found(annotation_set, parsed_group->name);
									annotation_group_t* destination_group = annotation_set->stored_groups + group_index;
									// 'Commit' the group with all its attributes
									memcpy(destination_group, parsed_group, sizeof(*destination_group));
								} else if (is_within_annotationfeatures_tag && current_element_type == ASAP_XML_ELEMENT_FEATURE) {
									annotation_feature_t* parsed_feature = &current_feature;
									// Check if a feature already exists with this name, if not create it
									i32 feature_index = find_annotation_feature_or_create_if_not_found(annotation_set, parsed_feature->name);
									annotation_feature_t* destination_feature = annotation_set->stored_features + feature_index;
									// 'Commit' the group with all its attributes
									memcpy(destination_feature, parsed_feature, sizeof(*destination_feature));
									destination_feature->id = feature_index;
								}
							} else if (pass == ASAP_XML_PARSE_ANNOTATIONS) {
								if (!is_within_annotationfeatures_tag && current_element_type == ASAP_XML_ELEMENT_FEATURE) {
									float value = current_feature.value;
									annotation_feature_t* parsed_feature = &current_feature;
									// Check if a feature already exists with this name, if not create it
									i32 feature_index = find_annotation_feature_or_create_if_not_found(annotation_set, parsed_feature->name);
									annotation_t* annotation = arrlastptr(annotation_set->stored_annotations);
									if (annotation && feature_index < COUNT(annotation->features)) {
										annotation->features[feature_index] = value;
									} else {
										fatal_error();
									}
								}
							}
							if (current_element_type == ASAP_XML_ELEMENT_ANNOTATIONFEATURES) {
								// We need to track this in order to disambiguate feature definitions from feature values
								// Feature definitions are stored separately within an <AnnotationFeatures> tag
								// Feature values are stored within an <Annotation>
								// (We are letting both situations use the <Feature> tag, but in a different way)
								is_within_annotationfeatures_tag = false;
							}

							// 'Pop' out of the element stack
							if (element_stack_index <= 0) {
								// Underflow! More YXML_ELEMEND than YXML_ELEMSTART?
								// yxml should throw an error in this case (malformed XML file?); this code should never be reached.
								fatal_error();
							}
							--element_stack_index;


						} break;

						case YXML_ATTRSTART: {
							// attribute: 'Name=..'
//						    console_print("attr start: %s\n", x->attr);
							attrcur = attrbuf;
							*attrcur = '\0';
						} break;
						case YXML_ATTRVAL: {
							// attribute value
//						    console_print("   attr val: %s\n", x->attr);
							if (!attrcur) break;
							char* tmp = x->data;
							while (*tmp && attrcur < attrbuf_end - 1) {
								*(attrcur++) = *(tmp++);
							}
							if (*tmp) {
								// too long attribute
								console_print("load_asap_xml_annotations(): encountered a too long XML attribute\n");
								goto failed;
							}
							*attrcur = '\0';
						} break;
						case YXML_ATTREND: {
							// end of attribute '.."'
							asap_xml_element_enum current_element_type = element_stack[element_stack_index];
							if (attrcur) {
//							    console_print("attr %s = %s\n", x->attr, attrbuf);
								if (pass == ASAP_XML_PARSE_GROUPS_AND_FEATURES) {
									if (current_element_type == ASAP_XML_ELEMENT_GROUP) {
										group_set_attribute(&current_group, x->attr, attrbuf);
									} else if (current_element_type == ASAP_XML_ELEMENT_FEATURE) {
										feature_set_attribute(annotation_set, &current_feature, x->attr, attrbuf);
									}
								} else if (pass == ASAP_XML_PARSE_ANNOTATIONS) {
									if (current_element_type == ASAP_XML_ELEMENT_ANNOTATION) {
										annotation_set_attribute(annotation_set, arrlastptr(annotation_set->stored_annotations), x->attr, attrbuf);
									} else if (current_element_type == ASAP_XML_ELEMENT_COORDINATE) {
										annotation_t* annotation = arrlastptr(annotation_set->stored_annotations);
										ASSERT(annotation);
										if (annotation) {
											v2f* coordinate = arrlastptr(annotation->coordinates);
											ASSERT(coordinate);
											if (coordinate) {
												coordinate_set_attribute(annotation_set, annotation, coordinate, x->attr, attrbuf);
											}
										}
									} else if (current_element_type == ASAP_XML_ELEMENT_FEATURE) {
										feature_set_attribute(annotation_set, &current_feature, x->attr, attrbuf);
									}
								}
							}
						} break;
						case YXML_PISTART:
						case YXML_PICONTENT:
						case YXML_PIEND:
							break; // processing instructions (uninteresting, skip)
						default: {
							console_print("yxml_parse(): unrecognized token (%d)\n", r);
							goto failed;
						}
					}
				}
			}
			if (yxml_eof(x) != YXML_OK || element_stack_index != 0) {
				console_print_error("load_asap_xml_annotations(): incomplete XML document\n");
				goto failed;
			}
		}
	}

	// At this point, the indices for the 'active' annotations are all nicely in order (as they are loaded).
	// So we simply set the indices in ascending order, as a reference to look up the actual annotation_t struct.
	// (later on, the indices might get reordered by the user, annotations might get deleted, inserted, etc.)
	ASSERT(annotation_set->active_annotation_indices == NULL);
	arrsetlen(annotation_set->active_annotation_indices, annotation_set->stored_annotation_count);
	annotation_set->active_annotation_count = annotation_set->stored_annotation_count;
	for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
		annotation_set->active_annotation_indices[i] = i;
	}

	copy_cstring(annotation_set->annotation_filename, filename, sizeof(annotation_set->annotation_filename));
	annotation_set->preferred_output_format = ANNOTATION_FILE_FORMAT_ASAP_XML;
	annotation_set->annotations_were_loaded_from_file = true;
	success = true;
	float seconds_elapsed = get_seconds_elapsed(start, get_clock());
	console_print_verbose("Loaded ASAP XML annotations in %g seconds.\n", seconds_elapsed);

	goto cleanup;
	// return success;
}

void asap_xml_print_color(char* buf, size_t bufsize, rgba_t rgba) {
	snprintf(buf, bufsize, "#%02x%02x%02x", rgba.r, rgba.g, rgba.b);
}

const char* get_annotation_type_name(annotation_type_enum type) {
	const char* result = "";
	switch(type) {
		case ANNOTATION_UNKNOWN_TYPE: default: break;
		case ANNOTATION_RECTANGLE: result = "Rectangle"; break;
		case ANNOTATION_POLYGON: result = "Polygon"; break;
		case ANNOTATION_SPLINE: result = "Spline"; break;
		case ANNOTATION_POINT: result = "Dot"; break;
	}
	return result;

}

void save_asap_xml_annotations(annotation_set_t* annotation_set, const char* filename_out) {
	ASSERT(annotation_set);
	ASSERT(filename_out);

	// Get an exclusive lock on this annotation_set_t instance (theoretically, a worker thread might also be busy saving)
	// We want this to always succeed in the end, so keep trying until we get it
	while (!atomic_compare_exchange(&annotation_set->is_saving_in_progress, 1, 0)) {
		console_print("save_asap_xml_annotations(): failed to get an exclusive lock on the annotation set, retrying...\n");
		platform_sleep(100);
	}

	FILE* fp = fopen(filename_out, "wb");
	if (fp) {
//		const char* base_tag = "<ASAP_Annotations><Annotations>";

		fprintf(fp, "<ASAP_Annotations>\n");

		fprintf(fp, "<AnnotationGroups>\n");

		for (i32 group_index = 1 /* Skip group 0 ('None') */; group_index < annotation_set->stored_group_count; ++group_index) {
			annotation_group_t* group = annotation_set->stored_groups + group_index;

			char color_buf[32];
			asap_xml_print_color(color_buf, sizeof(color_buf), group->color);

			const char* part_of_group = "None";

			fprintf(fp, "\t<Group Color=\"%s\" Name=\"%s\" PartOfGroup=\"%s\"><Attributes/></Group>\n",
			        color_buf, group->name, part_of_group);

		}

		fprintf(fp, "</AnnotationGroups>\n");

		if (annotation_set->active_feature_count > 0) {
			fprintf(fp, "<AnnotationFeatures>\n");

			for (i32 feature_index = 0; feature_index < annotation_set->active_feature_count; ++feature_index) {
				annotation_feature_t* feature = get_active_annotation_feature(annotation_set, feature_index);
				if (feature->restrict_to_group) {
					annotation_group_t* group = annotation_set->stored_groups + feature->group_id;
					fprintf(fp, "\t<Feature Name=\"%s\" RestrictToGroup=\"%s\"/>\n", feature->name, group->name);
				} else {
					fprintf(fp, "\t<Feature Name=\"%s\"/>\n", feature->name);
				}

			}

			fprintf(fp, "</AnnotationFeatures>\n");
		}

		// Now, write out the annotations (including coordinates and features)
		fprintf(fp, "<Annotations>\n");

		for (i32 annotation_index = 0; annotation_index < annotation_set->active_annotation_count; ++annotation_index) {
			annotation_t* annotation = get_active_annotation(annotation_set, annotation_index);
			char color_buf[32];
			asap_xml_print_color(color_buf, sizeof(color_buf), annotation->color);

			const char* part_of_group = annotation_set->stored_groups[annotation_set->active_group_indices[annotation->group_id]].name;
			const char* type_name = get_annotation_type_name(annotation->type);

			fprintf(fp, "<Annotation Color=\"%s\" Name=\"%s\" PartOfGroup=\"%s\" Type=\"%s\">\n",
			        color_buf, annotation->name, part_of_group, type_name);

			// Write coordinates
			ASSERT(annotation->coordinate_count == arrlen(annotation->coordinates));
			if (annotation->coordinate_count > 0 && annotation->coordinates != NULL) {
				fprintf(fp, "\t<Coordinates>\n");
				for (i32 coordinate_index = 0; coordinate_index < annotation->coordinate_count; ++coordinate_index) {
					v2f* coordinate = annotation->coordinates + coordinate_index;
					fprintf(fp, "\t\t<Coordinate Order=\"%d\" X=\"%g\" Y=\"%g\"/>\n", coordinate_index,
					        coordinate->x / annotation_set->mpp.x, coordinate->y / annotation_set->mpp.y);
				}
				fprintf(fp, "\t</Coordinates>\n");
			}

			// Write features
			if (annotation_set->active_feature_count > 0) {
				i32 features_written = 0;
				for (i32 feature_index = 0; feature_index < annotation_set->active_feature_count; ++feature_index) {
					i32 stored_index = annotation_set->active_feature_indices[feature_index];
					i32 value = annotation->features[stored_index];
					// Only output the feature if the value is nonzero; if the value is absent in the file, zero is implied
					if (value != 0.0f) {
						if (features_written == 0) {
							fprintf(fp, "\t<Features>\n");
						}
						++features_written;
						annotation_feature_t* feature = annotation_set->stored_features + stored_index;
						ASSERT(!feature->deleted);
						ASSERT(stored_index < COUNT(annotation->features));
						fprintf(fp, "\t\t<Feature Name=\"%s\" Value=\"%g\"/>\n", feature->name, annotation->features[stored_index]);
					}

				}
				if (features_written > 0) {
					fprintf(fp, "\t</Features>\n");
				}
			}


			fprintf(fp, "</Annotation>\n");
		}

		fprintf(fp, "</Annotations></ASAP_Annotations>\n");

		fclose(fp);


	}
	annotation_set->is_saving_in_progress = false; // release lock
}
