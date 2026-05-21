#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any


def find_windows_serial_port(pattern: str) -> str | None:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        return None

    ports = list(list_ports.comports())
    if not ports:
        return None
    if pattern.lower() not in {"auto", "com*"}:
        return pattern

    def score(port: Any) -> tuple[int, int, str]:
        text = f"{port.device} {port.description or ''} {port.hwid or ''}".upper()
        usb_score = 0 if any(token in text for token in ("USB", "CP210", "CH340", "CDC", "UART", "JTAG")) else 1
        match = re.search(r"COM(\d+)", port.device.upper())
        port_number = int(match.group(1)) if match else 999
        return (usb_score, port_number, port.device)

    return sorted(ports, key=score)[0].device


def rgb565le_to_png(raw: bytes, width: int, height: int, output: Path) -> None:
    try:
        from PIL import Image  # type: ignore
    except ImportError as exc:
        raise SystemExit("Pillow is required. Run: python -m pip install -r daemon/requirements.txt") from exc

    if len(raw) != width * height * 2:
        raise SystemExit(f"Unexpected raw size: got {len(raw)}, expected {width * height * 2}")

    pixels = bytearray(width * height * 3)
    out = 0
    for index in range(0, len(raw), 2):
        value = raw[index] | (raw[index + 1] << 8)
        pixels[out] = ((value >> 11) & 0x1F) * 255 // 31
        pixels[out + 1] = ((value >> 5) & 0x3F) * 255 // 63
        pixels[out + 2] = (value & 0x1F) * 255 // 31
        out += 3

    output.parent.mkdir(parents=True, exist_ok=True)
    Image.frombytes("RGB", (width, height), bytes(pixels)).save(output)


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture a CodexMeter screenshot over serial")
    parser.add_argument("output", nargs="?", default="screenshot.png")
    parser.add_argument("port", nargs="?", default="auto")
    parser.add_argument("--baud", type=int, default=921600)
    args = parser.parse_args()

    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is required. Run: python -m pip install -r daemon/requirements.txt") from exc

    port = find_windows_serial_port(args.port) if sys.platform.startswith("win") else args.port
    if not port:
        raise SystemExit("No serial port found")

    print(f"Taking screenshot from {port}...")
    with serial.Serial(port, args.baud, timeout=10) as ser:
        ser.reset_input_buffer()
        ser.write(b"screenshot\n")
        ser.flush()

        while True:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line.startswith("SCREENSHOT_START"):
                parts = line.split()
                width, height, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
                break
            if line == "SCREENSHOT_ERR":
                raise SystemExit("Device reported screenshot error")

        data = bytearray()
        while len(data) < raw_size:
            chunk = ser.read(min(4096, raw_size - len(data)))
            if not chunk:
                raise SystemExit(f"Timeout: got {len(data)} of {raw_size} bytes")
            data.extend(chunk)

    output = Path(args.output)
    rgb565le_to_png(bytes(data), width, height, output)
    print(f"Saved: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
