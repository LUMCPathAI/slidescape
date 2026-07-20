#include "doctest.h"

#include "dicomweb.h"

#ifndef SLIDESCAPE_TEST_DICOMWEB_FIXTURE_DIR
#define SLIDESCAPE_TEST_DICOMWEB_FIXTURE_DIR "tests/fixtures/dicomweb"
#endif

static std::string read_dicomweb_fixture(const char* filename) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", SLIDESCAPE_TEST_DICOMWEB_FIXTURE_DIR, filename);
	FILE* file = fopen(path, "rb");
	if (!file) return {};
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size < 0) {
		fclose(file);
		return {};
	}
	std::string contents((size_t)size, '\0');
	if (size > 0 && fread(contents.data(), 1, (size_t)size, file) != (size_t)size) {
		contents.clear();
	}
	fclose(file);
	return contents;
}

TEST_CASE("DICOMweb parses WSI DICOM JSON metadata") {
	const char* json =
		"[{"
		"\"00080008\":{\"vr\":\"CS\",\"Value\":[\"DERIVED\",\"PRIMARY\",\"VOLUME\",\"RESAMPLED\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"1.2.3.4\"]},"
		"\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"1.2.3\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"1.2.3.1\"]},"
		"\"00280002\":{\"vr\":\"US\",\"Value\":[3]},"
		"\"00280004\":{\"vr\":\"CS\",\"Value\":[\"YBR_FULL_422\"]},"
		"\"00280008\":{\"vr\":\"IS\",\"Value\":[160]},"
		"\"00280010\":{\"vr\":\"US\",\"Value\":[256]},"
		"\"00280011\":{\"vr\":\"US\",\"Value\":[256]},"
		"\"00480006\":{\"vr\":\"UL\",\"Value\":[3859]},"
		"\"00480007\":{\"vr\":\"UL\",\"Value\":[2543]},"
		"\"00209311\":{\"vr\":\"CS\",\"Value\":[\"TILED_FULL\"]},"
		"\"52009229\":{\"vr\":\"SQ\",\"Value\":[{\"00289110\":{\"vr\":\"SQ\",\"Value\":[{"
		"\"00280030\":{\"vr\":\"DS\",\"Value\":[\"0.008005837\",\"0.008005837\"]}"
		"}]}}]}"
		"}]";
	dicomweb_instance_metadata_t metadata = {};
	REQUIRE(dicomweb_parse_instance_metadata(json, strlen(json), &metadata));
	CHECK(strcmp(metadata.study_instance_uid, "1.2.3") == 0);
	CHECK(strcmp(metadata.series_instance_uid, "1.2.3.1") == 0);
	CHECK(strcmp(metadata.sop_instance_uid, "1.2.3.4") == 0);
	CHECK(strcmp(metadata.image_flavor, "VOLUME") == 0);
	CHECK(metadata.number_of_frames == 160);
	CHECK(metadata.rows == 256);
	CHECK(metadata.columns == 256);
	CHECK(metadata.total_pixel_matrix_columns == 3859);
	CHECK(metadata.total_pixel_matrix_rows == 2543);
	CHECK(metadata.is_tiled_full);
	CHECK(metadata.is_mpp_known);
	CHECK(metadata.mpp_x == doctest::Approx(8.005837f));
	CHECK(metadata.mpp_y == doctest::Approx(8.005837f));
	REQUIRE(metadata.frame_numbers != nullptr);
	CHECK(metadata.frame_map_count == 160);
	CHECK(metadata.frame_numbers[0] == 1);
	CHECK(metadata.frame_numbers[159] == 160);
	dicomweb_instance_metadata_destroy(&metadata);
}

TEST_CASE("DICOMweb extracts a native frame from multipart related") {
	const char* content_type =
		"multipart/related; type=\"image/jpeg\"; boundary=frame-boundary";
	const char body[] =
		"--frame-boundary\r\n"
		"Content-Type: image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.50\r\n"
		"\r\n"
		"\xff\xd8\x01\x02\xff\xd9"
		"\r\n--frame-boundary--\r\n";
	dicomweb_frame_t frame = {};
	REQUIRE(dicomweb_parse_multipart_frame((const u8*)body, sizeof(body) - 1,
	                                       content_type, &frame));
	CHECK(frame.size == 6);
	CHECK(frame.data[0] == 0xff);
	CHECK(frame.data[1] == 0xd8);
	CHECK(strcmp(frame.media_type, "image/jpeg") == 0);
	CHECK(strcmp(frame.transfer_syntax_uid, "1.2.840.10008.1.2.4.50") == 0);
}

TEST_CASE("DICOMweb rejects oversized multipart part headers") {
	const char* content_type = "multipart/related; boundary=frame-boundary";
	std::string body = "--frame-boundary\r\nX-Large: ";
	body.append(KILOBYTES(17), 'x');
	body += "\r\n\r\nframe\r\n--frame-boundary--\r\n";
	dicomweb_frame_t frame = {};
	CHECK_FALSE(dicomweb_parse_multipart_frame((const u8*)body.data(), body.size(),
	                                           content_type, &frame));
}

TEST_CASE("DICOMweb builds portable WADO-RS paths") {
	char path[512];
	REQUIRE(dicomweb_build_instance_metadata_path(path, sizeof(path), "/dicom-web",
	                                              "study", "series", "instance"));
	CHECK(strcmp(path, "/dicom-web/studies/study/series/series/instances/instance/metadata") == 0);
	REQUIRE(dicomweb_build_frame_path(path, sizeof(path), "/dicom-web/",
	                                  "study", "series", "instance", 17));
	CHECK(strcmp(path, "/dicom-web/studies/study/series/series/instances/instance/frames/17") == 0);
	CHECK_FALSE(dicomweb_build_frame_path(path, sizeof(path), "/dicom-web/",
	                                      "study", "series", "instance", 0));
}

TEST_CASE("DICOMweb resolves IDC Slim viewer series URLs") {
	const char* viewer_url =
		"https://viewer.imaging.datacommons.cancer.gov/slim/studies/"
		"2.25.211094631316408413440371843585977094852/series/"
		"1.3.6.1.4.1.5962.99.1.208792987.352384958.1640886332827.2.0";
	char resolved[1024];
	REQUIRE(dicomweb_resolve_series_url(resolved, sizeof(resolved), viewer_url));
	CHECK(strcmp(resolved,
		"https://proxy.imaging.datacommons.cancer.gov/current/"
		"viewer-only-no-downloads-see-tinyurl-dot-com-slash-3j3d9jyp/dicomWeb/studies/"
		"2.25.211094631316408413440371843585977094852/series/"
		"1.3.6.1.4.1.5962.99.1.208792987.352384958.1640886332827.2.0") == 0);

	CHECK_FALSE(dicomweb_resolve_series_url(
		resolved, sizeof(resolved),
		"https://viewer.imaging.datacommons.cancer.gov/slim/studies/not-a-uid/series/1.2.3"));
	REQUIRE(dicomweb_resolve_series_url(resolved, sizeof(resolved),
	                                  "https://archive.example/dicomWeb/studies/1/series/2"));
	CHECK(strcmp(resolved, "https://archive.example/dicomWeb/studies/1/series/2") == 0);
}

TEST_CASE("DICOMweb validates UID syntax") {
	CHECK(dicomweb_is_valid_uid("1.2.840.10008.1", strlen("1.2.840.10008.1")));
	CHECK(dicomweb_is_valid_uid("2.25.211094631316408413440371843585977094852",
	                            strlen("2.25.211094631316408413440371843585977094852")));
	CHECK_FALSE(dicomweb_is_valid_uid("", 0));
	CHECK_FALSE(dicomweb_is_valid_uid(".1.2", 4));
	CHECK_FALSE(dicomweb_is_valid_uid("1..2", 4));
	CHECK_FALSE(dicomweb_is_valid_uid("1.2\r\nX-Test:1", strlen("1.2\r\nX-Test:1")));
}

TEST_CASE("DICOMweb percent-encodes QIDO matching values") {
	char encoded[128];
	REQUIRE(dicomweb_encode_query_value(encoded, sizeof(encoded), "*H&E stain / lung*"));
	CHECK(strcmp(encoded, "%2AH%26E%20stain%20%2F%20lung%2A") == 0);
	char too_small[4];
	CHECK_FALSE(dicomweb_encode_query_value(too_small, sizeof(too_small), "a b"));
	CHECK(too_small[0] == '\0');
}

TEST_CASE("DICOMweb builds a bounded server-side study search") {
	dicomweb_study_search_t search = {};
	strcpy(search.study_instance_uid, "1.2.3");
	strcpy(search.patient_id, "patient 7");
	strcpy(search.study_date, "20260101-20261231");
	strcpy(search.description, "H&E");
	char path[512];
	REQUIRE(dicomweb_build_study_query_path(path, sizeof(path), "/dicom-web", &search));
	CHECK(strcmp(path,
	             "/dicom-web/studies?0020000D=1.2.3&00100020=patient%207"
	             "&00080020=20260101-20261231&00081030=%2AH%26E%2A") == 0);
}

TEST_CASE("DICOMweb remote image destruction releases dynamic metadata") {
	dicomweb_remote_image_t remote = {};
	remote.instances = (dicomweb_instance_metadata_t*)calloc(2, sizeof(*remote.instances));
	remote.instance_count = 2;
	remote.instance_capacity = 2;
	remote.instances[0].frame_numbers = (u32*)calloc(3, sizeof(u32));
	remote.instances[0].frame_map_count = 3;
	remote.available_series = (dicomweb_series_ref_t*)calloc(4, sizeof(*remote.available_series));
	remote.available_series_count = 4;
	remote.available_series_capacity = 4;
	dicomweb_remote_image_destroy(&remote);
	CHECK(remote.instances == nullptr);
	CHECK(remote.instance_count == 0);
	CHECK(remote.instance_capacity == 0);
	CHECK(remote.available_series == nullptr);
	CHECK(remote.available_series_count == 0);
	CHECK(remote.available_series_capacity == 0);
}

TEST_CASE("DICOMweb parses QIDO instance references") {
	const char* json =
		"[{"
		"\"00080008\":{\"vr\":\"CS\",\"Value\":[\"ORIGINAL\",\"PRIMARY\",\"VOLUME\"]},"
		"\"00080016\":{\"vr\":\"UI\",\"Value\":[\"1.2.840.10008.5.1.4.1.1.77.1.6\"]},"
		"\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"instance-1\"]},"
		"\"00200013\":{\"vr\":\"IS\",\"Value\":[7]},"
		"\"00280008\":{\"vr\":\"IS\",\"Value\":[108]},"
		"\"00480006\":{\"vr\":\"UL\",\"Value\":[2875]},"
		"\"00480007\":{\"vr\":\"UL\",\"Value\":[2057]}"
		"},{"
		"\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"instance-2\"]}"
		"}]";
	dicomweb_instance_ref_t refs[2] = {};
	REQUIRE(dicomweb_parse_instance_refs(json, strlen(json), refs, COUNT(refs)) == 2);
	CHECK(strcmp(refs[0].study_instance_uid, "study") == 0);
	CHECK(strcmp(refs[0].series_instance_uid, "series") == 0);
	CHECK(strcmp(refs[0].image_flavor, "VOLUME") == 0);
	CHECK(strcmp(refs[0].instance_number, "7") == 0);
	CHECK(strcmp(refs[0].number_of_frames, "108") == 0);
	CHECK(strcmp(refs[0].total_pixel_matrix_columns, "2875") == 0);
	CHECK(strcmp(refs[1].sop_instance_uid, "instance-2") == 0);
}

TEST_CASE("DICOMweb accepts scoped QIDO instances without repeated parent UIDs") {
	const char* json =
		"[{"
		"\"00080016\":{\"vr\":\"UI\",\"Value\":[\"1.2.840.10008.5.1.4.1.1.77.1.6\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"1.2.3.4\"]},"
		"\"00280008\":{\"vr\":\"IS\",\"Value\":[437]}"
		"}]";
	dicomweb_instance_ref_t ref = {};
	REQUIRE(dicomweb_parse_instance_refs(json, strlen(json), &ref, 1) == 1);
	CHECK(strcmp(ref.sop_instance_uid, "1.2.3.4") == 0);
	CHECK(ref.study_instance_uid[0] == '\0');
	CHECK(ref.series_instance_uid[0] == '\0');
	CHECK(strcmp(ref.number_of_frames, "437") == 0);
	CHECK(dicomweb_count_instance_refs(json, strlen(json)) == 1);
}

TEST_CASE("DICOMweb parses captured IDC scoped QIDO fixture") {
	std::string json = read_dicomweb_fixture("idc_scoped_instances.json");
	REQUIRE_FALSE(json.empty());
	CHECK(dicomweb_count_instance_refs(json.data(), json.size()) == 6);
	dicomweb_instance_ref_t refs[6] = {};
	REQUIRE(dicomweb_parse_instance_refs(json.data(), json.size(), refs, COUNT(refs)) == 6);
	CHECK(refs[0].study_instance_uid[0] == '\0');
	CHECK(refs[0].series_instance_uid[0] == '\0');
	CHECK(strcmp(refs[0].sop_instance_uid,
	             "1.3.6.1.4.1.5962.99.1.208792987.352384958.1640886332827.21.0") == 0);
	CHECK(strcmp(refs[0].number_of_frames, "437") == 0);
}

TEST_CASE("DICOMweb parses captured IDC volume metadata fixture") {
	std::string json = read_dicomweb_fixture("idc_volume_metadata.json");
	REQUIRE_FALSE(json.empty());
	dicomweb_instance_metadata_t metadata = {};
	REQUIRE(dicomweb_parse_instance_metadata(json.data(), json.size(), &metadata));
	CHECK(strcmp(metadata.image_flavor, "VOLUME") == 0);
	CHECK(metadata.is_tiled_full);
	CHECK(metadata.total_pixel_matrix_columns == 4481);
	CHECK(metadata.total_pixel_matrix_rows == 5345);
	CHECK(metadata.number_of_frames == 437);
	CHECK(metadata.is_mpp_known);
	dicomweb_instance_metadata_destroy(&metadata);
}

TEST_CASE("DICOMweb maps column-major frames to raster tiles") {
	const char* json =
		"[{"
		"\"00080008\":{\"vr\":\"CS\",\"Value\":[\"ORIGINAL\",\"PRIMARY\",\"VOLUME\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"instance\"]},"
		"\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series\"]},"
		"\"00280008\":{\"vr\":\"IS\",\"Value\":[4]},"
		"\"00280010\":{\"vr\":\"US\",\"Value\":[256]},"
		"\"00280011\":{\"vr\":\"US\",\"Value\":[256]},"
		"\"00480006\":{\"vr\":\"UL\",\"Value\":[512]},"
		"\"00480007\":{\"vr\":\"UL\",\"Value\":[512]},"
		"\"52009230\":{\"vr\":\"SQ\",\"Value\":["
		"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{\"0048021E\":{\"vr\":\"SL\",\"Value\":[1]},\"0048021F\":{\"vr\":\"SL\",\"Value\":[1]}}]}},"
		"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{\"0048021E\":{\"vr\":\"SL\",\"Value\":[1]},\"0048021F\":{\"vr\":\"SL\",\"Value\":[257]}}]}},"
		"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{\"0048021E\":{\"vr\":\"SL\",\"Value\":[257]},\"0048021F\":{\"vr\":\"SL\",\"Value\":[1]}}]}},"
		"{\"0048021A\":{\"vr\":\"SQ\",\"Value\":[{\"0048021E\":{\"vr\":\"SL\",\"Value\":[257]},\"0048021F\":{\"vr\":\"SL\",\"Value\":[257]}}]}}"
		"]}"
		"}]";
	dicomweb_instance_metadata_t metadata = {};
	REQUIRE(dicomweb_parse_instance_metadata(json, strlen(json), &metadata));
	REQUIRE(metadata.frame_map_count == 4);
	CHECK(metadata.frame_numbers[0] == 1);
	CHECK(metadata.frame_numbers[1] == 3);
	CHECK(metadata.frame_numbers[2] == 2);
	CHECK(metadata.frame_numbers[3] == 4);
	dicomweb_instance_metadata_destroy(&metadata);
}

TEST_CASE("DICOMweb parses archive study and series results") {
	const char* studies_json =
		"[{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study-uid\"]},"
		"\"00080020\":{\"vr\":\"DA\",\"Value\":[\"20260705\"]},"
		"\"00081030\":{\"vr\":\"LO\",\"Value\":[\"Pathology\"]}}]";
	dicomweb_study_ref_t studies[1] = {};
	REQUIRE(dicomweb_parse_study_refs(studies_json, strlen(studies_json), studies, 1) == 1);
	CHECK(strcmp(studies[0].study_instance_uid, "study-uid") == 0);
	CHECK(strcmp(studies[0].study_date, "20260705") == 0);
	CHECK(strcmp(studies[0].description, "Pathology") == 0);

	const char* series_json =
		"[{\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series-uid\"]},"
		"\"00200011\":{\"vr\":\"IS\",\"Value\":[3]},"
		"\"00080060\":{\"vr\":\"CS\",\"Value\":[\"SM\"]},"
		"\"0008103E\":{\"vr\":\"LO\",\"Value\":[\"H&E\"]}}]";
	dicomweb_series_ref_t series[1] = {};
	REQUIRE(dicomweb_parse_series_refs(series_json, strlen(series_json), series, 1) == 1);
	CHECK(strcmp(series[0].series_instance_uid, "series-uid") == 0);
	CHECK(strcmp(series[0].series_number, "3") == 0);
	CHECK(strcmp(series[0].modality, "SM") == 0);
	CHECK(strcmp(series[0].description, "H&E") == 0);
}

TEST_CASE("DICOMweb counts query results independently of caller capacity") {
	const char* studies_json =
		"["
		"{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study-1\"]}},"
		"{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study-2\"]}},"
		"{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study-3\"]}}"
		"]";
	CHECK(dicomweb_count_study_refs(studies_json, strlen(studies_json)) == 3);
	dicomweb_study_ref_t study = {};
	CHECK(dicomweb_parse_study_refs(studies_json, strlen(studies_json), &study, 1) == 1);
	CHECK(strcmp(study.study_instance_uid, "study-1") == 0);

	const char* instances_json =
		"["
		"{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"instance-1\"]}},"
		"{\"0020000D\":{\"vr\":\"UI\",\"Value\":[\"study\"]},"
		"\"0020000E\":{\"vr\":\"UI\",\"Value\":[\"series\"]},"
		"\"00080018\":{\"vr\":\"UI\",\"Value\":[\"instance-2\"]}}"
		"]";
	CHECK(dicomweb_count_instance_refs(instances_json, strlen(instances_json)) == 2);
}
