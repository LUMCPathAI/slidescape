/* Shared initialization for Slidescape's TLS/cryptography consumers. */
#ifndef SLIDESCAPE_TLS_PLATFORM_H
#define SLIDESCAPE_TLS_PLATFORM_H

#include <psa/crypto.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call before any Mbed TLS/PSA operation. Safe from concurrent callers.
 * Global PSA state lives until process exit; never free it per connection. */
psa_status_t slidescape_crypto_init(void);

#ifdef __cplusplus
}
#endif
#endif
