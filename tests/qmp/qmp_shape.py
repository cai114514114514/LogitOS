#!/usr/bin/env python3
"""On-device proof that Arabic and Hebrew now render with the right glyphs.

Boots the ISO headless, opens /shaping.txt from the Files window (single click
on the row -- the file association sends .txt to TextEdit), and screenshots the
desktop. Then it MEASURES the screenshot rather than only saving it, because "it
rendered" cannot tell shaped Arabic from unshaped Arabic and a picture in a
report is not a test.

The measurement is the one thing shaping visibly changes. Arabic letters join:
a shaped word is a small number of connected ink strokes, because only the
right-joining letters (alef, dal, ra, waw ...) break the connection. The same
word with one isolated glyph per code point is one separate blob per letter,
every letter surrounded by white. So: find the Arabic line, count how many
maximal runs of non-blank pixel COLUMNS it contains, and require it to be far
below the code point count.

For the string used here, u0627 u0644 u0639 u0631 u0628 u064A u0629
("al-arabiyya", 7 code points), the joined form is 2 ink groups -- alef-lam-ain
break after the alef and the rest runs to the end -- and the unshaped form is 7.
The bound below is 4, which no unshaped rendering can reach and which leaves
room for antialiasing to bridge or split a column.

Usage: qmp_shape.py <logit.iso> <disk.img> [out.ppm] [--explore]
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

iso, disk = sys.argv[1], sys.argv[2]
args = [a for a in sys.argv[3:] if not a.startswith("--")]
explore = "--explore" in sys.argv
out = args[0] if args else "build/shape_device.ppm"

fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
fd, serial = tempfile.mkstemp(suffix=".log"); os.close(fd)
qemu = os.environ.get("QEMU", "qemu-system-x86_64")

# Work on a copy of the disk. -snapshot still takes a write lock on the
# original, and three other agents are building in this tree; a harness that
# fails because someone else's QEMU is up is a harness nobody trusts.
fd, diskcopy = tempfile.mkstemp(suffix=".img"); os.close(fd)
with open(disk, "rb") as src, open(diskcopy, "wb") as dst:
    while True:
        chunk = src.read(1 << 20)
        if not chunk:
            break
        dst.write(chunk)

proc = subprocess.Popen([
    qemu, "-cpu", "max", "-cdrom", iso,
    "-drive", f"file={diskcopy},format=raw,if=none,id=hd0",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-snapshot", "-m", "512M",
    "-vga", "none", "-device", "virtio-gpu-pci",
    "-display", "none", "-no-reboot",
    "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server,nowait",
])


def fail(msg):
    print("FAIL:", msg)
    try:
        proc.kill()
    except Exception:
        pass
    sys.exit(1)


def armed():
    try:
        with open(serial, encoding="utf-8", errors="replace") as fh:
            return "LOGIT_BOOT_OK" in fh.read()
    except OSError:
        return False


for _ in range(400):
    if armed():
        break
    if proc.poll() is not None:
        fail("qemu exited during boot")
    time.sleep(0.1)
else:
    fail("LOGIT_BOOT_OK never appeared")
time.sleep(2.0)

s = socket.socket(socket.AF_UNIX)
for _ in range(50):
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
        sx = max(-120, min(120, tx - cur[0]))
        sy = max(-120, min(120, ty - cur[1]))
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
        cur[0] += sx; cur[1] += sy
        time.sleep(0.05)
    time.sleep(0.15)


def click(button="left"):
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": button, "down": d}}]}})
        time.sleep(0.12)


def shot(path):
    cmd({"execute": "screendump", "arguments": {"filename": path}})
    time.sleep(0.6)


json.loads(f.readline()); cmd({"execute": "qmp_capabilities"})

# ---------------------------------------------------------------- the drive --
ROW_Y = int(os.environ.get("SHAPE_ROW_Y", "0"))

if explore:
    shot(out)
    cmd({"execute": "quit"})
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    for p in (sock, serial, diskcopy):
        if os.path.exists(p):
            os.unlink(p)
    print("explore: wrote", out)
    sys.exit(0)

# The Files window auto-launches at cascade 0 and shows LogitOS HD as an icon
# grid; shaping.txt is meant to be the third icon of the first row, (594,215).
#
# THAT COORDINATE IS A GUESS ABOUT DISK CONTENTS, AND IT HAS ALREADY BEEN WRONG.
# fsroot/ holds eight entries; add or remove one and the grid renumbers, so the
# click lands on a different file and the .aex association opens a different
# app. When it happened, this harness measured a screenshot of PREVIEW playing
# sample.aac and reported eight failures, four of them phrased as "isolated
# forms are about half again as wide as joined ones" -- a confident, specific,
# entirely fictional diagnosis of an Arabic shaping regression. Every row
# measured 444 px and 1 ink group, including a four-letter Hebrew word, because
# the measurement was reading the desktop.
#
# So the harness now CHECKS WHAT IT OPENED before it measures anything. The
# kernel already says so on serial -- wm.c:1620 prints "[wm] launched <name>"
# on the success path -- and this file was already reading that serial log for
# the font report. Asserting on it costs nothing and converts a fabricated
# shaping bug into one true sentence naming the app that actually opened.
goto(594, 215)
click()
time.sleep(0.25)
click()
time.sleep(2.5)
shot(out)


def launched():
    try:
        with open(serial, encoding="utf-8", errors="replace") as fh:
            return [ln.split("launched", 1)[1].strip()
                    for ln in fh if "[wm] launched " in ln]
    except OSError:
        return []


apps = launched()
cmd({"execute": "quit"})
if "textedit" not in apps:
    print("apps launched:", ", ".join(apps) if apps else "(none)")
    fail("the click at (594,215) did not open TextEdit -- it opened %s. "
         "The icon grid depends on what is in fsroot/, so this coordinate goes "
         "stale whenever the disk contents change; re-find it with "
         "`python3 tests/qmp/qmp_shape.py <iso> <disk> out.ppm --explore` and "
         "look at out.ppm. NOTHING below this point is a statement about "
         "shaping." % (apps[-1] if apps else "nothing"))
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
# The font loader's own report, because "the Arabic line is blank" has two very
# different causes -- the shaper found nothing to do, or the font never loaded.
try:
    with open(serial, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("[text]"):
                print("serial:", line.rstrip())
except OSError:
    pass

for p in (sock, serial, diskcopy):
    if os.path.exists(p):
        os.unlink(p)
print("wrote", out)

# ----------------------------------------------------------- the assertion --
# A screenshot is not a test. What shaping visibly changes is JOINING: an Arabic
# word drawn with contextual forms is a small number of connected ink strokes,
# because only the right-joining letters (alef, dal, ra, waw, zain) break the
# connection. The same word drawn one isolated glyph per code point is one
# separate blob per letter, each surrounded by white -- and isolated forms are
# also much wider, because an isolated glyph carries both entry and exit serifs.
#
# So: project each text line's ink onto the x axis, count the maximal runs of
# non-blank columns, and require both the run count and the ink width to be far
# below what isolated forms can produce.


def read_ppm(path):
    d = open(path, "rb").read()
    i, fields = 0, []
    while len(fields) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        fields.append(d[i:j]); i = j
    if fields[0] != b"P6":
        fail("screenshot is %r, expected P6" % fields[0])
    return int(fields[1]), int(fields[2]), d[i + 1:]


W, H, PX = read_ppm(out)


def dark(x, y):
    o = (y * W + x) * 3
    return (PX[o] + PX[o + 1] + PX[o + 2]) / 3 < 140


# The TextEdit window opens third (Clock and Files auto-launch), so its content
# column is deterministic for a given boot. TOP and PITCH were read off the
# rendered document and are checked below against the document's blank lines --
# if the layout ever moves, the blank-line check fails loudly instead of
# silently measuring the wrong rows.
X0, X1 = 176, 620
TOP, PITCH = 169, 16


def line_ink(k):
    a = TOP + PITCH * k
    cols = [any(dark(x, y) for y in range(a, a + PITCH)) for x in range(X0, X1)]
    groups, run, npx = 0, 0, 0
    for c in cols:
        if c and run == 0:
            groups += 1
        run = run + 1 if c else 0
    xs = [i for i, c in enumerate(cols) if c]
    for y in range(a, a + PITCH):
        for x in range(X0, X1):
            if dark(x, y):
                npx += 1
    return groups, (xs[-1] - xs[0] + 1 if xs else 0), npx


bad = []

# 1. Calibration: fsroot/shaping.txt has blank lines at 1, 5, 11 and 14.
for k in (1, 5, 11, 14):
    _g, _w, npx = line_ink(k)
    if npx > 24:
        bad.append("line %d should be blank, has %d dark pixels -- the "
                   "TOP/PITCH calibration no longer matches the layout" % (k, npx))

# 2. The Arabic lines. (code points, max ink groups, max ink width in px)
#    The bounds are roughly twice what shaped text produces and roughly half
#    what isolated forms need, so neither antialiasing nor a font substitution
#    can flip the answer by accident.
ARABIC = {
    7:  ("al-arabiyya",   7, 4, 50),
    8:  ("marhaba",      13, 6, 95),
    9:  ("as-salam",     12, 6, 100),
    10: ("kataba+harakat", 6, 3, 55),
}
for k, (name, ncp, maxg, maxw) in ARABIC.items():
    g, wid, npx = line_ink(k)
    print("  arabic %-16s %2d code points -> %d ink groups, %d px wide"
          % (name, ncp, g, wid))
    if npx == 0:
        bad.append("%s: nothing was drawn" % name)
    elif g > maxg:
        bad.append("%s: %d ink groups (isolated forms would give about %d); "
                   "the letters are not joining" % (name, g, ncp))
    elif wid > maxw:
        bad.append("%s: %d px wide, over the %d px bound -- isolated forms are "
                   "about half again as wide as joined ones" % (name, wid, maxw))

# 3. Hebrew: no joining to check, but it must render at all.
g, wid, npx = line_ink(13)
print("  hebrew  %-16s  4 letters+space -> %d ink groups, %d px wide"
      % ("shalom olam", g, wid))
if npx == 0:
    bad.append("the Hebrew line drew nothing")

for m in bad:
    print("  FAIL:", m)
print("PASS: Arabic joins on the device" if not bad else "FAIL: %d problems" % len(bad))
sys.exit(1 if bad else 0)
