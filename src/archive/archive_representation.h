/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHIVE_MAX_FRAME_REPRESENTATIONS 8
#define ARCHIVE_MAX_ACCEPT_HEADER_SIZE 2048

typedef enum archive_frame_representation_source_t {
	ARCHIVE_FRAME_REPRESENTATION_STORED = 1,
	ARCHIVE_FRAME_REPRESENTATION_GENERATED,
} archive_frame_representation_source_t;

typedef struct archive_frame_representation_t {
	char media_type[32];
	char transfer_syntax_uid[65];
	archive_frame_representation_source_t source;
	i32 preference;
} archive_frame_representation_t;

typedef enum archive_representation_selection_result_t {
	ARCHIVE_REPRESENTATION_SELECTED = 0,
	ARCHIVE_REPRESENTATION_MISSING_ACCEPT,
	ARCHIVE_REPRESENTATION_NOT_ACCEPTABLE,
} archive_representation_selection_result_t;

/* NULL means the archive has no stored-frame contract for this syntax. */
const char* archive_frame_media_type_for_transfer_syntax(const char* transfer_syntax_uid);

archive_representation_selection_result_t archive_select_frame_representation(
	const char* accept, size_t accept_size,
	const archive_frame_representation_t* available, i32 available_count,
	archive_frame_representation_t* selected);

#ifdef __cplusplus
}
#endif
