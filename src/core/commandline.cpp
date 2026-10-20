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

// directories to know:
// exe directory: the directory the executable is located in
// run/working directory: the directory the program was launched from
// target/active directory: the directory of the input image
// output directory: the directory where an export operation saves files to

#include "common.h"
#include "viewer.h"
#include "gui.h" // for global data, TODO: refactor
#include "stringutils.h"
#include "tiff_write.h"

app_command_t app_parse_commandline(int argc, const char** argv) {
	app_command_t app_command = {};

	// Skip argument 0 (= the executable path)
	argc = ATLEAST(0, argc - 1);
	const char** args = argv + 1;
	for (i32 arg_index = 0; arg_index < argc; ++arg_index) {
		const char* arg = args[arg_index];
		if (strcmp(arg, "--version") == 0) {
			app_command.headless = true;
			app_command.command = COMMAND_PRINT_VERSION;
			app_command.exit_immediately = true;
		} else if (strcmp(arg, "--export") == 0) {
			app_command.headless = true;
			app_command.command = COMMAND_EXPORT;
			app_command.export_command.with_annotations = false;
			// TODO: allow use as conversion utility without need for ROI
			app_command.export_command.error = COMMAND_EXPORT_ERROR_NO_ROI;
			// slidescape 1.tiff --export --roi "Annotation 0"
			++arg_index;
			for (; arg_index < argc; ++arg_index) {
				arg = args[arg_index];
				if (strcmp(arg, "--roi") == 0) {
					if (arg_index < argc) {
						++arg_index;
						arg = args[arg_index];
						app_command.export_command.roi = arg;
						app_command.export_command.error = COMMAND_EXPORT_ERROR_NONE;
					}
				} else if (strcmp(arg, "--first-roi") == 0) {
					app_command.export_command.use_first_roi = true;
					app_command.export_command.error = COMMAND_EXPORT_ERROR_NONE;
				} else if (strcmp(arg, "--no-annotations") == 0) {
					app_command.export_command.with_annotations = false;
				} else if (strcmp(arg, "--with-annotations") == 0) {
					app_command.export_command.with_annotations = true;
				} else if (strcmp(arg, "--quality") == 0) {
					if (arg_index < argc) {
						++arg_index;
						arg = args[arg_index];
						i32 new_quality = atoi(arg);
						if (new_quality > 0 && new_quality <= 100) {
							tiff_export_jpeg_quality = new_quality;
						} else {
							console_print_error("Invalid JPEG quality setting '%s', defaulting to %d\n", arg, tiff_export_jpeg_quality);
						}
					}
				} else if (strcmp(arg, "--mpp") == 0) {
                    if (arg_index < argc) {
                        ++arg_index;
                        arg = args[arg_index];
                        tiff_export_mpp = atof(arg);
                        tiff_export_match_input_resolution = false;
                    }
                } else if (strcmp(arg, "--tile-size") == 0) {
                    if (arg_index < argc) {
                        ++arg_index;
                        arg = args[arg_index];
                        tiff_export_tile_width = atoi(arg);
                    }
                } else if (strcmp(arg, "--postfix") == 0) {
					if (arg_index < argc) {
						++arg_index;
						arg = args[arg_index];
						global_export_region_filename_postfix = arg;
					}
				} else {
					--arg_index; // not recognized, try again one level up
					break;
				}
			}
		} else  if (strcmp(arg, "--verbose") == 0) {
			is_verbose_mode = true;
		} else if (strcmp(arg, "--dicomweb-username") == 0) {
			if (++arg_index < argc) {
				app_command.dicomweb_username = args[arg_index];
			} else {
				console_print_error("--dicomweb-username expects a value\n");
			}
		} else if (strcmp(arg, "--dicomweb-password") == 0) {
			if (++arg_index < argc) {
				app_command.dicomweb_password = args[arg_index];
			} else {
				console_print_error("--dicomweb-password expects a value\n");
			}
		} else if (strcmp(arg, "--replay") == 0 || strcmp(arg, "--record-replay") == 0 ||
		           strcmp(arg, "--profile") == 0) {
			if (++arg_index >= argc) {
				console_print_error("%s expects a filename\n", arg);
				app_command.argument_error = true;
				break;
			}
			if (strcmp(arg, "--replay") == 0) {
				app_command.replay_path = args[arg_index];
			} else if (strcmp(arg, "--record-replay") == 0) {
				app_command.record_replay_path = args[arg_index];
			} else {
				app_command.profile_path = args[arg_index];
			}
		} else if (strcmp(arg, "--headless") == 0) {
			app_command.headless = true;
		} else if (strcmp(arg, "--overlay") == 0) {
			bool found_overlay_input = false;
			++arg_index;
			for (; arg_index < argc; ++arg_index) {
				arg = args[arg_index];
				if (strncmp(arg, "--", 2) == 0) {
					--arg_index; // not an overlay filename; try again one level up
					break;
				}
				arrput(app_command.overlay_inputs, arg);
				found_overlay_input = true;
			}
			if (!found_overlay_input) {
				console_print_error("--overlay expects at least one input file\n");
			}
		} else {
			// Unknown command, assume that it's an input file
			arrput(app_command.inputs, arg);

		}
	}



	if ((app_command.replay_path && (app_command.record_replay_path || arrlen(app_command.inputs) || arrlen(app_command.overlay_inputs))) ||
	    (app_command.headless && (app_command.replay_path || app_command.record_replay_path || app_command.profile_path))) {
		console_print_error("Replay/profile options require the graphical viewer; playback cannot be combined with recording or input files.\n");
		app_command.argument_error = true;
	}
	if (app_command.profile_path &&
	    ((app_command.replay_path && strcmp(app_command.profile_path, app_command.replay_path) == 0) ||
	     (app_command.record_replay_path && strcmp(app_command.profile_path, app_command.record_replay_path) == 0))) {
		console_print_error("Profile and replay filenames must differ.\n");
		app_command.argument_error = true;
	}
	return app_command;
}

void app_command_execute_immediately(app_command_t* app_command) {
	if (app_command->command == COMMAND_PRINT_VERSION) {
		console_print(APP_TITLE " " APP_VERSION "\n");
	}
}

bool app_load_commandline_inputs(app_state_t* app_state) {
	app_command_t* command = &app_state->command;
	bool success = true;
	if (arrlen(command->inputs) > 0) {
		const char* filename = command->inputs[0];
		u32 filetype_hint = load_next_image_as_overlay ? FILETYPE_HINT_OVERLAY : 0;
		success = app_load_input(app_state, filename, filetype_hint);
		if (success) {
			for (i32 overlay_index = 0; overlay_index < arrlen(command->overlay_inputs); ++overlay_index) {
				const char* overlay_filename = command->overlay_inputs[overlay_index];
				if (!load_generic_file(app_state, overlay_filename, FILETYPE_HINT_OVERLAY)) {
					success = false;
				}
			}
		}
	} else if (arrlen(command->overlay_inputs) > 0) {
		console_print_error("--overlay requires a base input file\n");
		success = false;
	}
	return success;
}

void export_region_get_name_hint(app_state_t* app_state, char* output_buffer, size_t output_size) {
	const char* name_hint = "output";
	if (arrlen(app_state->loaded_images) > 0) {
		for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
			image_t* image = app_state->loaded_images[i];
			if (image->name[0] != '\0') {
				size_t buffer_size = sizeof(image->name);
				char* new_name_hint = (char*)alloca(buffer_size);
				copy_cstring(new_name_hint, image->name, buffer_size);
				// Strip filename extension
				size_t len = strlen(new_name_hint);
				for (i32 pos = len-1; pos >= 1; --pos) {
					if (new_name_hint[pos] == '.') {
						new_name_hint[pos] = '\0';
						// add '_region'
						copy_cstring(new_name_hint + pos, global_export_region_filename_postfix, buffer_size - pos);
						name_hint = new_name_hint;
						break;
					}
				}
			}
		}
	}

	const char* filename_extension_hint = "";
	if (desired_region_export_format == 0) {
#if APPLE
		// macOS does not seem to like tile TIFF files in the Finder (will sometimes stop responding,
		// at least on my system). So choose the .ptif file extension by default as an alternative.
		filename_extension_hint = ".ptif";
#else
		filename_extension_hint = ".tiff";
#endif
	} else if (desired_region_export_format == 1) {
		filename_extension_hint = ".jpeg";
	} else if (desired_region_export_format == 2) {
		filename_extension_hint = ".png";
	}

	snprintf(output_buffer, output_size-1, "%s%s", name_hint, filename_extension_hint);
};

int app_command_execute(app_state_t* app_state) {
	app_command_t* command = &app_state->command;
	if (command->command == COMMAND_NONE) {
		if (command->headless) {
			return app_load_commandline_inputs(app_state) ? 0 : 1;
		}
	} else if (command->command == COMMAND_EXPORT) {
		for (i32 i = 0; i < arrlen(command->inputs); ++i) {
			console_print("input: %s\n", command->inputs[i]);
		}

		for (i32 input_index = 0; input_index < arrlen(command->inputs); ++input_index) {
			const char* filename = command->inputs[input_index];
			if (load_generic_file(app_state, filename, 0)) {
				if (arrlen(app_state->loaded_images) > 0) {
					image_t* image = app_state->loaded_images[0];
					if (image->backend == IMAGE_BACKEND_TIFF || image->backend == IMAGE_BACKEND_OPENSLIDE || image->backend == IMAGE_BACKEND_DICOM || image->backend == IMAGE_BACKEND_ISYNTAX) {
						u32 export_flags = 0;
						// TODO: allow configuration
						if (command->export_command.with_annotations) {
							export_flags |= EXPORT_FLAGS_ALSO_EXPORT_ANNOTATIONS;
						}
						export_flags |= EXPORT_FLAGS_PUSH_ANNOTATION_COORDINATES_INWARD;

						annotation_set_t* annotation_set = &app_state->scene.annotation_set;
						bool want_roi = true;
						if (command->export_command.error == COMMAND_EXPORT_ERROR_NO_ROI)  {
							want_roi = false;
						}
						if (want_roi && annotation_set->active_annotation_count > 0) {
							bool found_roi = false;
							annotation_t* roi_annotation = NULL;
							for (i32 i = 0; i < annotation_set->active_annotation_count; ++i) {
								annotation_t* annotation = get_active_annotation(&app_state->scene.annotation_set, i);
								if (command->export_command.use_first_roi || strncmp(annotation->name, command->export_command.roi, COUNT(annotation->name)-1) == 0) {
									found_roi = true;
									roi_annotation = annotation;
									// Makes no sense to export the annotations if the only one existing is the one specifying what to export
									if (annotation_set->active_annotation_count == 1 && annotation_set->active_group_count <= 1 && annotation_set->active_feature_count <= 1) {
										export_flags &= ~EXPORT_FLAGS_ALSO_EXPORT_ANNOTATIONS;
									}
									break;
								}
							}

							if (found_roi) {
								bounds2f world_bounds = bounds_for_annotation(roi_annotation);
								// TODO: MRXS backend: don't rely on origin_offset, pad the image with empty tiles instead
								bounds2f source_bounds = world_bounds;
								source_bounds.min = v2f_subtract(source_bounds.min, image->origin_offset);
								source_bounds.max = v2f_subtract(source_bounds.max, image->origin_offset);
								bounds2i pixel_bounds = world_bounds_to_pixel_bounds(&source_bounds, image->mpp_x, image->mpp_y);

								char filename_hint[512];
								export_region_get_name_hint(app_state, filename_hint, sizeof(filename_hint));
								export_cropped_bigtiff_with_resample(app_state, image, 0, world_bounds, pixel_bounds,
								                                     filename_hint, tiff_export_tile_width,
								                                     tiff_export_desired_color_space,
								                                     tiff_export_jpeg_quality, export_flags, !tiff_export_match_input_resolution,
								                                     V2F(tiff_export_mpp, tiff_export_mpp));
							} else {
								if (command->export_command.use_first_roi) {
									console_print_error("ROI export failed: could not find an annotation to use as ROI\n");
								} else if (command->export_command.roi) {
									console_print_error("ROI export failed: could not find an annotation with name \'%s\'\n", command->export_command.roi);
								}
							}
						} else if (!want_roi) {
							console_print("No ROI provided, using whole slide for export\n");
							bounds2i pixel_bounds = BOUNDS2I(0, 0, image->width_in_pixels, image->height_in_pixels);
							bounds2f world_bounds = pixel_bounds_to_world_bounds(pixel_bounds, image->mpp_x, image->mpp_y);

							// TODO: MRXS backend: don't rely on origin_offset, pad the image with empty tiles instead
							world_bounds.min = v2f_add(world_bounds.min, image->origin_offset);
							world_bounds.max = v2f_add(world_bounds.max, image->origin_offset);

							char filename_hint[512];
							export_region_get_name_hint(app_state, filename_hint, sizeof(filename_hint));
							export_cropped_bigtiff_with_resample(app_state, image, 0, world_bounds, pixel_bounds,
							                                     filename_hint, tiff_export_tile_width,
							                                     tiff_export_desired_color_space,
							                                     tiff_export_jpeg_quality, export_flags, !tiff_export_match_input_resolution,
							                                     V2F(tiff_export_mpp, tiff_export_mpp));
						}


					}
				}
			}
		}
	}
	return 0;

}
