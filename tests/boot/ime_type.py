#!/usr/bin/env python3
"""Drive the pinyin IME on a live machine over QMP: open TextEdit, type, save.

    ime_type.py <iso> <workdisk.img> <mode> <screenshot.ppm>
        mode = "ime"   -- Shift+Space first, so the letters go to the IME
               "ascii" -- the SAME keys with no Shift+Space (the negative control)

Exits 0 if the machine got as far as saving; the BYTES are asserted by the
caller (tests/boot/run-ime-test.sh) out of the disk image, because "what is on
media" is the only version of this question that cannot be answered by the
guest agreeing with itself.

NO -snapshot. The whole product is a file with 6 particular bytes in it, so the
write has to survive to the host. The caller passes a COPY of disk.img for
exactly the reason tests/boot/run-core-test.sh gives.

PACING. Every key is ONE input-send-event with ONE event in it, followed by a
sleep. The PS/2 controller has a one-byte output buffer and CLAUDE.md records
what happens when that is ignored: qmp_site.py sent Ctrl+L as four scancodes in
two bursts and the chord never arrived -- and it went unnoticed for weeks
because the browser's address bar was already focused, so "the chord did not
arrive" and "the chord worked" looked identical. This driver's chord has no
such alibi: if Shift+Space does not arrive, the guest never prints
`[ime] window N: pinyin ON` and this script says so and stops.

WHAT THIS HARNESS STRUCTURALLY CANNOT SEE, and it cost the owner the feature.
QMP injects scancodes BENEATH the host keyboard, so every link from the user's
fingers to QEMU is bypassed here by construction. This gate passed green while
the IME was unusable in practice, because the chord was Ctrl+Space and macOS --
the host this machine is developed on -- consumes Ctrl+Space itself as "select
the previous input source". A green run of this file means the guest works; it
has never meant a person can type Chinese. The chord is Shift+Space now for
that reason, and the reason is recorded here because the next chord change will
be argued from this file.
"""
import json
import re
import struct
from pathlib import Path
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from ime_geometry import textedit_frames

if len(sys.argv) != 5:
    sys.exit(__doc__)
iso, disk, mode, shot = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
if mode not in ("ime", "ascii", "usability"):
    sys.exit("ime_type.py: mode must be 'ime', 'ascii' or 'usability'")

fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
ser = tempfile.mktemp(suffix=".ser")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen([
    qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
    # Input/event ownership must run concurrently in the BKL regression.
    # Keep a one-core override for diagnosis; default real typing to four CPUs.
    "-smp", os.environ.get("IME_TEST_SMP", "4"), "-accel", "tcg,thread=multi",
    "-drive", f"file={disk},format=raw,if=none,id=hd0,file.locking=off",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-m", os.environ.get("IME_TEST_RAM", "512M"), "-vga", "none", "-device", "virtio-gpu-pci",
    "-display", "none", "-no-reboot",
    "-chardev", f"socket,id=ser0,path={ser},server=on,wait=on",
    "-serial", "chardev:ser0",
    "-qmp", f"unix:{sock},server,nowait"])

serial = socket.socket(socket.AF_UNIX)
for _ in range(400):
    try:
        serial.connect(ser); break
    except OSError:
        if proc.poll() is not None:
            sys.exit("qemu died before the serial socket appeared")
        time.sleep(0.1)
log = bytearray()
stop = False


def reader():
    while not stop:
        try:
            b = serial.recv(65536)
            if not b:
                break
            log.extend(b)
        except OSError:
            time.sleep(0.05)


threading.Thread(target=reader, daemon=True).start()

fails = []


def chk(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond:
        fails.append(msg)


def bail(msg):
    print("FAIL: " + msg, flush=True)
    try:
        with open(shot + ".log", "wb") as f:
            f.write(bytes(log))
    except OSError:
        pass
    proc.kill()
    sys.exit(1)


deadline = time.time() + 180
while b"LOGIT_BOOT_OK" not in log and time.time() < deadline:
    if proc.poll() is not None:
        bail("qemu died before boot")
    time.sleep(0.1)
if b"LOGIT_BOOT_OK" not in log:
    bail("never booted")
time.sleep(2.0)

s = socket.socket(socket.AF_UNIX)
for _ in range(80):
    try:
        s.connect(sock); break
    except OSError:
        time.sleep(0.1)
f = s.makefile("rw")


def recv():
    while True:
        line = f.readline()
        if not line:
            return None
        m = json.loads(line)
        if "return" in m or "error" in m:
            return m


def cmd(d):
    f.write(json.dumps(d) + "\n"); f.flush(); return recv()


cur = [640, 400]


def goto(tx, ty):
    while cur[0] != tx or cur[1] != ty:
        sx = max(-120, min(120, tx - cur[0])); sy = max(-120, min(120, ty - cur[1]))
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
        cur[0] += sx; cur[1] += sy; time.sleep(0.05)
    time.sleep(0.2)


def click():
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": d}}]}})
        time.sleep(0.14)


def raw(q, down):
    """ONE scancode, then a pause. See the pacing note in the docstring."""
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": down}}]}})
    time.sleep(0.14)


def key(q):
    raw(q, True); raw(q, False)


def ctrl_key(q):
    raw("ctrl", True)
    raw(q, True)
    raw(q, False)
    raw("ctrl", False)


def shift_key(q):
    """The IME toggle chord. Kept SEPARATE from ctrl_key rather than
    parameterised, because the two are not interchangeable here: Ctrl+S below is
    still Ctrl and always will be, and a single mod_key(mod, q) would let a
    future edit to the toggle silently move the save."""
    raw("shift", True)
    raw(q, True)
    raw(q, False)
    raw("shift", False)


json.loads(f.readline()); cmd({"execute": "qmp_capabilities"})

# THE DICTIONARY LINE, checked before anything is typed. If the load failed, the
# rest of this script would still run, still type five letters, and still save a
# file -- containing "nihao " -- which is EXACTLY what the negative control
# expects. Without this check a broken dictionary makes the positive test fail
# in a way that reads like a bad candidate rather than a missing file.
chk((b"[ime] /ime/pinyin.dat" in log or b"[ime] /ime/pinyin-qwen.dat" in log) and b"pinyin keys" in log,
    "the kernel loaded /ime/pinyin.dat at boot")
if fails:
    bail("no dictionary; nothing below would mean anything")

# The dock is CENTRED on its icon count, so a hardcoded x silently clicks a gap
# the day somebody adds an app -- qmp_term.py's comment, and its retry loop.
# TextEdit is index 1 of the root .aex order (Makefile APPS).
def dock_x(i, n):
    isz, gap = 48, 16
    return (1280 - (gap + n * (isz + gap))) // 2 + gap + i * (isz + gap) + isz // 2


launched = False
# Minimal app acceptance disks declare their actual catalog size. Keep the
# observed "launched TextEdit" assertion; a matching pixel alone is not proof.
counts = (int(os.environ['IME_TEST_DOCK_COUNT']),) if 'IME_TEST_DOCK_COUNT' in os.environ else (11, 10, 12, 9, 13, 8)
for count in counts:
    goto(dock_x(1, count), 753); click(); time.sleep(3.0)
    if b"launched TextEdit" in log:
        launched = True
        break
chk(launched, "TextEdit launched from the dock")
if not launched:
    cmd({"execute": "screendump", "arguments": {"filename": shot}})
    bail("no TextEdit")
time.sleep(2.0)

if mode != "ascii":
    mark = len(log)
    if mode == "usability":
        # This harness fixes the guest at 1280x800, 100% UI scale. The live
        # label position is derived from the WM's menu geometry and the font.
        goto(1040, 12); click()
    else:
        shift_key("spc")
    time.sleep(0.8)
    on = b"pinyin ON" in bytes(log[mark:])
    chk(on, "the toggle reached the guest and turned the IME on")
    if not on:
        cmd({"execute": "screendump", "arguments": {"filename": shot}})
        bail("the toggle never arrived -- see the pacing note")

if mode != "usability":
    for q in ("n", "i", "h", "a", "o"):
        key(q)
    time.sleep(0.6)
    cmd({"execute": "screendump", "arguments": {"filename": shot}})   # the bar, mid-composition
    time.sleep(0.4)
    key("spc")                     # commit candidate 1 (or a literal space in ascii mode)
    time.sleep(0.8)
else:
    chk(b"[ime] /ime/pinyin-qwen.dat" in log, "expanded Qwen dictionary is actually loaded")
    if fails: bail("wrong runtime dictionary")
    def spelling(s):
        for q in s: key(q)
    spelling("nihao"); key("comma")
    spelling("woaizhongguo"); key("spc"); key("dot")
    spelling("nihaom"); key("1"); spelling("a"); key("spc"); shift_key("slash")
    spelling("nvhai"); key("spc")
    spelling("shurufa"); key("spc")
    spelling("erweima"); key("spc")
    spelling("xian"); time.sleep(0.8)
    cmd({"execute":"screendump","arguments":{"filename":shot}})
    # The window location comes from the guest; text-row spacing comes from
    # the same hhea/head metrics text_line_height reads, not a guessed pixel.
    # The title is the document name (untitled.txt), not the app name. Limit
    # the match to frames announced AFTER the observed TextEdit launch, so a
    # Finder/Clock frame cannot supply a plausible but wrong click coordinate.
    # The original basename-only title match is corrected in the shared
    # parser: root-path document identity now names this /untitled.txt.
    frames=textedit_frames(bytes(log))
    if not frames: bail("no TextEdit geometry for candidate click")
    wx,wy=map(int,frames[-1])
    font=Path("fsroot/fonts/ui.ttf").read_bytes()
    tables={}
    for i in range(struct.unpack_from(">H",font,4)[0]):
        at=12+16*i; tag,_,off,size=struct.unpack_from(">4sIII",font,at);tables[tag]=font[off:off+size]
    ascent,descent,gap=struct.unpack_from(">hhh",tables[b"hhea"],4)
    units=struct.unpack_from(">H",tables[b"head"],18)[0]
    lh=(ascent-descent+gap)*16//units
    # BAR_PAD=10, BAR_GAP=4, titlebar=30 and anchor gap=6 at 100%.
    goto(wx+20,wy+30+6+10+lh*9+4+lh//2);click()
    time.sleep(0.6)
ctrl_key("s")                  # TextEdit saves to untitled.txt
time.sleep(2.5)
if mode == "usability":
    cmd({"execute":"screendump","arguments":{"filename":shot+".saved.ppm"}})

# Quiesce: the serial console has its own /bin/sh, and poweroff unmounts. LogitFS
# commits per write (see log_commit's barrier comment), so the bytes are on media
# before this -- the clean shutdown is what makes that a property of the test
# instead of a property of the timing. run-core-test.sh makes the same argument.
serial.sendall(b"\ncat /untitled.txt\n")
time.sleep(2.0)
serial.sendall(b"poweroff\n")
for _ in range(200):
    if proc.poll() is not None:
        break
    time.sleep(0.1)
stop = True
if proc.poll() is None:
    try:
        cmd({"execute": "quit"})
    except Exception:
        pass
    try:
        proc.wait(timeout=8)
    except Exception:
        proc.kill()

with open(shot + ".log", "wb") as fh:
    fh.write(bytes(log))
for p in (sock, ser):
    try:
        os.unlink(p)
    except OSError:
        pass

if fails:
    print("FAIL (%d):" % len(fails))
    for m in fails:
        print("   " + m)
    sys.exit(1)
print("typed: mode=%s -- the image is the caller's to check" % mode)
