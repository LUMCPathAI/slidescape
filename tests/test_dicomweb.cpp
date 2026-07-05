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
