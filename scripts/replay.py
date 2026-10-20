#!/usr/bin/env python3
"""Encode/dump Slidescape binary replays and summarize profiling CSV files."""
import argparse
import csv
import json
import math
from pathlib import Path
import struct

MAGIC = b"SCREPLAY" + struct.pack("<I", 1)
COMMANDS = {"open": 1, "wait": 2, "camera": 3, "close": 4, "exit": 5}
MAX_SIZE = 64 * 1024 * 1024


def encode(commands):
    output = bytearray(MAGIC)
    previous = 0.0
    for command in commands:
        time = float(command["time"])
        name = command["command"]
        args = command.get("args", [])
        if not math.isfinite(time) or time < previous:
            raise ValueError("timestamps must be finite, nonnegative and ordered within each segment")
        expected = {"open": (1,), "wait": (0,), "camera": (2, 3), "close": (0,), "exit": (0,)}
        if name not in COMMANDS or len(args) not in expected[name]:
            raise ValueError("unknown command or incorrect argument count")
        output.extend(struct.pack("<fII", time, COMMANDS[name], len(args)))
        for i, arg in enumerate(args):
            if name == "open":
                if not isinstance(arg, str) or not arg or "\0" in arg:
                    raise ValueError("open expects a nonempty filename")
                payload = arg.encode("utf-8") + b"\0"
                if len(payload) > 65536:
                    raise ValueError("filename too long")
            else:
                value = float(arg)
                if not math.isfinite(value) or (i < 2 and abs(value) > 1e9) or (i == 2 and not -16 <= value <= 30):
                    raise ValueError("camera argument outside supported range")
                payload = struct.pack("<f", value)
            output.extend(struct.pack("<I", len(payload)))
            output.extend(payload)
        previous = 0.0 if name == "wait" else time
        if len(output) > MAX_SIZE:
            raise ValueError("replay exceeds 64 MiB")
    return output


def decode(data):
    if not data.startswith(MAGIC) or len(data) > MAX_SIZE:
        raise ValueError("invalid header/version or oversized replay")
    cursor = len(MAGIC)
    names = {value: key for key, value in COMMANDS.items()}
    commands = []
    while cursor < len(data):
        time, kind, count = struct.unpack_from("<fII", data, cursor)
        cursor += 12
        if kind not in names or count > 3:
            raise ValueError("invalid command")
        args = []
        for _ in range(count):
            size, = struct.unpack_from("<I", data, cursor)
            cursor += 4
            if size > len(data) - cursor:
                raise ValueError("truncated argument")
            payload = data[cursor:cursor + size]
            cursor += size
            if kind == COMMANDS["open"]:
                if not 2 <= size <= 65536:
                    raise ValueError("invalid filename size")
                args.append(payload[:-1].decode("utf-8"))
            else:
                args.append(struct.unpack("<f", payload)[0])
        commands.append({"time": time, "command": names[kind], "args": args})
    encode(commands)  # Validate schemas, ordering and numeric values too.
    return commands


def summary(path):
    with path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("profile has no frames")
    def report_times(label, values):
        times = sorted(values)
        if not times:
            return
        def percentile(fraction):
            return times[math.ceil(fraction * len(times)) - 1]
        print(f"{label} ms: mean {sum(times)/len(times):.3f}, p50 {percentile(.5):.3f}, "
              f"p95 {percentile(.95):.3f}, p99 {percentile(.99):.3f}, max {times[-1]:.3f}")

    def pre_swap(row):
        if "pre_swap_ms" in row:
            return float(row["pre_swap_ms"]) if row["pre_swap_ms"] else None
        # Compatibility with captures made before explicit pre-swap timing.
        return max(0, float(row["frame_ms"]) - float(row["Present_ms"]))

    print(f"{len(rows)} frames; {float(rows[-1]['seconds']):.3f} seconds")
    print("Build configuration: " + ", ".join(sorted({row.get("build_configuration", "unrecorded") for row in rows})))
    report_times("Frame", [float(row["frame_ms"]) for row in rows])
    report_times("Pre-swap", [value for row in rows if (value := pre_swap(row)) is not None])
    report_times("Presentation", [float(row["Present_ms"]) for row in rows])
    for segment in sorted({int(row["replay_segment"]) for row in rows}):
        selected = [row for row in rows if int(row["replay_segment"]) == segment]
        report_times(f"Segment {segment} pre-swap",
                     [value for row in selected if (value := pre_swap(row)) is not None])
    if "visible_ready" in rows[0]:
        measured = complete = 0
        for row in rows:
            required = sum(int(row[key]) for key in
                           ("visible_ready", "visible_adjacent_plane", "visible_missing", "visible_failed"))
            if required and not int(row["visible_unready_levels"]):
                measured += 1
                complete += int(row["visible_ready"]) == required
        print(f"Requested-level/plane fully ready: {complete}/{measured} measured frames "
              "(known empty positions excluded; viewport tile counts, not pixel coverage)")
        if "ready_tiles" in rows[0]:
            ready = sum(int(row["ready_tiles"]) for row in rows)
            if ready:
                total = sum(float(row["request_to_ready_sum_ms"]) for row in rows)
                maximum = max(float(row["request_to_ready_max_ms"]) for row in rows)
                print(f"Request to texture ready: {ready} samples, mean {total/ready:.3f} ms, max {maximum:.3f} ms")
        count = sum(int(row["first_draws"]) for row in rows)
        if count:
            total = sum(float(row["request_to_draw_sum_ms"]) for row in rows)
            maximum = max(float(row["request_to_draw_max_ms"]) for row in rows)
            print(f"Request to first draw: {count} samples, mean {total/count:.3f} ms, max {maximum:.3f} ms")
        jobs = sum(int(row["worker_jobs"]) for row in rows)
        queued = sum(int(row["worker_queued_jobs"]) for row in rows)
        print(f"Completed worker jobs/batches: {jobs}; queue-delay samples: {queued}")
        for name in ("worker_queue_sum_ms", "worker_load_sum_ms", "worker_io_sum_ms", "worker_decode_sum_ms"):
            print(f"Total {name}: {sum(float(row[name]) for row in rows):.3f}")
        print("Worker durations overlap across threads; I/O/decode are subsets of worker load time.")
    for name in ("resident_bytes", "texture_bytes", "cpu_tile_bytes", "cache_table_bytes",
                 "tile_metadata_bytes", "mrxs_index_bytes", "pooled_bytes"):
        peak = max(int(row[name]) for row in rows)
        print(f"Peak sampled {name}: {peak / 2**20:.2f} MiB")
    print("Memory values are sampled estimates; categories must not be added to resident memory.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    make = sub.add_parser("encode", help="compile a JSON command array to binary")
    make.add_argument("input", type=Path)
    make.add_argument("output", type=Path)
    dump = sub.add_parser("dump", help="print binary commands as JSON")
    dump.add_argument("input", type=Path)
    stats = sub.add_parser("summary", help="summarize a profiling CSV")
    stats.add_argument("input", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "encode":
            if args.input.resolve() == args.output.resolve():
                raise ValueError("input and output must differ")
            args.output.write_bytes(encode(json.loads(args.input.read_text())))
        elif args.action == "dump":
            if args.input.stat().st_size > MAX_SIZE:
                raise ValueError("replay exceeds 64 MiB")
            print(json.dumps(decode(args.input.read_bytes()), indent=2))
        else:
            summary(args.input)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, struct.error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
