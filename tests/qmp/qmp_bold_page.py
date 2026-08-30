#!/usr/bin/env python3
"""Prove, on the real machine, that bold text actually RENDERS bold.

    python3 tests/qmp/qmp_bold_page.py <iso> <disk.img>

The implementation arc this closes (commit 2ec6f9873, 2026-08-28) is three
pieces that only pay off together: a `bold` field in struct logit_run (the
text-run ABI had no weight dimension), kernel face selection in
c/kernel/gui/text.c (face_font()/tl_fonts() pick fsroot/fonts/{ui,mono}-bold
for a LOGIT_FACE_BOLD run), and browser_paint.c finally reading the o->bold
that css_engine.c had computed since the day it was written. None of it had
ever been watched on the glass -- the existing host gate
(tests/unit/font_weight_test.c) links the real text.c against the real fonts,
which is one short of the claim: a bold bit can be correct in the kernel and
still dropped between css_engine.c and the syscall, which is exactly the gap
that held this tree at "every <h1> renders at regular weight" for four
rounds. So this boots the OS, serves tests/fixtures/bold/bold.html from the
host over SLIRP, loads it in the Browser and measures the PIXELS:

  advance      the painted width of a run's ink (leftmost to rightmost
               non-background pixel inside its band). Bold glyphs are wider
               and bold ADVANCES are wider; the advance is the number layout
               must also measure, so this is the correctness half.

  ink          the count of pixels inside the band that are not exactly the
               band's background colour. Anti-aliased stem edges are blends
               and therefore count as ink; a face that measured wider and
               drew the same outlines would pass advance and fail here.

Three proportional pairs (one per way css_engine.c sets o->bold: <b>,
<strong>, numeric font-weight:700) each assert advance AND ink strictly
exceed the regular twin's. The mono pair asserts the opposite on advance --
the BOLD mono advance must equal the regular one to the pixel, read from
the run START positions in the browser's own [dl] painted-text dump on the
serial (a run's start x is advance-pure where its ink edges are not; the
band width is viewport-wide and would make the check vacuous) -- while
still putting down more ink. The R1/B1 pair additionally asserts their ink
sits at the SAME depth inside otherwise identical bands, which is the
guest-side face of the vertical-metrics rule fsroot/fonts/README.md states
and tests/fixtures/bold/check_font_metrics.py asserts on the font bytes:
the kernel places the baseline from the run's first font's hhea.ascent, so
a bold ascent drift shows up as B1's glyphs sitting higher or lower than
R1's.

The specimen line at the top of the page is the gate statement verbatim --
  abc <b>abc</b> <strong>def</strong>
  <span style="font-weight:700">ghi</span> <code>x<b>y</b></code>
-- and is asserted against a plain-text control line of the same words:
marking most of a line up must leave measurably more ink on the glass.

The screendumps are kept in the temp directory named at the end so a failure
can be looked at rather than guessed at.
"""

import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, configure, pt   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXTURE = os.path.join(ROOT, "tests", "fixtures", "bold", "bold.html")

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# The band colours, spelled once here and once in bold.html. Odd triples so
# nothing in the desktop chrome, the wallpaper gradient or the font
# antialiasing can collide with them, and distinct from every colour
# tests/qmp/qmp_css_fidelity.py paints.
SPEC_BG = (254, 3, 5)     # the gate line itself
CTRL_BG = (5, 254, 3)     # the same words, unmarked
R1_BG = (3, 5, 254)       # "abc" plain
B1_BG = (254, 5, 250)     # <b>abc</b>
R2_BG = (5, 250, 254)     # "def" plain
B2_BG = (250, 254, 5)     # <strong>def</strong>
R3_BG = (254, 250, 5)     # "ghi" plain
B3_BG = (5, 5, 250)       # <span style="font-weight:700">ghi</span>
R4_BG = (250, 5, 254)     # <code>m</code><code>n</code> -- regular mono advance
B4_BG = (5, 254, 250)     # <code><b>m</b>n</code> -- bold mono advance

tmp = tempfile.mkdtemp(prefix="qmp_bold_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        with open(FIXTURE, "rb") as fh:
            raw = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

XRES = int(os.environ.get("QMP_XRES", "1280"))
YRES = int(os.environ.get("QMP_YRES", "800"))
SCALE = configure(XRES, YRES)
print("display %dx%d device px, backing scale %d%%" % (XRES, YRES, SCALE))

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
    proc.kill()
    sys.exit(1)


def ck(cond, name):
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


def band(img, colour, what):
    box = img.find_color(colour)
    if not box:
        die("could not find the %s band on screen (page did not render?)" % what)
    return box


def run_stats(img, box, bg):
    """Ink pixels, ink x-extent and ink y-extent of a band.

    `ink` is every pixel that is not EXACTLY the band colour: the glyph blend
    is dst = c*cov + bg*(1-cov), so every antialiased edge pixel differs from
    bg and counts. This is deliberately stricter than a darkness threshold --
    a faintly-painted glyph must not slip through as background."""
    x0, y0, x1, y1 = box
    n = 0
    minx = miny = 1 << 30
    maxx = maxy = -1
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if img.at(x, y) != bg:
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if n == 0:
        die("band %s was found but carries no ink -- the text run did not paint"
            % (bg,))
    return {"ink": n, "x0": minx, "x1": maxx, "y0": miny, "y1": maxy}


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)                          # desktop + dock composited

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(3.0)                        # ~2.7 MB .aex off virtio-blk, then ELF load
    ui.screendump(os.path.join(tmp, "launch.ppm"), settle=0.4)

    ui.click_at(pt(420), pt(145))          # address bar (points -> device px)
    for _ in range(60):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/bold.html" % PORT)
    ui.key("ret")
    time.sleep(10)
    shot = os.path.join(tmp, "bold.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    # ---- the three proportional pairs: advance AND ink must both grow ----
    for rbg, bbg, rwhat, bwhat in ((R1_BG, B1_BG, "plain abc", "<b>abc</b>"),
                                   (R2_BG, B2_BG, "plain def", "<strong>def</strong>"),
                                   (R3_BG, B3_BG, "plain ghi",
                                    "font-weight:700 ghi")):
        r = run_stats(img, band(img, rbg, rwhat), rbg)
        b = run_stats(img, band(img, bbg, bwhat), bbg)
        r_adv, b_adv = r["x1"] - r["x0"] + 1, b["x1"] - b["x0"] + 1
        print("%-12s advance %d px, ink %d px   vs   %-22s advance %d px, ink %d px"
              % (rwhat, r_adv, r["ink"], bwhat, b_adv, b["ink"]))
        ck(b_adv >= r_adv + 1,
           "%s paints at least 1px wider than its regular twin" % bwhat)
        ck(b["ink"] > r["ink"] + r["ink"] // 20,
           "%s puts down at least 5%% more ink than its regular twin" % bwhat)

    # ---- the mono pair: the advance must NOT move, the ink must grow ----
    # The band width is USELESS here (block bands are viewport-wide; both
    # measure 1126 px no matter what the glyphs do), so the advance is read
    # from the browser's own [dl] painted-run dump on the serial: each run's
    # start x is printed, and the two runs of a band are adjacent elements.
    # n_x - m_x is exactly the advance of the m run at its weight --
    # advance-pure where ink edges are not (bold stems widen ink without
    # moving a cell). The bold band bolds the FIRST glyph so its delta
    # measures the BOLD mono advance; equal deltas to the pixel is the claim
    # the Terminal's grid is built on.
    r4 = band(img, R4_BG, "regular mono")
    b4 = band(img, B4_BG, "bold mono")
    rs, bs = run_stats(img, r4, R4_BG), run_stats(img, b4, B4_BG)
    print("mono ink: <code>m</code><code>n</code> %d px  vs  <code><b>m</b>n</code> %d px"
          % (rs["ink"], bs["ink"]))
    ck(bs["ink"] > rs["ink"] + rs["ink"] // 20,
       "<code><b>m</b>n</code> puts down at least 5% more ink than <code>mn</code>")

    # The [dl] dump is per-PAINT, and a damage repaint after the first
    # compose could print only the repainted runs -- so walk the dumps from
    # the latest backwards and stop at the first that holds both bands.
    # A dump whose format changed yields zero pairs everywhere and the gate
    # dies naming the format, rather than passing on nothing.
    pairs = None
    for dump in reversed(serial().split("[dl] ---8<--- begin painted text")[1:]):
        runs = []
        for ln in dump.splitlines():
            m = re.match(r"\[dl\] (\d+),(\d+) (.+)$", ln)
            if m:
                runs.append((int(m.group(1)), int(m.group(2)), m.group(3)))
        by_y = {}
        for x, y, s in runs:
            if s in ("m", "n"):
                by_y.setdefault(y, {})
                if s not in by_y[y]:
                    by_y[y][s] = x
        found = sorted((y, d["m"], d["n"]) for y, d in by_y.items()
                       if "m" in d and "n" in d)
        if len(found) == 2:
            pairs = found
            break
    if pairs is None:
        die("no [dl] painted-text dump contains both m/n run pairs -- the"
            " dump format or the fixture changed under the driver")
    (_, mx, nx), (_, bmx, bnx) = pairs[0], pairs[1]
    r_adv, b_adv = nx - mx, bnx - bmx
    print("mono advance from [dl] run starts: regular %d px, bold %d px"
          " (m at %d/%d, n at %d/%d)" % (r_adv, b_adv, mx, bmx, nx, bnx))
    ck(r_adv == b_adv,
       "the BOLD mono advance equals the regular one to the pixel"
       " (a bold <code> must not move the Terminal's cell grid)")

    # ---- vertical metrics, on the glass ----
    # R1 and B1 are identical bands except for the weight, so their ink must
    # sit at the same depth: the kernel puts the baseline at hhea.ascent of
    # the run's first font, and a bold ascent drift would raise or lower B1's
    # glyphs whole pixels at this size. (The font bytes are asserted to match
    # in tests/fixtures/bold/check_font_metrics.py; this is the whole-pipeline
    # version of the same claim.)
    r1 = band(img, R1_BG, "plain abc")
    b1 = band(img, B1_BG, "<b>abc</b>")
    r, b = run_stats(img, r1, R1_BG), run_stats(img, b1, B1_BG)
    rtop, btop = r["y0"] - r1[1], b["y0"] - b1[1]
    rbot, bbot = r1[3] - r["y1"], b1[3] - b["y1"]
    print("ink depth in band: regular top %d bottom %d, bold top %d bottom %d"
          % (rtop, rbot, btop, bbot))
    ck(abs(rtop - btop) <= 1 and abs(rbot - bbot) <= 1,
       "the bold run's ink sits at the same depth as the regular run's"
       " (no vertical shift from a bold-face metrics drift)")

    # ---- the specimen line vs its unmarked control ----
    spec = run_stats(img, band(img, SPEC_BG, "specimen line"), SPEC_BG)
    ctrl = run_stats(img, band(img, CTRL_BG, "control line"), CTRL_BG)
    s_adv, c_adv = spec["x1"] - spec["x0"] + 1, ctrl["x1"] - ctrl["x0"] + 1
    print("specimen line: advance %d px, ink %d px   vs   control line: advance %d px, ink %d px"
          % (s_adv, spec["ink"], c_adv, ctrl["ink"]))
    ck(spec["ink"] > ctrl["ink"] + ctrl["ink"] // 20,
       "the gate line (abc <b>abc</b> <strong>def</strong> font-weight:700 ghi"
       " <code>x<b>y</b></code>) leaves at least 5% more ink than the same"
       " words unmarked")

    print("\nALL PASS -- artefacts in %s" % tmp)
finally:
    proc.kill()
