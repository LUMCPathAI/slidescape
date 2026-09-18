/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026 Pieter Valkema
  SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "source_io.h"
#include "platform.h"

#if !WINDOWS
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#endif

typedef struct source_local_t {
#if WINDOWS
	HANDLE handle;
#else
	int fd;
#endif
} source_local_t;

static source_read_result_t source_local_read(void* context, u64 offset, void* dest, size_t size) {
	source_local_t* local = context;
	u8* at = dest;
#if WINDOWS
	/* Buffered overlapped reads support arbitrary ZIP member alignment. Do not use
	   the viewer's unbuffered/aligned I/O path or its thread-local scratch arena. */
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!event) return SOURCE_READ_IO_ERROR;
	bool success = true;
	while (size) {
		DWORD amount = (DWORD)MIN(size, (size_t)0x7fffffff);
		OVERLAPPED operation = {0};
		operation.Offset = (DWORD)offset;
		operation.OffsetHigh = (DWORD)(offset >> 32);
		operation.hEvent = event;
		ResetEvent(event);
		DWORD count = 0;
		BOOL ok = ReadFile(local->handle, at, amount, &count, &operation);
		if (!ok && GetLastError() == ERROR_IO_PENDING)
			ok = GetOverlappedResult(local->handle, &operation, &count, TRUE);
		if (!ok || !count) { success = false; break; }
		size -= count;
		at += count;
		offset += count;
	}
	CloseHandle(event);
	return success ? SOURCE_READ_OK : SOURCE_READ_IO_ERROR;
#else
	while (size) {
		size_t amount = MIN(size, (size_t)0x7fffffff);
		ssize_t count = pread(local->fd, at, amount, (off_t)offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return SOURCE_READ_IO_ERROR;
		size -= (size_t)count;
		at += count;
		offset += (u64)count;
	}
	return SOURCE_READ_OK;
#endif
}

static void source_local_close(void* context) {
	source_local_t* local = context;
#if WINDOWS
	CloseHandle(local->handle);
#else
	close(local->fd);
#endif
	free(local);
}

bool source_blob_open_local(source_blob_t* blob, const char* path) {
	if (!blob || !path) return false;
	*blob = (source_blob_t){0};
	source_local_t* local = calloc(1, sizeof(*local));
	if (!local) return false;
	u64 size = 0;
#if WINDOWS
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
	wchar_t* wide = count > 0 ? malloc((size_t)count * sizeof(*wide)) : NULL;
	if (!wide) { free(local); return false; }
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count);
	local->handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
	free(wide);
	if (local->handle == INVALID_HANDLE_VALUE) { free(local); return false; }
	LARGE_INTEGER length;
	if (!GetFileSizeEx(local->handle, &length) || length.QuadPart < 0) {
		source_local_close(local); return false;
	}
	size = (u64)length.QuadPart;
#else
	local->fd = open(path, O_RDONLY);
	if (local->fd < 0) { free(local); return false; }
	struct stat info;
	if (fstat(local->fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
		source_local_close(local); return false;
	}
	size = (u64)info.st_size;
#endif
	*blob = (source_blob_t){size, local, source_local_read, source_local_close};
	return true;
}

void source_blob_close(source_blob_t* blob) {
	if (!blob) return;
	if (blob->close) blob->close(blob->context);
	*blob = (source_blob_t){0};
}

bool source_file_view(source_blob_t* blob, u64 offset, u64 size, source_file_t* file) {
	if (!file) return false;
	*file = (source_file_t){0};
	if (!blob || !blob->read || offset > blob->size || size > blob->size - offset) return false;
	*file = (source_file_t){blob, offset, size};
	return true;
}

source_read_result_t source_file_read(const source_file_t* file, u64 offset, void* dest, size_t size) {
	if (!file || !file->blob || !file->blob->read || (!dest && size)) return SOURCE_READ_IO_ERROR;
	if (offset > file->size || size > file->size - offset ||
		file->offset > file->blob->size || file->size > file->blob->size - file->offset)
		return SOURCE_READ_OUT_OF_BOUNDS;
	if (!size) return SOURCE_READ_OK;
	return file->blob->read(file->blob->context, file->offset + offset, dest, size);
}
