# SPDX-FileCopyrightText: 2026 Rashed Talukder
# SPDX-License-Identifier: Apache-2.0

import base64
import unittest
import zlib
import tempfile
import json
from pathlib import Path
from unittest import mock

import uitest_runner


class ScriptedSerial:
    def __init__(self, responses=None):
        self.responses = responses or {}
        self.pending = bytearray()
        self.writes = []
        self.reset_count = 0
        self.is_open = True
        self.timeout = 0.01

    def reset_input_buffer(self):
        self.pending.clear()
        self.reset_count += 1

    def write(self, data):
        self.writes.append(data)
        command = data.decode("ascii").strip()
        request, command = command.split(" ", 1)
        response = self.responses.get(command, b"")
        if isinstance(response, str):
            response = response.encode("ascii")
        self.pending.extend(f"UITEST: BEGIN {request[1:]}\n".encode("ascii"))
        self.pending.extend(response)
        self.pending.extend(f"UITEST: END {request[1:]}\n".encode("ascii"))
        return len(data)

    def flush(self):
        pass

    def read(self, size):
        if not self.pending:
            return b""
        chunk_size = min(size, 3, len(self.pending))
        chunk = bytes(self.pending[:chunk_size])
        del self.pending[:chunk_size]
        return chunk

    def close(self):
        self.is_open = False


def make_client(serial, timeout=0.02):
    client = object.__new__(uitest_runner.UITestClient)
    client.prefix = "UITEST"
    client.verbose = False
    client.timeout = timeout
    client._rx_buffer = b""
    client.ser = serial
    return client


class ClientTests(unittest.TestCase):
    def test_shot_reports_device_error_without_waiting_for_frame(self):
        client = make_client(ScriptedSerial({"SHOT": "UITEST: ERR SHOT ESP_ERR_NO_MEM\n"}))
        with self.assertRaisesRegex(ValueError, "ESP_ERR_NO_MEM"):
            client.shot()

    def test_get_and_assert_text(self):
        client = make_client(ScriptedSerial({"GET title":
            "UITEST: OK GET title VISIBLE:1 ENABLED:1 CHECKED:0 VALUE:0 TEXT:486f6d65 TRUNCATED:0\n"}))
        self.assertEqual(client.execute('ASSERT title text "Home"')["text"], "Home")
        with self.assertRaises(AssertionError):
            client.execute('ASSERT title text "Clock"')

    def test_wait_observes_changes(self):
        client = make_client(ScriptedSerial())
        with mock.patch.object(client, "get", side_effect=[{"value": 1}, {"value": 2}]):
            self.assertEqual(client.wait_for("slider", "value", 2), {"value": 2})

    def test_failure_report_preserves_artifact_error(self):
        client = make_client(ScriptedSerial())
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "script.txt"
            script.write_text("ASSERT title visible true\n")
            with mock.patch.object(client, "execute", side_effect=AssertionError("wrong state")):
                with self.assertRaisesRegex(RuntimeError, "script failed"):
                    client.run_script(script, directory)
            report = json.loads((Path(directory) / "report.json").read_text())
            self.assertFalse(report["passed"])
            self.assertIn("artifact_error", report["steps"][0])

    def test_waits_for_ready_after_serial_open_reset(self):
        serial = ScriptedSerial({"INFO": "UITEST: OK INFO W:320 H:240 ROT:0 PERIOD:30\n"})
        serial.pending.extend(
            b"ets Jul 29 2019 12:21:46\n"
            b"rst:0x1 (POWERON_RESET),boot:0x17\n"
            b"I (4572) UITEST: UI test harness ready - commands: INFO DUMP\n"
        )
        client = make_client(serial)
        client._wait_for_device_ready(detect_timeout=0.1, ready_timeout=0.1)
        self.assertEqual(serial.pending, b"")

    def test_serial_open_reset_without_ready_fails(self):
        serial = ScriptedSerial()
        serial.pending.extend(b"ets Jul 29 2019 12:21:46\nrst:0x1\n")
        client = make_client(serial)
        with self.assertRaisesRegex(RuntimeError, "did not become ready"):
            client._wait_for_device_ready(detect_timeout=0.01,
                                          ready_timeout=0.01)

    def test_matches_reply_verb_and_fragmented_lines(self):
        serial = ScriptedSerial({
            "INFO": (
                "UITEST: OK TAP 1 2\n"
                "UITEST: OK INFO W:320 H:240 ROT:0 PERIOD:30\n"
            )
        })
        lines = make_client(serial).info()
        self.assertEqual(lines[-1], "UITEST: OK INFO W:320 H:240 ROT:0 PERIOD:30")
        self.assertEqual(serial.reset_count, 1)
        self.assertEqual(serial.writes, [b"@1 INFO\n"])

    def test_raises_matching_device_error(self):
        serial = ScriptedSerial({"TAP 999 999": "UITEST: ERR TAP ESP_ERR_INVALID_ARG\n"})
        with self.assertRaisesRegex(RuntimeError, "ESP_ERR_INVALID_ARG"):
            make_client(serial).tap(999, 999)

    def test_raises_on_generic_overlong_error(self):
        serial = ScriptedSerial({"INFO": "UITEST: ERR command too long\n"})
        with self.assertRaisesRegex(RuntimeError, "command too long"):
            make_client(serial).info()

    def test_times_out_without_matching_reply(self):
        serial = ScriptedSerial({"INFO": "UITEST: OK TAP 1 2\n"})
        with self.assertRaisesRegex(RuntimeError, "timeout"):
            make_client(serial, timeout=0.001).info()

    def test_dump_returns_nodes(self):
        node = "UITEST: NODE d:0 id:- x:0 y:0 w:320 h:240 click:0 hidden:0"
        crc = zlib.crc32(f"{node}\n".encode("ascii")) & 0xFFFFFFFF
        serial = ScriptedSerial({
            "DUMP": (
                "---UITEST_DUMP_START---\n"
                f"{node}\n"
                f"UITEST: DUMP CRC32:{crc:08x} COUNT:1 TRUNCATED:0\n"
                "---UITEST_DUMP_END---\n"
            )
        })
        nodes = make_client(serial).dump()
        self.assertEqual(len(nodes), 1)

    def test_dump_reports_device_error(self):
        serial = ScriptedSerial({
            "DUMP": (
                "---UITEST_DUMP_START---\n"
                "UITEST: ERR DUMP ESP_ERR_NO_MEM\n"
                "---UITEST_DUMP_END---\n"
            )
        })
        with self.assertRaisesRegex(RuntimeError, "ESP_ERR_NO_MEM"):
            make_client(serial).dump()

    def test_dump_rejects_crc_mismatch(self):
        serial = ScriptedSerial({
            "DUMP": (
                "---UITEST_DUMP_START---\n"
                "UITEST: NODE d:0 id:- x:0 y:0 w:1 h:1 click:0 hidden:0\n"
                "UITEST: DUMP CRC32:00000000 COUNT:1 TRUNCATED:0\n"
                "---UITEST_DUMP_END---\n"
            )
        })
        with self.assertRaisesRegex(RuntimeError, "CRC32"):
            make_client(serial).dump()

    def test_dump_rejects_malformed_node(self):
        serial = ScriptedSerial({
            "DUMP": (
                "---UITEST_DUMP_START---\n"
                "UITEST: NODE broken\n"
                "UITEST: DUMP CRC32:00000000 COUNT:1 TRUNCATED:0\n"
                "---UITEST_DUMP_END---\n"
            )
        })
        with self.assertRaisesRegex(RuntimeError, "malformed"):
            make_client(serial).dump()

    def test_rejects_unsafe_or_long_commands(self):
        client = make_client(ScriptedSerial())
        with self.assertRaises(ValueError):
            client.command("INFO\nTAP 1 2")
        with self.assertRaises(ValueError):
            client.command("X" * (uitest_runner.MAX_COMMAND_BYTES + 1))

    def test_rejects_wrong_gesture_arguments(self):
        serial = ScriptedSerial({"TAP 10 20": "UITEST: OK TAP 250 230\n"})
        with self.assertRaisesRegex(RuntimeError, "mismatched"):
            make_client(serial).tap(10, 20)

    def test_dump_dispatch_rejects_truncation(self):
        serial = ScriptedSerial({"DUMP": (
            "---UITEST_DUMP_START---\n"
            "UITEST: DUMP CRC32:00000000 COUNT:0 TRUNCATED:1\n"
            "---UITEST_DUMP_END---\n")})
        with self.assertRaisesRegex(RuntimeError, "truncated"):
            make_client(serial).command("DUMP")

    def test_ignores_stale_request_envelope(self):
        serial = ScriptedSerial({"INFO": "UITEST: OK INFO W:320 H:240\n"})
        original_write = serial.write
        def write(data):
            count = original_write(data)
            serial.pending[:0] = b"UITEST: BEGIN 999\nUITEST: OK INFO stale\nUITEST: END 999\n"
            return count
        serial.write = write
        self.assertEqual(make_client(serial).info(), ["UITEST: OK INFO W:320 H:240"])

    def test_shot_decodes_and_checks_crc(self):
        serial = ScriptedSerial({"SHOT": (
            "---SCREENSHOT_START---\nPROTO:2\nSEQ:7\nFRAME:FULL\n"
            "CANVAS_W:1\nCANVAS_H:1\nW:1\nH:1\nSTRIDE:3\nFMT:RGB888\n"
            "ENC:BASE64\nTRANSPORT:BASE64\nCRC32:00000000\nAQID\n"
            "---SCREENSHOT_END---\nUITEST: OK SHOT\n")})
        with self.assertRaisesRegex(ValueError, "CRC32"):
            make_client(serial).shot()

    def test_shot_area_and_delta_share_canvas(self):
        def frame(kind, x, width, pixel, base_crc, crc):
            return (
                f"---SCREENSHOT_START---\nPROTO:2\nSEQ:1\nFRAME:{kind}\nX:{x}\nY:0\n"
                f"CANVAS_W:2\nCANVAS_H:1\nBASE_CRC32:{base_crc:08x}\nW:{width}\nH:1\n"
                f"STRIDE:{width * 3}\nFMT:RGB888\nENC:BASE64\nTRANSPORT:BASE64\n"
                f"CRC32:{crc:08x}\n{base64.b64encode(pixel).decode()}\n---SCREENSHOT_END---\n")
        key = bytes([1, 2, 3, 4, 5, 6])
        patch = bytes([9, 9, 9])
        serial = ScriptedSerial({
            "SHOT KEY": frame("KEY", 0, 2, key, 0, zlib.crc32(key)) + "UITEST: OK SHOT KEY\n",
            "SHOT DELTA": frame("DELTA", 1, 1, patch, zlib.crc32(key), zlib.crc32(patch))
                + "UITEST: OK SHOT DELTA\n",
            "SHOT AREA 1 0 1 1": frame("REGION", 1, 1, patch, 0, zlib.crc32(patch))
                + "UITEST: OK SHOT AREA 1 0 1 1\n",
        })
        client = make_client(serial)
        client.command("SHOT KEY")
        image = client.shot(mode=["delta"])
        self.assertEqual(image.tobytes("raw", "BGR"), key[:3] + patch)
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(client.execute("SHOT AREA 1 0 1 1", directory).keys(), {"image"})
        with self.assertRaises(ValueError):
            client.shot(mode=["AREA", "1"])

    def test_get_reports_truncated_text(self):
        client = make_client(ScriptedSerial({"GET title":
            "UITEST: OK GET title VISIBLE:1 ENABLED:1 CHECKED:0 VALUE:3 TEXT:486f TRUNCATED:1\n"}))
        result = client.get("title")
        self.assertTrue(result["truncated"])
        self.assertEqual((result["text"], result["value"]), ("Ho", 3))


if __name__ == "__main__":
    unittest.main()