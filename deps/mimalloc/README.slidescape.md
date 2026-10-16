# Vendored mimalloc

This directory contains the source and CMake support needed to build mimalloc
as a static process-wide allocator override for Windows/MSVC, Linux, and macOS
builds.

Upstream: https://github.com/microsoft/mimalloc

Revision: `31d034d94cdb8e22f7d7ed55967f581a2d6e831d`
(`v3.5.3-1-g31d034d9`)

Only the upstream `include`, `src`, and CMake build files used by Slidescape
are vendored. The allocator is enabled by default on supported platforms and
can be disabled through `SLIDESCAPE_USE_MIMALLOC`. Address-sanitizer builds
disable it by default and reject the unsupported combination if explicitly
enabled.
