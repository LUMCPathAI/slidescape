/* Slidescape's TF-PSA-Crypto profile, shared by every library consumer. */
#ifndef SLIDESCAPE_CRYPTO_CONFIG_H
#define SLIDESCAPE_CRYPTO_CONFIG_H

#define TF_PSA_CRYPTO_CONFIG_VERSION 0x01020000
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_BUILTIN_GET_ENTROPY
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_FS_IO

/* PSA owns process-wide keys and RNG state, used by concurrent tile requests. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

#define PSA_WANT_ALG_GCM 1
#define PSA_WANT_ALG_CHACHA20_POLY1305 1
#define PSA_WANT_ALG_ECDH 1
#define PSA_WANT_ALG_ECDSA 1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN 1
#define PSA_WANT_ALG_RSA_PSS 1
/* Upstream's X.509 PSS support also requires the RSA OAEP implementation. */
#define PSA_WANT_ALG_RSA_OAEP 1
#define PSA_WANT_ALG_SHA_256 1
#define PSA_WANT_ALG_SHA_384 1
#define PSA_WANT_ALG_SHA_512 1
/* Parse older trust anchors. The default X.509 profile still rejects SHA-1
 * signatures on peer/intermediate certificates; do not weaken that profile. */
#define PSA_WANT_ALG_SHA_1 1
#define PSA_WANT_ALG_HMAC 1
#define PSA_WANT_ALG_HKDF 1
#define PSA_WANT_ALG_HKDF_EXTRACT 1
#define PSA_WANT_ALG_HKDF_EXPAND 1
#define PSA_WANT_ALG_TLS12_PRF 1

#define PSA_WANT_ECC_MONTGOMERY_255 1
#define PSA_WANT_ECC_SECP_R1_256 1
#define PSA_WANT_ECC_SECP_R1_384 1
#define PSA_WANT_ECC_SECP_R1_521 1
#define PSA_WANT_KEY_TYPE_AES 1
#define PSA_WANT_KEY_TYPE_CHACHA20 1
#define PSA_WANT_KEY_TYPE_HMAC 1
#define PSA_WANT_KEY_TYPE_DERIVE 1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT 1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC 1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT 1

/* PEM/DER certificates and unencrypted, externally provisioned private keys. */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* Upstream performs architecture detection and runtime CPU capability checks. */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_AESNI_C
#define MBEDTLS_AESCE_C
#define MBEDTLS_ECP_NIST_OPTIM

/* No persistent PSA key files, MD5, legacy ciphers, or extra crypto drivers. */
#endif
