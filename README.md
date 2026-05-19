# CodexMeter

CodexMeter is a personal ESP32-S3 companion display for Codex Desktop. It runs
on the Waveshare ESP32-S3-Touch-LCD-3.49 and mirrors the parts of a live Codex
session that are useful at a glance: quota usage, current reply text, context
window progress, and the active pet.

The project started from the interaction ideas and early code structure of
Clawdmeter, then was rebuilt around Codex Desktop, this display format, and a
BLE plus USB sync flow that matches the current hardware.

## What It Shows

- Exact 5 hour and weekly Codex quota usage
- Reset timestamps for both quota windows
- Current project and live reply text from the active Codex session
- Context window usage with exact token counts
- The currently selected Codex pet, synced from the local Codex pet directory
- Recent session switching from the device

## Screenshots

| Live sync | Work log | Quick reply |
| --- | --- | --- |
| ![Live sync](screenshots/device-live.png) | ![Work log](screenshots/device-worklog.png) | ![Quick reply](screenshots/device-quick-answer.png) |

## Hardware

- Board: Waveshare ESP32-S3-Touch-LCD-3.49
- Display: 640x172 landscape UI rendered onto the panel's 172x640 native space
- Host: macOS with Codex Desktop or Codex CLI already authenticated
- Transport:
  - BLE for normal live payloads and device requests
  - USB serial for flashing, screenshots, and pet atlas updates

## Device Interaction

| Input | Behavior |
| --- | --- |
| Touch quota cards | Request an immediate quota refresh |
| Touch pet area | Toggle pet running animation |
| Touch message area | Open or select from the recent session list |
| Swipe in message area | Scroll output or move in the session list |
| BOOT short press | Jump to the next recent session |
| PWR long press | Shut down the board |

## Quick Start

Install Python dependencies for the host daemon:

```bash
python3 -m pip install -r daemon/requirements.txt
```

Build the firmware:

```bash
pio run -d firmware -e waveshare_lcd_349
```

Flash over USB:

```bash
./flash.sh /dev/cu.usbmodem1101
```

Start the daemon manually:

```bash
python3 daemon/codexmeter_daemon.py \
  --watch \
  --ble \
  --sync-pet-sprite \
  --cwd "$PWD"
```

Install the macOS LaunchAgent:

```bash
./install.sh
```

Useful daemon options:

- `--serial-port /dev/cu.usbmodem1101`
- `--auto-serial-port "/dev/cu.usbmodem*"`
- `--control-port 3490`
- `--pet-dir PATH`
- `--pet-anim-frames codex`

Control endpoint:

```text
http://127.0.0.1:3490/
```

Framebuffer screenshot capture:

```bash
./screenshot.sh screenshots/capture.png /dev/cu.usbmodem1101
```

## BLE Data Service

The firmware keeps the existing custom GATT UUID layout so the host daemon and
device stay aligned:

| Channel | UUID | Direction |
| --- | --- | --- |
| Data service | `4c41555a-4465-7669-6365-000000000001` | service |
| RX | `4c41555a-4465-7669-6365-000000000002` | host writes JSON |
| TX | `4c41555a-4465-7669-6365-000000000003` | device ack or nack notify |
| REQ | `4c41555a-4465-7669-6365-000000000004` | device request notify |

The current advertised BLE name is `CodexMeter`.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `firmware/` | PlatformIO firmware for the display and touch device |
| `firmware/src/main.cpp` | Current 640x172 strip UI, serial protocol, pet cache, touch handling |
| `firmware/src/ble.*` | BLE data service and request notifications |
| `daemon/codexmeter_daemon.py` | Host daemon for quota sync, reply mirroring, pet sync, and control page |
| `screenshots/` | Real framebuffer captures from the device used in this README |

## Attribution And Notices

CodexMeter is a personal derivative project inspired by Clawdmeter. The current
repo is focused on Codex-specific hardware behavior and documentation, but it
still carries some lineage in protocol shape and history. Third-party notices
and publishing caveats are collected in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
