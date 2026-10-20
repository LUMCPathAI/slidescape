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

## Profiling CSV

The existing profiler's ring buffer and UI remain available. `--profile` also streams
one CSV row per completed frame, continuing even if the profiler UI is paused. Output
is buffered, so normal shutdown is needed to retain its final rows. Each row contains:

- Time since capture began, frame number, measured frame duration, and start-to-start
  frame interval (which also reveals time outside the measured frame, such as sleeping).
- Existing CPU/wall section timings in milliseconds, including presentation time.
  Nested sections are inclusive and should not be summed. These are not GPU timer queries.
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

For comparisons, distinguish fresh-process and warm-cache runs. Restarting the process
does not clear the OS filesystem cache. Capture several runs and compare frame-time
percentiles, loading segments and sampled memory peaks, rather than a single average.
