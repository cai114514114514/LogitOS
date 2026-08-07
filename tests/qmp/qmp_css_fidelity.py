#!/usr/bin/env python3
"""Prove, on the real machine, that the CSS properties layout newly reads are
actually reaching the screen.

    python3 tests/qmp/qmp_css_fidelity.py <iso> <disk.img>

Host unit tests assert on the display list, which is one step short of the
claim: a box can have the right geometry in `struct item` and still be painted
wrong, and a `struct cstyle` field can be read correctly and then dropped on the
floor between layout and paint. This boots the OS, serves a fixture page from
the host over SLIRP, loads it in the Browser and measures the PIXELS.

Every block is painted an exact, deliberately odd colour so the harness can
locate it in a screendump without OCR or hardcoded layout coordinates. Three
properties are under test, each chosen because it was completely absent before:

  border-box   two boxes with identical `width:300px;padding:20px;border:5px`,
               one content-box and one border-box. Their painted widths must
               DIFFER by exactly 50px (2*20 padding + 2*5 border). Before, both
               were painted 300 wide -- content-box lost its padding.

  <pre>        four lines of preformatted text, one of them blank. The block's
               painted height must cover four line boxes, and the text pixels
               must form four distinct rows. Before, all of it collapsed onto a
               single line.

  flex wrap    three 200px items in a 500px row with `flex-wrap:wrap` and
               `justify-content:flex-end`. Two must share the first row, the
               third must be on a second row, and the first row's leftmost item
               must be pushed right by the 100px of slack. Before, flex was a
               single unwrapped row that ignored justify-content entirely.

A second fixture covers the PAINTER-side properties, which have the same
"correct in the display list, wrong on the screen" failure mode:

  alpha        a `rgba(254,0,0,.5)` veil over an opaque `#0000fe` plate. The
               overlapped pixels must be the arithmetic blend (~127,0,~127) and
               the opaque red must appear NOWHERE. Before, rgba painted solid.

  decoration   three single-word runs, one per text-decoration line, each in its
               own colour. Each must show a row of solid colour spanning the
               whole run -- at the TOP of the box for overline, mid-box for
               line-through, below it for underline. Before, only underline was
               drawn at all.

  float        a 120x80 left float inside a 420px block of text. Every glyph in
               the float's vertical band must be to the RIGHT of it, and the
               text must return to the left edge below it. Before, float was
               read into the cstyle and ignored.

A third fixture covers CUSTOM PROPERTIES, where the failure was not a missing
box but a wrong COLOUR -- so the assertions are colours, and the wrong answer
paints a different, equally findable one:

  theme        `--card` is #01fe02 at :root and #fa0102 inside
               `@media (prefers-color-scheme: dark)`, and `--page-bg` likewise.
               Before, the var() pre-pass could not see @media at all and took
               the last declaration in the file, so EVERY page rendered in its
               dark theme. Measured on the pre-change browser: the card painted
               #fa0102 at 240x120 and the viewport painted #050403 at 1180x572;
               with the fix the same two boxes are #01fe02 and #fefdfc and
               neither dark colour appears anywhere on screen.

  switch       `--sw` is #01fafa at :root and #fa01fa on `html.theme-night`,
               which this document does not carry. The night value is both more
               specific and later, so the cascade alone picks it -- and did:
               the pre-change browser painted #fa01fa. A class/id/attribute-
               gated declaration is a switch nobody threw, and must lose.

  late         `--late` is declared AFTER two @counter-style blocks, whose
               bodies hold bare descriptors rather than nested rules. This one
               guards the REPLACEMENT scanner rather than the old one: it used
               to swallow such a block's closing brace without leaving the
               at-rule and then collect nothing from the rest of the sheet (5
               custom properties out of wikipedia's 193). If that regresses,
               `--late` is never collected and the box paints its var()
               fallback #123456, a colour the correct render never produces.

Every fixture colour is unique across all three pages, and the third fixture
first asserts that the second fixture's float is GONE: nearly every check below
is "colour X is absent", which a navigation that silently failed would satisfy
all at once.

The screendumps are kept in the temp directory named at the end so a failure can
be looked at rather than guessed at.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# Odd values so nothing in the desktop chrome or the font antialiasing can
# collide with them.
CONTENT_BOX = (254, 1, 2)      # #fe0102
BORDER_BOX = (2, 254, 1)       # #02fe01
PRE_BG = (1, 2, 254)           # #0102fe
FLEX_A = (254, 1, 254)         # #fe01fe
FLEX_B = (1, 254, 254)         # #01fefe
FLEX_C = (254, 254, 1)         # #fefe01

PAGE = """<!doctype html>
<html><head><title>cssfid</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
/* Same authored width, same padding, same border, different box-sizing. */
.box { width: 300px; padding: 20px; border: 5px solid #000000; }
#cb { box-sizing: content-box; background: #fe0102; }
#bb { box-sizing: border-box;  background: #02fe01; }
/* Preformatted: newlines, runs of spaces and a blank line all preserved. */
#pre { background: #0102fe; font-size: 20px; margin: 0; }
/* A wrapping, end-justified flex row. */
#row { display: flex; width: 500px; flex-wrap: wrap;
       justify-content: flex-end; margin: 0; }
#row > div { width: 200px; height: 40px; flex-shrink: 0; }
#fa { background: #fe01fe; }
#fb { background: #01fefe; }
#fc { background: #fefe01; }
</style></head><body>
<div id="cb" class="box">CB</div>
<div id="bb" class="box">BB</div>
<pre id="pre">AAAA
  BBBB

CCCC</pre>
<div id="row"><div id="fa"></div><div id="fb"></div><div id="fc"></div></div>
</body></html>
"""

# ---- second fixture: the painter-side properties ----
PLATE = (0, 0, 254)            # #0000fe, the opaque backdrop for the alpha veil
VEIL = (254, 0, 0)             # #fe0000, the 50% overlay -- must never appear raw
OVER_C = (2, 3, 254)           # #0203fe  overline
STRIKE_C = (254, 2, 3)         # #fe0203  line-through
UNDER_C = (2, 254, 3)          # #02fe03  underline
FLOAT_C = (254, 1, 253)        # #fe01fd  the floated block
WRAP_BG = (253, 253, 254)      # #fdfdfe  the block whose text wraps around it

# Enough words that the text runs several lines past the 80px-tall float.
WORDS = ("alpha bravo charlie delta echo foxtrot golf hotel india juliet "
         "kilo lima mike november oscar papa quebec romeo sierra tango "
         "uniform victor whiskey xray yankee zulu ") * 3

PAGE2 = """<!doctype html>
<html><head><title>paintfid</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
/* An opaque plate with a half-transparent veil over its top half. */
#plate { background: #0000fe; width: 300px; height: 80px; }
#veil  { background: rgba(254,0,0,0.5); width: 300px; height: 40px; }
/* One single-word run per decoration line, each in its own colour so the
   harness can isolate it by exact pixel match. */
p.d { margin: 0; font-size: 24px; }
#ov { color: #0203fe; text-decoration: overline; }
#st { color: #fe0203; text-decoration: line-through; }
#un { color: #02fe03; text-decoration: underline; }
/* A left float with text wrapping beside and then below it. */
#wrap { background: #fdfdfe; width: 420px; }
#fl { float: left; width: 120px; height: 80px; background: #fe01fd; }
</style></head><body>
<div id="plate"><div id="veil"></div></div>
<p class="d" id="ov">OVERLINE</p>
<p class="d" id="st">STRIKE</p>
<p class="d" id="un">UNDER</p>
<div id="wrap"><div id="fl"></div>%s</div>
</body></html>
""" % WORDS

# ---- third fixture: custom properties, in the pixels ----
#
# The reported bug was a COLOUR, so the assertion has to be a colour. Every box
# here is painted through a var() whose value the cascade has to choose between
# two candidates, and the wrong choice paints a different, equally findable
# colour -- so a failure says WHICH rule won, not merely that something is off.
#
#   theme    `--card` is #01fe02 at :root and #fa0102 inside
#            `@media (prefers-color-scheme: dark)`. The browser renders for
#            light, so the card must be green and #fa0102 must appear NOWHERE.
#            Before, the pre-pass could not see @media and took the last
#            declaration in the file: the card was red and the page black.
#
#   switch   `--sw` is #01fafa at :root and #fa01fa on `html.theme-night`,
#            which this document does not carry. The night value is more
#            specific and later, so the cascade alone would pick it; it must
#            still lose, because a class-gated declaration is a switch nobody
#            threw. This is wikipedia's second disguise of the same bug.
#
#   late     `--late` is declared AFTER two @counter-style blocks, whose bodies
#            hold bare descriptors rather than nested rules. The scanner used to
#            swallow their closing brace without leaving the at-rule, and
#            collected nothing from the rest of the sheet -- five custom
#            properties out of wikipedia's 193. If that happens here `--late` is
#            never collected and the box paints its var() FALLBACK, #123456,
#            which is a colour the correct render never produces.
LIGHT_CARD = (1, 254, 2)        # #01fe02  the light theme's card
DARK_CARD = (250, 1, 2)         # #fa0102  the dark theme's -- must not appear
LIGHT_BG = (254, 253, 252)      # #fefdfc  the light page background
DARK_BG = (5, 4, 3)             # #050403  the dark one -- must not appear
SWITCH_BASE = (1, 250, 250)     # #01fafa  base value of --sw
SWITCH_NIGHT = (250, 1, 250)    # #fa01fa  the class-gated override
LATE_OK = (250, 250, 2)         # #fafa02  a var declared after the at-rules
LATE_FALLBACK = (18, 52, 86)    # #123456  the var() fallback = "never collected"

PAGE3 = """<!doctype html>
<html><head><title>varfid</title><style>
:root { --page-bg: #fefdfc; --card: #01fe02; --sw: #01fafa; }
@media (prefers-color-scheme: dark) {
  :root { --page-bg: #050403; --card: #fa0102; }
}
html.theme-night { --sw: #fa01fa; }
@counter-style fidone { system: numeric; symbols: '0' '1'; }
@counter-style fidtwo { system: numeric; symbols: '2' '3'; }
:root { --late: #fafa02; }
html, body { background: var(--page-bg); margin: 0; padding: 0; }
#card { background: var(--card); width: 240px; height: 120px; }
#sw   { background: var(--sw);   width: 240px; height: 60px;  }
#late { background: var(--late, #123456); width: 240px; height: 60px; }
</style></head><body>
<div id="card"></div><div id="sw"></div><div id="late"></div>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_cssfid_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        if "var" in self.path:
            raw = PAGE3.encode()
        else:
            raw = (PAGE2 if "paint" in self.path else PAGE).encode()
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
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-4000:])
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


def row_runs(img, box, colour):
    """(count, y) of the row inside `box` holding the most pixels of exactly
    `colour`. A decoration line is a solid rect, so its row wins outright over
    any row of anti-aliased glyph stems."""
    x0, y0, x1, y1 = box
    best, besty = 0, y0
    for y in range(y0, y1 + 1):
        c = 0
        for x in range(x0, x1 + 1):
            if img.at(x, y) == colour:
                c += 1
        if c > best:
            best, besty = c, y
    return best, besty


def count_where(img, box, pred):
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if pred(img.at(x, y)):
                n += 1
    return n


def leftmost_dark(img, box, y0, y1, thresh=100):
    """Smallest x of a near-black pixel in rows [y0,y1] of `box`, or None."""
    bx0, _, bx1, _ = box
    best = None
    for y in range(max(0, y0), min(img.h - 1, y1) + 1):
        for x in range(bx0, bx1 + 1):
            r, g, b = img.at(x, y)
            if r < thresh and g < thresh and b < thresh:
                if best is None or x < best:
                    best = x
                break
    return best


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)                          # desktop + dock composited

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    time.sleep(3.0)                        # ~2.7 MB .aex off virtio-blk, then ELF load
    ui.screendump(os.path.join(tmp, "launch.ppm"), settle=0.4)

    ui.click_at(420, 145)                  # address bar
    for _ in range(60):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/cssfid.html" % PORT)
    ui.key("ret")
    time.sleep(10)
    shot = os.path.join(tmp, "page.ppm")
    ui.screendump(shot)
    img = PPM(shot)

    # ---- box-sizing ----
    cb = block(img, CONTENT_BOX, "content-box")
    bb = block(img, BORDER_BOX, "border-box")
    cbw = cb[2] - cb[0] + 1
    bbw = bb[2] - bb[0] + 1
    print("content-box painted width %d, border-box painted width %d" % (cbw, bbw))
    # The background is painted inside the border, so both measured runs are
    # the border box minus the two 5px borders. The DIFFERENCE is what matters
    # and it is exactly the padding+border content-box adds: 50px.
    ck(cbw - bbw == 50,
       "content-box box is exactly 50px wider than the identical border-box one")
    ck(bbw == 300 - 10,
       "border-box painted the authored 300px (less its two 5px borders)")

    # ---- <pre> ----
    pre = block(img, PRE_BG, "pre")
    preh = pre[3] - pre[1] + 1
    # Rows inside the block that contain near-black text pixels, grouped into
    # runs: one run per line of text. The blank line must leave a gap.
    rows = []
    for y in range(pre[1], pre[3] + 1):
        dark = 0
        for x in range(pre[0], pre[2] + 1):
            r, g, b = img.at(x, y)
            if r < 100 and g < 100 and b < 100:
                dark += 1
        rows.append(dark > 0)
    runs = 0
    for i, on in enumerate(rows):
        if on and (i == 0 or not rows[i - 1]):
            runs += 1
    print("pre block height %d px, %d text rows" % (preh, runs))
    ck(runs == 3, "pre painted 3 rows of text (AAAA / BBBB / CCCC), not one")
    # 20px font -> 25px line box; four line boxes including the blank one.
    ck(preh >= 90, "pre block is tall enough for four line boxes (blank line kept)")

    # ---- flex wrap + justify-content ----
    fa = block(img, FLEX_A, "flex item A")
    fb = block(img, FLEX_B, "flex item B")
    fc = block(img, FLEX_C, "flex item C")
    print("flex A %s  B %s  C %s" % (fa, fb, fc))
    ck(abs(fa[1] - fb[1]) <= 2, "flex items A and B share the first row")
    ck(fc[1] > fa[3], "flex item C wrapped onto a second row")
    ck(abs((fb[0] - fa[0]) - 200) <= 2, "flex items are 200px apart on the row")
    # 500 - 400 = 100px of slack, pushed to the LEFT of A by justify-content:flex-end
    ck(fa[0] - pre[0] >= 90, "justify-content:flex-end pushed the first row right by ~100px")
    ck(abs(fc[2] - fb[2]) <= 2, "the wrapped row is flex-end aligned too")

    # ================= second fixture: the painter =================
    # y=175, not the 145 used above: the first click happens while the window is
    # still playing its open-pop animation and the bar is higher up. By now the
    # window has settled and 145 is the title bar.
    ui.click_at(420, 175)                  # address bar
    for _ in range(80):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/paint.html" % PORT)
    ui.key("ret")
    time.sleep(10)
    shot2 = os.path.join(tmp, "paint.ppm")
    ui.screendump(shot2)
    img = PPM(shot2)

    # ---- background-color alpha ----
    # find_color sees only the UNVEILED half of the plate, because the veiled
    # half is no longer #0000fe -- which is already half the proof. The veil sits
    # directly above it and is the same size, so that band is where the blend
    # must be, pixel for pixel.
    pure = block(img, PLATE, "unveiled half of the plate")
    pw = pure[2] - pure[0] + 1
    ph = pure[3] - pure[1] + 1
    veil_box = (pure[0], pure[1] - ph, pure[2], pure[1] - 1)
    ck(img.find_color(VEIL) is None,
       "the rgba(254,0,0,.5) veil never painted its raw colour anywhere")
    ck(abs(pw - 300) <= 2 and abs(ph - 40) <= 2,
       "exactly half the 300x80 plate survived as its own colour")
    # 50% of (254,0,0) over (0,0,254) is (~127, 0, ~127): green stays 0, and the
    # other two channels land halfway. Integer rounding puts each at 126 or 127.
    blended = count_where(img, veil_box,
                          lambda p: p[1] == 0 and 118 <= p[0] <= 136 and 118 <= p[2] <= 136)
    print("plate: unveiled %s (%dx%d); veiled band %s -> %d/%d blended px, sample %s"
          % (pure, pw, ph, veil_box, blended, pw * ph,
             img.at(pure[0] + 4, pure[1] - 4)))
    ck(blended >= pw * ph * 95 // 100,
       "the veiled band is the arithmetic blend of the veil over the plate")

    # ---- text-decoration ----
    for colour, name, lo, hi in ((OVER_C, "overline", 0.0, 0.25),
                                 (STRIKE_C, "line-through", 0.30, 0.70),
                                 (UNDER_C, "underline", 0.75, 1.0)):
        box = block(img, colour, name)
        w = box[2] - box[0] + 1
        h = box[3] - box[1] + 1
        run, ry = row_runs(img, box, colour)
        frac = (ry - box[1]) / float(h - 1) if h > 1 else 0.0
        print("%-13s box %s  solid row %d/%d px at %.2f of the box height"
              % (name, box, run, w, frac))
        ck(run >= w * 9 // 10,
           "%s is drawn as a solid line across the whole run" % name)
        ck(lo - 0.06 <= frac <= hi + 0.06,
           "%s sits at the right height inside the text box" % name)

    # ---- float ----
    fl = block(img, FLOAT_C, "left float")
    wrap = block(img, WRAP_BG, "wrapping block")
    flw, flh = fl[2] - fl[0] + 1, fl[3] - fl[1] + 1
    print("float %s (%dx%d)  wrap %s" % (fl, flw, flh, wrap))
    ck(abs(flw - 120) <= 2 and abs(flh - 80) <= 2,
       "the float painted at its declared 120x80")
    ck(fl[0] - wrap[0] <= 2, "the float sits against the block's left content edge")
    beside = leftmost_dark(img, wrap, fl[1], fl[3])
    below = leftmost_dark(img, wrap, fl[3] + 4, wrap[3])
    print("leftmost glyph beside the float: %s   below it: %s" % (beside, below))
    ck(beside is not None and beside > fl[2],
       "every glyph in the float's band is to the RIGHT of the float")
    ck(below is not None and below < fl[0] + 8,
       "text returns to the block's left edge below the float")
    ck(below is not None and beside is not None and beside - below > 100,
       "the two measures differ by the float's width, i.e. the lines really narrowed")

    # ================= third fixture: custom properties =================
    ui.click_at(420, 175)                  # address bar
    for _ in range(80):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/var.html" % PORT)
    ui.key("ret")
    time.sleep(10)
    shot3 = os.path.join(tmp, "var.ppm")
    ui.screendump(shot3)
    img = PPM(shot3)

    # Prove we are looking at THIS page before reading anything off it. Almost
    # every assertion below is "colour X is absent", and a navigation that
    # silently failed would leave the previous fixture on screen and satisfy all
    # of them at once. The float block is the previous fixture's most
    # distinctive mark; it must be gone.
    ck(img.find_color(FLOAT_C) is None,
       "the var fixture really loaded (the previous fixture's float is gone)")

    card = block(img, LIGHT_CARD, "light-theme card")
    cw = card[2] - card[0] + 1
    chh = card[3] - card[1] + 1
    print("card %s (%dx%d)  sample inside %s" % (card, cw, chh,
                                                 img.at(card[0] + 4, card[1] + 4)))
    ck(abs(cw - 240) <= 2 and abs(chh - 120) <= 2,
       "the card painted at its declared 240x120 through var(--card)")
    # The decisive pair. Neither colour appears in the correct render by
    # accident: #fa0102 and #050403 exist only inside the dark @media block.
    ck(img.find_color(DARK_CARD) is None,
       "NEGATIVE CONTROL: the dark @media block's card colour is nowhere on screen")
    ck(img.find_color(DARK_BG) is None,
       "...and neither is its page background")
    ck(img.find_color(LIGHT_BG) is not None,
       "the LIGHT page background is what got painted")

    sw = block(img, SWITCH_BASE, "theme-switch box")
    print("switch box %s  sample %s" % (sw, img.at(sw[0] + 4, sw[1] + 4)))
    ck(img.find_color(SWITCH_NIGHT) is None,
       "a class-gated (html.theme-night) override the document does not carry "
       "loses to the base value, despite being more specific and later")

    late = block(img, LATE_OK, "post-at-rule variable")
    print("late box %s  sample %s" % (late, img.at(late[0] + 4, late[1] + 4)))
    ck(img.find_color(LATE_FALLBACK) is None,
       "a variable declared AFTER two @counter-style blocks was still collected "
       "(the var() fallback never had to be used)")
    ck(abs((late[2] - late[0] + 1) - 240) <= 2,
       "...and it painted the full 240px box")

    print("\nALL PASS -- artefacts in %s" % tmp)
finally:
    proc.kill()
