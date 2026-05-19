#!/bin/bash
# Take a screenshot from the CodexMeter display via LVGL snapshot.
# Usage: ./screenshot.sh [output.png] [port]
set -euo pipefail

OUTPUT="${1:-screenshot.png}"
PORT="${2:-/dev/cu.usbmodem1101}"
SERIAL_PYTHON="${SERIAL_PYTHON:-}"

TMPRAW=$(mktemp -t codexmeter_screenshot_raw)
TMPMETA=$(mktemp -t codexmeter_screenshot_meta)
trap "rm -f '$TMPRAW' '$TMPMETA'" EXIT

if [ -z "$SERIAL_PYTHON" ]; then
    for candidate in "$HOME/.platformio/penv/bin/python" python3; do
        if [ -x "$candidate" ] || command -v "$candidate" >/dev/null 2>&1; then
            if "$candidate" -c 'import serial' >/dev/null 2>&1; then
                SERIAL_PYTHON="$candidate"
                break
            fi
        fi
    done
fi

if [ -z "$SERIAL_PYTHON" ]; then
    echo "Error: no Python interpreter with pyserial was found"
    exit 1
fi

echo "Taking screenshot from $PORT..."

"$SERIAL_PYTHON" - "$PORT" "$TMPRAW" "$TMPMETA" << 'PYEOF'
import serial, sys

port_path, raw_path, meta_path = sys.argv[1], sys.argv[2], sys.argv[3]

port = serial.Serial(port_path, 921600, timeout=10)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
        sys.exit(1)
    data += chunk

with open(raw_path, "wb") as f:
    f.write(data)
with open(meta_path, "w", encoding="utf-8") as f:
    f.write(f"{w}x{h}\n")

for _ in range(10):
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line == "SCREENSHOT_END":
        break

port.close()
print(f"Captured {w}x{h} ({len(data)} bytes)")
PYEOF

VIDEO_SIZE=$(cat "$TMPMETA")

ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size "$VIDEO_SIZE" \
    -i "$TMPRAW" -update 1 -frames:v 1 "$OUTPUT" >/dev/null 2>&1

if [ -f "$OUTPUT" ]; then
    echo "Saved: $OUTPUT"
else
    echo "Error: conversion failed"
    exit 1
fi
