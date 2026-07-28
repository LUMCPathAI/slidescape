/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

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
#include "dicomweb_client.h"
#include "platform.h"
#include "stringutils.h"
#include "gui.h"

#if COMPILER_MSVC
#include <direct.h>
#endif

struct console_log_item_t {
	i64 text_offset_in_string_pool;
	bool has_color;
	u32 item_type;
};

static console_log_item_t* console_log_items; //sb
static memrw_t console_string_pool;


void console_clear_log() {
	platform_mutex_lock(&console_printer_mutex);
	memrw_rewind(&console_string_pool);
	arrfree(console_log_items);
	console_log_items = NULL;
	platform_mutex_unlock(&console_printer_mutex);
}

bool console_fill_screen = false;
float console_fraction_of_height = 0.3f;

void console_execute_command(app_state_t* app_state, const char* command) {

	size_t command_len = strlen(command);
	if (command_len > 0) {
		char* command_copy = (char*)alloca(command_len + 1);
		memcpy(command_copy, command, command_len);
		command_copy[command_len] = '\0';

		char* cmd_end = find_next_token(command_copy, ' ');
		char* arg = NULL;
		if (cmd_end) {
			*(cmd_end - 1) = '\0'; // zero-terminate the command
			// Parse an argument
			bool within_quotes = false;
			bool arg_starts_with_quote = false;
			char* arg_parse_start = cmd_end;
			char* arg_end = NULL;
			for(char* s = arg_parse_start; *s != '\0'; ++s) {
				if (!within_quotes && (*s == ' ' || *s == '\t')) {
					if (arg != NULL) {
						// reached end of arg
						*s = '\0';
						if (arg_starts_with_quote && s > arg_parse_start) {
							char* prev = s - 1;
							if (*prev == '\"') *prev = '\0'; // strip closing quote off the arg
							arg_end = s + 1;
							break;
						}
					}
					continue; // skip whitespace
				}
				if (*s == '\"') {
					if (!within_quotes) {
						within_quotes = true;
						if (arg == NULL) {
							arg = s + 1;
							arg_starts_with_quote = true;
						}
					} else {
						within_quotes = false;
						char* next = s + 1;
						if (*next == '\0' && arg != NULL) {
							// terminate the command
							*s = '\0'; // strip closing quote off the arg
							break;
						}
					}
					continue;
				}
				if (arg == NULL) {
					arg = s;
				}
			}
		}
		char* cmd = command_copy;

		// Parse arguments
		if (strcmp(cmd, "exit") == 0) {
			is_program_running = false;
		} else if (strcmp(cmd, "open") == 0) {
			// TODO: queue up file loads, load them at an appropriate time
			if (arg) {
				u32 filetype_hint = load_next_image_as_overlay ? FILETYPE_HINT_OVERLAY : 0;
				load_generic_file(app_state, arg, filetype_hint);
			}
		} else if (strcmp(cmd, "close") == 0) {
			menu_close_file(app_state);
		} else if (strcmp(cmd, "zoom") == 0) {
			if (arg) {
				float new_zoom_level = strtof(arg, NULL);
				app_state->scene.zoom.pos = new_zoom_level;
				zoom_update_pos(&app_state->scene.zoom, app_state->scene.zoom.pos);
			} else {
				app_state->scene.need_zoom_reset = true;
			}
		} else if (strcmp(cmd, "cd") == 0) {
			if (arg) {
				chdir(arg);
				char buf[2048];
				if (getcwd(buf, sizeof(buf))) {
					console_print("%s\n", buf);
				}
			}
		} else if (strcmp(cmd, "pwd") == 0) {
			char buf[2048];
			if (getcwd(buf, sizeof(buf))) {
				console_print("%s\n", buf);
			}
		} else if (strcmp(cmd, "next") == 0) {
			// TODO: load the next file in the folder
		} else if (strcmp(cmd, "prev") == 0) {
			// TODO: load the previous file in the folder
		} else if (strcmp(cmd, "conheight") == 0) {
			if (arg) {
				float new_height = strtof(arg, NULL);
				console_fraction_of_height = new_height;
				console_fill_screen = (console_fraction_of_height >= 1.0f);
			}
		} else if (strcmp(cmd, "clear") == 0) {
			console_clear_log();
		} else if (strcmp(cmd, "dicomweb-stats") == 0) {
			dicomweb_print_tile_stats();
		} else if (strcmp(cmd, "wsi") == 0) {
			app_state->scene.draw_wsi_image = !app_state->scene.draw_wsi_image;
			if (app_state->scene.draw_wsi_image) {
				console_print("Enabled wsi image display");
			} else {
				console_print("Disabled wsi image display");
			}
		} else if (strcmp(cmd, "wsi-background") == 0) {
			app_state->scene.draw_wsi_background = !app_state->scene.draw_wsi_background;
			if (app_state->scene.draw_wsi_background) {
				console_print("Enabled wsi background display");
			} else {
				console_print("Disabled wsi background display");
			}
		} else if (strcmp(cmd, "macro") == 0) {
			app_state->scene.draw_macro_image = !app_state->scene.draw_macro_image;
			if (app_state->scene.draw_macro_image) {
				console_print("Enabled macro image display");
			} else {
				console_print("Disabled macro image display");
			}
		} else if (strcmp(cmd, "label") == 0) {
			app_state->scene.draw_label_image = !app_state->scene.draw_label_image;
			if (app_state->scene.draw_label_image) {
				console_print("Enabled label image display");
			} else {
				console_print("Disabled label image display");
			}
		} else if (strcmp(cmd, "grid") == 0) {
			app_state->scene.enable_grid = !app_state->scene.enable_grid;
			if (app_state->scene.enable_grid) {
				console_print("Enabled grid");
			} else {
				console_print("Disabled grid");
			}
		} else if (strcmp(cmd, "scalebar") == 0) {
			app_state->scene.scale_bar.enabled = !app_state->scene.scale_bar.enabled;
			if (app_state->scene.scale_bar.enabled) {
				console_print("Enabled scale bar");
			} else {
				console_print("Disabled scale bar");
			}
		} else if (strcmp(cmd, "vsync") == 0) {
			if (arg) {
				if (arg[0] == '0') is_vsync_enabled = 0;
				if (arg[0] == '1') is_vsync_enabled = 1;
				set_swap_interval(is_vsync_enabled ? 1 : 0);
			} else {
				console_print("vsync: %d\n", is_vsync_enabled);
			}
		} else if (strcmp(cmd, "progress_bar") == 0) {
			global_progress_bar_test_progress = 0.0f;
			gui_add_modal_progress_bar_popup("Testing progress bar...", &global_progress_bar_test_progress, false);
			gui_add_modal_message_popup("Success", "All done!");
			extern void begin_a_very_long_task();
			begin_a_very_long_task();
		} else if (strcmp(cmd, "modal") == 0) {
			gui_add_modal_message_popup("Modal test", "This is a modal message test.");
		} else if (strcmp(cmd, "tiff_save_description") == 0) {
			if (arrlen(app_state->loaded_images) > 0) {
				image_t* image = app_state->loaded_images[0];
				if (image->backend == IMAGE_BACKEND_TIFF) {
					tiff_ifd_t* ifd = image->tiff.main_image_ifd;
					if (ifd && ifd->image_description != NULL) {
						const char* filename;
						char filename_buf[512];
						if (arg) {
							filename = arg;
						} else {
							snprintf(filename_buf, sizeof(filename_buf), "%s%s.description.txt", get_active_directory(app_state), image->name);
							filename_buf[sizeof(filename_buf)-1] = '\0';
							filename = filename_buf;
						}
						FILE* out = fopen(filename, "wb");
						fwrite(ifd->image_description, ifd->image_description_length, 1, out);
						fclose(out);
						console_print("Saved TIFF ImageDescription tag to '%s'\n", filename);
					} else {
						console_print("TIFF file has no ImageDescription to output\n");
					}
				} else {
					console_print("No TIFF file loaded\n");
				}
			} else {
					console_print("No image loaded\n");
			}
		} else {
			console_print("Unknown command: %s\n", cmd);
		}
	}

}

/*static int TextEditCallbackStub(ImGuiInputTextCallbackData* data)
{
	ExampleAppConsole* console = (ExampleAppConsole*)data->UserData;
	return console->TextEditCallback(data);
}*/

#if 0
int     TextEditCallback(ImGuiInputTextCallbackData* data)
{
	//AddLog("cursor: %d, selection: %d-%d", data->CursorPos, data->SelectionStart, data->SelectionEnd);
	switch (data->EventFlag)
	{
		case ImGuiInputTextFlags_CallbackCompletion:
		{
			// Example of TEXT COMPLETION

			// Locate beginning of current word
			const char* word_end = data->Buf + data->CursorPos;
			const char* word_start = word_end;
			while (word_start > data->Buf)
			{
				const char c = word_start[-1];
				if (c == ' ' || c == '\t' || c == ',' || c == ';')
					break;
				word_start--;
			}

			// Build a list of candidates
			ImVector<const char*> candidates;
			for (int i = 0; i < Commands.Size; i++)
				if (Strnicmp(Commands[i], word_start, (int)(word_end - word_start)) == 0)
					candidates.push_back(Commands[i]);

			if (candidates.Size == 0)
			{
				// No match
				AddLog("No match for \"%.*s\"!\n", (int)(word_end - word_start), word_start);
			}
			else if (candidates.Size == 1)
			{
				// Single match. Delete the beginning of the word and replace it entirely so we've got nice casing.
				data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
				data->InsertChars(data->CursorPos, candidates[0]);
				data->InsertChars(data->CursorPos, " ");
			}
			else
			{
				// Multiple matches. Complete as much as we can..
				// So inputing "C"+Tab will complete to "CL" then display "CLEAR" and "CLASSIFY" as matches.
				int match_len = (int)(word_end - word_start);
				for (;;)
				{
					int c = 0;
					bool all_candidates_matches = true;
					for (int i = 0; i < candidates.Size && all_candidates_matches; i++)
						if (i == 0)
							c = toupper(candidates[i][match_len]);
						else if (c == 0 || c != toupper(candidates[i][match_len]))
							all_candidates_matches = false;
					if (!all_candidates_matches)
						break;
					match_len++;
				}

				if (match_len > 0)
				{
					data->DeleteChars((int)(word_start - data->Buf), (int)(word_end - word_start));
					data->InsertChars(data->CursorPos, candidates[0], candidates[0] + match_len);
				}

				// List matches
				AddLog("Possible matches:\n");
				for (int i = 0; i < candidates.Size; i++)
					AddLog("- %s\n", candidates[i]);
			}

			break;
		}
		case ImGuiInputTextFlags_CallbackHistory:
		{
			// Example of HISTORY
			const int prev_history_pos = HistoryPos;
			if (data->EventKey == ImGuiKey_UpArrow)
			{
				if (HistoryPos == -1)
					HistoryPos = History.Size - 1;
				else if (HistoryPos > 0)
					HistoryPos--;
			}
			else if (data->EventKey == ImGuiKey_DownArrow)
			{
				if (HistoryPos != -1)
					if (++HistoryPos >= History.Size)
						HistoryPos = -1;
			}

			// A better implementation would preserve the data on the current input line along with cursor position.
			if (prev_history_pos != HistoryPos)
			{
				const char* history_str = (HistoryPos >= 0) ? History[HistoryPos] : "";
				data->DeleteChars(0, data->BufTextLen);
				data->InsertChars(0, history_str);
			}
		}
	}
	return 0;
}
#endif


void draw_console_window(app_state_t* app_state, const char* window_title, bool* p_open) {

	// Workaround for modal popups being dismissed if the console is (re)activated while the modal dialog is visible.
	// (Problem: modal dialogs disappear if ImGui::Begin() is called while they are still visible, ONLY if the window wasn't active the previous frame.)
	// Solution/workaround: if there are modals, we only allow the console to be drawn if it was active already.
	bool is_any_modal_open = ImGui::GetTopMostPopupModal() != NULL || arrlen(gui_modal_stack) > 0;
	if (is_any_modal_open) {
		ImGuiWindow* window =ImGui::FindWindowByName(window_title);
		if (window) {
			if (!window->WasActive) {
				return;
			}
		} else {
			return;
		}
	}

	float desired_fraction_of_height = console_fraction_of_height;
	if (console_fill_screen) {
		desired_fraction_of_height = 1.0f;
	}

	if (console_fraction_of_height >= 1.0f) {
		console_fill_screen = true;
		console_fraction_of_height = 0.3f; // set back to default
	}

	rect2i viewport = app_state->client_viewport;
	viewport.x *= app_state->display_points_per_pixel;
	viewport.y *= app_state->display_points_per_pixel;
	viewport.w *= app_state->display_points_per_pixel;
	viewport.h *= app_state->display_points_per_pixel;

	float desired_width = (float) viewport.w;
	float desired_height = roundf((float)viewport.h * desired_fraction_of_height);
	if (show_menu_bar) {
		float vertical_space_left = viewport.h - desired_height;
		float need_space = 23.0f;
		if (vertical_space_left < need_space) {
			desired_height = (float)viewport.h - need_space;
		}
	}

	// Reserve enough left-over height for 1 separator + 1 input text
	const float footer_height_to_reserve = /*ImGui::GetStyle().ItemSpacing.y + */ImGui::GetFrameHeight();

	bool show_only_input_bar = false;
	if (desired_height < footer_height_to_reserve * 2) {
		desired_height = ImGui::GetTextLineHeightWithSpacing();
		show_only_input_bar = true;
	}

	ImGui::SetNextWindowSize(ImVec2(desired_width, desired_height), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(0,viewport.h - desired_height), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.8f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2());
	if (!ImGui::Begin(window_title, p_open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	ImGui::PopStyleVar(4);


	if (!show_only_input_bar) {
		ImGui::BeginChild("ScrollingRegion", ImVec2(0, -footer_height_to_reserve), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
		if (ImGui::BeginPopupContextWindow())
		{
			if (ImGui::Selectable("Clear")) console_clear_log();
			if (ImGui::MenuItem("Verbose mode", NULL, &is_verbose_mode)) {}
			if (ImGui::MenuItem("Fill screen", NULL, &console_fill_screen)) {}
			if (ImGui::MenuItem("Hide console")) { show_console_window = false; }
			ImGui::EndPopup();
		}
		platform_mutex_lock(&console_printer_mutex);
		i32 item_count = arrlen(console_log_items);
		if (item_count > 0) {
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing
			ImGui::PushFont(global_fixed_width_font, global_fixed_width_font->LegacySize);
			ImGuiListClipper clipper;
			clipper.Begin(arrlen(console_log_items));
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
					console_log_item_t item = console_log_items[i];
					const char* text = (char*)console_string_pool.data + item.text_offset_in_string_pool;
//				    if (!Filter.PassFilter(item))
//					    continue;

					// Normally you would store more information in your item than just a string.
					// (e.g. make Items[] an array of structure, store color/type etc.)
					ImVec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
					if (item.has_color) {
						if (item.item_type == 1)      { color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);}
						else if (item.item_type == 2) { color = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);}
						else if (strncmp(text, "# ", 2) == 0) { color = ImVec4(1.0f, 0.8f, 0.6f, 1.0f);  }
						ImGui::PushStyleColor(ImGuiCol_Text, color);
					}
					ImGui::TextUnformatted(text);
					if (item.has_color) {
						ImGui::PopStyleColor();
					}
				}
			}
			ImGui::PopFont();
			ImGui::PopStyleVar();
		}
		platform_mutex_unlock(&console_printer_mutex);
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(0.0f);

		ImGui::EndChild();
	}

//	ImGui::Separator();

	// Command-line
//	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, (ImVec2){0, 0});
//	ImGui::PushClipRect(ImVec2(0, ImGui::GetCursorPosY()), )
//	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer_height_to_reserve);
	ImGui::BeginChild("CommandRegion", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
	if (ImGui::BeginPopupContextWindow()) {
		if (ImGui::Selectable("Clear")) console_clear_log();
		if (ImGui::MenuItem("Verbose mode", NULL, &is_verbose_mode)) {}
		if (ImGui::MenuItem("Fill screen", NULL, &console_fill_screen)) {}
		ImGui::EndPopup();
	}
//	ImGui::PopStyleVar(1);

	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0.3f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0,0,0,1.0f));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0,0,0,1.0f));
	ImGui::PushFont(global_fixed_width_font, global_fixed_width_font->LegacySize);

	bool reclaim_focus = false;
	if (ImGui::IsWindowAppearing()) {
		reclaim_focus = true;
	}
	static char input_buf[2048] = {};
	ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
	ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue /*| ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory*/;

//	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 3.0f));
	if (ImGui::InputText("##console_command_input", input_buf, IM_ARRAYSIZE(input_buf), input_text_flags, /*&TextEditCallbackStub*/NULL, /*(void*)this*/NULL))
	{
		char* s = input_buf;
		if (s[0])
			console_execute_command(app_state, s);
		strcpy(s, "");
		reclaim_focus = true;
	}
//	ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
	ImGui::PopItemWidth();

	// Auto-focus on window apparition
	ImGui::SetItemDefaultFocus();
	if (reclaim_focus) {
		ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget
	}

//	ImGui::SameLine();
//	ImGui::SetCursorPosX(0);
//	ImGui::TextUnformatted(">");

	ImGui::PopFont();
	ImGui::PopStyleColor(3);

	ImGui::EndChild();

	ImGui::End();

}


void console_split_lines_and_add_log_item(char* raw, bool has_color, u32 item_type) {
	if (!console_string_pool.data) {
		memrw_init(&console_string_pool, KILOBYTES(64));
	}
	size_t num_lines = 0;
	char** lines = split_into_lines(raw, &num_lines);
	if (lines) {
		for (i32 i = 0; i < num_lines; ++i) {
			char* line = lines[i];
			size_t line_len = strlen(line);
			if (line && line_len > 0) {
				console_log_item_t new_item = {};
				platform_mutex_lock(&console_printer_mutex);
				new_item.text_offset_in_string_pool = memrw_string_pool_push(&console_string_pool, line);
				new_item.has_color = has_color;
				new_item.item_type = item_type;
				arrput(console_log_items, new_item);
				platform_mutex_unlock(&console_printer_mutex);
			}
		}
		free(lines);
	}
}

void console_print(const char* fmt, ...) {

	char buf[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf)-1, fmt, args);
	buf[sizeof(buf)-1] = 0;
	fprintf(stdout, "%s", buf);
	va_end(args);

	console_split_lines_and_add_log_item(buf, false, 0);
}

void console_print_verbose(const char* fmt, ...) {
	if (!is_verbose_mode) return;
	char buf[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf)-1, fmt, args);
	buf[sizeof(buf)-1] = 0;
	fprintf(stdout, "%s", buf);
	va_end(args);

	console_split_lines_and_add_log_item(buf, true, 2);
}


void console_print_error(const char* fmt, ...) {
	char buf[4096];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf)-1, fmt, args);
	buf[sizeof(buf)-1] = 0;
	fprintf(stderr, "%s", buf);
	va_end(args);

	console_split_lines_and_add_log_item(buf, true, 1);
}
