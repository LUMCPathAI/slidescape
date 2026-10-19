/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include "platform.h"
#include "viewer.h"
#include "remote.h"
#include "remote_url.h"
#include "stringutils.h"

#ifdef __APPLE__
// For loading the root certificates (requires linking against "-framework Security")
#include <Security/Security.h>
#endif

#include "mbedtls/build_info.h"
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/error.h"
#include "tls_platform.h"

//#define SERVER_PORT "443"
//#define SERVER_PORT "4433"
//#define SERVER_NAME "google.com"
//#define SERVER_NAME "localhost"
//#define GET_REQUEST "GET / HTTP/1.0\r\n\r\n"

#include "http_parser.h"

struct tls_connection_t {
	i64 start_clock;
	i64 sockfd;
	bool use_tls;
	mbedtls_net_context server_fd;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cacert;
};

// Adapted from https://github.com/HaxeFoundation/hxcpp/blob/7bd5ff3/src/hx/libs/ssl/SSL.cpp#L455-L491
// discussed here: https://stackoverflow.com/questions/42432473/programmatically-read-root-ca-certificates-in-ios
// copyright notice below from hxcpp, see: https://github.com/HaxeFoundation/hxcpp
/*
* Copyright (c) 2008 by the contributors
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following condition is met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*
* See individual source files for additional license information.
*
* THIS SOFTWARE IS PROVIDED BY THE HAXE PROJECT CONTRIBUTORS ``AS IS'' AND ANY
* EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED.
*/

mbedtls_x509_crt* ssl_cert_load_defaults() {
#if WINDOWS
    HCERTSTORE store;
	PCCERT_CONTEXT cert = NULL;
	mbedtls_x509_crt *chain = NULL;
	store = CertOpenSystemStore(0, (LPCSTR) "Root");
	if (store) {
		cert = CertEnumCertificatesInStore(store, cert);
		while (cert){
			if( chain == NULL ){
				chain = calloc(1, sizeof(mbedtls_x509_crt));
			}
			mbedtls_x509_crt_parse_der(chain, (unsigned char *)cert->pbCertEncoded, cert->cbCertEncoded );
			cert = CertEnumCertificatesInStore(store, cert);
		}
		CertCloseStore(store, 0);
	}
	if( chain != NULL )
		return chain;
#elif APPLE
    CFMutableDictionaryRef search;
    CFArrayRef result;
    SecKeychainRef keychain;
    SecCertificateRef item;
    CFDataRef dat;
    mbedtls_x509_crt *chain = NULL;

    // Load keychain
    if( SecKeychainOpen("/System/Library/Keychains/SystemRootCertificates.keychain",&keychain) != errSecSuccess )
        return NULL;

    // Search for certificates
    search = CFDictionaryCreateMutable( NULL, 0, NULL, NULL );
    CFDictionarySetValue( search, kSecClass, kSecClassCertificate );
    CFDictionarySetValue( search, kSecMatchLimit, kSecMatchLimitAll );
    CFDictionarySetValue( search, kSecReturnRef, kCFBooleanTrue );
    CFDictionarySetValue( search, kSecMatchSearchList, CFArrayCreate(NULL, (const void **)&keychain, 1, NULL) );
    if( SecItemCopyMatching( search, (CFTypeRef *)&result ) == errSecSuccess ){
        CFIndex n = CFArrayGetCount( result );
        for( CFIndex i = 0; i < n; i++ ){
            item = (SecCertificateRef)CFArrayGetValueAtIndex( result, i );

            // Get certificate in DER format
            dat = SecCertificateCopyData( item );
            if( dat ){
                if( chain == NULL ){
                    chain = calloc(1, sizeof(mbedtls_x509_crt));
                }
                mbedtls_x509_crt_parse_der( chain, (unsigned char *)CFDataGetBytePtr(dat), CFDataGetLength(dat) );
                CFRelease( dat );
            }
        }
    }
    CFRelease(keychain);
    if( chain != NULL )
        return chain;
#elif LINUX
    static const char* ca_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
    };
    mbedtls_x509_crt* chain = calloc(1, sizeof(mbedtls_x509_crt));
    if (!chain) return NULL;
    mbedtls_x509_crt_init(chain);
    for (i32 i = 0; i < COUNT(ca_paths); ++i) {
        if (mbedtls_x509_crt_parse_file(chain, ca_paths[i]) >= 0) return chain;
    }
    mbedtls_x509_crt_free(chain);
    free(chain);
#endif
    return NULL;
}


tls_connection_t* open_remote_connection(const char* hostname, i32 portno, void* alloced_mem_for_struct) {
	/* Mbed TLS 4 uses PSA's process-wide RNG and key store. Initialize before
	 * touching library objects so Windows threading callbacks are installed. */
	psa_status_t crypto_ret = slidescape_crypto_init();
	if (crypto_ret != PSA_SUCCESS) {
		console_print_error("PSA crypto initialization failed: %d\n", (int)crypto_ret);
		return NULL;
	}

	int ret = 1, len;
	int exit_code = MBEDTLS_EXIT_FAILURE;
	uint32_t flags;
	unsigned char buf[1024];
	tls_connection_t* connection = (tls_connection_t*) alloced_mem_for_struct;
	connection->use_tls = true;
	connection->start_clock = get_clock();

	/*
	 * 0. Initialize the session data
	 */
	mbedtls_net_init( &connection->server_fd );
	mbedtls_ssl_init( &connection->ssl );
	mbedtls_ssl_config_init( &connection->conf );
	mbedtls_x509_crt_init( &connection->cacert );

	/*
	 * 0. Initialize certificates
	 */
	console_print_verbose( "  . Loading the CA root certificate ..." );

    mbedtls_x509_crt* chain = ssl_cert_load_defaults();
    if (chain) {
        memcpy(&connection->cacert, chain, sizeof(mbedtls_x509_crt));
        free(chain);
    } else {
        console_print_error(" failed\n  ! no trusted operating-system CA certificates could be loaded\n");
        ret = MBEDTLS_ERR_X509_FILE_IO_ERROR;
        goto exit;
    }

	console_print_verbose( " ok (%d skipped)\n", ret );

	/*
     * 1. Start the connection
     */
	console_print_verbose( "  . Connecting to tcp/%s/%d...", hostname, portno );

	char server_port_string[32];
	snprintf(server_port_string, sizeof(server_port_string), "%d", portno);

	if( ( ret = mbedtls_net_connect( &connection->server_fd, hostname,
	                                 server_port_string, MBEDTLS_NET_PROTO_TCP ) ) != 0 )
	{
		console_print_error( " failed\n  ! mbedtls_net_connect returned %d\n\n", ret );
		goto exit;
	}

	console_print_verbose( " ok\n" );

	/*
	 * 2. Setup stuff
	 */
	console_print_verbose( "  . Setting up the SSL/TLS structure..." );

	if( ( ret = mbedtls_ssl_config_defaults( &connection->conf,
	                                         MBEDTLS_SSL_IS_CLIENT,
	                                         MBEDTLS_SSL_TRANSPORT_STREAM,
	                                         MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
	{
		console_print_error( " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret );
		goto exit;
	}

	console_print_verbose( " ok\n" );

	/* Remote slide data and credentials must never cross an unverified TLS connection. */
	mbedtls_ssl_conf_authmode( &connection->conf, MBEDTLS_SSL_VERIFY_REQUIRED );
	mbedtls_ssl_conf_ca_chain( &connection->conf, &connection->cacert, NULL );
	mbedtls_ssl_conf_read_timeout( &connection->conf, 250 );

	if( ( ret = mbedtls_ssl_setup( &connection->ssl, &connection->conf ) ) != 0 )
	{
		console_print_error( " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret );
		goto exit;
	}

	if( ( ret = mbedtls_ssl_set_hostname( &connection->ssl, hostname ) ) != 0 )
	{
		console_print_error( " failed\n  ! mbedtls_ssl_set_hostname returned %d\n\n", ret );
		goto exit;
	}

	mbedtls_ssl_set_bio( &connection->ssl, &connection->server_fd, mbedtls_net_send,
	                     mbedtls_net_recv, mbedtls_net_recv_timeout );

	/*
	 * 4. Handshake
	 */
	console_print_verbose( "  . Performing the SSL/TLS handshake..." );

	while( ( ret = mbedtls_ssl_handshake( &connection->ssl ) ) != 0 )
	{
		if( ret == MBEDTLS_ERR_SSL_TIMEOUT &&
		    get_seconds_elapsed(connection->start_clock, get_clock()) <= 30.0f )
		{
			continue;
		}
		if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
		{
			console_print_error( " failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n", (unsigned int) -ret );
			goto exit;
		}
	}

	console_print_verbose( " ok\n" );

	/*
	 * 5. Verify the server certificate
	 */
	console_print_verbose( "  . Verifying peer X.509 certificate..." );

	/* In real life, we probably want to bail out when ret != 0 */
	if( ( flags = mbedtls_ssl_get_verify_result( &connection->ssl ) ) != 0 )
	{
		char vrfy_buf[512];

		console_print_verbose( " failed\n" );

		mbedtls_x509_crt_verify_info( vrfy_buf, sizeof( vrfy_buf ), "  ! ", flags );

		console_print_verbose( "%s\n", vrfy_buf );
	}
	else
		console_print_verbose( " ok\n" );


//	mbedtls_ssl_close_notify( &connection->ssl );

	exit_code = MBEDTLS_EXIT_SUCCESS;

	exit:

#ifdef MBEDTLS_ERROR_C
	if( exit_code != MBEDTLS_EXIT_SUCCESS )
	{
		char error_buf[100];
		mbedtls_strerror( ret, error_buf, 100 );
		console_print_error("Last error was: %d - %s\n\n", ret, error_buf );
		mbedtls_net_free( &connection->server_fd );

		mbedtls_x509_crt_free( &connection->cacert );
		mbedtls_ssl_free( &connection->ssl );
		mbedtls_ssl_config_free( &connection->conf );

		return NULL;

	}
#endif

	return connection;

}

float close_remote_connection(tls_connection_t* connection) {
	if (connection->use_tls) {
		mbedtls_ssl_close_notify(&connection->ssl);
	}

	mbedtls_net_free( &connection->server_fd );

	if (connection->use_tls) {
		mbedtls_x509_crt_free(&connection->cacert);
		mbedtls_ssl_free(&connection->ssl);
		mbedtls_ssl_config_free(&connection->conf);
	}

	float seconds_elapsed = get_seconds_elapsed(connection->start_clock, get_clock());
	return seconds_elapsed;
}

static tls_connection_t* remote_connection_open_plain(const char* hostname, i32 portno) {
    tls_connection_t* connection = (tls_connection_t*)calloc(1, sizeof(tls_connection_t));
    if (!connection) return NULL;
    connection->start_clock = get_clock();
    connection->use_tls = false;
    mbedtls_net_init(&connection->server_fd);
    char port[32];
    snprintf(port, sizeof(port), "%d", portno);
    if (mbedtls_net_connect(&connection->server_fd, hostname, port, MBEDTLS_NET_PROTO_TCP) != 0) {
        mbedtls_net_free(&connection->server_fd);
        free(connection);
        return NULL;
    }
    return connection;
}

tls_connection_t* remote_connection_open(const char* hostname, i32 portno) {
    tls_connection_t* connection = (tls_connection_t*)calloc(1, sizeof(tls_connection_t));
    if (!connection) return NULL;
    if (!open_remote_connection(hostname, portno, connection)) {
        free(connection);
        return NULL;
    }
    return connection;
}

tls_connection_t* remote_connection_open_http(const char* hostname, i32 portno, bool use_tls) {
	tls_connection_t* connection = use_tls ? remote_connection_open(hostname, portno)
	                                        : remote_connection_open_plain(hostname, portno);
	if (connection && mbedtls_net_set_nonblock(&connection->server_fd) != 0) {
		remote_connection_close_and_free(connection);
		return NULL;
	}
	return connection;
}

void remote_connection_close_and_free(tls_connection_t* connection) {
    if (!connection) return;
    close_remote_connection(connection);
    free(connection);
}

static bool remote_request_was_cancelled(const remote_request_options_t* options) {
	return options && options->cancellation_generation &&
	       *options->cancellation_generation != options->expected_generation;
}

static void remote_request_set_error(const remote_request_options_t* options,
                                     http_response_error_t error) {
	if (options && options->out_error) *options->out_error = error;
}

http_response_t* remote_connection_request_http(
	tls_connection_t* connection, const char* host, remote_http_method_t method,
	const char* path, const char* accept, const char* content_type,
	const void* body, size_t body_size, const char* extra_headers,
	bool close_after_request, const remote_request_options_t* options) {
    ASSERT(connection);
	remote_request_set_error(options, HTTP_RESPONSE_ERROR_NONE);
	if (body_size > 0 && !body) {
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_INVALID_REQUEST);
		return NULL;
	}

	u32 read_poll_timeout_ms = options && options->read_poll_timeout_ms
	                               ? options->read_poll_timeout_ms : 250;
	float write_timeout_seconds = options && options->write_timeout_seconds > 0.0f
	                              ? options->write_timeout_seconds : 10.0f;
	float idle_read_timeout_seconds = options && options->idle_read_timeout_seconds > 0.0f
	                                  ? options->idle_read_timeout_seconds : 10.0f;
	float total_timeout_seconds = options && options->total_timeout_seconds > 0.0f
	                                  ? options->total_timeout_seconds : 30.0f;
	i64 request_start_clock = get_clock();
	char request[4096];
	i32 request_len = remote_build_http_request_headers(request, sizeof(request), method, host, path,
	                                                   accept, content_type, body_size, extra_headers,
	                                                   close_after_request);
	if (request_len <= 0 || request_len >= sizeof(request)) {
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_INVALID_REQUEST);
        return NULL;
    }

	i32 ret = 1;
	size_t bytes_written = 0;
	size_t total_write_size = (size_t)request_len + body_size;
	i64 write_start_clock = get_clock();
	while (bytes_written < total_write_size) {
		if (remote_request_was_cancelled(options)) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_CANCELLED);
			return NULL;
		}
		if (get_seconds_elapsed(request_start_clock, get_clock()) > total_timeout_seconds) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_TOTAL_TIMEOUT);
			return NULL;
		}
		if (get_seconds_elapsed(write_start_clock, get_clock()) > write_timeout_seconds) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_WRITE_TIMEOUT);
			return NULL;
        }
		const u8* write_data = bytes_written < (size_t)request_len
		                       ? (const u8*)request + bytes_written
		                       : (const u8*)body + (bytes_written - (size_t)request_len);
		size_t write_size = bytes_written < (size_t)request_len
		                    ? (size_t)request_len - bytes_written
		                    : total_write_size - bytes_written;
		ret = connection->use_tls
		      ? mbedtls_ssl_write(&connection->ssl, write_data, write_size)
		      : mbedtls_net_send(&connection->server_fd, write_data, write_size);
        if (ret > 0) {
            bytes_written += ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            console_print_error("remote_connection_request(): network write returned %d\n", ret);
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_NETWORK_WRITE);
            return NULL;
        }
    }

	http_response_parser_t parser = {0};
	if (!http_response_parser_init(&parser, options ? options->max_response_body_size : 0)) {
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_PARSE);
		return NULL;
	}

	u8 read_buffer[KILOBYTES(16)];
	i64 last_read_clock = get_clock();
	while (!parser.message_complete) {
		if (remote_request_was_cancelled(options)) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_CANCELLED);
			http_response_parser_destroy(&parser);
			return NULL;
		}
		if (get_seconds_elapsed(request_start_clock, get_clock()) > total_timeout_seconds) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_TOTAL_TIMEOUT);
			http_response_parser_destroy(&parser);
			return NULL;
		}
		if (get_seconds_elapsed(last_read_clock, get_clock()) > idle_read_timeout_seconds) {
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_IDLE_READ_TIMEOUT);
			http_response_parser_destroy(&parser);
			return NULL;
        }
        ret = connection->use_tls
              ? mbedtls_ssl_read(&connection->ssl, read_buffer, sizeof(read_buffer))
              : mbedtls_net_recv_timeout(&connection->server_fd, read_buffer,
	                                    sizeof(read_buffer), read_poll_timeout_ms);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
            ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            break;
        }
		if (ret < 0) {
            console_print_error("remote_connection_request(): mbedtls_ssl_read returned %d\n", ret);
			remote_request_set_error(options, HTTP_RESPONSE_ERROR_NETWORK_READ);
			http_response_parser_destroy(&parser);
            return NULL;
		}
		last_read_clock = get_clock();

		if (!http_response_parser_execute(&parser, read_buffer, (size_t)ret)) {
			remote_request_set_error(options, parser.error);
			http_response_parser_destroy(&parser);
            return NULL;
        }
    }

	http_response_parser_finish(&parser);
	if (parser.error != HTTP_RESPONSE_ERROR_NONE) {
		remote_request_set_error(options, parser.error);
		http_response_parser_destroy(&parser);
		return NULL;
	}
	http_response_t* response = http_response_parser_take_response(&parser);
	http_response_parser_destroy(&parser);
	return response;
}

http_response_t* remote_connection_request_with_headers_and_options(
	tls_connection_t* connection, const char* host, const char* path, const char* accept,
	const char* extra_headers, bool close_after_request,
	const remote_request_options_t* options) {
	return remote_connection_request_http(connection, host, REMOTE_HTTP_GET, path, accept, NULL,
	                                      NULL, 0, extra_headers, close_after_request, options);
}

http_response_t* remote_connection_request_with_headers(tls_connection_t* connection, const char* host,
                                                        const char* path, const char* accept,
                                                        const char* extra_headers,
                                                        bool close_after_request) {
    return remote_connection_request_with_headers_and_options(
        connection, host, path, accept, extra_headers, close_after_request, NULL);
}

static bool remote_build_credential_headers(char* headers, size_t headers_size,
		const char* api_token, const char* cookie_header) {
	if (!headers || headers_size == 0 ||
	    (api_token && strpbrk(api_token, "\r\n")) ||
	    (cookie_header && strpbrk(cookie_header, "\r\n"))) {
		return false;
	}
	headers[0] = '\0';
	size_t used = 0;
	if (api_token && api_token[0]) {
		i32 written = snprintf(headers + used, headers_size - used,
		                       "Authorization: Bearer %s\r\n", api_token);
		if (written < 0 || (size_t)written >= headers_size - used) return false;
		used += (size_t)written;
	}
	if (cookie_header && cookie_header[0]) {
		i32 written = snprintf(headers + used, headers_size - used,
		                       "Cookie: %s\r\n", cookie_header);
		if (written < 0 || (size_t)written >= headers_size - used) return false;
	}
	return true;
}

http_response_t* remote_connection_request(tls_connection_t* connection, const char* host, const char* path, const char* api_token, const char* cookie_header, bool close_after_request) {
    char headers[6144] = "";
	if (!remote_build_credential_headers(headers, sizeof(headers), api_token, cookie_header)) return NULL;
    return remote_connection_request_with_headers(connection, host, path,
                                                  "application/json,image/jpeg,*/*",
                                                  headers, close_after_request);
}

http_response_t* open_remote_uri_with_extra_headers_and_options(
		const char* uri, const char* api_token, const char* cookie_header,
		const remote_request_options_t* options) {
	remote_request_set_error(options, HTTP_RESPONSE_ERROR_NONE);

    remote_url_t url_info = {};
    if (!remote_url_parse(&url_info, uri)) {
        console_print_error("Error parsing URI \"%s\"\n", uri);
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_INVALID_REQUEST);
        return NULL;
    }
	if (!remote_url_protocol_equals(url_info.protocol, "https")) {
		console_print_error("Refusing non-HTTPS remote URI\n");
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_INVALID_REQUEST);
		return NULL;
	}

	char headers[6144];
	if (!remote_build_credential_headers(headers, sizeof(headers), api_token, cookie_header)) {
		console_print_error("Remote HTTP credentials are invalid or too large\n");
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_INVALID_REQUEST);
		return NULL;
	}

	console_print_verbose("Sending remote HTTP request; credentials and URI are redacted.\n");

    i32 portno = atoi(url_info.port);
    tls_connection_t* connection = open_remote_connection(url_info.site, portno, alloca(sizeof(tls_connection_t)));
    if (!connection) {
		remote_request_set_error(options, HTTP_RESPONSE_ERROR_CONNECTION);
        return NULL;
    }

	http_response_t* response = remote_connection_request_with_headers_and_options(
		connection, url_info.host_header, url_info.path,
		"application/json,image/jpeg,*/*", headers, true, options);
	close_remote_connection(connection);
    return response;
}

http_response_t* open_remote_uri_with_extra_headers(const char* uri, const char* api_token,
		const char* cookie_header) {
	return open_remote_uri_with_extra_headers_and_options(uri, api_token, cookie_header, NULL);
}

http_response_t* open_remote_uri(app_state_t *app_state, const char *uri, const char* api_token) {
    (void)app_state;
    return open_remote_uri_with_extra_headers(uri, api_token, NULL);
}

void http_response_destroy(http_response_t* response) {
    ASSERT(response);
    memrw_destroy(&response->buffer);
    free(response);
}
