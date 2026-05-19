#!/bin/bash
# Build and flash CodexMeter firmware to the Waveshare 3.49 LCD board.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/cu.usbmodem1101}"
PIO_BIN="${PIO_BIN:-$HOME/.platformio/penv/bin/pio}"

if [ ! -x "$PIO_BIN" ]; then
    PIO_BIN="$(command -v pio || true)"
fi

if [ -z "$PIO_BIN" ]; then
    echo "Error: PlatformIO CLI was not found"
    exit 1
fi

echo "=== Flashing CodexMeter ==="
echo "Port: $PORT"
echo ""

cd "$SCRIPT_DIR/firmware"
"$PIO_BIN" run -e waveshare_lcd_349 -t upload --upload-port "$PORT"

echo ""
echo "=== Done! ==="
