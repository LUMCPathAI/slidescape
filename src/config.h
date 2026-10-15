#pragma once

#ifndef NDEBUG
#define DO_DEBUG 1
#else
#define DO_DEBUG 0
#endif

#if !IS_SERVER
#define USE_CONSOLE_GUI 1
#endif

#define PANIC_DONT_INLINE 1

#define NO_TIMER_INITIALIZED_RUNTIME_CHECK

// ltalloc is disabled on all platforms. It has caused allocator corruption in
// multithreaded code, including reproducible failures in the Windows test suite.

#define USE_OPENGL_DEBUG_CONTEXT DO_DEBUG

#define USE_MULTIPLE_OPENGL_CONTEXTS 0
#define PREFER_DEDICATED_GRAPHICS 0

#define REMOTE_CLIENT_VERBOSE 0

#define LIBISYNTAX_NO_STB_SPRINTF_IMPLEMENTATION
#define LIBISYNTAX_NO_STB_IMAGE_IMPLEMENTATION
#define LIBISYNTAX_THREAD_POOL_SHARED_WITH_SLIDESCAPE

#define APP_TITLE "Slidescape"
#define APP_VERSION "0.49"

