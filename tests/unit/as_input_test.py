#!/usr/bin/env python3
"""Verify original A3 input tools with distinct ABI fields and real guest input.

Host probes replace only the kernel transport. Guest validation sends ordinary
PS/2 actions, waits for application-observed events and checks its real exit.
This is bounded interaction coverage, not the separate queue saturation gate.
"""

import argparse
import json
from pathlib import Path
import re
import shlex
import tempfile
import time

from PIL import Image
from as_abi_test import probe_runtime
from as_managed_test import ROOT, emit_ir, sanitized
from as_viewer_test import color_mask

EXAMPLES = ROOT / "fsroot/as/examples"
PROBE_HEADERS = '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
'''
EVENT = re.compile(r"^EV (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\r?$", re.M)
DONE = re.compile(r"^EVENTS-DONE (\d+)\r?$", re.M)
STATS = (
    "Events 31 queued, 7 merged, 2 evicted-motion, 0 dropped\n"
    "Raw-input 37 queued, 5 merged, 3 evicted-motion, 0 dropped-motion, "
    "0 dropped-semantic, hwm 11, backlog-batches 4\n"
)
# These are the documented stable event identities, independent of the source
# program's rendering/formatting. Distinct fields expose accidental swaps.
HOST_EVENTS = [(7, 53, 29, 0, 0, 0), (2, 54, 30, 1, 1, 0),
               (6, 55, 31, 1, 1, 0), (4, 56, 32, 4, 2, 0),
               (8, 57, 33, 2, 0, -1), (1, 88, 0, 1, 0, 0)]


def parse_events(text, complete=False):
    events = [tuple(map(int, match.groups())) for match in EVENT.finditer(text)]
    if complete:
        summary = list(DONE.finditer(text))
        if len(summary) != 1 or int(summary[0].group(1)) != len(events):
            raise AssertionError("event records do not match the application's complete count")
        if text.count("EVENTS-READY") != 1:
            raise AssertionError("missing or repeated input application readiness")
    return events


def check_stats(text):
    event = re.findall(r"^Events (\d+) queued, (\d+) merged, (\d+) evicted-motion, (\d+) dropped\r?$",
                       text, re.M)
    raw = re.findall(r"^Raw-input (\d+) queued, (\d+) merged, (\d+) evicted-motion, "
                     r"(\d+) dropped-motion, (\d+) dropped-semantic, hwm (\d+), "
                     r"backlog-batches (\d+)\r?$", text, re.M)
    if len(event) != 1 or len(raw) != 1:
        raise AssertionError("sysinfo lacks complete, unambiguous event and raw-input records")
    return {"events": dict(zip(("queued", "merged", "evicted", "dropped"), map(int, event[0]))),
            "raw": dict(zip(("queued", "merged", "evicted", "dropped_motion", "dropped_semantic",
                             "high_watermark", "backlog_batches"), map(int, raw[0])))}


def host_output(events=HOST_EVENTS):
    return ("EVENTS-READY\n" + "".join("EV " + " ".join(map(str, row)) + "\n" for row in events)
            + f"EVENTS-DONE {len(events)}\n")


def input_probe():
    rows = ",\n".join("{" + ",".join(map(str, row)) + "}" for row in HOST_EVENTS)
    return PROBE_HEADERS + '''
int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    static const int fields[][6] = {''' + rows + '''};
    static unsigned position;
    switch (number) {
    case SYS_GUI_CREATE:
        assert(!strcmp((const char *)(uintptr_t)a, "Events"));
        assert(b == ((uint64_t)900 << 16 | 600));
        return 0;
    case SYS_GUI_CLEAR:
        assert(a == 0x1E1E28);
        return 0;
    case SYS_GUI_TEXT:
    case SYS_GUI_FLUSH:
    case SYS_YIELD:
        return 0;
    case SYS_POLL_EVENT: {
        struct logit_event *event = (void *)(uintptr_t)a;
        memset(event, 0, sizeof *event);
        if (position < sizeof fields / sizeof fields[0]) {
            const int *row = fields[position++];
            event->type = row[0];
            event->a = row[1];
            event->b = row[2];
            event->mods = row[3];
            event->button = row[4];
            event->wheel = row[5];
            return 1;
        }
        if (position++ == sizeof fields / sizeof fields[0]) {
            event->type = EV_KEY;
            event->a = 'q';
            return 1;
        }
        return 0;
    }
    default:
        assert(!"unexpected input tool syscall");
        return -1;
    }
}
'''


def host_checks(compiler, negative=False):
    with tempfile.TemporaryDirectory(prefix="as-native-input-") as temporary:
        work = Path(temporary)
        runtime = probe_runtime(work, input_probe())
        ir = work / "events.ll"
        emit_ir(compiler, EXAMPLES / "events.as", ir)
        for mode in ("-O0", "-O2"):
            result = sanitized(ir, runtime, work / "events", mode)
            assert result.returncode == 0 and result.stdout == host_output(), result
            assert parse_events(result.stdout, complete=True) == HOST_EVENTS
        if negative:
            source = work / "events.as"
            original = (EXAMPLES / "events.as").read_text()
            anchor = 'print("EV", ev.type, ev.a, ev.b, ev.mods, ev.button, ev.wheel)'
            assert original.count(anchor) == 1
            source.write_text(original.replace(anchor,
                'print("EV", ev.type, ev.a, ev.b, ev.button, ev.mods, ev.wheel)'))
            emit_ir(compiler, source, ir)
            result = sanitized(ir, runtime, work / "swapped-fields", "-O0")
            assert result.returncode == 0 and result.stdout.endswith("EVENTS-DONE 6\n"), result
            assert parse_events(result.stdout, complete=True) != HOST_EVENTS, result
            print("PASS input control: swapped modifier/button fields rejected despite success marker")

        # Only the result count changes in invalid variants; the probe never
        # writes outside the supplied buffer. The production tool must reject
        # the count before constructing a raw text view over it.
        text_literal = json.dumps(STATS)
        transport = PROBE_HEADERS + '''
int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    const char *text = TEXT_LITERAL;
    assert(number == SYS_SYSINFO && b == 4096 && c == 0);
    memcpy((void *)(uintptr_t)a, text, strlen(text));
    return RESULT_COUNT;
}
'''.replace("TEXT_LITERAL", text_literal)
        emit_ir(compiler, EXAMPLES / "evqstat.as", work / "stats.ll")
        for label, count in (("normal", "strlen(text)"), ("error", "-1"), ("oversized", "4097")):
            directory = work / label
            directory.mkdir()
            runtime = probe_runtime(directory, transport.replace("RESULT_COUNT", count))
            for mode in ("-O0", "-O2"):
                result = sanitized(work / "stats.ll", runtime, directory / "stats", mode)
                if label == "normal":
                    assert result.returncode == 0 and result.stdout == STATS + "\n", result
                    assert check_stats(result.stdout)["raw"]["high_watermark"] == 11
                else:
                    assert result.returncode == 1 and "IOError" in result.stderr, result
        if negative:
            for bad in (STATS + STATS, STATS.replace("evicted-motion,", "other-field,"), "EVENTS-DONE 6\n"):
                try:
                    check_stats(bad)
                except AssertionError:
                    pass
                else:
                    raise AssertionError("incomplete/ambiguous sysinfo report passed")
            print("PASS statistics controls: incomplete/duplicate reports rejected")
            # A regex that silently drops a torn serial line could still see
            # the final success marker. The application's own count closes
            # that hole without retrying (and duplicating) real input events.
            complete = host_output()
            first_record = "EV " + " ".join(map(str, HOST_EVENTS[0])) + "\n"
            for bad in (complete.replace(first_record, "EV torn\n"),
                        complete + "EVENTS-DONE 6\n",
                        complete.replace("EVENTS-READY\n", "")):
                try:
                    parse_events(bad, complete=True)
                except AssertionError:
                    pass
                else:
                    raise AssertionError("incomplete event transcript passed")
            print("PASS event controls: torn records, duplicate summaries and missing readiness rejected")
    print("PASS native input tools: exact event fields, complete counters, I/O lengths, O0/O2")


def events_frame(frame):
    bounds = color_mask(frame, (30, 30, 40)).getbbox()
    # The compositor's titlebar seam covers canvas row zero. The remaining
    # 599 rows must be visible, and the origin used for input is one row above
    # this exact background rectangle. Do not infer it from screen center.
    if not bounds or (bounds[2] - bounds[0], bounds[3] - bounds[1]) != (900, 599):
        raise AssertionError("Events canvas not visible")
    bounds = (bounds[0], bounds[1] - 1, bounds[2], bounds[3])
    text = frame.crop((bounds[0] + 16, bounds[1] + 16, bounds[0] + 600, bounds[1] + 45))
    if color_mask(text, (154, 160, 176)).histogram()[255] < 30:
        raise AssertionError("Events window has no visible explanatory text")
    return bounds


def read_guest_stats(guest, executable):
    output = guest.capture("/bin/native-capture " + shlex.quote(executable), timeout=30)
    assert guest.last_capture_exit == 0, output
    return check_stats(output)


def run_guest_input(guest, executable, program, output_directory, statistics):
    before = read_guest_stats(guest, statistics)
    start = len(guest.log)
    status = "/state/" + program["name"] + "-exit.txt"
    # Keep stdout on the serial device for acknowledgments during interaction.
    # At completion require every printed event to match the app's own count;
    # torn/interleaved serial records must fail rather than disappear silently.
    guest.serial.sendall((shlex.quote(executable) + "\n/bin/echo $? > " + status + "\n").encode())
    guest.wait(b"EVENTS-READY", 60, start)
    deadline = time.monotonic() + 40
    while True:
        guest.screenshot()
        with Image.open(output_directory / "desktop.ppm") as image:
            frame = image.convert("RGB")
        try:
            bounds = events_frame(frame)
            break
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)
    screenshot = output_directory / (program["name"] + ".png")
    frame.save(screenshot)

    def records():
        return parse_events(bytes(guest.log[start:]).decode("utf-8", errors="replace"))

    def send(actions, predicate, label):
        previous = len(records())
        guest.qmp("input-send-event", {"events": actions})
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            delivered = records()[previous:]
            if any(predicate(row) for row in delivered):
                return
            time.sleep(0.05)
        raise AssertionError(f"Missing guest-observed {label}: {records()[previous:]}")

    def button(name, down):
        return {"type": "btn", "data": {"button": name, "down": down}}

    def key(name, down):
        return {"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": down}}

    # Earlier GUI cases may have left the pointer over a title bar. Move to
    # this canvas before pressing buttons, and track the same screen position
    # that the shared guest driver uses for later interactions.
    center = [(bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2]
    displacement = [center[axis] - guest.pointer[axis] for axis in (0, 1)]
    send([{"type": "rel", "data": {"axis": "x", "value": displacement[0]}},
          {"type": "rel", "data": {"axis": "y", "value": displacement[1]}}],
         lambda row: row[0] == 7, "pointer placement")
    guest.pointer[:] = center

    for dx, dy in ((4, 3), (-4, -3)):
        send([{"type": "rel", "data": {"axis": "x", "value": dx}},
              {"type": "rel", "data": {"axis": "y", "value": dy}}],
             lambda row: row[0] == 7, "pointer motion")
    for name, identity, press_type in (("left", 1, 2), ("right", 2, 4)):
        send([button(name, True)], lambda row: row[0] == press_type and row[4] == identity and row[3] == 0,
             name + " press")
        send([button(name, False)], lambda row: row[0] == 6 and row[4] == identity, name + " release")
    for name, direction in (("wheel-down", 1), ("wheel-up", -1)):
        send([button(name, True), button(name, False)],
             lambda row: row[0] == 8 and row[5] * direction > 0, name)
    # The keyboard event proves the modifier reached the guest before the
    # cross-device click. QMP transport completion alone cannot establish that.
    send([key("shift", True), key("x", True), key("x", False)],
         lambda row: row[0] == 1 and row[3] & 1, "shifted key")
    send([button("left", True)], lambda row: row[0] == 2 and row[4] == 1 and row[3] & 1, "shifted press")
    send([button("left", False)], lambda row: row[0] == 6 and row[4] == 1 and row[3] & 1, "shifted release")
    send([key("shift", False), key("x", True), key("x", False)],
         lambda row: row[0] == 1 and row[1] == 120 and row[3] == 0, "modifier release")
    guest.key("q")
    guest.wait(b"EVENTS-DONE", 30, start)
    code = guest.capture("/bin/cat " + status, timeout=30)
    assert code.strip() == "0", code
    text = bytes(guest.log[start:]).decode("utf-8", errors="replace")
    delivered = parse_events(text, complete=True)
    for row in delivered:
        if row[0] in (2, 4, 6, 7, 8):
            assert 0 <= row[1] < 900 and 0 <= row[2] < 600, row
    # The restored pointer remains at the canvas center. Unlike merely
    # checking the 900x600 bounds, this rejects raw screen coordinates that
    # happen to fit inside that large window as well.
    click = next(row for row in delivered if row[0] == 2 and row[3] == 0)
    assert abs(click[1] - (guest.pointer[0] - bounds[0])) <= 2, (click, bounds, guest.pointer)
    assert abs(click[2] - (guest.pointer[1] - bounds[1])) <= 2, (click, bounds, guest.pointer)
    program.update(exit_code=0, input_events=delivered, screenshot=str(screenshot), output=text)
    after = read_guest_stats(guest, statistics)
    for queue in ("events", "raw"):
        assert after[queue]["queued"] > before[queue]["queued"], (before, after)
    # Ordinary, acknowledged input must not overflow either semantic queue.
    # Motion saturation remains covered by the separate historical input gate.
    assert after["events"]["dropped"] == before["events"]["dropped"], (before, after)
    assert after["raw"]["dropped_semantic"] == before["raw"]["dropped_semantic"], (before, after)
    program["queue_counters"] = {"before": before, "after": after}
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    arguments = parser.parse_args()
    host_checks(arguments.compiler.resolve(), arguments.negative_control)
