/* Slidescape's Mbed TLS profile. Upstream defaults remain untouched in deps/. */
#ifndef SLIDESCAPE_TLS_CONFIG_H
#define SLIDESCAPE_TLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x04020000
#define MBEDTLS_NET_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_VERSION_C

/* HTTPS client and server, with forward secrecy and AEAD ciphers only. */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES

/* Read externally provisioned certificates/CRLs; do not issue certificates. */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* No DTLS, PSK, renegotiation, session tickets, 0-RTT or TLS debug dumps. */
#endif
