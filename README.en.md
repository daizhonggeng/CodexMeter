# CodexMeter

[简体中文](README.md) | [Third-Party Notes](THIRD_PARTY_NOTICES.md)

CodexMeter is an ESP32-S3 companion display for Codex Desktop. It runs on the
Waveshare ESP32-S3-Touch-LCD-3.49 and keeps the most useful live session data
visible on a dedicated screen: quota usage, reset times, recent Codex output,
context window progress, pet state, and recent session switching.

## Screenshots

| Live sync | Long reply | Short reply |
| --- | --- | --- |
| ![Live sync](screenshots/device-live.png) | ![Work log](screenshots/device-worklog.png) | ![Quick reply](screenshots/device-quick-answer.png) |

## Current Feature Set

### 1. Codex quota display

- Exact 5-hour quota percentage
- Exact weekly quota percentage
- Reset timestamps for both quota windows
- On-device manual quota refresh
- Visual refresh feedback while a refresh is pending or just completed

### 2. Codex output mirroring

- Reads the latest assistant output from the active session
- Wraps the text for the device layout and renders it in the right-side bubble
- Uses a larger centered layout for very short replies
- Uses multiline layout for longer replies
- Supports vertical scrolling in the message area for overflow content
- Shows sync state labels such as `SYNC`, `DONE`, `INPUT`, and `ERROR`

### 3. Context window progress

- Reads the current Codex input token count
- Reads the active model context window size
- Shows exact `used / total` values
- Displays a progress bar for context usage
- Applies normal / warning / alert colors based on usage level

### 4. Pet sync and animation

- Automatically reads the currently selected Codex pet
- Loads `pet.json` and `spritesheet.webp` from the local pet directory
- Pushes a new pet atlas over USB the first time that pet is selected
- Switches animation state over BLE or serial after the atlas is cached
- Supports idle / running / waiting / review / failed device states
- Restores the last written pet atlas from device-side cache after reboot

### 5. Recent session switching

- Reads the recent session list
- Displays up to 8 recent sessions on the device
- Tapping the message area toggles between output view and session list
- Vertical swipes move the selection inside the session list
- Tapping again confirms the selected session
- A short BOOT press jumps directly to the next recent session

### 6. Device integration and debugging

- Custom BLE GATT service for live sync
- USB serial for flashing, screenshots, pet atlas updates, and diagnostics
- Framebuffer screenshot export
- Local control page for daemon status and logs

## Interaction Model

### Touch gestures

| Area | Behavior |
| --- | --- |
| Left quota cards | Request a quota refresh |
| Middle pet area | Tap once to start running, tap again to return to idle |
| Right message area | Tap to open the recent session list; tap again to confirm selection |
| Right message area swipe | Scroll reply text, or move selection when the session list is open |

### Physical buttons

| Input | Behavior |
| --- | --- |
| BOOT short press | Jump to the next recent session |
| PWR long press for about 3 seconds | Shut the board down |

### Pet sync rules

1. The daemon watches the currently selected Codex pet.
2. If the device does not have that pet atlas yet, it asks for a USB update.
3. Once USB is connected, the daemon writes the current pet atlas into device cache.
4. After that, normal runtime updates only need animation-selection commands.

## How It Works

### Host daemon

`daemon/codexmeter_daemon.py` is responsible for:

- calling `codex app-server` for account and quota data
- reading Codex local state for recent and selected threads
- extracting recent assistant output and status from rollout logs
- loading the currently selected pet directory
- building compact payloads for quota, output, context, sessions, and pet state
- sending those payloads over BLE or serial

### Device firmware

`firmware/src/main.cpp` is responsible for:

- rendering the 640x172 three-column interface
- handling touch input and BOOT / PWR interactions
- parsing JSON payloads
- caching pet atlas / animation data locally
- driving pet animation and UI updates on-device

### Transport split

- **BLE**: normal live sync for quota, output, context, session list, and state updates
- **USB serial**: flashing, screenshots, pet atlas updates, and offline debugging

## Hardware

- Board: Waveshare ESP32-S3-Touch-LCD-3.49
- Logical UI layout: 640 x 172 landscape strip
- Native panel orientation: 172 x 640, mapped in firmware for landscape rendering
- Input: capacitive touch
- Power and shutdown: onboard PMU handling

## Quick Start

### 1. Install host dependencies

```bash
python3 -m pip install -r daemon/requirements.txt
```

### 2. Build firmware

```bash
pio run -d firmware -e waveshare_lcd_349
```

### 3. Flash over USB

```bash
./flash.sh /dev/cu.usbmodem1101
```

### 4. Start the daemon

```bash
python3 daemon/codexmeter_daemon.py \
  --watch \
  --ble \
  --sync-pet-sprite \
  --cwd "$PWD"
```

### 5. Install the macOS background service

```bash
./install.sh
```

## Useful Commands

### Screenshot capture

```bash
./screenshot.sh screenshots/capture.png /dev/cu.usbmodem1101
```

### Control page

Default control page:

```text
http://127.0.0.1:3490/
```

The control page exposes:

- current daemon mode
- recent sync status
- active serial port
- current pet
- recent error state
- daemon logs

## Important Parameters

### Daemon flags

- `--serial-port /dev/cu.usbmodem1101`
- `--auto-serial-port "/dev/cu.usbmodem*"`
- `--serial-baud 921600`
- `--control-port 3490`
- `--pet-dir PATH`
- `--pet-anim-frames codex`
- `--sync-pet-sprite`
- `--ble`
- `--watch`

### BLE service

| Channel | UUID | Direction |
| --- | --- | --- |
| Service | `6f1c0001-7f7d-4a2b-9b7a-3490c0d3e001` | service |
| RX | `6f1c0002-7f7d-4a2b-9b7a-3490c0d3e001` | host writes to device |
| TX | `6f1c0003-7f7d-4a2b-9b7a-3490c0d3e001` | device ack / nack notify |
| REQ | `6f1c0004-7f7d-4a2b-9b7a-3490c0d3e001` | device requests refresh from host |

Current advertised name:

```text
CodexMeter
```

## Repository Layout

| Path | Purpose |
| --- | --- |
| `firmware/` | PlatformIO firmware project |
| `firmware/src/main.cpp` | main device UI, touch, serial protocol, and pet cache |
| `firmware/src/ble.*` | BLE sync service |
| `daemon/codexmeter_daemon.py` | host-side sync daemon |
| `screenshots/` | device screenshots used in the docs |
| `flash.sh` | serial flashing helper |
| `install.sh` | macOS LaunchAgent installer |
| `screenshot.sh` | framebuffer screenshot helper |

## Project Status

This is a daily-usable personal hardware project build focused on:

- at-a-glance readability on the device
- accurate quota and output sync
- pet switching with local device cache
- efficient session switching on a small display

Natural next steps, if the project keeps growing, would be:

- fuller Bluetooth onboarding and pairing flows
- more device-side views
- richer pet state mapping
- deeper diagnostic screens

