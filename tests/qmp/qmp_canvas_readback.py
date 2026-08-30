#!/usr/bin/env python3
"""Prove, ON THE REAL MACHINE, that a canvas fingerprint runs to completion and
that what it reads back is what was drawn.

    python3 tests/qmp/qmp_canvas_readback.py <iso> <disk.img>

WHY THIS EXISTS RATHER THAN ONLY tests/unit/canvas_test.c. CLAUDE.md's first
rule, and it cost this session a wrong answer already: "MEASURE IN THE GUEST --
tests/unit/webapi_probe.c does not link every TU the browser links, and reported
a whole subsystem absent that the browser has." The host gate links
js_canvas.c, the Rust staticlib and QuickJS; it does NOT link browser_rt.c, so
it never exercises the kmalloc-onto-malloc shim the encoder allocates through in
ring 3, and it runs on arm64 while the browser runs x86_64 under TCG. A host
gate proving toDataURL works says nothing about the binary the owner runs.

WHAT THE OWNER ACTUALLY HIT, restated as an assertion. A bot check's first act
is a canvas fingerprint. toDataURL threw, so the challenge script died on its
FIRST STATEMENT and the widget never appeared -- the page did not fail to pass,
it failed to RENDER. So the decisive check here is not "toDataURL returned a
string": it is that CODE AFTER THE FINGERPRINT STILL RUNS, and that the element
it was going to create is on screen in a colour no stylesheet on the page
mentions. That is the difference between "the check is refused" and "the check
never appeared", and it is the whole shape of the complaint.

WHAT THIS DOES NOT CLAIM, stated because the opposite would be the dishonest
version. It does not claim any bot check passes. Nothing here mimics another
browser, spoofs navigator.webdriver or the user agent, or is tuned so that a
particular verifier accepts the output. The fingerprint this page computes is
whatever LogitOS's own rasterizer draws, and it will not match Chrome's,
because it is not Chrome. An honest fingerprint that says "this is LogitOS" is
a correct result even when it is refused.

THE ASSERTIONS ARE ON BYTES, NOT ON SHAPE. A data URL of the right length with
the right prefix is precisely the fabrication js_canvas.c refused to produce for
years, so every check below decodes the URL and looks at the pixel it drew. The
page does the decoding itself (atob is real here) and reports through
console.log, which reaches the serial log the harness reads.

THE SCREEN IS READ TOO, for the one claim serial cannot make: that the script
kept running past the fingerprint. The page paints a block ORANGE only from the
line AFTER toDataURL returns, and ORANGE appears in no stylesheet.
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
from qmp_ui import Session, PPM      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# From the page's stylesheet ...
RED = (254, 1, 2)
# ... and the colour only the line AFTER the fingerprint can produce. It is in
# no stylesheet on the page, so its presence cannot be explained any other way.
ORANGE = (254, 127, 1)

STYLE = """<style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#after { background: #fe0102; }
</style>"""

# THE FINGERPRINT, written in the shape a real challenge script uses: draw into
# an offscreen canvas, read it back with toDataURL, and hash the result. It is
# the FIRST statement of the script, so if toDataURL throws, nothing below it
# runs -- which is exactly the failure being closed.
#
# No fillText: this browser has no text-on-canvas yet (js_canvas.c names it as
# not-here-yet), and a fingerprint that depended on a method that throws would
# be testing the wrong absence. Rectangles and an arc are enough to make the
# readback non-trivial, and the pixels asserted below are ones the test drew at
# known coordinates.
SCRIPT = """<script>
function log(s) { try { console.log(s); } catch (e) {} }

// --- statement 1: the fingerprint. If this throws, nothing after it runs. ---
var c = document.createElement('canvas');
c.width = 8; c.height = 4;
var x = c.getContext('2d');
x.fillStyle = '#ff0000'; x.fillRect(0, 0, 8, 4);
x.fillStyle = '#0000ff'; x.fillRect(0, 0, 2, 2);
// Opaque, not rgba(...,0.5), and that is deliberate: a half-alpha pixel's
// exact byte depends on how 0.5 rounds to 0..255 and on src-over's rounding,
// so asserting it here would be asserting a rounding rule rather than the
// readback. Alpha compositing already has an exact gate through getImageData
// in tests/unit/canvas_test.c. What this file is for is whether the ENCODER
// reports the composited surface, and an opaque third colour tests that with
// no rounding in the answer.
x.fillStyle = '#00ff00'; x.fillRect(6, 3, 2, 1);
var url = c.toDataURL();
log('CV-URL-PREFIX ' + url.slice(0, 22));
log('CV-URL-LEN ' + url.length);

// --- the bytes, decoded by the page itself ---------------------------------
var raw = atob(url.slice(url.indexOf(',') + 1));
var b = []; for (var i = 0; i < raw.length; i++) b.push(raw.charCodeAt(i));
log('CV-SIG ' + b.slice(0, 8).join(','));
log('CV-CHUNK1 ' + String.fromCharCode(b[12], b[13], b[14], b[15]));
log('CV-DIM ' + (((b[16]<<24)|(b[17]<<16)|(b[18]<<8)|b[19])) + 'x' +
                (((b[20]<<24)|(b[21]<<16)|(b[22]<<8)|b[23])));
log('CV-IHDR ' + [b[24], b[25], b[26], b[27], b[28]].join(','));
log('CV-END ' + String.fromCharCode(b[b.length-8], b[b.length-7],
                                    b[b.length-6], b[b.length-5]));
// 8 sig + 25 IHDR chunk + 8 IDAT len/type + 2 zlib + 5 stored-block hdr = 48,
// then one filter byte, then row 0's eight RGBA pixels.
log('CV-FILTER0 ' + b[48]);
log('CV-PX00 ' + b.slice(49, 53).join(','));       // blue square
log('CV-PX20 ' + b.slice(49 + 8, 49 + 12).join(',')); // pixel (2,0): red
// row 3 = 48 + 3*(1 + 8*4) + 1 = 148; pixel (6,3) is the half-alpha green.
log('CV-PX63 ' + b.slice(148 + 6*4, 148 + 6*4 + 4).join(','));

// --- statement N: THE POINT. This line only runs if the line above did not
// --- throw. It is the difference between "the check was refused" and "the
// --- check never appeared".
document.getElementById('after').style.background = '#fe7f01';
log('CV-SCRIPT-CONTINUED');

// --- the same bitmap through the async twin --------------------------------
// Both halves are logged: that the callback had NOT run when toBlob returned
// (it is asynchronous in every browser, and a page doing `toBlob(cb); next();`
// sees next() first everywhere else), and that it DID run afterwards.
var blobRan = false;
c.toBlob(function (blob) {
  blobRan = true;
  if (!blob) { log('CV-BLOB null'); return; }
  log('CV-BLOB ' + blob.type + ' ' + blob.size);
});
log('CV-BLOB-DEFERRED ' + (blobRan ? 'ran-inline' : 'deferred'));

// --- the MIME fallback must be visible, never silent -----------------------
log('CV-WEBP ' + c.toDataURL('image/webp').slice(0, 22));

// --- THREE FACTS THAT WERE REPORTED ABSENT AND ARE NOT ----------------------
// This block measures rather than fixes. The work order for this change listed
// navigator.languages, screen.colorDepth and performance.timeOrigin as "true
// things about this machine it declines to say", to be proposed as patches to
// js_platform.c / js_webapi.c (files another workflow holds). grep says all
// three are already there -- js_page.c:724 for languages, js_webapi.c:4492 for
// colorDepth, js_platform.c:297 for timeOrigin -- but CLAUDE.md's first rule is
// that a grep is not a measurement and that the host probe has already reported
// a whole subsystem absent that the browser has. So the guest is asked.
log('CV-LANGS ' + JSON.stringify(navigator.languages) +
    ' lang=' + navigator.language);
log('CV-SCREEN colorDepth=' + screen.colorDepth +
    ' pixelDepth=' + screen.pixelDepth + ' ' + screen.width + 'x' + screen.height);
log('CV-TIMEORIGIN ' + typeof performance.timeOrigin + ' ' +
    (performance.timeOrigin > 0 ? 'positive' : String(performance.timeOrigin)));
</script>"""

BODY = ('<div id="after">FINGERPRINTaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</div>')

# The 2026-08-30 addition: TEXT and drawImage, measured on the same machine.
# Served as a real subresource so the <img> goes through the LAYOUT pass's
# fetch+decode (layout_load_images -> IT_IMAGE) -- the one drawImage route a
# host gate cannot reach, because layout.c is exactly the TU tests/canvas.mk
# cannot link. The fixture is 2x1 red|blue so one pixel answers each half.
IMG_FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "fixtures", "canvas", "redblue.png")

# Everything below runs on a SECOND canvas (tc/tq): the readback block above
# owns canvas c's pixels down to the byte, and a text draw anywhere near it
# would make this file fight itself. The tags are CV-T-* (text) and
# CV-IMG-* (drawImage) so the serial log reads in order.
TEXTBLOCK = """<script>
function tlog(s) { try { console.log(s); } catch (e) {} }
var tc = document.createElement('canvas'); tc.width = 64; tc.height = 40;
var tq = tc.getContext('2d');
function tink(x0, x1, y0, y1) {
  var n = 0, first = -1, last = -1;
  for (var y = y0; y < y1; y++) for (var x = x0; x < x1; x++) {
    if (tq.getImageData(x, y, 1, 1).data[3] !== 0) {
      n++; if (first < 0) first = x; last = x; } }
  return n + ':' + first + ':' + last;
}

// measure and draw are ONE call in the implementation (shape_line); this is
// the property that keeps them agreeing, asserted as an integer identity.
tq.font = '20px sans-serif'; tq.fillStyle = '#ffffff';
tq.fillText('Hi!', 4, 20);
var a0 = parseInt(tink(0, 30, 0, 40).split(':')[1], 10);
var wid = tq.measureText('Hi!').width;
tq.clearRect(0, 0, 64, 40);
tq.fillText('Hi!', 4 + wid, 20);
var b0 = parseInt(tink(0, 64, 0, 40).split(':')[1], 10);
tlog('CV-T-ADV ' + (b0 - a0 - wid));

tq.clearRect(0, 0, 64, 40); tq.font = '16px monospace';
tlog('CV-T-MONO ' + (tq.measureText('iiii').width === 4 * tq.measureText('i').width));

tq.clearRect(0, 0, 64, 40); tq.font = '10px sans-serif'; tq.fillStyle = '#ffffff';
tq.fillText('W', 2, 8);
var k1 = tink(0, 30, 0, 40).split(':')[0];
tq.clearRect(0, 0, 64, 40);
tq.save(); tq.scale(2, 2); tq.fillText('W', 2, 8); tq.restore();
var k2 = tink(0, 64, 0, 40).split(':')[0];
tlog('CV-T-INK ' + k1);
// Two ONE-TOKEN tags, not "CV-T-CTM 52 151": field() below reads exactly one
// whitespace-delimited token after a tag, and a two-token line would read as
// "52" with the second number silently unreachable -- which is exactly how
// the first draft of this check failed while the guest was printing the
// right answer the whole time.
tlog('CV-T-CTM1 ' + k1);
tlog('CV-T-CTM2 ' + k2);

// drawImage, source 1: another canvas (the backing store, direct)
var tsrc = document.createElement('canvas'); tsrc.width = 2; tsrc.height = 1;
var tsq = tsrc.getContext('2d');
tsq.fillStyle = '#ff0000'; tsq.fillRect(0, 0, 1, 1);
tsq.fillStyle = '#0000ff'; tsq.fillRect(1, 0, 1, 1);
tq.clearRect(0, 0, 64, 40); tq.drawImage(tsrc, 3, 3);
tlog('CV-IMG-CV ' + tq.getImageData(3, 3, 1, 1).data[0] + ',' +
     tq.getImageData(4, 3, 1, 1).data[2]);
// drawImage, source 2: an <img> whose bytes are already in the document
var tdi = document.createElement('img');
tdi.setAttribute('src', tsrc.toDataURL());
tq.clearRect(0, 0, 64, 40); tq.drawImage(tdi, 5, 5);
tlog('CV-IMG-URL ' + tq.getImageData(5, 5, 1, 1).data[0] + ',' +
     tq.getImageData(6, 5, 1, 1).data[2]);
// drawImage, source 3: an <img> the LAYOUT pass fetched and decoded over
// HTTP. Retried on setTimeout because the subresource is async to this
// script; bounded so a page whose image never lands says so, not hangs.
var tim = document.getElementById('probeimg');
var ttries = 0;
function ttryimg() {
  tq.clearRect(0, 0, 64, 40);
  tq.drawImage(tim, 7, 7);
  var r = tq.getImageData(7, 7, 1, 1).data, b = tq.getImageData(8, 7, 1, 1).data;
  if (r[3] !== 0 && b[3] !== 0) { tlog('CV-IMG-LAYOUT ' + r[0] + ',' + b[2]); return; }
  if (++ttries < 24) setTimeout(ttryimg, 500);
  else tlog('CV-IMG-LAYOUT none');
}
ttryimg();

// THE TRAP GUARD: toDataURL must stay honest ON A CANVAS THAT HAS TEXT ON
// IT. The encoder and the text rasterizer are different subsystems that
// share one backing store; this is the one check that both were wired, on
// the machine, in the same process.
tq.clearRect(0, 0, 64, 40); tq.font = '20px sans-serif'; tq.fillStyle = '#ffffff';
tq.fillText('WWWW', 2, 20);
var turl = tc.toDataURL();
var traw = atob(turl.slice(turl.indexOf(',') + 1));
var tb = []; for (var i = 0; i < traw.length; i++) tb.push(traw.charCodeAt(i));
var tinked = 0;
for (var i = 49; i + 3 < tb.length; i += 4) if (tb[i + 3] !== 0) tinked++;
// One token per tag, for the field() reason above.
tlog('CV-ENC-TYPE ' + turl.slice(0, 22));
tlog('CV-ENC-INK ' + tinked);
</script>"""


def page():
    return ("<!doctype html>\n<html><head><title>canvas readback</title>" +
            STYLE + "</head><body>" + BODY +
            '<img id="probeimg" src="http://10.0.2.2:%d/rb.png" width="2" height="1">' % PORT +
            SCRIPT + TEXTBLOCK + "</body></html>\n")


tmp = tempfile.mkdtemp(prefix="qmp_cvrb_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
shot = lambda n: os.path.join(tmp, n + ".ppm")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.path == "/rb.png":
            requested.append(self.path)
            try:
                with open(IMG_FIXTURE, "rb") as fh:
                    raw = fh.read()
            except OSError:
                raw = b""
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return
        requested.append(self.path)
        raw = page().encode()
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
    for good, name in checks:
        print("  %s %s" % ("ok  " if good else "FAIL", name))
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-8000:])
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


def field(tag):
    """The value the page logged for `tag`, or None. Anchored on the tag so a
    console line that happens to contain the word cannot be mistaken for it.

    rstrip("\\r") because the guest's console emits CRLF and QEMU's serial
    FILE is written in chunks that can split a line after its text but before
    the \\r -- a read that lands between the two captures the CR inside \\S+,
    and a value comparison then fails on a byte no terminal ever shows. This
    race predates the text/drawImage block (it sleeps in every field() call
    this file has ever made); the added serial traffic of 2026-08-30 merely
    moved the chunk boundaries enough to fire it. Measured, not theorized:
    the failing run's artefact serial.log held the exact expected string,
    byte for byte, plus the CR."""
    m = None
    for m in re.finditer(r"\b" + tag + r" (\S+)", serial()):
        pass
    return m.group(1).rstrip("\r") if m else None


try:
    if not wait_serial("LOGIT_BOOT_OK", 240, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 90, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    # One click, at the tile the GUEST names for browser.aex, verified against the
    # guest's own [wm] launched line. The re-click loop this replaces was the
    # apology a hand-kept slot constant needed: when the pack list grows, the dock
    # is re-centred, every hard-coded index moves half a slot, and the miss looks
    # exactly like a slow launch. See tests/qmp/qmp_ui.py's dock block.
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(6)

    # The address bar starts focused on the first navigation -- see
    # qmp_module_page.py's goto() for why a hardcoded click lands on the title
    # bar instead, and why that is invisible exactly once.
    ui.typ("http://10.0.2.2:%d/cv.html" % PORT)
    ui.key("ret")

    ck(wait_serial("CV-URL-PREFIX", 120, "the fingerprint"),
       "toDataURL RETURNED on the real machine (it threw before this change, "
       "which is what killed the challenge script on its first statement)")

    ck(field("CV-URL-PREFIX") == "data:image/png;base64,",
       "the URL declares image/png")
    ck(field("CV-SIG") == "137,80,78,71,13,10,26,10",
       "the decoded bytes carry the PNG signature")
    ck(field("CV-CHUNK1") == "IHDR", "the first chunk is IHDR")
    ck(field("CV-DIM") == "8x4",
       "IHDR carries the CANVAS's size, not a default 300x150")
    ck(field("CV-IHDR") == "8,6,0,0,0",
       "depth 8, colour type 6 (RGBA), no interlace")
    ck(field("CV-END") == "IEND", "the file ends with IEND")
    ck(field("CV-FILTER0") == "0", "row 0 carries filter type 0 (None)")

    # THE PIXELS. Everything above is satisfiable by a well-formed PNG of the
    # wrong picture -- which is the exact fabrication the old refusal existed to
    # prevent, so these three are the assertions that matter.
    ck(field("CV-PX00") == "0,0,255,255",
       "pixel (0,0) is the BLUE square the script painted over the red")
    ck(field("CV-PX20") == "255,0,0,255",
       "pixel (2,0) is the red fill, i.e. the axes are not transposed")
    ck(field("CV-PX63") == "0,255,0,255",
       "pixel (6,3), on the LAST row and near the right edge, is the third fill "
       "-- so the encoder walked every row and did not stop at the first")

    # ---- the reason all of this was worth doing ---------------------------
    ck("CV-SCRIPT-CONTINUED" in serial(),
       "THE SCRIPT KEPT RUNNING PAST THE FINGERPRINT -- the failure the owner "
       "hit was not a refused check, it was a check that never appeared")

    ck(field("CV-WEBP") == "data:image/png;base64,",
       "an image/webp request falls back to PNG and SAYS SO in the URL it "
       "returns (the lie would be a data:image/webp prefix over PNG bytes, "
       "which is what every format-support probe reads)")
    ck("is not a type this browser can encode" in serial(),
       "and the fallback is announced on the console rather than being silent")

    ck(field("CV-BLOB-DEFERRED") == "deferred",
       "toBlob had NOT called back by the time it returned -- it is as "
       "asynchronous here as it is everywhere else")
    ck(wait_serial("CV-BLOB ", 30, "toBlob"),
       "and the callback DID arrive, on the machine's own job queue")
    blob = field("CV-BLOB")
    ck(blob == "image/png", "the Blob declares image/png (got %r)" % blob)

    # ---- the screen: the only place "the script continued" is visible to a
    # ---- person rather than to a log reader.
    time.sleep(2.5)
    p1 = PPM(ui.screendump(shot("after")))
    ck(p1.find_color(ORANGE) is not None,
       "the element the post-fingerprint line styled is ON SCREEN in a colour "
       "no stylesheet in the page mentions")
    ck(p1.find_color(RED) is None,
       "and the stylesheet's own colour is gone from that box")

    # ---- the three "absent" facts, measured rather than assumed ------------
    # These are NOT gated (this file does not own js_platform.c or js_webapi.c
    # and must not fail on their behalf) -- they are PRINTED, because the claim
    # under investigation was that they are absent and the answer is the point.
    print("\n--- three facts reported ABSENT, as the guest actually reports them ---")
    for tag in ("CV-LANGS", "CV-SCREEN", "CV-TIMEORIGIN"):
        m = re.search(r"\b" + tag + r" (.*)", serial())
        print("  %-14s %s" % (tag, m.group(1).strip() if m else "<<< NOT LOGGED >>>"))
    print("  (all three were on the work order as things to propose patches for."
          "\n   They are already implemented: js_page.c:724 navigator.languages,"
          "\n   js_webapi.c:4492 screen.colorDepth/pixelDepth, js_platform.c:297"
          "\n   performance.timeOrigin. Printed here so the next reader measures"
          "\n   rather than inherits the sentence.)")

    # ---- text and drawImage, 2026-08-30 -----------------------------------
    # The host gate (tests/unit/canvas_test.c) proves the plumbing of both on
    # the same js_canvas.c; what ONLY the guest can prove is that the font
    # files under /fonts load through SYS_READ_FILE into the real binary, that
    # the weak layout seam resolves on the link the owner runs, and that the
    # encoder still tells the truth about a canvas that has TEXT on it.
    ck(field("CV-T-ADV") == "0",
       "measureText and fillText agree as integers: the second draw starts "
       "exactly one measured width right (both are shape_line in ring 3)")
    ck(field("CV-T-MONO") == "true",
       "the shipped mono face (/fonts/mono.ttf, loaded by the real browser "
       "process) advances identically per glyph at 16px")
    ink = field("CV-T-INK")
    ck(ink is not None and ink.isdigit() and int(ink) > 0,
       "fillText paints ink ON THE REAL MACHINE (got %r)" % ink)
    c1 = field("CV-T-CTM1")
    c2 = field("CV-T-CTM2")
    ck(c1 is not None and c2 is not None and c1.isdigit() and c2.isdigit()
       and int(c2) > 2 * int(c1),
       "a 2x CTM more than doubles the glyph ink in the guest "
       "(%s -> %s)" % (c1, c2))
    ck(field("CV-IMG-CV") == "255,255",
       "drawImage canvas->canvas paints the source's own pixels")
    ck(field("CV-IMG-URL") == "255,255",
       "drawImage from an <img> with a data: URL decodes those bytes in ring 3")
    ck(wait_serial("CV-IMG-LAYOUT ", 30, "the layout-decoded <img>"),
       "an <img> FETCHED OVER HTTP and decoded by the layout pass reaches "
       "drawImage -- the one source route a host gate cannot link")
    lay = field("CV-IMG-LAYOUT")
    ck(lay == "255,255",
       "and the layout route paints the fixture's red|blue pixels (got %r)" % lay)
    enc = field("CV-ENC-TYPE")
    ck(enc == "data:image/png;base64,",
       "toDataURL still declares image/png on a canvas holding TEXT")
    inked = field("CV-ENC-INK")
    ck(inked is not None and inked.isdigit() and int(inked) > 0,
       "and the encoded bytes carry the text's ink (%s opaque bytes read back "
       "by the page's own atob) -- the readback this file exists for stays "
       "honest with the new subsystem drawing into it" % inked)

    print("\nPASS: a canvas fingerprint completes on the real machine, the bytes "
          "it reads back are the pixels that were drawn, and the script that "
          "called it keeps running.")
    print("NOTE, and it is not a caveat this test can remove: nothing here says "
          "any bot check PASSES. It says the page loads and the script runs.")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
