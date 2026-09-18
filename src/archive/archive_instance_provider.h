/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "archive_index.h"
#include "archive_representation.h"
#include "archive_repository.h"
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boundary between the published DICOM model and its pixel representation.
 *
 * HTTP code supplies an immutable published index record. The provider loads and
 * digest-validates its virtual DICOM shell, validates the shell/source identities,
 * then returns DICOM JSON or one frame in the published transfer syntax. Future
 * native-format providers implement this same contract; routes must never learn
 * MRXS/iSyntax paths or tile rules.
 *
 * The Part 10 adapter keeps a bounded process-lifetime LRU of validated shell buffers,
 * source file handles, and delta-offset directories. Metadata is serialized on demand. Request-time providers perform only
 * shell lookup, bounded source reads, and optional frame generation.
 */
typedef struct archive_instance_frame_t {
	u8* data;
	size_t size;
	char media_type[32];
	char transfer_syntax_uid[65];
} archive_instance_frame_t;

typedef struct archive_instance_provider_cache_stats_t {
	u64 hits;
	u64 misses;
	u64 evictions;
	i32 resident_count;
	i32 capacity;
	u64 shell_bytes;
	u64 payload_bytes;
	u64 shell_hits;
	u64 shell_misses;
	u64 shell_evictions;
	i32 shell_resident_count;
	i32 shell_capacity;
} archive_instance_provider_cache_stats_t;

/* Main-thread lifecycle. Initialization also starts Slidescape's shared worker/TLS
 * runtime so every provider call made by the HTTP pool has a temporary arena. */
bool archive_instance_provider_initialize(void);
void archive_instance_provider_shutdown(void);
archive_instance_provider_cache_stats_t archive_instance_provider_get_cache_stats(void);
bool archive_instance_provider_get_metadata(const archive_repository_t* repository,
		const archive_instance_record_t* record, char** json, size_t* json_size,
		char* error, size_t error_capacity);
bool archive_instance_provider_get_series_metadata(
		const archive_repository_t* repository,
		const archive_instance_record_t* records, i32 record_count,
		char** json, size_t* json_size, char* error, size_t error_capacity);
i32 archive_instance_provider_get_frame_representations(
		const archive_repository_t* repository, const archive_instance_record_t* record,
		archive_frame_representation_t* representations, i32 capacity,
		char* error, size_t error_capacity);
bool archive_instance_provider_get_frame(const archive_repository_t* repository,
		const archive_instance_record_t* record, u32 one_based_frame,
		const archive_frame_representation_t* representation,
		archive_instance_frame_t* frame, char* error, size_t error_capacity);
void archive_instance_frame_destroy(archive_instance_frame_t* frame);

#ifdef __cplusplus
}
#endif
