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
#include "presenter_imgui_opengl_cached.h"

#include "imgui.h"

#include OPENGL_H

typedef struct cached_imgui_opengl_renderer_t {
	bool initialized;
	u32 generation;
	u32 shader;
	u32 vbo;
	u32 ibo;
	u32 vao;
	i32 attrib_pos;
	i32 attrib_uv;
	i32 attrib_color;
	i32 uniform_texture;
	i32 uniform_projection;
} cached_imgui_opengl_renderer_t;

static cached_imgui_opengl_renderer_t cached_imgui_renderer;

static bool cached_imgui_compile_shader(u32 shader, const char* source) {
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	i32 success = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		char log[1024] = {};
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		console_print_error("Cached ImGui shader compile failed: %s\n", log);
		return false;
	}
	return true;
}

static bool cached_imgui_init_renderer(void) {
	if (cached_imgui_renderer.initialized) {
		return true;
	}

	const char* vertex_shader_source =
		"#version 130\n"
		"uniform mat4 ProjMtx;\n"
		"in vec2 Position;\n"
		"in vec2 UV;\n"
		"in vec4 Color;\n"
		"out vec2 Frag_UV;\n"
		"out vec4 Frag_Color;\n"
		"void main() {\n"
		"    Frag_UV = UV;\n"
		"    Frag_Color = Color;\n"
		"    gl_Position = ProjMtx * vec4(Position.xy, 0.0, 1.0);\n"
		"}\n";

	const char* fragment_shader_source =
		"#version 130\n"
		"uniform sampler2D Texture;\n"
		"in vec2 Frag_UV;\n"
		"in vec4 Frag_Color;\n"
		"out vec4 Out_Color;\n"
		"void main() {\n"
		"    Out_Color = Frag_Color * texture(Texture, Frag_UV.st);\n"
		"}\n";

	u32 vertex_shader = glCreateShader(GL_VERTEX_SHADER);
	u32 fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
	if (!cached_imgui_compile_shader(vertex_shader, vertex_shader_source) ||
	    !cached_imgui_compile_shader(fragment_shader, fragment_shader_source))
	{
		glDeleteShader(vertex_shader);
		glDeleteShader(fragment_shader);
		return false;
	}

	u32 shader = glCreateProgram();
	glAttachShader(shader, vertex_shader);
	glAttachShader(shader, fragment_shader);
	glLinkProgram(shader);
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	i32 link_success = 0;
	glGetProgramiv(shader, GL_LINK_STATUS, &link_success);
	if (!link_success) {
		char log[1024] = {};
		glGetProgramInfoLog(shader, sizeof(log), NULL, log);
		console_print_error("Cached ImGui shader link failed: %s\n", log);
		glDeleteProgram(shader);
		return false;
	}

	cached_imgui_renderer.shader = shader;
	cached_imgui_renderer.attrib_pos = glGetAttribLocation(shader, "Position");
	cached_imgui_renderer.attrib_uv = glGetAttribLocation(shader, "UV");
	cached_imgui_renderer.attrib_color = glGetAttribLocation(shader, "Color");
	cached_imgui_renderer.uniform_texture = glGetUniformLocation(shader, "Texture");
	cached_imgui_renderer.uniform_projection = glGetUniformLocation(shader, "ProjMtx");

	glGenVertexArrays(1, &cached_imgui_renderer.vao);
	glGenBuffers(1, &cached_imgui_renderer.vbo);
	glGenBuffers(1, &cached_imgui_renderer.ibo);

	cached_imgui_renderer.initialized = true;
	return true;
}

void presenter_imgui_opengl_destroy_cached_draw_data(void) {
	if (cached_imgui_renderer.vao) glDeleteVertexArrays(1, &cached_imgui_renderer.vao);
	if (cached_imgui_renderer.vbo) glDeleteBuffers(1, &cached_imgui_renderer.vbo);
	if (cached_imgui_renderer.ibo) glDeleteBuffers(1, &cached_imgui_renderer.ibo);
	if (cached_imgui_renderer.shader) glDeleteProgram(cached_imgui_renderer.shader);
	memset_zero(&cached_imgui_renderer);
}

bool presenter_imgui_opengl_render_cached_draw_data(ImDrawData* draw_data, u32 generation) {
	if (!draw_data || draw_data->CmdListsCount != 1 || draw_data->TotalVtxCount <= 0 || draw_data->TotalIdxCount <= 0) {
		return false;
	}
	if (!cached_imgui_init_renderer()) {
		return false;
	}

	ImDrawList* draw_list = draw_data->CmdLists[0];
	for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; ++cmd_i) {
		if (draw_list->CmdBuffer[cmd_i].UserCallback) {
			return false;
		}
	}

	int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
	int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
	if (fb_width <= 0 || fb_height <= 0) {
		return true;
	}

	GLenum last_active_texture; glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint*)&last_active_texture);
	GLuint last_program; glGetIntegerv(GL_CURRENT_PROGRAM, (GLint*)&last_program);
	GLuint last_texture; glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint*)&last_texture);
	GLuint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, (GLint*)&last_array_buffer);
	GLuint last_element_array_buffer; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, (GLint*)&last_element_array_buffer);
	GLuint last_vertex_array; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, (GLint*)&last_vertex_array);
	GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
	GLint last_scissor_box[4]; glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
	GLenum last_blend_src_rgb; glGetIntegerv(GL_BLEND_SRC_RGB, (GLint*)&last_blend_src_rgb);
	GLenum last_blend_dst_rgb; glGetIntegerv(GL_BLEND_DST_RGB, (GLint*)&last_blend_dst_rgb);
	GLenum last_blend_src_alpha; glGetIntegerv(GL_BLEND_SRC_ALPHA, (GLint*)&last_blend_src_alpha);
	GLenum last_blend_dst_alpha; glGetIntegerv(GL_BLEND_DST_ALPHA, (GLint*)&last_blend_dst_alpha);
	GLenum last_blend_equation_rgb; glGetIntegerv(GL_BLEND_EQUATION_RGB, (GLint*)&last_blend_equation_rgb);
	GLenum last_blend_equation_alpha; glGetIntegerv(GL_BLEND_EQUATION_ALPHA, (GLint*)&last_blend_equation_alpha);
	GLboolean last_enable_blend = glIsEnabled(GL_BLEND);
	GLboolean last_enable_cull_face = glIsEnabled(GL_CULL_FACE);
	GLboolean last_enable_depth_test = glIsEnabled(GL_DEPTH_TEST);
	GLboolean last_enable_stencil_test = glIsEnabled(GL_STENCIL_TEST);
	GLboolean last_enable_scissor_test = glIsEnabled(GL_SCISSOR_TEST);

	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_SCISSOR_TEST);

	glViewport(0, 0, fb_width, fb_height);
	float L = draw_data->DisplayPos.x;
	float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
	float T = draw_data->DisplayPos.y;
	float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
	const float ortho_projection[4][4] = {
		{ 2.0f / (R - L), 0.0f,           0.0f, 0.0f },
		{ 0.0f,           2.0f / (T - B), 0.0f, 0.0f },
		{ 0.0f,           0.0f,          -1.0f, 0.0f },
		{ (R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f },
	};

	glUseProgram(cached_imgui_renderer.shader);
	glUniform1i(cached_imgui_renderer.uniform_texture, 0);
	glUniformMatrix4fv(cached_imgui_renderer.uniform_projection, 1, GL_FALSE, &ortho_projection[0][0]);

	glBindVertexArray(cached_imgui_renderer.vao);
	glBindBuffer(GL_ARRAY_BUFFER, cached_imgui_renderer.vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cached_imgui_renderer.ibo);
	glEnableVertexAttribArray(cached_imgui_renderer.attrib_pos);
	glEnableVertexAttribArray(cached_imgui_renderer.attrib_uv);
	glEnableVertexAttribArray(cached_imgui_renderer.attrib_color);
	glVertexAttribPointer(cached_imgui_renderer.attrib_pos, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, pos));
	glVertexAttribPointer(cached_imgui_renderer.attrib_uv, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, uv));
	glVertexAttribPointer(cached_imgui_renderer.attrib_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void*)offsetof(ImDrawVert, col));

	if (cached_imgui_renderer.generation != generation) {
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)draw_list->VtxBuffer.Size * (int)sizeof(ImDrawVert), draw_list->VtxBuffer.Data, GL_STATIC_DRAW);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)draw_list->IdxBuffer.Size * (int)sizeof(ImDrawIdx), draw_list->IdxBuffer.Data, GL_STATIC_DRAW);
		cached_imgui_renderer.generation = generation;
	}

	ImVec2 clip_off = draw_data->DisplayPos;
	ImVec2 clip_scale = draw_data->FramebufferScale;
	for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; ++cmd_i) {
		const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
		ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
		ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
		if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
			continue;
		}

		glScissor((int)clip_min.x, (int)((float)fb_height - clip_max.y), (int)(clip_max.x - clip_min.x), (int)(clip_max.y - clip_min.y));
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->GetTexID());
		glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
		                         sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
		                         (void*)(intptr_t)(pcmd->IdxOffset * sizeof(ImDrawIdx)),
		                         (GLint)pcmd->VtxOffset);
	}

	if (last_program == 0 || glIsProgram(last_program)) glUseProgram(last_program);
	glBindTexture(GL_TEXTURE_2D, last_texture);
	glActiveTexture(last_active_texture);
	glBindVertexArray(last_vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
	glBlendEquationSeparate(last_blend_equation_rgb, last_blend_equation_alpha);
	glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
	if (last_enable_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (last_enable_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (last_enable_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (last_enable_stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
	if (last_enable_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
	glScissor(last_scissor_box[0], last_scissor_box[1], last_scissor_box[2], last_scissor_box[3]);

	return true;
}

