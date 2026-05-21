#!/usr/bin/env python3
"""Codex pet hardware daemon.

First-version goals:
- read Codex account/rate-limit state through `codex app-server`
- emit the compact `codex_usage` payload understood by the ESP32 firmware
- optionally preview the current Codex pet spritesheet frames
- optionally send compact JSON payloads over BLE when `bleak` is installed

The daemon intentionally does not print auth tokens or full account metadata.
"""

from __future__ import annotations

import argparse
import asyncio
import fnmatch
import glob
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import queue
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from datetime import datetime
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEVICE_NAME = "CodexMeter"
DEFAULT_AUTO_SERIAL_PORT = "auto" if os.name == "nt" else "/dev/cu.usbmodem*"
SERVICE_UUID = "6f1c0001-7f7d-4a2b-9b7a-3490c0d3e001"
RX_CHAR_UUID = "6f1c0002-7f7d-4a2b-9b7a-3490c0d3e001"
REQ_CHAR_UUID = "6f1c0004-7f7d-4a2b-9b7a-3490c0d3e001"
_BLE_BRIDGE: "BleBridge | None" = None
_BLE_REFRESH_EVENT = threading.Event()
_LAST_PET_SYNC_KEY: tuple[Any, ...] | None = None
_LAST_PET_ATLAS_KEY: tuple[Any, ...] | None = None
_LAST_PET_ATLAS_MAP: dict[int, int] = {}
_LAST_PET_SELECT_KEY: tuple[Any, ...] | None = None
_LAST_OUTPUT_IMAGE_KEY: tuple[Any, ...] | None = None


def default_runtime_dir() -> Path:
    if os.name == "nt":
        root = os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()
        return Path(root) / "CodexMeter"
    return Path(tempfile.gettempdir())


_RUNTIME_DIR = default_runtime_dir()
_STATE_PATH = _RUNTIME_DIR / "codexmeter-sync-state.json"
_RUNTIME_STATE_PATH = _RUNTIME_DIR / "codexmeter-runtime-state.json"
_LOG_PATH = _RUNTIME_DIR / "codexmeter-daemon.log"
_DAEMON_STARTED_AT_MS = int(time.time() * 1000)
_DISPLAYED_ASSISTANT_MESSAGE_KEYS: set[tuple[int, str]] = set()
_SESSION_SELECT_EVENT = threading.Event()
_SESSION_LIST_EVENT = threading.Event()
_SESSION_NEXT_EVENT = threading.Event()
_SESSION_SELECT_LOCK = threading.Lock()
_SESSION_SELECT_INDEX: int | None = None
_SELECTED_THREAD_ID: str | None = None

CODEX_AVATAR_COLS = 8
CODEX_AVATAR_ROWS = 9
CODEX_IDLE_FRAMES = [
    (0, 0, 280),
    (0, 1, 110),
    (0, 2, 110),
    (0, 3, 140),
    (0, 4, 140),
    (0, 5, 320),
]
CODEX_IDLE_DURATION_MULTIPLIER = 6
CODEX_STATE_ROWS = {
    "failed": (5, 8, 140, 240),
    "jumping": (4, 5, 140, 280),
    "review": (8, 6, 150, 280),
    "running": (1, 8, 120, 220),
    "running-left": (2, 8, 120, 220),
    "running-right": (1, 8, 120, 220),
    "waving": (3, 4, 140, 280),
    "waiting": (6, 6, 150, 260),
}
DEVICE_ANIM_DURATION_MULTIPLIER = 2
BLE_WRITE_CHUNK_TEXT = 360


class CodexAppServerError(RuntimeError):
    pass


class CodexAppServerClient:
    def __init__(self, codex_bin: str = "codex", timeout_s: float = 20.0) -> None:
        self.codex_bin = codex_bin
        self.timeout_s = timeout_s
        self._proc: subprocess.Popen[str] | None = None
        self._queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self._reader: threading.Thread | None = None
        self._next_id = 1

    def __enter__(self) -> "CodexAppServerClient":
        self.start()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def start(self) -> None:
        if self._proc:
            return
        self._proc = subprocess.Popen(
            [self.codex_bin, "app-server", "--listen", "stdio://"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._reader.start()
        self.request(
            "initialize",
            {
                "clientInfo": {
                    "name": "codexmeter",
                    "title": "CodexMeter",
                    "version": "0.1.0",
                },
                "capabilities": {"experimentalApi": True},
            },
        )

    def close(self) -> None:
        proc = self._proc
        self._proc = None
        if not proc:
            return
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    def _read_stdout(self) -> None:
        assert self._proc and self._proc.stdout
        for line in self._proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self._queue.put(json.loads(line))
            except json.JSONDecodeError:
                continue

    def request(self, method: str, params: Any = None) -> Any:
        if not self._proc or not self._proc.stdin:
            raise CodexAppServerError("app-server is not running")
        req_id = self._next_id
        self._next_id += 1
        message: dict[str, Any] = {"id": req_id, "method": method}
        if params is not None:
            message["params"] = params
        self._proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self._proc.stdin.flush()

        deadline = time.monotonic() + self.timeout_s
        while time.monotonic() < deadline:
            try:
                reply = self._queue.get(timeout=0.25)
            except queue.Empty:
                if self._proc.poll() is not None:
                    raise CodexAppServerError("app-server exited before replying")
                continue
            if reply.get("id") != req_id:
                continue
            if "error" in reply:
                raise CodexAppServerError(f"{method} failed: {reply['error']}")
            return reply.get("result")
        raise CodexAppServerError(f"{method} timed out")

    def read_account(self) -> dict[str, Any]:
        return self.request("account/read", {"refreshToken": False})

    def read_rate_limits(self) -> dict[str, Any]:
        return self.request("account/rateLimits/read")


@dataclass
class UsagePayload:
    payload: dict[str, Any]
    summary: str


@dataclass
class CodexThreadNotification:
    title: str
    body: str | None
    status: str
    status_label: str
    mascot_state: str
    rollout_path: Path | None
    context_input_tokens: int | None = None
    context_window_tokens: int | None = None


@dataclass
class RecentSession:
    thread_id: str
    title: str
    cwd: Path
    rollout_path: Path | None
    updated_at_ms: int | None


def reset_minutes(window: dict[str, Any] | None, now: float) -> int:
    if not window:
        return -1
    resets_at = window.get("resetsAt")
    if resets_at is None:
        return -1
    return max(0, int(round((float(resets_at) - now) / 60.0)))


def reset_label(window: dict[str, Any] | None, now: float, fallback_mins: int) -> str:
    if not window or window.get("resetsAt") is None:
        return ""
    try:
        target = datetime.fromtimestamp(float(window["resetsAt"]))
    except (TypeError, ValueError, OSError):
        return ""

    today = datetime.fromtimestamp(now).date()
    if target.date() == today:
        return target.strftime("%H:%M RST")
    if fallback_mins >= 0 and fallback_mins < 7 * 24 * 60:
        return target.strftime("%a %H:%M")
    return target.strftime("%m/%d %H:%M")


def select_limit(rate_result: dict[str, Any], limit_id: str) -> dict[str, Any]:
    by_id = rate_result.get("rateLimitsByLimitId") or {}
    if limit_id in by_id:
        return by_id[limit_id]
    fallback = rate_result.get("rateLimits")
    if fallback:
        return fallback
    raise CodexAppServerError("no rate-limit snapshot returned by app-server")


def build_usage_payload(
    account_result: dict[str, Any],
    rate_result: dict[str, Any],
    limit_id: str,
) -> UsagePayload:
    now = time.time()
    account = account_result.get("account") or {}
    snapshot = select_limit(rate_result, limit_id)
    primary = snapshot.get("primary") or {}
    secondary = snapshot.get("secondary") or {}
    plan = snapshot.get("planType") or account.get("planType") or "unknown"
    actual_limit = snapshot.get("limitId") or limit_id
    limited = bool(snapshot.get("rateLimitReachedType"))
    status = "limited" if limited else "allowed"
    primary_reset_mins = reset_minutes(primary, now)
    weekly_reset_mins = reset_minutes(secondary, now)

    payload = {
        "kind": "codex_usage",
        "ok": True,
        "plan": plan,
        "limit": actual_limit,
        "p": int(primary.get("usedPercent") or 0),
        "pr": primary_reset_mins,
        "prl": reset_label(primary, now, primary_reset_mins),
        "w": int(secondary.get("usedPercent") or 0),
        "wr": weekly_reset_mins,
        "wrl": reset_label(secondary, now, weekly_reset_mins),
        "st": status,
    }
    summary = (
        f"plan={plan} limit={actual_limit} "
        f"5h={payload['p']}% reset={payload['pr']}m "
        f"week={payload['w']}% reset={payload['wr']}m "
        f"status={status}"
    )
    return UsagePayload(payload=payload, summary=summary)


def find_default_pet_dir() -> Path | None:
    for codex_home in codex_home_candidates():
        selected = selected_pet_id(codex_home)
        if selected:
            pet_dir = codex_home / "pets" / selected
            if (pet_dir / "pet.json").exists():
                return pet_dir

        pets_dir = codex_home / "pets"
        if pets_dir.exists():
            for pet_json in sorted(pets_dir.glob("*/pet.json")):
                return pet_json.parent
    return None


def codex_home_candidates() -> list[Path]:
    candidates: list[Path] = []
    env_home = os.environ.get("CODEX_HOME")
    if env_home:
        candidates.append(Path(env_home))
    candidates.append(Path.home() / ".codex")
    userprofile = os.environ.get("USERPROFILE")
    if userprofile:
        candidates.append(Path(userprofile) / ".codex")

    unique: list[Path] = []
    seen: set[str] = set()
    for path in candidates:
        key = str(path.expanduser())
        if key not in seen:
            unique.append(path.expanduser())
            seen.add(key)
    return unique


def selected_pet_id(codex_home: Path) -> str | None:
    state_path = codex_home / ".codex-global-state.json"
    if not state_path.exists():
        return None
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    atom_state = state.get("electron-persisted-atom-state") or state
    selected = atom_state.get("selected-avatar-id")
    if not isinstance(selected, str) or not selected:
        return None
    if selected.startswith("custom:"):
        return selected.split(":", 1)[1]
    return selected


def load_pet_state(pet_dir: Path | None, state: str) -> dict[str, Any] | None:
    if not pet_dir:
        return None
    pet_json = pet_dir / "pet.json"
    if not pet_json.exists():
        return None
    info = json.loads(pet_json.read_text(encoding="utf-8"))
    return {
        "kind": "pet_state",
        "pet": info.get("id", pet_dir.name),
        "name": info.get("displayName", pet_dir.name),
        "state": state,
        "anim": state,
        "mood": 2 if state in {"thinking", "running"} else 1,
    }


def parse_pair(value: str, default: tuple[int, int]) -> tuple[int, int]:
    try:
        left, right = value.lower().split("x", 1)
        parsed = (int(left), int(right))
        if parsed[0] > 0 and parsed[1] > 0:
            return parsed
    except (AttributeError, ValueError):
        pass
    return default


def parse_indexes(value: str) -> list[int]:
    out: list[int] = []
    for part in value.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            parsed = int(part)
        except ValueError:
            continue
        if parsed >= 0:
            out.append(parsed)
    return out or [0]


def codex_avatar_frame_index(row: int, col: int) -> int:
    return row * CODEX_AVATAR_COLS + col


def codex_idle_sequence() -> list[tuple[int, int]]:
    return [
        (codex_avatar_frame_index(row, col), duration * CODEX_IDLE_DURATION_MULTIPLIER)
        for row, col, duration in CODEX_IDLE_FRAMES
    ]


def codex_state_sequence(state: str) -> tuple[list[int], list[int], int]:
    normalized = (state or "idle").strip().lower()
    if normalized in {"thinking", "busy", "working"}:
        normalized = "running"
    idle = codex_idle_sequence()
    if normalized == "idle" or normalized not in CODEX_STATE_ROWS:
        return [frame for frame, _duration in idle], [duration for _frame, duration in idle], 0

    row, count, duration, last_duration = CODEX_STATE_ROWS[normalized]
    action = [
        (codex_avatar_frame_index(row, col), last_duration if col == count - 1 else duration)
        for col in range(count)
    ]
    sequence = action
    return (
        [frame for frame, _duration in sequence],
        [duration for _frame, duration in sequence],
        0,
    )


def device_state_sequence(state: str) -> tuple[list[int], list[int], int]:
    samples, durations, loop_start = codex_state_sequence(state)
    normalized = (state or "idle").strip().lower()
    if normalized in {"thinking", "busy", "working"}:
        normalized = "running"
    if normalized != "idle" and normalized in CODEX_STATE_ROWS:
        durations = [duration * DEVICE_ANIM_DURATION_MULTIPLIER for duration in durations]
    return samples, durations, loop_start


def codex_durations_for_frames(frames: list[int]) -> list[int] | None:
    for state in ("idle", "running", "review", "waiting", "failed"):
        state_frames, durations, _loop_start = device_state_sequence(state)
        if frames == state_frames:
            return durations
    return None


def codex_state_atlas_frames() -> list[int]:
    ordered: list[int] = []
    seen: set[int] = set()
    states = ["idle", "running", "review", "waiting", "failed"]
    for state in states:
        frames, _durations, _loop_start = codex_state_sequence(state)
        for frame in frames:
            if frame not in seen:
                seen.add(frame)
                ordered.append(frame)
    return ordered


def rgb565_bytes(image: Any) -> bytes:
    rgb = image.convert("RGB")
    out = bytearray(rgb.width * rgb.height * 2)
    idx = 0
    pixels = rgb.get_flattened_data() if hasattr(rgb, "get_flattened_data") else rgb.getdata()
    for r, g, b in pixels:
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[idx] = value & 0xFF
        out[idx + 1] = value >> 8
        idx += 2
    return bytes(out)


def cjk_font_path() -> str | None:
    candidates = [
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Supplemental/Songti.ttc",
    ]
    for path in candidates:
        if Path(path).exists():
            return path
    return None


def text_width(draw: Any, text: str, font: Any) -> int:
    box = draw.textbbox((0, 0), text, font=font)
    return box[2] - box[0]


def wrap_visual_text(draw: Any, text: str, font: Any, max_width: int, max_lines: int) -> list[str]:
    text = re.sub(r"\s+", " ", text.replace("\n", " ")).strip()
    if not text:
        return []
    lines: list[str] = []
    current = ""
    for ch in text:
        candidate = current + ch
        if current and text_width(draw, candidate, font) > max_width:
            lines.append(current.rstrip())
            current = ch.lstrip()
            if len(lines) >= max_lines:
                break
        else:
            current = candidate
    if current and len(lines) < max_lines:
        lines.append(current.rstrip())
    if len(lines) > max_lines:
        lines = lines[:max_lines]
    if len(lines) == max_lines and text_width(draw, lines[-1], font) > max_width - 12:
        while lines[-1] and text_width(draw, lines[-1] + "...", font) > max_width:
            lines[-1] = lines[-1][:-1]
        lines[-1] = lines[-1].rstrip() + "..."
    elif len(lines) == max_lines and len("".join(lines)) < len(text):
        while lines[-1] and text_width(draw, lines[-1] + "...", font) > max_width:
            lines[-1] = lines[-1][:-1]
        lines[-1] = lines[-1].rstrip() + "..."
    return lines


def render_codex_output_image(project: str, status: str, body: str, footer: str) -> tuple[int, int, bytes]:
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as exc:
        raise SystemExit("Pillow is required for --sync-output-image") from exc

    w, h = 320, 172
    img = Image.new("RGB", (w, h), (7, 9, 8))
    draw = ImageDraw.Draw(img)
    font_path = cjk_font_path()
    if font_path:
        title_font = ImageFont.truetype(font_path, 11)
        project_font = ImageFont.truetype(font_path, 14)
        status_font = ImageFont.truetype(font_path, 12)
        body_font = ImageFont.truetype(font_path, 19)
        short_body_font = ImageFont.truetype(font_path, 30)
        footer_font = ImageFont.truetype(font_path, 10)
    else:
        title_font = project_font = status_font = body_font = short_body_font = footer_font = ImageFont.load_default()

    draw.text((12, 12), limit_text(project, 34), fill=(166, 240, 218), font=project_font)
    pill = status or "Running"
    pill_w = max(54, text_width(draw, pill, status_font) + 18)
    draw.rounded_rectangle((w - pill_w - 13, 9, w - 13, 29), radius=10,
                           fill=(13, 25, 25), outline=(52, 220, 154), width=1)
    draw.text((w - pill_w - 4, 12), pill, fill=(142, 231, 192), font=status_font)

    bubble = (28, 43, 306, 128)
    draw.rounded_rectangle(bubble, radius=16, fill=(245, 246, 247), outline=(205, 213, 220), width=1)
    draw.polygon([(28, 76), (10, 86), (28, 96)], fill=(245, 246, 247))
    draw.line([(28, 76), (10, 86), (28, 96)], fill=(205, 213, 220), width=1)

    body_text = body or "我正在整理任务状态，右侧会同步回复。"
    if len(body_text.strip()) <= 4:
        box = draw.textbbox((0, 0), body_text.strip(), font=short_body_font)
        text_w = box[2] - box[0]
        text_h = box[3] - box[1]
        draw.text((168 - text_w // 2, 84 - text_h // 2 - 2), body_text.strip(),
                  fill=(17, 24, 32), font=short_body_font)
    else:
        lines = wrap_visual_text(draw, body_text, body_font, 238, 3)
        y = 62 if len(lines) == 1 else 54
        for line in lines:
            draw.text((50, y), line, fill=(17, 24, 32), font=body_font)
            y += 24
    dot_y = 113
    for i, color in enumerate([(53, 174, 228), (102, 194, 230), (154, 214, 235)]):
        draw.ellipse((252 + i * 16, dot_y, 258 + i * 16, dot_y + 6), fill=color)

    draw.rounded_rectangle((12, 137, 202, 143), radius=3, fill=(53, 212, 154))
    footer_text = footer or "source: codex activity"
    draw.text((12, 151), limit_text(footer_text, 42), fill=(169, 159, 147), font=footer_font)
    return w, h, rgb565_bytes(img)


def pet_frames_rgb565(pet_dir: Path, grid: str, max_size: str,
                      padding: int = 0,
                      frame_cells: str = "1x1",
                      crop_visible: bool = False,
                      samples: list[int] | None = None) -> tuple[int, int, bytes, str, int]:
    try:
        from PIL import Image
    except ImportError as exc:
        raise SystemExit("Pillow is required for --sync-pet-sprite") from exc

    pet_json = pet_dir / "pet.json"
    if not pet_json.exists():
        raise SystemExit(f"pet.json not found: {pet_json}")
    info = json.loads(pet_json.read_text(encoding="utf-8"))
    sheet_path = pet_dir / info.get("spritesheetPath", "spritesheet.webp")
    if not sheet_path.exists():
        raise SystemExit(f"spritesheet not found: {sheet_path}")

    cols, rows = parse_pair(grid, (8, 9))
    max_w, max_h = parse_pair(max_size, (112, 123))
    sheet = Image.open(sheet_path).convert("RGBA")
    frame_w = sheet.width // cols
    frame_h = sheet.height // rows
    if frame_w <= 0 or frame_h <= 0:
        raise SystemExit("invalid spritesheet grid")

    cells_w, cells_h = parse_pair(frame_cells, (1, 1)) if frame_cells != "auto" else (1, 1)
    if frame_cells == "auto" and rows >= 2:
        top = sheet.crop((0, 0, frame_w, frame_h))
        lower = sheet.crop((0, frame_h, frame_w, frame_h * 2))
        top_box = top.getbbox()
        lower_box = lower.getbbox()
        if top_box and lower_box and top_box[3] >= frame_h - 1 and lower_box[1] <= 1:
            cells_h = 2

    rendered: list[Any] = []
    for index in samples or [0]:
        if index >= cols * rows:
            continue
        col = index % cols
        row = index // cols
        frame = sheet.crop((
            col * frame_w,
            row * frame_h,
            col * frame_w + frame_w * cells_w,
            row * frame_h + frame_h * cells_h,
        ))
        if cells_h > 1:
            alpha = frame.getchannel("A")
            ranges: list[tuple[int, int]] = []
            start: int | None = None
            for y in range(frame.height):
                row_pixels = 0
                for x in range(frame.width):
                    if alpha.getpixel((x, y)) > 0:
                        row_pixels += 1
                        if row_pixels > 2:
                            break
                if row_pixels > 2 and start is None:
                    start = y
                elif row_pixels <= 2 and start is not None:
                    ranges.append((start, y))
                    start = None
            if start is not None:
                ranges.append((start, frame.height))
            if len(ranges) > 1 and ranges[1][0] - ranges[0][1] >= 4:
                frame = frame.crop((0, 0, frame.width, ranges[0][1]))
        if crop_visible:
            visible_box = frame.getbbox()
            if visible_box:
                frame = frame.crop(visible_box)
        if padding > 0:
            padded = Image.new("RGBA", (frame.width + padding * 2, frame.height + padding * 2),
                               (0, 0, 0, 0))
            padded.alpha_composite(frame, (padding, padding))
            frame = padded
        if frame.width > max_w or frame.height > max_h:
            scale = min(max_w / frame.width, max_h / frame.height)
            resized = (max(1, int(frame.width * scale)), max(1, int(frame.height * scale)))
            resampling = getattr(Image, "Resampling", Image).NEAREST
            frame = frame.resize(resized, resampling)
        rendered.append(frame)

    if not rendered:
        raise SystemExit("no valid pet animation frames")

    target_w = rendered[0].width
    target_h = rendered[0].height
    data = bytearray()
    for frame in rendered:
        canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
        canvas.alpha_composite(frame, ((target_w - frame.width) // 2, (target_h - frame.height) // 2))
        bg = Image.new("RGBA", canvas.size, (7, 9, 8, 255))
        data.extend(rgb565_bytes(Image.alpha_composite(bg, canvas)))
    return target_w, target_h, bytes(data), info.get("displayName", pet_dir.name), len(rendered)


def pet_sprite_rgb565(pet_dir: Path, grid: str, max_size: str,
                      padding: int = 0,
                      frame_cells: str = "1x1",
                      crop_visible: bool = False) -> tuple[int, int, bytes, str]:
    w, h, data, name, _frames = pet_frames_rgb565(
        pet_dir, grid, max_size, padding, frame_cells, crop_visible, [0]
    )
    return w, h, data, name


def write_pet_sprite_to_serial(ser: Any, pet_dir: Path, grid: str, max_size: str,
                               padding: int, frame_cells: str, crop_visible: bool,
                               anim_frames: str = "codex",
                               anim_state: str = "running",
    ack_timeout: float = 20.0) -> bool:
    if anim_frames.strip().lower() == "codex":
        samples, durations, loop_start = device_state_sequence(anim_state)
    else:
        samples = parse_indexes(anim_frames)
        durations = codex_durations_for_frames(samples) or [180] * len(samples)
        loop_start = 0
    w, h, data, name, frame_count = pet_frames_rgb565(
        pet_dir, grid, max_size, padding, frame_cells, crop_visible, samples
    )
    if frame_count > 1:
        duration_text = ",".join(str(duration) for duration in durations[:frame_count])
        header = (
            f"PET_ANIM_START {w} {h} {frame_count} {len(data)} "
            f"{loop_start} {duration_text}\n"
        ).encode("ascii")
        ack_prefix = "PET_ANIM_"
        ok_prefix = "PET_ANIM_ACK"
        label = "pet_anim"
    else:
        header = f"PET_SPRITE_START {w} {h} {len(data)}\n".encode("ascii")
        ack_prefix = "PET_SPRITE_"
        ok_prefix = "PET_SPRITE_ACK"
        label = "pet_sprite"
    ser.write(header)
    ser.flush()
    time.sleep(0.1)
    for start in range(0, len(data), 128):
        ser.write(data[start:start + 128])
        ser.flush()
        time.sleep(0.008)
    print(
        f"{label}={name} state={anim_state} {w}x{h} "
        f"frames={frame_count} loop={loop_start} bytes={len(data)}",
        flush=True,
    )

    deadline = time.monotonic() + ack_timeout
    cache_ack = frame_count <= 1
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith(ack_prefix):
            print(line, flush=True)
            if line.startswith(ok_prefix):
                break
            raise RuntimeError(line)
    else:
        raise TimeoutError(f"no {ok_prefix} for {name}")

    if frame_count > 1:
        cache_deadline = time.monotonic() + 2.0
        while time.monotonic() < cache_deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line.startswith("PET_CACHE_"):
                print(line, flush=True)
                cache_ack = line == "PET_CACHE_ACK"
                break
    time.sleep(0.25)
    return cache_ack


def write_pet_atlas_to_serial(ser: Any, pet_dir: Path, grid: str, max_size: str,
                              padding: int, frame_cells: str,
                              crop_visible: bool) -> dict[int, int]:
    samples = codex_state_atlas_frames()
    w, h, data, name, frame_count = pet_frames_rgb565(
        pet_dir, grid, max_size, padding, frame_cells, crop_visible, samples
    )
    if frame_count != len(samples):
        raise RuntimeError("pet atlas frame count mismatch")
    header = f"PET_ATLAS_START {w} {h} {frame_count} {len(data)}\n".encode("ascii")
    ser.write(header)
    ser.flush()
    time.sleep(0.1)
    for start in range(0, len(data), 512):
        ser.write(data[start:start + 512])
        ser.flush()
        time.sleep(0.004)
    print(f"pet_atlas={name} {w}x{h} frames={frame_count} bytes={len(data)}", flush=True)

    deadline = time.monotonic() + 240.0
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("PET_ATLAS_"):
            print(line, flush=True)
            if line.startswith("PET_ATLAS_ACK"):
                return {frame: index for index, frame in enumerate(samples)}
            raise RuntimeError(line)
    raise TimeoutError(f"no PET_ATLAS_ACK for {name}")


def write_pet_anim_select_to_serial(ser: Any, atlas_map: dict[int, int], state: str) -> None:
    samples, durations, loop_start = device_state_sequence(state)
    try:
        mapped = [atlas_map[frame] for frame in samples]
    except KeyError as exc:
        raise RuntimeError(f"pet atlas missing frame {exc.args[0]}") from exc
    indexes_text = ",".join(str(index) for index in mapped)
    duration_text = ",".join(str(duration) for duration in durations[:len(mapped)])
    ser.write(f"PET_ANIM_SELECT {len(mapped)} {loop_start} {indexes_text} {duration_text}\n".encode("ascii"))
    ser.flush()

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("PET_ANIM_SELECT_"):
            print(line, flush=True)
            if line.startswith("PET_ANIM_SELECT_ACK"):
                return
            raise RuntimeError(line)
    raise TimeoutError("no PET_ANIM_SELECT_ACK")


def load_sync_state() -> dict[str, Any]:
    try:
        data = json.loads(_STATE_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def save_sync_state(state: dict[str, Any]) -> None:
    try:
        _STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
        _STATE_PATH.write_text(json.dumps(state, separators=(",", ":")), encoding="utf-8")
    except OSError:
        pass


def load_runtime_state() -> dict[str, Any]:
    try:
        data = json.loads(_RUNTIME_STATE_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        data = {}
    return data if isinstance(data, dict) else {}


def write_runtime_state(state: dict[str, Any]) -> None:
    try:
        _RUNTIME_STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
        _RUNTIME_STATE_PATH.write_text(json.dumps(state, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    except OSError:
        pass


def save_runtime_state(**updates: Any) -> None:
    state = load_runtime_state()
    state.update(updates)
    state["updated_at"] = int(time.time())
    write_runtime_state(state)


def tail_text(path: Path, max_bytes: int = 12000) -> str:
    try:
        size = path.stat().st_size
        with path.open("rb") as handle:
            if size > max_bytes:
                handle.seek(size - max_bytes)
                handle.readline()
            return handle.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def pet_key_to_state_value(key: tuple[Any, ...]) -> str:
    return json.dumps(list(key), separators=(",", ":"), ensure_ascii=False)


def pet_render_key(pet_dir: Path | None, grid: str, max_size: str,
                   padding: int, frame_cells: str, crop_visible: bool,
                   anim_frames: str) -> tuple[Any, ...] | None:
    if not pet_dir:
        return None
    sprite_path = pet_dir / "spritesheet.webp"
    try:
        sprite_mtime = sprite_path.stat().st_mtime_ns
    except OSError:
        sprite_mtime = 0
    return (
        str(pet_dir),
        sprite_mtime,
        grid,
        max_size,
        padding,
        frame_cells,
        crop_visible,
        anim_frames,
    )


def current_pet_label(pet_dir: Path | None) -> str:
    if not pet_dir:
        return "当前宠物"
    pet_json = pet_dir / "pet.json"
    try:
        info = json.loads(pet_json.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return pet_dir.name
    value = info.get("displayName") or info.get("id") or pet_dir.name
    return str(value)


def mark_pet_cache_synced(pet_dir: Path | None, pet_key: tuple[Any, ...] | None) -> None:
    if not pet_key:
        return
    state = load_sync_state()
    state["pet_cache_key"] = pet_key_to_state_value(pet_key)
    state["pet_cache_name"] = current_pet_label(pet_dir)
    state["pet_cache_synced_at"] = int(time.time())
    save_sync_state(state)


def pet_cache_is_current(pet_key: tuple[Any, ...] | None) -> bool:
    if not pet_key:
        return True
    state = load_sync_state()
    return state.get("pet_cache_key") == pet_key_to_state_value(pet_key)


def find_serial_port(pattern: str) -> str | None:
    if os.name == "nt":
        return find_windows_serial_port(pattern)
    candidates = sorted(glob.glob(pattern))
    if not candidates and pattern == "/dev/cu.usbmodem*":
        candidates = sorted(glob.glob("/dev/tty.usbmodem*"))
    return candidates[0] if candidates else None


def find_windows_serial_port(pattern: str) -> str | None:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        return None

    ports = list(list_ports.comports())
    if not ports:
        return None

    wanted = (pattern or "auto").strip()
    if wanted.lower() in {"auto", "usb", "com*"}:
        candidates = ports
    else:
        candidates = [
            port for port in ports
            if fnmatch.fnmatch(port.device.upper(), wanted.upper())
            or fnmatch.fnmatch((port.description or "").upper(), wanted.upper())
        ]
        if not candidates and re.fullmatch(r"COM\d+", wanted, re.IGNORECASE):
            return wanted.upper()

    def score(port: Any) -> tuple[int, int, str]:
        text = f"{port.device} {port.description or ''} {port.hwid or ''}".upper()
        usb_score = 0 if any(token in text for token in ("USB", "CP210", "CH340", "CDC", "UART", "JTAG")) else 1
        match = re.search(r"COM(\d+)", port.device.upper())
        port_number = int(match.group(1)) if match else 999
        return (usb_score, port_number, port.device)

    return sorted(candidates, key=score)[0].device if candidates else None


def serial_update_pet_if_available(args: argparse.Namespace,
                                   payloads: list[dict[str, Any]],
                                   pet_dir: Path | None,
                                   pet_anim_state: str,
                                   project: str) -> bool:
    if not pet_dir:
        return False
    port = find_serial_port(args.auto_serial_port)
    if not port:
        save_runtime_state(mode="ble", pet_update="waiting_for_usb", serial_port=None)
        return False
    try:
        import serial  # type: ignore
    except ImportError as exc:
        print(f"serial pet update unavailable: {exc}", flush=True)
        return False

    save_runtime_state(mode="usb-update", pet_update="updating", serial_port=port)
    update_payloads = replace_codex_output(
        payloads,
        project,
        "USB Update",
        "正在通过 USB 写入新宠物,完成后可拔线。",
        keep_pet_state=False,
    )
    try:
        with serial.Serial(port, args.serial_baud, timeout=2, write_timeout=3) as ser:
            ser.dtr = False
            ser.rts = False
            time.sleep(4.0)
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            write_payloads_to_open_serial(
                ser,
                update_payloads,
                pet_dir,
                True,
                False,
                args.preview_grid,
                args.pet_max_size,
                args.pet_padding,
                args.pet_frame_cells,
                args.pet_crop_visible,
                args.pet_anim_frames,
                pet_anim_state,
            )
            done_payloads = replace_codex_output(
                payloads,
                project,
                "Updated",
                "宠物已写入设备,可以拔线。之后日常同步走蓝牙。",
                keep_pet_state=True,
            )
            write_payloads_to_open_serial(
                ser,
                done_payloads,
                None,
                False,
                False,
                args.preview_grid,
                args.pet_max_size,
                args.pet_padding,
                args.pet_frame_cells,
                args.pet_crop_visible,
                args.pet_anim_frames,
                pet_anim_state,
            )
        save_runtime_state(mode="ble", pet_update="synced", serial_port=port)
        return True
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"usb pet update failed: {exc}", flush=True)
        save_runtime_state(mode="ble", pet_update="failed", serial_port=port, last_error=str(exc))
        return False


def serial_mirror_payloads_if_available(args: argparse.Namespace,
                                        payloads: list[dict[str, Any]],
                                        pet_anim_state: str) -> bool:
    port = find_serial_port(args.auto_serial_port)
    if not port:
        return False
    try:
        import serial  # type: ignore
    except ImportError as exc:
        print(f"serial live mirror unavailable: {exc}", flush=True)
        return False

    try:
        with serial.Serial(port, args.serial_baud, timeout=2, write_timeout=3) as ser:
            ser.dtr = False
            ser.rts = False
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            write_payloads_to_open_serial(
                ser,
                payloads,
                None,
                False,
                False,
                args.preview_grid,
                args.pet_max_size,
                args.pet_padding,
                args.pet_frame_cells,
                args.pet_crop_visible,
                args.pet_anim_frames,
                pet_anim_state,
            )
        save_runtime_state(serial_port=port, last_serial_ok=int(time.time()))
        return True
    except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"usb live mirror failed: {exc}", flush=True)
        save_runtime_state(serial_port=port, last_error=str(exc))
        return False


def pet_update_required_payloads(payloads: list[dict[str, Any]],
                                 pet_dir: Path | None,
                                 project: str) -> list[dict[str, Any]]:
    message = "当前宠物还没写入设备,请插线更新。"
    return replace_codex_output(payloads, project, "USB Update", message, keep_pet_state=False)


def replace_codex_output(payloads: list[dict[str, Any]],
                         project: str,
                         status: str,
                         message: str,
                         keep_pet_state: bool = True) -> list[dict[str, Any]]:
    body_lines = device_display_lines(message, project)
    filtered = [
        payload for payload in payloads
        if keep_pet_state or payload.get("kind") != "pet_state"
    ]
    replaced = False
    for index, payload in enumerate(filtered):
        if payload.get("kind") != "codex_output":
            continue
        updated = dict(payload)
        updated["s"] = status
        for line_index in range(1, 9):
            updated[f"l{line_index}"] = body_lines[line_index - 1]
        filtered[index] = updated
        replaced = True
        break
    if not replaced:
        payload = {
            "kind": "codex_output",
            "p": project,
            "s": status,
        }
        for line_index in range(1, 9):
            payload[f"l{line_index}"] = body_lines[line_index - 1]
        filtered.append(payload)
    return filtered


def select_or_sync_pet_atlas(ser: Any, pet_dir: Path, pet_key: tuple[Any, ...],
                             grid: str, max_size: str, padding: int,
                             frame_cells: str, crop_visible: bool,
                             anim_state: str) -> None:
    global _LAST_PET_ATLAS_KEY, _LAST_PET_ATLAS_MAP, _LAST_PET_SELECT_KEY

    state = load_sync_state()
    atlas_state_key = pet_key_to_state_value(pet_key)
    if _LAST_PET_ATLAS_KEY == pet_key and _LAST_PET_ATLAS_MAP:
        atlas_map = _LAST_PET_ATLAS_MAP
    elif state.get("pet_atlas_key") == atlas_state_key:
        # The daemon may have restarted while the board kept the atlas in RAM.
        # Try the cheap command first; if the board says no, resend the atlas.
        atlas_map = {frame: index for index, frame in enumerate(codex_state_atlas_frames())}
    else:
        atlas_map = {}

    select_key = (pet_key, anim_state)
    if atlas_map and _LAST_PET_SELECT_KEY == select_key:
        return

    if atlas_map:
        try:
            write_pet_anim_select_to_serial(ser, atlas_map, anim_state)
            _LAST_PET_ATLAS_KEY = pet_key
            _LAST_PET_ATLAS_MAP = atlas_map
            _LAST_PET_SELECT_KEY = select_key
            return
        except (RuntimeError, TimeoutError):
            atlas_map = {}

    atlas_map = write_pet_atlas_to_serial(ser, pet_dir, grid, max_size,
                                          padding, frame_cells, crop_visible)
    write_pet_anim_select_to_serial(ser, atlas_map, anim_state)
    _LAST_PET_ATLAS_KEY = pet_key
    _LAST_PET_ATLAS_MAP = atlas_map
    _LAST_PET_SELECT_KEY = select_key
    state["pet_cache_key"] = atlas_state_key
    state["pet_cache_name"] = current_pet_label(pet_dir)
    state["pet_cache_synced_at"] = int(time.time())
    state["pet_atlas_key"] = atlas_state_key
    save_sync_state(state)


def write_output_image_to_serial(ser: Any, payload: dict[str, Any]) -> None:
    project = str(payload.get("project") or "CodexMeter")
    status = str(payload.get("status") or "Running")
    body = str(payload.get("raw_text") or payload.get("text") or "")
    footer = str(payload.get("footer") or "source: codex activity")
    w, h, data = render_codex_output_image(project, status, body, footer)
    ser.write(f"OUTPUT_IMAGE_START {w} {h} {len(data)}\n".encode("ascii"))
    ser.flush()
    time.sleep(0.08)
    for start in range(0, len(data), 256):
        ser.write(data[start:start + 256])
        ser.flush()
        time.sleep(0.002)

    print(f"output_image={project} {status} {w}x{h} bytes={len(data)}", flush=True)
    deadline = time.monotonic() + 45.0
    while time.monotonic() < deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if line.startswith("OUTPUT_IMAGE_"):
            print(line, flush=True)
            if line.startswith("OUTPUT_IMAGE_ACK"):
                break
            raise RuntimeError(line)
    else:
        raise TimeoutError(f"no OUTPUT_IMAGE_ACK for {project}")
    time.sleep(0.1)


def project_name_for_cwd(cwd: Path, override: str | None = None) -> str:
    if override:
        return limit_text(ascii_lcd_text(override), 48)
    try:
        result = subprocess.run(
            ["git", "remote", "get-url", "origin"],
            cwd=str(cwd),
            text=True,
            capture_output=True,
            timeout=2,
            check=False,
        )
        remote = result.stdout.strip()
        if result.returncode == 0 and remote:
            name = re.split(r"[:/]", remote.rstrip("/"))[-1]
            if name.endswith(".git"):
                name = name[:-4]
            if name:
                return limit_text(ascii_lcd_text(name), 48)
    except (OSError, subprocess.SubprocessError):
        pass
    return limit_text(ascii_lcd_text(cwd.name or "Current Project"), 48)


def find_thread_rollout(cwd: Path) -> tuple[str | None, Path | None, int | None]:
    selected = selected_recent_session(cwd)
    if selected:
        return selected.title, selected.rollout_path, selected.updated_at_ms

    cwd_s = str(cwd.resolve())
    for codex_home in codex_home_candidates():
        db_path = codex_home / "state_5.sqlite"
        if not db_path.exists():
            continue
        try:
            con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
            try:
                row = con.execute(
                    """
                    select title, rollout_path, updated_at_ms
                    from threads
                    where cwd = ?
                    order by updated_at_ms desc
                    limit 1
                    """,
                    (cwd_s,),
                ).fetchone()
            finally:
                con.close()
        except sqlite3.Error:
            continue
        if row:
            title, rollout_path, updated_at_ms = row
            return title, Path(rollout_path) if rollout_path else None, updated_at_ms
    return None, None, None


def list_recent_sessions(cwd: Path, limit: int = 8) -> list[RecentSession]:
    cwd_s = str(cwd.resolve())
    sessions: list[RecentSession] = []
    seen: set[str] = set()

    queries = [
        (
            """
            select id, title, cwd, rollout_path, updated_at_ms
            from threads
            where cwd = ? and archived = 0
            order by updated_at_ms desc
            limit ?
            """,
            (cwd_s, limit),
        ),
        (
            """
            select id, title, cwd, rollout_path, updated_at_ms
            from threads
            where archived = 0
            order by updated_at_ms desc
            limit ?
            """,
            (limit,),
        ),
    ]

    for codex_home in codex_home_candidates():
        db_path = codex_home / "state_5.sqlite"
        if not db_path.exists():
            continue
        for query, params in queries:
            try:
                con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
                try:
                    rows = con.execute(query, params).fetchall()
                finally:
                    con.close()
            except sqlite3.Error:
                continue
            for thread_id, title, session_cwd, rollout_path, updated_at_ms in rows:
                if not isinstance(thread_id, str) or thread_id in seen:
                    continue
                seen.add(thread_id)
                sessions.append(RecentSession(
                    thread_id=thread_id,
                    title=str(title or "Untitled"),
                    cwd=Path(session_cwd) if session_cwd else cwd,
                    rollout_path=Path(rollout_path) if rollout_path else None,
                    updated_at_ms=updated_at_ms if isinstance(updated_at_ms, int) else None,
                ))
                if len(sessions) >= limit:
                    return sessions
    return sessions


def selected_recent_session(cwd: Path) -> RecentSession | None:
    if not _SELECTED_THREAD_ID:
        return None
    for session in list_recent_sessions(cwd):
        if session.thread_id == _SELECTED_THREAD_ID:
            return session
    return None


def selected_session_index(cwd: Path, sessions: list[RecentSession] | None = None) -> int:
    if not _SELECTED_THREAD_ID:
        return 0
    sessions = sessions if sessions is not None else list_recent_sessions(cwd)
    for index, session in enumerate(sessions):
        if session.thread_id == _SELECTED_THREAD_ID:
            return index
    return 0


def consume_session_select_request(cwd: Path) -> bool:
    global _SESSION_SELECT_INDEX, _SELECTED_THREAD_ID
    if not _SESSION_SELECT_EVENT.is_set():
        return False
    with _SESSION_SELECT_LOCK:
        index = _SESSION_SELECT_INDEX
        _SESSION_SELECT_INDEX = None
        _SESSION_SELECT_EVENT.clear()
    if index is None:
        return False
    sessions = list_recent_sessions(cwd)
    if index < 0 or index >= len(sessions):
        return False
    _SELECTED_THREAD_ID = sessions[index].thread_id
    save_runtime_state(selected_session=sessions[index].title, selected_session_index=index)
    print(f"session selected {index}: {sessions[index].title}", flush=True)
    return True


def consume_session_next_request(cwd: Path) -> bool:
    global _SELECTED_THREAD_ID
    if not _SESSION_NEXT_EVENT.is_set():
        return False
    _SESSION_NEXT_EVENT.clear()
    sessions = list_recent_sessions(cwd)
    if not sessions:
        return False
    current = selected_session_index(cwd, sessions)
    index = (current + 1) % len(sessions)
    _SELECTED_THREAD_ID = sessions[index].thread_id
    save_runtime_state(selected_session=sessions[index].title, selected_session_index=index)
    print(f"session next selected {index}: {sessions[index].title}", flush=True)
    return True


def consume_session_list_request() -> bool:
    if not _SESSION_LIST_EVENT.is_set():
        return False
    _SESSION_LIST_EVENT.clear()
    return True


def build_session_list_payload(cwd: Path) -> dict[str, Any]:
    sessions = list_recent_sessions(cwd)
    payload: dict[str, Any] = {
        "kind": "session_list",
        "n": len(sessions),
        "i": selected_session_index(cwd, sessions),
    }
    for index, session in enumerate(sessions[:8], 1):
        title = session.title or "Untitled"
        payload[f"t{index}"] = limit_text(ascii_lcd_text(title), 42)
    return payload


def watch_probe_marker(args: argparse.Namespace) -> tuple[Any, ...]:
    cwd = args.cwd.expanduser().resolve()
    title, rollout_path, updated_at_ms = find_thread_rollout(cwd)
    rollout_size = 0
    rollout_mtime = 0
    if rollout_path and rollout_path.exists():
        try:
            stat = rollout_path.stat()
            rollout_size = stat.st_size
            rollout_mtime = stat.st_mtime_ns
        except OSError:
            pass

    pet_dir = args.pet_dir or find_default_pet_dir()
    pet_marker: tuple[str, int] = ("", 0)
    if pet_dir:
        sprite_path = pet_dir / "spritesheet.webp"
        try:
            pet_marker = (str(pet_dir), sprite_path.stat().st_mtime_ns)
        except OSError:
            pet_marker = (str(pet_dir), 0)

    return (_SELECTED_THREAD_ID, title, str(rollout_path) if rollout_path else "", updated_at_ms,
            rollout_size, rollout_mtime, pet_marker)


def wait_for_probe_change(args: argparse.Namespace,
                          last_marker: tuple[Any, ...] | None,
                          deadline: float,
                          serial_handle: Any | None = None) -> tuple[bool, tuple[Any, ...] | None]:
    while True:
        if serial_handle is not None:
            pump_serial_device_requests(serial_handle)
        marker = watch_probe_marker(args)
        if last_marker is None or marker != last_marker:
            return True, marker
        if _BLE_REFRESH_EVENT.is_set():
            _BLE_REFRESH_EVENT.clear()
            return False, marker
        if _SESSION_SELECT_EVENT.is_set():
            return True, marker
        if _SESSION_LIST_EVENT.is_set():
            return True, marker
        if _SESSION_NEXT_EVENT.is_set():
            return True, marker
        if time.monotonic() >= deadline:
            return False, marker
        time.sleep(max(0.05, args.probe_interval))


def pump_serial_device_requests(ser: Any) -> None:
    while True:
        try:
            waiting = getattr(ser, "in_waiting", 0)
        except Exception:
            waiting = 0
        if waiting <= 0:
            return
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            return
        if not line:
            return
        if line.startswith(("JSON_ACK", "JSON_NACK", "PET_", "OUTPUT_IMAGE_", "{\"ready\":true}")):
            continue
        if line.startswith("REFRESH_REQUEST"):
            _BLE_REFRESH_EVENT.set()
            save_runtime_state(last_device_refresh_request=int(time.time()))
            print(f"serial refresh requested: {line}", flush=True)
            continue
        if line.startswith("SESSION_LIST_REQUEST"):
            _SESSION_LIST_EVENT.set()
            print(f"serial session list requested: {line}", flush=True)
            continue
        if line.startswith("SESSION_NEXT_REQUEST"):
            _SESSION_NEXT_EVENT.set()
            print(f"serial session next requested: {line}", flush=True)
            continue
        if line.startswith("SESSION_SELECT_REQUEST"):
            match = re.search(r"SESSION_SELECT_REQUEST\s+(\d+)", line)
            if match:
                global _SESSION_SELECT_INDEX
                with _SESSION_SELECT_LOCK:
                    _SESSION_SELECT_INDEX = int(match.group(1))
                    _SESSION_SELECT_EVENT.set()
                print(f"serial session select requested {match.group(1)}", flush=True)
            continue


def open_live_serial_handle(args: argparse.Namespace) -> Any | None:
    port = find_serial_port(args.auto_serial_port)
    if not port:
        save_runtime_state(serial_port=None)
        return None
    try:
        import serial  # type: ignore
        ser = serial.Serial(port, args.serial_baud, timeout=0.05, write_timeout=3)
        ser.dtr = False
        ser.rts = False
        time.sleep(0.4)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        save_runtime_state(serial_port=port)
        print(f"serial live bridge connected: {port}", flush=True)
        return ser
    except Exception as exc:
        save_runtime_state(serial_port=port, last_error=str(exc))
        print(f"serial live bridge unavailable: {exc}", flush=True)
        return None


def latest_assistant_text(rollout_path: Path | None) -> str | None:
    if not rollout_path or not rollout_path.exists():
        return None
    latest: str | None = None
    try:
        with rollout_path.open("r", encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                try:
                    item = json.loads(line)
                except json.JSONDecodeError:
                    continue
                text = assistant_text_from_rollout_item(item)
                if text:
                    latest = text
    except OSError:
        return None
    return latest


def assistant_text_from_rollout_item(item: dict[str, Any]) -> str | None:
    payload = item.get("payload") or {}
    if item.get("type") == "event_msg" and payload.get("type") == "agent_message":
        message = payload.get("message")
        return message if isinstance(message, str) and message.strip() else None

    if item.get("type") != "response_item":
        return None
    if payload.get("type") != "message" or payload.get("role") != "assistant":
        return None
    parts: list[str] = []
    for part in payload.get("content") or []:
        if not isinstance(part, dict):
            continue
        text = part.get("text") or part.get("output_text")
        if isinstance(text, str) and text.strip():
            parts.append(text.strip())
    return "\n".join(parts) if parts else None


def rollout_item_timestamp_ms(item: dict[str, Any]) -> int:
    timestamp = item.get("timestamp")
    if not isinstance(timestamp, str):
        return 0
    try:
        return int(datetime.fromisoformat(timestamp.replace("Z", "+00:00")).timestamp() * 1000)
    except ValueError:
        return 0


def read_recent_rollout_items(rollout_path: Path | None, max_bytes: int = 8 * 1024 * 1024) -> list[dict[str, Any]]:
    if not rollout_path or not rollout_path.exists():
        return []
    try:
        size = rollout_path.stat().st_size
        with rollout_path.open("rb") as handle:
            if size > max_bytes:
                handle.seek(size - max_bytes)
                handle.readline()
            raw = handle.read()
    except OSError:
        return []

    items: list[dict[str, Any]] = []
    for line in raw.decode("utf-8", errors="ignore").splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict):
            items.append(item)
    return items


def clean_codex_text(text: str | None, limit: int = 96) -> str | None:
    if not isinstance(text, str):
        return None
    cleaned = re.sub(r"^\s{0,3}#{1,6}\s+", "", text)
    cleaned = re.sub(r"\*\*([^*]+)\*\*", r"\1", cleaned)
    cleaned = re.sub(r"__([^_]+)__", r"\1", cleaned)
    cleaned = re.sub(r"`([^`]+)`", r"\1", cleaned)
    cleaned = re.sub(r"\*([^*]+)\*", r"\1", cleaned)
    cleaned = re.sub(r"_([^_]+)_", r"\1", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    return limit_text(cleaned, limit) if cleaned else None


def assistant_text_from_payload(payload: dict[str, Any]) -> str | None:
    if payload.get("type") != "message" or payload.get("role") != "assistant":
        return None
    parts: list[str] = []
    for part in payload.get("content") or []:
        if not isinstance(part, dict):
            continue
        text = part.get("text") or part.get("output_text")
        cleaned = clean_codex_text(text, 160)
        if cleaned:
            parts.append(cleaned)
    return " ".join(parts) if parts else None


def user_text_from_payload(payload: dict[str, Any]) -> str | None:
    if payload.get("type") != "message" or payload.get("role") != "user":
        return None
    parts: list[str] = []
    for part in payload.get("content") or []:
        if not isinstance(part, dict):
            continue
        if part.get("type") != "input_text":
            continue
        cleaned = clean_codex_text(part.get("text"), 140)
        if cleaned:
            parts.append(cleaned)
    return "你: " + " ".join(parts) if parts else None


def function_activity(payload: dict[str, Any], in_progress: bool) -> str | None:
    name = str(payload.get("name") or "")
    if name == "exec_command":
        return "正在运行命令" if in_progress else "命令已完成"
    if name == "write_stdin":
        return "正在等待命令输出" if in_progress else "命令输出已更新"
    if name == "view_image":
        return "正在读取图片" if in_progress else "图片已读取"
    if name in {"apply_patch"} or payload.get("type") == "custom_tool_call":
        return "正在编辑文件" if in_progress else "文件已编辑"
    if "gmail" in name:
        return "正在调用 Gmail" if in_progress else "Gmail 调用完成"
    if name:
        tool_name = clean_codex_text(name.replace("_", " ").replace("-", " "), 48)
        if tool_name:
            return f"正在调用 {tool_name}" if in_progress else f"{tool_name} 已完成"
    return "正在调用工具" if in_progress else "工具调用完成"


def event_activity(payload: dict[str, Any]) -> str | None:
    event_type = payload.get("type")
    if event_type == "patch_apply_end":
        return "文件已编辑" if payload.get("success", True) else "文件编辑失败"
    if event_type == "web_search_end":
        query = clean_codex_text(payload.get("query"), 48)
        return f"已搜索 {query}" if query else "网页搜索完成"
    if event_type == "mcp_tool_call_end":
        invocation = payload.get("invocation") or {}
        tool = clean_codex_text(invocation.get("tool"), 48)
        return f"{tool} 调用完成" if tool else "工具调用完成"
    return None


def latest_rollout_activity(items: list[dict[str, Any]]) -> str | None:
    for item in items:
        timestamp_ms = rollout_item_timestamp_ms(item)
        if timestamp_ms < _DAEMON_STARTED_AT_MS:
            continue
        text = assistant_text_from_rollout_item(item)
        if not text:
            continue
        cleaned = clean_codex_text(text, 160)
        if not cleaned:
            continue
        key = (timestamp_ms, cleaned)
        if key not in _DISPLAYED_ASSISTANT_MESSAGE_KEYS:
            _DISPLAYED_ASSISTANT_MESSAGE_KEYS.add(key)
            return cleaned

    # Prefer Codex's newest conversational output, then fall back to tool
    # activity. User messages are intentionally not mirrored to the device:
    # the right-side bubble is the Codex output surface, and echoing prompts
    # can hide the assistant reply the user is looking for.
    for item in reversed(items):
        payload = item.get("payload") or {}
        if item.get("type") == "response_item":
            text = assistant_text_from_payload(payload)
            if text:
                return text
        if item.get("type") == "event_msg" and payload.get("type") == "agent_message":
            text = clean_codex_text(payload.get("message"), 160)
            if text:
                return text
        if item.get("type") == "event_msg" and payload.get("type") == "task_complete":
            text = clean_codex_text(payload.get("last_agent_message"), 160)
            if text:
                return text

    completed_calls: set[str] = set()
    for item in reversed(items):
        payload = item.get("payload") or {}
        payload_type = payload.get("type")
        if payload_type in {"function_call_output", "custom_tool_call_output"}:
            call_id = payload.get("call_id")
            if isinstance(call_id, str):
                completed_calls.add(call_id)
            continue
        if payload_type in {"function_call", "custom_tool_call"}:
            call_id = payload.get("call_id")
            return function_activity(payload, isinstance(call_id, str) and call_id not in completed_calls)
        if item.get("type") == "event_msg":
            activity = event_activity(payload)
            if activity:
                return activity
        if payload_type == "web_search_call":
            action = payload.get("action") or {}
            query = clean_codex_text(action.get("query") or action.get("url"), 48)
            return f"正在搜索 {query}" if query else "正在搜索网页"
    return None


def latest_rollout_status(items: list[dict[str, Any]]) -> str:
    latest_started = -1
    latest_finished = -1
    latest_failed = -1
    latest_waiting = -1
    completed_calls: set[str] = set()

    for index, item in enumerate(items):
        payload = item.get("payload") or {}
        event_type = payload.get("type")
        if item.get("type") == "event_msg":
            if event_type == "task_started":
                latest_started = index
            elif event_type == "task_complete":
                latest_finished = index
            elif event_type == "turn_aborted":
                latest_finished = index
                latest_failed = index
            elif event_type == "patch_apply_end" and not payload.get("success", True):
                latest_failed = index
            elif event_type == "mcp_tool_call_end":
                result = payload.get("result")
                if isinstance(result, dict) and "Err" in result:
                    latest_failed = index
        elif item.get("type") == "response_item":
            if event_type in {"function_call_output", "custom_tool_call_output"}:
                call_id = payload.get("call_id")
                if isinstance(call_id, str):
                    completed_calls.add(call_id)
            elif event_type in {"function_call", "custom_tool_call"}:
                call_id = payload.get("call_id")
                name = payload.get("name")
                if isinstance(call_id, str) and call_id not in completed_calls:
                    latest_started = max(latest_started, index)
                if name == "request_user_input":
                    latest_waiting = index

    if latest_waiting > latest_finished:
        return "waiting"
    if latest_failed > latest_finished:
        return "failed"
    if latest_started > latest_finished:
        return "running"
    if latest_finished >= 0:
        return "review"
    return "idle"


def latest_context_usage(items: list[dict[str, Any]]) -> tuple[int, int] | None:
    for item in reversed(items):
        payload = item.get("payload") or {}
        if item.get("type") != "event_msg" or payload.get("type") != "token_count":
            continue
        info = payload.get("info") or {}
        last_usage = info.get("last_token_usage") or {}
        input_tokens = last_usage.get("input_tokens")
        window_tokens = info.get("model_context_window")
        if isinstance(input_tokens, int) and isinstance(window_tokens, int) and window_tokens > 0:
            return input_tokens, window_tokens
    return None


def codex_status_label(status: str) -> str:
    return {
        "running": "SYNC",
        "waiting": "INPUT",
        "failed": "ERROR",
        "review": "DONE",
        "idle": "SYNC",
    }.get(status, "SYNC")


def codex_mascot_state(status: str) -> str:
    return {
        "running": "running",
        "waiting": "waiting",
        "failed": "failed",
        "review": "review",
    }.get(status, "idle")


def build_codex_thread_notification(cwd: Path, project: str, override_text: str | None = None) -> CodexThreadNotification:
    title, rollout_path, _updated_at_ms = find_thread_rollout(cwd)
    items = read_recent_rollout_items(rollout_path)
    status = latest_rollout_status(items) if items else "idle"
    context_usage = latest_context_usage(items) if items else None
    body = clean_codex_text(override_text, 160) if override_text else latest_rollout_activity(items)
    if not body:
        body = {
            "running": "Thinking",
            "waiting": "Waiting for input",
            "failed": "Task is blocked",
            "review": "Ready for review",
            "idle": f"Codex output is live for {project}.",
        }.get(status, "Codex output is live.")
    return CodexThreadNotification(
        title=title or project,
        body=body,
        status=status,
        status_label=codex_status_label(status),
        mascot_state=codex_mascot_state(status),
        rollout_path=rollout_path,
        context_input_tokens=context_usage[0] if context_usage else None,
        context_window_tokens=context_usage[1] if context_usage else None,
    )


def ascii_lcd_text(text: str) -> str:
    text = re.sub(r"\s+", " ", text.replace("\n", " ")).strip()
    text = "".join(ch if 32 <= ord(ch) < 127 else " " for ch in text)
    return re.sub(r"\s+", " ", text).strip()


def limit_text(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    if limit <= 3:
        return text[:limit]
    return text[: limit - 3].rstrip() + "..."


def char_display_units(ch: str) -> int:
    return 1 if ch.isascii() else 2


def split_dialog_lines(text: str, max_lines: int = 8, line_units: int = 26) -> list[str]:
    words = list(text)
    lines: list[str] = []
    line = ""
    units = 0
    index = 0
    while index < len(words) and len(lines) < max_lines:
        ch = words[index]
        if ch in ",.;:!?，。；：！？" and not line and lines:
            lines[-1] = lines[-1] + ch
            index += 1
            continue
        ch_units = char_display_units(ch)
        if line and units + ch_units > line_units:
            lines.append(line.rstrip())
            line = ""
            units = 0
            continue
        line += ch
        units += ch_units
        index += 1
    if line and len(lines) < max_lines:
        lines.append(line.rstrip())

    if index < len(words) and lines:
        last = lines[-1].rstrip()
        while last and sum(char_display_units(ch) for ch in last) > line_units - 3:
            last = last[:-1].rstrip()
        lines[-1] = last.rstrip(" ,.;:!?，。；：！？") + "..."

    while len(lines) < max_lines:
        lines.append("")
    return lines


def lcd_body_text(text: str, project: str) -> str:
    body_ascii = ascii_lcd_text(text)
    source_len = len(re.sub(r"\s+", "", text))
    if source_len and len(body_ascii) / source_len < 0.45:
        return text
    if len(body_ascii) < 12:
        return text or f"Codex is updating the {project} pet display."
    return body_ascii


def device_display_text(text: str, project: str) -> str:
    return " ".join(line for line in device_display_lines(text, project) if line)


def device_display_lines(text: str, project: str) -> list[str]:
    body = lcd_body_text(text, project)
    replacements = {
        "：": ":",
        "，": ",",
        "。": ".",
        "、": ",",
        "；": ";",
        "！": "!",
        "？": "?",
        "（": "(",
        "）": ")",
        "“": '"',
        "”": '"',
        "‘": "'",
        "’": "'",
        "…": "...",
    }
    for src, dst in replacements.items():
        body = body.replace(src, dst)
    body = re.sub(r"\s+", " ", body).strip()
    return split_dialog_lines(body)


def build_codex_output_payload(args: argparse.Namespace, notification: CodexThreadNotification | None = None) -> dict[str, Any]:
    cwd = args.cwd.expanduser().resolve()
    project = project_name_for_cwd(cwd, args.project)
    if notification is None:
        notification = build_codex_thread_notification(cwd, project, args.output_text)
    body_lines = device_display_lines(notification.body or "", project)
    footer = "source: codex activity" if notification.rollout_path else "source: local workspace"
    payload = {
        "kind": "codex_output",
        "p": project,
        "s": limit_text(notification.status_label, 12),
    }
    for index, line in enumerate(body_lines[:8], 1):
        payload[f"l{index}"] = line
    if notification.context_input_tokens is not None and notification.context_window_tokens:
        payload["ci"] = notification.context_input_tokens
        payload["cw"] = notification.context_window_tokens
    return payload


def write_payloads_to_open_serial(
    ser: Any,
    payloads: list[dict[str, Any]],
    pet_dir: Path | None = None,
    sync_pet_sprite: bool = False,
    sync_output_image: bool = False,
    preview_grid: str = "8x9",
    pet_max_size: str = "112x123",
    pet_padding: int = 0,
    pet_frame_cells: str = "1x1",
    pet_crop_visible: bool = False,
    pet_anim_frames: str = "codex",
    pet_anim_state: str = "running",
) -> None:
    global _LAST_PET_SYNC_KEY, _LAST_PET_ATLAS_KEY, _LAST_PET_ATLAS_MAP
    global _LAST_PET_SELECT_KEY, _LAST_OUTPUT_IMAGE_KEY
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    if sync_output_image:
        output_payload = next((payload for payload in payloads
                               if payload.get("kind") == "codex_output"), None)
        if output_payload is not None:
            image_key = (
                output_payload.get("project"),
                output_payload.get("status"),
                output_payload.get("raw_text") or output_payload.get("text"),
                output_payload.get("footer"),
            )
            if image_key != _LAST_OUTPUT_IMAGE_KEY:
                write_output_image_to_serial(ser, output_payload)
                _LAST_OUTPUT_IMAGE_KEY = image_key

    for payload in payloads:
        if sync_output_image and payload.get("kind") == "codex_output":
            continue
        ser.write(json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n")
        ser.flush()
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if line == "JSON_ACK":
                break
            if line == "JSON_NACK":
                raise RuntimeError(f"device rejected JSON payload: {payload.get('kind')}")
        else:
            raise TimeoutError(f"no JSON_ACK for {payload.get('kind')}")
        time.sleep(0.05)

    if sync_pet_sprite and pet_dir:
        pet_key = pet_render_key(
            pet_dir,
            preview_grid,
            pet_max_size,
            pet_padding,
            pet_frame_cells,
            pet_crop_visible,
            pet_anim_frames,
        )
        if pet_anim_frames.strip().lower() == "codex":
            try:
                select_or_sync_pet_atlas(ser, pet_dir, pet_key, preview_grid, pet_max_size,
                                         pet_padding, pet_frame_cells, pet_crop_visible,
                                         pet_anim_state)
                _LAST_PET_SYNC_KEY = pet_key
            except (OSError, RuntimeError, TimeoutError) as exc:
                print(f"pet sync skipped after error: {exc}", flush=True)
        elif pet_key != _LAST_PET_SYNC_KEY:
            try:
                cache_ok = write_pet_sprite_to_serial(
                    ser,
                    pet_dir,
                    preview_grid,
                    pet_max_size,
                    pet_padding,
                    pet_frame_cells,
                    pet_crop_visible,
                    pet_anim_frames,
                    pet_anim_state,
                )
                _LAST_PET_SYNC_KEY = pet_key
                if cache_ok:
                    mark_pet_cache_synced(pet_dir, pet_key)
            except (OSError, RuntimeError, TimeoutError) as exc:
                print(f"pet sync skipped after error: {exc}", flush=True)


def write_json_lines_to_serial(
    port: str,
    payloads: list[dict[str, Any]],
    pet_dir: Path | None = None,
    sync_pet_sprite: bool = False,
    sync_output_image: bool = False,
    preview_grid: str = "8x9",
    pet_max_size: str = "112x123",
    pet_padding: int = 0,
    pet_frame_cells: str = "1x1",
    pet_crop_visible: bool = False,
    pet_anim_frames: str = "codex",
    pet_anim_state: str = "running",
    baud: int = 921600,
) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is required for --serial-port") from exc
    with serial.Serial(port, baud, timeout=2) as ser:
        time.sleep(4.0)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        write_payloads_to_open_serial(ser, payloads, pet_dir, sync_pet_sprite, sync_output_image,
                                      preview_grid, pet_max_size, pet_padding, pet_frame_cells,
                                      pet_crop_visible, pet_anim_frames, pet_anim_state)


class BleBridge:
    def __init__(self, device_name: str) -> None:
        self.device_name = device_name
        self.queue: "queue.Queue[tuple[list[dict[str, Any]], queue.Queue[tuple[bool, str | None]]]]" = queue.Queue()
        self.thread = threading.Thread(target=self._thread_main, name="codexmeter-ble", daemon=True)
        self.thread.start()

    def send(self, payloads: list[dict[str, Any]], timeout: float = 30.0) -> None:
        result: "queue.Queue[tuple[bool, str | None]]" = queue.Queue(maxsize=1)
        self.queue.put((payloads, result))
        try:
            ok, message = result.get(timeout=timeout)
        except queue.Empty as exc:
            raise RuntimeError("BLE send timed out") from exc
        if not ok:
            raise RuntimeError(message or "BLE send failed")

    def _thread_main(self) -> None:
        try:
            asyncio.run(self._run())
        except Exception as exc:
            save_runtime_state(ble="error", last_error=f"BLE bridge stopped: {exc}")

    async def _run(self) -> None:
        try:
            from bleak import BleakClient, BleakScanner  # type: ignore
        except ImportError as exc:
            save_runtime_state(ble="error", last_error="bleak is required for --ble")
            while True:
                payloads, result = self.queue.get()
                result.put((False, str(exc)))

        while True:
            try:
                device = await BleakScanner.find_device_by_filter(
                    lambda d, ad: d.name == self.device_name or self.device_name in (ad.local_name or ""),
                    timeout=15,
                )
                if not device:
                    raise RuntimeError(f"BLE device not found: {self.device_name}")

                async with BleakClient(device) as client:
                    save_runtime_state(ble="connected", last_error="")

                    def on_refresh(_sender: Any, data: bytearray) -> None:
                        if data and data[0] == 0x02 and len(data) >= 2:
                            global _SESSION_SELECT_INDEX
                            with _SESSION_SELECT_LOCK:
                                _SESSION_SELECT_INDEX = int(data[1])
                                _SESSION_SELECT_EVENT.set()
                            print(f"ble session select requested {int(data[1])}", flush=True)
                        elif data and data[0] == 0x03:
                            _SESSION_LIST_EVENT.set()
                            print("ble session list requested", flush=True)
                        elif data and data[0] == 0x04:
                            _SESSION_NEXT_EVENT.set()
                            print("ble session next requested", flush=True)
                        elif data:
                            _BLE_REFRESH_EVENT.set()
                            save_runtime_state(last_device_refresh_request=int(time.time()))
                            print("ble refresh requested by device", flush=True)

                    try:
                        await client.start_notify(REQ_CHAR_UUID, on_refresh)
                    except Exception as exc:
                        print(f"ble refresh subscribe skipped: {exc}", flush=True)

                    while client.is_connected:
                        try:
                            payloads, result = await asyncio.to_thread(self.queue.get, True, 0.2)
                        except queue.Empty:
                            continue
                        try:
                            for data in ble_payload_packets(payloads):
                                await client.write_gatt_char(RX_CHAR_UUID, data, response=True)
                            result.put((True, None))
                        except Exception as exc:
                            result.put((False, str(exc)))
                            raise
            except Exception as exc:
                save_runtime_state(ble="error", last_error=str(exc))
                await asyncio.sleep(2.0)


def start_ble_bridge(device_name: str) -> None:
    global _BLE_BRIDGE
    if _BLE_BRIDGE is None:
        _BLE_BRIDGE = BleBridge(device_name)


async def send_ble_payloads(payloads: list[dict[str, Any]], device_name: str) -> None:
    if _BLE_BRIDGE is not None:
        await asyncio.to_thread(_BLE_BRIDGE.send, payloads)
        return

    try:
        from bleak import BleakClient, BleakScanner  # type: ignore
    except ImportError as exc:
        raise RuntimeError("bleak is required for --ble") from exc

    try:
        device = await BleakScanner.find_device_by_filter(
            lambda d, ad: d.name == device_name or device_name in (ad.local_name or ""),
            timeout=15,
        )
        if not device:
            raise RuntimeError(f"BLE device not found: {device_name}")
        async with BleakClient(device) as client:
            for data in ble_payload_packets(payloads):
                await client.write_gatt_char(RX_CHAR_UUID, data, response=True)
    except Exception as exc:
        raise RuntimeError(f"BLE send failed: {exc}") from exc


def ble_payload_packets(payloads: list[dict[str, Any]]) -> list[bytes]:
    packets: list[bytes] = []
    for payload_index, payload in enumerate(payloads):
        text = json.dumps(payload, ensure_ascii=True, separators=(",", ":"))
        if len(text.encode("utf-8")) <= 480:
            packets.append(text.encode("utf-8"))
            continue
        chunks = [
            text[start:start + BLE_WRITE_CHUNK_TEXT]
            for start in range(0, len(text), BLE_WRITE_CHUNK_TEXT)
        ]
        chunk_id = f"{int(time.time() * 1000) % 1000000:x}{payload_index:x}"
        for chunk_index, chunk in enumerate(chunks):
            packet = {
                "kind": "chunk",
                "id": chunk_id,
                "i": chunk_index,
                "n": len(chunks),
                "d": chunk,
            }
            packets.append(json.dumps(packet, ensure_ascii=True, separators=(",", ":")).encode("utf-8"))
    return packets


def pet_preview(pet_dir: Path, out_dir: Path, cols: int, rows: int, samples: list[int]) -> None:
    try:
        from PIL import Image  # type: ignore
    except ImportError as exc:
        raise SystemExit("Pillow is required for --pet-preview") from exc

    info_path = pet_dir / "pet.json"
    if not info_path.exists():
        raise SystemExit(f"pet.json not found: {info_path}")
    info = json.loads(info_path.read_text(encoding="utf-8"))
    sheet_path = pet_dir / info.get("spritesheetPath", "spritesheet.webp")
    if not sheet_path.exists():
        raise SystemExit(f"spritesheet not found: {sheet_path}")

    image = Image.open(sheet_path).convert("RGBA")
    frame_w = image.width // cols
    frame_h = image.height // rows
    if frame_w <= 0 or frame_h <= 0:
        raise SystemExit("invalid spritesheet grid")

    out_dir.mkdir(parents=True, exist_ok=True)
    for index in samples:
        if index < 0 or index >= cols * rows:
            continue
        col = index % cols
        row = index // cols
        frame = image.crop((col * frame_w, row * frame_h, (col + 1) * frame_w, (row + 1) * frame_h))
        frame.save(out_dir / f"frame_{index:03d}.png")
    print(f"pet={info.get('id', pet_dir.name)} sheet={image.width}x{image.height} frame={frame_w}x{frame_h}")
    print(f"wrote {out_dir}")


def read_usage(args: argparse.Namespace) -> UsagePayload:
    codex_bin = args.codex_bin or shutil.which("codex") or "codex"
    with CodexAppServerClient(codex_bin=codex_bin, timeout_s=args.timeout) as client:
        account = client.read_account()
        rates = client.read_rate_limits()
    return build_usage_payload(account, rates, args.limit)


def build_payloads(args: argparse.Namespace,
                   usage: UsagePayload | None = None) -> tuple[UsagePayload, list[dict[str, Any]], Path | None, str]:
    if usage is None:
        usage = read_usage(args)
    payloads = [usage.payload]

    pet_dir = args.pet_dir or find_default_pet_dir()
    cwd = args.cwd.expanduser().resolve()
    project = project_name_for_cwd(cwd, args.project)
    notification = build_codex_thread_notification(cwd, project, args.output_text)
    pet_anim_state = notification.mascot_state or args.pet_state
    pet_state = load_pet_state(pet_dir, pet_anim_state)
    if pet_state and not args.usage_once:
        payloads.append(pet_state)

    codex_output = build_codex_output_payload(args, notification)
    if not args.usage_once:
        payloads.append(codex_output)
    return usage, payloads, pet_dir, pet_anim_state


def output_payload_key(payloads: list[dict[str, Any]]) -> tuple[Any, ...]:
    output_payload = next((payload for payload in payloads
                           if payload.get("kind") == "codex_output"), {})
    session_payload = next((payload for payload in payloads
                            if payload.get("kind") == "session_list"), {})
    return (
        output_payload.get("project") or output_payload.get("p"),
        output_payload.get("status") or output_payload.get("s"),
        output_payload.get("raw_text") or output_payload.get("text") or output_payload.get("t"),
        output_payload.get("l1"),
        output_payload.get("l2"),
        output_payload.get("l3"),
        output_payload.get("l4"),
        output_payload.get("l5"),
        output_payload.get("l6"),
        output_payload.get("l7"),
        output_payload.get("l8"),
        output_payload.get("footer") or output_payload.get("f"),
        output_payload.get("state"),
        output_payload.get("ci") or output_payload.get("cti"),
        output_payload.get("cw") or output_payload.get("ctw"),
        session_payload.get("i"),
        session_payload.get("n"),
        session_payload.get("t1"),
        session_payload.get("t2"),
        session_payload.get("t3"),
        session_payload.get("t4"),
        session_payload.get("t5"),
        session_payload.get("t6"),
        session_payload.get("t7"),
        session_payload.get("t8"),
    )


def pet_payload_key(pet_dir: Path | None) -> tuple[Any, ...]:
    if not pet_dir:
        return ("", 0)
    sprite_path = pet_dir / "spritesheet.webp"
    try:
        return (str(pet_dir), sprite_path.stat().st_mtime_ns)
    except OSError:
        return (str(pet_dir), 0)


def emit_payloads(args: argparse.Namespace, usage: UsagePayload,
                  payloads: list[dict[str, Any]], pet_dir: Path | None,
                  pet_anim_state: str,
                  serial_handle: Any | None = None) -> None:
    payloads_to_emit = payloads
    state = load_runtime_state()
    save_runtime_state(
        current_pet=current_pet_label(pet_dir),
        current_pet_state=pet_anim_state,
        mode="serial" if serial_handle or args.serial_port else ("ble" if args.ble else "local"),
    )
    if args.ble and not serial_handle and not args.serial_port and args.sync_pet_sprite:
        pet_key = pet_render_key(
            pet_dir,
            args.preview_grid,
            args.pet_max_size,
            args.pet_padding,
            args.pet_frame_cells,
            args.pet_crop_visible,
            args.pet_anim_frames,
        )
        if not pet_cache_is_current(pet_key):
            project = project_name_for_cwd(args.cwd.expanduser().resolve(), args.project)
            payloads_to_emit = pet_update_required_payloads(payloads, pet_dir, project)
            save_runtime_state(pet_update="waiting_for_usb")
        else:
            save_runtime_state(pet_update="synced")

    if args.json:
        for payload in payloads_to_emit:
            print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")), flush=True)
    else:
        print(usage.summary, flush=True)
        for payload in payloads_to_emit:
            print(json.dumps(payload, ensure_ascii=False, separators=(",", ":")), flush=True)

    if serial_handle:
        write_payloads_to_open_serial(serial_handle, payloads_to_emit, pet_dir,
                                      args.sync_pet_sprite, args.sync_output_image,
                                      args.preview_grid,
                                      args.pet_max_size, args.pet_padding,
                                      args.pet_frame_cells, args.pet_crop_visible,
                                      args.pet_anim_frames, pet_anim_state)
    elif args.serial_port:
        write_json_lines_to_serial(args.serial_port, payloads_to_emit, pet_dir,
                                   args.sync_pet_sprite, args.sync_output_image,
                                   args.preview_grid,
                                   args.pet_max_size, args.pet_padding,
                                   args.pet_frame_cells, args.pet_crop_visible,
                                   args.pet_anim_frames, pet_anim_state,
                                   args.serial_baud)
    elif args.ble and state.get("ble") != "connected":
        serial_mirror_payloads_if_available(args, payloads_to_emit, pet_anim_state)
    if args.ble and serial_handle is None:
        try:
            asyncio.run(send_ble_payloads(payloads_to_emit, args.device_name))
            save_runtime_state(ble="connected", last_ble_ok=int(time.time()), last_error="")
        except RuntimeError as exc:
            save_runtime_state(ble="error", last_error=str(exc))
            if args.watch:
                print(f"ble send skipped after error: {exc}", flush=True)
            else:
                raise SystemExit(str(exc)) from exc


def control_page_html() -> str:
    state = load_runtime_state()
    sync_state = load_sync_state()
    rows = {
        "Mode": state.get("mode", "--"),
        "BLE": state.get("ble", "--"),
        "Pet update": state.get("pet_update", "--"),
        "Cached pet": sync_state.get("pet_cache_name", "--"),
        "Current pet": state.get("current_pet", "--"),
        "Serial": state.get("serial_port", "--"),
        "Last error": state.get("last_error", ""),
        "Updated": state.get("updated_at", "--"),
    }
    rows_html = "\n".join(
        f"<tr><th>{html.escape(str(k))}</th><td>{html.escape(str(v))}</td></tr>"
        for k, v in rows.items()
    )
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CodexMeter</title>
<style>
body{{margin:0;background:#070908;color:#eef6f4;font:14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}}
main{{max-width:920px;margin:0 auto;padding:28px}}
h1{{font-size:24px;margin:0 0 18px}}
.grid{{display:grid;grid-template-columns:1fr 1fr;gap:16px}}
section{{border:1px solid #263837;border-radius:8px;padding:16px;background:#0b1111}}
table{{width:100%;border-collapse:collapse}}
th{{width:128px;text-align:left;color:#8ee8c1;font-weight:600;padding:7px 8px 7px 0}}
td{{padding:7px 0;color:#d9e5e2;word-break:break-word}}
button{{background:#123131;color:#8ee8c1;border:1px solid #35d49a;border-radius:6px;padding:8px 12px;margin-right:8px;cursor:pointer}}
pre{{white-space:pre-wrap;max-height:420px;overflow:auto;background:#050707;border-radius:6px;padding:12px;color:#b8c7c3}}
@media(max-width:760px){{.grid{{grid-template-columns:1fr}}main{{padding:18px}}}}
</style>
</head>
<body><main>
<h1>CodexMeter Control</h1>
<div class="grid">
<section><h2>状态</h2><table>{rows_html}</table>
<button onclick="fetch('/api/pet-update',{{method:'POST'}}).then(()=>setTimeout(load,800))">写入当前宠物</button>
<button onclick="fetch('/api/restart',{{method:'POST'}})">重启同步服务</button>
</section>
<section><h2>说明</h2>
<p>日常输出走 BLE。宠物不一致时，插上 USB 后会自动写入；也可以点“写入当前宠物”。</p>
<p>USB 只负责刷固件、截图和首次写入新宠物。</p>
</section>
</div>
<section style="margin-top:16px"><h2>最近日志</h2><pre id="log"></pre></section>
</main>
<script>
async function load(){{
  const log = await fetch('/api/log').then(r=>r.text()).catch(()=> '');
  document.getElementById('log').textContent = log;
}}
load(); setInterval(load, 3000);
</script></body></html>"""


class ControlHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def _send(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/status":
            body = json.dumps({
                "runtime": load_runtime_state(),
                "sync": load_sync_state(),
            }, ensure_ascii=False).encode("utf-8")
            self._send(200, "application/json; charset=utf-8", body)
            return
        if path == "/api/log":
            self._send(200, "text/plain; charset=utf-8", tail_text(_LOG_PATH).encode("utf-8"))
            return
        self._send(200, "text/html; charset=utf-8", control_page_html().encode("utf-8"))

    def do_POST(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/restart":
            if os.name == "nt":
                save_runtime_state(last_error="Restart is not available from the Windows foreground runner")
                self._send(501, "application/json", b'{"ok":false,"error":"restart unavailable on Windows"}')
                return
            subprocess.Popen([
                "/bin/launchctl",
                "kickstart",
                "-k",
                f"gui/{os.getuid()}/com.codexmeter.sync",
            ])
            self._send(202, "application/json", b'{"ok":true}')
            return
        if path == "/api/pet-update":
            save_runtime_state(manual_pet_update_requested=int(time.time()))
            self._send(202, "application/json", b'{"ok":true}')
            return
        self._send(404, "application/json", b'{"ok":false}')


def start_control_server(port: int) -> None:
    if port <= 0:
        return

    def serve() -> None:
        try:
            server = ThreadingHTTPServer(("127.0.0.1", port), ControlHandler)
            save_runtime_state(control_url=f"http://127.0.0.1:{port}")
            server.serve_forever()
        except OSError as exc:
            print(f"control server disabled: {exc}", flush=True)

    threading.Thread(target=serve, daemon=True).start()


def consume_manual_pet_update_request() -> bool:
    state = load_runtime_state()
    requested = state.pop("manual_pet_update_requested", None)
    if requested is None:
        return False
    write_runtime_state(state)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="Codex pet hardware daemon")
    parser.add_argument("--codex-bin", default=None, help="Path to codex executable")
    parser.add_argument("--timeout", type=float, default=20.0, help="app-server request timeout")
    parser.add_argument("--limit", default="codex", help="rate limit id to display")
    parser.add_argument("--json", action="store_true", help="print only compact JSON payload")
    parser.add_argument("--usage-once", action="store_true", help="read Codex usage once and exit")
    parser.add_argument("--once", action="store_true", help="read usage and optional pet state once")
    parser.add_argument("--watch", action="store_true", help="keep refreshing until interrupted")
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between --watch refreshes")
    parser.add_argument("--probe-interval", type=float, default=0.2,
                        help="seconds between local rollout-file probes in --watch")
    parser.add_argument("--quota-interval", type=float, default=300.0,
                        help="seconds between periodic quota refreshes when no thread activity occurs")
    parser.add_argument("--ble", action="store_true", help="send payloads to the ESP32 over BLE")
    parser.add_argument("--device-name", default=DEVICE_NAME, help="BLE advertised name")
    parser.add_argument("--serial-port", default=None, help="write compact JSON lines to a serial port")
    parser.add_argument("--serial-baud", type=int, default=921600, help="serial baud for sync traffic")
    parser.add_argument("--auto-serial-port", default=DEFAULT_AUTO_SERIAL_PORT,
                        help="serial glob used by BLE mode for automatic pet updates; use auto on Windows")
    parser.add_argument("--control-port", type=int, default=3490,
                        help="local control page port; set 0 to disable")
    parser.add_argument("--sync-pet-sprite", action="store_true",
                        help="send the selected Codex pet spritesheet frame over serial")
    parser.add_argument("--sync-output-image", action="store_true",
                        help="render and send the Codex output panel as an RGB565 image")
    parser.add_argument("--pet-max-size", default="113x122",
                        help="maximum rendered pet sprite size, e.g. 112x123")
    parser.add_argument("--pet-padding", type=int, default=0,
                        help="transparent padding around the selected pet frame before scaling")
    parser.add_argument("--pet-frame-cells", default="1x1",
                        help="spritesheet cells per displayed frame, e.g. 1x1, 1x2, or auto")
    parser.add_argument("--pet-crop-visible", action="store_true",
                        help="crop transparent margins before scaling instead of matching Codex frame bounds")
    parser.add_argument("--pet-anim-frames", default="codex",
                        help="'codex' for Codex state animation, or comma-separated frame indexes")
    parser.add_argument("--cwd", type=Path, default=Path.cwd(), help="workspace used to find Codex thread output")
    parser.add_argument("--project", default=None, help="project name override for the display")
    parser.add_argument("--output-text", default=None, help="output text override for the display")
    parser.add_argument("--pet-dir", type=Path, default=None, help="Codex pet directory")
    parser.add_argument("--pet-state", default="thinking", help="pet state for --once")
    parser.add_argument("--pet-preview", type=Path, default=None, metavar="PET_DIR", help="export preview frames")
    parser.add_argument("--preview-out", type=Path, default=Path("assets/generated/mikoto/frames"))
    parser.add_argument("--preview-grid", default="8x9", help="spritesheet grid, e.g. 8x9")
    parser.add_argument("--preview-samples", default="0,1,12,24", help="comma-separated frame indexes")
    args = parser.parse_args()

    if args.pet_preview:
        cols_s, rows_s = args.preview_grid.lower().split("x", 1)
        samples = [int(part.strip()) for part in args.preview_samples.split(",") if part.strip()]
        pet_preview(args.pet_preview, args.preview_out, int(cols_s), int(rows_s), samples)
        return 0

    if not args.usage_once and not args.once and not args.watch:
        parser.print_help()
        return 0

    if args.watch:
        start_control_server(args.control_port)
        save_runtime_state(started_at=_DAEMON_STARTED_AT_MS, mode="starting")

    if args.watch and args.serial_port:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is required for --serial-port") from exc
        while True:
            try:
                with serial.Serial(args.serial_port, args.serial_baud, timeout=2, write_timeout=3) as ser:
                    ser.dtr = False
                    ser.rts = False
                    time.sleep(4.0)
                    ser.reset_input_buffer()
                    ser.reset_output_buffer()
                    global _LAST_PET_SYNC_KEY, _LAST_PET_ATLAS_KEY, _LAST_PET_SELECT_KEY
                    _LAST_PET_SYNC_KEY = None
                    _LAST_PET_ATLAS_KEY = None
                    _LAST_PET_SELECT_KEY = None
                    last_marker: tuple[Any, ...] | None = None
                    last_output_key: tuple[Any, ...] | None = None
                    last_pet_key: tuple[Any, ...] | None = None
                    cached_usage = read_usage(args)
                    next_quota_refresh = time.monotonic()
                    while True:
                        changed, last_marker = wait_for_probe_change(
                            args,
                            last_marker,
                            next_quota_refresh,
                        )
                        quota_due = not changed
                        if quota_due:
                            cached_usage = read_usage(args)
                        session_list_requested = consume_session_list_request()
                        cwd = args.cwd.expanduser().resolve()
                        session_changed = (
                            consume_session_select_request(cwd)
                            or consume_session_next_request(cwd)
                        )
                        if session_list_requested:
                            pet_dir = args.pet_dir or find_default_pet_dir()
                            emit_payloads(args, cached_usage, [build_session_list_payload(cwd)],
                                          pet_dir, args.pet_state, ser)
                            continue
                        usage, payloads, pet_dir, pet_anim_state = build_payloads(args, cached_usage)
                        current_output_key = output_payload_key(payloads)
                        current_pet_key = pet_payload_key(pet_dir)
                        should_emit = (
                            quota_due
                            or session_changed
                            or current_output_key != last_output_key
                            or current_pet_key != last_pet_key
                        )
                        if not should_emit:
                            continue
                        emit_payloads(args, usage, payloads, pet_dir, pet_anim_state, ser)
                        last_output_key = current_output_key
                        last_pet_key = current_pet_key
                        if changed:
                            next_quota_refresh = max(next_quota_refresh, time.monotonic() + args.quota_interval)
                        else:
                            next_quota_refresh = time.monotonic() + args.quota_interval
            except (OSError, RuntimeError, TimeoutError, serial.SerialException) as exc:
                print(f"serial reconnect after error: {exc}", flush=True)
                time.sleep(2.0)
    elif args.watch and args.ble:
        start_ble_bridge(args.device_name)
        last_marker: tuple[Any, ...] | None = None
        last_output_key: tuple[Any, ...] | None = None
        last_pet_key: tuple[Any, ...] | None = None
        live_serial = None
        cached_usage = read_usage(args)
        next_quota_refresh = time.monotonic()
        while True:
            if live_serial is None:
                live_serial = open_live_serial_handle(args)
            changed, last_marker = wait_for_probe_change(
                args,
                last_marker,
                next_quota_refresh,
                live_serial,
            )
            quota_due = not changed
            if quota_due:
                cached_usage = read_usage(args)
            session_list_requested = consume_session_list_request()
            cwd = args.cwd.expanduser().resolve()
            session_changed = (
                consume_session_select_request(cwd)
                or consume_session_next_request(cwd)
            )
            if session_list_requested:
                pet_dir = args.pet_dir or find_default_pet_dir()
                try:
                    emit_payloads(args, cached_usage, [build_session_list_payload(cwd)],
                                  pet_dir, args.pet_state, live_serial)
                except Exception as exc:
                    if live_serial is not None:
                        try:
                            live_serial.close()
                        except Exception:
                            pass
                        live_serial = None
                        save_runtime_state(serial_port=None, last_error=str(exc))
                        print(f"serial live bridge reset after error: {exc}", flush=True)
                        emit_payloads(args, cached_usage, [build_session_list_payload(cwd)],
                                      pet_dir, args.pet_state, None)
                    else:
                        raise
                continue
            usage, payloads, pet_dir, pet_anim_state = build_payloads(args, cached_usage)
            current_output_key = output_payload_key(payloads)
            current_pet_key = pet_payload_key(pet_dir)
            current_render_key = pet_render_key(
                pet_dir,
                args.preview_grid,
                args.pet_max_size,
                args.pet_padding,
                args.pet_frame_cells,
                args.pet_crop_visible,
                args.pet_anim_frames,
            )
            manual_pet_update = consume_manual_pet_update_request()
            pet_needs_update = args.sync_pet_sprite and not pet_cache_is_current(current_render_key)
            if (manual_pet_update or pet_needs_update) and find_serial_port(args.auto_serial_port):
                if live_serial is not None:
                    try:
                        live_serial.close()
                    except Exception:
                        pass
                    live_serial = None
                project = project_name_for_cwd(args.cwd.expanduser().resolve(), args.project)
                if serial_update_pet_if_available(args, payloads, pet_dir, pet_anim_state, project):
                    pet_needs_update = False
                    last_pet_key = None
            elif args.sync_pet_sprite:
                save_runtime_state(pet_update="synced")
            should_emit = (
                quota_due
                or session_changed
                or current_output_key != last_output_key
                or current_pet_key != last_pet_key
                or pet_needs_update
            )
            if not should_emit:
                continue
            try:
                emit_payloads(args, usage, payloads, pet_dir, pet_anim_state, live_serial)
            except Exception as exc:
                if live_serial is not None:
                    try:
                        live_serial.close()
                    except Exception:
                        pass
                    live_serial = None
                    save_runtime_state(serial_port=None, last_error=str(exc))
                    print(f"serial live bridge reset after error: {exc}", flush=True)
                    emit_payloads(args, usage, payloads, pet_dir, pet_anim_state, None)
                else:
                    raise
            last_output_key = current_output_key
            last_pet_key = current_pet_key
            if changed:
                next_quota_refresh = max(next_quota_refresh, time.monotonic() + args.quota_interval)
            else:
                next_quota_refresh = time.monotonic() + args.quota_interval
    else:
        while True:
            usage, payloads, pet_dir, pet_anim_state = build_payloads(args)
            emit_payloads(args, usage, payloads, pet_dir, pet_anim_state)
            if not args.watch:
                break
            time.sleep(max(1.0, args.interval))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
