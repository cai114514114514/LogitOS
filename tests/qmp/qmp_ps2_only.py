#!/usr/bin/env python3
"""PS/2 input still works, and nothing is delivered twice.

Two machines, one script.

  DEFAULT: no xHCI controller at all. The regression guard for the USB line of
  work -- it does not test USB, it requires that PS/2 behaves exactly as it did
  before USB existed and that the USB driver produced no output whatsoever.

  --with-usb: an xHCI controller WITH a usb-kbd and usb-mouse, alongside the
  PS/2 controller. This is the coexistence case: two complete input stacks,
  both live, both feeding the same window-manager ring. The counting below is
  what makes it a real test of that -- if both stacks delivered the same
  keystroke, every character would arrive twice and the exactly-once assertion
  would fail.

Two failure modes are being watched for.

  DISPLACEMENT. A USB stack that grabs the input path, or that wedges the boot
  before the PS/2 drivers arm, silently kills every QMP test driver in this
  directory -- qmp_ui.py, qmp_browser.py, qmp_apps.py, qmp_term.py all inject
  PS/2 events. That failure would show up here as no input at all.

  DOUBLE DELIVERY. Both drivers feed the same window-manager input ring by
  design (wm_key / wm_mouse_event only enqueue, which is why a second producer
  is safe at all). The thing that must NOT happen is one physical event
  arriving twice. So this counts rather than merely detects: three distinct
  characters are typed and each must appear EXACTLY once, and one click must
  produce EXACTLY one press.

Motion is not counted, on purpose: c/kernel/gui/evq.c coalesces consecutive
motion samples, so the count is meaningfully not one-to-one and asserting on it
would be asserting on the coalescer.

    python3 tests/qmp/qmp_ps2_only.py <iso> <disk.img>
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
WITH_USB = "--with-usb" in sys.argv

EV_KEY, EV_MOUSE, EV_MOUSE_UP, EV_MOUSE_MOVE = 1, 2, 6, 7
EV_BTN_LEFT = 1

tmp = tempfile.mkdtemp(prefix="qmp_ps2_")
qmp_path = os.path.join(tmp, "qmp.sock")
ser_path = os.path.join(tmp, "serial.sock")

# The i8042 is present in both machines (it is the pc machine's default).
cmd = [
    QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
    "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
    "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
    "-display", "none", "-no-reboot",
    "-serial", f"unix:{ser_path},server=on,wait=off",
    "-qmp", f"unix:{qmp_path},server,nowait",
]
if WITH_USB:
    cmd += ["-device", "qemu-xhci,id=xhci",
            "-device", "usb-kbd,id=ukbd", "-device", "usb-mouse,id=umouse"]

proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

log = ""


def die(msg):
    print("FAIL: " + msg)
    print("----- serial output (tail) -----")
    print(log[-5000:])
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
    out = []
    for m in re.finditer(r"^EV (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\s*$",
                         log[mark:], re.M):
        out.append(tuple(int(g) for g in m.groups()))
    return out


if not wait_for("LOGIT_BOOT_OK", 240):
    die("the kernel did not boot" + (" with both input stacks present"
                                     if WITH_USB else
                                     " on a machine with no xHCI controller"))

if WITH_USB:
    if not wait_for("USB_READY", 60):
        die("the USB stack never reported ready, so this run is not testing "
            "coexistence -- it is testing PS/2 on its own")
    m = re.search(r"USB_READY devices=(\d+)", log)
    if not m or int(m.group(1)) < 2:
        die("the USB devices did not enumerate; coexistence is not under test")
    print(f"boot: ok, BOTH input stacks live "
          f"(PS/2 armed + {m.group(1)} USB HID devices enumerated)")
else:
    # With no USB controller the driver's probe() must never have run.
    if "USB_READY" in log:
        die("the USB stack reported ready on a machine with no xHCI device")
    if "[xhci]" in log:
        die("the xHCI driver produced output with no xHCI device present")
    print("boot: ok, and the USB driver correctly did nothing (no xHCI device)")

pump(5)
qcmd({"execute": "qmp_capabilities"})

ser.sendall(b"as /usr/as/examples/events.as &\n")
if not wait_for("EVENTS-READY", 120):
    die("events.as did not open its window")
pump(1.5)

# ------------------------------------------------------ keys, counted
# Short presses: PS/2 auto-repeats in hardware after ~500 ms, and this is
# counting deliveries, so each key is held only briefly.
mark = len(log)
for k in ("x", "y", "z"):
    send(key(k, True))
    pump(0.25)
    send(key(k, False))
    pump(0.45)
pump(1.0)

evs = events_since(mark)
keys = [e[1] for e in evs if e[0] == EV_KEY]
if not keys:
    die(f"NO PS/2 KEY reached the app. This is the regression that breaks every "
        f"QMP driver in tests/qmp at once. Events seen: {evs}")

for ch in "xyz":
    n = keys.count(ord(ch))
    if n == 0:
        die(f"the key '{ch}' never arrived; codes seen: {keys}")
    if n > 1:
        die(f"the key '{ch}' arrived {n} times for ONE press -- input is being "
            f"delivered twice; codes seen: {keys}")
print(f"ps2 keyboard: ok -- 'x','y','z' each delivered exactly once ({keys})")

# --------------------------------------------------- motion and one click
mark = len(log)
for _ in range(6):
    send(*rel(6, 4))
    pump(0.12)
pump(0.5)
moves = [e for e in events_since(mark) if e[0] == EV_MOUSE_MOVE]
if not moves:
    die("no PS/2 pointer motion reached the app")
for e in moves:
    if not (0 <= e[1] < 900 and 0 <= e[2] < 600):
        die(f"pointer event outside the window canvas: {e}")
print(f"ps2 mouse motion: ok -- {len(moves)} motions delivered, window-local")

mark = len(log)
send(btn("left", True))
pump(0.6)
send(btn("left", False))
pump(0.8)

evs = events_since(mark)
downs = [e for e in evs if e[0] == EV_MOUSE and e[4] == EV_BTN_LEFT]
ups = [e for e in evs if e[0] == EV_MOUSE_UP and e[4] == EV_BTN_LEFT]
if len(downs) != 1:
    die(f"one physical click produced {len(downs)} press events (want exactly 1) "
        f"-- clicks are never coalesced, so this is double delivery: {evs}")
if len(ups) != 1:
    die(f"one physical release produced {len(ups)} release events (want exactly 1): {evs}")
print("ps2 mouse buttons: ok -- one click delivered exactly once")

send(key("q", True), key("q", False))
pump(2)
proc.kill()

if WITH_USB:
    print("PASS: with an xHCI controller, a USB keyboard, a USB mouse AND the "
          "PS/2 controller all present at once, every physical event reached a "
          "ring-3 app EXACTLY ONCE -- two live input stacks, no double delivery.")
else:
    print("PASS: with no xHCI device the machine boots, the USB driver stays out "
          "of the way entirely, and PS/2 keyboard and mouse deliver input to a "
          "ring-3 app exactly once per physical event.")
sys.exit(0)
