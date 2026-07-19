#include "doctest.h"

#include "dicomweb.h"

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
