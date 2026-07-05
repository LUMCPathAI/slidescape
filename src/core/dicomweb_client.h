/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema
*/

#pragma once

#include "common.h"
#include "dicomweb.h"
#include "work_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_state_t app_state_t;
typedef struct image_t image_t;

typedef enum dicomweb_async_query_kind_t {
	DICOMWEB_ASYNC_QUERY_NONE = 0,
	DICOMWEB_ASYNC_QUERY_STUDIES,
	DICOMWEB_ASYNC_QUERY_SERIES,
	DICOMWEB_ASYNC_QUERY_INSTANCES,
	DICOMWEB_ASYNC_OPEN_SERIES,
} dicomweb_async_query_kind_t;

typedef struct dicomweb_async_result_t {
	dicomweb_async_query_kind_t kind;
	i32 generation;
	i32 http_status;
	bool succeeded;
	bool cancelled;
	char message[512];
	i32 count;
	image_t* image;
	union {
		dicomweb_study_ref_t* studies;
		dicomweb_series_ref_t* series;
		dicomweb_instance_ref_t* instances;
		void* items;
	};
} dicomweb_async_result_t;

typedef struct dicomweb_async_context_t {
	completion_queue_t completions;
	volatile i32 generation;
	volatile i32 active_tasks;
	bool initialized;
} dicomweb_async_context_t;

void dicomweb_async_context_init(dicomweb_async_context_t* context);
void dicomweb_async_context_destroy(dicomweb_async_context_t* context);
i32 dicomweb_async_query(dicomweb_async_context_t* context,
                         dicomweb_async_query_kind_t kind,
                         const char* archive_url,
                         const char* username,
                         const char* password,
                         const char* study_uid,
                         const char* series_uid);
i32 dicomweb_async_open_series_url(dicomweb_async_context_t* context,
                                   const char* series_url,
                                   const char* username,
                                   const char* password);
void dicomweb_async_cancel(dicomweb_async_context_t* context);
bool dicomweb_async_poll(dicomweb_async_context_t* context,
                         dicomweb_async_result_t** out_result);
void dicomweb_async_result_destroy(dicomweb_async_result_t* result);
bool dicomweb_async_commit_open(app_state_t* app_state,
                                dicomweb_async_result_t* result);

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
