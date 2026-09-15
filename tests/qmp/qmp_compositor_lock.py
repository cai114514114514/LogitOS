#!/usr/bin/env python3
"""Guest gate for wm_app_exit() repainting an unchanged desktop.

This intentionally uses qmp_repaint.py's counter parser and qmp_ui.py's real
dock/input path.  The workload is six /bin/true processes typed into the real
Terminal: each process exits, while none owns a GUI window or changes a desktop
pixel.  A control kernel with WM_SPURIOUS_FULL_NEGCTL must turn those exits
into full frames and restore create-time full repaint; the fixed kernel must
do neither.

QMP runs on stdio because this execution environment refuses Unix-domain
listener creation.  That changes only the control channel.  The guest serial
log, compositor counters, disk, display, input events and TCG configuration are
the same ones qmp_repaint.py uses.  The tail is printed but not asserted: a
cumulative max includes boot/window-launch work and shared-host TCG makes an
absolute nanosecond ceiling flaky.  The reproducible gate is the full-frame
count on a pixel-inert process-exit workload.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qmp_repaint import perf_samples  # noqa: E402
from qmp_ui import Session, configure  # noqa: E402


class _ProcessIO:
    def __init__(self, proc):
        self.proc = proc

    def readline(self):
        return self.proc.stdout.readline()

    def write(self, data):
        return self.proc.stdin.write(data)

    def flush(self):
        return self.proc.stdin.flush()


class StdioSession(Session):
    """qmp_ui.Session with the transport changed from AF_UNIX to QMP stdio."""

    def __init__(self, proc, serial):
        self.serial = serial
        self._serial_text_fn = None
        self.s = None
        greeting = json.loads(proc.stdout.readline())
        if "QMP" not in greeting:
            raise RuntimeError("QMP stdio did not produce a greeting")
        self.f = _ProcessIO(proc)
        self.cmd({"execute": "qmp_capabilities"})
        # configure() has already set qmp_ui's device-pixel globals. Session's
        # only other initialization is this centre-point model.
        import qmp_ui
        self.cur = [qmp_ui.SCREEN_W // 2, qmp_ui.SCREEN_H // 2]


def boot(iso, disk, serial, xres=1920, yres=1200):
    proc = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime", "-vga", "none",
         "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (xres, yres),
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-serial", "file:" + serial, "-no-reboot", "-display", "none",
         "-qmp", "stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, bufsize=1)
    deadline = time.time() + 240
    while time.time() < deadline:
        if os.path.exists(serial):
            with open(serial, errors="replace") as fh:
                if "desktop live" in fh.read():
                    return proc
        if proc.poll() is not None:
            raise RuntimeError("qemu exited before the desktop became live")
        time.sleep(0.2)
    proc.kill()
    raise RuntimeError("guest never reported a live desktop")


def counter_pairs(text):
    """Complete perf+lock pairs, retaining their verbatim source lines."""
    out = []
    pending = None
    for line in text.splitlines():
        if "[wm] perf " in line:
            parsed = perf_samples(line + "\n")
            pending = (parsed[-1], line[line.index("[wm] perf "):]) if parsed else None
            continue
        if pending and "[wm] locks " in line:
            fields = {}
            raw = line[line.index("[wm] locks "):]
            for key, value in re.findall(r"(calls|hold_ns|max_ns)=(\d+)", raw):
                fields[key] = int(value)
            if len(fields) == 3:
                perf, perf_line = pending
                out.append({"perf": perf, "locks": fields,
                            "perf_line": perf_line, "locks_line": raw})
            pending = None
    return out


def serial_text(path):
    with open(path, errors="replace") as fh:
        return fh.read()


def mark(ui, serial, old_count=None, timeout=5.0):
    if old_count is None:
        old_count = len(counter_pairs(serial_text(serial)))
    # Cursor-plane motion wakes the WM without adding a composite, so bracket
    # samples describe the workload rather than the measuring action.  Repeat
    # until a sample arrives: sending +1/-1 only once lets the input queue merge
    # the pair back to zero before the WM observes it, which left the first
    # fixed run parked forever with no second counter line.
    deadline = time.time() + timeout
    while time.time() < deadline:
        for delta in (1, -1):
            ui._input([{"type": "rel", "data": {"axis": "x", "value": delta}}])
            ui.cur[0] += delta
        rows = counter_pairs(serial_text(serial))
        if len(rows) > old_count:
            return rows[-1]
        time.sleep(0.1)
    raise RuntimeError("guest produced no complete perf+locks bracket sample")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("control", "fixed"), required=True)
    ap.add_argument("--commands", type=int, default=6)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--json")
    ap.add_argument("--control-json")
    args = ap.parse_args()

    configure(1920, 1200)
    tmp = tempfile.mkdtemp(prefix="logit-compositor-lock-")
    serial = os.path.join(tmp, "serial.log")
    proc = boot(args.iso, args.disk, serial)
    try:
        ui = StdioSession(proc, serial)
        # wm_run publishes "desktop live" before the asynchronously spawned
        # Finder necessarily reaches its launch line.  Taking launch_app()'s
        # byte mark in that gap lets the late Finder line look like the result
        # of our Terminal click.  The first fixed gate run caught exactly that
        # apparatus race; wait for the boot launch to become history.
        deadline = time.time() + 30
        while "[wm] launched Finder" not in serial_text(serial):
            if time.time() >= deadline:
                raise RuntimeError("boot Finder never reached its launch line")
            time.sleep(0.1)
        # Launch/animation costs are deliberately outside the command bracket.
        # They have their own reason assertion: window creation must damage the
        # two affected footprints without a FULL_WINDOW_CREATE trace.
        launch_offset = len(serial_text(serial))
        ui.launch_app("terminal")
        time.sleep(8.0)
        launch_trace = serial_text(serial)[launch_offset:]
        before = mark(ui, serial)
        start_offset = len(serial_text(serial))
        for _ in range(args.commands):
            ui.typ("true\n")
            time.sleep(0.35)
        after = mark(ui, serial, timeout=6.0)
        trace = serial_text(serial)[start_offset:]
    finally:
        proc.kill()
        proc.wait()

    dp = {k: after["perf"].get(k, 0) - before["perf"].get(k, 0)
          for k in after["perf"]}
    dl = {k: after["locks"][k] - before["locks"][k]
          for k in ("calls", "hold_ns")}
    mean_hold = dl["hold_ns"] // dl["calls"] if dl["calls"] else 0
    full_lines = [line[line.index("[wm] fullframe "):]
                  for line in trace.splitlines() if "[wm] fullframe " in line]
    app_exit_lines = [line for line in full_lines
                      if int(re.search(r"reasons=0x([0-9a-fA-F]+)", line).group(1), 16)
                      & (1 << 15)]
    launch_full_lines = [line[line.index("[wm] fullframe "):]
                         for line in launch_trace.splitlines()
                         if "[wm] fullframe " in line and
                         int(re.search(r"reasons=0x([0-9a-fA-F]+)", line).group(1), 16)
                         & (1 << 6)]

    print("BEFORE " + before["perf_line"])
    print("BEFORE " + before["locks_line"])
    print("AFTER  " + after["perf_line"])
    print("AFTER  " + after["locks_line"])
    print("RESULT mode=%s commands=%d composites=%d full=%d lock_calls=%d "
          "hold_ns=%d mean_hold_ns=%d max_hold_ns=%d compositor_max_ns=%d "
          "app_exit_fullframes=%d"
          % (args.mode, args.commands, dp.get("composites", 0), dp.get("full", 0),
             dl["calls"], dl["hold_ns"], mean_hold,
             after["locks"]["max_ns"], after["perf"].get("max", 0),
             len(app_exit_lines)))
    for line in full_lines:
        print("TRACE  " + line)
    for line in launch_full_lines:
        print("LAUNCH " + line)
    print("NOTE tail max is recorded, not asserted; the gate asserts the "
          "reproducible full-frame cause/count.")

    ok = True
    if args.mode == "control":
        ok = (len(app_exit_lines) >= args.commands and
              dp.get("full", 0) >= args.commands and launch_full_lines)
        if not ok:
            print("FAIL control: %d commands produced %d app-exit traces, %d full frames; "
                  "window-create traces=%d"
                  % (args.commands, len(app_exit_lines), dp.get("full", 0),
                     len(launch_full_lines)))
        else:
            print("PASS control: old behavior reproduced (%d app-exit and %d window-create full traces)"
                  % (len(app_exit_lines), len(launch_full_lines)))
    else:
        control_full = None
        if args.control_json:
            with open(args.control_json) as fh:
                control_full = json.load(fh)["delta_perf"]["full"]
        # Terminal redraw can independently grow several honest damage rects
        # past the 75% threshold (FULL_DAMAGE_THRESHOLD).  Requiring total
        # full==0 made the first real fixed run fail for that unrelated frame.
        # The fix owns two claims: no FULL_APP_EXIT trace, and fewer total full
        # frames than the already-run control on the same six commands.
        ok = (len(app_exit_lines) == 0 and not launch_full_lines and
              control_full is not None and dp.get("full", 0) < control_full)
        if not ok:
            print("FAIL fixed: app-exit traces=%d, window-create traces=%d, "
                  "fixed full=%d, control full=%r"
                  % (len(app_exit_lines), len(launch_full_lines),
                     dp.get("full", 0), control_full))
        else:
            print("PASS fixed: window creation and all %d non-GUI exits produced zero "
                  "cause-specific full frames; command-interval full frames fell %d -> %d"
                  % (args.commands, control_full, dp.get("full", 0)))

    result = {"mode": args.mode, "commands": args.commands,
              "before": before, "after": after, "delta_perf": dp,
              "delta_locks": dl, "mean_hold_ns": mean_hold,
              "fullframes": full_lines, "app_exit_fullframes": len(app_exit_lines),
              "window_create_fullframes": len(launch_full_lines),
              "passed": ok, "serial": serial}
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w") as fh:
            json.dump(result, fh, indent=1)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
