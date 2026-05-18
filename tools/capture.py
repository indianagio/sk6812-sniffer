#!/usr/bin/env python3
"""
capture.py — Reads SK6812 frames from ESP32-H2 over serial and saves to .bin

Usage:
    python capture.py --port /dev/ttyACM0 --output frames.bin --count 100

Frame format (from firmware):
    0xAA 0xBB  — SOF
    uint16_le  — num_leds
    [R G B W] * num_leds
    0xCC 0xDD  — EOF
"""

import argparse
import serial
import struct
import sys
import time
from pathlib import Path

SOF = bytes([0xAA, 0xBB])
EOF_MARKER = bytes([0xCC, 0xDD])


def find_sof(ser: serial.Serial) -> bool:
    buf = b""
    while True:
        b = ser.read(1)
        if not b:
            return False
        buf = (buf + b)[-2:]
        if buf == SOF:
            return True


def read_frame(ser: serial.Serial):
    raw = ser.read(2)
    if len(raw) < 2:
        return None
    num_leds = struct.unpack("<H", raw)[0]
    if num_leds == 0 or num_leds > 1024:
        return None
    payload_size = num_leds * 4
    payload = ser.read(payload_size)
    if len(payload) < payload_size:
        return None
    footer = ser.read(2)
    if footer != EOF_MARKER:
        return None
    return num_leds, bytearray(payload)


def main():
    parser = argparse.ArgumentParser(description="SK6812 frame capture")
    parser.add_argument("--port",   required=True)
    parser.add_argument("--baud",   default=921600, type=int)
    parser.add_argument("--output", default="frames.bin")
    parser.add_argument("--count",  default=0, type=int, help="Stop after N frames (0=unlimited)")
    args = parser.parse_args()

    out_path = Path(args.output)
    frame_count = 0

    print(f"Opening {args.port} @ {args.baud} baud")
    print(f"Saving to {out_path}")
    print("Press Ctrl+C to stop\n")

    try:
        with serial.Serial(args.port, args.baud, timeout=2) as ser, \
             open(out_path, "wb") as f:

            f.write(b"SK6812CAP")
            f.write(struct.pack("<H", 1))

            start = time.time()
            while True:
                if not find_sof(ser):
                    print("Timeout waiting for SOF", file=sys.stderr)
                    continue

                result = read_frame(ser)
                if result is None:
                    print("Bad frame, re-syncing...", file=sys.stderr)
                    continue

                num_leds, rgbw = result
                ts = time.time() - start
                f.write(struct.pack("<dH", ts, num_leds))
                f.write(rgbw)
                f.flush()

                frame_count += 1
                if frame_count % 10 == 0:
                    print(f"\r  Captured {frame_count} frames "
                          f"({num_leds} LEDs) — {ts:.1f}s", end="", flush=True)

                if args.count and frame_count >= args.count:
                    print(f"\nReached {args.count} frames, stopping.")
                    break

    except KeyboardInterrupt:
        print(f"\nStopped. Captured {frame_count} frames → {out_path}")
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
