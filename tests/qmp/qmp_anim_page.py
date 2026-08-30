#!/usr/bin/env python3
"""The CSS animation clock, measured ON THE MACHINE at three times of one page.

    python3 tests/qmp/qmp_anim_page.py <iso> <disk.img> [tag]

Before the animation clock existed, `animation` and `transition` were
approximated to their static END-STATE: nothing drove frames, so a 2 s
fade-in painted as its final frame from the first repaint. This driver is
the gate that says the clock is real:

  KF     tests/fixtures/anim/kf.html -- @keyframes opacity 0->1 and
         translateX(0)->(100px), 2 s linear. Screenshots scheduled from the
         PAGE's own serial markers (guest clock -- host sleeps do not track
         guest time under TCG, see the phase-2 comment); the MID frame must
         differ from BOTH the start frame AND the end frame by a healthy
         pixel count. Against the pre-fix build the page renders its
         end-state at every t, so mid==end and the assertion goes RED --
         that red run is the negative control and is recorded in the gate's
         report.

  TRANS  tests/fixtures/anim/trans.html -- a class change at t0~=600 ms
         transitions opacity .15 -> 1 over 2 s. Same three-frame shape
         around the class change.

  STATIC tests/fixtures/anim/static.html -- the PERMANENT negative control,
         run as the first phase of every invocation (a prerequisite of the
         positive assertions, not a sibling target): @keyframes on a property
         the clock deliberately does not interpolate must NOT move, and
         `animation: nosuchkf` on opacity:0 must stay VISIBLE (the
         end-state approximation real scroll-reveal pages rely on).

  CAP    tests/fixtures/anim/cap.html -- 0/8/32/96/192 simultaneously
         animated boxes, rAF-delivered frames per phase counted BY THE PAGE.
         Measurement (printed, not asserted past sanity): it is the number
         the engine's concurrent-animation cap was set from.

Pixels, not logs: every assertion below counts differing pixels between two
screendumps. A frame that "changed" without changing pixels is the stall
CLAUDE.md warns about, and a clean serial log proves nothing.
"""

import http.server
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, pt   # noqa: E402

ISO = sys.argv[1]
DISK = sys.argv[2]
TAG = sys.argv[3] if len(sys.argv) > 3 else "run"

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, "tests", "fixtures", "anim")

# A whole 140x140 box changing alpha is ~19600 pixels; a box moving 50px is
# its whole area twice. Anything above 5000 is "the frame really moved";
# below 50 is "identical" (no caret, no clock, the chrome is static -- if
# that ever stops being true these thresholds must be re-derived, not nudged).
TH_MOVE = 5000
TH_SAME = 50
CHROME_Y_MIN = 40          # rows above this are desktop chrome, not the page


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        name = os.path.basename(self.path.split("?")[0])
        path = os.path.join(FIX, name)
        if not name.endswith(".html") or not os.path.exists(path):
            self.send_response(404)
            self.end_headers()
            return
        raw = open(path, "rb").read()
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

tmp = tempfile.mkdtemp(prefix="qmp_anim_")
serial_path = os.path.join(tmp, "serial.log")
qmp_path = os.path.join(tmp, "qmp.sock")

XRES = int(os.environ.get("QMP_XRES", "1280"))
YRES = int(os.environ.get("QMP_YRES", "800"))
SCALE = configure(XRES, YRES)

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
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
    try:
        proc.kill()
    except Exception:
        pass
    sys.exit(1)


def ck(cond, what):
    checks.append((bool(cond), what))
    print(("ok   " if cond else "FAIL ") + what)


def wait_serial(needle, secs, poll=0.25, what="marker"):
    """Faster polling than qmp_css_modern's: animation shots are scheduled
    from these markers and a 0.5 s poll granularity would eat the margin."""
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for %s" % what)
        time.sleep(poll)
    return False


def wait_marker(needle, what):
    if not wait_serial(needle, 120, what=what):
        die("page never printed %s (%s)" % (needle, what))


def shot(ui, name, settle=0.15):
    path = os.path.join(tmp, "%s_%s.ppm" % (TAG, name))
    ui.screendump(path, settle=settle)
    return path


def pxbuf(path):
    with open(path, "rb") as fh:
        data = fh.read()
    # P6 header: <w> <h> <maxval> then one whitespace byte
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        die("%s: not a P6 PPM" % path)
    w, h = int(m.group(1)), int(m.group(2))
    return data[m.end():], w, h


def diffcount(a, b, y_min=CHROME_Y_MIN):
    """Pixels whose ANY channel differs between two same-size screendumps.

    y_min crops the DESKTOP's top strip out of the comparison: the screen's
    top-right corner (x 1252-1268, y 10-22 on the 1280x800 run) ticks on its
    own between frames -- window-manager chrome, not the page. Found on the
    first pre-fix run, where it reddened the static control with '168 px' on
    a page that animates nothing; the bbox located it, the crop removes it,
    and the page content starts well below (first paint is past the address
    bar at y~=175)."""
    pa, w, h = pxbuf(a)
    pb, w2, h2 = pxbuf(b)
    if (w, h) != (w2, h2):
        die("screendump size mismatch %dx%d vs %dx%d" % (w, h, w2, h2))
    n = 0
    for y in range(y_min, h):
        base = y * w * 3
        for o in range(base, base + w * 3, 3):
            if pa[o] != pb[o] or pa[o + 1] != pb[o + 1] or pa[o + 2] != pb[o + 2]:
                n += 1
    return n


def navigate(ui, path, bar_y):
    ui.click_at(pt(420), pt(bar_y))
    for _ in range(80):
        ui.key("backspace")
    ui.typ("http://10.0.2.2:%d/%s" % (PORT, path))
    ui.key("ret")


rc = 1
try:
    if not wait_serial("LOGIT_BOOT_OK", 240, what="boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    time.sleep(6)

    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(3.0)

    # ---------------- PHASE 1: the permanent negative control --------------
    # Runs FIRST and die()s on failure, so the positive assertions below
    # never execute against a page that animates what it must not.
    navigate(ui, "static.html", 145)
    wait_marker("ANIM-STATIC-READY", "static page ready")
    st0 = shot(ui, "static_start", settle=0.15)
    time.sleep(1.0)
    st1 = shot(ui, "static_mid", settle=0.15)
    time.sleep(1.4)
    st2 = shot(ui, "static_end", settle=0.15)
    d01 = diffcount(st0, st1)
    d12 = diffcount(st1, st2)
    ck(d01 < TH_SAME,
       "static control: a @keyframes rule on margin-left does NOT move "
       "(start vs mid %d px)" % d01)
    ck(d12 < TH_SAME,
       "static control: ...and the page is still identical a second later "
       "(mid vs end %d px)" % d12)
    # #reveal: opacity:0 + an animation NAME with no @keyframes -- no
    # animation runs, so the box must stay invisible in both the pre-clock
    # and the clocked world. See static.html for why this is INVISIBLE and
    # not the "visible end-state" the old approximation's comment implies.
    from qmp_ui import PPM
    img = PPM(st1)
    ck(img.find_color((1, 3, 3)) is None,
       "static control: opacity:0 + a MISSING @keyframes name stays "
       "INVISIBLE (no animation ran; nothing 'released' the base opacity)")

    # ---------------- PHASE 2: @keyframes, mid != start AND mid != end -----
    # The SHOTS are scheduled from the PAGE's own markers, never from host
    # time.sleep: under TCG the guest clock runs at its own rate (~2x the
    # host here, measured 2026-08-30), and the first version of this gate
    # host-scheduled its "t=1 s" shot -- which captured guest t~1.6-2 s,
    # past the end of the 2 s animation, where mid==end and no clock could
    # ever pass. kf.html fires ANIM-KF-MID at +0.9 s guest; the capture's
    # own latency (serial poll + QMP screendump roundtrip) lands the frame
    # near t~1.2 s. Poll at 0.1 s so the latency stays small.
    navigate(ui, "kf.html", 175)
    wait_marker("ANIM-KF-READY", "kf page ready")
    kf0 = shot(ui, "kf_start", settle=0.10)
    if not wait_serial("ANIM-KF-MID", 60, poll=0.1, what="kf mid marker"):
        die("page never printed ANIM-KF-MID")
    kf1 = shot(ui, "kf_mid", settle=0.10)
    if not wait_serial("ANIM-KF-END", 60, poll=0.2, what="kf end marker"):
        die("page never printed ANIM-KF-END")
    time.sleep(0.6)
    kf2 = shot(ui, "kf_end", settle=0.30)
    d_ms = diffcount(kf1, kf0)
    d_me = diffcount(kf1, kf2)
    d_se = diffcount(kf0, kf2)
    print("keyframes: mid-vs-start %d px, mid-vs-end %d px, start-vs-end %d px"
          % (d_ms, d_me, d_se))
    ck(d_se > TH_MOVE,
       "kf sanity: the animation moved at all (start vs end %d px)" % d_se)
    ck(d_ms > TH_MOVE,
       "@keyframes: the MID frame differs from the START render (%d px)" % d_ms)
    ck(d_me > TH_MOVE,
       "@keyframes: the MID frame differs from the END-STATE render -- "
       "this is the assertion that is red before the clock exists (%d px)"
       % d_me)
    img2 = PPM(kf2)
    ck(img2.find_color((3, 1, 254)) is not None,
       "kf sanity: the slide box reached its end-state position/alpha")

    # ---------------- PHASE 3: transition on a class change ---------------
    # Marker-scheduled for the same guest-clock reason; the class change
    # (GO) is at +600 ms, MID at GO+900 ms, END at GO+2000 ms.
    navigate(ui, "trans.html", 175)
    wait_marker("ANIM-TRANS-READY", "trans page ready")
    tr0 = shot(ui, "trans_pre", settle=0.10)
    wait_marker("ANIM-TRANS-GO", "class change fired")
    if not wait_serial("ANIM-TRANS-MID", 60, poll=0.1, what="trans mid marker"):
        die("page never printed ANIM-TRANS-MID")
    tr1 = shot(ui, "trans_mid", settle=0.10)
    if not wait_serial("ANIM-TRANS-END", 60, poll=0.2, what="trans end marker"):
        die("page never printed ANIM-TRANS-END")
    time.sleep(0.6)
    tr2 = shot(ui, "trans_end", settle=0.30)
    d_ms = diffcount(tr1, tr0)
    d_me = diffcount(tr1, tr2)
    d_se = diffcount(tr0, tr2)
    print("transition: mid-vs-pre %d px, mid-vs-end %d px, pre-vs-end %d px"
          % (d_ms, d_me, d_se))
    ck(d_se > TH_MOVE,
       "transition sanity: the class change changed the pixels (%d px)" % d_se)
    ck(d_ms > TH_MOVE,
       "transition: the MID frame differs from the pre-change render (%d px)"
       % d_ms)
    ck(d_me > TH_MOVE,
       "transition: the MID frame differs from the END-STATE render -- "
       "red before the clock exists (%d px)" % d_me)

    # ---------------- PHASE 4: the cap, measured by the page ---------------
    navigate(ui, "cap.html", 175)
    if wait_serial("ANIM-CAP-DONE", 240, poll=0.5, what="cap phases"):
        for n, frames, ms in sorted(
                (int(a), int(b), int(c)) for a, b, c in re.findall(
                    r"ANIM-CAP n=(\d+) frames=(\d+) ms=(\d+)", serial())):
            print("cap: %3d animated boxes -> %3d frames / %.1f s"
                  % (n, frames, ms / 1000.0))
        base = re.findall(r"ANIM-CAP n=0 frames=(\d+)", serial())
        ck(base and int(base[-1]) > 20,
           "cap sanity: rAF itself delivered a healthy baseline "
           "(%s frames at n=0)" % (base[-1] if base else "no"))
        caps = re.findall(r"\[css-anim\] cap (\d+)", serial())
        if caps:
            print("cap: engine reported its cap line(s): cap=%s" %
                  ", ".join(caps))
    else:
        ck(False, "cap page never completed its phases")

    bad = sum(1 for ok, _ in checks if not ok)
    print("\nqmp_anim_page[%s]: %d checks, %d failures" % (TAG, len(checks), bad))
    print("artefacts in %s" % tmp)
    rc = 0 if bad == 0 else 1
finally:
    try:
        proc.terminate()
        proc.wait(timeout=10)
    except Exception:
        proc.kill()
raise SystemExit(rc)
