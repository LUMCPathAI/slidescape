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
#include "mathutils.h"
#include "stringutils.h"

#include "tiff.h"
#include "viewer.h"
#include "image_resize.h"
#include "jpeg_decoder.h"
#include "platform_mutex.h"

#include "tiff_write.h"

#include "stb_image_write.h" //for debugging only

typedef struct export_region_task_t {
	i32 plane;
    app_state_t* app_state;
    image_t* image;
    bounds2f world_bounds;
    bounds2i level0_bounds;
    const char* filename;
    u32 export_tile_width;
    u16 desired_photometric_interpretation;
    i32 quality;
    u32 export_flags;
    bool need_resize;
    v2f target_mpp;
} export_region_task_t;

typedef struct offset_fixup_t {
	u64 offset_to_fix;
	u64 offset_from_unknown_base;
} offset_fixup_t;

static inline void add_fixup(memrw_t* fixups_buffer, u64 offset_to_fix, u64 offset_from_unknown_base) {
	// NOTE: 'offset_to_fix' can't be a direct pointer to the value that needs to be fixed up, because
	// pointers to the destination buffer might be unstable because the destination buffer might resize.
	// so instead we are storing it as an offset from the start of the destination buffer.
	offset_fixup_t new_fixup = {offset_to_fix, offset_from_unknown_base};
	memrw_push_back(fixups_buffer, &new_fixup, sizeof(new_fixup));
}

static inline u64 memrw_push_bigtiff_tag(memrw_t* buffer, raw_bigtiff_tag_t* tag) {
	u64 write_offset = memrw_push_back(buffer, tag, sizeof(*tag));
	return write_offset;
}

static u64 add_large_bigtiff_tag(memrw_t* tag_buffer, memrw_t* data_buffer, memrw_t* fixups_buffer,
                                                u16 tag_code, u16 tag_type, u64 tag_data_count, void* tag_data) {
	// NOTE: tag_data is allowed to be NULL, in that case we are only pushing placeholder data (zeroes)
	u32 field_size = get_tiff_field_size(tag_type);
	u64 tag_data_size = field_size * tag_data_count;
	if (tag_data_size <= 8) {
		raw_bigtiff_tag_t tag = {tag_code, tag_type, tag_data_count, .data_u64 = 0};
		if (tag_data) {
			memcpy(tag.data, tag_data, tag_data_size);
		}
		u64 write_offset = memrw_push_bigtiff_tag(tag_buffer, &tag);
		return write_offset;
	} else {
		u64 data_offset = memrw_push_back(data_buffer, tag_data, tag_data_size);
		raw_bigtiff_tag_t tag = {tag_code, tag_type, tag_data_count, .offset = data_offset};
		u64 write_offset = memrw_push_bigtiff_tag(tag_buffer, &tag);
		// NOTE: we cannot store a raw pointer to the offset we need to fix later, because the buffer
		// might resize (pointer is unstable).
		add_fixup(fixups_buffer, write_offset + offsetof(raw_bigtiff_tag_t, offset), data_offset);
		return write_offset;
	}
}

typedef struct image_draft_tile_t {
    image_buffer_t buffer;
    i32 tile_x;
    i32 tile_y;
    i32 tile_index;
    i32 level;
} image_draft_tile_t;

typedef struct image_draft_level_t {
    i32 level;
    i32 width_in_pixels;
    i32 height_in_pixels;
    i32 width_in_tiles;
    i32 height_in_tiles;
    i32 tile_count;
    image_draft_tile_t* tiles;
    u64 offset_of_tile_offsets;
    u64 offset_of_tile_bytecounts;
    bool are_tile_offsets_inlined_in_tag;
    u64* tile_offsets;
    u64* tile_bytecounts;
} image_draft_level_t;

typedef struct image_draft_t {
    i32 base_width;
    i32 base_height;
    i32 tile_width;
    i32 tile_height;
    i32 level_count;
    image_draft_level_t levels[10];
    image_t* source_image;
    bounds2i source_level0_bounds;
    i32 source_plane;
    i32 source_base_level;
    float base_downsample_factor_x;
    float base_downsample_factor_y;
    memrw_t tag_buffer;
    memrw_t small_data_buffer;
    memrw_t fixups_buffer;
    tiff_header_t header;
    bool is_mpp_known;
    bool is_background_black;
    bool need_resize;
    v2f mpp;
    u64 image_data_base_offset;
    u64 current_image_data_write_offset;
    i32 total_tiles_to_export;
    float progress_per_exported_tile;
    float supertile_width;
    float supertile_height;
    i32 supertile_width_read;
    i32 supertile_height_read;
    u16 desired_photometric_interpretation;
    i32 quality;
    platform_mutex_t write_lock;
} image_draft_t;

typedef struct construct_export_tile_task_t {
    image_draft_t* draft;
    image_draft_level_t* frontier;
    file_stream_t fp;
    volatile i32* next_tile_index;
    volatile i32* started_count;
    volatile i32* participants_goal;
    volatile i32* finished_count;
} construct_export_tile_task_t;

static void shrink_tile_and_propagate_to_next_level(image_draft_t* draft, image_draft_tile_t* tile) {
    if (tile->level + 1 < draft->level_count) {

        // Find the parent tile and initialize an image buffer for it if needed
        image_draft_level_t* parent_level = draft->levels + tile->level + 1;
        i32 parent_tile_x = tile->tile_x / 2;
        i32 parent_tile_y = tile->tile_y / 2;
        image_draft_tile_t* parent_tile = parent_level->tiles + parent_tile_y * parent_level->width_in_tiles + parent_tile_x;

        // TODO: how to make this safe for multithreading?
        if (!parent_tile->buffer.pixels) {
            parent_tile->buffer = create_bgra_image_buffer(draft->tile_width, draft->tile_height);
        }

        // Prepare a 'view' of the parent tile, for the quadrant we want to fill with a 2x2 shrink of the tile
        image_buffer_t dest = parent_tile->buffer;
        i32 quadrant = (tile->tile_x % 2) + 2 * (tile->tile_y % 2);
        dest.pixels = parent_tile->buffer.pixels;
        if (quadrant % 2 == 1) {
            dest.pixels += (tile->buffer.width / 2) * tile->buffer.channels; // right half
        }
        if (quadrant >= 2) {
            dest.pixels += tile->buffer.width * (tile->buffer.height / 2) * tile->buffer.channels; // bottom half
        }
        dest.width /= 2;
        dest.height /= 2;

        // Do the shrink
        image_shrink_2x2(&tile->buffer, &dest, (rect2i){0, 0, draft->tile_width, draft->tile_height});
    }
}

static void write_finished_bigtiff_tile(image_draft_t* draft, image_draft_tile_t* tile, file_stream_t fp) {

    bool use_rgb = (draft->desired_photometric_interpretation == TIFF_PHOTOMETRIC_RGB);

    u8* compressed_buffer = NULL;
    u64 compressed_size = 0;
    jpeg_encode_tile(tile->buffer.pixels, draft->tile_width, draft->tile_width, draft->quality, NULL, NULL,
                     &compressed_buffer, &compressed_size, use_rgb);

    // JPEG encoding is the expensive part and happens before the lock. Only the shared append position,
    // tile offset tables, and progress counters are serialized.
    platform_mutex_lock(&draft->write_lock);

    fseeko64(fp, draft->current_image_data_write_offset, SEEK_SET); // this needed?

    fwrite(compressed_buffer, compressed_size, 1, fp);

    image_draft_level_t* draft_level = draft->levels + tile->level;

    draft_level->tile_offsets[tile->tile_index] = draft->current_image_data_write_offset;
    draft_level->tile_bytecounts[tile->tile_index] = compressed_size;

    draft->current_image_data_write_offset += compressed_size;
    global_tiff_export_progress += draft->progress_per_exported_tile;

	// Console 'progress bar': write 20 dots to stdout during export ....................
	i32 prev_console_dots = global_tiff_export_progress_console_dots_written;
	global_tiff_export_progress_console_dots_written = (i32)(global_tiff_export_progress * 20.0f + 0.5f);
	i32 dots_to_write = global_tiff_export_progress_console_dots_written - prev_console_dots;
	for (i32 i = 0; i < dots_to_write; ++i) {
		putc('.', stdout);
	}

    platform_mutex_unlock(&draft->write_lock);

    libc_free(compressed_buffer);

}

static void construct_base_tile_with_resampling(image_draft_t* draft, image_draft_tile_t* tile, file_stream_t fp) {

    temp_memory_t temp = begin_temp_memory_on_local_thread();


    if (draft->need_resize) {
        bool supertile_need_destroy = false;
        bool resized_tile_need_destroy = false;
        image_buffer_t resized_tile = create_bgra_image_buffer_using_arena(temp.arena, draft->tile_width, draft->tile_height);
        if (!resized_tile.is_valid) {
            resized_tile = create_bgra_image_buffer(draft->tile_width, draft->tile_height);
            resized_tile_need_destroy = true;
        }
        image_buffer_t supertile = create_bgra_image_buffer_using_arena(temp.arena, draft->supertile_width_read, draft->supertile_height_read);
        if (!supertile.is_valid) {
            supertile = create_bgra_image_buffer(draft->supertile_width_read, draft->supertile_height_read);
            supertile_need_destroy = true;
        }

        float supertile_y = draft->source_level0_bounds.top + draft->supertile_height * (tile->tile_y << draft->source_base_level);
        i32 supertile_read_y = (i32)floorf(supertile_y) - 4;
        float supertile_offset_y = supertile_y - (float)supertile_read_y;

        float supertile_x = draft->source_level0_bounds.left + draft->supertile_width * (tile->tile_x << draft->source_base_level);
        i32 supertile_read_x = (i32)floorf(supertile_x) - 4;
        float supertile_offset_x = supertile_x - (float)supertile_read_x;

        if (image_read_region(draft->source_image, draft->source_plane, draft->source_base_level, supertile_read_x, supertile_read_y,
                              draft->supertile_width_read, draft->supertile_height_read, supertile.pixels,
                              supertile.pixel_format)) {
            rect2f box = {supertile_offset_x, supertile_offset_y, draft->supertile_width, draft->supertile_height};

            if (image_resample_lanczos3(&supertile, &resized_tile, box)) {
//              stbi_write_png("debug_resample_result.png", draft->tile_width, draft->tile_width, 4, resized_tile.pixels, resized_tile.width * resized_tile.channels);
                tile->buffer = resized_tile;
                shrink_tile_and_propagate_to_next_level(draft, tile);
                write_finished_bigtiff_tile(draft, tile, fp);
            } else {
                // TODO: handle error condition
            }
        } else {
            // TODO: handle error condition
        }

        if (supertile_need_destroy) {
            destroy_image_buffer(&supertile);
        }
        if (resized_tile_need_destroy) {
            destroy_image_buffer(&resized_tile);
        }
    } else {
        // no need to resize
        bool tile_need_destroy = false;
        image_buffer_t tile_buffer = create_bgra_image_buffer_using_arena(temp.arena, draft->tile_width, draft->tile_height);
        if (!tile_buffer.is_valid) {
            tile_buffer = create_bgra_image_buffer(draft->tile_width, draft->tile_height);
            tile_need_destroy = true;
        }

        i32 x = draft->source_level0_bounds.left + draft->tile_width * tile->tile_x;
        i32 y = draft->source_level0_bounds.top + draft->tile_height * tile->tile_y;
        if (image_read_region(draft->source_image, draft->source_plane, draft->source_base_level, x, y,
                          draft->tile_width, draft->tile_height, tile_buffer.pixels,
                          tile_buffer.pixel_format)) {
            tile->buffer = tile_buffer;
            shrink_tile_and_propagate_to_next_level(draft, tile);
            write_finished_bigtiff_tile(draft, tile, fp);
        } else {
            // TODO: handle error condition
        }

        if (tile_need_destroy) {
            destroy_image_buffer(&tile_buffer);
        }
    }


    release_temp_memory(&temp);
}

static void construct_tiles_recursive(image_draft_t* draft, image_draft_tile_t* tile, file_stream_t fp, bool propagate_to_parent) {
    ASSERT(tile->level >= 0);
    if (tile->level == 0) {
        ASSERT(propagate_to_parent);
        construct_base_tile_with_resampling(draft, tile, fp);
    } else {
        // find child tiles
        image_draft_level_t* child_level = draft->levels + tile->level - 1;
        image_draft_tile_t* topleft = child_level->tiles + tile->tile_y * 2 * child_level->width_in_tiles + tile->tile_x * 2;
        construct_tiles_recursive(draft, topleft, fp, true);
        i32 tile_x_right = tile->tile_x * 2 + 1;
        i32 tile_y_bottom = tile->tile_y * 2 + 1;
        if (tile_x_right < child_level->width_in_tiles) {
            image_draft_tile_t* topright = topleft + 1;
            construct_tiles_recursive(draft, topright, fp, true);
        }
        if (tile_y_bottom < child_level->height_in_tiles) {
            image_draft_tile_t* bottomleft = topleft + child_level->width_in_tiles;
            construct_tiles_recursive(draft, bottomleft, fp, true);
        }
        if (tile_x_right < child_level->width_in_tiles && tile_y_bottom < child_level->height_in_tiles) {
            image_draft_tile_t* bottomright = topleft + child_level->width_in_tiles + 1;
            construct_tiles_recursive(draft, bottomright, fp, true);
        }

        // Now all quadrants of the tile are filled -> write out to file
//        stbi_write_png("debug_resample_result.png", draft->tile_width, draft->tile_width, 4, tile->buffer.pixels, tile->buffer.width * tile->buffer.channels);
        if (propagate_to_parent) {
            shrink_tile_and_propagate_to_next_level(draft, tile);
        }
        write_finished_bigtiff_tile(draft, tile, fp);
        if (propagate_to_parent) {
            destroy_image_buffer(&tile->buffer);
        }
    }
}

static void construct_export_tile_task_func(i32 logical_thread_index, void* userdata) {
    construct_export_tile_task_t* task = (construct_export_tile_task_t*)userdata;
    atomic_increment(task->started_count);
    while (*task->started_count < *task->participants_goal) {
        platform_sleep(1);
    }

    for (;;) {
        i32 tile_index = atomic_increment(task->next_tile_index) - 1;
        if (tile_index >= task->frontier->tile_count) {
            break;
        }

        image_draft_tile_t* tile = task->frontier->tiles + tile_index;
        // The frontier root is retained after writing so the serial top pass can propagate it upward.
        construct_tiles_recursive(task->draft, tile, task->fp, false);
    }

    atomic_increment(task->finished_count);
}

// NOTE (2026-05-19): parallel image export currently seems to be functional and stable on Windows, macOS and Linux.
// Optionally disable using the toggle below:
bool disable_parallel_image_export = false;

static i32 image_draft_choose_parallel_frontier_level(image_draft_t* draft) {
    if (disable_parallel_image_export) {
        return -1;
    }

    i32 top_level_index = draft->level_count - 1;
    i32 active_worker_thread_count = thread_pool_get_active_worker_thread_count(&global_thread_pool);
    if (top_level_index < 1 || active_worker_thread_count <= 1) {
        return -1;
    }

    i32 desired_task_count = ATLEAST(4, active_worker_thread_count * 4);
    for (i32 level = top_level_index; level >= 1; --level) {
        image_draft_level_t* draft_level = draft->levels + level;
        if (draft_level->tile_count >= desired_task_count) {
            return level;
        }
    }

    // Level 1 is the lowest useful frontier: each worker owns small subtrees, while base tiles
    // can still be destroyed immediately instead of being retained for the upper pass.
    return 1;
}

static void image_draft_finish_upper_levels_from_frontier(image_draft_t* draft, i32 frontier_level, file_stream_t fp) {
    i32 top_level_index = draft->level_count - 1;
    for (i32 level = frontier_level; level < top_level_index; ++level) {
        image_draft_level_t* draft_level = draft->levels + level;
        image_draft_level_t* parent_level = draft->levels + level + 1;

        for (i32 i = 0; i < draft_level->tile_count; ++i) {
            image_draft_tile_t* tile = draft_level->tiles + i;
            shrink_tile_and_propagate_to_next_level(draft, tile);
            destroy_image_buffer(&tile->buffer);
        }

        for (i32 i = 0; i < parent_level->tile_count; ++i) {
            image_draft_tile_t* tile = parent_level->tiles + i;
            write_finished_bigtiff_tile(draft, tile, fp);
            if (level + 1 == top_level_index) {
                destroy_image_buffer(&tile->buffer);
            }
        }
    }

    if (frontier_level == top_level_index) {
        image_draft_level_t* top_level = draft->levels + top_level_index;
        for (i32 i = 0; i < top_level->tile_count; ++i) {
            destroy_image_buffer(&top_level->tiles[i].buffer);
        }
    }
}

static void construct_tiles_parallel_from_frontier(image_draft_t* draft, i32 frontier_level, file_stream_t fp) {
    image_draft_level_t* frontier = draft->levels + frontier_level;
    volatile i32 next_tile_index = 0;
    volatile i32 started_count = 0;
    volatile i32 participants_goal = ATLEAST(1, ATMOST(frontier->tile_count, thread_pool_get_active_worker_thread_count(&global_thread_pool)));
    volatile i32 finished_count = 0;

    construct_export_tile_task_t task = {
            draft, frontier, fp,
            &next_tile_index, &started_count, &participants_goal, &finished_count,
    };

    i32 worker_task_count = participants_goal - 1;
    for (i32 i = 0; i < worker_task_count; ++i) {
        if (!thread_pool_submit_task(&global_thread_pool, construct_export_tile_task_func, &task, sizeof(task))) {
            atomic_decrement(&participants_goal);
        }
    }

    construct_export_tile_task_func(0, &task);

    while (finished_count < participants_goal) {
        thread_pool_do_work(&global_thread_pool);
    }
}

static void image_draft_prepare_bigtiff_ifds_and_tags(image_draft_t* draft) {
    // We will prepare all the tags, and push them into a temporary buffer, to be written to file later.
    // For non-inlined tags, the 'offset' field gets a placeholder offset because we don't know yet
    // where the tag data will be located in the file. For such tags we will:
    // - Push the data into a separate buffer, and remember the relative offset within that buffer
    // - Create a 'fixup', so that we can later substitute the offset once we know the base offset
    //   where we will store the separate data buffer in the output file.

    // temporary buffer for only the TIFF header + IFD tags
    draft->tag_buffer = memrw_create(KILOBYTES(64));
    // temporary buffer for all data >8 bytes not fitting in the raw TIFF tags (leaving out the pixel data)
    draft->small_data_buffer = memrw_create(MEGABYTES(1));
    // temporary buffer tracking the offsets that we need to fix after writing all the IFDs
    draft->fixups_buffer = memrw_create(1024);

    // Write the TIFF header (except the offset to the first IFD, which we will push when iterating over the IFDs)
    memset(&draft->header, 0, sizeof(draft->header));
    draft->header.byte_order_indication = 0x4949; // little-endian
    draft->header.filetype = 0x002B; // BigTIFF
    draft->header.bigtiff.offset_size = 0x0008;
    draft->header.bigtiff.always_zero = 0;
    memrw_push_back(&draft->tag_buffer, &draft->header, 8);

    i32 export_ifd_count = 0;
    i32 export_max_level = 0;

    // Tags that are the same for all image levels
    raw_bigtiff_tag_t tag_new_subfile_type = {TIFF_TAG_NEW_SUBFILE_TYPE, TIFF_UINT32, 1, .offset = TIFF_FILETYPE_REDUCEDIMAGE};
    u16 bits_per_sample[4] = {8, 8, 8, 0};
    raw_bigtiff_tag_t tag_bits_per_sample = {TIFF_TAG_BITS_PER_SAMPLE, TIFF_UINT16, 3, .offset = *(u64*)bits_per_sample};
    raw_bigtiff_tag_t tag_compression = {TIFF_TAG_COMPRESSION, TIFF_UINT16, 1, .offset = TIFF_COMPRESSION_JPEG};
    raw_bigtiff_tag_t tag_photometric_interpretation = {TIFF_TAG_PHOTOMETRIC_INTERPRETATION, TIFF_UINT16, 1, .offset = draft->desired_photometric_interpretation};
    raw_bigtiff_tag_t tag_orientation = {TIFF_TAG_ORIENTATION, TIFF_UINT16, 1, .offset = TIFF_ORIENTATION_TOPLEFT};
    raw_bigtiff_tag_t tag_samples_per_pixel = {TIFF_TAG_SAMPLES_PER_PIXEL, TIFF_UINT16, 1, .offset = 3};
    raw_bigtiff_tag_t tag_tile_length = {TIFF_TAG_TILE_LENGTH, TIFF_UINT16, 1, .offset = draft->tile_height};
    raw_bigtiff_tag_t tag_tile_width = {TIFF_TAG_TILE_WIDTH, TIFF_UINT16, 1, .offset = draft->tile_width};
    raw_bigtiff_tag_t tag_resolution_unit = {TIFF_TAG_RESOLUTION_UNIT, TIFF_UINT16, 1, .data_u16 = 3 /*RESUNIT_CENTIMETER*/};
    // NOTE: chroma subsampling is used for YCbCr-encoded images, but not for RGB
    u16 chroma_subsampling[4] = {2, 2, 0, 0};
    raw_bigtiff_tag_t tag_chroma_subsampling = {TIFF_TAG_YCBCRSUBSAMPLING, TIFF_UINT16, 2, .offset = *(u64*)(chroma_subsampling)};

    for (i32 level = 0; level < draft->level_count; ++level) {
        image_draft_level_t* draft_level = draft->levels + level;


        export_max_level = level;
        ++export_ifd_count;

        // Offset to the beginning of the next IFD (= 8 bytes directly after the current offset)
        u64 next_ifd_offset = draft->tag_buffer.used_size + sizeof(u64);
        memrw_push_back(&draft->tag_buffer, &next_ifd_offset, sizeof(u64));

        u64 tag_count_for_ifd = 0;
        u64 tag_count_for_ifd_offset = memrw_push_back(&draft->tag_buffer, &tag_count_for_ifd, sizeof(u64));


        // Include the NewSubfileType tag in every IFD except the first one
        if (level > 0) {
            memrw_push_back(&draft->tag_buffer, &tag_new_subfile_type, sizeof(raw_bigtiff_tag_t)); // 254
            ++tag_count_for_ifd;
        }


        raw_bigtiff_tag_t tag_image_width = {TIFF_TAG_IMAGE_WIDTH , TIFF_UINT32, 1, .offset = draft_level->width_in_pixels};
        raw_bigtiff_tag_t tag_image_length = {TIFF_TAG_IMAGE_LENGTH , TIFF_UINT32, 1, .offset = draft_level->height_in_pixels};

        // NOTE: The TIFF specification requires the tags to be in strict ascending order in the IFD.
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_image_width); ++tag_count_for_ifd; // 256
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_image_length); ++tag_count_for_ifd; // 257
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_bits_per_sample); ++tag_count_for_ifd; // 258
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_compression); ++tag_count_for_ifd; // 259
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_photometric_interpretation); ++tag_count_for_ifd; // 262

        // NOTE: For files cropped from Philips TIFF, ASAP will not correctly read the XResolution and YResolution tags
        // if the ImageDescription exists (or, if the ImageDescription still contains a reference to the Philips metadata?)
        // So for now, leave out the ImageDescription tag; it isn't really needed and only confuses some software.
        /*
        // Image description will be copied verbatim from the source
        if (source_ifd->image_description != NULL && source_ifd->image_description_length > 0) {
            add_large_bigtiff_tag(&tag_buffer, &small_data_buffer, &fixups_buffer, TIFF_TAG_IMAGE_DESCRIPTION,
                                  TIFF_ASCII, source_ifd->image_description_length, source_ifd->image_description);
            ++tag_count_for_ifd;
        }
        */

        // unused tag: strip offsets

        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_orientation); ++tag_count_for_ifd; // 274
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_samples_per_pixel); ++tag_count_for_ifd; // 277

        // unused tag: rows per strip
        // unused tag: strip byte counts

        if (draft->is_mpp_known) {
            raw_bigtiff_tag_t tag_x_resolution = {TIFF_TAG_X_RESOLUTION, TIFF_RATIONAL, 1, 0};
            float downsample_factor = (float)(1 << level);
            tiff_rational_t resolution_x = float_to_tiff_rational((1.0f / draft->mpp.x) * downsample_factor * 10000.0f);
            tag_x_resolution.offset = *(u64*)(&resolution_x);
            memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_x_resolution); // 282
            ++tag_count_for_ifd;

            raw_bigtiff_tag_t tag_y_resolution = {TIFF_TAG_Y_RESOLUTION, TIFF_RATIONAL, 1, 0};
            tiff_rational_t resolution_y = float_to_tiff_rational((1.0f / draft->mpp.y) * downsample_factor * 10000.0f);
            tag_y_resolution.offset = *(u64*)(&resolution_y);
            memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_y_resolution); // 283
            ++tag_count_for_ifd;

            memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_resolution_unit); ++tag_count_for_ifd; // 296
        }

#if 0
        // Copy Software tag verbatim from the source image.
        if (source_ifd->software != NULL && source_ifd->software_length > 0) {
            add_large_bigtiff_tag(&tag_buffer, &small_data_buffer, &fixups_buffer, TIFF_TAG_SOFTWARE,
                                  TIFF_ASCII, source_ifd->software_length, source_ifd->software); // 305
            ++tag_count_for_ifd;
        }
#endif

        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_tile_width); ++tag_count_for_ifd; // 322
        memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_tile_length); ++tag_count_for_ifd; // 323

        u64 tag_tile_offsets_write_offset = add_large_bigtiff_tag(&draft->tag_buffer, &draft->small_data_buffer, &draft->fixups_buffer,
                                                                  TIFF_TAG_TILE_OFFSETS, TIFF_UINT64, draft_level->tile_count, NULL); // 324
        draft_level->offset_of_tile_offsets = tag_tile_offsets_write_offset + offsetof(raw_bigtiff_tag_t, offset);
        if (draft_level->tile_count == 1) {
            draft_level->are_tile_offsets_inlined_in_tag = true; // no indirection (inlined, no offset in data buffer)
        }
        ++tag_count_for_ifd;

        u64 tag_tile_bytecounts_write_offset = add_large_bigtiff_tag(&draft->tag_buffer, &draft->small_data_buffer, &draft->fixups_buffer,
                                                                     TIFF_TAG_TILE_BYTE_COUNTS, TIFF_UINT64, draft_level->tile_count, NULL); // 325
        draft_level->offset_of_tile_bytecounts = tag_tile_bytecounts_write_offset + offsetof(raw_bigtiff_tag_t, offset);


        ++tag_count_for_ifd;

        // unused tag: SMinSampleValue
        // unused tag: SMaxSampleValue

        u8* tables_buffer = NULL;
        u64 tables_size = 0;
        jpeg_encode_tile(NULL, draft->tile_width, draft->tile_height, draft->quality, &tables_buffer, &tables_size, NULL,
                         NULL, 0);
        add_large_bigtiff_tag(&draft->tag_buffer, &draft->small_data_buffer, &draft->fixups_buffer,
                              TIFF_TAG_JPEG_TABLES, TIFF_UNDEFINED, tables_size, tables_buffer); // 347
        ++tag_count_for_ifd;
        if (tables_buffer) libc_free(tables_buffer);

        if (draft->desired_photometric_interpretation == TIFF_PHOTOMETRIC_YCBCR) {
            memrw_push_bigtiff_tag(&draft->tag_buffer, &tag_chroma_subsampling); // 530
            ++tag_count_for_ifd;
        }

        // Update the tag count, which was written incorrectly as a placeholder at the beginning of the IFD
        *(u64*)(draft->tag_buffer.data + tag_count_for_ifd_offset) = tag_count_for_ifd;

        /*if (need_reuse_tiles && level == 0) {
            u32 tile_offset_x = level0_pixel_bounds.left / tile_width;
            u32 tile_offset_y = level0_pixel_bounds.top / tile_height;
        }*/


    }

    u64 next_ifd_offset_terminator = 0;
    memrw_push_back(&draft->tag_buffer, &next_ifd_offset_terminator, sizeof(u64));

    // TODO: macro/label images

    // Adjust the offsets in the TIFF tags, so that they are counted from the beginning of the file.
    u64 data_buffer_base_offset = draft->tag_buffer.used_size;
    offset_fixup_t* fixups = (offset_fixup_t*)draft->fixups_buffer.data;
    u64 fixup_count = draft->fixups_buffer.used_count;
    for (i32 i = 0; i < fixup_count; ++i) {
        offset_fixup_t* fixup = fixups + i;
        u64 fixed_offset = fixup->offset_from_unknown_base + data_buffer_base_offset;
        *(u64*)(draft->tag_buffer.data + fixup->offset_to_fix) = fixed_offset;
    }

    // Resolve indirection to get the actual locations of the tile offsets and byte counts in the TIFF file.
    // (At this point these sections still contain only placeholder zeroes. We need to rewrite these later.)
    for (i32 level = 0; level <= export_max_level; ++level) {
        image_draft_level_t* draft_level = draft->levels + level;
        if (!draft_level->are_tile_offsets_inlined_in_tag) {
            u64 tile_offsets_actual_offset_in_file    = *(u64*)(draft->tag_buffer.data + draft_level->offset_of_tile_offsets);
            u64 tile_bytecounts_actual_offset_in_file = *(u64*)(draft->tag_buffer.data + draft_level->offset_of_tile_bytecounts);
            draft_level->offset_of_tile_offsets    = tile_offsets_actual_offset_in_file;
            draft_level->offset_of_tile_bytecounts = tile_bytecounts_actual_offset_in_file;
        }
    }
}

static void image_draft_write_bigtiff_ifds_and_small_data(image_draft_t* draft, file_stream_t fp) {
    fwrite(draft->tag_buffer.data, draft->tag_buffer.used_size, 1, fp);
    fwrite(draft->small_data_buffer.data, draft->small_data_buffer.used_size, 1,fp);
    draft->image_data_base_offset = draft->tag_buffer.used_size + draft->small_data_buffer.used_size;
}

void image_draft_destroy(image_draft_t* draft) {
    for (i32 i = 0; i < draft->level_count; ++i) {
        image_draft_level_t* draft_level = draft->levels + i;
        if (draft_level->tiles) {
            free(draft_level->tiles);
        }
        if (draft_level->tile_offsets) {
            free(draft_level->tile_offsets);
        }
        if (draft_level->tile_bytecounts) {
            free(draft_level->tile_bytecounts);
        }
    }
    memrw_destroy(&draft->tag_buffer);
    memrw_destroy(&draft->small_data_buffer);
    memrw_destroy(&draft->fixups_buffer);
}


bool export_cropped_bigtiff_with_resample(app_state_t* app_state, image_t* image, i32 plane, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                            u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags, bool need_resize, v2f target_mpp) {

    switch(desired_photometric_interpretation) {
        case TIFF_PHOTOMETRIC_YCBCR: break;
        case TIFF_PHOTOMETRIC_RGB: break;
        default: {
            console_print_error("Error exporting BigTIFF: unsupported photometric interpretation (%d)\n", desired_photometric_interpretation);
        } return false;
    }

    // We need to downscale the image by some factor.
    if (need_resize && !image->is_mpp_known) {
        console_print_error("Error exporting BigTIFF: source image resolution is unknown.\n");
        return false;
    }

    if (!(target_mpp.x > 0.0f && target_mpp.y > 0.0f)) {
        console_print_verbose("Error exporting BigTIFF: invalid target resolution.\n");
        return false;
    }

    float downsample_factor_x = 1.0f;
    float downsample_factor_y = 1.0f;
    i32 source_base_level = 0;
    if (need_resize) {
        // Only resize if the target resolution is actually different, otherwise don't bother.
        downsample_factor_x = image->mpp_x / target_mpp.x;
        downsample_factor_y = image->mpp_y / target_mpp.y;
        if (fabs(1.0f - downsample_factor_x) + fabs(1.0f - downsample_factor_y) < 0.001f) {
            need_resize = false;
        }
        float largest_downsample_factor = MAX(downsample_factor_x, downsample_factor_y);
        ASSERT(largest_downsample_factor > 0.0f);
        if (largest_downsample_factor < 0.25f) {
            float source_mpp_x = image->mpp_x;
            float source_mpp_y = image->mpp_y;
            do {
                source_mpp_x *= 2.0f;
                source_mpp_y *= 2.0f;
                ++source_base_level;
                downsample_factor_x = source_mpp_x / target_mpp.x;
                downsample_factor_y = source_mpp_y / target_mpp.y;
                largest_downsample_factor = MAX(downsample_factor_x, downsample_factor_y);
            } while (largest_downsample_factor < 0.25f);
        }
    }

    i32 target_level0_width_in_pixels = (level0_bounds.right - level0_bounds.left) >> source_base_level;
    i32 target_level0_height_in_pixels = (level0_bounds.bottom - level0_bounds.top) >> source_base_level;
    if (need_resize) {
        target_level0_width_in_pixels = roundf((float)target_level0_width_in_pixels * downsample_factor_x);
        target_level0_height_in_pixels = roundf((float)target_level0_height_in_pixels * downsample_factor_y);
    }

    image_draft_t draft = {};
    draft.level_count = 9; // default value, maybe override later if fewer are needed
    draft.base_width = target_level0_width_in_pixels;
    draft.base_height = target_level0_height_in_pixels;
    draft.tile_width = export_tile_width;
    draft.tile_height = export_tile_width;
    draft.is_mpp_known = true;
    draft.is_background_black = image->is_background_black;
    draft.need_resize = need_resize;
    draft.mpp = target_mpp;
    draft.desired_photometric_interpretation = desired_photometric_interpretation;
    draft.quality = quality;

    draft.source_image = image;
    draft.source_level0_bounds = level0_bounds;
    draft.source_plane = plane;
    draft.source_base_level = source_base_level;
    draft.base_downsample_factor_x = downsample_factor_x;
    draft.base_downsample_factor_y = downsample_factor_y;
    draft.supertile_width = (float)draft.tile_width / draft.base_downsample_factor_x;
    draft.supertile_height = (float)draft.tile_height / draft.base_downsample_factor_y;
    draft.supertile_width_read = ((i32)ceilf(draft.supertile_width) + 8);
    draft.supertile_height_read = ((i32)ceilf(draft.supertile_height) + 8);
    platform_mutex_init(&draft.write_lock);

    for (i32 i = 0; i < 9; ++i) {
        image_draft_level_t* draft_level = draft.levels + i;
        draft_level->level = i;

        // Calculate dimensions for the current downsampling i
        draft_level->width_in_pixels = draft.base_width >> i;
        draft_level->height_in_pixels = draft.base_height >> i;

        draft_level->width_in_tiles = (draft_level->width_in_pixels + (draft.tile_width - 1)) / draft.tile_width;
        draft_level->height_in_tiles = (draft_level->height_in_pixels + (draft.tile_height - 1)) / draft.tile_height;
        draft_level->tile_count = draft_level->width_in_tiles * draft_level->height_in_tiles;
        ASSERT(draft_level->tile_count > 0);
        draft.total_tiles_to_export += draft_level->tile_count;
        draft_level->tiles = calloc(1, draft_level->tile_count * sizeof(image_draft_tile_t));
        draft_level->tile_offsets = calloc(1, draft_level->tile_count * sizeof(u64));
        draft_level->tile_bytecounts = calloc(1, draft_level->tile_count * sizeof(u64));

        // Initialize tiles with self-referencing of the level and tile position (tile_x, tile_y)
        for (i32 tile_y = 0; tile_y < draft_level->height_in_tiles; ++tile_y) {
            image_draft_tile_t* tile_row = draft_level->tiles + tile_y * draft_level->width_in_tiles;
            for (i32 tile_x = 0; tile_x < draft_level->width_in_tiles; ++tile_x) {
                image_draft_tile_t* tile = tile_row + tile_x;
                tile->tile_x = tile_x;
                tile->tile_y = tile_y;
                tile->tile_index = tile_y * draft_level->width_in_tiles + tile_x;
                tile->level = i;
            }
        }

        // Don't bother adding more levels if everything already fits within a single tile
        if (draft_level->tile_count <= 1) {
            draft.level_count = i;
            break;
        }
    }

    if (draft.level_count <= 0) {
        fatal_error("invalid level count");
        return false;
    }

    // To construct the pyramid, we'll construct the base level first, then afterwards propagate its contents
    // up to higher levels

    // Problem: we need to somehow reconstruct the pyramid without using too much RAM.
    // Can we do it with some clever caching techniques while also preserving speed?
    // Idea: recurse from topmost layer touching each quadrant dependency in turn

    // To construct the base layer:
    // for each base layer tile, prepare a 'supertile' that can be resized into the final base tile
    // The first step will be Lanczos resampling, after which there will be several box shrink steps


    // Prepare all the IFDs and TIFF tags to be written out to file later
    image_draft_prepare_bigtiff_ifds_and_tags(&draft);

    FILE* fp = fopen64(filename, "wb");
    bool success = false;

    if (fp) {
        // Write out the IFDs and TIFF tags to file
        image_draft_write_bigtiff_ifds_and_small_data(&draft, fp);
        draft.current_image_data_write_offset = draft.image_data_base_offset;

        // TODO: progress bar progress managed on the main thread?
        global_tiff_export_progress = 0.05f;
        float progress_left = 0.99f - global_tiff_export_progress;
        draft.progress_per_exported_tile = progress_left / (float)(ATLEAST(1, draft.total_tiles_to_export));

        console_print_verbose("Starting TIFF export, total tiles to export = %d\n", draft.total_tiles_to_export);

        // Construct the images for each tile of the pyramid. The recursive dependency tree is split
        // at a frontier level: each worker owns one complete subtree below that level, and the small
        // remaining top section is finished serially from the retained frontier tiles.
        i32 parallel_frontier_level = image_draft_choose_parallel_frontier_level(&draft);
        if (parallel_frontier_level >= 1) {
            construct_tiles_parallel_from_frontier(&draft, parallel_frontier_level, fp);
            image_draft_finish_upper_levels_from_frontier(&draft, parallel_frontier_level, fp);
        } else {
            image_draft_level_t* top_level = draft.levels + draft.level_count - 1;
            for (i32 tile_y = 0; tile_y < top_level->height_in_tiles; ++tile_y) {
                for (i32 tile_x = 0; tile_x < top_level->width_in_tiles; ++tile_x) {
                    image_draft_tile_t* tile = top_level->tiles + tile_y * top_level->width_in_tiles + tile_x;
                    construct_tiles_recursive(&draft, tile, fp, true);
                }
            }
        }

        // Rewrite the tile offsets and tile bytecounts
        for (i32 i = 0; i < draft.level_count; ++i) {
            image_draft_level_t* draft_level = draft.levels + i;

            fseeko64(fp, draft_level->offset_of_tile_offsets, SEEK_SET);
            fwrite(draft_level->tile_offsets, sizeof(u64), draft_level->tile_count, fp);

            fseeko64(fp, draft_level->offset_of_tile_bytecounts, SEEK_SET);
            fwrite(draft_level->tile_bytecounts, sizeof(u64), draft_level->tile_count, fp);
        }


        fclose(fp);

        success = true;
        console_print("Exported region to '%s'\n", filename);
    }

    platform_mutex_destroy(&draft.write_lock);
    image_draft_destroy(&draft);

    if (export_flags & EXPORT_FLAGS_ALSO_EXPORT_ANNOTATIONS) {
        bool push_coordinates_inward = export_flags & EXPORT_FLAGS_PUSH_ANNOTATION_COORDINATES_INWARD;
        annotation_set_t derived_set = create_offsetted_annotation_set_for_area(&app_state->scene.annotation_set, world_bounds, push_coordinates_inward);

        size_t filename_len = strlen(filename);
        char* xml_filename = (char*)alloca(filename_len + 4);
        memcpy(xml_filename, filename, filename_len + 1);
        replace_file_extension(xml_filename, filename_len + 4, "xml");
        save_asap_xml_annotations(&derived_set, xml_filename);

        destroy_annotation_set(&derived_set);
    }


    return success;
}


void export_cropped_bigtiff_with_resample_func(i32 logical_thread_index, void* userdata) {
    export_region_task_t* task = (export_region_task_t*) userdata;
    bool success = export_cropped_bigtiff_with_resample(task->app_state, task->image, task->plane, task->world_bounds, task->level0_bounds,
                                          task->filename, task->export_tile_width,
                                          task->desired_photometric_interpretation, task->quality, task->export_flags,
                                          task->need_resize, task->target_mpp);
	global_tiff_export_progress = 1.0f;
	task->app_state->is_export_in_progress = false;

	atomic_decrement(&task->image->refcount);
}

void begin_export_cropped_bigtiff_with_resample(app_state_t* app_state, image_t* image, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                                                u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags, bool need_resize, v2f target_mpp) {

    export_region_task_t task = {0};
    task.app_state = app_state;
    task.image = image;
	task.plane = image->selected_plane;
    task.world_bounds = world_bounds;
    task.level0_bounds = level0_bounds;
    task.filename = filename;
    task.export_tile_width = export_tile_width;
    task.desired_photometric_interpretation = desired_photometric_interpretation;
    task.quality = quality;
    task.export_flags = export_flags;
    task.need_resize = need_resize;
    task.target_mpp = target_mpp;

	global_tiff_export_progress = 0.0f;
	global_tiff_export_progress_console_dots_written = 0;
	app_state->is_export_in_progress = true;

	atomic_increment(&image->refcount);
	if (!thread_pool_submit_task(&global_thread_pool, export_cropped_bigtiff_with_resample_func, &task, sizeof(task))) {
		atomic_decrement(&image->refcount);
		app_state->is_export_in_progress = false;
	};
}
