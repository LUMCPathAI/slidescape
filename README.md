# Slidescape

Slidescape is a whole-slide image viewer for digital pathology.

## Download
The latest binaries can be downloaded on GitHub from the [releases](https://github.com/amspath/slidescape/releases) section
for Windows, Linux and macOS.


## Features

### Supported image formats

The viewer has built-in support for:
* Tiled TIFF and BigTIFF (including generic and Philips TIFF variants).
* Philips iSyntax.
* Simple images (JPEG, PNG).
* DICOM.

If OpenSlide is present, the Aperio, Hamamatsu, Leica, MIRAX, Sakura, Trestle, and Ventana formats can additionally be loaded.

### Viewing options

To navigate an image, you can pan and zoom using either the mouse or the keyboard.
The mouse and keyboard sensitivity can be adjusted in the general options,
`Edit` > `General options...` under the tab `Controls`.

Basic image filters are available under `View` > `Image options`. 
These allow adjusting the black and white level and filtering out a background color.

There is experimental support for loading a second image as an overlay (e.g. a mask image).
To load an image as an overlay, press `F6` before loading the second image.


### Annotations
Slidescape can create and edit annotations in ASAP-compatible XML format and QuPath-compatible GeoJSON format.

Annotations can be manipulated in a variety of ways:
* New annotations can be created (points `Q`, lines `M`, rectangles `R`, freeforms `F`).
* Individual coordinates can be moved, inserted or deleted (to toggle editing of coordinates, press `E`).
* Annotations can be assigned a (color-coded) group.
* Annotations can be split into parts.

Changes to annotations are autosaved by default. If annotations were loaded from an existing file, the output format follows that file (`.xml`, `.geojson` or `.json`) and a backup of the original unchanged annotation file will be preserved with file extension `.orig`.

### Image exporting

Slidescape has capabilities for converting, cropping and resizing WSIs. The output will be a new pyramidal TIFF file.

To crop an image, you can specify a region of interest (ROI) by right-clicking a previously created annotation and selecting `Export region...`.

Alternatively, you can specify the region to export using the menu (`Edit` > `Select export region`), then use `File` > `Export` > `Export...` to export the file.

Resizing the WSI to a different resolution is also possible.

### Command-line interface

#### Starting the viewer
The basic command to start the viewer from the command-line is:
```
slidescape <input files>
```

To start the viewer with one or more overlay layers from the command-line, pass the base WSI first and then one or more files after `--overlay`:
```
slidescape base.mrxs --overlay another_wsi.tiff
```

The first file is loaded as the base image. Each file after `--overlay` is loaded as an overlay, including image registration against the base layer when available.

To force the viewer to run in headless mode and always exit immediately without opening the GUI, add `--headless`.

#### Exporting images (converting, cropping and resizing)
Image export operations can be performed using a command-line interface. The basic command is:
```
slidescape <input files> --export 
```

You can specify a number of additional command-line flags:

`--mpp <micrometers per pixel>`

Specifies the desired output resolution. If not specified, the resolution of the input file will be matched (=default).
If the output and input resolutions do not match, the WSI will be resampled to the new resolution.
For resampling, the lanczos3 method is used (this similar to how e.g. the Python PIL library does this).

`--quality <JPEG quality value>`

Sets the output JPEG quality setting. Should be a value between 1 and 100.
Choosing a higher JPEG compression quality can decrease image quality loss from recompression, at the cost of a higher file size.
Typically used values are 80 or 90 (default: 90).

`--tile-size <tile size in pixels>`

Sets the tile size in pixels (default: 512).

`--postfix <file name postfix>`

Specifies the text to append to the input filename for the output file, before the final .tiff file extension (default: ".exported").
Example: an input filename of `1.isyntax` with a postfix of ".exported" will generate an output filename of `1.exported.tiff`.

`--roi <name of annotation>`

Specifies the name of the region of interest (ROI) annotation in the associated annotation file.
The region to export will be set to a rectangle-shaped area bounded/encompassed by the specified annotation's coordinates.
An annotation file with the same name as the input WSI file is required to be present. Supported sidecar extensions are `.geojson`, `.json` and `.xml`.

Example: `slidescape 1.mrxs 2.mrxs --export --roi "Annotation 0"`

`--first-roi`

Specifies that the first annotation present in the associated annotation file should be used as the region of interest (ROI).
The region to export will be set to a rectangle-shaped area bounded/encompassed by the annotation's coordinates.
An annotation file with the same name as the input WSI file is required to be present.

Example: `slidescape 1.mrxs 2.mrxs --export --first-roi`

`--with-annotations`

Enables saving of annotations within the region of interest (ROI), as specified by the `--roi` or `--first-roi` flags.
If there any annotations are visible within the ROI, a new annotation file will be created for the output WSI containing those annotations.

Note that on Windows, the separate build `slidescape_console.exe` should be used instead of the regular `slidescape.exe`,
in order to make console output visible. See [README_console.txt](doc/README_console.txt) for more information.



## How to build

### Windows
You can build the program using CMake and the MinGW-w64 toolchain.

All library code is included with the source code distribution (there are no other external dependencies).

You may wish to install `nasm` so that the SIMD-optimized assembly code in libjpeg-turbo can be compiled.

### Linux / macOS

Make sure the build tools `cmake` and (optionally) `nasm` are installed.

The following libraries are required to be installed: SDL2 (on Linux and macOS), GLEW (Linux only).
```
mkdir build && cd build
cmake ..
cd ..
cmake --build build --target slidescape -- -j
./slidescape
```

### OpenSlide support

Slidescape can detect and load the [OpenSlide](https://github.com/openslide/openslide) library at runtime.

To enable OpenSlide support on Windows, download (or compile) the [64-bit binaries](https://openslide.org/download/)
and put all of the DLL files together in an `openslide/` folder, and put that folder in the same location as `slidescape.exe`.

To enable OpenSlide support on Linux, install the `openslide` library, either using a package manager or
by building and installing it manually. The program will try to locate `libopenslide.so`, either in the
default system library paths or in `/usr/local/lib/`.

On macOS release builds, the official OpenSlide binary distribution can be bundled inside `Slidescape.app`.
As an alternative, you can install the `openslide` library using Homebrew or MacPorts.
The program will try to locate `libopenslide.dylib` in the app bundle first, then `/opt/local/lib/libopenslide.dylib`,
`/opt/homebrew/opt/openslide/lib/libopenslide.dylib`, and `/usr/local/opt/openslide/lib/libopenslide.dylib`.


## Credits

Author: Pieter Valkema.

Slidescape embeds code from the following projects, under their respective licenses:
* [Dear ImGui](https://github.com/ocornut/imgui) (graphical interface library)
* [FreeType](https://www.freetype.org/index.html) (font renderer)
* [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) (JPEG image codec)
* [Mbed TLS](https://github.com/ARMmbed/mbedtls) (SSL/TLS and cryptography library)
* [Yxml](https://dev.yorhel.nl/yxml) (XML parser)
* [json.h](https://github.com/sheredom/json.h) (JSON parser)
* [linmath.h](https://github.com/datenwolf/linmath.h) (linear math library)
* [stb_image.h](https://github.com/nothings/stb) (image loader)
* [OpenJPEG](https://github.com/uclouvain/openjpeg) (discrete wavelet transform)
* [LZ4](https://github.com/lz4/lz4) (compression algorithm)
* [base64.c](http://web.mit.edu/freebsd/head/contrib/wpa/src/utils/base64.c) (base64 decoder)
* [cityhash](https://github.com/google/cityhash/blob/8af9b8c2b889d80c22d6bc26ba0df1afb79a30db/src/city.cc#L50) (byte swap operations)
* [stb_ds.h](https://github.com/nothings/stb/blob/master/stb_ds.h) (typesafe dynamic array and hash tables for C)
* [ltalloc](https://github.com/r-lyeh-archived/ltalloc) (fast memory allocator)
* [glad](https://github.com/Dav1dde/glad) (OpenGL loader)
* [Keycode](https://github.com/depp/keycode) (library for platform-independent keyboard input handling)
* [ImGuiFileDialog](https://github.com/aiekick/ImGuiFileDialog) (file dialog for dear ImGui)
* [libtiff](https://gitlab.com/libtiff/libtiff) (LZW decoder from libtiff)
* [minfft](https://github.com/aimukhin/minfft) (Fast Fourier Transform library)
* [HTTP parser](https://github.com/nodejs/http-parser) (HTTP parser)
* [Pillow](https://github.com/python-pillow/Pillow/tree/main) (image resampling)

Uses [SDL2](https://www.libsdl.org/download-2.0.php) and [GLEW](http://glew.sourceforge.net/) on Linux/macOS.

The application icon was made by [Freepik](https://www.flaticon.com/authors/freepik) from [Flaticon](https://www.flaticon.com/).

## License

Copyright (C) 2019-2025 Pieter Valkema. 

This program is free software: you can redistribute it and/or modify 
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

See [LICENSE.txt](https://github.com/amspath/slidescape/blob/master/LICENSE.txt) for more information.
