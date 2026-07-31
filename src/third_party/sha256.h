/*********************************************************************
* Filename:   sha256.h
* Author:     Brad Conte (brad AT bradconte.com)
* SPDX-License-Identifier: Apache-2.0
* Disclaimer: This code is presented "as is" without any guarantees.
* Details:    Defines the API for the corresponding SHA-256 implementation.
*********************************************************************/

#ifndef SHA256_H
#define SHA256_H

/*************************** HEADER FILES ***************************/
#include <stddef.h>
#include <stdint.h>

/****************************** MACROS ******************************/
#define SHA256_BLOCK_SIZE 32            // SHA256 outputs a 32 byte digest

/**************************** DATA TYPES ****************************/
typedef struct slidescape_sha256_context_t {
	uint8_t data[64];
	uint32_t datalen;
	unsigned long long bitlen;
	uint32_t state[8];
} slidescape_sha256_context_t;

/*********************** FUNCTION DECLARATIONS **********************/
void slidescape_sha256_init(slidescape_sha256_context_t *ctx);
void slidescape_sha256_update(slidescape_sha256_context_t *ctx,
		const uint8_t data[], size_t len);
void slidescape_sha256_final(slidescape_sha256_context_t *ctx, uint8_t hash[]);

#endif   // SHA256_H
