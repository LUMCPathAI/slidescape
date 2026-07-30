#include "doctest.h"

#include "archive_database.h"
#include "archive_object.h"
#include "archive_repository.h"
#include "archive_routes.h"
#include "archive_shell.h"
#include "archive_shell_recipe.h"
#include "archive_index.h"
#include "archive_ingest.h"
#include "archive_frame_map.h"
#include "archive_manifest.h"
#include "archive_representation.h"
#include "dicom.h"
#include "dicomweb.h"
#include "sqlite3.h"
#include "test_temp.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <stdlib.h>
#define archive_test_rmdir _rmdir
#else
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define archive_test_rmdir rmdir
#endif

static void archive_test_remove_database(const char* root) {
	char path[ARCHIVE_PATH_CAPACITY];
	const char* suffixes[] = {"", "-wal", "-shm"};
	for (const char* suffix : suffixes) {
		snprintf(path, sizeof(path), "%s/%s%s", root, ARCHIVE_DATABASE_FILENAME, suffix);
		remove(path);
	}
}

TEST_CASE("pathology archive selects a frame representation from Accept") {
	archive_frame_representation_t available[3] = {};
	snprintf(available[0].media_type, sizeof(available[0].media_type), "image/jp2");
	snprintf(available[0].transfer_syntax_uid,
	         sizeof(available[0].transfer_syntax_uid), "1.2.840.10008.1.2.4.90");
	available[0].source = ARCHIVE_FRAME_REPRESENTATION_STORED;
	available[0].preference = 100;
	snprintf(available[1].media_type, sizeof(available[1].media_type), "image/jpeg");
	snprintf(available[1].transfer_syntax_uid,
	         sizeof(available[1].transfer_syntax_uid), "1.2.840.10008.1.2.4.50");
	available[1].source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
	available[1].preference = 50;
	snprintf(available[2].media_type, sizeof(available[2].media_type),
	         "application/octet-stream");
	snprintf(available[2].transfer_syntax_uid,
	         sizeof(available[2].transfer_syntax_uid), "1.2.840.10008.1.2.1");
	available[2].source = ARCHIVE_FRAME_REPRESENTATION_GENERATED;
	available[2].preference = 200;

	archive_frame_representation_t selected = {};
	CHECK(archive_select_frame_representation(
		nullptr, 0, available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_MISSING_ACCEPT);

	const char* jpeg =
		"multipart/related; type=\"image/jpeg\"; "
		"transfer-syntax=1.2.840.10008.1.2.4.50";
	REQUIRE(archive_select_frame_representation(
		jpeg, strlen(jpeg), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_SELECTED);
	CHECK(strcmp(selected.media_type, "image/jpeg") == 0);
	CHECK(selected.source == ARCHIVE_FRAME_REPRESENTATION_GENERATED);

	const char* ranked =
		"multipart/related; type=\"image/jpeg\"; "
		"transfer-syntax=1.2.840.10008.1.2.4.50; q=0.4, "
		"multipart/related; type=\"image/jp2\"; "
		"transfer-syntax=1.2.840.10008.1.2.4.90; q=0.9";
	REQUIRE(archive_select_frame_representation(
		ranked, strlen(ranked), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_SELECTED);
	CHECK(strcmp(selected.media_type, "image/jp2") == 0);

	const char* wildcard =
		"multipart/related; type=\"*/*\"; transfer-syntax=*";
	REQUIRE(archive_select_frame_representation(
		wildcard, strlen(wildcard), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_SELECTED);
	CHECK(strcmp(selected.media_type, "application/octet-stream") == 0);

	const char* specificity =
		"multipart/related; type=\"image/*\"; q=0.8, "
		"multipart/related; type=\"image/jp2\"; q=0.8";
	REQUIRE(archive_select_frame_representation(
		specificity, strlen(specificity), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_SELECTED);
	CHECK(strcmp(selected.media_type, "image/jp2") == 0);

	const char* unacceptable =
		"multipart/related; type=\"image/jpeg\"; q=0, "
		"multipart/related; type=\"image/jls\"";
	CHECK(archive_select_frame_representation(
		unacceptable, strlen(unacceptable), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_NOT_ACCEPTABLE);

	const char* excluded_by_specific_range =
		"multipart/related; type=\"image/*\"; q=0.9, "
		"multipart/related; type=\"image/jp2\"; q=0";
	REQUIRE(archive_select_frame_representation(
		excluded_by_specific_range, strlen(excluded_by_specific_range),
		available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_SELECTED);
	CHECK(strcmp(selected.media_type, "image/jpeg") == 0);

	const char* malformed =
		"multipart/related; transfer-syntax=1.2.840.10008.1.2.4.90";
	CHECK(archive_select_frame_representation(
		malformed, strlen(malformed), available, COUNT(available), &selected) ==
		ARCHIVE_REPRESENTATION_NOT_ACCEPTABLE);
}

TEST_CASE("pathology archive recognizes its initial DICOMweb route profile") {
	archive_route_match_t route = {};
	CHECK(archive_route_match(ARCHIVE_HTTP_METHOD_GET, "/health", &route));
	CHECK(route.kind == ARCHIVE_ROUTE_HEALTH);

	CHECK(archive_route_match(ARCHIVE_HTTP_METHOD_GET, "/dicom-web/studies?limit=20", &route));
	CHECK(route.kind == ARCHIVE_ROUTE_QIDO_STUDIES);
	CHECK(archive_route_match(ARCHIVE_HTTP_METHOD_POST, "/dicom-web/studies", &route));
	CHECK(route.kind == ARCHIVE_ROUTE_STOW_STUDIES);

	CHECK(archive_route_match(
		ARCHIVE_HTTP_METHOD_GET,
		"/dicom-web/studies/1.2.3/series/1.2.4/metadata", &route));
	CHECK(route.kind == ARCHIVE_ROUTE_WADO_SERIES_METADATA);
	CHECK(strcmp(route.study_uid, "1.2.3") == 0);
	CHECK(strcmp(route.series_uid, "1.2.4") == 0);

	const char* frame_path = "/dicom-web/studies/1.2.3/series/1.2.4/instances/1.2.5/frames/1,7";
	CHECK(archive_route_match(ARCHIVE_HTTP_METHOD_GET, frame_path, &route));
	CHECK(route.kind == ARCHIVE_ROUTE_WADO_FRAMES);
	CHECK(strcmp(route.study_uid, "1.2.3") == 0);
	CHECK(strcmp(route.series_uid, "1.2.4") == 0);
	CHECK(strcmp(route.instance_uid, "1.2.5") == 0);
	CHECK(strcmp(route.frames, "1,7") == 0);
	u32 frames[4] = {};
	i32 frame_count = 0;
	REQUIRE(archive_frame_list_parse(route.frames, frames, COUNT(frames), &frame_count));
	CHECK(frame_count == 2);
	CHECK(frames[0] == 1);
	CHECK(frames[1] == 7);
	CHECK_FALSE(archive_frame_list_parse("0,1", frames, COUNT(frames), &frame_count));
	CHECK_FALSE(archive_frame_list_parse("1,", frames, COUNT(frames), &frame_count));
	CHECK_FALSE(archive_frame_list_parse("1,2,3", frames, 2, &frame_count));
	REQUIRE(archive_frame_list_parse("7,7,3", frames, COUNT(frames), &frame_count));
	CHECK(frame_count == 3);
	CHECK(frames[0] == 7);
	CHECK(frames[1] == 7);
	CHECK(frames[2] == 3);
	REQUIRE(archive_frame_list_parse("4294967295", frames, COUNT(frames), &frame_count));
	CHECK(frames[0] == UINT32_MAX);
	CHECK_FALSE(archive_frame_list_parse("4294967296", frames, COUNT(frames), &frame_count));

	CHECK_FALSE(archive_route_match(ARCHIVE_HTTP_METHOD_GET,
		"/dicom-web/studies/not-a-uid/series/1.2.4", &route));
	CHECK_FALSE(archive_route_match(ARCHIVE_HTTP_METHOD_GET,
		"/dicom-web/studies/1.2.3/series/1.2.4/instances/1.2.5/frames/0-10", &route));
}

TEST_CASE("fast-tier repair atomically restores published shell bytes") {
	char temp_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	char temp_base[L_tmpnam] = {};
	REQUIRE(tmpnam_s(temp_base, sizeof(temp_base)) == 0);
	snprintf(temp_path, sizeof(temp_path), "%s-shell-repair", temp_base);
#else
	REQUIRE(slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-shell-repair"));
#endif
	archive_repository_t repository = {};
	char error[512] = {};
	REQUIRE(archive_repository_initialize(
		&repository, temp_path, error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	const u8 shell_bytes[] = {'S', 'S', 'H', 'E', 'L', 'L', '1', '\0', 1, 2, 3, 4};
	archive_object_t shell = {};
	REQUIRE(archive_object_store_shell_bytes(
		&repository, shell_bytes, sizeof(shell_bytes), &shell,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	char shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(
		&repository, shell.storage_key, shell_path, sizeof(shell_path)));

	FILE* corrupt = fopen(shell_path, "wb");
	REQUIRE(corrupt != nullptr);
	REQUIRE(fputc(0, corrupt) != EOF);
	REQUIRE(fclose(corrupt) == 0);
	REQUIRE(archive_object_repair_shell_bytes(
		&repository, shell_bytes, sizeof(shell_bytes), shell.sha256,
		shell.storage_key, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	u8 restored[sizeof(shell_bytes)] = {};
	FILE* restored_file = fopen(shell_path, "rb");
	REQUIRE(restored_file != nullptr);
	REQUIRE(fread(restored, 1, sizeof(restored), restored_file) == sizeof(restored));
	CHECK(fgetc(restored_file) == EOF);
	REQUIRE(fclose(restored_file) == 0);
	CHECK(memcmp(restored, shell_bytes, sizeof(restored)) == 0);

	const u8 wrong_bytes[] = {9, 8, 7};
	CHECK(archive_object_repair_shell_bytes(
		&repository, wrong_bytes, sizeof(wrong_bytes), shell.sha256,
		shell.storage_key, error, sizeof(error)) == ARCHIVE_OBJECT_INTEGRITY_ERROR);
	REQUIRE(remove(shell_path) == 0);
	REQUIRE(archive_object_repair_shell_bytes(
		&repository, shell_bytes, sizeof(shell_bytes), shell.sha256,
		shell.storage_key, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	REQUIRE(remove(shell_path) == 0);

	char path[ARCHIVE_PATH_CAPACITY];
	snprintf(path, sizeof(path), "%s/shells/sha256/%.2s", temp_path, shell.sha256);
	CHECK(archive_test_rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/shells/sha256", temp_path);
	CHECK(archive_test_rmdir(path) == 0);
	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		snprintf(path, sizeof(path), "%s/%s", temp_path, child);
		CHECK(archive_test_rmdir(path) == 0);
	}
	snprintf(path, sizeof(path), "%s/repository.json", temp_path);
	CHECK(remove(path) == 0);
	archive_test_remove_database(temp_path);
	CHECK(archive_test_rmdir(temp_path) == 0);
}

TEST_CASE("pathology archive repository has explicit immutable and publication boundaries") {
	char temp_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	char temp_base[L_tmpnam] = {0};
	REQUIRE(tmpnam_s(temp_base, sizeof(temp_base)) == 0);
	snprintf(temp_path, sizeof(temp_path), "%s-archive", temp_base);
#else
	bool created_temp_directory = slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-archive-test");
	REQUIRE(created_temp_directory);
#endif
	archive_repository_t repository = {};
	char error[512] = {0};
	char path[ARCHIVE_PATH_CAPACITY];
	snprintf(path, sizeof(path), "%s/repository.json", temp_path);
	FILE* incompatible = fopen(path, "wb");
	REQUIRE(incompatible != nullptr);
	fputs("{\"kind\": \"slidescape-pathology-archive\", \"schemaVersion\": 99}\n", incompatible);
	fclose(incompatible);
	CHECK(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	      ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA);
	CHECK(remove(path) == 0);

	CHECK(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	CHECK(archive_repository_open(&repository, temp_path, error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	snprintf(path, sizeof(path), "%s/%s", temp_path, ARCHIVE_DATABASE_FILENAME);
	FILE* database_file = fopen(path, "rb");
	REQUIRE(database_file != nullptr);
	fclose(database_file);
	CHECK(archive_database_validate(&repository, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_test_remove_database(temp_path);
	CHECK(archive_repository_open(&repository, temp_path, error, sizeof(error)) ==
	      ARCHIVE_REPOSITORY_IO_ERROR);
	CHECK(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	      ARCHIVE_REPOSITORY_OK);
	CHECK(archive_repository_open(&repository, temp_path, error, sizeof(error)) ==
	      ARCHIVE_REPOSITORY_OK);
	sqlite3* database = nullptr;
	REQUIRE(sqlite3_open(path, &database) == SQLITE_OK);
	REQUIRE(sqlite3_exec(database, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr) ==
	        SQLITE_OK);
	REQUIRE(sqlite3_close(database) == SQLITE_OK);
	CHECK(archive_database_validate(&repository, error, sizeof(error)) ==
	      ARCHIVE_DATABASE_INCOMPATIBLE_SCHEMA);
	CHECK(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	      ARCHIVE_REPOSITORY_INCOMPATIBLE_SCHEMA);
	REQUIRE(sqlite3_open(path, &database) == SQLITE_OK);
	REQUIRE(sqlite3_exec(database, "PRAGMA user_version = 4;", nullptr, nullptr, nullptr) ==
	        SQLITE_OK);
	REQUIRE(sqlite3_close(database) == SQLITE_OK);
	CHECK(archive_database_validate(&repository, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_ingest_result_t ingest_result = {};
	CHECK_FALSE(archive_ingest_part10_objects(&repository, &ingest_result, error, sizeof(error)));
	CHECK(strstr(error, "no .dcm objects") != nullptr);
	archive_index_t empty_index = {};
	REQUIRE(archive_index_open(&empty_index, &repository, error, sizeof(error)));
	CHECK(empty_index.study_count == 0);
	CHECK(empty_index.series_count == 0);
	CHECK(empty_index.instance_count == 0);
	archive_index_destroy(&empty_index);

	archive_database_object_t part10_object = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 123,
		"application/dicom", "objects/sha256/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		&part10_object, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_object_t second_part10_object = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"abababababababababababababababababababababababababababababababab", 124,
		"application/dicom",
		"objects/sha256/ab/abababababababababababababababababababababababababababababababab",
		&second_part10_object, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_object_t descriptor_object = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 45,
		"application/vnd.slidescape.part10-bundle",
		"objects/sha256/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
		&descriptor_object, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_part10_member_t member = {};
	member.object_id = part10_object.id;
	snprintf(member.logical_name, sizeof(member.logical_name), "1.2.3.1.1.dcm");
	snprintf(member.study_instance_uid, sizeof(member.study_instance_uid), "1.2.3");
	snprintf(member.series_instance_uid, sizeof(member.series_instance_uid), "1.2.3.1");
	snprintf(member.sop_instance_uid, sizeof(member.sop_instance_uid), "1.2.3.1.1");
	snprintf(member.sop_class_uid, sizeof(member.sop_class_uid),
	         "1.2.840.10008.5.1.4.1.1.77.1.6");
	snprintf(member.transfer_syntax_uid, sizeof(member.transfer_syntax_uid),
	         "1.2.840.10008.1.2.4.90");
	snprintf(member.modality, sizeof(member.modality), "SM");
	snprintf(member.study_date, sizeof(member.study_date), "20260720");
	member.has_instance_number = true;
	member.instance_number = 1;
	member.frame_count = 437;
	member.rows = 240;
	member.columns = 240;
	member.total_pixel_matrix_columns = 4481;
	member.total_pixel_matrix_rows = 5345;
	snprintf(member.image_flavor, sizeof(member.image_flavor), "VOLUME");
	archive_database_part10_member_t bundle_members[2] = {member, member};
	bundle_members[1].object_id = second_part10_object.id;
	snprintf(bundle_members[1].logical_name,
	         sizeof(bundle_members[1].logical_name), "1.2.3.1.2.dcm");
	snprintf(bundle_members[1].sop_instance_uid,
	         sizeof(bundle_members[1].sop_instance_uid), "1.2.3.1.2");
	bundle_members[1].instance_number = 2;
	archive_database_bundle_t published_bundle = {};
	REQUIRE(archive_database_register_part10_bundle(
		&repository, descriptor_object.sha256, descriptor_object.id,
		bundle_members, 2,
		&published_bundle, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_bundle_t replayed_bundle = {};
	REQUIRE(archive_database_register_part10_bundle(
		&repository, descriptor_object.sha256, descriptor_object.id,
		bundle_members, 2,
		&replayed_bundle, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	CHECK(replayed_bundle.id == published_bundle.id);
	CHECK(replayed_bundle.already_registered);
	archive_database_object_t recipe_object = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 512,
		ARCHIVE_SHELL_RECIPE_MEDIA_TYPE,
		"objects/sha256/dd/dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
		&recipe_object, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_object_t shell_object = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 4096,
		"application/vnd.slidescape.virtual-dicom-shell",
		"shells/sha256/ee/eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		&shell_object, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_shell_publication_t shell_publication = {};
	archive_database_shell_instance_publication_t shell_instance_publications[2] = {};
	shell_instance_publications[0].instance = bundle_members[0];
	shell_instance_publications[0].shell_instance_index = 0;
	shell_instance_publications[1].instance = bundle_members[1];
	shell_instance_publications[1].shell_instance_index = 1;
	snprintf(shell_publication.representation_id,
	         sizeof(shell_publication.representation_id), "%s",
	         member.series_instance_uid);
	shell_publication.shell_object_id = shell_object.id;
	shell_publication.recipe_object_id = recipe_object.id;
	shell_publication.instances = shell_instance_publications;
	shell_publication.instance_count = 2;
	snprintf(shell_publication.representation_fingerprint,
	         sizeof(shell_publication.representation_fingerprint),
	         "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
	REQUIRE(archive_database_publish_part10_shells(
		&repository, published_bundle.id, &shell_publication, 1,
		error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	REQUIRE(archive_database_publish_part10_shells(
		&repository, published_bundle.id, &shell_publication, 1,
		error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	sqlite3* publication_database = nullptr;
	REQUIRE(sqlite3_open(path, &publication_database) == SQLITE_OK);
	sqlite3_stmt* publication_count = nullptr;
	REQUIRE(sqlite3_prepare_v2(publication_database,
		"SELECT "
		"(SELECT count(*) FROM representation WHERE state='PUBLISHED'),"
		"(SELECT count(*) FROM publication),"
		"(SELECT count(*) FROM outbox WHERE operation='SHELL_PUBLISHED'),"
		"(SELECT count(*) FROM instance_provider WHERE provider_kind='part10');",
		-1, &publication_count, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_step(publication_count) == SQLITE_ROW);
	CHECK(sqlite3_column_int(publication_count, 0) == 1);
	CHECK(sqlite3_column_int(publication_count, 1) == 1);
	CHECK(sqlite3_column_int(publication_count, 2) == 1);
	CHECK(sqlite3_column_int(publication_count, 3) == 2);
	CHECK(sqlite3_finalize(publication_count) == SQLITE_OK);
	CHECK(sqlite3_close(publication_database) == SQLITE_OK);
	archive_database_object_t conflicting_descriptor = {};
	REQUIRE(archive_database_register_object(
		&repository,
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 46,
		"application/vnd.slidescape.part10-bundle",
		"objects/sha256/cc/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		&conflicting_descriptor, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	archive_database_part10_member_t conflicting_member = member;
	conflicting_member.object_id = descriptor_object.id;
	archive_database_bundle_t conflicting_bundle = {};
	CHECK(archive_database_register_part10_bundle(
		&repository,
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		conflicting_descriptor.id, &conflicting_member, 1, &conflicting_bundle,
		error, sizeof(error)) == ARCHIVE_DATABASE_CONFLICT);
	/* The published shell model is now the serving authority. */
	REQUIRE(sqlite3_open(path, &publication_database) == SQLITE_OK);
	REQUIRE(sqlite3_exec(publication_database, "DELETE FROM part10_instance;",
	                     nullptr, nullptr, nullptr) == SQLITE_OK);
	CHECK(sqlite3_close(publication_database) == SQLITE_OK);
	archive_index_t index = {};
	REQUIRE(archive_index_open(&index, &repository, error, sizeof(error)));
	CHECK(index.study_count == 1);
	CHECK(index.series_count == 1);
	CHECK(index.instance_count == 2);
	const archive_instance_record_t* published_instance = archive_index_find_instance(
		&index, "1.2.3", "1.2.3.1", "1.2.3.1.1");
	REQUIRE(published_instance != nullptr);
	CHECK(strcmp(published_instance->provider, "part10") == 0);
	CHECK(strcmp(published_instance->object_key, part10_object.storage_key) == 0);
	CHECK(strcmp(published_instance->shell_key, shell_object.storage_key) == 0);
	CHECK(strcmp(published_instance->bundle_sha256,
	             published_bundle.bundle_digest) == 0);
	CHECK(strcmp(published_instance->representation_fingerprint,
	             shell_publication.representation_fingerprint) == 0);
	CHECK(strcmp(published_instance->recipe_key, recipe_object.storage_key) == 0);
	CHECK(strcmp(published_instance->recipe_sha256, recipe_object.sha256) == 0);
	CHECK(published_instance->recipe_byte_size == recipe_object.byte_size);
	CHECK(published_instance->shell_instance_index == 0);
	CHECK(strcmp(published_instance->transfer_syntax_uid, "1.2.840.10008.1.2.4.90") == 0);
	const archive_instance_record_t* second_published_instance =
		archive_index_find_instance(
			&index, "1.2.3", "1.2.3.1", "1.2.3.1.2");
	REQUIRE(second_published_instance != nullptr);
	CHECK(strcmp(second_published_instance->shell_key,
	             published_instance->shell_key) == 0);
	CHECK(second_published_instance->shell_instance_index == 1);
	CHECK(strcmp(second_published_instance->object_key,
	             second_part10_object.storage_key) == 0);
	const archive_instance_record_t* series_instances = nullptr;
	i32 series_instance_count = 0;
	REQUIRE(archive_index_find_series_instances(
		&index, "1.2.3", "1.2.3.1",
		&series_instances, &series_instance_count));
	CHECK(series_instance_count == 2);
	CHECK(series_instances == published_instance);
	CHECK(strcmp(series_instances[0].sop_instance_uid, "1.2.3.1.1") == 0);
	CHECK(strcmp(series_instances[1].sop_instance_uid, "1.2.3.1.2") == 0);
	CHECK_FALSE(archive_index_find_series_instances(
		&index, "1.2.3", "1.2.3.99",
		&series_instances, &series_instance_count));
	CHECK(series_instances == nullptr);
	CHECK(series_instance_count == 0);
	archive_study_query_t query = {};
	REQUIRE(archive_study_query_parse("/dicom-web/studies?0020000D=1.2.3&limit=1", &query));
	char json[4096];
	size_t json_size = 0;
	REQUIRE(archive_index_write_studies_json(&index, &query, json, sizeof(json), &json_size));
	dicomweb_study_ref_t result = {};
	CHECK(dicomweb_parse_study_refs(json, json_size, &result, 1) == 1);
	CHECK(strcmp(result.study_instance_uid, "1.2.3") == 0);
	CHECK(strcmp(result.study_date, "20260720") == 0);
	CHECK(strcmp(result.description, "Imported DICOM study") == 0);
	CHECK_FALSE(archive_study_query_parse("/dicom-web/studies?limit=1001", &query));

	archive_series_query_t series_query = {};
	REQUIRE(archive_series_query_parse(
		"/dicom-web/studies/1.2.3/series?0020000E=1.2.3.1&limit=10", "1.2.3", &series_query));
	REQUIRE(archive_index_write_series_json(&index, &series_query, json, sizeof(json), &json_size));
	dicomweb_series_ref_t series_result = {};
	CHECK(dicomweb_parse_series_refs(json, json_size, &series_result, 1) == 1);
	CHECK(strcmp(series_result.series_instance_uid, "1.2.3.1") == 0);
	CHECK(strcmp(series_result.series_number, "") == 0);
	CHECK(strcmp(series_result.modality, "SM") == 0);

	archive_instance_query_t instance_query = {};
	REQUIRE(archive_instance_query_parse(
		"/dicom-web/studies/1.2.3/series/1.2.3.1/instances?limit=10",
		"1.2.3", "1.2.3.1", &instance_query));
	REQUIRE(archive_index_write_instances_json(&index, &instance_query, json, sizeof(json), &json_size));
	dicomweb_instance_ref_t instance_result = {};
	CHECK(dicomweb_parse_instance_refs(json, json_size, &instance_result, 1) == 1);
	CHECK(strcmp(instance_result.sop_instance_uid, "1.2.3.1.1") == 0);
	CHECK(strcmp(instance_result.image_flavor, "VOLUME") == 0);
	CHECK(strcmp(instance_result.number_of_frames, "437") == 0);
	CHECK(strcmp(instance_result.total_pixel_matrix_columns, "4481") == 0);
	CHECK(strcmp(instance_result.total_pixel_matrix_rows, "5345") == 0);
	archive_index_destroy(&index);

	archive_source_bundle_manifest_t bundle = {};
	snprintf(bundle.bundle_id, sizeof(bundle.bundle_id), "idc-test-bundle");
	bundle.source_format = ARCHIVE_SOURCE_FORMAT_DICOM;
	memset(bundle.bundle_digest_sha256, 'a', 64);
	bundle.bundle_digest_sha256[64] = '\0';
	snprintf(bundle.adapter_name, sizeof(bundle.adapter_name), "slidescape-dicom");
	snprintf(bundle.adapter_version, sizeof(bundle.adapter_version), "1-0");
	bundle.object_count = 4;
	bundle.total_bytes = 199 * MEGABYTES(1);
	REQUIRE(archive_manifest_stage_source_bundle(&repository, &bundle, error, sizeof(error)));

	archive_virtual_instance_manifest_t instance = {};
	snprintf(instance.representation_id, sizeof(instance.representation_id), "idc-test-volume-1");
	snprintf(instance.bundle_id, sizeof(instance.bundle_id), "%s", bundle.bundle_id);
	instance.representation_version = 1;
	snprintf(instance.study_instance_uid, sizeof(instance.study_instance_uid), "1.2.3");
	snprintf(instance.series_instance_uid, sizeof(instance.series_instance_uid), "1.2.3.1");
	snprintf(instance.sop_instance_uid, sizeof(instance.sop_instance_uid), "1.2.3.1.1");
	snprintf(instance.sop_class_uid, sizeof(instance.sop_class_uid),
	         "1.2.840.10008.5.1.4.1.1.77.1.6");
	snprintf(instance.transfer_syntax_uid, sizeof(instance.transfer_syntax_uid),
	         "1.2.840.10008.1.2.4.90");
	snprintf(instance.image_flavor, sizeof(instance.image_flavor), "VOLUME");
	snprintf(instance.photometric_interpretation, sizeof(instance.photometric_interpretation), "RGB");
	instance.instance_number = 1;
	instance.frame_count = 437;
	instance.rows = 240;
	instance.columns = 240;
	instance.total_pixel_matrix_rows = 5345;
	instance.total_pixel_matrix_columns = 4481;
	instance.provider_kind = ARCHIVE_MANIFEST_PROVIDER_PART10;
	snprintf(instance.provider_locator, sizeof(instance.provider_locator),
	         "objects/test.dcm");
	instance.representation_source = ARCHIVE_MANIFEST_REPRESENTATION_PASSTHROUGH;
	instance.frame_map_kind = ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR;
	instance.tiles_per_row = 19;
	instance.tiles_per_column = 23;
	instance.optical_path_count = 1;
	instance.focal_plane_count = 1;
	snprintf(instance.default_optical_path_identifier,
	         sizeof(instance.default_optical_path_identifier), "0");
	instance.has_default_focal_plane_z_offset = true;
	instance.default_focal_plane_z_offset_um = 0.0f;
	instance.pixel_spacing_row_mm = 0.00025f;
	instance.pixel_spacing_column_mm = 0.00025f;
	instance.has_slide_coordinate_origin = true;
	instance.slide_coordinate_origin_x_mm = 25.5f;
	instance.slide_coordinate_origin_y_mm = 42.0f;
	instance.slide_coordinate_origin_z_mm = 0.0f;
	instance.has_image_orientation_slide = true;
	instance.image_orientation_slide[0] = 1.0f;
	instance.image_orientation_slide[4] = 1.0f;
	REQUIRE(archive_manifest_stage_virtual_instance(&repository, &instance, error, sizeof(error)));
	CHECK_FALSE(archive_manifest_stage_virtual_instance(&repository, &instance, error, sizeof(error)));
	archive_virtual_instance_manifest_t invalid_instance = instance;
	invalid_instance.pixel_spacing_column_mm = 0.0f;
	CHECK_FALSE(archive_virtual_instance_manifest_is_valid(&invalid_instance));
	invalid_instance = instance;
	invalid_instance.tiles_per_row = 18;
	CHECK_FALSE(archive_virtual_instance_manifest_is_valid(&invalid_instance));
	invalid_instance = instance;
	invalid_instance.optical_path_count = 2;
	invalid_instance.default_optical_path_identifier[0] = '\0';
	CHECK_FALSE(archive_virtual_instance_manifest_is_valid(&invalid_instance));
	snprintf(invalid_instance.frame_dimension_map_object_key,
	         sizeof(invalid_instance.frame_dimension_map_object_key),
	         "derived/test-dimensions.bin");
	CHECK(archive_virtual_instance_manifest_is_valid(&invalid_instance));

	snprintf(path, sizeof(path), "%s/staging/%s.source.json", temp_path, bundle.bundle_id);
	CHECK(remove(path) == 0);
	snprintf(path, sizeof(path), "%s/staging/%s.representation.json", temp_path,
	         instance.representation_id);
	FILE* representation_file = fopen(path, "rb");
	REQUIRE(representation_file != nullptr);
	char representation_json[8192] = {};
	size_t representation_size = fread(representation_json, 1,
	                                   sizeof(representation_json) - 1,
	                                   representation_file);
	CHECK(ferror(representation_file) == 0);
	fclose(representation_file);
	REQUIRE(representation_size > 0);
	CHECK(strstr(representation_json, "\"schemaVersion\": 2") != nullptr);
	CHECK(strstr(representation_json,
	             "\"provider\": {\"kind\": \"part10\", \"locator\": \"objects/test.dcm\"}") != nullptr);
	CHECK(strstr(representation_json, "\"mapKind\": \"regular-row-major\"") != nullptr);
	CHECK(strstr(representation_json, "\"tilesPerRow\": 19") != nullptr);
	CHECK(strstr(representation_json,
	             "\"defaultOpticalPathIdentifier\": \"0\"") != nullptr);
	CHECK(strstr(representation_json, "\"originMm\": [25.5, 42, 0]") != nullptr);
	CHECK(strstr(representation_json,
	             "\"imageOrientationSlide\": [1, 0, 0, 0, 1, 0]") != nullptr);
	CHECK(remove(path) == 0);

	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		snprintf(path, sizeof(path), "%s/%s", temp_path, child);
		CHECK(archive_test_rmdir(path) == 0);
	}
	snprintf(path, sizeof(path), "%s/repository.json", temp_path);
	CHECK(remove(path) == 0);
	archive_test_remove_database(temp_path);
	CHECK(archive_test_rmdir(temp_path) == 0);
}

TEST_CASE("pathology archive stores immutable objects by verified SHA-256") {
	char temp_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	char temp_base[L_tmpnam] = {0};
	REQUIRE(tmpnam_s(temp_base, sizeof(temp_base)) == 0);
	snprintf(temp_path, sizeof(temp_path), "%s-object", temp_base);
#else
	REQUIRE(slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-object-test"));
#endif
	archive_repository_t repository = {};
	char error[512] = {};
	REQUIRE(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	        ARCHIVE_REPOSITORY_OK);

	const char payload[] = "abc";
	archive_object_t stored = {};
	REQUIRE(archive_object_store_bytes(&repository, payload, sizeof(payload) - 1,
	                                   "application/octet-stream", &stored,
	                                   error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(stored.id > 0);
	CHECK(stored.byte_size == 3);
	CHECK(strcmp(stored.sha256,
	             "ba7816bf8f01cfea414140de5dae2223"
	             "b00361a396177a9cb410ff61f20015ad") == 0);
	CHECK(strcmp(stored.storage_key,
	             "objects/sha256/ba/"
	             "ba7816bf8f01cfea414140de5dae2223"
	             "b00361a396177a9cb410ff61f20015ad") == 0);
	CHECK_FALSE(stored.already_present);

	char object_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, stored.storage_key,
	                                     object_path, sizeof(object_path)));
	FILE* object_file = fopen(object_path, "rb");
	REQUIRE(object_file != nullptr);
	char read_payload[4] = {};
	CHECK(fread(read_payload, 1, 3, object_file) == 3);
	CHECK(fclose(object_file) == 0);
	CHECK(memcmp(read_payload, payload, 3) == 0);

	char source_path[ARCHIVE_PATH_CAPACITY];
	snprintf(source_path, sizeof(source_path), "%s/source.bin", temp_path);
	FILE* source = fopen(source_path, "wb");
	REQUIRE(source != nullptr);
	CHECK(fwrite(payload, 1, 3, source) == 3);
	CHECK(fclose(source) == 0);
	archive_object_t replay = {};
	REQUIRE(archive_object_import_file(&repository, source_path,
	                                   "application/octet-stream", &replay,
	                                   error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(replay.id == stored.id);
	CHECK(replay.already_present);

	char reference_source_path[ARCHIVE_PATH_CAPACITY];
	snprintf(reference_source_path, sizeof(reference_source_path),
	         "%s/reference-source.bin", temp_path);
	FILE* reference_source = fopen(reference_source_path, "wb");
	REQUIRE(reference_source != nullptr);
	const char reference_payload[] = "externally referenced";
	CHECK(fwrite(reference_payload, 1, sizeof(reference_payload) - 1,
	             reference_source) == sizeof(reference_payload) - 1);
	CHECK(fclose(reference_source) == 0);
	archive_object_t referenced = {};
	REQUIRE(archive_object_reference_file(
		&repository, reference_source_path, "application/octet-stream",
		&referenced, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	char referenced_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(
		&repository, referenced.storage_key,
		referenced_path, sizeof(referenced_path)));
#if !defined(_WIN32)
	struct stat referenced_info = {};
	REQUIRE(lstat(referenced_path, &referenced_info) == 0);
	CHECK(S_ISLNK(referenced_info.st_mode));
#endif
	FILE* referenced_file = fopen(referenced_path, "rb");
	REQUIRE(referenced_file != nullptr);
	char referenced_bytes[sizeof(reference_payload)] = {};
	CHECK(fread(referenced_bytes, 1, sizeof(reference_payload) - 1,
	            referenced_file) == sizeof(reference_payload) - 1);
	CHECK(fclose(referenced_file) == 0);
	CHECK(memcmp(referenced_bytes, reference_payload,
	             sizeof(reference_payload) - 1) == 0);

	const char inline_payload[] = "canonical-recipe";
	archive_object_t inline_object = {};
	REQUIRE(archive_object_store_inline_bytes(
		&repository, inline_payload, sizeof(inline_payload) - 1,
		ARCHIVE_SHELL_RECIPE_MEDIA_TYPE, &inline_object,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(inline_object.id > 0);
	CHECK(inline_object.storage_key[0] == '\0');
	u8* inline_data = nullptr;
	size_t inline_size = 0;
	REQUIRE(archive_object_read_inline_bytes(
		&repository, inline_object.sha256, inline_object.byte_size,
		ARCHIVE_SHELL_RECIPE_MEDIA_TYPE, 4096, &inline_data, &inline_size,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(inline_size == sizeof(inline_payload) - 1);
	CHECK(memcmp(inline_data, inline_payload, inline_size) == 0);
	free(inline_data);
	archive_object_t inline_replay = {};
	REQUIRE(archive_object_store_inline_bytes(
		&repository, inline_payload, sizeof(inline_payload) - 1,
		ARCHIVE_SHELL_RECIPE_MEDIA_TYPE, &inline_replay,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(inline_replay.id == inline_object.id);
	CHECK(inline_replay.already_present);

	size_t larger_control_size = 1024 * 1024 + 1;
	char* larger_control_payload = (char*)malloc(larger_control_size);
	REQUIRE(larger_control_payload != nullptr);
	memset(larger_control_payload, 'c', larger_control_size);
	archive_object_t larger_control_object = {};
	REQUIRE(archive_object_store_control_bytes(
		&repository, larger_control_payload, larger_control_size,
		"application/vnd.slidescape.test-control", &larger_control_object,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	free(larger_control_payload);
	CHECK(larger_control_object.storage_key[0] == '\0');

	size_t large_size = 1000000;
	char* large_payload = (char*)malloc(large_size);
	REQUIRE(large_payload != nullptr);
	memset(large_payload, 'a', large_size);
	archive_object_t large = {};
	REQUIRE(archive_object_store_bytes(&repository, large_payload, large_size,
	                                   "application/octet-stream", &large,
	                                   error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	free(large_payload);
	CHECK(strcmp(large.sha256,
	             "cdc76e5c9914fb9281a1c7e284d73e67"
	             "f1809a48a497200e046d39ccc7112cd0") == 0);

	const char shell_payload[] = "validated-shell";
	archive_object_t fast_shell = {};
	REQUIRE(archive_object_store_shell_bytes(
		&repository, shell_payload, sizeof(shell_payload) - 1, &fast_shell,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(strncmp(fast_shell.storage_key, "shells/sha256/",
	              strlen("shells/sha256/")) == 0);
	archive_object_t fast_shell_replay = {};
	REQUIRE(archive_object_store_shell_bytes(
		&repository, shell_payload, sizeof(shell_payload) - 1, &fast_shell_replay,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	CHECK(fast_shell_replay.id == fast_shell.id);
	CHECK(fast_shell_replay.already_present);
	char fast_shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, fast_shell.storage_key,
	                                     fast_shell_path, sizeof(fast_shell_path)));
	FILE* fast_shell_file = fopen(fast_shell_path, "wb");
	REQUIRE(fast_shell_file != nullptr);
	CHECK(fwrite("bad", 1, 3, fast_shell_file) == 3);
	CHECK(fclose(fast_shell_file) == 0);
	archive_object_t corrupted_shell = {};
	CHECK(archive_object_store_shell_bytes(
		&repository, shell_payload, sizeof(shell_payload) - 1, &corrupted_shell,
		error, sizeof(error)) == ARCHIVE_OBJECT_INTEGRITY_ERROR);

	archive_object_t conflicting = {};
	CHECK(archive_object_store_bytes(&repository, payload, 3, "application/dicom",
	                                 &conflicting, error, sizeof(error)) ==
	      ARCHIVE_OBJECT_CONFLICT);

	sqlite3* database = nullptr;
	char database_path[ARCHIVE_PATH_CAPACITY];
	snprintf(database_path, sizeof(database_path), "%s/%s",
	         temp_path, ARCHIVE_DATABASE_FILENAME);
	REQUIRE(sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READONLY, nullptr) ==
	        SQLITE_OK);
	sqlite3_stmt* count = nullptr;
	REQUIRE(sqlite3_prepare_v2(database, "SELECT count(*) FROM object;", -1,
	                           &count, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_step(count) == SQLITE_ROW);
	CHECK(sqlite3_column_int(count, 0) == 6);
	CHECK(sqlite3_finalize(count) == SQLITE_OK);
	CHECK(sqlite3_close(database) == SQLITE_OK);

	object_file = fopen(object_path, "wb");
	REQUIRE(object_file != nullptr);
	CHECK(fwrite("bad", 1, 3, object_file) == 3);
	CHECK(fclose(object_file) == 0);
	archive_object_t corrupted = {};
	CHECK(archive_object_store_bytes(&repository, payload, 3,
	                                 "application/octet-stream", &corrupted,
	                                 error, sizeof(error)) ==
	      ARCHIVE_OBJECT_INTEGRITY_ERROR);
	CHECK(strstr(error, "integrity") != nullptr);

	char path[ARCHIVE_PATH_CAPACITY];
	CHECK(remove(source_path) == 0);
	CHECK(remove(reference_source_path) == 0);
	CHECK(remove(referenced_path) == 0);
	snprintf(path, sizeof(path), "%s/objects/sha256/%.2s",
	         temp_path, referenced.sha256);
	CHECK(archive_test_rmdir(path) == 0);
	CHECK(remove(object_path) == 0);
	snprintf(path, sizeof(path), "%s/objects/sha256/ba", temp_path);
	CHECK(archive_test_rmdir(path) == 0);
	REQUIRE(archive_repository_make_path(&repository, large.storage_key,
	                                     object_path, sizeof(object_path)));
	CHECK(remove(object_path) == 0);
	snprintf(path, sizeof(path), "%s/objects/sha256/cd", temp_path);
	CHECK(archive_test_rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/objects/sha256", temp_path);
	CHECK(archive_test_rmdir(path) == 0);
	REQUIRE(archive_repository_make_path(&repository, fast_shell.storage_key,
	                                     object_path, sizeof(object_path)));
	CHECK(remove(object_path) == 0);
	snprintf(path, sizeof(path), "%s/shells/sha256/%.2s", temp_path, fast_shell.sha256);
	CHECK(archive_test_rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/shells/sha256", temp_path);
	CHECK(archive_test_rmdir(path) == 0);
	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		snprintf(path, sizeof(path), "%s/%s", temp_path, child);
		CHECK(archive_test_rmdir(path) == 0);
	}
	snprintf(path, sizeof(path), "%s/repository.json", temp_path);
	CHECK(remove(path) == 0);
	archive_test_remove_database(temp_path);
	CHECK(archive_test_rmdir(temp_path) == 0);
}

TEST_CASE("Part 10 metadata populates deterministic virtual-instance geometry") {
	dicom_instance_t source = {};
	snprintf(source.general_study.StudyInstanceUID.value,
	         sizeof(source.general_study.StudyInstanceUID.value), "1.2.3");
	snprintf(source.general_series.SeriesInstanceUID.value,
	         sizeof(source.general_series.SeriesInstanceUID.value), "1.2.3.1");
	snprintf(source.sop_instance_uid.value, sizeof(source.sop_instance_uid.value),
	         "1.2.3.1.1");
	snprintf(source.sop_class_uid.value, sizeof(source.sop_class_uid.value),
	         "1.2.840.10008.5.1.4.1.1.77.1.6");
	snprintf(source.transfer_syntax_uid.value, sizeof(source.transfer_syntax_uid.value),
	         "1.2.840.10008.1.2.4.91");
	source.image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
	source.photometric_interpretation = DICOM_PHOTOMETRIC_INTERPRETATION_RGB;
	source.instance_number = 3;
	source.number_of_frames = 6;
	source.rows = 240;
	source.columns = 240;
	source.total_pixel_matrix_rows = 480;
	source.total_pixel_matrix_columns = 720;
	source.pixel_spacing = V2F(0.0005f, 0.0005f);
	source.is_tiled_full = true;
	source.number_of_optical_paths = 1;
	source.total_pixel_matrix_focal_planes = 1;
	source.is_total_pixel_matrix_origin_known = true;
	source.total_pixel_matrix_origin_x_mm = 12.5f;
	source.total_pixel_matrix_origin_y_mm = 7.25f;
	source.total_pixel_matrix_origin_z_mm = 0.0f;
	source.is_image_orientation_slide_known = true;
	source.image_orientation_slide[0] = 1.0f;
	source.image_orientation_slide[4] = 1.0f;
	dicom_optical_path_t optical_path = {};
	optical_path.OpticalPathIdentifier.present = true;
	snprintf(optical_path.OpticalPathIdentifier.value,
	         sizeof(optical_path.OpticalPathIdentifier.value), "A");
	arrput(source.optical_paths, optical_path);
	for (i32 frame = 0; frame < 6; ++frame) {
		dicom_plane_position_slide_t position = {};
		position.z_offset_in_slide_coordinate_system = 1.5f;
		arrput(source.per_frame_plane_position_slide, position);
	}

	archive_virtual_instance_manifest_t manifest = {};
	snprintf(manifest.representation_id, sizeof(manifest.representation_id), "representation-1");
	snprintf(manifest.bundle_id, sizeof(manifest.bundle_id), "bundle-1");
	manifest.representation_version = 1;
	snprintf(manifest.provider_locator, sizeof(manifest.provider_locator), "objects/test.dcm");
	REQUIRE(archive_manifest_populate_part10_instance(&manifest, &source));
	CHECK(manifest.frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_REGULAR_ROW_MAJOR);
	CHECK(manifest.tiles_per_row == 3);
	CHECK(manifest.tiles_per_column == 2);
	CHECK(manifest.optical_path_count == 1);
	CHECK(strcmp(manifest.default_optical_path_identifier, "A") == 0);
	CHECK(manifest.focal_plane_count == 1);
	CHECK(manifest.has_default_focal_plane_z_offset);
	CHECK(manifest.default_focal_plane_z_offset_um == doctest::Approx(1.5f));
	CHECK(manifest.has_slide_coordinate_origin);
	CHECK(manifest.has_image_orientation_slide);
	CHECK(archive_virtual_instance_manifest_is_valid(&manifest));

	arrfree(source.per_frame_plane_position_slide);
	arrfree(source.optical_paths);
}

TEST_CASE("virtual DICOM shell loads one aligned Part 10 serving object") {
	dicom_instance_t source = {};
#define SET_DICOM_UI(field, text) \
	do { snprintf((field).value, sizeof((field).value), "%s", text); \
	     (field).len = strlen((field).value); } while (0)
	SET_DICOM_UI(source.general_study.StudyInstanceUID, "1.2.3");
	SET_DICOM_UI(source.general_series.SeriesInstanceUID, "1.2.3.1");
	SET_DICOM_UI(source.sop_instance_uid, "1.2.3.1.1");
	SET_DICOM_UI(source.sop_class_uid, "1.2.840.10008.5.1.4.1.1.77.1.6");
	SET_DICOM_UI(source.transfer_syntax_uid, "1.2.840.10008.1.2.1");
#undef SET_DICOM_UI
	snprintf(source.general_series.Modality.value,
	         sizeof(source.general_series.Modality.value), "SM");
	source.image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
	source.photometric_interpretation = DICOM_PHOTOMETRIC_INTERPRETATION_RGB;
	source.instance_number = 3;
	source.number_of_frames = 6;
	source.rows = 2;
	source.columns = 2;
	source.samples_per_pixel = 3;
	source.bits_allocated = 8;
	source.bits_stored = 8;
	source.high_bit = 7;
	source.total_pixel_matrix_rows = 4;
	source.total_pixel_matrix_columns = 6;
	source.pixel_spacing = V2F(0.0005f, 0.0005f);
	source.is_tiled_full = true;
	source.found_pixel_data = true;
	source.pixel_data.data_offset = 512;
	source.pixel_data.length = 72;
	source.number_of_optical_paths = 1;
	source.total_pixel_matrix_focal_planes = 1;
	source.is_total_pixel_matrix_origin_known = true;
	source.total_pixel_matrix_origin_x_mm = 12.5f;
	source.total_pixel_matrix_origin_y_mm = 7.25f;
	source.is_image_orientation_slide_known = true;
	source.image_orientation_slide[0] = 1.0f;
	source.image_orientation_slide[4] = 1.0f;
	source.patient.PatientID.present = true;
	snprintf(source.patient.PatientID.value,
	         sizeof(source.patient.PatientID.value), "PATIENT-01");
	source.patient.PatientName.present = true;
	snprintf(source.patient.PatientName.value,
	         sizeof(source.patient.PatientName.value), "Family^Given=Ideographic=Phonetic");
	source.patient.PatientBirthDate = {1980, 2, 3, true};
	source.patient.PatientSex.present = true;
	snprintf(source.patient.PatientSex.value,
	         sizeof(source.patient.PatientSex.value), "O");
	source.general_study.StudyDate = {2026, 7, 25, true};
	source.general_study.AccessionNumber.present = true;
	snprintf(source.general_study.AccessionNumber.value,
	         sizeof(source.general_study.AccessionNumber.value), "ACCESSION-01");
	source.general_study.StudyID.present = true;
	snprintf(source.general_study.StudyID.value,
	         sizeof(source.general_study.StudyID.value), "STUDY-01");
	u8 icc_profile[128] = {};
	icc_profile[3] = sizeof(icc_profile);
	memcpy(icc_profile + 36, "acsp", 4);
	dicom_optical_path_t optical_path = {};
	optical_path.OpticalPathIdentifier.present = true;
	snprintf(optical_path.OpticalPathIdentifier.value,
	         sizeof(optical_path.OpticalPathIdentifier.value), "A");
	optical_path.OpticalPathDescription.present = true;
	snprintf(optical_path.OpticalPathDescription.value,
	         sizeof(optical_path.OpticalPathDescription.value), "Brightfield path");
	optical_path.ColorSpace.present = true;
	snprintf(optical_path.ColorSpace.value,
	         sizeof(optical_path.ColorSpace.value), "SRGB");
	optical_path.illumination_type.CodeValue.present = true;
	snprintf(optical_path.illumination_type.CodeValue.value,
	         sizeof(optical_path.illumination_type.CodeValue.value), "111744");
	optical_path.illumination_type.CodingSchemeDesignator.present = true;
	snprintf(optical_path.illumination_type.CodingSchemeDesignator.value,
	         sizeof(optical_path.illumination_type.CodingSchemeDesignator.value),
	         "DCM");
	optical_path.illumination_type.CodeMeaning.present = true;
	snprintf(optical_path.illumination_type.CodeMeaning.value,
	         sizeof(optical_path.illumination_type.CodeMeaning.value),
	         "Brightfield illumination");
	optical_path.icc_profile = (u8*)icc_profile;
	optical_path.icc_profile_size = sizeof(icc_profile);
	optical_path.light_path_filter_pass_through_wavelength = 550;
	optical_path.light_path_filter_pass_band[0] = 500;
	optical_path.light_path_filter_pass_band[1] = 600;
	optical_path.image_path_filter_pass_through_wavelength = 560;
	optical_path.image_path_filter_pass_band[0] = 510;
	optical_path.image_path_filter_pass_band[1] = 610;
	optical_path.objective_lens_power = 40.0f;
	optical_path.objective_lens_numerical_aperture = 0.95f;
	optical_path.present_fields =
		DICOM_OPTICAL_LIGHT_FILTER_PASS |
		DICOM_OPTICAL_LIGHT_FILTER_BAND |
		DICOM_OPTICAL_IMAGE_FILTER_PASS |
		DICOM_OPTICAL_IMAGE_FILTER_BAND |
		DICOM_OPTICAL_OBJECTIVE_POWER |
		DICOM_OPTICAL_OBJECTIVE_APERTURE;
	arrput(source.optical_paths, optical_path);
	source.ContainerIdentifier.present = true;
	snprintf(source.ContainerIdentifier.value,
	         sizeof(source.ContainerIdentifier.value), "SLIDE-01");
	dicom_specimen_t specimen = {};
	specimen.SpecimenIdentifier.present = true;
	snprintf(specimen.SpecimenIdentifier.value,
	         sizeof(specimen.SpecimenIdentifier.value), "SPECIMEN-01");
	snprintf(specimen.SpecimenUID.value, sizeof(specimen.SpecimenUID.value),
	         "1.2.3.4.5");
	specimen.SpecimenUID.present = true;
	specimen.SpecimenUID.len = strlen(specimen.SpecimenUID.value);
	specimen.SpecimenShortDescription.present = true;
	snprintf(specimen.SpecimenShortDescription.value,
	         sizeof(specimen.SpecimenShortDescription.value), "H&E section");
	auto content_string = [](const char* value) {
		size_t size = strlen(value) + 1;
		char* copy = (char*)malloc(size);
		if (copy) memcpy(copy, value, size);
		return copy;
	};
	dicom_specimen_preparation_step_t preparation_step = {};
	dicom_specimen_preparation_item_t collection_method = {};
	collection_method.parent_index = UINT32_MAX;
	collection_method.value_type = DICOM_CONTENT_VALUE_CODE;
	collection_method.concept_name.value = content_string("111701");
	collection_method.concept_name.coding_scheme = content_string("DCM");
	collection_method.concept_name.meaning = content_string("Processing type");
	collection_method.code_value.value = content_string("17636008");
	collection_method.code_value.coding_scheme = content_string("SCT");
	collection_method.code_value.coding_scheme_version =
		content_string("2026");
	collection_method.code_value.meaning = content_string("Specimen collection");
	arrput(preparation_step.items, collection_method);
	dicom_specimen_preparation_item_t stain_text = {};
	stain_text.parent_index = 0;
	stain_text.value_type = DICOM_CONTENT_VALUE_TEXT;
	stain_text.relationship_type = content_string("HAS PROPERTIES");
	stain_text.concept_name.value = content_string("424361007");
	stain_text.concept_name.coding_scheme = content_string("SCT");
	stain_text.concept_name.meaning = content_string("Using substance");
	stain_text.scalar_value = content_string("hematoxylin and eosin");
	arrput(preparation_step.items, stain_text);
	dicom_specimen_preparation_item_t thickness = {};
	thickness.parent_index = UINT32_MAX;
	thickness.value_type = DICOM_CONTENT_VALUE_NUM;
	thickness.concept_name.value = content_string("118570006");
	thickness.concept_name.coding_scheme = content_string("SCT");
	thickness.concept_name.meaning = content_string("Tissue section thickness");
	thickness.scalar_value = content_string("4.5");
	thickness.measurement_units.value = content_string("um");
	thickness.measurement_units.coding_scheme = content_string("UCUM");
	thickness.measurement_units.meaning = content_string("micrometer");
	arrput(preparation_step.items, thickness);
	dicom_specimen_preparation_item_t tracking_uid = {};
	tracking_uid.parent_index = UINT32_MAX;
	tracking_uid.value_type = DICOM_CONTENT_VALUE_UID;
	tracking_uid.concept_name.value = content_string("112039");
	tracking_uid.concept_name.coding_scheme = content_string("DCM");
	tracking_uid.concept_name.meaning = content_string("Tracking Unique Identifier");
	tracking_uid.scalar_value = content_string("1.2.826.0.1.3680043.10.1000");
	arrput(preparation_step.items, tracking_uid);
	arrput(specimen.preparation_steps, preparation_step);
	arrput(source.specimens, specimen);

	archive_shell_build_part10_input_t input = {};
	char recipe[ARCHIVE_SHELL_RECIPE_CAPACITY] = {};
	size_t recipe_size = 0;
	char fingerprint[65] = {};
	archive_shell_part10_recipe_input_t recipe_input = {
		.bundle_sha256 =
			"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		.source_sha256 =
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		.source_byte_size = sizeof(dicom_header_t) + source.pixel_data.data_offset +
		                    source.pixel_data.length,
		.sop_instance_uid = source.sop_instance_uid.value,
	};
	REQUIRE(archive_shell_build_part10_recipe(
		&recipe_input, recipe, &recipe_size, fingerprint));
	CHECK(recipe_size == strlen(recipe));
	CHECK(strstr(recipe, "adapter-policy\tpart10-delta-payload-v7\n") != nullptr);
	char replay_recipe[ARCHIVE_SHELL_RECIPE_CAPACITY] = {};
	size_t replay_recipe_size = 0;
	char replay_fingerprint[65] = {};
	REQUIRE(archive_shell_build_part10_recipe(
		&recipe_input, replay_recipe, &replay_recipe_size, replay_fingerprint));
	CHECK(replay_recipe_size == recipe_size);
	CHECK(memcmp(replay_recipe, recipe, recipe_size) == 0);
	CHECK(strcmp(replay_fingerprint, fingerprint) == 0);
	++recipe_input.source_byte_size;
	char changed_recipe[ARCHIVE_SHELL_RECIPE_CAPACITY] = {};
	size_t changed_recipe_size = 0;
	char changed_fingerprint[65] = {};
	REQUIRE(archive_shell_build_part10_recipe(
		&recipe_input, changed_recipe, &changed_recipe_size, changed_fingerprint));
	CHECK(strcmp(changed_fingerprint, fingerprint) != 0);
	--recipe_input.source_byte_size;
	input.instance = &source;
	input.source_sha256 =
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	input.source_byte_size = sizeof(dicom_header_t) + source.pixel_data.data_offset +
	                         source.pixel_data.length;
	input.representation_fingerprint = fingerprint;
	archive_shell_buffer_t encoded = {};
	char error[512] = {};
	REQUIRE(archive_shell_build_part10(&input, &encoded, error, sizeof(error)));
	CHECK(encoded.size > ARCHIVE_SHELL_HEADER_SIZE);

	archive_shell_view_t shell = {};
	REQUIRE(archive_shell_open(&shell, encoded.data, encoded.size, error, sizeof(error)));
	CHECK(shell.source_count == 1);
	CHECK(shell.instance_count == 1);
	char first_fingerprint_byte[3] = {fingerprint[0], fingerprint[1], '\0'};
	CHECK(shell.representation_fingerprint[0] ==
	      (u8)strtoul(first_fingerprint_byte, nullptr, 16));
	archive_shell_source_t stored_source = {};
	REQUIRE(archive_shell_get_source(&shell, 0, &stored_source));
	CHECK(stored_source.byte_size == input.source_byte_size);
	CHECK(stored_source.sha256[0] == 0xaa);

	archive_shell_instance_t instance = {};
	REQUIRE(archive_shell_get_instance(&shell, 0, &instance));
	CHECK(strcmp(instance.sop_instance_uid, "1.2.3.1.1") == 0);
	CHECK(instance.provider_kind == ARCHIVE_SHELL_PROVIDER_PART10);
	CHECK(instance.frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID);
	CHECK(instance.payload_map_kind == ARCHIVE_SHELL_PAYLOAD_FIXED_STRIDE);
	CHECK(instance.tiles_per_row == 3);
	CHECK(instance.tiles_per_column == 2);
	CHECK(instance.has_slide_coordinate_origin);
	CHECK(instance.has_image_orientation_slide);
	CHECK(archive_shell_get_attribute_count(&shell, &instance) == 22);
	CHECK(archive_shell_get_governed_attribute_count(&shell) == 0);
	CHECK(archive_shell_get_attribute_override_count(
		      &shell, &instance) == 22);
	archive_shell_attribute_t attribute = {};
	REQUIRE(archive_shell_get_attribute(&shell, &instance, 0, &attribute));
	CHECK(attribute.tag == 0x00080016);
	CHECK(strcmp(attribute.vr, "UI") == 0);
	CHECK(strcmp(attribute.string_value, source.sop_class_uid.value) == 0);
	bool found_patient_id = false;
	bool found_patient_name = false;
	bool found_study_date = false;
	for (u32 i = 0; i < archive_shell_get_attribute_count(&shell, &instance); ++i) {
		REQUIRE(archive_shell_get_attribute(&shell, &instance, i, &attribute));
		if (attribute.tag == 0x00100020) {
			CAPTURE(attribute.string_value);
			CHECK(strcmp(attribute.string_value, "PATIENT-01") == 0);
			found_patient_id = true;
		}
		if (attribute.tag == 0x00100010) {
			CAPTURE(attribute.string_value);
			CHECK(strcmp(attribute.vr, "PN") == 0);
			CHECK(strcmp(attribute.string_value,
			             "Family^Given=Ideographic=Phonetic") == 0);
			found_patient_name = true;
		}
		if (attribute.tag == 0x00080020) {
			CAPTURE(attribute.string_value);
			CHECK(strcmp(attribute.string_value, "20260725") == 0);
			found_study_date = true;
		}
	}
	CHECK(found_patient_id);
	CHECK(found_patient_name);
	CHECK(found_study_date);

	archive_shell_frame_t frame = {};
	REQUIRE(archive_shell_get_frame(&shell, &instance, 5, &frame));
	CHECK(frame.row_position == 3);
	CHECK(frame.column_position == 5);
	archive_shell_payload_range_t payload = {};
	REQUIRE(archive_shell_get_payload_range(&shell, &instance, 5, &payload));
	CHECK(payload.source_index == 0);
	CHECK(payload.size == 12);
	CHECK(payload.offset == sizeof(dicom_header_t) + source.pixel_data.data_offset + 60);

	dicom_instance_t compact_source = source;
	u32 compact_offsets[6] = {0, 20, 45, 75, 110, 150};
	u32 compact_sizes[6] = {16, 25, 26, 35, 40, 30};
	compact_source.is_pixel_data_encapsulated = true;
	compact_source.are_all_offsets_read = true;
	compact_source.pixel_data_offset_count = COUNT(compact_offsets);
	compact_source.pixel_data_offsets = compact_offsets;
	compact_source.pixel_data_sizes = compact_sizes;
	compact_source.pixel_data_start_offset = 256;
	compact_source.file_handle = 0;
	archive_shell_build_part10_input_t compact_input = input;
	compact_input.instance = &compact_source;
	compact_input.source_byte_size = 1024;
	archive_shell_buffer_t compact_encoded = {};
	REQUIRE(archive_shell_build_part10(
		&compact_input, &compact_encoded, error, sizeof(error)));
	archive_shell_view_t compact_shell = {};
	REQUIRE(archive_shell_open(
		&compact_shell, compact_encoded.data, compact_encoded.size,
		error, sizeof(error)));
	archive_shell_instance_t compact_instance = {};
	REQUIRE(archive_shell_get_instance(
		&compact_shell, 0, &compact_instance));
	CHECK(compact_instance.payload_map_kind ==
	      ARCHIVE_SHELL_PAYLOAD_DELTA_OFFSETS);
	for (u32 frame_index = 0; frame_index < COUNT(compact_offsets);
	     ++frame_index) {
		archive_shell_payload_range_t compact_payload = {};
		REQUIRE(archive_shell_get_payload_range(
			&compact_shell, &compact_instance, frame_index,
			&compact_payload));
		CHECK(compact_payload.source_index == 0);
		CHECK(compact_payload.offset ==
		      sizeof(dicom_header_t) +
			      compact_source.pixel_data_start_offset +
			      compact_offsets[frame_index]);
		CHECK(compact_payload.size == compact_sizes[frame_index]);
	}
	archive_shell_buffer_destroy(&compact_encoded);

	archive_shell_optical_path_t stored_path = {};
	REQUIRE(archive_shell_get_optical_path(&shell, &instance, 0, &stored_path));
	CHECK(strcmp(stored_path.identifier, "A") == 0);
	CHECK(strcmp(stored_path.description, "Brightfield path") == 0);
	CHECK(strcmp(stored_path.color_space, "SRGB") == 0);
	CHECK(strcmp(stored_path.illumination_type_code_value, "111744") == 0);
	CHECK(strcmp(stored_path.illumination_type_coding_scheme, "DCM") == 0);
	CHECK(strcmp(stored_path.illumination_type_code_meaning,
	             "Brightfield illumination") == 0);
	CHECK(stored_path.icc_profile_size == sizeof(icc_profile));
	CHECK(memcmp(stored_path.icc_profile, icc_profile, sizeof(icc_profile)) == 0);
	CHECK(stored_path.light_path_filter_pass_through_wavelength == 550);
	CHECK(stored_path.light_path_filter_pass_band[0] == 500);
	CHECK(stored_path.light_path_filter_pass_band[1] == 600);
	CHECK(stored_path.image_path_filter_pass_through_wavelength == 560);
	CHECK(stored_path.image_path_filter_pass_band[0] == 510);
	CHECK(stored_path.image_path_filter_pass_band[1] == 610);
	CHECK(stored_path.objective_lens_power == doctest::Approx(40.0f));
	CHECK(stored_path.objective_lens_numerical_aperture == doctest::Approx(0.95f));
	CHECK((stored_path.source_preserved_fields &
	       ARCHIVE_SHELL_OPTICAL_IDENTIFIER) != 0);
	CHECK((stored_path.source_preserved_fields &
	       ARCHIVE_SHELL_OPTICAL_DESCRIPTION) != 0);
	CHECK((stored_path.source_preserved_fields &
	       ARCHIVE_SHELL_OPTICAL_ICC_PROFILE) != 0);
	CHECK(stored_path.inferred_fields == 0);
	CHECK(strcmp(archive_shell_get_container_identifier(&shell, &instance),
	             "SLIDE-01") == 0);
	REQUIRE(archive_shell_get_specimen_count(&shell, &instance) == 1);
	archive_shell_specimen_t stored_specimen = {};
	REQUIRE(archive_shell_get_specimen(&shell, &instance, 0, &stored_specimen));
	CHECK(strcmp(stored_specimen.identifier, "SPECIMEN-01") == 0);
	CHECK(strcmp(stored_specimen.uid, "1.2.3.4.5") == 0);
	CHECK(strcmp(stored_specimen.short_description, "H&E section") == 0);
	CHECK_FALSE(archive_shell_specimen_metadata_is_shared(&shell));
	CHECK_FALSE(archive_shell_preparation_metadata_is_shared(&shell));
	REQUIRE(archive_shell_get_preparation_item_count(&shell, &instance) == 4);
	archive_shell_preparation_item_t stored_preparation = {};
	REQUIRE(archive_shell_get_preparation_item(
		&shell, &instance, 0, &stored_preparation));
	CHECK(stored_preparation.specimen_index == 0);
	CHECK(stored_preparation.step_index == 0);
	CHECK(stored_preparation.parent_item_index == UINT32_MAX);
	CHECK(stored_preparation.value_kind == ARCHIVE_SHELL_CONTENT_CODE);
	CHECK(strcmp(stored_preparation.concept_name_code_value, "111701") == 0);
	CHECK(strcmp(stored_preparation.code_value, "17636008") == 0);
	CHECK(strcmp(stored_preparation.code_coding_scheme, "SCT") == 0);
	CHECK(strcmp(stored_preparation.code_coding_scheme_version,
	             "2026") == 0);
	REQUIRE(archive_shell_get_preparation_item(
		&shell, &instance, 1, &stored_preparation));
	CHECK(stored_preparation.parent_item_index == 0);
	CHECK(stored_preparation.value_kind == ARCHIVE_SHELL_CONTENT_TEXT);
	CHECK(strcmp(stored_preparation.relationship_type,
	             "HAS PROPERTIES") == 0);
	CHECK(strcmp(stored_preparation.scalar_value,
	             "hematoxylin and eosin") == 0);
	REQUIRE(archive_shell_get_preparation_item(
		&shell, &instance, 2, &stored_preparation));
	CHECK(stored_preparation.value_kind == ARCHIVE_SHELL_CONTENT_NUM);
	CHECK(strcmp(stored_preparation.scalar_value, "4.5") == 0);
	CHECK(strcmp(stored_preparation.measurement_units_code_value, "um") == 0);
	CHECK(strcmp(stored_preparation.measurement_units_coding_scheme,
	             "UCUM") == 0);
	REQUIRE(archive_shell_get_preparation_item(
		&shell, &instance, 3, &stored_preparation));
	CHECK(stored_preparation.value_kind == ARCHIVE_SHELL_CONTENT_UID);
	CHECK(strcmp(stored_preparation.scalar_value,
	             "1.2.826.0.1.3680043.10.1000") == 0);

	dicom_instance_t second_source = source;
	second_source.specimens = nullptr;
	dicom_specimen_t second_specimen = source.specimens[0];
	arrput(second_source.specimens, second_specimen);
	snprintf(second_source.sop_instance_uid.value,
	         sizeof(second_source.sop_instance_uid.value), "1.2.3.1.2");
	second_source.sop_instance_uid.len =
		strlen(second_source.sop_instance_uid.value);
	snprintf(second_source.patient.PatientID.value,
	         sizeof(second_source.patient.PatientID.value), "PATIENT-01");
	second_source.instance_number = 2;
	archive_shell_part10_recipe_input_t aggregate_recipe_instances[2] = {
		recipe_input,
		{
			.bundle_sha256 = recipe_input.bundle_sha256,
			.source_sha256 =
				"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			.source_byte_size = input.source_byte_size,
			.sop_instance_uid = second_source.sop_instance_uid.value,
		},
	};
	archive_shell_part10_aggregate_recipe_input_t aggregate_recipe_input = {
		.bundle_sha256 = recipe_input.bundle_sha256,
		.study_instance_uid = source.general_study.StudyInstanceUID.value,
		.series_instance_uid = source.general_series.SeriesInstanceUID.value,
		.instances = aggregate_recipe_instances,
		.instance_count = 2,
	};
	char* aggregate_recipe = nullptr;
	size_t aggregate_recipe_size = 0;
	char aggregate_fingerprint[65] = {};
	REQUIRE(archive_shell_build_part10_aggregate_recipe(
		&aggregate_recipe_input, &aggregate_recipe, &aggregate_recipe_size,
		aggregate_fingerprint));
	CHECK(strstr(aggregate_recipe,
	             "grouping-policy\tpart10-study-series-v1\n") != nullptr);
	CHECK(strstr(aggregate_recipe,
	             "adapter-policy\tpart10-delta-payload-v7\n") != nullptr);
	archive_shell_build_part10_input_t aggregate_instances[2] = {
		input,
		{
			.instance = &second_source,
			.source_sha256 = aggregate_recipe_instances[1].source_sha256,
			.source_byte_size = input.source_byte_size,
		},
	};
	archive_shell_build_part10_aggregate_input_t aggregate_input = {
		.instances = aggregate_instances,
		.instance_count = 2,
		.representation_fingerprint = aggregate_fingerprint,
	};
	archive_shell_buffer_t aggregate_encoded = {};
	REQUIRE(archive_shell_build_part10_aggregate(
		&aggregate_input, &aggregate_encoded, error, sizeof(error)));
	archive_shell_view_t aggregate_shell = {};
	REQUIRE(archive_shell_open(
		&aggregate_shell, aggregate_encoded.data, aggregate_encoded.size,
		error, sizeof(error)));
	CHECK(aggregate_shell.source_count == 2);
	CHECK(aggregate_shell.instance_count == 2);
	archive_shell_instance_t aggregate_first = {};
	archive_shell_instance_t aggregate_second = {};
	REQUIRE(archive_shell_get_instance(
		&aggregate_shell, 0, &aggregate_first));
	REQUIRE(archive_shell_get_instance(
		&aggregate_shell, 1, &aggregate_second));
	CHECK(strcmp(aggregate_first.sop_instance_uid, "1.2.3.1.1") == 0);
	CHECK(strcmp(aggregate_second.sop_instance_uid, "1.2.3.1.2") == 0);
	CHECK(aggregate_first.source_index == 0);
	CHECK(aggregate_second.source_index == 1);
	CHECK(archive_shell_get_governed_attribute_count(
		      &aggregate_shell) == 10);
	CHECK(archive_shell_get_attribute_override_count(
		      &aggregate_shell, &aggregate_first) == 12);
	CHECK(archive_shell_get_attribute_override_count(
		      &aggregate_shell, &aggregate_second) == 12);
	CHECK(archive_shell_get_attribute_count(
		      &aggregate_shell, &aggregate_first) == 22);
	CHECK(archive_shell_specimen_metadata_is_shared(&aggregate_shell));
	CHECK(archive_shell_preparation_metadata_is_shared(&aggregate_shell));
	bool found_second_patient_id = false;
	for (u32 i = 0;
	     i < archive_shell_get_attribute_count(
		     &aggregate_shell, &aggregate_second); ++i) {
		REQUIRE(archive_shell_get_attribute(
			&aggregate_shell, &aggregate_second, i, &attribute));
		if (attribute.tag == 0x00100020) {
			CHECK(strcmp(attribute.string_value, "PATIENT-01") == 0);
			found_second_patient_id = true;
		}
	}
	CHECK(found_second_patient_id);
	REQUIRE(archive_shell_get_payload_range(
		&aggregate_shell, &aggregate_second, 5, &payload));
	CHECK(payload.source_index == 1);
	CHECK(strcmp(archive_shell_get_container_identifier(
		             &aggregate_shell, &aggregate_second),
	             "SLIDE-01") == 0);
	REQUIRE(archive_shell_get_preparation_item_count(
		&aggregate_shell, &aggregate_second) == 4);
	REQUIRE(archive_shell_get_preparation_item(
		&aggregate_shell, &aggregate_second, 1, &stored_preparation));
	CHECK(stored_preparation.parent_item_index == 0);
	CHECK(strcmp(stored_preparation.scalar_value,
	             "hematoxylin and eosin") == 0);
	snprintf(second_source.patient.PatientID.value,
	         sizeof(second_source.patient.PatientID.value), "PATIENT-02");
	archive_shell_buffer_t conflicting_aggregate = {};
	CHECK_FALSE(archive_shell_build_part10_aggregate(
		&aggregate_input, &conflicting_aggregate, error, sizeof(error)));
	CHECK(strstr(error, "00100020") != nullptr);
	archive_shell_buffer_destroy(&conflicting_aggregate);
	snprintf(second_source.patient.PatientID.value,
	         sizeof(second_source.patient.PatientID.value), "PATIENT-01");
	snprintf(second_source.specimens[0].SpecimenIdentifier.value,
	         sizeof(second_source.specimens[0].SpecimenIdentifier.value),
	         "SPECIMEN-CONFLICT");
	CHECK_FALSE(archive_shell_build_part10_aggregate(
		&aggregate_input, &conflicting_aggregate, error, sizeof(error)));
	CHECK(strstr(error, "container or specimen") != nullptr);
	archive_shell_buffer_destroy(&conflicting_aggregate);
	archive_shell_buffer_destroy(&aggregate_encoded);
	free(aggregate_recipe);

	CHECK_FALSE(archive_shell_open(&shell, encoded.data, encoded.size - 1,
	                               error, sizeof(error)));
	u8 saved_offset_byte = encoded.data[ARCHIVE_SHELL_HEADER_SIZE + 16];
	encoded.data[ARCHIVE_SHELL_HEADER_SIZE + 16] = saved_offset_byte + 1;
	CHECK_FALSE(archive_shell_open(&shell, encoded.data, encoded.size,
	                               error, sizeof(error)));
	encoded.data[ARCHIVE_SHELL_HEADER_SIZE + 16] = saved_offset_byte;
	encoded.data[8] = 99;
	CHECK_FALSE(archive_shell_open(&shell, encoded.data, encoded.size, error, sizeof(error)));
	encoded.data[8] = ARCHIVE_SHELL_FORMAT_VERSION;
	archive_shell_buffer_destroy(&encoded);
	icc_profile[36] = 'x';
	CHECK_FALSE(archive_shell_build_part10(&input, &encoded, error, sizeof(error)));
	CHECK(strstr(error, "invalid ICC profile") != nullptr);
	for (i32 step_index = 0;
	     step_index < arrlen(source.specimens[0].preparation_steps);
	     ++step_index) {
		dicom_specimen_preparation_step_t* step =
			source.specimens[0].preparation_steps + step_index;
		for (i32 item_index = 0; item_index < arrlen(step->items);
		     ++item_index) {
			dicom_specimen_preparation_item_t* item = step->items + item_index;
			free(item->relationship_type);
			free(item->concept_name.value);
			free(item->concept_name.coding_scheme);
			free(item->concept_name.coding_scheme_version);
			free(item->concept_name.meaning);
			free(item->code_value.value);
			free(item->code_value.coding_scheme);
			free(item->code_value.coding_scheme_version);
			free(item->code_value.meaning);
			free(item->measurement_units.value);
			free(item->measurement_units.coding_scheme);
			free(item->measurement_units.coding_scheme_version);
			free(item->measurement_units.meaning);
			free(item->scalar_value);
		}
		arrfree(step->items);
	}
	arrfree(source.specimens[0].preparation_steps);
	arrfree(second_source.specimens);
	arrfree(source.specimens);
	arrfree(source.optical_paths);
}

TEST_CASE("virtual DICOM shell bounds a final encapsulated frame before trailing elements") {
	char temp_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	char temp_base[L_tmpnam] = {0};
	REQUIRE(tmpnam_s(temp_base, sizeof(temp_base)) == 0);
	snprintf(temp_path, sizeof(temp_path), "%s-last-frame.dcm", temp_base);
#else
	int descriptor = slidescape_test_make_temp_file(
		temp_path, sizeof(temp_path), "slidescape-last-frame-test");
	REQUIRE(descriptor >= 0);
	close(descriptor);
#endif
	constexpr size_t source_size = 512;
	constexpr size_t frame_offset = 256;
	u8 bytes[source_size] = {};
	bytes[frame_offset + 0] = 0xfe;
	bytes[frame_offset + 1] = 0xff;
	bytes[frame_offset + 2] = 0x00;
	bytes[frame_offset + 3] = 0xe0;
	bytes[frame_offset + 4] = 4;
	bytes[frame_offset + 8] = 0xff;
	bytes[frame_offset + 9] = 0x4f;
	bytes[frame_offset + 10] = 0xff;
	bytes[frame_offset + 11] = 0xd9;
	bytes[frame_offset + 12] = 0xfe;
	bytes[frame_offset + 13] = 0xff;
	bytes[frame_offset + 14] = 0xdd;
	bytes[frame_offset + 15] = 0xe0;
	FILE* file = fopen(temp_path, "wb");
	REQUIRE(file != nullptr);
	REQUIRE(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
	REQUIRE(fclose(file) == 0);

	dicom_instance_t source = {};
#define SET_ENCAPSULATED_UI(field, text) \
	do { snprintf((field).value, sizeof((field).value), "%s", text); \
	     (field).len = strlen((field).value); } while (0)
	SET_ENCAPSULATED_UI(source.general_study.StudyInstanceUID, "1.2.3");
	SET_ENCAPSULATED_UI(source.general_series.SeriesInstanceUID, "1.2.3.1");
	SET_ENCAPSULATED_UI(source.sop_instance_uid, "1.2.3.1.2");
	SET_ENCAPSULATED_UI(source.sop_class_uid, "1.2.840.10008.5.1.4.1.1.77.1.6");
	SET_ENCAPSULATED_UI(source.transfer_syntax_uid, "1.2.840.10008.1.2.4.91");
#undef SET_ENCAPSULATED_UI
	snprintf(source.general_series.Modality.value,
	         sizeof(source.general_series.Modality.value), "SM");
	source.image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
	source.photometric_interpretation = DICOM_PHOTOMETRIC_INTERPRETATION_RGB;
	source.number_of_frames = 1;
	source.rows = 2;
	source.columns = 2;
	source.samples_per_pixel = 3;
	source.bits_allocated = 8;
	source.bits_stored = 8;
	source.high_bit = 7;
	source.total_pixel_matrix_rows = 2;
	source.total_pixel_matrix_columns = 2;
	source.number_of_optical_paths = 1;
	source.total_pixel_matrix_focal_planes = 1;
	source.is_tiled_full = true;
	source.found_pixel_data = true;
	source.is_pixel_data_encapsulated = true;
	source.are_all_offsets_read = true;
	source.pixel_data_start_offset = (u32)(frame_offset - sizeof(dicom_header_t));
	source.pixel_data_offset_count = 1;
	source.pixel_data_offsets = (u32*)calloc(1, sizeof(u32));
	source.pixel_data_sizes = (u32*)malloc(sizeof(u32));
	REQUIRE(source.pixel_data_offsets != nullptr);
	REQUIRE(source.pixel_data_sizes != nullptr);
	/* Reproduce the legacy parser estimate that extends through trailing elements. */
	source.pixel_data_sizes[0] = (u32)(source_size - frame_offset);
	source.file_handle = open_file_handle_for_simultaneous_access(temp_path);

	archive_shell_build_part10_input_t input = {
		.instance = &source,
		.source_sha256 =
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		.source_byte_size = source_size,
		.representation_fingerprint =
			"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	};
	char error[512] = {};
	archive_shell_buffer_t encoded = {};
	REQUIRE(archive_shell_build_part10(&input, &encoded, error, sizeof(error)));
	archive_shell_view_t shell = {};
	REQUIRE(archive_shell_open(&shell, encoded.data, encoded.size, error, sizeof(error)));
	archive_shell_instance_t instance = {};
	REQUIRE(archive_shell_get_instance(&shell, 0, &instance));
	archive_shell_payload_range_t payload = {};
	REQUIRE(archive_shell_get_payload_range(&shell, &instance, 0, &payload));
	CHECK(payload.offset == frame_offset);
	CHECK(payload.size == 12);

	archive_shell_buffer_destroy(&encoded);
	file_handle_close(source.file_handle);
	free(source.pixel_data_offsets);
	free(source.pixel_data_sizes);
	CHECK(remove(temp_path) == 0);
}

TEST_CASE("explicit Part 10 frame maps preserve DICOM tile and dimension order") {
	dicom_instance_t source = {};
	source.number_of_frames = 8;
	source.instance_number = 1;
	source.rows = 240;
	source.columns = 240;
	source.total_pixel_matrix_rows = 240;
	source.total_pixel_matrix_columns = 480;
	source.number_of_optical_paths = 2;
	source.total_pixel_matrix_focal_planes = 2;
	source.is_tiled_full = true;
	source.image_flavor = DICOM_IMAGE_FLAVOR_VOLUME;
	source.photometric_interpretation = DICOM_PHOTOMETRIC_INTERPRETATION_RGB;
	source.pixel_spacing = V2F(0.001f, 0.002f);
	snprintf(source.general_study.StudyInstanceUID.value,
	         sizeof(source.general_study.StudyInstanceUID.value), "1.2.3");
	snprintf(source.general_series.SeriesInstanceUID.value,
	         sizeof(source.general_series.SeriesInstanceUID.value), "1.2.3.1");
	snprintf(source.sop_instance_uid.value, sizeof(source.sop_instance_uid.value),
	         "1.2.3.1.1");
	snprintf(source.sop_class_uid.value, sizeof(source.sop_class_uid.value),
	         "1.2.840.10008.5.1.4.1.1.77.1.6");
	snprintf(source.transfer_syntax_uid.value, sizeof(source.transfer_syntax_uid.value),
	         "1.2.840.10008.1.2.4.91");
	source.is_total_pixel_matrix_origin_known = true;
	source.total_pixel_matrix_origin_x_mm = 10.0f;
	source.total_pixel_matrix_origin_y_mm = 20.0f;
	source.is_image_orientation_slide_known = true;
	source.image_orientation_slide[0] = 1.0f;
	source.image_orientation_slide[4] = 1.0f;
	for (i32 i = 0; i < 2; ++i) {
		dicom_optical_path_t optical_path = {};
		optical_path.OpticalPathIdentifier.present = true;
		snprintf(optical_path.OpticalPathIdentifier.value,
		         sizeof(optical_path.OpticalPathIdentifier.value), "%c", 'A' + i);
		arrput(source.optical_paths, optical_path);
	}

	char error[512] = {};
	archive_frame_map_t map = {};
	REQUIRE(archive_frame_map_build_part10(&map, &source, error, sizeof(error)));
	REQUIRE(map.frame_count == 8);
	CHECK(map.entries[0].column_position == 1);
	CHECK(map.entries[0].row_position == 1);
	CHECK(map.entries[0].focal_plane_index == 0);
	CHECK(map.entries[0].optical_path_index == 0);
	CHECK(map.entries[1].column_position == 241);
	CHECK(map.entries[1].focal_plane_index == 0);
	CHECK(map.entries[2].column_position == 1);
	CHECK(map.entries[2].focal_plane_index == 1);
	CHECK(map.entries[4].focal_plane_index == 0);
	CHECK(map.entries[4].optical_path_index == 1);
	CHECK(map.entries[7].column_position == 241);
	CHECK(map.entries[7].focal_plane_index == 1);
	CHECK(map.entries[7].optical_path_index == 1);
	CHECK(map.entries[1].x_offset_mm == doctest::Approx(10.48f));
	CHECK(map.entries[1].y_offset_mm == doctest::Approx(20.0f));

	size_t encoded_size = ARCHIVE_FRAME_MAP_HEADER_SIZE +
		(size_t)map.frame_count * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
	u8* encoded = (u8*)malloc(encoded_size);
	REQUIRE(encoded != nullptr);
	size_t written = 0;
	REQUIRE(archive_frame_map_serialize(&map, encoded, encoded_size, &written,
	                                   error, sizeof(error)));
	CHECK(written == encoded_size);
	archive_frame_map_t parsed = {};
	REQUIRE(archive_frame_map_parse(&parsed, encoded, encoded_size, error, sizeof(error)));
	CHECK(parsed.frame_count == map.frame_count);
	CHECK(parsed.entries[7].optical_path_index == 1);
	CHECK(parsed.entries[7].focal_plane_index == 1);
	encoded[ARCHIVE_FRAME_MAP_HEADER_SIZE + 20] = 0xff;
	archive_frame_map_t invalid = {};
	CHECK_FALSE(archive_frame_map_parse(&invalid, encoded, encoded_size,
	                                    error, sizeof(error)));

	char temp_path[ARCHIVE_PATH_CAPACITY];
#if defined(_WIN32)
	char temp_base[L_tmpnam] = {0};
	REQUIRE(tmpnam_s(temp_base, sizeof(temp_base)) == 0);
	snprintf(temp_path, sizeof(temp_path), "%s-frame-map", temp_base);
#else
	REQUIRE(slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-frame-map-test"));
#endif
	archive_repository_t repository = {};
	REQUIRE(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	        ARCHIVE_REPOSITORY_OK);
	archive_virtual_instance_manifest_t manifest = {};
	snprintf(manifest.representation_id, sizeof(manifest.representation_id),
	         "multidimensional-volume");
	snprintf(manifest.bundle_id, sizeof(manifest.bundle_id), "bundle-1");
	manifest.representation_version = 1;
	snprintf(manifest.provider_locator, sizeof(manifest.provider_locator),
	         "objects/test.dcm");
	REQUIRE(archive_manifest_materialize_part10_frame_map(
		&repository, &manifest, &source, error, sizeof(error)));
	CHECK(manifest.frame_map_kind == ARCHIVE_MANIFEST_FRAME_MAP_EXPLICIT);
	CHECK(strcmp(manifest.frame_map_object_key,
	             manifest.frame_dimension_map_object_key) == 0);
	CHECK(archive_virtual_instance_manifest_is_valid(&manifest));
	REQUIRE(archive_manifest_stage_virtual_instance(
		&repository, &manifest, error, sizeof(error)));
	char staged_map_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, manifest.frame_map_object_key,
	                                     staged_map_path, sizeof(staged_map_path)));
	FILE* staged_map = fopen(staged_map_path, "rb");
	REQUIRE(staged_map != nullptr);
	CHECK(fseek(staged_map, 0, SEEK_END) == 0);
	CHECK(ftell(staged_map) == (long)encoded_size);
	fclose(staged_map);
	CHECK_FALSE(archive_manifest_materialize_part10_frame_map(
		&repository, &manifest, &source, error, sizeof(error)));

	char staged_manifest_path[ARCHIVE_PATH_CAPACITY];
	snprintf(staged_manifest_path, sizeof(staged_manifest_path),
	         "%s/staging/%s.representation.json", temp_path,
	         manifest.representation_id);
	CHECK(remove(staged_manifest_path) == 0);
	CHECK(remove(staged_map_path) == 0);
	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		char child_path[ARCHIVE_PATH_CAPACITY];
		snprintf(child_path, sizeof(child_path), "%s/%s", temp_path, child);
		CHECK(archive_test_rmdir(child_path) == 0);
	}
	char descriptor_path[ARCHIVE_PATH_CAPACITY];
	snprintf(descriptor_path, sizeof(descriptor_path), "%s/repository.json", temp_path);
	CHECK(remove(descriptor_path) == 0);
	archive_test_remove_database(temp_path);
	CHECK(archive_test_rmdir(temp_path) == 0);

	source.number_of_frames = 2;
	source.concatenation_uid.len = 5;
	snprintf(source.concatenation_uid.value, sizeof(source.concatenation_uid.value), "1.2.9");
	source.concatenation_frame_offset_number = 4;
	archive_frame_map_t concatenation_map = {};
	REQUIRE(archive_frame_map_build_part10(
		&concatenation_map, &source, error, sizeof(error)));
	CHECK(concatenation_map.entries[0].column_position == 1);
	CHECK(concatenation_map.entries[0].focal_plane_index == 0);
	CHECK(concatenation_map.entries[0].optical_path_index == 1);
	CHECK(concatenation_map.entries[1].column_position == 241);
	CHECK(concatenation_map.entries[1].optical_path_index == 1);
	archive_frame_map_destroy(&concatenation_map);

	free(encoded);
	archive_frame_map_destroy(&parsed);
	archive_frame_map_destroy(&map);
	arrfree(source.optical_paths);
}

TEST_CASE("sparse Part 10 frame maps use explicit per-frame assignments") {
	dicom_instance_t source = {};
	source.number_of_frames = 3;
	source.rows = 100;
	source.columns = 100;
	source.total_pixel_matrix_rows = 300;
	source.total_pixel_matrix_columns = 300;
	source.number_of_optical_paths = 2;
	for (i32 i = 0; i < 2; ++i) {
		dicom_optical_path_t optical_path = {};
		optical_path.OpticalPathIdentifier.present = true;
		snprintf(optical_path.OpticalPathIdentifier.value,
		         sizeof(optical_path.OpticalPathIdentifier.value), "%c", 'A' + i);
		arrput(source.optical_paths, optical_path);
	}
	const i32 rows[] = {201, 1, 101};
	const i32 columns[] = {1, 201, 101};
	const float focal_offsets[] = {2.0f, -1.0f, 2.0f};
	const char* optical_paths[] = {"B", "A", "B"};
	for (i32 frame = 0; frame < 3; ++frame) {
		dicom_plane_position_slide_t position = {};
		position.row_position_in_total_image_pixel_matrix = rows[frame];
		position.column_position_in_total_image_pixel_matrix = columns[frame];
		position.offset_in_slide_coordinate_system =
			V2F((float)columns[frame] / 10.0f, (float)rows[frame] / 10.0f);
		position.z_offset_in_slide_coordinate_system = focal_offsets[frame];
		arrput(source.per_frame_plane_position_slide, position);
		dicom_sh_t identifier = {};
		identifier.present = true;
		snprintf(identifier.value, sizeof(identifier.value), "%s", optical_paths[frame]);
		arrput(source.per_frame_optical_path_identifiers, identifier);
	}

	char error[512] = {};
	archive_frame_map_t map = {};
	REQUIRE(archive_frame_map_build_part10(&map, &source, error, sizeof(error)));
	CHECK(map.focal_plane_count == 2);
	CHECK(map.entries[0].row_position == 201);
	CHECK(map.entries[0].column_position == 1);
	CHECK(map.entries[0].focal_plane_index == 1);
	CHECK(map.entries[0].optical_path_index == 1);
	CHECK(map.entries[1].focal_plane_index == 0);
	CHECK(map.entries[1].optical_path_index == 0);
	CHECK(map.entries[2].focal_plane_index == 1);
	CHECK(map.entries[2].optical_path_index == 1);

	archive_frame_map_destroy(&map);
	arrfree(source.per_frame_optical_path_identifiers);
	arrfree(source.per_frame_plane_position_slide);
	arrfree(source.optical_paths);
}
