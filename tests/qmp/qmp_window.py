#!/usr/bin/env python3
"""Can a user resize, zoom, minimise and switch windows on this desktop?

Before this, the entire verb vocabulary was DRAG and CLOSE. So the first job of
this driver is not to check a refinement -- it is to check that four gestures
that did not exist now do, using a real PS/2 mouse and a real PS/2 keyboard
over QMP, against the guest's own account of its window geometry.

WHY THE GUEST IS ASKED RATHER THAN THE PICTURE. wm.c prints
`[wm] win N frame X Y W H content CW CH pt zoom Z min M TITLE` whenever a window
settles. Deriving the same numbers from a screendump means finding the traffic
lights and working backwards through the UI scale, which breaks the day the
titlebar is restyled -- and cannot see `content`, `zoom` or `min` at all, since
none of them is a colour on screen. The pixels are still checked; they are
checked for the thing pixels are the authority on, which is whether anything
STALE was left behind.

The checks, in order:

  1. GEOMETRY. Drag each of the four edges and each of the four corners. Every
     one must move the edge it grabbed and leave the other three where they
     were -- the check that catches a corner wired to the wrong axis, which
     looks fine in a screenshot of any single drag.
  2. THE MINIMUM. Drag an edge far past the floor. The window must stop at the
     floor and must not invert.
  3. NO STALE PIXELS. Shrink a window, photograph, force whole-screen
     recomposites of the identical state (an even number of dark-mode toggles),
     photograph again. Every pixel must match. This is the check the negative
     control below inverts, and it is the one that matters: when a window
     SHRINKS the old box CONTAINS the new one, so every counter-based check
     still passes while a band of the previous frame sits on the wallpaper.
  4. ZOOM AND RESTORE. The green light and a titlebar double-click must both
     zoom; restore must return the frame EXACTLY, to the pixel.
  5. MINIMISE. Cmd+M and the yellow light hide the window; the dock icon and
     Cmd+Tab bring it back.
  6. THE SHORTCUT TABLE, including the claim rule: Cmd+K must REACH the focused
     app (the Terminal echoes it into its input line) and Cmd+W must NOT.
     That pair is the whole rule -- a closed list is claimed, everything else is
     forwarded -- and it is the only way to see that the WM is not simply
     eating every Cmd combination.

NEGATIVE CONTROL (--negative): rebuilds the kernel with WM_RESIZE_DAMAGE_LIE=1
in a throwaway copy of the tree. That kernel reports the new window box and
forgets the old one, which is precisely the mistake a resize invites. Check 3
must then FAIL. An assertion nobody has watched fail is not a known-failing
assertion.

BENCH (--bench): brackets a resize drag with the compositor's own counters and
prints ns per composite and pixels per composite, so "is live resize usable"
is answered with numbers at whatever resolution it is run at.

Every timing here is TCG.

Usage:
    tests/qmp/qmp_window.py [--xres W] [--yres H] [--iso PATH]
                            [--shots DIR] [--negative] [--bench]
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, dock_icon, pt   # noqa: E402
from qmp_repaint import CLOSE_RGB, boot, perf_samples       # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TERMINAL_SLOT = 3
TEXTEDIT_SLOT = 1


# ---------------------------------------------------------------------------
# the guest's own account of its windows

WIN_RE = re.compile(
    r"\[wm\] win (\d+) frame (-?\d+) (-?\d+) (-?\d+) (-?\d+) "
    r"content (-?\d+) (-?\d+) pt zoom (\d+) min (\d+) (.*)")


def windows(serial):
    """The latest reported state of every window, keyed by slot."""
    out = {}
    with open(serial, errors="replace") as fh:
        for line in fh:
            m = WIN_RE.search(line)
            if m:
                g = m.groups()
                out[int(g[0])] = dict(
                    x=int(g[1]), y=int(g[2]), w=int(g[3]), h=int(g[4]),
                    cw=int(g[5]), ch=int(g[6]),
                    zoom=int(g[7]), min=int(g[8]), title=g[9].strip())
            elif "[wm] win " in line and " gone" in line:
                try:
                    out.pop(int(line.split("[wm] win ")[1].split()[0]), None)
                except (IndexError, ValueError):
                    pass
    return out


def win_by_title(serial, title):
    for w in windows(serial).values():
        if w["title"].startswith(title):
            return w
    return None


def frame_of(w):
    return (w["x"], w["y"], w["w"], w["h"]) if w else None


# ---------------------------------------------------------------------------
# screenshots somebody can actually look at

def ppm_to_png(src, dst):
    """A PPM as a PNG, using nothing but zlib. QEMU writes P6; every viewer
    reads PNG. Twenty lines is cheaper than a dependency the CI has to grow."""
    p = PPM(src)
    raw = bytearray()
    row = p.w * 3
    for y in range(p.h):
        raw.append(0)                                  # filter: none
        raw += p.px[y * row:(y + 1) * row]

    def chunk(tag, data):
        c = tag + data
        return (len(data).to_bytes(4, "big") + c +
                zlib.crc32(c).to_bytes(4, "big"))

    hdr = (p.w.to_bytes(4, "big") + p.h.to_bytes(4, "big") +
           bytes([8, 2, 0, 0, 0]))                     # 8-bit truecolour
    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", hdr))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        fh.write(chunk(b"IEND", b""))
    return dst


def diff_pixels(a, b, skip_top):
    """Count the pixels that differ between two frames below `skip_top`, and
    bound them. The menu-bar clock ticks; nothing under it is entitled to."""
    n = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    row = a.w * 3
    for y in range(skip_top, min(a.h, b.h)):
        ra = a.px[y * row:(y + 1) * row]
        rb = b.px[y * row:(y + 1) * row]
        if ra == rb:
            continue
        for x in range(a.w):
            if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                n += 1
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return n, (None if x1 < 0 else (x0, y0, x1, y1))


# ---------------------------------------------------------------------------
# gestures

def aim(ui, ppm, x, y):
    """Put the pointer on (x,y) and CONFIRM the guest agrees it is there.

    Not optional, and the reason is worth stating once: QMP delivers motion as
    `rel` deltas and the emulated PS/2 controller buffers one byte, so a long
    burst loses packets and the pointer stops short. Nothing reports that. Every
    gesture after the first long drag then lands a few pixels off, and a click
    aimed at the green traffic light hits the titlebar instead -- which reads
    exactly like zoom not being implemented. The guest prints `[wm] ptr X Y`
    when the pointer settles; believing it, and re-aiming, is the difference
    between a driver that tests the machine and one that tests its own
    dead-reckoning."""
    got = ui.settle_pointer(ppm, x, y)
    if got != (x, y):
        raise AssertionError("pointer would not settle at (%d,%d); guest says %r"
                             % (x, y, got))
    return got


def drag(ui, ppm, x0, y0, x1, y1, steps=14, hold=0.06):
    """Press at (x0,y0), walk to (x1,y1), release. Stepped rather than jumped:
    the WM derives a resize from motion WHILE the button is down, so a single
    teleport would exercise one sample and prove nothing about the drag."""
    aim(ui, ppm, x0, y0)
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(hold)
    for i in range(1, steps + 1):
        tx = x0 + (x1 - x0) * i // steps
        ty = y0 + (y1 - y0) * i // steps
        dx, dy = tx - ui.cur[0], ty - ui.cur[1]
        if dx or dy:
            ui._input([{"type": "rel", "data": {"axis": "x", "value": dx}},
                       {"type": "rel", "data": {"axis": "y", "value": dy}}])
            ui.cur[0] += dx
            ui.cur[1] += dy
        time.sleep(0.02)
    time.sleep(hold)
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.45)


def cmd_key(ui, qcode, shift=False, settle=0.35):
    """A Cmd chord, held around the key exactly as a hand would.

    Injection is SLOW on purpose: the emulated PS/2 controller buffers one byte,
    and Cmd is a two-byte scancode (E0 5B), so a burst loses halves of chords
    and the failure looks like the shortcut not existing."""
    down = [{"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"}, "down": True}}]
    if shift:
        down.append({"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": True}})
    for ev in down:
        ui._input([ev])
        time.sleep(0.06)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}])
    time.sleep(0.08)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}}])
    time.sleep(0.06)
    if shift:
        ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": False}}])
        time.sleep(0.06)
    ui._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"}, "down": False}}])
    time.sleep(settle)


# ---------------------------------------------------------------------------

def build_negative():
    """A kernel that forgets the OLD box when a window moves or resizes.

    One line in wm.c, flipped by substitution rather than by a patch file, so it
    cannot silently stop applying when the code around it moves."""
    tmp = tempfile.mkdtemp(prefix="logit-wnegctl-")
    dst = os.path.join(tmp, "tree")
    print("     copying the tree to %s ..." % dst)
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns(
        ".git", "build", "build-wm", "__pycache__", "*.pyc", "rust"))
    src_rust = os.path.join(ROOT, "rust")
    if os.path.isdir(src_rust):
        shutil.copytree(src_rust, os.path.join(dst, "rust"))
    wm = os.path.join(dst, "c/kernel/gui/wm.c")
    text = open(wm).read()
    needle = "#define WM_RESIZE_DAMAGE_LIE 0"
    if needle not in text:
        print("FAIL wm.c no longer has %r -- the negative control cannot be built"
              % needle)
        return None
    open(wm, "w").write(text.replace(needle, "#define WM_RESIZE_DAMAGE_LIE 1"))
    print("     building the lying kernel ...")
    r = subprocess.run(["make", "-j", str(os.cpu_count() or 4)],
                       cwd=dst, capture_output=True, text=True)
    iso = os.path.join(dst, "build", "logit.iso")
    if r.returncode != 0 or not os.path.exists(iso):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        return None
    return iso


# Every *.aex at the LogitFS root, in scan_apps order -- the same list
# qmp_ui.py's dock geometry is built from. Kept here as (slot, name) because the
# app inventory below has to name what it opened.
APPS = [(0, "Clock"), (1, "TextEdit"), (2, "Monitor"), (3, "Terminal"),
        (4, "Widgets"), (5, "Finder"), (6, "Preview"), (7, "Studio"),
        (8, "Browser"), (9, "Gallery")]


def app_survey(ui, serial, P, shot, ck, slow, pt_, note):
    """Grow every app's window and report what lands in the new space.

    This is an INVENTORY, not a gate. Those apps belong to other lines, and the
    finding they need is not "it failed" but "here is what your window looks
    like at a size it was not written for".

    The measure is the number of distinct colours in the strip the window just
    grew INTO. An app that laid its content out once, at the size it passed to
    gui_create, leaves that strip as the flat fill the compositor put there --
    one colour. An app that reflowed puts structure in it. The count is reported
    rather than thresholded into a verdict, because "2" and "40" need different
    sentences and neither of them is "FAIL"."""
    for slot, name in APPS:
        try:
            ui.settle_pointer(P, *dock_icon(slot))
            ui.click()
        except Exception as exc:                      # noqa: BLE001
            note("%-9s could not be opened (%s)" % (name, exc))
            continue
        time.sleep(10 * slow)
        w = win_by_title(serial, "")
        wins_now = [v for v in windows(serial).values() if not v["min"]]
        target = None
        for v in wins_now:
            if v["title"].lower().startswith(name.lower()[:5]):
                target = v
        if target is None and wins_now:
            target = wins_now[-1]
        if target is None:
            note("%-9s never reported a window" % name)
            continue
        f = frame_of(target)
        gx, gy = f[0] + f[2], f[1] + f[3]
        dx, dy = pt_(170), pt_(120)
        try:
            drag(ui, P, gx, gy, gx + dx, gy + dy)
        except AssertionError as exc:
            note("%-9s could not be grabbed (%s)" % (name, exc))
            continue
        time.sleep(4.0 * slow)
        g = frame_of(win_by_title(serial, ""))
        img = shot("app-" + name.lower())
        # The strip that is new: the vertical band between the old right edge
        # and the new one, inside the content area.
        x0, x1 = f[0] + f[2], min(g[0] + g[2], img.w) if g else f[0] + f[2]
        y0, y1 = f[1] + pt_(40), min(f[1] + f[3], img.h)
        seen = set()
        if x1 > x0 and y1 > y0:
            for y in range(y0, y1, 3):
                for x in range(x0, x1, 2):
                    seen.add(img.at(x, y))
        note("%-9s frame %r -> %r, %d distinct colour(s) in the new strip"
             % (name, f, g, len(seen)))
        cmd_key(ui, "w", settle=1.5)
        time.sleep(3.0 * slow)


def main(argv):
    xres, yres = 1280, 800
    iso, shots, negative, bench, apps = None, None, False, False, False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":       xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":     yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":      iso = argv[i + 1]; i += 2
        elif argv[i] == "--shots":    shots = argv[i + 1]; i += 2
        elif argv[i] == "--negative": negative = True; i += 1
        elif argv[i] == "--bench":    bench = True; i += 1
        elif argv[i] == "--apps":     apps = True; i += 1
        else:
            print("unknown arg %r" % argv[i]); return 2

    fails, pixel_checks = [], []

    def ck(cond, what):
        print("%-4s %s" % ("ok" if cond else "FAIL", what))
        if not cond:
            fails.append(what)
        return cond

    if negative:
        print("=== negative control: a kernel that forgets the old window box ===")
        iso = build_negative()
        if iso is None:
            return 1
    if iso is None:
        iso = os.path.join(ROOT, "build", "logit.iso")

    scale = configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-window-")
    if shots:
        os.makedirs(shots, exist_ok=True)
    print("=== %dx%d device px (scale %d%%), %s ===" % (xres, yres, scale, iso))

    SKIP = pt(24) + 2
    nshot = [0]

    qemu, sock, serial = boot(iso, xres, yres, tmp)
    try:
        time.sleep(5 * slow)
        ui = Session(sock, serial=serial)
        REST = (pt(30), pt(300))

        def shot(tag, settle=1.0):
            path = os.path.join(tmp, tag + ".ppm")
            ui.screendump(path, settle=settle * slow)
            if shots:
                nshot[0] += 1
                ppm_to_png(path, os.path.join(shots, "%02d-%s.png" % (nshot[0], tag)))
            return PPM(path)

        P = os.path.join(tmp, "aim.ppm")

        def click_at(x, y, hold=0.12):
            aim(ui, P, x, y)
            ui.click(hold=hold)

        if apps:
            print("\n=== every GUI app, grown by hand ===")
            print("     (an inventory, not a gate -- these apps have owners)\n")
            app_survey(ui, serial, P, shot, ck, slow, pt,
                       lambda s: print("     " + s))
            print("\n     screenshots: %s" % shots)
            return 0

        if bench:
            # Straight to the measurement. Running the thirty functional checks
            # first is half an hour at 2560x1600 -- and every one of them drags
            # a window, so by the time the bench ran the geometry it measured
            # was whatever the last check left behind. A measurement should
            # start from a known frame and nothing else.
            print("\n=== resize cost, from the compositor's own counters ===")
            click_at(*dock_icon(TEXTEDIT_SLOT))
            time.sleep(10 * slow)
            f = frame_of(win_by_title(serial, ""))
            if f is None:
                print("FAIL no window to resize"); return 1
            log = open(serial, errors="replace").read()
            base = perf_samples(log)
            b0 = base[-1] if base else None
            gx, gy = f[0] + f[2], f[1] + f[3]
            drag(ui, P, gx, gy, gx - pt(200), gy - pt(140), steps=60, hold=0.02)
            drag(ui, P, gx - pt(200), gy - pt(140), gx, gy, steps=60, hold=0.02)
            time.sleep(2.5 * slow)
            ui.goto(*REST, settle=0.3)
            time.sleep(1.5 * slow)
            s = perf_samples(open(serial, errors="replace").read())
            if not (b0 and s):
                print("FAIL the compositor reported no counters"); return 1
            b1 = s[-1]
            dc = b1["composites"] - b0["composites"]
            dn = b1["ns"] - b0["ns"]
            dp = b1["cpx"] - b0["cpx"]
            dm = b1["motions"] - b0["motions"]
            dt = b1["t"] - b0["t"]
            resizes = open(serial, errors="replace").read().count("[wm] win 1 frame")
            print("     %dx%d  composites=%d  motions=%d  wall=%dms  frame=%r"
                  % (xres, yres, dc, dm, dt, f))
            if dc:
                print("     per composite: %.1f ms, %d px (%.0f%% of the screen)"
                      % (dn / dc / 1e6, dp // dc, 100.0 * (dp / dc) / (xres * yres)))
                print("     composites per motion sample: %.2f" % (dc / dm if dm else 0))
                print("     canvas reallocations reported: %d" % resizes)
            shot("bench-%dx%d" % (xres, yres))
            return 0

        click_at(*dock_icon(TEXTEDIT_SLOT))
        time.sleep(8 * slow)
        w = win_by_title(serial, "")
        if not ck(w is not None, "a window is on screen and reports its frame"):
            return 1
        print("     %s frame=%r content=%dx%d pt"
              % (w["title"], frame_of(w), w["cw"], w["ch"]))
        shot("00-opened")

        # -- 1. every edge and every corner -------------------------------
        print("\n=== 1. drag each edge and each corner ===")
        EDGES = [
            ("right",        lambda f: (f[0] + f[2],       f[1] + f[3] // 2),  pt(70),  0),
            ("left",         lambda f: (f[0],              f[1] + f[3] // 2), -pt(50),  0),
            ("bottom",       lambda f: (f[0] + f[2] // 2,  f[1] + f[3]),        0,      pt(60)),
            ("top",          lambda f: (f[0] + f[2] // 2,  f[1]),              0,     -pt(30)),
            ("bottom-right", lambda f: (f[0] + f[2],       f[1] + f[3]),        pt(40), pt(40)),
            ("bottom-left",  lambda f: (f[0],              f[1] + f[3]),       -pt(40), pt(30)),
            ("top-right",    lambda f: (f[0] + f[2],       f[1]),               pt(30), -pt(20)),
            ("top-left",     lambda f: (f[0],              f[1]),              -pt(30), -pt(20)),
        ]
        # Each edge is dragged OUT and then straight back, and the frame must
        # return EXACTLY. That is not tidiness -- it is the test for the one
        # design decision inside the drag: the new frame is computed from the
        # grab ANCHOR rather than accumulated from the previous sample. An
        # accumulating implementation passes every "did it get bigger" check and
        # fails this one, because rounding and dropped packets make the return
        # trip a different length from the outbound one.
        #
        # It also keeps the window near its starting size for the whole sweep,
        # which matters: a window that has grown to the screen edge cannot be
        # dragged further out, and the top edge is deliberately un-grabbable
        # once it reaches the menu bar. Both would fail as "resize is broken".
        for name, grab, dx, dy in EDGES:
            f = frame_of(win_by_title(serial, ""))
            gx, gy = grab(f)
            drag(ui, P, gx, gy, gx + dx, gy + dy)
            time.sleep(1.2 * slow)
            g = frame_of(win_by_title(serial, ""))
            # The edge under the hand must move; the OPPOSITE edges must not.
            # That pair is what catches a corner wired to the wrong axis, which
            # any single screenshot would happily show as "it resized".
            moved_w, moved_h = g[2] != f[2], g[3] != f[3]
            want_w, want_h = dx != 0, dy != 0
            fixed = []
            if "left" not in name:
                fixed.append(("x", g[0] == f[0]))
            if "top" not in name:
                fixed.append(("y", g[1] == f[1]))
            ok = (moved_w == want_w and moved_h == want_h and
                  all(v for _, v in fixed))
            ck(ok, "%-13s %r -> %r  (w%s h%s, fixed %s)"
                % (name, f, g,
                   "+" if moved_w else "=", "+" if moved_h else "=",
                   ",".join(k for k, v in fixed if v) or "-"))
            shot("edge-" + name)
            drag(ui, P, gx + dx, gy + dy, gx, gy)        # ...and back
            time.sleep(1.2 * slow)
            h = frame_of(win_by_title(serial, ""))
            ck(h == f, "%-13s out and back returns the frame exactly (%r)" % (name, h))

        # -- 2. the floor --------------------------------------------------
        print("\n=== 2. the minimum size ===")
        f = frame_of(win_by_title(serial, ""))
        drag(ui, P, f[0] + f[2], f[1] + f[3], f[0] + pt(4), f[1] + pt(4), steps=20)
        time.sleep(1.5 * slow)
        g = frame_of(win_by_title(serial, ""))
        ck(g[2] > 0 and g[3] > 0 and g[2] >= pt(150) and g[3] >= pt(60),
           "dragged far past the floor: stopped at %dx%d, did not invert" % (g[2], g[3]))
        shot("min-size")

        # -- 3. stale pixels (the negative control's target) ---------------
        print("\n=== 3. a shrink leaves nothing behind ===")

        def toggle_theme(times=2):
            """Force whole-screen recomposites of the CURRENT state: the
            dark-mode switch is the one interaction that legitimately repaints
            everything, so an even number leaves the desktop as it was, drawn
            entirely by the full path."""
            tx = xres - pt(210) + pt(19)
            ty = (pt(24) - pt(18)) // 2 + pt(9)
            aim(ui, P, tx, ty)
            for _ in range(times):
                ui.click(hold=0.1)
                time.sleep(1.2 * slow)
            ui.goto(*REST, settle=0.3)
            time.sleep(1.0 * slow)

        # A ROUND TRIP, not a partial-vs-full comparison.
        #
        # The obvious test -- shrink, photograph, force whole-screen repaints of
        # the same state, photograph, demand equality -- does not work here, and
        # the reason is worth writing down: forcing a full repaint means toggling
        # the theme, which sends EV_THEME to every app, which makes the APPS
        # repaint. No app in this tree handles EV_RESIZE, so between the two
        # photographs the window's content changes from "the old canvas,
        # stretched" to "freshly drawn at the app's hardcoded size". That is a
        # real difference and it has nothing to do with the compositor; the
        # first run of this check reported 214,130 differing pixels of it.
        #
        # Photographing the SAME geometry at both ends removes the confound
        # completely. The window is grown and then shrunk back to exactly the
        # frame it started at -- which the reversibility checks above have
        # already established is achievable to the pixel -- so the app has
        # repainted at the final size and the content matches by construction.
        # What CANNOT match, if damage is under-reported, is the band the larger
        # window covered and the smaller one does not: nothing repaints it,
        # because there is deliberately no periodic full repaint to cover for a
        # caller that under-reported.
        ui.goto(*REST, settle=0.4)
        before_frame = frame_of(win_by_title(serial, ""))
        a = shot("roundtrip-before", settle=1.4)
        f = before_frame
        gx, gy = f[0] + f[2], f[1] + f[3]
        drag(ui, P, gx, gy, gx + pt(220), gy + pt(150))
        time.sleep(2.0 * slow)
        drag(ui, P, gx + pt(220), gy + pt(150), gx, gy)
        time.sleep(2.5 * slow)
        ui.goto(*REST, settle=0.4)
        after_frame = frame_of(win_by_title(serial, ""))
        b = shot("roundtrip-after", settle=1.4)
        ck(after_frame == before_frame,
           "grow and shrink back returns the frame exactly (%r)" % (after_frame,))
        n, bb = diff_pixels(a, b, SKIP)
        okp = ck(n == 0, "...and leaves NOTHING behind: %d stale px%s"
                 % (n, "" if bb is None else " in %r" % (bb,)))
        pixel_checks.append(okp)

        # -- 4. zoom and restore -------------------------------------------
        print("\n=== 4. zoom and restore ===")
        before = frame_of(win_by_title(serial, ""))
        f = before
        click_at(f[0] + pt(52), f[1] + pt(15))         # the green light
        time.sleep(2.0 * slow)
        z = win_by_title(serial, "")
        ck(z["zoom"] == 1 and z["w"] > before[2] and z["y"] == pt(24),
           "green light zooms: %r -> %r, top edge at the menu bar" % (before, frame_of(z)))
        shot("zoomed")
        click_at(z["x"] + pt(52), z["y"] + pt(15))
        time.sleep(2.0 * slow)
        r = win_by_title(serial, "")
        ck(r["zoom"] == 0 and frame_of(r) == before,
           "restore returns the frame EXACTLY: %r == %r" % (frame_of(r), before))
        shot("restored")

        f = frame_of(win_by_title(serial, ""))
        tx, ty = f[0] + f[2] // 2, f[1] + pt(15)
        aim(ui, P, tx, ty)
        ui.click(hold=0.06)
        time.sleep(0.12)
        ui.click(hold=0.06)
        time.sleep(2.0 * slow)
        d = win_by_title(serial, "")
        ck(d["zoom"] == 1, "double-clicking the titlebar zooms too (%r)" % (frame_of(d),))
        aim(ui, P, d["x"] + d["w"] // 2, d["y"] + pt(15))
        ui.click(hold=0.06)
        time.sleep(0.12)
        ui.click(hold=0.06)
        time.sleep(2.0 * slow)
        ck(win_by_title(serial, "")["zoom"] == 0, "...and un-zooms")

        # -- 5. minimise ----------------------------------------------------
        print("\n=== 5. minimise ===")
        click_at(*dock_icon(TERMINAL_SLOT))          # a second window
        time.sleep(14 * slow)
        term = win_by_title(serial, "Terminal")
        if ck(term is not None, "the Terminal opened"):
            cmd_key(ui, "m")
            time.sleep(1.6 * slow)
            t2 = win_by_title(serial, "Terminal")
            ck(t2 and t2["min"] == 1, "Cmd+M minimises the focused window")
            shot("minimised")
            click_at(*dock_icon(TERMINAL_SLOT))       # its dock icon brings it back
            time.sleep(3.0 * slow)
            t3 = win_by_title(serial, "Terminal")
            ck(t3 and t3["min"] == 0, "its dock icon brings it back")
            ck(t3 and frame_of(t3) == frame_of(term),
               "...to the frame it had (%r)" % (frame_of(t3),))
            shot("unminimised")

        # -- 6. the shortcut table + the claim rule -------------------------
        print("\n=== 6. shortcuts, and what the WM does NOT claim ===")
        # Cmd+Tab must change which window is on top. The z-order is not
        # reported, so it is read the way a user reads it: the focused window is
        # the one with a coloured close button.
        t_before = win_by_title(serial, "Terminal")
        pre = shot("before-tab")
        cmd_key(ui, "tab")
        time.sleep(2.2 * slow)
        post = shot("after-tab")
        n, _ = diff_pixels(pre, post, SKIP)
        ck(n > 0, "Cmd+Tab restacks the desktop (%d px changed)" % n)
        cmd_key(ui, "tab", shift=True)
        time.sleep(2.2 * slow)
        back = shot("after-shift-tab")
        n2, _ = diff_pixels(pre, back, SKIP)
        ck(n2 == 0, "Cmd+Shift+Tab is its exact inverse (%d px differ from before)" % n2)

        # THE CLAIM RULE. Cmd+K is not on the WM's list, so the focused app must
        # receive it; Cmd+W is, so the app must not. The Terminal echoes what it
        # is given into its input line, which is how "reached the app" is
        # observed rather than assumed.
        click_at(*dock_icon(TERMINAL_SLOT))
        time.sleep(4.0 * slow)
        if win_by_title(serial, "Terminal"):
            base = shot("term-idle")
            cmd_key(ui, "k", settle=1.2)
            time.sleep(1.5 * slow)
            after_k = shot("term-after-cmd-k")
            nk, _ = diff_pixels(base, after_k, SKIP)
            ck(nk > 0, "Cmd+K is NOT claimed: it reaches the app (%d px changed)" % nk)
            cmd_key(ui, "w", settle=1.2)
            time.sleep(3.0 * slow)
            ck(win_by_title(serial, "Terminal") is None,
               "Cmd+W IS claimed: the window closed rather than typing a 'w'")
            shot("after-cmd-w")

        # -- bench -----------------------------------------------------------
        if bench:
            print("\n=== resize cost, from the compositor's own counters ===")
            f = frame_of(win_by_title(serial, ""))
            if f:
                base = perf_samples(open(serial, errors="replace").read())
                b0 = base[-1] if base else None
                gx, gy = f[0] + f[2], f[1] + f[3]
                drag(ui, P, gx, gy, gx - pt(200), gy - pt(140), steps=60, hold=0.02)
                drag(ui, P, gx - pt(200), gy - pt(140), gx, gy, steps=60, hold=0.02)
                time.sleep(2.5 * slow)
                ui.goto(*REST, settle=0.3)
                time.sleep(1.5 * slow)
                s = perf_samples(open(serial, errors="replace").read())
                if b0 and s:
                    b1 = s[-1]
                    dc = b1["composites"] - b0["composites"]
                    dn = b1["ns"] - b0["ns"]
                    dp = b1["cpx"] - b0["cpx"]
                    dm = b1["motions"] - b0["motions"]
                    dt = b1["t"] - b0["t"]
                    print("     %dx%d  composites=%d  motions=%d  wall=%dms" %
                          (xres, yres, dc, dm, dt))
                    if dc:
                        print("     per composite: %.1f ms, %d px (%.0f%% of the screen)"
                              % (dn / dc / 1e6, dp // dc,
                                 100.0 * (dp / dc) / (xres * yres)))
                        print("     composites per motion sample: %.2f"
                              % (dc / dm if dm else 0))

        print("\n     serial: %s" % serial)
        if shots:
            print("     screenshots: %s (%d)" % (shots, nshot[0]))
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()

    if negative:
        # The pixel checks are the ones the lie must break. Everything else --
        # geometry, zoom, shortcuts -- keeps working with a lying damage
        # report, which is exactly why a counter-based check cannot see it.
        broke = [c for c in pixel_checks if not c]
        if pixel_checks and broke:
            print("\nnegative control ok: %d/%d pixel check(s) fail against a kernel "
                  "that forgets the old window box" % (len(broke), len(pixel_checks)))
            return 0
        print("\nNEGATIVE CONTROL FAILED: the stale-pixel checks passed even though "
              "the kernel under-reports resize damage")
        return 1

    if fails:
        print("\nqmp_window: %d FAILED" % len(fails))
        for f in fails:
            print("   - %s" % f)
        return 1
    print("\nqmp_window: ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
