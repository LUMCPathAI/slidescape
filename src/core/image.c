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
#include "image.h"
#include "jpeg_decoder.h"
#include "isyntax_reader.h" // for isyntax_cache_t
#include "isyntax_streamer.h"

#define STBI_ASSERT(x) ASSERT(x)
#include "stb_image.h" // for stbi_image_free()

#include "tile_loader.h"
#include "tile_cache.h"

const char* get_image_backend_name(image_t* image) {
    const char* result = "--";
    if (image->backend == IMAGE_BACKEND_TIFF) {
        result = "TIFF";
    } else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
        result = "OpenSlide";
    } else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
        result = "iSyntax";
    } else if (image->backend == IMAGE_BACKEND_DICOM) {
        result = "DICOM";
    } else if (image->backend == IMAGE_BACKEND_MRXS) {
        result = "MRXS";
    } else if (image->backend == IMAGE_BACKEND_SLIDE_SCORE) {
        result = "Slide Score";
    } else if (image->backend == IMAGE_BACKEND_DICOMWEB) {
        result = "DICOMweb";
    } else if (image->backend == IMAGE_BACKEND_STBI) {
        result = "stb_image";
    }
    return result;
}

const char* get_image_descriptive_type_name(image_t* image) {
    const char* result = "--";
    if (image->type == IMAGE_TYPE_WSI) {
        if (image->backend == IMAGE_BACKEND_TIFF) {
            result = "WSI (TIFF)";
        } else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
            result = "WSI (OpenSlide)";
        } else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
            result = "WSI (iSyntax)";
        } else if (image->backend == IMAGE_BACKEND_DICOM) {
            result = "WSI (DICOM)";
        } else if (image->backend == IMAGE_BACKEND_MRXS) {
            result = "WSI (MRXS)";
        } else if (image->backend == IMAGE_BACKEND_SLIDE_SCORE) {
            result = "WSI (Slide Score)";
        } else if (image->backend == IMAGE_BACKEND_DICOMWEB) {
            result = "WSI (DICOMweb)";
        } else if (image->backend == IMAGE_BACKEND_STBI) {
            result = "Simple image";
        }
    } else {
        result = "Unknown";
    }
    return result;
}

static void image_change_resolution(image_t* image, float mpp_x, float mpp_y) {
    image->mpp_x = mpp_x;
    image->mpp_y = mpp_y;
    image->width_in_um = image->width_in_pixels * mpp_x;
    image->height_in_um = image->height_in_pixels * mpp_y;

    if (image->type == IMAGE_TYPE_WSI) {
        // shorthand pointers for backend-specific data structure
        tiff_t* tiff = &image->tiff;
        wsi_t* openslide_image = &image->openslide_wsi;
        isyntax_t* isyntax = &image->isyntax;


        if (image->backend == IMAGE_BACKEND_TIFF) {
            tiff->mpp_x = mpp_x;
            tiff->mpp_y = mpp_y;
        } else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
            openslide_image->mpp_x = mpp_x;
            openslide_image->mpp_y = mpp_y;
        } else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
            isyntax->mpp_x = mpp_x;
            isyntax->mpp_y = mpp_y;
        }

        for (i32 i = 0; i < image->level_count; ++i) {
            level_image_t* level_image = image->level_images + i;
            level_image->um_per_pixel_x = image->mpp_x * level_image->downsample_factor;
            level_image->um_per_pixel_y = image->mpp_y * level_image->downsample_factor;
            level_image->x_tile_side_in_um = level_image->tile_width * level_image->um_per_pixel_x;
            level_image->y_tile_side_in_um = level_image->tile_height * level_image->um_per_pixel_y;

            // If this downsampling level is 'backed' by a corresponding image pyramid level (not guaranteed),
            // then we also need to update the dimension info for the backend-specific data structure
            if (level_image->exists) {
                i32 pyramid_image_index = level_image->pyramid_image_index;
                if (image->backend == IMAGE_BACKEND_TIFF) {
                    ASSERT(pyramid_image_index < tiff->ifd_count);
                    tiff_ifd_t* ifd = tiff->ifds + pyramid_image_index;
                    ifd->um_per_pixel_x = level_image->um_per_pixel_x;
                    ifd->um_per_pixel_y = level_image->um_per_pixel_y;
                    ifd->x_tile_side_in_um = level_image->x_tile_side_in_um;
                    ifd->y_tile_side_in_um = level_image->y_tile_side_in_um;

//					TODO: ifd->x_resolution = create_tiff_rational(...);
//					ifd->y_resolution = create_tiff_rational(...);

                } else if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
                    wsi_level_t* wsi_level = openslide_image->levels + pyramid_image_index;
                    wsi_level->um_per_pixel_x = level_image->um_per_pixel_x;
                    wsi_level->um_per_pixel_y = level_image->um_per_pixel_y;
                    wsi_level->x_tile_side_in_um = level_image->x_tile_side_in_um;
                    wsi_level->y_tile_side_in_um = level_image->y_tile_side_in_um;
                } else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
                    // TODO: stub
                }
            }
        }
    }
}


// TODO: write 'drivers' / interfaces to be queried, instead of this copy-pasta

bool init_image_from_tiff(image_t* image, tiff_t tiff, bool is_overlay, image_t* parent_image) {
    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_TIFF;
    image->tiff = tiff;
    image->is_freshly_loaded = true;

    image->mpp_x = tiff.mpp_x;
    image->mpp_y = tiff.mpp_y;
    image->is_mpp_known = tiff.is_mpp_known;
    ASSERT(tiff.main_image_ifd);
    image->tile_width = tiff.main_image_ifd->tile_width;
    image->tile_height = tiff.main_image_ifd->tile_height;
    image->width_in_pixels = tiff.main_image_ifd->image_width;
    image->width_in_um = tiff.main_image_ifd->image_width * tiff.mpp_x;
    image->height_in_pixels = tiff.main_image_ifd->image_height;
    image->height_in_um = tiff.main_image_ifd->image_height * tiff.mpp_y;

    image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
    image->clip_rect_count = 1;

    // TODO: fix code duplication with tiff_deserialize()
    if (tiff.level_image_ifd_count > 0 && tiff.main_image_ifd->tile_width) {

        if (tiff.main_image_ifd->is_tiled) {

            // This is a tiled image (as we expect most WSIs to be).
            // We need to fill out the level_image_t structures to initialize the backend-agnostic image_t

            memset(image->level_images, 0, sizeof(image->level_images));
            image->level_count = tiff.max_downsample_level + 1;

            if (image->level_count > WSI_MAX_LEVELS) {
                fatal_error();
            }

            i32 ifd_index = 0;
            i32 next_ifd_index_to_check_for_match = 0;
            tiff_ifd_t* ifd = tiff.level_images_ifd + ifd_index;
            for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
                level_image_t* level_image = image->level_images + level_index;

                i32 wanted_downsample_level = level_index;
                bool found_ifd = false;
                for (ifd_index = next_ifd_index_to_check_for_match; ifd_index < tiff.level_image_ifd_count; ++ifd_index) {
                    ifd = tiff.level_images_ifd + ifd_index;
                    if (ifd->downsample_level == wanted_downsample_level) {
                        // match!
                        found_ifd = true;
                        next_ifd_index_to_check_for_match = ifd_index + 1; // next iteration, don't reuse the same IFD!
                        break;
                    }
                }

                if (found_ifd) {
                    // The current downsampling level is backed by a corresponding IFD level image in the TIFF.
                    level_image->exists = true;
                    level_image->pyramid_image_index = ifd_index;
                    level_image->downsample_factor = ifd->downsample_factor;
                    level_image->width_in_pixels = ifd->image_width;
                    level_image->height_in_pixels = ifd->image_height;
                    level_image->tile_count = ifd->tile_count;
                    level_image->width_in_tiles = ifd->width_in_tiles;
                    ASSERT(level_image->width_in_tiles > 0);
                    level_image->height_in_tiles = ifd->height_in_tiles;
                    level_image->tile_width = ifd->tile_width;
                    level_image->tile_height = ifd->tile_height;
#if DO_DEBUG
                    if (level_image->tile_width != image->tile_width) {
						console_print("Warning: level image %d (ifd #%d) tile width (%d) does not match base level (%d)\n", level_index, ifd_index, level_image->tile_width, image->tile_width);
					}
					if (level_image->tile_height != image->tile_height) {
						console_print("Warning: level image %d (ifd #%d) tile width (%d) does not match base level (%d)\n", level_index, ifd_index, level_image->tile_width, image->tile_width);
					}
#endif
                    level_image->um_per_pixel_x = ifd->um_per_pixel_x;
                    level_image->um_per_pixel_y = ifd->um_per_pixel_y;
                    level_image->x_tile_side_in_um = ifd->x_tile_side_in_um;
                    level_image->y_tile_side_in_um = ifd->y_tile_side_in_um;
                    ASSERT(level_image->x_tile_side_in_um > 0);
                    ASSERT(level_image->y_tile_side_in_um > 0);
                    level_image->tiles = (tile_t*) calloc(1, ifd->tile_count * sizeof(tile_t));
                    ASSERT(ifd->tile_byte_counts != NULL);
                    ASSERT(ifd->tile_offsets != NULL);
                    // mark the empty tiles, so that we can skip loading them later on
                    for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
                        tile_t* tile = level_image->tiles + tile_index;
                        u64 tile_byte_count = ifd->tile_byte_counts[tile_index];
                        if (tile_byte_count == 0) {
                            tile->is_empty = true;
                        }
                        // Facilitate some introspection by storing self-referential information
                        // in the tile_t struct. This is needed for some specific cases where we
                        // pass around pointers to tile_t structs without caring exactly where they
                        // came from.
                        // (Specific example: we use this when exporting a selected region as BigTIFF)
                        tile->tile_index = tile_index;
                        tile->tile_x = tile_index % level_image->width_in_tiles;
                        tile->tile_y = tile_index / level_image->width_in_tiles;
                    }
                } else {
                    // The current downsampling level has no corresponding IFD level image :(
                    // So we need only some placeholder information.
                    level_image->exists = false;
                    level_image->downsample_factor = exp2f((float)wanted_downsample_level);
                    // Just in case anyone tries to divide by zero:
                    level_image->tile_width = image->tile_width;
                    level_image->tile_height = image->tile_height;
                    level_image->um_per_pixel_x = image->mpp_x * level_image->downsample_factor;
                    level_image->um_per_pixel_y = image->mpp_y * level_image->downsample_factor;
                    level_image->x_tile_side_in_um = level_image->um_per_pixel_x * (float)tiff.main_image_ifd->tile_width;
                    level_image->y_tile_side_in_um = level_image->um_per_pixel_y * (float)tiff.main_image_ifd->tile_height;
                }
                DUMMY_STATEMENT;
            }
        } else if (tiff.is_ndpi) {
            DUMMY_STATEMENT;
        } else {
            // The image is NOT tiled
            memset(image->level_images, 0, sizeof(image->level_images));
            image->level_count = 1;
            level_image_t* level_image = image->level_images + 0;
            tiff_ifd_t* ifd = tiff.main_image_ifd;
            level_image->exists = true;
            level_image->pyramid_image_index = 0;
            level_image->downsample_factor = ifd->downsample_factor;
            level_image->width_in_pixels = ifd->image_width;
            level_image->height_in_pixels = ifd->image_height;
            level_image->tile_count = 1;
            level_image->width_in_tiles = 1;
            ASSERT(level_image->width_in_tiles > 0);
            level_image->height_in_tiles = 1;
            level_image->tile_width = ifd->tile_width;
            level_image->tile_height = ifd->tile_height;
            level_image->um_per_pixel_x = ifd->um_per_pixel_x;
            level_image->um_per_pixel_y = ifd->um_per_pixel_y;
            level_image->x_tile_side_in_um = ifd->x_tile_side_in_um;
            level_image->y_tile_side_in_um = ifd->y_tile_side_in_um;
            ASSERT(level_image->x_tile_side_in_um > 0);
            ASSERT(level_image->y_tile_side_in_um > 0);
            level_image->tiles = (tile_t*) calloc(1, sizeof(tile_t)); // only 1 tile in this case
            ASSERT(ifd->strip_byte_counts != NULL);
            ASSERT(ifd->strip_offsets != NULL);
            tile_t* tile = level_image->tiles + 0;
            tile->tile_index = 0;
            tile->tile_x = 0;
            tile->tile_y = 0;
        }


    }

    // TODO: establish the concept of a 'parent image' / fix dimensions not being exactly right
    // TODO: automatically register (translate/stretch) image
    // For now, we shall assume that the first loaded image is the parent image, and that the resolution of the overlay
    // is identical to the parent (although this is sometimes not strictly true, e.g. the TIFF resolution tags in the
    // Kaggle challenge prostate biopsies are *slightly* different in the base and mask images. But if we take those
    // resolution tags at face value, the images will not be correctly aligned!)
    if (is_overlay && parent_image) {
        ASSERT(parent_image->mpp_x > 0.0f && parent_image->mpp_y > 0.0f);
        image_change_resolution(image, parent_image->mpp_x, parent_image->mpp_y);
    }


    image->is_valid = true;
    image->is_freshly_loaded = true;
    return image->is_valid;
}

static inline i64 ceil_div_i64(i64 a, i64 b) {
    return (a + b - 1) / b;
}

static inline i64 ceil_div_f64(i64 a, double b) {
    return (i64)ceil((double)a / b);
}

bool init_image_from_slide_score(image_t* image, slide_score_remote_image_t* remote, bool is_overlay) {
    (void)is_overlay;
    slide_score_get_image_metadata_result_t* metadata = &remote->metadata;
    if (metadata->level_0_width <= 0 || metadata->level_0_height <= 0 || metadata->tile_width <= 0 || metadata->tile_height <= 0) {
        return false;
    }

    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_SLIDE_SCORE;
    image->slide_score = *remote;
    image->is_local = false;
    image->is_freshly_loaded = true;
    image->mpp_x = metadata->mpp_x;
    image->mpp_y = metadata->mpp_y;
    image->is_mpp_known = metadata->mpp_x > 0.0f && metadata->mpp_y > 0.0f;
    image->tile_width = metadata->tile_width;
    image->tile_height = metadata->tile_height;
    image->width_in_pixels = metadata->level_0_width;
    image->height_in_pixels = metadata->level_0_height;
    image->width_in_um = image->is_mpp_known ? image->width_in_pixels * image->mpp_x : (float)image->width_in_pixels;
    image->height_in_um = image->is_mpp_known ? image->height_in_pixels * image->mpp_y : (float)image->height_in_pixels;

    image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
    image->clip_rect_count = 1;

    snprintf(image->name, sizeof(image->name), "%s", metadata->filename[0] ? metadata->filename : "Slide Score image");
    snprintf(image->directory, sizeof(image->directory), "https://%s/i/%d", remote->client.server_name, remote->image_id);

    memset(image->level_images, 0, sizeof(image->level_images));
    image->level_count = metadata->downsample_count > 0 ? metadata->downsample_count : metadata->level_count;
    if (image->level_count <= 0) image->level_count = 1;
    image->level_count = ATMOST(image->level_count, IMAGE_PYRAMID_MAX_LEVELS);

    for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
        double downsample = 1.0;
        if (level_index < metadata->downsample_count) {
            downsample = metadata->downsamples[level_index];
        } else if (level_index > 0) {
            downsample = (double)(1 << level_index);
        }
        if (downsample < 1.0) downsample = 1.0;

        level_image_t* level_image = image->level_images + level_index;
        level_image->exists = true;
        level_image->pyramid_image_index = remote->max_deepzoom_level - (i32)floor(log2(downsample) + 0.5);
        level_image->downsample_factor = (float)downsample;
        level_image->width_in_pixels = ceil_div_f64(image->width_in_pixels, downsample);
        level_image->height_in_pixels = ceil_div_f64(image->height_in_pixels, downsample);
        level_image->tile_width = image->tile_width;
        level_image->tile_height = image->tile_height;
        level_image->width_in_tiles = (u32)ceil_div_i64(level_image->width_in_pixels, level_image->tile_width);
        level_image->height_in_tiles = (u32)ceil_div_i64(level_image->height_in_pixels, level_image->tile_height);
        level_image->tile_count = level_image->width_in_tiles * level_image->height_in_tiles;
        level_image->um_per_pixel_x = image->is_mpp_known ? image->mpp_x * level_image->downsample_factor : level_image->downsample_factor;
        level_image->um_per_pixel_y = image->is_mpp_known ? image->mpp_y * level_image->downsample_factor : level_image->downsample_factor;
        level_image->x_tile_side_in_um = level_image->um_per_pixel_x * (float)level_image->tile_width;
        level_image->y_tile_side_in_um = level_image->um_per_pixel_y * (float)level_image->tile_height;
        level_image->tiles = (tile_t*)calloc(1, level_image->tile_count * sizeof(tile_t));
        for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
            tile_t* tile = level_image->tiles + tile_index;
            tile->tile_index = tile_index;
            tile->tile_x = tile_index % level_image->width_in_tiles;
            tile->tile_y = tile_index / level_image->width_in_tiles;
        }
    }

    image->is_valid = true;
    return true;
}

u8* decode_associated_image_from_isyntax(isyntax_t* isyntax, isyntax_image_t* image) {
	u32 jpeg_size = 0;
	u8* jpeg_compressed = isyntax_get_associated_image_jpeg(isyntax, image, &jpeg_size);

	i32 channels_in_file = 0;
	u8* pixels = jpeg_decode_image(jpeg_compressed, jpeg_size, &image->width, &image->height, &channels_in_file);
	return pixels;

}

bool init_image_from_isyntax(image_t* image, isyntax_t* isyntax, bool is_overlay) {
    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_ISYNTAX;
    image->isyntax = *isyntax;
    isyntax = &image->isyntax;
    image->is_freshly_loaded = true;

    image->mpp_x = isyntax->mpp_x;
    image->mpp_y = isyntax->mpp_y;
    image->is_mpp_known = isyntax->is_mpp_known;
    isyntax_image_t* wsi_image = isyntax->images + isyntax->wsi_image_index;
    image->tile_width = isyntax->tile_width;
    image->tile_height = isyntax->tile_height;
    image->width_in_pixels = wsi_image->width;
    image->width_in_um = wsi_image->width * isyntax->mpp_x;
    image->height_in_pixels = wsi_image->height;
    image->height_in_um = wsi_image->height * isyntax->mpp_y;

    // Construct clipping rectangles from valid data envelopes 
    if(isyntax->valid_data_envelope_rectangle_count == 0){ // iSyntax v1
    	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
    	image->clip_rect_count = 1;
    } else { // iSyntax v2
        for (i32 i = 0; i < MIN(isyntax->valid_data_envelope_rectangle_count, COUNT(isyntax->valid_data_envelopes_rectangles)); ++i){
            rect2i* rectangle = &isyntax->valid_data_envelopes_rectangles[i];
            image->clip_rects[image->clip_rect_count] = RECT2F(
                rectangle->x * image->mpp_x,
                rectangle->y * image->mpp_y,
                rectangle->w * image->mpp_x,
                rectangle->h * image->mpp_y
            );
            image->clip_rect_count +=1;
        }
    }

    // TODO: fix code duplication with tiff_deserialize()
    if (wsi_image->level_count > 0 && isyntax->tile_width) {

        memset(image->level_images, 0, sizeof(image->level_images));
        image->level_count = wsi_image->level_count;

        if (image->level_count > WSI_MAX_LEVELS) {
            fatal_error();
        }

        for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
            level_image_t* level_image = image->level_images + level_index;
            isyntax_level_t* isyntax_level = wsi_image->levels + level_index;

            level_image->exists = true;
            level_image->pyramid_image_index = level_index; // not used
            level_image->downsample_factor = exp2f((float)level_index);
            level_image->width_in_pixels = isyntax_level->width_in_tiles * isyntax->tile_width; // TODO: check that this is right...
            level_image->height_in_pixels = isyntax_level->height_in_tiles * isyntax->tile_height; // TODO: check that this is right...
            level_image->tile_count = isyntax_level->tile_count;
            level_image->width_in_tiles = isyntax_level->width_in_tiles;
            ASSERT(level_image->width_in_tiles > 0);
            level_image->height_in_tiles = isyntax_level->height_in_tiles;
            level_image->tile_width = isyntax->tile_width;
            level_image->tile_height = isyntax->tile_height;
            level_image->um_per_pixel_x = level_image->downsample_factor * isyntax->mpp_x;
            level_image->um_per_pixel_y = level_image->downsample_factor * isyntax->mpp_y;
            level_image->x_tile_side_in_um = level_image->um_per_pixel_x * isyntax->tile_width;
            level_image->y_tile_side_in_um = level_image->um_per_pixel_x * isyntax->tile_height;
            ASSERT(level_image->x_tile_side_in_um > 0);
            ASSERT(level_image->y_tile_side_in_um > 0);
            level_image->origin_offset = isyntax_level->origin_offset;
            level_image->tiles = (tile_t*) calloc(1, level_image->tile_count * sizeof(tile_t));
            for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
                tile_t* tile = level_image->tiles + tile_index;
                // Facilitate some introspection by storing self-referential information
                // in the tile_t struct. This is needed for some specific cases where we
                // pass around pointers to tile_t structs without caring exactly where they
                // came from.
                // (Specific example: we use this when exporting a selected region as BigTIFF)
                tile->tile_index = tile_index;
                tile->tile_x = tile_index % level_image->width_in_tiles;
                tile->tile_y = tile_index / level_image->width_in_tiles;

                isyntax_tile_t* isyntax_tile = isyntax_level->tiles + tile_index;
                if (!isyntax_tile->exists) {
                    tile->is_empty = true;
                }
            }
            DUMMY_STATEMENT;
        }
    }

	// TODO (pvalkema): defer this/load lazily
	isyntax_image_t* macro_image = isyntax->images + isyntax->macro_image_index;
	if (macro_image->image_type == ISYNTAX_IMAGE_TYPE_MACROIMAGE) {
		u8* pixels = decode_associated_image_from_isyntax(isyntax, macro_image);
        if (pixels) {
            image->macro_image.pixels = pixels;
            image->macro_image.width = macro_image->width;
            image->macro_image.height = macro_image->height;
            if(macro_image->is_mpp_known){
                image->macro_image.mpp = macro_image->mpp_x; // not always, in version >100 mpp are provided for macro and label images (value around 27.0)
            }else{
                image->macro_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
            }
            if(macro_image->origin_x == 0 || macro_image->origin_y == 0){ // version 5
                image->macro_image.world_pos.x = -((float)(wsi_image->offset_x + wsi_image->level0_padding) * isyntax->mpp_x);
                image->macro_image.world_pos.y = -((float)(wsi_image->offset_y + wsi_image->level0_padding) * isyntax->mpp_y);
            }else{ // version >100
                image->macro_image.world_pos.x = (float)(macro_image->origin_x);
                image->macro_image.world_pos.y = (float)(macro_image->origin_y);
            }
            image->macro_image.is_valid = true;
        }
    }
    isyntax_image_t* label_image = isyntax->images + isyntax->label_image_index;
    if (label_image->image_type == ISYNTAX_IMAGE_TYPE_LABELIMAGE) {
		u8* pixels = decode_associated_image_from_isyntax(isyntax, label_image);
        if (pixels) {
            image->label_image.pixels = pixels;
            image->label_image.width = label_image->width;
            image->label_image.height = label_image->height;
            if(label_image->is_mpp_known){
                image->label_image.mpp = label_image->mpp_x; // not always, in version >100 mpp are provided for macro and label images
            }else{
                image->label_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
            }
			if (image->label_image.is_valid) {
				image->label_image.world_pos.x = image->macro_image.world_pos.x // macro image left side (origin)
					+ image->macro_image.width * image->macro_image.mpp // macro image right side
					+ image->label_image.width * image->label_image.mpp; // add label height (will rotate 90 degrees to the right to 'fit' in place)
				// TODO: fix iSyntax label image display (right now it's still rotated)
				image->label_image.world_pos.y = image->macro_image.world_pos.y;
			}
	        image->label_image.is_valid = true;
        }
    }


    image->is_valid = true;
    image->is_freshly_loaded = true;
    return image->is_valid;
}

bool init_image_from_dicom(image_t* image, dicom_series_t* dicom, bool is_overlay) {
    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_DICOM;
    image->dicom = *dicom;
    dicom = &image->dicom;
    image->is_freshly_loaded = true;

    dicom_instance_t* base_level_instance = dicom->wsi.level_instances[0];
    ASSERT(base_level_instance);
    if (!base_level_instance) return false;

    image->mpp_x = dicom->wsi.mpp_x;
    image->mpp_y = dicom->wsi.mpp_y;
    image->is_mpp_known = dicom->wsi.is_mpp_known;
    if (image->mpp_x <= 0.0f || image->mpp_y <= 0.0f) {
        image->is_mpp_known = false;
        image->mpp_x = 1.0f;
        image->mpp_y = 1.0f;
    }

    image->tile_width = base_level_instance->columns;
    image->tile_height = base_level_instance->rows;
    image->width_in_pixels = base_level_instance->total_pixel_matrix_columns;
    image->width_in_um = base_level_instance->total_pixel_matrix_columns * image->mpp_x;
    image->height_in_pixels = base_level_instance->total_pixel_matrix_rows;
    image->height_in_um = base_level_instance->total_pixel_matrix_rows * image->mpp_y;

	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
	image->clip_rect_count = 1;

    // TODO: fix code duplication with tiff_deserialize()
    if (dicom->wsi.instance_count > 0 && image->tile_width) {

        memset(image->level_images, 0, sizeof(image->level_images));
        image->level_count = dicom->wsi.max_downsample_level + 1;

        if (image->level_count > WSI_MAX_LEVELS) {
            fatal_error();
        }

	    i32 instance_index = 0;
	    i32 next_instance_index_to_check_for_match = 0;
	    dicom_instance_t* level_instance = dicom->wsi.level_instances[instance_index];
        for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
            level_image_t* level_image = image->level_images + level_index;

	        i32 wanted_downsample_level = level_index;
	        bool found_instance = false;
	        for (instance_index = next_instance_index_to_check_for_match; instance_index < dicom->wsi.instance_count; ++instance_index) {
		        level_instance = dicom->wsi.level_instances[instance_index];
		        if (level_instance->downsample_level == wanted_downsample_level) {
			        // match!
			        found_instance = true;
			        next_instance_index_to_check_for_match = instance_index + 1; // next iteration, don't reuse the same IFD!
			        break;
		        }
	        }

			if (found_instance) {
				level_image->exists = true;
				level_image->needs_indexing = false;
				level_image->pyramid_image_index = instance_index; // can differ from the level index if levels are missing
				level_image->downsample_factor = exp2f((float)level_index);
				level_image->width_in_pixels = level_instance->total_pixel_matrix_columns; // TODO: check that this is right
				level_image->height_in_pixels = level_instance->total_pixel_matrix_rows; // TODO: check that this is right
				level_image->width_in_tiles = level_instance->width_in_tiles;
				ASSERT(level_image->width_in_tiles > 0);
				level_image->height_in_tiles = level_instance->height_in_tiles;
				ASSERT(level_image->height_in_tiles > 0);
				level_image->tile_count = level_instance->tile_count;
				level_image->tile_width = level_instance->columns;
				level_image->tile_height = level_instance->rows;
				if (level_instance->rows != image->tile_width) {
					ASSERT(!"tile width is not equal across all levels");
					return false;
				}
				if (level_instance->columns != image->tile_height) {
					ASSERT(!"tile height is not equal across all levels");
					return false;
				}
				level_image->um_per_pixel_x = level_image->downsample_factor * dicom->wsi.mpp_x;
				level_image->um_per_pixel_y = level_image->downsample_factor * dicom->wsi.mpp_y;
				level_image->x_tile_side_in_um = level_image->um_per_pixel_x * level_instance->columns;
				level_image->y_tile_side_in_um = level_image->um_per_pixel_x * level_instance->rows;
				ASSERT(level_image->x_tile_side_in_um > 0);
				ASSERT(level_image->y_tile_side_in_um > 0);
				level_image->origin_offset = level_instance->origin_offset;
				level_image->tiles = (tile_t*) calloc(1, level_image->tile_count * sizeof(tile_t));
				for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
					tile_t* tile = level_image->tiles + tile_index;
					// Facilitate some introspection by storing self-referential information
					// in the tile_t struct. This is needed for some specific cases where we
					// pass around pointers to tile_t structs without caring exactly where they
					// came from.
					// (Specific example: we use this when exporting a selected region as BigTIFF)
					tile->tile_index = tile_index;
					tile->tile_x = tile_index % level_image->width_in_tiles;
					tile->tile_y = tile_index / level_image->width_in_tiles;

					dicom_tile_t* dicom_tile = level_instance->tiles + tile_index;
					if (!dicom_tile->exists) {
						tile->is_empty = true;
					} else if (!dicom_tile->is_offset_known) {
						level_image->needs_indexing = true;
					}
				}
				DUMMY_STATEMENT;
			} else {
				// The current downsampling level has no corresponding DICOM instance :(
				// So we need only some placeholder information.
				level_image->exists = false;
				level_image->downsample_factor = exp2f((float)wanted_downsample_level);
				// Just in case anyone tries to divide by zero:
				level_image->tile_width = image->tile_width;
				level_image->tile_height = image->tile_height;
				level_image->um_per_pixel_x = image->mpp_x * level_image->downsample_factor;
				level_image->um_per_pixel_y = image->mpp_y * level_image->downsample_factor;
				level_image->x_tile_side_in_um = level_image->um_per_pixel_x * (float)image->tile_width;
				level_image->y_tile_side_in_um = level_image->um_per_pixel_y * (float)image->tile_height;
			}


        }
    }

    /*isyntax_image_t* macro_image = isyntax->images + isyntax->macro_image_index;
    if (macro_image->image_type == ISYNTAX_IMAGE_TYPE_MACROIMAGE) {
        if (macro_image->pixels) {
            image->macro_image.pixels = macro_image->pixels;
            macro_image->pixels = NULL; // transfer ownership
            image->macro_image.width = macro_image->width;
            image->macro_image.height = macro_image->height;
            image->macro_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
            image->macro_image.world_pos.x = -((float)base_level_instance->offset_x * isyntax->mpp_x);
            image->macro_image.world_pos.y = -((float)base_level_instance->offset_y * isyntax->mpp_y);
            image->macro_image.is_valid = true;
        }
    }
    isyntax_image_t* label_image = isyntax->images + isyntax->label_image_index;
    if (label_image->image_type == ISYNTAX_IMAGE_TYPE_LABELIMAGE) {
        if (label_image->pixels) {
            image->label_image.pixels = label_image->pixels;
            label_image->pixels = NULL; // transfer ownership
            image->label_image.width = label_image->width;
            image->label_image.height = label_image->height;
            image->label_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
//			image->label_image.world_pos.x = -((float)wsi_image->offset_x * isyntax->mpp_x);
//			image->label_image.world_pos.y = -((float)wsi_image->offset_y * isyntax->mpp_y);
            image->label_image.is_valid = true;
        }
    }*/


    image->is_valid = true;
    image->is_freshly_loaded = true;
    return image->is_valid;
}

bool init_image_from_dicomweb(image_t* image, const dicomweb_remote_image_t* remote, bool is_overlay) {
	(void)is_overlay;
	if (!image || !remote || remote->instance_count <= 0) return false;
	const dicomweb_instance_metadata_t* base = remote->instances;
	if (!base->rows || !base->columns || !base->total_pixel_matrix_columns ||
	    !base->total_pixel_matrix_rows || !base->is_tiled_full) return false;

	image->type = IMAGE_TYPE_WSI;
	image->backend = IMAGE_BACKEND_DICOMWEB;
	image->dicomweb = *remote;
	image->is_local = false;
	image->is_freshly_loaded = true;
	image->mpp_x = base->is_mpp_known ? base->mpp_x : 1.0f;
	image->mpp_y = base->is_mpp_known ? base->mpp_y : 1.0f;
	image->is_mpp_known = base->is_mpp_known;
	image->tile_width = base->columns;
	image->tile_height = base->rows;
	image->width_in_pixels = base->total_pixel_matrix_columns;
	image->height_in_pixels = base->total_pixel_matrix_rows;
	image->width_in_um = image->width_in_pixels * image->mpp_x;
	image->height_in_um = image->height_in_pixels * image->mpp_y;
	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
	image->clip_rect_count = 1;
	snprintf(image->name, sizeof(image->name), "DICOMweb %.64s", remote->series_instance_uid);
	snprintf(image->directory, sizeof(image->directory), "%s://%s:%d%s",
	         remote->use_tls ? "https" : "http", remote->hostname, remote->port, remote->root);

	i32 max_level = 0;
	for (i32 i = 0; i < remote->instance_count; ++i) {
		const dicomweb_instance_metadata_t* instance = remote->instances + i;
		double ratio = (double)base->total_pixel_matrix_columns /
		               (double)instance->total_pixel_matrix_columns;
		i32 level = (i32)floor(log2(ratio) + 0.5);
		if (level >= 0 && level < IMAGE_PYRAMID_MAX_LEVELS) max_level = MAX(max_level, level);
	}
	image->level_count = max_level + 1;
	for (i32 level = 0; level < image->level_count; ++level) {
		level_image_t* level_image = image->level_images + level;
		level_image->downsample_factor = exp2f((float)level);
		level_image->tile_width = image->tile_width;
		level_image->tile_height = image->tile_height;
		level_image->um_per_pixel_x = image->mpp_x * level_image->downsample_factor;
		level_image->um_per_pixel_y = image->mpp_y * level_image->downsample_factor;
		level_image->x_tile_side_in_um = level_image->um_per_pixel_x * image->tile_width;
		level_image->y_tile_side_in_um = level_image->um_per_pixel_y * image->tile_height;
		level_image->pyramid_image_index = -1;
	}
	for (i32 i = 0; i < remote->instance_count; ++i) {
		const dicomweb_instance_metadata_t* instance = remote->instances + i;
		if (strcmp(instance->image_flavor, "VOLUME") != 0 || !instance->is_tiled_full ||
		    instance->rows != image->tile_height || instance->columns != image->tile_width) continue;
		double ratio = (double)base->total_pixel_matrix_columns /
		               (double)instance->total_pixel_matrix_columns;
		i32 level = (i32)floor(log2(ratio) + 0.5);
		if (level < 0 || level >= image->level_count) continue;
		level_image_t* level_image = image->level_images + level;
		level_image->exists = true;
		level_image->pyramid_image_index = i;
		level_image->width_in_pixels = instance->total_pixel_matrix_columns;
		level_image->height_in_pixels = instance->total_pixel_matrix_rows;
		level_image->width_in_tiles = (instance->total_pixel_matrix_columns + instance->columns - 1) /
		                              instance->columns;
		level_image->height_in_tiles = (instance->total_pixel_matrix_rows + instance->rows - 1) /
		                               instance->rows;
		level_image->tile_count = level_image->width_in_tiles * level_image->height_in_tiles;
		if ((i64)level_image->tile_count != instance->number_of_frames) return false;
		level_image->tiles = (tile_t*)calloc(level_image->tile_count, sizeof(tile_t));
		if (!level_image->tiles) return false;
		for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
			tile_t* tile = level_image->tiles + tile_index;
			tile->tile_index = tile_index;
			tile->tile_x = tile_index % level_image->width_in_tiles;
			tile->tile_y = tile_index / level_image->width_in_tiles;
			if (!instance->frame_numbers ||
			    tile_index >= instance->frame_map_count ||
			    instance->frame_numbers[tile_index] == 0) {
				tile->is_empty = true;
			}
		}
	}
	image->is_valid = image->level_images[0].exists;
	return image->is_valid;
}

bool init_image_from_mrxs(image_t* image, mrxs_t* mrxs, bool is_overlay) {
	image->type = IMAGE_TYPE_WSI;
	image->backend = IMAGE_BACKEND_MRXS;
	image->mrxs = *mrxs;
	mrxs = &image->mrxs;
	mrxs_init_runtime_cache(mrxs);
	image->is_freshly_loaded = true;

	image->mpp_x = mrxs->mpp_x;
	image->mpp_y = mrxs->mpp_y;
	image->is_mpp_known = mrxs->is_mpp_known;
	image->is_background_black = mrxs->is_fluorescence;
	if (image->mpp_x <= 0.0f || image->mpp_y <= 0.0f) {
		image->is_mpp_known = false;
		image->mpp_x = 1.0f;
		image->mpp_y = 1.0f;
	}

	image->tile_width = mrxs->tile_width;
	image->tile_height = mrxs->tile_height;
	image->width_in_pixels = mrxs->base_width_in_pixels;
	image->width_in_um = image->width_in_pixels * image->mpp_x;
	image->height_in_pixels = mrxs->base_height_in_pixels;
	image->height_in_um = image->height_in_pixels * image->mpp_y;
	if (mrxs->has_overlapping_tiles) {
		image->origin_offset = V2F((float)mrxs->camera_origin_x * image->mpp_x,
		                           (float)mrxs->camera_origin_y * image->mpp_y);
	}

	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
	image->clip_rect_count = 1;

	// TODO: fix code duplication with tiff_deserialize()
	if (mrxs->level_count > 0 && image->tile_width) {

		memset(image->level_images, 0, sizeof(image->level_images));
		image->level_count = mrxs->level_count;

		if (image->level_count > WSI_MAX_LEVELS) {
			fatal_error();
		}

		for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
			level_image_t* level_image = image->level_images + level_index;
			mrxs_level_t* mrxs_level = mrxs->levels + level_index;

			level_image->exists = true;
			level_image->needs_indexing = false;
			level_image->pyramid_image_index = level_index; // not used
			level_image->downsample_factor = exp2f((float)level_index);
			level_image->width_in_pixels = ceil_div_f64(image->width_in_pixels, level_image->downsample_factor);
			level_image->height_in_pixels = ceil_div_f64(image->height_in_pixels, level_image->downsample_factor);
			level_image->width_in_tiles = mrxs->has_overlapping_tiles
			                              ? (u32)ceil_div_i64(level_image->width_in_pixels, mrxs_level->tile_width)
			                              : mrxs_level->width_in_tiles;
			ASSERT(level_image->width_in_tiles > 0);
			level_image->height_in_tiles = mrxs->has_overlapping_tiles
			                               ? (u32)ceil_div_i64(level_image->height_in_pixels, mrxs_level->tile_height)
			                               : mrxs_level->height_in_tiles;
			ASSERT(level_image->height_in_tiles > 0);
			level_image->tile_count = level_image->height_in_tiles * level_image->width_in_tiles;
			level_image->tile_width = mrxs_level->tile_width;
			level_image->tile_height = mrxs_level->tile_height;
			if (mrxs_level->tile_width != image->tile_width) {
				ASSERT(!"tile width is not equal across all levels");
				return false;
			}
			if (mrxs_level->tile_height != image->tile_height) {
				ASSERT(!"tile height is not equal across all levels");
				return false;
			}
			level_image->um_per_pixel_x = level_image->downsample_factor * mrxs->mpp_x;
			level_image->um_per_pixel_y = level_image->downsample_factor * mrxs->mpp_y;
			level_image->x_tile_side_in_um = level_image->um_per_pixel_x * mrxs_level->tile_width;
			level_image->y_tile_side_in_um = level_image->um_per_pixel_y * mrxs_level->tile_height;
			ASSERT(level_image->x_tile_side_in_um > 0);
			ASSERT(level_image->y_tile_side_in_um > 0);
			level_image->origin_offset = V2F(0,0); //mrxs_level->origin_offset;
			level_image->tiles = (tile_t*) calloc(1, level_image->tile_count * sizeof(tile_t));
			for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
				tile_t* tile = level_image->tiles + tile_index;
				// Facilitate some introspection by storing self-referential information
				// in the tile_t struct. This is needed for some specific cases where we
				// pass around pointers to tile_t structs without caring exactly where they
				// came from.
				// (Specific example: we use this when exporting a selected region as BigTIFF)
				tile->tile_index = tile_index;
				tile->tile_x = tile_index % level_image->width_in_tiles;
				tile->tile_y = tile_index / level_image->width_in_tiles;

				if (!mrxs->has_overlapping_tiles && tile_index < mrxs_level->height_in_tiles * mrxs_level->width_in_tiles) {
					mrxs_tile_t* mrxs_tile = mrxs_level->tiles + tile_index;
					if (mrxs_tile->hier_entry.length == 0) {
						tile->is_empty = true;
					}
				}
			}
			DUMMY_STATEMENT;
		}
	}

	u8* thumbnail_pixels = mrxs_decode_simple_image_to_rgba(mrxs, &mrxs->thumbnail_image);
	if (thumbnail_pixels) {
		image->macro_image.pixels = thumbnail_pixels;
		image->macro_image.width = mrxs->thumbnail_image.width;
		image->macro_image.height = mrxs->thumbnail_image.height;
		image->macro_image.mpp = image->width_in_um / (float)MAX(1, image->macro_image.width);
		image->macro_image.world_pos = V2F(0, 0);
		image->macro_image.is_valid = true;
	}

	u8* barcode_pixels = mrxs_decode_simple_image_to_rgba(mrxs, &mrxs->barcode_image);
	if (barcode_pixels) {
		image->label_image.pixels = barcode_pixels;
		image->label_image.width = mrxs->barcode_image.width;
		image->label_image.height = mrxs->barcode_image.height;
		image->label_image.mpp = image->macro_image.is_valid ? image->macro_image.mpp : image->mpp_x;
		if (image->macro_image.is_valid) {
			image->label_image.world_pos.x = image->macro_image.width * image->macro_image.mpp;
			image->label_image.world_pos.y = 0.0f;
		}
		image->label_image.is_valid = true;
	}

	/*isyntax_image_t* macro_image = isyntax->images + isyntax->macro_image_index;
	if (macro_image->image_type == ISYNTAX_IMAGE_TYPE_MACROIMAGE) {
		if (macro_image->pixels) {
			image->macro_image.pixels = macro_image->pixels;
			macro_image->pixels = NULL; // transfer ownership
			image->macro_image.width = macro_image->width;
			image->macro_image.height = macro_image->height;
			image->macro_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
			image->macro_image.world_pos.x = -((float)base_level_instance->offset_x * isyntax->mpp_x);
			image->macro_image.world_pos.y = -((float)base_level_instance->offset_y * isyntax->mpp_y);
			image->macro_image.is_valid = true;
		}
	}
	isyntax_image_t* label_image = isyntax->images + isyntax->label_image_index;
	if (label_image->image_type == ISYNTAX_IMAGE_TYPE_LABELIMAGE) {
		if (label_image->pixels) {
			image->label_image.pixels = label_image->pixels;
			label_image->pixels = NULL; // transfer ownership
			image->label_image.width = label_image->width;
			image->label_image.height = label_image->height;
			image->label_image.mpp = 0.0315f * 1000.0f; // apparently, always this value
//			image->label_image.world_pos.x = -((float)wsi_image->offset_x * isyntax->mpp_x);
//			image->label_image.world_pos.y = -((float)wsi_image->offset_y * isyntax->mpp_y);
			image->label_image.is_valid = true;
		}
	}*/


	image->is_valid = true;
	image->is_freshly_loaded = true;
	return image->is_valid;
}

bool init_image_from_stbi(image_t* image, simple_image_t* simple, bool is_overlay) {
    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_STBI;
    image->simple = *simple;
    simple = &image->simple;
    image->is_freshly_loaded = true;

    image->mpp_x = 1.0f;
    image->mpp_y = 1.0f;
    image->is_mpp_known = false;
    image->tile_width = simple->width;
    image->tile_height = simple->height;
    image->width_in_pixels = simple->width;
    image->width_in_um = (float)simple->width * image->mpp_x;
    image->height_in_pixels = simple->height;
    image->height_in_um = (float)simple->height * image->mpp_y;

	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
	image->clip_rect_count = 1;

    image->level_count = 1;
    level_image_t* level_image = image->level_images + 0;
    memset(level_image, 0, sizeof(*level_image));

    level_image->exists = true;
    level_image->pyramid_image_index = 0; // not used
    level_image->downsample_factor = 1.0f;
    level_image->width_in_pixels = image->width_in_pixels;
    level_image->height_in_pixels = image->height_in_pixels;
    level_image->tile_count = 1;
    level_image->width_in_tiles = 1;
    ASSERT(level_image->width_in_tiles > 0);
    level_image->height_in_tiles = 1;
    level_image->tile_width = image->width_in_pixels;
    level_image->tile_height = image->height_in_pixels;
    level_image->um_per_pixel_x = level_image->downsample_factor * image->mpp_x;
    level_image->um_per_pixel_y = level_image->downsample_factor * image->mpp_y;
    level_image->x_tile_side_in_um = level_image->um_per_pixel_x * image->tile_width;
    level_image->y_tile_side_in_um = level_image->um_per_pixel_x * image->tile_height;
    ASSERT(level_image->x_tile_side_in_um > 0);
    ASSERT(level_image->y_tile_side_in_um > 0);
    level_image->origin_offset = (v2f){};
    level_image->tiles = (tile_t*) calloc(1, level_image->tile_count * sizeof(tile_t));
    for (i32 tile_index = 0; tile_index < level_image->tile_count; ++tile_index) {
        tile_t* tile = level_image->tiles + tile_index;
        // Facilitate some introspection by storing self-referential information
        // in the tile_t struct. This is needed for some specific cases where we
        // pass around pointers to tile_t structs without caring exactly where they
        // came from.
        // (Specific example: we use this when exporting a selected region as BigTIFF)
        tile->tile_index = tile_index;
        tile->tile_x = tile_index % level_image->width_in_tiles;
        tile->tile_y = tile_index / level_image->width_in_tiles;
    }

    image->is_valid = true;
    image->is_freshly_loaded = true;
    return image->is_valid;
}

// TODO: solve issue on macOS with OpenSlide backend, not all tiles displaying properly (Retina screen related maybe?)

void init_image_from_openslide(image_t* image, wsi_t* wsi, bool is_overlay) {
    image->type = IMAGE_TYPE_WSI;
    image->backend = IMAGE_BACKEND_OPENSLIDE;
    image->openslide_wsi = *wsi;
    image->is_freshly_loaded = true;
    image->mpp_x = wsi->mpp_x;
    image->mpp_y = wsi->mpp_y;
    image->is_mpp_known = wsi->is_mpp_known;
    image->tile_width = wsi->tile_width;
    image->tile_height = wsi->tile_height;
    image->width_in_pixels = wsi->width;
    image->width_in_um = (float)wsi->width * wsi->mpp_x;
    image->height_in_pixels = wsi->height;
    image->height_in_um = (float)wsi->height * wsi->mpp_y;

	image->clip_rects[0] = RECT2F(0.0f, 0.0f, image->width_in_um, image->height_in_um);
	image->clip_rect_count = 1;

    ASSERT(wsi->levels[0].x_tile_side_in_um > 0);
    if (wsi->level_count > 0 && wsi->levels[0].x_tile_side_in_um > 0) {
        ASSERT(wsi->max_downsample_level >= 0);

        memset(image->level_images, 0, sizeof(image->level_images));
        image->level_count = wsi->max_downsample_level + 1;

        // TODO: check against downsample level, see init_image_from_tiff()
        if (wsi->level_count > image->level_count) {
            fatal_error();
        }
        if (image->level_count > WSI_MAX_LEVELS) {
            fatal_error();
        }

        i32 wsi_level_index = 0;
        i32 next_wsi_level_index_to_check_for_match = 0;
        wsi_level_t *wsi_file_level = wsi->levels + wsi_level_index;
        for (i32 downsample_level = 0; downsample_level < image->level_count; ++downsample_level) {
            level_image_t *downsample_level_image = image->level_images + downsample_level;
            i32 wanted_downsample_level = downsample_level;
            bool found_wsi_level_for_downsample_level = false;
            for (wsi_level_index = next_wsi_level_index_to_check_for_match;
                wsi_level_index < wsi->level_count; ++wsi_level_index) {
                wsi_file_level = wsi->levels + wsi_level_index;
                if (wsi_file_level->downsample_level == wanted_downsample_level) {
                    // match!
                    found_wsi_level_for_downsample_level = true;
                    next_wsi_level_index_to_check_for_match =
                            wsi_level_index + 1; // next iteration, don't reuse the same WSI level!
                    break;
                }
            }

            if (found_wsi_level_for_downsample_level) {
                // The current downsampling level is backed by a corresponding IFD level image in the TIFF.
                downsample_level_image->exists = true;
                downsample_level_image->pyramid_image_index = wsi_level_index;
                downsample_level_image->downsample_factor = wsi_file_level->downsample_factor;
                downsample_level_image->tile_count = wsi_file_level->tile_count;
                downsample_level_image->width_in_pixels = wsi_file_level->width;
                downsample_level_image->height_in_pixels = wsi_file_level->height;
                downsample_level_image->width_in_tiles = wsi_file_level->width_in_tiles;
                ASSERT(downsample_level_image->width_in_tiles > 0);
                downsample_level_image->height_in_tiles = wsi_file_level->height_in_tiles;
                downsample_level_image->tile_width = wsi_file_level->tile_width;
                downsample_level_image->tile_height = wsi_file_level->tile_height;
#if DO_DEBUG
                if (downsample_level_image->tile_width != image->tile_width) {
                        console_print("Warning: level image %d (WSI level #%d) tile width (%d) does not match base level (%d)\n", downsample_level, wsi_level_index, downsample_level_image->tile_width, image->tile_width);
                    }
                    if (downsample_level_image->tile_height != image->tile_height) {
                        console_print("Warning: level image %d (WSI level #%d) tile width (%d) does not match base level (%d)\n", downsample_level, wsi_level_index, downsample_level_image->tile_width, image->tile_width);
                    }
#endif
                downsample_level_image->um_per_pixel_x = wsi_file_level->um_per_pixel_x;
                downsample_level_image->um_per_pixel_y = wsi_file_level->um_per_pixel_y;
                downsample_level_image->x_tile_side_in_um = wsi_file_level->x_tile_side_in_um;
                downsample_level_image->y_tile_side_in_um = wsi_file_level->y_tile_side_in_um;
                ASSERT(downsample_level_image->x_tile_side_in_um > 0);
                ASSERT(downsample_level_image->y_tile_side_in_um > 0);
                downsample_level_image->tiles = (tile_t *) calloc(1, wsi_file_level->tile_count * sizeof(tile_t));
                // Note: OpenSlide doesn't allow us to quickly check if tiles are empty or not.
                for (i32 tile_index = 0; tile_index < downsample_level_image->tile_count; ++tile_index) {
                    tile_t *tile = downsample_level_image->tiles + tile_index;
                    // Facilitate some introspection by storing self-referential information
                    // in the tile_t struct. This is needed for some specific cases where we
                    // pass around pointers to tile_t structs without caring exactly where they
                    // came from.
                    // (Specific example: we use this when exporting a selected region as BigTIFF)
                    tile->tile_index = tile_index;
                    tile->tile_x = tile_index % downsample_level_image->width_in_tiles;
                    tile->tile_y = tile_index / downsample_level_image->width_in_tiles;
                }
            } else {
                // The current downsampling level has no corresponding IFD level image :(
                // So we need only some placeholder information.
                downsample_level_image->exists = false;
                downsample_level_image->downsample_factor = exp2f((float) wanted_downsample_level);
                // Just in case anyone tries to divide by zero:
                downsample_level_image->tile_width = image->tile_width;
                downsample_level_image->tile_height = image->tile_height;
                downsample_level_image->um_per_pixel_x = image->mpp_x * downsample_level_image->downsample_factor;
                downsample_level_image->um_per_pixel_y = image->mpp_y * downsample_level_image->downsample_factor;
                downsample_level_image->x_tile_side_in_um =
                        downsample_level_image->um_per_pixel_x * (float) wsi->levels[0].tile_width;
                downsample_level_image->y_tile_side_in_um =
                        downsample_level_image->um_per_pixel_y * (float) wsi->levels[0].tile_height;
            }

        }

    }
    ASSERT(image->level_count > 0);
    image->is_valid = true;

}

void unload_openslide_wsi(wsi_t* wsi) {
    if (wsi->osr) {
        openslide.close(wsi->osr);
        wsi->osr = NULL;
    }
}


// TODO: optimize?
float f32_rgb_to_f32_y(float R, float G, float B) {
    float Co  = R - B;
    float tmp = B + Co/2;
    float Cg  = G - tmp;
    float Y   = tmp + Cg/2;
    return Y;
}

// TODO: optimize?
void image_convert_u8_rgba_to_f32_y(u8* src, float* dest, i32 w, i32 h, i32 components) {
    if (components == 3 || components == 4) {
        i32 row_elements = w * components;
        for (i32 y = 0; y < h; ++y) {
            u8* src_pixel = src + y * row_elements;
            float* dst_pixel = dest + y * w;
            for (i32 x = 0; x < w; ++x) {
                float r = (float)(src_pixel[0]) * (1.0f/255.0f);
                float g = (float)(src_pixel[1]) * (1.0f/255.0f);
                float b = (float)(src_pixel[2]) * (1.0f/255.0f);
                float luminance = f32_rgb_to_f32_y(r, g, b);
                *dst_pixel++ = luminance;
                src_pixel += components;
            }
        }
    } else {
        printf("number of components (%d) not supported\n", components);
        exit(1);
    }
}

// TODO: optimize?
void image_convert_u8_bgra_to_f32_y(u8* src, float* dest, i32 w, i32 h, i32 components) {
	if (components == 3 || components == 4) {
		i32 row_elements = w * components;
		for (i32 y = 0; y < h; ++y) {
			u8* src_pixel = src + y * row_elements;
			float* dst_pixel = dest + y * w;
			for (i32 x = 0; x < w; ++x) {
				float b = (float)(src_pixel[0]) * (1.0f/255.0f);
				float g = (float)(src_pixel[1]) * (1.0f/255.0f);
				float r = (float)(src_pixel[2]) * (1.0f/255.0f);
				float luminance = f32_rgb_to_f32_y(r, g, b);
				*dst_pixel++ = luminance;
				src_pixel += components;
			}
		}
	} else {
		printf("number of components (%d) not supported\n", components);
		exit(1);
	}
}

void do_invert_colors(u8* pixels, i32 w, i32 h, enum pixel_format_enum pixel_format) {
	if (pixel_format == PIXEL_FORMAT_U8_BGRA || pixel_format == PIXEL_FORMAT_U8_RGBA) {
		u32 pixel_count = w * h;
		u8* pos = pixels;
		for (u32 j = 0; j < pixel_count; ++j) {
			pos[0] = 255 - pos[0];
			pos[1] = 255 - pos[1];
			pos[2] = 255 - pos[2];
			pos += 4;
		}
	} else {
		console_print_error("do_invert_colors(): unsupported/unknown pixel format\n");
	}
}

static bool image_read_region_impl(image_t* image, i32 level, i32 x, i32 y, i32 w, i32 h,
                                   void* dest, pixel_format_enum desired_pixel_format) {
    ASSERT(dest != NULL);

	// TODO: make inverting colors configurable
	bool8 invert_colors = false;

	if (w <= 0 || h <= 0) {
		return false;
	}
	if (level < 0 || level >= image->level_count) {
		console_print_error("image_read_region(): level %d out of bounds (valid range 0-%d)\n", level, image->level_count-1);
		return false;
	}

    pixel_format_enum intermediate_pixel_format = PIXEL_FORMAT_UNDEFINED;
    void* intermediate_pixel_buffer = NULL;

    switch (image->backend) {
        default: {
            console_print_error("image_read_region(): not implemented for backend '%s'\n", get_image_backend_name(image));
            return false;
        } break;
        case IMAGE_BACKEND_OPENSLIDE: {
            intermediate_pixel_format = PIXEL_FORMAT_U8_BGRA;
            if (desired_pixel_format == intermediate_pixel_format) {
                intermediate_pixel_buffer = (uint32_t*) dest;
            } else {
                intermediate_pixel_buffer = malloc(w * h * sizeof(uint32_t));
            }
            openslide.read_region(image->openslide_wsi.osr, intermediate_pixel_buffer, x, y, level, w, h);

            // Check for (partially) empty tiles
            u32 pixel_count = w * h;
            u32* pixels = (u32*)intermediate_pixel_buffer;
            u32 nonempty_pixel_count = 0;
            u32 image_background_color = image->is_background_black ? 0 /*black*/ : 0xFFFFFFFF; // white
            for (i32 i = 0; i < pixel_count; ++i) {
                // Fill in any empty pixels with the background color.
                u32 p = pixels[i];
                nonempty_pixel_count += (p > 0);
                pixels[i] = p ? p : image_background_color;
            }
        } break;
        case IMAGE_BACKEND_TIFF:
        case IMAGE_BACKEND_DICOM:
        case IMAGE_BACKEND_DICOMWEB:
        case IMAGE_BACKEND_MRXS:
        case IMAGE_BACKEND_STBI: {
			level_image_t* level_image = image->level_images + level;
			ASSERT(level_image->exists);

			i32 tile_width = (i32)level_image->tile_width;
			i32 tile_height = (i32)level_image->tile_height;

			i32 start_tile_x;
			i32 end_tile_x;
			i32 x_remainder;
			i32 x_remainder_last;

			// Round down to the next lower multiple of tile_width, even when x < 0
			start_tile_x = (x >= 0) ? x / tile_width : (x - tile_width + 1) / tile_width;
			end_tile_x = ((x + w - 1) >= 0) ? (x + w - 1) / tile_width : ((x + w - 1) - tile_width + 1) / tile_width;

			// Normalize the remainder into [0, tile_width - 1], even for negative x.
			x_remainder = ((x % tile_width) + tile_width) % tile_width;
			x_remainder_last = (((x + w - 1) % tile_width) + tile_width) % tile_width;

			i32 start_tile_y;
			i32 end_tile_y;
			i32 y_remainder;
			i32 y_remainder_last;

			// Round down to the next lower multiple of tile_height, even when y < 0
			start_tile_y = (y >= 0) ? y / tile_height : (y - tile_height + 1) / tile_height;
			end_tile_y = ((y + h - 1) >= 0) ? (y + h - 1) / tile_height : ((y + h - 1) - tile_height + 1) / tile_height;

			// Normalize the remainder into [0, tile_height - 1], even for negative y.
			y_remainder = ((y % tile_height) + tile_height) % tile_height;
			y_remainder_last = (((y + h - 1) % tile_height) + tile_height) % tile_height;

			// Read all source tiles needed for this region
			bounds2i level_tiles_bounds = BOUNDS2I(0, 0, (i32)level_image->width_in_tiles, (i32)level_image->height_in_tiles);
			bounds2i region_tiles = BOUNDS2I(start_tile_x, start_tile_y, end_tile_x + 1, end_tile_y + 1);
			bounds2i tiles_within_level_bounds = clip_bounds2i(region_tiles, level_tiles_bounds);
			i32 width_in_tiles_to_read = tiles_within_level_bounds.right - tiles_within_level_bounds.left;
			i32 height_in_tiles_to_read = tiles_within_level_bounds.bottom - tiles_within_level_bounds.top;

			if (width_in_tiles_to_read > 0 && height_in_tiles_to_read > 0) {
				load_tile_task_t *wishlist = calloc(width_in_tiles_to_read * height_in_tiles_to_read, sizeof(load_tile_task_t));
				i32 tiles_to_load = 0;

				task_group_t read_task_group = {0};

                // NOTE: we are getting an exclusive lock for the purpose of requesting image tiles
                // TODO: check if we can do without locking here? Shouldn't this be on the tile level rather than globally for the whole image?
                platform_mutex_lock(&image->lock);

				// Retain every source tile used by this read. Parallel read_region() callers can overlap:
				// one caller may have observed a shared tile as ready before another caller finishes and
				// releases it. Keep tile pixels alive until all overlapping readers have copied from them.
				for (i32 tile_y = tiles_within_level_bounds.min.y; tile_y < tiles_within_level_bounds.max.y; ++tile_y) {
					for (i32 tile_x = tiles_within_level_bounds.min.x; tile_x < tiles_within_level_bounds.max.x; ++tile_x) {
						tile_t* tile = get_tile(level_image, tile_x, tile_y);
						if (!tile->is_empty) {
							tile_cache_pin_cpu_tile(image, level, tile->tile_index, TILE_CACHE_DEMAND_READ_REGION);
						}
					}
				}

				// request tiles
				for (i32 tile_y = tiles_within_level_bounds.min.y; tile_y < tiles_within_level_bounds.max.y; ++tile_y) {
					for (i32 tile_x = tiles_within_level_bounds.min.x; tile_x < tiles_within_level_bounds.max.x; ++tile_x) {
						tile_t *tile = get_tile(level_image, tile_x, tile_y);
						if (tile->is_empty) continue; // no need to load empty tiles
						if (tile_cache_tile_has_cpu_pixels(image, level, tile->tile_index)) {
							continue; // already cached
						}
						if (tile_cache_tile_is_busy(image, level, tile->tile_index)) {
							continue; // another read_region() or viewer request is already loading it
						}
						wishlist[tiles_to_load++] = (load_tile_task_t) {
								.resource_id = image->resource_id,
								.image = image,
								.level = level,
								.tile_index = tile->tile_index,
								.need_gpu_residency = false,
								.need_cpu_residency = true,
								.invert_colors = invert_colors,
								.task_group = &read_task_group,
								.refcount_to_decrement = 1, // refcount will be decremented at end of thread proc load_tile_func()
						};

					}
				}

				i32 tile_loads_submitted = tile_loader_submit_requests(image, wishlist, tiles_to_load);
				i32 tile_loads_completed = 0;

                platform_mutex_unlock(&image->lock);

				// Retrieve requested tiles. Multiple export workers can read adjacent regions whose source
				// tile coverage overlaps, so some needed tiles may have been submitted by another caller.
				// Wait until every required tile is cached/empty, not just until this call's submissions finish.
				for (;;) {
					tile_cache_result_t cache_result = {0};
					if (tile_cache_poll_load_result(image, &cache_result)) {
						platform_mutex_lock(&image->lock);
						++tile_loads_completed;
						tile_cache_result_action_t action = {0};
						tile_cache_apply_result(image, &cache_result, false, &action);
						if (action.mark_empty) {
							tile_t* tile = get_tile_from_tile_index(image, cache_result.level, cache_result.tile_index);
							tile->is_empty = true;
						}
						platform_mutex_unlock(&image->lock);
						free(action.pixels_to_free);
					} else {
						bool all_tiles_ready = true;
						i32 retry_tiles_to_load = 0;
                        platform_mutex_lock(&image->lock);
						for (i32 tile_y = tiles_within_level_bounds.min.y; tile_y < tiles_within_level_bounds.max.y; ++tile_y) {
							for (i32 tile_x = tiles_within_level_bounds.min.x; tile_x < tiles_within_level_bounds.max.x; ++tile_x) {
								tile_t *tile = get_tile(level_image, tile_x, tile_y);
								if (!tile->is_empty && !tile_cache_tile_has_cpu_pixels(image, level, tile->tile_index)) {
									all_tiles_ready = false;
									if (!tile_cache_tile_is_busy(image, level, tile->tile_index)) {
										wishlist[retry_tiles_to_load++] = (load_tile_task_t) {
												.resource_id = image->resource_id,
												.image = image,
												.level = level,
												.tile_index = tile->tile_index,
												.need_gpu_residency = false,
												.need_cpu_residency = true,
												.invert_colors = invert_colors,
												.task_group = &read_task_group,
												.refcount_to_decrement = 1,
										};
									}
								}
							}
						}
						if (retry_tiles_to_load > 0) {
							tile_loads_submitted += tile_loader_submit_requests(image, wishlist, retry_tiles_to_load);
						}
                        platform_mutex_unlock(&image->lock);

						if (all_tiles_ready) {
							if (task_group_is_complete(&read_task_group)) {
								break;
							} else {
								// It seems we're done, BUT: we still need to process tasks that not yet completed
								console_print("image_read_region(): all tiles are ready, but not all submitted jobs have returned yet (%d/%d)\n", tile_loads_submitted, tile_loads_completed);
								platform_sleep(1);
								continue;
							}
						} else if (thread_pool_is_work_waiting_to_start(&global_thread_pool)) {
							thread_pool_do_work(&global_thread_pool);
						} else {
							platform_sleep(1);
						}
					}
				}

				// NOTE: careful: we need to make sure that all submitted tasks are processed (completion work queue might otherwise be accessed after destruction)
				if (!task_group_is_complete(&read_task_group)) {
					fatal_error();
				}
				free(wishlist);


			}

			intermediate_pixel_format = PIXEL_FORMAT_U8_BGRA;
			if (desired_pixel_format == intermediate_pixel_format) {
				intermediate_pixel_buffer = (uint32_t*) dest;
			} else {
				intermediate_pixel_buffer = malloc(w * h * sizeof(uint32_t));
			}

			// Fill in background
			u8 image_background_color = image->is_background_black ? 0 /*black*/ : 0xFF; // white
			memset(intermediate_pixel_buffer, image_background_color, w * h * sizeof(uint32_t));

			// Allocate memory for tile pixels (will reuse for consecutive libisyntax_tile_read() calls)
//			uint32_t* tile_pixels = (uint32_t*)malloc(tile_width * tile_height * sizeof(uint32_t));

			// Read tiles and copy the relevant portion of each tile to the region
            platform_mutex_lock(&image->lock);
			for (i32 tile_y = start_tile_y; tile_y <= end_tile_y; ++tile_y) {
				for (i32 tile_x = start_tile_x; tile_x <= end_tile_x; ++tile_x) {
					// Calculate the portion of the tile to be copied
					i32 src_x = (tile_x == start_tile_x) ? x_remainder : 0;
					i32 src_y = (tile_y == start_tile_y) ? y_remainder : 0;
					i32 dest_x = (tile_x == start_tile_x) ? 0 : (tile_x - start_tile_x) * tile_width - x_remainder;
					i32 dest_y = (tile_y == start_tile_y) ? 0 : (tile_y - start_tile_y) * tile_height - y_remainder;
					i32 copy_width = (tile_x == end_tile_x) ? x_remainder_last - src_x + 1 : tile_width - src_x;
					i32 copy_height = (tile_y == end_tile_y) ? y_remainder_last - src_y + 1 : tile_height - src_y;

					if (tile_x >= 0 && tile_y >= 0 && tile_x < level_image->width_in_tiles && tile_y < level_image->height_in_tiles) {
						// Source tile is within range -> try to read its pixels
						tile_t* tile = get_tile(level_image, tile_x, tile_y);
						u8* cached_pixels = tile_cache_get_cpu_pixels(image, level, tile->tile_index);
						if (!tile->is_empty && cached_pixels) {
							// Copy the relevant portion of the tile to the region
							u32* pixels = (u32*)cached_pixels;
							for (int64_t i = 0; i < copy_height; ++i) {
								int64_t dest_index = (dest_y + i) * w + dest_x;
								int64_t src_index = (src_y + i) * tile_width + src_x;
								memcpy(((u32*)intermediate_pixel_buffer) + dest_index,
								       pixels + src_index,
								       copy_width * sizeof(uint32_t));
							}
						}
					}

				}
			}

			// release tiles
			// TODO: proper tile caching
			for (i32 tile_y = tiles_within_level_bounds.min.y; tile_y < tiles_within_level_bounds.max.y; ++tile_y) {
				for (i32 tile_x = tiles_within_level_bounds.min.x; tile_x < tiles_within_level_bounds.max.x; ++tile_x) {
					tile_t* tile = get_tile(level_image, tile_x, tile_y);
					tile_cache_unpin_cpu_tile(image, level, tile->tile_index, TILE_CACHE_DEMAND_READ_REGION);
				}
			}
            platform_mutex_unlock(&image->lock);



		} break;
		case IMAGE_BACKEND_ISYNTAX: {
			enum isyntax_pixel_format_t translated_pixel_format = 0;
			if (desired_pixel_format == PIXEL_FORMAT_U8_BGRA) {
				intermediate_pixel_format = PIXEL_FORMAT_U8_BGRA;
				translated_pixel_format = LIBISYNTAX_PIXEL_FORMAT_BGRA;
			} else if (desired_pixel_format == PIXEL_FORMAT_U8_RGBA || desired_pixel_format == PIXEL_FORMAT_F32_Y) {
				intermediate_pixel_format = PIXEL_FORMAT_U8_RGBA;
				translated_pixel_format = LIBISYNTAX_PIXEL_FORMAT_RGBA;
			} else {
				console_print_error("image_read_region(): unknown/unsupported pixel format for backend '%s'\n", get_image_backend_name(image));
				return false;
			}
			isyntax_t* isyntax = &image->isyntax;
			if (!isyntax->cache) {
				i32 cache_size = 2000;
				if (libisyntax_cache_create("isyntax-to-tiff cache", cache_size, &isyntax->cache) != LIBISYNTAX_OK) {
					fatal_error("Failed to create iSyntax cache");
					return false;
				}
				// Inject the already initialized block allocators to the cache
				isyntax_cache_t* cache = isyntax->cache;
				cache->allocator_block_width = isyntax->block_width;
				cache->allocator_block_height = isyntax->block_height;
				cache->ll_coeff_block_allocator = isyntax->ll_coeff_block_allocator;
				cache->h_coeff_block_allocator = isyntax->h_coeff_block_allocator;
				cache->is_block_allocator_owned = false;
			}

			if (desired_pixel_format == intermediate_pixel_format) {
				intermediate_pixel_buffer = (uint32_t*) dest;
			} else {
				intermediate_pixel_buffer = malloc(w * h * sizeof(uint32_t));
			}

			// NOTE: libisyntax adjusts the x and y read position to account for the iSyntax padding area.
			// However, Slidescape keeps the (0,0) origin at the edge of the padding area instead (not the actual image).
			// So we need to pre-emptively 'undo' the offsetting that libisyntax does.
			// TODO: change Slidescape origin to keep coordinate system the same as libisyntax?
			isyntax_image_t* wsi = &isyntax->images[0];
			int32_t offset = ((3 << wsi->level_count) - 3) >> level;
			x -= offset;
			y -= offset;

			if (!wsi->first_load_complete && !wsi->first_load_in_progress) {
				isyntax_do_first_load_immediately(isyntax, wsi, image->resource_id, TILE_LOADER_COMPLETION_EVENT_TILE_LOADED);
			}

			if (libisyntax_read_region(&image->isyntax, isyntax->cache, level, x, y, w, h, intermediate_pixel_buffer, translated_pixel_format) != LIBISYNTAX_OK) {
				console_print_error("image_read_region(): unknown error in libisyntax_read_region()\n");
				return false;
			}
		} break;
    }

	if (intermediate_pixel_buffer && invert_colors) {
		do_invert_colors(intermediate_pixel_buffer, w, h, intermediate_pixel_format);
	}


    bool success = true;
    if (intermediate_pixel_format == desired_pixel_format) {
        return true; // we're already done!
    } else {
        // need to convert between pixel formats
        if (intermediate_pixel_format == PIXEL_FORMAT_U8_BGRA) {
            if (desired_pixel_format == PIXEL_FORMAT_F32_Y) {
                image_convert_u8_bgra_to_f32_y(intermediate_pixel_buffer, dest, w, h, 4);
            } else if (desired_pixel_format == PIXEL_FORMAT_U8_RGBA) {
                // swap RGBA to BGRA
                i32 pixel_count = w * h;
                for (i32 i = 0; i < pixel_count; ++i) {
                    u32 pixel = ((u32*)intermediate_pixel_buffer)[i];
#if HOST_BIG_ENDIAN
                    u32 new_pixel = (pixel & 0x00FF00FF) | ((pixel >> 16) & 0x0000FF00) | ((pixel << 16) & 0xFF000000);
#else
                    u32 new_pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
#endif
                    ((u32*)dest)[i] = new_pixel;
                }
            } else {
                console_print_error("image_read_region(): pixel conversion (%d to %d) not implemented\n", intermediate_pixel_format, desired_pixel_format);
                success = false;
            }
        } else if (intermediate_pixel_format == PIXEL_FORMAT_U8_RGBA) {
        	if (desired_pixel_format == PIXEL_FORMAT_F32_Y) {
        		image_convert_u8_rgba_to_f32_y(intermediate_pixel_buffer, dest, w, h, 4);
        	} else if (desired_pixel_format == PIXEL_FORMAT_U8_RGBA) {
        		// swap BGRA to RGBA
        		i32 pixel_count = w * h;
        		for (i32 i = 0; i < pixel_count; ++i) {
        			u32 pixel = ((u32*)intermediate_pixel_buffer)[i];
#if HOST_BIG_ENDIAN
        			u32 new_pixel = (pixel & 0x00FF00FF) | ((pixel >> 16) & 0x0000FF00) | ((pixel << 16) & 0xFF000000);
#else
        			u32 new_pixel = (pixel & 0xFF00FF00) | ((pixel & 0xFF) << 16) | ((pixel >> 16) & 0xFF);
#endif
        			((u32*)dest)[i] = new_pixel;
        		}
        	} else {
        		console_print_error("image_read_region(): pixel conversion (%d to %d) not implemented\n", intermediate_pixel_format, desired_pixel_format);
        		success = false;
        	}
        }
        if (intermediate_pixel_buffer) free(intermediate_pixel_buffer);
    }
    return success;
}

bool image_read_region(image_t* image, i32 level, i32 x, i32 y, i32 w, i32 h,
                       void* dest, pixel_format_enum desired_pixel_format) {
	if (!tile_cache_begin_read(image)) {
		return false;
	}
	bool result = image_read_region_impl(image, level, x, y, w, h, dest, desired_pixel_format);
	tile_cache_end_read(image);
	return result;
}

void do_level_image_indexing(image_t* image, level_image_t* level_image, i32 scale) {
    if (image->backend == IMAGE_BACKEND_DICOM) {
        if (dicom_wsi_index_level(&image->dicom, level_image->pyramid_image_index)) {
            level_image->needs_indexing = false;
        }
    }
}

typedef struct level_indexing_task_t {
	image_t* image;
	level_image_t* level_image;
	i32 scale;
} level_indexing_task_t;


void level_image_indexing_task_func(i32 logical_thread_index, void* userdata) {
	level_indexing_task_t* task = (level_indexing_task_t*) userdata;
	do_level_image_indexing(task->image, task->level_image, task->scale);
	atomic_decrement(&task->image->refcount); // release
}

void begin_level_image_indexing(image_t* image, level_image_t* level_image, i32 scale) {
	ASSERT(!level_image->indexing_job_submitted);
	level_indexing_task_t task = {0};
	task.image = image;
	task.level_image = level_image;
	task.scale = scale;

	level_image->indexing_job_submitted = true;
	atomic_increment(&image->refcount); // retain
	if (!thread_pool_submit_task(&global_thread_pool, level_image_indexing_task_func, &task, sizeof(task))) {
		atomic_decrement(&image->refcount); // chicken out
		level_image->indexing_job_submitted = false;
	};
}

bool image_initialize_runtime(image_t* image) {
	if (!image || !image->is_valid) {
		return false;
	}
	bool initialized_lock = false;
	if (!image->lock_initialized) {
		platform_mutex_init(&image->lock);
		image->lock_initialized = true;
		initialized_lock = true;
	}
	if (!image->tile_cache) {
		image->tile_cache = tile_cache_create(image);
		if (!image->tile_cache) {
			if (initialized_lock) {
				platform_mutex_destroy(&image->lock);
				image->lock_initialized = false;
			}
			return false;
		}
	}
	return true;
}

static bool check_image_texture_destroyed(renderer_texture_handle_t texture) {
    if (texture == 0) {
        return true;
    } else {
        console_print_error("image_destroy(): texture %u is still in use (did you call renderer_destroy_image_resources() first?)\n", texture);
        ASSERT(!"check_image_texture_destroyed");
        return false;
    }
}

void image_destroy(image_t* image) {
	if (!image) {
		return;
	}
	tile_cache_begin_shutdown(image);
	while (tile_cache_get_active_read_count(image) > 0) {
		if (thread_pool_is_work_waiting_to_start(&global_thread_pool)) {
			thread_pool_do_work(&global_thread_pool);
		} else {
			platform_sleep(1);
		}
	}
    image->is_deleted = true;
    while (image->refcount > 0) {
//		console_print_error("refcount = %d\n", image->refcount);
	    if (thread_pool_is_work_waiting_to_start(&global_thread_pool)) {
		    thread_pool_do_work(&global_thread_pool);
	    } else {
		    platform_sleep(1);
	    }
    }
	if (image) {
		if (image->type == IMAGE_TYPE_WSI) {
			if (image->backend == IMAGE_BACKEND_OPENSLIDE) {
				unload_openslide_wsi(&image->openslide_wsi);
			} else if (image->backend == IMAGE_BACKEND_TIFF) {
				tiff_destroy(&image->tiff);
			} else if (image->backend == IMAGE_BACKEND_ISYNTAX) {
				isyntax_destroy(&image->isyntax);
			} else if (image->backend == IMAGE_BACKEND_DICOM) {
				dicom_destroy(&image->dicom);
			} else if (image->backend == IMAGE_BACKEND_MRXS) {
				mrxs_destroy(&image->mrxs);
			} else if (image->backend == IMAGE_BACKEND_SLIDE_SCORE) {
				DUMMY_STATEMENT;
			} else if (image->backend == IMAGE_BACKEND_DICOMWEB) {
				dicomweb_remote_image_destroy(&image->dicomweb);
			} else if (image->backend == IMAGE_BACKEND_STBI) {
				if (image->simple.pixels) {
					stbi_image_free(image->simple.pixels);
					image->simple.pixels = NULL;
				}
                check_image_texture_destroyed(image->simple.texture);
				image->simple.is_valid = false;
			} else {
				fatal_error("invalid image backend");
			}
		} else {
			fatal_error("invalid image type");
		}

		for (i32 i = 0; i < image->level_count; ++i) {
			level_image_t* level_image = image->level_images + i;
			if (level_image->tiles) {
				for (i32 j = 0; j < level_image->tile_count; ++j) {
                    check_image_texture_destroyed(tile_cache_get_gpu_texture(image, i, j));
				}
			}
			free(level_image->tiles);
			level_image->tiles = NULL;
		}

		tile_cache_destroy(image->tile_cache);
		image->tile_cache = NULL;

		if (image->macro_image.is_valid) {
			if (image->macro_image.pixels) stbi_image_free(image->macro_image.pixels);
            check_image_texture_destroyed(image->macro_image.texture);
			memset(&image->macro_image, 0, sizeof(image->macro_image));
		}
		if (image->label_image.is_valid) {
			if (image->label_image.pixels) stbi_image_free(image->label_image.pixels);
            check_image_texture_destroyed(image->label_image.texture);
            memset(&image->label_image, 0, sizeof(image->label_image));
		}

		if (image->lock_initialized) {
            platform_mutex_destroy(&image->lock);
			image->lock_initialized = false;
		}

	}
}
