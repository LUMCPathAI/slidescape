#include "doctest.h"

#include "remote_url.h"

#include <cstring>

TEST_CASE("remote URL parser handles HTTPS hosts, ports, queries, and IPv6 literals") {
	remote_url_t parsed = {};
	REQUIRE(remote_url_parse(&parsed, "https://example.test/slides/1?frame=2#ignored"));
	CHECK(strcmp(parsed.protocol, "https") == 0);
	CHECK(strcmp(parsed.site, "example.test") == 0);
	CHECK(strcmp(parsed.host_header, "example.test") == 0);
	CHECK(strcmp(parsed.port, "443") == 0);
	CHECK(strcmp(parsed.path, "/slides/1?frame=2") == 0);

	REQUIRE(remote_url_parse(&parsed, "HTTPS://example.test:8443?study=4"));
	CHECK(strcmp(parsed.site, "example.test") == 0);
	CHECK(strcmp(parsed.host_header, "example.test:8443") == 0);
	CHECK(strcmp(parsed.port, "8443") == 0);
	CHECK(strcmp(parsed.path, "/?study=4") == 0);

	REQUIRE(remote_url_parse(&parsed, "https://[2001:db8::1]:443/path"));
	CHECK(strcmp(parsed.site, "2001:db8::1") == 0);
	CHECK(strcmp(parsed.host_header, "[2001:db8::1]:443") == 0);
	CHECK(strcmp(parsed.path, "/path") == 0);
}

TEST_CASE("remote URL parser rejects malformed or unsafe authorities") {
	remote_url_t parsed = {};
	CHECK_FALSE(remote_url_parse(&parsed, "example.test/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "ftp://example.test/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://user:secret@example.test/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://example.test:0/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://example.test:65536/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://2001:db8::1/path"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://example.test/path\r\nInjected: true"));
	CHECK_FALSE(remote_url_parse(&parsed, "https://example.test/path with spaces"));
}
