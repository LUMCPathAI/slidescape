#include "doctest.h"

#include "http_response_parser.h"

TEST_CASE("HTTP response parser accepts split messages") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	const char* first = "HTTP/1.1 200 OK\r\nContent-Ty";
	const char* second = "pe: application/dicom+json\r\nContent-Length: 2\r\n\r\n[";
	const char* third = "]";
	CHECK(http_response_parser_execute(&parser, first, strlen(first)));
	CHECK(http_response_parser_execute(&parser, second, strlen(second)));
	CHECK(http_response_parser_execute(&parser, third, strlen(third)));
	http_response_parser_finish(&parser);
	http_response_t* response = http_response_parser_take_response(&parser);
	REQUIRE(response != nullptr);
	CHECK(response->status_code == 200);
	CHECK(response->content_length == 2);
	CHECK(strcmp(response->content_type, "application/dicom+json") == 0);
	CHECK(response->connection_reusable);
	CHECK(memcmp(response->buffer.data, "[]", 2) == 0);
	memrw_destroy(&response->buffer);
	free(response);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser exposes server connection-close semantics") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	const char* response =
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 2\r\n"
		"Connection: close\r\n\r\n"
		"[]";
	REQUIRE(http_response_parser_execute(&parser, response, strlen(response)));
	http_response_parser_finish(&parser);
	http_response_t* parsed = http_response_parser_take_response(&parser);
	REQUIRE(parsed != nullptr);
	CHECK_FALSE(parsed->connection_reusable);
	memrw_destroy(&parsed->buffer);
	free(parsed);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser rejects oversized bodies") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 3));
	const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ntest";
	CHECK_FALSE(http_response_parser_execute(&parser, response, strlen(response)));
	CHECK(parser.error == HTTP_RESPONSE_ERROR_BODY_TOO_LARGE);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser rejects oversized headers") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	std::string response = "HTTP/1.1 200 OK\r\nX-Large: ";
	response.append(KILOBYTES(65), 'x');
	response += "\r\nContent-Length: 0\r\n\r\n";
	CHECK_FALSE(http_response_parser_execute(&parser, response.data(), response.size()));
	CHECK(parser.error == HTTP_RESPONSE_ERROR_HEADER_TOO_LARGE);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser rejects truncated messages") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc";
	REQUIRE(http_response_parser_execute(&parser, response, strlen(response)));
	http_response_parser_finish(&parser);
	CHECK(parser.error == HTTP_RESPONSE_ERROR_TRUNCATED);
	CHECK(http_response_parser_take_response(&parser) == nullptr);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser accepts a connection-close delimited body") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	const char* response =
		"HTTP/1.1 200 OK\r\n"
		"Connection: close\r\n"
		"Content-Type: application/dicom+json\r\n\r\n"
		"[{\"00080005\":{\"vr\":\"CS\"}}]";
	REQUIRE(http_response_parser_execute(&parser, response, strlen(response)));
	CHECK_FALSE(parser.message_complete);
	http_response_parser_finish(&parser);
	http_response_t* parsed = http_response_parser_take_response(&parser);
	REQUIRE(parsed != nullptr);
	CHECK(parsed->status_code == 200);
	CHECK(parsed->content_length == strlen("[{\"00080005\":{\"vr\":\"CS\"}}]"));
	CHECK(strcmp(parsed->content_type, "application/dicom+json") == 0);
	CHECK_FALSE(parsed->connection_reusable);
	memrw_destroy(&parsed->buffer);
	free(parsed);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response parser accepts empty 204 responses") {
	http_response_parser_t parser = {};
	REQUIRE(http_response_parser_init(&parser, 1024));
	const char* response = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
	REQUIRE(http_response_parser_execute(&parser, response, strlen(response)));
	http_response_parser_finish(&parser);
	http_response_t* parsed = http_response_parser_take_response(&parser);
	REQUIRE(parsed != nullptr);
	CHECK(parsed->status_code == 204);
	CHECK(parsed->content_length == 0);
	memrw_destroy(&parsed->buffer);
	free(parsed);
	http_response_parser_destroy(&parser);
}

TEST_CASE("HTTP response errors distinguish request deadline phases") {
	CHECK(strcmp(http_response_error_string(HTTP_RESPONSE_ERROR_TOTAL_TIMEOUT),
	             "total request deadline exceeded") == 0);
	CHECK(strcmp(http_response_error_string(HTTP_RESPONSE_ERROR_WRITE_TIMEOUT),
	             "HTTP request write deadline exceeded") == 0);
	CHECK(strcmp(http_response_error_string(HTTP_RESPONSE_ERROR_IDLE_READ_TIMEOUT),
	             "HTTP response idle-read deadline exceeded") == 0);
}
