#!/usr/bin/env python3
"""Run a REAL canvas capability probe in the guest and report where it gets to.

    python3 tests/qmp/qmp_canvas_fingerprint.py <iso> <disk.img>

HOW THIS DIFFERS FROM qmp_canvas_readback.py, WHICH ALREADY PASSES. That file
asserts the encoder is honest: it draws three flat rectangles at coordinates it
chose, decodes the data URL and checks the bytes. Every drawing call in it was
picked BECAUSE this browser implements it. That is the right shape for gating an
encoder and the wrong shape for answering the owner's question, because it
cannot discover a wall it was written to avoid -- CLAUDE.md's rule 1, the
harness looking at itself rather than at the machine.

So this file does the opposite. It runs the canvas fingerprint that real bot
checks actually ship -- FingerprintJS's `canvas` component, whose draw list has
been copied into a decade of challenge scripts -- VERBATIM, at its real size,
with its real font strings and its real emoji, and reports how far it gets. It
is allowed to fail. A failure here is the next work order and is worth more than
a pass.

WHAT IT MUST NOT DO, and this is a hard boundary rather than a preference.
Nothing here is tuned so that any checker accepts anything. No signal is
spoofed: not navigator.webdriver, not the user agent, not the platform. The
fingerprint this machine produces is whatever LogitOS's own rasterizer draws,
it will not equal Chrome's, and it is not supposed to. THIS FILE DOES NOT CLAIM
ANY CHALLENGE PASSES. It claims the page loads and the script runs, and it
reports precisely where that stops being true.

THREE PAGES, because "it works" and "it fails" are both single data points and
the useful answer is the boundary between them:

  /inventory.html  typeof every canvas member a fingerprint touches. This is
                   the cheap census that says which wall comes next, and it
                   runs BEFORE anything can throw and hide the rest.
  /fpjs.html       the FingerprintJS canvas component, unmodified, at 2000x200.
                   Wrapped in one try/catch that records the step number, so a
                   throw reports WHICH CALL and not just that there was one.
  /shapes.html     the same probe shape -- draw, read back, hash -- restricted
                   to primitives this browser has. It exists to prove the
                   readback path itself is not the thing that fails, so that a
                   failure in fpjs.html is attributable to one missing method
                   rather than to the encoder.
  /bigsize.html    the readback at the size a REAL fingerprint uses, 2000x200.
                   Added because after three pages nothing had encoded more than
                   45,000 pixels and "the readback works" would have been a
                   claim about a size no probe uses. It found a wall of its own
                   that has nothing to do with the encoder -- see the comment
                   above BIGSIZE.

THE HASH IS FNV-1a AND THAT IS DELIBERATE, stated so nobody reads significance
into it. FingerprintJS hashes with murmur x64/128. Which hash is used has no
bearing on whether the probe COMPLETES, which is the only question here, and
implementing murmur would put a second thing in this file that can be wrong.
What matters about the hash is that it is computed over the whole data URL, so
a probe that reports one at all necessarily read every byte back.
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

# The colour the page paints ONLY from the line after the probe finishes. In no
# stylesheet, so its presence on screen has exactly one explanation.
ORANGE = (254, 127, 1)

HEAD = """<style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#after { background: #2233ee; }
</style>
<script>
function log(s) { try { console.log(s); } catch (e) {} }
// FNV-1a over the whole string. See this file's docstring for why not murmur:
// the hash's identity is irrelevant, computing it over every byte is not.
function h32(s) {
  var h = 0x811c9dc5;
  for (var i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i) & 0xff;
    h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
  }
  return ('0000000' + h.toString(16)).slice(-8);
}
</script>"""

# --------------------------------------------------------------------------
# PAGE 1: the census. Nothing here draws, so nothing here can throw and take
# the rest of the answer with it.
# --------------------------------------------------------------------------
INVENTORY = HEAD + """
<div id="after">INVENTORY</div>
<script>
var c = document.createElement('canvas');
c.width = 32; c.height = 32;
var x = c.getContext('2d');
log('FP-CTX ' + (x ? 'present' : 'ABSENT'));
// The draw calls a canvas fingerprint makes, in the order FingerprintJS makes
// them. Reported one per line so an absent one names itself.
var M = ['rect','isPointInPath','fillRect','fillText','strokeText','measureText',
         'arc','beginPath','closePath','fill','stroke','clip','drawImage',
         'createLinearGradient','createRadialGradient','getImageData',
         'putImageData','save','restore','translate','rotate','scale',
         'setTransform','globalCompositeOperation','ellipse','bezierCurveTo'];
for (var i = 0; i < M.length; i++) {
  var t;
  try { t = typeof x[M[i]]; } catch (e) { t = 'THREW'; }
  log('FP-M ' + M[i] + ' ' + t);
}
// The element-level readback pair.
log('FP-M toDataURL ' + (typeof c.toDataURL));
log('FP-M toBlob ' + (typeof c.toBlob));
// The state properties a fingerprint sets before drawing text. These are
// assignments, not calls: an engine that simply stores an unknown property
// accepts them silently, which is worth knowing separately from whether the
// method that CONSUMES them exists.
try { x.font = '11pt no-real-font-123'; log('FP-P font ' + x.font); }
catch (e) { log('FP-P font THREW ' + e); }
try { x.textBaseline = 'alphabetic'; log('FP-P textBaseline ' + x.textBaseline); }
catch (e) { log('FP-P textBaseline THREW ' + e); }
try { x.globalCompositeOperation = 'multiply';
      log('FP-P globalCompositeOperation ' + x.globalCompositeOperation); }
catch (e) { log('FP-P globalCompositeOperation THREW ' + e); }
document.getElementById('after').style.background = '#fe7f01';
log('FP-INVENTORY-DONE');
</script>"""

# --------------------------------------------------------------------------
# PAGE 2: FingerprintJS's canvas component, verbatim.
#
# The draw list, the 2000x200 size, the winding test, the '#f60'/'#069' fills,
# the deliberately-absent font family 'no-real-font-123', the pangram and the
# emoji are all as shipped. STEP is bumped before each statement so a throw
# names the call rather than the file.
# --------------------------------------------------------------------------
FPJS = HEAD + """
<div id="after">FPJS</div>
<script>
var STEP = 'start';
var t0 = Date.now();
try {
  var result = [];
  STEP = 'createElement';
  var canvas = document.createElement('canvas');
  canvas.width = 2000; canvas.height = 200;
  canvas.style.display = 'inline';
  STEP = 'getContext';
  var ctx = canvas.getContext('2d');

  STEP = 'rect';
  ctx.rect(0, 0, 10, 10);
  ctx.rect(2, 2, 6, 6);
  STEP = 'isPointInPath(evenodd)';
  result.push('canvas winding:' +
              ((ctx.isPointInPath(5, 5, 'evenodd') === false) ? 'yes' : 'no'));

  STEP = 'textBaseline=';
  ctx.textBaseline = 'alphabetic';
  STEP = 'fillStyle/fillRect';
  ctx.fillStyle = '#f60';
  ctx.fillRect(125, 1, 62, 20);
  STEP = 'font=';
  ctx.fillStyle = '#069';
  ctx.font = '11pt no-real-font-123';
  STEP = 'fillText(pangram)';
  ctx.fillText('Cwm fjordbank glyphs vext quiz, \\ud83d\\ude03', 2, 15);
  STEP = 'fillText(alpha)';
  ctx.fillStyle = 'rgba(102, 204, 0, 0.2)';
  ctx.font = '18pt Arial';
  ctx.fillText('Cwm fjordbank glyphs vext quiz, \\ud83d\\ude03', 4, 45);

  STEP = 'globalCompositeOperation';
  ctx.globalCompositeOperation = 'multiply';
  ctx.fillStyle = 'rgb(255,0,255)';
  ctx.beginPath(); ctx.arc(50, 50, 50, 0, Math.PI * 2, true);
  ctx.closePath(); ctx.fill();
  ctx.fillStyle = 'rgb(0,255,255)';
  ctx.beginPath(); ctx.arc(100, 50, 50, 0, Math.PI * 2, true);
  ctx.closePath(); ctx.fill();
  ctx.fillStyle = 'rgb(255,255,0)';
  ctx.beginPath(); ctx.arc(75, 100, 50, 0, Math.PI * 2, true);
  ctx.closePath(); ctx.fill();
  ctx.fillStyle = 'rgb(255,0,255)';
  STEP = 'evenodd fill';
  ctx.arc(75, 75, 75, 0, Math.PI * 2, true);
  ctx.arc(75, 75, 25, 0, Math.PI * 2, true);
  ctx.fill('evenodd');

  STEP = 'toDataURL';
  var url = canvas.toDataURL();
  STEP = 'hash';
  result.push('canvas fp:' + url);
  var joined = result.join('~');
  log('FP-OK steps=all len=' + joined.length + ' hash=' + h32(joined) +
      ' urllen=' + url.length + ' ms=' + (Date.now() - t0));
  log('FP-WINDING ' + result[0]);
  log('FP-URLPFX ' + url.slice(0, 22));
} catch (e) {
  log('FP-THREW step=' + STEP + ' ms=' + (Date.now() - t0) +
      ' err=' + (e && e.name) + ': ' + (e && e.message));
}
document.getElementById('after').style.background = '#fe7f01';
log('FP-FPJS-DONE');
</script>"""

# --------------------------------------------------------------------------
# PAGE 3: the same probe SHAPE -- draw, read back, hash -- using only what this
# browser has. If this completes and fpjs.html does not, the difference is the
# missing method and not the readback.
# --------------------------------------------------------------------------
SHAPES = HEAD + """
<div id="after">SHAPES</div>
<script>
var STEP = 'start'; var t0 = Date.now();
try {
  var result = [];
  STEP = 'setup';
  var canvas = document.createElement('canvas');
  canvas.width = 300; canvas.height = 150;
  var ctx = canvas.getContext('2d');
  STEP = 'winding';
  ctx.rect(0, 0, 10, 10); ctx.rect(2, 2, 6, 6);
  result.push('winding:' + ((ctx.isPointInPath(5, 5, 'evenodd') === false) ? 'yes' : 'no'));
  STEP = 'rects';
  ctx.fillStyle = '#f60'; ctx.fillRect(125, 1, 62, 20);
  STEP = 'arcs';
  ctx.fillStyle = 'rgba(102,204,0,0.7)';
  ctx.beginPath(); ctx.arc(50, 50, 40, 0, Math.PI * 2, true); ctx.closePath(); ctx.fill();
  ctx.fillStyle = 'rgba(0,102,153,0.7)';
  ctx.beginPath(); ctx.arc(80, 60, 40, 0, Math.PI * 2, true); ctx.closePath(); ctx.fill();
  STEP = 'gradient';
  var g = ctx.createLinearGradient(0, 100, 300, 150);
  g.addColorStop(0, '#ff0000'); g.addColorStop(1, '#0000ff');
  ctx.fillStyle = g; ctx.fillRect(0, 100, 300, 50);
  STEP = 'bezier';
  ctx.strokeStyle = '#000000'; ctx.lineWidth = 3;
  ctx.beginPath(); ctx.moveTo(10, 90);
  ctx.bezierCurveTo(80, 20, 200, 160, 290, 90); ctx.stroke();
  STEP = 'toDataURL';
  var url = canvas.toDataURL();
  STEP = 'hash';
  result.push('fp:' + url);
  var joined = result.join('~');
  log('SH-OK ' + result[0] + ' urllen=' + url.length +
      ' hash=' + h32(joined) + ' ms=' + (Date.now() - t0));
  STEP = 'getImageData cross-check';
  // The SECOND oracle inside the guest, and it is the one that makes the hash
  // mean something. getImageData is an independent path to the same surface --
  // it copies the backing store and never touches the encoder -- so hashing
  // BOTH says whether the readback agrees with the pixels or merely exists.
  var d = ctx.getImageData(0, 0, 300, 150).data;
  var s = ''; for (var i = 0; i < d.length; i += 997) s += String.fromCharCode(d[i]);
  log('SH-IMAGEDATA len=' + d.length + ' sample=' + h32(s) +
      ' px0=' + d[0] + ',' + d[1] + ',' + d[2] + ',' + d[3]);
  // And the pixel under the gradient, from BOTH paths, by value.
  var q = ctx.getImageData(150, 125, 1, 1).data;
  log('SH-GRADPX ' + q[0] + ',' + q[1] + ',' + q[2] + ',' + q[3]);
} catch (e) {
  log('SH-THREW step=' + STEP + ' err=' + (e && e.name) + ': ' + (e && e.message));
}
document.getElementById('after').style.background = '#fe7f01';
log('SH-DONE');
</script>"""

# --------------------------------------------------------------------------
# PAGE 4: THE REAL PROBE'S CANVAS SIZE, encoded for real.
#
# WHY THIS PAGE EXISTS, and it is a gap in the other three rather than an
# extra. fpjs.html uses FingerprintJS's real 2000x200 and never reaches
# toDataURL, because fillText throws first. shapes.html reaches toDataURL but
# only at 300x150. So after three pages NOTHING had encoded a canvas the size a
# real fingerprint actually uses, and "the readback works" would have been a
# claim about 45,000 pixels standing in for 400,000.
#
# That matters here specifically. The encoder emits STORED deflate blocks, so
# the output is ~1.001x the raw RGBA: 2000x200 is 1,600,000 bytes of pixels ->
# ~1.6 MB of PNG -> ~2.14 MB of base64 -> a JS string of the same length, with
# the surface still live. Peak is several megabytes in a ring-3 heap on a
# 512 MiB machine, and LEN is a u16 so the IDAT is ~25 stored blocks rather
# than the 1 and 2 the host gate's small cases exercise. None of that is
# exercised anywhere else, and all of it is on the path the owner's page takes.
BIGSIZE = HEAD + """
<div id="after">BIGSIZE</div>
<script>
var STEP = 'start'; var t0 = Date.now();
try {
  STEP = 'setup 2000x200';
  var canvas = document.createElement('canvas');
  canvas.width = 2000; canvas.height = 200;
  var ctx = canvas.getContext('2d');
  ctx.fillStyle = '#f60'; ctx.fillRect(125, 1, 62, 20);
  ctx.fillStyle = 'rgb(255,0,255)';
  ctx.beginPath(); ctx.arc(50, 50, 50, 0, Math.PI * 2, true); ctx.closePath(); ctx.fill();
  // A mark on the LAST row at the FAR right, so a truncated encode -- the
  // failure a multi-block stored stream fails by -- cannot pass unnoticed.
  ctx.fillStyle = '#00ff00'; ctx.fillRect(1996, 199, 4, 1);
  var tdraw = Date.now() - t0;
  STEP = 'toDataURL';
  var t1 = Date.now();
  var url = canvas.toDataURL();
  var tenc = Date.now() - t1;
  log('BIG-OK urllen=' + url.length + ' drawms=' + tdraw + ' encms=' + tenc);
  STEP = 'decode head/tail';
  // DECODED IN TWO SMALL SLICES, NOT WHOLE, AND THE FIRST VERSION OF THIS PAGE
  // IS WHY. It did `atob(url.slice(url.indexOf(',') + 1))` on the entire
  // 2,133,852-character URL and never came back: measured in the guest
  // 2026-08-30, the script watchdog fired --
  //
  //     [js] watchdog: script exceeded its CPU slice (wall time) -- interrupted
  //     InternalError: interrupted at <anonymous> (<platform>:438)
  //
  // and the InternalError is NOT catchable by the try/catch around it (the
  // interrupt handler keeps asking to interrupt), so the page's own error path
  // never ran, BIG-DONE never logged and the harness sat in a timeout. The
  // encode itself had already finished in 30 ms; the wall is js_platform.c's
  // atob at megabyte scale, which is a real finding about this machine and is
  // reported as one rather than worked around silently.
  //
  // Base64 is 4 characters per 3 bytes with no state, so a slice starting at a
  // multiple of 4 decodes to exactly the corresponding bytes. The whole payload
  // is a multiple of 4 (it is padded), so the tail slice is aligned too.
  var b64 = url.slice(url.indexOf(',') + 1);
  var t2 = Date.now();
  var head = atob(b64.slice(0, 64));                 // 48 bytes: sig+IHDR+IDAT hdr
  var tail = atob(b64.slice(b64.length - 24));       // 18 bytes: ...IEND + CRC
  log('BIG-B64LEN ' + b64.length + ' slicedecodems=' + (Date.now() - t2));
  log('BIG-DIM ' + ((head.charCodeAt(16)<<24)|(head.charCodeAt(17)<<16)|
                    (head.charCodeAt(18)<<8)|head.charCodeAt(19)) + 'x' +
                   ((head.charCodeAt(20)<<24)|(head.charCodeAt(21)<<16)|
                    (head.charCodeAt(22)<<8)|head.charCodeAt(23)));
  log('BIG-SIG ' + head.charCodeAt(0) + ',' + head.charCodeAt(1) + ',' +
      head.charCodeAt(2) + ',' + head.charCodeAt(3));
  log('BIG-TAIL ' + tail.charCodeAt(tail.length-8) + ',' + tail.charCodeAt(tail.length-7) +
      ',' + tail.charCodeAt(tail.length-6) + ',' + tail.charCodeAt(tail.length-5));
  // The green mark, read back out of the encoded bytes. Offset arithmetic:
  // 8 sig + 25 IHDR + 8 IDAT len/type + 2 zlib + 5 first stored-block header
  // = 48 is row 0's filter byte. Rows are 1 + 2000*4 = 8001 bytes, but a
  // stored block boundary inserts a 5-byte header every 65535 bytes, so the
  // last row's offset is NOT plain arithmetic -- which is exactly why this is
  // asserted through getImageData instead, an independent path with no
  // block structure in it at all.
  var q = ctx.getImageData(1998, 199, 1, 1).data;
  log('BIG-LASTROW ' + q[0] + ',' + q[1] + ',' + q[2] + ',' + q[3]);
} catch (e) {
  log('BIG-THREW step=' + STEP + ' err=' + (e && e.name) + ': ' + (e && e.message));
}
document.getElementById('after').style.background = '#fe7f01';
log('BIG-DONE');
</script>"""

PAGES = {
    "/inventory.html": INVENTORY,
    "/fpjs.html": FPJS,
    "/shapes.html": SHAPES,
    "/bigsize.html": BIGSIZE,
}

tmp = tempfile.mkdtemp(prefix="qmp_cvfp_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        body = PAGES.get(self.path)
        if body is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        raw = ("<!doctype html>\n<html><head><title>probe</title>" + body +
               "</body></html>\n").encode()
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


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    print(serial()[-9000:])
    proc.kill()
    sys.exit(1)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def lines(prefix):
    return [m.group(1).strip()
            for m in re.finditer(r"\b" + prefix + r" (.*)", serial())]


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

    base = "http://10.0.2.2:%d" % PORT

    def nav(url):
        """Ctrl+T, then the URL, then Enter -- NO TYPED PIXEL COORDINATES.

        THIS FILE GOT IT WRONG FIRST AND THE FIRST RUN IS WHY THIS COMMENT
        EXISTS. Pages 2 and 3 were reached with `ui.click_at(300, 60)` to focus
        the address bar. That is precisely the defect qmp_site.py's header
        records ("It clicked the address bar at a hardcoded (420,145). The
        browser grew a tab strip and that coordinate moved") and that
        tools/check-test-liveness.py flags on five existing drivers. It is also
        invisible exactly once: browser.c starts with `editing = 1`, so the
        FIRST navigation of a boot types into an already-focused bar and the
        click is decoration. Measured 2026-08-29 -- page 1 loaded, page 2
        loaded, page 3 never did, and the harness sat in a 300 s timeout
        reporting nothing about the machine.

        Ctrl+T is browser.c's own new-tab shortcut: it sets `editing = 1` with
        an EMPTY url buffer, so there is no geometry and no leftover text.
        key_mods() is the PACED form -- qmp_site.py:511 records that putting
        ctrl-down and key-down in one input-send-event drops the letter every
        time in a PS/2 controller with a one-byte buffer."""
        ui.key_mods(("ctrl",), "t", settle=0.25)
        time.sleep(0.8)
        ui.typ(url)
        ui.key("ret")

    # ---------------- page 1: the census --------------------------------
    nav(base + "/inventory.html")
    if not wait_serial("FP-INVENTORY-DONE", 120, "the inventory"):
        die("the inventory page never finished")

    print("\n=== 1. CANVAS MEMBER CENSUS, as the guest reports it ===")
    absent = []
    for ln in lines("FP-M"):
        name, kind = (ln.split() + ["?"])[:2]
        print("   %-24s %s" % (name, kind))
        if kind != "function":
            absent.append(name)
    for ln in lines("FP-P"):
        print("   [prop] %s" % ln)
    print("   ABSENT/non-function: %s" % (", ".join(absent) or "none"))

    # ---------------- page 2: the real probe ----------------------------
    nav(base + "/fpjs.html")
    if not wait_serial("FP-FPJS-DONE", 300, "the FingerprintJS probe"):
        die("the FingerprintJS probe page never finished")

    print("\n=== 2. FingerprintJS canvas component, VERBATIM, 2000x200 ===")
    ok = lines("FP-OK")
    threw = lines("FP-THREW")
    for ln in ok:
        print("   COMPLETED: %s" % ln)
    for ln in threw:
        print("   THREW:     %s" % ln)
    for ln in lines("FP-WINDING") + lines("FP-URLPFX"):
        print("   %s" % ln)

    # ---------------- page 3: the same shape, this browser's primitives --
    nav(base + "/shapes.html")
    if not wait_serial("SH-DONE", 300, "the shapes probe"):
        die("the shapes probe page never finished")

    print("\n=== 3. THE SAME PROBE SHAPE over primitives this browser has ===")
    for tag in ("SH-OK", "SH-THREW", "SH-IMAGEDATA", "SH-GRADPX"):
        for ln in lines(tag):
            print("   %-14s %s" % (tag, ln))

    # ---------------- page 4: the real probe's canvas size --------------
    nav(base + "/bigsize.html")
    if not wait_serial("BIG-DONE", 420, "the 2000x200 encode"):
        die("the 2000x200 page never finished")

    print("\n=== 4. FingerprintJS's ACTUAL canvas size (2000x200) encoded ===")
    for tag in ("BIG-OK", "BIG-THREW", "BIG-B64LEN", "BIG-DIM", "BIG-SIG",
                "BIG-TAIL", "BIG-LASTROW"):
        for ln in lines(tag):
            print("   %-14s %s" % (tag, ln))

    shot = os.path.join(tmp, "shapes.ppm")
    time.sleep(2.0)
    p = PPM(ui.screendump(shot))
    print("\n   post-probe DOM write visible on screen: %s" %
          ("yes" if p.find_color(ORANGE) else "NO"))

    print("\n--- what this run does and does not say -------------------------")
    print("It says the pages loaded and the scripts ran to the point named")
    print("above. It says NOTHING about whether any bot check accepts this")
    print("browser: nothing here is tuned toward a checker and no signal is")
    print("spoofed. An honest fingerprint that says 'this is LogitOS' is a")
    print("correct result even when it is refused.")
    proc.kill()
    sys.exit(0 if (ok or threw) else 1)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
