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
import threading
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
C_ACC  = (0x14, 0x62, 0xC8)      # chart bars, links, a playing video's frame
C_DIM  = (0x6B, 0x70, 0x80)      # scrollbar thumb
C_FG   = (0x22, 0x24, 0x2A)      # ordinary text -- the fully-covered glyph core

MENUBAR = 30                     # rows above this carry the clock, which ticks

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
log = bytearray()
_reader_stop = False


def _reader():
    """Drain the serial socket CONTINUOUSLY, on its own thread.

    Not a tidiness point: the chardev socket has a finite buffer, and when the
    host stops reading it QEMU blocks inside the write -- which stalls the GUEST
    in serial_puts and looks exactly like the OS hanging. The old poll-when-we
    -feel-like-it pump made that a coin flip, and it got worse the moment the
    terminal started reporting its own frame timings on fd 2."""
    while not _reader_stop:
        try:
            b = serial.recv(65536)
            if not b:
                break
            log.extend(b)
        except OSError:
            time.sleep(0.05)


threading.Thread(target=_reader, daemon=True).start()


def pump(seconds=0.0):
    if seconds:
        time.sleep(seconds)


def wait_for(marker, timeout):
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.2)
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


def launch_terminal():
    """Click the Terminal's dock icon, and CHECK that it launched.

    The dock is centred on the icon count, so every icon moves the day somebody
    adds an app -- which is exactly what happened: a tenth icon shifted the
    whole row 32 px left, the click landed in a gap, and nineteen pixel
    assertions failed one after another saying nothing about the terminal. The
    guest announces `[wm] launched Terminal` on the serial console, so the
    launch is verifiable rather than assumed; if the first geometry misses, try
    the neighbouring icon counts."""
    for n in (qmp_ui.NAPPS, qmp_ui.NAPPS + 1, qmp_ui.NAPPS + 2,
              qmp_ui.NAPPS + 3, qmp_ui.NAPPS - 1):
        s.click_at(*dock_icon(TERMINAL_SLOT, n))
        time.sleep(2.5)
        if b"launched Terminal" in log:
            if n != qmp_ui.NAPPS:
                print(f"  (the dock holds {n} icons, not qmp_ui.NAPPS={qmp_ui.NAPPS})")
            return True
    return False


if not launch_terminal():
    print("FAIL: the Terminal never launched -- everything below would be noise")
    s.screendump(OUT.replace(".ppm", ".nolaunch.ppm"), settle=0.5)
    try:
        s.cmd({"execute": "quit"})
    except Exception:
        pass
    proc.kill()
    sys.exit(1)
time.sleep(1.5)

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

# ------------------------------------------------ 5. video, judged by MOTION -
# "A frame appeared" is not video. What has to be true is that successive
# frames DIFFER, in the region the video owns, while everything else on the
# screen stays exactly as it was -- which is also the whole claim of the video
# frame type: it updates its own rectangle in place instead of repainting the
# scrollback thirty times a second.
print("5. a video that actually moves, inside the region it owns")
ctrl("l")
time.sleep(0.8)
run("show /media/sample.h264", 4.0)

vid = []
for i in range(3):
    vid.append(shot(f"vid{i}"))
    time.sleep(1.0)

border = box(vid[1], C_ACC)          # the video's 1px accent frame
bw, bh = box_wh(border)
chk(border is not None and bw > 100 and bh > 80,
    f"the video is on screen with its own border ({bw}x{bh})")
chk(bw == 322 and bh == 242,
    f"and it is the stream's natural 320x240 plus the 1px frame (got {bw}x{bh})")


def changed(a, b, region=None, skip_top=MENUBAR):
    """Pixels differing between two dumps, optionally inside a region and
    always ignoring the menu bar, whose clock ticks on its own."""
    n = 0
    row = a.w * 3
    x0, y0, x1, y1 = region if region else (0, skip_top, a.w - 1, a.h - 1)
    if y0 < skip_top:
        y0 = skip_top
    for y in range(y0, y1 + 1):
        base = y * row
        if a.px[base + x0 * 3:base + (x1 + 1) * 3] == b.px[base + x0 * 3:base + (x1 + 1) * 3]:
            continue
        for x in range(x0, x1 + 1):
            o = base + x * 3
            if a.px[o:o + 3] != b.px[o:o + 3]:
                n += 1
    return n


if border:
    inner = (border[0] + 1, border[1] + 1, border[2] - 1, border[3] - 1)
    m1 = changed(vid[0], vid[1], inner)
    m2 = changed(vid[1], vid[2], inner)
    chk(m1 > 3000 and m2 > 3000,
        f"successive frames differ inside the video ({m1}, {m2} px)")
    # Everything else must be still. This is the in-place claim: if the video
    # were an image frame per picture the scrollback would move under it.
    outside = changed(vid[1], vid[2]) - m2
    chk(outside < 200,
        f"and the rest of the window is not repainted ({outside} px changed outside)")

# The control for the motion detector: a STILL image, measured exactly the same
# way, must not move. Without this, "the pixels changed" could be the clock, a
# cursor, or dither.
print("5b. control -- a still image, measured the same way, does not move")
ctrl("l")
time.sleep(0.8)
run("show /media/dot.png 120", 3.0)
st1 = shot("still1")
time.sleep(1.2)
st2 = shot("still2")
chk(changed(st1, st2) == 0,
    f"a still image is identical across a second ({changed(st1, st2)} px changed)")

perf = [l for l in log.decode("utf-8", "replace").splitlines() if "TERMPERF" in l]
rates = [l for l in perf if "video_rate" in l]
opens = [l for l in perf if "video_open" in l]
chk(len(opens) >= 1, "the terminal reported opening the stream")
chk(len(rates) >= 1, "and reported a frame rate")
if rates:
    last = rates[-1].split()
    kv = dict(p.split("=") for p in last if "=" in p)
    fr, el = int(kv.get("frames", 0)), int(kv.get("elapsed_ms", 1))
    fps = fr * 1000.0 / max(el, 1)
    print(f"     measured {fr} frames in {el} ms = {fps:.1f} fps")
    chk(fr >= 16 and fps > 5,
        f"playback is a stream, not a slideshow ({fps:.1f} fps over {fr} frames)")
for l in perf[:6] + perf[-4:]:
    print("     " + l.strip())

# The second codec, on the same path. H.265 is a different decoder with a
# different NAL header, and `show` picks between them by parsing that header
# rather than by looking at the file name -- so this is the assertion that the
# choice is made from the bytes.
print("5c. the H.265 stream, through the same frame type")
ctrl("l")
time.sleep(0.8)
run("show /media/sample.h265", 6.0)
h5 = [l for l in log.decode("utf-8", "replace").splitlines() if "video_open" in l]
chk(len(h5) > len(opens), "the H.265 stream opened too")
if len(h5) > len(opens):
    kv = dict(p.split("=") for p in h5[-1].split() if "=" in p)
    print(f"     H.265 first frame {kv.get('w')}x{kv.get('h')} in {kv.get('first_frame_ms')} ms")
    chk(kv.get("w") == "176" and kv.get("h") == "144",
        f"at the fixture's real geometry (got {kv.get('w')}x{kv.get('h')})")

# ----------------------------------- 6. a binary never reaches the grid ------
# The report that started this: opening a TTF printed it. Two halves, tested
# separately because they are different mechanisms -- `show` refuses (the
# producer looked at the bytes), and the terminal's guard collapses whatever
# still arrives (the backstop that covers `cat` and every program not written
# yet).
# The other half of the video-vs-image number: what ONE inline image costs, on
# the same machine, in the same session. Reported rather than gated -- the point
# of measuring first was to find out whether an image frame per picture would
# do, and the answer has to be allowed to be "the timings are fine, the problem
# is structural" (an image frame appends a scrollback line, so 30 fps evicts a
# 600-line scrollback in twenty seconds).
print("5d. the cost of one image frame, for the comparison the design rests on")
before = len([l for l in log.decode("utf-8", "replace").splitlines() if "TERMPERF image" in l])
ctrl("l")
time.sleep(0.8)
run("show /media/dot.png", 4.0)
imgs = [l for l in log.decode("utf-8", "replace").splitlines() if "TERMPERF image" in l]
chk(len(imgs) > before, "the image frame this section measures actually happened")
if len(imgs) > before:
    print("     " + imgs[-1].strip())
paints = [l for l in log.decode("utf-8", "replace").splitlines() if "TERMPERF paint " in l]
pres = [l for l in log.decode("utf-8", "replace").splitlines() if "video_vs_image" in l]
if paints:
    print("     " + paints[-1].strip())
if pres:
    print("     " + pres[-1].strip())

print("6. a binary is not painted -- by pixels, against a full screen of text")
ctrl("l")
time.sleep(0.8)
run("ls /bin", 3.0)
p_text = shot("ink_text")
ink_text = count(p_text, C_FG)

ctrl("l")
time.sleep(0.8)
run("cat /fonts/mono.ttf", 5.0)
p_bin = shot("ink_bin")
ink_bin = count(p_bin, C_FG)
print(f"     ink: a screen of text = {ink_text} px, a 9636-byte font = {ink_bin} px")
chk(ink_text > 800, f"the text baseline really is a screen of text ({ink_text} px)")
chk(ink_bin * 3 < ink_text,
    f"cat of a font leaves far less ink than ordinary text ({ink_bin} vs {ink_text})")

# ---------------------------------------- 7. the compatibility claim, tested -
print("7. the same command redirected to a file -- no protocol bytes")
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

# The refusal, in BYTES. The pixel check above proves nothing was painted; this
# proves the right thing was said, and that the refusal is not a terminal
# effect -- the serial console's shell has no rich channel at all, and `show`
# still declines. A byte assertion is also the only way to state "and NOT the
# font": the first bytes of an sfnt are 00 01 00 00, and they must not appear.
print("7b. the refusal itself, as bytes, on a shell with no rich channel")
log.clear()
serial.sendall(b"echo " + mark + b"\nshow /fonts/mono.ttf\necho " + mark + b"\n")
wait_for(mark + b"\n", 20)
pump(4.0)
body3 = bytes(log)
f1 = body3.find(mark)
f2 = body3.find(mark, f1 + len(mark)) if f1 >= 0 else -1
f3 = body3.find(mark, f2 + len(mark)) if f2 >= 0 else -1
ref = body3[f2:f3] if (f2 >= 0 and f3 > f2) else b""
chk(b"TrueType font" in ref, f"show names the format ({len(ref)} bytes of output)")
chk(b"00 01 00 00" in ref, "and hexdumps the bytes that prove it")
chk(b"\x00\x01\x00\x00" not in ref, "while NOT emitting the font's actual bytes")
chk(len(ref) < 2000, f"the whole refusal is a few lines, not a file dump ({len(ref)} bytes)")

# And redirected: the refusal is plain text on fd 1, so it lands in the file
# like any other output -- with no protocol bytes, same as everything else.
log.clear()
serial.sendall(b"echo " + mark + b"\nshow /fonts/mono.ttf > /ttf.txt\nwc /ttf.txt\necho " + mark + b"\n")
wait_for(mark + b"\n", 20)
pump(4.0)
body4 = bytes(log)
chk(b"LRT\x01" not in body4, "the redirected refusal contains no protocol frames")

# ------------------------------------------- 8. `clear` actually clears -----
# It did not. c/apps/coreutils/clear.c was the only escape-emitting program in
# the tree, and this terminal has no escape parser on purpose -- put_char()
# refuses C0 -- so ESC was dropped and the three printable bytes that followed
# it ("[2J", "[H") were painted as text. The window filled up instead of
# emptying. It is a side-band frame now (RT_T_CLEAR), and the escapes survive
# for the listener they were right for, which is tested below on the serial
# console.
print("8. `clear` empties the screen (over the side band, not two ESC bytes)")
ctrl("l")
time.sleep(0.8)
run("ls /bin", 3.5)
p_full = shot("clear_before")
ink_full = count(p_full, C_FG)
# Slower than run()'s default. PS/2 has a one-byte buffer and this keystroke
# travels terminal -> control pipe -> shell -> rich pipe -> terminal before it
# is echoed; a dropped letter runs a command that does not exist, and the
# assertion below would then read "clear does not clear" while measuring a
# typo. The error-ink check is what tells the two apart -- a mistyped command
# prints "not found" in red, and `clear` prints nothing at all.
typ("clear\n", settle=0.22)
time.sleep(5.0)
p_cleared = shot("clear_after")
ink_cleared = count(p_cleared, C_FG)
typo = count(p_cleared, C_ERR)
print(f"     ink: before clear = {ink_full} px, after = {ink_cleared} px,"
      f" error ink = {typo} px")
chk(ink_full > 800, f"the pre-clear screen really is full of text ({ink_full} px)")
chk(typo == 0, f"the keystrokes arrived -- no command-not-found line ({typo} px of red)")
chk(ink_cleared * 8 < ink_full,
    f"and `clear` emptied it ({ink_cleared} px left of {ink_full})")

# The other half, and the control that makes the first half mean something: on
# the serial console there is NO rich channel, so `clear` must fall back to the
# escapes -- a terminal that really is a VT still gets what it understands. If
# this fails, the frame path is not being chosen by the channel's presence.
# Polled to a deadline rather than slept at: wait_for() cannot be used here
# because the guest echoes CR LF, so `mark + b"\n"` never matches and it always
# burns its whole timeout.
log.clear()
serial.sendall(b"echo " + mark + b"\nclear\necho " + mark + b"\n")
deadline = time.time() + 25
while time.time() < deadline and b"\x1b[2J" not in log:
    time.sleep(0.25)
body5 = bytes(log)
chk(b"\x1b[2J" in body5,
    "on the serial console (no rich channel) `clear` still emits ESC [ 2 J")
chk(b"LRT\x01" not in body5, "and emits no protocol bytes there")

# ----------------------------- 9. a truncated table says it was truncated ----
# The terminal's table storage is TROW=48 rows (c/apps/gui/terminal.c). /bin on
# this disk holds 55 programs and `dir` sends every one of them in a single
# frame, so seven rows used to be dropped in silence -- a table that simply
# ended, indistinguishable on screen from a directory with 48 entries in it.
# Raising the constant would move the number, not fix the shape.
print("9. a table larger than the window's storage says so, instead of ending")
ctrl("l")
time.sleep(0.8)
log.clear()
run("dir /bin", 3.5)
p_tr = shot("trunc")
# Nothing else on this screen writes stderr, and the notice is the only thing
# drawn in the error colour -- so any ink of it at all is the notice.
n_notice = count(p_tr, C_ERR)
chk(n_notice > 20, f"the truncation notice is drawn, in the error colour ({n_notice} px)")
warn = [l for l in log.decode("utf-8", "replace").splitlines()
        if "TERMWARN table truncated" in l]
chk(len(warn) >= 1, "and the drop is reported on fd 2, as bytes")
if warn:
    print("     " + warn[-1].strip())
    kv = dict(p.split("=") for p in warn[-1].split() if "=" in p)
    shown, frame = int(kv.get("shown_rows", 0)), int(kv.get("frame_rows", 0))
    chk(frame > shown and shown > 0,
        f"the report names both numbers ({shown} shown of {frame} sent)")

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
