/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2023  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "platform.h" // for get_clock() and get_seconds_elapsed()

#define OPENSLIDE_API_IMPL
#include "openslide_api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef _WIN32
HINSTANCE win32_load_library_with_possible_filenames(const wchar_t** filenames, i32 filename_count) {
    HINSTANCE instance = NULL;
    for (i32 i = 0; i < filename_count; ++i) {
        const wchar_t* filename = filenames[i];
        instance = LoadLibraryW(filename);
		if (instance) {
			return instance;
		}
    }
    return instance;
}

const wchar_t* one_past_last_slash_w(const wchar_t* s, i32 max) {
    if (max <= 0) return s;
    size_t len = wcsnlen(s, (size_t)(max - 1));
    size_t stripped_len = 0;
    const wchar_t* pos = s + len - 1;
    for (; pos >= s; --pos) {
        wchar_t c = *pos;
        if (c == '/' || c == '\\')  {
            break;
        } else {
            ++stripped_len;
        }
    }
    const wchar_t* result = pos + 1; // gone back one too far
    ASSERT(stripped_len > 0 && stripped_len <= len);
    return result;
}

#endif

bool init_openslide() {
	i64 debug_start = get_clock();

#ifdef _WIN32
	// Look for DLLs in an openslide/ folder in the same location as the .exe
	wchar_t dll_path[4096];
	GetModuleFileNameW(NULL, dll_path, sizeof(dll_path));
    wchar_t* pos = (wchar_t*)one_past_last_slash_w(dll_path, sizeof(dll_path));
	i32 chars_left = COUNT(dll_path) - (pos - dll_path);
	wcsncpy(pos, L"openslide", chars_left);
	SetDllDirectoryW(dll_path);

    const wchar_t* dll_filenames[] = { L"libopenslide-1.dll", L"libopenslide-0.dll" };

	HINSTANCE library_handle = win32_load_library_with_possible_filenames(dll_filenames, COUNT(dll_filenames));
	if (!library_handle) {
		// If DLL not found in the openslide/ folder, look one folder up (in the same location as the .exe)
		*pos = '\0';
		SetDllDirectoryW(dll_path);
		library_handle = win32_load_library_with_possible_filenames(dll_filenames, COUNT(dll_filenames));
	}
	SetDllDirectoryW(NULL);

#elif defined(__APPLE__)
	void* library_handle = dlopen("@executable_path/libopenslide.dylib", RTLD_LAZY);
	if (!library_handle) {
		library_handle = dlopen("libopenslide.dylib", RTLD_LAZY);
		if (!library_handle) {
			// Check expected library path for MacPorts
			library_handle = dlopen("/opt/local/lib/libopenslide.dylib", RTLD_LAZY);
			if (!library_handle) {
				// Check expected library path for Homebrew
				// The path differs for Apple Silicon and Intel. We'll check the Apple Silicon prefix first,
				// and fall back on the Intel prefix if that fails.
				library_handle = dlopen("/opt/homebrew/opt/openslide/lib/libopenslide.dylib", RTLD_LAZY);
				if (!library_handle) {
					// Check expected library path for Homebrew (Intel)
					library_handle = dlopen("/usr/local/opt/openslide/lib/libopenslide.dylib", RTLD_LAZY);
				}
			}
		}
	}
#else
	void* library_handle = dlopen("libopenslide.so", RTLD_LAZY);
	if (!library_handle) {
		library_handle = dlopen("/usr/local/lib/libopenslide.so", RTLD_LAZY);
	}
#endif

	bool success = false;
	if (library_handle) {

#ifdef _WIN32
#define GET_PROC(proc) if (!(openslide.proc = (void*) GetProcAddress(library_handle, "openslide_" #proc))) goto failed;
#else
#define GET_PROC(proc) if (!(openslide.proc = (void*) dlsym(library_handle, "openslide_" #proc))) goto failed;
#endif
		GET_PROC(detect_vendor);
		GET_PROC(open);
		GET_PROC(get_level_count);
		GET_PROC(get_level0_dimensions);
		GET_PROC(get_level_dimensions);
		GET_PROC(get_level_downsample);
		GET_PROC(get_best_level_for_downsample);
		GET_PROC(read_region);
		GET_PROC(close);
		GET_PROC(get_error);
		GET_PROC(get_property_names);
		GET_PROC(get_property_value);
		GET_PROC(get_associated_image_names);
		GET_PROC(get_associated_image_dimensions);
		GET_PROC(read_associated_image);
		GET_PROC(get_version);
#undef GET_PROC

		float seconds = get_seconds_elapsed(debug_start, get_clock());
		if (seconds > 0.1f) {
			console_print("OpenSlide initialized (loading took %g seconds)\n", seconds);
		} else {
			console_print("OpenSlide initialized\n");
		}
		success = true;

	} else failed: {
#ifdef _WIN32
		//win32_diagnostic("LoadLibraryA");
		console_print("OpenSlide not available: could not load libopenslide-1.dll or libopenslide-0.dll\n");
#elif defined(__APPLE__)
		console_print("OpenSlide not available: could not load libopenslide.dylib (not installed?)\n");
#else
		console_print("OpenSlide not available: could not load libopenslide.so (not installed?)\n");
#endif
		success = false;
	}
	return success;
}
