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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "platform.h"
#include "viewer.h"

typedef enum export_flags_enum {
	EXPORT_FLAGS_NONE = 0,
	EXPORT_FLAGS_ALSO_EXPORT_ANNOTATIONS = 0x1,
	EXPORT_FLAGS_PUSH_ANNOTATION_COORDINATES_INWARD = 0x2,
} export_flags_enum;

bool export_cropped_bigtiff(app_state_t* app_state, image_t* image, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                              u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags);
bool export_cropped_bigtiff_with_resample(app_state_t* app_state, image_t* image, i32 plane, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                                          u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags, bool need_resize, v2f target_mpp);
void begin_export_cropped_bigtiff(app_state_t* app_state, image_t* image, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                                  u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags);
void begin_export_cropped_bigtiff_with_resample(app_state_t* app_state, image_t* image, bounds2f world_bounds, bounds2i level0_bounds, const char* filename,
                                                u32 export_tile_width, u16 desired_photometric_interpretation, i32 quality, u32 export_flags, bool need_resize, v2f target_mpp);

#ifdef __cplusplus
}
#endif

