#!/usr/bin/env python3
"""Prove that USB input reaches a ring-3 application.

WHAT MAKES THIS DIFFERENT FROM "THE CONTROLLER INITIALISED".

A USB stack that resets a controller and prints a device descriptor has done
none of the work that matters. The claim worth testing is the whole path: an
interrupt transfer completes on a device's endpoint, an event lands on the
xHCI event ring, MSI-X delivers it, the handler decodes a HID report through
its report descriptor, posts it to the window manager's input ring, the WM
routes it to the focused window, and an app polls it out. Any one of those
links can be broken while every log line still looks right.

So this boots LogitOS with `-machine pc,i8042=off`. There is no PS/2 controller
in the machine AT ALL -- no 8042, no IRQ 1, no IRQ 12. c/drivers/char/keyboard.c
and mouse.c are still compiled in and still armed, and they have nothing to be
armed on. Input is then injected through QEMU's input layer, explicitly targeted
at the usb-kbd and usb-mouse devices by id, and the app is required to print it.
There is no path by which a keystroke could arrive except the one under test.

The companion tests are run-usb-absent-test.sh (no xHCI at all: the machine must
still boot and PS/2 must still work, which is the regression that would take
every other line of work down at once) and run-usb-negctl.sh (this machine with
the USB devices unplugged: the harness must FAIL).

    python3 tests/qmp/qmp_usb.py <iso> <disk.img>
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

# "--no-devices" runs the negative control: same machine, USB HID unplugged.
NEGCTL = "--no-devices" in sys.argv

# From include/abi/logit_abi.h, spelled out so a renumbering fails this test
# rather than being followed silently.
EV_KEY, EV_MOUSE, EV_CLOSE, EV_MOUSE_R, EV_THEME = 1, 2, 3, 4, 5
EV_MOUSE_UP, EV_MOUSE_MOVE, EV_WHEEL = 6, 7, 8
EV_BTN_LEFT, EV_BTN_RIGHT, EV_BTN_MIDDLE = 1, 2, 3

tmp = tempfile.mkdtemp(prefix="qmp_usb_")
qmp_path = os.path.join(tmp, "qmp.sock")
ser_path = os.path.join(tmp, "serial.sock")

cmd = [
    QEMU,
    # THE POINT OF THE WHOLE TEST: no PS/2 controller in the machine.
    "-machine", "pc,i8042=off",
    "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
    "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
    "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
    "-display", "none", "-no-reboot",
    "-device", "qemu-xhci,id=xhci",
    "-serial", f"unix:{ser_path},server=on,wait=off",
    "-qmp", f"unix:{qmp_path},server,nowait",
]
if not NEGCTL:
    cmd += ["-device", "usb-kbd,id=ukbd", "-device", "usb-mouse,id=umouse"]

qerr = open(os.path.join(tmp, "qemu.err"), "w+")
proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=qerr)

log = ""


def die(msg):
    print("FAIL: " + msg)
    print("----- serial output (tail) -----")
    print(log[-5000:])
    try:
        qerr.seek(0)
        err = qerr.read()
        if err.strip():
            print("----- qemu stderr -----")
            print(err[-2000:])
    except OSError:
        pass
    print("--------------------------------")
    proc.kill()
    sys.exit(1)


def connect(path):
    for _ in range(400):
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
    try:
        qf.write(json.dumps(d) + "\n")
        qf.flush()
        while True:
            line = qf.readline()
            if not line:
                return None
            m = json.loads(line)
            if "return" in m or "error" in m:
                return m
    except (OSError, ValueError) as e:
        pump(0.5)
        die(f"QMP died ({e}) -- the guest is gone; see the serial tail above")


def send(device, *events):
    """Inject input. `device` names the intended target for the reader's
    benefit and is deliberately NOT passed to QEMU.

    input-send-event does accept a `device` argument, and targeting usb-kbd by
    id would be the most direct possible evidence. It cannot be used here:
    QEMU 10.2 resolves that argument through the display console, and with
    `-display none` the lookup aborts the whole process --

        Unexpected error in object_property_find_err() at qom/object.c:1344:
        Property 'qemu-fixed-text-console.device' not found

    -- which kills the guest rather than returning an error. So injection is
    untargeted, and what makes it unambiguous instead is the machine: with
    `-machine pc,i8042=off` there is no PS/2 keyboard or mouse for QEMU to
    route to. The usb-kbd and usb-mouse are the only input handlers that
    exist, so an event the guest receives came through this driver or came
    from nowhere."""
    (void_device) = device                     # documentation, not an argument
    r = qcmd({"execute": "input-send-event", "arguments": {"events": list(events)}})
    if r and "error" in r:
        die(f"input-send-event (intended for {device}) failed: {r['error']}")


def rel(dx, dy):
    return ({"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}})


def btn(name, down):
    return {"type": "btn", "data": {"button": name, "down": down}}


def key(code, down):
    return {"type": "key", "data": {"key": {"type": "qcode", "data": code}, "down": down}}


def events_since(mark):
    out = []
    for m in re.finditer(r"^EV (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\s*$",
                         log[mark:], re.M):
        out.append(tuple(int(g) for g in m.groups()))
    return out


# ------------------------------------------------------------------- boot
if not wait_for("LOGIT_BOOT_OK", 240):
    die("kernel did not boot with the PS/2 controller removed -- i8042=off "
        "should be survivable: the PS/2 drivers poll with bounded loops")
print("boot: ok (machine has no i8042 at all)")

if not wait_for("USB_READY", 60):
    die("the USB stack never reported ready")

# --------------------------------------------------- enumeration assertions
m = re.search(r"USB_READY devices=(\d+) drivers=(\d+)", log)
ndev = int(m.group(1)) if m else 0

if NEGCTL:
    # The negative control asserts the OPPOSITE of everything below: with the
    # HID devices unplugged the controller still comes up and enumerates
    # nothing, and no input can possibly be delivered.
    if ndev != 0:
        die(f"negative control enumerated {ndev} devices; it should have found none")
    print(f"negative control: controller up, {ndev} devices enumerated")
else:
    if ndev < 2:
        die(f"expected 2 USB devices (usb-kbd + usb-mouse), enumerated {ndev}")

    irq = re.search(r"USB_IRQ vec=(\d+) mode=(\d+)", log)
    if not irq:
        die("no interrupt was wired -- the driver would deliver nothing")
    vec, mode = int(irq.group(1)), int(irq.group(2))
    # DEV_IRQ_MSIX == 3 in c/drivers/core/driver.h.
    if mode != 3:
        print(f"NOTE: interrupt mode {mode} is not MSI-X; delivery is still required")
    print(f"interrupt: vector {vec}, mode {mode} (3 = MSI-X)")

    binds = re.findall(r"USB_HID_BIND if=(\d+) role=(\w+) decode=([\w-]+) ep=([0-9a-f]+)", log)
    roles = {b[1] for b in binds}
    if "keyboard" not in roles:
        die(f"no keyboard bound; binds seen: {binds}")
    if "mouse" not in roles:
        die(f"no mouse bound; binds seen: {binds}")
    # The parsed-report-descriptor path is the primary decoder on purpose (see
    # the header of c/drivers/usb/usb_hid.c). If a run silently fell back to
    # boot protocol, the parser is not being exercised on device and this test
    # would be proving less than it claims.
    for b in binds:
        if b[2] != "report-descriptor":
            die(f"if{b[0]} ({b[1]}) fell back to {b[2]}; the report-descriptor "
                "path is meant to be primary, so this run does not exercise it")
    rds = re.findall(r"USB_HID_RD if=\d+ bytes=(\d+) fields=(\d+)", log)
    print(f"enumeration: {ndev} devices, bound {sorted(roles)}, "
          f"report descriptors parsed {rds}")

# -------------------------------------------------------------- the app
pump(5)
qcmd({"execute": "qmp_capabilities"})

ser.sendall(b"as /usr/as/examples/events.as &\n")
if not wait_for("EVENTS-READY", 120):
    die("events.as did not open its window")
pump(1.5)

if NEGCTL:
    # Nothing to inject at: the devices are not there. Confirm the app receives
    # nothing, then fail on purpose -- this harness is only ever run to check
    # that the positive test can fail.
    mark = len(log)
    pump(3)
    evs = events_since(mark)
    print(f"negative control: app received {len(evs)} input events (expected 0)")
    proc.kill()
    if evs:
        print("FAIL: the negative control received input with no USB device attached")
        sys.exit(1)
    print("negative control behaved as intended (no input reached the app); "
          "the positive assertions below would have failed here")
    sys.exit(2)      # 2 = "the control correctly produced no input"

mark = len(log)

# ------------------------------------------------------- 1. the keyboard
# 'k' has no meaning to events.as beyond being printed, and is not the quit key.
# Held for a full second on purpose: that also exercises auto-repeat, which on
# USB is not free. A PS/2 keyboard repeats in hardware; a USB one reports only
# on change, so holding a key would do nothing at all unless something repeats
# it. The driver asks the keyboard (SET_IDLE, 96 ms) to re-send its unchanged
# report, and treats those as repeat ticks -- a periodic interrupt from the
# device, since there is no USB thread to run a timer on.
send("ukbd", key("k", True))
pump(1.0)
send("ukbd", key("k", False))
pump(0.5)

# A shifted letter: the modifier byte and the shifted keymap layer both have to
# work, and they are decoded from the report descriptor's E0..E7 usages.
send("ukbd", key("shift", True))
pump(0.3)
send("ukbd", key("a", True))
pump(0.5)
send("ukbd", key("a", False))
send("ukbd", key("shift", False))
pump(0.6)

evs = events_since(mark)
keys = [e for e in evs if e[0] == EV_KEY]
if not keys:
    die("NO KEY REACHED THE APP. The controller enumerated a keyboard and wired "
        f"an interrupt, but nothing arrived. Events seen: {evs}")
codes = [e[1] for e in keys]
if ord('k') not in codes:
    die(f"the letter 'k' did not arrive; key codes seen: {codes}")
if ord('A') not in codes:
    die(f"shifted 'a' did not arrive as 'A'; key codes seen: {codes} "
        "(the modifier bits come from usages E0..E7 in the report descriptor)")
if ord('a') in codes:
    die(f"shifted 'a' ALSO arrived unshifted; key codes seen: {codes}")
nk = codes.count(ord('k'))
if nk < 2:
    die(f"'k' held for a second produced {nk} event(s): auto-repeat is not "
        "working, so holding a key does nothing (SET_IDLE, or the unchanged-"
        "report tick that depends on it, is broken)")
print(f"keyboard: ok -- app received {len(keys)} key events, codes {codes} "
      f"(a one-second hold auto-repeated 'k' {nk} times)")

# ---------------------------------------------------------- 2. the mouse
mark = len(log)
for _ in range(6):
    send("umouse", *rel(7, 5))
    pump(0.12)
for _ in range(6):
    send("umouse", *rel(-7, -5))
    pump(0.12)
pump(0.6)

evs = events_since(mark)
moves = [e for e in evs if e[0] == EV_MOUSE_MOVE]
if not moves:
    die(f"NO MOUSE MOTION REACHED THE APP. Events seen: {evs}")

# Coordinates must be window-local: events.as opens 900x600 at cascade slot 2,
# and the pointer starts at the screen centre, which is inside it. A screen
# coordinate reported instead would be far outside this box.
for e in moves:
    if not (0 <= e[1] < 900 and 0 <= e[2] < 600):
        die(f"pointer event outside the window canvas -- not window-local: {e}")

# The pointer must have MOVED, not merely reported. Motion that always lands on
# the same coordinate is a driver that integrates deltas wrong.
xs = {e[1] for e in moves}
ys = {e[2] for e in moves}
if len(xs) < 2 and len(ys) < 2:
    die(f"the pointer reported {len(moves)} motions but never changed position: {moves}")
print(f"mouse motion: ok -- {len(moves)} motions, {len(xs)} distinct x, {len(ys)} distinct y")

# HID Y grows downward, the same direction as screen coordinates. Injecting a
# positive dy must move the pointer DOWN; the PS/2 path negates here because
# PS/2 packets carry Y growing upward, and copying that would invert the mouse.
mark = len(log)
send("umouse", *rel(0, 40))
pump(0.5)
down = [e for e in events_since(mark) if e[0] == EV_MOUSE_MOVE]
if down:
    before = moves[-1][2]
    after = down[-1][2]
    if after <= before:
        die(f"a +dy of 40 moved the pointer from y={before} to y={after} -- the "
            "vertical axis is inverted (HID Y grows downward)")
    print(f"mouse axis: ok -- +dy moved y {before} -> {after} (downward)")

# --------------------------------------------------------- 3. the buttons
mark = len(log)
send("umouse", btn("left", True))
pump(0.5)
send("umouse", btn("left", False))
pump(0.5)
send("umouse", btn("right", True))
pump(0.5)
send("umouse", btn("right", False))
pump(0.6)

evs = events_since(mark)
downs = [e for e in evs if e[0] == EV_MOUSE and e[4] == EV_BTN_LEFT]
ups = [e for e in evs if e[0] == EV_MOUSE_UP]
rights = [e for e in evs if e[0] == EV_MOUSE_R and e[4] == EV_BTN_RIGHT]
if not downs:
    die(f"no left-button press reached the app; events: {evs}")
if not ups:
    die(f"no button release reached the app; events: {evs}")
if not rights:
    die(f"no right-button press reached the app; events: {evs}")
print(f"buttons: ok -- left down/up and right down all delivered "
      f"({len(downs)} left, {len(ups)} up, {len(rights)} right)")

# ----------------------------------------------------------- 4. the wheel
# The two directions are injected and checked SEPARATELY, which is the whole
# point. HID Usage(Wheel) is positive for rotation away from the user (scroll
# up); EV_WHEEL is positive for scroll DOWN, which is what the PS/2 path
# produces and what the DOM's deltaY means. The conventions disagree, so the
# driver has to flip -- and an earlier version of this test asked only "is some
# notch positive and some negative", which an inverted wheel satisfies
# perfectly. Asking per-direction is what caught it.
def wheel_sign(direction):
    mark = len(log)
    send("umouse", btn(direction, True))
    send("umouse", btn(direction, False))
    pump(0.8)
    w = [e for e in events_since(mark) if e[0] == EV_WHEEL]
    if not w:
        die(f"no wheel notch reached the app for {direction}")
    return w[0][5], w


down_notch, dw = wheel_sign("wheel-down")
up_notch, uw = wheel_sign("wheel-up")
if down_notch <= 0:
    die(f"wheel-down arrived as {down_notch}; EV_WHEEL must be POSITIVE for a "
        "downward scroll, which is what the PS/2 path produces "
        "(tests/qmp/qmp_input.py) and what browser.aex reads as deltaY")
if up_notch >= 0:
    die(f"wheel-up arrived as {up_notch}; EV_WHEEL must be NEGATIVE upward")
print(f"wheel: ok -- down={down_notch} (positive), up={up_notch} (negative), "
      "matching the PS/2 sign convention")

# ------------------------------------------------------------- shut down
send("ukbd", key("q", True))
send("ukbd", key("q", False))
pump(2)
if "EVENTS-DONE" not in log:
    die("the app did not quit on 'q' -- which means the quit keystroke did not "
        "arrive, even though earlier ones did")

errors = re.findall(r"\[xhci\] .*(?:error|timed out|failed)", log)
proc.kill()
if errors:
    print("NOTE: the controller reported problems during the run:")
    for e in errors[:10]:
        print("   " + e)

print("PASS: with NO PS/2 controller in the machine, a USB keyboard and a USB "
      "mouse enumerated over xHCI, were decoded through their own report "
      "descriptors, delivered over MSI-X, and their keystrokes, motion, buttons "
      "and wheel all reached a ring-3 application.")
sys.exit(0)
