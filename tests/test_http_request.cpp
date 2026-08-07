#include "doctest.h"

#include "http_request.h"

TEST_CASE("HTTP request builder emits JSON POST bodies safely") {
	char request[1024];
	i32 length = remote_build_http_request_headers(
		request, sizeof(request), REMOTE_HTTP_POST, "ssi.example.test", "/SSI/session/create",
		"application/json", "application/json", 37, "X-Test: yes\r\n", true);
	REQUIRE(length > 0);
	CHECK(strstr(request, "POST /SSI/session/create HTTP/1.1\r\n") != nullptr);
	CHECK(strstr(request, "Content-Type: application/json\r\n") != nullptr);
	CHECK(strstr(request, "Content-Length: 37\r\n") != nullptr);
	CHECK(strstr(request, "Connection: close\r\n") != nullptr);
}

TEST_CASE("HTTP request builder rejects header injection") {
	char request[1024];
	CHECK(remote_build_http_request_headers(
		request, sizeof(request), REMOTE_HTTP_GET, "ssi.example.test", "/safe\r\nInjected: yes",
		"application/json", nullptr, 0, nullptr, true) < 0);
	CHECK(remote_build_http_request_headers(
		request, sizeof(request), REMOTE_HTTP_GET, "ssi.example.test", "/safe",
		"application/json", nullptr, 0, "X-Test: yes\r\n\r\nInjected: yes", true) < 0);
}

