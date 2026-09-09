#!/usr/bin/env python3
"""Replay a SAVED page into the real browser, in the guest, from local bytes.

    python3 tests/qmp/qmp_replay.py --iso build/logit.iso --disk build/disk.img \
        --page tests/scoreboard/full-corpus/google-search.host.html \
        --name google-search --out /tmp/replay.json

WHY THIS EXISTS, AND IT IS RULE 1
=================================
`qmp_site.py` scores the LIVE web, and its own header says the corpus "changes
under you". That is fine for a scoreboard and fatal for a bug hunt. Measured on
2026-08-30, on the same commit, twelve hours apart:

  2026-08-29 21:31  google-search served 91,755 B, 5 inline scripts, and the
                    page URL-encoded a TypeError THIS ENGINE threw into its own
                    `sg_ss` telemetry parameter and navigated to report it.
  2026-08-30 11:12  google-search served 91,950 B, 5 inline scripts, no sg_ss,
                    a different exception entirely.

Nothing in the tree changed between those two runs that touches the failing
path. A live re-run therefore cannot answer "did my fix work?" -- a green second
run is indistinguishable from the site having shipped a new bundle, and that is
exactly the shape rule 1 is about. The bytes are already committed next to the
serial log (`*.host.html`, written by qmp_site.py's own host probe), so the
specimen is available and the only missing piece was a way to feed it back in.

WHAT IT IS AND IS NOT
=====================
It is `qmp_site.py`'s guest half with the network replaced by a local server:
same boot, same Dock click, same Ctrl+T/Ctrl+L keyboard navigation path, same
serial capture. It is NOT a scoreboard row and never publishes one -- an
offline replay is served from `http://10.0.2.2:<port>/`, so the page's origin,
its cookies and anything it derives from its own URL are all different from the
live load. Use it to ask "does this engine still throw X on these exact bytes",
never "does this site work".

The exit code is about the HARNESS (did the measurement happen), like
qmp_site.py: a page that throws is a result, not a failure.

SUBRESOURCES ARE REFUSED, NOT FAKED. Any path the page asks for that is not the
document itself gets a 404 and is recorded. Serving a fabricated 200 would let a
missing subresource read as a present one -- the tree's own "absent beats
present-and-wrong" rule, applied to the harness.
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

# THE DOCK SLOT COMES FROM qmp_ui, NEVER FROM A LITERAL HERE. The first draft of
# this file spelled `BROWSER_SLOT = 3` and the run came back "the Dock never
# launched the Browser" -- a harness failure that reads exactly like a broken
# guest. qmp_ui.py's own comment explains why the number is not obvious (the
# dock is centred, so every app added moves every icon) and why it is kept in
# exactly one place.
PARK = (40, 780)
BOOT_BUDGET = float(os.environ.get("REPLAY_BOOT", 300))
SELFTEST_BUDGET = float(os.environ.get("REPLAY_SELFTEST", 90))
SETTLE = float(os.environ.get("REPLAY_SETTLE", 25))

SELFTEST = ("<!doctype html><html><head><title>sb</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('RP-READY-%s');</script></body></html>")


def ctrl(ui, ch):
    ui.key_mods(["ctrl"], ch)
    time.sleep(0.3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--page", required=True, help="the saved .html document")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--serial", default=None, help="where to copy the serial log")
    # A query string reaches the page as location.search and reaches nothing
    # else: the server below routes on the PATH only. That is what lets one
    # fixture be its own negative control -- the census page and the census
    # negative control are the same bytes with the same script, which is the
    # only way a control can prove the instrument rather than a copy of it.
    ap.add_argument("--query", default="", help="query string appended to the page URL")
    args = ap.parse_args()

    with open(args.page, "rb") as fh:
        doc = fh.read()

    token = "%d" % (os.getpid() & 0xFFFF)
    selftest = (SELFTEST % token).encode()
    misses = []

    class Serve(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def _send(self, body, code=200):
            self.send_response(code)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/sb.html":
                self._send(selftest)
            elif path == "/page.html":
                self._send(doc)
            else:
                misses.append(self.path)
                self._send(b"<!doctype html>not here", 404)

        def do_POST(self):
            misses.append("POST " + self.path)
            self._send(b"", 404)

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="replay_%s_" % re.sub(r"\W+", "_", args.name))
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")

    rec = {"name": args.name, "page": os.path.abspath(args.page),
           "bytes": len(doc), "verdict": "HARNESS", "why": "did not run",
           "url": "http://10.0.2.2:%d/page.html%s" % (
               port, ("?" + args.query.lstrip("?")) if args.query else ""),
           "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "exceptions": [], "subresource_404": [], "serial_log": None}

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]

    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            with open(args.out, "w") as fh:
                json.dump(rec, fh, indent=1)
            print(json.dumps({"name": args.name, "verdict": "HARNESS",
                              "why": rec["why"]}))
            return 0

    qlog = open(os.path.join(tmp, "qemu.log"), "wb")
    proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)

    def serial(frm=0):
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")[frm:]
        except OSError:
            return ""

    def wait_for(needle, secs, frm=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial(frm):
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def finish(verdict, why):
        rec["verdict"] = verdict
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["subresource_404"] = misses
        dst = args.serial or os.path.join(os.path.dirname(os.path.abspath(args.out)),
                                          "%s.replay.serial.txt" % args.name)
        try:
            with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(serial())
            rec["serial_log"] = dst
        except OSError:
            pass
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1, ensure_ascii=False)
        print(json.dumps({"name": args.name, "verdict": verdict, "why": why,
                          "exceptions": rec["exceptions"]},
                         indent=1, ensure_ascii=False))
        sys.exit(0)

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            finish("HARNESS", "the window manager never brought the desktop up")
        time.sleep(3)

# One click, at the tile the GUEST names for browser.aex, verified against the
        # guest's own [wm] launched line -- the re-click loop this replaces was
        # the apology a hand-kept slot constant needed. See qmp_ui's dock block.
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish("HARNESS", str(e))
        time.sleep(7)

        mark = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("RP-READY-" + token, SELFTEST_BUDGET, mark):
            finish("HARNESS", "the self-test page never loaded -- nothing was "
                              "measured about the specimen")

        ctrl(ui, "t")
        time.sleep(1.5)
        ui.goto(*PARK)

        mark = len(serial())
        ctrl(ui, "l")
        ui.typ(rec["url"])
        ui.key("ret")
        if not wait_for("[browser] load: ", 20.0, mark):
            finish("HARNESS", "the address bar never took the URL")
        if not wait_for("[browser] load done", 90.0, mark):
            finish("HARNESS", "the page never finished loading")
        time.sleep(SETTLE)

        log = serial(mark)
        seen = []
        for ln in log.splitlines():
            for tag in ("[browser] JS exception:", "[js] uncaught in event listener:",
                        "[js] uncaught in timer:", "[error]"):
                i = ln.find(tag)
                if i >= 0:
                    msg = ln[i:].strip()
                    if msg not in seen:
                        seen.append(msg)
        rec["exceptions"] = seen
        finish("REPLAYED", "%d distinct exception line(s)" % len(seen))
    except Exception as e:                                   # noqa: BLE001
        finish("HARNESS", "the harness itself raised: %r" % (e,))
    return 0


if __name__ == "__main__":
    sys.exit(main())
