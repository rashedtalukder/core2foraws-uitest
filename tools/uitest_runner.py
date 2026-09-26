# SPDX-FileCopyrightText: 2026 Rashed Talukder
# SPDX-License-Identifier: Apache-2.0
"""
Host-side runner for the Core2 for AWS LVGL UI test harness.

A small Playwright-style driver that injects touch input over UART and reads
back the firmware's structured replies. Pair with the screenshot component for
visual assertions.

Examples:
    # Print device geometry, dump the widget tree, then tap and swipe:
    python uitest_runner.py --port /dev/cu.usbserial-02036BEC info
    python uitest_runner.py --port /dev/cu.usbserial-02036BEC dump
    python uitest_runner.py --port /dev/cu.usbserial-02036BEC tap 160 120
    python uitest_runner.py --port /dev/cu.usbserial-02036BEC swipe 300 120 20 120 250
    python uitest_runner.py --port /dev/cu.usbserial-02036BEC click home.wifi_btn

    # Run a script of commands (one per line), e.g. a smoke test:
    python uitest_runner.py --port ... script smoke.txt

Firmware requirements:
    CONFIG_UITEST_ENABLED=y
    (do NOT also enable CONFIG_SCREENSHOT_SERIAL_TRIGGER — both read stdin)
"""

import argparse
import importlib.util
import json
import re
import secrets
import shlex
import sys
import time
import zlib
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install -r requirements.txt")

PREFIX = "UITEST"
# Matches CONFIG_ESP_CONSOLE_UART_BAUDRATE in the factory firmware.
DEFAULT_BAUD = 921600
DEFAULT_REPLY_TIMEOUT = 10.0
SHOT_TIMEOUT = 120.0
MAX_COMMAND_BYTES = 159
OPEN_RESET_DETECT_TIMEOUT = 0.5
OPEN_RESET_READY_TIMEOUT = 10.0


def screenshot_module():
    path = Path(__file__).resolve().parents[2] / "core2foraws-screenshot/tools/screenshot_capture.py"
    spec = importlib.util.spec_from_file_location("screenshot_capture", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CaptureStream:
    def __init__(self, client):
        self.client = client

    @property
    def timeout(self):
        return self.client.ser.timeout

    @timeout.setter
    def timeout(self, value):
        self.client.ser.timeout = value

    def read_until(self, delimiter, size=None):
        client = self.client
        if not client._rx_buffer:
            client._rx_buffer = client.ser.read(min(256, size or 256))
        end = client._rx_buffer.find(delimiter)
        count = end + len(delimiter) if end >= 0 else len(client._rx_buffer)
        count = min(count, size or count)
        result, client._rx_buffer = client._rx_buffer[:count], client._rx_buffer[count:]
        return result


class UITestClient:
    def __init__(self, port, baud=DEFAULT_BAUD, prefix=PREFIX, verbose=False,
                 timeout=DEFAULT_REPLY_TIMEOUT, screenshot_marker="SCREENSHOT"):
        self.prefix = prefix
        self.verbose = verbose
        self.timeout = timeout
        self.screenshot_marker = screenshot_marker
        self._rx_buffer = b""
        self._request_id = secrets.randbelow(0x7FFFFFFE) + 1
        # Keep DTR/RTS deasserted so opening the port does not reset the board.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        try:
            self._wait_for_device_ready()
        except Exception:
            self.ser.close()
            raise
        self.ser.reset_input_buffer()

    def _wait_for_device_ready(self, detect_timeout=OPEN_RESET_DETECT_TIMEOUT,
                               ready_timeout=OPEN_RESET_READY_TIMEOUT):
        """Wait for a correlated reply, whether or not serial open reset the board."""
        deadline = time.monotonic() + ready_timeout
        while time.monotonic() < deadline:
            try:
                self.command("INFO", timeout=min(0.7, max(0, deadline - time.monotonic())))
                return
            except RuntimeError:
                continue
        raise RuntimeError("UI test harness did not become ready")

    def close(self):
        if self.ser.is_open:
            self.ser.close()

    def _send(self, line):
        if not line or "\n" in line or "\r" in line:
            raise ValueError("command must be one non-empty line")
        try:
            encoded = line.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError("commands must contain ASCII only") from exc
        if len(encoded) > MAX_COMMAND_BYTES:
            raise ValueError(f"command exceeds {MAX_COMMAND_BYTES} bytes")
        if self.verbose:
            print(f">> {line}")
        self.ser.reset_input_buffer()
        self._rx_buffer = b""
        self.ser.write(encoded + b"\n")
        self.ser.flush()

    def _read_until(self, predicate, timeout=None):
        """Collect lines until predicate(line) is True or timeout. Returns the
        list of matching/relevant lines that carry our prefix."""
        if timeout is None:
            timeout = self.timeout
        deadline = time.monotonic() + timeout
        collected = []
        while time.monotonic() < deadline:
            self._rx_buffer += self.ser.read(256)
            if len(self._rx_buffer) > 8192 or len(collected) > 4096:
                raise RuntimeError("reply exceeds size limit")
            while b"\n" in self._rx_buffer:
                raw, self._rx_buffer = self._rx_buffer.split(b"\n", 1)
                line = raw.decode("ascii", errors="replace").strip()
                if not line:
                    continue
                if self.verbose:
                    print(f"<< {line}")
                if line.startswith(self.prefix) or line.startswith("---"):
                    collected.append(line)
                if predicate(line):
                    return collected
        return collected

    def _begin(self, line, timeout):
        self._request_id = (getattr(self, "_request_id", 0) % 0xFFFFFFFF) + 1
        self._send(f"@{self._request_id} {line}")
        marker = f"{self.prefix}: BEGIN {self._request_id}"
        replies = self._read_until(lambda response: response == marker, timeout)
        if marker not in replies:
            raise RuntimeError(f"command timeout waiting for request {self._request_id}")

    def _finish(self, timeout):
        marker = f"{self.prefix}: END {self._request_id}"
        replies = self._read_until(lambda response: response == marker, timeout)
        if marker not in replies:
            raise RuntimeError(f"command timeout waiting for completion {self._request_id}")
        return replies[:-1]

    def _transaction(self, line, timeout=None):
        timeout = self.timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        self._begin(line, timeout)
        return self._finish(max(0, deadline - time.monotonic()))

    def _check_reply(self, line, replies):
        verb = line.split()[0]
        error = next((reply for reply in replies if reply.startswith(f"{self.prefix}: ERR ")), None)
        if error:
            raise RuntimeError(f"command failed: {line!r} -> {error}")
        expected = f"{self.prefix}: OK {line}"
        valid = any(reply == expected or
                    (verb in ("INFO", "GET") and reply.startswith(expected + " "))
                    for reply in replies)
        if not valid:
            raise RuntimeError(f"command timeout or mismatched reply: {line!r}")
        return replies

    def command(self, line, timeout=None):
        """Dispatch with command-specific parsing and correlated completion."""
        if not line or "\n" in line or "\r" in line:
            raise ValueError("command must be one non-empty line")
        fields = line.split()
        if not fields:
            raise ValueError("empty command")
        line = " ".join([fields[0].upper()] + fields[1:])
        if line == "DUMP":
            return self.dump()
        if fields[0].upper() == "SHOT":
            return self.shot(mode=fields[1:])
        return self._check_reply(line, self._transaction(line, timeout))

    # ── high-level helpers ────────────────────────────────────────
    def info(self):
        return self.command("INFO")

    def tap(self, x, y):
        return self.command(f"TAP {x} {y}")

    def long_press(self, x, y, ms):
        return self.command(f"LONGPRESS {x} {y} {ms}")

    def swipe(self, x0, y0, x1, y1, ms):
        return self.command(f"SWIPE {x0} {y0} {x1} {y1} {ms}")

    def click(self, obj_id):
        return self.command(f"CLICK {obj_id}")

    def get(self, obj_id, timeout=None):
        replies = self.command(f"GET {obj_id}", timeout=timeout)
        pattern = re.compile(
            rf"^{re.escape(self.prefix)}: OK GET {re.escape(obj_id)} "
            r"VISIBLE:([01]) ENABLED:([01]) CHECKED:([01]) VALUE:(-?\d+) "
            r"TEXT:([0-9a-f]+|-) TRUNCATED:([01])$")
        matches = [pattern.fullmatch(reply) for reply in replies]
        match = next((match for match in matches if match is not None), None)
        if match is None:
            raise RuntimeError("malformed GET response")
        try:
            text = "" if match[5] == "-" else bytes.fromhex(match[5]).decode("utf-8")
        except (ValueError, UnicodeError) as exc:
            raise RuntimeError("invalid GET text encoding") from exc
        return dict(id=obj_id, visible=match[1] == "1", enabled=match[2] == "1",
                    checked=match[3] == "1", value=int(match[4]), text=text,
                    truncated=match[6] == "1")

    def wait_for(self, obj_id, field, expected, timeout=None):
        if field not in ("visible", "enabled", "checked", "value", "text"):
            raise ValueError(f"unknown assertion field: {field}")
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        observed = None
        while time.monotonic() < deadline:
            observed = self.get(obj_id, timeout=max(0, deadline - time.monotonic()))
            if observed[field] == expected:
                return observed
        raise AssertionError(f"{obj_id}.{field}: expected {expected!r}, observed {observed!r}")

    def execute(self, line, artifact_dir=None, name="shot"):
        fields = shlex.split(line)
        if not fields:
            raise ValueError("empty command")
        verb = fields[0].upper()
        if verb in ("ASSERT", "WAIT"):
            if len(fields) < 4:
                raise ValueError(f"{verb} <id> <field> <expected>")
            obj_id, field = fields[1:3]
            value = " ".join(fields[3:])
            if field in ("visible", "enabled", "checked"):
                if value not in ("true", "false", "0", "1"):
                    raise ValueError("boolean assertion expects true/false or 0/1")
                value = value in ("true", "1")
            elif field == "value":
                value = int(value)
            elif field != "text":
                raise ValueError(f"unknown assertion field: {field}")
            if verb == "WAIT":
                return self.wait_for(obj_id, field, value)
            observed = self.get(obj_id)
            if observed[field] != value:
                raise AssertionError(f"{obj_id}.{field}: expected {value!r}, observed {observed[field]!r}")
            return observed
        if verb == "GET" and len(fields) == 2:
            return self.get(fields[1])
        if verb == "SHOT":
            output = Path(artifact_dir or ".") / f"{name}.png"
            self.shot(output, fields[1:])
            return {"image": str(output)}
        if verb == "COMPARE" and len(fields) == 2:
            output = Path(artifact_dir or ".") / f"{name}.png"
            image = self.shot(output)
            if not screenshot_module().compare_golden(image, fields[1], output.with_suffix(".diff.png")):
                raise AssertionError(f"image differs from {fields[1]}")
            return {"image": str(output)}
        return self.command(line)

    def run_script(self, path, artifact_dir, continue_on_error=False):
        directory = Path(artifact_dir)
        directory.mkdir(parents=True, exist_ok=True)
        report = {"script": str(path), "steps": [], "passed": True}
        try:
            for number, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                step = {"line": number, "command": line}
                started = time.monotonic()
                try:
                    step["result"] = self.execute(line, directory, f"step-{number}")
                    step["passed"] = True
                except (RuntimeError, ValueError, AssertionError, OSError) as exc:
                    step.update(passed=False, error=str(exc))
                    report["passed"] = False
                    try:
                        self.command("CANCEL")
                        step["failure_image"] = self.execute("SHOT", directory, f"failure-{number}")
                    except (RuntimeError, ValueError, OSError) as artifact_error:
                        step["artifact_error"] = str(artifact_error)
                step["duration_ms"] = round((time.monotonic() - started) * 1000)
                report["steps"].append(step)
                if not step["passed"] and not continue_on_error:
                    break
        finally:
            (directory / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        if not report["passed"]:
            raise RuntimeError(f"script failed; see {directory / 'report.json'}")
        return report

    @staticmethod
    def _shot_command(mode):
        fields = list(mode or [])
        if not fields:
            return "SHOT"
        keyword = fields[0].upper()
        if keyword in ("KEY", "DELTA") and len(fields) == 1:
            return f"SHOT {keyword}"
        if keyword == "AREA" and len(fields) == 5:
            try:
                values = [int(value) for value in fields[1:]]
            except ValueError:
                pass
            else:
                return "SHOT AREA " + " ".join(str(value) for value in values)
        raise ValueError("SHOT [KEY|DELTA|AREA <x> <y> <w> <h>]")

    def shot(self, output=None, mode=None):
        """Capture a frame; mode is None, ["KEY"], ["DELTA"], or ["AREA", x, y, w, h]."""
        line = self._shot_command(mode)
        screenshot = screenshot_module()
        deadline = time.monotonic() + SHOT_TIMEOUT
        self._begin(line, SHOT_TIMEOUT)
        marker = getattr(self, "screenshot_marker", "SCREENSHOT")
        metadata, payload = screenshot.capture(
            CaptureStream(self), f"---{marker}_START---", f"---{marker}_END---",
            max(0, deadline - time.monotonic()), expect_running=True,
            error_prefix=f"{self.prefix}: ERR SHOT")
        if metadata is None:
            raise RuntimeError("SHOT frame missing")
        # DELTA frames patch the canvas kept from earlier KEY/FULL frames.
        decoder = getattr(self, "_frame_decoder", None)
        if decoder is None:
            decoder = self._frame_decoder = screenshot.FrameDecoder()
        image = decoder.apply(metadata, payload)
        self._check_reply(line, self._finish(max(0, deadline - time.monotonic())))
        if output is not None:
            image.save(output)
            screenshot.write_sidecar(Path(output), metadata)
        return image

    def dump(self):
        lines = self._transaction("DUMP")
        error = next(
            (line for line in lines if line.startswith(f"{self.prefix}: ERR DUMP")),
            None,
        )
        if error:
            raise RuntimeError(f"DUMP failed: {error}")
        if not any(line.startswith(f"---{self.prefix}_DUMP_END---") for line in lines):
            raise RuntimeError("DUMP timed out")
        escaped_prefix = re.escape(self.prefix)
        node_pattern = re.compile(
            rf"^{escaped_prefix}: NODE d:(?P<depth>\d+) id:(?P<id>\S+) "
            r"x:(?P<x>-?\d+) y:(?P<y>-?\d+) w:(?P<width>-?\d+) "
            r"h:(?P<height>-?\d+) click:(?P<click>[01]) hidden:(?P<hidden>[01])$"
        )
        trailer_pattern = re.compile(
            rf"^{escaped_prefix}: DUMP CRC32:(?P<crc>[0-9a-fA-F]{{8}}) "
            r"COUNT:(?P<count>\d+) TRUNCATED:(?P<truncated>[01])$"
        )
        nodes = [line for line in lines if line.startswith(f"{self.prefix}: NODE")]
        for node in nodes:
            if node_pattern.fullmatch(node) is None:
                raise RuntimeError(f"malformed DUMP node: {node}")

        trailer_line = next(
            (line for line in lines if line.startswith(f"{self.prefix}: DUMP CRC32:")),
            None,
        )
        trailer = trailer_pattern.fullmatch(trailer_line or "")
        if trailer is None:
            raise RuntimeError("DUMP integrity trailer missing or malformed")
        if int(trailer.group("count")) != len(nodes):
            raise RuntimeError("DUMP node count mismatch")
        payload = "".join(f"{node}\n" for node in nodes).encode("ascii")
        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        expected_crc = int(trailer.group("crc"), 16)
        if actual_crc != expected_crc:
            raise RuntimeError(
                f"DUMP CRC32 mismatch: got {actual_crc:08x}, expected {expected_crc:08x}"
            )
        if trailer.group("truncated") != "0":
            raise RuntimeError("DUMP truncated; completeness cannot be asserted")
        if lines.count(f"---{self.prefix}_DUMP_START---") != 1 or lines.count(f"---{self.prefix}_DUMP_END---") != 1:
            raise RuntimeError("DUMP framing invalid")
        result = []
        for node in nodes:
            fields = node_pattern.fullmatch(node).groupdict()
            result.append({key: value if key == "id" else int(value)
                           for key, value in fields.items()})
        return result


def main():
    p = argparse.ArgumentParser(description="LVGL UI test harness runner")
    p.add_argument("--port", required=True, help="Serial port")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--screenshot-marker", default="SCREENSHOT")
    p.add_argument(
        "--timeout", type=float, default=DEFAULT_REPLY_TIMEOUT,
        help="Command reply timeout in seconds",
    )
    p.add_argument("-v", "--verbose", action="store_true")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info")
    sub.add_parser("dump")

    t = sub.add_parser("tap")
    t.add_argument("x", type=int)
    t.add_argument("y", type=int)

    lp = sub.add_parser("longpress")
    lp.add_argument("x", type=int)
    lp.add_argument("y", type=int)
    lp.add_argument("ms", type=int)

    sw = sub.add_parser("swipe")
    sw.add_argument("x0", type=int)
    sw.add_argument("y0", type=int)
    sw.add_argument("x1", type=int)
    sw.add_argument("y1", type=int)
    sw.add_argument("ms", type=int)

    cl = sub.add_parser("click")
    cl.add_argument("id")

    shot_parser = sub.add_parser("shot")
    shot_parser.add_argument("--output", default="screenshot.png")
    shot_parser.add_argument("--area", nargs=4, type=int, metavar=("X", "Y", "W", "H"),
                             help="Capture only this rectangle")
    get_parser = sub.add_parser("get")
    get_parser.add_argument("id")
    sub.add_parser("cancel")

    sc = sub.add_parser("script")
    sc.add_argument("file")
    sc.add_argument("--artifacts", default="uitest-artifacts")
    sc.add_argument(
        "--continue-on-error", action="store_true",
        help="Continue executing later script lines after a command failure",
    )

    args = p.parse_args()
    if args.timeout <= 0:
        p.error("--timeout must be greater than zero")
    client = UITestClient(
        args.port, args.baud, verbose=args.verbose, timeout=args.timeout,
        screenshot_marker=args.screenshot_marker
    )
    try:
        if args.cmd == "info":
            for l in client.info():
                print(l)
        elif args.cmd == "dump":
            for l in client.dump():
                print(l)
        elif args.cmd == "tap":
            client.tap(args.x, args.y)
            print("ok")
        elif args.cmd == "longpress":
            client.long_press(args.x, args.y, args.ms)
            print("ok")
        elif args.cmd == "swipe":
            client.swipe(args.x0, args.y0, args.x1, args.y1, args.ms)
            print("ok")
        elif args.cmd == "click":
            client.click(args.id)
            print("ok")
        elif args.cmd == "shot":
            mode = ["AREA", *map(str, args.area)] if args.area else None
            client.shot(args.output, mode)
            print("ok")
        elif args.cmd == "get":
            print(json.dumps(client.get(args.id)))
        elif args.cmd == "cancel":
            client.command("CANCEL")
        elif args.cmd == "script":
            client.run_script(args.file, args.artifacts, args.continue_on_error)
            print("script complete")
    finally:
        client.close()


if __name__ == "__main__":
    main()
