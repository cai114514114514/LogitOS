#!/usr/bin/env python3
"""Drive real input at LogitOS and check what an app actually receives.

Two things are under test, and neither can be seen from inside the kernel.

1. THE EVENT ABI. EV_MOUSE_UP / EV_MOUSE_MOVE / EV_WHEEL, the button id and the
   modifier flags each cross a PS/2 packet, an IRQ, the WM's deferred input
   queue, window routing and a coalescing ring before an app sees them. This
   launches /usr/as/examples/events.as -- an app whose whole job is to print the
   events it is handed -- and injects a click, a release, a right-click, wheel
   notches and a shifted click through QEMU's input layer, then checks the lines.

2. THE RING DOES NOT OVERFLOW UNDER MOTION. The per-window ring is 256 entries
   and drops silently when full. c/kernel/gui/evq.c coalesces a motion sample
   onto an unread motion sample so motion occupies at most one slot. This floods
   the pointer and reads the kernel's own queued/merged/dropped counters back
   through SYS_SYSINFO -- a measurement, not an assertion. tests/unit/evq_test.c
   is the deterministic half of the same proof (100k samples, host-side); this
   is the half that exercises the real path with a live app draining it.

    python3 tests/qmp/qmp_input.py <iso> <disk.img>

Two unix sockets: QMP to inject input, and a bidirectional serial console so the
harness can type into /bin/sh. (The other boot tests pipe stdin, which cannot be
combined with driving QMP from the same process.)
"""

import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# From include/abi/logit_abi.h. Spelled out rather than parsed: if a renumbering
# ever silently changes what an app receives, this test should FAIL, not follow.
EV_KEY, EV_MOUSE, EV_CLOSE, EV_MOUSE_R, EV_THEME = 1, 2, 3, 4, 5
EV_MOUSE_UP, EV_MOUSE_MOVE, EV_WHEEL = 6, 7, 8
EV_MOD_SHIFT, EV_MOD_CTRL, EV_MOD_ALT = 1, 2, 4
EV_BTN_LEFT, EV_BTN_RIGHT, EV_BTN_MIDDLE = 1, 2, 3

tmp = tempfile.mkdtemp(prefix="qmp_input_")
qmp_path = os.path.join(tmp, "qmp.sock")
ser_path = os.path.join(tmp, "serial.sock")

# 1280x800 so the desktop geometry is the one the WM lays out at boot: the Finder
# takes cascade slot 0 and the Clock slot 1, so events.as gets slot 2 at
# (166,126). Its 900x600 window therefore covers the screen centre (640,400),
# which is where the cursor starts -- so the pointer needs no walking to a target
# this harness cannot observe.
proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", f"file={DISK},format=raw,if=none,id=hd0",
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-serial", f"unix:{ser_path},server=on,wait=off",
     "-qmp", f"unix:{qmp_path},server,nowait"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

log = ""


def die(msg):
    print("FAIL: " + msg)
    print("----- serial output (tail) -----")
    print(log[-4000:])
    print("--------------------------------")
    proc.kill()
    sys.exit(1)


def connect(path):
    for _ in range(300):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except OSError:
            if proc.poll() is not None:
                die("qemu exited before its sockets came up")
            time.sleep(0.1)
    die(f"could not connect to {path}")


ser = connect(ser_path)
ser.setblocking(False)
qmp = connect(qmp_path)
qf = qmp.makefile("rw")


def pump(seconds):
    """Drain the serial socket for `seconds`, accumulating into `log`."""
    global log
    end = time.time() + seconds
    while time.time() < end:
        try:
            b = ser.recv(65536)
            if b:
                log += b.decode("utf-8", "replace")
                continue
        except (BlockingIOError, OSError):
            pass
        time.sleep(0.03)


def wait_for(marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.2)
        if marker in log:
            return True
        if proc.poll() is not None:
            return marker in log
    return False


def qcmd(d):
    qf.write(json.dumps(d) + "\n")
    qf.flush()
    while True:
        line = qf.readline()
        if not line:
            return None
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m


def send(*events):
    qcmd({"execute": "input-send-event", "arguments": {"events": list(events)}})


def rel(dx, dy):
    return ({"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}})


def btn(name, down):
    return {"type": "btn", "data": {"button": name, "down": down}}


def key(code, down):
    return {"type": "key", "data": {"key": {"type": "qcode", "data": code}, "down": down}}


def events_since(mark):
    """The 'EV type a b mods button wheel' lines printed after `mark`."""
    out = []
    for m in re.finditer(r"^EV (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\s*$",
                         log[mark:], re.M):
        out.append(tuple(int(g) for g in m.groups()))
    return out


def stats():
    """Run evqstat.as on the guest shell and parse the WM's event counters."""
    mark = len(log)
    ser.sendall(b"as /usr/as/examples/evqstat.as\n")
    end = time.time() + 60
    while time.time() < end:
        pump(0.3)
        m = re.search(r"Events (\d+) queued, (\d+) merged, (\d+) dropped", log[mark:])
        if m:
            return tuple(int(g) for g in m.groups())
    die("evqstat.as printed no Events line")


# ---------------------------------------------------------------- boot + app
if not wait_for("LOGIT_BOOT_OK", 180):
    die("kernel did not boot")
pump(6)                                    # let the desktop launch Finder + Clock
qcmd({"execute": "qmp_capabilities"})

ser.sendall(b"as /usr/as/examples/events.as &\n")
if not wait_for("EVENTS-READY", 90):
    die("events.as did not open its window")
pump(1)

# ------------------------------------------------------- 1. the event stream
mark = len(log)

send(*rel(4, 3)); pump(0.25)               # motion
send(*rel(-4, -3)); pump(0.25)

send(btn("left", True)); pump(0.4)         # press / release
send(btn("left", False)); pump(0.4)

send(btn("right", True)); pump(0.4)
send(btn("right", False)); pump(0.4)

send(btn("wheel-down", True)); send(btn("wheel-down", False)); pump(0.4)
send(btn("wheel-up", True)); send(btn("wheel-up", False)); pump(0.4)

send(key("shift", True)); pump(0.2)        # modifier carried on a click
send(btn("left", True)); pump(0.4)
send(btn("left", False)); pump(0.4)
send(key("shift", False)); pump(0.5)

evs = events_since(mark)
if not evs:
    die("the app received no events at all")


def find(pred, what):
    for e in evs:
        if pred(e):
            return e
    die(f"no event matching {what}\n  got: {evs}")


# (type, a, b, mods, button, wheel)
find(lambda e: e[0] == EV_MOUSE_MOVE, "EV_MOUSE_MOVE")
find(lambda e: e[0] == EV_MOUSE and e[4] == EV_BTN_LEFT and e[3] == 0,
     "EV_MOUSE with button=LEFT and no modifiers")
find(lambda e: e[0] == EV_MOUSE_UP and e[4] == EV_BTN_LEFT, "EV_MOUSE_UP with button=LEFT")
find(lambda e: e[0] == EV_MOUSE_R and e[4] == EV_BTN_RIGHT, "EV_MOUSE_R with button=RIGHT")
find(lambda e: e[0] == EV_MOUSE_UP and e[4] == EV_BTN_RIGHT, "EV_MOUSE_UP with button=RIGHT")
find(lambda e: e[0] == EV_WHEEL and e[5] > 0, "EV_WHEEL scrolling down (positive)")
find(lambda e: e[0] == EV_WHEEL and e[5] < 0, "EV_WHEEL scrolling up (negative)")
find(lambda e: e[0] == EV_MOUSE and e[3] & EV_MOD_SHIFT, "a shifted EV_MOUSE (mods carries SHIFT)")

# Coordinates are window-local. The window is at (166,126) with a 30px titlebar
# and a 900x600 canvas, and the pointer sits near the screen centre, so every
# pointer event must land inside the canvas -- an event reported at the screen
# coordinate instead would be far outside it.
for e in evs:
    if e[0] in (EV_MOUSE, EV_MOUSE_R, EV_MOUSE_UP, EV_MOUSE_MOVE, EV_WHEEL):
        if not (0 <= e[1] < 900 and 0 <= e[2] < 600):
            die(f"pointer event outside the window canvas -- coordinates are not "
                f"window-local: {e}")

print(f"event ABI: ok ({len(evs)} events; move/press/release/right/wheel+-/shift all seen)")

# ---------------------------------------------------------- 2. the ring flood
base = stats()

# The PS/2 mouse is a RELATIVE device -- "abs" events need a tablet and QEMU
# silently discards them for PS/2, which from in here looks exactly like a broken
# window manager. One command per sample, too: QEMU sums the relative axes inside
# one input-send-event group, so batching injects a single large jump instead of
# N samples, which is the opposite of a flood.
#
# What bounds the numbers at the other end is the device: PS/2 reports at ~100
# samples/second, so the GUEST sees ~100 motion events per second however fast
# the host injects. That is also the real-hardware rate, and it is why 256 slots
# are at risk -- not because a mouse outruns the ring, but because an app that
# stops polling for a couple of seconds (a synchronous layout, a blocking fetch)
# lets two seconds of pointer fill it.
SAMPLES = 9000
JITTER = [(3, 2), (-3, 2), (3, -2), (-3, -2)]
t0 = time.time()
for n in range(SAMPLES):
    dx, dy = JITTER[n % 4]
    send(*rel(dx, dy))
print(f"flooded {SAMPLES} pointer samples in {time.time() - t0:.1f}s")
pump(3)

after = stats()
d_queued, d_merged, d_dropped = (after[i] - base[i] for i in range(3))
print(f"ring: queued +{d_queued}, merged +{d_merged}, dropped +{d_dropped}")

send(key("q", True), key("q", False))      # events.as quits on 'q'
pump(2)
ser.sendall(b"exit\n")
pump(1)
proc.kill()

if d_dropped != 0:
    die(f"{d_dropped} events dropped during the flood -- the ring overflowed")
if d_merged == 0:
    die("coalescing never engaged (0 merged), so this run proves nothing about it")
if d_merged <= d_queued:
    die(f"only {d_merged} of {d_merged + d_queued} events merged -- coalescing is "
        "firing occasionally rather than doing the work")

total = d_merged + d_queued
# How much of the ring the motion would have taken WITHOUT coalescing, if the app
# had been mid-repaint the whole time -- which is the case the ring has to
# survive. Stated as a fraction rather than "it would have overflowed", because
# how many samples get through depends on host load and a run that only sends 96
# does not prove overflow; what every run does prove is that motion took
# d_queued slots instead of `total`.
print(f"PASS: event ABI verified end to end; {SAMPLES} injected samples -> {total} events "
      f"reached the ring, {d_merged} merged into the tail, {d_queued} took a slot, "
      f"0 dropped (unmerged, those {total} would have filled {100 * total // 256}% of the "
      f"256-slot ring)")
sys.exit(0)
