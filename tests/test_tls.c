/* SPDX-License-Identifier: GPL-3.0-or-later
 * Offline integration tests for the actual vendored TLS/PSA configuration.
 */
#include "tls_platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct tls_pipe_t {
	unsigned char bytes[65536];
	size_t size;
} tls_pipe_t;

typedef struct tls_io_t {
	tls_pipe_t* incoming;
	tls_pipe_t* outgoing;
} tls_io_t;

typedef struct tls_pair_t {
	mbedtls_ssl_context client, server;
	mbedtls_ssl_config client_config, server_config;
	mbedtls_x509_crt ca, certificate;
	mbedtls_pk_context key;
	tls_pipe_t to_client, to_server;
	tls_io_t client_io, server_io;
} tls_pair_t;

static int pipe_send(void* context, const unsigned char* data, size_t size) {
	tls_pipe_t* pipe = ((tls_io_t*)context)->outgoing;
	if (size > sizeof(pipe->bytes) - pipe->size) return MBEDTLS_ERR_SSL_WANT_WRITE;
	memcpy(pipe->bytes + pipe->size, data, size);
	pipe->size += size;
	return (int)size;
}

static int pipe_recv(void* context, unsigned char* data, size_t size) {
	tls_pipe_t* pipe = ((tls_io_t*)context)->incoming;
	if (!pipe->size) return MBEDTLS_ERR_SSL_WANT_READ;
	if (size > pipe->size) size = pipe->size;
	memcpy(data, pipe->bytes, size);
	pipe->size -= size;
	memmove(pipe->bytes, pipe->bytes + size, pipe->size);
	return (int)size;
}

static int handshake_pending(int result) {
	return result == 0 || result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
}

/* Return zero only when the handshake/data exchange or requested rejection succeeds. */
static int exercise_tls(mbedtls_ssl_protocol_version version, int cipher, const char* certificate,
                        int trust_ca, const char* hostname, uint32_t expected_flags) {
	if (slidescape_crypto_init() != PSA_SUCCESS) return 1;
	tls_pair_t* pair = calloc(1, sizeof(*pair));
	if (!pair) return 1;
	int failed = 1, ret = 0;
	char path[1024];
	const int suites[] = {cipher, 0};
	const unsigned char message[] = "GET /dicomweb/studies HTTP/1.1\r\n\r\n";
	unsigned char received[sizeof(message)];
	mbedtls_ssl_init(&pair->client);
	mbedtls_ssl_init(&pair->server);
	mbedtls_ssl_config_init(&pair->client_config);
	mbedtls_ssl_config_init(&pair->server_config);
	mbedtls_x509_crt_init(&pair->ca);
	mbedtls_x509_crt_init(&pair->certificate);
	mbedtls_pk_init(&pair->key);
#define TLS_CHECK(call) do { ret = (call); if (ret != 0) goto cleanup; } while (0)
	TLS_CHECK(mbedtls_x509_crt_parse_file(&pair->ca, SLIDESCAPE_TLS_FIXTURE_DIR "/ca.pem"));
	snprintf(path, sizeof(path), "%s/%s.pem", SLIDESCAPE_TLS_FIXTURE_DIR, certificate);
	TLS_CHECK(mbedtls_x509_crt_parse_file(&pair->certificate, path));
	snprintf(path, sizeof(path), "%s/%s.key", SLIDESCAPE_TLS_FIXTURE_DIR, certificate);
	TLS_CHECK(mbedtls_pk_parse_keyfile(&pair->key, path, NULL));
	TLS_CHECK(mbedtls_ssl_config_defaults(&pair->client_config, MBEDTLS_SSL_IS_CLIENT,
	                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
	TLS_CHECK(mbedtls_ssl_config_defaults(&pair->server_config, MBEDTLS_SSL_IS_SERVER,
	                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));
	mbedtls_ssl_conf_min_tls_version(&pair->client_config, version);
	mbedtls_ssl_conf_max_tls_version(&pair->client_config, version);
	mbedtls_ssl_conf_min_tls_version(&pair->server_config, version);
	mbedtls_ssl_conf_max_tls_version(&pair->server_config, version);
	mbedtls_ssl_conf_ciphersuites(&pair->client_config, suites);
	mbedtls_ssl_conf_ciphersuites(&pair->server_config, suites);
	mbedtls_ssl_conf_authmode(&pair->client_config, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_authmode(&pair->server_config, MBEDTLS_SSL_VERIFY_NONE);
	/* An empty trust chain must reject the peer, rather than skip verification. */
	mbedtls_x509_crt empty_ca;
	mbedtls_x509_crt_init(&empty_ca);
	mbedtls_ssl_conf_ca_chain(&pair->client_config, trust_ca ? &pair->ca : &empty_ca, NULL);
	TLS_CHECK(mbedtls_ssl_conf_own_cert(&pair->server_config, &pair->certificate, &pair->key));
	TLS_CHECK(mbedtls_ssl_setup(&pair->client, &pair->client_config));
	TLS_CHECK(mbedtls_ssl_setup(&pair->server, &pair->server_config));
	TLS_CHECK(mbedtls_ssl_set_hostname(&pair->client, hostname));
	pair->client_io = (tls_io_t){&pair->to_client, &pair->to_server};
	pair->server_io = (tls_io_t){&pair->to_server, &pair->to_client};
	mbedtls_ssl_set_bio(&pair->client, &pair->client_io, pipe_send, pipe_recv, NULL);
	mbedtls_ssl_set_bio(&pair->server, &pair->server_io, pipe_send, pipe_recv, NULL);
	for (int step = 0; step < 10000; ++step) {
		if (!mbedtls_ssl_is_handshake_over(&pair->client)) {
			ret = mbedtls_ssl_handshake_step(&pair->client);
			if (!handshake_pending(ret)) {
				uint32_t flags = mbedtls_ssl_get_verify_result(&pair->client);
				if (expected_flags && ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED &&
				    (flags & expected_flags) == expected_flags) failed = 0;
				goto cleanup;
			}
		}
		if (!mbedtls_ssl_is_handshake_over(&pair->server)) {
			ret = mbedtls_ssl_handshake_step(&pair->server);
			if (!handshake_pending(ret)) goto cleanup;
		}
		if (mbedtls_ssl_is_handshake_over(&pair->client) && mbedtls_ssl_is_handshake_over(&pair->server)) {
			if (expected_flags || mbedtls_ssl_get_verify_result(&pair->client) != 0) goto cleanup;
			if (mbedtls_ssl_get_version_number(&pair->client) != version) goto cleanup;
			ret = mbedtls_ssl_write(&pair->client, message, sizeof(message));
			if (ret != sizeof(message)) goto cleanup;
			ret = mbedtls_ssl_read(&pair->server, received, sizeof(received));
			if (ret != sizeof(message) || memcmp(message, received, sizeof(message))) goto cleanup;
			ret = mbedtls_ssl_write(&pair->server, message, sizeof(message));
			if (ret != sizeof(message)) goto cleanup;
			ret = mbedtls_ssl_read(&pair->client, received, sizeof(received));
			if (ret != sizeof(message) || memcmp(message, received, sizeof(message))) goto cleanup;
			failed = 0;
			break;
		}
	}
cleanup:
	if (failed) {
		char error[200];
		mbedtls_strerror(ret, error, sizeof(error));
		fprintf(stderr, "TLS test failed: version=%x cipher=%x cert=%s host=%s trust=%d: %d %s\n",
		        version, cipher, certificate, hostname, trust_ca, ret, error);
	}
	mbedtls_ssl_free(&pair->client);
	mbedtls_ssl_free(&pair->server);
	mbedtls_ssl_config_free(&pair->client_config);
	mbedtls_ssl_config_free(&pair->server_config);
	mbedtls_x509_crt_free(&pair->ca);
	mbedtls_x509_crt_free(&pair->certificate);
	mbedtls_pk_free(&pair->key);
	free(pair);
	return failed;
#undef TLS_CHECK
}

#if defined(_WIN32)
static DWORD WINAPI concurrent_tls(void* arg) {
#else
static void* concurrent_tls(void* arg) {
#endif
	int* failed = arg;
	for (int i = 0; i < 8; ++i)
		*failed += exercise_tls(MBEDTLS_SSL_VERSION_TLS1_3, MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
		                        "rsa", 1, "localhost", 0);
	return 0;
}

int main(void) {
	/* First initialization occurs concurrently, before any other PSA call. */
	int failures[4] = {0}, failed = 0;
#if defined(_WIN32)
	HANDLE threads[4];
	for (int i = 0; i < 4; ++i) {
		threads[i] = CreateThread(NULL, 0, concurrent_tls, &failures[i], 0, NULL);
		if (!threads[i]) return 1;
	}
	for (int i = 0; i < 4; ++i) {
		if (WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0) return 1;
		CloseHandle(threads[i]);
	}
#else
	pthread_t threads[4];
	for (int i = 0; i < 4; ++i)
		if (pthread_create(&threads[i], NULL, concurrent_tls, &failures[i]) != 0) return 1;
	for (int i = 0; i < 4; ++i)
		if (pthread_join(threads[i], NULL) != 0) return 1;
#endif
	for (int i = 0; i < 4; ++i) failed += failures[i];
	const int tls12_rsa[] = {MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	                        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	                        MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256};
	const int tls12_ec[] = {MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	                       MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	                       MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256};
	const int tls13[] = {MBEDTLS_TLS1_3_AES_128_GCM_SHA256, MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
	                    MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256};
	for (int i = 0; i < 3; ++i) {
		failed += exercise_tls(MBEDTLS_SSL_VERSION_TLS1_2, tls12_rsa[i], "rsa", 1, "localhost", 0);
		failed += exercise_tls(MBEDTLS_SSL_VERSION_TLS1_2, tls12_ec[i], "ecdsa", 1, "localhost", 0);
		failed += exercise_tls(MBEDTLS_SSL_VERSION_TLS1_3, tls13[i], "rsa", 1, "localhost", 0);
		failed += exercise_tls(MBEDTLS_SSL_VERSION_TLS1_3, tls13[i], "ecdsa", 1, "localhost", 0);
	}
	for (int i = 0; i < 2; ++i) {
		mbedtls_ssl_protocol_version version = i ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2;
		int cipher = i ? tls13[0] : tls12_rsa[0];
		failed += exercise_tls(version, cipher, "rsa", 0, "localhost", MBEDTLS_X509_BADCERT_NOT_TRUSTED);
		failed += exercise_tls(version, cipher, "rsa", 1, "wrong.example", MBEDTLS_X509_BADCERT_CN_MISMATCH);
		failed += exercise_tls(version, cipher, "expired", 1, "localhost", MBEDTLS_X509_BADCERT_EXPIRED);
		failed += exercise_tls(version, cipher, "sha1", 1, "localhost", MBEDTLS_X509_BADCERT_BAD_MD);
	}
	if (!failed) puts("TLS: 32 concurrent handshakes, 12 cipher/certificate cases and 8 certificate rejections passed.");
	return failed ? 1 : 0;
}
