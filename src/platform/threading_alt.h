/* Native Windows synchronization types for TF-PSA-Crypto. */
#ifndef SLIDESCAPE_THREADING_ALT_H
#define SLIDESCAPE_THREADING_ALT_H

/* Match Slidescape's existing Windows Vista-or-newer platform baseline. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK mbedtls_platform_mutex_t;
typedef CONDITION_VARIABLE mbedtls_platform_condition_variable_t;

#endif
