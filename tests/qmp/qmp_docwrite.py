#!/usr/bin/env python3
"""Prove, on the real machine, that document.write/writeln work.

    python3 tests/qmp/qmp_docwrite.py <iso> <disk.img> [--expect-off]

Host assertions cannot settle this. document.write's whole job is to splice
markup into the document AT THE POSITION OF THE WRITING SCRIPT, and then have
layout and paint treat that markup exactly like source markup -- which is only
observable by booting the OS, loading a page, and reading the screen.

The fixture (tests/fixtures/docwrite/docwrite.html) has three scripts:

  script 1 (inline, between #before and #after)   two document.write calls
  script 2 (inline, between #after and #last)     reads back #w1, then writeln
  script 3 (inline, after #last)                  logs DOM order, arms a timer

What each assertion proves, and where it was first watched FAILING:

  DW-DOM-ORDER before,w1,w2,after,w3,last
      The written nodes are in the tree, in call order, BETWEEN the script
      that wrote them and the markup that followed it in the source -- the
      tree a real browser's parser produces by inserting into the input
      stream just before the insertion point. Watched red on the pre-feature
      build: the order line read "before,after,last" because every write
      threw TypeError and killed its script.

  The eight colour blocks, in vertical order before < w1 < w2 < w4 < after
  < w3 < last (plus head-written fromhead on top)
      The written markup reached LAYOUT and PAINT, and in the same order as
      the DOM assertion above (two independent channels, like qmp_live_page:
      the tree via the serial log, the pixels via the screendump). Watched
      red on the pre-feature build as "could not find the red block".

  DW-W1-READ WRITTENONE
      A later script sees an earlier script's write (the python.org shape).

  DW-WLN-NEWLINE-OK
      writeln appended a U+000A text node after its markup, not another
      element.

  DW-WRITTEN-SCRIPT-RAN + DW-DOM-ORDER2 ...w4... + DW-LATE-WROTE + no
  orange on screen
      A <script> WRITTEN by document.write executed (in a real browser
      written input goes through the main parser, which runs scripts -- the
      fragment algorithm an innerHTML-based engine parses with does not),
      and that written script could itself write: w4 sits after it, between
      w2 and after in ORDER2. ORDER2 is logged from a timer after everything
      ran, so it must be the pre-drain order plus w4 -- the engine's one
      documented timing deviation, visible as the difference between the
      two order lines. The same timer's own write (the negative control's
      subject) contributed nothing: no ",late" at the end of the line, and
      the orange #late block never reaches the screen. Watched red on a
      build with the after-load gate open (writes appended at body end):
      the order line ended ",late" and the orange block was on screen.

--expect-off is the compiled-out control (make test-docwrite-negctl): the same
driver asserts the feature is ABSENT -- order line "before,after,last", w1
reads as null, the three written blocks are missing, and the page still
renders -- so the positive gate above is known to measure THIS feature and not
some other reason blocks happen to appear.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM  # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
EXPECT_OFF = "--expect-off" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "..", "fixtures", "docwrite", "docwrite.html")
with open(FIXTURE, "r", encoding="utf-8") as fh:
    PAGE = fh.read()

# Odd, unique colours (qmp_live_page's trick) so each block is locatable in a
# screendump without coordinates. ORANGE exists only in the after-load write,
# which must never reach the screen. FROMHEAD is written by a script inside
# <head>: it must paint at the TOP of the body, above source body content.
# W4 is written by the WRITTEN script (script 1's third write) -- its presence
# on screen, between W2 and AFTER, is the proof that written scripts both RUN
# and can themselves write at their own position.
FROMHEAD, BEFORE, W1, W2, AFTER, W3, LAST = ((127, 127, 1), (1, 127, 254),
                                              (254, 1, 2), (2, 254, 1),
                                              (127, 1, 254), (1, 2, 254),
                                              (254, 1, 127))
W4 = (1, 254, 254)
ORANGE = (254, 127, 1)

ORDER = "fromhead,before,w1,w2,after,w3,last"
# ORDER2 adds w4 -- the written script's own write, run by the time the timer
# fires -- and NOTHING else: the pre-drain order plus exactly that.
ORDER_LATE = "fromhead,before,w1,w2,w4,after,w3,last"
ORDER_OFF = "before,after,last"

tmp = tempfile.mkdtemp(prefix="qmp_docwrite_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw = PAGE.encode() if self.path.startswith("/docwrite") else b"not found\n"
        self.send_response(200 if raw != b"not found\n" else 404)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok: " if cond else "FAIL: ") + name)
    if not cond:
        die(name)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def block(img, colour, what):
    box = img.find_color(colour)
    if not box:
        die("could not find the %s block on screen (page did not render?)" % what)
    return box


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 60, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(6)                          # ~2.8 MB .aex off virtio-blk, ELF load, first paint

    ui.click_at(420, 145)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/docwrite.html" % PORT)
    ui.key("ret")

    # ---- 1. the tree: written nodes exist, in call order, at the writing
    #          script's position --------------------------------------------
    if EXPECT_OFF:
        ck(wait_serial("DW-DOM-ORDER " + ORDER_OFF, 60, "control DOM order"),
           "CONTROL: with the feature compiled out the order line is "
           "'%s' (every write threw and killed its script)" % ORDER_OFF)
        ck("DW-W1-READ null" in serial(),
           "CONTROL: the later script reads w1 as null")
    else:
        ck(wait_serial("DW-SCRIPT1-DONE", 60, "script 1"),
           "script 1 ran to completion -- its document.write calls returned")
        ck(wait_serial("DW-DOM-ORDER " + ORDER, 60, "DOM order"),
           "written nodes are in the tree in call order, between the writing "
           "script and the markup that followed it in the source, with the "
           "head-written block at the TOP of the body")
        ck("DW-W1-READ WRITTENONE" in serial(),
           "a later script reads back an earlier script's write (python.org shape)")
        ck("DW-WLN-NEWLINE-OK" in serial() and "DW-WLN-NEWLINE-BAD" not in serial(),
           "writeln appended a newline text node after its markup")
        # Not order-sensitive ON PURPOSE: the drain runs written scripts one
        # pass after the parse-collected ones (the engine's documented timing
        # deviation), and a future streaming parser would interleave them the
        # spec way -- the gate must stay green under that improvement, so all
        # it demands is that the marker printed at all.
        ck(wait_serial("DW-WRITTEN-SCRIPT-RAN", 60, "the written script"),
           "a <script> WRITTEN by document.write EXECUTED (main-parser "
           "semantics -- a fragment parse alone would leave it inert)")

    time.sleep(2.0)
    p0 = PPM(ui.screendump(shot("p0")))

    # ---- 2. the pixels: the written markup reached layout and paint, in the
    #          same order the tree claims ----------------------------------
    if EXPECT_OFF:
        ck(p0.find_color(FROMHEAD) is None and p0.find_color(W1) is None
           and p0.find_color(W2) is None and p0.find_color(W4) is None
           and p0.find_color(W3) is None,
           "CONTROL: none of the written blocks (incl. the written script's) "
           "is on screen")
        b_before = block(p0, BEFORE, "'before' control block")
        b_after = block(p0, AFTER, "'after' control block")
        b_last = block(p0, LAST, "'last' control block")
        ck(b_before[1] < b_after[1] < b_last[1],
           "CONTROL: the page still renders in source order")
    else:
        b_fh = block(p0, FROMHEAD, "head-written block")
        b_before = block(p0, BEFORE, "'before' control block")
        b_w1 = block(p0, W1, "first document.write block")
        b_w2 = block(p0, W2, "second document.write block")
        b_w4 = block(p0, W4, "the WRITTEN script's own write")
        b_after = block(p0, AFTER, "'after' control block")
        b_w3 = block(p0, W3, "document.writeln block")
        b_last = block(p0, LAST, "'last' control block")
        ck(b_fh[1] < b_before[1] < b_w1[1] < b_w2[1] < b_w4[1] < b_after[1]
           < b_w3[1] < b_last[1],
           "all eight blocks paint in the written/DOM order on screen "
           "(y: %d < %d < %d < %d < %d < %d < %d < %d)"
           % (b_fh[1], b_before[1], b_w1[1], b_w2[1], b_w4[1], b_after[1],
              b_w3[1], b_last[1]))
        ck(p0.find_color(ORANGE) is None,
           "the after-load write's colour is not on screen yet")

    # ---- 3. THE NEGATIVE CONTROL: a write after load contributes nothing --
    # DW-LATE-WROTE is printed AFTER the write call returns, so its presence
    # means the timer fired and the call was made (and ignored) -- not that
    # the timer never ran. On the --expect-off build the call THROWS instead
    # (that is what absence means: "uncaught in timer: TypeError"), so the
    # control waits for the throw and asserts the order2 log never happened.
    if EXPECT_OFF:
        ck(wait_serial("uncaught in timer: TypeError: write is not a function", 60,
                       "the after-load timer"),
           "CONTROL: the after-load write THREW (the pre-feature behaviour)")
        ck("DW-DOM-ORDER2" not in serial(),
           "CONTROL: the timer never got past the write to log an order")
        p1 = PPM(ui.screendump(shot("p1")))
        ck(p1.find_color(ORANGE) is None,
           "CONTROL: no orange on screen")
        print("\nPASS: CONTROL (feature compiled out) -- writes are absent, "
              "pages survive")
        proc.kill()
        sys.exit(0)

    ck(wait_serial("DW-LATE-WROTE", 60, "the after-load timer"),
       "the timer fired and its document.write call returned")
    order2 = None
    for line in serial().splitlines():
        if line.strip().endswith("DW-DOM-ORDER2 " + ORDER_LATE):
            order2 = line.strip()
    ck(order2 is not None,
       "the DOM order after the after-load write is the pre-drain order PLUS "
       "w4 (the written script's own write, run by now) and NOTHING else -- "
       "#late never entered the tree")
    time.sleep(1.5)
    p1 = PPM(ui.screendump(shot("p1")))
    ck(p1.find_color(ORANGE) is None,
       "the after-load write changed NO pixels -- its block is not on screen")
    b_last1 = block(p1, LAST, "'last' control block (still there)")
    ck(b_last1[1] == b_last[1],
       "and the page below it did not move (%d == %d)" % (b_last1[1], b_last[1]))

    mode = "CONTROL (feature compiled out): writes are absent, pages survive"
    if EXPECT_OFF:
        print("\nPASS: " + mode)
    else:
        print("\nPASS: document.write/writeln insert at the writing script's "
              "position, in order, and a write after load changes nothing")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
