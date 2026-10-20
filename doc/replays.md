# Timed viewer replays and profiling

Replays run the graphical viewer noninteractively, including its normal tile workers,
cache, uploads and rendering. They require a working window/display and graphics context;
`--headless` is the separate export path and cannot be combined with these options.

```sh
# Record a local slide navigation session. Quit the viewer to finish the recording.
slidescape --record-replay navigation.scr slide.mrxs

# Play it back and stream profiling data to CSV.
slidescape --replay navigation.scr --profile navigation.csv

# Profiling also works during ordinary interactive use.
slidescape --profile interactive.csv slide.mrxs

python3 scripts/replay.py dump navigation.scr
python3 scripts/replay.py summary navigation.csv
```

On macOS, the executable is `Slidescape.app/Contents/MacOS/Slidescape`. On Windows,
`slidescape_console.exe` also provides console diagnostics. Playback starts a separate
instance rather than forwarding its arguments to an existing viewer.

The minimal recorder captures local base-image opens, closes and camera targets. It adds
an automatic synchronization after opening and an exit command on normal shutdown.
Zoom animation itself is not recorded frame by frame. Camera positions during zoom are
converted to the final target position; tiny floating-point drift below 0.05 micrometres
is ignored. Panning updates omit zoom when the zoom target has not changed.

This first version does not record focus-plane selection, rotation, overlays, annotations,
remote authentication or other GUI operations. Keep those settings fixed when recording
and comparing runs. Recorded local paths are used verbatim; use the dump/encode tool to
replace paths when moving a replay. Relative paths resolve against the process working
directory. Recordings are commands, not screenshots or embedded slide files.

## Commands and time

Each command has a timestamp in seconds since the start of playback or completion of
the last `wait`. Timestamp zero starts at the first viewer update after window setup.
Use the same window dimensions, display scale, viewer settings, build and machine for
performance comparisons. The CSV records the actual viewport and display scale.

| Command | Arguments | Behavior |
| --- | --- | --- |
| `open` | UTF-8 path | Open/replace the base slide through the normal file-loading path. |
| `wait` | None | Wait for file initialization/indexing, then reset the timestamp origin to zero. |
| `camera` | float x, float y, optional float zoom | Set the target view. Coordinates are the camera center in scene micrometres. Zoom is the viewer's log2 downsample position: 0 = native resolution, 1 = 2× downsample. |
| `close` | None | Close the loaded images through the normal close path. |
| `exit` | None | Request application shutdown. |

Camera updates use the existing zoom animation, including the viewer's zoom limits and
fast-rendering setting. Omitting zoom preserves the current target. Panning without an
active zoom moves the camera immediately. With an active zoom, the pivot is adjusted so
that animation finishes at the requested center. Late camera commands are caught up in
order (up to 64 per frame); open, close, wait and exit yield a frame boundary.

`wait` keeps rendering while work proceeds. It waits for MRXS indexing across all
planes/levels, DICOM level indexing, and iSyntax's initial load where applicable.
It does **not** wait for all visible tiles, empty worker queues, or camera animation.
With no image loaded it immediately resets the clock. Timestamps must be finite,
nonnegative and nondecreasing within each segment. A blocked wait fails after 120 seconds.

EOF stops playback and returns control to the user. Include `exit` for unattended runs.
Invalid files, failed commands and synchronization timeouts stop playback and request
application exit with status 1. Successful explicit exit returns 0. Input/recording and
playback cannot be combined in one invocation.

A short hand-authored replay can be compiled using the helper (JSON is only the authoring
format; the viewer reads binary):

```json
[
  {"time": 0, "command": "open", "args": ["slide.mrxs"]},
  {"time": 0, "command": "wait"},
  {"time": 0.2, "command": "camera", "args": [12000, 24000, 4]},
  {"time": 2, "command": "camera", "args": [14000, 26000]},
  {"time": 4, "command": "close"},
  {"time": 4, "command": "wait"},
  {"time": 0.5, "command": "exit"}
]
```

```sh
python3 scripts/replay.py encode navigation.json navigation.scr
```

## Binary version 1

All numeric fields are little-endian, with no structure padding. Floats are IEEE 754
binary32. Header: eight bytes `SCREPLAY`, followed by a `uint32` version (currently 1).
Each following command is:

```
float32 timestamp
uint32 command       // open=1, wait=2, camera=3, close=4, exit=5
uint32 argument_count
repeat argument_count times:
    uint32 payload_size
    byte[payload_size] payload
```

Open has one string argument; camera has two or three separate float32 arguments; other
commands have none. String size includes a trailing null. The parser re-terminates its
last byte and rejects embedded nulls. File size is limited to 64 MiB and string payloads
to 64 KiB. Camera values must be finite, coordinates within ±1e9 micrometres and encoded
zoom within [-16, 30] (the viewer then applies its configured zoom limits).

The file is read into an owned `mem_t`. Only the next command is parsed; argument lengths
are checked before reading, and string arguments reference that buffer without per-command
allocation. Unknown commands or versions are errors, rather than silently skipped actions.

## Profiler window

Open **View > Debug > Show profiler** (F2) to collect extended statistics without writing a
CSV. Collection and memory sampling run while the window is open and unpaused;
a `--profile` capture continues independently when the window is closed or paused.

- **Hide Present** removes the swap span and rescales the main-thread flame graph
  to pre-swap work, including clipping the parent Frame span. The frame-history
  graph also switches to pre-swap duration; the display-refresh budget is its guide.
- **Workers** shows a stacked execution breakdown (decode / I/O / other) and totals
  and means for completed jobs. Queue waiting is separate. Worker timings overlap
  across threads, so they are not drawn on the main-thread flame graph. A true worker
  timeline would need individual task spans, which these aggregate counters do not retain.
- Worker and **Tile latency** totals cover approximately the last second ending at
  the selected frame. **Selected frame only** narrows this to one frame. Averages
  use the total sample count; an idle window shows no samples rather than stale work.
- **Visible tiles** shows requested-level/plane readiness for the selected frame,
  including adjacent-plane fallback, missing, failed, empty, and indexing states.
- **Memory** shows the latest sampled values and their age at that frame.
- **Accumulated main-thread sections** includes every invocation in the frame;
  the flame graph retains only the last span of a repeated section.

Click a history bar or **Pause** to freeze the GUI's history and statistics. CSV
capture continues using the live ring, independently of the frozen GUI snapshot.
Resume returns to current data. The build configuration is displayed in the window.

## Profiling CSV

The existing profiler's ring buffer and UI remain available. `--profile` also streams
one CSV row per completed frame, continuing even if the profiler UI is paused. Output
is buffered, so normal shutdown is needed to retain its final rows. Each row contains:

- Time since capture began, frame number, measured frame duration, and start-to-start
  frame interval (which also reveals time outside the measured frame, such as sleeping).
- Explicit `pre_swap_ms`: frame start through the start of buffer swap. Blank if no
  swap occurred. With vsync enabled this is the primary main-thread performance
  metric; keep presentation time and frame intervals alongside it. A vsync-disabled
  run is optional and measures a different workload, not GPU execution time.
- Existing CPU/wall section timings in milliseconds, including presentation time.
  Nested sections are inclusive and should not be summed. Repeated invocations within
  a frame are accumulated in CSV; the flamegraph retains the last individual span.
  Upload and deferred texture finalization have separate sections. The SDL path also
  separates platform event handling from input-state processing. Windows reports
  intentional background input waiting in `Input Wait_ms`; this is included in
  pre-swap wall time but should be excluded when evaluating active processing.
  Timed playback disables this idling. These are not GPU timer queries.
- Build configuration (`build_configuration`, e.g. Debug or RelWithDebInfo).
- Camera position/actual zoom, viewport dimensions, display scale and image count.
- Replay segment and number of completed commands. A pending wait does not increment it.
- Process resident memory; estimated cached texture pixel bytes; cache-owned CPU pixels;
  hashmap storage/entry count; shared tile metadata; published MRXS descriptors; and idle
  buffers retained by the tile-buffer pool.

Memory is sampled about once per second and on image-count changes. Intervening rows
repeat the latest sample. Cache sampling scans under the cache lock; its time is shown
in `Memory sample_ms`, so these frames can be identified when comparing timings.

These counters are intentionally not a complete heap census: texture estimates exclude
driver overhead, CPU tile counts exclude in-flight buffers and backend-private caches,
and index counts exclude unpublished allocations. RSS includes memory from other parts
of the process and is not additive with these categories. Sampled peaks can miss short
spikes. The CSV capture itself also adds some overhead.

Use an optimized build (Release or RelWithDebInfo) for performance baselines; Debug
captures are useful for validating instrumentation but are not representative of
optimized performance. The summary prints the configuration stored in the CSV; older
captures report it as unrecorded.

For comparisons, distinguish fresh-process and warm-cache runs. Restarting the process
does not clear the OS filesystem cache. Capture several runs and compare frame-time
percentiles, loading segments and sampled memory peaks, rather than a single average.

### Tile readiness and latency

`visible_ready`, `visible_adjacent_plane`, `visible_missing`, `visible_failed`, and
`visible_empty` count tile positions in the viewport bounds at the requested drawing
level. They are refreshed every frame, aggregated across rendered image layers. Known
empty positions are excluded from the readiness denominator. Adjacent-plane fallback
is available imagery but not readiness of the selected plane. `visible_missing` may
still be covered by coarser imagery or a macro image; these counters do not measure
pixel coverage, stencil visibility, or the rotated viewport's exact footprint.
`visible_unready_levels` flags requested levels whose selected-plane index is not yet
published. Do not interpret a zero tile count during indexing as full readiness.

`ready_tiles`, `request_to_ready_sum_ms`, and `request_to_ready_max_ms` measure admitted
cache request to texture availability for rendering. This includes queueing, reading,
decoding, completion processing, and upload/finalization. It does not mean that GPU
execution has completed. Unlike first-draw latency, it does not include a prefetched
tile waiting to be drawn much later. Inspect both distributions together.

`first_draws`, `request_to_draw_sum_ms`, and `request_to_draw_max_ms` describe the first
draw lookup after an admitted cache request, including queueing, decoding, completion
handling, upload, and waiting to be drawn. Compute a run's mean as total sum / total
count, not an average of per-frame means. The CSV stores counts, sums, and maxima;
event-level latency percentiles cannot be reconstructed from these aggregates.
Cache hits without a new request contribute
no latency sample. Coarser/adjacent-plane draws count too; tiles never drawn are absent
from this distribution. This measures draw submission, not GPU completion or scanout.
Request timing is associated with a capture generation, so old requests cannot leak
into a restarted capture. The legacy iSyntax streamer bypasses cache admission and
therefore currently contributes readiness, but no request-to-draw latency samples.

### Worker timings

Worker counters are increments since the previous CSV row. Each completed job/batch
publishes once under a short lock; measurement itself uses thread-local storage.
`worker_load_sum_ms` is elapsed execution time, and `worker_queue_sum_ms` is elapsed
submission-to-start time. `worker_jobs` counts jobs/batches, not decoded tiles.
`worker_queued_jobs` is the denominator for queue-delay means. Legacy iSyntax streaming
jobs have execution measurements but no queue-delay samples. Jobs still executing at
capture end are not included; normal loading/closing synchronization is not changed.

`worker_io_sum_ms` covers source reads, direct file reads, and HTTP request/response
operations reached from those workers. `worker_io_bytes` counts successfully returned
payload bytes (not disk-device traffic, TLS overhead, or failed/partial HTTP bodies).
Nested source reads are counted once. HTTP timings include parsing and server waits;
connection setup outside the request belongs to the remaining worker time.
`worker_decode_sum_ms` covers the MRXS/TIFF/DICOM decode and pixel conversion stages,
shared JPEG decoding, and iSyntax tile/coefficient decoding. Nested codec calls count
once. Some iSyntax work runs in the streaming coordinator and remains in the residual
worker time. OpenSlide's opaque `read_region()` remains combined in worker load time;
its internal I/O/decode cannot be separated by these hooks. Uninstrumented codecs and
other work (allocation, compositing, locks, etc.) also remain in the residual.

Worker durations overlap across threads and may exceed wall time. I/O and decode
are subsets of load time, not additional work to add to it. Compare these counters,
readiness, and latency alongside pre-swap timing using the same replay, viewport,
vsync setting, build configuration, and cache conditions. The instrumentation adds
two timing fields per cache entry but introduces no texture budget or eviction policy.
