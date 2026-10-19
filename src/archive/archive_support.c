#define STB_SPRINTF_IMPLEMENTATION
#define SHEREDOM_JSON_IMPLEMENTATION
#include "common.h"
#include "json.h"

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
