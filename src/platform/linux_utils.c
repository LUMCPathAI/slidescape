/*
  BSD 2-Clause License

  Copyright (c) 2019-2023, Pieter Valkema

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

#include "profiler.h"
#include "common.h"
#include "platform.h"

int platform_stat(const char* filename, struct stat* st) {
	return stat(filename, st);
}

file_stream_t file_stream_open_for_reading(const char* filename) {
	FILE* handle = fopen64(filename, "rb");
	return handle;
}

file_stream_t file_stream_open_for_writing(const char* filename) {
	FILE* handle = fopen64(filename, "wb");
	return handle;
}

i64 file_stream_read(void* dest, size_t bytes_to_read, file_stream_t file_stream) {
	size_t bytes_read = fread(dest, 1, bytes_to_read, file_stream);
	return bytes_read;
}

void file_stream_write(void* source, size_t bytes_to_write, file_stream_t file_stream) {
	size_t ret = fwrite(source, 1, bytes_to_write, file_stream);
}

i64 file_stream_get_filesize(file_stream_t file_stream) {
	struct stat st;
	if (fstat(fileno(file_stream), &st) == 0) {
		i64 filesize = st.st_size;
		return filesize;
	} else {
		return 0;
	}
}

i64 file_stream_get_pos(file_stream_t file_stream) {
	fpos_t prev_read_pos = {0}; // NOTE: fpos_t may be a struct!
	int ret = fgetpos64(file_stream, &prev_read_pos); // for restoring the file position later
	ASSERT(ret == 0); (void)ret;
#ifdef _FPOSOFF
	return _FPOSOFF(prev_read_pos);
#else
	STATIC_ASSERT(sizeof(off_t) == 8);
	// Somehow, it is unclear what is the 'correct' way to convert an fpos_t to a simple integer?
	return *(i64*)(&prev_read_pos);
#endif
}

bool file_stream_set_pos(file_stream_t file_stream, i64 offset) {
	fpos_t pos = {offset};
	int ret = fsetpos64(file_stream, &pos);
	return (ret == 0);
}

void file_stream_close(file_stream_t file_stream) {
	fclose(file_stream);
}

file_handle_t open_file_handle_for_simultaneous_access(const char* filename) {
	file_handle_t fd = open(filename, O_RDONLY);
	if (fd == -1) {
		console_print_error("Error: Could not reopen file for asynchronous I/O\n");
		return 0;
	} else {
		return fd;
	}
}

void file_handle_close(file_handle_t file_handle) {
	if (file_handle) {
		close(file_handle);
	}
}

size_t file_handle_read_at_offset(void* dest, file_handle_t file_handle, u64 offset, size_t bytes_to_read) {
	i64 profile_start = profiler_worker_io_begin();
	size_t bytes_read = pread(file_handle, dest, bytes_to_read, offset);
	profiler_worker_io_end(profile_start, bytes_read == (size_t)-1 ? 0 : bytes_read);
	return bytes_read;
}
