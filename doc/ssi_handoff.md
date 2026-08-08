# 3DHISTECH Simple Slide Interface handoff

Status: working read-only WSI backend, validated against the LUMC SlideCenter
installation on 2026-08-06.

## Scope and current status

Slidescape can open a SlideCenter slide by its 32-character ID, establish an SSI
session, open the slide read-only, read its base and extended properties, load
the scanner preview and label, and stream JPEG tiles into the normal viewer
cache. It also lists the immediate slides in the slide's SlideCenter folder and
can switch among them from **View > Slide tray**. Interactive testing
confirms that the image remains stationary while changing pyramid levels.

The implementation began in commit `c2d1fb1` (`Add 3DHistech SSI backend`) and
was consolidated with URI/geometry work and this handoff in `c286af5`. The main
files are:

- `src/core/ssi.h`: SSI endpoint, session, slide, and pyramid data structures.
- `src/core/ssi_protocol.c`: JSON parsing, endpoint validation, URI parsing,
  common-canvas geometry, and tile request construction.
- `src/core/ssi_client.c`: session/slide lifecycle and image creation.
- `src/core/image.c`: conversion of SSI base properties to Slidescape's sparse
  image pyramid.
- `src/core/viewer_io_file.cpp`: worker-side HTTPS tile retrieval and JPEG
  decoding.
- `src/core/app_input.cpp` and `src/core/gui.cpp`: Open URI, command-line input,
  credential prompting, and expired-session handling.
- `tests/test_ssi.cpp`: protocol, geometry, and URI unit coverage.
- `tests/ssi_probe.py`: opt-in credential-safe live smoke probe.

Supported inputs are:

```text
ssi:<32-character-slide-id>
mview:OfflTelSlide?HUBIP=...&HUBSecure=true&SldID=...
```

An `mview:` URI supplies the host and slide ID. `Capt` is parsed as a sanitized
initial display-name hint and is replaced by the authoritative SlideCenter
`Name` after metadata retrieval. `AccID` is used with SSI `session/connect` to
attach to the launcher session without credentials; SSI returns an independent
session owned by Slidescape. If the launcher session is missing or expired, the
GUI falls back to its credential prompt. `page` is ignored. Plain `ssi:` input
obtains the two server URLs from the environment. Both forms work through Open
URI and the command line.

On Windows, **Edit > General options > Integration** can make Slidescape the
per-user handler for `mview:` links. The toggle creates only
`HKEY_CURRENT_USER\Software\Classes\mview`; it does not alter a machine-wide
SlideViewer registration. Any pre-existing per-user registration is copied to
Slidescape-owned recovery data before the override is installed. Disabling the
toggle verifies Slidescape's ownership marker and installed command before
removing it, then restores the saved registration or reveals the untouched
machine-wide handler. Slidescape refuses automatic changes when the handler or
recovery data has been modified, or when Windows has a protected per-user
Default Apps choice for `mview:`.

The GUI prompts for a username and password if they are absent or rejected. It
also prompts again when tile responses indicate that the slide/session has
expired. Credentials are held in process memory, password-bearing temporary
buffers are cleared where practical, and credentials, session IDs, and slide
tokens are not written to logs.

## Pyramid geometry

SSI coordinates use one glass-slide canvas shared by every pyramid level. The
implementation deliberately does not introduce per-level render origins or
change the image clip rectangle.

For level `L`:

- downsample is `2^L`;
- occupied tile bounds are the native SSI bounds divided by `2^L` using integer
  division;
- the allocated tile grid covers the complete `Width` by `Height` canvas at
  that downsample;
- tiles outside the occupied SSI bounds exist in the grid but are marked empty.

This last rule matters at low-resolution levels. It preserves a well-defined
full pyramid even when trailing canvas tiles have no slide data.

## Validation performed

The private reference export `T25-231.mrxs` and live SSI slide
`fe2a3b0ec0f18b9b3981094ba2e3c2df` were compared locally. The private fixture
and comparison script are intentionally not part of the repository.

The native MRXS and SSI responses agreed on:

- canvas: `143360 x 311040`;
- ten levels (`0..9`) with power-of-two downsampling;
- MPP: `0.176600441501104` in both axes;
- native occupied tile bounds: X `293..490`, Y `612..1161`;
- full level-9 grid: `2 x 3`, with data only at X `0`, Y `1..2`.

Fourteen representative tiles were compared at levels 0, 1, 4, 8, and 9. The
mean absolute RGB error averaged `0.48` and was at most `2.43`, consistent with
different JPEG compression settings. No visual abnormality was seen in the
final interactive test.

Verification completed on Windows:

- focused SSI tests: 9/9 passed (86 assertions);
- complete `slidescape_tests` suite passed;
- `slidescape` GUI target built successfully;
- `slidescape_console` target built successfully;
- credential-safe live session/open/base-properties/tile/cleanup probe passed.

Before the connection and scheduling changes, a 2026-08-06 live feature run
completed seven sequential requests in 1047 ms of aggregate request time and
1082 ms total open time. After the changes, the three requests required to
create the viewer took 440 ms of request time and the viewer was ready after
462 ms. Optional metadata and associated images completed 392 ms later. All
seven requests took 818 ms of aggregate request time and transferred 678772
bytes. These values are diagnostic baselines, not performance guarantees.
Verbose mode reports per-request status, duration, response size, viewer-ready
latency, optional-data latency, and first-tile decode/display latency.

## Performance architecture

The required open path consists of `session/create`, `slide/open`,
`base_properties`, and `slide_attributes`. Session creation and slide opening
reuse one persistent HTTPS connection. Base properties then continue on that
connection while slide attributes are requested concurrently on a second
connection. The image is committed only after both succeed, so the
authoritative SlideCenter `Name` is present before the first viewer frame.

`all_properties`, `folder_content`, `PREVIEW`, and `LABEL` are then loaded on a
background task using a persistent HTTPS connection. The result is committed
on the main thread so the folder list, associated images, and metadata are
updated without racing the renderer. Failures in this optional path are
non-fatal. Headless mode performs the same work synchronously to keep
command-line probing deterministic.

The live reference run completed the four required requests in 519 ms elapsed
time, compared with 620 ms when their individual request durations are summed;
the overlap is the parallel base-properties/slide-attributes phase. The slide
was committed as `T25-231`. Optional folder enumeration returned two immediate
slides and completed together with metadata and associated images afterward.

All SSI tile and control operations share a bounded pool of twelve persistent
HTTPS connections per process. Control operations (including keepalive and
slide switching) opportunistically take an idle pooled connection. They never
wait behind tile traffic: if every slot is occupied, they immediately use a
temporary connection instead. A failed idle pooled connection is discarded and
retried once. Telemetry distinguishes control reuse from fallback handshakes.

The cache plans aligned fixed chunks of up to `4 x 2` tiles at one
pyramid level. Needing one tile schedules every missing tile in that clipped
chunk, including off-screen neighbors as GPU prefetch. The worker always fetches
the complete chunk rectangle, decodes its JPEG once, and splits it into the
normal 256-pixel cache tiles. The 12-request pipeline can keep up to 96 useful
tiles in flight, matching the best measured rectangle concurrency.

SSI batches wait in a backend-owned priority queue rather than as immutable
entries in the global FIFO. A newer viewer generation immediately removes old
pending chunks and releases their tile-cache reservations. Up to twelve short
worker pumps pull the highest-priority current chunks. During a pan, an active
chunk is adopted by the new generation wherever its tiles remain wanted;
obsolete tiles are discarded before decode/upload. Active HTTP responses are
allowed to finish so their persistent connections remain reusable. Live
telemetry showed that aborting them caused almost one replacement TLS handshake
per cancellation, while requests normally completed in about 43 ms.

Verbose telemetry reports cumulative requests, useful tile count, rectangle
tile-equivalents, decoded/stale/failure counts, windowed
decoded tiles/s, average rectangle area, average useful tiles and usefulness
percentage, latency, payload size, and connection-pool counters every eight
requests. Pending chunks discarded before network execution are reported
separately. SSI transport/decode failures remain retryable and never mark their
tiles permanently empty.

A live warmed-connection sweep over individual level-0 tiles produced roughly
200--2400 tiles/s depending on concurrency and server cache state. SSI returned
predictable rectangle dimensions (`2 x 2` tiles as `512 x 512` pixels and
`4 x 2` as `1024 x 512`). Cropping the bottom-right tile from each rectangle
agreed with the corresponding separately requested tile to a decoded-pixel MAE
below `0.007`, confirming the coordinate order used by the splitter. Four
concurrent `4 x 2` rectangles produced about 3400--3700 tile-equivalents/s,
while twelve concurrent rectangles reached about 5200--5400
tile-equivalents/s. The application now uses the measured twelve-request sweet
spot; the dedicated pending queue and stale rejection limit how long those
workers can be occupied by obsolete viewer work. Absolute values can benefit
from server-side caches and are diagnostic rather than guarantees.

SSI does not provide a documented stored/native JPEG encoding. Six raw JPEG
payloads extracted from the matching MRXS export were compared with SSI output
at qualities 80, 90, and 100 across levels 0, 1, 4, 8, and 9. None matched
byte-for-byte or as decoded pixels, proving that this server recompresses even
single-tile responses. Quality 80 used the same quantization tables as the
native MRXS JPEGs but still changed pixels. Quality 100 introduced the least
additional decoded-pixel error, while detailed payloads grew by roughly
2.5--2.8 times; the viewer therefore retains quality 90 as a bandwidth/quality
compromise.

## Credentials and local testing

The backend uses:

```text
SLIDESCAPE_SSI_API_URL
SLIDESCAPE_SSI_SLIDECENTER_URL
SLIDESCAPE_SSI_USERNAME
SLIDESCAPE_SSI_PASSWORD
```

`SLIDESCAPE_SSI_CA_BUNDLE` can point the Python smoke probe at a private CA
bundle. TLS verification should not be disabled. A local credential batch file
must remain untracked and must be invoked with `call` in the same `cmd.exe`
process as the program being tested; environment changes in a separate Codex
terminal are not inherited by agent-launched processes.

## Read-only functionality exposed by SSI

The following assessment is based on the live Swagger schema and value-redacted
probes against the reference slide. No credentials, handles, or metadata values
were printed or retained.

### Folder and slide discovery

SSI exposes:

- `server/{session_id}/folder_content`: returns the parent folder, current
  folder, immediate subfolders, and immediate slides. An empty `folder_id`
  requests the root.
- `server/{session_id}/search_folders`: searches folder names.
- `server/{session_id}/search_slides`: searches slide names using `EXACT`,
  `CONTAINS`, or `REGEX`, optionally below a folder ID. Results include the
  complete folder path.

Slide records can contain ID, SlideCenter display name, scan state, project
type, size, creation/modification/expiry dates, description, staining, organ,
barcode, examination status, service unit, DICOM study/series UIDs, scan
profile, and HIS integration state.

Although it is absent from the documented `BaseProperties` schema, the live
server includes `FolderId` in `base_properties`. Slidescape parses this field
and uses it with `folder_content`, avoiding the slow global search path. The
live reference folder returned two immediate slides in 159 ms.

The example launcher `AccID` was tested directly as `folder_id`; SlideCenter
rejected it with HTTP 400 because it is not the folder GUID. Its 20-character
form instead matches the SSI session handle accepted by `session/connect`. In
the same live check, `Capt=T25-231` exactly matched `slide_attributes.Name`.

A browser must not run global search synchronously on the UI thread. It should
use asynchronous cancellable requests, explicit timeouts, cached folder
contents, and lazy one-folder-at-a-time navigation. If a future launcher or
local cache can supply the parent folder ID, `folder_content` is the preferred
path and avoids a global search.

The shared **Slide tray** window is used by local DICOM, DICOMweb, and SSI, and
opens automatically whenever the current source contains multiple slides. For
SSI it lists the immediate folder. Selecting another slide opens it in the
existing SSI session, obtains its base properties and authoritative name, and
transfers session ownership and a snapshot of the current tray to the new image
before closing the old slide token. The tray therefore remains visible while a
background `folder_content` refresh obtains any changes. A failed refresh keeps
the existing list. Deliberately closing the final SSI image still closes the
session.

### Associated images

`slide/{slide_token}/single_image` supports:

- `THUMBNAIL`: generated preview;
- `PREVIEW`: scanner-created original preview;
- `LABEL`: slide label area;
- `SCANMAP`: one pixel per level-0 256-by-256 tile, white for data and black for
  empty space.

All four were available for the reference slide:

| Type | Returned image |
| --- | --- |
| Thumbnail | JPEG, `1120 x 2430` |
| Preview | JPEG, `1268 x 2751` |
| Label | JPEG, `1289 x 1039` |
| Scanmap | PNG, `560 x 1215` |

The scanmap dimensions exactly match the full level-0 tile grid. SSI permits
BMP, PNG, TIFF, and JPEG encodings, although scanmaps are restricted to
lossless/raw formats.

Recommended mappings are: label to Slidescape's existing label view, preview
to the existing macro/overview view, thumbnail to future browser rows, and
scanmap as an optional authoritative sparse-tile mask or diagnostic.

The backend now implements the first two mappings. It requests `PREVIEW` and
`LABEL` asynchronously after the WSI becomes usable, decodes them to the
existing RGBA associated-image model, and falls back from unavailable
`PREVIEW` to `THUMBNAIL`. Missing associated images do not prevent the WSI from
opening.

### Captions, barcode, and metadata

`server/{session_id}/{slide_id}/slide_attributes` worked and returned the 18
SlideCenter record fields listed above. `Name` is the authoritative
SlideCenter display name; it may differ from SSI base property `OriginalName`.
`BarCode`, `Description`, `Staining`, and `Organ` are directly available.

`slide/{slide_token}/all_properties` also worked:

- 71 flat properties with hierarchical reading disabled;
- 382 properties with hierarchical reading enabled.

Available property names include objective magnification/name, scanner hardware
and software versions, scanning dates/profile, anonymized barcode, and—in the
hierarchical set—barcode value/image geometry, slide/project names, creation
timestamps, output resolution, and barcode position. Values should be treated
as potentially identifying clinical metadata and displayed or logged only
deliberately.

The backend now retrieves and retains both the SlideCenter attributes and the
hierarchical all-properties JSON as read-only metadata. With verbose mode
enabled, both documents are printed as received for development testing. This
output can contain identifying clinical metadata and must not be attached to
public issues or CI logs.

The `Capt` value from an `mview:` launcher can be used as an immediate UI hint,
but it is not an SSI request parameter and should not replace `Name` or
`OriginalName` as authoritative metadata.

### Other future read-only capabilities

The API also offers:

- arbitrary rectangular image extraction with optional target dimensions;
- session and slide ping endpoints;
- GeoJSON annotation retrieval;
- marker counters and TMA marker retrieval;
- field-of-view metadata and FOV images.

Write/delete annotation and TMA endpoints also exist, but they remain outside
the current read-only scope. The service-wide terminate and local-slide-open
operations must not be exposed through the viewer backend.

## Recommended next implementation order

1. Add a read-only metadata panel over the retained `slide_attributes` and
   `all_properties` documents, with deliberate handling of identifying fields.
2. Add thumbnails to the existing immediate-folder slide list.
3. Extend the immediate-folder list to lazy subfolder navigation. Treat global slide
   search as an optional asynchronous operation with cancellation and a visible
   timeout; do not make it a prerequisite for direct slide opening.
4. Periodic keepalive is implemented: while an SSI image is open, a background
   job sends the documented session and slide ping requests every 60 seconds.
   Authentication/not-found responses use the existing credential recovery;
   transient failures are logged and retried at the next interval.
5. If twelve blocking rectangle pumps noticeably delay unrelated work, replace
   the pumps with an asynchronous SSI transport while preserving the fixed
   chunk queue, twelve-connection cap, and stale-generation semantics.
6. Add read-only GeoJSON annotations, marker counters, TMA, and FOV support only
   after the lower-risk metadata and image features are stable.

The live API documentation used for this assessment is hosted at
`https://<slidecenter.example.hostname>/SSI/swagger/index.html` with its schema
at `/SSI/swagger/v1/swagger.json`.
