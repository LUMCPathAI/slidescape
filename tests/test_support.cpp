#include "common.h"

#include <stdarg.h>

extern "C" {

extern bool is_verbose_mode;

void console_print(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void console_print_verbose(const char* fmt, ...) {
	if (!is_verbose_mode) return;
	va_list args;
	va_start(args, fmt);
	vfprintf(stdout, fmt, args);
	va_end(args);
}

void console_print_error(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

}
