#include "doctest.h"

#include "image.h"
#include "ssi.h"
#include "tile_cache.h"
#include "tile_loader.h"

TEST_CASE("SSI tile-cache policy keeps a bounded rectangle pipeline") {
	image_t image = {};
	image.backend = IMAGE_BACKEND_SSI;
	image.tile_cache = tile_cache_create(&image);
	REQUIRE(image.tile_cache != nullptr);
	tile_cache_policy_t policy = tile_cache_get_policy(&image);
	CHECK(policy.batch_size == TILE_LOAD_BATCH_MAX);
	CHECK(policy.max_inflight_tiles == 12 * TILE_LOAD_BATCH_MAX);
	CHECK(policy.max_submit_per_tick == 12 * TILE_LOAD_BATCH_MAX);
	CHECK(policy.latency_bound);
	tile_cache_destroy(image.tile_cache);
	image.tile_cache = nullptr;
}

TEST_CASE("SSI parses its API endpoint without permitting cleartext remote credentials") {
	ssi_endpoint_t endpoint = {};
	REQUIRE(ssi_parse_endpoint("https://slidecenter.example.test:8443/SSI/", &endpoint));
	CHECK(strcmp(endpoint.hostname, "slidecenter.example.test") == 0);
	CHECK(strcmp(endpoint.host_header, "slidecenter.example.test:8443") == 0);
	CHECK(strcmp(endpoint.root, "/SSI") == 0);
	CHECK(endpoint.port == 8443);
	CHECK(endpoint.use_tls);
	CHECK_FALSE(ssi_parse_endpoint("http://slidecenter.example.test/SSI", &endpoint));
	CHECK(ssi_parse_endpoint("http://127.0.0.1:5123/SSI", &endpoint));
}

TEST_CASE("SSI parses base properties and maps normalized viewer tiles") {
	const char* json = R"({
		"OriginalName":"Melanoma_0032",
		"FolderId":"7f6bb88e5e8b402a83e59c576a078be3",
		"Width":4864,
		"Height":2816,
		"MicronPerPixelWidth":0.11693787,
		"MicronPerPixelHeight":0.11716536,
		"SlideIlluminationType":"BRIGHTFIELD",
		"MagnificationLevelMax":9,
		"BitPerChannel":8,
		"ChannelCount":3,
		"FocusLevelMin":-3,
		"FocusLevelMax":3,
		"TileXMinNative":5,
		"TileXMaxNative":17,
		"TileYMinNative":3,
		"TileYMaxNative":9
	})";
	ssi_slide_properties_t properties = {};
	REQUIRE(ssi_parse_slide_properties(json, strlen(json), &properties));
	CHECK(strcmp(properties.original_name, "Melanoma_0032") == 0);
	CHECK(strcmp(properties.folder_id, "7f6bb88e5e8b402a83e59c576a078be3") == 0);
	CHECK(properties.width == 4864);
	CHECK(properties.height == 2816);
	CHECK(properties.magnification_level_max == 9);
	CHECK(properties.is_brightfield);

	ssi_level_geometry_t geometry = {};
	REQUIRE(ssi_get_level_geometry(&properties, 0, &geometry));
	CHECK(geometry.tile_x_min == 5);
	CHECK(geometry.tile_x_max == 17);
	CHECK(geometry.width_in_tiles == 19);
	CHECK(geometry.height_in_tiles == 11);
	REQUIRE(ssi_get_level_geometry(&properties, 9, &geometry));
	CHECK(geometry.tile_x_min == 0);
	CHECK(geometry.tile_x_max == 0);
	CHECK(geometry.width_in_tiles == 1);

	ssi_slide_properties_t exported_properties = properties;
	exported_properties.width = 143360;
	exported_properties.height = 311040;
	exported_properties.tile_x_min_native = 293;
	exported_properties.tile_x_max_native = 490;
	exported_properties.tile_y_min_native = 612;
	exported_properties.tile_y_max_native = 1161;
	REQUIRE(ssi_get_level_geometry(&exported_properties, 9, &geometry));
	CHECK(geometry.width_in_tiles == 2);
	CHECK(geometry.height_in_tiles == 3);
	CHECK(geometry.tile_x_min == 0);
	CHECK(geometry.tile_x_max == 0);
	CHECK(geometry.tile_y_min == 1);
	CHECK(geometry.tile_y_max == 2);

	ssi_remote_image_t remote = {};
	REQUIRE(ssi_parse_endpoint("https://slidecenter.example.test/SSI", &remote.endpoint));
	remote.properties = properties;
	strcpy(remote.slide_token, "32fd90e7-9c1f-4685-9033-f8de2a383d6e");
	image_t image = {};
	REQUIRE(init_image_from_ssi(&image, &remote, false));
	CHECK(image.origin_offset.x == 0.0f);
	CHECK(image.origin_offset.y == 0.0f);
	CHECK(image.level_images[0].origin_offset.x == 0.0f);
	CHECK(image.level_images[0].width_in_tiles == 19);
	CHECK(image.level_images[0].tiles[0].is_empty);
	CHECK_FALSE(image.level_images[0].tiles[3 * 19 + 5].is_empty);
	CHECK(image.level_images[0].tiles[3 * 19 + 18].is_empty);
	for (i32 level = 0; level < image.level_count; ++level) {
		free(image.level_images[level].tiles);
	}
	char path[1024];
	REQUIRE(ssi_build_tile_path(path, sizeof(path), &remote, 0, 7, 7));
	CHECK(strstr(path, "x1=7&y1=7&x2=7&y2=7") != nullptr);
	REQUIRE(ssi_build_tile_rectangle_path(path, sizeof(path), &remote, 0, 7, 5, 10, 6));
	CHECK(strstr(path, "x1=7&y1=5&x2=10&y2=6") != nullptr);
	CHECK_FALSE(ssi_build_tile_rectangle_path(path, sizeof(path), &remote, 0, 7, 5, 20, 6));
	CHECK(strstr(path, "magnification=0") != nullptr);
	CHECK_FALSE(ssi_build_tile_path(path, sizeof(path), &remote, 0, 0, 0));
	REQUIRE(ssi_build_tile_path(path, sizeof(path), &remote, 1, 4, 4));
	CHECK(strstr(path, "x1=4&y1=4&x2=4&y2=4") != nullptr);
	CHECK(strstr(path, "magnification=1") != nullptr);
}

TEST_CASE("SSI accepts secure 3DHISTECH mview launcher links") {
	const char* uri = "mview:OfflTelSlide?HUBIP=slidecenter.example.test&"
	                  "HUBSecure=true&SldID=fe2a3b0ec0f18b9b3981094ba2e3c2df&"
	                  "AccID=308163XY172C8CCLLHG7&page=1&Capt=T25-231";
	ssi_uri_config_t config = {};
	CHECK(ssi_uri_is_supported(uri));
	REQUIRE(ssi_parse_mview_uri(uri, &config));
	CHECK(strcmp(config.api_url,
	             "https://slidecenter.example.test/SSI") == 0);
	CHECK(strcmp(config.slide_center_url,
	             "https://slidecenter.example.test") == 0);
	CHECK(strcmp(config.slide_guid, "fe2a3b0ec0f18b9b3981094ba2e3c2df") == 0);
	CHECK(strcmp(config.session_id, "308163XY172C8CCLLHG7") == 0);
	CHECK(strcmp(config.caption, "T25-231") == 0);
	CHECK_FALSE(ssi_parse_mview_uri(
		"mview:OfflTelSlide?HUBIP=example.test&HUBSecure=true&"
		"SldID=fe2a3b0ec0f18b9b3981094ba2e3c2df&AccID=bad%2Fsession", &config));
	CHECK_FALSE(ssi_parse_mview_uri(
		"mview:OfflTelSlide?HUBIP=example.test&HUBSecure=false&"
		"SldID=fe2a3b0ec0f18b9b3981094ba2e3c2df", &config));
}

TEST_CASE("SSI parses the SlideCenter display name from slide attributes") {
	const char* json = R"({
		"Id":"fe2a3b0ec0f18b9b3981094ba2e3c2df",
		"Name":"T25-231",
		"BarCode":"not logged by the parser"
	})";
	char name[64];
	REQUIRE(ssi_parse_slide_attribute_name(json, strlen(json), name, sizeof(name)));
	CHECK(strcmp(name, "T25-231") == 0);
	CHECK_FALSE(ssi_parse_slide_attribute_name("[]", 2, name, sizeof(name)));
}

TEST_CASE("SSI parses bounded immediate folder slides") {
	const char* json = R"({
		"ParentFolder":{"Id":"11111111111111111111111111111111","Name":"Parent"},
		"CurrentFolder":{"Id":"7f6bb88e5e8b402a83e59c576a078be3","Name":"Current"},
		"Folders":[],
		"Slides":[
			{"Id":"fe2a3b0ec0f18b9b3981094ba2e3c2df","Name":"T25-231"},
			{"Id":"08908d29693e4a99a341a1fbcc9d7596","Name":""},
			{"Id":"not-a-slide-id","Name":"Ignored"}
		]
	})";
	ssi_slide_ref_t* slides = NULL;
	i32 slide_count = 0;
	REQUIRE(ssi_parse_folder_content(json, strlen(json), &slides, &slide_count));
	REQUIRE(slide_count == 2);
	CHECK(strcmp(slides[0].id, "fe2a3b0ec0f18b9b3981094ba2e3c2df") == 0);
	CHECK(strcmp(slides[0].name, "T25-231") == 0);
	CHECK(strcmp(slides[1].name, slides[1].id) == 0);
	free(slides);

	slides = NULL;
	slide_count = -1;
	REQUIRE(ssi_parse_folder_content("{\"Slides\":null}",
	                                 strlen("{\"Slides\":null}"),
	                                 &slides, &slide_count));
	CHECK(slides == NULL);
	CHECK(slide_count == 0);
}

TEST_CASE("SSI direct URI contains only a slide identifier") {
	char guid[64];
	CHECK(ssi_uri_is_supported("ssi:fe2a3b0ec0f18b9b3981094ba2e3c2df"));
	REQUIRE(ssi_parse_uri_slide_guid("ssi:fe2a3b0ec0f18b9b3981094ba2e3c2df",
	                                 guid, sizeof(guid)));
	CHECK(strcmp(guid, "fe2a3b0ec0f18b9b3981094ba2e3c2df") == 0);
	CHECK_FALSE(ssi_parse_uri_slide_guid("ssi:not-a-guid", guid, sizeof(guid)));
}

TEST_CASE("SSI parses opaque handles without exposing them to request builders") {
	char handle[128];
	REQUIRE(ssi_parse_json_string("\"7QCH8N8WHI6Y6SVH2MAP\"",
	                              strlen("\"7QCH8N8WHI6Y6SVH2MAP\""),
	                              handle, sizeof(handle)));
	CHECK(strcmp(handle, "7QCH8N8WHI6Y6SVH2MAP") == 0);
	CHECK_FALSE(ssi_parse_json_string("{}", 2, handle, sizeof(handle)));
}
