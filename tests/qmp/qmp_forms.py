#!/usr/bin/env python3
"""Can a human type into a search box on this machine? Answer it with a screendump.

    python3 tests/qmp/qmp_forms.py <iso> <disk.img> [--expect-no-focus]

WHY THIS TEST AND NOT A HOST TEST. The defect being fixed is not "text fields
render badly". It is that browser.c had no focus model, so every keystroke went
to <body> and NO WEB PAGE ON THIS MACHINE COULD ACCEPT A CHARACTER. That is a
statement about the keyboard, the window and the painter together, and a host
test cannot contradict it -- tests/unit/forms_test.c can prove that
fc_edit_insert() changes a string, which was never the part in doubt.

So this drives the real machine over QMP: launch the Browser, load a page with a
real <form>, CLICK the field, type into it with the emulated PS/2 keyboard, and
then assert on three independent channels, because any one of them alone can
lie:

  1. THE DOM. The page's own `input` listener logs the value over serial. This
     proves the keystroke reached the element and that the event fired -- a page
     whose JavaScript watches its search box (which is all of them) sees it.

  2. THE PIXELS. The field is drawn with a deliberately odd border colour, so
     the harness can find its box in a screendump without OCR and count the dark
     pixels INSIDE it. Typing five characters must make ink appear where there
     was none. A value that is in the DOM but not on screen is not a fixed bug;
     it is a different one.

  3. THE NETWORK. Enter submits the form, and the harness's own HTTP server
     records the request line. `GET /search?q=hello` arriving is the whole
     feature, end to end: focus -> keystroke -> value -> implicit submission ->
     urlencoded query -> navigation -> a request a real server would answer.

It also exercises the rest of the model, on the same page and in the same run:
Tab moves focus between fields, clicking a <label> ticks the checkbox it labels,
and the checkbox's `change` event fires.

THE NEGATIVE CONTROL. `--expect-no-focus`, run against a browser.aex built with
-DBROWSER_NO_FOCUS (make test-forms-negctl). That build is the behaviour that
shipped yesterday -- keys to <body>, no element takes focus -- so every
assertion above must be FALSE, while the page still LOADS and still PAINTS. If
the control passes, this test is not measuring the focus model and none of its
numbers mean anything.
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
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
CONTROL = "--expect-no-focus" in sys.argv[3:]
# --contenteditable drives the SAME three channels at a composer instead of an
# <input>: the DOM (the page reads its own textContent back), the pixels (ink
# inside the box's border), and -- standing in for the network, because a
# composer does not submit anything -- the `input` event's inputType, which is
# what a React composer actually reads and the one channel that can be right
# while both of the others are.
CE = "--contenteditable" in sys.argv[3:]

# Deliberately odd colours: nothing in the chrome, the wallpaper or the page can
# collide with them, so find_color() locates a control without OCR.
FIELD_EDGE = (1, 254, 2)        # the search box's border
BOX2_EDGE = (254, 1, 2)         # the second field's border (for Tab)
ANCHOR = (254, 1, 254)          # a plain block: "the page rendered at all"

TYPED = "hello"

PAGE = """<!doctype html>
<html><head><title>forms</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
#anchor { background: #fe01fe; width: 240px; height: 30px; }
#q  { font-size: 24px; width: 300px; background: #ffffff;
      border: 3px solid #01fe02; color: #000000; }
#q2 { font-size: 24px; width: 200px; background: #ffffff;
      border: 3px solid #fe0102; color: #000000; }
form { display: block; margin: 12px 0; }
label { font-size: 20px; }
</style></head><body>
<div id="anchor">ANCHOR</div>
<form id="f" action="/search" method="get">
  <input id="q" name="q" autocomplete="off"><br>
  <input id="q2" name="r"><br>
</form>
<label id="lab" for="cb">TICKBOX</label><input id="cb" type="checkbox" name="cb" form="f">
<input type="hidden" name="nope" value="x">
<script>
var q = document.getElementById('q');
var q2 = document.getElementById('q2');
var cb = document.getElementById('cb');
q.addEventListener('focus', function () {
  console.log('FORMS-FOCUS ' + (document.activeElement ? document.activeElement.id : '?'));
});
q.addEventListener('input', function () { console.log('FORMS-INPUT ' + q.value); });
q2.addEventListener('focus', function () { console.log('FORMS-FOCUS2 ' + q2.id); });
cb.addEventListener('change', function () { console.log('FORMS-CHECK ' + cb.checked); });
/* Diagnostics, so a failure says WHICH stage broke rather than only that the
   checkbox did not tick. The label failure was chased for a whole QEMU cycle
   on the strength of "no FORMS-CHECK"; with these three lines the run says
   whether the pointer produced a mousedown, whether it produced a mouseup, and
   whether the two agreed enough to make a click -- which is where it broke. */
document.addEventListener('mousedown', function (e) {
  console.log('FORMS-MD ' + (e.target ? (e.target.id || e.target.tagName) : '?'));
});
document.addEventListener('mouseup', function (e) {
  console.log('FORMS-MU ' + (e.target ? (e.target.id || e.target.tagName) : '?'));
});
document.addEventListener('click', function (e) {
  console.log('FORMS-CLICK ' + (e.target ? (e.target.id || e.target.tagName) : '?'));
});
document.getElementById('lab').addEventListener('click', function () {
  console.log('FORMS-LABCLICK');
});
cb.addEventListener('click', function () { console.log('FORMS-CBCLICK ' + cb.checked); });
document.getElementById('f').addEventListener('submit', function () {
  console.log('FORMS-SUBMIT ' + q.value);
});
/* Report the BINDINGS separately from the typing: if `value` is missing the
   run should say so rather than looking like a keyboard failure. */
console.log('FORMS-READY ' + (typeof q.value) + ' ' + (typeof q.focus) + ' ' +
            (typeof q.setSelectionRange) + ' ' + (cb.checked === false));
</script>
</body></html>
"""

# The composer page. A `contenteditable` div, styled exactly like the <input>
# above so the pixel channel is measured the same way -- an odd border colour to
# find the box without OCR, a white interior, black text.
#
# It deliberately does NOT intercept Enter. A real chat composer does (that is
# how the message is sent), and browser.c honours the cancellation because the
# page's keydown runs first -- but then the harness would be measuring the
# page's own handler rather than the editing model. So Enter here does what an
# uncancelled Enter is supposed to do, and the interception is asserted
# separately below by cancelling ONE beforeinput and requiring nothing to
# change.
CE_EDGE = (1, 254, 2)
CE_PAGE = """<!doctype html>
<html><head><title>ce</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
#anchor { background: #fe01fe; width: 240px; height: 30px; }
#c { font-size: 24px; width: 420px; min-height: 90px; background: #ffffff;
     border: 3px solid #01fe02; color: #000000; padding: 4px; }
</style></head><body>
<div id="anchor">ANCHOR</div>
<div id="c" contenteditable></div>
<script>
var c = document.getElementById('c');
c.addEventListener('focus', function () {
  console.log('CE-FOCUS ' + (document.activeElement ? document.activeElement.id : '?'));
});
c.addEventListener('beforeinput', function (e) {
  console.log('CE-BEFORE ' + e.inputType);
  /* A composer that cancels an edit, which is the mechanism a chat page uses
     to make Enter send instead of break the line. Cancelling by inputType is
     what makes this assertable from a harness that cannot call into the page:
     Shift+Enter is refused and nothing else is. */
  if (e.inputType === 'insertLineBreak') { e.preventDefault(); console.log('CE-VETOED'); }
});
c.addEventListener('input', function (e) {
  /* THE inputType IS LOGGED SEPARATELY FROM THE TEXT, because they are two
     different claims: "the characters arrived" and "the page was told what
     kind of change it was". A composer managed by a framework needs both. */
  console.log('CE-INPUT [' + e.inputType + '] ' + JSON.stringify(c.textContent));
  var s = document.getSelection();
  console.log('CE-SEL n=' + (s.anchorNode ? s.anchorNode.nodeType : '-') +
              ' off=' + s.anchorOffset + ' collapsed=' + s.isCollapsed +
              ' ranges=' + s.rangeCount + ' txt=' + JSON.stringify(String(s)));
  var r = document.createRange();
  r.selectNodeContents(c);
  console.log('CE-RANGE ' + JSON.stringify(r.toString()) + ' blocks=' + c.children.length);
});
window.__veto = function (v) { veto = v; };
console.log('CE-READY ' + (typeof document.getSelection) + ' ' +
            (typeof document.createRange) + ' ' + (c.isContentEditable === true));
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_forms_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

# Every request line the guest makes, so the SUBMISSION can be asserted against
# what a server actually received rather than against the browser's own report.
REQUESTS = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        REQUESTS.append(self.path)
        if self.path.startswith("/search"):
            body = ("<!doctype html><html><body style='background:#01fe02'>"
                    "<p>SUBMITTED</p></body></html>").encode()
        elif self.path.startswith("/ce"):
            body = CE_PAGE.encode()
        else:
            body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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
    print("----- requests seen: %r" % (REQUESTS,))
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


def wait_request(prefix, secs):
    end = time.time() + secs
    while time.time() < end:
        for r in REQUESTS:
            if r.startswith(prefix):
                return r
        time.sleep(0.25)
    return None


def slow_type(ui, text):
    """One key at a time, slowly. The emulated PS/2 controller holds ONE byte:
    a burst is silently dropped, which looks exactly like a browser that
    ignores the keyboard -- i.e. like the bug under test."""
    for ch in text:
        ui.key(ch, settle=0.30)


def interior(box, inset=6):
    x0, y0, x1, y1 = box
    return (x0 + inset, y0 + inset, x1 - inset, y1 - inset)


def dark_bbox(p, box, thresh=90):
    """The bounding box of every dark pixel in `box`, or None.

    first_dark() gives the FIRST such pixel, which for a word of text is the
    top-left tip of its first glyph -- and that is the wrong thing to click.
    See the note at the label click for what it cost."""
    x0, y0, x1, y1 = box
    lx = ly = 1 << 30
    hx = hy = -1
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = p.at(x, y)
            if (r + g + b) / 3 < thresh:
                if x < lx: lx = x
                if x > hx: hx = x
                if y < ly: ly = y
                if y > hy: hy = y
    return None if hx < 0 else (lx, ly, hx, hy)


def browser_frame():
    """The Browser window's frame, from the guest's own report on serial.

    Needed because the WM grabs a RESIZE at the window's edge and never passes
    that press to the app, so a click a couple of pixels inside the frame is
    swallowed -- silently, and looking exactly like an app that ignores the
    mouse."""
    m = None
    for line in serial().splitlines():
        g = re.match(r"\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+).*Browser", line)
        if g:
            m = tuple(int(v) for v in g.groups())
    return m


try:
    if not wait_serial("LOGIT_BOOT_OK", 180, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 60, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path)
    ui.click_at(*dock_icon(BROWSER_SLOT))
    for _ in range(4):
        if wait_serial("launched Browser", 15, "browser launch"):
            break
        ui.click_at(*dock_icon(BROWSER_SLOT))
    else:
        die("the Dock never launched the Browser")
    time.sleep(6)

    ui.click_at(420, 145)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/%s" % (PORT, "ce.html" if CE else "form.html"))
    ui.key("ret")

    if CE:
        # ============================ contenteditable ======================
        #
        # THE SAME THREE CHANNELS. The DOM (the page reads its own textContent
        # back), the pixels (ink inside the composer's painted border), and --
        # standing in for the network, since a composer submits nothing -- the
        # inputType on the `input` event. The third is the one that can be
        # wrong while the first two are right, and it is exactly the difference
        # between a page that looks fixed and one that works: React learns a
        # composer changed from the event, not from the DOM.
        ck(wait_serial("CE-READY", 90, "page load"),
           "the composer page loaded and its script ran")
        m = re.search(r"CE-READY (\S+) (\S+) (\S+)", serial())
        ck(m is not None, "the script reported the bindings")
        ck(m.group(1) == "function", "document.getSelection() exists")
        ck(m.group(2) == "function", "document.createRange() exists")
        ck(m.group(3) == "true", "and the div reports isContentEditable")

        time.sleep(3)
        p0 = PPM(ui.screendump(shot("ce_loaded")))
        ck(p0.find_color(ANCHOR) is not None, "the page is on screen")
        box = p0.find_color(CE_EDGE)
        ck(box is not None, "THE COMPOSER IS DRAWN -- a bordered box, found by "
                            "its colour, no OCR")
        bx0, by0, bx1, by1 = box
        print("   composer box: %r (%dx%d)" % (box, bx1 - bx0 + 1, by1 - by0 + 1))
        ink0 = p0.dark_pixels(interior(box))
        print("   ink inside the empty composer: %d px" % ink0)

        # ---- click it, then type ----
        # Aim at the TOP-LEFT quarter: the composer is 90 px tall and empty, so
        # its text will be on the first line. A click in the vertical middle
        # lands below every line there will ever be, which is a legitimate
        # click (it resolves to the end of the content) but makes the caret's
        # position harder to reason about when something goes wrong.
        cx = bx0 + (bx1 - bx0) // 4
        cy = by0 + 14
        ui.click_at(cx, cy)
        time.sleep(1.0)
        focused = wait_serial("CE-FOCUS ", 10, "focus")
        if CONTROL:
            ck(not focused, "CONTROL: clicking the composer focuses nothing")
        else:
            ck(focused, "clicking the composer FOCUSES it")
            f = re.search(r"CE-FOCUS (\S+)", serial())
            ck(f is not None and f.group(1) == "c",
               "and document.activeElement is the composer")

        mark = len(serial())
        slow_type(ui, TYPED)
        time.sleep(2.0)
        got = serial()[mark:]

        if CONTROL:
            ck("CE-INPUT" not in got,
               "CONTROL: five keystrokes into a contenteditable produced no "
               "`input` event -- which is the bug this whole line is about")
            print("\nPASS (control).")
            proc.kill()
            sys.exit(0)

        ck(("CE-INPUT [insertText] \"%s\"" % TYPED) in got,
           "THE CHARACTERS ARRIVED IN THE DOM: the page read %r back out of "
           "its own textContent" % TYPED)
        ck("CE-INPUT [insertText]" in got,
           "AND `input` CARRIED inputType=insertText -- the field a React "
           "composer reads, and the one an event can be missing while the "
           "text is on screen and everything looks fine")
        ck("CE-BEFORE insertText" in got,
           "with a `beforeinput` of the same inputType before it")
        # One event per keystroke, not one for the batch.
        n_in = len(re.findall(r"CE-INPUT \[insertText\]", got))
        ck(n_in == len(TYPED),
           "one `input` per keystroke: %d for %d characters" % (n_in, len(TYPED)))

        sel = re.findall(r"CE-SEL n=(\S+) off=(\S+) collapsed=(\S+) ranges=(\S+) txt=(\S+)", got)
        ck(len(sel) > 0, "and document.getSelection() answered from the page")
        last = sel[-1]
        print("   selection after typing: node type %s, offset %s, collapsed %s, "
              "ranges %s" % (last[0], last[1], last[2], last[3]))
        ck(last[0] == "3",
           "the caret's anchorNode is a TEXT NODE -- created by the first "
           "keystroke, because an empty composer has none")
        ck(last[1] == str(len(TYPED)),
           "at offset %d, i.e. after everything typed" % len(TYPED))
        ck(last[2] == "true" and last[3] == "1",
           "collapsed, with one range")
        rng = re.findall(r"CE-RANGE (\S+) blocks=(\d+)", got)
        ck(rng and rng[-1][0] == '"%s"' % TYPED,
           "and a Range over the composer's contents reads back %r" % TYPED)

        time.sleep(1.5)
        p1 = PPM(ui.screendump(shot("ce_typed")))
        box1 = p1.find_color(CE_EDGE) or box
        ink1 = p1.dark_pixels(interior(box1))
        print("   ink inside the composer after typing: %d px (was %d)" % (ink1, ink0))
        ck(ink1 > ink0 + 40,
           "AND THEY ARE ON SCREEN: %d -> %d dark pixels inside the box, which "
           "is five glyphs that were not there before" % (ink0, ink1))

        # ---- Enter splits the block ----
        mark = len(serial())
        ui.key("ret", settle=0.8)
        time.sleep(1.5)
        got = serial()[mark:]
        ck("CE-INPUT [insertParagraph]" in got,
           "Enter reports inputType=insertParagraph, not insertText -- the two "
           "are different edits and a composer branches on which")
        slow_type(ui, "there")
        time.sleep(1.5)
        got = serial()[mark:]
        rng = re.findall(r"CE-RANGE (\S+) blocks=(\d+)", got)
        ck(rng and int(rng[-1][1]) >= 2,
           "and the composer now holds TWO block children, not one: Enter "
           "SPLIT the paragraph (blocks=%s)" % (rng[-1][1] if rng else "?"))
        ck(rng and rng[-1][0] == '"%sthere"' % TYPED,
           "with all the text still there and in order (%s)"
           % (rng[-1][0] if rng else "?"))

        # ---- backspace ----
        mark = len(serial())
        for _ in range(2):
            ui.key("backspace", settle=0.4)
        time.sleep(1.5)
        got = serial()[mark:]
        ck("CE-INPUT [deleteContentBackward]" in got,
           "Backspace reports deleteContentBackward")
        rng = re.findall(r"CE-RANGE (\S+) blocks=(\d+)", got)
        ck(rng and rng[-1][0] == '"%sthe"' % TYPED,
           "and two characters are gone (%s)" % (rng[-1][0] if rng else "?"))

        # ---- a cancelled edit changes nothing ----
        # The mechanism a chat page uses to make Enter send instead of break a
        # line. The page cancels insertLineBreak and only that.
        mark = len(serial())
        ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": True}},
                   {"type": "key", "data": {"key": {"type": "qcode", "data": "ret"}, "down": True}}])
        ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "ret"}, "down": False}},
                   {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": False}}])
        time.sleep(1.8)
        got = serial()[mark:]
        ck("CE-BEFORE insertLineBreak" in got,
           "Shift+Enter raises beforeinput with inputType=insertLineBreak")
        ck("CE-VETOED" in got, "the page cancels it...")
        ck("CE-INPUT [insertLineBreak]" not in got,
           "...and NO `input` follows -- a cancelled edit that still reported "
           "one would be a page told its content changed when it did not")

        print("\nPASS: a human can click a contenteditable composer on a real "
              "page, type into it, see the characters, and the page's own "
              "JavaScript sees `input` with the right inputType and can read "
              "the caret back through document.getSelection().")
        proc.kill()
        sys.exit(0)

    ready = wait_serial("FORMS-READY", 90, "page load")
    ck(ready, "the page loaded and its script ran")
    m = re.search(r"FORMS-READY (\S+) (\S+) (\S+) (\S+)", serial())
    if CONTROL:
        # The control drops js_forms.o, so the bindings are absent. The page
        # still loads and still paints; that is the point of checking it.
        ck(m is None or m.group(1) != "string",
           "CONTROL: element.value is not a string (the bindings are absent)")
    else:
        ck(m is not None, "the script reported the bindings")
        ck(m.group(1) == "string", "element.value is a string")
        ck(m.group(2) == "function", "element.focus() exists")
        ck(m.group(3) == "function", "element.setSelectionRange() exists")
        ck(m.group(4) == "true", "checkbox.checked starts false")

    time.sleep(3)
    p0 = PPM(ui.screendump(shot("loaded")))
    ck(p0.find_color(ANCHOR) is not None,
       "the page is on screen (its anchor block was painted)")
    field = p0.find_color(FIELD_EDGE)
    ck(field is not None,
       "THE TEXT FIELD IS DRAWN -- an <input> now produces a painted box, "
       "which before this change it did not")
    fx0, fy0, fx1, fy1 = field
    print("   field box: %r (%dx%d)" % (field, fx1 - fx0 + 1, fy1 - fy0 + 1))
    ck(fx1 - fx0 > 100 and fy1 - fy0 > 15,
       "and it is the size the CSS asked for, not a collapsed sliver")
    ck(p0.find_color(BOX2_EDGE) is not None, "so is the second field")

    ink_before = p0.dark_pixels(interior(field))
    print("   ink inside the empty field: %d px" % ink_before)

    # ---- click the field, then type -------------------------------------
    cx = (fx0 + fx1) // 2
    cy = (fy0 + fy1) // 2
    ui.click_at(cx, cy)
    time.sleep(1.0)
    focused = wait_serial("FORMS-FOCUS ", 10, "focus")
    if CONTROL:
        ck(not focused, "CONTROL: clicking the field focuses nothing")
    else:
        ck(focused, "clicking the field FOCUSES it (a `focus` event fired)")
        f = re.search(r"FORMS-FOCUS (\S+)", serial())
        ck(f is not None and f.group(1) == "q",
           "and document.activeElement is that element")

    slow_type(ui, TYPED)
    time.sleep(1.5)

    if CONTROL:
        ck("FORMS-INPUT" not in serial(),
           "CONTROL: five keystrokes produced no `input` event -- the keys went "
           "to <body>, which is exactly the bug")
    else:
        ck(wait_serial("FORMS-INPUT " + TYPED, 20, "the typed value"),
           "THE CHARACTERS ARRIVED: the page's `input` listener read back "
           "%r from element.value" % TYPED)

    time.sleep(1.5)
    p1 = PPM(ui.screendump(shot("typed")))
    field1 = p1.find_color(FIELD_EDGE)
    ck(field1 is not None, "the field is still on screen after typing")
    ink_after = p1.dark_pixels(interior(field1))
    print("   ink inside the field after typing: %d px (was %d)" % (ink_after, ink_before))
    if CONTROL:
        ck(ink_after <= ink_before + 12,
           "CONTROL: nothing was painted into the field (%d -> %d px) -- the "
           "keystrokes reached no element" % (ink_before, ink_after))
    else:
        ck(ink_after > ink_before + 40,
           "AND THEY ARE ON SCREEN: the field went from %d to %d dark pixels, "
           "which is five glyphs of ink that were not there before"
           % (ink_before, ink_after))

    # ---- Tab moves focus -------------------------------------------------
    ui.key("tab", settle=0.6)
    time.sleep(1.0)
    if CONTROL:
        ck("FORMS-FOCUS2" not in serial(), "CONTROL: Tab moved no focus")
    else:
        ck(wait_serial("FORMS-FOCUS2", 10, "Tab"),
           "Tab moved focus to the next field, in DOM order")

    # ---- the label ticks its checkbox ------------------------------------
    # The label is TEXT, so aim at a glyph rather than at the middle of a band.
    p2 = PPM(ui.screendump(shot("prelabel")))
    # Aim BELOW the second field, not below the first: after Tab the second
    # field is focused and carries a caret, and first_dark() would find that
    # one-pixel caret and click the field again. Locating the band from the
    # second field's own painted box is the only way to be sure the click lands
    # on the label -- a harness that clicks the wrong thing reports a product
    # bug that does not exist.
    #
    # AND AIM AT THE MIDDLE OF THE WORD, not at its first dark pixel. The label
    # sits at page x=0, i.e. flush against the browser window's left edge, and
    # the WM claims a band a few pixels wide there as a RESIZE GRAB: a press
    # inside it is consumed by the compositor and never delivered to the app,
    # while the RELEASE still is (by design -- see the note above the release
    # loop in wm.c). The guest therefore saw a mouseup with no mousedown, made
    # no `click` out of the pair, and the checkbox did not tick. That is a
    # harness aiming at the frame, and it cost a full QEMU cycle to tell apart
    # from a browser that ignores clicks. The bbox centre is inside the word;
    # the assertion below refuses to run at all if it is not inside the window.
    b2 = p2.find_color(BOX2_EDGE) or field
    lab_band = (b2[0], b2[3] + 8, b2[0] + 300, b2[3] + 90)
    bb = dark_bbox(p2, lab_band)
    hit = None
    if bb:
        hit = ((bb[0] + bb[2]) // 2, (bb[1] + bb[3]) // 2)
        fr = browser_frame()
        if fr:
            ck(hit[0] > fr[0] + 8 and hit[0] < fr[0] + fr[2] - 8,
               "the label click lands in the window's CONTENT, clear of the "
               "resize band (x=%d, window x=%d..%d)" % (hit[0], fr[0], fr[0] + fr[2]))
        print("   label ink %r -> clicking %r" % (bb, hit))
        ui.click_at(hit[0], hit[1])
        time.sleep(1.2)
    if CONTROL:
        ck("FORMS-CHECK" not in serial(), "CONTROL: clicking the label ticked nothing")
    else:
        ck(hit is not None, "the <label> was found on screen")
        ck(wait_serial("FORMS-CHECK true", 10, "the checkbox"),
           "clicking the LABEL ticked the checkbox it labels and fired `change`")

    # ---- Enter submits ----------------------------------------------------
    # Focus is on the second field after Tab; go back to the first so the
    # implicit submission is from the field that holds the typed text.
    ui.click_at(cx, cy)
    time.sleep(0.8)
    ui.key("ret", settle=0.6)
    got = wait_request("/search", 45)
    if CONTROL:
        ck(got is None,
           "CONTROL: Enter submitted nothing -- the server saw no /search request")
        print("\nPASS (control): with the focus routing compiled out, every "
              "assertion the positive run makes is false. The page still loaded "
              "and still painted, so the absence is about the focus model.")
        proc.kill()
        sys.exit(0)

    ck(got is not None,
       "Enter SUBMITTED THE FORM -- the harness's own HTTP server received the "
       "request")
    print("   the server received: %s" % got)
    ck(got.startswith("/search?"),
       "to the form's action, with a query string (method=get)")
    ck("q=" + TYPED in got,
       "carrying the typed value under the field's name (q=%s)" % TYPED)
    # FORM ASSOCIATION, both directions, and the first version of this test got
    # it backwards. The checkbox lives OUTSIDE <form id=f> and is submitted only
    # because it carries form="f"; the hidden input beside it carries no such
    # attribute and must therefore be absent, however ordinary it looks. An
    # earlier run demanded cb=on from a checkbox with no association at all and
    # read the browser's correct refusal as a bug.
    ck("cb=on" in got,
       "and the ticked checkbox -- which is OUTSIDE the form and reaches it "
       "only through form=\"f\" -- as the HTML default value 'on'")
    ck("nope=" not in got,
       "while the control outside the form with NO form= attribute is not "
       "submitted at all")
    ck("r=" in got, "and the empty second field, which a form still submits")

    time.sleep(3)
    p3 = PPM(ui.screendump(shot("submitted")))
    ck(p3.find_color((1, 254, 2)) is not None,
       "and the browser NAVIGATED to the response -- the result page is painted")

    print("\nPASS: a human can click a text field on a real page, type into it, "
          "see the characters, and press Enter to submit the form to a server.")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
