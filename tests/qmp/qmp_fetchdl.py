#!/usr/bin/env python3
"""qmp_fetchdl.py -- the first-byte deadline, watched failing then fixed.

    python3 tests/qmp/qmp_fetchdl.py --iso build-fetch/logit.iso \
        --disk build-fetch/disk.img --out /tmp/sf.json

WHAT THIS IS THE CONTROL FOR
=============================
browser_rt.c's bfetch has one clock per attempt, BF_REQ_MS (60 s), reset on
every hop -- right for a request that is making progress (there are long
gaps between an HTML page's own phases) and wrong for a request that has
sent its headers and received not one response byte: a server that accepts
the TCP/TLS handshake and then never writes holds one of its origin's TWO
connection slots in total silence for the full 60 s, and anything else
queued behind it waits too. BF_FIRSTBYTE_MS (12 s, browser_rt.c) is the fix:
a SEPARATE, SHORTER clock that only applies before the first response byte
arrives, and is disarmed the instant one does.

This driver serves a page with exactly two subresources sharing one origin
(hpool's cap is 2 connections per host, so both dial at once):

    stall.js  -- the server accepts the connection, reads the request, and
                 never writes a byte. The dead request this whole feature
                 exists to cut loose.
    slow5.js  -- the server writes its first byte at 5 s. Alive, just slow.
                 THE HONEST NEGATIVE: this one must NOT be cut, because 5 s
                 is well inside both BF_FIRSTBYTE_MS and BF_REQ_MS.

TWO BUILDS, ONE DRIVER
=======================
--expect-off runs against a browser built with -DBFETCH_NO_FIRSTBYTE_DEADLINE
(tests/fetchdl.mk's browser-nofirstbyte.aex): the first-byte deadline is
compiled to nothing and only BF_REQ_MS's 60 s idle deadline still applies, so
stall.js must run the FULL 60 s before the page gives up on it. Without
--expect-off (the ordinary build) it must be cut at ~12 s.

WHY THE TIMING EVIDENCE IS FROM THE GUEST, NOT THE HOST -- AND THE INSTRUMENT
THIS DRIVER FIRST REACHED FOR THAT WAS WRONG (rule 1: suspect the apparatus)
=============================================================================
Per AGENTS.md rule 5, host wall clock is not trusted for a measurement --
other agents run QEMU on this host concurrently. The first version of this
driver bracketed events with "[wm] perf t=<ms>" (the window manager's
once-a-second composite counter) and it read the negctl run's failure as
~15 s of guest time -- which would have meant the fix was broken, since that
build has BF_FIRSTBYTE_MS compiled OUT. The raw serial said otherwise:
"[wa] t=76010 (+60740) idle reqs=3 ..." -- a real 60.7 s gap, exactly
BF_REQ_MS. The [wm] line simply STOPS PRINTING while the browser holds the
netlock across a long synchronous network wait (visible in the same capture
as "[netlock] acq 7320055 recursive 29 ..." -- the WM is not getting a
timeslice), so "the nearest wm stamp before the event" silently degrades
into "the last wm stamp before the browser stopped yielding", which is not
the same clock reading at all. Trusting it would have been exactly the
"0 samples over 0 sites" shape CLAUDE.md warns about: a plausible number
under a real header, from an instrument that had quietly stopped measuring.

The instrument this driver actually uses is bfetch's OWN "[wa] t=<ms> ..."
line (browser_rt.c, unconditional, unrelated to this change -- built for the
webaccel work and reused here rather than re-invented): "nav" stamps when
the navigation is armed, "loadend" stamps when the whole load's bfetch_stats
settles, both against the SAME monotonic clock, both printed exactly once
per load with no dependency on a scheduler getting a timeslice. Elapsed
guest ms is `t_loadend - t_nav`, millisecond-exact, straight off the wire.

WHAT "GREEN" MEANS
====================
positive (ordinary build): stall.js is named in a "[browser] fetch stalled:
no response after 12 s: ...stall.js" line, its guest-clock gap from nav is
BELOW STALL_GUEST_MAX (25 s -- 12 s deadline plus boot-noise margin, still an
order of magnitude under BF_REQ_MS), slow5.js is NEVER named in that line
(it must not be cut), and "[browser] load done" appears with the page's own
SF-END stamp -- the load event actually fires.

negctl (--expect-off): stall.js is NEVER named in a "fetch stalled" line
(the deadline is compiled to nothing) and its guest-clock gap from nav is
ABOVE STALL_GUEST_MIN (45 s) -- i.e. it really did wait out BF_REQ_MS's 60 s
idle deadline instead. A run that finishes fast with the deadline compiled
out would mean the positive gate is not measuring this feature at all.
"""
import argparse
import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session       # noqa: E402

FIXDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "fixtures", "fetchdl")
PARK = (40, 780)
BOOT_BUDGET = float(os.environ.get("SF_BOOT", 300))
# The positive run needs only ~15-20 s of load; the negctl run needs to
# outlive BF_REQ_MS's 60 s idle deadline (browser_rt.c) plus engine time.
# One budget covers both -- it is a ceiling, not a measurement, so being
# generous here costs nothing.
LOAD_BUDGET = float(os.environ.get("SF_LOAD", 100))
SETTLE = float(os.environ.get("SF_SETTLE", 3))

# Guest-clock gap bounds (ms, [wm] perf t= resolution). See the module
# docstring for why these numbers and why 1 s resolution is enough for them.
STALL_GUEST_MAX_MS = 25000     # positive: must be cut well under BF_REQ_MS
STALL_GUEST_MIN_MS = 45000     # negctl: must have actually waited out ~60 s

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")


class Serve(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send_js(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/javascript")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def do_GET(self):
        path = self.path.split("?", 1)[0].lstrip("/")
        if path == "" or path == "page.html":
            body = open(os.path.join(FIXDIR, "page.html"), "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass
            return
        if path == "stall.js":
            # THE DEAD REQUEST. Accept, read the request line/headers (the
            # base handler already did that before calling do_GET), then
            # write NOTHING. Held well past every deadline under test so the
            # *client* decides when this is over, never the server -- a
            # server-side timeout here would just be a second clock hiding
            # whichever one the test actually exercises.
            time.sleep(90.0)
            return
        if path == "slow5.js":
            # ALIVE, JUST SLOW. First byte at 5 s -- inside BF_FIRSTBYTE_MS
            # (12 s) and BF_REQ_MS (60 s) alike. Must not be cut.
            time.sleep(5.0)
            self._send_js(b"var __slow5_ran = 1;\n")
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, *_a):
        pass


# ---- serial-line parsing ---------------------------------------------------

# browser_rt.c's own open-time stamps (see the module docstring for why these
# and not [wm] perf). Both are printed against the SAME monotonic clock, once
# per load, unconditionally.
WA_NAV_RE = re.compile(r"\[wa\] t=(\d+) \(\+\d+\) nav")
WA_LOADEND_RE = re.compile(r"\[wa\] t=(\d+) loadend reqs=(\d+) dials=(\d+) reuses=(\d+)")
STALLED_RE = re.compile(r"\[browser\] fetch stalled: no response after (\d+) s: (\S+)")
LOADDONE_RE = re.compile(r"\[browser\] load done: (\d+) requests, (\d+) connections "
                         r"dialled, (\d+) reused")
SFEND_RE = re.compile(r"SF-END ")


def guest_load_ms(text):
    """t_loadend - t_nav, both from browser_rt.c's [wa] stamps -- the guest's
    own monotonic clock, millisecond-exact, no scheduler dependency. Returns
    None if either stamp is missing (a harness gap, not evidence either
    way -- see the negctl branch below for what that means for the gate)."""
    m_nav = WA_NAV_RE.search(text)
    m_end = WA_LOADEND_RE.search(text)
    if not m_nav or not m_end:
        return None
    return int(m_end.group(1)) - int(m_nav.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build-fetch/logit.iso")
    ap.add_argument("--disk", default="build-fetch/disk.img")
    ap.add_argument("--out", required=True)
    ap.add_argument("--expect-off", action="store_true",
                    help="NEGATIVE CONTROL MODE: run against a browser built "
                         "-DBFETCH_NO_FIRSTBYTE_DEADLINE (tests/fetchdl.mk "
                         "builds browser-nofirstbyte.aex). stall.js must NOT "
                         "be named in a 'fetch stalled' line and must take "
                         "close to BF_REQ_MS's full 60 s -- a fast run here "
                         "means the positive gate is not measuring this "
                         "feature.")
    args = ap.parse_args()

    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            print("HARNESS FAIL: the %s (%s) is missing or empty" % (what, p))
            sys.exit(2)

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="qmp_fetchdl_")
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")

    rec = {"expect_off": args.expect_off, "gate": None,
           "started": time.strftime("%Y-%m-%dT%H:%M:%S")}

    cmd = [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "1G", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def serial():
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def wait_for(needle, secs, frm=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial()[frm:]:
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def finish(code, why):
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["serial_tail"] = serial()[-4000:]
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w") as fh:
            json.dump(rec, fh, indent=1)
        print(json.dumps({"gate": rec.get("gate"), "why": why}, indent=1))
        sys.exit(code)

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish(2, "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            finish(2, "the window manager never brought the desktop up")
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish(2, str(e))
        time.sleep(7)
        ui.goto(*PARK)

        mark = len(serial())
        ui.key_mods(["ctrl"], "l")
        time.sleep(0.3)
        target = "http://10.0.2.2:%d/page.html" % port
        ui.typ(target)
        ui.key("ret")
        if not wait_for("[browser] load: ", 20.0, mark):
            finish(2, "the address bar never took the URL")
        if not wait_for("[browser] load done", LOAD_BUDGET, mark):
            finish(2, "the page never finished ('load done' never appeared "
                      "within %.0f s)" % LOAD_BUDGET)
        time.sleep(SETTLE)
        sl = serial()[mark:]
        rec["serial_slice"] = sl[-6000:]

        stalled = [(int(m.group(1)), m.group(2)) for m in STALLED_RE.finditer(sl)]
        stalled_urls = {u for _n, u in stalled}
        loaddone = LOADDONE_RE.search(sl)
        got_end = SFEND_RE.search(sl) is not None

        stall_named = any(u.endswith("stall.js") for u in stalled_urls)
        slow5_named = any(u.endswith("slow5.js") for u in stalled_urls)
        load_ms = guest_load_ms(sl)

        rec["gate"] = {
            "stall_named_stalled": stall_named,
            "slow5_named_stalled": slow5_named,
            "load_ms": load_ms,
            "loaddone": loaddone.groups() if loaddone else None,
            "sf_end_seen": got_end,
        }

        print("stall_named=%s slow5_named=%s load_ms=%s loaddone=%s SF-END=%s"
              % (stall_named, slow5_named, load_ms,
                 loaddone.groups() if loaddone else None, got_end))

        if not got_end:
            finish(2, "the page's own SF-END never printed -- the load "
                      "event did not reach the final inline script")
        if not loaddone:
            finish(2, "no '[browser] load done' line in the load's serial slice")
        if load_ms is None:
            finish(2, "could not read both a '[wa] ... nav' and a "
                      "'[wa] ... loadend' stamp -- harness gap, not "
                      "evidence either way")
        if slow5_named:
            finish(1, "GATE RED: slow5.js (first byte at 5 s, well inside "
                      "both deadlines) was named in a 'fetch stalled' line "
                      "-- the honest-negative case failed: an alive request "
                      "was cut")

        if args.expect_off:
            if stall_named:
                finish(1, "NEGCTL RED (wrong reason): stall.js was named in "
                          "a 'fetch stalled' line with "
                          "-DBFETCH_NO_FIRSTBYTE_DEADLINE compiled in -- the "
                          "guard did not disable the feature it claims to")
            if load_ms < STALL_GUEST_MIN_MS:
                finish(1, "NEGCTL RED (wrong reason): the load finished "
                          "after only %d ms of guest time -- the positive "
                          "gate's 12 s cut would ALSO explain a load this "
                          "fast, so this does not prove BF_REQ_MS's 60 s "
                          "idle deadline is what fired" % load_ms)
            finish(0, "NEGCTL RED AS EXPECTED: stall.js was never named in a "
                      "'fetch stalled' line and the load took %d ms of guest "
                      "time (>= %d ms, t_loadend - t_nav) to finish -- "
                      "BF_REQ_MS's ordinary 60 s idle deadline is what caught "
                      "it, exactly as compiling out BF_FIRSTBYTE_MS predicts"
                      % (load_ms, STALL_GUEST_MIN_MS))

        if not stall_named:
            finish(1, "GATE RED: stall.js was never named in a 'fetch "
                      "stalled' line -- the first-byte deadline did not fire")
        if load_ms > STALL_GUEST_MAX_MS:
            finish(1, "GATE RED: the load took %d ms of guest time -- above "
                      "the %d ms bound, which means something close to "
                      "BF_REQ_MS's 60 s fired instead of BF_FIRSTBYTE_MS's "
                      "12 s" % (load_ms, STALL_GUEST_MAX_MS))
        finish(0, "GATE GREEN: load finished in %d ms of guest time "
                  "(<= %d ms, t_loadend - t_nav), stall.js named in a 'fetch "
                  "stalled' line, slow5.js NOT cut, load event fired"
                  % (load_ms, STALL_GUEST_MAX_MS))
    finally:
        try:
            proc.kill()
        except OSError:
            pass


if __name__ == "__main__":
    main()
