/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_http.h"
#include "archive_ingest.h"
#include "archive_index.h"
#include "archive_repository.h"

#include "common.h"
#include "timerutils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void archive_print_usage(const char* executable) {
	fprintf(stderr,
		"Usage:\n"
		"  %s init REPOSITORY\n"
		"  %s check REPOSITORY\n"
		"  %s ingest REPOSITORY [--source PATH] [--reference | --zip-directory]\n"
		"  %s list REPOSITORY [--base-url URL]\n"
		"  %s serve REPOSITORY [--port PORT] [--max-requests COUNT]\n"
		"    --zip-directory captures exactly the supplied directory tree as an owned ZIP.\n",
		executable, executable, executable, executable, executable);
}

static bool archive_print_series_urls(
		const archive_repository_t* repository, const char* base_url,
		char* error, size_t error_capacity) {
	archive_index_t index = {0};
	if (!archive_index_open(&index, repository, error, error_capacity))
		return false;
	size_t base_length = strlen(base_url);
	while (base_length && base_url[base_length - 1] == '/') --base_length;
	for (i32 i = 0; i < index.series_count; ++i) {
		const archive_series_record_t* series = index.series + i;
		printf("%.*s/studies/%s/series/%s\n",
		       (i32)base_length, base_url, series->study_instance_uid,
		       series->series_instance_uid);
	}
	archive_index_destroy(&index);
	return true;
}

static bool archive_parse_unsigned(const char* text, u64 maximum, u64* value) {
	if (!text || !text[0]) return false;
	char* end = NULL;
	errno = 0;
	u64 parsed = strtoull(text, &end, 10);
	if (errno || !end || *end || parsed > maximum) return false;
	*value = parsed;
	return true;
}

int main(int argc, char** argv) {
	init_timer();
	if (argc < 3) {
		archive_print_usage(argv[0]);
		return 2;
	}
	archive_repository_t repository = {0};
	char error[512] = {0};
	archive_repository_status_t status;
	if (strcmp(argv[1], "init") == 0) {
		status = archive_repository_initialize(&repository, argv[2], error, sizeof(error));
		if (status != ARCHIVE_REPOSITORY_OK) {
			fprintf(stderr, "Archive initialization failed: %s%s%s\n",
			        archive_repository_status_string(status), error[0] ? ": " : "", error);
			return 1;
		}
		printf("Initialized pathology archive repository at %s\n", repository.root);
		return 0;
	}
	status = archive_repository_open(&repository, argv[2], error, sizeof(error));
	if (status != ARCHIVE_REPOSITORY_OK) {
		fprintf(stderr, "Archive repository check failed: %s%s%s\n",
		        archive_repository_status_string(status), error[0] ? ": " : "", error);
		return 1;
	}
	if (strcmp(argv[1], "check") == 0) {
		printf("Pathology archive repository is valid (schema %d): %s\n",
		       ARCHIVE_REPOSITORY_SCHEMA_VERSION, repository.root);
		return 0;
	}
	if (strcmp(argv[1], "ingest") == 0) {
		const char* source_path = NULL;
		bool reference_source = false;
		bool capture_directory = false;
		for (i32 i = 3; i < argc; ++i) {
			if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
				source_path = argv[++i];
			} else if (strcmp(argv[i], "--reference") == 0) {
				reference_source = true;
			} else if (strcmp(argv[i], "--zip-directory") == 0) {
				capture_directory = true;
			} else {
				archive_print_usage(argv[0]);
				return 2;
			}
		}
		if (reference_source && !source_path) {
			fprintf(stderr, "--reference requires an explicit --source path\n");
			return 2;
		}
		if (capture_directory && (!source_path || reference_source)) {
			fprintf(stderr, "--zip-directory requires --source DIRECTORY and cannot be combined with --reference\n");
			return 2;
		}
		archive_ingest_result_t result = {0};
		bool ingested;
		if (capture_directory) {
			ingested = archive_ingest_capture_directory(&repository, source_path, &result, error, sizeof(error));
		} else if (source_path) {
			ingested = archive_ingest_path(
				&repository, source_path, reference_source,
				&result, error, sizeof(error));
		} else {
			ingested = archive_ingest_part10_objects(
				&repository, &result, error, sizeof(error));
		}
		if (!ingested) {
			fprintf(stderr, "Archive ingestion failed: %s\n", error[0] ? error : "unknown error");
			return 1;
		}
		if (reference_source)
			printf("Development reference mode: source bytes remain externally owned "
			       "and must not be moved, changed, or deleted; their absolute paths "
			       "are stored in SQLite and objects/ contains no reference payloads "
			       "or symbolic links.\n");
		printf("Published %d virtual DICOM instance%s in %d series and %d stud%s\n",
		       result.instance_count, result.instance_count == 1 ? "" : "s",
		       result.series_count, result.study_count, result.study_count == 1 ? "y" : "ies");
		if (!archive_print_series_urls(
				&repository, "http://127.0.0.1:8043/dicom-web",
				error, sizeof(error)))
			fprintf(stderr, "Could not list published series URLs: %s\n", error);
		return 0;
	}
	if (strcmp(argv[1], "list") == 0) {
		const char* base_url = "http://127.0.0.1:8043/dicom-web";
		if (argc == 5 && strcmp(argv[3], "--base-url") == 0) {
			base_url = argv[4];
		} else if (argc != 3) {
			archive_print_usage(argv[0]);
			return 2;
		}
		if (!archive_print_series_urls(
				&repository, base_url, error, sizeof(error))) {
			fprintf(stderr, "Could not list published series URLs: %s\n",
			        error[0] ? error : "unknown error");
			return 1;
		}
		return 0;
	}
	if (strcmp(argv[1], "serve") == 0) {
		u16 port = 8043;
		size_t max_requests = 0;
		for (i32 i = 3; i < argc; i += 2) {
			if (i + 1 >= argc) {
				archive_print_usage(argv[0]);
				return 2;
			}
			u64 value = 0;
			if (strcmp(argv[i], "--port") == 0 && archive_parse_unsigned(argv[i + 1], 65535, &value) && value > 0) {
				port = (u16)value;
			} else if (strcmp(argv[i], "--max-requests") == 0 &&
			           archive_parse_unsigned(argv[i + 1], 1000000, &value)) {
				max_requests = (size_t)value;
			} else {
				archive_print_usage(argv[0]);
				return 2;
			}
		}
		return archive_http_serve(&repository, port, max_requests);
	}
	archive_print_usage(argv[0]);
	return 2;
}
