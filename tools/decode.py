#!/usr/bin/env python3
"""
decode.py — Reads frames.bin and exports to JSON or CSV, or prints a summary.

Usage:
    python decode.py --input frames.bin --format json --output frames.json
    python decode.py --input frames.bin --format csv  --output frames.csv
    python decode.py --input frames.bin --summary
"""

import argparse
import struct
import json
import csv
import sys
from pathlib import Path

MAGIC = b"SK6812CAP"


def read_frames(path: Path):
    with open(path, "rb") as f:
        magic = f.read(len(MAGIC))
        if magic != MAGIC:
            raise ValueError(f"Not a SK6812CAP file (magic={magic!r})")
        version = struct.unpack("<H", f.read(2))[0]
        if version != 1:
            raise ValueError(f"Unsupported version {version}")
        while True:
            hdr = f.read(10)
            if len(hdr) < 10:
                break
            ts, num_leds = struct.unpack("<dH", hdr)
            payload = f.read(num_leds * 4)
            if len(payload) < num_leds * 4:
                break
            leds = []
            for i in range(num_leds):
                r, g, b, w = payload[i*4], payload[i*4+1], payload[i*4+2], payload[i*4+3]
                leds.append({"r": r, "g": g, "b": b, "w": w})
            yield ts, num_leds, leds


def export_json(frames, out_path: Path):
    data = []
    for ts, num_leds, leds in frames:
        data.append({"timestamp": round(ts, 4), "num_leds": num_leds, "leds": leds})
    with open(out_path, "w") as f:
        json.dump(data, f, indent=2)
    print(f"Exported {len(data)} frames → {out_path}")


def export_csv(frames, out_path: Path):
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "led_index", "r", "g", "b", "w"])
        count = 0
        for ts, num_leds, leds in frames:
            for i, led in enumerate(leds):
                writer.writerow([f"{ts:.4f}", i, led["r"], led["g"], led["b"], led["w"]])
            count += 1
    print(f"Exported {count} frames → {out_path}")


def print_summary(frames):
    count = 0
    first_ts = None
    last_ts = 0.0
    num_leds = 0
    for ts, nl, leds in frames:
        if first_ts is None:
            first_ts = ts
            num_leds = nl
        last_ts = ts
        count += 1
    if count == 0:
        print("No frames found.")
        return
    duration = last_ts - (first_ts or 0)
    fps = count / duration if duration > 0 else 0
    print(f"Frames   : {count}")
    print(f"LEDs     : {num_leds}")
    print(f"Duration : {duration:.2f} s")
    print(f"Avg FPS  : {fps:.1f}")


def main():
    parser = argparse.ArgumentParser(description="SK6812 frame decoder")
    parser.add_argument("--input",   default="frames.bin")
    parser.add_argument("--format",  choices=["json", "csv"], default="json")
    parser.add_argument("--output",  default=None)
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()

    in_path = Path(args.input)
    if not in_path.exists():
        print(f"File not found: {in_path}", file=sys.stderr)
        sys.exit(1)

    if args.summary:
        print_summary(read_frames(in_path))
        return

    out_path = Path(args.output or f"frames.{args.format}")
    if args.format == "json":
        export_json(read_frames(in_path), out_path)
    else:
        export_csv(read_frames(in_path), out_path)


if __name__ == "__main__":
    main()
