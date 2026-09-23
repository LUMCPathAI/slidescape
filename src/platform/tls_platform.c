/*
 * Slidescape, a whole-slide image viewer for digital pathology.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tls_platform.h"
#include "mbedtls/threading.h"

static psa_status_t crypto_status = PSA_ERROR_BAD_STATE;

#if defined(_WIN32)
static INIT_ONCE crypto_once = INIT_ONCE_STATIC_INIT;

static int tls_mutex_init(mbedtls_platform_mutex_t* mutex) {
	InitializeSRWLock(mutex);
	return 0;
}

static void tls_mutex_destroy(mbedtls_platform_mutex_t* mutex) { (void)mutex; }

static int tls_mutex_lock(mbedtls_platform_mutex_t* mutex) {
	AcquireSRWLockExclusive(mutex);
	return 0;
}

static int tls_mutex_unlock(mbedtls_platform_mutex_t* mutex) {
	ReleaseSRWLockExclusive(mutex);
	return 0;
}

static int tls_cond_init(mbedtls_platform_condition_variable_t* cond) {
	InitializeConditionVariable(cond);
	return 0;
}

static void tls_cond_destroy(mbedtls_platform_condition_variable_t* cond) { (void)cond; }

static int tls_cond_signal(mbedtls_platform_condition_variable_t* cond) {
	WakeConditionVariable(cond);
	return 0;
}

static int tls_cond_broadcast(mbedtls_platform_condition_variable_t* cond) {
	WakeAllConditionVariable(cond);
	return 0;
}

static int tls_cond_wait(mbedtls_platform_condition_variable_t* cond, mbedtls_platform_mutex_t* mutex) {
	return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}

static BOOL CALLBACK tls_init_once(PINIT_ONCE once, PVOID param, PVOID* context) {
	(void)once; (void)param; (void)context;
	/* Install callbacks before any PSA call, including calls from other threads. */
	mbedtls_threading_set_alt(tls_mutex_init, tls_mutex_destroy, tls_mutex_lock, tls_mutex_unlock,
	                         tls_cond_init, tls_cond_destroy, tls_cond_signal, tls_cond_broadcast, tls_cond_wait);
	crypto_status = psa_crypto_init();
	return TRUE;
}
#else
static pthread_once_t crypto_once = PTHREAD_ONCE_INIT;

static void tls_init_once(void) {
	crypto_status = psa_crypto_init();
}
#endif

psa_status_t slidescape_crypto_init(void) {
#if defined(_WIN32)
	if (!InitOnceExecuteOnce(&crypto_once, tls_init_once, NULL, NULL)) return PSA_ERROR_BAD_STATE;
#else
	if (pthread_once(&crypto_once, tls_init_once) != 0) return PSA_ERROR_BAD_STATE;
#endif
	return crypto_status;
}
