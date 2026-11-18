Slidescape for Windows (console version)
========================================

This is a Windows build of Slidescape intended for use from the command line.

The console version is almost identical to the regular build, except for the following differences:
- When you start Slidescape normally, a console window will appear.
- When you start Slidescape from the command-line, there will be properly functioning console output.
- This version will not try to register filetype associations (to prevent interference with the regular non-console version).


Available command-line commands
-------------------------------

--export
Convert, crop and/or resize input WSI(s) into a new pyramidal TIFF file (see section below for explanation).

--version
Print the Slidescape version number and exit.

--verbose
Enable verbose mode.

--headless
Suppress opening the GUI and exit immediately.


Cropping/converting WSIs from the command-line
----------------------------------------------

Slidescape supports cropping/converting and resizing WSI files, both using the graphical user interface and using the command-line.

The output will always be a pyramidal TIFF file. Supported input formats are:
- TIFF (including BigTIFF variants)
- Philips iSyntax
- DICOM
- Any other formats that OpenSlide can open

The command-line syntax is as follows:
slidescape_console.exe <input files> --export 

You can specify a number of additional command-line flags:

--mpp <micrometers per pixel>
Specifies the desired output resolution. If not specified, the resolution of the input file will be matched (=default).
If the output and input resolutions do not match, the WSI will be resampled to the new resolution.
For resampling, the lanczos3 method is used (this similar to how e.g. the Python PIL library does this).

--quality <JPEG quality value>
Sets the output JPEG quality setting. Should be a value between 1 and 100. 
Choosing a higher JPEG compression quality can decrease image quality loss from recompression, at the cost of a higher file size.
Typically used values are 80 or 90 (default: 90).

--tile-size <tile size in pixels>
Sets the tile size in pixels (default: 512). 

--postfix <file name postfix>
Specifies the text to append to the input filename for the output file, before the final .tiff file extension (default: ".exported").
Example: an input filename of "1.isyntax" with a postfix of ".exported" will generate an output filename of "1.exported.tiff".

--roi <name of annotation>
Specifies the name of the region of interest (ROI) annotation in the associated annotation file.
The region to export will be set to a rectangle-shaped area bounded/encompassed by the specified annotation's coordinates.
An annotation file with the same name as the input WSI file is required to be present. Supported annotation formats are GeoJSON (.geojson, .json) and ASAP XML (.xml).
Example: slidescape_console.exe 1.mrxs 2.mrxs --export --roi "Annotation 0"

--first-roi
Specifies that the first annotation present in the associated annotation file should be used as the region of interest (ROI).
The region to export will be set to a rectangle-shaped area bounded/encompassed by the annotation's coordinates.
An annotation file with the same name as the input WSI file is required to be present.
Example: slidescape_console.exe 1.mrxs 2.mrxs --export --first-roi

--with-annotations
Enables saving of annotations within the region of interest (ROI), as specified by the --roi or --first-roi flags.
If there any annotations are visible within the ROI, a new annotation file will be created for the output WSI containing those annotations.


To iterate over all WSI files with a particular extension in a folder (e.g. .isyntax), you can use batch scripts.


Example batch script for converting all WSIs in a folder to TIFF format:

@echo off

REM This script converts all WSIs with the specified filename extension in the current folder to an output TIFF file.

set INPUT_EXT=isyntax
set JPEG_QUALITY=90
set POSTFIX=".exported"

FOR /R %%f in (.\*%INPUT_EXT%) do (
  slidescape_console.exe "%%f" --export --quality %JPEG_QUALITY% --postfix %POSTFIX%
)


Example batch script for cropping all WSIs with associated XMLs in a folder:

@echo off

REM This script crops all WSI files in the current folder (with the ROI defined by the first annotation in the associated XML) to an output TIFF file.

set INPUT_EXT=mrxs
set JPEG_QUALITY=90
set POSTFIX=".cropped"

FOR /R %%f in (.\*xml) do (
  slidescape_console.exe "%%~nf.%INPUT_EXT%" --export --first-roi --quality %JPEG_QUALITY% --postfix %POSTFIX%
)
