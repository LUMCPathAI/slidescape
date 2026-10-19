/*
  BSD 2-Clause License

  Copyright (c) 2019-2024, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once
#ifndef COMMON_H
#define COMMON_H

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef APP_TITLE
#define APP_TITLE "Application"
#endif

#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

// Platform detection
#ifdef _WIN32
#define WINDOWS 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WINVER 0x0600
#define OPENGL_H <glad/glad.h>
#define PATH_SEP "\\"
#else
#define WINDOWS 0
#define PATH_SEP "/"
#endif

#ifdef __APPLE__
#define APPLE 1
#include <TargetConditionals.h>
#if TARGET_CPU_ARM64
#define APPLE_ARM 1
#else
#define APPLE_ARM 0
#endif
#define OPENGL_H <OpenGL/gl3.h>
#else
#define APPLE 0
#define APPLE_ARM 0
#endif

#if defined(__linux__) || (!defined(__APPLE__) && (defined(__unix__) || defined(_POSIX_VERSION)))
#define LINUX 1
#define OPENGL_H <GL/glew.h>
#else
#define LINUX 0
#endif

#if LINUX
#include <features.h>
#endif
#if !WINDOWS
#include <unistd.h> // for access(), F_OK
#include <stdlib.h>
//#define _aligned_malloc(size, alignment) aligned_alloc(alignment, size)
//#define _aligned_free(ptr) free(ptr)
#endif

// Compiler detection
#ifdef _MSC_VER
#define COMPILER_MSVC 1
#else
#define COMPILER_MSVC 0
#endif

#ifdef __GNUC__
#define COMPILER_GCC 1
#else
#define COMPILER_GCC 0
#endif

// Host byte order is assumed to be known at compile time. If a new compiler does
// not expose byte-order macros, define HOST_BIG_ENDIAN in config.h.
#ifndef HOST_BIG_ENDIAN
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define HOST_BIG_ENDIAN 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define HOST_BIG_ENDIAN 0
#elif defined(_WIN32) || defined(__LITTLE_ENDIAN__) || defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__) || \
      defined(__ARMEL__) || defined(__AARCH64EL__) || defined(__i386__) || defined(__x86_64__)
#define HOST_BIG_ENDIAN 0
#elif defined(__BIG_ENDIAN__) || defined(_MIPSEB) || defined(__MIPSEB) || defined(__MIPSEB__) || \
      defined(__ARMEB__) || defined(__AARCH64EB__) || defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
#define HOST_BIG_ENDIAN 1
#else
#error "Could not determine host byte order. Define HOST_BIG_ENDIAN in config.h."
#endif
#endif

#define HOST_LITTLE_ENDIAN (!HOST_BIG_ENDIAN)
#define DATA_ENDIAN_DIFFERS(data_is_big_endian) ((data_is_big_endian) != HOST_BIG_ENDIAN)

// IDE detection (for dealing with pesky preprocessor highlighting issues)
#if defined(__JETBRAINS_IDE__)
#define CODE_EDITOR 1
#else
#define CODE_EDITOR 0
#endif

// Use 64-bit file offsets for fopen, etc.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#if COMPILER_MSVC
#include <io.h>
#define access _access
#define F_OK 0 // check for file existence
#define	S_ISDIR(m)	(((m) & 0xF000) == 0x4000) // check for whether a file is a directory (from stat.h)
#define	S_ISREG(m)	(((m) & 0xF000) == 0x8000) // check for whether a file is a regular file (from stat.h)
#define wstat _wstat
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#define alloca _alloca
#define fseeko64 _fseeki64
#define fopen64 fopen
#endif

#if APPLE
#define fseeko64 fseeko
#define fopen64 fopen
#define fgetpos64 fgetpos
#define fsetpos64 fsetpos
#endif

#ifndef THREAD_LOCAL
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif
#endif

#if LINUX
#include <fcntl.h>
#endif

// adapted from lz4.c
#ifndef FORCE_INLINE
#  ifdef _MSC_VER    /* Visual Studio */
#    define FORCE_INLINE static __forceinline
#  else
#    if defined (__cplusplus) || defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
#      ifdef __GNUC__
#        define FORCE_INLINE static inline __attribute__((always_inline))
#      else
#        define FORCE_INLINE static inline
#      endif
#    else
#      define FORCE_INLINE static
#    endif /* __STDC_VERSION__ */
#  endif  /* _MSC_VER */
#endif /* LZ4_FORCE_INLINE */

// Explicit names for ownership boundaries that return memory allocated by the C runtime.
FORCE_INLINE void* libc_malloc(size_t size) {
	return malloc(size);
}
FORCE_INLINE void* libc_realloc(void* memory, size_t new_size) {
	return realloc(memory, new_size);
}
FORCE_INLINE void libc_free(void* memory) {
	free(memory);
}

// Typesafe dynamic array and hash tables for C
// https://github.com/nothings/stb/blob/master/stb_ds.h
// NOTE: need to define STB_DS_IMPLEMENTATION in one source file
#if __has_include("stb_ds.h")
#include "stb_ds.h"
#define arrlastptr(a) ((a)+(stbds_header(a)->length-1))
#endif

// NOTE: need to define STB_SPRINTF_IMPLEMENTATION in one source file
#ifndef STB_SPRINTF_H_INCLUDE
#include "stb_sprintf.h"
#endif
#undef sprintf
#undef snprintf
#undef vsprintf
#undef vsnprintf
#define sprintf   stbsp_sprintf
#define snprintf  stbsp_snprintf
#define vsprintf  stbsp_vsprintf
#define vsnprintf stbsp_vsnprintf

// Route output through application-provided logging functions when enabled.
// Otherwise, write directly to stdio. This does not require a GUI.
#ifndef USE_CONSOLE_LOGGING
#define USE_CONSOLE_LOGGING 0
#endif

#ifdef __cplusplus
extern "C" {
#endif
#if !USE_CONSOLE_LOGGING
#define console_print printf
#define console_print_error(...) fprintf(stderr, __VA_ARGS__)
extern bool is_verbose_mode; // NOTE: necessary to define this variable somewhere
#define console_print_verbose(...) do { if (is_verbose_mode) fprintf(stdout, __VA_ARGS__); } while(0)
#else
void console_print(const char *fmt, ...);
void console_print_verbose(const char *fmt, ...);
void console_print_error(const char *fmt, ...);
#endif
#ifdef __cplusplus
}
#endif

// Typedef choices for numerical types
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef long long i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;

typedef int32_t bool32;
typedef int8_t bool8;

// String type with a known size (don't assume zero-termination)
typedef struct str_t {
	const char* s;
	size_t len;
} str_t;

// Convenience macros
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#define ATLEAST(a, b) MAX(a, b)
#define ATMOST(a, b) MIN(a, b)
#define ABS(x) ((x) < 0 ? -(x) : (x))

#define LERP(t,a,b)               ( (a) + (t) * (float) ((b)-(a)) )
#define UNLERP(t,a,b)             ( ((t) - (a)) / (float) ((b) - (a)) )

#define CLAMP(x,xmin,xmax)  ((x) < (xmin) ? (xmin) : (x) > (xmax) ? (xmax) : (x))

#define MACRO_VAR(name) concat(name, __LINE__)
#define defer(start, end) for (int MACRO_VAR(_i_) = (start, 0); !MACRO_VAR(_i_); (MACRO_VAR(_i_)+=1,end))


#define SQUARE(x) ((x)*(x))

#define memset_zero(x) memset((x), 0, sizeof(*(x)))

#define KILOBYTES(n) (1024LL*(n))
#define MEGABYTES(n) (1024LL*KILOBYTES(n))
#define GIGABYTES(n) (1024LL*MEGABYTES(n))
#define TERABYTES(n) (1024LL*GIGABYTES(n))


#if defined(SOURCE_PATH_SIZE) && !defined(__FILENAME__)
#define __FILENAME__ (__FILE__ + SOURCE_PATH_SIZE)
#elif !defined(__FILENAME__)
#define __FILENAME__ __FILE__
#endif

#define fatal_error(...) _ssc_fatal_error(__FILE__, __LINE__, __func__, "" __VA_ARGS__)
#if FATAL_ERROR_DONT_INLINE
#define FATAL_ERROR_INLINE_SPECIFIER
// Not inlining _fatal_error() shaves a few kilobytes off the executable size.
// NOTE: if FATAL_ERROR_DONT_INLINE is enabled, FATAL_ERROR_IMPLEMENTATION must be defined in exactly 1 source file.
#ifdef __cplusplus
extern "C"
#endif
void _ssc_fatal_error(const char* source_filename, i32 line, const char* func, const char* message);
#else
#define FATAL_ERROR_IMPLEMENTATION
#define FATAL_ERROR_INLINE_SPECIFIER FORCE_INLINE
#endif //FATAL_ERROR_DONT_INLINE
#ifdef FATAL_ERROR_IMPLEMENTATION
FATAL_ERROR_INLINE_SPECIFIER void _ssc_fatal_error(const char* source_filename, i32 line, const char* func, const char* message) {
	fprintf(stderr, "%s(): %s:%d\n", func, source_filename, line);
	if (message[0] != '\0') fprintf(stderr, "Error: %s\n", message);
	fprintf(stderr, "A fatal error occurred (aborting).\n");
#if DO_DEBUG
	#if COMPILER_GCC
	__builtin_trap();
	#elif COMPILER_MSVC
	__debugbreak();
	#endif
#endif
	abort();
}
#endif //FATAL_ERROR_IMPLEMENTATION

#ifndef NDEBUG
#define DO_DEBUG 1
#define ASSERT(expr) do {if (!(expr)) fatal_error();} while(0)
#else
#define DO_DEBUG 0
#define ASSERT(expr)
#endif

//http://www.pixelbeat.org/programming/gcc/static_assert.html
#define ASSERT_CONCAT_(a, b) a##b
#define ASSERT_CONCAT(a, b) ASSERT_CONCAT_(a, b)
/* These can't be used after statements in c89. */
#ifdef __COUNTER__
#define STATIC_ASSERT(e) \
    ;enum { ASSERT_CONCAT(ASSERT_CONCAT(static_assert_, __COUNTER__), __LINE__) = 1/(int)(!!(e)) }
#else
/* This can't be used twice on the same line so ensure if using in headers
   * that the headers are not included twice (by wrapping in #ifndef...#endif)
   * Note it doesn't cause an issue when used on same line of separate modules
   * compiled with gcc -combine -fwhole-program.  */
  #define STATIC_ASSERT(e) \
    ;enum { ASSERT_CONCAT(assert_line_, __LINE__) = 1/(int)(!!(e)) }
#endif

#define DUMMY_STATEMENT do { int __x = 5; } while(0)

FORCE_INLINE u64 next_pow2(u64 x) {
	ASSERT(x > 1);
	x -= 1;
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	x |= (x >> 32);
	return x + 1;
}

FORCE_INLINE i32 div_floor(i32 a, i32 b) {
	return a / b - (a % b < 0);
}




#endif
