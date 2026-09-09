#!/usr/bin/env python3
"""Does Chinese render in the GUI Terminal, and does it keep the column grid?

TWO SOURCES OF TRUTH DISAGREE, WHICH IS WHY THIS EXISTS.

  CLAUDE.md says of the shipped fonts: "mono.ttf (Noto Sans Mono: printable
  ASCII plus NBSP -- no CJK at all, so Han typed into the Terminal has no
  glyph)". That half is verifiable off the disk: fsroot/fonts/mono.ttf's
  format-4 cmap covers 96 code points and not one of them is Han.

  c/kernel/gui/text.c:16 says the loaded fonts are "in fallback order after
  whichever one was asked for", and tl_fonts() puts F_UI -- a Noto Sans SC
  subset with 6,763 Han glyphs -- second in the list a mono request walks.

Only one of those can be describing the screen. THE ONLY WAY TO SETTLE IT IS
THE SCANOUT, so this driver types Han into the Terminal over the real input
path and screendumps the result.

WHY THE IME AND NOT A FILE OF CJK BYTES. There is no Han anywhere in fsroot,
and adding some would answer a different question: `cat` proves the FONT has
the glyph, which zh.wikipedia has proved in the browser since M14. The claim
under dispute is about the TERMINAL's monospace path -- text_draw_mono_sz ->
layout(LOGIT_FACE_MONO) -> shape_line's cell>0 branch -- reached through the
keyboard, which is how a person meets it.

WHAT THIS HARNESS STRUCTURALLY CANNOT SEE, copied from tests/boot/ime_type.py
because it is exactly as true here: QMP injects scancodes BENEATH the host
keyboard, so a green run means the guest works and has never meant that a
person can type this on a Mac. That is why the product of this run is a PPM
and not a verdict -- the pixels are the evidence, the exit code is only about
whether the machine got far enough to produce them.

THE LINE IT TYPES IS MIXED ON PURPOSE:  ab<Han><Han>cd

A screenshot of Han alone answers "is there a glyph" and nothing else. A cell
grid can be broken in two directions that a pure-CJK line hides completely:
a Han drawn one cell wide overlaps its neighbour, and a Han drawn one cell wide
that ADVANCES two leaves a gap. Both are only visible against ASCII that must
still land on the grid after it -- so "cd" is the measurement and the Han is
the input.

Usage: qmp_cjk_term.py <iso> <disk.img> <out-prefix>
"""

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui
from qmp_ui import PPM, Session

ISO, DISK, PREFIX = sys.argv[1], sys.argv[2], sys.argv[3]
# TERMINAL_SLOT = 3 used to live here; the tile now comes from the guest's own
# [wm] dock line -- see the launch below.

fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
ser = tempfile.mktemp(suffix=".ser")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen([
    qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
    "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-snapshot",                  # nothing here has to reach the host but a PPM
    "-m", "512M", "-vga", "none", "-device", "virtio-gpu-pci",
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
serlog = PREFIX + ".serial.txt"
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
    open(serlog, "wb").write(bytes(log))
    proc.kill()
    sys.exit(1)


deadline = time.time() + 240
while b"LOGIT_BOOT_OK" not in log and time.time() < deadline:
    if proc.poll() is not None:
        bail("qemu died before boot")
    time.sleep(0.1)
if b"LOGIT_BOOT_OK" not in log:
    bail("never booted")
time.sleep(2.0)

s = Session(sock, serial=serlog,
            serial_text_fn=lambda: bytes(log).decode("utf-8", "replace"))

# The dictionary line first: without it the IME types nothing and the Terminal
# would show `nihao `, which is the ASCII control's expected output, not this
# test's failure. ime_type.py makes the same check for the same reason.
chk(b"[ime] /ime/pinyin.dat" in log and b"pinyin keys" in log,
    "the kernel loaded /ime/pinyin.dat at boot")
chk(b"/fonts/ui.ttf" in log and b"/fonts/mono.ttf" in log,
    "both fonts loaded")
for line in bytes(log).decode("utf-8", "replace").splitlines():
    if "[text] /fonts/" in line:
        print("       " + line.strip())

# The old loop accepted b"terminal" in log.lower() as its oracle -- and the
# [wm] dock line printed at BOOT contains terminal.aex, so the oracle was
# satisfied before the click happened and the check could not fail. It also
# re-guessed the app count per retry (11, 10, 12, ...). launch_app marks the
# log, clicks the tile the guest names for terminal.aex, and requires a
# launched line naming Terminal AFTER the mark.
try:
    s.launch_app("terminal")
    launched = True
except AssertionError as e:
    print("       " + str(e))
    launched = False
chk(launched, "Terminal launched from the dock")
time.sleep(3.0)
s.screendump(PREFIX + "-0-open.ppm")

# --- the line: ab <IME nihao=你好> cd -------------------------------------
s.key("a"); s.key("b")
time.sleep(0.3)

mark = len(log)
s.key_mods(["shift"], "spc")
time.sleep(1.0)
on = b"pinyin ON" in bytes(log[mark:])
chk(on, "Shift+Space reached the Terminal window and turned the IME on")
if not on:
    s.screendump(PREFIX + "-fail.ppm")
    bail("the toggle never arrived -- see the pacing note in ime_type.py")

for q in ("n", "i", "h", "a", "o"):
    s.key(q, settle=0.15)
time.sleep(0.8)
s.screendump(PREFIX + "-1-composing.ppm")     # the candidate bar, mid-composition
s.key("spc")                                  # commit candidate 1 -> 你好
time.sleep(0.8)

mark = len(log)
s.key_mods(["shift"], "spc")                  # back to ASCII
time.sleep(0.8)
chk(b"pinyin OFF" in bytes(log[mark:]), "Shift+Space turned the IME back off")
s.key("c"); s.key("d")
time.sleep(1.2)
s.screendump(PREFIX + "-2-line.ppm")          # ab你好cd on the shell's input line

# And the same string through the OUTPUT path rather than the input line: the
# shell will not find a command called ab你好cd and says so on fd 2, which the
# Terminal draws in red as a scrollback line. The input line and the scrollback
# are two different draw calls in terminal.c and only one of them is exercised
# above.
s.key("ret")
time.sleep(2.5)
s.screendump(PREFIX + "-3-output.ppm")

open(serlog, "wb").write(bytes(log))
stop = True
try:
    s.cmd({"execute": "quit"})
except Exception:
    pass
try:
    proc.wait(timeout=10)
except Exception:
    proc.kill()

print("shots: " + PREFIX + "-{0-open,1-composing,2-line,3-output}.ppm")
sys.exit(1 if fails else 0)
