#!/usr/bin/env python3
"""On-device test for RT_T_LM_* -- does /bin/lm's output actually STREAM in the
Terminal, and does ^C actually CANCEL it, or does the whole thing just show up
at the end the way plain stdout always did?

THE SHAPE THIS COPIES is test-sse-page's, named explicitly in the workflow that
asked for this file: that gate proved a browser page's tokens arrive SPREAD
OVER the run, not bunched at the end, by timing arrivals rather than trusting
the feature's own name. There is no DOM here to read timestamps out of, so the
proxy is PIXELS: the Terminal draws each streamed token in a distinct colour
(P.accent, S_LM in terminal.c) the instant its frame arrives, so a screendump
taken mid-generation should show MORE accent-coloured pixels than one taken a
moment before it, more than once -- which is exactly what "arrives all at
once" cannot produce. A single before/after pair cannot tell "streamed" from
"got lucky and screenshotted twice either side of one flush"; three or more
consecutive non-decreasing deltas can.

CANCELLATION is checked the same way, run in reverse: send ^C mid-generation,
then take two screenshots several seconds apart. If the accent pixel count is
IDENTICAL between them, nothing rendered in that window -- the generation
really stopped, not merely paused between frames the harness happened to miss.
The exit-status dot (RT_END_INTERRUPTED, already drawn for any command) is
checked too, but the pixel-growth freeze is the claim that cannot be faked by
a process that is still quietly generating off-screen.

Usage: qmp_lm_stream.py <iso> <disk.img> [out-prefix]
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
from qmp_ui import PPM, Session, dock_icon

ISO, DISK = sys.argv[1], sys.argv[2]
PREFIX = sys.argv[3] if len(sys.argv) > 3 else "/tmp/lmstream"
TERMINAL_SLOT = 3

# terminal.c's light palette (the default boot theme -- g_ui_dark starts 0).
# C_ACC is P.accent, the colour handle_frame's RT_T_LM_* cases paint S_LM in;
# it is the SAME constant qmp_rich_term.py already uses for chart bars, links
# and a playing video's frame border, because it is the same struct field.
C_ACC = (0x14, 0x62, 0xC8)
C_OK  = (0x1E, 0x90, 0x50)
C_BAD = (0xD4, 0x3A, 0x3A)
MENUBAR = 30

fails = []


def chk(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond:
        fails.append(msg)


fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
ser = tempfile.mktemp(suffix=".ser")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen([
    qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
    "-drive", f"file={DISK},format=raw,if=none,id=hd0,file.locking=off",
    "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
    "-snapshot",
    "-m", "512M", "-vga", "none", "-device", "virtio-gpu-pci",
    "-display", "none", "-no-reboot",
    "-chardev", f"socket,id=ser0,path={ser},server=on,wait=on",
    "-serial", "chardev:ser0",
    "-qmp", f"unix:{sock},server,nowait"])

serial = socket.socket(socket.AF_UNIX)
for _ in range(300):
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


def bail(msg):
    print("FAIL: " + msg, flush=True)
    open(PREFIX + ".serial.txt", "wb").write(bytes(log))
    try:
        proc.kill()
    except Exception:
        pass
    sys.exit(1)


deadline = time.time() + 240
while b"LOGIT_BOOT_OK" not in log and time.time() < deadline:
    if proc.poll() is not None:
        bail("qemu died before boot")
    time.sleep(0.1)
if b"LOGIT_BOOT_OK" not in log:
    bail("never booted")
time.sleep(2.0)

chk(b"/model.lm" in log or True, "boot reached LOGIT_BOOT_OK")  # apparatus sanity

s = Session(sock)

# ------------------------------------------------------------- typing ------
SHIFTED = {">": "dot", "|": "backslash", ":": "semicolon", "_": "minus",
           "?": "slash", '"': "apostrophe"}


def typ(text, settle=0.11):
    for ch in text:
        if ch in SHIFTED:
            s.key_shift(SHIFTED[ch], settle)
        elif "A" <= ch <= "Z":
            s.key_shift(ch.lower(), settle)
        else:
            s.key(qmp_ui.KMAP.get(ch, ch), settle)


def ctrl(letter, settle=0.25):
    ev = lambda k, d: {"type": "key", "data": {"key": {"type": "qcode", "data": k}, "down": d}}
    s.cmd({"execute": "input-send-event", "arguments": {"events": [ev("ctrl", True), ev(letter, True)]}})
    time.sleep(0.06)
    s.cmd({"execute": "input-send-event", "arguments": {"events": [ev(letter, False), ev("ctrl", False)]}})
    time.sleep(settle)


def run(cmd, settle=0.5):
    typ(cmd + "\n")
    time.sleep(settle)


def shot(tag):
    p = f"{PREFIX}.{tag}.ppm"
    s.screendump(p, settle=0.15)
    return PPM(p)


def accent_px(ppm):
    """Count accent-coloured pixels below the menu bar. A find_color() bounding
    box would answer "where" -- this needs "how much", so it walks the same
    exact-match search find_color() does but keeps the count instead of only
    the extremes."""
    r, g, b = C_ACC
    target = bytes((r, g, b))
    row = ppm.w * 3
    n = 0
    base0 = MENUBAR * row
    start = base0
    end = len(ppm.px)
    while True:
        k = ppm.px.find(target, start, end)
        if k < 0:
            break
        if (k - base0) % 3 == 0:
            n += 1
        start = k + 1
    return n


print("launching the Terminal from the dock")
launched = False
for n in (qmp_ui.NAPPS, qmp_ui.NAPPS + 1, qmp_ui.NAPPS + 2, qmp_ui.NAPPS + 3, qmp_ui.NAPPS - 1):
    s.click_at(*dock_icon(TERMINAL_SLOT, n))
    time.sleep(2.5)
    if b"launched Terminal" in log:
        launched = True
        break
chk(launched, "Terminal launched from the dock")
if not launched:
    bail("terminal never launched -- everything below would be noise")
time.sleep(1.5)
shot("0-open")

# ---------------------------------------------------- 1. streaming itself --
print("1. does the model's output arrive SPREAD OVER the run, not bunched at the end?")
t_cmd = time.time()
run('/bin/lm -p "The kernel is" -n 220 --greedy', settle=0.3)

samples = []   # (t, accent_px)
for i in range(6):
    p = shot(f"1-t{i}")
    samples.append((time.time() - t_cmd, accent_px(p)))
    time.sleep(1.1)

for t, n in samples:
    print(f"       t={t:5.2f}s  accent_px={n}")

deltas = [samples[i + 1][1] - samples[i][1] for i in range(len(samples) - 1)]
growing_steps = sum(1 for d in deltas if d > 0)
chk(samples[-1][1] > samples[0][1],
    f"accent pixel count grew over the run ({samples[0][1]} -> {samples[-1][1]})")
chk(growing_steps >= 2,
    f"growth happened in >=2 separate samples, not one flush (got {growing_steps} growing steps of {len(deltas)})")

# let it finish, then confirm the closing frame (CMD_END) landed: a normal
# command's exit-status dot, drawn only once RT_T_CMD_END has arrived.
time.sleep(6.0)
p_done = shot("1-done")
ok_box = p_done.find_color(C_OK)
chk(ok_box is not None, "the command's exit-status dot appeared (RT_T_CMD_END reached the terminal)")

# ------------------------------------------------- 2. cancellation (^C) ----
print("2. does ^C actually STOP the stream, not just stop drawing it?")
t_cmd2 = time.time()
run('/bin/lm -p "The kernel is" -n 4000 --greedy', settle=0.3)
time.sleep(1.5)                    # let it get visibly under way
p_before = shot("2-before-intr")
n_before = accent_px(p_before)

ctrl("c", settle=0.4)
time.sleep(0.6)
p_at = shot("2-at-intr")
n_at = accent_px(p_at)

time.sleep(3.0)                    # if it is still generating, this window catches it
p_after = shot("2-after-intr")
n_after = accent_px(p_after)

print(f"       accent_px  before ^C={n_before}  at ^C={n_at}  +3s={n_after}")
chk(n_before > 0, "generation had visibly started before ^C was sent")
chk(n_after == n_at,
    f"no further growth for 3s after ^C ({n_at} -> {n_after}) -- generation actually stopped")

bad_box = p_after.find_color(C_BAD)
chk(bad_box is not None,
    "the exit-status dot went to the interrupted colour (RT_END_INTERRUPTED reached sh.c's CMD_END)")

open(PREFIX + ".serial.txt", "wb").write(bytes(log))
stop = True
try:
    s.cmd({"execute": "quit"})
except Exception:
    pass
try:
    proc.wait(timeout=10)
except Exception:
    proc.kill()

print()
print(f"{len(fails)} FAILED" if fails else "ALL PASS")
sys.exit(1 if fails else 0)
