#!/usr/bin/env python3
"""THE MOTION GATE: does the desktop actually MOVE, and does it pay for what it
moved?

Two gestures are driven here -- Expose (every window scales out into a pickable
grid) and the dock fly (the yellow traffic light shrinks a window into its dock
icon) -- and each is checked three ways, because "it looks animated" is a
sentence and this tree does not accept sentences about pixels.

  1. GEOMETRY IN MOTION, from the guest's own account. A screendump can show a
     window between two places; it cannot show that the compositor MEANT to put
     it there rather than having drawn it late, once, at a size it will keep.
     So wm.c prints `[wm] anim <what> win N p <progress> rect x y w h alpha A
     home x y w h` on every animated frame, plus the destination cell for every
     window when the grid is built, and this file asserts the hard property:
     a mid-flight rectangle lies STRICTLY BETWEEN the window's own frame and
     the cell it is aimed at, on every axis, and the progress it is stamped
     with increases monotonically. That is what "it animated" actually claims.
     Reading it off the serial rather than out of a screendump is the same
     reasoning the existing `[wm] win N frame ...` report was added for --
     re-deriving the window manager's placement from pixels means reimplementing
     it in the harness, where it then silently rots.

  2. THE PICTURE, at three points in each flight (t=0, mid, settled). Attached
     and looked at, not merely produced: a gate that writes a PNG nobody opens
     has tested the PNG writer.

  3. PARTIAL == FULL, on the machine. After the flights have settled, force
     whole-screen recomposites and require every pixel outside the selected
     window's stateful chrome to remain identical. This is qmp_damage.py's
     instrument pointed at the case that
     makes damage easiest to get wrong: an animation moves a window many times
     with no input event in between, so there is no click or drag to blame the
     leftovers on.

THE NEGATIVE CONTROL. `--negative` builds a kernel whose animation damage
report is empty (wm.c's WM_ANIM_DAMAGE_LIE), while its guest geometry
trace continues to move normally. The mid-flight picture checks must catch that
the motion never reached scanout. Endpoint round trips remain useful for
stacking and cleanup, but are not asked to prove an intermediate frame because
the terminal full repaint is allowed to heal one.

Exit code is non-zero if any check fails (inverted under --negative).

    python3 tests/qmp/qmp_motion.py [--iso build/logit.iso] [--disk build/disk.img]
                                    [--out DIR] [--negative]
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                                                   # noqa: E402

QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
XRES, YRES = 1280, 800

# The three apps launched alongside the Finder, by dock slot. Chosen for SHAPE,
# not for what they do: Expose's whole job is to make windows tellable apart, so
# a grid of four identically sized rectangles would let a broken aspect fit pass
# unnoticed. Clock is small and nearly square, Terminal is wide, TextEdit is
# tall-ish, and the Finder is the largest of the four.
# The four windows are named apps, not slots: LAUNCH_SLOTS = [0, 1, 3] held
# scan_apps indices that moved under it the day the pack list grew. The tiles
# come off the guest's [wm] dock line now (launch_app / dock_icon_of).
LAUNCH_APPS = ["clock", "textedit", "terminal"]
NEGATIVE_COPY_IGNORE = (".git", "build*", "__pycache__", "*.pyc", "rust")


# ---------------------------------------------------------------------------
# boot
# ---------------------------------------------------------------------------

def boot(iso, disk):
    tmp = tempfile.mkdtemp(prefix="logit-motion-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen(
        [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 300
    while time.time() < deadline:
        if os.path.exists(serial) and "desktop live" in read(serial):
            time.sleep(3.0)             # let the Finder's open pop finish
            return proc, sock, serial, tmp
        if proc.poll() is not None:
            raise RuntimeError("qemu exited before the desktop came up")
        time.sleep(0.2)
    raise RuntimeError("guest never reported a live desktop")


def read(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# ---------------------------------------------------------------------------
# the guest's own account of what moved
# ---------------------------------------------------------------------------

ANIM_RE = re.compile(
    r"\[wm\] anim (\w+) win (\d+) p (-?\d+) rect (-?\d+) (-?\d+) (-?\d+) (-?\d+) "
    r"alpha (-?\d+) home (-?\d+) (-?\d+) (-?\d+) (-?\d+)")
DAMAGE_RE = re.compile(
    r"\[wm\] anim (\w+) win (\d+).*? damage from "
    r"(-?\d+) (-?\d+) (-?\d+) (-?\d+) cur "
    r"(-?\d+) (-?\d+) (-?\d+) (-?\d+) sent "
    r"(-?\d+) (-?\d+) (-?\d+) (-?\d+)")
CELL_RE = re.compile(
    r"\[wm\] expose cell (\d+) win (\d+) rect (-?\d+) (-?\d+) (-?\d+) (-?\d+)")
WIN_RE = re.compile(
    r"\[wm\] win (\d+) frame (-?\d+) (-?\d+) (-?\d+) (-?\d+) content .* min (\d+)")


def anim_frames(text, what):
    """Every `[wm] anim <what> ...` line, as dicts, in order."""
    out = []
    for m in ANIM_RE.finditer(text):
        if m.group(1) != what:
            continue
        g = [int(v) for v in m.groups()[1:]]
        out.append(dict(win=g[0], p=g[1], rect=tuple(g[2:6]), alpha=g[6],
                        home=tuple(g[7:11])))
    return out


def animation_damage_records(text):
    """Damage footprints emitted beside the real guest animation trace."""
    out = []
    for m in DAMAGE_RE.finditer(text):
        nums = [int(v) for v in m.groups()[1:]]
        out.append(dict(what=m.group(1), win=nums[0],
                        previous=tuple(nums[1:5]), current=tuple(nums[5:9]),
                        sent=tuple(nums[9:13])))
    return out


def rect_contains(outer, inner):
    return (outer[0] <= inner[0] and outer[1] <= inner[1]
            and outer[2] >= inner[2] and outer[3] >= inner[3])


def cells(text):
    """{win index: (x, y, w, h)} for the most recently built grid."""
    out = {}
    for m in CELL_RE.finditer(text):
        out[int(m.group(2))] = tuple(int(v) for v in m.groups()[2:])
    return out


def windows(text):
    """{win index: (x, y, w, h, minimized)} from the last report of each."""
    out = {}
    for m in WIN_RE.finditer(text):
        g = [int(v) for v in m.groups()]
        out[g[0]] = tuple(g[1:])
    return out


def ordinary_cell_ids(cell_map, ordinary_windows):
    """Expose cell IDs that also have an ordinary `[wm] win` report."""
    return sorted(set(cell_map).intersection(ordinary_windows))


def traces_cover_cells(frames, cell_map):
    """Every published Expose cell has at least one trace record."""
    return bool(cell_map) and set(cell_map).issubset(f["win"] for f in frames)


def progress_is_bounded_monotonic(frames):
    """All observed per-window progress sequences stay inside [0,256] and rise."""
    previous = {}
    if not frames:
        return False
    for frame in frames:
        win, progress = frame["win"], frame["p"]
        if not 0 <= progress <= 256 or progress < previous.get(win, progress):
            return False
        previous[win] = progress
    return True


def rect_center_distance2(rect, point):
    """Squared distance from an integer rectangle centre to a point."""
    x, y, w, h = rect
    dx = x + w // 2 - point[0]
    dy = y + h // 2 - point[1]
    return dx * dx + dy * dy


def window_is_home(report, home):
    """A final `[wm] win` report is restored, unminimized, at exact home geometry."""
    return report is not None and tuple(report[:4]) == tuple(home) and report[4] == 0


def between(v, a, b, slack=2):
    """Is v strictly between a and b (either order), allowing for rounding?

    `slack` is not a fudge factor for a wrong answer: the interpolation is
    integer, so an endpoint can be reproduced exactly at p=0 or p=256 and the
    check below deliberately excludes only the frames that are AT an endpoint.
    It is here so that a value one pixel inside an endpoint still counts as
    'moved', which is what a quantised eased curve produces on its first step."""
    lo, hi = (a, b) if a <= b else (b, a)
    return lo - slack <= v <= hi + slack


def strictly_inside(v, a, b, margin=3):
    """v is between a and b AND not sitting on either end."""
    lo, hi = (a, b) if a <= b else (b, a)
    if hi - lo <= 2 * margin:
        return True                     # the axis barely moves; nothing to prove
    return lo + margin < v < hi - margin


# ---------------------------------------------------------------------------
# PPM -> PNG, so every frame this gate talks about can actually be opened
# ---------------------------------------------------------------------------

def ppm_to_png(ppm, png):
    d = open(ppm, "rb").read()
    i = d.index(b"255\n") + 4
    w, h = (int(x) for x in d[:i].split()[1:3])
    px = d[i:]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    open(png, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))
    return png


class Gate:
    def __init__(self):
        self.rows = []

    def check(self, ok, name, detail=""):
        self.rows.append((bool(ok), name, detail))
        return bool(ok)

    def report(self):
        print("\n=== results ===")
        bad = 0
        for ok, name, detail in self.rows:
            print("  %-4s %-58s %s" % ("ok" if ok else "FAIL", name, detail))
            if not ok:
                bad += 1
        print("")
        if bad:
            print("qmp_motion: %d FAILED of %d" % (bad, len(self.rows)))
        else:
            print("PASS: all %d checks" % len(self.rows))
        return bad


def pixels(path, stride=997):
    """A deterministic sample of a PPM's pixels, as bytes.

    Sampled rather than whole because these are 3 MB files and the question
    asked of them ("is this the same picture") does not need every byte. The
    stride is prime so the sample cannot land on a single column."""
    d = open(path, "rb").read()
    i = d.index(b"255\n") + 4
    px = d[i:]
    return px[::stride]


def frame_diff(a, b):
    """Fraction of sampled bytes that differ between two screendumps."""
    pa, pb = pixels(a), pixels(b)
    n = min(len(pa), len(pb))
    if n == 0:
        return 0.0
    return sum(1 for k in range(n) if pa[k] != pb[k]) / float(n)


def sample_byte_in_rects(byte_offset, top, width, rects):
    """Whether a sampled RGB byte lies in any screen-space half-open rect."""
    pixel = byte_offset // 3
    x = pixel % width
    y = top + pixel // width
    return any(rx <= x < rx + rw and ry <= y < ry + rh
               for rx, ry, rw, rh in rects)


def burst(ses, outdir, tag, n=16, gap=0.0):
    """As many screendumps as fast as QMP will take them.

    NOT a fixed handful. The first frame of either gesture is the expensive one
    -- it is whole-screen, and it renders every moving window into a scratch
    surface at full size before scaling it -- so under TCG a three-shot burst
    fired the instant the guest says the gesture began lands entirely INSIDE
    that first composite and photographs the state before it, three times.
    That is exactly what happened the first time this file ran, and it looked
    like the animation not existing. Sampling across the whole flight and
    picking afterwards is the fix, and it is honest: which frame was mid-flight
    is a question about the frames, so it is answered from the frames."""
    got = []
    for k in range(n):
        p = os.path.join(outdir, "%s_%d.ppm" % (tag, k))
        ses.screendump(p, settle=gap)
        got.append(p)
    return got


def pick_mid(shots, before, after, outdir, tag):
    """The most mid-flight shot: the one least like BOTH endpoints.

    A frame that is merely 'different from the start' could be the settled end
    state; a frame that is different from both endpoints is, necessarily,
    somewhere in between them. Returns (path, score) where score is the smaller
    of the two differences -- so a score near zero means no shot in the burst
    caught the flight at all, and the caller can fail rather than quietly
    attach an endpoint and call it mid-flight."""
    best, best_score = None, -1.0
    for p in shots:
        s = min(frame_diff(p, before), frame_diff(p, after))
        if s > best_score:
            best, best_score = p, s
    if best:
        dst = os.path.join(outdir, tag + "_mid.ppm")
        with open(best, "rb") as fh, open(dst, "wb") as out:
            out.write(fh.read())
        return dst, best_score
    return None, 0.0


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def build_negative():
    """A kernel that reports no damage for a moving window, in a throwaway
    copy of the tree.

    The lie is one line in wm.c, flipped by substitution rather than by a patch
    file, so it cannot silently stop applying when the surrounding code moves --
    the same method, and the same reasoning, as qmp_damage.py's control."""
    import shutil
    tmp = tempfile.mkdtemp(prefix="logit-motion-negctl-")
    dst = os.path.join(tmp, "tree")
    print("     copying the tree to %s ..." % dst)
    shutil.copytree(ROOT, dst, ignore=shutil.ignore_patterns(*NEGATIVE_COPY_IGNORE))
    src_rust = os.path.join(ROOT, "rust")
    if os.path.isdir(src_rust):
        shutil.copytree(src_rust, os.path.join(dst, "rust"))
    wm = os.path.join(dst, "c/kernel/gui/wm.c")
    text = open(wm).read()
    needle = "#define WM_ANIM_DAMAGE_LIE 0"
    if needle not in text:
        print("FAIL wm.c no longer has %r -- the control cannot be built" % needle)
        return None
    open(wm, "w").write(text.replace(needle, "#define WM_ANIM_DAMAGE_LIE 1"))
    print("     building the lying kernel ...")
    r = subprocess.run(["make", "-j", str(os.cpu_count() or 4)],
                       cwd=dst, capture_output=True, text=True)
    iso = os.path.join(dst, "build", "logit.iso")
    if r.returncode != 0 or not os.path.exists(iso):
        print(r.stdout[-3000:])
        print(r.stderr[-3000:])
        return None
    return iso


def main(argv):
    iso, disk = "build/logit.iso", "build/disk.img"
    outdir = "/tmp/mo/M"
    negative = False
    i = 1
    while i < len(argv):
        if argv[i] == "--iso":    iso = argv[i + 1]; i += 2
        elif argv[i] == "--disk": disk = argv[i + 1]; i += 2
        elif argv[i] == "--out":  outdir = argv[i + 1]; i += 2
        elif argv[i] == "--negative": negative = True; i += 1
        else:
            print("unknown arg %r" % argv[i]); return 2
    os.makedirs(outdir, exist_ok=True)
    g = Gate()

    if negative:
        print("=== negative control: a kernel that under-reports animation damage ===")
        iso = build_negative()
        if iso is None:
            print("qmp_motion: could not build the negative control")
            return 1

    pixel_checks = []
    damage_contract_ok = False

    proc, sock, serial, tmp = boot(iso, disk)
    try:
        ses = qmp_ui.Session(sock, serial=serial)

        def key_super(qcode):
            """Cmd + <key>. qmp_ui has shift but no super, and the WM's whole
            shortcut table is Cmd-modified -- see wm_shortcut."""
            ses._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"}, "down": True}},
                        {"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}])
            ses._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}},
                        {"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"}, "down": False}}])
            time.sleep(0.15)

        def diff_below_menubar(a, b, top=24, stride=13, ignore_rects=()):
            """Differing sampled pixels BELOW the menu bar.

            The bar is excluded because its clock shows SECONDS: two frames a
            gesture apart are entitled to differ there, and a comparison that
            called that a failure would be measuring the clock. `ignore_rects`
            removes the picked window's original frame: its focus outline and
            traffic-light state can legitimately change after a pick/round trip,
            and neither is evidence about compositing correctness around it."""
            pa, pb = open(a, "rb").read(), open(b, "rb").read()
            ia, ib = pa.index(b"255\n") + 4, pb.index(b"255\n") + 4
            off = top * XRES * 3
            xa, xb = pa[ia + off:], pb[ib + off:]
            n = min(len(xa), len(xb))
            return sum(1 for k in range(0, n, stride)
                       if not sample_byte_in_rects(k, top, XRES, ignore_rects)
                       and xa[k] != xb[k])

        def full_recomposite():
            """Force a full repaint without changing the final desktop state."""
            key_super("e")
            time.sleep(2.5)
            ses.key("esc")
            time.sleep(3.0)

        # Somewhere that magnifies no dock icon, so the dock is the same picture
        # in every photograph taken for comparison.
        neutral = (XRES - 40, YRES // 2)

        # -------------------------------------------------------------- setup
        # Finder contributes the desktop-layer cell; these launches contribute
        # three ordinary windows of different shapes -- see LAUNCH_APPS.
        #
        # WAIT ON THE FACT, not on a clock. Every sleep here used to be a fixed
        # two seconds, and under TCG on a loaded host that is sometimes not
        # enough -- the run then proceeds with zero windows and every later
        # check fails for a reason that has nothing to do with what is being
        # tested. Finder is a desktop layer and deliberately has no ordinary
        # `[wm] win` report. Each dock launch below adds one reported window, so
        # the honest waits are one, two and three -- never a boot-time Finder.
        def wait_windows(n, timeout=90.0):
            end = time.time() + timeout
            while time.time() < end:
                w = {k: v for k, v in windows(read(serial)).items() if v[2] > 0}
                if len(w) >= n:
                    return w
                time.sleep(0.3)
            return {k: v for k, v in windows(read(serial)).items() if v[2] > 0}

        for k, name in enumerate(LAUNCH_APPS):
            # Every tile here is a first launch. A click aimed at the WRONG tile
            # names that app and fails in launch_app before the four-cell Expose
            # geometry below can measure nonsense.
            ses.launch_app(name)
            live = wait_windows(k + 1)
        time.sleep(2.0)                       # let the last open pop settle
        wins = windows(read(serial))
        live = {k: v for k, v in wins.items() if v[2] > 0}
        print("[setup] %d windows: %s" % (len(live), sorted(live)))
        g.check(len(live) >= 3, "at least three windows are open to expose",
                "%d open" % len(live))

        ppm = os.path.join(outdir, "probe.ppm")
        ses.screendump(os.path.join(outdir, "expose_t0.ppm"), settle=0.4)

        # ============================================================ EXPOSE
        # The HOT CORNER, which is the primary trigger: park the pointer in the
        # top-right and leave it there. The pointer clamps at (W-1, 0), so this
        # is reachable by shoving rather than by aiming -- which is the point of
        # a hot corner.
        mark = len(read(serial))
        ses.goto(XRES - 1, 0, settle=0.0)
        deadline = time.time() + 5.0
        fired = False
        while time.time() < deadline:
            if "[wm] expose on" in read(serial)[mark:]:
                fired = True
                break
            time.sleep(0.004)
        g.check(fired, "the hot corner opened Expose (pointer parked, no click)")
        # Sync on the first frame that actually MOVED something, not on the
        # gesture starting. `expose on` is printed before the first composite,
        # and that composite is the expensive one -- whole-screen, with every
        # window scaled -- so a burst fired at the announcement photographs the
        # picture from before it, repeatedly. Under TCG a whole-screen frame is
        # ~114 ms (the guest's own perf line says so), against 180 ms of flight,
        # so the window in which a mid-flight picture EXISTS is a couple of
        # frames wide and has to be aimed at.
        moved_re = re.compile(r"\[wm\] anim expose win \d+ p ([1-9]\d*)")
        end = time.time() + 3.0
        while time.time() < end:
            if moved_re.search(read(serial)[mark:]):
                break
            time.sleep(0.004)
        shots = burst(ses, outdir, "expose_shot", n=30, gap=0.02)
        time.sleep(2.0)
        settled = os.path.join(outdir, "expose_settled.ppm")
        ses.screendump(settled, settle=0.5)
        midpath, midscore = pick_mid(shots, os.path.join(outdir, "expose_t0.ppm"),
                                     settled, outdir, "expose")
        expose_picture_ok = g.check(
            midscore > 0.02,
            "a frame was caught mid-flight, unlike BOTH endpoints",
            "differs from start and end by >= %.1f%% of sampled pixels"
            % (midscore * 100))
        pixel_checks.append(expose_picture_ok)

        text = read(serial)[mark:]
        cel = cells(text)
        frames = anim_frames(text, "expose")
        g.check(len(cel) >= 3, "the grid published a cell for every window",
                "%d cells" % len(cel))
        g.check(traces_cover_cells(frames, cel),
                "the flight traced every moving cell at least once",
                "%d traced frames for IDs %s"
                % (len(frames), sorted({f["win"] for f in frames})))

        # THE CLAIM: every traced rectangle lies between the window's own frame
        # and the cell it is aimed at.
        bad = []
        for f in frames:
            c = cel.get(f["win"])
            if c is None:
                continue
            hx, hy, hw, hh = f["home"]
            rx, ry, rw, rh = f["rect"]
            if not (between(rx, hx, c[0]) and between(ry, hy, c[1])
                    and between(rw, hw, c[2]) and between(rh, hh, c[3])):
                bad.append((f["win"], f["p"], f["rect"], f["home"], c))
        g.check(not bad, "every animated rect is between the frame and its cell",
                "%d outside" % len(bad) if bad else "%d frames checked" % len(frames))

        # ...and at least one of them is genuinely PART WAY, on both axes. A
        # flight that jumped straight to the destination would satisfy the
        # "between" check above on every frame and still not be an animation.
        moving = [f for f in frames
                  if f["win"] in cel
                  and strictly_inside(f["rect"][2], f["home"][2], cel[f["win"]][2])
                  and strictly_inside(f["rect"][3], f["home"][3], cel[f["win"]][3])]
        g.check(len(moving) >= 3,
                "windows were caught mid-flight, neither at home nor in the cell",
                "%d intermediate frames" % len(moving))

        # TCG scheduling controls how many samples fit inside 180 ms, so fixed
        # sample counts are not correctness. Check every observed per-window
        # sequence; the mid-flight pixels and geometry above separately prove
        # this was an interpolation rather than a cut.
        g.check(progress_is_bounded_monotonic(frames),
                "every observed easing sequence is monotonic and bounded",
                "; ".join("win %d p=%s" % (wi, [f["p"] for f in frames
                                                   if f["win"] == wi])
                          for wi in sorted({f["win"] for f in frames})))

        # Scaled DOWN, and aspect preserved. A cell is not allowed to magnify a
        # window past 1:1 or to squash it: the picker's whole job is to keep
        # windows recognisable.
        aspect_bad = []
        for wi, c in cel.items():
            hw = wins.get(wi, (0, 0, 0, 0))[2]
            hh = wins.get(wi, (0, 0, 0, 0))[3]
            if hw <= 0 or hh <= 0:
                continue
            if c[2] > hw + 1 or c[3] > hh + 1:
                aspect_bad.append(("magnified", wi, (hw, hh), c[2:]))
                continue
            # w/h ratio preserved to within a pixel of integer scaling
            if abs(c[2] * hh - c[3] * hw) > max(hw, hh):
                aspect_bad.append(("aspect", wi, (hw, hh), c[2:]))
        g.check(not aspect_bad, "every thumbnail is scaled down and keeps its aspect",
                str(aspect_bad) if aspect_bad else "%d cells" % len(cel))

        # PICK a window: click its cell. The mode must leave, and the picked
        # window must end up on top.
        # Finder's desktop layer can legitimately be cell/window ID 0, but it
        # has no ordinary frame and therefore no yellow traffic light. Choose a
        # minimise target only from cells for which WIN_RE published geometry.
        # Refuse an empty intersection here, while both dicts are still intact,
        # instead of indexing wins[target] later and turning a gate failure into
        # an unlabelled KeyError.
        ordinary_cells = ordinary_cell_ids(cel, wins)
        if not ordinary_cells:
            g.check(False, "an ordinary Expose cell existed to pick and minimise",
                    "cell IDs %s; ordinary window IDs %s"
                    % (sorted(cel), sorted(wins)))
            g.report()
            return 1
        target = ordinary_cells[0]
        target_home = tuple(wins[target][:4])
        c = cel[target]
        mark2 = len(read(serial))
        ses.click_at(c[0] + c[2] // 2, c[1] + c[3] // 2, settle=0.4)
        time.sleep(2.0)
        t2 = read(serial)[mark2:]
        g.check("expose off pick=%d" % target in t2,
                "clicking a thumbnail picks that window and leaves the mode",
                "picked %d" % target)
        back = anim_frames(t2, "unexpose")
        g.check(traces_cover_cells(back, cel),
                "leaving traces every moving cell too",
                "%d traced frames for IDs %s"
                % (len(back), sorted({f["win"] for f in back})))
        ses.screendump(os.path.join(outdir, "expose_picked.ppm"), settle=0.5)

        # ======================================================== DOCK FLY
        wins = windows(read(serial))
        # The window that was picked is focused and on top; minimise it with its
        # own yellow traffic light, at the coordinates draw_frame paints it.
        fx, fy = wins[target][0], wins[target][1]
        mark3 = len(read(serial))
        ses.screendump(os.path.join(outdir, "min_t0.ppm"), settle=0.4)
        before_aim = dict(wins)
        ses.goto(fx + 34, fy + 15, settle=0.15)
        after_aim = windows(read(serial))
        aim_moved = [wi for wi, report in before_aim.items()
                     if after_aim.get(wi) != report]
        g.check(not aim_moved,
                "aiming at the picked window cannot drag one behind it",
                "unchanged IDs %s" % sorted(before_aim)
                if not aim_moved else "moved IDs %s" % aim_moved)
        ses.click(hold=0.02)
        minshots = burst(ses, outdir, "min_shot")
        time.sleep(2.0)
        minset = os.path.join(outdir, "min_settled.ppm")
        ses.screendump(minset, settle=0.5)
        mmid, mscore = pick_mid(minshots, os.path.join(outdir, "min_t0.ppm"),
                                minset, outdir, "min")
        min_picture_ok = g.check(
            mscore > 0.02,
            "the dock fly was caught mid-flight, unlike BOTH endpoints",
            "differs from start and end by >= %.1f%% of sampled pixels"
            % (mscore * 100))
        pixel_checks.append(min_picture_ok)

        t3 = read(serial)[mark3:]
        mf = [f for f in anim_frames(t3, "min") if f["win"] == target]
        # One genuinely intermediate guest position plus the mid-flight
        # screendump and final state refute a cut without pricing the number of
        # frames a 500 ms TCG composite can fit inside a 220 ms animation.
        g.check(any(0 < f["p"] < 256 for f in mf),
                "the minimise exposed an intermediate in-flight position",
                "%d traced frames" % len(mf))
        if mf:
            first = mf[0]
            g.check(any(f["rect"][2] < f["home"][2]
                        and f["rect"][3] < f["home"][3] for f in mf),
                    "the window shrank on its way to the dock",
                    "home %dx%d; samples %s"
                    % (first["home"][2], first["home"][3],
                       [(f["rect"][2], f["rect"][3]) for f in mf]))
            def expected_min_alpha(progress):
                return 255 if progress <= 128 else 255 - 255 * (progress - 128) // 128
            alpha_bad = [f for f in mf
                         if f["alpha"] != expected_min_alpha(f["p"])]
            g.check(not alpha_bad,
                    "every sampled frame follows the second-half fade curve",
                    "p/alpha %s" % [(f["p"], f["alpha"]) for f in mf])
            # The terminal frame clears min_t0 before tracing, so the last trace
            # need not already be on the icon. It must have moved closer; the
            # following min=1 report proves that the untraced terminal state
            # then landed.
            dx, dy = ses.dock_icon_of("clock")
            home_d2 = rect_center_distance2(first["home"], (dx, dy))
            nearest_d2 = min(rect_center_distance2(f["rect"], (dx, dy)) for f in mf)
            g.check(nearest_d2 < home_d2,
                    "the traced flight moves closer to its dock icon",
                    "distance-squared %d -> %d" % (home_d2, nearest_d2))
        wins2 = windows(read(serial))
        g.check(wins2.get(target, (0, 0, 0, 0, 0))[4] == 1,
                "the window reports itself minimised once it lands")

        # Inspect the state while the target is still minimised. A shrinking
        # rectangle must erase every larger rectangle that preceded it. The
        # deliberately lying build damages only the new, smaller footprint and
        # therefore leaves a trail until a full repaint. Restoring the window
        # first would cover that trail again and make the control invisible.
        ses.goto(*neutral, settle=0.8)
        min_partial = os.path.join(outdir, "min_partial.ppm")
        ses.screendump(min_partial, settle=0.8)
        full_recomposite()
        ses.goto(*neutral, settle=0.8)
        min_full = os.path.join(outdir, "min_full.ppm")
        ses.screendump(min_full, settle=0.8)
        dmin = diff_below_menubar(min_partial, min_full)
        pixel_checks.append(g.check(
            dmin == 0,
            "settled minimise partial == forced full repaint",
            "%d differing sampled px" % dmin))

        # RESTORE: the dock icon flies it back out.
        mark4 = len(read(serial))
        # The last trace is deliberately not an endpoint: TCG may sample the
        # flight at p=123 and then land it without another trace. Its centre is
        # therefore NOT the dock icon. The registry-published icon geometry is
        # the one coordinate shared by paint and hit-testing.
        slot_x, slot_y = ses.dock_icon_of("clock")
        ses.goto(slot_x, slot_y, settle=0.2)
        ses.click(hold=0.02)
        rshots = burst(ses, outdir, "restore_shot", n=12)
        time.sleep(2.5)
        rset = os.path.join(outdir, "restore_settled.ppm")
        ses.screendump(rset, settle=0.5)
        rmidp, rscore = pick_mid(rshots, minset, rset, outdir, "restore")
        # The Clock is only 8% of the screen and the first sampled restore can
        # still be very close to the 50x50 dock endpoint. A 2% whole-screen
        # threshold rejects a real early frame under TCG. The guest trace and
        # exact final geometry below establish which transition it belongs to;
        # 0.2% keeps this picture check above the menu clock's few pixels.
        restore_picture_ok = g.check(
            rscore > 0.002, "the restore was caught mid-flight too",
            "%.1f%%" % (rscore * 100))
        pixel_checks.append(restore_picture_ok)
        t4 = read(serial)[mark4:]
        rf = [f for f in anim_frames(t4, "restore") if f["win"] == target]
        # The mid-flight screendump above supplies the visual intermediate; one
        # guest trace supplies interpolated geometry on a heavily loaded TCG.
        g.check(len(rf) >= 1, "clicking the dock icon starts a traced restore",
                "%d traced frames" % len(rf))
        restored = windows(t4).get(target)
        g.check(window_is_home(restored, target_home),
                "the restore settles at the original frame and clears minimised",
                "home %s; final %s" % (target_home, restored))

        # ============================== PARTIAL == FULL, after a flight
        #
        # THE CHECK THE NEGATIVE CONTROL IS BUILT FOR, and the only one here
        # that can see the mistake it makes. Everything above compares the
        # desktop before a gesture with the desktop after it, and a flight ends
        # with a deliberate whole-screen repaint (see wm_anim_tick) -- so a
        # per-frame damage bug is HEALED before any of those checks look. The
        # smear exists only while the flight is running and for as long as
        # nothing repaints over it.
        #
        # So: photograph the settled desktop, force whole-screen recomposites,
        # photograph again, and require the pixels outside the picked window's
        # original frame to match. Focus outlines and traffic-light state inside
        # that frame may legitimately change and are not compositor correctness.
        # A partial compositor's invariant is that `back` holds a correct
        # composite of the WHOLE screen when a frame returns; this asks that
        # question directly. The full repaint is forced with an Expose round
        # trip, which begins and ends with dirty_full() and changes nothing
        # else -- no theme flip, no z-order change, nothing to undo.
        ses.goto(*neutral, settle=0.8)
        pf_a = os.path.join(outdir, "partial.ppm")
        ses.screendump(pf_a, settle=0.8)
        full_recomposite()
        ses.goto(*neutral, settle=0.8)
        pf_b = os.path.join(outdir, "full.ppm")
        ses.screendump(pf_b, settle=0.8)
        d = diff_below_menubar(pf_a, pf_b, ignore_rects=(target_home,))
        pixel_checks.append(g.check(
            d == 0,
            "after the flights, partial == full outside the picked window",
            "%d differing sampled px" % d))

        # ================================ the other two ways in, and two ways out
        #
        # THE ROUND TRIP IS THE ASSERTION. "Leaves the stacking unchanged" and
        # "leaves no stale pixels" are the same claim about the same picture, so
        # they are checked as one: photograph the desktop, open the picker, come
        # back out without picking anything, photograph again, and require the
        # pixels outside the picked window's original frame to be identical.
        # That is the instrument qmp_damage.py uses for
        # drags and scrolls, pointed at a gesture that moves every window at
        # once -- which is the case most able to leave something behind.
        #
        # The menu bar remains excluded because its clock displays SECONDS. The
        # picked window is excluded because focus/traffic-light redraw state is
        # allowed to differ; stale pixels anywhere around it still fail.
        for tag, dismiss in (("chord", lambda: key_super("e")),
                             ("escape", lambda: ses.key("esc")),
                             ("wallpaper", lambda: ses.click_at(12, 40, settle=0.3))):
            ses.goto(*neutral, settle=0.6)
            before = os.path.join(outdir, "trip_%s_before.ppm" % tag)
            ses.screendump(before, settle=0.6)
            m = len(read(serial))
            key_super("e")                      # the CHORD opens it, every time
            end = time.time() + 6.0
            opened = False
            while time.time() < end:
                if "[wm] expose on" in read(serial)[m:]:
                    opened = True
                    break
                time.sleep(0.01)
            g.check(opened, "Cmd+E opens the picker (%s round trip)" % tag)
            time.sleep(2.5)
            dismiss()
            time.sleep(3.0)
            after = os.path.join(outdir, "trip_%s_after.ppm" % tag)
            ses.goto(*neutral, settle=0.6)
            ses.screendump(after, settle=0.6)
            g.check("[wm] expose off pick=-1" in read(serial)[m:],
                    "%s leaves the picker WITHOUT picking anything" % tag)
            d = diff_below_menubar(before, after, ignore_rects=(target_home,))
            pixel_checks.append(g.check(
                d == 0,
                "%s: pixels outside the picked window return identical" % tag,
                "%d differing sampled px" % d))

        # The scanout pictures prove that users see intermediate frames and no
        # residue. This record proves the exact partial-compositor obligation
        # that makes those frames safe: every submitted animation rectangle
        # contains both the footprint from the previous frame and the current
        # one. It is emitted by the same function immediately before
        # dirty_rect(), so the negative build can mutate that function while
        # leaving geometry traces and endpoint full repaints intact.
        damage_records = animation_damage_records(read(serial))
        damage_bad = [d for d in damage_records
                      if not (rect_contains(d["sent"], d["previous"])
                              and rect_contains(d["sent"], d["current"]))]
        damage_modes = {d["what"] for d in damage_records}
        damage_contract_ok = g.check(
            not damage_bad
            and {"expose", "unexpose", "min", "restore"}.issubset(damage_modes),
            "every animation damage covers its previous and current footprint",
            "%d records across %s"
            % (len(damage_records), sorted(damage_modes))
            if not damage_bad else "%d under-reported records" % len(damage_bad))

        # ===================================================== the damage bill
        # Every animated frame between the two full ones must cost less than the
        # whole screen. The kernel's own counters say so; see wm_perf_report.
        perf = re.findall(r"\[wm\] perf .*?full=(\d+) rects=(\d+) cpx=(\d+) fpx=(\d+)",
                          read(serial))
        g.check(len(perf) >= 2, "the compositor reported its damage counters",
                "%d reports" % len(perf))

        # ------------------------------------------------------------- images
        pngs = []
        for name in ("expose_t0", "expose_mid", "expose_settled", "expose_picked",
                     "min_t0", "min_mid", "min_settled",
                     "restore_mid", "restore_settled"):
            p = os.path.join(outdir, name + ".ppm")
            if os.path.exists(p) and os.path.getsize(p) > 0:
                pngs.append(ppm_to_png(p, os.path.join(outdir, name + ".png")))
        print("\n[frames] %d PNGs in %s" % (len(pngs), outdir))
        for p in pngs:
            print("   " + p)
        # The burst PPMs are scratch and there are ~44 of them at 3 MB each.
        for junk in os.listdir(outdir):
            if "_shot_" in junk and junk.endswith(".ppm"):
                os.remove(os.path.join(outdir, junk))
        _ = ppm, midpath, mmid, rmidp, live
    finally:
        try:
            ses.cmd({"execute": "quit"})
        except Exception:
            pass
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
        print("\n[serial] " + serial)

    bad = g.report()
    if negative:
        # THE CONTROL'S VERDICT IS INVERTED. At least one load-bearing picture
        # or exact guest damage-contract check must fail against the mutation.
        # Endpoint round trips may still pass because terminal full repaints are
        # deliberately allowed to heal them.
        control_checks = pixel_checks + [damage_contract_ok]
        n_pass = sum(1 for c in control_checks if c)
        print("\n=== negative control ===")
        print("  %d of %d visual/damage checks still passed against the lying kernel"
              % (n_pass, len(control_checks)))
        if not control_checks:
            print("  FAIL: no control checks ran at all")
            return 1
        if n_pass == len(control_checks):
            print("  FAIL: the lie changed nothing they could see -- these checks\n"
                  "        do not police the animation's damage reporting")
            return 1
        print("  PASS: the control is detected (%d checks caught it)"
              % (len(control_checks) - n_pass))
        return 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
