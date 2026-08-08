#!/usr/bin/env python3
"""Does the screen ever show a window that is HALF DRAWN?

Reported by the person who uses this machine, and reported as a LOOK, not as a
cost: "I click once in the Browser and it does a reload-refresh"; "I swipe up
5 cm with two fingers in Finder and it refreshes 3 or 4 times." Neither
sentence is about milliseconds. A repaint you can SEE is a different defect
from a repaint that costs too much, and only the second one has a counter.

THE MECHANISM. `struct win` has exactly one canvas (wm.c: `struct surface
surf`). The application draws straight into the buffer the compositor blits
from, and SYS_GUI_FLUSH swaps nothing -- it marks the window damaged and
returns. So an app frame is:

    aui_begin() -> gui_clear(bg)      the whole canvas becomes one flat colour
    ... N drawing syscalls ...        the canvas fills in
    aui_end()   -> SYS_GUI_FLUSH      "this window changed", asynchronously

The compositor runs on its OWN thread and consumes that damage whenever it next
gets the CPU. Nothing stops the app from beginning frame N+1 -- clearing the
canvas -- before the compositor has drawn frame N, and nothing stops the input
path from marking the window dirty (a click raises and focuses it) while the
app is already repainting for that same click. Either way the compositor blits
a canvas that has been erased and not yet repainted, and the window flashes
flat. A burst of events -- a scroll, or the press AND release of one click --
is exactly the condition that makes the app run ahead of the compositor.

WHAT THIS MEASURES: pixels, not counters. A frame counter cannot tell a correct
repaint from a torn one -- both are one frame -- so this photographs the screen
continuously through the gesture and asks a question about the CONTENT of the
window.

The probe is chosen from the machine's own settled picture rather than guessed:
the PROBE_ROWS scanlines of the window's content rectangle that hold the most
distinct colours. In a painted Finder those rows cross glyphs, icons and the
toolbar and hold hundreds of colours between them. A canvas that has just been
cleared holds ONE. There is no threshold argument to have about that ratio, and
the modal colour of the thinnest frame is printed too -- when it equals the
toolkit's background (244,245,248 light) the frame is not merely odd, it is
identifiably the state immediately after gui_clear.

    fail: any captured frame whose probe rows hold fewer than a quarter of the
          colours the same rows hold when the machine is settled.

Screendumps run at ~270/s on this host, so the sampling is dense enough to see
a state that lasts a single app frame; the guest's own `torn=` counter (wm.c
perf line) is read back as an independent second opinion on the same events.

THE NEGATIVE CONTROL is the fix compiled out. --negative builds a kernel with
WM_MIDFRAME_GUARD flipped to 0 in a throwaway copy of the tree -- the
compositor blits whatever is in the canvas at the moment it happens to run,
which is what this machine did before -- and requires the checks above to FAIL.
An assertion nobody has watched fail is not a known-failing assertion.

Usage:
    tests/qmp/qmp_flash.py [--xres W] [--yres H] [--iso PATH]
                           [--rounds N] [--keep DIR] [--negative]
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, pt                # noqa: E402
from qmp_repaint import boot                                  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The Finder is the one window the desktop opens with, and wm.c places the first
# window deterministically: x = S(110), y = S(70), under a 30-point titlebar,
# with the canvas size the app asks for (c/apps/gui/files.c WINW/WINH). So the
# content rectangle is arithmetic, not a search -- which matters, because every
# way of FINDING the window in the picture (its close button, its frame) is
# itself something that could be missing in the very frame this is hunting.
FINDER_X_PT, FINDER_Y_PT = 110, 70
FINDER_W_PT, FINDER_H_PT = 640, 444
TITLEBAR_PT = 30

# WHERE THE GESTURE LANDS, and it is chosen so that it changes NOTHING.
#
# Window-local, inside the Finder's toolbar strip (files.c TOOLBAR_H 46) and to
# the right of the sidebar (SIDEBAR_W 168), in the gap between the Back pill
# (x 176..206) and the View pill (x 526..578). handle_click() takes that branch,
# hits no button and returns; the app still repaints the whole window, because
# an immediate-mode toolkit repaints for every event it is handed.
#
# That is the point. The user's first report is "one left-click, and it does a
# reload-refresh" -- a click that changes nothing visible. Aiming at the file
# list instead would make the picture change for a legitimate reason, and worse:
# files.c calls two clicks on the same row a DOUBLE click when they are within
# 12 of its own frames of each other -- which two rounds of this driver are --
# so it opens the file and launches an app. A brand-new window IS a blank canvas
# on screen, legitimately (it has never been drawn into), and mistaking that for
# this defect is exactly the false positive to design out rather than filter.
AIM_X_PT, AIM_Y_PT = 350, 23

PROBE_ROWS = 10

# How many wheel notches a 5 cm two-finger swipe delivers here.
SWIPE_NOTCHES = 8

# c/apps/gui/aui.c: aui_t.bg, light and dark -- what gui_clear paints. And the
# fill wm.c gives a canvas that has never been drawn into at all.
NAMED = {(244, 245, 248): "aui_t.bg (light): the colour gui_clear paints",
         (28, 28, 32): "aui_t.bg (dark): the colour gui_clear paints",
         (250, 250, 252): "the fill wm.c gives a brand-new canvas"}


def build_negative():
    """A kernel with the mid-frame guard compiled out, in a throwaway copy.

    Flipped by substitution rather than by a patch file, so it cannot silently
    stop applying when the surrounding code moves -- and the run says so out
    loud if the line it is looking for has gone."""
    tmp = tempfile.mkdtemp(prefix="logit-flash-negctl-")
    dst = os.path.join(tmp, "tree")
    print("     copying the tree to %s ..." % dst)
    # `build*`, not `build`: a working tree that several lines are building in
    # at once has build-wm/, build-clip/ and friends beside it, and copying
    # half a gigabyte of other people's object files to compile one file
    # differently is minutes of nothing.
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns(
        ".git", "build*", "__pycache__", "*.pyc", "rust"))
    src_rust = os.path.join(ROOT, "rust")
    if os.path.isdir(src_rust):
        shutil.copytree(src_rust, os.path.join(dst, "rust"))
    wm = os.path.join(dst, "c/kernel/gui/wm.c")
    text = open(wm).read()
    needle = "#define WM_MIDFRAME_GUARD 1"
    if needle not in text:
        print("FAIL wm.c no longer has %r -- the negative control cannot be "
              "built" % needle)
        return None
    open(wm, "w").write(text.replace(needle, "#define WM_MIDFRAME_GUARD 0"))
    print("     building the unguarded kernel ...")
    r = subprocess.run(["make", "-j", str(os.cpu_count() or 4)],
                       cwd=dst, capture_output=True, text=True)
    iso = os.path.join(dst, "build", "logit.iso")
    if r.returncode != 0 or not os.path.exists(iso):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        return None
    # The disk image is a build product too, and boot() reads it from ROOT --
    # so the negative kernel boots the same disk the positive one did, which is
    # what makes the two runs comparable.
    return iso


def content_rect():
    """The Finder's content rectangle, in DEVICE pixels."""
    return (pt(FINDER_X_PT), pt(FINDER_Y_PT) + pt(TITLEBAR_PT),
            pt(FINDER_W_PT), pt(FINDER_H_PT))


def aim_point(rect):
    return rect[0] + pt(AIM_X_PT), rect[1] + pt(AIM_Y_PT)


def row_colours(p, rect, y):
    x0, _, w, _ = rect
    x1 = min(x0 + w, p.w)
    base = y * p.w * 3
    row = p.px[base + x0 * 3: base + x1 * 3]
    return Counter(row[i:i + 3] for i in range(0, len(row) - 2, 3))


def pick_probe(path, rect):
    """The PROBE_ROWS richest scanlines of a settled window, and their colours.

    Choosing the rows from the machine's own picture is what keeps this honest
    at any resolution, theme or file listing: it never assumes where the Finder
    draws, only that a painted window has SOMETHING in it somewhere."""
    p = PPM(path)
    x0, y0, w, h = rect
    scored = []
    for y in range(y0, min(y0 + h, p.h), 2):
        scored.append((len(row_colours(p, rect, y)), y))
    scored.sort(reverse=True)
    rows = sorted(y for _, y in scored[:PROBE_ROWS])
    return rows


def probe(path, rect, rows):
    """(distinct colours, modal colour, modal fraction) over the probe rows."""
    p = PPM(path)
    c = Counter()
    for y in rows:
        if y < p.h:
            c.update(row_colours(p, rect, y))
    total = sum(c.values())
    if not total:
        return 0, (0, 0, 0), 0.0
    col, n = c.most_common(1)[0]
    return len(c), (col[0], col[1], col[2]), n / float(total)


def wheel_event(ui, down=True):
    btn = "wheel-down" if down else "wheel-up"
    ui._input([{"type": "btn", "data": {"button": btn, "down": True}},
               {"type": "btn", "data": {"button": btn, "down": False}}])


def button(ui, down):
    ui._input([{"type": "btn", "data": {"button": "left", "down": down}}])


def run_round(ui, tmp, rect, rows, floor, tag, schedule, seconds):
    """Sample the screen flat out for `seconds`, firing `schedule` as time comes.

    `schedule` is [(offset_seconds, callable)]. The events and the photography
    share one loop deliberately: a burst of dumps taken AFTER the gesture
    finished is a picture of the machine at rest, which is the state that was
    never in question."""
    scratch = os.path.join(tmp, "scratch.ppm")
    t0 = time.time()
    nxt = 0
    worst = None
    n = 0
    while True:
        now = time.time() - t0
        if now >= seconds and nxt >= len(schedule):
            break
        while nxt < len(schedule) and schedule[nxt][0] <= now:
            schedule[nxt][1]()
            nxt += 1
        ui.screendump(scratch, settle=0)
        n += 1
        cols, modal, frac = probe(scratch, rect, rows)
        if worst is None or cols < worst[0]:
            keep = os.path.join(tmp, "%s-thinnest.ppm" % tag)
            shutil.copyfile(scratch, keep)
            worst = (cols, modal, frac, keep)
        if cols < floor:
            ev = os.path.join(tmp, "%s-torn-%03d.ppm" % (tag, n))
            shutil.copyfile(scratch, ev)
    return n, worst


def guest_counters(serial):
    """The last `[wm] perf` line's torn/defer/late/drawmax, or None."""
    try:
        text = open(serial, errors="replace").read()
    except OSError:
        return None
    got = None
    for line in text.splitlines():
        i = line.find("[wm] perf ")
        if i < 0:
            continue
        d = {}
        for tok in line[i + 10:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = int(v)
                except ValueError:
                    pass
        if "torn" in d:
            got = d
    return got


def main(argv):
    xres, yres, rounds = 1280, 800, 4
    iso, keep, negative = None, None, False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":       xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres":     yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--rounds":   rounds = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":      iso = argv[i + 1]; i += 2
        elif argv[i] == "--keep":     keep = argv[i + 1]; i += 2
        elif argv[i] == "--negative": negative = True; i += 1
        else:
            print("unknown arg %r" % argv[i]); return 2
    if negative and iso is None:
        iso = build_negative()
        if iso is None:
            return 2
    if iso is None:
        iso = os.path.join(ROOT, "build", "logit.iso")

    scale = configure(xres, yres)
    tmp = keep or tempfile.mkdtemp(prefix="logit-flash-")
    os.makedirs(tmp, exist_ok=True)
    print("=== %dx%d device px (scale %d%%), %s ===" % (xres, yres, scale, iso))
    print("    hunting a composited frame in which an app's canvas is half drawn")

    qemu, sock, serial = boot(iso, xres, yres, tmp)
    rect = content_rect()
    try:
        time.sleep(6)
        ui = Session(sock, serial=serial)
        launched0 = open(serial, errors="replace").read().count("[wm] launched")
        cx, cy = aim_point(rect)
        got = ui.settle_pointer(os.path.join(tmp, "aim.ppm"), cx, cy)
        if got != (cx, cy):
            print("     pointer would not settle in the Finder (%r, wanted %r)"
                  % (got, (cx, cy)))
            return 2
        time.sleep(1.5)

        base = os.path.join(tmp, "settled.ppm")
        ui.screendump(base, settle=0.6)
        rows = pick_probe(base, rect)
        rich, modal0, frac0 = probe(base, rect, rows)
        print("     settled: %d distinct colours across %d probe rows "
              "(modal %s, %.0f%%)" % (rich, len(rows), modal0, 100 * frac0))
        if rich < 60:
            print("     the settled window has almost no colour in it -- the "
                  "content rect is wrong, or the Finder never painted")
            return 2
        floor = rich // 4
        print("     a frame is TORN if its probe rows hold fewer than %d "
              "colours" % floor)

        overall, frames = None, 0
        for r in range(rounds):
            down = (r % 2 == 0)
            # A 5 cm two-finger swipe, as the machine receives it: a BURST of
            # wheel notches, ~25 ms apart. The spacing is the whole experiment.
            # A macOS trackpad turns one flick into a dozen scroll events in a
            # couple of hundred milliseconds, and an app frame here costs 50-150
            # ms -- so notch k+1 is queued and waiting while the app is still
            # painting notch k, the app never goes idle, and it starts erasing
            # the canvas for the next frame the instant it flushes the last one.
            # Notches a leisurely 120 ms apart let the app finish and yield
            # between them, the compositor gets a complete canvas every time,
            # and the defect does not appear at all -- measured, not assumed.
            sched = [(0.10 + 0.025 * k, lambda d=down: wheel_event(ui, d))
                     for k in range(SWIPE_NOTCHES)]
            # ...then one click. The app repaints for the press AND for the
            # release, and the compositor has the first repaint's damage in hand
            # while the second is clearing the canvas.
            sched += [(1.00, lambda: button(ui, True)),
                      (1.06, lambda: button(ui, False))]
            n, worst = run_round(ui, tmp, rect, rows, floor,
                                 "r%02d" % r, sched, 2.2)
            frames += n
            if overall is None or worst[0] < overall[0]:
                overall = worst
            print("     round %d: %d frames sampled, thinnest %d colours "
                  "(worst so far %d)" % (r, n, worst[0], overall[0]))
            time.sleep(0.8)

        launched = open(serial, errors="replace").read().count("[wm] launched")
        if launched != launched0:
            print("     a window OPENED during the run (%d launches). A window "
                  "that has never been" % (launched - launched0))
            print("     drawn into is legitimately blank, so nothing measured "
                  "after this means anything.")
            return 2

        cols, modal, frac, path = overall
        print("     %d frames examined at ~%d/s" % (frames, int(frames / (rounds * 2.2))))
        print("     thinnest frame: %d colours, modal %s at %.1f%%  [%s]"
              % (cols, modal, 100 * frac, os.path.basename(path)))
        if modal in NAMED:
            print("       modal colour is %s" % NAMED[modal])

        g = guest_counters(serial)
        if g is not None:
            print("     guest's own account: torn=%d composites of a half-drawn "
                  "window, defer=%d rectangles held back, late=%d past the "
                  "deadline, longest app frame %d ms"
                  % (g.get("torn", -1), g.get("defer", -1), g.get("late", -1),
                     g.get("drawmax", -1)))

        torn = cols < floor
        print("")
        if torn:
            print("  TORN FRAME CAPTURED. The screen showed the window with %d "
                  "colours across the" % cols)
            print("  probe rows, where the settled window holds %d. That frame "
                  "is neither the old" % rich)
            print("  picture nor the new one: it is the canvas mid-draw, on "
                  "screen.")
            print("  file: %s" % path)
        else:
            print("  no torn frame: every sampled frame held a painted window "
                  "(thinnest %d, floor %d)" % (cols, floor))
        if g is not None and (g.get("torn", 0) > 0) != torn:
            print("  NOTE: the guest counted %d torn composites and the camera "
                  "%s -- the two" % (g.get("torn", 0),
                                     "caught one" if torn else "caught none"))
            print("  instruments disagree, which is information about the "
                  "sampling rate, not about the defect.")

        if negative:
            if torn:
                print("negative control: FAILED AS REQUIRED")
                return 0
            print("negative control: DID NOT FAIL -- the guard is still in the "
                  "build, so this proves nothing")
            return 1
        return 1 if torn else 0
    finally:
        qemu.kill()
        if keep:
            print("     frames kept in %s" % tmp)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
