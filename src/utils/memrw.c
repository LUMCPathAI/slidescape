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
#include "memrw.h"

void memrw_maybe_grow(memrw_t* buffer, u64 new_size) {
	if (new_size > buffer->capacity) {
        if (buffer->is_growing_disallowed) {
            fatal_error("fixed-capacity buffer is overflowing");
        }
		u64 new_capacity = next_pow2(new_size);
		void* new_ptr = realloc(buffer->data, new_capacity);
		if (!new_ptr) fatal_error();
		buffer->data = new_ptr;
//#if DO_DEBUG
//		console_print_verbose("memrw_maybe_grow(): expanded buffer size from %u to %u\n", buffer->capacity, new_capacity);
//#endif
		buffer->capacity = new_capacity;
	}
}

// TODO: do we actually want this to be a dynamic array, or a read/write stream?
u64 memrw_push_back(memrw_t* buffer, void* data, u64 size) {
	u64 new_size = buffer->used_size + size;
	memrw_maybe_grow(buffer, new_size);
	u64 write_offset = buffer->used_size;
	void* write_pos = buffer->data + write_offset;
	if (data) {
		memcpy(write_pos, data, size);
	} else {
		memset(write_pos, 0, size);
	}
	buffer->used_size += size;
	buffer->cursor = buffer->used_size;
	buffer->used_count += 1;
	return write_offset;
}

void memrw_init(memrw_t* buffer, u64 capacity) {
	memset(buffer, 0, sizeof(*buffer));
	buffer->data = (u8*) malloc(capacity);
    ASSERT(capacity > 0);
    buffer->data[0] = '\0';
	buffer->capacity = capacity;
}

memrw_t memrw_create(u64 capacity) {
	memrw_t result = {0};
	memrw_init(&result, capacity);
	return result;
}

void memrw_rewind(memrw_t* buffer) {
	buffer->used_size = 0;
	buffer->used_count = 0;
	buffer->cursor = 0;
}

void memrw_seek(memrw_t* buffer, i64 offset) {
	if (offset >= 0 && offset < (i64)buffer->used_size) {
		buffer->cursor = offset;
	} else {
		fatal_error();
	};
}

i64 memrw_write(const void* src, memrw_t* buffer, i64 bytes_to_write) {
	ASSERT(bytes_to_write >= 0);
	memrw_maybe_grow(buffer, buffer->cursor + bytes_to_write);
	i64 bytes_left = buffer->capacity - buffer->cursor;
	if (bytes_left >= 1) {
		bytes_to_write = MIN(bytes_to_write, bytes_left);
		memcpy(buffer->data + buffer->cursor, src, bytes_to_write);
		buffer->cursor += bytes_to_write;
		buffer->used_size = MAX(buffer->cursor, (i64)buffer->used_size);
		return bytes_to_write;
	}
	return 0;
}

i64 memrw_putc(i64 c, memrw_t* buffer) {
	return memrw_write(&c, buffer, 1);
}

i64 memrw_write_string(const char* s, memrw_t* buffer) {
	size_t len = strlen(s);
	return memrw_write(s, buffer, len);
}

i64 memrw_write_string_urlencode(const char* s, memrw_t* buffer) {
    size_t len = strlen(s);
    i64 bytes_written = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            bytes_written += memrw_putc(c, buffer);
        } else {
            static const char* hex = "0123456789abcdef";
            char c_encoded[4] = {'%', hex[(c >> 4) & 15], hex[c & 15]};
            bytes_written += memrw_write(c_encoded, buffer, 3);
        }
    }
    return bytes_written;
}

// Push a zero-terminated string onto the buffer, and return the offset in the buffer (for use as a string pool)
i64 memrw_string_pool_push(memrw_t* buffer, const char* s) {
	i64 cursor = buffer->cursor;
	memrw_write_string(s, buffer);
	memrw_putc('\0', buffer);
	return cursor;
}

i64 memrw_printf(memrw_t* buffer, const char* fmt, ...) {
	char buf[4096];
	va_list args;
	va_start(args, fmt);
	i32 ret = vsnprintf(buf, sizeof(buf)-1, fmt, args);
	buf[sizeof(buf)-1] = 0;
	memrw_write_string(buf, buffer);
	va_end(args);
	return ret;
}

i64 memrw_read(void* dest, memrw_t* buffer, size_t bytes_to_read) {
	i64 bytes_left = buffer->used_size - buffer->cursor;
	if (bytes_left >= 1) {
		bytes_to_read = MIN(bytes_to_read, (size_t)bytes_left);
		memcpy(dest, buffer->data + buffer->cursor, bytes_to_read);
		buffer->cursor += bytes_to_read;
		return bytes_to_read;
	}
	return 0;
}

void memrw_destroy(memrw_t* buffer) {
	if (buffer->data) free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
}

