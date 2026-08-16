#!/usr/bin/env python3
"""Drive /usr/as/examples/asview.as -- the AetherScript image viewer -- over QMP
and assert against the PIXELS.

WHY PIXELS. The claim is not "the script ran"; a script that printed every
marker below and drew nothing would satisfy a serial-log test completely. The
claim is that an application written in AetherScript put a picture on the
screen, responded to a key, and -- handed something it cannot decode -- said so
on the window instead of showing an empty one. Only the frame settles any of
that.

WHAT MAKES THE MEASUREMENT EXACT RATHER THAN A TOLERANCE.
build/dot.png (tests/unit/dot_gen.py) is a SOLID 60x40 rectangle of
RGB(255,0,229), a colour nothing else in the UI uses -- it was generated for
exactly this kind of question. Every step from that file to the screendump is
integer and lossless:

  * the PNG has no alpha, so the decoder hands the kernel a=255 and
    fb_blit_rgba writes the source byte with no compositing;
  * SYS_GUI_BLIT rescales NEAREST-NEIGHBOUR, so a scaled copy of a solid
    rectangle is still exactly that colour -- there is no edge blending;
  * the driver boots at 1280x800, where fb.c's pick_scale is 100 and one point
    is one device pixel, so a window rect IS a screen rect.

So the assertion is A COUNT OF PIXELS OF ONE EXACT COLOUR, equal to the area of
a rectangle this file computes ITSELF from the same arithmetic image.fit() uses.
It is deliberately not read out of the program's own output -- the program
prints its rect as well, and the two are compared, which is a second and
independent check that the library and this driver agree on what "fit" means.

WHAT EACH CASE PROVES
  fit      the picture is on screen at the size fit-to-window says it is.
  actual   pressing 'a' changes that count to exactly 60x40. The app is
           interactive ON A STILL IMAGE, where nothing changes unless the app
           changes it -- an animated fixture would have made "the screen
           changed" worthless, because it changes on its own.
  next     the same viewer on /media/img/still.bmp -- a second directory and a
           second decoder -- where 'n' steps to another image in the listing.
           /media holds exactly one picture, so a next/previous walk proved
           there would have proved nothing.
  refuse   handed /media/sample.h264 (a real file on the disk that is not an
           image) the window shows a sentence naming the path and the reason,
           and holds ZERO pixels of the picture colour. Measured as both: the
           count is 0 AND the picture box has many distinct colours, where
           blank would have one or two. This is the unit's negative control and
           it PASSES by showing a refusal, not by failing to draw.
  scope    the same picture, decoded by a process whose capability was narrowed
           away from it (`as --scope /usr/as`). M28 refusals are catchable, so
           the app must still open a window and must NAME the capability.

Usage: qmp_asview.py --iso ISO --disk DISK [--out DIR] [--only a,b] [--keep]
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

from qmp_ui import Session, configure, PPM  # noqa: E402

# --- the viewer's own geometry, mirrored from fsroot/as/examples/asview.as ---
# Mirrored and not imported because there is nothing to import from: the
# program is AetherScript. Every number here is a claim about that file, and
# the rect comparison below is what keeps the claim honest -- asview prints the
# rect it actually used and this driver requires its own arithmetic to agree.
WIN_W, WIN_H = 560, 400
HEADER, FOOTER, PAD = 30, 22, 8
BOX = (PAD, HEADER, WIN_W - 2 * PAD, WIN_H - HEADER - FOOTER)

# ...and the window's position, which is ASKED FOR rather than predicted.
#
# wm.c places a new window at `S(110 + cascade * 28)`, and the first attempt
# here computed that from a count of the windows this driver had opened. It was
# wrong on the first run and the failure is worth keeping: the DESKTOP had
# already opened Finder, so cascade was 1 before the viewer existed, and the
# check reported the picture 28 pixels from where it should be -- a real-looking
# geometry bug caused entirely by the test's model of the machine. Any fixed
# arithmetic here is a second copy of the compositor's placement policy, and it
# will be wrong again the next time the desktop opens one more window at boot.
#
# So the frame comes from the kernel's own report on the console:
#     [wm] win 1 frame 138 98 560 430 content 560 400 pt zoom 0 min 0 asview
# which makes the assertion a THREE-WAY agreement -- the kernel's window
# position, the app's own printed rect, and this file's independent fit
# arithmetic all have to land on the same pixels -- instead of a two-way one
# with a guess standing in for the third. TITLEBAR_H is still a constant
# because `frame` is the outer frame and `content` starts one title bar below
# it; the printed content size is checked against it below.
TITLEBAR_H = 30
WIN_FRAME_RE = re.compile(
    r"\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) .*asview")

DOT_RGB = (0xFF, 0x00, 0xE5)     # tests/unit/dot_gen.py
DOT_W, DOT_H = 60, 40


def fit_rect(iw, ih, bx, by, bw, bh):
    """image.fit() in Python, integer for integer.

    The comparison is done before any division for the same reason the
    AetherScript does it that way: `iw*bh <= bw*ih` is `iw/ih <= bw/bh` with
    nothing rounded before the decision. Python's // truncates toward negative
    infinity and AetherScript's / truncates toward zero; every value here is
    positive, so they agree -- and if a future box could be negative this
    function would have to say so rather than differ silently."""
    if iw * bh <= bw * ih:
        h = bh
        w = iw * bh // ih
    else:
        w = bw
        h = ih * bw // iw
    w = max(w, 1)
    h = max(h, 1)
    return (bx + (bw - w) // 2, by + (bh - h) // 2, w, h)


def centre_rect(iw, ih, bx, by, bw, bh):
    return (bx + (bw - iw) // 2, by + (bh - ih) // 2, iw, ih)


def count_colour(ppm, rgb):
    """How many pixels are EXACTLY this colour.

    str.find over the raw plane is used rather than a per-pixel loop because a
    1280x800 frame is a million pixels and this runs four times; the `% 3`
    guard is what keeps it correct, since a three-byte pattern can otherwise
    match straddling two pixels."""
    target = bytes(rgb)
    n = 0
    i = ppm.px.find(target)
    while i >= 0:
        if i % 3 == 0:
            n += 1
        i = ppm.px.find(target, i + 1)
    return n


def colour_box(ppm, rgb):
    """Bounding box (x0, y0, x1, y1) of that colour, or None."""
    return ppm.find_color(rgb)


def distinct_colours(ppm, x, y, w, h):
    """How many different colours appear in a screen rect.

    This is the blank-window detector, and it is the right instrument for it:
    a window that drew nothing but its background has one colour, plus a
    second if it also filled the picture box; a window with a wrapped sentence
    of antialiased text has dozens of grey levels. A brightness threshold
    would have to know whether the desktop is in dark mode; counting distinct
    values does not."""
    seen = set()
    row = ppm.w * 3
    for yy in range(y, min(y + h, ppm.h)):
        base = yy * row + x * 3
        line = ppm.px[base:base + w * 3]
        for i in range(0, len(line) - 2, 3):
            seen.add(line[i:i + 3])
    return len(seen)


def differing_pixels(a, b, x, y, w, h):
    """How many pixels differ between two frames inside one screen rect.

    The `next` case needs this because both fixtures are 40x28 and land in
    exactly the same destination rect, so every number the program PRINTS about
    them is identical. The only thing that can distinguish "it loaded the next
    file" from "it redrew the same one" is the picture itself."""
    row = a.w * 3
    n = 0
    for yy in range(y, min(y + h, a.h, b.h)):
        base = yy * row + x * 3
        la = a.px[base:base + w * 3]
        lb = b.px[base:base + w * 3]
        for i in range(0, min(len(la), len(lb)) - 2, 3):
            if la[i:i + 3] != lb[i:i + 3]:
                n += 1
    return n


def ppm_to_png(src, dst):
    """A PPM is not an attachable artifact; a PNG is. Written by hand rather
    than through PIL so the gate has no optional dependency."""
    p = PPM(src)
    raw = b"".join(b"\x00" + p.px[y * p.w * 3:(y + 1) * p.w * 3] for y in range(p.h))

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", p.w, p.h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(dst, "wb") as fh:
        fh.write(png)
    return dst


def read(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def main(argv):
    iso = os.path.join(ROOT, "build", "logit.iso")
    disk = os.path.join(ROOT, "build", "disk.img")
    outdir = os.path.join(ROOT, "build", "asview-shots")
    only, keep = None, False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--iso":     iso = argv[i + 1]; i += 2
        elif a == "--disk":  disk = argv[i + 1]; i += 2
        elif a == "--out":   outdir = argv[i + 1]; i += 2
        elif a == "--only":  only = set(argv[i + 1].split(",")); i += 2
        elif a == "--keep":  keep = True; i += 1
        else:
            print("unknown arg %r" % a); return 2

    os.makedirs(outdir, exist_ok=True)
    configure(1280, 800)                  # scale 100: a point IS a device pixel
    tmp = tempfile.mkdtemp(prefix="logit-asview-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    fails = []

    def ck(cond, what, detail=""):
        print("%-4s %s%s" % ("ok" if cond else "FAIL", what,
                             ("  [%s]" % detail) if detail else ""), flush=True)
        if not cond:
            fails.append(what)

    # PRIVATE COPIES, and this is not tidiness -- it is the lesson
    # tests/qmp/qmp_preview.py already paid for. `-snapshot` keeps the guest's
    # writes out of build/disk.img; it does not keep ANOTHER PROCESS's writes
    # out of the guest. Several lines work in this tree at once and a `make` in
    # any of them rewrites build/disk.img under a QEMU that reads it lazily.
    run_iso = os.path.join(tmp, "logit.iso")
    run_disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(iso, run_iso)
    shutil.copyfile(disk, run_disk)

    serial_fh = open(serial, "wb")
    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", run_iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % run_disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
         "-serial", "stdio", "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdin=subprocess.PIPE, stdout=serial_fh, stderr=subprocess.DEVNULL)

    def shell(cmd):
        qemu.stdin.write((cmd + "\n").encode())
        qemu.stdin.flush()

    def wait_for(pattern, mark, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, read(serial)[mark:])
            if m:
                return m
            if qemu.poll() is not None:
                return None
            time.sleep(0.4)
        return None

    def win_frame(mark):
        """The LAST asview window frame the kernel reported after `mark`.

        Last and not first: the console interleaves output from four cores and
        an earlier line can be shredded mid-word (the Finder frame line in this
        gate's own log is), so a torn line simply fails to match and the next
        clean one wins."""
        got = None
        for m in WIN_FRAME_RE.finditer(read(serial)[mark:]):
            got = tuple(int(g) for g in m.groups())
        if got is None:
            return None
        return (got[0], got[1], got[4], got[5])     # x, y, content w, content h

    shots = []

    def shot(ui, name):
        p = os.path.join(tmp, name + ".ppm")
        ui.screendump(p, settle=1.0)
        png = ppm_to_png(p, os.path.join(outdir, name + ".png"))
        if png not in shots:
            shots.append(png)
        return PPM(p)

    try:
        deadline = time.time() + 300
        while time.time() < deadline:
            if "desktop live" in read(serial):
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.3)
        else:
            print("FAIL desktop never came up"); return 1
        time.sleep(4)
        ui = Session(sock, serial=serial)

        # ---------------------------------------------------------------- fit
        if only is None or "fit" in only:
            mark = len(read(serial))
            # `&` so the console shell stays usable for the later cases. The
            # window is adopted by the script's process on its first
            # SYS_GUI_CREATE (wm.c) and raised, so it has the keyboard.
            shell("as /usr/as/examples/asview.as /media/dot.png &")
            ready = wait_for(r"asview: ready", mark, 120)
            ck(ready is not None, "asview starts and reports ready")
            if ready is None:
                raise SystemExit(1)

            m = wait_for(r"asview: image (\S+) (\d+)x(\d+)", mark, 30)
            ck(m is not None and m.group(1) == "PNG"
               and int(m.group(2)) == DOT_W and int(m.group(3)) == DOT_H,
               "the decode reports PNG 60x40",
               m.group(0) if m else "no line")

            want = fit_rect(DOT_W, DOT_H, *BOX)
            m = wait_for(r"asview: mode fit rect=(-?\d+),(-?\d+),(\d+),(\d+)", mark, 30)
            got = tuple(int(g) for g in m.groups()) if m else None
            ck(got == want,
               "the viewer's fit rect equals this driver's own arithmetic",
               "guest %s vs host %s" % (got, want))

            # Let the window's open animation finish: while it runs the
            # compositor is drawing the surface SCALED, so a count taken during
            # it is a count of a different rectangle. Two equal samples in a
            # row is the settling test -- a fixed sleep would be a guess.
            n = -1
            for _ in range(10):
                ppm = shot(ui, "1-fit")
                m2 = count_colour(ppm, DOT_RGB)
                if m2 == n:
                    break
                n = m2
                time.sleep(1.0)

            expect = want[2] * want[3]
            ck(n == expect,
               "the picture covers exactly %d pixels of RGB%s" % (expect, DOT_RGB),
               "counted %d" % n)

            box = colour_box(ppm, DOT_RGB)
            fr = win_frame(mark)
            ck(fr is not None and (fr[2], fr[3]) == (WIN_W, WIN_H),
               "the kernel reports the window the app asked for (%dx%d content)"
               % (WIN_W, WIN_H), "frame %s" % (fr,))
            want_xy = (fr[0] + want[0], fr[1] + TITLEBAR_H + want[1]) if fr else None
            ck(box is not None and want_xy is not None
               and (box[0], box[1]) == want_xy,
               "and it starts at the pixel the KERNEL's window position implies",
               "at %s, expected %s" % (box[:2] if box else None, want_xy))

            # ------------------------------------------------------- actual
            mark = len(read(serial))
            ui.key("a")
            m = wait_for(r"asview: mode 1:1 rect=(-?\d+),(-?\d+),(\d+),(\d+)", mark, 30)
            ck(m is not None, "pressing 'a' switches the viewer to actual size",
               m.group(0) if m else "no mode line -- the key never arrived")
            if m is not None:
                got = tuple(int(g) for g in m.groups())
                ck(got == centre_rect(DOT_W, DOT_H, *BOX),
                   "the 1:1 rect equals this driver's own arithmetic",
                   "guest %s" % (got,))

            n2 = -1
            for _ in range(6):
                ppm = shot(ui, "2-actual")
                c = count_colour(ppm, DOT_RGB)
                if c == n2:
                    break
                n2 = c
                time.sleep(0.8)
            ck(n2 == DOT_W * DOT_H,
               "and the picture now covers exactly %d pixels" % (DOT_W * DOT_H),
               "counted %d" % n2)
            ck(n2 != n, "the key CHANGED the frame (not the same count twice)",
               "%d -> %d" % (n, n2))

            mark = len(read(serial))
            ui.key("q")
            ck(wait_for(r"asview: bye", mark, 30) is not None,
               "'q' quits the viewer")

        # --------------------------------------------------------------- next
        # A SECOND DIRECTORY AND A SECOND DECODER, and both are the point.
        # /media/dot.png is the only image in /media, so `n` there is correctly
        # a no-op and proves nothing about the directory walk. /media/img holds
        # six fixtures in five formats, so this case exercises sys.ls() through
        # SYS_DIR_COUNT/SYS_DIR_NAME, asview's extension filter, and a format
        # that is NOT the one every other case uses -- a viewer that only ever
        # decoded PNG would pass every check above.
        if only is None or "next" in only:
            mark = len(read(serial))
            time.sleep(1.0)
            shell("as /usr/as/examples/asview.as /media/img/still.bmp &")
            m = wait_for(r"asview: image (\S+) (\d+)x(\d+)", mark, 120)
            ck(m is not None and m.group(1) == "BMP",
               "a second format decodes: BMP", m.group(0) if m else "no line")
            first = m.group(0) if m else ""
            wait_for(r"asview: mode fit", mark, 30)
            before = shot(ui, "5-before")

            mark = len(read(serial))
            ui.key("n")
            m = wait_for(r"asview: open (/media/img/\S+)", mark, 60)
            ck(m is not None and m.group(1) != "/media/img/still.bmp",
               "'n' steps to another image in the same directory",
               m.group(1) if m else "nothing opened -- the listing was empty?")
            m2 = wait_for(r"asview: image (\S+) (\d+)x(\d+)", mark, 60)
            ck(m2 is not None and m2.group(0) != first,
               "and it decodes to a DIFFERENT picture than the one before",
               m2.group(0) if m2 else "no decode")
            # WAIT FOR THE REPAINT, not for a guess at how long one takes. The
            # first version of this case shot the frame as soon as the decode
            # line appeared and captured the PREVIOUS picture, header and all
            # -- both fixtures are 40x28, so the rect line was identical and
            # nothing in the log said the artifact was stale. The redraw prints
            # its own marker; that is the event to wait on.
            wait_for(r"asview: mode fit", mark, 30)
            fr = win_frame(mark)
            ox, oy = (fr[0], fr[1] + TITLEBAR_H) if fr else (0, 0)
            # THE WHOLE CONTENT AREA, not just the picture box -- and the
            # difference between the two is a fact about the fixtures, learned
            # the hard way. still.bmp and still.webp are the SAME 40x28 test
            # pattern in two formats (that is why the image tests ship both),
            # they land in the same rect, and comparing only the picture box
            # therefore reported "0 pixels differ" for a viewer that had
            # correctly loaded the next file. The header line -- the filename
            # and "BMP 40x28" / "WebP 40x28" -- is the part that has to change,
            # and it is inside the window and outside that box.
            #
            # AND THE SHOT IS RETRIED RATHER THAN TIMED. `asview: mode fit` is
            # printed in the MIDDLE of draw(), before the last gui_text and
            # before the flush, so a screendump issued the moment it appears
            # catches the previous frame -- which is exactly what happened, and
            # produced an artifact PNG showing the old filename under the new
            # one's log line. A fixed sleep would be a guess at how long a
            # 497x348 blit takes under TCG; waiting for the frame to actually
            # differ is the condition itself.
            diff, after = 0, before
            for _ in range(12):
                time.sleep(1.0)
                after = shot(ui, "5-next")
                diff = differing_pixels(before, after, ox, oy, WIN_W, WIN_H)
                if diff > 0:
                    break
            ck(diff > 200,
               "and the WINDOW ON SCREEN changed with it (%d pixels differ)" % diff,
               "same rect, same-looking fixture: the header is what must move")
            mark = len(read(serial))
            ui.key("q")
            wait_for(r"asview: bye", mark, 30)

        # ------------------------------------------------------------- refuse
        if only is None or "refuse" in only:
            mark = len(read(serial))
            time.sleep(1.0)
            shell("as /usr/as/examples/asview.as /media/sample.h264 &")
            m = wait_for(r"asview: error (.*)", mark, 120)
            msg = m.group(1).strip() if m else ""
            ck(m is not None, "the viewer refuses a file that is not an image")
            ck("/media/sample.h264" in msg,
               "the refusal names the PATH", msg[:110])
            ck("not an image" in msg and "match no format" in msg,
               "the refusal gives the REASON, with the bytes it looked at",
               msg[:160])
            ck(wait_for(r"asview: ready", mark, 60) is not None,
               "and it still opens a window instead of exiting silently")

            ppm = shot(ui, "3-refuse")
            ck(count_colour(ppm, DOT_RGB) == 0,
               "no picture is on screen (0 pixels of the picture colour)")
            fr = win_frame(mark)
            ck(fr is not None, "the kernel reports the refusal window's frame")
            ox, oy = (fr[0], fr[1] + TITLEBAR_H) if fr else (0, 0)
            nc = distinct_colours(ppm, ox + BOX[0], oy + BOX[1], BOX[2], BOX[3])
            ck(nc > 20,
               "the window is NOT BLANK -- the message box holds %d distinct colours" % nc,
               "a blank fill would hold 1")

            mark = len(read(serial))
            ui.key("q")
            wait_for(r"asview: bye", mark, 30)

        # -------------------------------------------------------------- scope
        if only is None or "scope" in only:
            mark = len(read(serial))
            time.sleep(1.0)
            # --scope BEFORE the script path: as.c refuses a trailing one
            # outright (see run-as-cap-test.sh's history). The narrowed process
            # can still read its own library tree and still open a window; what
            # it may not do is reach /media.
            shell("as --scope /usr/as /usr/as/examples/asview.as /media/dot.png &")
            m = wait_for(r"asview: error (.*)", mark, 120)
            msg = m.group(1).strip() if m else ""
            ck(m is not None, "a narrowed process is refused the file")
            ck("refused" in msg and "capability scope" in msg and "/usr/as" in msg,
               "the refusal NAMES the capability that was missing", msg[:150])
            ck(wait_for(r"asview: ready", mark, 60) is not None,
               "the refusal is catchable: the app runs on and draws it")
            ppm = shot(ui, "4-scope")
            ck(count_colour(ppm, DOT_RGB) == 0,
               "and nothing of the picture reached the screen")
            mark = len(read(serial))
            ui.key("q")
            wait_for(r"asview: bye", mark, 30)

    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        serial_fh.close()
        shutil.copyfile(serial, os.path.join(outdir, "serial.log"))
        if keep:
            print("kept: %s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    for p in shots:
        print("shot: %s" % p)
    print("log:  %s" % os.path.join(outdir, "serial.log"))
    if fails:
        print("FAIL: %d check(s) -- %s" % (len(fails), "; ".join(fails)))
        return 1
    print("PASS: asview draws, responds to a key, and names what it cannot decode")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
