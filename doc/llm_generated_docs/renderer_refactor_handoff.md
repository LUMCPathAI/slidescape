# Renderer Refactor Handoff

This note captures the current state of the renderer abstraction work and the intended path toward an optional Vulkan backend.

## Current State

The Windows entry point no longer owns WGL/OpenGL setup directly. `win32_main.cpp` goes through `presenter.*`, and `presenter_backend_win32_opengl.cpp` owns the current WGL/OpenGL window/context/present path. `win32_gui.cpp` is renderer-neutral: it initializes the Win32 ImGui platform backend and delegates renderer-side ImGui setup/new-frame/rendering to `presenter_*`.

The Linux/macOS SDL2 entry point follows the same facade. `linux_main.cpp` goes through `presenter.*`, and `presenter_backend_sdl_opengl.cpp` owns the current SDL OpenGL window/context/loader/present path plus renderer-side ImGui OpenGL calls. `linux_gui.cpp` delegates swap interval changes to the presenter facade.

The core viewer no longer includes `OPENGL_H` or issues direct GL state calls. `viewer.cpp` calls `renderer.h`, which is currently implemented by `renderer_opengl.c`. The OpenGL code still performs the actual shader setup, framebuffer handling, texture upload, and quad drawing.

## Important Boundaries

- `src/platform/presenter.h`: public presenter facade used by Win32 and Linux/macOS platform code.
- `src/platform/presenter_backend.h`: backend vtable used by the presenter facade. Calls are direct after initialization; avoid per-frame or per-tile null checks.
- `src/core/renderer.h`: renderer-facing API used by core viewer code. This is the boundary to shape before implementing Vulkan.
- `src/core/renderer_opengl.c` and `.h`: current OpenGL implementation details and globals.
- `src/core/renderer_opengl_shader.c` and `.h`: OpenGL shader compilation/linking helpers. These are backend-private helpers and should not be used by core viewer code directly.

## Near-Term Plan

Before adding Vulkan, keep making `renderer.h` less OpenGL-shaped while preserving the existing OpenGL behavior:

1. Keep direct function calls for hot paths. Do not add runtime dispatch inside tile drawing unless there is a measured reason.
2. Defer a full command-buffer abstraction until after a small Vulkan prototype clarifies what data the Vulkan backend actually needs.
3. Consider replacing the current `u32`-backed `renderer_texture_handle_t` with an indexed/generation handle once Vulkan resource lifetime becomes concrete.
4. Consider splitting renderer API by lifecycle/resource/draw responsibilities if `renderer.h` grows too broad.

## Known Remaining OpenGL Surfaces

- `renderer_opengl.c` still owns OpenGL resources and GL state.
- `renderer_opengl.h` still exposes OpenGL globals to OpenGL implementation files.
- `presenter_backend_sdl_opengl.cpp` still directly uses SDL OpenGL and ImGui OpenGL for the Linux/macOS GUI path.
- Image and tile structs still store renderer texture handles as integer fields. This is acceptable for now, but Vulkan will likely require an indexed or pointer-like backend resource handle internally.

## Caution

Some of the OpenGL rendering code predates this refactor and may be rough. Prefer preserving behavior during boundary work. Fix obvious boundary leaks, but avoid optimizing or redesigning rendering behavior until there is either a failing case or a Vulkan prototype that forces a better abstraction.
