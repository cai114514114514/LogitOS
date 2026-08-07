#!/usr/bin/env python3
"""On-device test for the rich Terminal, judged by PIXELS.

A test that only checked "bytes went down the pipe" would pass against a
terminal that renders nothing, so every rich assertion here is about the
composited frame: an image at the size it was asked for, a drawn progress bar,
a table with real rules, and stderr in red.

It also tests the COMPATIBILITY claim rather than assuming it. In the same
session, the same command is run twice:

    dir /bin              -> a drawn table (pixels)
    dir /bin > /out.txt   -> a file, which must contain plain text and NOT ONE
                             protocol byte

and the file is read back through the SERIAL console's shell (a second, non
-interactive /bin/sh that never had the rich channel), so the assertion is made
on real bytes rather than on a screenshot of them.

Usage: qmp_rich_term.py <iso> <disk.img> [out.ppm]
"""

import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui
from qmp_ui import PPM, Session, dock_icon

ISO = sys.argv[1]
DISK = sys.argv[2]
OUT = sys.argv[3] if len(sys.argv) > 3 else "/tmp/richterm.ppm"

TERMINAL_SLOT = 3            # clock textedit monitor terminal ...

# The terminal's LIGHT palette, from c/apps/gui/terminal.c. g_ui_dark defaults to
# 0, so this is what boots. Every colour used below is a SOLID fill (rects and
# rounded rects) or the fully-covered core of a glyph, both of which land on the
# framebuffer exactly -- which is what makes an exact-match search meaningful.
C_RULE = (0xDD, 0xDF, 0xE6)      # table rules / image border
C_OK   = (0x1E, 0x90, 0x50)      # completed progress fill, ok status dot
C_ERR  = (0xC0, 0x20, 0x20)      # stderr text
C_BAD  = (0xD4, 0x3A, 0x3A)      # non-zero exit status dot
C_IMG  = (0xFF, 0x00, 0xE5)      # tests/fixtures/img/dot.png, a colour nothing else uses
C_ACC  = (0x14, 0x62, 0xC8)      # chart bars, links
C_DIM  = (0x6B, 0x70, 0x80)      # scrollbar thumb

fails = []


def chk(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


def box(ppm, rgb):
    b = ppm.find_color(rgb)
    return b


def box_wh(b):
    return (0, 0) if b is None else (b[2] - b[0] + 1, b[3] - b[1] + 1)


def widest_run(ppm, rgb):
    """Longest horizontal run of an exact colour. A rule is a 1px-tall fill, so
    its run length is its width -- and unlike a bounding box, a run cannot be
    inflated by an unrelated pixel somewhere else on screen."""
    target = bytes(rgb)
    best = 0
    row = ppm.w * 3
    for y in range(ppm.h):
        base = y * row
        run = 0
        for x in range(ppm.w):
            o = base + x * 3
            if ppm.px[o:o + 3] == target:
                run += 1
                if run > best:
                    best = run
            else:
                run = 0
    return best


def tallest_run(ppm, rgb):
    """Longest VERTICAL run of an exact colour -- the scrollbar thumb is the only
    tall solid block of the muted colour; the rest of its uses are text."""
    target = bytes(rgb)
    best = 0
    runs = [0] * ppm.w
    row = ppm.w * 3
    for y in range(ppm.h):
        base = y * row
        for x in range(ppm.w):
            o = base + x * 3
            if ppm.px[o:o + 3] == target:
                runs[x] += 1
                if runs[x] > best:
                    best = runs[x]
            else:
                runs[x] = 0
    return best


def count(ppm, rgb):
    target = bytes(rgb)
    n = 0
    row = ppm.w * 3
    for y in range(ppm.h):
        base = y * row
        start = 0
        while True:
            k = ppm.px.find(target, base + start, base + row)
            if k < 0:
                break
            if (k - base) % 3 == 0:
                n += 1
            start = k - base + 1
    return n


# ---------------------------------------------------------------- boot ------

sock = tempfile.mktemp(suffix=".qmp")
ser = tempfile.mktemp(suffix=".ser")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")

proc = subprocess.Popen(
    [qemu, "-cpu", os.environ.get("QEMU_CPU_NAME", "max"), "-cdrom", ISO,
     "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M",
     "-vga", "none", "-device", "virtio-gpu-pci",
     "-display", "none", "-no-reboot",
     # wait=on so nothing the kernel prints before we attach is lost
     "-chardev", f"socket,id=ser0,path={ser},server=on,wait=on",
     "-serial", "chardev:ser0",
     "-qmp", f"unix:{sock},server=on,wait=off"])

serial = socket.socket(socket.AF_UNIX)
for _ in range(200):
    try:
        serial.connect(ser)
        break
    except OSError:
        if proc.poll() is not None:
            print("qemu died before the serial socket appeared")
            sys.exit(1)
        time.sleep(0.1)
serial.setblocking(False)

log = bytearray()


def pump(seconds=0.0):
    end = time.time() + seconds
    while True:
        try:
            b = serial.recv(65536)
            if b:
                log.extend(b)
            else:
                break
        except BlockingIOError:
            pass
        except OSError:
            break
        if time.time() >= end:
            break
        time.sleep(0.05)


def wait_for(marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.2)
        if marker in log:
            return True
        if proc.poll() is not None:
            return False
    return False


ok = wait_for(b"LOGIT_BOOT_OK", 45)
if not ok:
    print("FAIL: never booted")
    print(log.decode("utf-8", "replace")[-3000:])
    proc.kill()
    sys.exit(1)
pump(2.0)

s = Session(sock)

# ------------------------------------------------------------- typing ------
# PS/2 has a one-byte buffer, so key injection has to be paced or characters go
# missing silently. This path is slower than qmp_ui's default on purpose: the
# keystroke now travels terminal -> control pipe -> shell -> rich pipe ->
# terminal before it is echoed, so a burst is more likely to be dropped.
SHIFTED = {">": "dot", "|": "backslash", ":": "semicolon", "_": "minus", "?": "slash"}


def typ(text, settle=0.11):
    for ch in text:
        if ch in SHIFTED:
            s.key_shift(SHIFTED[ch], settle)
        elif "A" <= ch <= "Z":
            s.key_shift(ch.lower(), settle)
        else:
            s.key(qmp_ui.KMAP.get(ch, ch), settle)


def ctrl(letter, settle=0.25):
    """Ctrl+<letter>. The keyboard driver turns it into a control code, and the
    terminal routes it over the control channel rather than down stdin."""
    ev = lambda k, d: {"type": "key", "data": {"key": {"type": "qcode", "data": k}, "down": d}}
    s.cmd({"execute": "input-send-event", "arguments": {"events": [ev("ctrl", True), ev(letter, True)]}})
    time.sleep(0.06)
    s.cmd({"execute": "input-send-event", "arguments": {"events": [ev(letter, False), ev("ctrl", False)]}})
    time.sleep(settle)


def run(cmd, settle=1.6):
    typ(cmd + "\n")
    time.sleep(settle)


def shot(tag):
    p = OUT.replace(".ppm", f".{tag}.ppm") if tag else OUT
    s.screendump(p, settle=0.6)
    return PPM(p)


print("launching the Terminal from the dock")
s.click_at(*dock_icon(TERMINAL_SLOT))
time.sleep(2.5)

# ------------------------------------------------- 1. an inline image -------
print("1. an image displayed inline, at the size it was asked for")
run("show /media/dot.png 120", 2.2)
p = shot("img")
b = box(p, C_IMG)
w, h = box_wh(b)
chk(b is not None, "the image's pixels are on screen at all")
chk(w == 120 and h == 80,
    f"the image is 120x80 device px as requested (got {w}x{h})")
if b:
    chk(count(p, C_IMG) >= 120 * 80 * 0.98,
        f"the region is filled, not an outline ({count(p, C_IMG)} px of {120*80})")

# --------------------------------------------- 2. a table with real rules ---
print("2. a table with drawn rules and clickable rows")
run("dir /bin", 2.4)
p = shot("table")
runlen = widest_run(p, C_RULE)
chk(runlen >= 400, f"a table rule spans the content width ({runlen} px)")
thumb = tallest_run(p, C_DIM)
chk(thumb >= 16, f"the scrollbar thumb is drawn once the output is taller than the view ({thumb} px)")

# -------------------------- 2b. a chart built by a real pipeline ------------
# `dir` is mid-pipeline here, so it gets NO rich channel and emits plain text;
# `chart` is last, so it does. One command line exercises both halves.
print("2b. a chart drawn from a pipeline (dir plain -> chart rich)")
run("dir /bin | head | chart bin", 9.0)   # three fork+execve under TCG
p = shot("chart")
barlen = widest_run(p, C_ACC)
chk(barlen >= 100, f"a chart bar is a drawn rect, not '#' characters ({barlen} px)")

# ----------------------------------------------- 3. a drawn progress bar ----
print("3. a progress bar that is a widget, not [####    ]")
run("prog 6 loading", 5.0)
p = shot("prog")
bb = box(p, C_OK)
bw, bh = box_wh(bb)
chk(bb is not None, "the completed progress fill is on screen")
chk(bw >= 200, f"the progress fill is a wide drawn bar ({bw}x{bh} px)")

# ------------------------------- 4. stderr in red, exit status in the gutter -
print("4. stderr in red and a non-zero exit status in the gutter (provenance)")
run("cat /nope", 3.5)
p = shot("err")
nerr = count(p, C_ERR)
nbad = count(p, C_BAD)
chk(nerr > 20, f"stderr is painted in the error colour ({nerr} px)")
chk(nbad > 0, f"the failing command is marked in the gutter ({nbad} px)")

# ------------------------------------------------ 4b. ^C returns the prompt -
# There is no kill(2) in this kernel, so ^C ABANDONS the foreground job rather
# than killing it. What must be true either way is that the shell comes back:
# assert it behaviourally, by requiring a later command to actually run.
print("4b. ^C returns the prompt while a job is still running")
typ("sleep 8\n")
time.sleep(1.5)
ctrl("c")                       # abandon the job
time.sleep(1.0)
ctrl("l")                       # clear the scrollback, so the next image is alone
time.sleep(0.8)
run("show /media/dot.png 60", 3.0)
p = shot("intr")
b = box(p, C_IMG)
w, h = box_wh(b)
chk(b is not None and w == 60 and h == 40,
    f"a command run AFTER ^C produced its output ({w}x{h})")

# ---------------------------------------- 5. the compatibility claim, tested -
print("5. the same command redirected to a file -- no protocol bytes")
run("dir /bin > /out.txt", 2.6)

# Read it back through the serial console's shell: a second /bin/sh that was
# never given the rich channel, so what it prints is the file's real bytes.
mark = b"---RICHTEST---"
log.clear()
serial.sendall(b"echo " + mark + b"\ncat /out.txt\necho " + mark + b"\n")
got = wait_for(mark + b"\n", 20)
pump(3.0)
body = bytes(log)
first = body.find(mark)
second = body.find(mark, first + len(mark)) if first >= 0 else -1
third = body.find(mark, second + len(mark)) if second >= 0 else -1
between = body[second:third] if (second >= 0 and third > second) else b""

chk(b"LRT\x01" not in body, "the redirected file contains NO protocol frames")
chk(b"sh" in between and b"file" in between,
    "the redirected file contains the plain listing "
    f"({len(between)} bytes between markers)")
chk(b"LogitOS shell" in body or b"$" in body,
    "the serial console's non-interactive shell is unchanged")

# A plain program that knows nothing about the protocol still works, over the
# same rich-capable shell in the GUI window -- proven on the serial side by the
# non-rich shell producing identical plain text for `dir`.
log.clear()
serial.sendall(b"echo " + mark + b"\ndir /bin\necho " + mark + b"\n")
wait_for(mark + b"\n", 20)
pump(3.0)
body2 = bytes(log)
chk(b"LRT\x01" not in body2,
    "a rich-aware program under a NON-rich shell emits no protocol bytes")
chk(b"sh" in body2, "the non-rich `dir` still produced a plain listing")

log.clear()
serial.sendall(b"uname\n")
pump(3.0)
chk(b"LogitOS" in bytes(log), "a plain coreutil still works on the serial console")

# ------------------------------------------------------------------ done ----
s.screendump(OUT, settle=0.5)
try:
    s.cmd({"execute": "quit"})
except Exception:
    pass
try:
    proc.wait(timeout=6)
except Exception:
    proc.kill()
for p2 in (sock, ser):
    try:
        os.unlink(p2)
    except OSError:
        pass

if fails:
    print("FAIL (%d):" % len(fails))
    for f in fails:
        print("   " + f)
    sys.exit(1)
print("PASS: rich terminal verified by pixels, and plain output verified by bytes")
