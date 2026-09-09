#!/usr/bin/env python3
"""Per-operation DOM cost against document size, ON THE DEVICE, in the real browser.

    python3 tests/qmp/qmp_jsdomscale.py --iso build/logit.iso --disk build/disk.img

WHY A GUEST HALF EXISTS AT ALL
==============================
`webapi_probe --domscale` already plots the same shape on the host, and the
shape is the finding -- whether a per-operation cost is flat or rises with the
document is a property of the algorithm, and TCG multiplies a constant without
bending a line.  So this file is NOT here to discover the shape.  It is here for
three things the host binary structurally cannot say:

  1. THE HOST BINARY IS NOT THE BROWSER.  It is arm64/darwin, clang -O2, the
     system allocator, and it does not link layout.c at all (PROBE_SRC excludes
     $(BROWSER_PIPE)).  Everything below runs in browser.aex, x86_64 under TCG,
     on mini-libc's arena allocator, with the layout engine present and able to
     be re-entered.  If a row is flat on the host and rising here, the growth is
     in something only the browser has.

  2. THE ABSOLUTE NUMBER DECIDES WHETHER ANY OF IT MATTERS.  "querySelectorAll
     is O(N) per call" is a shape.  "one querySelectorAll on a 500-element
     subtree costs X ms on the machine the owner is using" is what says whether
     a page doing a few thousand of them spends the 45 s budget.  tools/perf/'s
     rule -- measure inside the guest, the host is contended -- is exactly this.

  3. THE HOST HARNESS'S CLOCK IS FAKE.  js_page_set_clock() there is a counter
     that advances a millisecond per read, frozen for the whole of a synchronous
     eval.  Date.now() in here is the device's real clock through a syscall.

WHAT IS MEASURED, AND THE PREDICTIONS ARE WRITTEN DOWN BEFORE THE BOOT
=====================================================================
Seven rows.  Two of them are controls that must come back FLAT, and they are
first because a scaling harness with no flat row has never been shown able to
report "not quadratic" -- which is CLAUDE.md rule 5 for this kind of instrument:

  baseline        pure JS arithmetic, never touches the DOM.  MUST be flat.  If
                  it rises, the document got bigger and nothing else did, so the
                  rise is GC pressure or the allocator and every row below it is
                  contaminated and none of it counts.
  getElementById  MUST be flat.  dom.c indexes ids in a hash table
                  (dom_get_element_by_id), so this is the row that proves a
                  lookup CAN be O(1) here -- without it, "all lookups are slow"
                  is indistinguishable from "this machine is slow".

  querySelector('#anchor')          predicted RISING
  querySelectorAll('.c').length     predicted RISING
  getElementsByTagName('div').length predicted RISING
  children.length                   predicted RISING
  walk by nextSibling               predicted FLAT per element

THE ROW LIST IS NOT SHARED WITH THE HOST HARNESS AND THAT IS A STATED LIMIT.
webapi_probe.c's DS_OPS is a C array in a file another line is editing this
hour; lifting it into a file both sides read is the right fix and is not worth
a merge conflict today.  So these are seven hand-copied operations, and the
claim they support is the SHAPE OF THESE SEVEN, not "the host table reproduces".
One jar, two doors -- named rather than hidden (CLAUDE.md rule 3).

REPS ARE ADAPTIVE AND THE COUNT IS REPORTED.  A fixed repetition count either
takes microseconds at N=1 (unmeasurable against a 10 ms clock tick) or minutes
at N=500 on a row that costs milliseconds per call.  Each cell doubles its
repetition count until it has spent at least MIN_MS, then divides.  `ops` is
printed for every cell so a cell that measured almost nothing is visible rather
than averaged into the table.

ONE CELL PER TIMER CALLBACK, and this is load-bearing rather than tidy.  The
whole matrix as one synchronous script would be one CPU slice, and js_page.c
arms a 45 s deadline at the last <script>; a matrix that takes longer than that
would be interrupted half way and report a table of whatever it got to.  A
setTimeout callback calls js_page_slice_begin() and re-arms, so every cell gets
its own budget.  It also means each cell is its own js_prof slice.

FAILURE IS A VERDICT, NOT AN EXCEPTION -- same rule as qmp_jsslice.py.  The exit
code is about the HARNESS (did the measurement happen), never about the browser.
"""
import argparse, http.server, json, os, re, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# BROWSER_SLOT is IMPORTED, never re-spelled -- qmp_jsslice.py lost a whole boot
# to a hand-copied 5 when the dock had said 8 for days.
from qmp_ui import Session, configure   # noqa: E402

# Deep inside the page content area; the page paints a 2000 px filler so this
# lands on the document rather than on a control.  Taken from qmp_site.py's
# VIEWPORT, not invented.
SIZES = (1, 10, 100, 500)

PAGE = r"""<!doctype html><html><head><title>domscale</title></head><body>
<div id=pad style="width:100%%;height:1200px;background:#dde"></div>
<div id=box></div><div id=sink></div>
<script>
var TOK = '%(tok)s';
var SIZES = %(sizes)s;
var MIN_MS = %(minms)d;
var CAP = 200000;

function log(s) { console.log('DSG-' + TOK + ' ' + s); }

/* Build a document of N elements under #box, with #anchor LAST so a lookup
   that walks from the head pays the full N.  Putting it first would hide the
   defect this exists to find.  innerHTML so the setup is one crossing and is
   not itself part of any measurement. */
function build(n) {
  var box = document.getElementById('box');
  var a = [];
  for (var i = 0; i < n; i++)
    a.push('<div class=c data-i="' + i + '"><span>t</span></div>');
  a.push('<div class=c id=anchor data-i="anchor">a</div>');
  box.innerHTML = a.join('');
}

/* Each row is a function of (k) doing k operations and returning a checksum.
   The checksum is returned and logged so a row that was optimised away, or
   that threw and got caught somewhere, cannot read as "fast".  A row whose
   answer does not change with N is a row whose answer nobody looked at. */
var ROWS = [
 ['baseline-purejs', 'control', 'K', function (k) {
    var s = 0; for (var i = 0; i < k; i++) s += i; return s; }],
 ['getElementById', 'control', 'K', function (k) {
    var s = 0; for (var i = 0; i < k; i++) s += document.getElementById('anchor') ? 1 : 0;
    return s; }],
 ['querySelector-#id', 'lookup', 'K', function (k) {
    var s = 0; for (var i = 0; i < k; i++) s += document.querySelector('#anchor') ? 1 : 0;
    return s; }],
 ['querySelectorAll-.class', 'lookup', 'K', function (k) {
    var r = document.getElementById('box'), s = 0;
    for (var i = 0; i < k; i++) s += r.querySelectorAll('.c').length; return s; }],
 ['getElementsByTagName-len', 'lookup', 'K', function (k) {
    var r = document.getElementById('box'), s = 0;
    for (var i = 0; i < k; i++) s += r.getElementsByTagName('div').length; return s; }],
 ['children-length', 'collection', 'K', function (k) {
    var r = document.getElementById('box'), s = 0;
    for (var i = 0; i < k; i++) s += r.children.length; return s; }],
 ['walk-nextSibling', 'traversal', 'N', function (k) {
    var r = document.getElementById('box'), s = 0;
    for (var i = 0; i < k; i++) { var n = r.firstChild; while (n) { s++; n = n.nextSibling; } }
    return s; }]
];

/* One cell.  Doubles the operation count until the elapsed time clears MIN_MS,
   so a 10 ms clock tick is never the thing being reported.  Returns ns per
   operation -- per ELEMENT for a 'N' row, because dividing an inherently O(N)
   traversal by the call count would make a perfectly linear walk look
   quadratic, which is how a scaling harness lies. */
function cell(row, n) {
  var name = row[0], cls = row[1], per = row[2], fn = row[3];
  var k = 1, ms = 0, chk = 0, t0, t1;
  while (k <= CAP) {
    t0 = Date.now(); chk = fn(k); t1 = Date.now(); ms = t1 - t0;
    if (ms >= MIN_MS) break;
    k = k * 2;
  }
  var units = (per === 'N') ? k * (n + 1) : k;
  var nsop = units ? (ms * 1e6 / units) : -1;
  log('row=' + name + ' cls=' + cls + ' N=' + n + ' ops=' + k +
      ' units=' + units + ' ms=' + ms + ' nsop=' + nsop.toFixed(1) +
      ' chk=' + chk);
}

/* The schedule, flattened: one timer callback per cell.  See the header on why
   this is not a nested loop. */
var PLAN = [];
for (var si = 0; si < SIZES.length; si++)
  for (var ri = 0; ri < ROWS.length; ri++) PLAN.push([SIZES[si], ri]);

var pi = 0, built = -1;
function step() {
  if (pi >= PLAN.length) { log('DONE'); return; }
  var n = PLAN[pi][0], ri = PLAN[pi][1];
  pi++;
  try {
    if (n !== built) { build(n); built = n; log('built N=' + n); }
    cell(ROWS[ri], n);
  } catch (e) {
    log('row=' + ROWS[ri][0] + ' N=' + n + ' THREW ' + e);
  }
  setTimeout(step, 0);
}
log('LOADED');
setTimeout(step, 0);
</script></body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--out")
    ap.add_argument("--min-ms", type=int, default=60,
                    help="a cell doubles its op count until it spends this long")
    ap.add_argument("--budget", type=float, default=900.0,
                    help="seconds to wait for the matrix to finish")
    args = ap.parse_args()

    rec = {"verdict": "HARNESS", "why": "did not run", "rows": [],
           "predicted": {"baseline-purejs": "flat", "getElementById": "flat",
                         "querySelector-#id": "RISING",
                         "querySelectorAll-.class": "RISING",
                         "getElementsByTagName-len": "RISING",
                         "children-length": "RISING",
                         "walk-nextSibling": "flat"},
           "sizes": list(SIZES)}

    def emit(verdict, why, code=0):
        rec["verdict"], rec["why"] = verdict, why
        if args.out:
            with open(args.out, "w", encoding="utf-8") as fh:
                json.dump(rec, fh, indent=1)
        print(json.dumps({"verdict": verdict, "why": why,
                          "cells": len(rec["rows"])}, indent=1))
        # The header's own contract: "the exit code is about the HARNESS (did
        # the measurement happen), never about the browser" -- but until
        # 2026-08-30 every HARNESS verdict also exited 0, which made a boot
        # that never happened indistinguishable from a completed measurement
        # to anything invoking this (tools/check-test-liveness.py rule 1
        # caught exactly that). REFUTED/PARTIAL stay 0 on purpose: they are
        # findings, not failures of the instrument.
        sys.exit(1 if verdict == "HARNESS" else code)

    tmp = tempfile.mkdtemp(prefix="jsdomscale.")
    serial_path = os.path.join(tmp, "serial.log")
    qmp_path = os.path.join(tmp, "qmp.sock")
    if os.path.exists(qmp_path):
        os.unlink(qmp_path)

    tok = "%d" % (os.getpid() & 0xFFFF)
    page = (PAGE % {"tok": tok, "sizes": json.dumps(list(SIZES)),
                    "minms": args.min_ms}).encode()

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
    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            emit("HARNESS", "the %s (%s) is missing or empty" % (what, p))
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
            time.sleep(0.5)
        return False

    try:
        if not wait_for("LOGIT_BOOT_OK", 240):
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
        ui.typ("http://10.0.2.2:%d/d.html" % port)
        ui.key("ret")
        if not wait_for("DSG-%s LOADED" % tok, 90, mark):
            emit("HARNESS", "the fixture page never loaded -- keyboard, Ctrl+T, "
                            "the address bar or SLIRP is broken, so nothing was "
                            "measured")

        done = wait_for("DSG-%s DONE" % tok, args.budget, mark)
        text = serial(mark)
        with open(os.path.join(tmp, "keep.serial.txt"), "w") as fh:
            fh.write(serial())
        rec["serial_log"] = os.path.join(tmp, "keep.serial.txt")

        pat = re.compile(r"DSG-%s row=(\S+) cls=(\S+) N=(\d+) ops=(\d+) "
                         r"units=(\d+) ms=(-?\d+) nsop=(-?[\d.]+) chk=(\S+)" % tok)
        for m in pat.finditer(text):
            rec["rows"].append({"row": m.group(1), "cls": m.group(2),
                                "N": int(m.group(3)), "ops": int(m.group(4)),
                                "units": int(m.group(5)), "ms": int(m.group(6)),
                                "nsop": float(m.group(7)), "chk": m.group(8)})
        rec["watchdog_lines"] = [ln.strip() for ln in text.replace("\r", "").splitlines()
                                 if "watchdog" in ln]
        rec["threw"] = [ln.strip() for ln in text.replace("\r", "").splitlines()
                        if "THREW" in ln]

        # ---- the table, and the control asserted rather than printed ------
        by = {}
        for r in rec["rows"]:
            by.setdefault(r["row"], {})[r["N"]] = r
        ns = sorted(SIZES)
        print("\n== domscale, ON THE DEVICE (x86_64 under TCG, real browser) ==")
        print("%-26s %-11s %s %10s %8s" %
              ("operation", "class",
               "".join("%12d" % n for n in ns), "grow", "shape"))
        print("%-26s %-11s %s" %
              ("", "", "".join("%12s" % "ns/op" for n in ns)))
        growth = {}
        for name, _cls, _per, _fn in ():
            pass
        for r in ROWS_ORDER:
            cells = by.get(r)
            if not cells:
                print("%-26s %-11s  (no cells -- not measured)" % (r, "?"))
                continue
            cls = cells[list(cells)[0]]["cls"]
            first = cells.get(ns[0]), cells.get(ns[-1])
            line = "%-26s %-11s" % (r, cls)
            for n in ns:
                c = cells.get(n)
                line += "%12s" % ("%.0f" % c["nsop"] if c else "-")
            g = 0.0
            if first[0] and first[1] and first[0]["nsop"] > 0:
                g = first[1]["nsop"] / first[0]["nsop"]
            growth[r] = g
            shape = "RISING" if g >= 8 else ("soft" if g >= 2.5 else "flat")
            print(line + "%9.1fx %8s" % (g, shape))
        rec["growth"] = growth

        base = growth.get("baseline-purejs", 99)
        gid = growth.get("getElementById", 99)
        ok_ctl = base < 2.5 and gid < 2.5
        print("\ncontrol: baseline %.2fx, getElementById %.2fx over N=%d..%d -- %s"
              % (base, gid, ns[0], ns[-1],
                 "flat, as they must be" if ok_ctl else
                 "NOT FLAT: every row above is contaminated and none of it counts"))
        rec["control_ok"] = bool(ok_ctl)

        if not done:
            emit("HARNESS", "the matrix never printed DONE within %.0f s -- "
                            "%d cells were collected and the table above is "
                            "partial" % (args.budget, len(rec["rows"])))
        if not rec["rows"]:
            emit("HARNESS", "no cells parsed out of the serial log")
        if not ok_ctl:
            emit("REFUTED", "a control row is not flat, so no row means anything")
        rising = [r for r in rec["predicted"]
                  if rec["predicted"][r] == "RISING" and growth.get(r, 0) >= 8]
        want = [r for r in rec["predicted"] if rec["predicted"][r] == "RISING"]
        if len(rising) == len(want):
            emit("CONFIRMED",
                 "on the device, with both controls flat, all %d predicted rows "
                 "rise: %s" % (len(want), ", ".join("%s %.0fx" % (r, growth[r])
                                                    for r in want)))
        emit("PARTIAL",
             "controls flat; %d of %d predicted rows rose (%s)"
             % (len(rising), len(want),
                ", ".join("%s %.1fx" % (r, growth.get(r, 0)) for r in want)))
    finally:
        try:
            proc.kill()
        except Exception:
            pass


# The row names in table order, mirroring the ROWS array inside PAGE.  Spelled
# once here and used only for ordering the printed table; a name that drifts
# shows up as "(no cells -- not measured)" rather than as a silently dropped
# row, which is the failure this ordering list could otherwise cause.
ROWS_ORDER = ["baseline-purejs", "getElementById", "querySelector-#id",
              "querySelectorAll-.class", "getElementsByTagName-len",
              "children-length", "walk-nextSibling"]

if __name__ == "__main__":
    main()
