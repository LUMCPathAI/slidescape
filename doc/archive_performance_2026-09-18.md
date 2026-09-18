# Archive and DICOMweb performance pass — 18 September 2026

The principal cold-open problem was the viewer's bootstrap representation, not bulk
pixel I/O. MRXS's compact sparse map was expanded into per-frame DICOM functional
groups, transferred as JSON, then expanded again into a large client JSON tree.
The occupied frames were sparse; the serialized description of each frame was verbose.

## Measurements

Measured on this macOS arm64 workstation using the two series supplied for this task.
Debug and RelWithDebInfo were built separately. These are illustrative local timings,
not a throughput guarantee. Application-cold runs start a fresh archive and viewer;
the filesystem cache was **not** flushed. The baseline metadata comparison had a warm
archive, making it a conservative comparison with the new application-cold results.

| Measurement | MRXS | Part 10 |
|---|---:|---:|
| Original bootstrap metadata | 23,188,266 bytes | 772,979 bytes |
| Compact bootstrap metadata | **19,936 bytes** | **1,327 bytes** |
| Original Debug metadata parsing | 348 ms | 4.35 ms |
| Compact Debug metadata parsing | **1.18 ms** | **0.06 ms** |
| Original Debug preparation | 437 ms | 9.12 ms |
| Compact Debug preparation, fresh archive | **48.75 ms** | **2.80 ms** |
| Compact optimized preparation, fresh archive | **20.28 ms** | **2.13 ms** |
| Optimized first tile upload submitted | **89.50 ms** | **71.16 ms** |

Preparation starts inside the series-opening worker and ends after metadata parsing
and image/cache setup. First upload includes the subsequent handoff and tile pipeline;
it is not a measurement of physical display presentation or the fully refined viewport.
Application launch, GUI initialization, and opening-task queue delay are outside these
measurements. The remaining roughly 50–70 ms between preparation and first upload needs
further viewer scheduling/render-pipeline tracing if a lower first-pixel target is needed.

MRXS's top level has 56,542 occupied frames in 2,655,776 possible grid positions.
Its standard JSON was 17,290,423 bytes; the compact representation is 8,847 bytes.
All 10 MRXS levels and all 3 Part 10 VOLUME levels produced identical dense client
frame maps from standard and compact metadata. Part 10 additionally stops requesting
the THUMBNAIL metadata; QIDO already identifies it as outside the tiled VOLUME pyramid.

## Tile latency: decoding dominates

The original `average request` already included HTTP, multipart parsing, and decoding
of all frames in a batch. A four-frame request is not comparable to one tile read.
Each sample was rounded to integer milliseconds before averaging, despite displaying
the average with one decimal place.

The new average covers the latest **100 completed requests**, with fractional
milliseconds retained. It reports HTTP, decode, and other time separately. Before
100 requests, it uses the samples available. Counters remain process-lifetime totals;
the rolling window can temporarily contain requests from the previously opened slide.
HTTP includes connection establishment/retry, server time, transfer, and HTTP parsing.
“Other” includes multipart parsing, mapping, allocation cleanup, and path setup. Viewer
queue delay and GPU upload are outside this request timer.

Representative optimized viewer windows, predominantly four-frame requests:

| Rolling average | MRXS JPEG | Part 10 JPEG 2000 |
|---|---:|---:|
| Complete request | 2.84 ms | 8.36 ms |
| HTTP | 0.56 ms | 0.65 ms |
| Decode | 2.26 ms | 7.68 ms |
| Other | 0.02 ms | 0.03 ms |

Tile content, batch composition, concurrent workers, CPU scheduling, and build mode
all affect these numbers. Debug JPEG 2000 windows were approximately 21–26 ms here.
These windows are diagnostic samples, not a controlled before/after codec benchmark.
CPU sampling identified OpenJPEG entropy decoding and inverse wavelet transforms as
major costs. A small RGB8 output conversion fast path now avoids general per-sample
scaling/division; it does not eliminate the JPEG 2000 decoding cost. Multipart boundary
search now uses `memchr` to skip compressed bytes in bulk.

Isolated single-frame HTTP medians across first/middle/last frames were approximately
0.19–0.31 ms (MRXS) and 0.21–0.25 ms (Part 10), with profiling enabled. Under the
12-client Python load, server work excluding receive/send was about 0.03 ms on average,
and sending about 0.025 ms. Receiving headers averaged about 0.4 ms in that particular
load generator; that includes waiting for the client to send them. It is not disk time.
The archive sends stored JPEG/JPEG 2000 bytes for these viewer requests; decoding is
performed in the viewer.

## Response admission correction

The old memory admission reserved 256 MiB for every WADO request, including tiny JPEG
batches. Its 512 MiB budget admitted only two concurrent requests. A 12-client run
produced 42 HTTP 503 responses in 400 requests.

Frame requests now reserve a shell-derived upper bound for their retained frame
buffers plus multipart headers before allocation. The global 512 MiB and per-response
256 MiB limits remain. A run spanning all 13 VOLUME instances completed 1,560 batch
requests with 12 concurrent clients and **zero failures**. Compact metadata reserves
its separate 8 MiB maximum, so small bootstraps do not each consume the full standard
metadata allowance. Full DICOM JSON metadata retains the conservative reservation.
Decoder working allocations and source caches remain outside the response-buffer
budget; it is not a whole-process RSS limit.

## Compact bootstrap contract

The viewer requests instance metadata with the single Accept value:

```
application/vnd.slidescape.wsi-metadata.v1+json
```

The same media type identifies the response. This is an explicit Slidescape extension,
not DICOM JSON and not an authoritative metadata export. Standard instance/series
metadata endpoints keep their complete existing DICOM JSON projection. No repository,
shell, recipe, or UID migration is involved.

The version-1 object contains `version`, `studyUID`, `seriesUID`, `instanceUID`,
`imageFlavor`, `photometricInterpretation`, `rows`, `columns`, `matrixRows`,
`matrixColumns`, `samplesPerPixel`, `frameCount`, `tiledFull`, `mpp`, and `runs`.
`mpp` is `[x, y]` in micrometres per pixel; zero denotes unavailable spacing.
Each run is `[zero_based_grid_index, one_based_first_frame, frame_count]`, in frame
order. Both indices increase together inside a run. Grid indices are row-major and
holes remain absent. Runs need not be spatially sorted. Full grids use an empty run
array and implicit consecutive frame numbers.

The projection is restricted to aligned VOLUME grids with one optical path and one
focal plane. Unsupported layouts return 406. The client retries ordinary DICOM JSON
when the extension is rejected, and also accepts servers that ignore the custom Accept
and return DICOM JSON directly. Both behaviors were exercised in the actual viewer.
The client checks version, UIDs, dimensions, expanded allocation bounds, complete frame
coverage, and overlapping/invalid runs. It also checks returned UIDs against the
requested instance. No source paths, file offsets, ICC blobs, specimen metadata, or
repeated physical coordinates are needed in this display bootstrap.

## Reproducing the profile

Run the viewer with `--verbose`. It reports preparation stages, first tile upload,
and rolling tile-request stages. For archive timings, enable:

```sh
SLIDESCAPE_ARCHIVE_PROFILE=1 ./slidescape_archive serve data/archive_repo --port 8043
```

Per-request logs distinguish queue, receive, work, and send time. They contain route
kinds rather than URLs or metadata. Profiling is off by default.

The standalone profiler starts/stops its own archive process on a spare loopback port
for each supplied series. Temporary response bodies are removed afterward:

```sh
python3 tests/profile_archive.py --binary ./slidescape_archive \
  --repository data/archive_repo --probe ./dicomweb_probe \
  --requests 20 --clients 12 --server-timings SERIES_URL [SERIES_URL ...]
```

It reports cold metadata, representative first/middle/last frame latency, concurrent
batch failures/latency, optional standard/compact equivalence, and offline multipart/
decode timings. `dicomweb_probe profile` and `compare` can also operate on saved local
responses. “Cold metadata” for later instances may reuse the first instance's aggregate
shell. With `--probe`, full metadata comparison also warms payload directories before
frame timings; those frame timings are not a cold-storage benchmark.

## Verification and next priorities

macOS Debug/optimized archive, viewer, and probe builds passed. Full CTest and targeted
AddressSanitizer tests passed, including synthetic loopback tests, malformed/overlapping
compact runs, unsupported dimensions, standard metadata preservation, and concurrent
requests. Actual viewer checks covered compact serving, a 406 fallback server, and a
server ignoring the custom Accept. Windows/Linux and disk-cache-cold storage were not
tested.

Next priorities are the remaining first-upload handoff, compact/sparse client tile
state instead of allocating state for every possible grid position, and decoder work.
Use optimized builds when evaluating latency. ARM JPEG SIMD is currently disabled by
an existing encoder correctness workaround; splitting decoder enablement from that
encoder issue deserves its own pixel-correctness investigation. For JPEG 2000, evaluate
codec/SIMD improvements or separately published display representations with explicit
quality policy. Retain the original source; do not silently introduce lossy conversion.
Persistent HTTP connections matter more for the future VPS/TLS path, but they are not
the dominant cost in these loopback measurements.
