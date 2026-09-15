#!/usr/bin/env python3
"""Guest gate for keeping compositor work outside the WM state mutex.

The workload is deliberately ordinary: launch the real Terminal, then run six
real /bin/true processes.  The control rebuild holds wm_lock across each pixel
pass; the fixed build releases it while graphics_lock freezes the exact state
being rendered.  `render_locked` is the reproducible causal count and is what
the gate asserts.  Lock maxima are printed and saved, never thresholded: shared
host TCG made the same source range from 69 to 486 ms before this gate existed.
"""

import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qmp_compositor_lock import StdioSession, boot, counter_pairs, mark, serial_text  # noqa: E402


def lock_rows(text):
    rows = []
    for line in text.splitlines():
        if "[wm] locks " not in line:
            continue
        raw = line[line.index("[wm] locks "):]
        vals = {key: int(value) for key, value in
                re.findall(r"(calls|hold_ns|max_ns|render_locked)=(\d+)", raw)}
        if len(vals) == 4:
            rows.append((vals, raw))
    return rows


def tag_snapshot(text):
    out = {}
    for line in text.splitlines():
        if "[wm] locktag " not in line:
            continue
        raw = line[line.index("[wm] locktag "):]
        fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", raw))
        numeric = ("calls", "wall_ns", "cpu_ns", "max_wall_ns",
                   "max_cpu_ns", "redispatches")
        # Serial output shares the tty with the shell.  An interrupted line is
        # not a zero-valued sample; ignore it and retain the last complete one.
        if "num" not in fields or "tag" not in fields or any(
                name not in fields for name in numeric):
            continue
        key = (fields["tag"], int(fields["num"]))
        out[key] = {name: int(fields[name]) for name in numeric}
    return out


def top_snapshot(text):
    lines = [line[line.index("[wm] locktop "):]
             for line in text.splitlines() if "[wm] locktop " in line]
    starts = [i for i, line in enumerate(lines) if "rank=1 " in line]
    return lines[starts[-1]:starts[-1] + 6] if starts else []


def snap_after_mark(ui, serial, old_count=None):
    old_top_count = sum("[wm] locktop rank=1 " in line
                        for line in serial_text(serial).splitlines())
    mark(ui, serial, old_count=old_count, timeout=8.0)
    # mark() returns as soon as the paired locks line lands.  The tag/top dump is
    # intentionally printed just after unlocking, so wait for rank 6 from that
    # same report rather than racing the serial writer.
    deadline = time.time() + 3
    while time.time() < deadline:
        text = serial_text(serial)
        new_top_count = sum("[wm] locktop rank=1 " in line
                            for line in text.splitlines())
        if new_top_count > old_top_count and top_snapshot(text) and tag_snapshot(text):
            time.sleep(0.1)
            text = serial_text(serial)
            return lock_rows(text)[-1], tag_snapshot(text), top_snapshot(text)
        time.sleep(0.05)
    raise RuntimeError("guest produced no complete WM-lock attribution snapshot")


def delta_tags(before, after):
    out = []
    for key in sorted(set(before) | set(after), key=lambda k: k[1]):
        b = before.get(key, {})
        a = after.get(key, {})
        d = {name: a.get(name, 0) - b.get(name, 0)
             for name in ("calls", "wall_ns", "cpu_ns", "redispatches")}
        d["max_wall_ns"] = a.get("max_wall_ns", 0)
        d["max_cpu_ns"] = a.get("max_cpu_ns", 0)
        if d["calls"] or d["wall_ns"]:
            out.append((key, d))
    return sorted(out, key=lambda item: item[1]["wall_ns"], reverse=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("control", "fixed"), required=True)
    ap.add_argument("--commands", type=int, default=6)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--json")
    ap.add_argument("--control-json")
    args = ap.parse_args()

    from qmp_ui import configure
    configure(1920, 1200)
    import tempfile
    tmp = tempfile.mkdtemp(prefix="logit-wm-state-lock-")
    serial = os.path.join(tmp, "serial.log")
    proc = boot(args.iso, args.disk, serial)
    try:
        ui = StdioSession(proc, serial)
        deadline = time.time() + 30
        while "[wm] launched Finder" not in serial_text(serial):
            if time.time() >= deadline:
                raise RuntimeError("boot Finder never reached its launch line")
            time.sleep(0.1)
        ui.launch_app("terminal")
        time.sleep(8.0)
        before_pair_count = len(counter_pairs(serial_text(serial)))
        before, before_tags, _ = snap_after_mark(ui, serial, before_pair_count)
        for _ in range(args.commands):
            ui.typ("true\n")
            time.sleep(0.35)
        after_pair_count = len(counter_pairs(serial_text(serial)))
        after, after_tags, tops = snap_after_mark(ui, serial, after_pair_count)
    finally:
        proc.kill()
        proc.wait()

    bv, bline = before
    av, aline = after
    pairs = counter_pairs(serial_text(serial))
    # Match the same two lock reports to their immediately preceding perf rows.
    perf_before = next(row for row in reversed(pairs) if row["locks_line"] == bline)
    perf_after = next(row for row in reversed(pairs) if row["locks_line"] == aline)
    composites = perf_after["perf"].get("composites", 0) - perf_before["perf"].get("composites", 0)
    calls = av["calls"] - bv["calls"]
    hold_ns = av["hold_ns"] - bv["hold_ns"]
    mean_ns = hold_ns // calls if calls else 0
    render_locked = av["render_locked"] - bv["render_locked"]
    tags = delta_tags(before_tags, after_tags)

    print("BEFORE " + perf_before["perf_line"])
    print("BEFORE " + bline)
    print("AFTER  " + perf_after["perf_line"])
    print("AFTER  " + aline)
    print("RESULT mode=%s composites=%d calls=%d hold_ns=%d mean_hold_ns=%d "
          "max_hold_ns=%d render_locked=%d" %
          (args.mode, composites, calls, hold_ns, mean_ns, av["max_ns"], render_locked))
    for (name, num), d in tags[:8]:
        print("TAG tag=%s num=%d calls=%d wall_ns=%d cpu_ns=%d "
              "max_wall_ns=%d max_cpu_ns=%d redispatches=%d" %
              (name, num, d["calls"], d["wall_ns"], d["cpu_ns"],
               d["max_wall_ns"], d["max_cpu_ns"], d["redispatches"]))
    for line in tops:
        print("TOP " + line)
    print("NOTE tail max is recorded, not asserted; the gate asserts the reproducible "
          "render-under-state-lock cause/count.")

    ok = composites > 0 and calls > 0
    if args.mode == "control":
        ok = ok and render_locked == composites
        verdict = "old boundary reproduced: %d/%d composites held wm_lock" % (render_locked, composites)
    else:
        control_ok = False
        if args.control_json:
            with open(args.control_json) as fh:
                ctl = json.load(fh)
            control_ok = ctl.get("passed") and ctl.get("render_locked") == ctl.get("composites")
        ok = ok and render_locked == 0 and control_ok
        verdict = ("all %d composites ran with wm_lock released" % composites
                   if render_locked == 0 else
                   "%d/%d composites still held wm_lock" %
                   (render_locked, composites))
    print(("PASS " if ok else "FAIL ") + args.mode + ": " + verdict)

    result = {"mode": args.mode, "composites": composites, "calls": calls,
              "hold_ns": hold_ns, "mean_hold_ns": mean_ns,
              "max_hold_ns": av["max_ns"], "render_locked": render_locked,
              "tags": [{"tag": key[0], "num": key[1], **values}
                       for key, values in tags],
              "tops": tops, "before_perf": perf_before["perf_line"],
              "before_locks": bline, "after_perf": perf_after["perf_line"],
              "after_locks": aline, "serial": serial, "passed": bool(ok)}
    if args.json:
        os.makedirs(os.path.dirname(args.json) or ".", exist_ok=True)
        with open(args.json, "w") as fh:
            json.dump(result, fh, indent=1)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
