#define STB_SPRINTF_IMPLEMENTATION
#define SHEREDOM_JSON_IMPLEMENTATION
#include "common.h"
#include "json.h"
#include "remote.h"

#include <stdarg.h>

#undef console_print
#undef console_print_verbose
#undef console_print_error

/* The archive reuses nongui parsers but intentionally does not link the GUI console. */
void console_print(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stdout, format, args);
	va_end(args);
}

void console_print_verbose(const char* format, ...) {
	if (!is_verbose_mode) return;
	va_list args;
	va_start(args, format);
	vfprintf(stdout, format, args);
	va_end(args);
}

void console_print_error(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

/* Native archive adapters never perform TIFF HTTP range reads. This satisfies the
 * nongui image-library link contract without admitting remote source locators. */
u8* download_remote_chunk(const char* hostname, i32 portno, const char* filename,
		i64 chunk_offset, i64 chunk_size, i32* downloaded_size,
		i32 timeout_seconds) {
	(void)hostname;
	(void)portno;
	(void)filename;
	(void)chunk_offset;
	(void)chunk_size;
	(void)downloaded_size;
	(void)timeout_seconds;
	return NULL;
}
