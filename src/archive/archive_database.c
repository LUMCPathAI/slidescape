/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "archive_database.h"

#include "sqlite3.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char archive_database_schema[] =
	"BEGIN IMMEDIATE;\n"
	"CREATE TABLE IF NOT EXISTS archive_schema (\n"
	"  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),\n"
	"  schema_version INTEGER NOT NULL,\n"
	"  information_model TEXT NOT NULL\n"
	") STRICT;\n"
	"INSERT OR IGNORE INTO archive_schema VALUES (1, 5, 'virtual-dicom-shell-v0.1');\n"
	"CREATE TABLE IF NOT EXISTS schema_migration (\n"
	"  version INTEGER PRIMARY KEY,\n"
	"  name TEXT NOT NULL,\n"
	"  applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
	") STRICT;\n"
	"INSERT OR IGNORE INTO schema_migration(version, name) VALUES (2, 'initial durable model v2');\n"
	"INSERT OR IGNORE INTO schema_migration(version, name) VALUES (3, 'inline control objects');\n"
	"INSERT OR IGNORE INTO schema_migration(version, name) VALUES (4, 'logical slide aggregate shells');\n"
	"INSERT OR IGNORE INTO schema_migration(version, name) VALUES (5, 'external object locators');\n"
	"CREATE TABLE IF NOT EXISTS object (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  sha256 TEXT NOT NULL UNIQUE\n"
	"    CHECK (length(sha256) = 64 AND sha256 NOT GLOB '*[^0-9a-f]*'),\n"
	"  byte_size INTEGER NOT NULL CHECK (byte_size >= 0),\n"
	"  media_type TEXT NOT NULL,\n"
	"  storage_key TEXT UNIQUE,\n"
	"  external_path TEXT UNIQUE,\n"
	"  inline_data BLOB,\n"
	"  retention_state TEXT NOT NULL DEFAULT 'ACTIVE'\n"
	"    CHECK (retention_state IN ('ACTIVE', 'QUARANTINED', 'HELD', 'DELETABLE')),\n"
	"  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
	"  CHECK ((storage_key IS NOT NULL) + (external_path IS NOT NULL) +\n"
	"         (inline_data IS NOT NULL) = 1),\n"
	"  CHECK (inline_data IS NULL OR length(inline_data) = byte_size)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS bundle (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  bundle_digest TEXT NOT NULL UNIQUE CHECK (length(bundle_digest) = 64),\n"
	"  source_format TEXT NOT NULL,\n"
	"  adapter_name TEXT NOT NULL,\n"
	"  adapter_version TEXT NOT NULL,\n"
	"  configuration_digest TEXT CHECK (configuration_digest IS NULL OR length(configuration_digest) = 64),\n"
	"  descriptor_object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  state TEXT NOT NULL CHECK (state IN ('RECEIVING', 'STORED', 'VALIDATING', 'STAGED', 'PUBLISHED', 'REJECTED')),\n"
	"  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS bundle_object (\n"
	"  bundle_id INTEGER NOT NULL REFERENCES bundle(id),\n"
	"  object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  logical_name TEXT NOT NULL,\n"
	"  role TEXT NOT NULL,\n"
	"  ordinal INTEGER NOT NULL CHECK (ordinal >= 0),\n"
	"  PRIMARY KEY (bundle_id, logical_name),\n"
	"  UNIQUE (bundle_id, ordinal)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS part10_instance (\n"
	"  sop_instance_uid TEXT PRIMARY KEY,\n"
	"  object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  sop_class_uid TEXT NOT NULL,\n"
	"  study_instance_uid TEXT NOT NULL,\n"
	"  series_instance_uid TEXT NOT NULL,\n"
	"  transfer_syntax_uid TEXT NOT NULL,\n"
	"  modality TEXT NOT NULL,\n"
	"  study_date TEXT NOT NULL CHECK (length(study_date) IN (0, 8)),\n"
	"  instance_number INTEGER,\n"
	"  frame_count INTEGER NOT NULL CHECK (frame_count > 0),\n"
	"  rows INTEGER NOT NULL CHECK (rows > 0),\n"
	"  columns INTEGER NOT NULL CHECK (columns > 0),\n"
	"  total_pixel_matrix_rows INTEGER NOT NULL CHECK (total_pixel_matrix_rows > 0),\n"
	"  total_pixel_matrix_columns INTEGER NOT NULL CHECK (total_pixel_matrix_columns > 0),\n"
	"  image_flavor TEXT NOT NULL,\n"
	"  registered_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS metadata_version (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  fingerprint TEXT NOT NULL UNIQUE CHECK (length(fingerprint) = 64),\n"
	"  schema_version INTEGER NOT NULL CHECK (schema_version > 0),\n"
	"  canonical_metadata_object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  provenance_object_id INTEGER REFERENCES object(id),\n"
	"  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS representation (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  representation_id TEXT NOT NULL,\n"
	"  version INTEGER NOT NULL CHECK (version > 0),\n"
	"  fingerprint TEXT NOT NULL UNIQUE CHECK (length(fingerprint) = 64),\n"
	"  bundle_id INTEGER NOT NULL REFERENCES bundle(id),\n"
	"  metadata_version_id INTEGER NOT NULL REFERENCES metadata_version(id),\n"
	"  shell_object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  state TEXT NOT NULL CHECK (state IN ('STAGED', 'PUBLISHED', 'RETIRED', 'REJECTED')),\n"
	"  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
	"  UNIQUE (representation_id, version)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS study (\n"
	"  representation_id INTEGER NOT NULL REFERENCES representation(id),\n"
	"  study_instance_uid TEXT NOT NULL,\n"
	"  study_date TEXT,\n"
	"  accession_number TEXT,\n"
	"  description TEXT,\n"
	"  PRIMARY KEY (representation_id, study_instance_uid)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS series (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  representation_id INTEGER NOT NULL,\n"
	"  study_instance_uid TEXT NOT NULL,\n"
	"  series_instance_uid TEXT NOT NULL,\n"
	"  series_number INTEGER,\n"
	"  modality TEXT NOT NULL,\n"
	"  description TEXT,\n"
	"  FOREIGN KEY (representation_id, study_instance_uid)\n"
	"    REFERENCES study(representation_id, study_instance_uid),\n"
	"  UNIQUE (representation_id, study_instance_uid, series_instance_uid)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS instance (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  representation_id INTEGER NOT NULL,\n"
	"  study_instance_uid TEXT NOT NULL,\n"
	"  series_instance_uid TEXT NOT NULL,\n"
	"  sop_instance_uid TEXT NOT NULL,\n"
	"  sop_class_uid TEXT NOT NULL,\n"
	"  instance_number INTEGER,\n"
	"  image_flavor TEXT NOT NULL,\n"
	"  frame_count INTEGER NOT NULL CHECK (frame_count > 0),\n"
	"  rows INTEGER NOT NULL CHECK (rows > 0),\n"
	"  columns INTEGER NOT NULL CHECK (columns > 0),\n"
	"  total_pixel_matrix_rows INTEGER NOT NULL CHECK (total_pixel_matrix_rows > 0),\n"
	"  total_pixel_matrix_columns INTEGER NOT NULL CHECK (total_pixel_matrix_columns > 0),\n"
	"  metadata_object_id INTEGER NOT NULL REFERENCES object(id),\n"
	"  shell_instance_index INTEGER NOT NULL CHECK (shell_instance_index >= 0),\n"
	"  FOREIGN KEY (representation_id, study_instance_uid, series_instance_uid)\n"
	"    REFERENCES series(representation_id, study_instance_uid, series_instance_uid),\n"
	"  UNIQUE (representation_id, sop_instance_uid)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS instance_provider (\n"
	"  instance_id INTEGER PRIMARY KEY REFERENCES instance(id),\n"
	"  provider_kind TEXT NOT NULL,\n"
	"  source_object_id INTEGER REFERENCES object(id),\n"
	"  frame_map_object_id INTEGER REFERENCES object(id),\n"
	"  parameters_object_id INTEGER REFERENCES object(id),\n"
	"  adapter_version TEXT NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS frame_representation (\n"
	"  instance_id INTEGER NOT NULL REFERENCES instance(id),\n"
	"  media_type TEXT NOT NULL,\n"
	"  transfer_syntax_uid TEXT NOT NULL,\n"
	"  source TEXT NOT NULL CHECK (source IN ('STORED', 'GENERATED')),\n"
	"  source_object_id INTEGER REFERENCES object(id),\n"
	"  policy_version TEXT,\n"
	"  preference INTEGER NOT NULL DEFAULT 0,\n"
	"  PRIMARY KEY (instance_id, media_type, transfer_syntax_uid)\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS publication (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  representation_id INTEGER NOT NULL UNIQUE REFERENCES representation(id),\n"
	"  committed_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
	"  actor TEXT NOT NULL,\n"
	"  policy_version TEXT NOT NULL\n"
	") STRICT;\n"
	"CREATE TABLE IF NOT EXISTS outbox (\n"
	"  id INTEGER PRIMARY KEY,\n"
	"  publication_id INTEGER NOT NULL REFERENCES publication(id),\n"
	"  operation TEXT NOT NULL,\n"
	"  state TEXT NOT NULL DEFAULT 'PENDING'\n"
	"    CHECK (state IN ('PENDING', 'RUNNING', 'COMPLETE', 'FAILED')),\n"
	"  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP\n"
	") STRICT;\n"
	"CREATE INDEX IF NOT EXISTS study_qido_uid ON study(study_instance_uid);\n"
	"CREATE INDEX IF NOT EXISTS series_qido_uid ON series(study_instance_uid, series_instance_uid);\n"
	"CREATE INDEX IF NOT EXISTS instance_qido_uid\n"
	"  ON instance(study_instance_uid, series_instance_uid, sop_instance_uid);\n"
	"CREATE INDEX IF NOT EXISTS representation_published\n"
	"  ON representation(state, representation_id, version);\n"
	"CREATE INDEX IF NOT EXISTS part10_instance_study_series\n"
	"  ON part10_instance(study_instance_uid, series_instance_uid);\n"
	"PRAGMA application_id = 1397510209;\n"
	"PRAGMA user_version = 5;\n"
	"COMMIT;\n";

static const char* archive_database_required_tables[] = {
	"archive_schema",
	"schema_migration",
	"object",
	"bundle",
	"bundle_object",
	"metadata_version",
	"representation",
	"study",
	"series",
	"instance",
	"instance_provider",
	"frame_representation",
	"publication",
	"outbox",
	"part10_instance",
};

static void archive_database_set_error(char* error, size_t capacity, const char* format, ...) {
	if (!error || capacity == 0) return;
	va_list args;
	va_start(args, format);
	vsnprintf(error, capacity, format, args);
	va_end(args);
}

static bool archive_database_path(const archive_repository_t* repository, char* path,
		size_t path_capacity) {
	return archive_repository_make_path(repository, ARCHIVE_DATABASE_FILENAME, path, path_capacity);
}

static bool archive_database_sha256_is_valid(const char* sha256) {
	if (!sha256 || strlen(sha256) != 64) return false;
	for (i32 i = 0; i < 64; ++i) {
		if (!((sha256[i] >= '0' && sha256[i] <= '9') ||
		      (sha256[i] >= 'a' && sha256[i] <= 'f'))) return false;
	}
	return true;
}

static archive_database_status_t archive_database_configure(sqlite3* database,
		char* error, size_t error_capacity) {
	int ignored = 0;
	if (sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, &ignored) != SQLITE_OK ||
	    sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &ignored) != SQLITE_OK ||
	    sqlite3_exec(database, "PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;",
	                 NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not configure archive database: %s",
		                           sqlite3_errmsg(database));
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	return ARCHIVE_DATABASE_OK;
}

static archive_database_status_t archive_database_read_pragma(sqlite3* database,
		const char* pragma, i32* value, char* error, size_t error_capacity) {
	sqlite3_stmt* statement = NULL;
	int result = sqlite3_prepare_v2(database, pragma, -1, &statement, NULL);
	if (result != SQLITE_OK || sqlite3_step(statement) != SQLITE_ROW) {
		archive_database_set_error(error, error_capacity, "could not read archive database schema: %s",
		                           sqlite3_errmsg(database));
		sqlite3_finalize(statement);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	*value = sqlite3_column_int(statement, 0);
	sqlite3_finalize(statement);
	return ARCHIVE_DATABASE_OK;
}

static archive_database_status_t archive_database_validate_open(sqlite3* database,
		char* error, size_t error_capacity) {
	i32 application_id = 0;
	i32 schema_version = 0;
	archive_database_status_t status = archive_database_read_pragma(
		database, "PRAGMA application_id;", &application_id, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_read_pragma(
			database, "PRAGMA user_version;", &schema_version, error, error_capacity);
	}
	if (status != ARCHIVE_DATABASE_OK) return status;
	if ((u32)application_id != ARCHIVE_DATABASE_APPLICATION_ID ||
	    schema_version != ARCHIVE_DATABASE_SCHEMA_VERSION) {
		archive_database_set_error(error, error_capacity,
			"archive database schema is incompatible (application %08x, schema %d)",
			(u32)application_id, schema_version);
		return ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA;
	}

	sqlite3_stmt* statement = NULL;
	int result = sqlite3_prepare_v2(database,
		"SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?1;", -1,
		&statement, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not inspect archive database: %s",
		                           sqlite3_errmsg(database));
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	for (i32 i = 0; i < COUNT(archive_database_required_tables); ++i) {
		sqlite3_reset(statement);
		sqlite3_clear_bindings(statement);
		sqlite3_bind_text(statement, 1, archive_database_required_tables[i], -1, SQLITE_STATIC);
		if (sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
				"archive database is missing required table '%s'",
				archive_database_required_tables[i]);
			sqlite3_finalize(statement);
			return ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA;
		}
	}
	sqlite3_finalize(statement);
	return ARCHIVE_DATABASE_OK;
}

archive_database_status_t archive_database_initialize(const archive_repository_t* repository,
		char* error, size_t error_capacity) {
	if (!repository || !repository->root[0]) return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path))) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}

	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
		NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                           path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	i32 application_id = 0;
	i32 schema_version = 0;
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_read_pragma(
			database, "PRAGMA application_id;", &application_id, error, error_capacity);
		if (status == ARCHIVE_DATABASE_OK) {
			status = archive_database_read_pragma(
				database, "PRAGMA user_version;", &schema_version, error, error_capacity);
		}
		if (status == ARCHIVE_DATABASE_OK &&
		    (application_id != 0 || schema_version != 0) &&
		    ((u32)application_id != ARCHIVE_DATABASE_APPLICATION_ID ||
		     schema_version != ARCHIVE_DATABASE_SCHEMA_VERSION)) {
			archive_database_set_error(error, error_capacity,
				"archive database schema is incompatible (application %08x, schema %d)",
				(u32)application_id, schema_version);
			status = ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA;
		}
	}
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA journal_mode = WAL; PRAGMA synchronous = FULL;",
	                 NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not configure archive durability: %s",
		                           sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	if (status == ARCHIVE_DATABASE_OK && application_id == 0 && schema_version == 0 &&
	    sqlite3_exec(database, archive_database_schema, NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not initialize archive database: %s",
		                           sqlite3_errmsg(database));
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_validate_open(database, error, error_capacity);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity, "could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

archive_database_status_t archive_database_validate(const archive_repository_t* repository,
		char* error, size_t error_capacity) {
	if (!repository || !repository->root[0]) return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path))) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}

	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                           path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_validate_open(database, error, error_capacity);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity, "could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

static archive_database_status_t archive_database_register_located_object(
		const archive_repository_t* repository, const char* sha256, u64 byte_size,
		const char* media_type, const char* storage_key,
		const char* external_path,
		archive_database_object_t* object, char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	bool has_storage_key = storage_key && storage_key[0];
	bool has_external_path = external_path && external_path[0];
	if (!repository || !repository->root[0] || !archive_database_sha256_is_valid(sha256) ||
	    !media_type || !media_type[0] || strlen(media_type) >= sizeof(object->media_type) ||
	    has_storage_key == has_external_path ||
	    (has_storage_key &&
	     strlen(storage_key) >= sizeof(object->storage_key)) ||
	    (has_external_path &&
	     strlen(external_path) >= sizeof(object->external_path)) ||
	    byte_size > INT64_MAX || !object) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path))) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}

	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                           path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_validate_open(database, error, error_capacity);
	}
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA synchronous = FULL;", NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity,
		                           "could not configure object transaction durability: %s",
		                           sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not begin object transaction: %s",
		                           sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}

	bool inserted = false;
	sqlite3_stmt* statement = NULL;
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"INSERT INTO object(sha256, byte_size, media_type, storage_key, external_path) "
			"VALUES(?1, ?2, ?3, ?4, ?5) ON CONFLICT(sha256) DO NOTHING;", -1,
			&statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, (sqlite3_int64)byte_size) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 3, media_type, -1, SQLITE_STATIC) != SQLITE_OK ||
		    (has_storage_key ?
		     sqlite3_bind_text(statement, 4, storage_key, -1,
		                       SQLITE_STATIC) :
		     sqlite3_bind_null(statement, 4)) != SQLITE_OK ||
		    (has_external_path ?
		     sqlite3_bind_text(statement, 5, external_path, -1,
		                       SQLITE_STATIC) :
		     sqlite3_bind_null(statement, 5)) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not register immutable object: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			inserted = sqlite3_changes(database) == 1;
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT id, sha256, byte_size, media_type, storage_key, external_path,"
			" inline_data IS NOT NULL "
			"FROM object WHERE sha256 = ?1;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not read immutable object record: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* stored_sha256 = (const char*)sqlite3_column_text(statement, 1);
			const char* stored_media_type = (const char*)sqlite3_column_text(statement, 3);
			const char* stored_storage_key = (const char*)sqlite3_column_text(statement, 4);
			const char* stored_external_path =
				(const char*)sqlite3_column_text(statement, 5);
			sqlite3_int64 stored_byte_size = sqlite3_column_int64(statement, 2);
			if (!stored_sha256 || !stored_media_type ||
			    sqlite3_column_int(statement, 6) ||
			    has_storage_key != (stored_storage_key != NULL) ||
			    has_external_path != (stored_external_path != NULL) ||
			    strcmp(stored_sha256, sha256) != 0 ||
			    stored_byte_size != (sqlite3_int64)byte_size ||
			    strcmp(stored_media_type, media_type) != 0 ||
			    (has_storage_key &&
			     strcmp(stored_storage_key, storage_key) != 0) ||
			    (has_external_path &&
			     strcmp(stored_external_path, external_path) != 0)) {
				archive_database_set_error(error, error_capacity,
					"immutable object digest is already registered with different metadata");
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				object->id = sqlite3_column_int64(statement, 0);
				memcpy(object->sha256, sha256, 65);
				object->byte_size = byte_size;
				memcpy(object->media_type, media_type, strlen(media_type) + 1);
				if (has_storage_key)
					memcpy(object->storage_key, storage_key,
					       strlen(storage_key) + 1);
				if (has_external_path)
					memcpy(object->external_path, external_path,
					       strlen(external_path) + 1);
				object->already_registered = !inserted;
			}
		}
		sqlite3_finalize(statement);
	}
	if (status == ARCHIVE_DATABASE_OK) {
		if (sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
			archive_database_set_error(error, error_capacity,
			                           "could not commit immutable object: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
	} else {
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity, "could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

archive_database_status_t archive_database_register_object(
		const archive_repository_t* repository, const char* sha256,
		u64 byte_size, const char* media_type, const char* storage_key,
		archive_database_object_t* object, char* error,
		size_t error_capacity) {
	return archive_database_register_located_object(
		repository, sha256, byte_size, media_type, storage_key,
		NULL, object, error, error_capacity);
}

archive_database_status_t archive_database_register_external_object(
		const archive_repository_t* repository, const char* sha256,
		u64 byte_size, const char* media_type, const char* external_path,
		archive_database_object_t* object, char* error,
		size_t error_capacity) {
	return archive_database_register_located_object(
		repository, sha256, byte_size, media_type, NULL,
		external_path, object, error, error_capacity);
}

archive_database_status_t archive_database_find_object(
		const archive_repository_t* repository, const char* sha256,
		archive_database_object_t* object, char* error,
		size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (!repository || !repository->root[0] ||
	    !archive_database_sha256_is_valid(sha256) || !object)
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path)))
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	sqlite3* database = NULL;
	int result = sqlite3_open_v2(
		path, &database,
		SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(
			error, error_capacity,
			"could not open archive database '%s': %s", path,
			database ? sqlite3_errmsg(database) :
				"SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_validate_open(
			database, error, error_capacity);
	sqlite3_stmt* statement = NULL;
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(
			database,
			"SELECT id,byte_size,media_type,"
			" COALESCE(storage_key,''),"
			" COALESCE(external_path,''),"
			" inline_data IS NOT NULL"
			" FROM object WHERE sha256=?1;",
			-1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(
			    statement, 1, sha256, -1,
			    SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(
				error, error_capacity,
				"could not read immutable object record: %s",
				sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* media_type =
				(const char*)sqlite3_column_text(statement, 2);
			const char* storage_key =
				(const char*)sqlite3_column_text(statement, 3);
			const char* external_path =
				(const char*)sqlite3_column_text(statement, 4);
			i64 byte_size = sqlite3_column_int64(statement, 1);
			bool has_storage_key =
				storage_key && storage_key[0];
			bool has_external_path =
				external_path && external_path[0];
			if (!media_type || !storage_key || !external_path ||
			    byte_size < 0 ||
			    has_storage_key == has_external_path ||
			    sqlite3_column_int(statement, 5) ||
			    strlen(media_type) >= sizeof(object->media_type) ||
			    strlen(storage_key) >= sizeof(object->storage_key) ||
			    strlen(external_path) >=
				    sizeof(object->external_path)) {
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				object->id = sqlite3_column_int64(statement, 0);
				memcpy(object->sha256, sha256, 65);
				object->byte_size = (u64)byte_size;
				snprintf(object->media_type,
				         sizeof(object->media_type), "%s",
				         media_type);
				snprintf(object->storage_key,
				         sizeof(object->storage_key), "%s",
				         storage_key);
				snprintf(object->external_path,
				         sizeof(object->external_path), "%s",
				         external_path);
				object->already_registered = true;
			}
		}
		sqlite3_finalize(statement);
	}
	if (sqlite3_close(database) != SQLITE_OK &&
	    status == ARCHIVE_DATABASE_OK)
		status = ARCHIVE_DATABASE_IO_ERROR;
	return status;
}

archive_database_status_t archive_database_register_inline_object(
		const archive_repository_t* repository, const char* sha256,
		const void* data, size_t size, const char* media_type,
		archive_database_object_t* object, char* error, size_t error_capacity) {
	if (object) memset(object, 0, sizeof(*object));
	if (!repository || !repository->root[0] ||
	    !archive_database_sha256_is_valid(sha256) || !data || !size ||
	    size > INT64_MAX || !media_type || !media_type[0] ||
	    !object || strlen(media_type) >= sizeof(object->media_type)) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path)))
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity,
			"could not open archive database '%s': %s", path,
			database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_validate_open(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA synchronous = FULL; BEGIN IMMEDIATE;",
	                 NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity,
			"could not begin inline object transaction: %s",
			sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}

	bool inserted = false;
	sqlite3_stmt* statement = NULL;
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"INSERT INTO object(sha256, byte_size, media_type, inline_data) "
			"VALUES(?1,?2,?3,?4) ON CONFLICT(sha256) DO NOTHING;",
			-1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, (sqlite3_int64)size) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 3, media_type, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_blob64(statement, 4, data, (sqlite3_uint64)size,
		                        SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
				"could not register inline object: %s", sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			inserted = sqlite3_changes(database) == 1;
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT id, byte_size, media_type, storage_key, external_path, inline_data "
			"FROM object WHERE sha256=?1;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
				"could not read inline object record: %s", sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* stored_media_type =
				(const char*)sqlite3_column_text(statement, 2);
			const void* stored_data = sqlite3_column_blob(statement, 5);
			i64 stored_size = sqlite3_column_int64(statement, 1);
			if (!stored_media_type || sqlite3_column_type(statement, 3) != SQLITE_NULL ||
			    sqlite3_column_type(statement, 4) != SQLITE_NULL ||
			    !stored_data || stored_size != (i64)size ||
			    sqlite3_column_bytes(statement, 5) != (i32)size ||
			    strcmp(stored_media_type, media_type) != 0 ||
			    memcmp(stored_data, data, size) != 0) {
				archive_database_set_error(error, error_capacity,
					"immutable object digest is already registered with different metadata");
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				object->id = sqlite3_column_int64(statement, 0);
				memcpy(object->sha256, sha256, 65);
				object->byte_size = size;
				memcpy(object->media_type, media_type, strlen(media_type) + 1);
				object->already_registered = !inserted;
			}
		}
		sqlite3_finalize(statement);
	}
	if (status == ARCHIVE_DATABASE_OK) {
		if (sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
			archive_database_set_error(error, error_capacity,
				"could not commit inline object: %s", sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
	} else {
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity,
			"could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

archive_database_status_t archive_database_read_inline_object(
		const archive_repository_t* repository, const char* sha256,
		u64 expected_size, const char* expected_media_type, size_t maximum_size,
		u8** data, size_t* size, char* error, size_t error_capacity) {
	if (data) *data = NULL;
	if (size) *size = 0;
	if (!repository || !repository->root[0] ||
	    !archive_database_sha256_is_valid(sha256) || !expected_size ||
	    expected_size > maximum_size || expected_size > SIZE_MAX ||
	    !expected_media_type || !expected_media_type[0] || !data || !size) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path)))
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity,
			"could not open archive database '%s': %s", path,
			database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_validate_open(database, error, error_capacity);
	sqlite3_stmt* statement = NULL;
	u8* result_data = NULL;
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT byte_size, media_type, storage_key, external_path, inline_data "
			"FROM object WHERE sha256=?1;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
				"could not read inline object: %s", sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			i64 stored_size = sqlite3_column_int64(statement, 0);
			const char* stored_media_type =
				(const char*)sqlite3_column_text(statement, 1);
			const void* stored_data = sqlite3_column_blob(statement, 4);
			if (stored_size != (i64)expected_size || !stored_media_type ||
			    strcmp(stored_media_type, expected_media_type) != 0 ||
			    sqlite3_column_type(statement, 2) != SQLITE_NULL ||
			    sqlite3_column_type(statement, 3) != SQLITE_NULL ||
			    !stored_data ||
			    sqlite3_column_bytes(statement, 4) != (i32)expected_size) {
				archive_database_set_error(error, error_capacity,
					"inline object metadata or size does not match its reference");
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				result_data = malloc((size_t)expected_size);
				if (!result_data) {
					archive_database_set_error(error, error_capacity,
						"out of memory while reading inline object");
					status = ARCHIVE_DATABASE_IO_ERROR;
				} else {
					memcpy(result_data, stored_data, (size_t)expected_size);
				}
			}
		}
		sqlite3_finalize(statement);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity,
			"could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	if (status != ARCHIVE_DATABASE_OK) {
		free(result_data);
		return status;
	}
	*data = result_data;
	*size = (size_t)expected_size;
	return ARCHIVE_DATABASE_OK;
}

archive_database_status_t archive_database_register_part10_bundle(
		const archive_repository_t* repository, const char* bundle_digest,
		i64 descriptor_object_id, const archive_database_part10_member_t* members,
		i32 member_count, archive_database_bundle_t* bundle,
		char* error, size_t error_capacity) {
	if (bundle) memset(bundle, 0, sizeof(*bundle));
	if (!repository || !repository->root[0] ||
	    !archive_database_sha256_is_valid(bundle_digest) ||
	    descriptor_object_id <= 0 || !members || member_count <= 0 || !bundle) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}
	for (i32 i = 0; i < member_count; ++i) {
		const archive_database_part10_member_t* member = members + i;
		if (member->object_id <= 0 || !member->logical_name[0] ||
		    !member->sop_instance_uid[0] || !member->sop_class_uid[0] ||
		    !member->study_instance_uid[0] || !member->series_instance_uid[0] ||
		    !member->transfer_syntax_uid[0] || member->frame_count <= 0 ||
		    member->rows <= 0 || member->columns <= 0 ||
		    member->total_pixel_matrix_rows <= 0 ||
		    member->total_pixel_matrix_columns <= 0) {
			return ARCHIVE_DATABASE_INVALID_ARGUMENT;
		}
	}
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path))) {
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	}
	sqlite3* database = NULL;
	int result = sqlite3_open_v2(path, &database,
		SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL);
	if (result != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                           path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK) {
		status = archive_database_validate_open(database, error, error_capacity);
	}
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA synchronous = FULL; BEGIN IMMEDIATE;",
	                 NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not begin bundle transaction: %s",
		                           sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}

	bool inserted = false;
	sqlite3_stmt* statement = NULL;
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT sha256 FROM object WHERE id = ?1;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, descriptor_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not read Part 10 bundle descriptor object: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* descriptor_digest = (const char*)sqlite3_column_text(statement, 0);
			if (!descriptor_digest || strcmp(descriptor_digest, bundle_digest) != 0) {
				archive_database_set_error(error, error_capacity,
				                           "bundle digest does not match its descriptor object");
				status = ARCHIVE_DATABASE_CONFLICT;
			}
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"INSERT INTO bundle(bundle_digest, source_format, adapter_name, adapter_version, "
			"descriptor_object_id, state) VALUES(?1, 'dicom', 'slidescape-dicom', '1', ?2, 'PUBLISHED') "
			"ON CONFLICT(bundle_digest) DO NOTHING;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, bundle_digest, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, descriptor_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not register Part 10 bundle: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			inserted = sqlite3_changes(database) == 1;
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT id, descriptor_object_id, source_format, adapter_name, adapter_version "
			"FROM bundle WHERE bundle_digest = ?1;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, bundle_digest, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not read Part 10 bundle: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* source_format = (const char*)sqlite3_column_text(statement, 2);
			const char* adapter_name = (const char*)sqlite3_column_text(statement, 3);
			const char* adapter_version = (const char*)sqlite3_column_text(statement, 4);
			if (sqlite3_column_int64(statement, 1) != descriptor_object_id ||
			    !source_format || strcmp(source_format, "dicom") != 0 ||
			    !adapter_name || strcmp(adapter_name, "slidescape-dicom") != 0 ||
			    !adapter_version || strcmp(adapter_version, "1") != 0) {
				archive_database_set_error(error, error_capacity,
				                           "bundle digest is registered with different metadata");
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				bundle->id = sqlite3_column_int64(statement, 0);
				bundle->descriptor_object_id = descriptor_object_id;
				memcpy(bundle->bundle_digest, bundle_digest, 65);
				bundle->already_registered = !inserted;
			}
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}

	for (i32 i = 0; i < member_count && status == ARCHIVE_DATABASE_OK; ++i) {
		const archive_database_part10_member_t* member = members + i;
		result = sqlite3_prepare_v2(database,
			"INSERT INTO part10_instance(sop_instance_uid, object_id, sop_class_uid, "
			"study_instance_uid, series_instance_uid, transfer_syntax_uid, modality, study_date, "
			"instance_number, frame_count, rows, columns, total_pixel_matrix_rows, "
			"total_pixel_matrix_columns, image_flavor) "
			"VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15) "
			"ON CONFLICT(sop_instance_uid) DO NOTHING;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, member->sop_instance_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, member->object_id) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 3, member->sop_class_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 4, member->study_instance_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 5, member->series_instance_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 6, member->transfer_syntax_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 7, member->modality, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 8, member->study_date, -1, SQLITE_STATIC) != SQLITE_OK ||
		    (member->has_instance_number
			    ? sqlite3_bind_int64(statement, 9, member->instance_number)
			    : sqlite3_bind_null(statement, 9)) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 10, member->frame_count) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 11, member->rows) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 12, member->columns) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 13, member->total_pixel_matrix_rows) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 14, member->total_pixel_matrix_columns) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 15, member->image_flavor, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not register SOP Instance UID '%s': %s",
			                           member->sop_instance_uid, sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		result = sqlite3_prepare_v2(database,
			"SELECT object_id, sop_class_uid, study_instance_uid, series_instance_uid, "
			"transfer_syntax_uid, modality, study_date, instance_number, frame_count, rows, columns, "
			"total_pixel_matrix_rows, total_pixel_matrix_columns, image_flavor "
			"FROM part10_instance WHERE sop_instance_uid = ?1;",
			-1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, member->sop_instance_uid, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not verify SOP Instance UID '%s': %s",
			                           member->sop_instance_uid, sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* sop_class_uid = (const char*)sqlite3_column_text(statement, 1);
			const char* study_instance_uid = (const char*)sqlite3_column_text(statement, 2);
			const char* series_instance_uid = (const char*)sqlite3_column_text(statement, 3);
			const char* transfer_syntax_uid = (const char*)sqlite3_column_text(statement, 4);
			const char* modality = (const char*)sqlite3_column_text(statement, 5);
			const char* study_date = (const char*)sqlite3_column_text(statement, 6);
			const char* image_flavor = (const char*)sqlite3_column_text(statement, 13);
			if (sqlite3_column_int64(statement, 0) != member->object_id ||
			    !sop_class_uid || strcmp(sop_class_uid, member->sop_class_uid) != 0 ||
			    !study_instance_uid || strcmp(study_instance_uid, member->study_instance_uid) != 0 ||
			    !series_instance_uid || strcmp(series_instance_uid, member->series_instance_uid) != 0 ||
			    !transfer_syntax_uid || strcmp(transfer_syntax_uid, member->transfer_syntax_uid) != 0 ||
			    !modality || strcmp(modality, member->modality) != 0 ||
			    !study_date || strcmp(study_date, member->study_date) != 0 ||
			    (sqlite3_column_type(statement, 7) == SQLITE_NULL) != !member->has_instance_number ||
			    (member->has_instance_number &&
			     sqlite3_column_int64(statement, 7) != member->instance_number) ||
			    sqlite3_column_int64(statement, 8) != member->frame_count ||
			    sqlite3_column_int64(statement, 9) != member->rows ||
			    sqlite3_column_int64(statement, 10) != member->columns ||
			    sqlite3_column_int64(statement, 11) != member->total_pixel_matrix_rows ||
			    sqlite3_column_int64(statement, 12) != member->total_pixel_matrix_columns ||
			    !image_flavor || strcmp(image_flavor, member->image_flavor) != 0) {
				archive_database_set_error(error, error_capacity,
					"SOP Instance UID '%s' is already registered with different content or metadata",
					member->sop_instance_uid);
				status = ARCHIVE_DATABASE_CONFLICT;
			}
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		result = sqlite3_prepare_v2(database,
			"INSERT INTO bundle_object(bundle_id, object_id, logical_name, role, ordinal) "
			"VALUES(?1, ?2, ?3, 'part10-instance', ?4) "
			"ON CONFLICT(bundle_id, logical_name) DO NOTHING;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, member->object_id) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 3, member->logical_name, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int(statement, 4, i) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not attach object to Part 10 bundle: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		result = sqlite3_prepare_v2(database,
			"SELECT object_id, role, ordinal FROM bundle_object "
			"WHERE bundle_id = ?1 AND logical_name = ?2;", -1, &statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 2, member->logical_name, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not verify Part 10 bundle member: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* role = (const char*)sqlite3_column_text(statement, 1);
			if (sqlite3_column_int64(statement, 0) != member->object_id ||
			    !role || strcmp(role, "part10-instance") != 0 ||
			    sqlite3_column_int(statement, 2) != i) {
				archive_database_set_error(error, error_capacity,
				                           "Part 10 bundle member '%s' conflicts with existing metadata",
				                           member->logical_name);
				status = ARCHIVE_DATABASE_CONFLICT;
			}
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}

	if (status == ARCHIVE_DATABASE_OK) {
		result = sqlite3_prepare_v2(database,
			"SELECT count(*) FROM bundle_object WHERE bundle_id = ?1;", -1,
			&statement, NULL);
		if (result != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not verify Part 10 bundle membership: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else if (sqlite3_column_int(statement, 0) != member_count) {
			archive_database_set_error(error, error_capacity,
			                           "Part 10 bundle membership conflicts with an existing bundle");
			status = ARCHIVE_DATABASE_CONFLICT;
		}
		sqlite3_finalize(statement);
	}
	if (status == ARCHIVE_DATABASE_OK) {
		if (sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
			archive_database_set_error(error, error_capacity,
			                           "could not commit Part 10 bundle: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
	} else {
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity, "could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

archive_database_status_t archive_database_register_mrxs_bundle(
		const archive_repository_t* repository, const char* bundle_digest,
		i64 descriptor_object_id, archive_database_bundle_t* bundle,
		char* error, size_t error_capacity) {
	if (bundle) memset(bundle, 0, sizeof(*bundle));
	if (!repository || !repository->root[0] ||
	    !archive_database_sha256_is_valid(bundle_digest) ||
	    descriptor_object_id <= 0 || !bundle)
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path)))
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	sqlite3* database = NULL;
	sqlite3_stmt* statement = NULL;
	archive_database_status_t status = ARCHIVE_DATABASE_OK;
	if (sqlite3_open_v2(path, &database,
			SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
		status = ARCHIVE_DATABASE_IO_ERROR;
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_validate_open(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA synchronous=FULL; BEGIN IMMEDIATE;",
	                 NULL, NULL, NULL) != SQLITE_OK)
		status = ARCHIVE_DATABASE_IO_ERROR;
	bool inserted = false;
	if (status == ARCHIVE_DATABASE_OK) {
		const char* sql =
			"INSERT INTO bundle(bundle_digest,source_format,adapter_name,"
			"adapter_version,descriptor_object_id,state)"
			" SELECT ?1,'mrxs','slidescape-mrxs','2',id,'PUBLISHED'"
			" FROM object WHERE id=?2 AND sha256=?1"
			" ON CONFLICT(bundle_digest) DO NOTHING;";
		if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, bundle_digest, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, descriptor_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE)
			status = ARCHIVE_DATABASE_IO_ERROR;
		else
			inserted = sqlite3_changes(database) == 1;
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		const char* sql =
			"SELECT id,descriptor_object_id,source_format,adapter_name,"
			"adapter_version FROM bundle WHERE bundle_digest=?1;";
		if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, bundle_digest, -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* format = (const char*)sqlite3_column_text(statement, 2);
			const char* adapter = (const char*)sqlite3_column_text(statement, 3);
			const char* version = (const char*)sqlite3_column_text(statement, 4);
			if (sqlite3_column_int64(statement, 1) != descriptor_object_id ||
			    !format || strcmp(format, "mrxs") != 0 ||
			    !adapter || strcmp(adapter, "slidescape-mrxs") != 0 ||
			    !version || strcmp(version, "2") != 0) {
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				bundle->id = sqlite3_column_int64(statement, 0);
				bundle->descriptor_object_id = descriptor_object_id;
				memcpy(bundle->bundle_digest, bundle_digest, 65);
				bundle->already_registered = !inserted;
			}
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		const char* sql =
			"INSERT INTO bundle_object(bundle_id,object_id,logical_name,role,ordinal)"
			" VALUES(?1,?2,'bundle.mrxs.manifest','bundle-descriptor',0)"
			" ON CONFLICT(bundle_id,logical_name) DO NOTHING;";
		if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, descriptor_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE)
			status = ARCHIVE_DATABASE_IO_ERROR;
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		const char* sql =
			"SELECT object_id,role,ordinal FROM bundle_object "
			"WHERE bundle_id=?1 AND logical_name='bundle.mrxs.manifest';";
		if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* role = (const char*)sqlite3_column_text(statement, 1);
			if (sqlite3_column_int64(statement, 0) != descriptor_object_id ||
			    !role || strcmp(role, "bundle-descriptor") != 0 ||
			    sqlite3_column_int(statement, 2) != 0)
				status = ARCHIVE_DATABASE_CONFLICT;
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK) {
		const char* sql =
			"SELECT count(*) FROM bundle_object WHERE bundle_id=?1;";
		if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle->id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else if (sqlite3_column_int(statement, 0) != 1) {
			status = ARCHIVE_DATABASE_CONFLICT;
		}
		sqlite3_finalize(statement);
		statement = NULL;
	}
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
		status = ARCHIVE_DATABASE_IO_ERROR;
	if (status != ARCHIVE_DATABASE_OK)
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
	if (status != ARCHIVE_DATABASE_OK && (!error || !error[0]))
		archive_database_set_error(error, error_capacity,
		                           "could not register MRXS bundle");
	sqlite3_finalize(statement);
	sqlite3_close(database);
	return status;
}

static const char* archive_database_frame_media_type(const char* transfer_syntax_uid) {
	if (strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.51") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.57") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.70") == 0) return "image/jpeg";
	if (strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.90") == 0 ||
	    strcmp(transfer_syntax_uid, "1.2.840.10008.1.2.4.91") == 0) return "image/jp2";
	return "application/octet-stream";
}

archive_database_status_t archive_database_publish_shells(
		const archive_repository_t* repository, i64 bundle_id,
		const archive_database_shell_publication_t* publications,
		i32 publication_count, char* error, size_t error_capacity) {
	if (!repository || !repository->root[0] || bundle_id <= 0 ||
	    !publications || publication_count <= 0) return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	for (i32 i = 0; i < publication_count; ++i) {
		const archive_database_shell_publication_t* publication = publications + i;
		if (!publication->representation_id[0] ||
		    publication->shell_object_id <= 0 ||
		    publication->recipe_object_id <= 0 ||
		    !archive_database_sha256_is_valid(
			    publication->representation_fingerprint) ||
		    !publication->instances || publication->instance_count <= 0) {
			return ARCHIVE_DATABASE_INVALID_ARGUMENT;
		}
		const char* provider_kind = publication->provider_kind[0] ?
			publication->provider_kind : "part10";
		const char* adapter_version = publication->adapter_version[0] ?
			publication->adapter_version : "7";
		const char* frame_source = publication->frame_source[0] ?
			publication->frame_source : "STORED";
		const char* frame_policy_version =
			publication->frame_policy_version[0] ?
			publication->frame_policy_version :
				"part10-cached-delta-payload-v9";
		if ((strcmp(provider_kind, "part10") != 0 &&
		     strcmp(provider_kind, "mrxs-native") != 0) ||
		    !adapter_version[0] ||
		    (strcmp(frame_source, "STORED") != 0 &&
		     strcmp(frame_source, "GENERATED") != 0) ||
		    !frame_policy_version[0])
			return ARCHIVE_DATABASE_INVALID_ARGUMENT;
		for (i32 j = 0; j < publication->instance_count; ++j) {
			const archive_database_shell_instance_publication_t* shell_instance =
				publication->instances + j;
			const archive_database_part10_member_t* instance =
				&shell_instance->instance;
			if (instance->object_id <= 0 ||
			    !instance->study_instance_uid[0] ||
			    !instance->series_instance_uid[0] ||
			    !instance->sop_instance_uid[0] ||
			    !instance->sop_class_uid[0] ||
			    !instance->transfer_syntax_uid[0] ||
			    shell_instance->shell_instance_index != (u32)j) {
				return ARCHIVE_DATABASE_INVALID_ARGUMENT;
			}
		}
	}

	char path[ARCHIVE_PATH_CAPACITY];
	if (!archive_database_path(repository, path, sizeof(path)))
		return ARCHIVE_DATABASE_INVALID_ARGUMENT;
	sqlite3* database = NULL;
	if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) !=
	    SQLITE_OK) {
		archive_database_set_error(error, error_capacity, "could not open archive database '%s': %s",
		                           path, database ? sqlite3_errmsg(database) : "SQLite open failed");
		sqlite3_close(database);
		return ARCHIVE_DATABASE_IO_ERROR;
	}
	archive_database_status_t status =
		archive_database_configure(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK)
		status = archive_database_validate_open(database, error, error_capacity);
	if (status == ARCHIVE_DATABASE_OK &&
	    sqlite3_exec(database, "PRAGMA synchronous = FULL; BEGIN IMMEDIATE;",
	                 NULL, NULL, NULL) != SQLITE_OK) {
		archive_database_set_error(error, error_capacity,
		                           "could not begin shell publication: %s",
		                           sqlite3_errmsg(database));
		status = ARCHIVE_DATABASE_IO_ERROR;
	}

	for (i32 i = 0; i < publication_count && status == ARCHIVE_DATABASE_OK; ++i) {
		const archive_database_shell_publication_t* publication = publications + i;
		sqlite3_stmt* statement = NULL;
		i64 metadata_version_id = 0;
		i64 representation_id = 0;
		bool representation_inserted = false;

		const char* validate_sql =
			"SELECT "
			" EXISTS(SELECT 1 FROM bundle WHERE id=?1 AND state='PUBLISHED'),"
			" EXISTS(SELECT 1 FROM object WHERE id=?2 "
			"   AND media_type='application/vnd.slidescape.virtual-dicom-shell' "
			"   AND storage_key GLOB 'shells/sha256/*'),"
			" EXISTS(SELECT 1 FROM object WHERE id=?3 "
			"   AND media_type IN ("
			"     'application/vnd.slidescape.virtual-dicom-recipe',"
			"     'application/vnd.slidescape.mrxs-native-recipe'));";
		if (sqlite3_prepare_v2(database, validate_sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 1, bundle_id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, publication->shell_object_id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 3, publication->recipe_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			archive_database_set_error(error, error_capacity,
			                           "could not validate shell publication objects: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else if (!sqlite3_column_int(statement, 0) ||
		           !sqlite3_column_int(statement, 1) ||
		           !sqlite3_column_int(statement, 2)) {
			archive_database_set_error(error, error_capacity,
			                           "shell publication references invalid bundle or object metadata");
			status = ARCHIVE_DATABASE_CONFLICT;
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		const char* metadata_sql =
			"INSERT INTO metadata_version(fingerprint, schema_version, "
			"canonical_metadata_object_id, provenance_object_id) "
			"VALUES(?1, 1, ?2, ?3) ON CONFLICT(fingerprint) DO NOTHING;";
		if (sqlite3_prepare_v2(database, metadata_sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, publication->representation_fingerprint,
		                      -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 2, publication->shell_object_id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 3, publication->recipe_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not register shell metadata version: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		const char* metadata_select =
			"SELECT id, canonical_metadata_object_id, provenance_object_id "
			"FROM metadata_version WHERE fingerprint=?1;";
		if (sqlite3_prepare_v2(database, metadata_select, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, publication->representation_fingerprint,
		                      -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else if (sqlite3_column_int64(statement, 1) != publication->shell_object_id ||
		           sqlite3_column_int64(statement, 2) != publication->recipe_object_id) {
			status = ARCHIVE_DATABASE_CONFLICT;
		} else {
			metadata_version_id = sqlite3_column_int64(statement, 0);
		}
		if (status != ARCHIVE_DATABASE_OK)
			archive_database_set_error(error, error_capacity,
			                           "representation fingerprint has conflicting provenance");
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		const char* representation_sql =
			"INSERT INTO representation(representation_id, version, fingerprint, bundle_id, "
			"metadata_version_id, shell_object_id, state) "
			"VALUES(?1, 1, ?2, ?3, ?4, ?5, 'STAGED') "
			"ON CONFLICT DO NOTHING;";
		if (sqlite3_prepare_v2(database, representation_sql, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, publication->representation_id,
		                      -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 2, publication->representation_fingerprint,
		                      -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 3, bundle_id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 4, metadata_version_id) != SQLITE_OK ||
		    sqlite3_bind_int64(statement, 5, publication->shell_object_id) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_DONE) {
			archive_database_set_error(error, error_capacity,
			                           "could not stage shell representation: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			representation_inserted = sqlite3_changes(database) == 1;
		}
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		const char* representation_select =
			"SELECT id, representation_id, version, bundle_id, metadata_version_id, "
			"shell_object_id, fingerprint FROM representation "
			"WHERE representation_id=?1 AND version=1;";
		if (sqlite3_prepare_v2(database, representation_select, -1, &statement, NULL) != SQLITE_OK ||
		    sqlite3_bind_text(statement, 1, publication->representation_id,
		                      -1, SQLITE_STATIC) != SQLITE_OK ||
		    sqlite3_step(statement) != SQLITE_ROW) {
			status = ARCHIVE_DATABASE_IO_ERROR;
		} else {
			const char* logical_id = (const char*)sqlite3_column_text(statement, 1);
			const char* fingerprint =
				(const char*)sqlite3_column_text(statement, 6);
			if (!logical_id ||
			    strcmp(logical_id, publication->representation_id) != 0 ||
			    sqlite3_column_int(statement, 2) != 1 ||
			    sqlite3_column_int64(statement, 3) != bundle_id ||
			    sqlite3_column_int64(statement, 4) != metadata_version_id ||
			    sqlite3_column_int64(statement, 5) != publication->shell_object_id ||
			    !fingerprint ||
			    strcmp(fingerprint,
			           publication->representation_fingerprint) != 0) {
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				representation_id = sqlite3_column_int64(statement, 0);
			}
		}
		if (status != ARCHIVE_DATABASE_OK)
			archive_database_set_error(error, error_capacity,
			                           "shell representation conflicts with existing publication");
		sqlite3_finalize(statement);
		statement = NULL;
		if (status != ARCHIVE_DATABASE_OK) break;

		for (i32 j = 0;
		     j < publication->instance_count &&
		     status == ARCHIVE_DATABASE_OK; ++j) {
			const archive_database_shell_instance_publication_t* shell_instance =
				publication->instances + j;
			const archive_database_part10_member_t* instance =
				&shell_instance->instance;
			i64 instance_id = 0;

			const char* member_sql =
				"SELECT EXISTS(SELECT 1 FROM bundle_object "
				"WHERE bundle_id=?1 AND object_id=?2);";
			if (sqlite3_prepare_v2(database, member_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, bundle_id) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 2, instance->object_id) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_ROW) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			} else if (!sqlite3_column_int(statement, 0)) {
				status = ARCHIVE_DATABASE_CONFLICT;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* study_sql =
				"INSERT INTO study(representation_id, study_instance_uid, study_date) "
				"VALUES(?1,?2,?3) ON CONFLICT DO NOTHING;";
			if (sqlite3_prepare_v2(database, study_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, instance->study_instance_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 3, instance->study_date,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* series_sql =
				"INSERT INTO series(representation_id, study_instance_uid, "
				"series_instance_uid, modality) VALUES(?1,?2,?3,?4) "
				"ON CONFLICT DO NOTHING;";
			if (sqlite3_prepare_v2(database, series_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, instance->study_instance_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 3, instance->series_instance_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 4, instance->modality,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* instance_sql =
				"INSERT INTO instance(representation_id, study_instance_uid, "
				"series_instance_uid, sop_instance_uid, sop_class_uid, "
				"instance_number, image_flavor, frame_count, rows, columns, "
				"total_pixel_matrix_rows, total_pixel_matrix_columns, "
				"metadata_object_id, shell_instance_index) "
				"VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14) "
				"ON CONFLICT DO NOTHING;";
			bool instance_failed =
				sqlite3_prepare_v2(database, instance_sql, -1,
				                   &statement, NULL) != SQLITE_OK ||
				sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
				sqlite3_bind_text(statement, 2, instance->study_instance_uid,
				                  -1, SQLITE_STATIC) != SQLITE_OK ||
				sqlite3_bind_text(statement, 3, instance->series_instance_uid,
				                  -1, SQLITE_STATIC) != SQLITE_OK ||
				sqlite3_bind_text(statement, 4, instance->sop_instance_uid,
				                  -1, SQLITE_STATIC) != SQLITE_OK ||
				sqlite3_bind_text(statement, 5, instance->sop_class_uid,
				                  -1, SQLITE_STATIC) != SQLITE_OK;
			if (!instance_failed) {
				instance_failed =
					(instance->has_instance_number
						? sqlite3_bind_int64(statement, 6,
						                    instance->instance_number)
						: sqlite3_bind_null(statement, 6)) != SQLITE_OK ||
					sqlite3_bind_text(statement, 7, instance->image_flavor,
					                  -1, SQLITE_STATIC) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 8, instance->frame_count) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 9, instance->rows) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 10, instance->columns) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 11,
					                   instance->total_pixel_matrix_rows) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 12,
					                   instance->total_pixel_matrix_columns) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 13,
					                   publication->shell_object_id) != SQLITE_OK ||
					sqlite3_bind_int64(statement, 14,
					                   shell_instance->shell_instance_index) != SQLITE_OK ||
					sqlite3_step(statement) != SQLITE_DONE;
			}
			if (instance_failed) status = ARCHIVE_DATABASE_IO_ERROR;
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* instance_select =
				"SELECT id, shell_instance_index, metadata_object_id "
				"FROM instance WHERE representation_id=?1 AND sop_instance_uid=?2;";
			if (sqlite3_prepare_v2(database, instance_select, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, instance->sop_instance_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_ROW) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			} else if (sqlite3_column_int64(statement, 1) !=
			               shell_instance->shell_instance_index ||
			           sqlite3_column_int64(statement, 2) !=
			               publication->shell_object_id) {
				status = ARCHIVE_DATABASE_CONFLICT;
			} else {
				instance_id = sqlite3_column_int64(statement, 0);
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* provider_kind = publication->provider_kind[0] ?
				publication->provider_kind : "part10";
			const char* adapter_version = publication->adapter_version[0] ?
				publication->adapter_version : "7";
			const char* frame_media_type = publication->frame_media_type[0] ?
				publication->frame_media_type :
				archive_database_frame_media_type(instance->transfer_syntax_uid);
			const char* frame_source = publication->frame_source[0] ?
				publication->frame_source : "STORED";
			const char* frame_policy_version =
				publication->frame_policy_version[0] ?
				publication->frame_policy_version :
					"part10-cached-delta-payload-v9";
			const char* provider_sql =
				"INSERT INTO instance_provider(instance_id, provider_kind, "
				"source_object_id, parameters_object_id, adapter_version) "
				"VALUES(?1,?2,?3,?4,?5) ON CONFLICT DO NOTHING;";
			if (sqlite3_prepare_v2(database, provider_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, instance_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, provider_kind,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 3, instance->object_id) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 4,
			                       publication->recipe_object_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 5, adapter_version,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;
			const char* provider_select =
				"SELECT provider_kind,source_object_id,parameters_object_id,"
				"adapter_version FROM instance_provider WHERE instance_id=?1;";
			if (sqlite3_prepare_v2(database, provider_select, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, instance_id) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_ROW) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			} else {
				const char* retained_kind =
					(const char*)sqlite3_column_text(statement, 0);
				const char* retained_version =
					(const char*)sqlite3_column_text(statement, 3);
				if (!retained_kind ||
				    strcmp(retained_kind, provider_kind) != 0 ||
				    sqlite3_column_int64(statement, 1) != instance->object_id ||
				    sqlite3_column_int64(statement, 2) !=
					    publication->recipe_object_id ||
				    !retained_version ||
				    strcmp(retained_version, adapter_version) != 0)
					status = ARCHIVE_DATABASE_CONFLICT;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;

			const char* frame_sql =
				"INSERT INTO frame_representation(instance_id, media_type, "
				"transfer_syntax_uid, source, source_object_id, policy_version, "
				"preference) VALUES(?1,?2,?3,?4,?5,?6,100) "
				"ON CONFLICT DO NOTHING;";
			if (sqlite3_prepare_v2(database, frame_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, instance_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, frame_media_type,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 3, instance->transfer_syntax_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 4, frame_source,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 5, instance->object_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 6, frame_policy_version,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status != ARCHIVE_DATABASE_OK) break;
			const char* frame_select =
				"SELECT source,source_object_id,policy_version,preference "
				"FROM frame_representation WHERE instance_id=?1 "
				"AND media_type=?2 AND transfer_syntax_uid=?3;";
			if (sqlite3_prepare_v2(database, frame_select, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, instance_id) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 2, frame_media_type,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_bind_text(statement, 3, instance->transfer_syntax_uid,
			                      -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_ROW) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			} else {
				const char* retained_source =
					(const char*)sqlite3_column_text(statement, 0);
				const char* retained_policy =
					(const char*)sqlite3_column_text(statement, 2);
				if (!retained_source ||
				    strcmp(retained_source, frame_source) != 0 ||
				    sqlite3_column_int64(statement, 1) != instance->object_id ||
				    !retained_policy ||
				    strcmp(retained_policy, frame_policy_version) != 0 ||
				    sqlite3_column_int(statement, 3) != 100)
					status = ARCHIVE_DATABASE_CONFLICT;
			}
			sqlite3_finalize(statement);
			statement = NULL;
		}

		i64 publication_id = 0;
		if (representation_inserted && status == ARCHIVE_DATABASE_OK) {
			const char* publication_sql =
				"INSERT INTO publication(representation_id, actor, policy_version) "
				"VALUES(?1,'filesystem-ingest',?2);";
			if (sqlite3_prepare_v2(database, publication_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
			    sqlite3_bind_text(
				    statement, 2,
				    publication->frame_policy_version[0] ?
				    publication->frame_policy_version :
				    "part10-cached-delta-payload-v9",
				    -1, SQLITE_STATIC) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			} else {
				publication_id = sqlite3_last_insert_rowid(database);
			}
			sqlite3_finalize(statement);
			statement = NULL;
			if (status == ARCHIVE_DATABASE_OK) {
				const char* outbox_sql =
					"INSERT INTO outbox(publication_id, operation) "
					"VALUES(?1,'SHELL_PUBLISHED');";
				if (sqlite3_prepare_v2(database, outbox_sql, -1,
				                       &statement, NULL) != SQLITE_OK ||
				    sqlite3_bind_int64(statement, 1, publication_id) != SQLITE_OK ||
				    sqlite3_step(statement) != SQLITE_DONE) {
					status = ARCHIVE_DATABASE_IO_ERROR;
				}
				sqlite3_finalize(statement);
				statement = NULL;
			}
		}

		if (status == ARCHIVE_DATABASE_OK) {
			const char* publish_sql =
				"UPDATE representation SET state='PUBLISHED' "
				"WHERE id=?1 AND state IN ('STAGED','PUBLISHED');";
			if (sqlite3_prepare_v2(database, publish_sql, -1,
			                       &statement, NULL) != SQLITE_OK ||
			    sqlite3_bind_int64(statement, 1, representation_id) != SQLITE_OK ||
			    sqlite3_step(statement) != SQLITE_DONE ||
			    sqlite3_changes(database) != 1) {
				status = ARCHIVE_DATABASE_IO_ERROR;
			}
			sqlite3_finalize(statement);
			statement = NULL;
		}
		if (status != ARCHIVE_DATABASE_OK)
			archive_database_set_error(
				error, error_capacity,
				"could not publish aggregate virtual DICOM shell: %s",
				sqlite3_errmsg(database));
	}

	if (status == ARCHIVE_DATABASE_OK) {
		if (sqlite3_exec(database, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
			archive_database_set_error(error, error_capacity,
			                           "could not commit shell publications: %s",
			                           sqlite3_errmsg(database));
			status = ARCHIVE_DATABASE_IO_ERROR;
		}
	} else {
		sqlite3_exec(database, "ROLLBACK;", NULL, NULL, NULL);
	}
	if (sqlite3_close(database) != SQLITE_OK && status == ARCHIVE_DATABASE_OK) {
		archive_database_set_error(error, error_capacity, "could not close archive database");
		status = ARCHIVE_DATABASE_IO_ERROR;
	}
	return status;
}

archive_database_status_t archive_database_publish_part10_shells(
		const archive_repository_t* repository, i64 bundle_id,
		const archive_database_shell_publication_t* publications,
		i32 publication_count, char* error, size_t error_capacity) {
	return archive_database_publish_shells(
		repository, bundle_id, publications, publication_count,
		error, error_capacity);
}

const char* archive_database_status_string(archive_database_status_t status) {
	switch (status) {
		case ARCHIVE_DATABASE_OK: return "ok";
		case ARCHIVE_DATABASE_INVALID_ARGUMENT: return "invalid argument";
		case ARCHIVE_DATABASE_IO_ERROR: return "I/O error";
		case ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA: return "incompatible schema";
		case ARCHIVE_DATABASE_CONFLICT: return "conflict";
		default: return "unknown database error";
	}
}
