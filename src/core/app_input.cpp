/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

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
#include "viewer.h"
#include "slide_score.h"
#include "dicomweb_client.h"
#include "ssi.h"
#include "gui.h"
#include "stringutils.h"

#if APPLE
#include <mach-o/dyld.h> // for _NSGetExecutablePath
#endif

static bool file_uri_to_local_path(const char* uri, char* out_path, size_t out_path_size) {
	static const char file_scheme[] = "file://";
	if (strncmp(uri, file_scheme, strlen(file_scheme)) != 0) return false;

	const char* src = uri + strlen(file_scheme);
	if (strncmp(src, "localhost/", 10) == 0) {
		src += strlen("localhost");
	}

	return uri_percent_decode(src, out_path, out_path_size) > 0;
}

static bool app_make_executable_relative_filename(const char* leaf_filename, char* buffer, size_t buffer_size) {
	char exe_path[1024] = "";
#if APPLE
	u32 exe_path_size = sizeof(exe_path);
	if (_NSGetExecutablePath(exe_path, &exe_path_size) != 0) {
		exe_path[0] = 0;
	}
#endif
	const char* exe = exe_path;
	if (!exe[0]) {
		if (!g_argv || !g_argv[0] || !g_argv[0][0]) return false;
		exe = g_argv[0];
	}

	const char* filename = one_past_last_slash(exe, (i32)strlen(exe) + 1);
	if (filename <= exe) return false;

	size_t dir_len = (size_t)(filename - exe);
	size_t leaf_len = strlen(leaf_filename);
	if (dir_len + leaf_len + 1 > buffer_size) return false;
	memcpy(buffer, exe, dir_len);
	memcpy(buffer + dir_len, leaf_filename, leaf_len + 1);
	return true;
}

bool app_get_slide_score_api_key_filename(char* buffer, size_t buffer_size) {
	static const char leaf_filename[] = "api_key.txt";
	if (global_settings_dir) {
		snprintf(buffer, buffer_size, "%s" PATH_SEP "%s", global_settings_dir, leaf_filename);
		return true;
	}

	if (app_make_executable_relative_filename(leaf_filename, buffer, buffer_size)) {
		return true;
	}
	snprintf(buffer, buffer_size, "%s", leaf_filename);
	return true;
}

static bool app_read_slide_score_api_key_from_file(const char* filename, char* buffer, size_t buffer_size) {
	mem_t* key_file = platform_read_entire_file(filename);
	if (key_file) {
		size_t key_len = ATMOST(key_file->len, buffer_size - 1);
		memcpy(buffer, key_file->data, key_len);
		buffer[key_len] = 0;
		free(key_file);
		return true;
	}
	return false;
}

void app_read_slide_score_api_key(char* buffer, size_t buffer_size) {
	static const char leaf_filename[] = "api_key.txt";
	buffer[0] = 0;
	char key_filename[1024];
	app_get_slide_score_api_key_filename(key_filename, sizeof(key_filename));
	if (app_read_slide_score_api_key_from_file(key_filename, buffer, buffer_size)) {
		return;
	}

	if (file_exists(leaf_filename) && app_read_slide_score_api_key_from_file(leaf_filename, buffer, buffer_size)) {
		return;
	}

	char executable_relative[1024];
	if (app_make_executable_relative_filename(leaf_filename, executable_relative, sizeof(executable_relative))) {
		app_read_slide_score_api_key_from_file(executable_relative, buffer, buffer_size);
	}
}

bool app_write_slide_score_api_key(const char* api_key) {
	char key_filename[1024];
	app_get_slide_score_api_key_filename(key_filename, sizeof(key_filename));
	file_stream_t fp = file_stream_open_for_writing(key_filename);
	if (fp) {
		file_stream_write((void*)api_key, strlen(api_key), fp);
		file_stream_close(fp);
		return true;
	}
	return false;
}

bool app_load_input(app_state_t* app_state, const char* input, u32 filetype_hint) {
	if (filetype_hint == 0 && ssi_uri_is_supported(input)) {
		if (ssi_try_open_uri(app_state, input)) return true;
		if (!app_state->headless) {
			gui_prompt_for_ssi_credentials(input, ssi_get_last_status());
			return true;
		}
		return false;
	}
	if (filetype_hint == 0 && (strncmp(input, "https://", 8) == 0 ||
	                           strncmp(input, "http://", 7) == 0) &&
	    strstr(input, "/studies/") && strstr(input, "/series/")) {
		const char* username = app_state->command.dicomweb_username;
		const char* password = app_state->command.dicomweb_password;
		if (!username) username = getenv("SLIDESCAPE_DICOMWEB_USERNAME");
		if (!password) password = getenv("SLIDESCAPE_DICOMWEB_PASSWORD");
		return dicomweb_open_series(app_state, input, username, password);
	}
	if (filetype_hint == 0 && (strncmp(input, "https://", 8) == 0 ||
	                           strncmp(input, "http://", 7) == 0) &&
	    strstr(input, "/dicom-web")) {
		copy_cstring(remote_uri, input, sizeof(remote_uri));
		show_open_uri_window = true;
		return true;
	}
	if (filetype_hint == 0 && slide_score_uri_is_supported(input)) {
		char api_key[4096];
		app_read_slide_score_api_key(api_key, sizeof(api_key));
		return slide_score_try_open_uri(app_state, input, api_key);
	}

	char local_path[4096];
	if (file_uri_to_local_path(input, local_path, sizeof(local_path))) {
		return load_generic_file(app_state, local_path, filetype_hint);
	}
	return load_generic_file(app_state, input, filetype_hint);
}
