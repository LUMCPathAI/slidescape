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
#include "platform.h"
#include "viewer.h"
#include "remote.h"
#include "stringutils.h"

#ifdef __APPLE__
// For loading the root certificates (requires linking against "-framework Security")
#include <Security/Security.h>
#endif

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PLATFORM_C)
#include "mbedtls/platform.h"
#else
#include <stdio.h>
#include <stdlib.h>
#define mbedtls_time            time
#define mbedtls_time_t          time_t
#define mbedtls_fprintf         fprintf
#define mbedtls_printf          printf
#define mbedtls_exit            exit
#define MBEDTLS_EXIT_SUCCESS    EXIT_SUCCESS
#define MBEDTLS_EXIT_FAILURE    EXIT_FAILURE
#endif /* MBEDTLS_PLATFORM_C */

#include "mbedtls/net_sockets.h"
#include "mbedtls/debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"

//#define SERVER_PORT "443"
//#define SERVER_PORT "4433"
//#define SERVER_NAME "google.com"
//#define SERVER_NAME "localhost"
//#define GET_REQUEST "GET / HTTP/1.0\r\n\r\n"

#include "http_parser.h"

#define DEBUG_LEVEL DO_DEBUG

struct tls_connection_t {
	i64 start_clock;
	i64 sockfd;
	bool use_tls;
	mbedtls_net_context server_fd;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cacert;
};

static void my_debug( void *ctx, int level,
                      const char *file, int line,
                      const char *str )
{
	((void) level);

	mbedtls_fprintf( (FILE *) ctx, "%s:%04d: %s", file, line, str );
	fflush(  (FILE *) ctx  );
}


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
	int ret = 1, len;
	int exit_code = MBEDTLS_EXIT_FAILURE;
	uint32_t flags;
	unsigned char buf[1024];
	const char *pers = "ssl_client1";
	tls_connection_t* connection = (tls_connection_t*) alloced_mem_for_struct;
	connection->use_tls = true;
	connection->start_clock = get_clock();

#if defined(MBEDTLS_DEBUG_C)
	mbedtls_debug_set_threshold( DEBUG_LEVEL );
#endif

	/*
	 * 0. Initialize the RNG and the session data
	 */
	mbedtls_net_init( &connection->server_fd );
	mbedtls_ssl_init( &connection->ssl );
	mbedtls_ssl_config_init( &connection->conf );
	mbedtls_x509_crt_init( &connection->cacert );
	mbedtls_ctr_drbg_init( &connection->ctr_drbg );

	console_print_verbose( "\n  . Seeding the random number generator..." );
	fflush( stdout );

	mbedtls_entropy_init( &connection->entropy );
	if( ( ret = mbedtls_ctr_drbg_seed( &connection->ctr_drbg, mbedtls_entropy_func, &connection->entropy,
	                                   (const unsigned char *) pers,
	                                   strlen( pers ) ) ) != 0 )
	{
		console_print_verbose( " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret );
		goto exit;
	}

	// TODO: console append to last entry?
	console_print_verbose( " ok\n" );

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
	mbedtls_ssl_conf_rng( &connection->conf, mbedtls_ctr_drbg_random, &connection->ctr_drbg );
	mbedtls_ssl_conf_dbg( &connection->conf, my_debug, stdout );

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

	mbedtls_ssl_set_bio( &connection->ssl, &connection->server_fd, mbedtls_net_send, mbedtls_net_recv, NULL );

	/*
	 * 4. Handshake
	 */
	console_print_verbose( "  . Performing the SSL/TLS handshake..." );

	while( ( ret = mbedtls_ssl_handshake( &connection->ssl ) ) != 0 )
	{
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
		mbedtls_ctr_drbg_free( &connection->ctr_drbg );
		mbedtls_entropy_free( &connection->entropy );

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
		mbedtls_ctr_drbg_free(&connection->ctr_drbg);
		mbedtls_entropy_free(&connection->entropy);
	}

	float seconds_elapsed = get_seconds_elapsed(connection->start_clock, get_clock());
	return seconds_elapsed;
}

bool remote_request(tls_connection_t* connection, const char* request, i32 request_len, memrw_t* mem_buffer) {

	bool success = false;
	/*
	 * 3. Write the GET request
	 */
//	mbedtls_printf( "  > Write to server:" );
//	fflush( stdout );

//	u8 buf[1024];
//	i32 len = sprintf( (char *) buf, GET_REQUEST );
	i32 ret = 1;
	i32 len = request_len;

#ifdef REMOTE_VERBOSE
	console_print("Writing request: %s", request);
#endif

	while( ( ret = mbedtls_ssl_write( &connection->ssl, (const u8*)request, request_len ) ) <= 0 )
	{
		if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
		{
			console_print_error( " failed\n  ! mbedtls_ssl_write returned %d\n\n", ret );
			goto exit;
		}
	}

	len = ret;
#ifdef REMOTE_VERBOSE
	console_print( " %d bytes written\n\n%s", request_len, request );
#endif

	/*
	 * 7. Read the HTTP response
	 */
#ifdef REMOTE_VERBOSE
	console_print( "  < Read from server:" );
#endif
	fflush( stdout );

	u8 read_buffer[KILOBYTES(16)];

	do
	{
		len = sizeof( read_buffer );
		memset( read_buffer, 0, sizeof( read_buffer ) );
		ret = mbedtls_ssl_read( &connection->ssl, read_buffer, len );

		if( ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE )
			continue;

		if( ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ) {
			ret = 0;
			success = true;
			break;
		}

		if( ret < 0 )
		{
			console_print_error( "failed\n  ! mbedtls_ssl_read returned %d\n\n", ret );
			break;
		}

		if( ret == 0 )
		{
//			mbedtls_printf( "\n\nEOF\n\n" );
			success = true;
			break;
		}

		len = ret;
#ifdef REMOTE_VERBOSE
		console_print( " %d bytes read\n", len );
#endif
		memrw_push_back(mem_buffer, read_buffer, len);
	}
	while( 1 );

	exit:
    memrw_putc(0, mem_buffer); // zero-terminate
	return success;

}

u8 *do_http_request(const char *hostname, i32 portno, const char *uri, i32 *bytes_read, i32 thread_id) {
	tls_connection_t* connection = open_remote_connection(hostname, portno, alloca(sizeof(tls_connection_t)));
	if (connection) {
		static const char requestfmt[] = "GET %s HTTP/1.1\r\nConnection: close\r\n\r\n";
		char request[4096];
		snprintf(request, sizeof(request), requestfmt, uri);
		size_t request_len = strlen(request);

		memrw_t mem_buffer = memrw_create(MEGABYTES(1));
		bool read_ok = remote_request(connection, request, request_len, &mem_buffer);
		float seconds_elapsed = close_remote_connection(connection);
		if (read_ok) {
			console_print_verbose( "[thread %d] http request: %d bytes read in %g seconds\n", thread_id, mem_buffer.used_size, seconds_elapsed );
			if (bytes_read) {
				*bytes_read = mem_buffer.used_size;
			}
			return mem_buffer.data;
//			console_print("Downloaded case list '%s' in %g seconds.\n", filename, seconds_elapsed);
		} else {
			memrw_destroy(&mem_buffer);
			return NULL;
		}
	}
	return NULL;
}

u8 *download_remote_chunk(const char *hostname, i32 portno, const char *filename, i64 chunk_offset, i64 chunk_size,
                          i32 *bytes_read, i32 thread_id) {

	char uri[2048] = {0};
	snprintf(uri, sizeof(uri), "/slide/%s/%lld/%lld", filename, chunk_offset, chunk_size);
	u8* read_buffer = do_http_request(hostname, portno, uri, bytes_read, thread_id);
	return read_buffer;

}

u8 *download_remote_batch(const char *hostname, i32 portno, const char *filename, i64 *chunk_offsets, i64 *chunk_sizes,
                          i32 batch_size, i32 *bytes_read, i32 thread_id) {
	ASSERT(batch_size > 0);
	char uri[4092] = {0};
	char* pos = uri;
	i32 bytes_printed = snprintf(uri, sizeof(uri), "/slide/%s", filename);
	pos += bytes_printed;
	i32 bytes_left = sizeof(uri) - bytes_printed;
	for (i32 i = 0; i < batch_size; ++i) {
		if (bytes_left <= 0) {
			ASSERT(!"uri became too long");
			return NULL;
		}
		bytes_printed += snprintf(pos, bytes_left, "/%lld/%lld", chunk_offsets[i], chunk_sizes[i]);
		pos = uri + bytes_printed;
		bytes_left = sizeof(uri) - bytes_printed;
	}
	u8* read_buffer = do_http_request(hostname, portno, uri, bytes_read, thread_id);
	return read_buffer;
}

bool open_remote_slide(app_state_t *app_state, const char *hostname, i32 portno, const char *filename) {

	bool success = false;

	static const char requestfmt[] = "GET /slide/%s/header HTTP/1.1\r\nConnection: close\r\n\r\n";
	char request[4096];
	snprintf(request, sizeof(request), requestfmt, filename);
	i32 request_len = (i32)strlen(request);

	tls_connection_t* connection = open_remote_connection(hostname, portno, alloca(sizeof(tls_connection_t)));
	if (!connection) {
		return false;
	}

	memrw_t mem_buffer = memrw_create(MEGABYTES(2));
	bool32 read_ok = remote_request(connection, request, request_len, &mem_buffer);
	float seconds_elapsed = close_remote_connection(connection);

	if (read_ok && mem_buffer.used_size > 0) {
		// now we should have the whole HTTP response
#if REMOTE_CLIENT_VERBOSE
		console_print("HTTP read finished, length = %d\n", mem_buffer->len);
#endif
//	fwrite(read_buffer, total_bytes_read, 1, stdout);

		tiff_t tiff = {0};
		if (tiff_deserialize(&tiff, mem_buffer.data, mem_buffer.used_size)) {
			tiff.is_remote = true;
			tiff.location = (network_location_t){ .hostname = hostname, .portno = portno, .filename = filename };

			unload_all_images(app_state);
			image_t* image = (image_t*)calloc(1, sizeof(image_t));
			bool is_valid = init_image_from_tiff(image, tiff, false, NULL);
			if (image->is_valid) {
                platform_mutex_init(&image->lock);
				image->lock_initialized = true;
			}
			add_image(app_state, image, true, false);
			success = is_valid;
		} else {
			tiff_destroy(&tiff);
		}

	}
	memrw_destroy(&mem_buffer);


//	float seconds_elapsed = close_remote_connection(connection);
	console_print("Open remote took %g seconds\n", seconds_elapsed);
	return success;
}


// from https://stackoverflow.com/questions/726122/best-ways-of-parsing-a-url-using-c
typedef struct url_info_t
{
    const char* protocol;
    const char* site;
    const char* port;
    const char* path;
} url_info_t;

url_info_t* split_url(url_info_t* info, const char* url)
{
    if (!info || !url)
        return NULL;
    info->protocol = strtok(strcpy((char*)malloc(strlen(url)+1), url), "://");
    if (!info->protocol)
        return NULL;
    info->site = strstr(url, "://");
    if (info->site)
    {
        info->site += 3;
        char* site_port_path = strcpy((char*)calloc(1, strlen(info->site) + 1), info->site);
        info->site = strtok(site_port_path, ":");
        info->site = strtok(site_port_path, "/");
    }
    else
    {
        char* site_port_path = strcpy((char*)calloc(1, strlen(url) + 1), url);
        info->site = strtok(site_port_path, ":");
        info->site = strtok(site_port_path, "/");
    }
    char* URL = strcpy((char*)malloc(strlen(url) + 1), url);
    info->port = strstr(URL + 6, ":");
    char* port_path = 0;
    char* port_path_copy = 0;
    if (info->port && isdigit(*(port_path = (char*)info->port + 1)))
    {
        port_path_copy = strcpy((char*)malloc(strlen(port_path) + 1), port_path);
        char * r = strtok(port_path, "/");
        if (r)
            info->port = r;
        else
            info->port = port_path;
    }
    else
        info->port = "80";
    if (port_path_copy)
        info->path = port_path_copy + strlen(info->port ? info->port : "");
    else
    {
        char* path = strstr(URL + 8, "/");
        info->path = path ? path : "/";
    }
    int r = strcmp(info->protocol, info->site) == 0;
    if (r && strcmp(info->port, "80") == 0)
        info->protocol = "http";
    else if (r)
        info->protocol = "tcp";
    return info;
}

static i32 header_field_callback(http_parser *parser, const char *p, size_t len) {
//    parser->data;
    return 0;
}

typedef struct http_response_parse_context_t {
    http_response_t* response;
    bool message_complete;
    bool last_was_header_value;
    char header_name[128];
    size_t header_name_length;
    char header_value[512];
    size_t header_value_length;
} http_response_parse_context_t;

static void http_response_commit_header(http_response_parse_context_t* context) {
    context->header_name[context->header_name_length] = '\0';
    context->header_value[context->header_value_length] = '\0';
    if (strcasecmp(context->header_name, "Content-Type") == 0) {
        copy_cstring(context->response->content_type, context->header_value,
                     sizeof(context->response->content_type));
    }
    context->header_name_length = 0;
    context->header_value_length = 0;
}

static i32 body_callback(http_parser *parser, const char *p, size_t len) {
    http_response_parse_context_t* context = (http_response_parse_context_t*)parser->data;
    if (context->last_was_header_value) {
        http_response_commit_header(context);
        context->last_was_header_value = false;
    }
    http_response_t* response = context->response;
    memrw_push_back(&response->buffer, (void*)p, len);
    response->content_length += len;
    return 0;
}

static i32 response_header_field_callback(http_parser* parser, const char* p, size_t len) {
    http_response_parse_context_t* context = (http_response_parse_context_t*)parser->data;
    if (context->last_was_header_value) {
        http_response_commit_header(context);
        context->last_was_header_value = false;
    }
    size_t available = sizeof(context->header_name) - 1 - context->header_name_length;
    size_t copy_length = ATMOST(len, available);
    memcpy(context->header_name + context->header_name_length, p, copy_length);
    context->header_name_length += copy_length;
    return 0;
}

static i32 response_header_value_callback(http_parser* parser, const char* p, size_t len) {
    http_response_parse_context_t* context = (http_response_parse_context_t*)parser->data;
    size_t available = sizeof(context->header_value) - 1 - context->header_value_length;
    size_t copy_length = ATMOST(len, available);
    memcpy(context->header_value + context->header_value_length, p, copy_length);
    context->header_value_length += copy_length;
    context->last_was_header_value = true;
    return 0;
}

static i32 message_complete_callback(http_parser *parser) {
    http_response_parse_context_t* context = (http_response_parse_context_t*)parser->data;
    context->message_complete = true;
    return 0;
}

static i32 remote_build_get_request(char* request, size_t request_size, const char* host, const char* path, const char* api_token, const char* cookie_header, bool close_after_request) {
    char token_header_string[4196] = "";
    if (api_token && api_token[0] != '\0') {
        snprintf(token_header_string, sizeof(token_header_string), "Authorization: Bearer %s\r\n", api_token);
    }
    char cookie_header_string[1024] = "";
    if (cookie_header && cookie_header[0] != '\0') {
        snprintf(cookie_header_string, sizeof(cookie_header_string), "Cookie: %s\r\n", cookie_header);
    }

    static const char requestfmt[] =
            "GET %s HTTP/1.1\r\n"
            "Accept: application/json,image/jpeg,*/*\r\n"
            "Accept-Language: en,nl;q=0.9,en-US;q=0.8,af;q=0.7\r\n%s%s"
            "Cache-Control: max-age=0\r\n"
            "Connection: %s\r\n"
            "Host: %s\r\n"
            "Upgrade-Insecure-Requests: 1\r\n\r\n";
    return snprintf(request, request_size, requestfmt, path, token_header_string, cookie_header_string,
                    close_after_request ? "close" : "keep-alive", host);
}

static i32 remote_build_get_request_with_headers(char* request, size_t request_size,
                                                 const char* host, const char* path,
                                                 const char* accept, const char* extra_headers,
                                                 bool close_after_request) {
    return snprintf(request, request_size,
                    "GET %s HTTP/1.1\r\n"
                    "Accept: %s\r\n"
                    "%s"
                    "Connection: %s\r\n"
                    "Host: %s\r\n\r\n",
                    path, accept ? accept : "*/*", extra_headers ? extra_headers : "",
                    close_after_request ? "close" : "keep-alive", host);
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
    return use_tls ? remote_connection_open(hostname, portno)
                   : remote_connection_open_plain(hostname, portno);
}

void remote_connection_close_and_free(tls_connection_t* connection) {
    if (!connection) return;
    close_remote_connection(connection);
    free(connection);
}

http_response_t* remote_connection_request_with_headers(tls_connection_t* connection, const char* host,
                                                        const char* path, const char* accept,
                                                        const char* extra_headers,
                                                        bool close_after_request) {
    ASSERT(connection);

    char request[4096];
    i32 request_len = remote_build_get_request_with_headers(request, sizeof(request), host, path,
                                                            accept, extra_headers, close_after_request);
    if (request_len <= 0 || request_len >= sizeof(request)) {
        return NULL;
    }

    i32 ret = 1;
    i32 bytes_written = 0;
    while (bytes_written < request_len) {
        ret = connection->use_tls
              ? mbedtls_ssl_write(&connection->ssl, (const u8*)request + bytes_written,
                                  request_len - bytes_written)
              : mbedtls_net_send(&connection->server_fd, (const u8*)request + bytes_written,
                                 request_len - bytes_written);
        if (ret > 0) {
            bytes_written += ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            console_print_error("remote_connection_request(): network write returned %d\n", ret);
            return NULL;
        }
    }

    http_response_t* response = (http_response_t*)calloc(1, sizeof(http_response_t));
    response->buffer = memrw_create(MEGABYTES(1));

    http_response_parse_context_t context = {};
    context.response = response;

    http_parser_settings settings = {};
    settings.on_header_field = response_header_field_callback;
    settings.on_header_value = response_header_value_callback;
    settings.on_body = body_callback;
    settings.on_message_complete = message_complete_callback;

    http_parser parser = {};
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = (void*)&context;

    u8 read_buffer[KILOBYTES(16)];
    while (!context.message_complete) {
        ret = connection->use_tls
              ? mbedtls_ssl_read(&connection->ssl, read_buffer, sizeof(read_buffer))
              : mbedtls_net_recv(&connection->server_fd, read_buffer, sizeof(read_buffer));

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) {
            break;
        }
        if (ret < 0) {
            console_print_error("remote_connection_request(): mbedtls_ssl_read returned %d\n", ret);
            http_response_destroy(response);
            return NULL;
        }

        size_t nparsed = http_parser_execute(&parser, &settings, (char*)read_buffer, ret);
        if (nparsed != (size_t)ret && !context.message_complete) {
            console_print_error("remote_connection_request(): HTTP parser stopped after %zu of %d bytes\n", nparsed, ret);
            http_response_destroy(response);
            return NULL;
        }
    }

    response->status_code = parser.status_code;
    memrw_putc(0, &response->buffer);
    return response;
}

http_response_t* remote_connection_request(tls_connection_t* connection, const char* host, const char* path, const char* api_token, const char* cookie_header, bool close_after_request) {
    char headers[6144] = "";
    size_t used = 0;
    if (api_token && api_token[0]) {
        used += snprintf(headers + used, sizeof(headers) - used,
                         "Authorization: Bearer %s\r\n", api_token);
    }
    if (cookie_header && cookie_header[0] && used < sizeof(headers)) {
        snprintf(headers + used, sizeof(headers) - used, "Cookie: %s\r\n", cookie_header);
    }
    return remote_connection_request_with_headers(connection, host, path,
                                                  "application/json,image/jpeg,*/*",
                                                  headers, close_after_request);
}

http_response_t* open_remote_uri_with_extra_headers(const char *uri, const char* api_token, const char* cookie_header) {

    url_info_t url_info = {};
    if (!split_url(&url_info, uri)) {
        console_print_error("Error parsing URI \"%s\"\n", uri);
        return NULL;
    }

    char request[4096];
    remote_build_get_request(request, sizeof(request), url_info.site, url_info.path, api_token, cookie_header, true);
    i32 request_len = (i32)strlen(request);

    console_print_verbose("%s\n", request);

    i32 portno = atoi(url_info.port);
    if (portno == 80 && strcmp(url_info.protocol, "https") == 0) {
        portno = 443;
    }
    tls_connection_t* connection = open_remote_connection(url_info.site, portno, alloca(sizeof(tls_connection_t)));
    if (!connection) {
        return NULL;
    }

    memrw_t mem_buffer = memrw_create(MEGABYTES(2));
    bool32 read_ok = remote_request(connection, request, request_len, &mem_buffer);
    float seconds_elapsed = close_remote_connection(connection);

    http_response_t* response = NULL;
    if (read_ok && mem_buffer.used_size > 0) {
        // now we should have the whole HTTP response
        console_print_verbose("%s\n", mem_buffer.data);

        response = calloc(1, sizeof(http_response_t));
        response->buffer = memrw_create(MEGABYTES(2));

        http_parser_settings settings = {};
        settings.on_header_field = header_field_callback;
        settings.on_body = body_callback;
        settings.on_message_complete = message_complete_callback;

        http_parser* parser = malloc(sizeof(http_parser));
        http_parser_init(parser, HTTP_RESPONSE);
        http_response_parse_context_t context = {};
        context.response = response;
        parser->data = (void*)&context;

        i32 nparsed = http_parser_execute(parser, &settings, (char*)mem_buffer.data, mem_buffer.used_size);
        response->status_code = parser->status_code;
        memrw_putc(0, &response->buffer); // zero-terminate the content string

        console_print_verbose("Content length = %d\n", response->content_length);
        if (response->content_length < KILOBYTES(64)) {
            console_print_verbose("%s\n", response->buffer.data);
        }

        free(parser);

    }

    memrw_destroy(&mem_buffer);

    return response;
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
