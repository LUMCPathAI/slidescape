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

#include "common.h"
#include "platform.h"
#include "mrxs.h"
#include "stringutils.h"
#include "listing.h"
#include "image_loader.h"
#include "image_resize.h"
#include "jpeg_decoder.h"
#include "stb_image.h"
#include "miniz.h"

#include <ctype.h> // for isspace()
#include <math.h>



static bool mrxs_source_path(char* path, size_t capacity, const char* directory, const char* name) {
	if (!directory || !name) return false;
	int length = snprintf(path, capacity, "%s%s%s", directory, *directory ? "/" : "", name);
	return length >= 0 && (size_t)length < capacity;
}

static mem_t* mrxs_read_metadata(source_fs_t* source, const char* directory, const char* name) {
	char path[SOURCE_PATH_CAPACITY];
	if (!mrxs_source_path(path, sizeof(path), directory, name)) return NULL;
	/* Parser inputs are resident metadata, unlike tile data. */
	return source_fs_read_entire(source, path, GIGABYTES(1));
}

static void mrxs_slidedat_ini_parse_section_name(mrxs_t* mrxs, char* section_name, enum mrxs_section_enum* section, i32* layer, i32* level) {
    if (strncmp(section_name, "GENERAL", 7) == 0) {
        *section = MRXS_SECTION_GENERAL;
    } else if (strncmp(section_name, "HIERARCHICAL", 12) == 0) {
        *section = MRXS_SECTION_HIERARCHICAL;
    } else if (strncmp(section_name, "DATAFILE", 8) == 0) {
        *section = MRXS_SECTION_DATAFILE;
    } else {

	    // Check which (non)hier/val combination this section belongs to
	    for (i32 i = 0; i < mrxs->hier_count; ++i) {
		    mrxs_hier_t* hier = mrxs->hier + i;
		    if (!hier->is_ini_section_parsed && hier->section && strcmp(section_name, hier->section) == 0) {
			    *section = MRXS_SECTION_LAYER_N_SECTION;
				*layer = i;
				*level = -1;
			    hier->is_ini_section_parsed = true; // assume each section occurs only once, don't check twice
			    return;
		    }
		    for (i32 j = 0; j < hier->val_count; ++j) {
			    mrxs_hier_val_t* hier_val = hier->val + j;
			    if (!hier_val->is_ini_section_parsed && strcmp(section_name, hier_val->section) == 0) {
				    *section = MRXS_SECTION_LAYER_N_LEVEL_N_SECTION;
				    *layer = i;
				    *level = j;
				    hier_val->is_ini_section_parsed = true; // assume each section occurs only once, don't check twice
				    return;
			    }
		    }
	    }
	    for (i32 i = 0; i < mrxs->nonhier_count; ++i) {
		    mrxs_nonhier_t* nonhier = mrxs->nonhier + i;
		    if (!nonhier->is_ini_section_parsed && nonhier->section && strcmp(section_name, nonhier->section) == 0) {
			    *section = MRXS_SECTION_NONHIERLAYER_N_SECTION;
			    *layer = i;
			    *level = -1;
			    nonhier->is_ini_section_parsed = true; // assume each section occurs only once, don't check twice
			    return;
		    }
		    for (i32 j = 0; j < nonhier->val_count; ++j) {
			    mrxs_nonhier_val_t* nonhier_val = nonhier->val + j;
			    if (!nonhier_val->is_ini_section_parsed && strcmp(section_name, nonhier_val->section) == 0) {
				    *section = MRXS_SECTION_NONHIERLAYER_N_LEVEL_N_SECTION;
				    *layer = i;
				    *level = j;
				    nonhier_val->is_ini_section_parsed = true; // assume each section occurs only once, don't check twice
				    return;
			    }
		    }
	    }
    }
}

static inline const char* mrxs_string_pool_push(mrxs_t* mrxs, const char* s) {
   return (char*)mrxs->string_pool.data + memrw_string_pool_push(&mrxs->string_pool, s);
}

static enum mrxs_image_format_enum mrxs_ini_parse_image_format(const char* value) {
    if (strcmp(value, "JPEG") == 0) {
        return MRXS_IMAGE_FORMAT_JPEG;
    } else if (strcmp(value, "PNG") == 0) {
        return MRXS_IMAGE_FORMAT_PNG;
    } else if (strcmp(value, "BMP24") == 0) {
        return MRXS_IMAGE_FORMAT_BMP;
    } else {
        return MRXS_IMAGE_FORMAT_UNKNOWN;
    }
}

bool mrxs_parse_slidedat_ini(mrxs_t* mrxs, mem_t* slidedat_ini) {
    i64 start = get_clock();
	bool success = true;
    mrxs->string_pool = memrw_create(slidedat_ini->len);
    mrxs->string_pool.is_growing_disallowed = true; // we want stable string pointers, so the buffer can't realloc()
    size_t num_lines = 0;
    char** lines = split_into_lines((char*)slidedat_ini->data, &num_lines);
    if (lines) {
        enum mrxs_section_enum section = MRXS_SECTION_UNKNOWN;
        i32 layer = -1;
        i32 level = -1;
		char section_name[128] = "";
        for (i32 line_index = 0; line_index < num_lines; ++line_index) {
            char* line = lines[line_index];
            while (*(u8*)line >= 128) {
                ++line; // skip the first few characters which may be invalid
            }
            if (line[0] == '[') {
				// Section name: strip [ and ] characters from the string, then parse further
				char* pos = line + 1;
				while (isspace(*pos)) {
					++pos;
				}
				size_t len = copy_cstring(section_name, pos, sizeof(section_name));
				if (len > 0) {
					pos = section_name + len - 1;
					while (pos > section_name) {
						if (*pos == ']') {
							*pos = '\0';
							break;
						}
						--pos;
					}
				}
                mrxs_slidedat_ini_parse_section_name(mrxs, section_name, &section, &layer, &level);
            } else {
                char* value = find_next_token(line, '=');
                if (!value) {
                    // TODO: error condition
                    continue;
                }
                char* key = line;
                *(value-1) = '\0'; // remove '=' sign to zero-terminate key
                // strip trailing whitespace after key
                size_t key_len = strlen(key);
                if (key_len >= 1) {
                    for (i32 i = key_len - 1; i >= 0; --i) {
                        if (isspace(key[i])) {
                            key[i] = '\0';
                        } else {
                            break;
                        }
                    }
                }
                // strip preceding whitespace before value
                while(isspace(*value)) {
                    ++value;
                }

                switch(section) {
                    case MRXS_SECTION_GENERAL: {
                        if (strcmp(key, "IMAGENUMBER_X") == 0) {
                            mrxs->base_width_in_tiles = atoi(value);
                        } else if (strcmp(key, "IMAGENUMBER_Y") == 0) {
                            mrxs->base_height_in_tiles = atoi(value);
                        } else if (strcmp(key, "CURRENT_SLIDE_VERSION") == 0) {
                            mrxs->slide_version_major = atoi(value);
                            if (value[1] == '.' && value[2] != '\0') {
                                mrxs->slide_version_minor = atoi(value + 2);
                            }
                        } else if (strcmp(key, "CameraImageDivisionsPerSide") == 0) {
                            mrxs->camera_image_divisions_per_slide = atoi(value);
                        } else if (strcmp(key, "SLIDE_TYPE") == 0) {
                            mrxs->is_fluorescence = (strcmp(value, "SLIDE_TYPE_FLUORESCENCE") == 0);
                        }
                    } break;
                    case MRXS_SECTION_HIERARCHICAL: {
                        if (strcmp(key, "INDEXFILE") == 0) {
                            mrxs->index_dat_filename = mrxs_string_pool_push(mrxs, value);
                        } else if (strcmp(key, "HIER_COUNT") == 0) {
                            mrxs->hier_count = atoi(value);
                            if (mrxs->hier == NULL) {
                                mrxs->hier = calloc(mrxs->hier_count, sizeof(mrxs_hier_t));
                            } else {
                                //TODO: error condition
                            }
                        } else if (strcmp(key, "NONHIER_COUNT") == 0) {
                            mrxs->nonhier_count = atoi(value);
                            if (mrxs->nonhier == NULL) {
                                mrxs->nonhier = calloc(mrxs->nonhier_count, sizeof(mrxs_nonhier_t));
                            } else {
                                //TODO: error condition
                            }
                        } else if (strncmp(key, "HIER_", 5) == 0) {
                            i32 hier_index = atoi(key + 5);
                            if (hier_index >= 0 && hier_index < mrxs->hier_count) {
                                mrxs_hier_t* hier = mrxs->hier + hier_index;
                                char* key_part2 = find_next_token(key + 5, '_');
                                if (strcmp(key_part2, "NAME") == 0) {
                                    if (strcmp(value, "Slide zoom level") == 0) {
                                        mrxs->slide_zoom_level_hier_index = hier_index;
                                        hier->name = MRXS_HIER_SLIDE_ZOOM_LEVEL;
                                    } else if (strcmp(value, "Slide filter level") == 0) {
                                        hier->name = MRXS_HIER_SLIDE_FILTER_LEVEL;
                                    } else if (strcmp(value, "Microscope focus level") == 0) {
                                        hier->name = MRXS_HIER_MICROSCOPE_FOCUS_LEVEL;
                                    } else if (strcmp(value, "Scan info layer") == 0) {
                                        hier->name = MRXS_HIER_SCAN_INFO_LAYER;
                                    }
                                } else if (strcmp(key_part2, "COUNT") == 0) {
                                    hier->val_count = atoi(value);
                                    hier->val = calloc(hier->val_count, sizeof(mrxs_hier_val_t));
                                } else if (strcmp(key_part2, "SECTION") == 0) {
                                    hier->section = mrxs_string_pool_push(mrxs, value);
                                } else if (strncmp(key_part2, "VAL_", 3) == 0) {
                                    i32 val_index = atoi(key_part2 + 4);
                                    if (val_index >= 0 && val_index < hier->val_count) {
                                        mrxs_hier_val_t* hier_val = hier->val + val_index;
                                        char* key_part3 = find_next_token(key_part2 + 4, '_');
                                        if (key_part3 == NULL) {
                                            hier_val->name = mrxs_string_pool_push(mrxs, value);
                                            if (strncmp(value, "ZoomLevel_", 10) == 0) {
                                                hier_val->type = MRXS_HIER_VAL_ZOOMLEVEL;
                                                hier_val->index = atoi(value + 10);
                                                if (hier_val->index >= 0 && hier_val->index < COUNT(mrxs->levels)) {
                                                    mrxs->levels[hier_val->index].hier_val_index = hier_val->index;
                                                }
                                            }
                                        } else if (strcmp(key_part3, "SECTION") == 0) {
                                            hier_val->section = mrxs_string_pool_push(mrxs, value);
                                            if (hier_val->type == MRXS_HIER_VAL_ZOOMLEVEL) {
                                                if (hier_val->index >= 0 && hier_val->index < COUNT(mrxs->levels)) {
                                                    mrxs->levels[hier_val->index].section_name = hier_val->section;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (strncmp(key, "NONHIER_", 8) == 0) {
                            i32 nonhier_index = atoi(key + 8);
                            if (nonhier_index >= 0 && nonhier_index < mrxs->nonhier_count) {
                                mrxs_nonhier_t* nonhier = mrxs->nonhier + nonhier_index;
                                char* key_part2 = find_next_token(key + 8, '_');
                                if (strcmp(key_part2, "NAME") == 0) {
                                    if (strcmp(value, "Scan data layer") == 0) {
                                        nonhier->name = MRXS_NONHIER_SCAN_DATA_LAYER;
                                    } else if (strcmp(value, "StitchingLayer") == 0) {
                                        nonhier->name = MRXS_NONHIER_STITCHING_LAYER;
                                    } else if (strcmp(value, "StitchingIntensityLayer") == 0) {
                                        nonhier->name = MRXS_NONHIER_STITCHING_INTENSITY_LAYER;
                                    } else if (strcmp(value, "VIMSLIDE_HISTOGRAM_DATA") == 0) {
                                        nonhier->name = MRXS_NONHIER_VIMSLIDE_HISTOGRAM_DATA;
                                    }
                                } else if (strcmp(key_part2, "COUNT") == 0) {
                                    nonhier->val_count = atoi(value);
                                    nonhier->val = calloc(nonhier->val_count, sizeof(mrxs_nonhier_val_t));
                                } else if (strcmp(key_part2, "SECTION") == 0) {
                                    nonhier->section = mrxs_string_pool_push(mrxs, value);
                                } else if (strncmp(key_part2, "VAL_", 3) == 0) {
                                    i32 val_index = atoi(key_part2 + 4);
                                    if (val_index >= 0 && val_index < nonhier->val_count) {
                                        mrxs_nonhier_val_t* nonhier_val = nonhier->val + val_index;
                                        char* key_part3 = find_next_token(key_part2 + 4, '_');
                                        if (key_part3 == NULL) {
                                            nonhier_val->name = mrxs_string_pool_push(mrxs, value);
                                            if (nonhier->name == MRXS_NONHIER_SCAN_DATA_LAYER) {
                                                if (strcmp(value, "ScanDataLayer_ScanMap") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_SCANMAP;
                                                } else if (strcmp(value, "ScanDataLayer_XMLInfoHeader") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_XMLINFOHEADER;
                                                } else if (strcmp(value, "ScanDataLayer_SlideThumbnail") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_SLIDETHUMBNAIL;
                                                } else if (strcmp(value, "ScanDataLayer_SlideBarcode") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEBARCODE;
                                                } else if (strcmp(value, "ScanDataLayer_SlidePreview") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEPREVIEW;
                                                } else if (strcmp(value, "ScanDataLayer_StagePositionMap") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_STAGEPOSITIONMAP;
                                                } else if (strcmp(value, "ScanDataLayer_Empty") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANDATALAYER_EMPTY;
                                                } else if (strcmp(value, "ProfileXMLHeader") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_PROFILEXMLHEADER;
                                                } else if (strcmp(value, "ProfileXML") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_PROFILEXML;
                                                } else if (strcmp(value, "ScannedFOVsMap") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_SCANNEDFOVSMAP;
                                                }
                                            } else if (nonhier->name == MRXS_NONHIER_STITCHING_LAYER) {
                                                if (strcmp(value, "DataLevel_V1.0") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_DATALEVEL_V1_0;
                                                }
                                            } else if (nonhier->name == MRXS_NONHIER_STITCHING_INTENSITY_LAYER) {
                                                if (strcmp(value, "StitchingIntensityLevel") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_STITCHING_INTENSITY_LEVEL;
                                                }
                                            } else if (nonhier->name == MRXS_NONHIER_VIMSLIDE_HISTOGRAM_DATA) {
                                                if (strcmp(value, "default") == 0) {
                                                    nonhier_val->type = MRXS_NONHIER_VAL_VIMSLIDE_HISTOGRAM_DATA_DEFAULT;
                                                }
                                            }

                                        } else if (strcmp(key_part3, "SECTION") == 0) {
                                            nonhier_val->section = mrxs_string_pool_push(mrxs, value);
                                        } else if (strcmp(key_part3, "IMAGENUMBER_X") == 0) {
                                            nonhier_val->imagenumber_x = atoi(value);
                                        } else if (strcmp(key_part3, "IMAGENUMBER_Y") == 0) {
                                            nonhier_val->imagenumber_y = atoi(value);
                                        }
                                    }
                                }
                            }
                        }
                    } break;
                    case MRXS_SECTION_DATAFILE: {
                        if (strcmp(key, "FILE_COUNT") == 0) {
                            mrxs->dat_count = atoi(value);
                            ASSERT(mrxs->dat_filenames == NULL);
                            if (mrxs->dat_filenames == NULL) {
                                mrxs->dat_filenames = calloc(mrxs->dat_count, sizeof(char*));
                            } else {
                                //todo: error condition
                            }
                        } else if (strncmp(key, "FILE_", 5) == 0) {
                            i32 file_index = atoi(key + 5);
                            if (file_index >= 0 && file_index < mrxs->dat_count && mrxs->dat_filenames) {
                                ASSERT(mrxs->dat_filenames[file_index] == NULL);
                                mrxs->dat_filenames[file_index] = (char*)mrxs->string_pool.data + memrw_string_pool_push(&mrxs->string_pool, value);
                            }
                        }
                    } break;


					case MRXS_SECTION_LAYER_N_LEVEL_N_SECTION: {
						ASSERT(layer != -1 && level != -1);
						if (layer != -1 && level != -1) {
							mrxs_hier_t* hier = mrxs->hier + layer;
							mrxs_hier_val_t* hier_val = hier->val + level;

							if (hier_val->type == MRXS_HIER_VAL_ZOOMLEVEL) {
								mrxs_level_t* mrxs_level = mrxs->levels + hier_val->index;
								if (strcmp(key, "DIGITIZER_WIDTH") == 0) {
									mrxs_level->tile_width = atoi(value);
								} else if (strcmp(key, "DIGITIZER_HEIGHT") == 0) {
									mrxs_level->tile_height = atoi(value);
								} else if (strcmp(key, "OVERLAP_X") == 0) {
                                    mrxs_level->overlap_x = atof(value);
                                } else if (strcmp(key, "OVERLAP_Y") == 0) {
                                    mrxs_level->overlap_y = atof(value);
                                } else if (strcmp(key, "MICROMETER_PER_PIXEL_X") == 0) {
									mrxs_level->um_per_pixel_x = atof(value);
								} else if (strcmp(key, "MICROMETER_PER_PIXEL_Y") == 0) {
									mrxs_level->um_per_pixel_y = atof(value);
								} else if (strcmp(key, "IMAGE_FILL_COLOR_BGR") == 0) {
									mrxs_level->image_fill_color_bgr = atoi(value);
								} else if (strcmp(key, "IMAGE_FORMAT") == 0) {
                                    mrxs_level->image_format = mrxs_ini_parse_image_format(value);
								}
							}
						}
					} break;

                    case MRXS_SECTION_NONHIERLAYER_N_LEVEL_N_SECTION: {
                        ASSERT(layer != -1 && level != -1);
                        if (layer != -1 && level != -1) {
                            mrxs_nonhier_t* nonhier = mrxs->nonhier + layer;
                            mrxs_nonhier_val_t* nonhier_val = nonhier->val + level;

                            if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SCANMAP) {
                                if (strcmp(key, "SCANMAP_IMAGE_TYPE") == 0) {
                                    mrxs->scanmap_image.format = mrxs_ini_parse_image_format(value);
                                } else if (strcmp(key, "SCANMAP_IMAGE_WIDTH") == 0) {
                                    mrxs->scanmap_image.width = atoi(value);
                                } else if (strcmp(key, "SCANMAP_IMAGE_HEIGHT") == 0) {
                                    mrxs->scanmap_image.height = atof(value);
                                }
                            } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_STAGEPOSITIONMAP) {
                                if (strcmp(key, "STAGEPOSMAP_IMAGE_TYPE") == 0) {
                                    mrxs->stageposmap_image.format = mrxs_ini_parse_image_format(value);
                                } else if (strcmp(key, "STAGEPOSMAP_IMAGE_WIDTH") == 0) {
                                    mrxs->stageposmap_image.width = atoi(value);
                                } else if (strcmp(key, "STAGEPOSMAP_IMAGE_HEIGHT") == 0) {
                                    mrxs->stageposmap_image.height = atof(value);
                                }
                            } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SLIDETHUMBNAIL) {
                                if (strcmp(key, "THUMBNAIL_IMAGE_TYPE") == 0) {
                                    mrxs->thumbnail_image.format = mrxs_ini_parse_image_format(value);
                                } else if (strcmp(key, "THUMBNAIL_IMAGE_WIDTH") == 0) {
                                    mrxs->thumbnail_image.width = atoi(value);
                                } else if (strcmp(key, "THUMBNAIL_IMAGE_HEIGHT") == 0) {
                                    mrxs->thumbnail_image.height = atof(value);
                                }
                            } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEBARCODE) {
                                if (strcmp(key, "BARCODE_IMAGE_TYPE") == 0) {
                                    mrxs->barcode_image.format = mrxs_ini_parse_image_format(value);
                                } else if (strcmp(key, "BARCODE_IMAGE_WIDTH") == 0) {
                                    mrxs->barcode_image.width = atoi(value);
                                } else if (strcmp(key, "BARCODE_IMAGE_HEIGHT") == 0) {
                                    mrxs->barcode_image.height = atof(value);
                                }
                            }
                        }
                    } break;

                    case MRXS_SECTION_UNKNOWN:
                    default: {

                    } break;
                }
            }
        }
        free(lines);
    }
    if (!mrxs->index_dat_filename || mrxs->dat_count == 0 || !mrxs->dat_filenames) {
        success = false;
    }

	mrxs_level_t* base_level = mrxs->levels + 0;
	if (base_level->um_per_pixel_x > 0.0f && base_level->um_per_pixel_y > 0.0f) {
		mrxs->is_mpp_known = true;
		mrxs->mpp_x = base_level->um_per_pixel_x;
		mrxs->mpp_y = base_level->um_per_pixel_y;
	} else {
		mrxs->is_mpp_known = false;
		mrxs->mpp_x = 1.0f;
		mrxs->mpp_y = 1.0f;
	}
	mrxs->tile_width = base_level->tile_width;
	mrxs->tile_height = base_level->tile_height;

	console_print_verbose("Parsing Slidedat.ini took %g seconds.\n", get_seconds_elapsed(start, get_clock()));

    return success;
}

bool mrxs_read_index_dat_slide_zoom_level(mrxs_t* mrxs, mem_t* index_dat, mrxs_hier_val_t* hier_val) {
	i32 scale = hier_val->index;
	if (!(scale >= 0 && scale < mrxs->level_count)) {
		return false;
	}
	mrxs_level_t* level = mrxs->levels + scale;

	i32 page_count = 0; // the first page doesn't count, it always has 0 entries
	for (;;) {
		u32 entry_count = 0;
		u32 next_ptr = 0;
		mem_read(&entry_count, index_dat, 4);
		mem_read(&next_ptr, index_dat, 4);
		if (entry_count > 0) {
			// read entries
			for (i32 j = 0; j < entry_count; ++j) {
				mrxs_hier_entry_t entry = {};
				mem_read(&entry, index_dat, sizeof(entry));
				i32 tile_index_x = entry.image % mrxs->base_width_in_tiles;
				i32 tile_index_y = entry.image / mrxs->base_width_in_tiles;
				tile_index_x >>= scale;
				tile_index_y >>= scale;
				if (tile_index_x < level->width_in_tiles && tile_index_y < level->height_in_tiles) {
					mrxs_tile_t* tile = level->tiles + tile_index_y * level->width_in_tiles + tile_index_x;
					tile->hier_entry = entry;
					DUMMY_STATEMENT;
				} else {
					DUMMY_STATEMENT;
				}
				DUMMY_STATEMENT;
			}
		}
		if (next_ptr != 0 && next_ptr < index_dat->len) {
			mem_seek(index_dat, next_ptr);
			++page_count;
		} else {
			break; // last page reached
		}
	}
	return true;
}

bool mrxs_read_stitching_intensity_level(mrxs_t* mrxs, mem_t* index_dat, mrxs_nonhier_val_t* nonhier_val) {
    i32 page_count = 0; // the first page doesn't count, it always has 0 entries
    for (;;) {
        u32 entry_count = 0;
        u32 next_ptr = 0;
        mem_read(&entry_count, index_dat, 4);
        mem_read(&next_ptr, index_dat, 4);
        if (entry_count > 0) {
            // read entries
            for (i32 j = 0; j < entry_count; ++j) {
                mrxs_nonhier_entry_t entry = {};
                mem_read(&entry, index_dat, sizeof(entry));
                mrxs->stitching_intensity_layer_entry = entry;
                DUMMY_STATEMENT;
                return true; // only one (relevant) entry is expected
            }
        }
        if (next_ptr != 0 && next_ptr < index_dat->len) {
            mem_seek(index_dat, next_ptr);
            ++page_count;
        } else {
            break; // last page reached
        }
    }
    return false;
}

bool mrxs_read_nonhier_first_entry(mem_t* index_dat, mrxs_nonhier_entry_t* out_entry) {
    for (;;) {
        u32 entry_count = 0;
        u32 next_ptr = 0;
        mem_read(&entry_count, index_dat, 4);
        mem_read(&next_ptr, index_dat, 4);
        if (entry_count > 0) {
            mem_read(out_entry, index_dat, sizeof(*out_entry));
            return true;
        }
        if (next_ptr != 0 && next_ptr < index_dat->len) {
            mem_seek(index_dat, next_ptr);
        } else {
            break;
        }
    }
    return false;
}

bool mrxs_load_slide_position_file(mrxs_t* mrxs) {
    mrxs_nonhier_entry_t entry = mrxs->stitching_intensity_layer_entry;
    bool success = false;
    if (entry.length == 0 || !mrxs->dat_files || entry.file >= (u32)mrxs->dat_count) {
        return false;
    }
    const source_file_t* file = mrxs->dat_files + entry.file;
    if (file->blob) {
        temp_memory_t temp = begin_temp_memory_on_local_thread();
        u8* compressed_data = (u8*)arena_push_size(temp.arena, entry.length);
        source_read_result_t read = source_file_read(file, entry.offset, compressed_data, entry.length);
        if (read == SOURCE_READ_OK) {
            size_t out_len = 0;
            int flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
            mrxs_slide_position_t* position_file = tinfl_decompress_mem_to_heap(compressed_data, entry.length, &out_len, flags);
            ASSERT(sizeof(mrxs_slide_position_t) == 9);
            if (out_len % sizeof(mrxs_slide_position_t) == 0) {
                mrxs->camera_positions = position_file;
                mrxs->camera_position_count = out_len / sizeof(mrxs_slide_position_t);
                success = true;
            } else {
                libc_free(position_file); // error: length not a multiple
            }
        }
        release_temp_memory(&temp);
    }

    /*if (success) {
        for (i32 i = 0; i < mrxs->camera_position_count; ++i) {
            mrxs_slide_position_t position = mrxs->camera_positions[i];
            printf("camera position %d: flag=%d, x=%d, y=%d\n", i, position.flag, position.x, position.y);
        }
    }*/

    return success;
}

static inline i32 mrxs_ceil_div_i32(i32 a, i32 b) {
    return (a + b - 1) / b;
}

static inline bool mrxs_rects_overlap_f32(float a_left, float a_top, float a_right, float a_bottom,
                                          float b_left, float b_top, float b_right, float b_bottom) {
    return a_left < b_right && a_right > b_left && a_top < b_bottom && a_bottom > b_top;
}

static void mrxs_fill_bgra(u8* pixels, i32 width, i32 height, u32 fill_color_bgr) {
    u8 b = (u8)(fill_color_bgr & 0xff);
    u8 g = (u8)((fill_color_bgr >> 8) & 0xff);
    u8 r = (u8)((fill_color_bgr >> 16) & 0xff);
    for (i32 i = 0; i < width * height; ++i) {
        pixels[i * 4 + 0] = b;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = r;
        pixels[i * 4 + 3] = 255;
    }
}

static bool mrxs_prepare_overlap_geometry(mrxs_t* mrxs) {
    if (!mrxs->has_overlapping_tiles || !mrxs->camera_positions || mrxs->camera_position_count <= 0) {
        return false;
    }
    i32 divisions = mrxs->camera_image_divisions_per_slide;
    mrxs_level_t* base_level = mrxs->levels + 0;
    if (divisions <= 0 || base_level->tile_width <= 0 || base_level->tile_height <= 0) {
        return false;
    }

    mrxs->camera_grid_width = mrxs_ceil_div_i32(mrxs->base_width_in_tiles, divisions);
    mrxs->camera_grid_height = mrxs_ceil_div_i32(mrxs->base_height_in_tiles, divisions);
    i32 camera_count = mrxs->camera_grid_width * mrxs->camera_grid_height;
    if (camera_count <= 0 || mrxs->camera_position_count < camera_count) {
        return false;
    }

    mrxs->camera_width = divisions * base_level->tile_width;
    mrxs->camera_height = divisions * base_level->tile_height;
    mrxs->cameras = calloc(camera_count, sizeof(mrxs_camera_t));
    if (!mrxs->cameras) {
        return false;
    }

    bool version_has_presence_flag = (mrxs->slide_version_major > 1 ||
                                      (mrxs->slide_version_major == 1 && mrxs->slide_version_minor >= 9));
    i32 min_x = INT32_MAX;
    i32 min_y = INT32_MAX;
    i32 max_x = INT32_MIN;
    i32 max_y = INT32_MIN;
    i32 present_count = 0;
    for (i32 i = 0; i < camera_count; ++i) {
        mrxs_slide_position_t position = mrxs->camera_positions[i];
        bool present = version_has_presence_flag ? (position.flag != 0) : true;
        mrxs_camera_t* camera = mrxs->cameras + i;
        camera->present = present;
        camera->raw_x = position.x;
        camera->raw_y = position.y;
        if (present) {
            min_x = MIN(min_x, position.x);
            min_y = MIN(min_y, position.y);
            max_x = MAX(max_x, position.x + mrxs->camera_width);
            max_y = MAX(max_y, position.y + mrxs->camera_height);
            ++present_count;
        }
    }

    if (present_count == 0) {
        return false;
    }

    mrxs->camera_origin_x = min_x;
    mrxs->camera_origin_y = min_y;
    for (i32 i = 0; i < camera_count; ++i) {
        mrxs_camera_t* camera = mrxs->cameras + i;
        camera->x = camera->raw_x - min_x;
        camera->y = camera->raw_y - min_y;
    }

    mrxs->base_width_in_pixels = max_x - min_x;
    mrxs->base_height_in_pixels = max_y - min_y;
    mrxs->max_aligned_overlap_level = 0;
    while (((1 << (mrxs->max_aligned_overlap_level + 1)) <= divisions) &&
           ((divisions % (1 << (mrxs->max_aligned_overlap_level + 1))) == 0)) {
        ++mrxs->max_aligned_overlap_level;
    }

    console_print_verbose("MRXS overlap geometry: %d/%d camera positions present, logical size %dx%d px, aligned through level %d\n",
                          present_count, camera_count, mrxs->base_width_in_pixels, mrxs->base_height_in_pixels,
                          mrxs->max_aligned_overlap_level);
    return true;
}

bool mrxs_open_from_source(mrxs_t* mrxs, source_fs_t* source, const char* directory) {
    bool success = false;
	mrxs->source = source;
	if (!source || !directory) return false;

	i64 start = get_clock();
    mem_t* slidedat_ini = mrxs_read_metadata(source, directory, "Slidedat.ini");

    if (slidedat_ini) {
        if (mrxs_parse_slidedat_ini(mrxs, slidedat_ini)) {
            mem_t* index_dat = mrxs_read_metadata(source, directory, mrxs->index_dat_filename);
	        if (index_dat) {
		        i64 index_parse_start = get_clock();
		        char version[6] = {};
                char slide_id[33] = {};
                u32 hier_root = 0;
                u32 nonhier_root = 0;
                mem_read(version, index_dat, 5);
                mem_read(slide_id, index_dat, 32);
                mem_read(&hier_root, index_dat, 4);
                mem_read(&nonhier_root, index_dat, 4);

                bool failed = false;
                if (hier_root > 0 && hier_root < index_dat->len) {

					// Initialize some basic stuff
					mrxs_hier_t* hier_zoom_levels = mrxs->hier + mrxs->slide_zoom_level_hier_index;
					mrxs->level_count = hier_zoom_levels->val_count;
					for (i32 i = 0; i < mrxs->level_count; ++i) {
						mrxs_level_t* level = mrxs->levels + i;
						level->width_in_tiles = (mrxs->base_width_in_tiles + (1 << i) - 1) >> i;
						level->height_in_tiles = (mrxs->base_height_in_tiles + (1 << i) - 1) >> i;
						u32 tile_count = level->width_in_tiles * level->height_in_tiles;
						level->tiles = calloc(tile_count, sizeof(mrxs_tile_t ));
					}

                    // Additional initialization to deal with slides with overlapping images
                    mrxs_level_t* base_level = mrxs->levels + 0;
                    mrxs->has_overlapping_tiles = (base_level->overlap_x != 0.0f || base_level->overlap_y != 0.0f);
                    if (mrxs->has_overlapping_tiles) {
                        // Calculate the actual width and height of the image
                        float base_width = 0.0f;
                        for (i32 i = 0; i < base_level->width_in_tiles; ++i) {
                            base_width += base_level->tile_width;
                            if ((i + 1) % mrxs->camera_image_divisions_per_slide == 0 && i != base_level->width_in_tiles - 1) {
                                base_width -= base_level->overlap_x;
                            }
                        }
                        float base_height = 0.0f;
                        for (i32 i = 0; i < base_level->height_in_tiles; ++i) {
                            base_height += base_level->tile_height;
                            if ((i + 1) % mrxs->camera_image_divisions_per_slide == 0 && i != base_level->height_in_tiles - 1) {
                                base_height -= base_level->overlap_y;
                            }
                        }
                        mrxs->base_width_in_pixels = roundf(base_width);
                        mrxs->base_height_in_pixels = roundf(base_height);

                    } else {
                        mrxs->base_width_in_pixels = base_level->width_in_tiles * base_level->tile_width;
                        mrxs->base_height_in_pixels = base_level->height_in_tiles * base_level->tile_height;
                    }

	                // There is one record stored for each HIER_i_VAL_j combination (all in a flat array)
					i32 record_index = 0;
	                for (i32 hier_index = 0; hier_index < mrxs->hier_count; ++hier_index) {
		                mrxs_hier_t* hier = mrxs->hier + hier_index;
		                for (i32 val_index = 0; val_index < hier->val_count; ++val_index, ++record_index) {
			                mrxs_hier_val_t* hier_val = hier->val + val_index;

			                mem_seek(index_dat, hier_root + record_index * sizeof(u32));
			                u32 record_ptr = 0;
			                mem_read(&record_ptr, index_dat, 4);
			                mem_seek(index_dat, record_ptr);
							if (hier->name == MRXS_HIER_SLIDE_ZOOM_LEVEL && hier_val->type == MRXS_HIER_VAL_ZOOMLEVEL) {
								if (!mrxs_read_index_dat_slide_zoom_level(mrxs, index_dat, hier_val)) {
									goto label_failed;
								}
							}

		                }
	                }

                } else {
					label_failed:
					failed = true;
                    // TODO: error condition
                }

                if (!failed && nonhier_root > 0 && nonhier_root < index_dat->len) {
                    mem_seek(index_dat, nonhier_root);

                    // There is one record stored for each HIER_i_VAL_j combination (all in a flat array)
                    i32 record_index = 0;
                    for (i32 nonhier_index = 0; nonhier_index < mrxs->nonhier_count; ++nonhier_index) {
                        mrxs_nonhier_t* nonhier = mrxs->nonhier + nonhier_index;
                        for (i32 val_index = 0; val_index < nonhier->val_count; ++val_index, ++record_index) {
                            mrxs_nonhier_val_t* nonhier_val = nonhier->val + val_index;

                            mem_seek(index_dat, nonhier_root + record_index * sizeof(u32));
                            u32 record_ptr = 0;
                            mem_read(&record_ptr, index_dat, 4);
                            mem_seek(index_dat, record_ptr);
                            if (nonhier->name == MRXS_NONHIER_STITCHING_INTENSITY_LAYER && nonhier_val->type == MRXS_NONHIER_VAL_STITCHING_INTENSITY_LEVEL) {
                                if (!mrxs_read_stitching_intensity_level(mrxs, index_dat, nonhier_val)) {
                                    goto label_failed;
                                }
                            } else if (nonhier->name == MRXS_NONHIER_SCAN_DATA_LAYER) {
                                mrxs_simple_image_t* image = NULL;
                                if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SCANMAP) {
                                    image = &mrxs->scanmap_image;
                                } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_STAGEPOSITIONMAP) {
                                    image = &mrxs->stageposmap_image;
                                } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SLIDETHUMBNAIL) {
                                    image = &mrxs->thumbnail_image;
                                } else if (nonhier_val->type == MRXS_NONHIER_VAL_SCANDATALAYER_SLIDEBARCODE) {
                                    image = &mrxs->barcode_image;
                                }
                                if (image) {
                                    mrxs_read_nonhier_first_entry(index_dat, &image->entry);
                                }
                            }

                        }
                    }

					success = true;
                } else {
                    failed = true;
                    // TODO: error condition
                }

                free(index_dat);

		        console_print_verbose("Parsing index took %g seconds.\n", get_seconds_elapsed(index_parse_start, get_clock()));
            }
        }
        free(slidedat_ini);
    }
	i64 clock_index_loaded = get_clock();
	console_print_verbose("Slidedat.ini and index loaded in %g seconds.\n", get_seconds_elapsed(start, clock_index_loaded));

	if (success) {
		ASSERT(mrxs->dat_filenames && mrxs->dat_count > 0);
		mrxs->dat_files = calloc((size_t)mrxs->dat_count, sizeof(*mrxs->dat_files));
		if (!mrxs->dat_files) return false;
		for (i32 i = 0; i < mrxs->dat_count; ++i) {
			char path[SOURCE_PATH_CAPACITY];
			if (!mrxs_source_path(path, sizeof(path), directory, mrxs->dat_filenames[i]) ||
				!source_fs_open_file(source, path, mrxs->dat_files + i)) {
				success = false;
				console_print_error("Could not open MRXS data member: %s\n", mrxs->dat_filenames[i]);
				break;
			}
		}

        console_print_verbose("Opening file handles to %d dat files took %g seconds.\n", mrxs->dat_count, get_seconds_elapsed(clock_index_loaded, get_clock()));

        if (success && mrxs->has_overlapping_tiles) {
            // Load the slide position file
            if (mrxs_load_slide_position_file(mrxs)) {
                mrxs_prepare_overlap_geometry(mrxs);
            }
        }


    }

    return success;
}

bool mrxs_open_from_directory(mrxs_t* mrxs, file_info_t* file, directory_info_t* directory) {
	(void)directory;
	return mrxs_open_from_source(mrxs, source_fs_open_directory(file->full_filename), "");
}

typedef struct mrxs_discovery_t {
	source_fs_t* source;
	char directory[SOURCE_PATH_CAPACITY];
	u32 count;
} mrxs_discovery_t;

static bool mrxs_discover_entry(void* context, const source_entry_t* entry) {
	mrxs_discovery_t* discovery = context;
	if (entry->is_directory) return source_fs_list(discovery->source, entry->path, mrxs_discover_entry, discovery);
	const char* leaf = strrchr(entry->path, '/');
	leaf = leaf ? leaf + 1 : entry->path;
	if (!strcasecmp(leaf, "Slidedat.ini")) {
		++discovery->count;
		size_t length = leaf == entry->path ? 0 : (size_t)(leaf - entry->path - 1);
		memcpy(discovery->directory, entry->path, length);
		discovery->directory[length] = 0;
	}
	return discovery->count <= 1;
}

bool mrxs_open_from_zip(mrxs_t* mrxs, const char* filename, char* error, size_t capacity) {
	source_fs_t* source = source_fs_open_zip(filename, error, capacity);
	if (!source) return false;
	mrxs_discovery_t discovery = {0};
	discovery.source = source;
	bool scanned = source_fs_list(source, "", mrxs_discover_entry, &discovery);
	if (!scanned || discovery.count != 1) {
		if (error && capacity) snprintf(error, capacity, "%s", discovery.count > 1 ?
			"ZIP contains multiple MRXS slides; selecting a slide is not supported yet" :
			"ZIP does not contain a discoverable MRXS slide (Slidedat.ini)");
		source_fs_close(source);
		return false;
	}
	bool success = mrxs_open_from_source(mrxs, source, discovery.directory);
	if (!success && error && capacity) snprintf(error, capacity, "Could not open MRXS members in ZIP");
	return success;
}

u8* mrxs_decode_image_to_bgra(u8* compressed_data, size_t compressed_length, enum mrxs_image_format_enum image_format,
        i32 expected_width, i32 expected_height) {
    u8* result = NULL;
    if (image_format == MRXS_IMAGE_FORMAT_JPEG) {
        // JPEG compression
        i32 width = 0;
        i32 height = 0;
        i32 channels_in_file = 0;
        u8* pixels = jpeg_decode_image(compressed_data, compressed_length, &width, &height, &channels_in_file);
        if (pixels && width == expected_width && height == expected_height && channels_in_file == 4) {
            // success
            result = pixels;
        } else {
            if (!pixels) {
                console_print_error("mrxs_decode_tile_to_bgra(): JPEG decoding error\n");
            } else if (width != expected_width && height != expected_height) {
                console_print_error("mrxs_decode_tile_to_bgra(): unexpected tile width/height; expected (%d, %d), got (%d, %d)\n", expected_width, width, expected_height, height);
            } else if (channels_in_file != 4) {
                console_print_error("mrxs_decode_tile_to_bgra(): expected JPEG channels in file to be 4, got %d\n", channels_in_file);
            }
            if (pixels) free(pixels);
            result = NULL;
        }
    } else if (image_format == MRXS_IMAGE_FORMAT_BMP || image_format == MRXS_IMAGE_FORMAT_PNG) {
        // Decode image using stb_image.h
        i32 width = 0;
        i32 height = 0;
        i32 channels_in_file = 0;
        u32* pixels = (u32*)stbi_load_from_memory(compressed_data, compressed_length, &width, &height, &channels_in_file, 4);
        if (pixels && width == expected_width && height == expected_height) {
            // success
            // swap RGBA to BGRA
            i32 pixel_count = width * height;
            for (i32 i = 0; i < pixel_count; ++i) {
                u32 pixel = pixels[i];
#if HOST_BIG_ENDIAN
                u32 new_pixel = (pixel & 0x00FF00FF) | ((pixel >> 16) & 0x0000FF00) | ((pixel << 16) & 0xFF000000);
#else
                u32 new_pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
#endif
                pixels[i] = new_pixel;
            }
            result = (u8*)pixels;
        } else {
            if (!pixels) {
                console_print_error("mrxs_decode_image_to_bgra(): decoding error in stbi_load_from_memory(), image format = %d\n", image_format);
            } else if (width != expected_width && height != expected_height) {
                console_print_error("mrxs_decode_image_to_bgra(): unexpected tile width/height; expected (%d, %d), got (%d, %d)\n", expected_width, width, expected_height, height);
            }
            if (pixels) free(pixels);
            result = NULL;
        }
    } else {
        console_print_error("mrxs_decode_image_to_bgra(): unknown or unsupported image format: %d\n", image_format);
    }
    return result;
}

u8* mrxs_decode_simple_image_to_rgba(mrxs_t* mrxs, mrxs_simple_image_t* image) {
    if (!mrxs || !image || image->entry.length == 0 || !mrxs->dat_files || image->entry.file >= mrxs->dat_count ||
        image->width <= 0 || image->height <= 0) {
        return NULL;
    }

    u8* result = NULL;
    const source_file_t* file = mrxs->dat_files + image->entry.file;
    if (file->blob) {
        temp_memory_t temp = begin_temp_memory_on_local_thread();
        u8* compressed_data = (u8*)arena_push_size(temp.arena, image->entry.length);
        source_read_result_t read = source_file_read(file, image->entry.offset, compressed_data, image->entry.length);
        if (read == SOURCE_READ_OK) {
            i32 width = 0;
            i32 height = 0;
            i32 channels_in_file = 0;
            result = stbi_load_from_memory(compressed_data, image->entry.length, &width, &height, &channels_in_file, 4);
            if (!result || width != image->width || height != image->height) {
                if (result) {
                    stbi_image_free(result);
                    result = NULL;
                }
            }
        }
        release_temp_memory(&temp);
    }
    return result;
}

static u8* mrxs_decode_stored_tile_to_bgra(mrxs_t* mrxs, i32 level, i32 stored_tile_index) {
    u8* result = NULL;
    if (!(level >= 0 && level < mrxs->level_count)) {
        return NULL;
    }
    mrxs_level_t* mrxs_level = mrxs->levels + level;
    if (stored_tile_index >= 0 && stored_tile_index < mrxs_level->width_in_tiles * mrxs_level->height_in_tiles) {
        mrxs_tile_t* tile = mrxs_level->tiles + stored_tile_index;
        mrxs_hier_entry_t hier_entry = tile->hier_entry;
        if (mrxs->dat_files && hier_entry.length > 0 && hier_entry.file < mrxs->dat_count) {
            const source_file_t* file = mrxs->dat_files + hier_entry.file;
            if (file->blob) {
                temp_memory_t temp = begin_temp_memory_on_local_thread();
                u8* compressed_tile_data = (u8*)arena_push_size(temp.arena, hier_entry.length);
                source_read_result_t read = source_file_read(file, hier_entry.offset, compressed_tile_data, hier_entry.length);
                if (read == SOURCE_READ_OK) {
                    result = mrxs_decode_image_to_bgra(compressed_tile_data, hier_entry.length, mrxs_level->image_format,
                                                       mrxs_level->tile_width, mrxs_level->tile_height);
                }
                release_temp_memory(&temp);
            }
        }
    }
    return result;
}

void mrxs_init_runtime_cache(mrxs_t* mrxs) {
    if (!mrxs->decoded_tile_cache_mutex_initialized) {
        platform_mutex_init(&mrxs->decoded_tile_cache_mutex);
        mrxs->decoded_tile_cache_mutex_initialized = true;
    }
}

static void mrxs_decoded_tile_cache_evict_one(mrxs_t* mrxs) {
    i32 oldest_index = -1;
    u64 oldest_tick = UINT64_MAX;
    for (i32 i = 0; i < mrxs->decoded_tile_cache_count; ++i) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + i;
        if (entry->refcount == 0 && entry->last_used < oldest_tick) {
            oldest_tick = entry->last_used;
            oldest_index = i;
        }
    }
    if (oldest_index >= 0) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + oldest_index;
        free(entry->pixels);
        *entry = mrxs->decoded_tile_cache_entries[mrxs->decoded_tile_cache_count - 1];
        --mrxs->decoded_tile_cache_count;
    }
}

static u8* mrxs_acquire_cached_stored_tile(mrxs_t* mrxs, i32 level, i32 stored_tile_index) {
    if (!mrxs->decoded_tile_cache_mutex_initialized) {
        return mrxs_decode_stored_tile_to_bgra(mrxs, level, stored_tile_index);
    }

    platform_mutex_lock(&mrxs->decoded_tile_cache_mutex);
    for (i32 i = 0; i < mrxs->decoded_tile_cache_count; ++i) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + i;
        if (entry->level == level && entry->stored_tile_index == stored_tile_index) {
            ++entry->refcount;
            entry->last_used = ++mrxs->decoded_tile_cache_tick;
            u8* pixels = entry->pixels;
            platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
            return pixels;
        }
    }
    platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);

    u8* pixels = mrxs_decode_stored_tile_to_bgra(mrxs, level, stored_tile_index);
    if (!pixels) {
        return NULL;
    }

    platform_mutex_lock(&mrxs->decoded_tile_cache_mutex);
    for (i32 i = 0; i < mrxs->decoded_tile_cache_count; ++i) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + i;
        if (entry->level == level && entry->stored_tile_index == stored_tile_index) {
            ++entry->refcount;
            entry->last_used = ++mrxs->decoded_tile_cache_tick;
            u8* cached_pixels = entry->pixels;
            platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
            free(pixels);
            return cached_pixels;
        }
    }

    i32 max_entries = 256;
    if (mrxs->decoded_tile_cache_count >= max_entries) {
        mrxs_decoded_tile_cache_evict_one(mrxs);
    }

    if (mrxs->decoded_tile_cache_count >= mrxs->decoded_tile_cache_capacity) {
        i32 new_capacity = mrxs->decoded_tile_cache_capacity > 0 ? mrxs->decoded_tile_cache_capacity * 2 : 64;
        new_capacity = MIN(new_capacity, max_entries);
        mrxs_decoded_tile_cache_entry_t* new_entries = realloc(mrxs->decoded_tile_cache_entries,
                                                               new_capacity * sizeof(*new_entries));
        if (!new_entries) {
            platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
            free(pixels);
            return NULL;
        }
        mrxs->decoded_tile_cache_entries = new_entries;
        mrxs->decoded_tile_cache_capacity = new_capacity;
    }

    if (mrxs->decoded_tile_cache_count < mrxs->decoded_tile_cache_capacity) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + mrxs->decoded_tile_cache_count++;
        entry->level = level;
        entry->stored_tile_index = stored_tile_index;
        entry->pixels = pixels;
        entry->refcount = 1;
        entry->last_used = ++mrxs->decoded_tile_cache_tick;
        platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
        return pixels;
    }

    platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
    return pixels;
}

static void mrxs_release_cached_stored_tile(mrxs_t* mrxs, i32 level, i32 stored_tile_index, u8* pixels) {
    if (!pixels) {
        return;
    }
    if (!mrxs->decoded_tile_cache_mutex_initialized) {
        free(pixels);
        return;
    }

    platform_mutex_lock(&mrxs->decoded_tile_cache_mutex);
    for (i32 i = 0; i < mrxs->decoded_tile_cache_count; ++i) {
        mrxs_decoded_tile_cache_entry_t* entry = mrxs->decoded_tile_cache_entries + i;
        if (entry->level == level && entry->stored_tile_index == stored_tile_index && entry->pixels == pixels) {
            ASSERT(entry->refcount > 0);
            --entry->refcount;
            entry->last_used = ++mrxs->decoded_tile_cache_tick;
            platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
            return;
        }
    }
    platform_mutex_unlock(&mrxs->decoded_tile_cache_mutex);
    free(pixels);
}

static void mrxs_copy_patch_to_output(u8* output, i32 output_width, u8* patch, i32 patch_width, i32 patch_height,
                                      i32 dst_x, i32 dst_y) {
    for (i32 y = 0; y < patch_height; ++y) {
        u8* dst = output + ((dst_y + y) * output_width + dst_x) * 4;
        u8* src = patch + y * patch_width * 4;
        memcpy(dst, src, patch_width * 4);
    }
}

static void mrxs_composite_decoded_tile(u8* output, i32 output_width, i32 output_height,
                                        u8* decoded_tile, i32 src_width, i32 src_height,
                                        float src_global_x, float src_global_y,
                                        float out_global_x, float out_global_y) {
    float src_left = src_global_x;
    float src_top = src_global_y;
    float src_right = src_global_x + (float)src_width;
    float src_bottom = src_global_y + (float)src_height;
    float out_left = out_global_x;
    float out_top = out_global_y;
    float out_right = out_global_x + (float)output_width;
    float out_bottom = out_global_y + (float)output_height;

    if (!mrxs_rects_overlap_f32(src_left, src_top, src_right, src_bottom, out_left, out_top, out_right, out_bottom)) {
        return;
    }

    i32 dst_global_left = MAX((i32)floorf(MAX(src_left, out_left)), (i32)out_global_x);
    i32 dst_global_top = MAX((i32)floorf(MAX(src_top, out_top)), (i32)out_global_y);
    i32 dst_global_right = MIN((i32)ceilf(MIN(src_right, out_right)), (i32)(out_global_x + output_width));
    i32 dst_global_bottom = MIN((i32)ceilf(MIN(src_bottom, out_bottom)), (i32)(out_global_y + output_height));
    i32 dst_w = dst_global_right - dst_global_left;
    i32 dst_h = dst_global_bottom - dst_global_top;
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    i32 dst_x = dst_global_left - (i32)out_global_x;
    i32 dst_y = dst_global_top - (i32)out_global_y;
    float src_box_x = (float)dst_global_left - src_global_x;
    float src_box_y = (float)dst_global_top - src_global_y;

    bool integral_copy = fabsf(src_box_x - roundf(src_box_x)) < 0.001f &&
                         fabsf(src_box_y - roundf(src_box_y)) < 0.001f;
    if (integral_copy) {
        i32 src_x = (i32)roundf(src_box_x);
        i32 src_y = (i32)roundf(src_box_y);
        if (src_x >= 0 && src_y >= 0 && src_x + dst_w <= src_width && src_y + dst_h <= src_height) {
            for (i32 y = 0; y < dst_h; ++y) {
                u8* dst = output + ((dst_y + y) * output_width + dst_x) * 4;
                u8* src = decoded_tile + ((src_y + y) * src_width + src_x) * 4;
                memcpy(dst, src, dst_w * 4);
            }
            return;
        }
    }

    image_buffer_t src = {
        .pixels = decoded_tile,
        .channels = 4,
        .width = src_width,
        .height = src_height,
        .stride_in_pixels = src_width,
        .stride_in_bytes = src_width * 4,
        .pixel_format = PIXEL_FORMAT_U8_BGRA,
        .is_valid = true,
    };
    image_buffer_t patch = create_bgra_image_buffer(dst_w, dst_h);
    if (patch.is_valid) {
        rect2f box = RECT2F(src_box_x, src_box_y, (float)dst_w, (float)dst_h);
        if (image_resample_lanczos3(&src, &patch, box)) {
            mrxs_copy_patch_to_output(output, output_width, patch.pixels, patch.width, patch.height, dst_x, dst_y);
        }
        destroy_image_buffer(&patch);
    }
}

static void mrxs_composite_decoded_region(u8* output, i32 output_width, i32 output_height,
                                          u8* decoded_tile, i32 src_width, i32 src_height,
                                          float src_box_x, float src_box_y, float src_box_w, float src_box_h,
                                          float dst_global_x, float dst_global_y,
                                          float out_global_x, float out_global_y) {
    float dst_left = dst_global_x;
    float dst_top = dst_global_y;
    float dst_right = dst_global_x + src_box_w;
    float dst_bottom = dst_global_y + src_box_h;
    float out_left = out_global_x;
    float out_top = out_global_y;
    float out_right = out_global_x + (float)output_width;
    float out_bottom = out_global_y + (float)output_height;

    if (!mrxs_rects_overlap_f32(dst_left, dst_top, dst_right, dst_bottom, out_left, out_top, out_right, out_bottom)) {
        return;
    }

    i32 dst_global_left = MAX((i32)floorf(MAX(dst_left, out_left)), (i32)out_global_x);
    i32 dst_global_top = MAX((i32)floorf(MAX(dst_top, out_top)), (i32)out_global_y);
    i32 dst_global_right = MIN((i32)ceilf(MIN(dst_right, out_right)), (i32)(out_global_x + output_width));
    i32 dst_global_bottom = MIN((i32)ceilf(MIN(dst_bottom, out_bottom)), (i32)(out_global_y + output_height));
    i32 dst_w = dst_global_right - dst_global_left;
    i32 dst_h = dst_global_bottom - dst_global_top;
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    float src_clip_x = src_box_x + ((float)dst_global_left - dst_global_x);
    float src_clip_y = src_box_y + ((float)dst_global_top - dst_global_y);
    i32 dst_x = dst_global_left - (i32)out_global_x;
    i32 dst_y = dst_global_top - (i32)out_global_y;

    bool integral_copy = fabsf(src_clip_x - roundf(src_clip_x)) < 0.001f &&
                         fabsf(src_clip_y - roundf(src_clip_y)) < 0.001f &&
                         fabsf(src_box_w - roundf(src_box_w)) < 0.001f &&
                         fabsf(src_box_h - roundf(src_box_h)) < 0.001f &&
                         fabsf(dst_global_x - roundf(dst_global_x)) < 0.001f &&
                         fabsf(dst_global_y - roundf(dst_global_y)) < 0.001f;
    if (integral_copy) {
        i32 src_x = (i32)roundf(src_clip_x);
        i32 src_y = (i32)roundf(src_clip_y);
        if (src_x >= 0 && src_y >= 0 && src_x + dst_w <= src_width && src_y + dst_h <= src_height) {
            for (i32 y = 0; y < dst_h; ++y) {
                u8* dst = output + ((dst_y + y) * output_width + dst_x) * 4;
                u8* src = decoded_tile + ((src_y + y) * src_width + src_x) * 4;
                memcpy(dst, src, dst_w * 4);
            }
            return;
        }
    }

    image_buffer_t src = {
        .pixels = decoded_tile,
        .channels = 4,
        .width = src_width,
        .height = src_height,
        .stride_in_pixels = src_width,
        .stride_in_bytes = src_width * 4,
        .pixel_format = PIXEL_FORMAT_U8_BGRA,
        .is_valid = true,
    };
    image_buffer_t patch = create_bgra_image_buffer(dst_w, dst_h);
    if (patch.is_valid) {
        rect2f box = RECT2F(src_clip_x, src_clip_y, (float)dst_w, (float)dst_h);
        if (image_resample_lanczos3(&src, &patch, box)) {
            mrxs_copy_patch_to_output(output, output_width, patch.pixels, patch.width, patch.height, dst_x, dst_y);
        }
        destroy_image_buffer(&patch);
    }
}

typedef struct mrxs_decode_cache_entry_t {
    i32 level;
    i32 stored_tile_index;
    u8* pixels;
} mrxs_decode_cache_entry_t;

typedef struct mrxs_decode_cache_t {
    mrxs_decode_cache_entry_t* entries;
    i32 count;
    i32 capacity;
} mrxs_decode_cache_t;

static u8* mrxs_decode_cache_get(mrxs_decode_cache_t* cache, mrxs_t* mrxs, i32 level, i32 stored_tile_index) {
    for (i32 i = 0; i < cache->count; ++i) {
        mrxs_decode_cache_entry_t* entry = cache->entries + i;
        if (entry->level == level && entry->stored_tile_index == stored_tile_index) {
            return entry->pixels;
        }
    }

    u8* pixels = mrxs_acquire_cached_stored_tile(mrxs, level, stored_tile_index);
    if (!pixels) {
        return NULL;
    }

    if (cache->count >= cache->capacity) {
        i32 new_capacity = cache->capacity > 0 ? cache->capacity * 2 : 32;
        mrxs_decode_cache_entry_t* new_entries = realloc(cache->entries, new_capacity * sizeof(*new_entries));
        if (!new_entries) {
            mrxs_release_cached_stored_tile(mrxs, level, stored_tile_index, pixels);
            return NULL;
        }
        cache->entries = new_entries;
        cache->capacity = new_capacity;
    }

    mrxs_decode_cache_entry_t* entry = cache->entries + cache->count++;
    entry->level = level;
    entry->stored_tile_index = stored_tile_index;
    entry->pixels = pixels;
    return pixels;
}

static void mrxs_decode_cache_destroy(mrxs_decode_cache_t* cache, mrxs_t* mrxs) {
    for (i32 i = 0; i < cache->count; ++i) {
        mrxs_release_cached_stored_tile(mrxs, cache->entries[i].level,
                                        cache->entries[i].stored_tile_index,
                                        cache->entries[i].pixels);
    }
    free(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

static u8* mrxs_decode_aligned_overlap_tile_to_bgra(mrxs_t* mrxs, i32 level, i32 logical_tile_index) {
    mrxs_level_t* mrxs_level = mrxs->levels + level;
    i32 logical_width_in_tiles = mrxs_ceil_div_i32(mrxs_ceil_div_i32(mrxs->base_width_in_pixels, 1 << level),
                                                   mrxs_level->tile_width);
    i32 logical_height_in_tiles = mrxs_ceil_div_i32(mrxs_ceil_div_i32(mrxs->base_height_in_pixels, 1 << level),
                                                    mrxs_level->tile_height);
    if (!(logical_tile_index >= 0 && logical_tile_index < logical_width_in_tiles * logical_height_in_tiles)) {
        return NULL;
    }

    u8* result = malloc(mrxs_level->tile_width * mrxs_level->tile_height * 4);
    if (!result) {
        return NULL;
    }
    mrxs_fill_bgra(result, mrxs_level->tile_width, mrxs_level->tile_height,
                   mrxs->is_fluorescence ? 0 : mrxs_level->image_fill_color_bgr);

    i32 logical_tile_x = logical_tile_index % logical_width_in_tiles;
    i32 logical_tile_y = logical_tile_index / logical_width_in_tiles;
    float scale = (float)(1 << level);
    float out_global_x = (float)(logical_tile_x * mrxs_level->tile_width);
    float out_global_y = (float)(logical_tile_y * mrxs_level->tile_height);
    float out_right = out_global_x + (float)mrxs_level->tile_width;
    float out_bottom = out_global_y + (float)mrxs_level->tile_height;
    i32 divisions_at_level = mrxs->camera_image_divisions_per_slide >> level;
    if (divisions_at_level <= 0) {
        free(result);
        return NULL;
    }

    for (i32 camera_y = 0; camera_y < mrxs->camera_grid_height; ++camera_y) {
        for (i32 camera_x = 0; camera_x < mrxs->camera_grid_width; ++camera_x) {
            i32 camera_index = camera_y * mrxs->camera_grid_width + camera_x;
            mrxs_camera_t* camera = mrxs->cameras + camera_index;
            if (!camera->present) {
                continue;
            }

            float camera_level_x = (float)camera->x / scale;
            float camera_level_y = (float)camera->y / scale;
            float camera_level_right = camera_level_x + (float)(divisions_at_level * mrxs_level->tile_width);
            float camera_level_bottom = camera_level_y + (float)(divisions_at_level * mrxs_level->tile_height);
            if (!mrxs_rects_overlap_f32(camera_level_x, camera_level_y, camera_level_right, camera_level_bottom,
                                        out_global_x, out_global_y, out_right, out_bottom)) {
                continue;
            }

            for (i32 local_tile_y = 0; local_tile_y < divisions_at_level; ++local_tile_y) {
                for (i32 local_tile_x = 0; local_tile_x < divisions_at_level; ++local_tile_x) {
                    i32 stored_tile_x = camera_x * divisions_at_level + local_tile_x;
                    i32 stored_tile_y = camera_y * divisions_at_level + local_tile_y;
                    if (stored_tile_x >= mrxs_level->width_in_tiles || stored_tile_y >= mrxs_level->height_in_tiles) {
                        continue;
                    }
                    float src_global_x = camera_level_x + (float)(local_tile_x * mrxs_level->tile_width);
                    float src_global_y = camera_level_y + (float)(local_tile_y * mrxs_level->tile_height);
                    if (!mrxs_rects_overlap_f32(src_global_x, src_global_y,
                                                src_global_x + (float)mrxs_level->tile_width,
                                                src_global_y + (float)mrxs_level->tile_height,
                                                out_global_x, out_global_y, out_right, out_bottom)) {
                        continue;
                    }

                    i32 stored_tile_index = stored_tile_y * mrxs_level->width_in_tiles + stored_tile_x;
                    u8* decoded_tile = mrxs_decode_stored_tile_to_bgra(mrxs, level, stored_tile_index);
                    if (decoded_tile) {
                        mrxs_composite_decoded_tile(result, mrxs_level->tile_width, mrxs_level->tile_height,
                                                    decoded_tile, mrxs_level->tile_width, mrxs_level->tile_height,
                                                    src_global_x, src_global_y, out_global_x, out_global_y);
                        free(decoded_tile);
                    }
                }
            }
        }
    }

    return result;
}

static u8* mrxs_decode_fractional_overlap_tile_to_bgra(mrxs_t* mrxs, i32 level, i32 logical_tile_index) {
    mrxs_level_t* mrxs_level = mrxs->levels + level;
    i32 logical_width_in_tiles = mrxs_ceil_div_i32(mrxs_ceil_div_i32(mrxs->base_width_in_pixels, 1 << level),
                                                   mrxs_level->tile_width);
    i32 logical_height_in_tiles = mrxs_ceil_div_i32(mrxs_ceil_div_i32(mrxs->base_height_in_pixels, 1 << level),
                                                    mrxs_level->tile_height);
    if (!(logical_tile_index >= 0 && logical_tile_index < logical_width_in_tiles * logical_height_in_tiles)) {
        return NULL;
    }

    u8* result = malloc(mrxs_level->tile_width * mrxs_level->tile_height * 4);
    if (!result) {
        return NULL;
    }
    mrxs_fill_bgra(result, mrxs_level->tile_width, mrxs_level->tile_height,
                   mrxs->is_fluorescence ? 0 : mrxs_level->image_fill_color_bgr);

    i32 logical_tile_x = logical_tile_index % logical_width_in_tiles;
    i32 logical_tile_y = logical_tile_index / logical_width_in_tiles;
    float scale = (float)(1 << level);
    float out_global_x = (float)(logical_tile_x * mrxs_level->tile_width);
    float out_global_y = (float)(logical_tile_y * mrxs_level->tile_height);
    float out_right = out_global_x + (float)mrxs_level->tile_width;
    float out_bottom = out_global_y + (float)mrxs_level->tile_height;
    float camera_width_at_level = (float)mrxs->camera_width / scale;
    float camera_height_at_level = (float)mrxs->camera_height / scale;
    mrxs_decode_cache_t decode_cache = {0};

    for (i32 camera_y = 0; camera_y < mrxs->camera_grid_height; ++camera_y) {
        for (i32 camera_x = 0; camera_x < mrxs->camera_grid_width; ++camera_x) {
            i32 camera_index = camera_y * mrxs->camera_grid_width + camera_x;
            mrxs_camera_t* camera = mrxs->cameras + camera_index;
            if (!camera->present) {
                continue;
            }

            float nominal_camera_x = ((float)(camera_x * mrxs->camera_width)) / scale;
            float nominal_camera_y = ((float)(camera_y * mrxs->camera_height)) / scale;
            float actual_camera_x = (float)camera->x / scale;
            float actual_camera_y = (float)camera->y / scale;
            float actual_camera_right = actual_camera_x + camera_width_at_level;
            float actual_camera_bottom = actual_camera_y + camera_height_at_level;

            if (!mrxs_rects_overlap_f32(actual_camera_x, actual_camera_y, actual_camera_right, actual_camera_bottom,
                                        out_global_x, out_global_y, out_right, out_bottom)) {
                continue;
            }

            float nominal_camera_right = nominal_camera_x + camera_width_at_level;
            float nominal_camera_bottom = nominal_camera_y + camera_height_at_level;
            i32 stored_tile_min_x = MAX(0, (i32)floorf(nominal_camera_x / (float)mrxs_level->tile_width));
            i32 stored_tile_min_y = MAX(0, (i32)floorf(nominal_camera_y / (float)mrxs_level->tile_height));
            i32 stored_tile_max_x = MIN(mrxs_level->width_in_tiles, (i32)ceilf(nominal_camera_right / (float)mrxs_level->tile_width));
            i32 stored_tile_max_y = MIN(mrxs_level->height_in_tiles, (i32)ceilf(nominal_camera_bottom / (float)mrxs_level->tile_height));

            for (i32 stored_tile_y = stored_tile_min_y; stored_tile_y < stored_tile_max_y; ++stored_tile_y) {
                for (i32 stored_tile_x = stored_tile_min_x; stored_tile_x < stored_tile_max_x; ++stored_tile_x) {
                    float stored_left = (float)(stored_tile_x * mrxs_level->tile_width);
                    float stored_top = (float)(stored_tile_y * mrxs_level->tile_height);
                    float stored_right = stored_left + (float)mrxs_level->tile_width;
                    float stored_bottom = stored_top + (float)mrxs_level->tile_height;

                    float fragment_nominal_left = MAX(nominal_camera_x, stored_left);
                    float fragment_nominal_top = MAX(nominal_camera_y, stored_top);
                    float fragment_nominal_right = MIN(nominal_camera_right, stored_right);
                    float fragment_nominal_bottom = MIN(nominal_camera_bottom, stored_bottom);
                    float fragment_w = fragment_nominal_right - fragment_nominal_left;
                    float fragment_h = fragment_nominal_bottom - fragment_nominal_top;
                    if (fragment_w <= 0.0f || fragment_h <= 0.0f) {
                        continue;
                    }

                    float dst_global_x = actual_camera_x + (fragment_nominal_left - nominal_camera_x);
                    float dst_global_y = actual_camera_y + (fragment_nominal_top - nominal_camera_y);
                    if (!mrxs_rects_overlap_f32(dst_global_x, dst_global_y, dst_global_x + fragment_w, dst_global_y + fragment_h,
                                                out_global_x, out_global_y, out_right, out_bottom)) {
                        continue;
                    }

                    i32 stored_tile_index = stored_tile_y * mrxs_level->width_in_tiles + stored_tile_x;
                    u8* decoded_tile = mrxs_decode_cache_get(&decode_cache, mrxs, level, stored_tile_index);
                    if (decoded_tile) {
                        float src_box_x = fragment_nominal_left - stored_left;
                        float src_box_y = fragment_nominal_top - stored_top;
                        mrxs_composite_decoded_region(result, mrxs_level->tile_width, mrxs_level->tile_height,
                                                      decoded_tile, mrxs_level->tile_width, mrxs_level->tile_height,
                                                      src_box_x, src_box_y, fragment_w, fragment_h,
                                                      dst_global_x, dst_global_y, out_global_x, out_global_y);
                    }
                }
            }
        }
    }

    mrxs_decode_cache_destroy(&decode_cache, mrxs);
    return result;
}

u8* mrxs_decode_tile_to_bgra(mrxs_t* mrxs, i32 level, i32 tile_index) {
	u8* result = NULL;
	if (level >= 0 && level < mrxs->level_count) {
        if (!mrxs->has_overlapping_tiles) {
            // No overlapping tiles -> relatively straightforward
            result = mrxs_decode_stored_tile_to_bgra(mrxs, level, tile_index);
        } else {
            if (mrxs->cameras && level <= mrxs->max_aligned_overlap_level) {
                result = mrxs_decode_aligned_overlap_tile_to_bgra(mrxs, level, tile_index);
            } else if (mrxs->cameras) {
                result = mrxs_decode_fractional_overlap_tile_to_bgra(mrxs, level, tile_index);
            } else {
                result = mrxs_decode_stored_tile_to_bgra(mrxs, level, tile_index);
            }
        }
	}
	return result;
}

// Set the thread pool to submit parallel jobs to
// TODO(pvalkema): rethink this?
void mrxs_set_thread_pool(mrxs_t* mrxs, thread_pool_t* thread_pool) {
	mrxs->work_submission_pool = thread_pool;
}

void mrxs_destroy(mrxs_t* mrxs) {
	while (mrxs->refcount > 0) {
		platform_sleep(1);
		if (mrxs->work_submission_pool) {
			thread_pool_do_work(mrxs->work_submission_pool);
		} else {
			static bool already_printed = false;
			if (!already_printed) {
				console_print_error("mrxs_destroy(): work_submission_pool not set; refcount = %d, waiting to reach 0\n", mrxs->refcount);
				already_printed = true;
			}
		}
	}
	free(mrxs->dat_files);
	source_fs_close(mrxs->source);
	memrw_destroy(&mrxs->string_pool);
	if (mrxs->dat_filenames) {
		free(mrxs->dat_filenames);
	}
	if (mrxs->hier) {
		for (i32 i = 0; i < mrxs->hier_count; ++i) {
			free(mrxs->hier[i].val);
		}
		free(mrxs->hier);
	}
	if (mrxs->nonhier) {
		for (i32 i = 0; i < mrxs->nonhier_count; ++i) {
			free(mrxs->nonhier[i].val);
		}
		free(mrxs->nonhier);
	}
	for (i32 i = 0; i < COUNT(mrxs->levels); ++i) {
		free(mrxs->levels[i].tiles);
	}
    if (mrxs->camera_positions) {
        libc_free(mrxs->camera_positions);
    }
    if (mrxs->cameras) {
        free(mrxs->cameras);
    }
    if (mrxs->decoded_tile_cache_entries) {
        for (i32 i = 0; i < mrxs->decoded_tile_cache_count; ++i) {
            free(mrxs->decoded_tile_cache_entries[i].pixels);
        }
        free(mrxs->decoded_tile_cache_entries);
    }
    if (mrxs->decoded_tile_cache_mutex_initialized) {
        platform_mutex_destroy(&mrxs->decoded_tile_cache_mutex);
    }
	memset(mrxs, 0, sizeof(mrxs_t));
}
