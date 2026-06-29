#!/usr/bin/env python3
"""Unit tests for CodexMeter daemon payload helpers."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from daemon import codexmeter_daemon as daemon  # noqa: E402


class UsagePayloadTests(unittest.TestCase):
    def test_build_usage_payload_uses_selected_limit_snapshot(self) -> None:
        account = {"account": {"planType": "pro"}}
        rates = {
            "rateLimitsByLimitId": {
                "codex": {
                    "limitId": "codex",
                    "planType": "team",
                    "primary": {"usedPercent": 42, "resetsAt": 1060},
                    "secondary": {"usedPercent": 13, "resetsAt": 3700},
                }
            }
        }

        with mock.patch.object(daemon.time, "time", return_value=1000.0):
            usage = daemon.build_usage_payload(account, rates, "codex")

        self.assertEqual(usage.payload["kind"], "codex_usage")
        self.assertTrue(usage.payload["ok"])
        self.assertEqual(usage.payload["plan"], "team")
        self.assertEqual(usage.payload["limit"], "codex")
        self.assertEqual(usage.payload["p"], 42)
        self.assertEqual(usage.payload["pr"], 1)
        self.assertEqual(usage.payload["w"], 13)
        self.assertEqual(usage.payload["wr"], 45)
        self.assertEqual(usage.payload["st"], "allowed")
        self.assertIn("5h=42%", usage.summary)
        self.assertIn("week=13%", usage.summary)

    def test_build_usage_payload_marks_limited_state(self) -> None:
        rates = {
            "rateLimits": {
                "primary": {"usedPercent": 100, "resetsAt": 1000},
                "secondary": {"usedPercent": 80, "resetsAt": 1000},
                "rateLimitReachedType": "primary",
            }
        }

        with mock.patch.object(daemon.time, "time", return_value=1000.0):
            usage = daemon.build_usage_payload({"account": {}}, rates, "codex")

        self.assertEqual(usage.payload["st"], "limited")
        self.assertIn("status=limited", usage.summary)


class DisplayTextTests(unittest.TestCase):
    def test_device_display_lines_preserves_cjk_and_normalizes_punctuation(self) -> None:
        lines = daemon.device_display_lines("你好，世界！", "CodexMeter")

        self.assertEqual(lines[0], "你好,世界!")
        self.assertEqual(len(lines), 8)
        self.assertTrue(all(isinstance(line, str) for line in lines))

    def test_split_dialog_lines_truncates_long_text(self) -> None:
        lines = daemon.split_dialog_lines("x" * 300, max_lines=3, line_units=10)

        self.assertEqual(len(lines), 3)
        self.assertEqual(lines[-1], "xxxxxxx...")


class RolloutParsingTests(unittest.TestCase):
    def test_latest_context_usage_reads_latest_token_count(self) -> None:
        items = [
            {
                "type": "event_msg",
                "payload": {
                    "type": "token_count",
                    "info": {
                        "last_token_usage": {"input_tokens": 10},
                        "model_context_window": 200,
                    },
                },
            },
            {
                "type": "event_msg",
                "payload": {
                    "type": "token_count",
                    "info": {
                        "last_token_usage": {"input_tokens": 25},
                        "model_context_window": 400,
                    },
                },
            },
        ]

        self.assertEqual(daemon.latest_context_usage(items), (25, 400))

    def test_latest_rollout_status_detects_waiting_tool_call(self) -> None:
        items = [
            {
                "type": "response_item",
                "payload": {
                    "type": "function_call",
                    "name": "request_user_input",
                    "call_id": "call-1",
                },
            }
        ]

        self.assertEqual(daemon.latest_rollout_status(items), "waiting")


class BlePayloadTests(unittest.TestCase):
    def test_ble_payload_packets_reassembles_large_payload(self) -> None:
        payload = {"kind": "codex_output", "l1": "x" * 1200}

        packets = daemon.ble_payload_packets([payload])
        self.assertGreater(len(packets), 1)

        chunks = [json.loads(packet.decode("utf-8")) for packet in packets]
        self.assertTrue(all(chunk["kind"] == "chunk" for chunk in chunks))
        self.assertEqual([chunk["i"] for chunk in chunks], list(range(len(chunks))))
        self.assertTrue(all(chunk["n"] == len(chunks) for chunk in chunks))
        self.assertEqual(len({chunk["id"] for chunk in chunks}), 1)

        assembled = "".join(chunk["d"] for chunk in chunks)
        self.assertEqual(json.loads(assembled), payload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
