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
#include "renderer.h"
#include "tile_buffer_pool.h"
#include "renderer_opengl_shader.h"
#include "tile_cache.h"

#include OPENGL_H

#define RENDERER_OPENGL_IMPL
#include "renderer_opengl.h"

static void opengl_init_renderer(app_state_t* app_state);

static void init_draw_rect() {
	ASSERT(!rect_initialized);
	rect_initialized = true;

	glGenVertexArrays(1, &vao_rect);
	glBindVertexArray(vao_rect);

	glGenBuffers(1, &vbo_rect);
	glGenBuffers(1, &ebo_rect);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_rect);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_rect);

    // Expand the rectangle by a minimal amount to avoid occasional white lines between adjacent textures
    // (I think these are caused by floating point rounding errors)
#define VERT_EPSILON (0.00001f)
#define UV_EPSILON (0.00001f)

	static float vertices[] = {
			0.0f - VERT_EPSILON, 0.0f - VERT_EPSILON, 0.0f - VERT_EPSILON, 0.0f - UV_EPSILON, 0.0f - UV_EPSILON, // x, y, z,  u, v
			1.0f + VERT_EPSILON, 0.0f - VERT_EPSILON, 0.0f - VERT_EPSILON, 1.0f + UV_EPSILON, 0.0f - UV_EPSILON,
			0.0f - VERT_EPSILON, 1.0f + VERT_EPSILON, 0.0f - VERT_EPSILON, 0.0f - UV_EPSILON, 1.0f + UV_EPSILON,

//			0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
//			1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
			1.0f + VERT_EPSILON, 1.0f + VERT_EPSILON, 0.0f - VERT_EPSILON, 1.0f + UV_EPSILON, 1.0f + UV_EPSILON,
	};
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	static u16 indices[] = {
			0,1,2,1,2,3,
	};
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	u32 vertex_stride = 5 * sizeof(float);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertex_stride, (void*)0); // position coordinates
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vertex_stride, (void*)(3*sizeof(float))); // texture coordinates
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);

}

static void init_draw_normalized_quad() {
	static bool initialized = false;
	ASSERT(!initialized);
	initialized = true;

	glGenVertexArrays(1, &vao_screen);
	glBindVertexArray(vao_screen);

	glGenBuffers(1, &vbo_screen);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_screen);

	static float vertices[] = {
			-1.0f, -1.0f, 0.0f, 0.0f, // x, y, (z = 0,)  u, v
			+1.0f, -1.0f, 1.0f, 0.0f,
			-1.0f, +1.0f, 0.0f, 1.0f,

			-1.0f, +1.0f, 0.0f, 1.0f,
			+1.0f, -1.0f, 1.0f, 0.0f,
			+1.0f, +1.0f, 1.0f, 1.0f,
	};
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	u32 vertex_stride = 4 * sizeof(float);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vertex_stride, (void*)0); // position coordinates
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vertex_stride, (void*)(2*sizeof(float))); // texture coordinates
	glEnableVertexAttribArray(1);
}

static void opengl_draw_rect(renderer_texture_handle_t texture) {
	glBindVertexArray(vao_rect);
//	glUniform1i(basic_shader_u_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
}


pixel_transfer_state_t* renderer_submit_texture_upload(app_state_t *app_state, i32 width, i32 height,
                                                       i32 bytes_per_pixel, u8 *pixels, bool finalize) {
	pixel_transfer_state_t* transfer_state = app_state->pixel_transfer_states + app_state->next_pixel_transfer_to_submit;
	app_state->next_pixel_transfer_to_submit = (app_state->next_pixel_transfer_to_submit + 1) % COUNT(app_state->pixel_transfer_states);
	i64 buffer_size = width * height * bytes_per_pixel;
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, transfer_state->pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, buffer_size, NULL, GL_STREAM_DRAW);
	void* mapped_buffer = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	memcpy(mapped_buffer, pixels, buffer_size);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

	if (!finalize) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        transfer_state->texture_width = width;
        transfer_state->texture_height = height;
        transfer_state->need_finalization = true;

    } else {
        u32 texture = 0; //gl_gen_texture();
//        glEnable(GL_TEXTURE_2D);
        glGenTextures(1, &texture);
        transfer_state->texture = texture;
        transfer_state->texture_width = width;
        transfer_state->texture_height = height;
//	printf("Generated texture %d\n", texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, default_texture_mag_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, default_texture_min_filter);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        transfer_state->need_finalization = false;

    }
	return transfer_state;


}

void renderer_finalize_texture_upload(pixel_transfer_state_t* transfer_state) {
	if (transfer_state->need_finalization) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, transfer_state->pbo);

        u32 texture = 0; //gl_gen_texture();
//        glEnable(GL_TEXTURE_2D);
        glGenTextures(1, &texture);
        i32 width = transfer_state->texture_width;
        i32 height = transfer_state->texture_height;
//	printf("Generated texture %d\n", texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, default_texture_mag_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, default_texture_min_filter );
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        transfer_state->texture = texture;
        transfer_state->need_finalization = false;
	}

}

static u32 renderer_pixel_format_to_opengl(renderer_pixel_format_t pixel_format) {
	u32 result = GL_BGRA;
	switch (pixel_format) {
		case RENDERER_PIXEL_FORMAT_BGRA: result = GL_BGRA; break;
		case RENDERER_PIXEL_FORMAT_RGBA: result = GL_RGBA; break;
	}
	return result;
}

renderer_texture_handle_t renderer_create_texture(void* pixels, i32 width, i32 height, renderer_pixel_format_t pixel_format) {
	u32 opengl_pixel_format = renderer_pixel_format_to_opengl(pixel_format);
	u32 texture = 0; //gl_gen_texture();
	glGenTextures(1, &texture);
//	printf("Generated texture %d\n", texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, default_texture_mag_filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, default_texture_min_filter);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, opengl_pixel_format, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);

//	glGenerateMipmap(GL_TEXTURE_2D);
//	gl_diagnostic("glTexImage2D");
	return texture;
}

void renderer_upload_tile_on_worker_thread(image_t* image, void* tile_pixels, i32 scale, i32 tile_index, i32 tile_width, i32 tile_height) {


#if USE_MULTIPLE_OPENGL_CONTEXTS

	glEnable(GL_TEXTURE_2D);
	if (!threadlocal_thread_memory->pbo) {
		glGenBuffers(1, &threadlocal_thread_memory->pbo);
	}
	size_t pixel_memory_size = tile_width * tile_height * BYTES_PER_PIXEL;
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, threadlocal_thread_memory->pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, pixel_memory_size, NULL, GL_STREAM_DRAW);

	void* mapped_buffer = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);

//write data into the mapped buffer, possibly in another thread.
	memcpy(mapped_buffer, tile_pixels, pixel_memory_size);
	tile_buffer_release((u8*)tile_pixels, pixel_memory_size);

// after reading is complete back on the main thread
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, threadlocal_thread_memory->pbo);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

	//Sleep(5);

	u32 texture = 0; //gl_gen_texture();
//        glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tile_width, tile_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	glFinish();

	ASSERT(image);
	tile_cache_store_gpu_texture(image, scale, tile_index, texture);
#else
	tile_cache_result_t completion_task = {};
	completion_task.pixel_memory = (u8*)tile_pixels;
	completion_task.tile_width = tile_width;
	completion_task.tile_height = tile_height;
	completion_task.level = scale;
	completion_task.tile_index = tile_index;
	completion_task.resource_id = image ? image->resource_id : 0;
	completion_task.want_gpu_residency = true;
	//	console_print("[thread %d] Loaded tile: level=%d tile_x=%d tile_y=%d\n", logical_thread_index, level, tile_x, tile_y);
	if (!tile_cache_post_load_result(image, &completion_task)) {
		if (completion_task.pixel_memory) {
			tile_buffer_release(completion_task.pixel_memory,
			                    (size_t)tile_width * tile_height * BYTES_PER_PIXEL);
		}
	}
#endif

}

void renderer_destroy_texture(renderer_texture_handle_t texture) {
	glDeleteTextures(1, &texture);
}

static void maybe_resize_overlay(framebuffer_t* framebuffer, i32 width, i32 height) {
	if (framebuffer->width != width || framebuffer->height != height) {
		framebuffer->width = width;
		framebuffer->height = height;

		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->framebuffer);
		glBindTexture(GL_TEXTURE_2D, framebuffer->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL); // reallocate
		glBindRenderbuffer(GL_RENDERBUFFER, framebuffer->depth_stencil_rbo);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height); // allocate
		glBindFramebuffer(GL_FRAMEBUFFER, 0); // unbind
	}
}

static void init_layer_framebuffers(app_state_t* app_state) {
	ASSERT(!layer_framebuffers_initialized);
	layer_framebuffers_initialized = true;

	i32 width = app_state->client_viewport.w;
	i32 height = app_state->client_viewport.h;

	for (i32 framebuffer_index = 0; framebuffer_index < COUNT(layer_framebuffers); ++framebuffer_index) {
		framebuffer_t* framebuffer = layer_framebuffers + framebuffer_index;
		glGenFramebuffers(1, &framebuffer->framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->framebuffer);

		framebuffer->width = width;
		framebuffer->height = height;

		// Generate the texture
		glGenTextures(1, &framebuffer->texture);
		glBindTexture(GL_TEXTURE_2D, framebuffer->texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL); // allocate
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glBindTexture(GL_TEXTURE_2D, 0);

		// Attach the texture to currently bound overlay_framebuffer object
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, framebuffer->texture, 0);

		// We also want OpenGL to do depth and stencil testing -> add attachments to the overlay_framebuffer
		// We do not need to sample the depth and stencil buffers, so creating a renderbuffer object (RBO) is sufficient.
		glGenRenderbuffers(1, &framebuffer->depth_stencil_rbo);
		glBindRenderbuffer(GL_RENDERBUFFER, framebuffer->depth_stencil_rbo);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height); // allocate
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, framebuffer->depth_stencil_rbo);

		// Verify that the overlay_framebuffer is complete
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			console_print_error("OpenGL error (glCheckFramebufferStatus): overlay_framebuffer is not complete\n");
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0); // unbind
	}



	// Now that the overlay_framebuffer is complete, we can start rendering to it.
}

void renderer_begin_image_render(app_state_t* app_state, scene_t* scene, mat4x4 projection_view_matrix) {
	glUseProgram(basic_shader.program);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(basic_shader.u_tex, 0);

	glUniformMatrix4fv(basic_shader.u_projection_view_matrix, 1, GL_FALSE, &projection_view_matrix[0][0]);

	glUniform3fv(basic_shader.u_background_color, 1, (GLfloat *) &app_state->clear_color);
	if (app_state->use_image_adjustments) {
		glUniform1f(basic_shader.u_black_level, app_state->black_level);
		glUniform1f(basic_shader.u_white_level, app_state->white_level);
	} else {
		glUniform1f(basic_shader.u_black_level, 0.0f);
		glUniform1f(basic_shader.u_white_level, 1.0f);
	}
	glUniform1i(basic_shader.u_use_transparent_filter, scene->use_transparent_filter);
	glUniform1i(basic_shader.u_draw_outlines, scene->draw_outlines);
	if (scene->use_transparent_filter) {
		glUniform3fv(basic_shader.u_transparent_color, 1, (GLfloat *) &scene->transparent_color);
		glUniform1f(basic_shader.u_transparent_tolerance, scene->transparent_tolerance);
	}
}

void renderer_set_image_model_matrix(mat4x4 model_matrix) {
	glUniformMatrix4fv(basic_shader.u_model_matrix, 1, GL_FALSE, &model_matrix[0][0]);
}

void renderer_draw_textured_rect(renderer_texture_handle_t texture) {
	opengl_draw_rect(texture);
}

void renderer_disable_stencil_test() {
	glDisable(GL_STENCIL_TEST);
}

void renderer_begin_stencil_write() {
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	//glStencilMask(0xFF);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); // don't actually draw the stencil rectangle
	glDepthMask(GL_FALSE); // don't write to depth buffer
}

void renderer_end_stencil_write() {
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	//glStencilMask(0xFF);
	glStencilFunc(GL_LEQUAL, 2, 0xFF);
	//glDisable(GL_STENCIL_TEST);
}

void renderer_set_tile_blend_enabled(bool enabled) {
	if (enabled) {
		glEnable(GL_BLEND);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	} else {
		glDisable(GL_BLEND);
	}
}

void renderer_finish_image_render() {
	glDisable(GL_STENCIL_TEST);
}

void renderer_clear_render_target(v4f clear_color, i32 client_width, i32 client_height) {
//	glDrawBuffer(GL_BACK);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilMask(0xFF);
	glViewport(0, 0, client_width, client_height);
	glClearColor(clear_color.r, clear_color.g, clear_color.b, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void renderer_ensure_layer_render_targets(app_state_t* app_state) {
	if (!layer_framebuffers_initialized) {
		init_layer_framebuffers(app_state);
	}
}

void renderer_prepare_layer_render_target(i32 target_index, i32 width, i32 height, v4f clear_color) {
	ASSERT(target_index >= 0 && target_index < COUNT(layer_framebuffers));
	framebuffer_t* framebuffer = layer_framebuffers + target_index;
	maybe_resize_overlay(framebuffer, width, height);

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->framebuffer);
	renderer_clear_render_target(clear_color, width, height);
}

void renderer_bind_screen_render_target() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void renderer_final_blit_layers(float layer_time) {
	glUseProgram(finalblit_shader.program);
	glUniform1f(finalblit_shader.u_t, layer_time);
	glBindVertexArray(vao_screen);
	glDisable(GL_DEPTH_TEST); // because we want to make sure the quad always renders in front of everything else
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, layer_framebuffers[0].texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, layer_framebuffers[1].texture);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
}

renderer_texture_handle_t renderer_get_dummy_texture() {
	return dummy_texture;
}

renderer_texture_handle_t renderer_get_transparent_texture() {
	return transparent_texture;
}

void renderer_init(app_state_t* app_state) {
	opengl_init_renderer(app_state);
}

void renderer_set_viewport(i32 width, i32 height) {
	glViewport(0, 0, width, height);
}

void renderer_finish() {
	glFinish();
}

static void opengl_init_renderer(app_state_t* app_state) {

	if (use_fast_rendering) {
		default_texture_mag_filter = GL_NEAREST;
		default_texture_min_filter = GL_NEAREST_MIPMAP_NEAREST;
	}

	for (i32 i = 0; i < COUNT(app_state->pixel_transfer_states); ++i) {
		pixel_transfer_state_t* transfer_state = app_state->pixel_transfer_states + i;
		u32 pbo = 0;
		glGenBuffers(1, &pbo);
		transfer_state->pbo = pbo;
		transfer_state->initialized = true;
	}

	// Load the basic shader program (used to render the scene)
	basic_shader.program = opengl_load_basic_shader_program("shaders/basic.vert", "shaders/basic.frag");
	basic_shader.u_projection_view_matrix = opengl_get_uniform(basic_shader.program, "projection_view_matrix");
	basic_shader.u_model_matrix = opengl_get_uniform(basic_shader.program, "model_matrix");
	basic_shader.u_tex = opengl_get_uniform(basic_shader.program, "the_texture");
	basic_shader.u_black_level = opengl_get_uniform(basic_shader.program, "black_level");
	basic_shader.u_white_level = opengl_get_uniform(basic_shader.program, "white_level");
	basic_shader.u_background_color = opengl_get_uniform(basic_shader.program, "bg_color");
	basic_shader.u_transparent_color = opengl_get_uniform(basic_shader.program, "transparent_color");
	basic_shader.u_transparent_tolerance = opengl_get_uniform(basic_shader.program, "transparent_tolerance");
	basic_shader.u_use_transparent_filter = opengl_get_uniform(basic_shader.program, "use_transparent_filter");
	basic_shader.u_draw_outlines = opengl_get_uniform(basic_shader.program, "draw_outlines");
	basic_shader.attrib_location_pos = opengl_get_attrib(basic_shader.program, "pos");
	basic_shader.attrib_location_tex_coord = opengl_get_attrib(basic_shader.program, "tex_coord");

	// load the shader that blits different layers of the scene together
	finalblit_shader.program = opengl_load_basic_shader_program("shaders/finalblit.vert", "shaders/finalblit.frag");
	finalblit_shader.u_texture0 = opengl_get_uniform(finalblit_shader.program, "texture0");
	finalblit_shader.u_texture1 = opengl_get_uniform(finalblit_shader.program, "texture1");
	finalblit_shader.u_t = opengl_get_uniform(finalblit_shader.program, "t");
	finalblit_shader.attrib_location_pos = opengl_get_attrib(finalblit_shader.program, "pos");
	finalblit_shader.attrib_location_tex_coord = opengl_get_attrib(finalblit_shader.program, "tex_coord");

	glUseProgram(finalblit_shader.program);
	glUniform1i(finalblit_shader.u_texture0, 0);
	glUniform1i(finalblit_shader.u_texture1, 1);

	init_draw_normalized_quad();

#ifdef STRINGIFY_SHADERS
	opengl_write_stringified_shaders();
#endif
	init_draw_rect();

	u32 dummy_texture_color = MAKE_BGRA(255, 255, 0, 255);
	dummy_texture = renderer_create_texture(&dummy_texture_color, 1, 1, RENDERER_PIXEL_FORMAT_BGRA);

	// Make sure NVIDIA drivers don't complain about undefined base level for texture 0.
	glBindTexture(GL_TEXTURE_2D, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &dummy_texture_color);
	
	u32 transparent_texture_color = MAKE_BGRA(255, 255, 255, 0);
	transparent_texture = renderer_create_texture(&transparent_texture_color, 1, 1, GL_BGRA);

	glBindTexture(GL_TEXTURE_2D, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &transparent_texture_color);

}
