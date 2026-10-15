/* libjpeg-turbo build number */
#define BUILD  "@BUILD@"

/* How to hide global symbols. */
#ifdef _MSC_VER
#define HIDDEN
#else
#define HIDDEN  __attribute__((visibility("hidden")))
#endif

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#ifdef _MSC_VER
#define INLINE __forceinline
#else
#define INLINE inline __attribute__((always_inline))
#endif

/* How to obtain thread-local storage */
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

/* Define to the full name of this package. */
#define PACKAGE_NAME  "@CMAKE_PROJECT_NAME@"

/* Version number of package */
#define VERSION  "3.1.4.1"

/* The size of `size_t', as computed by sizeof. */
#ifdef _MSC_VER
#if defined(_WIN64)
#define SIZEOF_SIZE_T 8
#elif defined(_WIN32)
#define SIZEOF_SIZE_T 4
#elif defined(__SIZEOF_SIZE_T__)
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#else
#error "Unable to determine sizeof(size_t)"
#endif
#else
#define SIZEOF_SIZE_T __SIZEOF_POINTER__
#endif

/* Define if your compiler has __builtin_ctzl() and sizeof(unsigned long) == sizeof(size_t). */
#ifndef _WIN32
#define HAVE_BUILTIN_CTZL
#endif

/* Define to 1 if you have the <intrin.h> header file. */
#if !defined(__APPLE__) && defined(__has_include)
#if __has_include(<intrin.h>)
#define HAVE_INTRIN_H
#endif
#endif

#if defined(_WIN32) && defined(HAVE_INTRIN_H)
#if (SIZEOF_SIZE_T == 8)
#define HAVE_BITSCANFORWARD64
#elif (SIZEOF_SIZE_T == 4)
#define HAVE_BITSCANFORWARD
#endif
#endif

#if defined(__has_attribute)
#if __has_attribute(fallthrough)
#define FALLTHROUGH  __attribute__((fallthrough));
#else
#define FALLTHROUGH
#endif
#else
#define FALLTHROUGH
#endif

/*
 * Define BITS_IN_JSAMPLE as either
 *   8   for 8-bit sample values (the usual setting)
 *   12  for 12-bit sample values
 * Only 8 and 12 are legal data precisions for lossy JPEG according to the
 * JPEG standard, and the IJG code does not support anything else!
 */

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8

/* Support arithmetic encoding */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding */
#define D_ARITH_CODING_SUPPORTED 1

/* Use accelerated SIMD routines. */
#ifdef SLIDESCAPE_JPEG_WITH_SIMD
#define WITH_SIMD 1
#endif

#endif
