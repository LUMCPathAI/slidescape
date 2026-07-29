/*
  BSD 2-Clause License

  Copyright (c) 2019-2026, Pieter Valkema

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
#include "win32_utils.h"

wchar_t* win32_string_widen(const char* s, size_t len, wchar_t* buffer) {
	int characters_written = MultiByteToWideChar(CP_UTF8, 0, s, -1, buffer, len);
	if (characters_written > 0) {
		int last_character = MIN(characters_written, ((int)len)-1);
		buffer[last_character] = '\0';
	} else {
		win32_diagnostic("MultiByteToWideChar");
		buffer[0] = '\0';
	}
	return buffer;
}

char* win32_string_narrow(wchar_t* s, char* buffer, size_t buffer_size) {
	int bytes_written = WideCharToMultiByte(CP_UTF8, 0, s, -1, buffer, buffer_size, NULL, NULL);
	if (bytes_written > 0) {
		ASSERT(bytes_written < buffer_size);
		int last_byte = MIN(bytes_written, ((int)buffer_size)-1);
		buffer[last_byte] = '\0';
	} else {
		win32_diagnostic("WideCharToMultiByte");
		buffer[0] = '\0';
	}
	return buffer;
}

void win32_diagnostic(const char* prefix) {
	DWORD error_id = GetLastError();
	char* message_buffer;
	/*size_t size = */FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                                 NULL, error_id, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&message_buffer, 0, NULL);
	console_print("%s: (error code 0x%x) %s\n", prefix, (u32)error_id, message_buffer);
	LocalFree(message_buffer);
}

void win32_diagnostic_verbose(const char* prefix) {
	DWORD error_id = GetLastError();
	char* message_buffer;
	/*size_t size = */FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                                 NULL, error_id, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&message_buffer, 0, NULL);
	console_print_verbose("%s: (error code 0x%x) %s\n", prefix, (u32)error_id, message_buffer);
	LocalFree(message_buffer);
}

HANDLE win32_open_overlapped_file_handle(const char* filename) {
	// NOTE: Using the FILE_FLAG_NO_BUFFERING flag *might* be faster, but I am not actually noticing a speed increase,
	// so I am keeping it turned off for now.
	size_t filename_len = strlen(filename) + 1;
	wchar_t* wide_filename = win32_string_widen(filename, filename_len, (wchar_t*) alloca(2 * filename_len));
	HANDLE handle = CreateFileW(wide_filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL | /*FILE_FLAG_SEQUENTIAL_SCAN |*/
	                            /*FILE_FLAG_NO_BUFFERING |*/ FILE_FLAG_OVERLAPPED,
	                            NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		win32_diagnostic("CreateFileW");
	}
	return handle;
}

file_handle_t open_file_handle_for_simultaneous_access(const char* filename) {
	HANDLE handle = win32_open_overlapped_file_handle(filename);
	if (handle == INVALID_HANDLE_VALUE) {
		return 0; //NOTE: can a valid handle ever be 0?
	} else {
		return handle;
	}
}

void file_handle_close(file_handle_t file_handle) {
	if (file_handle) {
		CloseHandle(file_handle);
	}
}

size_t win32_overlapped_read(thread_memory_t* thread_memory, HANDLE file_handle, void* dest, u32 read_size, i64 offset) {
	// We align reads to 4K boundaries, so that file handles can be used with the FILE_FLAG_NO_BUFFERING flag.
	// See: https://docs.microsoft.com/en-us/windows/win32/fileio/file-buffering
	i64 aligned_offset = offset & ~(KILOBYTES(4)-1);
	i64 align_delta = offset - aligned_offset;
	ASSERT(align_delta >= 0);

	i64 end_byte = offset + read_size;
	i64 raw_read_size = end_byte - aligned_offset;
	ASSERT(raw_read_size >= 0);

	i64 bytes_to_read_in_last_sector = raw_read_size % KILOBYTES(4);
	if (bytes_to_read_in_last_sector > 0) {
		raw_read_size += KILOBYTES(4) - bytes_to_read_in_last_sector;
	}

	ASSERT(thread_memory != NULL);
	temp_memory_t temp_memory = begin_temp_memory(&thread_memory->temp_arena);
	i64 bytes_left_in_temp_memory = arena_get_bytes_left(&thread_memory->temp_arena);
	bool need_free_temp_dest;
	u8* temp_dest;
	if (bytes_left_in_temp_memory >= raw_read_size) {
		temp_dest = (u8*)arena_push_size(&thread_memory->temp_arena, raw_read_size);
		need_free_temp_dest = false;
	} else {
		temp_dest = (u8*)malloc(raw_read_size);
		need_free_temp_dest = true;
	}

	// To submit an async I/O request on Win32, we need to fill in an OVERLAPPED structure with the
	// offset in the file where we want to do the read operation
	LARGE_INTEGER offset_ = {.QuadPart = (i64)aligned_offset};
	OVERLAPPED overlapped = {0};
	overlapped.Offset = offset_.LowPart;
	overlapped.OffsetHigh = (DWORD)offset_.HighPart;
	overlapped.hEvent = thread_memory->async_io_events[0];
	ResetEvent(thread_memory->async_io_events[0]); // reset the event to unsignaled state

	if (!ReadFile(file_handle, temp_dest, raw_read_size, NULL, &overlapped)) {
		DWORD error = GetLastError();
		if (error != ERROR_IO_PENDING) {
			win32_diagnostic("ReadFile");
			if (need_free_temp_dest) {
				free(temp_dest);
			}
			release_temp_memory(&temp_memory);
			return 0;
		}
	}

	// Wait for the result of the I/O operation (blocking, because we specify bWait=TRUE)
	DWORD bytes_read = 0;
	if (!GetOverlappedResult(file_handle, &overlapped, &bytes_read, TRUE)) {
		win32_diagnostic("GetOverlappedResult");
	}

	// This should not be strictly necessary, but do it just in case GetOverlappedResult exits early (paranoia)
	// NOTE: this seems to sometimes return an error 0x6 (handle invalid) if used in quick successive calls... (?)
//	if(WaitForSingleObject(overlapped.hEvent, INFINITE) != WAIT_OBJECT_0) {
//		win32_diagnostic("WaitForSingleObject");
//	}

	memcpy(dest, temp_dest + align_delta, read_size);

	if (need_free_temp_dest) {
		free(temp_dest);
	}
	release_temp_memory(&temp_memory);
	return read_size;
}

size_t file_handle_read_at_offset(void* dest, file_handle_t file_handle, u64 offset, size_t bytes_to_read) {
	if (threadlocal_thread_memory == NULL) {
		init_thread_memory(&global_system_info);
	}
	ASSERT(threadlocal_thread_memory != NULL);
	size_t bytes_read = win32_overlapped_read(threadlocal_thread_memory, file_handle, dest, bytes_to_read, offset);
	return bytes_read;
}

int platform_stat(const char* filename, struct stat* st) {
	size_t filename_len = strlen(filename) + 1;
	wchar_t* wide_filename = win32_string_widen(filename, filename_len, (wchar_t*) alloca(2 * filename_len));
	return wstat(wide_filename, st);
}

file_stream_t file_stream_open_for_reading(const char* filename) {
//	console_print_verbose("Attempting CreateFile() for reading...\n");

	size_t filename_len = strlen(filename) + 1;
	wchar_t* wide_filename = win32_string_widen(filename, filename_len, (wchar_t*) alloca(2 * filename_len));
	HANDLE handle = CreateFileW(wide_filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL /* | FILE_FLAG_SEQUENTIAL_SCAN */
		/*| FILE_FLAG_NO_BUFFERING |*/ /* | FILE_FLAG_OVERLAPPED*/,
		                        NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		win32_diagnostic_verbose("CreateFile");
		return 0;
	} else {
		return handle;
	}
}

file_stream_t file_stream_open_for_writing(const char* filename) {
//	console_print_verbose("Attempting CreateFile()...\n");

	size_t filename_len = strlen(filename) + 1;
	wchar_t* wide_filename = win32_string_widen(filename, filename_len, (wchar_t*) alloca(2 * filename_len));
	HANDLE handle = CreateFileW(wide_filename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
	                            FILE_ATTRIBUTE_NORMAL /* | FILE_FLAG_SEQUENTIAL_SCAN */
		/*| FILE_FLAG_NO_BUFFERING |*/ /* | FILE_FLAG_OVERLAPPED*/,
		                        NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		win32_diagnostic("CreateFile");
		return 0;
	} else {
		return handle;
	}
}

i64 file_stream_read(void* dest, size_t bytes_to_read, file_stream_t file_stream) {
	DWORD bytes_read;
//	console_print_verbose("Attempting ReadFile()...\n");
//	OVERLAPPED overlapped = {};
	if (!ReadFile(file_stream, dest, bytes_to_read, &bytes_read, NULL)) {
		win32_diagnostic("ReadFile");
		return 0;
	} else {
//		console_print_verbose("ReadFile(): %d bytes read\n", bytes_read);
		return (i64)bytes_read;
	}
}

void file_stream_write(void* source, size_t bytes_to_write, file_stream_t file_stream) {
	DWORD bytes_written;
	if (!WriteFile(file_stream, source, bytes_to_write, &bytes_written, NULL)) {
		win32_diagnostic("WriteFile");
	}
}

i64 file_stream_get_filesize(file_stream_t file_stream) {
	LARGE_INTEGER filesize = {0};
	if (!GetFileSizeEx(file_stream, &filesize)) {
		win32_diagnostic("GetFileSizeEx");
	}
	return filesize.QuadPart;
}

i64 file_stream_get_pos(file_stream_t file_stream) {
	LARGE_INTEGER file_position = {0};
	LARGE_INTEGER distance_to_move = {0};
	if (!SetFilePointerEx(file_stream, distance_to_move, &file_position, FILE_CURRENT)) {
		win32_diagnostic("SetFilePointerEx");
	}
	return file_position.QuadPart;
}

bool file_stream_set_pos(file_stream_t file_stream, i64 offset) {
	LARGE_INTEGER new_file_pointer = {0};
	new_file_pointer.QuadPart = offset;
	if (!SetFilePointerEx(file_stream, new_file_pointer, NULL, FILE_BEGIN)) {
		win32_diagnostic("SetFilePointerEx");
		return false;
	} else {
		return true;
	}
}

void file_stream_close(file_stream_t file_stream) {
	if (!CloseHandle(file_stream)) {
		win32_diagnostic("CloseHandle");
	}
}

