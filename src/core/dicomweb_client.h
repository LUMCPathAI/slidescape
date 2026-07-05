/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema
*/

#pragma once

#include "common.h"
#include "dicomweb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_state_t app_state_t;
typedef struct image_t image_t;

bool dicomweb_open_series(app_state_t* app_state, const char* series_url,
                          const char* username, const char* password);
bool dicomweb_open_related_series(app_state_t* app_state, const image_t* current_image,
                                  i32 series_index);
i32 dicomweb_query_studies(const char* archive_url, const char* username, const char* password,
                           dicomweb_study_ref_t* out, i32 capacity);
i32 dicomweb_query_series(const char* archive_url, const char* username, const char* password,
                          const char* study_uid, dicomweb_series_ref_t* out, i32 capacity);
i32 dicomweb_query_instances(const char* archive_url, const char* username, const char* password,
                             const char* study_uid, const char* series_uid,
                             dicomweb_instance_ref_t* out, i32 capacity);
bool dicomweb_build_series_url(char* dest, size_t dest_size, const char* archive_url,
                               const char* study_uid, const char* series_uid);
const char* dicomweb_get_last_status(void);
void dicomweb_load_tile_batch_func(i32 logical_thread_index, void* userdata);

#ifdef __cplusplus
}
#endif
