#!/usr/bin/env python3
"""Does an idle page's first click, and a nested dispatch inside a handler,
survive the CPU-slice watchdog?  ON THE DEVICE.

    python3 tests/qmp/qmp_jsslice.py --iso build/logit.iso --disk build/disk.img
    python3 tests/qmp/qmp_jsslice.py --iso build/logit.iso --disk build/disk-noslice.img --label noslice

WHY THIS EXISTS
===============
The owner opened chat.deepseek.com, the UI rendered, the page was inert, and the
console said "[watchdog] script exceeded its CPU slice -- interrupted".  The
budget is 45 SECONDS of wall clock and 2,000,000 interrupt calls, and js_page.c
calls the second one "astronomically above legit".  So the interesting question
was never the budget: a page that burns 45 s has a problem the budget merely
reports.

The ROOT CAUSE, measured on the device serial log before any fix existed:
js_dom_dispatch() -- every click, keydown, load, scroll -- brackets NOTHING, so
an event handler inherits whatever deadline the last <script> or timer callback
left armed.  A chat UI that finishes loading and then sits waiting for input
never re-arms, and its first click after 45 s is killed having run at most
10,000 branches:

    [js] watchdog: script exceeded its CPU slice (wall time) -- interrupted
    [js] watchdog: fuel=13 (=130000 branches+calls) since_begin_ms=55070

fuel=13 of a 2,000,000 budget -- 0.00065%.  The 45 s were not spent running
script.  THE FIX (c/apps/browser/js_dom.c, js_dom_dispatch): bracket every
OUTERMOST dispatch with js_page_slice_begin()/js_page_slice_end(), gated by a
nesting-depth counter so a handler that triggers a synchronous nested dispatch
(window.scrollTo() inside a click handler, the concrete case in the wild) still
gets exactly one slice for the whole call tree rather than one per nesting
level.  THIS FILE HAS TWO JOBS NOW, one per build it is pointed at:

  against the FIXED browser (browser.aex)   -- prove the bug is gone: three
      clicks, the middle one 52+ s after load, ALL run to completion with no
      watchdog line, because js_dom_dispatch now arms a fresh 45 s window for
      every outermost dispatch instead of inheriting page-load's.

  against the CONTROL browser (browser-noslice.aex, built with
      -DJS_DOM_NO_SLICE_BRACKET -- see the #ifndef in js_dom.c) -- prove the
      bug SHAPE is still exactly what the owner saw: this build has the guard
      macro remove BOTH the begin() and the end() call, so js_dom_dispatch is
      byte-for-byte back to bracketing nothing.  Its click 2 must be watchdog-
      bitten, on the wall-time rail, at roughly the fuel=13 the field report
      measured (the fixture's constants below reproduce that order of
      magnitude, not the exact number -- TCG timing is not the same run to
      run, see AGENTS.md).

tests/unit/webapi_probe.c --prof-selftest reproduces the mechanism on the HOST
against a clock that advances 1 ms per read.  This file is the device half, and
it exists because a host reproduction with a fake clock is an argument, not a
measurement: on the device the clock is a real syscall, the interpreter is
x86_64 under TCG on an arm64 host, and nothing about the timing is the same.

WHAT IT DRIVES, AND WHY THE ANSWER IS KNOWN BEFORE THE BOOT
===========================================================
One page, no timers, no rAF -- deliberately.  A timer callback calls
js_page_slice_begin() and RE-ARMS the deadline (js_page.c:771, untouched by
this fix), so a page with a running animation loop is immune to the ORIGINAL
bug and would prove nothing either way.  A page that finishes loading and goes
quiet -- a chat UI waiting for you to type -- never re-arms, and that is the
specimen.

Every click's handler: one window.scrollTo() call (see NESTED DISPATCH below),
then a 60,000-iteration loop, then a console.log.  webapi_probe's selftest
measured, against quickjs.c's own source, that such a loop costs 2.00 poll
events per iteration (the loop test and the back edge).  So the loop alone
costs 120,000 poll events = 12 interrupt calls = 12 fuel, against a fuel budget
of 2,000,000 -- FIVE ORDERS OF MAGNITUDE under the fuel rail and cannot possibly
exhaust it on its own.  Every prediction below follows from that arithmetic and
is written down here, before the boot, for BOTH builds:

  click 1, immediately after load  -- the handler RUNS, no watchdog, on BOTH
      builds.  This is the negative control on the CLICK itself: without it, a
      run where the browser never delivered a click at all would look exactly
      like a run where the watchdog ate it, and the whole measurement would be
      of a mouse that missed.  It is ALSO the negative control on the nested
      dispatch (see below): if scrollTo's synchronous "scroll" dispatch never
      reaches its listener here -- seconds after page load, nowhere near any
      deadline in either build -- clicks 2 and 3 could not test the nested
      case either, and the harness says so rather than guessing.

  click 2, MORE than 45 s later:
      FIXED build   -- the handler RUNS, no watchdog.  js_dom_dispatch() arms a
          fresh 45 s slice for this outermost dispatch; the handler costs 12
          fuel (astronomically under budget) so it completes.  THIS IS THE FIX.
      CONTROL build -- the handler is WATCHDOG-BITTEN, on the wall-time rail.
          js_dom_dispatch() (JS_DOM_NO_SLICE_BRACKET compiled out both halves)
          never re-arms, so the deadline from page load -- expired minutes ago
          -- is still the one slice_interrupt() checks against. THIS IS THE BUG,
          reproduced on demand rather than waited for.

  click 3, straight after click 2  -- the handler RUNS, no watchdog, on BOTH
      builds, and for a DIFFERENT reason on each:
      FIXED build   -- click 3 is its own outermost dispatch and gets its own
          fresh slice, same as click 2 did.
      CONTROL build -- slice_interrupt() sets g_slice_armed = 0 when it bites
          ("one interrupt per slice, not a storm"), so after click 2's bite the
          deadline is gone and click 3 runs completely unpoliced.  This is the
          signature that separates the bug from a slow handler: a slow handler
          is slow every time, but this one is inert for exactly one click and
          then works -- which is the shape the owner actually saw.

NESTED DISPATCH, and what it can and cannot prove here. Every click's handler
now opens with window.scrollTo(0, 10+n) (n = clicks so far), which drives
win_scroll_set() in js_cssom.c and dispatches a SYNCHRONOUS "scroll" event
through this SAME js_dom_dispatch() before scrollTo() returns to the click
handler -- see the NESTING paragraph in js_dom.c's bracket comment. The target
climbs by one every click (10, 11, 12) because win_scroll_set only dispatches
on an actual position CHANGE, so a fixed target would only fire once. What this
proves: on the FIXED build, click 2's handler completing (SLICE-RAN) means the
OUTER handler survived a nested dispatch in the middle of its own 45-s-later
slice -- exactly the "outer handler still completes" property this exists to
check, using the SAME assertion (SLICE-RAN) the core fix already needed, rather
than a fourth click. What it does NOT and cannot prove: there is no [js] serial
line for a slice that completed cleanly -- the watchdog's "[js] watchdog: ..."
lines print ONLY on a bite -- and no per-slice profiler line reaches the guest
console here either, because js_prof_enable() is never called outside
webapi_probe's HOST selftest (browser.c never turns the profiler on for a live
boot). So "the profiler does not mark the page idle" has no existing [js] line
to assert against on this driver, and none is invented here to manufacture one:
SCROLL-NESTED-<tok>-<n> below is a PAGE-authored console.log on the same
channel SLICE-RAN/SLICE-LOADED already use, not a new kernel-side serial line.
click 1's nested marker is REQUIRED (see above); clicks 2 and 3's are reported
in "observed" but not required, because whether the nested dispatch's own log
call lands before or after the interrupt counter reaches 10,000 in the CONTROL
build's bitten click 2 is a branch-count race this driver does not control and
asserting it would make the gate flaky rather than more informative.

FAILURE IS A VERDICT, NOT AN EXCEPTION.  Every exit prints one JSON line.  The
exit code is about the HARNESS (did the measurement happen), never about the
browser -- the same rule tests/qmp/qmp_site.py states, and for the same reason:
a driver that cannot tell "the click missed" from "the watchdog fired" is not
an instrument.  The verdict WORDS say what they mean rather than what a stale
prediction expected: FIXED (all three clicks clean -- the shipped behaviour)
and BUG-PRESENT (click 2 bitten, click 3 clean -- the pre-fix behaviour) are
both legitimate, build-dependent OUTCOMES, and this file's two Makefile
callers (test-jsslice against the fixed build, test-jsslice-negctl against the
control build) each grep for the one their build must produce.
"""
import argparse, http.server, json, os, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# BROWSER_SLOT is IMPORTED, never re-spelled. This driver's first run failed
# with "the Dock never launched the Browser" because it carried its own copy of
# that number and the copy was 5; qmp_ui.py has said 8 since the dock grew an
# app. One jar, two doors -- CLAUDE.md rule 3, paid for again here in one boot.
from qmp_ui import Session, configure   # noqa: E402

# From qmp_site.py's VIEWPORT = (110, 200, 1270, 655): a point deep inside the
# page's content area, far from the tab strip and the address bar.  The <div>
# the page paints is 2000 px tall and full width, so this lands on it with a
# margin of hundreds of pixels in every direction -- which is the point.  A
# coordinate that has to be RIGHT is the failure tools/check-test-liveness.py
# already found in five drivers in this tree.
CLICK_XY = (600, 420)

PAGE = """<!doctype html><html><head><title>slice</title></head><body>
<div id=pad style="width:100%%;height:2000px;background:#88aacc"></div>
<script>
var n = 0;
/* NESTED DISPATCH, on every click -- see the file header's NESTED DISPATCH
 * section. The target grows by one each click (win_scroll_set only dispatches
 * on an actual position change, so a fixed target would only fire once). */
window.addEventListener('scroll', function () {
  console.log('SCROLL-NESTED-%(tok)s-' + n);
});
document.getElementById('pad').addEventListener('click', function () {
  window.scrollTo(0, 10 + n);
  var s = 0; for (var i = 0; i < 60000; i++) s += i;   /* 12 fuel, see header */
  n++; console.log('SLICE-RAN-%(tok)s-' + n + '-' + s);
});
console.log('SLICE-LOADED-%(tok)s');
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--out")
    ap.add_argument("--label", default="",
                    help="purely cosmetic tag recorded in the JSON (e.g. "
                         "'noslice') so a --out file names which build "
                         "produced it; never affects the verdict")
    ap.add_argument("--wait", type=float, default=52.0,
                    help="seconds to idle between click 1 and click 2 "
                         "(must exceed the 45 s JS_SLICE_MS_DEFAULT)")
    args = ap.parse_args()

    rec = {"verdict": "HARNESS", "why": "did not run", "build": args.label,
           "wait_s": args.wait, "predictions": {}, "observed": {}}

    def emit(verdict, why, code=0):
        rec["verdict"], rec["why"] = verdict, why
        if args.out:
            with open(args.out, "w", encoding="utf-8") as fh:
                json.dump(rec, fh, indent=1)
        print(json.dumps({"verdict": verdict, "why": why, "build": args.label,
                          "observed": rec["observed"]}, indent=1))
        # "The exit code is about the HARNESS (did the measurement happen),
        # never about the browser" -- but until 2026-08-30 every HARNESS
        # verdict also exited 0, so a boot that never happened read as a
        # completed experiment to anything invoking this
        # (tools/check-test-liveness.py rule 1 caught exactly that). Every
        # non-HARNESS verdict stays 0 on purpose: it is a finding, not an
        # instrument failure -- the Makefile targets decide pass/fail by
        # grepping for the ONE verdict word their build must produce.
        sys.exit(1 if verdict == "HARNESS" else code)

    tmp = tempfile.mkdtemp(prefix="jsslice.")
    serial_path = os.path.join(tmp, "serial.log")
    qmp_path = os.path.join(tmp, "qmp.sock")
    # A stale unix socket makes QEMU refuse to bind and the boot reads as a
    # broken guest -- one of the four instruments that lied in this tree on
    # 2026-08-28.  The temp dir is fresh, but the removal is free.
    if os.path.exists(qmp_path):
        os.unlink(qmp_path)

    tok = "%d" % (os.getpid() & 0xFFFF)
    page = (PAGE % {"tok": tok}).encode()

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            try:
                self.wfile.write(page)
            except OSError:
                pass

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), H)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    dev_w, dev_h = 1280, 800
    configure(dev_w, dev_h)
    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none",
           "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (dev_w, dev_h),
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    qlog = open(os.path.join(tmp, "qemu.log"), "wb")
    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            emit("HARNESS", "the %s (%s) is missing or empty" % (what, p))
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

    try:
        if not wait_for("LOGIT_BOOT_OK", 180):
            emit("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        time.sleep(3)
        # One click, at the tile the GUEST names for browser.aex, verified against the
        # guest's own [wm] launched line -- the re-click loop this replaces was
        # the apology a hand-kept slot constant needed. See qmp_ui's dock block.
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            emit("HARNESS", str(e))
        time.sleep(7)

        mark = len(serial())
        ui.key_mods(["ctrl"], "t")
        ui.typ("http://10.0.2.2:%d/s.html" % port)
        ui.key("ret")
        if not wait_for("SLICE-LOADED-" + tok, 60, mark):
            emit("HARNESS", "the fixture page never loaded -- keyboard, Ctrl+T, "
                            "the address bar or SLIRP is broken, so nothing was "
                            "measured about the watchdog")
        time.sleep(2.0)

        # ---- click 1: the NEGATIVE CONTROL, on the click AND on nesting -----
        rec["predictions"]["click1"] = "handler RUNS, no watchdog, nested scroll fires -- both builds"
        m1 = len(serial())
        ui.click_at(*CLICK_XY)
        ran1 = wait_for("SLICE-RAN-%s-1-" % tok, 20, m1)
        bit1 = "watchdog" in serial(m1)
        nested1 = ("SCROLL-NESTED-%s-0" % tok) in serial(m1)
        rec["observed"]["click1_handler_ran"] = ran1
        rec["observed"]["click1_watchdog"] = bit1
        rec["observed"]["click1_nested_scroll"] = nested1
        if not ran1:
            emit("HARNESS",
                 "the control click never reached the page (handler did not "
                 "run) -- the click missed or events are not delivered, so a "
                 "later 'the handler did not run' would mean nothing")
        if bit1:
            emit("UNEXPECTED", "the watchdog bit the very first click, before "
                               "any deadline could have expired, on either build")
        if not nested1:
            emit("HARNESS",
                 "click 1's handler ran but its window.scrollTo() never "
                 "produced a 'scroll' dispatch (no SCROLL-NESTED marker) -- "
                 "win_scroll_set()/js_dom_dispatch() nesting is not reaching "
                 "the page listener, so clicks 2 and 3 cannot test the nested "
                 "case either and nothing about it was measured")

        # ---- idle past the deadline, running no JS at all -----------------
        time.sleep(args.wait)

        # ---- click 2: THE ONE THAT DIFFERS BY BUILD ------------------------
        rec["predictions"]["click2"] = (
            "FIXED build: handler RUNS, no watchdog (fresh 45s slice from "
            "js_dom_dispatch). CONTROL build (-DJS_DOM_NO_SLICE_BRACKET): "
            "watchdog BITES on the wall-time rail (stale page-load deadline, "
            "field-measured fuel=13/2000000).")
        m2 = len(serial())
        ui.click_at(*CLICK_XY)
        bit2 = wait_for("watchdog", 25, m2)
        ran2 = ("SLICE-RAN-%s-2-" % tok) in serial(m2)
        nested2 = ("SCROLL-NESTED-%s-1" % tok) in serial(m2)
        wd_lines = [ln.strip() for ln in serial(m2).replace("\r", "").splitlines()
                    if "watchdog" in ln]
        rec["observed"]["click2_watchdog"] = bit2
        rec["observed"]["click2_handler_ran"] = ran2
        rec["observed"]["click2_nested_scroll"] = nested2
        rec["observed"]["click2_watchdog_lines"] = wd_lines
        rail = None
        for ln in wd_lines:
            if "wall time" in ln:
                rail = "time"
            elif "instruction fuel" in ln:
                rail = "fuel"
        rec["observed"]["rail"] = rail

        # ---- click 3: THE SIGNATURE, clean on both builds -------------------
        rec["predictions"]["click3"] = (
            "handler RUNS again, no watchdog, on BOTH builds -- FIXED gets "
            "its own fresh slice same as click2; CONTROL runs unpoliced "
            "because click2's bite cleared g_slice_armed. This is what "
            "separates the bug from a slow handler: inert for exactly one "
            "click, then works.")
        time.sleep(1.0)
        m3 = len(serial())
        ui.click_at(*CLICK_XY)
        ran3 = wait_for("SLICE-RAN-%s-" % tok, 25, m3)
        bit3 = "watchdog" in serial(m3)
        nested3 = ("SCROLL-NESTED-%s-2" % tok) in serial(m3)
        rec["observed"]["click3_handler_ran"] = ran3
        rec["observed"]["click3_watchdog"] = bit3
        rec["observed"]["click3_nested_scroll"] = nested3

        with open(os.path.join(tmp, "keep.serial.txt"), "w") as fh:
            fh.write(serial())
        rec["serial_log"] = os.path.join(tmp, "keep.serial.txt")

        # THE TWO LEGITIMATE SHAPES. Both are real outcomes of a real build;
        # neither is "the test failing" by itself -- see the file header.
        if not bit2 and ran2 and ran3 and not bit3:
            emit("FIXED",
                 "a click %.0f s after load ran to completion with a nested "
                 "dispatch inside it (received its own fresh 45s slice "
                 "instead of inheriting the stale page-load deadline); "
                 "control clicks (immediately after load, and right after "
                 "the idle period) also ran clean" % (args.wait,))
        if bit2 and ran3 and not bit3:
            emit("BUG-PRESENT",
                 "click %.0f s after load was watchdog-bitten on the %s rail "
                 "(stale deadline from page load never re-armed by "
                 "js_dom_dispatch); click 3 immediately after ran clean "
                 "because the bite cleared g_slice_armed -- exactly the shape "
                 "the owner's field report described"
                 % (args.wait, rail))
        emit("INCONCLUSIVE",
             "neither known shape held: click2 watchdog=%s handler=%s rail=%s, "
             "click3 handler=%s watchdog=%s -- read the serial log rather than "
             "trust this summary"
             % (bit2, ran2, rail, ran3, bit3))
    finally:
        try:
            proc.kill()
        except Exception:
            pass


if __name__ == "__main__":
    main()
