# Core2 for AWS — LVGL UI Test Harness

Version 1.0.0.

A Playwright-style automation layer for LVGL running on the device. It registers
a **synthetic LVGL pointer input device** plus a **UART command listener**, so a
host test runner can inject taps, clicks, long-presses and swipes against the
*real* UI on the *real* hardware. Combined with the
[screenshot component](https://github.com/rashedtalukder/core2foraws-screenshot) you get a full
act → capture → assert loop for automated visual QA.

## How it works

```text
Host runner ──UART cmd──> listener task ──> sample queue ──> lv_indev read_cb
                                                                      │
                                                              LVGL core dispatch
                                                                      │
                                                              widgets / events
   host  <───screenshot/DUMP──────────────────────────────────────── │
```

Injected points enter LVGL through a second `lv_indev_t` exactly like the real
FT6336 touch controller, so press / click / long-press / gesture / scroll
events are produced by LVGL's own pipeline — these are genuine interactions, not
synthetic event shortcuts.

### Coordinate space

On the Core2 the display is **320 × 240 landscape**, origin top-left, with **no
axis swap or mirror** (touch is configured 1:1 with the display). Injected
coordinates are raw LVGL/display coordinates: `0 ≤ x < 320`, `0 ≤ y < 240`.
Query the live geometry at runtime with the `INFO` command rather than
hardcoding it — it follows any rotation change.

## Enabling

In `menuconfig` → *Core2 for AWS UI Test Harness*:

| Option | Default | Description |
| --- | --- | --- |
| `UITEST_ENABLED` | `n` | Master enable |
| `UITEST_CMD_PREFIX` | `UITEST` | Prefix on every reply line |
| `UITEST_COMMAND_LINE_LENGTH` | `160` | Maximum UART command length including terminator |
| `UITEST_READ_PERIOD_MS` | `30` | Synthetic LVGL input polling period |
| `UITEST_SAMPLE_QUEUE_DEPTH` | `256` | Maximum samples reserved by one queued gesture |
| `UITEST_MAX_GESTURE_MS` | `5000` | LONGPRESS/SWIPE duration limit |
| `UITEST_DRAIN_TIMEOUT_MS` | `8000` | Maximum wait for LVGL to consume a gesture |
| `UITEST_SETTLE_MS` | `100` | Delay after release before replying `OK` |
| `UITEST_LVGL_LOCK_TIMEOUT_MS` | `1000` | Maximum LVGL mutex wait |
| `UITEST_SHOT_TIMEOUT_MS` | `60000` | Maximum wait for the screenshot worker before `ERR SHOT ESP_ERR_TIMEOUT` |
| `UITEST_MAX_REGISTERED` | `32` | Size of the id → object table |
| `UITEST_MAX_ID_LENGTH` | `64` | Copied widget ID size including terminator |
| `UITEST_MAX_DUMP_NODES` | `256` | Maximum nodes captured by DUMP |
| `UITEST_MAX_DUMP_DEPTH` | `32` | Maximum serialized tree depth |
| `UITEST_TASK_STACK_SIZE` | `8192` | Listener task stack |

> **Do not** also enable `CONFIG_SCREENSHOT_SERIAL_TRIGGER`. Both components read
> stdin; the UI test harness already provides a `SHOT` command (including
> `KEY`, `DELTA`, and `AREA` variants) that calls the screenshot component and
> waits for completion before accepting the next command.

`uitest_init()` is invoked from `app_main()` after the UI starts (guarded by
`CONFIG_UITEST_ENABLED`). `uitest_deinit()` stops the listener and removes the
synthetic input device; it waits for an in-progress command (bounded by the
drain timeout) and returns `ESP_ERR_TIMEOUT` if it must be retried. The
listener blocks in `select()` on stdin rather than polling.

## Command protocol

Commands are newline-terminated ASCII sent over UART. Every reply is prefixed
with `<PREFIX>:` (default `UITEST:`).

The host uses correlated envelopes: `@123 TAP 10 20` produces
`UITEST: BEGIN 123`, the usual command reply, then `UITEST: END 123`.
IDs are nonzero 32-bit unsigned integers. Commands execute serially; SHOT keeps
the envelope open until the dedicated capture worker finishes. Legacy commands
without `@id` remain accepted, but the runner requires envelope-capable firmware
and never treats stale replies as current results. Use one host client per
serial port and serialize calls to that client.

| Command | Reply | Action |
| --- | --- | --- |
| `INFO` | `UITEST: OK INFO W:320 H:240 ROT:0 PERIOD:30` | Report geometry + read period |
| `TAP <x> <y>` | `UITEST: OK TAP ...` | Press then release at a point |
| `LONGPRESS <x> <y> <ms>` | `UITEST: OK LONGPRESS ...` | Hold a press for `ms` then release |
| `SWIPE <x0> <y0> <x1> <y1> <ms>` | `UITEST: OK SWIPE ...` | Interpolated pressed drag over `ms` |
| `CLICK <id>` | `UITEST: OK CLICK <id>` | Tap the center of a registered widget |
| `DUMP` | `NODE` lines between `---UITEST_DUMP_START/END---` | Serialize the active screen widget tree |
| `SHOT` | Screenshot stream + `UITEST: OK SHOT` | Full-screen capture; waits for completion |
| `SHOT KEY` | Screenshot stream + `UITEST: OK SHOT KEY` | Reset the delta baseline and send a full keyframe |
| `SHOT DELTA` | Screenshot stream + `UITEST: OK SHOT DELTA` | Send only the region changed since the last KEY/DELTA frame |
| `SHOT AREA <x> <y> <w> <h>` | Screenshot stream + `UITEST: OK SHOT AREA ...` | Capture a rectangle of the active screen |
| `GET <id>` | `OK GET <id> VISIBLE:1 ENABLED:1 CHECKED:0 VALUE:0 TEXT:<hex> TRUNCATED:0` | Query visible/enabled/checked state, numeric value, or text |
| `CANCEL` | `UITEST: OK CANCEL` | Discard queued samples and wait for pointer reset |

`TAP`/`LONGPRESS`/`SWIPE`/`CLICK` reserve their complete sample sequence before
enqueueing, so concurrent callers cannot interleave or leave a partial press.
They only reply `OK` **after** LVGL consumes the final release and the configured
settle delay expires. Durations are rounded up to the input polling period.

Malformed commands, trailing arguments, out-of-range coordinates, excessive
durations, queue limits, and drain timeouts return command-specific `ERR` lines.
Overlong UART lines are drained and rejected as one command so subsequent input
remains synchronized.

Drain timeouts discard queued input and schedule a pointer reset; they return
an error, not a successful gesture. CANCEL only acknowledges after LVGL consumes
the reset and the settle delay expires. Public C injection functions return
after queueing, unlike serial commands, which wait for drain. Concurrent C
callers must coordinate logical test transactions with the serial client.

SHOT uses the asynchronous screenshot worker when `SCREENSHOT_ASYNC` is enabled
and gives up after `UITEST_SHOT_TIMEOUT_MS`, canceling queued captures;
otherwise it captures synchronously on the listener task. DELTA frames are only
valid against the host's canvas from a preceding `SHOT KEY`/`SHOT DELTA`.

GET returns text as UTF-8 bytes encoded in hexadecimal (`-` for empty text).
Text longer than 127 bytes is cut on a UTF-8 boundary and reported with
`TRUNCATED:1`. Text applies to labels, textareas, and checkboxes. Numeric VALUE
applies to sliders, bars, arcs (value), and rollers/dropdowns (selected index),
otherwise zero.

### `DUMP` output

```text
---UITEST_DUMP_START---
UITEST: NODE d:0 id:- x:0 y:0 w:320 h:240 click:0 hidden:0
UITEST: NODE d:1 id:home.wifi_btn x:240 y:8 w:64 h:32 click:1 hidden:0
...
UITEST: DUMP CRC32:4d82a19f COUNT:42 TRUNCATED:0
---UITEST_DUMP_END---
```

This is the "accessibility tree" — tests can assert on structure (bounds, state,
clickability) instead of pixels alone. The tree is copied into bounded temporary
storage while holding the LVGL lock, then printed after unlocking. Deep or large
trees emit a `WARN DUMP truncated ...` line rather than recursing indefinitely.
The host validates every NODE field, node count, CRC32 and framing, rejects
`TRUNCATED:1`, and returns a list of structured dictionaries. Incomplete trees
must not be used to assert that a widget is absent.

## Stable locators (`CLICK <id>`)

Coordinate taps are realistic but brittle to layout changes. To target a widget
by name, tag it in the firmware:

```c
#include "uitest.h"

lv_obj_t *btn = lv_button_create(parent);
uitest_register(btn, "home.wifi_btn");
```

Then from the host: `CLICK home.wifi_btn`. The harness resolves the object, reads
its live coordinates, verifies that it is visible, clickable, and on the active
screen, and enabled through its ancestor chain. LVGL's hit test must resolve its
center to that widget, including clipping and top/system-layer occlusion.
The geometry is a point-in-time check; applications must keep the widget stable
until the queued tap finishes. IDs are copied, must be unique, cannot contain
whitespace, and are removed automatically when LVGL deletes the object.

## Host runner

```bash
pip install -r tools/requirements.txt

PORT=/dev/cu.usbserial-02036BEC
python tools/uitest_runner.py --port $PORT info
python tools/uitest_runner.py --port $PORT dump
python tools/uitest_runner.py --port $PORT tap 160 120
python tools/uitest_runner.py --port $PORT swipe 300 120 20 120 250
python tools/uitest_runner.py --port $PORT click home.wifi_btn
python tools/uitest_runner.py --port $PORT --timeout 15 longpress 160 120 5000

# Run a smoke-test script (one command per line, # for comments):
python tools/uitest_runner.py --port $PORT script smoke.txt
python tools/uitest_runner.py --port $PORT script smoke.txt --continue-on-error
python tools/uitest_runner.py --port $PORT get page.title
python tools/uitest_runner.py --port $PORT shot --output capture.png
python tools/uitest_runner.py --port $PORT shot --area 0 0 160 120 --output corner.png
```

The runner defaults to 921600 baud to match the factory console; pass `--baud`
if your firmware's `CONFIG_ESP_CONSOLE_UART_BAUDRATE` differs.

The runner opens the port with DTR/RTS deasserted, then actively requests INFO
until a correlated reply proves readiness (10-second startup budget). This also
handles CP2104 adapters that reset on open or drop the boot log. Gesture replies
must match their exact arguments. SHOT uses a 120-second deadline and the sibling
screenshot component's parser/decoder; the frame and CRC must pass before the
command succeeds or a PNG/JSON sidecar is saved. Use `--screenshot-marker` for a
non-default marker. Keep both components' tools directories available.

Scripts dispatch DUMP and SHOT through their specialized parsers. They also
accept `ASSERT <id> <field> <expected>`, `WAIT <id> <field> <expected>`,
`GET <id>`, `COMPARE <reference.png>`, and `SHOT [KEY|DELTA|AREA x y w h]`.
Fields are `visible`, `enabled`,
`checked`, `value`, or `text`; quote text containing spaces. WAIT polls within
`--timeout`. COMPARE performs an exact image comparison and writes a diff on
mismatch. Scripts save per-step results/timings in `report.json`, screenshots
and sidecars under `--artifacts` (default `uitest-artifacts`). On failure the
runner attempts CANCEL and a failure screenshot; artifact errors remain in the
report and cannot turn a failed test into a pass. Later steps run only with
`--continue-on-error`.

Run host regression tests with:

```bash
cd components/core2foraws-uitest/tools
python -m unittest discover -v
```

The suite exercises fragmented UART replies, stale/unrelated responses, command
timeouts, command length checks, device errors, malformed DUMP nodes, and DUMP
CRC mismatches, truncated dumps, screenshot CRC failures, state assertions,
waits and reports. Native registry/parser and cancellation tests use the actual
C functions with minimal RTOS/LVGL stubs under ASan/UBSan; they are not a
scheduler or display-driver simulation.

## Limitations (prototype)

- Single synthetic pointer; no multitouch / pinch.
- Gesture timing is quantized upward to `UITEST_READ_PERIOD_MS`.
- `DUMP` reports geometry/flags, not text/value contents.
- The listener and the screenshot serial trigger cannot both own stdin.

## License

Apache-2.0. Copyright 2026 Rashed Talukder. See LICENSE and NOTICE for license terms and required attribution.
Generate the public API reference with `doxygen Doxyfile` from this directory.
