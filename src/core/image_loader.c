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

#include "image_loader.h"

#include "dicom_wsi.h"
#include "listing.h"
#include "stringutils.h"

#define STBI_ASSERT(x) ASSERT(x)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

void load_openslide_wsi(wsi_t* wsi, const char* filename, bool openslide_loading_done, thread_pool_t* thread_pool) {
	if (!openslide_loading_done && thread_pool) {
#if DO_DEBUG
		console_print("Waiting for OpenSlide to finish loading...\n");
#endif
		thread_pool_wait_for_completion(thread_pool);
	}

	// TODO: check if necessary anymore?
	unload_openslide_wsi(wsi);

	wsi->osr = openslide.open(filename);
	if (wsi->osr) {
		const char* error_string = openslide.get_error(wsi->osr);
		if (error_string != NULL) {
			console_print_error("OpenSlide error: %s\n", error_string);
			unload_openslide_wsi(wsi);
			return;
		}

		console_print_verbose("OpenSlide: opened '%s'\n", filename);

		wsi->level_count = openslide.get_level_count(wsi->osr);
		if (wsi->level_count == -1) {
			error_string = openslide.get_error(wsi->osr);
			console_print_error("OpenSlide error: %s\n", error_string);
			unload_openslide_wsi(wsi);
			return;
		}
		console_print_verbose("OpenSlide: WSI has %d levels\n", wsi->level_count);
		if (wsi->level_count > WSI_MAX_LEVELS) {
			fatal_error();
		}

		openslide.get_level0_dimensions(wsi->osr, &wsi->width, &wsi->height);
		ASSERT(wsi->width > 0);
		ASSERT(wsi->height > 0);

		wsi->tile_width = WSI_TILE_DIM;
		wsi->tile_height = WSI_TILE_DIM;

		const char* const* wsi_properties = openslide.get_property_names(wsi->osr);
		if (wsi_properties) {
			i32 property_index = 0;
			const char* property = wsi_properties[0];
			for (; property != NULL; property = wsi_properties[++property_index]) {
				const char* property_value = openslide.get_property_value(wsi->osr, property);
				console_print_verbose("%s = %s\n", property, property_value);
			}
		}

		wsi->mpp_x = 1.0f; // microns per pixel (default)
		wsi->mpp_y = 1.0f; // microns per pixel (default)
		wsi->is_mpp_known = false;
		const char* mpp_x_string = openslide.get_property_value(wsi->osr, "openslide.mpp-x");
		const char* mpp_y_string = openslide.get_property_value(wsi->osr, "openslide.mpp-y");
		if (mpp_x_string) {
			float mpp = atof(mpp_x_string);
			if (mpp > 0.0f) {
				wsi->mpp_x = mpp;
				wsi->is_mpp_known = true;
			}
		}
		if (mpp_y_string) {
			float mpp = atof(mpp_y_string);
			if (mpp > 0.0f) {
				wsi->mpp_y = mpp;
				wsi->is_mpp_known = true;
			}
		}

		for (i32 i = 0; i < wsi->level_count; ++i) {
			wsi_level_t* level = wsi->levels + i;

			openslide.get_level_dimensions(wsi->osr, i, &level->width, &level->height);
			ASSERT(level->width > 0);
			ASSERT(level->height > 0);
			i64 partial_block_x = level->width % WSI_TILE_DIM;
			i64 partial_block_y = level->height % WSI_TILE_DIM;
			level->width_in_tiles = (i32)(level->width / WSI_TILE_DIM) + (partial_block_x != 0);
			level->height_in_tiles = (i32)(level->height / WSI_TILE_DIM) + (partial_block_y != 0);
			level->tile_width = WSI_TILE_DIM;
			level->tile_height = WSI_TILE_DIM;

			float raw_downsample_factor = openslide.get_level_downsample(wsi->osr, i);
			float raw_downsample_level = log2f(raw_downsample_factor);
			i32 downsample_level = (i32)roundf(raw_downsample_level);

			level->downsample_level = downsample_level;
			level->downsample_factor = exp2f(level->downsample_level);
			wsi->max_downsample_level = MAX(level->downsample_level, wsi->max_downsample_level);
			level->um_per_pixel_x = level->downsample_factor * wsi->mpp_x;
			level->um_per_pixel_y = level->downsample_factor * wsi->mpp_y;
			level->x_tile_side_in_um = level->um_per_pixel_x * (float)WSI_TILE_DIM;
			level->y_tile_side_in_um = level->um_per_pixel_y * (float)WSI_TILE_DIM;
			level->tile_count = level->width_in_tiles * level->height_in_tiles;
		}

		const char* barcode = openslide.get_property_value(wsi->osr, "philips.PIM_DP_UFS_BARCODE");
		if (barcode) {
			wsi->barcode = barcode;
		}

		const char* const* wsi_associated_image_names = openslide.get_associated_image_names(wsi->osr);
		if (wsi_associated_image_names) {
			i32 name_index = 0;
			const char* name = wsi_associated_image_names[0];
			for (; name != NULL; name = wsi_associated_image_names[++name_index]) {
				i64 w = 0;
				i64 h = 0;
				openslide.get_associated_image_dimensions(wsi->osr, name, &w, &h);
				console_print_verbose("%s : w=%lld h=%lld\n", name, w, h);
			}
		}
	}
}

static viewer_file_type_enum viewer_determine_file_type(file_info_t* file) {
	if (file->is_regular_file) {
		if (strcasecmp(file->filename_in_directory, "Slidedat.ini") == 0) {
			return VIEWER_FILE_TYPE_MRXS;
		} else if (strlen(file->ext) == 0) {
			if (is_file_a_dicom_file(file->header, MIN(file->filesize, sizeof(file->header)))) {
				return VIEWER_FILE_TYPE_DICOM;
			} else {
				return VIEWER_FILE_TYPE_UNKNOWN;
			}
		}
		if (strcasecmp(file->ext, "tiff") == 0 ||
		    strcasecmp(file->ext, "tif") == 0 ||
		    strcasecmp(file->ext, "bif") == 0 ||
		    strcasecmp(file->ext, "ptif") == 0) {
			return VIEWER_FILE_TYPE_TIFF;
		} else if (strcasecmp(file->ext, "ndpi") == 0) {
			return VIEWER_FILE_TYPE_NDPI;
		} else if (strcasecmp(file->ext, "png") == 0 ||
		           strcasecmp(file->ext, "jpg") == 0 ||
		           strcasecmp(file->ext, "jpeg") == 0 ||
		           strcasecmp(file->ext, "bmp") == 0 ||
		           strcasecmp(file->ext, "ppm") == 0) {
			return VIEWER_FILE_TYPE_SIMPLE_IMAGE; // i.e. stb_image compatible
		} else if (strcasecmp(file->ext, "xml") == 0) {
			return VIEWER_FILE_TYPE_XML;
		} else if (strcasecmp(file->ext, "json") == 0 || strcasecmp(file->ext, "geojson") == 0) {
			return VIEWER_FILE_TYPE_JSON;
		} else if (strcasecmp(file->ext, "dcm") == 0) {
			return VIEWER_FILE_TYPE_DICOM;
		} else if (strcasecmp(file->ext, "isyntax") == 0 || strcasecmp(file->ext, "i2syntax") == 0) {
			return VIEWER_FILE_TYPE_ISYNTAX;
		} else if (strcasecmp(file->ext, "mrxs") == 0) {
			return VIEWER_FILE_TYPE_MRXS;
		} else {
			if (is_file_a_dicom_file(file->header, MIN(file->filesize, sizeof(file->header)))) {
				return VIEWER_FILE_TYPE_DICOM;
			} else {
				// TODO: this is a total guess, maybe flesh out more?
				return VIEWER_FILE_TYPE_OPENSLIDE_COMPATIBLE;
			}
		}
	}
	return VIEWER_FILE_TYPE_UNKNOWN;
}

file_info_t viewer_get_file_info(const char* filename) {
	file_info_t file = {0};
	size_t filename_len = strlen(filename);
	if (filename_len >= sizeof(file.full_filename)) {
		console_print_error("viewer_get_file_info(): filename too long (length=%u): '%s'\n", filename_len, filename);
		return file;
	}
	memcpy(file.full_filename, filename, filename_len);
	const char* ext = get_file_extension(filename);
	copy_cstring(file.ext, ext, sizeof(file.ext));
	const char* filename_in_directory = one_past_last_slash(file.full_filename, filename_len);
	copy_cstring(file.filename_in_directory, filename_in_directory, sizeof(file.filename_in_directory));
	i64 prefix_len = filename_in_directory - file.full_filename;
	ASSERT(prefix_len >= 0 && prefix_len < filename_len);
	if (prefix_len > 0 && prefix_len < filename_len) {
		memcpy(file.filename_prefix, filename, prefix_len);
	}

	struct stat st;
	if (platform_stat(filename, &st) == 0) {
		file.is_valid = true;
		file.is_directory = S_ISDIR(st.st_mode);
		file.is_regular_file = S_ISREG(st.st_mode);
		if (file.is_regular_file) {
			file.filesize = st.st_size;
			file_stream_t fp = file_stream_open_for_reading(filename);
			if (fp) {
				size_t bytes_to_read = MIN(file.filesize, sizeof(file.header));
				size_t bytes_read = file_stream_read(file.header, bytes_to_read, fp);
				if (bytes_read == bytes_to_read) {
					file.type = viewer_determine_file_type(&file);
					switch(file.type) {
						default: break;
						case VIEWER_FILE_TYPE_TIFF:
						case VIEWER_FILE_TYPE_NDPI:
						case VIEWER_FILE_TYPE_MRXS:
						case VIEWER_FILE_TYPE_OPENSLIDE_COMPATIBLE:
							file.is_openslide_compatible = true;
							// fallthrough
						case VIEWER_FILE_TYPE_SIMPLE_IMAGE:
						case VIEWER_FILE_TYPE_DICOM:
						case VIEWER_FILE_TYPE_ISYNTAX:
							file.is_image = true;
							break;
					}
				} else {
					console_print_error("viewer_get_file_info(): read header failed (tried to read %d bytes, but read %d)\n", bytes_to_read, bytes_read);
					file.is_valid = false;
				}
				file_stream_close(fp);
			} else {
				file.is_valid = false;
			}
		}
	}
	return file;
}

void viewer_directory_info_destroy(directory_info_t* info) {
	arrfree(info->dicom_files);
	arrfree(info->nondicom_files);
	if (info->directories) {
		for (i32 i = 0; i < arrlen(info->directories); ++i) {
			viewer_directory_info_destroy(info->directories + i);
		}
	}
	arrfree(info->directories);
	info->is_valid = false;
}

enum {
	// PS3.10 section 8.2 permits at most eight File ID components, including the file itself.
	DICOM_DIRECTORY_RECURSION_LIMIT = 7,
};

static void viewer_join_path(char* dest, size_t dest_size, const char* directory, const char* name) {
	size_t directory_len = strlen(directory);
	if (directory_len > 0 && (directory[directory_len - 1] == '/' || directory[directory_len - 1] == '\\')) {
		snprintf(dest, dest_size, "%s%s", directory, name);
	} else {
		snprintf(dest, dest_size, "%s" PATH_SEP "%s", directory, name);
	}
}

static directory_info_t viewer_get_directory_info_internal(const char* path,
		i32 dicom_recursion_levels_remaining, bool dicom_only) {
	directory_info_t directory = {0};
	copy_cstring(directory.path, path, sizeof(directory.path));
	directory_listing_t* listing = create_directory_listing_and_find_first_file(path, NULL);
	if (listing) {
		directory.is_valid = true;
		do {
			char* current_filename = get_current_filename_from_directory_listing(listing);
			char full_filename[512];
			viewer_join_path(full_filename, sizeof(full_filename), path, current_filename);
			file_info_t file = viewer_get_file_info(full_filename);
			if (file.is_valid) {
				if (file.is_directory && dicom_recursion_levels_remaining > 0) {
					directory_info_t subdir_info = viewer_get_directory_info_internal(
						full_filename, dicom_recursion_levels_remaining - 1, true);
					if (subdir_info.contains_dicom_files) {
						directory.contains_dicom_files = true;
						arrput(directory.directories, subdir_info);
					} else {
						viewer_directory_info_destroy(&subdir_info);
					}
				} else if (file.is_regular_file) {
					if (file.type == VIEWER_FILE_TYPE_DICOM) {
						directory.contains_dicom_files = true;
						arrput(directory.dicom_files, file);
					} else if (!dicom_only) {
						arrput(directory.nondicom_files, file);
						if (file.is_image) {
							directory.contains_nondicom_images = true;
						}
						if (file.type == VIEWER_FILE_TYPE_MRXS) {
							directory.contains_mrxs_files = true;
						}
					}
				}
			}
		} while (find_next_file(listing));
		close_directory_listing(listing);
	}

	return directory;
}

directory_info_t viewer_get_directory_info(const char* path) {
	return viewer_get_directory_info_internal(path, 0, false);
}

directory_info_t viewer_get_dicom_directory_info(const char* path) {
	return viewer_get_directory_info_internal(path, DICOM_DIRECTORY_RECURSION_LIMIT, false);
}

static bool image_try_load_with_openslide(image_t* image, const char* filename, image_load_options_t* options, bool is_overlay) {
	if (!options->openslide_available) {
		console_print("Can't try to load %s using OpenSlide, because OpenSlide is not available\n", filename);
		image->is_valid = false;
		return false;
	}

	image->type = IMAGE_TYPE_WSI;
	image->backend = IMAGE_BACKEND_OPENSLIDE;
	wsi_t wsi = {0};
	load_openslide_wsi(&wsi, filename, options->openslide_loading_done, options->thread_pool);
	if (wsi.osr) {
		init_image_from_openslide(image, &wsi, is_overlay);
		return image->is_valid;
	}
	return false;
}

image_t* image_load_from_file(file_info_t* file, directory_info_t* directory, image_load_options_t* options) {
	image_load_options_t default_options = {0};
	default_options.use_builtin_tiff_backend = true;
	default_options.use_native_mrxs_backend = true;
	default_options.thread_pool = &global_thread_pool;
	if (!options) options = &default_options;
	if (!options->thread_pool) options->thread_pool = &global_thread_pool;

	image_t* image = (image_t*)calloc(1, sizeof(image_t));
	image->is_local = true;
	image->resource_id = options->resource_id;

	bool is_overlay = options->is_overlay;
	image_t* parent_image = options->parent_image;

	const char* filename = file->full_filename;
	size_t filename_len = strlen(filename);
	const char* name = one_past_last_slash(filename, filename_len);
	copy_cstring(image->name, name, sizeof(image->name));

	if (name > filename) {
		size_t directory_len = (u64)name - (u64)filename;
		memcpy(image->directory, filename, ATMOST(directory_len, sizeof(image->directory)));
	}

	if (file->type == VIEWER_FILE_TYPE_SIMPLE_IMAGE) {
		// Load using stb_image
		image->type = IMAGE_TYPE_WSI;
		image->backend = IMAGE_BACKEND_STBI;
		image->simple.channels = 4; // desired: RGBA
		image->simple.pixels = stbi_load(filename, &image->simple.width, &image->simple.height, &image->simple.channels_in_file, 4);
		if (image->simple.pixels) {
			image->is_freshly_loaded = true;
			image->is_valid = true;
			init_image_from_stbi(image, &image->simple, is_overlay);
		}
	} else if (options->use_builtin_tiff_backend && (file->type == VIEWER_FILE_TYPE_TIFF /*|| file->type == VIEWER_FILE_TYPE_NDPI*/)) {
		// Try to open as TIFF, using the built-in backend
		tiff_t tiff = {0};
		if (open_tiff_file(&tiff, filename)) {
			init_image_from_tiff(image, tiff, is_overlay, parent_image);
		} else {
			tiff_destroy(&tiff);
			image->is_valid = false;
		}
	} else if (file->type == VIEWER_FILE_TYPE_ISYNTAX) {
		// Try to open as iSyntax
		isyntax_t isyntax = {0};
		isyntax_set_thread_pool(&isyntax, options->thread_pool);
		if (isyntax_open(&isyntax, filename, LIBISYNTAX_OPEN_FLAG_INIT_ALLOCATORS)) {
			init_image_from_isyntax(image, &isyntax, is_overlay);
		}
	} else if (file->type == VIEWER_FILE_TYPE_DICOM) {
		if (file->is_regular_file) {
			dicom_series_t dicom = {0};
			directory_info_t parent_directory = {0};
			directory_info_t* dicom_directory = directory;
			if (!dicom_directory) {
				parent_directory = viewer_get_directory_info(file->filename_prefix);
				dicom_directory = &parent_directory;
			}
			if (dicom_directory->is_valid &&
			    dicom_open_from_directory(&dicom, dicom_directory, file)) {
				init_image_from_dicom(image, &dicom, is_overlay);
			} else {
				dicom_destroy(&dicom);
			}
			viewer_directory_info_destroy(&parent_directory);
		} else if (file->is_directory && directory) {
			dicom_series_t dicom = {0};
			if (dicom_open_from_directory(&dicom, directory, NULL)) {
				init_image_from_dicom(image, &dicom, is_overlay);
			} else {
				dicom_destroy(&dicom);
			}
		}
	} else if (options->use_native_mrxs_backend && file->type == VIEWER_FILE_TYPE_MRXS) {
		mrxs_t mrxs = {0};
		mrxs_set_thread_pool(&mrxs, options->thread_pool);
		bool opened_successfully = false;
		if (file->is_regular_file) {
			// Strip .mrxs extension to get the name of the corresponding slide folder
			char basename[512];
			copy_cstring(basename, filename, sizeof(basename));
			char* end = basename + filename_len;
			for (char* pos = end - 1; pos >= basename; --pos) {
				if (*pos == '.') {
					*pos = '\0';
					break;
				}
				if (*pos == '/' || *pos == '\\') {
					break;
				}
			}
			file_info_t slide_dir_file_info = viewer_get_file_info(basename);
			if (slide_dir_file_info.is_directory) {
				opened_successfully = mrxs_open_from_directory(&mrxs, &slide_dir_file_info, NULL);
			}
		} else if (file->is_directory && directory) {
			// We directly opened the slide folder instead of the .mrxs
			opened_successfully = mrxs_open_from_directory(&mrxs, file, NULL);
		}
		if (opened_successfully) {
			init_image_from_mrxs(image, &mrxs, is_overlay);
		} else {
			mrxs_destroy(&mrxs);
			image_try_load_with_openslide(image, filename, options, is_overlay);
		}
	} else {
		// Try to load the file using OpenSlide
		image_try_load_with_openslide(image, filename, options, is_overlay);
	}

	if (image->is_valid && !image_initialize_runtime(image)) {
		image->is_valid = false;
	}

	return image;
}
