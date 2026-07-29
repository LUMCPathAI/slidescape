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
#include "platform.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_widget_flamegraph.h"

#include "openslide_api.h"
#include "viewer.h"
#include "remote.h"
#include "tiff_write.h"
#include "image.h"
#include "image_registration.h"
#include "dicom.h"
#include "slide_score.h"
#include "profiler.h"


#define GUI_IMPL
#include "gui.h"
#include "stringutils.h"

#if WINDOWS
#include "ltalloc.h"
static void*   imgui_malloc_wrapper(size_t size, void* user_data)    { IM_UNUSED(user_data); return ltmalloc(size); }
static void    imgui_free_wrapper(void* ptr, void* user_data)        { IM_UNUSED(user_data); ltfree(ptr); }
#else
static void*   imgui_malloc_wrapper(size_t size, void* user_data)    { IM_UNUSED(user_data); return malloc(size); }
static void    imgui_free_wrapper(void* ptr, void* user_data)        { IM_UNUSED(user_data); free(ptr); }
#endif


static char imgui_ini_filename[512];

static void draw_dicom_slides_window(app_state_t* app_state) {
	if (!show_dicom_slides_window || arrlen(app_state->loaded_images) == 0) return;
	image_t* image = app_state->loaded_images[0];
	if (image->backend != IMAGE_BACKEND_DICOM || arrlen(image->dicom.slides) <= 1) {
		show_dicom_slides_window = false;
		return;
	}

	ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("DICOM slides", &show_dicom_slides_window, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Select a slide from this directory:");
		ImGui::Separator();
		for (i32 i = 0; i < arrlen(image->dicom.slides); ++i) {
			dicom_slide_t* slide = image->dicom.slides + i;
			const char* label = one_past_last_slash(slide->representative_filename,
			                                       strlen(slide->representative_filename));
			bool selected = i == image->dicom.selected_slide_index;
			if (ImGui::Selectable(label, selected) && !selected) {
				char filename[512];
				copy_cstring(filename, slide->representative_filename, sizeof(filename));
				load_generic_file(app_state, filename, FILETYPE_HINT_BASE_IMAGE);
				break;
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", slide->series_instance_uid.value);
			}
		}
	}
	ImGui::End();
}

static void gui_sync_extra_drawlist_shared_data(ImDrawListSharedData* shared_data) {
	ImDrawListSharedData* imgui_shared_data = ImGui::GetDrawListSharedData();
	shared_data->TexUvWhitePixel = imgui_shared_data->TexUvWhitePixel;
	shared_data->TexUvLines = imgui_shared_data->TexUvLines;
	shared_data->FontAtlas = imgui_shared_data->FontAtlas;
	shared_data->Font = imgui_shared_data->Font;
	shared_data->FontSize = imgui_shared_data->FontSize;
	shared_data->FontScale = imgui_shared_data->FontScale;
	shared_data->CurveTessellationTol = imgui_shared_data->CurveTessellationTol;
	shared_data->SetCircleTessellationMaxError(imgui_shared_data->CircleSegmentMaxError);
	shared_data->InitialFringeScale = imgui_shared_data->InitialFringeScale;
	shared_data->InitialFlags = imgui_shared_data->InitialFlags;
	shared_data->ClipRectFullscreen = imgui_shared_data->ClipRectFullscreen;
	shared_data->Context = imgui_shared_data->Context;
}

void imgui_create_context() {
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::SetAllocatorFunctions(imgui_malloc_wrapper, imgui_free_wrapper);
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	if (global_settings_dir) {
		snprintf(imgui_ini_filename, sizeof(imgui_ini_filename), "%s" PATH_SEP "%s", global_settings_dir, "imgui.ini");
		io.IniFilename = imgui_ini_filename;
	}
}

// Reset a drawlist to make it empty again (needs to be called at the beginning of every frame for each active drawlist)
void gui_drawlist_reset_for_new_frame(ImDrawList* drawlist) {
	// (no need to PopTexture()/PopClipRect()) since we reset every frame
	// see: https://github.com/ocornut/imgui/issues/6406#issuecomment-1632826410
	gui_sync_extra_drawlist_shared_data(drawlist->_Data);
	drawlist->_ResetForNewFrame();
	drawlist->PushTexture(ImGui::GetIO().Fonts->TexRef);
	drawlist->PushClipRectFullScreen();
}

void gui_reset_all_extra_drawlists() {
	for (i32 i = 0; i < global_active_extra_drawlists; ++i) {
		ImDrawList* drawlist = global_extra_drawlists[i];
		if (drawlist) {
			gui_drawlist_reset_for_new_frame(drawlist);
		}
	}
	gui_mark_extra_drawlists_modified();
}

void gui_destroy_all_extra_drawlists() {
	for (i32 i = 0; i < MAX_EXTRA_DRAWLISTS; ++i) {
		ImDrawListSharedData* shared_data = global_extra_drawlist_shared_datas + i;
		if (global_extra_drawlists[i]) {
			delete global_extra_drawlists[i];
			global_extra_drawlists[i] = NULL;
		}
		if (shared_data->FontAtlas && shared_data->FontAtlas->DrawListSharedDatas.contains(shared_data)) {
			ImFontAtlasRemoveDrawListSharedData(shared_data->FontAtlas, shared_data);
		}
		shared_data->DrawLists.resize(0);
	}
	global_active_extra_drawlists = 0;
	gui_mark_extra_drawlists_modified();
}

// Retrieve one of the global extra drawlists by index, and initialize it if necessary.
ImDrawList* gui_get_extra_drawlist(i32 drawlist_index) {
	ASSERT(drawlist_index < MAX_EXTRA_DRAWLISTS);
	ImDrawList* drawlist = global_extra_drawlists[drawlist_index];
	if (!drawlist) {
		ImDrawListSharedData* shared_data = global_extra_drawlist_shared_datas + drawlist_index;
		gui_sync_extra_drawlist_shared_data(shared_data);
		ImFontAtlasAddDrawListSharedData(shared_data->FontAtlas, shared_data);
		global_extra_drawlists[drawlist_index] = drawlist = new ImDrawList(global_extra_drawlist_shared_datas + drawlist_index);
		gui_drawlist_reset_for_new_frame(drawlist);
//		console_print("draw_annotation_batch(): initialized ImDrawList #%d\n", drawlist_index);
	}
	return drawlist;
}

void gui_mark_extra_drawlists_modified() {
	++global_extra_drawlists_generation;
	if (global_extra_drawlists_generation == 0) {
		global_extra_drawlists_generation = 1;
	}
}

ImDrawData* gui_get_merged_extra_drawlists_data(ImGuiViewport* viewport, ImVec2 framebuffer_scale) {
	static ImDrawListSharedData merged_shared_data;
	static ImDrawList* merged_drawlist;
	static ImDrawData merged_draw_data = ImDrawData();
	static ImDrawData unmerged_draw_data = ImDrawData();
	static ImVector<ImDrawList*> unmerged_drawlists;
	static u32 merged_generation;

	if (global_active_extra_drawlists <= 0) {
		return NULL;
	}

	if (!enable_merged_extra_drawlists) {
		unmerged_draw_data.Clear();
		unmerged_draw_data.DisplayPos = viewport->Pos;
		unmerged_draw_data.DisplaySize = viewport->Size;
		unmerged_draw_data.FramebufferScale = framebuffer_scale;
		unmerged_draw_data.OwnerViewport = viewport;
		unmerged_draw_data.Textures = &ImGui::GetPlatformIO().Textures;
		unmerged_drawlists.resize(0);
		for (i32 i = 0; i < global_active_extra_drawlists; ++i) {
			ImDrawList* drawlist = global_extra_drawlists[i];
			if (drawlist && drawlist->VtxBuffer.Size > 0 && drawlist->IdxBuffer.Size > 0) {
				unmerged_drawlists.push_back(drawlist);
				unmerged_draw_data.TotalIdxCount += drawlist->IdxBuffer.Size;
				unmerged_draw_data.TotalVtxCount += drawlist->VtxBuffer.Size;
			}
		}
		unmerged_draw_data.CmdLists = unmerged_drawlists;
		unmerged_draw_data.CmdListsCount = unmerged_drawlists.Size;
		unmerged_draw_data.Valid = true;
		if (unmerged_draw_data.CmdListsCount <= 0 || unmerged_draw_data.TotalVtxCount <= 0) {
			return NULL;
		}
		return &unmerged_draw_data;
	}

	if (!merged_drawlist) {
		gui_sync_extra_drawlist_shared_data(&merged_shared_data);
		ImFontAtlasAddDrawListSharedData(merged_shared_data.FontAtlas, &merged_shared_data);
		merged_drawlist = new ImDrawList(&merged_shared_data);
	}

	if (merged_generation != global_extra_drawlists_generation) {
		gui_sync_extra_drawlist_shared_data(merged_drawlist->_Data);
		merged_drawlist->_ResetForNewFrame();
		merged_drawlist->CmdBuffer.resize(0);
		merged_drawlist->IdxBuffer.resize(0);
		merged_drawlist->VtxBuffer.resize(0);

		// Merge annotation worker draw lists into one backend upload. The draw
		// commands stay separate, so clip rects and texture ids are preserved.
		for (i32 i = 0; i < global_active_extra_drawlists; ++i) {
			ImDrawList* src = global_extra_drawlists[i];
			if (!src || src->VtxBuffer.Size <= 0 || src->IdxBuffer.Size <= 0) {
				continue;
			}

			u32 vtx_offset = (u32)merged_drawlist->VtxBuffer.Size;
			u32 idx_offset = (u32)merged_drawlist->IdxBuffer.Size;
			merged_drawlist->VtxBuffer.resize(merged_drawlist->VtxBuffer.Size + src->VtxBuffer.Size);
			merged_drawlist->IdxBuffer.resize(merged_drawlist->IdxBuffer.Size + src->IdxBuffer.Size);
			memcpy(merged_drawlist->VtxBuffer.Data + vtx_offset, src->VtxBuffer.Data, src->VtxBuffer.Size * sizeof(src->VtxBuffer.Data[0]));
			memcpy(merged_drawlist->IdxBuffer.Data + idx_offset, src->IdxBuffer.Data, src->IdxBuffer.Size * sizeof(src->IdxBuffer.Data[0]));
			for (int cmd_i = 0; cmd_i < src->CmdBuffer.Size; ++cmd_i) {
				ImDrawCmd cmd = src->CmdBuffer[cmd_i];
				if (cmd.ElemCount == 0) {
					continue;
				}
				cmd.VtxOffset += vtx_offset;
				cmd.IdxOffset += idx_offset;
				merged_drawlist->CmdBuffer.push_back(cmd);
			}
		}
		// The buffers were populated directly rather than through PrimReserve().
		// Restore the write cursors expected by ImDrawData::AddDrawList().
		merged_drawlist->_VtxWritePtr = merged_drawlist->VtxBuffer.Size > 0
		                                ? merged_drawlist->VtxBuffer.Data + merged_drawlist->VtxBuffer.Size : NULL;
		merged_drawlist->_IdxWritePtr = merged_drawlist->IdxBuffer.Size > 0
		                                ? merged_drawlist->IdxBuffer.Data + merged_drawlist->IdxBuffer.Size : NULL;
		merged_generation = global_extra_drawlists_generation;
	}

	merged_draw_data.Clear();
	if (merged_drawlist->VtxBuffer.Size <= 0 || merged_drawlist->IdxBuffer.Size <= 0 || merged_drawlist->CmdBuffer.Size <= 0) {
		return NULL;
	}
	merged_draw_data.DisplayPos = viewport->Pos;
	merged_draw_data.DisplaySize = viewport->Size;
	merged_draw_data.FramebufferScale = framebuffer_scale;
	merged_draw_data.OwnerViewport = viewport;
	merged_draw_data.Textures = &ImGui::GetPlatformIO().Textures;
	merged_draw_data.AddDrawList(merged_drawlist);
	merged_draw_data.Valid = true;
	return &merged_draw_data;
}

void gui_make_next_window_appear_in_center_of_screen() {
	ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
}

void menu_close_file(app_state_t* app_state) {
	unload_all_images(app_state);
	unload_and_reinit_annotations(&app_state->scene.annotation_set);
	if (app_state->remember_annotation_groups_as_template && app_state->scene.annotation_set_template.is_valid) {
		annotation_set_init_from_template(&app_state->scene.annotation_set, &app_state->scene.annotation_set_template);
	}
}

void gui_draw_polygon_outline(v2f* points, i32 count, rgba_t rgba, bool closed, float thickness, ImDrawList* drawlist) {
	if (count < 2) return;
	if (!drawlist) {
		drawlist = ImGui::GetBackgroundDrawList();
	}
	u32 color = *(u32*)(&rgba);
	// Workaround for problem with acute angles
	// (lines are being drawn incorrectly with not enough thickness if the angles are too sharp)
	// Solution until the problem is fixed in ImGui: split into segments based on whether the angle is acute or not
	// https://github.com/ocornut/imgui/issues/3366#issuecomment-664779883
	i32 i = 0;
	bool has_at_least_one_split = false;
	while (i + 1 < count) {
		i32 nlin = 2;
		while (i + nlin < count) {
			v2f v0 = points[i+nlin-2];
			v2f v1 = points[i+nlin-1];
			v2f v2 = points[i+nlin];
			v2f s0 = v2f_subtract(v1, v0);
			v2f s1 = v2f_subtract(v2, v1);
			float dotprod = v2f_dot(s0, s1);
			if (dotprod < 0) {
				has_at_least_one_split = true;
				break;
			}
			++nlin;
		}

		// If it's the last segment, we may need to 'close' the polygon (but this only works if there are no splits)
		ImDrawFlags flags = 0;
		if (i + nlin == count && closed && !has_at_least_one_split) {
			flags = ImDrawFlags_Closed;
		}

		drawlist->AddPolyline((ImVec2*)(points + i), nlin, color, flags, thickness);
		i += nlin-1;
	}

	// Close the polygon using a manually added line in case of a split due to acute angles
	if (closed && has_at_least_one_split) {
		drawlist->AddLine(points[0], points[count - 1], color, thickness);
	}
}

void gui_draw_polygon_outline_in_scene(v2f* points, i32 count, rgba_t color, bool closed, float thickness, scene_t* scene, ImDrawList* drawlist) {
	for (i32 i = 0; i < count; ++i) {
		points[i] = world_pos_to_screen_pos(scene, points[i]);
	}
	gui_draw_polygon_outline(points, count, color, closed, thickness, drawlist);
}

void gui_draw_bounds_in_scene(bounds2f bounds, rgba_t color, float thickness, scene_t* scene, ImDrawList* drawlist) {
	v2f points[4];
	points[0] = V2F(bounds.left, bounds.top);
	points[1] = V2F(bounds.left, bounds.bottom);
	points[2] = V2F(bounds.right, bounds.bottom);
	points[3] = V2F(bounds.right, bounds.top);
	gui_draw_polygon_outline_in_scene(points, 4, color, true, thickness, scene, drawlist);
}

static void check_presence_of_debug_test_files() {
	static bool checked;
	if (!checked) {
		checked = true;
	}
}

bool gui_draw_selected_annotation_submenu_section(app_state_t* app_state, scene_t* scene, annotation_set_t* annotation_set) {
	// Change annotation type
	bool proceed = annotation_set->selection_count > 0;
	if (proceed) {
		if (annotation_set->selection_count == 1) {
			annotation_t* selected_annotation = annotation_set->selected_annotations[0];
			if (selected_annotation->type == ANNOTATION_POLYGON && selected_annotation->coordinate_count == 4) {
				if (ImGui::BeginMenu("Set annotation type")) {
					if (ImGui::MenuItem("Freeform", NULL, true)) {}
					if (ImGui::MenuItem("Rectangle", NULL, false)) {
						annotation_history_begin_action(annotation_set);
						annotation_history_track_annotation(annotation_set, selected_annotation);
						selected_annotation->type = ANNOTATION_RECTANGLE;
						annotation_set_rectangle_coordinates_to_bounding_box(annotation_set, selected_annotation);
						annotation_history_end_action(annotation_set);
					}
					ImGui::EndMenu();
				}
			} else if (selected_annotation->type == ANNOTATION_RECTANGLE) {
				if (ImGui::BeginMenu("Set annotation type")) {
					if (ImGui::MenuItem("Freeform", NULL, false)) {
						annotation_history_begin_action(annotation_set);
						annotation_history_track_annotation(annotation_set, selected_annotation);
						selected_annotation->type = ANNOTATION_POLYGON;
						notify_annotation_set_modified(annotation_set);
						annotation_history_end_action(annotation_set);
					}
					if (ImGui::MenuItem("Rectangle", NULL, true)) {}
					ImGui::EndMenu();
				}
			}
		}

        // Assign group
        if (annotation_set->active_group_count > 1) {
            if (ImGui::BeginMenu("Set annotation group")) {
                for (i32 group_index = 0; group_index < annotation_set->active_group_count; ++group_index) {
                    annotation_group_t* group = annotation_set->stored_groups + group_index;

                    if (ImGui::MenuItem(group->name)) {
                        set_group_for_selected_annotations(annotation_set, group_index);
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Assign...", NULL, &show_annotation_group_assignment_window)) {}

                ImGui::EndMenu();
            }
        }

		const char* delete_text = annotation_set->selection_count > 1 ? "Delete annotations" : "Delete annotation";
		if (ImGui::MenuItem(delete_text, "Del")) {
			if (dont_ask_to_delete_annotations) {
				delete_selected_annotations(app_state, annotation_set);
			} else {
				show_delete_annotation_prompt = true;
			}
		};

        // Option for setting the selection box around the selected annotation(s)
        if (annotation_set->selection_count >= 1) {
            if (ImGui::MenuItem("Export region...")) {
                set_region_encompassing_selected_annotations(annotation_set, scene);
	            show_export_region_dialog = true;
            }
        }
	}

	return proceed;
}

void gui_draw_insert_annotation_submenu(app_state_t* app_state) {
	if (ImGui::BeginMenu("New annotation", arrlen(app_state->loaded_images) > 0)) {
		if (ImGui::MenuItem("Point", "Q")) {
			viewer_switch_tool(app_state, TOOL_CREATE_POINT);
		}
		if (ImGui::MenuItem("Line", "M")) {
			viewer_switch_tool(app_state, TOOL_CREATE_LINE);
		}
		if (ImGui::MenuItem("Freeform", "F")) {
			viewer_switch_tool(app_state, TOOL_CREATE_FREEFORM);
		}
//		if (ImGui::MenuItem("Ellipse", "E")) {
//			viewer_switch_tool(app_state, TOOL_CREATE_ELLIPSE);
//		}
		if (ImGui::MenuItem("Rectangle", "R")) {
			viewer_switch_tool(app_state, TOOL_CREATE_RECTANGLE);
		}
//		if (ImGui::MenuItem("Text", "T")) {
//			viewer_switch_tool(app_state, TOOL_CREATE_TEXT);
//		}
		ImGui::EndMenu();
	}
}

static void gui_draw_main_menu_bar(app_state_t* app_state) {
	check_presence_of_debug_test_files();

	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	bool ret = ImGui::BeginMainMenuBar();
	ImGui::PopStyleVar(1);
	if (ret) {
		scene_t* scene = &app_state->scene;

		static struct {
			bool open_file;
			bool close;
			bool save;
			bool open_remote;
			bool open_uri;
			bool exit_program;
			bool new_dataset_asap_xml;
			bool new_dataset_geojson;
			bool select_region_create_box;
			bool select_region_encompass_annotations;
			bool select_region_whole_slide;
			bool deselect;
			bool crop_region;
			bool export_region;
			bool reset_zoom;
		} menu_items_clicked;
		memset(&menu_items_clicked, 0, sizeof(menu_items_clicked));

		bool prev_is_vsync_enabled = is_vsync_enabled;
		bool prev_fullscreen = is_fullscreen;
		bool prev_enable_autosave = app_state->enable_autosave;
		bool prev_remember_annotation_groups_as_template = app_state->remember_annotation_groups_as_template;
		bool has_image_loaded = (arrlen(app_state->loaded_images) > 0);
		bool can_save = scene->annotation_set.modified;

		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Open file...", "Ctrl+O", &menu_items_clicked.open_file)) {}
			if (ImGui::MenuItem("Open URI...", "Ctrl+U", &menu_items_clicked.open_uri)) {}
			if (ImGui::MenuItem("Close", "Ctrl+W", &menu_items_clicked.close)) {}
			ImGui::Separator();
			if (ImGui::MenuItem("Save", "Ctrl+S", &menu_items_clicked.save, can_save)) {}
			ImGui::Separator();

			if (ImGui::MenuItem("Export...", NULL, &menu_items_clicked.export_region, scene->can_export_region)) {}

			ImGui::Separator();
			if (ImGui::MenuItem("Exit", "Alt+F4", &menu_items_clicked.exit_program)) {}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit")) {
			if (ImGui::MenuItem("Undo annotation edit", "Ctrl+Z", false, annotation_history_can_undo(&scene->annotation_set))) {
				annotation_history_undo(&scene->annotation_set);
			}
			if (ImGui::MenuItem("Redo annotation edit", "Ctrl+Y", false, annotation_history_can_redo(&scene->annotation_set))) {
				annotation_history_redo(&scene->annotation_set);
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("Select export region", has_image_loaded)) {
				if (ImGui::MenuItem("Draw selection box...", NULL, &menu_items_clicked.select_region_create_box)) {}
				ImGui::Separator();
				if (ImGui::MenuItem("Set region to whole slide", NULL, &menu_items_clicked.select_region_whole_slide, has_image_loaded)) {}
				bool encompass_option_enabled = scene->annotation_set.selection_count > 0;
				if (scene->annotation_set.selection_count == 1) {
					if (ImGui::MenuItem("Set region to selected annotation", NULL, &menu_items_clicked.select_region_encompass_annotations, encompass_option_enabled)) {}
				} else {
					if (ImGui::MenuItem("Set region to selected annotations", NULL, &menu_items_clicked.select_region_encompass_annotations, encompass_option_enabled)) {}
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Deselect region", NULL, &menu_items_clicked.deselect, app_state->scene.has_selection_box)) {}
			ImGui::Separator();
			if (ImGui::MenuItem("Restrict view to region", NULL, app_state->scene.is_cropped, app_state->scene.has_selection_box || app_state->scene.is_cropped)) {
				menu_items_clicked.crop_region = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("General options...", NULL, &show_general_options_window)) {}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Annotation")) {
			gui_draw_insert_annotation_submenu(app_state);
			ImGui::Separator();
			if (ImGui::MenuItem("Annotations...", NULL, &show_annotations_window)) {}
			if (ImGui::MenuItem("Assign group/feature...", "G", &show_annotation_group_assignment_window)) {}
			ImGui::Separator();
			if (ImGui::MenuItem("Autosave", NULL, &app_state->enable_autosave)) {}
			if (ImGui::MenuItem("Remember groups/features", NULL, &app_state->remember_annotation_groups_as_template)) {}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View")) {
			prev_fullscreen = is_fullscreen = check_fullscreen(app_state->main_window); // double-check just in case...
			if (ImGui::MenuItem("Reset zoom", NULL, &menu_items_clicked.reset_zoom)) {}
			ImGui::Separator();
			if (ImGui::MenuItem("Fullscreen", "F11", &is_fullscreen)) {}
			if (ImGui::MenuItem("Image options...", NULL, &show_image_options_window)) {}
			if (ImGui::MenuItem("Layers...", "L", &show_layers_window)) {}
			bool has_dicom_slides = has_image_loaded &&
			                        app_state->loaded_images[0]->backend == IMAGE_BACKEND_DICOM &&
			                        arrlen(app_state->loaded_images[0]->dicom.slides) > 1;
			if (ImGui::MenuItem("DICOM slides...", NULL, &show_dicom_slides_window, has_dicom_slides)) {}
			ImGui::Separator();
			bool* show_scale_bar = has_image_loaded ? &scene->scale_bar.enabled : NULL;
			if (ImGui::MenuItem("Show scale bar", "Ctrl+B", show_scale_bar, (show_scale_bar != NULL))) {}
			bool* show_grid = has_image_loaded ? &scene->enable_grid : NULL;
			if (ImGui::MenuItem("Show grid", "Ctrl+G", show_grid, (show_grid != NULL))) {}
			ImGui::Separator();

			if (ImGui::BeginMenu("Debug")) {
				if (ImGui::MenuItem("Show console", "F3 or `", &show_console_window)) {}
				if (ImGui::MenuItem("Show profiler", "F2", &show_profiler_window)) {}
				if (ImGui::MenuItem("Show demo window", "F1", &show_demo_window)) {}
				if (ImGui::MenuItem("Show debugging window", "Ctrl+F1", &show_debugging_window)) {}
				ImGui::Separator();
                if (ImGui::MenuItem("Open remote WSI...", NULL, &menu_items_clicked.open_remote)) {}
				ImGui::Separator();
				if (ImGui::MenuItem("Show menu bar", "Alt+F12", &show_menu_bar)) {}
				if (ImGui::MenuItem("Load next as overlay", "F6", &load_next_image_as_overlay)) {}
				if (ImGui::MenuItem("Show mouse position", NULL, &show_mouse_pos_overlay)) {}
                ImGui::Separator();
                if (ImGui::MenuItem("Set working directory...")) {
                    open_file_dialog(app_state, OPEN_FILE_DIALOG_CHOOSE_DIRECTORY, 0);
                }
                ImGui::EndMenu();
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Help")) {
			if (ImGui::MenuItem("About...", NULL, &show_about_window)) {}
			ImGui::EndMenu();
		}

		ImGui::EndMainMenuBar();


		if (menu_items_clicked.exit_program) {
			is_program_running = false;
		} else if (menu_items_clicked.open_file) {
            u32 filetype_hint = load_next_image_as_overlay ? FILETYPE_HINT_OVERLAY : 0;
			open_file_dialog(app_state, OPEN_FILE_DIALOG_LOAD_GENERIC_FILE, filetype_hint);
		} else if (menu_items_clicked.open_uri) {
            show_open_uri_window = true;
        } else if (menu_items_clicked.close) {
			menu_close_file(app_state);
		} else if (menu_items_clicked.save) {
			save_annotations(app_state, &app_state->scene.annotation_set, true, false);
		} else if (menu_items_clicked.open_remote) {
			show_open_remote_window = true;
		} else if (prev_fullscreen != is_fullscreen) {
			bool currently_fullscreen = check_fullscreen(app_state->main_window);
			if (currently_fullscreen != is_fullscreen) {
				toggle_fullscreen(app_state->main_window);
			}
		} else if (menu_items_clicked.select_region_create_box) {
			app_state->mouse_mode = MODE_CREATE_SELECTION_BOX;
		} else if (menu_items_clicked.select_region_encompass_annotations) {
			set_region_encompassing_selected_annotations(&scene->annotation_set, scene);
		} else if (menu_items_clicked.select_region_whole_slide) {
			if (arrlen(app_state->loaded_images) > 0) {
				image_t* image = app_state->loaded_images[0];
				// TODO: what to do if there are multiple layers?
				set_region_for_whole_slide(scene, image);
				scene->need_zoom_reset = true;
			}
		} else if (menu_items_clicked.deselect) {
			scene->has_selection_box = false;
		} else if (menu_items_clicked.crop_region) {
			if (!scene->is_cropped) {
				rect2f final_crop_rect = rect2f_recanonicalize(&scene->selection_box);
				bounds2f bounds = rect2f_to_bounds(final_crop_rect);
				scene->crop_bounds = bounds;
				scene->is_cropped = true;
				scene->has_selection_box = false;
			} else {
				scene->is_cropped = false;
				scene->has_selection_box = false;
			}
		} else if (prev_is_vsync_enabled != is_vsync_enabled) {
			set_swap_interval(is_vsync_enabled ? 1 : 0);
			viewer_save_options(app_state);
		} else if (menu_items_clicked.export_region) {

			if (scene->can_export_region) {
				show_export_region_dialog = true;
			} else {
				ASSERT(!"Trying to export a region without a selected region");
			}


		} else if (menu_items_clicked.reset_zoom) {
			scene->need_zoom_reset = true;
		}

		if (prev_enable_autosave != app_state->enable_autosave ||
		    prev_remember_annotation_groups_as_template != app_state->remember_annotation_groups_as_template
        ) {
			viewer_save_options(app_state);
		}
	}

}

// TODO
typedef struct image_layer_t {
	i32 index;
	char name[512];
	v2f origin_offset; // TODO: transfer from image->origin_offset
	image_backend_enum backend;
	bool is_loaded;
	bool enabled;
	bool is_mask;
	bool uses_lut;
} image_layer_t;

typedef struct annotation_layer_t {
	i32 index;
	char name[512];
} annotation_layer_t;

void draw_layers_window(app_state_t* app_state) {
	if (!show_layers_window) return;

	ImGui::SetNextWindowPos(ImVec2(20, 50), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(460,541), ImGuiCond_FirstUseEver);

	ImGui::Begin("Layers", &show_layers_window);

	const float TEXT_BASE_WIDTH = ImGui::CalcTextSize("A").x;

	i32 image_count = arrlen(app_state->loaded_images);
	if (layers_window_selected_image_index >= image_count) layers_window_selected_image_index = 0;

	static ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody;

	if (ImGui::BeginTable("layers_table", 4, flags)) {
		// The first column will use the default _WidthStretch when ScrollX is Off and _WidthFixed when ScrollX is On
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 20.0f);
		ImGui::TableSetupColumn("##layers_table_checkbox", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, 25.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, TEXT_BASE_WIDTH * 18.0f);
		ImGui::TableHeadersRow();

		i32 enabled_image_count = 0; // //TODO: calculate once and keep track?
		for (i32 i = 0; i < image_count; ++i) {
			if (app_state->loaded_images[i]->is_enabled) ++enabled_image_count;
		}

		for (i32 image_index = 0; image_index < image_count; ++image_index) {
			image_t* image = app_state->loaded_images[image_index];
			ImGui::TableNextRow();

			// Display index
			ImGui::TableNextColumn();
			ImGui::Text("%d", image_index);

			// Display 'enabled' checkbox
			ImGui::TableNextColumn();
			char checkbox_label[32];
			snprintf(checkbox_label, sizeof(checkbox_label), "##layers_checkbox_%d", image_index);

			bool checkbox_disabled = false;
			if ((image->is_enabled && enabled_image_count == 1) /*|| (!image->is_enabled && enabled_image_count == 2)*/) {
				ImGui::BeginDisabled();
				checkbox_disabled = true;
			}

			if (ImGui::Checkbox(checkbox_label, &image->is_enabled)) {
				if (!image->is_enabled && app_state->enabled_image_count == 1) {
					image->is_enabled = true; // don't allow the number of enabled images to drop to zero.
				} else if (image->is_enabled && app_state->enabled_image_count >= 2) {
					// don't allow the number of enabled images to grow above 2.
					// -> disable either the last checked image or the one we just pressed
					if (app_state->last_layer_enabled < image_count) {
						image_t* last_image = app_state->loaded_images[app_state->last_layer_enabled];
						if (last_image->is_valid && last_image->is_enabled) {
							last_image->is_enabled = false;
							layers_window_selected_image_index = image_index;
							app_state->last_layer_enabled = image_index;
						} else {
							image->is_enabled = false;
						}
					} else {
						image->is_enabled = false;
					}
				} else if (image->is_enabled) {
					app_state->last_layer_enabled = image_index;
					layers_window_selected_image_index = image_index;
				}
			}
			// TODO: recount enabled image count

			if (checkbox_disabled) {
				ImGui::EndDisabled();
			}

			// Display layer name
			ImGui::TableNextColumn();
			char name[256];
			snprintf(name, sizeof(name)-1, "%s", image->name);
			bool selected = ImGui::Selectable(name);

			// Displayer layer type
			ImGui::TableNextColumn();
			const char* type = get_image_descriptive_type_name(image);
			if (selected) layers_window_selected_image_index = image_index;
			ImGui::TextUnformatted(type);
		}

		ImGui::EndTable();
	}

	bool disable_gui = image_count == 0;
	if (disable_gui) {
		ImGui::BeginDisabled();
	}
	if (ImGui::ButtonEx("Load paired image...", ImVec2(0,0))) {
		open_file_dialog(app_state, OPEN_FILE_DIALOG_LOAD_GENERIC_FILE,FILETYPE_HINT_OVERLAY);
	}
	ImGui::SameLine();
	ImGui::Checkbox("Load next image as overlay (F6)", &load_next_image_as_overlay);

	if (disable_gui) {
		ImGui::EndDisabled();
	}

	ImGui::NewLine();
	if (layers_window_selected_image_index < image_count) {
		image_t* image = app_state->loaded_images[layers_window_selected_image_index];
		ImGui::Text("Adjust position offset for layer %d:", layers_window_selected_image_index);
		ASSERT(image->mpp_x != 0.0f);
		ASSERT(image->mpp_y != 0.0f);
		i32 px_x = roundf(image->origin_offset.x / image->mpp_x);
		i32 px_y = roundf(image->origin_offset.y / image->mpp_y);
		if (ImGui::DragInt("Offset X", &px_x, 1.0f, 0.0f, 0.0f, "%d px")) {
			image->origin_offset.x = ((float)px_x * image->mpp_x);
		}
		if (ImGui::DragInt("Offset Y", &px_y, 1.0f, 0.0f, 0.0f, "%d px")) {
			image->origin_offset.y = ((float)px_y * image->mpp_y);
		}
		if (ImGui::Button("Reset")) {
			image->origin_offset.x = 0.0f;
			image->origin_offset.y = 0.0f;
		}
        if (layers_window_selected_image_index > 0) {
            ImGui::SameLine();
            if (ImGui::Button("Re-register")) {
                image_transform_t transform = do_image_registration(app_state->loaded_images[0], image, 2);
                if (transform.is_valid) {
                    // apply translation
                    if (transform.translate.x != 0.0f || transform.translate.y != 0.0f) {
                        image->origin_offset = transform.translate;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Re-register (local)")) {
                console_print("\nLocal image registration:\n");
                image_transform_t transform1 = do_local_image_registration(app_state->loaded_images[0], image, app_state->scene.camera, app_state->scene.zoom.level, 2048, REGISTER_PREPROCESS_NONE);
                image_transform_t transform2 = do_local_image_registration(app_state->loaded_images[0], image, app_state->scene.camera, app_state->scene.zoom.level, 2048, REGISTER_PREPROCESS_ISOLATE_HEMATOXYLIN);
                image_transform_t best_transform = (transform1.response > transform2.response) ? transform1 : transform2;
//                image_transform_t best_transform = transform1;
                if (best_transform.is_valid) {
                    // apply differential translation
                    if (best_transform.translate.x != 0.0f || best_transform.translate.y != 0.0f) {
                        image->origin_offset = v2f_add(image->origin_offset, best_transform.translate);
                    }
                }
            }
        }
//		ImGui::DragFloat("Offset Y", &image->origin_offset.y, image->mpp_y, 0.0f, 0.0f, "%g px");
	}
	ImGui::NewLine();

    bool disable_layer_transition_control = (image_count < 2);
    if (disable_layer_transition_control) {
        ImGui::BeginDisabled();
    }
	ImGui::Text("Currently displayed layer: %d.\nPress Space or F5 to toggle layers.", app_state->scene.active_layer);

	ImGui::SliderFloat("Layer transition", &app_state->scene.target_layer_time, 0.0f, 1.0f);
    if (disable_layer_transition_control) {
        ImGui::EndDisabled();
    }

	ImGui::End();
}


void draw_export_region_dialog(app_state_t* app_state) {
	if (show_export_region_dialog) {
		image_t* image = (arrlen(app_state->loaded_images) > 0) ? app_state->loaded_images[0] : NULL;
		scene_determine_if_export_is_possible(&app_state->scene, image);
		if (!app_state->scene.can_export_region) {
			// TODO: add image backend structs
			// TODO: why can't we specify a reason here?
			gui_add_modal_message_popup("Error##draw_export_region_dialog",
			                            "Exporting a region is not possible.\n");
			console_print_error("Error: exporting a region is not possible.\n");
		}
		ImGui::OpenPopup("Export region");
        if (tiff_export_match_input_resolution) {
            tiff_export_mpp = app_state->loaded_images[0]->mpp_x;
        }
		show_export_region_dialog = false;
	}
	gui_make_next_window_appear_in_center_of_screen();
	ImGui::SetNextWindowSize(ImVec2(800.0f, 400.0f), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Export region", NULL, 0/*ImGuiWindowFlags_AlwaysAutoResize*/)) {
		scene_t* scene = &app_state->scene;
		image_t* image = app_state->loaded_images[0];

		bool display_export_annotations_checkbox = (scene->annotation_set.active_annotation_count > 0);
		i32 lines_at_bottom = 2;
//		if (display_export_annotations_checkbox) {
//			++lines_at_bottom;
//		}

		ImGui::BeginGroup();
		ImGui::BeginChild("item view", ImVec2(0, -lines_at_bottom * ImGui::GetFrameHeightWithSpacing())); // Leave room for 2 lines below us

		static bool also_export_annotations = false;
		static bool allow_coordinates_outside_region = false;

		if (scene->can_export_region) {
			if (image->mpp_x > 0.0f && image->mpp_y > 0.0f) {
				bounds2i pixel_bounds = scene->selection_pixel_bounds;

				ImGui::Text("Region to export: %s", scene->selection_description);

				if (ImGui::TreeNodeEx("Adjust region", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
					rect2i export_rect = {pixel_bounds.left, pixel_bounds.top, pixel_bounds.right - pixel_bounds.left, pixel_bounds.bottom - pixel_bounds.top};
					bool changed = false;
					changed |= ImGui::InputInt("Offset X##export_pixel_bounds", &export_rect.x);
					changed |= ImGui::InputInt("Offset Y##export_pixel_bounds", &export_rect.y);
					changed |= ImGui::InputInt("Width##export_pixel_bounds", &export_rect.w);
					changed |= ImGui::InputInt("Height##export_pixel_bounds", &export_rect.h);

					if (changed) {
						if (!scene->has_selection_box) {
							scene->has_selection_box = true;
						}
						scene->selection_description = "selected area (modified)";
						scene->selection_box = pixel_rect_to_world_rect(export_rect, image->mpp_x, image->mpp_y);
						// TODO: MRXS backend: don't rely on origin_offset, pad the image with empty tiles instead
						scene->selection_box.x += image->origin_offset.x;
						scene->selection_box.y += image->origin_offset.y;
						scene->selection_pixel_bounds = BOUNDS2I(export_rect.x, export_rect.y, export_rect.x + export_rect.w, export_rect.y + export_rect.h);
//						pixel_bounds = scene->selection_pixel_bounds;
					}
				}
				ImGui::NewLine();

//				ImGui::Text("Selected region (pixel coordinates):\nx=%d\ny=%d\nwidth=%d\nheight=%d",
//							 pixel_bounds.left, pixel_bounds.top,
//							 pixel_bounds.right - pixel_bounds.left, pixel_bounds.bottom - pixel_bounds.top);

				const char* export_formats[] = {"Tiled TIFF", };//"JPEG", "PNG"}; TODO: implement JPEG and PNG export
				if (ImGui::BeginCombo("Export format", export_formats[desired_region_export_format])) // The second parameter is the label previewed before opening the combo.
				{
					for (i32 i = 0; i < COUNT(export_formats); ++i) {
						if (ImGui::Selectable(export_formats[i], desired_region_export_format == i)) {
							desired_region_export_format = i;
						}
					}
					ImGui::EndCombo();
				}

				if (desired_region_export_format == 0) {
					if (ImGui::TreeNodeEx("Encoding options", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
						ImGui::SliderInt("JPEG encoding quality", &tiff_export_jpeg_quality, 0, 100);
						bool prefer_rgb = tiff_export_desired_color_space == TIFF_PHOTOMETRIC_RGB;
						if (ImGui::Checkbox("Use RGB encoding (instead of YCbCr)", &prefer_rgb)) {
							tiff_export_desired_color_space = prefer_rgb ? TIFF_PHOTOMETRIC_RGB : TIFF_PHOTOMETRIC_YCBCR;
						}
					}

				}

                if (ImGui::TreeNodeEx("Output resolution", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
                    if (ImGui::Checkbox("Match input resolution", &tiff_export_match_input_resolution)) {
                        tiff_export_mpp = image->mpp_x;
                    }
                    if (tiff_export_match_input_resolution) {
                        ImGui::BeginDisabled();
                    }
                    float magnification = (0.25f * 40.0f) / tiff_export_mpp;
                    if (ImGui::SliderFloat("Magnification", &magnification, 1.0f,  (0.25f * 40.0f) / image->mpp_x, "%g", 0)) {
                        tiff_export_mpp = (0.25f * 40.0f) / magnification;
                    }
                    if (ImGui::InputFloat("Resolution (µm/pixel)", &tiff_export_mpp, 0.0f, 0.0f, "%g", 0)) {}
                    if (tiff_export_match_input_resolution) {
                        ImGui::EndDisabled();
                    }
                }

				ImGui::NewLine();

				if (display_export_annotations_checkbox) {
					ImGui::Checkbox("Also export annotations", &also_export_annotations);
					if (also_export_annotations) {
						if (ImGui::TreeNodeEx("Annotation export options", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {


							bool dummy_checkbox_value = false;
							bool* checkbox_value_ptr = &allow_coordinates_outside_region;
							if (!also_export_annotations) {
								ImGui::BeginDisabled();
								checkbox_value_ptr = &dummy_checkbox_value;
							}
							ImGui::Checkbox("Allow coordinates to extend outside selected region", checkbox_value_ptr);

							if (!also_export_annotations) {
								ImGui::EndDisabled();
							}
						}
					}
					ImGui::NewLine();
				}
			}

		}
		ImGui::EndChild(); // end of top area -- now start drawing bottom area


		const char* name_hint = "output";
		if (arrlen(app_state->loaded_images) > 0) {
			for (i32 i = 0; i < arrlen(app_state->loaded_images); ++i) {
				image_t* image = app_state->loaded_images[i];
				if (image->name[0] != '\0') {
					size_t buffer_size = sizeof(image->name);
					char* new_name_hint = (char*)alloca(buffer_size);
					copy_cstring(new_name_hint, image->name, buffer_size);
					// Strip filename extension
					size_t len = strlen(new_name_hint);
					i32 append_pos = len;
					for (i32 pos = len-1; pos >= 1; --pos) {
						if (new_name_hint[pos] == '.') {
							append_pos = pos;
							break;
						}
					}
					new_name_hint[append_pos] = '\0';
					// TODO: allow custom postfix?
					// add '_export'
					copy_cstring(new_name_hint + append_pos, global_export_region_filename_postfix, buffer_size - append_pos);
					name_hint = new_name_hint;
				}
			}
		}

		const char* filename_extension_hint = "";
		if (desired_region_export_format == 0) {
#if APPLE
			// macOS does not seem to like tile TIFF files in the Finder (will sometimes stop responding,
			// at least on my system). So choose the .ptif file extension by default as an alternative.
			filename_extension_hint = ".ptif";
#else
			filename_extension_hint = ".tiff";
#endif
		} else if (desired_region_export_format == 1) {
			filename_extension_hint = ".jpeg";
		} else if (desired_region_export_format == 2) {
			filename_extension_hint = ".png";
		}

		char filename_hint[512];
		snprintf(filename_hint, sizeof(filename_hint)-1, "%s%s", name_hint, filename_extension_hint);

		char* filename_buffer = global_export_save_as_filename;
		size_t filename_buffer_size = sizeof(global_export_save_as_filename);

		ImGui::InputTextWithHint("##export_region_output_filename", filename_hint, filename_buffer, filename_buffer_size);
		ImGui::SameLine();
		if (save_file_dialog_open || ImGui::Button("Browse...")) {
			if (save_file_dialog(app_state, filename_buffer, filename_buffer_size, "BigTIFF (*.tiff)\0*.tiff;*.tif;*.ptif\0All\0*.*\0Text\0*.TXT\0", filename_hint)) {
				size_t filename_len = strlen(filename_buffer);
				if (filename_len > 0) {
					const char* extension = get_file_extension(filename_buffer);
					if (!(strcasecmp(extension, "tiff") == 0 || strcasecmp(extension, "tif") == 0 || strcasecmp(extension, "ptif") == 0)) {
						// if extension incorrect, append it at the end
						i64 remaining_len = filename_buffer_size - filename_len;
						copy_cstring(filename_buffer + filename_len, ".tiff", remaining_len);
					}
				} else {
//					console_print_verbose("Export region: save file dialog returned 0\n");
				}
			}
		}

		static bool is_overwrite_confirm_dialog_open;
		if (ImGui::Button("Export", ImVec2(120, 0)) || is_overwrite_confirm_dialog_open) {
			if (filename_buffer[0] == '\0') {
				snprintf(filename_buffer, filename_buffer_size-1, "%s%s", get_active_directory(app_state), filename_hint);
			}
			bool proceed_with_export = true;

			static bool need_overwrite_confirm_dialog;
			if (!is_overwrite_confirm_dialog_open) {
				if (file_exists(filename_buffer)) {
					need_overwrite_confirm_dialog = true;
				}
			}
			if (need_overwrite_confirm_dialog) {
				ImGui::OpenPopup("Overwrite existing file?##export_region");
				is_overwrite_confirm_dialog_open = true;
				need_overwrite_confirm_dialog = false;
			}
			if (is_overwrite_confirm_dialog_open) {
				proceed_with_export = false;
				gui_make_next_window_appear_in_center_of_screen();
				if (ImGui::BeginPopupModal("Overwrite existing file?##export_region", NULL, ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::Text("Overwrite existing file '%s'?\n\n", filename_buffer);

					if (ImGui::Button("Overwrite", ImVec2(120, 0))) {
						is_overwrite_confirm_dialog_open = false;
						proceed_with_export = true;
						ImGui::CloseCurrentPopup();
					}
					ImGui::SetItemDefaultFocus();
					ImGui::SameLine();
					if (ImGui::Button("Cancel", ImVec2(120, 0))) {
						is_overwrite_confirm_dialog_open = false;
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}

			if (proceed_with_export) {
				switch(image->backend) {
					case IMAGE_BACKEND_OPENSLIDE:
					case IMAGE_BACKEND_DICOM:
					case IMAGE_BACKEND_ISYNTAX:
					case IMAGE_BACKEND_MRXS:
					case IMAGE_BACKEND_TIFF: {
						u32 export_flags = 0;
						if (display_export_annotations_checkbox) {
							if (also_export_annotations) {
								export_flags |= EXPORT_FLAGS_ALSO_EXPORT_ANNOTATIONS;
							}
							if (!allow_coordinates_outside_region) {
								export_flags |= EXPORT_FLAGS_PUSH_ANNOTATION_COORDINATES_INWARD;
							}
						}
						u32 export_tile_width = (image->tile_width == image->tile_height) ? image->tile_width : WSI_TILE_DIM;
						// Export TIFF by resampling level 0 and reconstructing the pyramid.
						begin_export_cropped_bigtiff_with_resample(app_state, image, scene->crop_bounds,
						                                           scene->selection_pixel_bounds,
						                                           filename_buffer, export_tile_width,
						                                           tiff_export_desired_color_space,
						                                           tiff_export_jpeg_quality, export_flags, !tiff_export_match_input_resolution,
						                                           V2F(tiff_export_mpp, tiff_export_mpp));
						gui_add_modal_progress_bar_popup("Exporting region...", &global_tiff_export_progress, false);
					} break;
					default: {
						gui_add_modal_message_popup("Error##draw_export_region_dialog",
						                            "This image backend is currently not supported for exporting a region.\n");
						console_print_error("Error: image backend not supported for exporting a region\n");
					}
				}
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		ImGui::SetItemDefaultFocus();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}


		ImGui::EndGroup();

		ImGui::EndPopup();
	}
}

static void draw_mouse_pos_overlay(app_state_t* app_state, bool* p_open) {
	scene_t* scene = &app_state->scene;

	static int corner = 0;
	ImGuiIO& io = ImGui::GetIO();
	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (corner != -1)
	{
		const float PAD = 10.0f;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImVec2 work_pos = viewport->WorkPos; // Use work area to avoid menu-bar/task-bar, if any!
		ImVec2 work_size = viewport->WorkSize;
		ImVec2 window_pos, window_pos_pivot;
		window_pos.x = (corner & 1) ? (work_pos.x + work_size.x - PAD) : (work_pos.x + PAD);
		window_pos.y = (corner & 2) ? (work_pos.y + work_size.y - PAD) : (work_pos.y + PAD);
		window_pos_pivot.x = (corner & 1) ? 1.0f : 0.0f;
		window_pos_pivot.y = (corner & 2) ? 1.0f : 0.0f;
		ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
		window_flags |= ImGuiWindowFlags_NoMove;
	}
	ImGui::SetNextWindowBgAlpha(0.65f); // Transparent background
	if (ImGui::Begin("Mouse pos overlay", p_open, window_flags))
	{
        static int level = 0;
        if (ImGui::IsMousePosValid()) {
			ImGui::Text("Mouse Position: (%.1f,%.1f)", io.MousePos.x, io.MousePos.y);
		} else {
			ImGui::Text("Mouse Position: <invalid>");
		}
		// TODO: how to check if a scene is enabled?
		if (arrlen(app_state->loaded_images) > 0) {
			ImGui::Text("Scene Position: (%.1f,%.1f)", scene->mouse.x, scene->mouse.y);
            image_t* image = app_state->loaded_images[0];
            v2i pixel_pos = world_pos_to_pixel_pos(scene->mouse, image->mpp_x, level);
            ImGui::Text("Pixel Position: (%d,%d)", pixel_pos.x, pixel_pos.y);
            v2i clicked_pixel_pos = world_pos_to_pixel_pos(scene->left_clicked_pos, image->mpp_x, level);
            ImGui::Text("Clicked Position: (%d,%d)", clicked_pixel_pos.x, clicked_pixel_pos.y);
            v2i clicked_offset = v2i_subtract(pixel_pos, clicked_pixel_pos);
            ImGui::Text("Clicked Offset: (%d,%d)", clicked_offset.x, clicked_offset.y);
		} else {
			ImGui::Text("Scene Position: <invalid>");
		}

		if (ImGui::BeginPopupContextWindow())
		{
			if (ImGui::MenuItem("Custom",       NULL, corner == -1)) corner = -1;
			if (ImGui::MenuItem("Top-left",     NULL, corner == 0)) corner = 0;
			if (ImGui::MenuItem("Top-right",    NULL, corner == 1)) corner = 1;
			if (ImGui::MenuItem("Bottom-left",  NULL, corner == 2)) corner = 2;
			if (ImGui::MenuItem("Bottom-right", NULL, corner == 3)) corner = 3;
            ImGui::Separator();

            if (ImGui::SliderInt("Level", &level, 0, 10)) {}
			if (p_open && ImGui::MenuItem("Close")) *p_open = false;
			ImGui::EndPopup();
		}

		ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
		v2f transformed_pos = world_pos_to_screen_pos(scene, scene->mouse);
		draw_list->AddCircle(transformed_pos, 20.0f, ImGui::GetColorU32(IM_COL32(70, 70, 70, 255)), 24, 2.0f);

	}
	ImGui::End();
}

void save_changes_modal(app_state_t* app_state, annotation_set_t* annotation_set) {
	if (show_save_quit_prompt) {
		ImGui::OpenPopup("Save changes?");
		show_save_quit_prompt = false;
	}
	gui_make_next_window_appear_in_center_of_screen();
	if (ImGui::BeginPopupModal("Save changes?", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("There are unsaved changes to the currently loaded annotations.\nProceed?\n\n");
		ImGui::Separator();

		//static int unused_i = 0;
		//ImGui::Combo("Combo", &unused_i, "Delete\0Delete harder\0");

//		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
//		ImGui::Checkbox("Don't ask me next time", &dont_ask_to_delete_annotations);
//		ImGui::PopStyleVar();

		if (ImGui::Button("Save", ImVec2(120, 0)) || was_key_pressed(app_state->input, KEY_Return)) {
			save_annotations(app_state, annotation_set, true, false);
			show_save_quit_prompt = false;
			is_program_running = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		if (ImGui::Button("Don't save", ImVec2(120, 0))) {
			show_save_quit_prompt = false;
			is_program_running = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			show_save_quit_prompt = false;
			need_quit = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void gui_draw_open_uri_window(app_state_t* app_state) {
    ImGui::SetNextWindowPos(ImVec2(120, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(806, 382), ImGuiCond_FirstUseEver);

    ImGui::Begin("Open URI", &show_open_uri_window);

    ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
    bool entered = false;

    if (ImGui::IsWindowAppearing() && !ImGui::IsAnyItemActive()) {
        ImGui::SetKeyboardFocusHere();
    }
    entered = entered || ImGui::InputTextEx("##URI", "Enter URI here", remote_uri, sizeof(remote_uri), ImVec2(-FLT_MIN, 0), input_flags);
    if (ImGui::TreeNodeEx("Supported links: Slide Score API (optionally prepended by https:// or slidescore://)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::BulletText("*.slidescore.com/image/1");
		ImGui::BulletText("*.slidescore.com/Image/Details?imageId=1");
		ImGui::BulletText("*.slidescore.com/i/1/.../SlideScoreMetadata.json");
        ImGui::TextWrapped("Slide Score links require an API token (except for QuPath metadata links).");
        ImGui::TreePop();
    }

    static char token_buf[4096];
    static bool first_time = true;
    if (first_time) {
        app_read_slide_score_api_key(token_buf, sizeof(token_buf));
        first_time = false;
    }
    static bool save_api_key = true;
    static bool is_api_key_dirty = false;
    static char open_uri_status[512] = "";
    if (ImGui::TreeNodeEx("API token", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
        if (ImGui::Checkbox("Save API key", &save_api_key)) {}
        ImGuiInputTextFlags api_key_input_flags = 0;
        if (ImGui::InputTextEx("##API_token", "Enter API token here", token_buf, sizeof(token_buf), ImVec2(-FLT_MIN, 0), api_key_input_flags)) {
            is_api_key_dirty = true;
        }

    }

    static bool pressed_connect;
    if (entered || ImGui::Button("Connect")) {
        pressed_connect = true;
        if (save_api_key && is_api_key_dirty) {
            app_write_slide_score_api_key(token_buf);
            is_api_key_dirty = false;
        }
        if (!slide_score_try_open_uri(app_state, remote_uri, token_buf)) {
            http_response_t* response = open_remote_uri(app_state, remote_uri, token_buf);
            if (response) {
                debug_slide_score_api_handle_response((const char *) (response->buffer.data), response->content_length, SLIDE_SCORE_API_GET_IMAGE_METADATA);
                http_response_destroy(response);
            }
        }
        const char* slide_score_status = slide_score_get_last_status();
        if (slide_score_status && slide_score_status[0]) {
            snprintf(open_uri_status, sizeof(open_uri_status), "%s", slide_score_status);
        } else {
            snprintf(open_uri_status, sizeof(open_uri_status), "Use the console (F3) to view connection results.");
        }
    }
    if (pressed_connect) {
        ImGui::TextWrapped("%s", open_uri_status);
    }
    ImGui::End();
}

static void profiler_flamegraph_getter(float* start, float* end, ImU8* level, const char** caption, const void* data, int idx) {
	const profiler_frame_t* frame = (const profiler_frame_t*)data;
	if (idx < 0 || idx >= PROFILER_SECTION_COUNT) return;
	const profiler_section_t* s = &frame->sections[idx];
	if (start) *start = (s->start > 0 && frame->frame_start > 0) ? get_seconds_elapsed(frame->frame_start, s->start) * 1000.0f : 0.0f;
	if (end)   *end   = (s->end   > 0 && frame->frame_start > 0) ? get_seconds_elapsed(frame->frame_start, s->end)   * 1000.0f : 0.0f;
	if (level)   *level   = s->level;
	if (caption) *caption = profiler_section_names[idx];
}

void draw_profiler_window(app_state_t* app_state) {
	static i32 pinned_frame = -1;

	if (!ImGui::Begin("Profiler", &show_profiler_window)) {
		ImGui::End();
		return;
	}

	if (ImGui::Button(global_profiler.paused ? "Resume" : "Pause")) {
		global_profiler.paused = !global_profiler.paused;
		if (!global_profiler.paused) pinned_frame = -1;
	}
	ImGui::SameLine();

	i32 display_idx = (pinned_frame >= 0) ? pinned_frame : global_profiler.displayed_frame;
	const profiler_frame_t* frame = &global_profiler.frames[display_idx];

	float frame_ms = (frame->frame_end > 0 && frame->frame_start > 0)
		? get_seconds_elapsed(frame->frame_start, frame->frame_end) * 1000.0f : 0.0f;
	float fps = frame_ms > 0.0f ? 1000.0f / frame_ms : 0.0f;
	ImGui::Text("%.2f ms  |  %.1f FPS%s", frame_ms, fps, global_profiler.paused ? "  [PAUSED]" : "");

	ImGui::Separator();

	// Frametime history graph
	const float graph_height = 56.0f;
	const float max_display_ms = 33.3f;
	const float target_ms = 1000.0f / 60.0f;
	// Frame pacing and timer sampling fluctuate slightly around the ideal refresh
	// interval. Do not flag a clean vsync-paced frame as an overrun.
	const float warning_ms = target_ms + 0.5f;

	ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	float canvas_w = ImGui::GetContentRegionAvail().x;
	ImVec2 canvas_size = ImVec2(canvas_w, graph_height);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	draw_list->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(20, 20, 20, 220));
	draw_list->AddRect(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(80, 80, 80, 255));

	float target_y = canvas_pos.y + canvas_size.y * (1.0f - target_ms / max_display_ms);
	draw_list->AddLine(ImVec2(canvas_pos.x, target_y), ImVec2(canvas_pos.x + canvas_size.x, target_y), IM_COL32(80, 200, 80, 100), 1.0f);

	const i32 buf_size = PROFILER_FRAME_BUFFER_SIZE;
	float bar_w = canvas_w / (float)buf_size;

	bool clicked_graph = false;
	i32 clicked_frame_idx = -1;
	ImVec2 mouse_pos = ImGui::GetIO().MousePos;
	bool mouse_in_graph = mouse_pos.x >= canvas_pos.x && mouse_pos.x < canvas_pos.x + canvas_w &&
	                      mouse_pos.y >= canvas_pos.y && mouse_pos.y < canvas_pos.y + canvas_size.y;

	if (mouse_in_graph && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
		clicked_graph = true;
		float rel_x = (mouse_pos.x - canvas_pos.x) / canvas_w;
		clicked_frame_idx = (i32)(rel_x * buf_size);
		if (clicked_frame_idx < 0) clicked_frame_idx = 0;
		if (clicked_frame_idx >= buf_size) clicked_frame_idx = buf_size - 1;
	}

	for (i32 i = 0; i < buf_size; ++i) {
		const profiler_frame_t* f = &global_profiler.frames[i];
		float ft = (f->frame_end > 0 && f->frame_start > 0)
			? get_seconds_elapsed(f->frame_start, f->frame_end) * 1000.0f : 0.0f;

		float bar_h = ft / max_display_ms * canvas_size.y;
		if (bar_h > canvas_size.y) bar_h = canvas_size.y;

		float x0 = canvas_pos.x + i * bar_w;
		float x1 = x0 + (bar_w >= 2.0f ? bar_w - 1.0f : bar_w);
		float y1 = canvas_pos.y + canvas_size.y;
		float y0 = y1 - bar_h;

		bool is_selected = (i == display_idx);
		ImU32 color;
		if (is_selected) {
			color = IM_COL32(255, 255, 80, 255);
		} else if (ft > target_ms * 1.5f) {
			color = IM_COL32(220, 70, 70, 220);
		} else if (ft > warning_ms) {
			color = IM_COL32(220, 170, 60, 220);
		} else {
			color = IM_COL32(70, 170, 70, 220);
		}

		if (bar_h > 0.0f) {
			draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);
		}
	}

	// Transparent button to capture interactions over the graph area
	ImGui::InvisibleButton("##framegraph", canvas_size);

	if (clicked_graph && clicked_frame_idx >= 0) {
		pinned_frame = clicked_frame_idx;
		global_profiler.paused = true;
	}

	if (ImGui::IsItemHovered()) {
		float rel_x = (mouse_pos.x - canvas_pos.x) / canvas_w;
		i32 hover_idx = (i32)(rel_x * buf_size);
		if (hover_idx < 0) hover_idx = 0;
		if (hover_idx >= buf_size) hover_idx = buf_size - 1;
		const profiler_frame_t* hf = &global_profiler.frames[hover_idx];
		float hft = (hf->frame_end > 0 && hf->frame_start > 0)
			? get_seconds_elapsed(hf->frame_start, hf->frame_end) * 1000.0f : 0.0f;
		ImGui::SetTooltip("Frame %d: %.2f ms (%.1f FPS)\nClick to inspect", hover_idx, hft, hft > 0.0f ? 1000.0f / hft : 0.0f);
	}

	if (!global_profiler.paused) pinned_frame = -1;

	// Flamegraph
	ImGui::Separator();
	ImGui::TextUnformatted("Main Thread");

	if (frame->frame_start > 0) {
		float scale_max = (frame->frame_end > 0 && frame->frame_end > frame->frame_start)
			? get_seconds_elapsed(frame->frame_start, frame->frame_end) * 1000.0f
			: FLT_MAX;
		ImGuiWidgetFlameGraph::PlotFlame("CPU",
			profiler_flamegraph_getter,
			(const void*)frame,
			PROFILER_SECTION_COUNT,
			0,
			NULL,
			0.0f,
			scale_max,
			ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
	} else {
		ImGui::TextDisabled("No frame data yet.");
	}

	ImGui::End();
}

void gui_draw(app_state_t* app_state, input_t* input, i32 client_width, i32 client_height) {
	ImGuiIO &io = ImGui::GetIO();

	gui_want_capture_mouse = io.WantCaptureMouse;
	gui_want_capture_keyboard = io.WantCaptureKeyboard;

	// TODO: check if cursor is in client area before taking over control of the cursor from ImGui
	if (gui_want_capture_mouse) {
		// Cursor is handled by ImGui
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	} else {
		// We are updating the cursor ourselves
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		update_cursor();
	}

	if (show_menu_bar) gui_draw_main_menu_bar(app_state);
    if (show_open_uri_window) gui_draw_open_uri_window(app_state);

	if (show_open_remote_window) {
		ImGui::SetNextWindowPos(ImVec2(120, 100), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(256, 156), ImGuiCond_FirstUseEver);

		ImGui::Begin("Open remote", &show_open_remote_window);

		ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue;
		bool entered = false;
		entered = entered || ImGui::InputText("Hostname", remote_hostname, sizeof(remote_hostname), input_flags);
		entered = entered || ImGui::InputText("Port", remote_port, sizeof(remote_port), input_flags);
		entered = entered || ImGui::InputText("Filename", remote_filename, sizeof(remote_filename), input_flags);
		if (entered || ImGui::Button("Connect")) {
			const char* ext = get_file_extension(remote_filename);
#if DO_DEBUG
			if (strcmp(remote_filename, "test_google.html") == 0) {
				i32 bytes_read = 0;
				u8* read_buffer = do_http_request(remote_hostname, atoi(remote_port), "/test", &bytes_read, 0);
				if (read_buffer) {
					FILE* test_out = fopen("test_google2.html", "wb");
					fwrite(read_buffer, bytes_read, 1, test_out);
					fclose(test_out);
					free(read_buffer);
				}
			}
			else
#endif
			// Open as 'slide'
			if (open_remote_slide(app_state, remote_hostname, atoi(remote_port), remote_filename)) {
				show_open_remote_window = false; // success!
			}


		}
		ImGui::End();
	}


	// 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
	if (show_demo_window) {
		ImGui::ShowDemoWindow(&show_demo_window);
	}

	if (show_debugging_window) {

		ImGui::SetNextWindowPos(ImVec2(120,100), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(350,500), ImGuiCond_FirstUseEver);

		if (ImGui::Begin("Debugging", &show_debugging_window)) {
//			ImGui::TextUnformatted("Worker threads");
			ImGui::SliderInt("Worker threads", thread_pool_get_active_worker_thread_count_ptr(&global_thread_pool), 1, thread_pool_get_worker_thread_count(&global_thread_pool));
			ImGui::SliderInt("Min level display", &global_lowest_scale_to_render, 0, 16);
			ImGui::SliderInt("Max level display", &global_highest_scale_to_render, 0, 16);
			ImGui::SliderFloat("Rotation", &app_state->scene.rotation, -1.0f * IM_PI, 1.0f * IM_PI);
			if (ImGui::Button("Reset rotation")) {
				app_state->scene.rotation = 0.0f;
				app_state->scene.sin_rotation = 0.0f;
				app_state->scene.cos_rotation = 1.0f;
			}
            ImGui::Checkbox("Draw WSI image", &app_state->scene.draw_wsi_image);
            ImGui::Checkbox("Draw WSI background", &app_state->scene.draw_wsi_background);
            ImGui::Checkbox("Draw label image", &app_state->scene.draw_label_image);
            ImGui::Checkbox("Draw macro image", &app_state->scene.draw_macro_image);
            ImGui::Checkbox("Draw tile outlines", &app_state->scene.draw_outlines);
            ImGui::Checkbox("Draw clip rectangles", &app_state->scene.draw_envelopes);
			ImGui::Separator();
			ImGui::Checkbox("Use annotation mip coordinates", &annotation_enable_mip_coordinates);
			if (ImGui::Checkbox("Cache annotation draw lists", &enable_annotation_drawlist_cache)) {
				gui_mark_extra_drawlists_modified();
			}
			if (ImGui::Checkbox("Merge extra draw lists", &enable_merged_extra_drawlists)) {
				gui_mark_extra_drawlists_modified();
			}
			if (ImGui::Checkbox("Persistent extra draw-list buffers", &enable_persistent_extra_drawlist_buffers)) {
				gui_mark_extra_drawlists_modified();
			}
			ImGui::Text("Annotation mip level: %d", annotation_mip_current_level);
			ImGui::SliderInt("Min annotation mip", &annotation_mip_min_level, -1, ANNOTATION_MIP_LEVEL_COUNT - 1);
			ImGui::SliderInt("Max annotation mip", &annotation_mip_max_level, -1, ANNOTATION_MIP_LEVEL_COUNT - 1);
			if (annotation_mip_min_level > annotation_mip_max_level) {
				annotation_mip_max_level = annotation_mip_min_level;
			}

            // Options for adjusting level offsets
            if (arrlen(app_state->loaded_images) > 0) {
                image_t* image = app_state->loaded_images[0];
                if (ImGui::TreeNodeEx("Adjust level offsets", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog)) {
                    for (i32 i = 0; i < image->level_count; ++i) {
                        level_image_t* level_image = image->level_images + i;
                        float offset_in_pixels = level_image->origin_offset.x / image->mpp_x;
                        char label[64];
                        snprintf(label, sizeof(label), "Level %d", i);
                        if (ImGui::DragFloat(label, &offset_in_pixels, 0.125f)) {
                            float offset_in_um = offset_in_pixels * image->mpp_x;
                            level_image->origin_offset.x = offset_in_um;
                            level_image->origin_offset.y = offset_in_um;
                        }
                    }
                }
            }
		}
		ImGui::End();
	}

	if (show_image_options_window) {
		static int counter = 0;

		ImGui::SetNextWindowPos(ImVec2(25, 50), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(388,409), ImGuiCond_FirstUseEver);

		ImGui::Begin("Image options", &show_image_options_window);

		if (ImGui::Button("Reset zoom")) {
			app_state->scene.need_zoom_reset = true;
		}
		ImGui::SameLine();
		float zoom_objective_factor = 40.0f * exp2f(-app_state->scene.zoom.pos);
		ImGui::Text("Current zoom level: %.1f (%gx)", app_state->scene.zoom.pos, zoom_objective_factor);

		if (ImGui::SliderFloat("Zoom level", &app_state->scene.zoom.pos, -5.0f, 15.0f, "%.1f")) {
			zoom_update_pos(&app_state->scene.zoom, app_state->scene.zoom.pos);
		}
        if (ImGui::SliderInt("Min zoom (near)", &viewer_min_level, -5, 15)) {
			viewer_max_level = ATLEAST(viewer_min_level, viewer_max_level);
		}
		if (ImGui::SliderInt("Max zoom (far)", &viewer_max_level, -5, 15)) {
			viewer_min_level = ATMOST(viewer_min_level, viewer_max_level);
		}
		ImGui::NewLine();

		ImGui::Checkbox("Use image adjustments", &app_state->use_image_adjustments);

		bool disable_gui = !app_state->use_image_adjustments;
		if (disable_gui) {
			ImGui::BeginDisabled();
		}

		ImGui::SliderFloat("Black level", &app_state->black_level, 0.0f, 1.0f);
		ImGui::SliderFloat("White level", &app_state->white_level, 0.0f, 1.0f);

		if (disable_gui) {
			ImGui::EndDisabled();
		}

		ImGui::NewLine();
		ImGui::Checkbox("Filter transparent color", &app_state->scene.use_transparent_filter);
		disable_gui = !app_state->scene.use_transparent_filter;
		if (disable_gui) {
			ImGui::BeginDisabled();
		}
		ImGui::ColorEdit3("Transparent color", (float*) &app_state->scene.transparent_color); // Edit 3 floats representing a color
		ImGui::SliderFloat("Tolerance", &app_state->scene.transparent_tolerance, 0.0f, 0.2f);
		if (disable_gui) {
			ImGui::EndDisabled();
		}


		ImGui::NewLine();
		ImGui::ColorEdit3("Background color", (float*) &app_state->clear_color); // Edit 3 floats representing a color


		ImGui::End();
	}

	if (show_general_options_window) {

		ImGui::SetNextWindowPos(ImVec2(120, 100), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);

		ImGui::Begin("General options", &show_general_options_window);
		bool general_options_changed = false;
		static ImGuiComboFlags combo_flags = 0;
		ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
		if (ImGui::BeginTabBar("General options tab bar", tab_bar_flags)) {
			if (ImGui::BeginTabItem("Appearance")) {
				ImGui::Text("Graphical user interface");
				// General BeginCombo() API, you have full control over your selection data and display type.
				// (your selection data could be an index, a pointer to the object, an id for the object, a flag stored in the object itself, etc.)
				const char* items[] = {"Dark (default)", "Light", "Classic"};
				static i32 style_color = 0;
				int old_style_color = style_color;

//		        ImGui::Text("User interface colors");               // Display some text (you can use a format strings too)
				if (ImGui::BeginCombo("Colors##user interface", items[style_color],
				                      combo_flags)) // The second parameter is the label previewed before opening the combo.
				{
					for (int n = 0; n < IM_ARRAYSIZE(items); n++) {
						bool is_selected = (style_color == n);
						if (ImGui::Selectable(items[n], is_selected))
							style_color = n;
						if (style_color)
							ImGui::SetItemDefaultFocus();   // Set the initial focus when opening the combo (scrolling + for keyboard navigation support in the upcoming navigation branch)
					}
					ImGui::EndCombo();

					if (style_color != old_style_color) {
						if (style_color == 0) {
							ImGui::StyleColorsDark();
						} else if (style_color == 1) {
							ImGui::StyleColorsLight();
						} else if (style_color == 2) {
							ImGui::StyleColorsClassic();
						}
					}
				}
				ImGui::SliderFloat("Opacity##user interface", &ImGui::GetStyle().Alpha, 0.20f, 1.0f, "%.2f"); // Not exposing zero here so user doesn't "lose" the UI (zero alpha clips all widgets). But application code could have a toggle to switch between zero and non-zero.

				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Controls")) {
				ImGui::TextUnformatted("Panning speed");
				general_options_changed |= ImGui::SliderInt("Mouse sensitivity", &app_state->mouse_sensitivity, 1, 50);
				general_options_changed |= ImGui::SliderInt("Keyboard sensitivity", &app_state->keyboard_base_panning_speed, 1, 50);
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Advanced")) {
				bool prev_use_builtin_tiff_backend = app_state->use_builtin_tiff_backend;
				bool prev_use_native_mrxs_backend = global_use_native_mrxs_backend;
				bool prev_is_vsync_enabled = is_vsync_enabled;

				ImGui::Text("\nTIFF backend");
//		        ImGui::Checkbox("Prefer built-in TIFF backend over OpenSlide", &use_builtin_tiff_backend);
				const char* tiff_backends[] = {"Built-in", "OpenSlide"};
				if (ImGui::BeginCombo("##tiff_backend", tiff_backends[1 - app_state->use_builtin_tiff_backend],
				                      combo_flags)) // The second parameter is the label previewed before opening the combo.
				{
					if (ImGui::Selectable(tiff_backends[0], app_state->use_builtin_tiff_backend)) {
						app_state->use_builtin_tiff_backend = true;
					}
					if (app_state->use_builtin_tiff_backend) ImGui::SetItemDefaultFocus();
					if (is_openslide_available) {
						if (ImGui::Selectable(tiff_backends[1], !app_state->use_builtin_tiff_backend)) {
							app_state->use_builtin_tiff_backend = false;
						}
						if (!app_state->use_builtin_tiff_backend) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

                ImGui::Text("\nMRXS backend");
                const char* mrxs_backends[] = {"Built-in", "OpenSlide"};
                if (ImGui::BeginCombo("##mrxs_backend", mrxs_backends[1 - global_use_native_mrxs_backend],
                                      combo_flags)) // The second parameter is the label previewed before opening the combo.
                {
                    if (ImGui::Selectable(mrxs_backends[0], global_use_native_mrxs_backend)) {
                        global_use_native_mrxs_backend = true;
                    }
                    if (global_use_native_mrxs_backend) ImGui::SetItemDefaultFocus();
                    if (is_openslide_available) {
                        if (ImGui::Selectable(mrxs_backends[1], !global_use_native_mrxs_backend)) {
                            global_use_native_mrxs_backend = false;
                        }
                        if (!global_use_native_mrxs_backend) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

				ImGui::NewLine();

				ImGui::Checkbox("Enable Vsync", &is_vsync_enabled);
				if (prev_is_vsync_enabled != is_vsync_enabled) {
					set_swap_interval(is_vsync_enabled ? 1 : 0);
				}
				general_options_changed |= (prev_use_builtin_tiff_backend != app_state->use_builtin_tiff_backend);
				general_options_changed |= (prev_use_native_mrxs_backend != global_use_native_mrxs_backend);
				general_options_changed |= (prev_is_vsync_enabled != is_vsync_enabled);
				ImGui::EndTabItem();
			}


			ImGui::EndTabBar();
		}

		ImGui::End();
		if (general_options_changed) {
			viewer_save_options(app_state);
		}
	}

	if (show_annotations_window || show_annotation_group_assignment_window) {
		draw_annotations_window(app_state, input);
	}
//  draw_annotation_inspector(app_state, &app_state->scene.annotation_set);
//	draw_annotation_palette_window();



	if (show_layers_window) {
		draw_layers_window(app_state);
	}
	draw_dicom_slides_window(app_state);

	if (show_about_window) {
		ImGui::Begin("About " APP_TITLE, &show_about_window, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

		ImGui::TextUnformatted(APP_TITLE " - a whole-slide image viewer for digital pathology");
		ImGui::Text("Author: Pieter Valkema\n");
		ImGui::TextUnformatted("Version: " APP_VERSION );


		ImGui::Text("\nLicense information:\nThis program is free software: you can redistribute it and/or modify\n"
		            "  it under the terms of the GNU General Public License as published by\n"
		            "  the Free Software Foundation, either version 3 of the License, or\n"
		            "  (at your option) any later version.\n\n");
		if (ImGui::Button("View releases on GitHub")) {
#if WINDOWS
			ShellExecuteW(0, 0, L"https://github.com/amspath/slidescape/releases", 0, 0 , SW_SHOW );
#elif APPLE
			system("open https://github.com/amspath/slidescape/releases");
#elif LINUX
			system("gio open https://github.com/amspath/slidescape/releases");
#endif
		};

		ImGui::End();
	}

	if (show_mouse_pos_overlay) {
		draw_mouse_pos_overlay(app_state, &show_mouse_pos_overlay);
	}

	if (show_console_window) {
		draw_console_window(app_state, "Console", &show_console_window);
	}

	if (show_profiler_window) {
		draw_profiler_window(app_state);
	}

	// Draw modal popups last
	draw_export_region_dialog(app_state);
	annotation_modal_dialog(app_state, &app_state->scene.annotation_set);
	save_changes_modal(app_state, &app_state->scene.annotation_set);
	gui_do_modal_popups();

#if (LINUX || APPLE)
	gui_draw_open_file_dialog(app_state);
#endif
}

static i32 ticks_to_delay_before_first_dialog = 1;

void gui_do_modal_popups() {

	if (ticks_to_delay_before_first_dialog > 0) {
		// For whatever reason, modal dialogs might not open properly on the first frame of the program.
		// So, display the first dialog (e.g. "Could not load file") only after a short delay.
		--ticks_to_delay_before_first_dialog;
	} else {
		if (arrlen(gui_modal_stack) > 0) {
			gui_modal_popup_t* popup = gui_modal_stack;
			if (popup->need_open) {
				ImGui::OpenPopup(popup->title);

				// Check that the popup is actually open!
				ImGuiContext& g = *GImGui;
				ImGuiWindow* window = g.CurrentWindow;
				const ImGuiID id = window->GetID(popup->title);
				if (ImGui::IsPopupOpen(id, ImGuiPopupFlags_None)) {
					popup->need_open = false;
				}
			}
			gui_make_next_window_appear_in_center_of_screen();
			if (popup->type == GUI_MODAL_MESSAGE) {
				if (ImGui::BeginPopupModal(popup->title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
					ImGui::TextUnformatted(popup->message);
					if (ImGui::Button("OK", ImVec2(120, 0))) {
						popup->need_open = false;
						ImGui::CloseCurrentPopup();
						arrdel(gui_modal_stack, 0);
					}
					ImGui::EndPopup();
				}
			} else if (popup->type == GUI_MODAL_PROGRESS_BAR) {
				if (ImGui::BeginPopupModal(popup->title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
					float progress = (popup->progress) ? *popup->progress : 0.0f;
					float difference = progress - popup->visual_progress;
					if (difference > 0.0f) {
						popup->visual_progress += ATLEAST(MIN(difference, 0.002f), difference * 0.1f);
						if (popup->visual_progress > progress) popup->visual_progress = progress;
					} else if (difference < 0.0f) {
						difference = -difference;
						popup->visual_progress -= ATLEAST(MIN(difference, 0.002f), difference * 0.1f);
						if (popup->visual_progress < progress) popup->visual_progress = progress;
					}
					ImGui::ProgressBar(popup->visual_progress, ImVec2(0.0f, 0.0f), "");
					if (progress >= 1.0f || (popup->allow_cancel && ImGui::Button("Cancel", ImVec2(120, 0)))) {
						ImGui::CloseCurrentPopup();
						arrdel(gui_modal_stack, 0);
					}
					ImGui::EndPopup();
				}
			}
		}
	}
}

void gui_add_modal_message_popup(const char* title, const char* message, ...) {
	gui_modal_popup_t popup = {};
	popup.type = GUI_MODAL_MESSAGE;
	popup.title = title;

	va_list args;
	va_start(args, message);
	vsnprintf(popup.message, sizeof(popup.message)-1, message, args);
	popup.message[sizeof(popup.message)-1] = 0;
	va_end(args);

	popup.need_open = true;
	arrpush(gui_modal_stack, popup);
}

void gui_add_modal_progress_bar_popup(const char* title, float* progress, bool allow_cancel) {
	gui_modal_popup_t popup = {};
	popup.type = GUI_MODAL_PROGRESS_BAR;
	popup.title = title;
	popup.progress = progress;
	popup.allow_cancel = allow_cancel;
	popup.need_open = true;
	arrpush(gui_modal_stack, popup);
}

static void a_very_long_task(i32 logical_thread_index, void* userdata) {
	for (i32 i = 0; i < 10; ++i) {
		platform_sleep(1000);
		global_progress_bar_test_progress = (i + 1) * 0.1f;
	}
}

// for testing the progress bar popup
void begin_a_very_long_task() {
	thread_pool_submit_task(&global_thread_pool, a_very_long_task, NULL, 0);
}
