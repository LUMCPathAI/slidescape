# AGENTS.md

Guidance for LLM coding agents working on Slidescape.

## Project Overview

Slidescape is a whole-slide image viewer for digital pathology. It loads large tiled pathology images, displays them through an OpenGL/Dear ImGui viewer, supports annotations, and can export/crop/resize slides from both the GUI and command line.

Important source areas:

- `src/core/`: viewer state, GUI, scene logic, annotations, image loading/export flow, command-line handling.
- `src/platform/`: platform abstraction and OS-specific code. Windows files are prefixed `win32_`; Linux/macOS files are generally prefixed `linux_` and use SDL2 for the GUI path.
- `src/tiff/`, `src/dicom/`, `src/isyntax/`, `src/mrxs/`: image format readers/writers.
- `src/utils/`: shared utility code.
- `src/third_party/`: small vendored single-file or compact libraries used directly by Slidescape.
- `src/imgui/` and `deps/`: vendored upstream dependencies. Avoid editing these unless the task is explicitly about updating or patching a dependency.
- `resources/`, `shaders/`, `doc/`: application resources, shader sources, and user-facing documentation.

## Language And Style

- This is primarily a C/C++ codebase, but prefer C-style code.
- Use C for new implementation code unless an existing boundary requires C++.
- C++ is mostly used where needed for Dear ImGui integration or existing C++ translation units. When writing C++, keep it C-like: plain structs, functions, explicit ownership, minimal templates, minimal RAII, and no modern C++ abstractions unless there is a strong local precedent.
- Follow the surrounding file's style. The repository mostly uses tabs for indentation (`.editorconfig`: tab width 4).
- Always use braces for `if`, `else`, `for`, `while`, and similar control flow, even for a single statement. Put the body on separate lines, with the opening brace on the same line as the condition.
- Prefer one statement per line and descriptive local names over compressed expressions or multiple statements on one line.
- Preserve readability and debugger visibility when refactoring code (some code might be intentionally verbose to aid readability and debugging.)
- Keep code straightforward and explicit. Avoid clever abstractions, hidden allocations, exceptions, RTTI-dependent designs, and heavy standard-library usage in hot paths.
- Prefer existing project types, macros, and helpers from `common.h`, `platform.h`, `arena.h`, and nearby modules.
- Be careful with integer sizes and file offsets. Whole-slide images can be very large, so preserve 64-bit sizes/offsets where relevant.
- Add comments where appropriate, especially where intent might not be immediately obvious for future readers.

## Dependencies

- Minimize new dependencies.
- If a library is needed, prefer small, lightweight, permissively licensed code that can be vendored into the repository. Single-header/single-source libraries in the style of stb are a good fit.
- Performance, memory use, binary size, portability, and ease of vendoring matter more than broad framework features.
- Do not add package-manager-only runtime dependencies for core functionality without explicit approval.
- Keep third-party code isolated under `src/third_party/`, `src/imgui/`, or `deps/` as appropriate, and document licenses in `doc/third-party-licences/` when adding vendored code.

## Portability

Preserve support for Windows, Linux, and macOS.

- Put cross-platform APIs in `src/platform/platform.h` or nearby platform abstraction headers when practical.
- Keep OS-specific implementations in `src/platform/`:
	- Windows: `win32_*` files.
	- Linux/macOS GUI path: `linux_*` files and SDL2-based ImGui backend.
	- Shared platform logic: `platform.c`, `graphical_app.c`, `work_queue.c`, `shader.c`, `openslide_api.c`.
- If changing platform behavior, check both branches in `CMakeLists.txt`: `WIN32`, `APPLE`, and the non-Apple Unix/Linux path.
- Avoid introducing Windows-only APIs, POSIX-only APIs, compiler-specific intrinsics, path separator assumptions, or OpenGL loader assumptions into shared code. Use existing wrappers and macros such as `WINDOWS`, `LINUX`, `APPLE`, `PATH_SEP`, and `OPENGL_H`.
- Runtime OpenSlide support is intentionally dynamically loaded. Do not turn it into a required link-time dependency.

## Performance And Memory

- Treat image decoding, tile access, rendering, export, and registration paths as performance-sensitive.
- Avoid unnecessary copies of pixel buffers, tile data, strings, and large metadata arrays.
- Prefer arena/temp allocation patterns already used in the code for transient work.
- Be explicit about allocation ownership. If ownership crosses module boundaries, document it in the function contract or with a short local comment.
- Be cautious in multithreaded code. Check `src/platform/work_queue.*`, thread-local memory, and synchronization wrappers before changing worker behavior.

## Build And Verification

The project uses CMake. Common targets include:

- `slidescape`: main GUI application.
- `slidescape_console`: Windows console build for command-line output.
- `slidescape_nongui`: internal non-GUI library used by tests and shared format/core code.
- `slidescape_tests`: doctest-based test executable registered with CTest.
- `slidescape_archive`: early pathology-first archive service, gated behind
  `BUILD_SLIDESCAPE_ARCHIVE=ON`. Its current loopback-only HTTP slice exposes health,
  study/series/instance QIDO plus a Part 10 provider for instance/series metadata and
  negotiated ordered multi-frame WADO; STOW and broader generated representations
  remain. Build it for archive work, not as a routine viewer health check.
- `dicom_dict_gen`: DICOM dictionary generation tool.

Suggested local checks:

```sh
cmake --build cmake-build-debug --target slidescape
cmake --build cmake-build-debug --target slidescape_console
cmake --build cmake-build-debug --target slidescape_tests
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target dicom_dict_gen
```

On Windows, agents launched from CLion may not inherit a shell `PATH` containing CMake, Ninja, or MinGW. If plain `cmake`, `ninja`, `gcc`, or `g++` are not found, check for CLion's bundled toolchain and call it directly. If a build directory already exists, `CMakeCache.txt` will usually show the exact paths in `CMAKE_MAKE_PROGRAM`, `CMAKE_C_COMPILER`, and `CMAKE_CXX_COMPILER`. A typical bundled-toolchain layout is:

```powershell
$clion = '<path-to-CLion-install>'
$env:PATH="$clion\bin\mingw\bin;" + $env:PATH
& "$clion\bin\cmake\win\x64\bin\cmake.exe" --build cmake-build-debug --target slidescape
```

Even when invoking the full `cmake.exe` path from `CMakeCache.txt`, prepend CLion's `bin\mingw\bin` directory to `PATH` first. The compiler may otherwise start but fail to launch helper programs such as `cc1.exe`, causing Ninja to report `FAILED: [code=1]` with no useful compiler diagnostic.

### Temporary Codex-on-Windows build workaround

Pending a proper fix in the Codex Windows sandbox/process runner, sandboxed Ninja builds may hang after a compiler child has exited. Ninja 1.13.2 captures each command's output through an inheritable named pipe and waits for pipe completion using `GetQueuedCompletionStatus()`. In affected Codex sessions, the compiler produces its object file and exits, but the pipe completion never arrives. Ninja remains alive and idle, and does not update `.ninja_deps` or `.ninja_log` or proceed to linking. Running the same Ninja command outside the agent sandbox completes normally. This is not a Slidescape CMake issue, and invoking Ninja directly inside the sandbox is not sufficient.

- Run builds outside the agent sandbox using the shell tool's escalation mechanism (`sandbox_permissions: "require_escalated"`), with the exact cached Ninja executable as a narrowly scoped approval prefix. Prepend the directory containing the cached MinGW compiler to `PATH` first.
- Use one build invocation with a generous timeout. The build is expected to finish in well under a minute, often only a couple of seconds (depending on caches). If it appears stuck, do not start an overlapping CMake or Ninja retry.
- Before retrying, inspect the object file, `.ninja_deps`, `.ninja_log`, target executable timestamps, and `ninja -n <target>`. A new object with unchanged Ninja metadata and executable indicates the known post-compile pipe hang; an updated executable plus `ninja -n` reporting no work indicates the build completed.
- A stuck Ninja process typically remains at near-zero CPU with no compiler child. If deeper diagnosis is necessary, its main thread will be waiting in `GetQueuedCompletionStatus()`, and a named pipe such as `\\.\pipe\ninja_pid<PID>_sp...` will remain open.
- Clean up only exact Ninja PIDs known to have been launched by the current agent attempt. Do not kill all `ninja.exe` or `cmake.exe` processes: the user may be building from CLion at the same time. If ownership cannot be established, ask the user before terminating anything.
- Report IDE confirmation as the verification source rather than attributing an updated executable to the agent when builds overlapped.

Typical direct-Ninja command, to be executed with outside-sandbox approval:

```powershell
$clion = '<path-to-CLion-install>'
$env:PATH="$clion\bin\mingw\bin;" + $env:PATH
& "$clion\bin\ninja\win\x64\ninja.exe" -C cmake-build-debug slidescape
```

If a build directory does not exist, configure one first:

```sh
cmake -S . -B build
cmake --build build --target slidescape
```

The test suite lives under `tests/` and uses doctest plus CTest. It currently covers low-level utilities, manifest-driven fixture smoke checks, TIFF fixture opening/tile decoding, and iSyntax fixture metadata/tile decoding through `slidescape_nongui`. Fixture metadata lives under `tests/fixtures/`: keep public downloadable fixture entries in `manifest.txt`, and put machine-local private entries in ignored `local_manifest.txt` files using `local_manifest.example.txt` as a template. Private WSI fixture files may exist under `data_for_testing/`, but they are not publishable and must not be required for a clean public test run. Fixture-dependent tests should skip/return cleanly when their manifest entries are absent or the referenced files have not been downloaded.

Note: `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is the repository root, so local builds may update generated executables such as `slidescape.exe`. Do not include generated binaries in source changes unless explicitly requested.

## Change Discipline

- Keep changes narrowly scoped to the request.
- Do not reformat unrelated code or churn vendored dependencies.
- Do not modify generated files such as `src/stringified_shaders.h`, `src/stringified_icon.h`, or generated DICOM dictionary outputs unless the task requires regenerating them.
- Avoid broad CMake rewrites. When adding files, update the existing source lists in `CMakeLists.txt` in the same style.
- Preserve GPL license headers in project source files and existing third-party license headers in vendored code.
- When adding user-facing behavior, update `README.md` or `doc/` if users need to know about it.

## Before Finishing

- Build at least the target touched by the change when feasible.
- If you touched platform-specific code, mention which platforms were actually built or checked.
- If you changed image parsing, export, rendering, annotation autosave, threading, or memory ownership, summarize the risk and the verification performed.
- Leave the working tree free of unrelated generated files and binaries unless the user explicitly asked for them.
