#include "common.h"
#include "doctest.h"

#include "stringutils.h"
#include "tiff.h"
#include "isyntax.h"
#include "dicom.h"
#include "dicom_wsi.h"
#include "dicomweb.h"
#include "image_loader.h"
#include "jpeg_decoder.h"
#include "archive_database.h"
#include "archive_frame_map.h"
#include "archive_index.h"
#include "archive_instance_provider.h"
#include "archive_mrxs_adapter.h"
#include "archive_object.h"
#include "archive_repository.h"
#include "archive_shell.h"
#include "archive_shell_recipe.h"
#include "test_temp.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef SLIDESCAPE_TEST_FIXTURE_MANIFEST
#define SLIDESCAPE_TEST_FIXTURE_MANIFEST "tests/fixtures/manifest.txt"
#endif

#ifndef SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST
#define SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST "tests/fixtures/local_manifest.txt"
#endif

#define MAX_TEST_FIXTURES 128

static void remove_archive_database_files(const char* root) {
	char path[ARCHIVE_PATH_CAPACITY];
	const char* suffixes[] = {"", "-wal", "-shm"};
	for (const char* suffix : suffixes) {
		snprintf(path, sizeof(path), "%s/%s%s", root, ARCHIVE_DATABASE_FILENAME, suffix);
		remove(path);
	}
}

struct fixture_t {
	char id[128];
	char type[32];
	char visibility[32];
	char tags[256];
	char path[1024];
	char source_url[1024];
};

struct fixture_manifest_t {
	fixture_t fixtures[MAX_TEST_FIXTURES];
	size_t fixture_count;
	bool loaded;
};

static void copy_string(char* dest, size_t dest_size, const char* src) {
	if (dest_size == 0) return;
	snprintf(dest, dest_size, "%s", src ? src : "");
}

static bool test_path_is_absolute(const char* path) {
	if (!path || !path[0]) return false;
	if (path[0] == '/' || path[0] == '\\') return true;
	if (strlen(path) >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
	    path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
		return true;
	}
	return false;
}

static void test_path_dirname(char* dest, size_t dest_size, const char* path) {
	if (dest_size == 0) return;
	const char* last_slash = NULL;
	for (const char* pos = path; pos && *pos; ++pos) {
		if (*pos == '/' || *pos == '\\') last_slash = pos;
	}
	if (!last_slash) {
		copy_string(dest, dest_size, ".");
		return;
	}
	size_t len = (size_t)(last_slash - path);
	if (len >= dest_size) len = dest_size - 1;
	memcpy(dest, path, len);
	dest[len] = '\0';
}

static void join_test_path(char* dest, size_t dest_size, const char* lhs, const char* rhs) {
	if (dest_size == 0) return;
	if (!rhs || !rhs[0] || test_path_is_absolute(rhs)) {
		copy_string(dest, dest_size, rhs);
		return;
	}
	if (!lhs || !lhs[0] || strcmp(lhs, ".") == 0) {
		copy_string(dest, dest_size, rhs);
		return;
	}
	size_t lhs_len = strlen(lhs);
	char separator = (lhs[lhs_len - 1] == '/' || lhs[lhs_len - 1] == '\\') ? '\0' : '/';
	if (separator) {
		snprintf(dest, dest_size, "%s/%s", lhs, rhs);
	} else {
		snprintf(dest, dest_size, "%s%s", lhs, rhs);
	}
}

static bool test_fixture_file_exists(const char* path) {
	struct stat st;
	return platform_stat(path, &st) == 0;
}

static size_t split_manifest_line(char* line, char** fields, size_t field_capacity) {
	size_t field_count = 0;
	char* field_start = line;
	for (char* pos = line; ; ++pos) {
		if (*pos == '|' || *pos == '\0') {
			char previous = *pos;
			*pos = '\0';
			if (field_count < field_capacity) fields[field_count++] = trim_whitespace(field_start);
			if (previous == '\0') break;
			field_start = pos + 1;
		}
	}
	return field_count;
}

static void load_fixture_manifest(fixture_manifest_t* manifest, const char* manifest_path) {
	FILE* fp = fopen(manifest_path, "rb");
	if (!fp) return;

	char manifest_dir[1024];
	test_path_dirname(manifest_dir, COUNT(manifest_dir), manifest_path);

	char line[4096];
	while (fgets(line, COUNT(line), fp)) {
		char* text = trim_whitespace(line);
		if (!text[0] || text[0] == '#') continue;

		char* fields[6] = {};
		size_t field_count = split_manifest_line(text, fields, COUNT(fields));
		if (field_count < 5 || manifest->fixture_count >= COUNT(manifest->fixtures)) continue;

		fixture_t& fixture = manifest->fixtures[manifest->fixture_count++];
		copy_string(fixture.id, COUNT(fixture.id), fields[0]);
		copy_string(fixture.type, COUNT(fixture.type), fields[1]);
		copy_string(fixture.visibility, COUNT(fixture.visibility), fields[2]);
		copy_string(fixture.tags, COUNT(fixture.tags), fields[3]);
		join_test_path(fixture.path, COUNT(fixture.path), manifest_dir, fields[4]);
		if (field_count >= 6) copy_string(fixture.source_url, COUNT(fixture.source_url), fields[5]);
	}

	fclose(fp);
}

static fixture_manifest_t* fixture_manifest() {
	static fixture_manifest_t manifest = {};
	if (!manifest.loaded) {
		load_fixture_manifest(&manifest, SLIDESCAPE_TEST_FIXTURE_MANIFEST);
		load_fixture_manifest(&manifest, SLIDESCAPE_TEST_LOCAL_FIXTURE_MANIFEST);
		manifest.loaded = true;
	}
	return &manifest;
}

static bool fixture_has_tag(const fixture_t& fixture, const char* tag) {
	size_t tag_len = strlen(tag);
	size_t offset = 0;
	size_t tags_len = strlen(fixture.tags);
	while (offset <= tags_len) {
		char candidate[128];
		size_t comma = offset;
		while (comma < tags_len && fixture.tags[comma] != ',') ++comma;
		size_t len = comma - offset;
		if (len >= COUNT(candidate)) len = COUNT(candidate) - 1;
		memcpy(candidate, fixture.tags + offset, len);
		candidate[len] = '\0';
		char* trimmed = trim_whitespace(candidate);
		if (strlen(trimmed) == tag_len && memcmp(trimmed, tag, tag_len) == 0) return true;
		if (comma == tags_len) break;
		offset = comma + 1;
	}
	return false;
}

static const fixture_t* first_available_fixture(const char* type, const char* tag = NULL) {
	fixture_manifest_t* manifest = fixture_manifest();
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, type) != 0) continue;
		if (tag && !fixture_has_tag(fixture, tag)) continue;
		if (test_fixture_file_exists(fixture.path)) return &fixture;
	}
	return NULL;
}

static bool read_file_prefix(const char* filename, u8* dest, size_t bytes_to_read) {
	FILE* fp = fopen(filename, "rb");
	if (!fp) return false;
	size_t bytes_read = fread(dest, 1, bytes_to_read, fp);
	fclose(fp);
	return bytes_read == bytes_to_read;
}

static bool pixel_buffer_has_variation(const u32* pixels, size_t pixel_count) {
	if (pixel_count == 0) return false;
	u32 first = pixels[0];
	for (size_t i = 1; i < pixel_count; ++i) {
		if (pixels[i] != first) return true;
	}
	return false;
}

static void ensure_image_read_region_thread_pool(void) {
	if (!global_thread_pool.initialized) {
		init_global_system_info(false);
		if (global_system_info.suggested_total_thread_count < 2) {
			global_system_info.suggested_total_thread_count = 2;
		}
		init_thread_pool(&global_thread_pool, 128, true, false, NULL);
	}
}

static void ensure_dicom_initialized(void) {
	static bool initialized = false;
	if (!initialized) {
		REQUIRE(dicom_init());
		initialized = true;
	}
}

#if !defined(_WIN32)
static const char* test_dicom_image_flavor(
		dicom_image_flavor_enum image_flavor) {
	switch (image_flavor) {
		case DICOM_IMAGE_FLAVOR_VOLUME: return "VOLUME";
		case DICOM_IMAGE_FLAVOR_LABEL: return "LABEL";
		case DICOM_IMAGE_FLAVOR_OVERVIEW: return "OVERVIEW";
		case DICOM_IMAGE_FLAVOR_THUMBNAIL: return "THUMBNAIL";
		default: return "";
	}
}

static void check_archive_part10_provider(dicom_instance_t* instance, u32 one_based_frame) {
	REQUIRE(archive_instance_provider_initialize());
	char temp_path[ARCHIVE_PATH_CAPACITY] = {};
	bool created_temp_directory = slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-provider-test");
	REQUIRE(created_temp_directory);
	archive_repository_t repository = {};
	char error[512] = {};
	REQUIRE(archive_repository_initialize(&repository, temp_path, error, sizeof(error)) ==
	        ARCHIVE_REPOSITORY_OK);
	archive_object_t source_object = {};
	REQUIRE(archive_object_reference_file(
		&repository, instance->filename, "application/dicom", &source_object,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	bool shell_identity_is_complete =
		instance->general_study.StudyInstanceUID.value[0] &&
		instance->general_series.SeriesInstanceUID.value[0] &&
		instance->sop_instance_uid.value[0] && instance->sop_class_uid.value[0] &&
		instance->transfer_syntax_uid.value[0];
	archive_object_t stored_shell = {};
	archive_object_t stored_recipe = {};
	char* recipe = nullptr;
	size_t recipe_size = 0;
	char representation_fingerprint[65] = {};
	const char* bundle_sha256 =
		"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
	u32 preparation_count = 0;
	bool preparation_has_relationship = false;
	bool preparation_has_scheme_version = false;
	if (shell_identity_is_complete) {
		archive_shell_part10_recipe_input_t recipe_input = {};
		recipe_input.bundle_sha256 = bundle_sha256;
		recipe_input.source_sha256 = source_object.sha256;
		recipe_input.source_byte_size = source_object.byte_size;
		recipe_input.sop_instance_uid = instance->sop_instance_uid.value;
		archive_shell_part10_aggregate_recipe_input_t aggregate_recipe_input = {};
		aggregate_recipe_input.bundle_sha256 = bundle_sha256;
		aggregate_recipe_input.study_instance_uid =
			instance->general_study.StudyInstanceUID.value;
		aggregate_recipe_input.series_instance_uid =
			instance->general_series.SeriesInstanceUID.value;
		aggregate_recipe_input.instances = &recipe_input;
		aggregate_recipe_input.instance_count = 1;
		REQUIRE(archive_shell_build_part10_aggregate_recipe(
			&aggregate_recipe_input, &recipe, &recipe_size,
			representation_fingerprint));
		REQUIRE(archive_object_store_control_bytes(
			&repository, recipe, recipe_size, ARCHIVE_SHELL_RECIPE_MEDIA_TYPE,
			&stored_recipe, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
		archive_shell_build_part10_input_t shell_input = {};
		shell_input.instance = instance;
		shell_input.source_sha256 = source_object.sha256;
		shell_input.source_byte_size = source_object.byte_size;
		shell_input.representation_fingerprint = representation_fingerprint;
		archive_shell_build_part10_aggregate_input_t aggregate_shell_input = {};
		aggregate_shell_input.instances = &shell_input;
		aggregate_shell_input.instance_count = 1;
		aggregate_shell_input.representation_fingerprint =
			representation_fingerprint;
		archive_shell_buffer_t encoded_shell = {};
		INFO(error);
		REQUIRE(archive_shell_build_part10_aggregate(
			&aggregate_shell_input, &encoded_shell, error, sizeof(error)));
		archive_shell_view_t shell_view = {};
		REQUIRE(archive_shell_open(&shell_view, encoded_shell.data, encoded_shell.size,
		                           error, sizeof(error)));
		archive_shell_instance_t shell_instance = {};
		REQUIRE(archive_shell_get_instance(&shell_view, 0, &shell_instance));
		CHECK(strcmp(shell_instance.sop_instance_uid, instance->sop_instance_uid.value) == 0);
		archive_shell_payload_range_t shell_payload = {};
		REQUIRE(archive_shell_get_payload_range(&shell_view, &shell_instance,
		                                        one_based_frame - 1, &shell_payload));
		CHECK(shell_payload.offset + shell_payload.size <= shell_input.source_byte_size);
		for (i32 i = 0; i < arrlen(instance->optical_paths); ++i) {
			const dicom_optical_path_t* parsed = instance->optical_paths + i;
			archive_shell_optical_path_t stored = {};
			REQUIRE(archive_shell_get_optical_path(
				&shell_view, &shell_instance, (u32)i, &stored));
			CHECK(strcmp(stored.identifier,
			             parsed->OpticalPathIdentifier.value) == 0);
			CHECK(strcmp(stored.color_space, parsed->ColorSpace.value) == 0);
			CHECK(strcmp(stored.illumination_type_code_value,
			             parsed->illumination_type.CodeValue.value) == 0);
			CHECK(strcmp(stored.illumination_type_coding_scheme,
			             parsed->illumination_type.CodingSchemeDesignator.value) == 0);
			CHECK(strcmp(stored.illumination_type_code_meaning,
			             parsed->illumination_type.CodeMeaning.value) == 0);
			CHECK(stored.icc_profile_size == parsed->icc_profile_size);
			if (stored.icc_profile_size)
				CHECK(memcmp(stored.icc_profile, parsed->icc_profile,
				             parsed->icc_profile_size) == 0);
		}
		CHECK(strcmp(archive_shell_get_container_identifier(
			             &shell_view, &shell_instance),
		             instance->ContainerIdentifier.value) == 0);
		REQUIRE(archive_shell_get_specimen_count(
			        &shell_view, &shell_instance) ==
		        (u32)arrlen(instance->specimens));
		for (i32 i = 0; i < arrlen(instance->specimens); ++i) {
			archive_shell_specimen_t stored = {};
			REQUIRE(archive_shell_get_specimen(
				&shell_view, &shell_instance, (u32)i, &stored));
			CHECK(strcmp(stored.identifier,
			             instance->specimens[i].SpecimenIdentifier.value) == 0);
			CHECK(strcmp(stored.uid,
			             instance->specimens[i].SpecimenUID.value) == 0);
			CHECK(strcmp(stored.short_description,
			             instance->specimens[i].SpecimenShortDescription.value) == 0);
		}
		preparation_count = 0;
		for (i32 specimen_index = 0;
		     specimen_index < arrlen(instance->specimens); ++specimen_index) {
			const dicom_specimen_t* parsed_specimen =
				instance->specimens + specimen_index;
			for (i32 step_index = 0;
			     step_index < arrlen(parsed_specimen->preparation_steps);
			     ++step_index)
				preparation_count +=
					(u32)arrlen(
						parsed_specimen->preparation_steps[step_index].items);
		}
		REQUIRE(archive_shell_get_preparation_item_count(
			        &shell_view, &shell_instance) == preparation_count);
		u32 preparation_index = 0;
		for (i32 specimen_index = 0;
		     specimen_index < arrlen(instance->specimens); ++specimen_index) {
			const dicom_specimen_t* parsed_specimen =
				instance->specimens + specimen_index;
			for (i32 step_index = 0;
			     step_index < arrlen(parsed_specimen->preparation_steps);
			     ++step_index) {
				const dicom_specimen_preparation_step_t* parsed_step =
					parsed_specimen->preparation_steps + step_index;
				u32 step_base = preparation_index;
				for (i32 item_index = 0;
				     item_index < arrlen(parsed_step->items);
				     ++item_index, ++preparation_index) {
					const dicom_specimen_preparation_item_t* parsed =
						parsed_step->items + item_index;
					preparation_has_relationship |=
						parsed->relationship_type &&
						parsed->relationship_type[0];
					preparation_has_scheme_version |=
						(parsed->concept_name.coding_scheme_version &&
						 parsed->concept_name.coding_scheme_version[0]) ||
						(parsed->code_value.coding_scheme_version &&
						 parsed->code_value.coding_scheme_version[0]) ||
						(parsed->measurement_units.coding_scheme_version &&
						 parsed->measurement_units.coding_scheme_version[0]);
					archive_shell_preparation_item_t stored = {};
					REQUIRE(archive_shell_get_preparation_item(
						&shell_view, &shell_instance,
						preparation_index, &stored));
					CHECK(stored.specimen_index == (u32)specimen_index);
					CHECK(stored.step_index == (u32)step_index);
					CHECK(stored.parent_item_index ==
					      (parsed->parent_index == UINT32_MAX ?
					       UINT32_MAX : step_base + parsed->parent_index));
					CHECK((u32)stored.value_kind ==
					      (u32)parsed->value_type);
					CHECK(strcmp(
						stored.relationship_type,
						parsed->relationship_type ?
							parsed->relationship_type : "") == 0);
					CHECK(strcmp(
						stored.concept_name_code_value,
						parsed->concept_name.value ?
							parsed->concept_name.value : "") == 0);
					CHECK(strcmp(
						stored.concept_name_coding_scheme,
						parsed->concept_name.coding_scheme ?
							parsed->concept_name.coding_scheme : "") == 0);
					CHECK(strcmp(
						stored.concept_name_coding_scheme_version,
						parsed->concept_name.coding_scheme_version ?
							parsed->concept_name.coding_scheme_version : "") == 0);
					if (parsed->value_type ==
					    DICOM_CONTENT_VALUE_CODE) {
						CHECK(strcmp(
							stored.code_value,
							parsed->code_value.value ?
								parsed->code_value.value : "") == 0);
						CHECK(strcmp(
							stored.code_coding_scheme,
							parsed->code_value.coding_scheme ?
								parsed->code_value.coding_scheme : "") == 0);
						CHECK(strcmp(
							stored.code_coding_scheme_version,
							parsed->code_value.coding_scheme_version ?
								parsed->code_value.coding_scheme_version : "") == 0);
					} else {
						CHECK(strcmp(
							stored.scalar_value,
							parsed->scalar_value ?
								parsed->scalar_value : "") == 0);
					}
				}
			}
		}
		REQUIRE(archive_object_store_shell_bytes(
			&repository, encoded_shell.data, encoded_shell.size, &stored_shell,
			error, sizeof(error)) == ARCHIVE_OBJECT_OK);
		archive_shell_buffer_destroy(&encoded_shell);
	}
	free(recipe);
	archive_instance_record_t record = {};
	snprintf(record.study_instance_uid, sizeof(record.study_instance_uid), "%s",
	         instance->general_study.StudyInstanceUID.value);
	snprintf(record.series_instance_uid, sizeof(record.series_instance_uid), "%s",
	         instance->general_series.SeriesInstanceUID.value);
	snprintf(record.sop_instance_uid, sizeof(record.sop_instance_uid), "%s",
	         instance->sop_instance_uid.value);
	snprintf(record.sop_class_uid, sizeof(record.sop_class_uid), "%s", instance->sop_class_uid.value);
	snprintf(record.provider, sizeof(record.provider), "part10");
	snprintf(record.object_key, sizeof(record.object_key), "%s", source_object.storage_key);
	snprintf(record.object_sha256, sizeof(record.object_sha256), "%s", source_object.sha256);
	record.object_byte_size = source_object.byte_size;
	snprintf(record.shell_key, sizeof(record.shell_key), "%s", stored_shell.storage_key);
	snprintf(record.shell_sha256, sizeof(record.shell_sha256), "%s", stored_shell.sha256);
	record.shell_byte_size = stored_shell.byte_size;
	snprintf(record.bundle_sha256, sizeof(record.bundle_sha256), "%s", bundle_sha256);
	snprintf(record.representation_fingerprint,
	         sizeof(record.representation_fingerprint), "%s",
	         representation_fingerprint);
	snprintf(record.recipe_key, sizeof(record.recipe_key), "%s", stored_recipe.storage_key);
	snprintf(record.recipe_sha256, sizeof(record.recipe_sha256), "%s", stored_recipe.sha256);
	record.recipe_byte_size = stored_recipe.byte_size;
	snprintf(record.transfer_syntax_uid, sizeof(record.transfer_syntax_uid), "%s",
	         instance->transfer_syntax_uid.value);
	if (instance->instance_number >= 0)
		snprintf(record.instance_number, sizeof(record.instance_number), "%lld",
		         (long long)instance->instance_number);
	snprintf(record.number_of_frames, sizeof(record.number_of_frames), "%lld",
	         (long long)instance->number_of_frames);
	snprintf(record.total_pixel_matrix_columns, sizeof(record.total_pixel_matrix_columns), "%u",
	         instance->total_pixel_matrix_columns);
	snprintf(record.total_pixel_matrix_rows, sizeof(record.total_pixel_matrix_rows), "%u",
	         instance->total_pixel_matrix_rows);
	snprintf(record.image_flavor, sizeof(record.image_flavor), "%s",
	         test_dicom_image_flavor(instance->image_flavor));

	char shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, stored_shell.storage_key,
	                                     shell_path, sizeof(shell_path)));
	CHECK(remove(shell_path) == 0);
	char* metadata_json = NULL;
	size_t metadata_size = 0;
	REQUIRE(archive_instance_provider_get_metadata(&repository, &record,
	        &metadata_json, &metadata_size, error, sizeof(error)));
	CHECK(test_fixture_file_exists(shell_path));
	bool has_optical_path_sequence =
		strstr(metadata_json, "\"00480105\":{\"vr\":\"SQ\"") != NULL;
	CHECK(has_optical_path_sequence);
	bool has_icc_profile = false;
	for (i32 i = 0; i < arrlen(instance->optical_paths); ++i)
		has_icc_profile |= instance->optical_paths[i].icc_profile_size > 0;
	if (has_icc_profile) {
		bool has_inline_icc =
			strstr(metadata_json,
			       "\"00282000\":{\"vr\":\"OB\",\"InlineBinary\":") != NULL;
		CHECK(has_inline_icc);
	}
	if (instance->ContainerIdentifier.present) {
		bool has_container =
			strstr(metadata_json, "\"00400512\":{\"vr\":\"LO\"") != NULL;
		CHECK(has_container);
	}
	if (arrlen(instance->specimens) > 0) {
		bool has_specimens =
			strstr(metadata_json, "\"00400560\":{\"vr\":\"SQ\"") != NULL;
		CHECK(has_specimens);
	}
	if (preparation_count > 0) {
		bool has_preparation_sequence = strstr(
			metadata_json,
			"\"00400610\":{\"vr\":\"SQ\"") != NULL;
		bool has_step_content_sequence = strstr(
			metadata_json,
			"\"00400612\":{\"vr\":\"SQ\"") != NULL;
		bool has_value_type = strstr(
			metadata_json,
			"\"0040A040\":{\"vr\":\"CS\"") != NULL;
		bool has_concept_name = strstr(
			metadata_json,
			"\"0040A043\":{\"vr\":\"SQ\"") != NULL;
		CHECK(has_preparation_sequence);
		CHECK(has_step_content_sequence);
		CHECK(has_value_type);
		CHECK(has_concept_name);
		if (preparation_has_relationship) {
			bool has_relationship_type = strstr(
				metadata_json,
				"\"0040A010\":{\"vr\":\"CS\"") != NULL;
			CHECK(has_relationship_type);
		}
		if (preparation_has_scheme_version) {
			bool has_coding_scheme_version = strstr(
				metadata_json,
				"\"00080103\":{\"vr\":\"SH\"") != NULL;
			CHECK(has_coding_scheme_version);
		}
	}
	if (instance->patient.PatientID.present) {
		bool has_patient_id =
			strstr(metadata_json, "\"00100020\":{\"vr\":\"LO\"") != NULL;
		CHECK(has_patient_id);
	}
	if (instance->general_study.StudyDate.present) {
		bool has_study_date =
			strstr(metadata_json, "\"00080020\":{\"vr\":\"DA\"") != NULL;
		CHECK(has_study_date);
	}
	dicomweb_instance_metadata_t metadata = {};
	REQUIRE(dicomweb_parse_instance_metadata(metadata_json, metadata_size, &metadata));
	CHECK(strcmp(metadata.sop_instance_uid, record.sop_instance_uid) == 0);
	CHECK(metadata.number_of_frames == instance->number_of_frames);
	CHECK(strcmp(metadata.image_flavor, record.image_flavor) == 0);
	CHECK(metadata.is_tiled_full == instance->is_tiled_full);
	dicomweb_instance_metadata_destroy(&metadata);
	char* series_metadata_json = nullptr;
	size_t series_metadata_size = 0;
	REQUIRE(archive_instance_provider_get_series_metadata(
		&repository, &record, 1, &series_metadata_json,
		&series_metadata_size, error, sizeof(error)));
	CHECK(series_metadata_size == metadata_size);
	CHECK(memcmp(series_metadata_json, metadata_json, metadata_size) == 0);
	free(series_metadata_json);

	dicom_instance_t synthetic_instances[2] = {*instance, *instance};
	const char* synthetic_uids[2] = {
		"1.2.826.0.1.3680043.10.543.1001",
		"1.2.826.0.1.3680043.10.543.1002",
	};
	archive_shell_build_part10_input_t synthetic_inputs[2] = {};
	for (i32 i = 0; i < 2; ++i) {
		snprintf(synthetic_instances[i].sop_instance_uid.value,
		         sizeof(synthetic_instances[i].sop_instance_uid.value),
		         "%s", synthetic_uids[i]);
		synthetic_instances[i].sop_instance_uid.len =
			strlen(synthetic_instances[i].sop_instance_uid.value);
		synthetic_instances[i].instance_number = i + 1;
		synthetic_inputs[i].instance = synthetic_instances + i;
		synthetic_inputs[i].source_sha256 = source_object.sha256;
		synthetic_inputs[i].source_byte_size = source_object.byte_size;
	}
	const char* synthetic_fingerprint =
		"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
	archive_shell_build_part10_aggregate_input_t synthetic_aggregate_input = {};
	synthetic_aggregate_input.instances = synthetic_inputs;
	synthetic_aggregate_input.instance_count = 2;
	synthetic_aggregate_input.representation_fingerprint =
		synthetic_fingerprint;
	archive_shell_buffer_t synthetic_shell = {};
	REQUIRE(archive_shell_build_part10_aggregate(
		&synthetic_aggregate_input, &synthetic_shell,
		error, sizeof(error)));
	archive_object_t synthetic_shell_object = {};
	REQUIRE(archive_object_store_shell_bytes(
		&repository, synthetic_shell.data, synthetic_shell.size,
		&synthetic_shell_object, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	archive_shell_buffer_destroy(&synthetic_shell);
	archive_instance_record_t synthetic_records[2] = {record, record};
	for (i32 i = 0; i < 2; ++i) {
		snprintf(synthetic_records[i].sop_instance_uid,
		         sizeof(synthetic_records[i].sop_instance_uid),
		         "%s", synthetic_uids[i]);
		snprintf(synthetic_records[i].shell_key,
		         sizeof(synthetic_records[i].shell_key), "%s",
		         synthetic_shell_object.storage_key);
		snprintf(synthetic_records[i].shell_sha256,
		         sizeof(synthetic_records[i].shell_sha256), "%s",
		         synthetic_shell_object.sha256);
		synthetic_records[i].shell_byte_size =
			synthetic_shell_object.byte_size;
		synthetic_records[i].shell_instance_index = (u32)i;
		snprintf(synthetic_records[i].instance_number,
		         sizeof(synthetic_records[i].instance_number), "%d", i + 1);
		snprintf(synthetic_records[i].representation_fingerprint,
		         sizeof(synthetic_records[i].representation_fingerprint),
		         "%s", synthetic_fingerprint);
	}
	series_metadata_json = nullptr;
	series_metadata_size = 0;
	REQUIRE(archive_instance_provider_get_series_metadata(
		&repository, synthetic_records, 2, &series_metadata_json,
		&series_metadata_size, error, sizeof(error)));
	const char* first_uid_position =
		strstr(series_metadata_json, synthetic_uids[0]);
	const char* second_uid_position =
		strstr(series_metadata_json, synthetic_uids[1]);
	REQUIRE(first_uid_position != nullptr);
	REQUIRE(second_uid_position != nullptr);
	CHECK(first_uid_position < second_uid_position);
	CHECK(strstr(series_metadata_json, "}},{") != nullptr);
	free(series_metadata_json);
	free(metadata_json);

	archive_instance_provider_shutdown();
	REQUIRE(archive_instance_provider_initialize());
	FILE* corrupt_shell = fopen(shell_path, "wb");
	REQUIRE(corrupt_shell != NULL);
	REQUIRE(fputc(0, corrupt_shell) != EOF);
	REQUIRE(fclose(corrupt_shell) == 0);
	metadata_json = NULL;
	metadata_size = 0;
	REQUIRE(archive_instance_provider_get_metadata(&repository, &record,
	        &metadata_json, &metadata_size, error, sizeof(error)));
	free(metadata_json);

	archive_frame_representation_t representations[ARCHIVE_MAX_FRAME_REPRESENTATIONS] = {};
	i32 representation_count = archive_instance_provider_get_frame_representations(
		&repository, &record, representations, COUNT(representations),
		error, sizeof(error));
	REQUIRE(representation_count >= 1);
	CHECK(representations[0].source == ARCHIVE_FRAME_REPRESENTATION_STORED);
	archive_instance_frame_t frame = {};
	REQUIRE(archive_instance_provider_get_frame(&repository, &record, one_based_frame,
	        representations, &frame, error, sizeof(error)));
	CHECK(frame.size > 0);
	CHECK(strcmp(frame.transfer_syntax_uid, instance->transfer_syntax_uid.value) == 0);
	if (instance->transfer_syntax_uid.as_enum == DICOM_JPEGBaseline8Bit) {
		REQUIRE(frame.size >= 2);
		CHECK(frame.data[0] == 0xff);
		CHECK(frame.data[1] == 0xd8);
	} else if (instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000Lossless ||
	           instance->transfer_syntax_uid.as_enum == DICOM_JPEG2000) {
		REQUIRE(frame.size >= 2);
		CHECK(frame.data[0] == 0xff);
		CHECK(frame.data[1] == 0x4f);
	}
	dicomweb_instance_metadata_t decoder_metadata = {};
	decoder_metadata.rows = instance->rows;
	decoder_metadata.columns = instance->columns;
	dicomweb_frame_t frame_view = {frame.data, frame.size};
	snprintf(frame_view.media_type, sizeof(frame_view.media_type), "%s", frame.media_type);
	snprintf(frame_view.transfer_syntax_uid, sizeof(frame_view.transfer_syntax_uid), "%s",
	         frame.transfer_syntax_uid);
	u8* decoded_frame = dicomweb_decode_frame_to_bgra(&decoder_metadata, &frame_view);
	REQUIRE(decoded_frame != NULL);
	CHECK(pixel_buffer_has_variation((u32*)decoded_frame,
	                                (size_t)instance->rows * (size_t)instance->columns));
	archive_instance_frame_destroy(&frame);

	if (representation_count > 1) {
		archive_instance_frame_t generated = {};
		REQUIRE(archive_instance_provider_get_frame(
			&repository, &record, one_based_frame, representations + 1,
			&generated, error, sizeof(error)));
		CHECK(strcmp(generated.media_type, "application/octet-stream") == 0);
		CHECK(strcmp(generated.transfer_syntax_uid, "1.2.840.10008.1.2.1") == 0);
		CHECK(generated.size == (size_t)instance->rows * instance->columns * 3);
		size_t pixel_count = (size_t)instance->rows * instance->columns;
		size_t sample_pixels[] = {0, pixel_count / 2, pixel_count - 1};
		for (size_t pixel : sample_pixels) {
			CHECK(generated.data[pixel * 3 + 0] == decoded_frame[pixel * 4 + 2]);
			CHECK(generated.data[pixel * 3 + 1] == decoded_frame[pixel * 4 + 1]);
			CHECK(generated.data[pixel * 3 + 2] == decoded_frame[pixel * 4 + 0]);
		}
		archive_instance_frame_destroy(&generated);
	}
	free(decoded_frame);

	CHECK(remove(shell_path) == 0);
	char synthetic_shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(
		&repository, synthetic_shell_object.storage_key,
		synthetic_shell_path, sizeof(synthetic_shell_path)));
	CHECK(remove(synthetic_shell_path) == 0);
	char source_object_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(&repository, source_object.storage_key,
	                                     source_object_path, sizeof(source_object_path)));
	CHECK(remove(source_object_path) == 0);
	char shell_prefix_path[ARCHIVE_PATH_CAPACITY];
	snprintf(shell_prefix_path, sizeof(shell_prefix_path), "%s/shells/sha256/%.2s",
	         temp_path, stored_shell.sha256);
	CHECK(rmdir(shell_prefix_path) == 0);
	if (strncmp(stored_shell.sha256, synthetic_shell_object.sha256, 2) != 0) {
		snprintf(shell_prefix_path, sizeof(shell_prefix_path),
		         "%s/shells/sha256/%.2s", temp_path,
		         synthetic_shell_object.sha256);
		CHECK(rmdir(shell_prefix_path) == 0);
	}
	char object_prefix_path[ARCHIVE_PATH_CAPACITY];
	snprintf(object_prefix_path, sizeof(object_prefix_path), "%s/objects/sha256/%.2s",
	         temp_path, source_object.sha256);
	CHECK(rmdir(object_prefix_path) == 0);
	snprintf(object_prefix_path, sizeof(object_prefix_path), "%s/objects/sha256", temp_path);
	CHECK(rmdir(object_prefix_path) == 0);
	snprintf(shell_prefix_path, sizeof(shell_prefix_path), "%s/shells/sha256", temp_path);
	CHECK(rmdir(shell_prefix_path) == 0);
	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		char path[ARCHIVE_PATH_CAPACITY];
		snprintf(path, sizeof(path), "%s/%s", temp_path, child);
		CHECK(rmdir(path) == 0);
	}
	char descriptor[ARCHIVE_PATH_CAPACITY];
	snprintf(descriptor, sizeof(descriptor), "%s/repository.json", temp_path);
	CHECK(remove(descriptor) == 0);
	remove_archive_database_files(temp_path);
	CHECK(rmdir(temp_path) == 0);
}
#endif

typedef struct selected_tiff_tile_t {
	i32 level;
	i32 tile_x;
	i32 tile_y;
	i32 tile_index;
	i32 read_width;
	i32 read_height;
	u8* pixels;
} selected_tiff_tile_t;

static bool select_decodable_tiff_tile(image_t* image, selected_tiff_tile_t* out_tile) {
	ASSERT(image && image->backend == IMAGE_BACKEND_TIFF);
	level_image_t* level_image = image->level_images + 0;
	tiff_ifd_t* ifd = image->tiff.level_images_ifd + level_image->pyramid_image_index;
	for (i32 tile_y = 0; tile_y < (i32)level_image->height_in_tiles; ++tile_y) {
		for (i32 tile_x = 0; tile_x < (i32)level_image->width_in_tiles; ++tile_x) {
			i32 tile_index = tile_y * (i32)level_image->width_in_tiles + tile_x;
			if (ifd->tile_offsets[tile_index] == 0 || ifd->tile_byte_counts[tile_index] == 0) continue;

			u8* pixels = tiff_decode_tile(0, &image->tiff, ifd, tile_index, 0, tile_x, tile_y);
			if (!pixels) continue;

			i64 x = (i64)tile_x * (i64)level_image->tile_width;
			i64 y = (i64)tile_y * (i64)level_image->tile_height;
			i32 read_width = (i32)MIN((i64)level_image->tile_width, level_image->width_in_pixels - x);
			i32 read_height = (i32)MIN((i64)level_image->tile_height, level_image->height_in_pixels - y);
			if (read_width <= 0 || read_height <= 0) {
				free(pixels);
				continue;
			}

			*out_tile = {};
			out_tile->level = 0;
			out_tile->tile_x = tile_x;
			out_tile->tile_y = tile_y;
			out_tile->tile_index = tile_index;
			out_tile->read_width = read_width;
			out_tile->read_height = read_height;
			out_tile->pixels = pixels;
			return true;
		}
	}
	return false;
}

static image_t* load_tiff_fixture_image(const fixture_t* fixture) {
	file_info_t file = viewer_get_file_info(fixture->path);
	REQUIRE(file.is_valid);
	REQUIRE(file.is_regular_file);
	REQUIRE(file.type == VIEWER_FILE_TYPE_TIFF);

	image_load_options_t options = {};
	options.use_builtin_tiff_backend = true;
	options.resource_id = 2000;
	options.thread_pool = &global_thread_pool;
	image_t* image = image_load_from_file(&file, NULL, &options);
	REQUIRE(image != NULL);
	REQUIRE(image->is_valid);
	REQUIRE(image->backend == IMAGE_BACKEND_TIFF);
	return image;
}

static image_t* load_dicom_fixture_image(const fixture_t* fixture) {
	ensure_dicom_initialized();
	ensure_image_read_region_thread_pool();

	file_info_t file = viewer_get_file_info(fixture->path);
	REQUIRE(file.is_valid);

	image_load_options_t options = {};
	options.resource_id = 3000;
	options.thread_pool = &global_thread_pool;
	image_t* image = NULL;
	if (file.is_directory) {
		directory_info_t directory = viewer_get_dicom_directory_info(fixture->path);
		REQUIRE(directory.is_valid);
		REQUIRE(directory.contains_dicom_files);
		file.type = VIEWER_FILE_TYPE_DICOM;
		image = image_load_from_file(&file, &directory, &options);
		viewer_directory_info_destroy(&directory);
	} else {
		REQUIRE(file.is_regular_file);
		REQUIRE(file.type == VIEWER_FILE_TYPE_DICOM);
		image = image_load_from_file(&file, NULL, &options);
	}
	REQUIRE(image != NULL);
	REQUIRE(image->is_valid);
	REQUIRE(image->backend == IMAGE_BACKEND_DICOM);
	return image;
}

static image_t* load_selected_dicom_fixture_image(const fixture_t* directory_fixture,
		const char* selected_filename) {
	ensure_dicom_initialized();
	ensure_image_read_region_thread_pool();

	file_info_t file = viewer_get_file_info(selected_filename);
	REQUIRE(file.is_valid);
	REQUIRE(file.is_regular_file);
	REQUIRE(file.type == VIEWER_FILE_TYPE_DICOM);
	directory_info_t directory = viewer_get_dicom_directory_info(directory_fixture->path);
	REQUIRE(directory.is_valid);
	REQUIRE(directory.contains_dicom_files);

	image_load_options_t options = {};
	options.resource_id = 3000;
	options.thread_pool = &global_thread_pool;
	image_t* image = image_load_from_file(&file, &directory, &options);
	viewer_directory_info_destroy(&directory);
	REQUIRE(image != NULL);
	REQUIRE(image->is_valid);
	REQUIRE(image->backend == IMAGE_BACKEND_DICOM);
	return image;
}

TEST_CASE("fixture manifest is readable") {
	fixture_manifest_t* manifest = fixture_manifest();
	REQUIRE(manifest->fixture_count > 0);

	bool has_public_fixture = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		CAPTURE(fixture.id);
		CHECK(fixture.type[0] != '\0');
		CHECK(fixture.path[0] != '\0');
		if (strcmp(fixture.visibility, "public") == 0) has_public_fixture = true;
	}
	CHECK(has_public_fixture);
}

TEST_CASE("available WSI fixtures have expected extensions") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (!test_fixture_file_exists(fixture.path)) continue;

		const char* extension = get_file_extension(fixture.path);
		CAPTURE(fixture.path);
		if (strcmp(fixture.type, "tiff") == 0) {
			CHECK((strcmp(extension, "tif") == 0 || strcmp(extension, "tiff") == 0));
		} else if (strcmp(fixture.type, "isyntax") == 0) {
			CHECK(strcmp(extension, "isyntax") == 0);
		} else if (strcmp(fixture.type, "dicom") == 0) {
			if (fixture_has_tag(fixture, "dicom-directory")) {
				file_info_t file = viewer_get_file_info(fixture.path);
				CHECK(file.is_directory);
				continue;
			}
			u8 header[256] = {};
			REQUIRE(read_file_prefix(fixture.path, header, sizeof(header)));
			CHECK(is_file_a_dicom_file(header, sizeof(header)));
		} else if (strcmp(fixture.type, "asap_xml") == 0) {
			CHECK(strcmp(extension, "xml") == 0);
		}
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping fixture extension checks: no manifest fixtures are present locally.");
	}
}

TEST_CASE("TIFF fixtures have valid TIFF or BigTIFF headers") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "tiff") != 0 || !test_fixture_file_exists(fixture.path)) continue;

		u8 header[4] = {};
		CAPTURE(fixture.path);
		REQUIRE(read_file_prefix(fixture.path, header, sizeof(header)));

		bool little_endian_tiff = header[0] == 'I' && header[1] == 'I' && header[2] == 42 && header[3] == 0;
		bool little_endian_bigtiff = header[0] == 'I' && header[1] == 'I' && header[2] == 43 && header[3] == 0;
		bool big_endian_tiff = header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 42;
		bool big_endian_bigtiff = header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 43;
		CHECK((little_endian_tiff || little_endian_bigtiff || big_endian_tiff || big_endian_bigtiff));
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping TIFF header checks: no TIFF fixtures are present locally.");
	}
}

TEST_CASE("iSyntax fixture starts with XML metadata") {
	const fixture_t* fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax header check: no iSyntax fixture is present locally.");
		return;
	}

	u8 header[256] = {};
	REQUIRE(read_file_prefix(fixture->path, header, sizeof(header)));
	CHECK(memcmp(header, "<?xml", 5) == 0);
	CHECK(static_cast<bool>(strstr((char*)header, "DPUfsImport") != NULL));
	CHECK(static_cast<bool>(strstr((char*)header, "DICOM_MANUFACTURER") != NULL));
}

TEST_CASE("ASAP XML fixture exposes at least one annotation node") {
	const fixture_t* fixture = first_available_fixture("asap_xml");
	if (!fixture) {
		MESSAGE("Skipping ASAP XML fixture check: no ASAP XML fixture is present locally.");
		return;
	}

	FILE* fp = fopen(fixture->path, "rb");
	REQUIRE(fp != NULL);
	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	char* xml = (char*)calloc((size_t)file_size + 1, 1);
	REQUIRE(static_cast<bool>(xml != NULL));
	REQUIRE(fread(xml, 1, (size_t)file_size, fp) == (size_t)file_size);
	fclose(fp);

	CHECK(static_cast<bool>(strstr(xml, "<Annotations") != NULL));
	CHECK(static_cast<bool>(strstr(xml, "<Annotation") != NULL));
	CHECK(static_cast<bool>(strstr(xml, "<Coordinate") != NULL));

	free(xml);
}

TEST_CASE("open TIFF fixtures through Slidescape TIFF reader") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "tiff") != 0 || !test_fixture_file_exists(fixture.path)) continue;

		tiff_t tiff = {};
		CAPTURE(fixture.path);
		REQUIRE(open_tiff_file(&tiff, fixture.path));

		CHECK(tiff.filesize > 0);
		CHECK(tiff.ifd_count > 0);
		REQUIRE(tiff.ifds != NULL);
		REQUIRE(tiff.main_image_ifd != NULL);
		CHECK(tiff.main_image_ifd->image_width > 0);
		CHECK(tiff.main_image_ifd->image_height > 0);
		CHECK(tiff.level_image_ifd_count > 0);
		CHECK(tiff.level_images_ifd != NULL);

		for (u64 level_index = 0; level_index < tiff.level_image_ifd_count; ++level_index) {
			tiff_ifd_t* ifd = tiff.level_images_ifd + level_index;
			CHECK(ifd->image_width > 0);
			CHECK(ifd->image_height > 0);
			CHECK(ifd->tile_width > 0);
			CHECK(ifd->tile_height > 0);
			CHECK(ifd->width_in_tiles > 0);
			CHECK(ifd->height_in_tiles > 0);
			CHECK(ifd->tile_count == (u64)ifd->width_in_tiles * (u64)ifd->height_in_tiles);
			CHECK(ifd->downsample_factor >= 1.0f);
		}

		tiff_destroy(&tiff);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping TIFF reader fixture checks: no TIFF fixtures are present locally.");
	}
}

TEST_CASE("TODO: TIFF fixture metadata serializes and deserializes" * doctest::skip()) {
	// This currently does not round-trip the cropped TIFF fixture metadata correctly:
	// deserialized.level_image_ifd_count differs from the original and cleanup trips ltalloc
	// guard checks. Keep this as a regression target once tiff_serialize/tiff_deserialize
	// semantics are clarified or fixed.
}

TEST_CASE("open DICOM WSI fixtures through Slidescape DICOM reader") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	for (size_t i = 0; i < manifest->fixture_count; ++i) {
		fixture_t& fixture = manifest->fixtures[i];
		if (strcmp(fixture.type, "dicom") != 0 ||
		    !fixture_has_tag(fixture, "dicom-metadata") ||
		    !test_fixture_file_exists(fixture.path)) {
			continue;
		}

		CAPTURE(fixture.id);
		CAPTURE(fixture.path);
		if (fixture_has_tag(fixture, "dicom-directory")) {
			directory_info_t shallow_directory = viewer_get_directory_info(fixture.path);
			REQUIRE(shallow_directory.is_valid);
			CHECK_FALSE(shallow_directory.contains_dicom_files);
			CHECK(arrlen(shallow_directory.directories) == 0);
			viewer_directory_info_destroy(&shallow_directory);

			directory_info_t directory = viewer_get_dicom_directory_info(fixture.path);
			REQUIRE(directory.is_valid);
			CHECK(arrlen(directory.dicom_files) == 0);
			CHECK(arrlen(directory.directories) > 0);
			CHECK(directory.contains_dicom_files);
			viewer_directory_info_destroy(&directory);
		}
		image_t* image = load_dicom_fixture_image(&fixture);

		CHECK(image->dicom.wsi.instance_count > 0);
		CHECK(image->level_count > 0);
		CHECK(image->width_in_pixels > 0);
		CHECK(image->height_in_pixels > 0);
		CHECK(image->tile_width > 0);
		CHECK(image->tile_height > 0);

		for (i32 level_index = 0; level_index < image->level_count; ++level_index) {
			level_image_t* level = image->level_images + level_index;
			if (!level->exists) continue;
			CAPTURE(level_index);
			CHECK(level->width_in_pixels > 0);
			CHECK(level->height_in_pixels > 0);
			CHECK(level->width_in_tiles > 0);
			CHECK(level->height_in_tiles > 0);
			CHECK(level->tile_count == level->width_in_tiles * level->height_in_tiles);
			CHECK(level->tile_width > 0);
			CHECK(level->tile_height > 0);
		}

		if (fixture_has_tag(fixture, "dicom-multislide")) {
			CHECK(arrlen(image->dicom.slides) > 1);
			CHECK(image->dicom.selected_slide_index >= 0);
			CHECK(image->dicom.selected_slide_index < arrlen(image->dicom.slides));
		}

		image_destroy(image);
		free(image);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping DICOM WSI metadata checks: no tagged DICOM fixture is present locally.");
	}
}

TEST_CASE("aggregate DICOM shell keeps each instance payload bound to its source") {
	const fixture_t* fixture =
		first_available_fixture("dicom", "dicom-directory");
	if (!fixture) {
		MESSAGE("Skipping aggregate shell source check: no DICOM directory fixture is present locally.");
		return;
	}
	image_t* image = load_dicom_fixture_image(fixture);
	dicom_instance_t* first = nullptr;
	dicom_instance_t* second = nullptr;
	for (i32 i = 0; i < arrlen(image->dicom.instances) && !second; ++i) {
		for (i32 j = i + 1; j < arrlen(image->dicom.instances); ++j) {
			if (strcmp(
				    image->dicom.instances[i].general_study.StudyInstanceUID.value,
				    image->dicom.instances[j].general_study.StudyInstanceUID.value) == 0 &&
			    strcmp(
				    image->dicom.instances[i].general_series.SeriesInstanceUID.value,
				    image->dicom.instances[j].general_series.SeriesInstanceUID.value) == 0) {
				first = image->dicom.instances + i;
				second = image->dicom.instances + j;
				break;
			}
		}
	}
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	struct stat first_info = {};
	struct stat second_info = {};
	REQUIRE(platform_stat(first->filename, &first_info) == 0);
	REQUIRE(platform_stat(second->filename, &second_info) == 0);
	archive_shell_build_part10_input_t instances[2] = {
		{
			.instance = first,
			.source_sha256 =
				"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			.source_byte_size = (u64)first_info.st_size,
		},
		{
			.instance = second,
			.source_sha256 =
				"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			.source_byte_size = (u64)second_info.st_size,
		},
	};
	archive_shell_build_part10_aggregate_input_t input = {
		.instances = instances,
		.instance_count = 2,
		.representation_fingerprint =
			"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
	};
	archive_shell_buffer_t encoded = {};
	char error[512] = {};
	REQUIRE(archive_shell_build_part10_aggregate(
		&input, &encoded, error, sizeof(error)));
	archive_shell_view_t shell = {};
	REQUIRE(archive_shell_open(
		&shell, encoded.data, encoded.size, error, sizeof(error)));
	for (u32 i = 0; i < 2; ++i) {
		archive_shell_instance_t instance = {};
		archive_shell_payload_range_t payload = {};
		REQUIRE(archive_shell_get_instance(&shell, i, &instance));
		REQUIRE(archive_shell_get_payload_range(
			&shell, &instance, 0, &payload));
		CHECK(instance.source_index == i);
		CHECK(payload.source_index == i);
	}
	archive_shell_buffer_destroy(&encoded);
	image_destroy(image);
	free(image);
}

TEST_CASE("P480 MRXS adapter builds a regular virtual DICOM pyramid") {
	const fixture_t* fixture = first_available_fixture("mrxs", "mrxs-p480");
	if (!fixture) {
		MESSAGE("Skipping P480 MRXS archive spike: private fixture is absent.");
		return;
	}
	const char* fingerprint =
		"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
	REQUIRE(archive_instance_provider_initialize());
	archive_mrxs_adapter_t adapter = {};
	char error[512] = {};
	REQUIRE(archive_mrxs_adapter_open(
		&adapter, fixture->path, fingerprint, error, sizeof(error)));
	CHECK(adapter.slide.level_count == 10);
	CHECK_FALSE(adapter.slide.is_fluorescence);
	CHECK_FALSE(adapter.slide.has_overlapping_tiles);
	CHECK(adapter.slide.base_width_in_tiles == 1114);
	CHECK(adapter.slide.base_height_in_tiles == 2384);
	CHECK(adapter.slide.levels[0].tile_width == 256);
	CHECK(adapter.slide.levels[0].tile_height == 256);
	CHECK(adapter.slide.levels[0].um_per_pixel_x ==
	      doctest::Approx(0.0883002207505519));

	archive_shell_buffer_t encoded = {};
	REQUIRE(archive_mrxs_adapter_build_shell(
		&adapter,
		"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		1, fingerprint, &encoded, error, sizeof(error)));
	archive_shell_view_t shell = {};
	REQUIRE(archive_shell_open(
		&shell, encoded.data, encoded.size, error, sizeof(error)));
	CHECK(shell.instance_count == 10);
	for (u32 i = 0; i < shell.instance_count; ++i) {
		archive_shell_instance_t instance = {};
		REQUIRE(archive_shell_get_instance(&shell, i, &instance));
		CHECK(instance.provider_kind == ARCHIVE_SHELL_PROVIDER_MRXS_NATIVE);
		CHECK(instance.provider_parameter_version == ARCHIVE_MRXS_ADAPTER_VERSION);
		CHECK(instance.frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_REGULAR_GRID);
		CHECK(instance.payload_map_kind == ARCHIVE_SHELL_PAYLOAD_FRAME_INDEX);
		CHECK(strcmp(instance.study_instance_uid,
		             adapter.study_instance_uid) == 0);
		CHECK(strcmp(instance.series_instance_uid,
		             adapter.series_instance_uid) == 0);
		CHECK(strcmp(instance.sop_instance_uid,
		             adapter.sop_instance_uids[i]) == 0);
		CHECK(strcmp(instance.transfer_syntax_uid,
		             "1.2.840.10008.1.2.4.50") == 0);
	}

	u32 lowest_level = (u32)adapter.slide.level_count - 1;
	u32 sample_frame = 0;
	mrxs_level_t* sample_level = adapter.slide.levels + lowest_level;
	for (i32 i = 0;
	     i < sample_level->width_in_tiles * sample_level->height_in_tiles;
	     ++i) {
		if (sample_level->tiles[i].hier_entry.length) {
			sample_frame = (u32)i + 1;
			break;
		}
	}
	REQUIRE(sample_frame > 0);
	u8* rgb = nullptr;
	size_t rgb_size = 0;
	REQUIRE(archive_mrxs_adapter_get_rgb_frame(
		&adapter, lowest_level, sample_frame, &rgb, &rgb_size,
		error, sizeof(error)));
	CHECK(rgb_size ==
	      (size_t)adapter.slide.levels[lowest_level].tile_width *
	      adapter.slide.levels[lowest_level].tile_height * 3);
	CHECK(pixel_buffer_has_variation((const u32*)rgb, rgb_size / 4));
	free(rgb);
	u8* native_jpeg = nullptr;
	size_t native_jpeg_size = 0;
	REQUIRE(archive_mrxs_adapter_get_jpeg_frame(
		&adapter, lowest_level, sample_frame,
		&native_jpeg, &native_jpeg_size, error, sizeof(error)));
	REQUIRE(native_jpeg_size > 2);
	CHECK(native_jpeg[0] == 0xff);
	CHECK(native_jpeg[1] == 0xd8);
	i32 native_width = 0;
	i32 native_height = 0;
	i32 native_channels = 0;
	u8* decoded_native = jpeg_decode_image(
		native_jpeg, (u32)native_jpeg_size,
		&native_width, &native_height, &native_channels);
	REQUIRE(decoded_native != nullptr);
	CHECK(native_width == sample_level->tile_width);
	CHECK(native_height == sample_level->tile_height);
	free(decoded_native);
	u32 blank_frame = 0;
	for (i32 i = 0;
	     i < sample_level->width_in_tiles * sample_level->height_in_tiles;
	     ++i) {
		if (!sample_level->tiles[i].hier_entry.length) {
			blank_frame = (u32)i + 1;
			break;
		}
	}
	REQUIRE(blank_frame > 0);
	u8* blank_jpeg = nullptr;
	size_t blank_jpeg_size = 0;
	REQUIRE(archive_mrxs_adapter_get_jpeg_frame(
		&adapter, lowest_level, blank_frame,
		&blank_jpeg, &blank_jpeg_size, error, sizeof(error)));
	i32 blank_width = 0;
	i32 blank_height = 0;
	i32 blank_channels = 0;
	u8* decoded_blank = jpeg_decode_image(
		blank_jpeg, (u32)blank_jpeg_size,
		&blank_width, &blank_height, &blank_channels);
	REQUIRE(decoded_blank != nullptr);
	CHECK(blank_width == sample_level->tile_width);
	CHECK(blank_height == sample_level->tile_height);
	free(decoded_blank);
	free(blank_jpeg);

	char temp_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(slidescape_test_make_temp_directory(
		temp_path, sizeof(temp_path), "slidescape-mrxs-provider"));
	archive_repository_t repository = {};
	REQUIRE(archive_repository_initialize(
		&repository, temp_path, error, sizeof(error)) == ARCHIVE_REPOSITORY_OK);
	const char descriptor[] = "slidescape-mrxs-test-bundle-v1\n";
	archive_object_t descriptor_object = {};
	REQUIRE(archive_object_store_control_bytes(
		&repository, descriptor, sizeof(descriptor) - 1,
		"application/vnd.slidescape.mrxs-bundle",
		&descriptor_object, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	archive_database_bundle_t bundle = {};
	REQUIRE(archive_database_register_mrxs_bundle(
		&repository, descriptor_object.sha256, descriptor_object.id,
		&bundle, error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	char* recipe = nullptr;
	size_t recipe_size = 0;
	REQUIRE(archive_mrxs_adapter_build_recipe(
		fixture->path, descriptor_object.sha256, descriptor_object.byte_size,
		fingerprint, &recipe, &recipe_size));
	archive_object_t recipe_object = {};
	REQUIRE(archive_object_store_control_bytes(
		&repository, recipe, recipe_size, ARCHIVE_MRXS_RECIPE_MEDIA_TYPE,
		&recipe_object, error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	free(recipe);
	archive_shell_buffer_destroy(&encoded);
	REQUIRE(archive_mrxs_adapter_build_shell(
		&adapter, descriptor_object.sha256, descriptor_object.byte_size,
		fingerprint, &encoded, error, sizeof(error)));
	archive_object_t shell_object = {};
	REQUIRE(archive_object_store_shell_bytes(
		&repository, encoded.data, encoded.size, &shell_object,
		error, sizeof(error)) == ARCHIVE_OBJECT_OK);
	archive_database_shell_instance_publication_t published[ARCHIVE_MRXS_MAX_LEVELS] = {};
	for (i32 i = 0; i < adapter.slide.level_count; ++i) {
		mrxs_level_t* level = adapter.slide.levels + i;
		archive_database_part10_member_t* instance = &published[i].instance;
		instance->object_id = descriptor_object.id;
		snprintf(instance->study_instance_uid,
		         sizeof(instance->study_instance_uid), "%s",
		         adapter.study_instance_uid);
		snprintf(instance->series_instance_uid,
		         sizeof(instance->series_instance_uid), "%s",
		         adapter.series_instance_uid);
		snprintf(instance->sop_instance_uid,
		         sizeof(instance->sop_instance_uid), "%s",
		         adapter.sop_instance_uids[i]);
		snprintf(instance->sop_class_uid, sizeof(instance->sop_class_uid),
		         "1.2.840.10008.5.1.4.1.1.77.1.6");
		snprintf(instance->transfer_syntax_uid,
		         sizeof(instance->transfer_syntax_uid),
		         "1.2.840.10008.1.2.4.50");
		snprintf(instance->modality, sizeof(instance->modality), "SM");
		snprintf(instance->image_flavor, sizeof(instance->image_flavor), "VOLUME");
		instance->has_instance_number = true;
		instance->instance_number = i + 1;
		instance->frame_count =
			(i64)level->width_in_tiles * level->height_in_tiles;
		instance->rows = level->tile_height;
		instance->columns = level->tile_width;
		u32 scale = 1u << i;
		instance->total_pixel_matrix_rows =
			(adapter.slide.base_height_in_pixels + scale - 1) / scale;
		instance->total_pixel_matrix_columns =
			(adapter.slide.base_width_in_pixels + scale - 1) / scale;
		published[i].shell_instance_index = (u32)i;
	}
	archive_database_shell_publication_t publication = {};
	snprintf(publication.representation_id,
	         sizeof(publication.representation_id), "%s",
	         adapter.series_instance_uid);
	publication.shell_object_id = shell_object.id;
	publication.recipe_object_id = recipe_object.id;
	snprintf(publication.representation_fingerprint,
	         sizeof(publication.representation_fingerprint), "%s", fingerprint);
	publication.instances = published;
	publication.instance_count = adapter.slide.level_count;
	snprintf(publication.provider_kind, sizeof(publication.provider_kind),
	         "mrxs-native");
	snprintf(publication.adapter_version, sizeof(publication.adapter_version), "2");
	snprintf(publication.frame_media_type, sizeof(publication.frame_media_type),
	         "image/jpeg");
	snprintf(publication.frame_source, sizeof(publication.frame_source), "GENERATED");
	snprintf(publication.frame_policy_version,
	         sizeof(publication.frame_policy_version), "mrxs-jpeg-v2");
	REQUIRE(archive_database_publish_shells(
		&repository, bundle.id, &publication, 1,
		error, sizeof(error)) == ARCHIVE_DATABASE_OK);
	REQUIRE(archive_database_publish_shells(
		&repository, bundle.id, &publication, 1,
		error, sizeof(error)) == ARCHIVE_DATABASE_OK);

	archive_index_t index = {};
	REQUIRE(archive_index_open(&index, &repository, error, sizeof(error)));
	REQUIRE(index.instance_count == adapter.slide.level_count);
	const archive_instance_record_t* record =
		archive_index_find_instance(
			&index, adapter.study_instance_uid, adapter.series_instance_uid,
			adapter.sop_instance_uids[lowest_level]);
	REQUIRE(record != nullptr);
	CHECK(strcmp(record->provider, "mrxs-native") == 0);
	char* metadata = nullptr;
	size_t metadata_size = 0;
	CAPTURE(error);
	REQUIRE(archive_instance_provider_get_metadata(
		&repository, record, &metadata, &metadata_size,
		error, sizeof(error)));
	CHECK(strstr(metadata, adapter.sop_instance_uids[lowest_level]) != nullptr);
	free(metadata);
	archive_frame_representation_t representation = {};
	REQUIRE(archive_instance_provider_get_frame_representations(
		&repository, record, &representation, 1,
		error, sizeof(error)) == 1);
	CHECK(representation.source == ARCHIVE_FRAME_REPRESENTATION_GENERATED);
	CHECK(strcmp(representation.media_type, "image/jpeg") == 0);
	archive_instance_frame_t provided_frame = {};
	REQUIRE(archive_instance_provider_get_frame(
		&repository, record, sample_frame, &representation,
		&provided_frame, error, sizeof(error)));
	CHECK(provided_frame.size ==
	      native_jpeg_size);
	CHECK(provided_frame.data[0] == 0xff);
	CHECK(provided_frame.data[1] == 0xd8);
	CHECK(memcmp(provided_frame.data, native_jpeg, native_jpeg_size) == 0);
	archive_instance_frame_destroy(&provided_frame);
	free(native_jpeg);
	archive_index_destroy(&index);

	char shell_path[ARCHIVE_PATH_CAPACITY];
	REQUIRE(archive_repository_make_path(
		&repository, shell_object.storage_key,
		shell_path, sizeof(shell_path)));
	CHECK(remove(shell_path) == 0);
#if !defined(_WIN32)
	char path[ARCHIVE_PATH_CAPACITY];
	snprintf(path, sizeof(path), "%s/shells/sha256/%.2s",
	         temp_path, shell_object.sha256);
	CHECK(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/shells/sha256", temp_path);
	CHECK(rmdir(path) == 0);
	const char* children[] = {
		"staging", "derived", "representations", "bundles", "shells", "objects"
	};
	for (const char* child : children) {
		snprintf(path, sizeof(path), "%s/%s", temp_path, child);
		CHECK(rmdir(path) == 0);
	}
	snprintf(path, sizeof(path), "%s/repository.json", temp_path);
	CHECK(remove(path) == 0);
	remove_archive_database_files(temp_path);
	CHECK(rmdir(temp_path) == 0);
#endif
	archive_shell_buffer_destroy(&encoded);
	archive_mrxs_adapter_close(&adapter);
	archive_instance_provider_shutdown();
}

TEST_CASE("select each slide in a local multi-slide DICOM directory") {
	const fixture_t* fixture = first_available_fixture("dicom", "dicom-multislide");
	if (!fixture) {
		MESSAGE("Skipping local DICOM slide selection check: no multi-slide fixture is present locally.");
		return;
	}

	image_t* initial_image = load_dicom_fixture_image(fixture);
	i32 slide_count = arrlen(initial_image->dicom.slides);
	REQUIRE(slide_count > 1);
	if (fixture_has_tag(*fixture, "dicom-jpeg2000")) {
		for (i32 slide_index = 0; slide_index < slide_count; ++slide_index) {
			CAPTURE(slide_index);
			CHECK(trim_whitespace(initial_image->dicom.slides[slide_index].series_description)[0] != '\0');
		}
	}
	dicom_slide_t* slides = (dicom_slide_t*)calloc((size_t)slide_count, sizeof(dicom_slide_t));
	REQUIRE(slides != NULL);
	memcpy(slides, initial_image->dicom.slides, (size_t)slide_count * sizeof(dicom_slide_t));
	image_destroy(initial_image);
	free(initial_image);

	for (i32 expected_index = 0; expected_index < slide_count; ++expected_index) {
		image_t* selected_image = load_selected_dicom_fixture_image(
			fixture, slides[expected_index].representative_filename);
		CAPTURE(expected_index);
		CAPTURE(slides[expected_index].representative_filename);
		CHECK(arrlen(selected_image->dicom.slides) == slide_count);
		CHECK(selected_image->dicom.selected_slide_index == expected_index);
		REQUIRE(selected_image->dicom.selected_slide_index >= 0);
		CHECK(strcmp(selected_image->dicom.slides[selected_image->dicom.selected_slide_index]
		                 .series_instance_uid.value,
		             slides[expected_index].series_instance_uid.value) == 0);
		image_destroy(selected_image);
		free(selected_image);
	}
	free(slides);
}

TEST_CASE("real sparse DICOM fixture round-trips its explicit frame map") {
	const fixture_t* fixture = first_available_fixture("dicom", "dicom-sparse");
	if (!fixture) {
		MESSAGE("Skipping sparse DICOM frame-map check: no tagged fixture is present locally.");
		return;
	}

	ensure_dicom_initialized();
	CAPTURE(fixture->id);
	CAPTURE(fixture->path);
	dicom_series_t dicom = {};
	REQUIRE(dicom_open_single_instance_from_path(&dicom, fixture->path));
	REQUIRE(arrlen(dicom.instances) == 1);
	dicom_instance_t* instance = dicom.instances;
	REQUIRE_FALSE(instance->is_tiled_full);
	REQUIRE(instance->number_of_frames > 1);
	REQUIRE(arrlen(instance->per_frame_plane_position_slide) ==
	        instance->number_of_frames);
	REQUIRE(arrlen(instance->optical_paths) == 1);
	REQUIRE(instance->optical_paths[0].OpticalPathIdentifier.present);
	if (instance->number_of_optical_paths) {
		CHECK(instance->number_of_optical_paths ==
		      (u32)arrlen(instance->optical_paths));
	}

	u64 tiles_per_row =
		((u64)instance->total_pixel_matrix_columns + instance->columns - 1) /
		instance->columns;
	u64 tiles_per_column =
		((u64)instance->total_pixel_matrix_rows + instance->rows - 1) /
		instance->rows;
	REQUIRE(tiles_per_row > 0);
	REQUIRE(tiles_per_column > 0);

	char error[512] = {};
	archive_frame_map_t map = {};
	bool map_built =
		archive_frame_map_build_part10(&map, instance, error, sizeof(error));
	CAPTURE(error);
	REQUIRE(map_built);
	REQUIRE(map.frame_count == instance->number_of_frames);
	CHECK(map.rows == instance->rows);
	CHECK(map.columns == instance->columns);
	CHECK(map.total_pixel_matrix_rows == instance->total_pixel_matrix_rows);
	CHECK(map.total_pixel_matrix_columns == instance->total_pixel_matrix_columns);
	CHECK(map.optical_path_count == (u32)arrlen(instance->optical_paths));

	bool differs_from_implicit_raster_order = false;
	for (u32 frame = 0; frame < map.frame_count; ++frame) {
		const dicom_plane_position_slide_t* source =
			instance->per_frame_plane_position_slide + frame;
		const archive_frame_map_entry_t* entry = map.entries + frame;
		CAPTURE(frame);
		CHECK(entry->row_position ==
		      (u32)source->row_position_in_total_image_pixel_matrix);
		CHECK(entry->column_position ==
		      (u32)source->column_position_in_total_image_pixel_matrix);
		CHECK(entry->x_offset_mm ==
		      doctest::Approx(source->offset_in_slide_coordinate_system.x));
		CHECK(entry->y_offset_mm ==
		      doctest::Approx(source->offset_in_slide_coordinate_system.y));
		CHECK(entry->z_offset_um ==
		      doctest::Approx(source->z_offset_in_slide_coordinate_system));
		CHECK(entry->optical_path_index < map.optical_path_count);
		CHECK(entry->focal_plane_index < map.focal_plane_count);
		CHECK((entry->flags & ARCHIVE_FRAME_MAP_ENTRY_XY_OFFSET_KNOWN) != 0);
		CHECK((entry->flags & ARCHIVE_FRAME_MAP_ENTRY_Z_OFFSET_KNOWN) != 0);
		CHECK((entry->flags & ARCHIVE_FRAME_MAP_ENTRY_OPTICAL_PATH_KNOWN) != 0);

		u32 implicit_column =
			(u32)(((u64)frame % tiles_per_row) * instance->columns + 1);
		u32 implicit_row =
			(u32)(((u64)frame / tiles_per_row) * instance->rows + 1);
		if (entry->column_position != implicit_column ||
		    entry->row_position != implicit_row) {
			differs_from_implicit_raster_order = true;
		}
	}
	CHECK(differs_from_implicit_raster_order);

	size_t encoded_size = ARCHIVE_FRAME_MAP_HEADER_SIZE +
		(size_t)map.frame_count * ARCHIVE_FRAME_MAP_ENTRY_SIZE;
	u8* encoded = (u8*)malloc(encoded_size);
	REQUIRE(encoded != NULL);
	size_t written = 0;
	REQUIRE(archive_frame_map_serialize(&map, encoded, encoded_size, &written,
	                                   error, sizeof(error)));
	REQUIRE(written == encoded_size);

	archive_frame_map_t parsed = {};
	REQUIRE(archive_frame_map_parse(&parsed, encoded, encoded_size,
	                                error, sizeof(error)));
	REQUIRE(parsed.frame_count == map.frame_count);
	for (u32 frame = 0; frame < parsed.frame_count; ++frame) {
		CAPTURE(frame);
		CHECK(memcmp(parsed.entries + frame, map.entries + frame,
		             sizeof(*map.entries)) == 0);
	}

	struct stat source_info = {};
	REQUIRE(platform_stat(fixture->path, &source_info) == 0);
	archive_shell_build_part10_input_t shell_input = {};
	shell_input.instance = instance;
	shell_input.source_sha256 =
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	shell_input.source_byte_size = (u64)source_info.st_size;
	shell_input.representation_fingerprint =
		"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
	archive_shell_buffer_t encoded_shell = {};
	REQUIRE(archive_shell_build_part10(&shell_input, &encoded_shell,
	                                  error, sizeof(error)));
	archive_shell_view_t shell_view = {};
	REQUIRE(archive_shell_open(&shell_view, encoded_shell.data, encoded_shell.size,
	                           error, sizeof(error)));
	archive_shell_instance_t shell_instance = {};
	REQUIRE(archive_shell_get_instance(&shell_view, 0, &shell_instance));
	CHECK(shell_instance.frame_map_kind == ARCHIVE_SHELL_FRAME_MAP_EXPLICIT);
	u32 sampled_frames[] = {0, shell_instance.frame_count / 2,
	                        shell_instance.frame_count - 1};
	for (u32 frame_index : sampled_frames) {
		archive_shell_frame_t shell_frame = {};
		REQUIRE(archive_shell_get_frame(&shell_view, &shell_instance,
		                                frame_index, &shell_frame));
		CHECK(shell_frame.row_position == map.entries[frame_index].row_position);
		CHECK(shell_frame.column_position == map.entries[frame_index].column_position);
		archive_shell_payload_range_t payload = {};
		REQUIRE(archive_shell_get_payload_range(&shell_view, &shell_instance,
		                                        frame_index, &payload));
		CHECK(payload.offset + payload.size <= shell_input.source_byte_size);
	}
	archive_shell_buffer_destroy(&encoded_shell);

	archive_frame_map_destroy(&parsed);
	free(encoded);
	archive_frame_map_destroy(&map);
	dicom_destroy(&dicom);
}

TEST_CASE("decode a representative DICOM WSI frame") {
	fixture_manifest_t* manifest = fixture_manifest();
	bool checked_any = false;
	bool checked_archive_provider = false;
	for (size_t fixture_index = 0; fixture_index < manifest->fixture_count; ++fixture_index) {
		fixture_t& fixture = manifest->fixtures[fixture_index];
		if (strcmp(fixture.type, "dicom") != 0 ||
		    !fixture_has_tag(fixture, "dicom-tile") ||
		    !test_fixture_file_exists(fixture.path)) {
			continue;
		}

		CAPTURE(fixture.id);
		CAPTURE(fixture.path);
		image_t* image = load_dicom_fixture_image(&fixture);
		#if !defined(_WIN32)
		if (!checked_archive_provider) {
			dicom_instance_t* smallest = nullptr;
			u64 smallest_size = UINT64_MAX;
			for (i32 i = 0; i < arrlen(image->dicom.instances); ++i) {
				dicom_instance_t* candidate = image->dicom.instances + i;
				struct stat info = {};
				bool complete =
					candidate->general_study.StudyInstanceUID.value[0] &&
					candidate->general_series.SeriesInstanceUID.value[0] &&
					candidate->sop_instance_uid.value[0] &&
					candidate->sop_class_uid.value[0] &&
					candidate->transfer_syntax_uid.value[0] &&
					candidate->number_of_frames > 0 &&
					platform_stat(candidate->filename, &info) == 0 &&
					info.st_size > 0;
				if (complete && (u64)info.st_size < smallest_size) {
					smallest = candidate;
					smallest_size = (u64)info.st_size;
				}
			}
			/*
			 * The provider integration path uses the development reference mode
			 * and repeatedly verifies its source through the content-addressed
			 * link. Keep that coverage on a small representative instance;
			 * direct parsing/shell/decode checks still cover every configured
			 * fixture without copying volume objects.
			 */
			if (smallest && smallest_size <= MEGABYTES(16)) {
				check_archive_part10_provider(smallest, 1);
				checked_archive_provider = true;
			}
		}
		#endif
		if (fixture_has_tag(fixture, "dicom-jpeg2000")) {
			bool found_jpeg2000 = false;
			for (i32 i = 0; i < arrlen(image->dicom.instances); ++i) {
				dicom_uid_enum syntax = image->dicom.instances[i].transfer_syntax_uid.as_enum;
				if (syntax == DICOM_JPEG2000Lossless || syntax == DICOM_JPEG2000) {
					found_jpeg2000 = true;
					break;
				}
			}
			CHECK(found_jpeg2000);
		}

		level_image_t* level = NULL;
		i32 level_index = -1;
		i32 tile_index = -1;
		for (i32 candidate_level = 0; candidate_level < image->level_count && tile_index < 0; ++candidate_level) {
			level_image_t* candidate = image->level_images + candidate_level;
			if (!candidate->exists) continue;
			for (i32 candidate_tile = 0; candidate_tile < candidate->tile_count; ++candidate_tile) {
				if (!candidate->tiles[candidate_tile].is_empty) {
					level = candidate;
					level_index = candidate_level;
					tile_index = candidate_tile;
					break;
				}
			}
		}

		REQUIRE(level != NULL);
		REQUIRE(level_index >= 0);
		REQUIRE(tile_index >= 0);
		CAPTURE(level_index);
		CAPTURE(tile_index);

		dicom_instance_t* level_instance = image->dicom.wsi.level_instances[level->pyramid_image_index];
		REQUIRE(level_instance != NULL);
		dicom_tile_t* dicom_tile = level_instance->tiles + tile_index;
		REQUIRE(dicom_tile->exists);
		if (!dicom_tile->is_offset_known) {
			REQUIRE(dicom_wsi_index_level(&image->dicom, level->pyramid_image_index));
		}
		REQUIRE(dicom_tile->is_offset_known);
		REQUIRE(dicom_tile->data_size > 0);

		u8* pixels = dicom_wsi_decode_tile_to_bgra(&image->dicom, level->pyramid_image_index, tile_index);
		REQUIRE(pixels != NULL);
		CHECK(pixel_buffer_has_variation((u32*)pixels, (size_t)level->tile_width * (size_t)level->tile_height));

		if (fixture_has_tag(fixture, "dicom-concatenation")) {
			bool uses_multiple_instances = false;
			dicom_instance_t* first_instance = NULL;
			for (i32 i = 0; i < level_instance->tile_count; ++i) {
				dicom_tile_t* tile = level_instance->tiles + i;
				if (!tile->exists) continue;
				if (!first_instance) {
					first_instance = tile->instance;
				} else if (tile->instance != first_instance) {
					uses_multiple_instances = true;
					break;
				}
			}
			CHECK(uses_multiple_instances);
		}

		free(pixels);
		image_destroy(image);
		free(image);
		checked_any = true;
	}

	if (!checked_any) {
		MESSAGE("Skipping DICOM WSI frame decode check: no tagged DICOM fixture is present locally.");
	}
}

TEST_CASE("decode a representative TIFF tile") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping TIFF tile decode check: no TIFF fixture is present locally.");
		return;
	}

	tiff_t tiff = {};
	REQUIRE(open_tiff_file(&tiff, fixture->path));
	REQUIRE(tiff.main_image_ifd != NULL);
	tiff_ifd_t* ifd = tiff.main_image_ifd;
	REQUIRE(ifd->tile_count > 0);

	u8* pixels = NULL;
	i32 tile_index = -1;
	for (u64 i = 0; i < ifd->tile_count; ++i) {
		if (ifd->tile_offsets[i] == 0 || ifd->tile_byte_counts[i] == 0) continue;
		pixels = tiff_decode_tile(0, &tiff, ifd, (i32)i, 0, (i32)(i % ifd->width_in_tiles), (i32)(i / ifd->width_in_tiles));
		if (pixels) {
			tile_index = (i32)i;
			break;
		}
	}

	CAPTURE(tile_index);
	REQUIRE(pixels != NULL);
	size_t pixel_count = (size_t)ifd->tile_width * (size_t)ifd->tile_height;
	CHECK(pixel_buffer_has_variation((u32*)pixels, pixel_count));

	free(pixels);
	tiff_destroy(&tiff);
}

TEST_CASE("load TIFF fixture as image_t and read a tile region") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping image_read_region TIFF check: no TIFF fixture is present locally.");
		return;
	}

	ensure_image_read_region_thread_pool();

	image_t* image = load_tiff_fixture_image(fixture);
	level_image_t* level_image = image->level_images + 0;
	REQUIRE(level_image->exists);

	selected_tiff_tile_t tile = {};
	REQUIRE(select_decodable_tiff_tile(image, &tile));
	CAPTURE(fixture->path);
	CAPTURE(tile.tile_index);
	CAPTURE(tile.tile_x);
	CAPTURE(tile.tile_y);

	i32 x = tile.tile_x * (i32)level_image->tile_width;
	i32 y = tile.tile_y * (i32)level_image->tile_height;
	size_t region_size = (size_t)tile.read_width * (size_t)tile.read_height * sizeof(u32);
	u32* region = (u32*)calloc((size_t)tile.read_width * (size_t)tile.read_height, sizeof(u32));
	REQUIRE(region != NULL);

	REQUIRE(image_read_region(image, 0, x, y, tile.read_width, tile.read_height, region, PIXEL_FORMAT_U8_BGRA));
	for (i32 row = 0; row < tile.read_height; ++row) {
		u32* expected_row = (u32*)tile.pixels + (size_t)row * (size_t)level_image->tile_width;
		u32* actual_row = region + (size_t)row * (size_t)tile.read_width;
		CHECK(memcmp(actual_row, expected_row, (size_t)tile.read_width * sizeof(u32)) == 0);
	}
	CHECK(region_size > 0);

	free(region);
	free(tile.pixels);
	image_destroy(image);
	free(image);
}

TEST_CASE("image_read_region converts TIFF BGRA pixels to F32 luminance") {
	const fixture_t* fixture = first_available_fixture("tiff", "tiff-tile");
	if (!fixture) fixture = first_available_fixture("tiff");
	if (!fixture) {
		MESSAGE("Skipping image_read_region F32 conversion check: no TIFF fixture is present locally.");
		return;
	}

	ensure_image_read_region_thread_pool();

	image_t* image = load_tiff_fixture_image(fixture);
	level_image_t* level_image = image->level_images + 0;
	REQUIRE(level_image->exists);

	selected_tiff_tile_t tile = {};
	REQUIRE(select_decodable_tiff_tile(image, &tile));

	i32 read_width = MIN(tile.read_width, 16);
	i32 read_height = MIN(tile.read_height, 16);
	REQUIRE(read_width > 0);
	REQUIRE(read_height > 0);

	i32 x = tile.tile_x * (i32)level_image->tile_width;
	i32 y = tile.tile_y * (i32)level_image->tile_height;
	float* luminance = (float*)calloc((size_t)read_width * (size_t)read_height, sizeof(float));
	REQUIRE(luminance != NULL);

	REQUIRE(image_read_region(image, 0, x, y, read_width, read_height, luminance, PIXEL_FORMAT_F32_Y));
	for (i32 row = 0; row < read_height; ++row) {
		for (i32 col = 0; col < read_width; ++col) {
			u8* bgra = tile.pixels + ((size_t)row * (size_t)level_image->tile_width + (size_t)col) * 4;
			float b = (float)bgra[0] * (1.0f / 255.0f);
			float g = (float)bgra[1] * (1.0f / 255.0f);
			float r = (float)bgra[2] * (1.0f / 255.0f);
			float expected = f32_rgb_to_f32_y(r, g, b);
			CHECK(luminance[(size_t)row * (size_t)read_width + (size_t)col] == doctest::Approx(expected));
		}
	}

	free(luminance);
	free(tile.pixels);
	image_destroy(image);
	free(image);
}

TEST_CASE("open iSyntax fixture and read WSI metadata through libisyntax" ) {
	const fixture_t* fixture = first_available_fixture("isyntax", "isyntax-metadata");
	if (!fixture) fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax metadata check: no iSyntax fixture is present locally.");
		return;
	}

	REQUIRE(libisyntax_init() == LIBISYNTAX_OK);

	isyntax_t* isyntax = NULL;
	REQUIRE(libisyntax_open(fixture->path, (libisyntax_open_flags_t)0, &isyntax) == LIBISYNTAX_OK);
	REQUIRE(isyntax != NULL);

	CHECK(libisyntax_get_tile_width(isyntax) > 0);
	CHECK(libisyntax_get_tile_height(isyntax) > 0);
	CHECK(isyntax->image_count > 0);
	CHECK(isyntax->wsi_image_index >= 0);
	CHECK(isyntax->wsi_image_index < isyntax->image_count);

	const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax);
	REQUIRE(wsi != NULL);
	i32 level_count = libisyntax_image_get_level_count(wsi);
	CHECK(level_count > 0);

	for (i32 level_index = 0; level_index < level_count; ++level_index) {
		const isyntax_level_t* level = libisyntax_image_get_level(wsi, level_index);
		CAPTURE(level_index);
		CHECK(libisyntax_level_get_scale(level) >= 0);
		CHECK(libisyntax_level_get_width(level) > 0);
		CHECK(libisyntax_level_get_height(level) > 0);
		CHECK(libisyntax_level_get_width_in_tiles(level) > 0);
		CHECK(libisyntax_level_get_height_in_tiles(level) > 0);
		CHECK(libisyntax_level_get_mpp_x(level) > 0.0f);
		CHECK(libisyntax_level_get_mpp_y(level) > 0.0f);
	}

	libisyntax_close(isyntax);
}

TEST_CASE("decode an iSyntax tile through libisyntax") {
	const fixture_t* fixture = first_available_fixture("isyntax", "isyntax-tile");
	if (!fixture) fixture = first_available_fixture("isyntax");
	if (!fixture) {
		MESSAGE("Skipping iSyntax tile decode check: no iSyntax fixture is present locally.");
		return;
	}

	REQUIRE(libisyntax_init() == LIBISYNTAX_OK);

	isyntax_t* isyntax = NULL;
	REQUIRE(libisyntax_open(fixture->path, (libisyntax_open_flags_t)0, &isyntax) == LIBISYNTAX_OK);
	REQUIRE(isyntax != NULL);

	isyntax_cache_t* cache = NULL;
	REQUIRE(libisyntax_cache_create("slidescape_tests iSyntax cache", 256, &cache) == LIBISYNTAX_OK);
	REQUIRE(cache != NULL);
	REQUIRE(libisyntax_cache_inject(cache, isyntax) == LIBISYNTAX_OK);

	const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax);
	REQUIRE(wsi != NULL);
	i32 level_count = libisyntax_image_get_level_count(wsi);
	REQUIRE(level_count > 0);

	const isyntax_level_t* selected_level = NULL;
	const isyntax_tile_t* selected_tile = NULL;
	i32 selected_level_index = -1;
	for (i32 level_index = level_count - 1; level_index >= 0; --level_index) {
		const isyntax_level_t* level = libisyntax_image_get_level(wsi, level_index);
		if (!level || !level->tiles) continue;
		for (u64 tile_index = 0; tile_index < level->tile_count; ++tile_index) {
			const isyntax_tile_t* tile = level->tiles + tile_index;
			if (tile->exists) {
				selected_level = level;
				selected_tile = tile;
				selected_level_index = level_index;
				break;
			}
		}
		if (selected_tile) break;
	}

	REQUIRE(selected_level != NULL);
	REQUIRE(selected_tile != NULL);
	CAPTURE(selected_level_index);
	CAPTURE(selected_tile->tile_x);
	CAPTURE(selected_tile->tile_y);

	i32 tile_width = libisyntax_get_tile_width(isyntax);
	i32 tile_height = libisyntax_get_tile_height(isyntax);
	REQUIRE(tile_width > 0);
	REQUIRE(tile_height > 0);

	size_t pixel_count = (size_t)tile_width * (size_t)tile_height;
	u32* pixels = (u32*)calloc(pixel_count, sizeof(u32));
	REQUIRE(static_cast<bool>(pixels != NULL));

	REQUIRE(libisyntax_tile_read(isyntax, cache, selected_level_index, selected_tile->tile_x, selected_tile->tile_y,
	                             pixels, LIBISYNTAX_PIXEL_FORMAT_RGBA) == LIBISYNTAX_OK);
	CHECK(pixel_buffer_has_variation(pixels, pixel_count));

	free(pixels);
	libisyntax_cache_destroy(cache);
	libisyntax_close(isyntax);
}

TEST_CASE("TODO: load ASAP XML fixture through annotation parser" * doctest::skip()) {
	// This should call load_asap_xml_annotations() or a lower-level parser once annotation loading can
	// be exercised without constructing a full app_state_t/GUI viewer context.
}
