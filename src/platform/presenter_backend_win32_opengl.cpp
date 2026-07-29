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
#include "presenter_backend.h"
#include "presenter_imgui_opengl_cached.h"
#include "win32_graphical_app.h"
#include "viewer.h"
#include "renderer.h"
#include "stringutils.h"

#include <glad/glad.h>
#include <GL/wgl.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

HGLRC glrcs[MAX_THREAD_COUNT];

const char* wgl_extensions_string;

PFNWGLSWAPINTERVALEXTPROC       wglSwapIntervalEXT = NULL;
PFNWGLGETSWAPINTERVALEXTPROC    wglGetSwapIntervalEXT = NULL;
PFNWGLGETEXTENSIONSSTRINGEXTPROC wglGetExtensionsStringEXT = NULL;
PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = NULL;
PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB = NULL;
PFNSETPIXELFORMATPROC wglSetPixelFormat = NULL;
PFNDESCRIBEPIXELFORMATPROC wglDescribePixelFormat = NULL;
PFNCHOOSEPIXELFORMATPROC wglChoosePixelFormat = NULL;
PFNWGLGETPROCADDRESSPROC wglGetProcAddress_alt = NULL; // need to rename, because these are already declared in wingdi.h
PFNWGLCREATECONTEXTPROC wglCreateContext_alt = NULL;
PFNWGLMAKECURRENTPROC wglMakeCurrent_alt = NULL;
PFNWGLDELETECONTEXTPROC wglDeleteContext_alt = NULL;
PFNWGLGETCURRENTDCPROC wglGetCurrentDC_alt = NULL;
PFNSWAPBUFFERSPROC wglSwapBuffers = NULL;

static HMODULE opengl32_dll_handle;

// https://stackoverflow.com/questions/589064/how-to-enable-vertical-sync-in-opengl/589232#589232
static bool win32_wgl_extension_supported(const char *extension_name) {
	ASSERT(wgl_extensions_string);
	bool supported = (strstr(wgl_extensions_string, extension_name) != NULL);
	return supported;
}

static void* gl_get_proc_address(const char *name) {
	void* proc = (void*) GetProcAddress(opengl32_dll_handle, name);
	if (!proc) {
		proc = (void*) wglGetProcAddress_alt(name);
		if (!proc) {
			console_print("Error initalizing OpenGL: could not load proc '%s'.\n", name);
		}
	}
	return proc;
}

#if USE_OPENGL_DEBUG_CONTEXT

#define skip_if_already_encountered(x) { \
	static bool id_##x##_already_encountered;\
    if (id == x) { if (id_##x##_already_encountered) return; else  id_##x##_already_encountered = true; } }

static void GLAPIENTRY opengl_debug_message_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                                     const char* message, const void* user_param)
{
	skip_if_already_encountered(131154); // (NVIDIA) Pixel-path performance warning: Pixel transfer is synchronized with 3D rendering.

	char severity_unknown_string[32];
	const char* severity_string = NULL;
	switch (severity) {
		case GL_DEBUG_SEVERITY_HIGH: severity_string =  "HIGH"; break;
		case GL_DEBUG_SEVERITY_MEDIUM: severity_string = "MEDIUM"; break;
		case GL_DEBUG_SEVERITY_LOW: severity_string = "LOW"; break;
		case GL_DEBUG_SEVERITY_NOTIFICATION: severity_string = "NOTIFICATION"; break;
		default: {
			snprintf(severity_unknown_string, sizeof(severity_unknown_string), "0x%x", severity);
			severity_string = severity_unknown_string;
		} break;
	}

	char type_unknown_string[32];
	const char* type_string = NULL;
	switch (type) {
		case GL_DEBUG_TYPE_ERROR: type_string = "ERROR"; break;
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: type_string = "DEPRECATED_BEHAVIOR"; break;
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: type_string = "UNDEFINED_BEHAVIOR"; break;
		case GL_DEBUG_TYPE_PORTABILITY: type_string = "PORTABILITY"; break;
		case GL_DEBUG_TYPE_PERFORMANCE: type_string = "PERFORMANCE"; break;
		case GL_DEBUG_TYPE_OTHER: type_string = "OTHER"; break;
		case GL_DEBUG_TYPE_MARKER: type_string = "MARKER"; break;
		case GL_DEBUG_TYPE_PUSH_GROUP: type_string = "PUSH_GROUP"; break;
		case GL_DEBUG_TYPE_POP_GROUP: type_string = "POP_GROUP"; break;
		default: {
			snprintf(type_unknown_string, sizeof(type_unknown_string), "0x%x", type);
			type_string = type_unknown_string;
		} break;
	}

	console_print("GL CALLBACK: type = %s, id = %d, severity = %s,\n    MESSAGE: %s\n", type_string, id, severity_string, message);
}

#undef skip_if_already_encountered

#endif //USE_OPENGL_DEBUG_CONTEXT

static bool win32_init_opengl(HWND window, HINSTANCE instance, const char* window_class_name, bool use_software_renderer) {
	i64 debug_start = get_clock();

	// Set environment variable needed for Mesa3D software driver support
	// See:
	// https://github.com/pal1000/mesa-dist-win (section 'OpenGL context configuration override')
	// https://gitlab.freedesktop.org/mesa/mesa/-/blob/master/src/mesa/main/version.c
	// https://stackoverflow.com/questions/4788398/changes-via-setenvironmentvariable-do-not-take-effect-in-library-that-uses-geten
	_putenv_s("MESA_GL_VERSION_OVERRIDE", "4.3FC");

	if (use_software_renderer) {
		char dll_path[4096];
		GetModuleFileNameA(NULL, dll_path, sizeof(dll_path));
		char* pos = (char*)one_past_last_slash(dll_path, sizeof(dll_path));
		i32 chars_left = sizeof(dll_path) - (pos - dll_path);
		copy_cstring(pos, "softwarerenderer", chars_left);
		SetDllDirectoryA(dll_path);
		opengl32_dll_handle = LoadLibraryA("opengl32software.dll");
		use_fast_rendering = true;
		// NOTE: We again need access to the extra dll file in win32_init_gui(),
		// because ImGui has its own OpenGL loader code.
		// So, don't call SetDllDirectoryA() which ordinarily we would want to do here.
//		SetDllDirectoryA(NULL);
	} else {
		opengl32_dll_handle = LoadLibraryA("opengl32.dll");

	}

	if (!opengl32_dll_handle) {
		win32_diagnostic("LoadLibraryA");
		console_print("Error initializing OpenGL: failed to load opengl32.dll.\n");
		return false;
	}


	// Initializing OpenGL on Windows is somewhat tricky.
	// https://mariuszbartosik.com/opengl-4-x-initialization-in-windows-without-a-framework/

	// Dynamically import all of the functions we need from opengl32.dll and all the wgl functions
	wglGetProcAddress_alt = (PFNWGLGETPROCADDRESSPROC) GetProcAddress(opengl32_dll_handle, "wglGetProcAddress");
	if (!wglGetProcAddress_alt) {
		console_print("Error initalizing OpenGL: could not load proc 'wglGetProcAddress'.\n");
		fatal_error();
	}
	wglCreateContext_alt = (PFNWGLCREATECONTEXTPROC) gl_get_proc_address("wglCreateContext");
	if (!wglCreateContext_alt) { fatal_error(); }
	wglMakeCurrent_alt = (PFNWGLMAKECURRENTPROC) gl_get_proc_address("wglMakeCurrent");
	if (!wglMakeCurrent_alt) { fatal_error(); }
	wglDeleteContext_alt = (PFNWGLDELETECONTEXTPROC) gl_get_proc_address("wglDeleteContext");
	if (!wglDeleteContext_alt) { fatal_error(); }
	wglGetCurrentDC_alt = (PFNWGLGETCURRENTDCPROC) gl_get_proc_address("wglGetCurrentDC");
	if (!wglGetCurrentDC_alt) { fatal_error(); }
	wglSetPixelFormat = (PFNSETPIXELFORMATPROC) gl_get_proc_address("wglSetPixelFormat");
	if (!wglSetPixelFormat) { fatal_error(); }
	wglDescribePixelFormat = (PFNDESCRIBEPIXELFORMATPROC) gl_get_proc_address("wglDescribePixelFormat");
	if (!wglDescribePixelFormat) { fatal_error(); }
	wglChoosePixelFormat = (PFNCHOOSEPIXELFORMATPROC) gl_get_proc_address("wglChoosePixelFormat");
	if (!wglChoosePixelFormat) { fatal_error(); }
	wglSwapBuffers = (PFNSWAPBUFFERSPROC) gl_get_proc_address("wglSwapBuffers");
	if (!wglSwapBuffers) { fatal_error(); }

	// We want to create an OpenGL context using wglCreateContextAttribsARB, instead of the regular wglCreateContext.
	// Unfortunately, that's considered an OpenGL extension. Therefore, we first need to create a "dummy" context
	// (and destroy it again) solely for the purpose of creating the actual OpenGL context that we want.
	// (Why bother? Mostly because we want to have worker threads for loading textures in the background. For this
	// to work properly, we want multiple OpenGL contexts (one per thread), and we want all the contexts
	// to be able share their resources, which requires creating them with wglCreateContextAttribsARB.)

	// Set up a 'dummy' window, because Win32 requires a device context (DC) coupled to a window for creating
	// OpenGL contexts.
	HWND dummy_window = CreateWindowExA(0, window_class_name, "dummy window",
			                            0/*WS_DISABLED*/, 0, 0, 640, 480, NULL, NULL, instance, 0);
	HDC dummy_dc = GetDC(dummy_window);

	PIXELFORMATDESCRIPTOR desired_pixel_format = {};
	desired_pixel_format.nSize = sizeof(desired_pixel_format);
	desired_pixel_format.nVersion = 1;
	desired_pixel_format.iPixelType = PFD_TYPE_RGBA;
	desired_pixel_format.dwFlags = PFD_SUPPORT_OPENGL|PFD_DRAW_TO_WINDOW|PFD_DOUBLEBUFFER;
	desired_pixel_format.cColorBits = 32;
	desired_pixel_format.cAlphaBits = 8;
	desired_pixel_format.cStencilBits = 8;
	desired_pixel_format.iLayerType = PFD_MAIN_PLANE;

	int suggested_pixel_format_index = wglChoosePixelFormat(dummy_dc, &desired_pixel_format);
	PIXELFORMATDESCRIPTOR suggested_pixel_format;
	wglDescribePixelFormat(dummy_dc, suggested_pixel_format_index, sizeof(suggested_pixel_format), &suggested_pixel_format);
	if (use_software_renderer) {
		if (!wglSetPixelFormat(dummy_dc, suggested_pixel_format_index, &suggested_pixel_format)) {
			win32_diagnostic("wglSetPixelFormat");
			fatal_error();
		}
	} else {
		if (!SetPixelFormat(dummy_dc, suggested_pixel_format_index, &suggested_pixel_format)) {
			win32_diagnostic("SetPixelFormat");
			fatal_error();
		}
	}

	// Create the OpenGL context for the main thread.
	HGLRC dummy_glrc = wglCreateContext_alt(dummy_dc);
	if (!dummy_glrc) {
		win32_diagnostic("wglCreateContext");
		fatal_error();
	}

	if (!wglMakeCurrent_alt(dummy_dc, dummy_glrc)) {
		win32_diagnostic("wglMakeCurrent");
		fatal_error();
	}

	// Before we go any further, try to report the supported OpenGL version from openg32.dll
	PFNGLGETSTRINGPROC temp_glGetString = (PFNGLGETSTRINGPROC) gl_get_proc_address("glGetString");
	if (temp_glGetString == NULL) {
		fatal_error();
	}

	char version_string[256] = {};
	char* version_string_retrieved = (char*)temp_glGetString(GL_VERSION);
	copy_cstring(version_string, version_string_retrieved, sizeof(version_string));
	if (use_software_renderer) {
		console_print("OpenGL software renderer: %s\n", version_string);
	} else {
		console_print("OpenGL supported version: %s\n", version_string);
	}

	bool is_opengl_version_supported = false;
	i32 major_required = 3;
	i32 minor_required = 3;
	if (strlen(version_string) >= 3) {
		i32 major_version = version_string[0] - '0';
		i32 minor_version = version_string[2] - '0';
		if (major_version > major_required || (major_version == major_required && minor_version >= minor_required)) {
			is_opengl_version_supported = true;
		}
	}

	// To test the software renderer
//	if (!use_software_renderer) is_opengl_version_supported = false;

	if (!is_opengl_version_supported) {

		bool success = false;
		if (!use_software_renderer) {
			// If the hardware renderer isn't working (maybe on a remote desktop environment?),
			// we could try using the Mesa3D software renderer instead (if available).
			wglMakeCurrent_alt(dummy_dc, NULL);
			wglDeleteContext_alt(dummy_glrc);

			wglSwapIntervalEXT = NULL;
			wglGetSwapIntervalEXT = NULL;
			wglGetExtensionsStringEXT = NULL;
			wglCreateContextAttribsARB = NULL;
			wglChoosePixelFormatARB = NULL;
			wglSetPixelFormat = NULL;
			wglDescribePixelFormat = NULL;
			wglChoosePixelFormat = NULL;
			wglGetProcAddress_alt = NULL;
			wglCreateContext_alt = NULL;
			wglMakeCurrent_alt = NULL;
			wglDeleteContext_alt = NULL;
			wglGetCurrentDC_alt = NULL;
			wglSwapBuffers = NULL;

			FreeLibrary(opengl32_dll_handle);
			opengl32_dll_handle = NULL;

			global_is_using_software_renderer = true;
			success = win32_init_opengl(window, instance, window_class_name, true);
		}

		if (!success) {
			char buf[4096];
			snprintf(buf, sizeof(buf), "Error: OpenGL version is insufficient.\n"
									   "Required: %d.%d\n\n"
									   "Available on this system:\n%s", major_required, minor_required, version_string);
			console_print_error("%s\n", buf);
			message_box(window, buf);
			exit(0);
		}
		return success;
	}

	if (strstr(version_string, "NVIDIA")) {
	    is_nvidia_gpu = true;
	}

	// Now try to load the extensions we will need.

#define GET_WGL_PROC(proc, type) do { proc = (type) wglGetProcAddress_alt(#proc); } while(0)

	GET_WGL_PROC(wglGetExtensionsStringEXT, PFNWGLGETEXTENSIONSSTRINGEXTPROC);
	if (!wglGetExtensionsStringEXT) {
		console_print("Error: wglGetExtensionsStringEXT is unavailable\n");
		fatal_error();
	}
	wgl_extensions_string = wglGetExtensionsStringEXT();
//	puts(wgl_extensions_string);

	if (win32_wgl_extension_supported("WGL_EXT_swap_control")) {
		GET_WGL_PROC(wglSwapIntervalEXT, PFNWGLSWAPINTERVALEXTPROC);
		GET_WGL_PROC(wglGetSwapIntervalEXT, PFNWGLGETSWAPINTERVALEXTPROC);
	} else {
		console_print("Error: WGL_EXT_swap_control is unavailable\n");
		fatal_error();
	}

	if (win32_wgl_extension_supported("WGL_ARB_create_context")) {
		GET_WGL_PROC(wglCreateContextAttribsARB, PFNWGLCREATECONTEXTATTRIBSARBPROC);
	} else {
		console_print("Error: WGL_ARB_create_context is unavailable\n");
		fatal_error();
	}

	if (win32_wgl_extension_supported("WGL_ARB_pixel_format")) {
		GET_WGL_PROC(wglChoosePixelFormatARB, PFNWGLCHOOSEPIXELFORMATARBPROC);
	} else {
		console_print("Error: WGL_ARB_pixel_format is unavailable\n");
		fatal_error();
	}

#undef GET_WGL_PROC

	// Now we're finally ready to create the real context.
	// https://mariuszbartosik.com/opengl-4-x-initialization-in-windows-without-a-framework/

	const int pixel_attribs[] = {
			WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
			WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
			WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
			WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
			WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
			WGL_COLOR_BITS_ARB, 32,
			WGL_ALPHA_BITS_ARB, 8,
			WGL_DEPTH_BITS_ARB, 24,
			WGL_STENCIL_BITS_ARB, 8,
			WGL_SAMPLE_BUFFERS_ARB, GL_TRUE,
			WGL_SAMPLES_ARB, 4,
			0
	};

	HDC dc = GetDC(window);

	u32 num_formats = 0;
	suggested_pixel_format_index = 0;
	memset_zero(&suggested_pixel_format);
	bool32 status = wglChoosePixelFormatARB(dc, pixel_attribs, NULL, 1, &suggested_pixel_format_index, &num_formats);
	if (!status || num_formats == 0) {
		console_print("wglChoosePixelFormatARB() failed.");
		fatal_error();
	}
	wglDescribePixelFormat(dc, suggested_pixel_format_index, sizeof(suggested_pixel_format), &suggested_pixel_format);
	if (!wglSetPixelFormat(dc, suggested_pixel_format_index, &suggested_pixel_format)) {
		win32_diagnostic("wglSetPixelFormat");
	}

#if USE_OPENGL_DEBUG_CONTEXT
	int context_attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
			WGL_CONTEXT_MINOR_VERSION_ARB, 3,
			WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
			WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB, // Ask for a debug context
			0
	};
#else
	int context_attribs[] = {
			WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
			WGL_CONTEXT_MINOR_VERSION_ARB, 3,
			WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
			0
	};
#endif

	glrcs[0] = wglCreateContextAttribsARB(dc, NULL, context_attribs);
	if (glrcs[0] == NULL) {
		console_print("wglCreateContextAttribsARB() failed.");
		fatal_error();
	}


	// Delete the dummy context and start using the real one.
	wglMakeCurrent_alt(NULL, NULL);
	wglDeleteContext_alt(dummy_glrc);
	ReleaseDC(dummy_window, dummy_dc);
	DestroyWindow(dummy_window);
	if (!wglMakeCurrent_alt(dc, glrcs[0])) {
		win32_diagnostic("wglMakeCurrent");
		fatal_error();
	}
	ReleaseDC(window, dc);

	// Now, get the OpenGL proc addresses using GLAD.
	if (!gladLoadGLLoader((GLADloadproc) gl_get_proc_address)) {
		console_print("Error initializing OpenGL: failed to initialize GLAD.\n");
		fatal_error();
	}

	// Hack: Enabling synchronous debug output (on NVIDIA drivers) apparently disables OpenGL driver multithreading.
	// We badly want to disable this, because we are already heavily using threads in the program!
	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);



	// Create separate OpenGL contexts for each worker thread, so that they can load textures (etc.) on the fly
#if USE_MULTIPLE_OPENGL_CONTEXTS
	ASSERT(logical_cpu_count > 0);
	for (i32 thread_index = 1; thread_index < total_thread_count; ++thread_index) {
		HGLRC glrc = wglCreateContextAttribsARB(dc, glrcs[0], context_attribs);
		if (!glrc) {
			console_print("Thread %d: wglCreateContextAttribsARB() failed.", thread_index);
			fatal_error();
		}
/*		if (!wglShareLists(glrcs[0], glrc)) {
			console_print("Thread %d: ", thread_index);
			win32_diagnostic("wglShareLists");
			fatal_error();
		}*/
		glrcs[thread_index] = glrc;
	}
#endif

	// Try to enable debug output on the main thread.
#if USE_OPENGL_DEBUG_CONTEXT
	i32 gl_context_flags;
	glGetIntegerv(GL_CONTEXT_FLAGS, &gl_context_flags);
	if (gl_context_flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
//		console_print("enabling debug output for thread %d...\n", 0);
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(opengl_debug_message_callback, 0);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, NULL, false);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW, 0, NULL, true);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, NULL, true);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, NULL, true);
	}
	glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_OTHER, 0,
	                     GL_DEBUG_SEVERITY_HIGH, -1, "OpenGL debugging enabled");
#endif

	// debug
	console_print("Initialized OpenGL in %g seconds.\n", get_seconds_elapsed(debug_start, get_clock()));

	glDrawBuffer(GL_BACK);

	return true;
}

static bool presenter_win32_opengl_init_window(const presenter_window_desc_t* desc, window_handle_t* out_window, void** out_present_handle) {
	HWND window = desc->existing_window;
	HINSTANCE instance = (HINSTANCE)desc->native_instance;
	const char* window_class_name = desc->native_window_class_name;
	bool result = win32_init_opengl(window, instance, window_class_name, false);
	if (result && out_window) {
		*out_window = window;
	}
	if (result && out_present_handle) {
		*out_present_handle = wglGetCurrentDC_alt();
	}
	return result && out_window && *out_window && out_present_handle && *out_present_handle;
}

static void presenter_win32_opengl_init_imgui(app_state_t* app_state, window_handle_t window) {
	(void)app_state;
	(void)window;
	ImGui_ImplOpenGL3_Init(NULL, global_is_using_software_renderer ? "opengl32software.dll" : "opengl32.dll");
}

static void presenter_win32_opengl_imgui_new_frame() {
	ImGui_ImplOpenGL3_NewFrame();
}

static void presenter_win32_opengl_render_imgui_draw_data(ImDrawData* draw_data) {
	ImGui_ImplOpenGL3_RenderDrawData(draw_data);
}

static bool presenter_win32_opengl_render_cached_imgui_draw_data(ImDrawData* draw_data, u32 generation) {
	return presenter_imgui_opengl_render_cached_draw_data(draw_data, generation);
}

static void presenter_win32_opengl_set_swap_interval(int interval) {
	if (wglSwapIntervalEXT) {
		wglSwapIntervalEXT(interval);
	}
}

static int presenter_win32_opengl_get_refresh_rate(window_handle_t window, void* present_handle) {
	(void)window;
	int refresh_rate = 0;
	HDC glrc_hdc = (HDC)present_handle;
	if (glrc_hdc) {
		refresh_rate = GetDeviceCaps(glrc_hdc, VREFRESH);
	}
	return refresh_rate;
}

static void presenter_win32_opengl_get_drawable_size(window_handle_t window, i32* out_width, i32* out_height) {
	RECT rect;
	GetClientRect(window, &rect);
	if (out_width) *out_width = rect.right - rect.left;
	if (out_height) *out_height = rect.bottom - rect.top;
}

static void presenter_win32_opengl_present(window_handle_t window, void* present_handle) {
	(void)window;
	HDC glrc_hdc = (HDC)present_handle;
	if (glrc_hdc) {
		wglSwapBuffers(glrc_hdc);
	}
}

static void presenter_win32_opengl_shutdown(window_handle_t window, void* present_handle) {
	(void)window;
	(void)present_handle;
	presenter_imgui_opengl_destroy_cached_draw_data();
}

presenter_backend_t presenter_opengl_get_backend() {
	static const presenter_backend_t backend = {
		presenter_win32_opengl_init_window,
		presenter_win32_opengl_init_imgui,
		presenter_win32_opengl_imgui_new_frame,
		presenter_win32_opengl_render_imgui_draw_data,
		presenter_win32_opengl_render_cached_imgui_draw_data,
		presenter_win32_opengl_set_swap_interval,
		presenter_win32_opengl_get_refresh_rate,
		presenter_win32_opengl_get_drawable_size,
		presenter_win32_opengl_present,
		presenter_win32_opengl_shutdown,
	};
	return backend;
}
