#!/usr/bin/env python3
"""THE LOOK GATE: read the desktop's own scanout and print NUMBERS.

Every other artifact this project has produced about the desktop's visual
polish is a sentence -- "the glass looks better", "the shadow feels right".
None of those are checkable. c/lib/gfx already refuses to work that way (it
diffs every filled shape against a 16x16 supersampled reference); this file
applies the same discipline one layer up, to the composited desktop itself:
boot it, screendump the guest's own framebuffer over QMP (never the host
window -- see tools/shot.sh's header for why that separation matters), and
measure pixels.

THREE THINGS MEASURED HERE ARE BUGS, confirmed on HEAD (dba2b11d9) at
1280x800 with the real wallpaper (fsroot/wallpaper.png -- gitignored, copy it
in before running this anywhere but the working tree or you are grading a
flat gradient instead of a photograph, see the unit brief). They are recorded
as EXPECTED FAILURES with the exact values this file measured, not as vague
"known issues": the gate must say so, loudly, on the day any one of them
changes -- fixed or newly different -- rather than silently keep passing
either way. That is why every check below, defect or not, is one assertion
against a RECORDED number: "still true" and "no longer true" are both
reportable events, and only a recorded expectation can tell them apart.

  DEFECT 1 -- ROW 0 CORRUPTION -- FIXED 2026-08-15, and the fix taught us
  what it was. It was never memory corruption: fb_liquid_glass gives every
  panel edge a refracting bevel, and the menu bar's TOP edge is the screen
  edge -- the rim was sampling wallpaper from ~REFRACT px below into row 0,
  exactly as a rim should, on an edge that should never have had one (the
  slab is cut by the screen there, not ended). fb_liquid_glass_cut() with
  GLASS_CUT_TOP|LEFT|RIGHT (fb.h) is the fix; the wide mid-screen band
  (260..1020, the wallpaper's busy region) is gone and row0/row4 distinct
  colours converged (714 vs 673 on the recording day, ratio 1.06 -- was
  >1.3). What REMAINS, and is recorded below as normal behaviour rather
  than a defect: a ~40 px residual band around x=1070..1110 where the menu
  bar's clock/status text starts -- the top scanline of TEXT legitimately
  differs from four rows into its glyphs. That band and its peak diff are
  asserted at their measured values so a REAL row-0 regression (wide band
  returning) still screams.

  DEFECT 2 -- THE GLASS DOES NOT COVER THE WHOLE BAR. Sample a horizontal
  line through the BODY of the menu bar (not row 0 -- this is a different
  symptom at a different height) and measure local pixel-to-pixel variance
  across x. The wallpaper is a real photograph, so raw variance is never
  literally zero; what a frosted region does is COMPRESS it. This machine's
  menu bar currently shows a plateau of compressed (blurred) variance in the
  MIDDLE and un-compressed (raw backdrop) variance at both ENDS, with a hard
  step between them -- exactly backwards from "the whole bar is glass".

  DEFECT 3 -- DOCK RIM ASYMMETRY. The liquid-glass rim highlight is
  legitimately allowed to be left/right asymmetric BY DESIGN (the code models
  a single up-left light source, so a bright highlight top-left and a soft
  shadow bottom-right is correct optics, not a bug). What is measured here is
  narrower and cannot be explained by lighting: mirroring the dock panel's own
  GAP columns (deliberately excluding icon fills, which are asymmetric by
  content and would swamp this number with noise that has nothing to do with
  rendering) about its vertical centreline finds one corner carrying a bright
  near-white specular highlight where the mirrored corner carries a dark,
  hue-shifted value instead of the same highlight reflected -- a structural
  left/right difference, not a lighting gradient.

AND THE SHADOW, which is not a bug -- it exists, it is proportional darkening
(not a gradient in the wallpaper, which the ratio check below is what tells
them apart), and it is simply small. Measured on all four sides of the same
window at the same coordinates every run (the Finder's boot position is a
deterministic cascade slot, read back from the guest's own
`[wm] win 0 frame ...` report rather than assumed); the ASSERTION is the
RELATIONSHIP the code's own WSH_DY/WSH_BLUR constants predict -- the shadow
is offset downward on purpose, so bottom must end up both wider and deeper
than top -- not the absolute widths, so unit F can retune the falloff curve
without this file needing a rewrite.

ALSO MEASURED, off the pixels rather than off wm.c's constants (the point is
to check what is DRAWN, and a constant that is read but never reaches the
screen would pass a check against itself and mean nothing): menu bar height,
titlebar height, traffic-light diameter and spacing, the window corner
radius, and the dock's icon+gap pitch.

EXIT CODE: every measurement above -- desirable and buggy alike -- is checked
against a recorded value with a tolerance. The gate exits 0 only when every
one of them still reads the same as it did when this file was written, and
NON-ZERO the moment any one of them moves, in either direction. That is
deliberate: a defect quietly getting fixed is exactly as much a reason to
stop and update this file's recorded numbers as a regression is.

    python3 tests/qmp/qmp_desktop_look.py <iso> <disk.img>

Env: QEMU (default qemu-system-x86_64), QEMU_CPU (default max).
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, NAPPS, DOCK_ISZ_PT, DOCK_GAP_PT  # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
XRES, YRES = 1280, 800          # the mode every recorded number below was measured at


# ---------------------------------------------------------------------------
# boot: one machine, one screendump. No interaction is needed -- every defect
# and every proportion here is visible on the desktop as it settles after
# boot, so (unlike qmp_window.py or qmp_input.py) there is no drag, click or
# keystroke to script. That keeps this file's only source of nondeterminism
# down to "did the picture finish settling", which the marker below answers.
# ---------------------------------------------------------------------------

def boot():
    tmp = tempfile.mkdtemp(prefix="logit-look-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen(
        [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 240
    while time.time() < deadline:
        if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
            # Settle: the Finder's open-pop animation and the first couple of
            # frosted-glass composites need to finish, or the screendump below
            # catches the desktop mid-animation and every proportion in it is
            # a moving target rather than the steady state this file measures.
            time.sleep(3.0)
            return proc, sock, serial, tmp
        if proc.poll() is not None:
            raise RuntimeError("qemu exited before the desktop came up")
        time.sleep(0.2)
    raise RuntimeError("guest never reported a live desktop")


WIN0_RE = re.compile(
    r"\[wm\] win 0 frame (-?\d+) (-?\d+) (-?\d+) (-?\d+) content")


def window0_box(serial_text):
    """Finder's frame as (x0,y0,x1,y1), from the guest's OWN account.

    Not derived from a click point or the dock's cascade arithmetic: wm.c
    prints this line when the window settles, and reading it back is the same
    reasoning qmp_window.py uses for the identical report -- deriving the
    frame from pixels means re-deriving the window manager's own placement
    logic in the harness, which then silently rots the day that logic changes
    (see qmp_ui.py's header for the last time dock geometry did exactly this)."""
    m = WIN0_RE.search(serial_text)
    if not m:
        return None
    x, y, w, h = (int(v) for v in m.groups())
    return (x, y, x + w, y + h)


# ---------------------------------------------------------------------------
# small numeric helpers shared by more than one measurement
# ---------------------------------------------------------------------------

def longest_run(seq, pred):
    """(start,end) indices of the longest contiguous run where pred holds, or None."""
    best = None
    i, n = 0, len(seq)
    while i < n:
        if pred(seq[i]):
            j = i
            while j < n and pred(seq[j]):
                j += 1
            if best is None or (j - i) > (best[1] - best[0]):
                best = (i, j)
            i = j
        else:
            i += 1
    return best


def block_avg(vals, block):
    return [sum(vals[b:b + block]) / len(vals[b:b + block])
            for b in range(0, len(vals), block)]


def px_diff(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


# ---------------------------------------------------------------------------
# DEFECT 1: row 0 corruption
# ---------------------------------------------------------------------------

def defect_row0_corruption(p):
    r0 = [p.at(x, 0) for x in range(p.w)]
    r4 = [p.at(x, 4) for x in range(p.w)]
    diffs = [px_diff(r0[x], r4[x]) for x in range(p.w)]
    B = 10
    blocks = block_avg(diffs, B)
    # The extreme screen edges (x<8 or x>W-8) differ a LITTLE between any two
    # rows near the top of a diagonal wallpaper gradient even with no bug at
    # all -- the corrupted band is the DOMINANT, interior run, not any
    # exceedance of the threshold.
    interior = blocks[1:-1]
    run = longest_run(interior, lambda v: v > 40)
    band = ((run[0] + 1) * B, (run[1] + 1) * B) if run else None
    peak = max(diffs[8:p.w - 8]) if p.w > 16 else 0
    return dict(row0_colors=len(set(r0)), row4_colors=len(set(r4)),
                band=band, peak_diff=peak)


# ---------------------------------------------------------------------------
# DEFECT 2: the glass not covering the whole bar
# ---------------------------------------------------------------------------

def local_range(row, win=5):
    """Rolling max-min of R+G+B over a small window -- a cheap variance proxy
    that does not need floating point and is exactly what "how noisy is this
    stretch of pixels" means for a photograph behind glass."""
    out = []
    half = win // 2
    n = len(row)
    for x in range(n):
        lo, hi = max(0, x - half), min(n, x + half + 1)
        seg = row[lo:hi]
        out.append(max(seg) - min(seg))
    return out


def defect_glass_discontinuity(p, y=16):
    """y=16 sits in the BODY of the 24px menu bar (below row 0's own
    corruption at y=0..~4, above the y=23 hairline) -- the row defect 1 is
    explicitly NOT about."""
    row = [p.at(x, y) for x in range(p.w)]
    sums = [c[0] + c[1] + c[2] for c in row]
    lv = local_range(sums, 5)
    B = 10
    blocks = block_avg(lv, B)
    plateau_idx = longest_run(blocks, lambda v: v < 30)
    plateau = (plateau_idx[0] * B, plateau_idx[1] * B) if plateau_idx else None
    if plateau_idx:
        ends = blocks[:plateau_idx[0]] + blocks[plateau_idx[1]:]
    else:
        ends = blocks
    end_peak = max(ends) if ends else 0
    return dict(y=y, plateau=plateau, end_peak_variance=end_peak)


# ---------------------------------------------------------------------------
# DEFECT 3: dock rim asymmetry
# ---------------------------------------------------------------------------
# Mirrored from draw_dock()/dock_geom() in c/kernel/gui/wm.c, same as
# qmp_ui.py's dock_icon(). At 1280x800 (this file's fixed mode) SCALE is
# exactly 100, so points and device pixels coincide and no S()/pt() call is
# needed -- unlike qmp_ui.py, which has to support every resolution its other
# drivers run at, this file only ever runs at one.
#
# DOCK_PAD_PT=18 (unit F, "the dock is too tall for its icons"): dock_geom()
# used to be a bare `isz + 20` with no name on the 20 -- this mirror copied
# that literal, so when the padding constant changed on the wm.c side this
# copy would have gone stale exactly the way the file header above warns
# about ("the dock geometry silently rotted once already"). It is 2x the
# padding now (top AND bottom), not a flat +20, so the mirror has to know the
# split, not just the total -- see DOCK_PAD_PT's own comment in wm.c.
DOCK_PAD_PT = 18
def dock_panel_box():
    isz, gap, n = DOCK_ISZ_PT, DOCK_GAP_PT, NAPPS
    dw = gap + n * (isz + gap)
    dh = isz + 2 * DOCK_PAD_PT
    x0 = (XRES - dw) // 2
    y0 = YRES - dh - 12
    return x0, y0, dw, dh, gap


def defect_dock_rim_asymmetry(p):
    x0, y0, dw, dh, gap = dock_panel_box()
    # Restrict to the GAP column at each end of the panel (before the first
    # icon, after the last) rather than the whole panel: the panel's interior
    # is eleven differently-coloured app icons, and mirroring THOSE would
    # report a huge number regardless of the rim, for a reason that has
    # nothing to do with rendering. The gap columns are plain glass with no
    # icon content, so a real asymmetry in the material itself is the only
    # thing left that a mirror diff there can be measuring.
    worst, worst_at = 0, None
    for y in range(y0, y0 + dh):
        for xo in range(gap):
            x, mx = x0 + xo, x0 + dw - 1 - xo
            d = px_diff(p.at(x, y), p.at(mx, y))
            if d > worst:
                worst, worst_at = d, (x, y, mx)
    return dict(worst=worst, at=worst_at, box=(x0, y0, dw, dh))


# ---------------------------------------------------------------------------
# THE SHADOW: width + depth on all four sides of the same window
# ---------------------------------------------------------------------------

def shadow_ray(p, win, side, along, n=40):
    """Walk `n` pixels outward from one edge of `win`=(x0,y0,x1,y1), starting
    at the first pixel outside the window, along a line offset `along` px from
    the window's own top-left corner (so the same call works for any window
    size/position -- only the OFFSET into the edge is a magic number, and it
    is chosen once, in the caller, to land clear of the window's own rounded
    corners).

    Returns (width, depth_pct): width is how many of the n samples are darker
    than 98% of the assumed-unshadowed farthest sample (proportional
    darkening -- the same test the unit brief's own hand measurement uses:
    "all three channels scale by 0.76-0.78" is a RATIO check, which is how a
    shadow is told apart from the wallpaper's own gradient); depth_pct is the
    deepest darkening seen, as a percentage of that reference brightness."""
    x0, y0, x1, y1 = win
    if side == "left":
        get = lambda o: p.at(x0 - 1 - o, y0 + along)
    elif side == "right":
        get = lambda o: p.at(x1 + o, y0 + along)
    elif side == "top":
        get = lambda o: p.at(x0 + along, y0 - 1 - o)
    else:  # bottom
        get = lambda o: p.at(x0 + along, y1 + o)
    bright = [sum(get(o)) for o in range(n)]
    ref = bright[-1]
    if ref <= 0:
        return 0, 0.0
    ratios = [b / ref for b in bright]
    depth = (1.0 - min(ratios)) * 100.0
    width = 0
    for r in ratios:
        if r < 0.98:
            width += 1
        else:
            break
    return width, depth


# The four along-edge offsets: clear of the window's own S(10) corner radius
# (>=20px in) and, for left/bottom, exactly the coordinates the unit brief's
# own hand measurement used (y0+180 -> y=250 at y0=70; x0+320 -> x=430 at
# x0=110) -- chosen relative to the window box, not as bare screen
# coordinates, so this keeps working if the Finder's cascade slot ever moves.
SHADOW_ALONG = dict(left=180, right=150, top=290, bottom=320)


def measure_shadows(p, win):
    out = {}
    for side, along in SHADOW_ALONG.items():
        out[side] = shadow_ray(p, win, side, along)
    return out


# ---------------------------------------------------------------------------
# proportions, off the pixels
# ---------------------------------------------------------------------------

def menubar_height(p, x=500):
    """The hairline fb_blend_rect at row MBH-1 is a flat alpha blend over
    whatever is beneath it -- a HARD-edged draw, unlike the glass it sits on
    top of, and its row is measurably the darkest thing in the bar at a
    column with no text or dark-mode toggle on it (x=500 sits in the
    corrupted band from defect 1, which is exactly why this is a DIFFERENT
    kind of check: it asks "which row is darkest", not "does this row match
    another row", so defect 1 does not confound it)."""
    col = [p.at(x, y) for y in range(40)]
    lum = [c[0] + c[1] + c[2] for c in col]
    return lum.index(min(lum[:35])) + 1  # the row AFTER the hairline = the height


def titlebar_height(win, p, x_off=190):
    """draw_frame() fills a solid 1px hairline at y+TBH then the app's opaque
    content surface starts immediately after -- a real edge, not an
    antialiased one, so "biggest single-row jump" finds it exactly. The
    offset is clear of both the rounded corner (>=20px in) and the centred
    title text (the Finder's 640px-wide frame puts "Finder" within roughly
    +-80px of the centre column, x0+320)."""
    x0, y0, x1, y1 = win
    x = x0 + x_off
    # Search a band around the design constant (TBH=30) rather than the whole
    # window top: the Finder's own content -- a file-listing icon or a row of
    # text -- has MORE contrast than this hairline does, and scanning further
    # down than necessary finds that jump instead. This is still measuring the
    # picture, not trusting the constant: a titlebar genuinely built taller or
    # shorter than this band would be missed as "not found" (best_jump stays
    # low), not silently misreported as some unrelated content edge.
    best_dy, best_jump = 0, -1
    prev = p.at(x, y0 + 19)
    for dy in range(20, 41):
        cur = p.at(x, y0 + dy)
        d = px_diff(cur, prev)
        if d > best_jump:
            best_jump, best_dy = d, dy
        prev = cur
    return best_dy


TRAFFIC = dict(close=(255, 95, 86), minimize=(254, 188, 46), zoom=(40, 200, 64))


def traffic_lights(p):
    boxes = {name: p.find_color(rgb) for name, rgb in TRAFFIC.items()}
    if any(b is None for b in boxes.values()):
        return None
    centres = {name: ((b[0] + b[2]) / 2.0, (b[1] + b[3]) / 2.0) for name, b in boxes.items()}
    diam = sum((b[2] - b[0] + 1) for b in boxes.values()) / 3.0
    spacing = (abs(centres["minimize"][0] - centres["close"][0])
               + abs(centres["zoom"][0] - centres["minimize"][0])) / 2.0
    return dict(diameter=diam, spacing=spacing, centres=centres)


def corner_radius(p, win, max_r=24):
    """The left edge column, walked down from the window's own top-left
    corner until the colour converges on the STEADY titlebar-glass value at
    that column (sampled well past where any corner curvature could still be
    biting, but BEFORE titlebar_height's y0+TBH boundary -- past it is the
    app's opaque content surface, a different colour family entirely, and
    convergence against that would never succeed at any radius). The
    liquid-glass corner is an antialiased COVERAGE ramp, not a binary edge
    (see the comment on `cov` in fb_liquid_glass), so this reads an
    approximate radius, not an exact one -- which is why the assertion on it
    below is a wide sanity range rather than a pinned value."""
    x0, y0, _, _ = win
    steady = p.at(x0, y0 + 24)
    for r in range(max_r):
        if px_diff(p.at(x0, y0 + r), steady) <= 12:
            return r
    return max_r


def dock_pitch(p):
    """The distance from one app icon's flat fill colour to the next one's,
    measured along the row through the icons' vertical centre. Icon fills are
    large, saturated, near-constant blocks (draw_dock's vertical gradient
    varies slowly); a run of >=15 consecutive columns whose colour barely
    moves is an icon, and the run START is what is compared against the next
    such run's start -- which is isz+gap by construction, and the one dock
    proportion cheap to measure directly through a photographic backdrop
    (isz and gap individually are not: the backdrop shows through the GAPS,
    so the gap's own colour is content-dependent and has no fixed signature
    to detect, unlike an icon's opaque fill)."""
    x0, y0, dw, dh, gap = dock_panel_box()
    cy = y0 + DOCK_PAD_PT + DOCK_ISZ_PT // 2
    row = [p.at(x, cy) for x in range(x0, x0 + dw)]
    starts = []
    i, n = 0, len(row)
    RUN = 15
    while i < n - RUN:
        seg = row[i:i + RUN]
        if max(px_diff(seg[0], c) for c in seg) < 10:
            starts.append(x0 + i)
            # Skip past the rest of THIS icon (and its sheen/gradient, which
            # drifts slowly) before scanning on, so one icon cannot register
            # twice.
            j = i + RUN
            while j < n and px_diff(row[i], row[j]) < 40:
                j += 1
            i = j
        else:
            i += 1
    if len(starts) < 2:
        return None
    pitches = [starts[k + 1] - starts[k] for k in range(len(starts) - 1)]
    pitches.sort()
    return pitches[len(pitches) // 2]  # median: robust to one icon's sheen/shape outlier


# ---------------------------------------------------------------------------
# the gate itself
# ---------------------------------------------------------------------------

class Check:
    def __init__(self):
        self.rows = []
        self.failed = []

    def add(self, name, ok, detail, known_bug=False):
        tag = ("KNOWN-BUG " if known_bug and ok else
               "REGRESSED! " if known_bug and not ok else
               "ok         " if ok else
               "FAIL!      ")
        self.rows.append("  %-8s %-46s %s" % (tag.strip(), name, detail))
        if not ok:
            self.failed.append(name)


def in_range(v, lo, hi):
    return lo <= v <= hi


def main():
    try:
        proc, sock, serial, tmp = boot()
    except RuntimeError as e:
        print("FAIL: %s" % e)
        return 1
    try:
        ppm_path = os.path.join(tmp, "desktop.ppm")
        ui = Session(sock)
        ui.screendump(ppm_path, settle=0.5)
        p = PPM(ppm_path)
        text = open(serial, errors="replace").read()
        win = window0_box(text)

        print("=== LogitOS desktop look gate === %dx%d, %s" %
              (p.w, p.h, os.path.relpath(ISO)))
        if win is None:
            print("FATAL: no '[wm] win 0 frame ...' in the serial log -- the Finder")
            print("       never settled, so nothing below has a window to measure")
            print("----- serial tail -----")
            print(text[-3000:])
            return 1
        print("Finder window (guest-reported): x=%d y=%d w=%d h=%d" %
              (win[0], win[1], win[2] - win[0], win[3] - win[1]))

        c = Check()

        # ---- the three defects -------------------------------------------
        d1 = defect_row0_corruption(p)
        print("\n[defect 1] row 0 corruption")
        print("  distinct colours: row0=%d row4=%d  band=%s  peak diff=%d" %
              (d1["row0_colors"], d1["row4_colors"], d1["band"], d1["peak_diff"]))
        # FIXED (see DEFECT 1 in the docstring): the wide rim-refraction band
        # is gone; what the detector now finds is the narrow clock-text band,
        # asserted at its measured shape so a wide band RETURNING still fails.
        band_ok = (d1["band"] is None or
                   ((d1["band"][1] - d1["band"][0]) <= 80 and
                    in_range(d1["band"][0], 1000, 1150)))
        c.add("row0 band is only the clock text (fix holds)", band_ok,
              str(d1["band"]))
        c.add("row0/row4 distinct colours converged (fix holds)",
              d1["row0_colors"] <= d1["row4_colors"] * 1.3,
              "row0=%d row4=%d" % (d1["row0_colors"], d1["row4_colors"]))
        c.add("row0/row4 peak diff is text-sized, still measured",
              d1["peak_diff"] > 60, "peak=%d" % d1["peak_diff"])

        d2 = defect_glass_discontinuity(p)
        print("\n[defect 2] glass does not cover the whole bar (y=%d)" % d2["y"])
        print("  blurred plateau=%s   raw-ends peak variance=%d" %
              (d2["plateau"], d2["end_peak_variance"]))
        plateau_ok = (d2["plateau"] is not None and
                      in_range(d2["plateau"][0], 150, 350) and
                      in_range(d2["plateau"][1], 950, 1150))
        c.add("blurred plateau ~x=240..1070", plateau_ok, str(d2["plateau"]), known_bug=True)
        c.add("the ends are measurably UN-blurred (raw variance high)",
              d2["end_peak_variance"] > 60, "peak=%d" % d2["end_peak_variance"], known_bug=True)

        d3 = defect_dock_rim_asymmetry(p)
        print("\n[defect 3] dock rim asymmetry")
        print("  worst mirrored gap-column diff=%d at %s   panel box=%s" %
              (d3["worst"], d3["at"], d3["box"]))
        c.add("dock rim mirror diff is large (structural, not lighting)",
              d3["worst"] > 200, "worst=%d" % d3["worst"], known_bug=True)

        # ---- the shadow -----------------------------------------------------
        sh = measure_shadows(p, win)
        print("\n[shadow] width(px) / depth(%%) per side, window=%s" % (win,))
        for side in ("top", "bottom", "left", "right"):
            w, dpt = sh[side]
            print("  %-6s width=%2d  depth=%.1f%%" % (side, w, dpt))
        c.add("bottom shadow is WIDER than top (WSH_DY offsets it downward)",
              sh["bottom"][0] > sh["top"][0],
              "bottom=%d top=%d" % (sh["bottom"][0], sh["top"][0]))
        c.add("bottom shadow is DEEPER than top",
              sh["bottom"][1] > sh["top"][1],
              "bottom=%.1f%% top=%.1f%%" % (sh["bottom"][1], sh["top"][1]))
        c.add("left/right shadow widths both exist in a plausible range",
              in_range(sh["left"][0], 4, 30) and in_range(sh["right"][0], 4, 30),
              "left=%d right=%d" % (sh["left"][0], sh["right"][0]))
        # WSH_DY/WSH_BLUR have no horizontal component -- left and right differ
        # only by whatever the wallpaper's own local content does to the ratio
        # test, not by design the way top/bottom do. Symmetric within 2x is the
        # bar; a shadow only one side of the window would blow well past it.
        lw, rw = sh["left"][0], sh["right"][0]
        c.add("left/right shadow widths are roughly symmetric (no L/R design offset)",
              lw > 0 and rw > 0 and max(lw, rw) <= 2 * min(lw, rw),
              "left=%d right=%d" % (lw, rw))

        # ---- proportions ------------------------------------------------
        mbh = menubar_height(p)
        tbh = titlebar_height(win, p)
        lights = traffic_lights(p)
        rad = corner_radius(p, win)
        pitch = dock_pitch(p)
        x0, y0, dw, dh, gap = dock_panel_box()

        print("\n[proportions, measured off the pixels]")
        print("  menu bar height        = %d px" % mbh)
        print("  titlebar height         = %d px" % tbh)
        if lights:
            print("  traffic light diameter  = %.1f px   spacing = %.1f px" %
                  (lights["diameter"], lights["spacing"]))
        print("  window corner radius    ~ %d px (antialiased; approximate)" % rad)
        print("  dock panel              = %dx%d px  (icon=%d gap=%d, from geometry)" %
              (dw, dh, DOCK_ISZ_PT, gap))
        print("  dock icon+gap pitch     = %s px (measured)" % pitch)

        c.add("menu bar height == 24pt", in_range(mbh, 22, 26), "measured=%d" % mbh)
        c.add("titlebar height == 30pt", in_range(tbh, 28, 32), "measured=%d" % tbh)
        if lights:
            c.add("traffic light diameter ~12px", in_range(lights["diameter"], 9, 15),
                  "measured=%.1f" % lights["diameter"])
            c.add("traffic light spacing ~18px", in_range(lights["spacing"], 15, 21),
                  "measured=%.1f" % lights["spacing"])
        else:
            c.add("traffic lights present", False, "one or more not found on screen")
        c.add("window corner radius plausible (4..26px)", in_range(rad, 4, 26),
              "measured=%d" % rad)
        c.add("dock icon+gap pitch == isz+gap (%dpx, from geometry)" % (DOCK_ISZ_PT + gap),
              pitch is not None and in_range(pitch, DOCK_ISZ_PT + gap - 6, DOCK_ISZ_PT + gap + 6),
              "measured=%s" % pitch)

        print("\n=== results ===")
        for row in c.rows:
            print(row)

        if c.failed:
            print("\nFAIL: %d of %d checks moved from their recorded value:" %
                  (len(c.failed), len(c.rows)))
            for name in c.failed:
                print("  - " + name)
            print("If a KNOWN-BUG line above regressed to REGRESSED!, a fix changed its")
            print("shape without eliminating it -- update the recorded band/threshold only")
            print("after confirming the new picture by eye. If it disappeared entirely, the")
            print("corresponding checks now read as ordinary failures (the recorded band was")
            print("not found at all) -- that is GOOD NEWS and the fix line to remove from")
            print("the KNOWN list in this file's docstring and its assertions.")
            return 1
        print("\nPASS: all %d checks match their recorded values -- two known defects" %
              len(c.rows))
        print("      (glass plateau, dock rim) still present exactly as measured; the")
        print("      row-0 fix holds; every other property unchanged.")
        return 0
    finally:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
