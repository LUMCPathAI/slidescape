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

#include "common.h"
#include "stringutils.h"

#include <ctype.h>

void strip_character(char* s, char character_to_strip) {
	if (!s) return;
	char c;
	while ((c = *s)) {
		if (c == character_to_strip) *s = '\0';
		++s;
	}
}

char* trim_whitespace(char* s) {
	if (!s) return NULL;
	while (*s && isspace((unsigned char)*s)) ++s;
	char* end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) --end;
	*end = '\0';
	return s;
}

char* find_next_token(const char* s, char separator) {
	if (!s) return NULL;
	char c;
	while ((c = *s++)) {
		if (c == separator) return (char*)s;
	}
	return NULL;
}

// NOTE: this serves as an alternative to strncpy: copies up to dest_size-1 and always null-terminates
size_t copy_cstring(char* dest, const char* src, size_t dest_size) {
	size_t len = 0;

	if (!src) {
		if (dest_size > 0) dest[0] = '\0';
		return 0;
	}

	if (dest_size > 0) {
		for (; len + 1 < dest_size && src[len]; ++len) {
			dest[len] = src[len];
		}
		dest[len] = '\0';
	}

	while (src[len]) {
		++len;
	}

	return len; // source length; truncation if return >= dest_size
}

void dots_to_underscores(char* s, i32 max) {
	for (char* pos = s; pos < s + max; ++pos) {
		char c = *pos;
		if (c == '\0') break;
		if (c == '.') *pos = '_';
	}
}

const char* one_past_last_slash(const char* s, i32 max) {
	i32 len = (i32)strnlen(s, max - 1);
	i32 stripped_len = 0;
	const char* pos = s + len - 1;
	for (; pos >= s; --pos) {
		char c = *pos;
		if (c == '/' || c == '\\')  {
			break;
		} else {
			++stripped_len;
		}
	}
	const char* result = pos + 1; // gone back one too far
	ASSERT(stripped_len >= 0 && stripped_len <= len);
	return result;
}

const char* get_file_extension(const char* filename) {
	size_t len = strlen(filename);
	const char* end = filename + len;
	for (const char* pos = end - 1; pos >= filename; --pos) {
		if (*pos == '.') {
			return pos + 1;
		}
		if (*pos == '/' || *pos == '\\') {
			break;
		}
	}
	return end; // no extension
}

void replace_file_extension(char* filename, i32 max_len, const char* new_ext) {
	size_t new_ext_len = strlen(new_ext);
	size_t original_len = strlen(filename);
	char* end = filename + original_len;
	char* append_pos = end; // where we will add the new extension
	bool found_period = false;
	// Strip the original extension
	for (char* pos = end; pos > filename;) {
		--pos;
		if (*pos == '/' || *pos == '\\') {
			// gone too far, default to end of original filename
			break;
		}
		if (*pos == '.') {
			found_period = true;
			if (new_ext_len == 0) {
				*pos = '\0'; // done: only strip extension
				return;
			} else {
				append_pos = pos + 1;
				break;
			}
		}
	}
	char* buffer_end = filename + max_len;
	if (!found_period) {
		if (new_ext_len == 0) return;
		// Add the missing period
		if (append_pos < buffer_end) {
			*append_pos++ = '.';
		}
	}
	// Now append the new extension
	const char* new_ext_pos = new_ext;
	for (i64 append_len = (buffer_end - append_pos); append_len > 0; --append_len) {
		*append_pos = *new_ext_pos;
		if (*new_ext_pos == '\0') break;
		++append_pos;
		++new_ext_pos;
	}
}

char** split_into_lines(char* buffer, size_t* num_lines) {
	size_t lines_counted = 0;
	size_t capacity = 0;
	char** lines = NULL;
	bool32 newline = true;
	char* pos = buffer;
	int c;
	do {
		c = *pos;
		if (c == '\n' || c == '\r') {
			*pos = '\0';
			newline = true;
		} else if (newline) {
			size_t line_index = lines_counted++;
			if (lines_counted > capacity) {
				capacity = MAX(capacity, 8) * 2;
				lines = (char**) realloc(lines, capacity * sizeof(char*));
			}
			lines[line_index] = pos;
			newline = false;
		}
		++pos;
	} while (c != '\0');
	if (num_lines) *num_lines = lines_counted;
	return lines;
}

size_t count_lines(char* buffer) {
	size_t lines_counted = 0;
	bool32 newline = true;
	char* pos = buffer;
	int c;
	do {
		c = *pos;
		if (c == '\n' || c == '\r') {
			newline = true;
		} else if (newline) {
			lines_counted++;
			newline = false;
		}
		++pos;
	} while (c != '\0');
	return lines_counted;
}

bool hex_digit_to_value(char c, u8* out_value) {
	if (c >= '0' && c <= '9') {
		*out_value = (u8)(c - '0');
		return true;
	} else if (c >= 'a' && c <= 'f') {
		*out_value = (u8)(c - 'a' + 10);
		return true;
	} else if (c >= 'A' && c <= 'F') {
		*out_value = (u8)(c - 'A' + 10);
		return true;
	}
	return false;
}

size_t uri_percent_decode(const char* src, char* dest, size_t dest_size) {
	size_t out_len = 0;
	if (dest_size == 0) return 0;
	while (*src && out_len + 1 < dest_size) {
		if (*src == '%' && src[1] && src[2]) {
			u8 high = 0, low = 0;
			if (hex_digit_to_value(src[1], &high) && hex_digit_to_value(src[2], &low)) {
				dest[out_len++] = (char)((high << 4) | low);
				src += 3;
				continue;
			}
		}
		dest[out_len++] = *src++;
	}
	dest[out_len] = 0;
	return out_len;
}
