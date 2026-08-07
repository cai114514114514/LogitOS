#!/usr/bin/env python3
"""Is the window manager still alive after real use? -- as an ASSERTION.

    python3 tests/qmp/qmp_freeze.py <iso> <disk.img> [outdir]

This file used to be the reason this line exists. It reproduced a user's exact
sequence -- open Code Studio, type into it, open the Terminal, run a command,
drag a window -- screendumped between steps, printed

    DIFF term-open->typed: 3
    DIFF typed->dragged: 0

and then exited 0. Both numbers, and any other pair of numbers, were a pass. Its
dock coordinates were literals (832,753) and (576,753) that had already stopped
being derived from anything, and its "diff" sampled one byte in 997, so a frozen
compositor and a working one were indistinguishable to it. It could report that
QEMU had died. It could report nothing at all about the guest.

What it does now:

  * geometry comes from tests/qmp/qmp_ui.py, so a display-mode or app-count
    change moves the clicks instead of orphaning them;
  * each dock click is CONFIRMED by the guest's own `[wm] launched <name>` on
    the serial console, so a click that misses is an error rather than an
    app that mysteriously did not open;
  * every "the screen changed" claim is measured against a QUIET BASELINE taken
    over the same duration with no input at all. On this desktop the menu-bar
    clock and the Clock app repaint on their own, so "some pixels differ" is
    the null hypothesis, not the result;
  * and it exits non-zero when any of that fails.

The freeze this hunts is real: a WM whose input loop has wedged still paints
its animations, so a screenshot looks fine. What it cannot do is respond -- so
the test is "did MY input change the picture, much more than time alone did".
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                     # noqa: E402
from qmp_ui import PPM, Session, dock_icon                        # noqa: E402

if len(sys.argv) < 3:
    sys.stderr.write(__doc__)
    sys.exit(2)
ISO, DISK = sys.argv[1], sys.argv[2]
OUT = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp(prefix="freeze_")
os.makedirs(OUT, exist_ok=True)

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
MODE_W, MODE_H = (int(v) for v in os.environ.get("FS_MODE", "1280x800").split("x"))
SCALE = qmp_ui.configure(MODE_W, MODE_H)

# scan_apps() order, which is the vfs enumeration order of *.aex at the root:
# clock textedit monitor terminal widgets files preview studio browser.
# Both slots below are CHECKED against the name the guest reports, so a
# reordering is a failure message and not a silently different test.
STUDIO_SLOT, STUDIO_NAME = 7, "Code Studio"
TERM_SLOT, TERM_NAME = 3, "Terminal"

tmp = tempfile.mkdtemp(prefix="freeze_run_")
ser_path = os.path.join(tmp, "ser.sock")
qmp_path = os.path.join(tmp, "qmp.sock")

srv = socket.socket(socket.AF_UNIX)
srv.bind(ser_path)
srv.listen(1)
buf = bytearray()
lock = threading.Lock()


def _pump():
    conn, _ = srv.accept()
    while True:
        try:
            data = conn.recv(8192)
        except OSError:
            return
        if not data:
            return
        with lock:
            buf.extend(data)


threading.Thread(target=_pump, daemon=True).start()


def serial():
    with lock:
        return bytes(buf).decode("utf-8", "replace")


proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
     "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-rtc", "base=localtime",
     "-vga", "none",
     "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (MODE_W, MODE_H),
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-display", "none", "-no-reboot",
     "-chardev", "socket,id=ser0,path=%s" % ser_path, "-serial", "chardev:ser0",
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

ROWS = []


def ck(cond, name, detail=""):
    ROWS.append((bool(cond), name, detail))
    print(("ok  : " if cond else "FAIL: ") + name +
          (("  -- " + detail) if detail else ""), flush=True)
    return bool(cond)


def done(code=0):
    with open(os.path.join(OUT, "freeze-serial.log"), "w",
              encoding="utf-8") as fh:
        fh.write(serial())
    try:
        proc.kill()
    except OSError:
        pass
    bad = [r for r in ROWS if not r[0]]
    print("\n---- freeze check ----")
    for ok, name, detail in ROWS:
        print("  %-4s %s%s" % ("ok" if ok else "FAIL", name,
                               ("  -- " + detail) if detail else ""))
    if bad:
        print("\nFAIL: %d of %d checks failed; artefacts in %s"
              % (len(bad), len(ROWS), OUT))
        print(serial()[-3000:])
        sys.exit(1)
    print("\nPASS: the desktop still responds to input after the user's sequence")
    sys.exit(code)


def wait_serial(needle, secs):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            ck(False, "QEMU stayed up", "it exited with %r" % proc.poll())
            done(1)
        time.sleep(0.25)
    return False


if not wait_serial("LOGIT_BOOT_OK", 300):
    ck(False, "the kernel booted")
    done(1)
ck(True, "the kernel booted")
if not wait_serial("desktop live", 120):
    ck(False, "the compositor brought the desktop up")
    done(1)
ck(True, "the compositor brought the desktop up")
time.sleep(4)

ui = Session(qmp_path)
shot_n = [0]


def shot(tag):
    shot_n[0] += 1
    p = os.path.join(OUT, "%02d-%s.ppm" % (shot_n[0], tag))
    return PPM(ui.screendump(p, settle=0.6))


def changed(a, b):
    """Pixels that differ between two frames. Whole frame, every pixel.

    The old sampled version looked at one byte in 997, which is 0.1% of the
    evidence and turns a real change into a coin flip."""
    n = 0
    row = a.w * 3
    for y in range(min(a.h, b.h)):
        ra = a.px[y * row:(y + 1) * row]
        rb = b.px[y * row:(y + 1) * row]
        if ra == rb:
            continue
        for x in range(0, row, 3):
            if ra[x:x + 3] != rb[x:x + 3]:
                n += 1
    return n


def launched():
    return re.findall(r"\[wm\] launched (.+)", serial())


def open_app(slot, name):
    before = len(launched())
    ui.click_at(*dock_icon(slot))
    end = time.time() + 90
    while time.time() < end:
        got = launched()
        if len(got) > before:
            return ck(got[before].strip() == name,
                      "the Dock opened %s from slot %d" % (name, slot),
                      "the guest said it launched %r" % got[before].strip())
        if "already live, focusing" in serial()[-4000:]:
            return ck(True, "the Dock opened %s from slot %d" % (name, slot),
                      "already running; focused")
        time.sleep(0.3)
    return ck(False, "the Dock opened %s from slot %d" % (name, slot),
              "no launch appeared -- the click at %r hit nothing"
              % (dock_icon(slot),))


# ---- the QUIET BASELINE -----------------------------------------------------
# How much of this desktop repaints on its own? The menu-bar clock ticks and the
# Clock app redraws every second, so a bare "the pixels changed" claim is worth
# nothing until it is compared with this. Measured over the same wall-clock
# span the input steps below take.
QUIET = 6.0
q0 = shot("quiet-a")
time.sleep(QUIET)
q1 = shot("quiet-b")
BASE = changed(q0, q1)
print("baseline: %d pixels change in %.0f s with NO input" % (BASE, QUIET),
      flush=True)
FLOOR = max(BASE * 8, 4000)

# ---- 1. Code Studio, and typing into it -------------------------------------
open_app(STUDIO_SLOT, STUDIO_NAME)
time.sleep(4)
a = shot("studio-open")
for ch in "print":
    ui.key(ch, settle=0.12)
time.sleep(2.5)
b = shot("studio-typed")
d = changed(a, b)
ck(d > FLOOR, "typing into %s changed the screen" % STUDIO_NAME,
   "%d pixels changed vs a %d-pixel quiet baseline (floor %d)"
   % (d, BASE, FLOOR))

# ---- 2. the Terminal, and a command in it -----------------------------------
open_app(TERM_SLOT, TERM_NAME)
time.sleep(5)
c = shot("term-open")
ui.typ("uname\n")
time.sleep(4)
e = shot("term-typed")
d = changed(c, e)
ck(d > FLOOR, "a command typed into the %s produced output" % TERM_NAME,
   "%d pixels changed vs a %d-pixel quiet baseline (floor %d)"
   % (d, BASE, FLOOR))

# ---- 3. is the COMPOSITOR still taking input? -------------------------------
# Dragging a window is the cheapest question that only the window manager itself
# can answer: it is not the app repainting, it is the WM moving a surface and
# recompositing everything under it. wm.c places the nth window at
# S(110+28n), S(70+28n); the Terminal is the 4th (Finder, Clock, Code Studio,
# Terminal), so its titlebar is 30 points tall starting there. If that arithmetic
# is ever wrong the press lands in the app's content instead and this check
# fails -- which is the correct outcome, not a false alarm to be tuned away.
TERM_CASCADE = 3
tx = qmp_ui.pt(110 + 28 * TERM_CASCADE)
ty = qmp_ui.pt(70 + 28 * TERM_CASCADE)
grip = (tx + qmp_ui.pt(200), ty + qmp_ui.pt(14))
before = shot("pre-drag")
ui.goto(*grip)
ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
time.sleep(0.2)
ui.goto(grip[0] - qmp_ui.pt(180), grip[1] + qmp_ui.pt(90))
time.sleep(0.3)
ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
time.sleep(2.5)
after = shot("post-drag")
d = changed(before, after)
ck(d > FLOOR, "the window manager still moves a window when dragged",
   "%d pixels changed vs a %d-pixel quiet baseline (floor %d)"
   % (d, BASE, FLOOR))

# ---- 4. the control -------------------------------------------------------
# Everything above says "the screen changed a lot". This says the screen does
# NOT change a lot on its own, taken AFTER all of the above so it speaks about
# the machine in the state the assertions were made in. Without it, a desktop
# with a busy animation somewhere would pass every check while frozen to input.
z0 = shot("quiet-c")
time.sleep(QUIET)
z1 = shot("quiet-d")
idle = changed(z0, z1)
ck(idle < FLOOR, "CONTROL: with no input, the screen stays put",
   "%d pixels changed in %.0f idle seconds (floor %d)" % (idle, QUIET, FLOOR))

done(0)
