#!/usr/bin/env python3
"""<track> captions as PIXELS, in the real browser, on the machine.

    python3 tests/qmp/qmp_videosrc.py <iso> <disk.img>

WHAT THIS IS FOR. tests/unit/videosrc_test.c proves the engine half: bytes
parse to cues, cues are found at media time, playback advances through them.
NOTHING in that process can prove a caption reached the SCREEN -- the renderer
lives in js_media.c behind gui_text_run, which the host has no equivalent of.
This harness closes that gap.

THE VIDEO IS SOLID BLACK, ON A WHITE PAGE, AND THAT IS THE INSTRUMENT. The
driver locates the <video> box as the one large dark rectangle on the page,
then counts BRIGHT pixels in its bottom band. On black content every bright
pixel in that band is the caption's, so the count cannot accidentally measure
the video -- the failure mode of counting "changed pixels", which a rolling
logo or a progress bar satisfies as happily as a caption does.

THREE WINDOWS ARE SAMPLED, not one: inside cue 1 (bright), inside the gap
after it (dark), inside cue 2 (bright again). Two cues and a gap is the
minimum that says the renderer is following TIME rather than painting once:
a renderer that drew the first cue and never cleared it passes any single
window and fails the gap, and a renderer that never drew at all passes the
gap and fails both cues.

NO PCM CHECK, DELIBERATELY: the black fixture has no audio track, so there is
nothing for QEMU's card to write -- the clock is the monotonic fallback in
that run, which is an explicitly supported path (see the no-card control in
tests/mse.mk). Captured PCM for the audio-mastered clock is test-video-page's
job and stays there.

KNOWN-RED AS OF 2026-08-30, AND THIS IS THE HONEST STATE RATHER THAN A
WEAKENED ASSERTION. With a <track> attached to a PLAYING element the browser
freezes outright 1-2 s in (every timer stops, composites stop, no error
anywhere) -- reproducible 4/4 runs with the track, 0/2 without, at varying
times inside the first active cue. Isolated to date, each by its own run:
  - NOT the caption pixels: a build that runs the lookup but draws nothing
    still freezes; a build that draws (this one) rendered cue 1 as 181 bright
    pixels with a clean 0-pixel gap before the freeze arrived
  - NOT playing an audio-less video alone: same page minus the <track>
    element plays all 60 frames to 'ended'
  - NOT a fetch during playback: fetching an unrelated page mid-play, no
    track, plays to the end
  - NOT the engine: tests/unit/videosrc_test.c drives attach+active over a
    whole film on the host, and subs.c is ASan/UBSan-clean over 50k lookups
    on this exact fixture
so the defect is the attach/active path under the guest build or its
interaction with the pump, unfound as of this note. The gate stays red with
its numbers rather than green without its assertion.
"""

import os
import sys
import subprocess
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session              # noqa: E402
import qmp_addrbar                      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BLACK = os.path.join(ROOT, "tests", "fixtures", "video", "black.mp4")
CAPS = os.path.join(ROOT, "tests", "fixtures", "video", "captions-black.vtt")

# Cue windows in captions-black.vtt, with the margins the 500 ms page tick and
# QEMU's scheduling need to land a screenshot inside them.
W1 = (0.35, 1.15)      # "CAPTION ON SCREEN"
GAP = (1.30, 1.90)
W2 = (2.05, 2.75)      # "SECOND CAPTION LINE"

# NO `display: block` ON THE VIDEO, AND THAT IS A MEASURED DECISION, not a
# style preference. layout.c's block-child path (layout_flow's has_block_child
# branch) dispatches block children straight to layout_block and never calls
# flow_node, whose replaced-element cases -- video, canvas, svg, controls --
# are what create the IT_VIDEO item. A `video { display: block }` therefore
# lays out as an EMPTY BLOCK (probed host-side: items = one 0-height rect, no
# IT_VIDEO), the media engine never hears about the box, no frame is ever
# blitted and no caption can be drawn -- while every engine counter still
# rises, which is exactly how the earlier video gates passed without pixels.
# The default (inline) video goes through flow_node and works; this page uses
# that path and the defect is reported separately with its diff.
PAGE = """<!doctype html>
<html><head><title>captions</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; }
video { width: 512px; height: 384px; background: #000000; }
</style></head><body>
<video id="v" src="/black.mp4">
  <track default src="/caps.vtt">
</video>
<div id="s">idle</div>
<script>
var v = document.getElementById('v');
function tick() {
  var st = (typeof v.__mediaStats === 'function') ? v.__mediaStats() : null;
  console.log('VS-TICK t=' + (st ? (v.currentTime).toFixed(3) : '?') +
              ' shown=' + (st ? st.framesShown : '?') +
              ' ended=' + (v.ended ? 1 : 0) +
              ' err=' + (v.error ? v.error.code : 0));
  if (v.ended || (v.error && v.error.code)) { clearInterval(id); console.log('VS-END'); }
}
var id = setInterval(tick, 500);
v.onerror = function () {
  console.log('VS-ERROR code=' + (v.error ? v.error.code : '?') +
              ' msg=' + (v.error ? v.error.message : '?'));
};
v.play();
</script>
</body></html>
"""

tmp = tempfile.mkdtemp(prefix="qmp_videosrc_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
qemu_err_path = os.path.join(tmp, "qemu.err")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw, ctype = b"not found\n", "text/plain"
        code = 404
        if self.path.startswith("/page"):
            raw, ctype, code = PAGE.encode(), "text/html", 200
        elif self.path.startswith("/black.mp4"):
            with open(BLACK, "rb") as fh:
                raw = fh.read()
            ctype, code = "video/mp4", 200
        elif self.path.startswith("/caps.vtt"):
            with open(CAPS, "rb") as fh:
                raw = fh.read()
            ctype, code = "text/vtt", 200
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL,
    stderr=open(qemu_err_path, "wb"))

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- QEMU stderr -----")
    try:
        with open(qemu_err_path, "rb") as fh:
            print(fh.read().decode("utf-8", "replace")[-2000:])
    except OSError:
        print("(none)")
    print("----- serial (tail) -----")
    print(serial()[-6000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append(bool(cond))
    print(("ok: " if cond else "FAIL: ") + name)
    if not cond:
        die(name)


def note(s):
    print("     " + s)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def last_tick():
    """The newest VS-TICK line as a dict, or None."""
    best = None
    for ln in serial().splitlines():
        if "VS-TICK" in ln:
            d = {}
            for tok in ln.split():
                if "=" in tok:
                    k, _, v = tok.partition("=")
                    d[k] = v
            best = d
    return best


def read_ppm(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if not raw.startswith(b"P6"):
        return None, None
    # P6\n<w> <h>\n<max>\n<pixels>
    parts = raw.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return (w, h), parts[3]


def find_video_box(px, w, h):
    """The one large dark rectangle: rows whose dark-pixel run spans >350 px.

    The video is the only black thing on a white page, and it is 512 CSS px
    wide, so the dark run in any row crossing it is unmistakable.
    """
    top = bot = -1
    left = 1 << 30
    right = 0
    for y in range(0, h, 4):
        row = px[(y * w) * 3:(y * w + w) * 3]
        run = run_start = 0
        best_run = best_start = 0
        for x in range(w):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            if r < 60 and g < 60 and b < 60:
                if run == 0:
                    run_start = x
                run += 1
                if run > best_run:
                    best_run, best_start = run, run_start
            else:
                run = 0
        if best_run > 350:
            if top < 0:
                top = y
            bot = y
            if best_start < left:
                left = best_start
            if best_start + best_run > right:
                right = best_start + best_run
    if top < 0:
        return None
    return top, bot, left, right


def bright_in_band(px, w, box, frac_lo=0.70, frac_hi=0.97):
    """Pixels with luminance > 200 in the caption band of the video box."""
    top, bot, left, right = box
    y0 = top + int((bot - top) * frac_lo)
    y1 = top + int((bot - top) * frac_hi)
    n = 0
    for y in range(y0, y1, 2):
        for x in range(left, right, 2):
            o = (y * w + x) * 3
            if 0.3 * px[o] + 0.5 * px[o + 1] + 0.2 * px[o + 2] > 200:
                n += 1
    return n


try:
    if not wait_serial("LOGIT_BOOT_OK", 300, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 120, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(8)

    bar = qmp_addrbar.focus(ui)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    qmp_addrbar.typed_echo(ui, bar)
    ui.key("ret")

    if not wait_serial("VS-TICK", 90, "the page's script"):
        die("the page never ticked -- the script or the video never started")
    if not wait_serial("track /caps.vtt: 3 cues", 60, "the track fetch+parse"):
        die("the <track> never loaded (no 'track /caps.vtt: N cues' line)")

    # The video box, found once: it does not move for the life of the page.
    shot = os.path.join(tmp, "locate.ppm")
    ui.screendump(shot, settle=0.5)
    (w, h), px = read_ppm(shot)
    if w is None:
        die("the screendump is not a P6 PPM")
    box = find_video_box(px, w, h)
    if not box:
        die("no large dark rectangle on a white page -- the video box was not found")
    note("video box found: y %d..%d x %d..%d" % box)

    def sample(window, name):
        """Wait for media time inside `window`, then count caption pixels."""
        end = time.time() + 30
        while time.time() < end:
            tk = last_tick()
            if tk and tk.get("t") not in (None, "?"):
                t = float(tk["t"])
                if window[0] <= t <= window[1]:
                    p = os.path.join(tmp, name + ".ppm")
                    ui.screendump(p, settle=0.3)
                    _s, px2 = read_ppm(p)
                    return bright_in_band(px2, w, box)
            time.sleep(0.4)
        return -1

    b1 = sample(W1, "cue1")
    note("cue-1 window: %d bright caption pixels" % b1)
    ck(b1 > 40, "inside cue 1 the caption band is BRIGHT (%d px)" % b1)
    bg = sample(GAP, "gap")
    note("gap window:   %d bright caption pixels" % bg)
    ck(bg < 10, "inside the gap the caption band is DARK (%d px) -- cues END" % bg)
    b2 = sample(W2, "cue2")
    note("cue-2 window: %d bright caption pixels" % b2)
    ck(b2 > 40, "inside cue 2 a DIFFERENT cue is bright again (%d px)" % b2)

    # Playback proof, the same instrument test-video-page uses: a RISE.
    seen = []
    end = time.time() + 40
    while time.time() < end:
        tk = last_tick()
        if tk and tk.get("shown", "?") != "?":
            seen.append(int(tk["shown"]))
        if "VS-END" in serial():
            break
        time.sleep(0.5)
    note("framesShown over time: %s" % seen)
    ck(any(b > a for a, b in zip(seen, seen[1:])),
       "framesShown ROSE -- the video played, captions rode a moving clock")
    ck(serial().count("VS-ERROR") == 0, "no media error was raised")

    print("\nqmp_videosrc: %d checks, 0 failures" % len(checks))
    print("VIDEOSRC-OS-OK")
except SystemExit:
    raise
finally:
    try:
        proc.kill()
    except Exception:
        pass
