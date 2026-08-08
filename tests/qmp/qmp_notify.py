#!/usr/bin/env python3
"""Notifications, in pixels.

This is a visual feature, so a claim about it that is not a screenshot is not a
claim. Every check here photographs what it asserts and leaves the frame behind
in a directory it prints at the end.

Six checks, each a separate claim:

  appear   a notification is drawn where the design says it is (top-right, under
           the menu bar) and is GONE on its own a few seconds later. Transience
           is the whole point of the presentation; a card that stays is a modal
           with a nicer shape.

  stack    seven raised at once: exactly NOTIFY_VISIBLE are on screen, at the
           three slot positions, and the kernel's own log says showing=3 with
           the rest queued -- nothing dropped, nothing overwritten. The design
           is stated in include/abi/logit_abi.h and this is it happening.

  focus    THE ONE THAT MATTERS MOST. Type into a window, raise a notification
           in the middle of the typing, keep typing. Every keystroke must still
           land in the window. A notification that takes focus is not a
           notification, and this is the only way to find out.

  dismiss  click a card: it goes, and the one below it moves up into its place.

  stale    THE DAMAGE CHECK, and the one the negative control breaks. Photograph
           the idle desktop; raise a notification; let it expire; photograph
           again. ZERO differing pixels. The compositor has no periodic full
           repaint to cover for an overlay that under-reports what it drew, so
           any pixel the card touched and did not report stays on screen
           forever -- and shows up here as a difference.

  cost     what it costs, from the compositor's OWN counters, so that "it
           appears rather than sliding in" is a decision with a number behind
           it. Build with NOTIFYANIM=1 and run this again to see the other one.

Notifications are raised from the SERIAL console's /bin/sh -- a completely
different process from the GUI app being typed into, which is what makes the
focus check mean anything.

Usage:
    tests/qmp/qmp_notify.py [--iso PATH] [--disk PATH] [--xres W] [--yres H]
                            [--only NAME[,NAME...]] [--keep]
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import PPM, Session, configure, dock_icon, pt   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TEXTEDIT_SLOT = 1

# draw_frame() in c/kernel/gui/wm.c: only the FOCUSED window paints its close
# button in this colour, so its bounding box is both "which window has focus"
# and "where is it". Same constant qmp_repaint.py uses, for the same reason --
# and the reason c/kernel/gui/notify.c deliberately does NOT use it for an
# error card (a second source of these pixels would break both drivers).
CLOSE_RGB = (255, 95, 86)

# c/kernel/gui/notify.c: the level tile's colour, exactly. An opaque saturated
# square is the one thing in a card a screenshot can look for without knowing
# anything about the font -- and these are checked against the source below, so
# the driver cannot quietly go on hunting for a colour nobody draws any more.
TILE_INFO  = (64, 148, 255)
TILE_WARN  = (255, 172, 56)
TILE_ERROR = (255, 76, 92)

# ...the geometry too, from the same file.
CARD_W, CARD_H, CARD_GAP, CARD_MARGIN, CARD_TOP = 340, 74, 10, 14, 36
NOTIFY_VISIBLE = 3
NOTIFY_MS = 4000


def check_constants():
    """The numbers above, against the file they came from.

    A pixel driver that hunts for a colour the kernel stopped drawing does not
    fail -- it finds nothing and reports the feature missing, which is a
    different bug from the one it would be blamed for. Ten lines of grep is the
    price of never debugging that."""
    src = open(os.path.join(ROOT, "c/kernel/gui/notify.c")).read()
    want = [("*r =  64; *g = 148; *b = 255", TILE_INFO),
            ("*r = 255; *g = 172; *b =  56", TILE_WARN),
            ("*r = 255; *g =  76; *b =  92", TILE_ERROR)]
    for text, rgb in want:
        if text not in src:
            raise SystemExit("qmp_notify: notify.c no longer draws %r; this driver is stale" % (rgb,))
    for name, val in (("CARD_W", CARD_W), ("CARD_H", CARD_H), ("CARD_GAP", CARD_GAP),
                      ("CARD_MARGIN", CARD_MARGIN), ("TILE", 38)):
        if "#define %-11s %d" % (name, val) not in src and "#define %s %d" % (name, val) not in src:
            raise SystemExit("qmp_notify: notify.c's %s moved; this driver is stale" % name)


def card_box(i, screen_w):
    """Slot `i`'s rectangle in device pixels -- notify.c card_box()."""
    w, h = pt(CARD_W), pt(CARD_H)
    x = screen_w - w - pt(CARD_MARGIN)
    y = pt(CARD_TOP) + i * (h + pt(CARD_GAP))
    return x, y, w, h


# ---------------------------------------------------------------------------
# pixels

def find_in(ppm, rgb, box):
    """Bounding box + count of pixels exactly `rgb` inside `box`, or (0, None).

    Region-limited on purpose: an exact colour match over the whole screen can
    collide with the wallpaper gradient, and a check that occasionally finds a
    notification in the wallpaper is worse than no check."""
    x0b, y0b, x1b, y1b = box
    target = bytes(rgb)
    n = 0
    bx0 = by0 = 1 << 30
    bx1 = by1 = -1
    row = ppm.w * 3
    for y in range(max(0, y0b), min(ppm.h, y1b)):
        base = y * row
        start = max(0, x0b) * 3
        end = min(ppm.w, x1b) * 3
        while True:
            k = ppm.px.find(target, base + start, base + end)
            if k < 0:
                break
            off = k - base
            if off % 3:
                start = off + 1
                continue
            x = off // 3
            n += 1
            if x < bx0: bx0 = x
            if x > bx1: bx1 = x
            if y < by0: by0 = y
            if y > by1: by1 = y
            start = off + 3
    return n, (None if bx1 < 0 else (bx0, by0, bx1, by1))


def diff_pixels(a, b, skip_top):
    """Count and bound the pixels that differ, below `skip_top`.

    skip_top exists for the menu-bar clock, which changes twice a second and is
    nothing to do with this. Same shape as qmp_damage.py's, deliberately."""
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


def ink_in(ppm, box, bg_tol=28):
    """Pixels in `box` that are far from the window's own background.

    Counting ink is how "did the keystroke arrive" is answered without teaching
    this driver to read the font: a character drawn is ink added, and the count
    is monotonic in the number of characters. The background is sampled from the
    box's own top-left corner rather than assumed, so it works in both themes."""
    x0, y0, x1, y1 = box
    br, bgc, bb = ppm.at(x0 + 2, y0 + 2)
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b = ppm.at(x, y)
            if abs(r - br) + abs(g - bgc) + abs(b - bb) > bg_tol:
                n += 1
    return n


# ---------------------------------------------------------------------------
# the machine

class Guest:
    """QEMU with a QMP socket AND a serial console this driver can TYPE into.

    qmp_repaint.boot() gives the serial as a log file, which is read-only -- and
    every notification here is raised from the serial /bin/sh, by a process that
    is not the GUI app under test, which is what makes the focus check mean
    anything.

    The console is `-serial stdio` with stdin held open as a PIPE, which is the
    exact shape tests/boot/run-shell-test.sh has always used. A `-chardev
    socket` was tried first and is not equivalent: the guest received the first
    write and then nothing -- not the trailing newline of it, and none of the
    three commands sent afterwards. Bytes written to a socket chardev after the
    front end has back-pressured are dropped rather than queued. The pipe does
    not have that behaviour, and it is already the tested path."""

    def __init__(self, iso, disk, xres, yres, tmp):
        self.tmp = tmp
        self.sock = os.path.join(tmp, "qmp.sock")
        self.serial = os.path.join(tmp, "serial.log")
        self.serfh = open(self.serial, "wb")
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64",
             "-cdrom", iso,
             "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
             "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
             "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
             "-rtc", "base=localtime",
             "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (xres, yres),
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-serial", "stdio", "-no-reboot",
             "-display", "none", "-qmp", "unix:%s,server,nowait" % self.sock],
            stdin=subprocess.PIPE, stdout=self.serfh, stderr=subprocess.DEVNULL)
        deadline = time.time() + 300
        while time.time() < deadline:
            if os.path.exists(self.serial) and "desktop live" in self.log():
                break
            if self.qemu.poll() is not None:
                raise RuntimeError("qemu exited early")
            time.sleep(0.2)
        else:
            raise RuntimeError("guest never reported a live desktop")
        self.s = Session(self.sock, serial=self.serial)
        # The serial shell is init's child and comes up a moment after "desktop
        # live"; wait for it to answer rather than sleeping a guessed interval.
        # ...and it is asked more than once. "desktop live" is printed by the WM
        # before init has spawned /bin/sh, so the first line typed can land
        # before there is anything reading the console. Retrying is the fix;
        # a longer sleep is a guess that gets slower and still races.
        for _ in range(10):
            self.sh("echo shell-is-up")
            if self.wait_for("shell-is-up", 12, after=1):   # >1: the echo of the line itself
                break
        else:
            raise RuntimeError("the serial shell never answered")

    def log(self):
        try:
            with open(self.serial, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def sh(self, line):
        self.qemu.stdin.write((line + "\n").encode())
        self.qemu.stdin.flush()

    def wait_for(self, needle, secs=15, after=0):
        end = time.time() + secs
        while time.time() < end:
            if self.log().count(needle) > after:
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        try:
            self.qemu.stdin.close()
        except OSError:
            pass
        self.qemu.terminate()
        try:
            self.qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.qemu.kill()
        self.serfh.close()


def quiesce(g, secs=20):
    """Wait until nothing is on the notification overlay.

    Checks must not bleed into one another: a card left over from the previous
    one shifts every slot below it, and `dismiss` -- which asserts that a
    specific card is in a specific slot -- then fails for a reason that has
    nothing to do with dismissing. The kernel prints showing= on every change,
    so this asks it rather than sleeping a guessed interval."""
    end = time.time() + secs
    while time.time() < end:
        lines = [l for l in g.log().splitlines() if "[wm] notify " in l]
        if not lines or "showing=0" in lines[-1]:
            time.sleep(0.6)          # let the last expiry's frame land
            return True
        time.sleep(0.3)
    return False


def perf(text):
    """The last `[wm] perf ...` sample in a serial log, as ints."""
    out = None
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
        out = d
    return out


# ---------------------------------------------------------------------------
# the checks

def t_appear(g, shots, W, H):
    """It is drawn where the design says, and it goes away by itself."""
    box = (W - pt(CARD_W) - pt(CARD_MARGIN) - 4, 0, W, pt(CARD_TOP) + pt(CARD_H) * 4)
    before = PPM(g.s.screendump(shots("appear-0-idle")))
    n0, _ = find_in(before, TILE_INFO, box)

    g.sh('notify "Download" "logit.iso finished" 0')
    assert g.wait_for("[wm] notify post", 10), "the kernel never saw the notification"
    time.sleep(1.0)
    shot = PPM(g.s.screendump(shots("appear-1-showing")))
    n1, bb = find_in(shot, TILE_INFO, box)
    assert n1 > 200, "no notification tile on screen (found %d px of %r)" % (n1, TILE_INFO)
    assert n0 == 0, "that colour was already on screen before the notification (%d px)" % n0

    cx, cy, cw, ch = card_box(0, W)
    assert cx <= bb[0] and bb[2] < cx + cw, "the tile is not inside slot 0 (%r vs %r)" % (bb, (cx, cy, cw, ch))
    assert cy <= bb[1] and bb[3] < cy + ch, "the tile is not inside slot 0 vertically (%r)" % (bb,)
    print("     drawn at %r; slot 0 is x=%d..%d y=%d..%d" % (bb, cx, cx + cw, cy, cy + ch))

    # Transient: gone on its own, with nobody touching anything.
    time.sleep(NOTIFY_MS / 1000.0 + 2.0)
    gone = PPM(g.s.screendump(shots("appear-2-expired")))
    n2, _ = find_in(gone, TILE_INFO, box)
    assert n2 == 0, "the notification is still on screen %d ms later (%d px)" % (NOTIFY_MS, n2)
    print("     gone by itself after %d ms, with no click" % NOTIFY_MS)


def t_stack(g, shots, W, H):
    """Seven at once: three show, four queue, none is lost."""
    seen = g.log().count("[wm] notify post")
    g.sh("notify --burst 7")
    assert g.wait_for("[wm] notify post", 15, after=seen + 6), "the burst did not all arrive"
    time.sleep(1.0)
    shot = PPM(g.s.screendump(shots("stack-7-at-once")))

    box = (W - pt(CARD_W) - pt(CARD_MARGIN) - 4, 0, W, H)
    tiles = 0
    for i in range(NOTIFY_VISIBLE + 2):          # look two slots PAST the limit
        cx, cy, cw, ch = card_box(i, W)
        n, _ = find_in(shot, TILE_INFO, (cx, cy, cx + cw, cy + ch))
        n += find_in(shot, TILE_WARN, (cx, cy, cx + cw, cy + ch))[0]
        n += find_in(shot, TILE_ERROR, (cx, cy, cx + cw, cy + ch))[0]
        if n > 200:
            tiles += 1
            assert i < NOTIFY_VISIBLE, "a card is drawn in slot %d, past the %d-card limit" % (i, NOTIFY_VISIBLE)
    assert tiles == NOTIFY_VISIBLE, "%d cards on screen, expected %d" % (tiles, NOTIFY_VISIBLE)

    # ...and the kernel's own account of it agrees with the picture.
    lines = [l for l in g.log().splitlines() if "[wm] notify post" in l]
    last = lines[-1]
    assert "showing=%d" % NOTIFY_VISIBLE in last, "kernel says %r" % last
    q = int(last.split("queued=")[1].split()[0])
    assert q >= 4, "only %d queued of a burst of 7 -- something was dropped (%r)" % (q, last)
    assert "dropped=0" in last, "the burst dropped notifications: %r" % last
    print("     %d on screen, %d queued, 0 dropped -- %s" % (tiles, q, last.split("] ")[1]))

    # And they drain: wait them out and the screen is clear again.
    time.sleep(NOTIFY_MS / 1000.0 * 3 + 3)
    clear = PPM(g.s.screendump(shots("stack-drained")))
    for c in (TILE_INFO, TILE_WARN, TILE_ERROR):
        n, _ = find_in(clear, c, box)
        assert n == 0, "%r is still on screen after the queue should have drained (%d px)" % (c, n)
    print("     the whole queue drained on its own")


def t_focus(g, shots, W, H):
    """Keystrokes keep arriving in the focused window while a card is up.

    A notification is not a window: it has no surface, no input queue and no
    place in the z-order. This is how that is checked rather than asserted --
    type, interrupt, type again, and require the second batch of characters to
    have landed exactly like the first."""
    g.s.click_at(*dock_icon(TEXTEDIT_SLOT))
    time.sleep(3.0)
    base = PPM(g.s.screendump(shots("focus-0-empty")))

    # Where the window is, DERIVED rather than assumed: the WM cascades new
    # windows, so a fixed rectangle is a rectangle that is right until somebody
    # opens a second app. Only the FOCUSED window paints its close button in
    # colour (draw_frame in wm.c), and that circle's centre is at
    # (win.x + S(16), win.y + S(15)) -- so its bounding box locates the window.
    # TextEdit draws its text at window-local (10, 8) points, below the S(30)
    # titlebar (c/apps/gui/textedit.c redraw()).
    n, cb = find_in(base, CLOSE_RGB, (0, 0, W, H))
    assert cb is not None, "no focused window on screen -- TextEdit did not open"
    wx = (cb[0] + cb[2]) // 2 - pt(16)
    wy = (cb[1] + cb[3]) // 2 - pt(15)
    band = (wx + pt(6), wy + pt(30) + pt(2), wx + pt(220), wy + pt(30) + pt(26))
    print("     TextEdit at (%d,%d); watching its first text line %r" % (wx, wy, band))

    g.s.typ("aaaaaa")
    time.sleep(1.0)
    one = PPM(g.s.screendump(shots("focus-1-typed")))
    d1 = ink_in(one, band) - ink_in(base, band)
    assert d1 > 40, "the first six keystrokes did not draw anything (%d px)" % d1

    # ...now interrupt, from a different process entirely.
    g.sh('notify "Interruption" "typed through, on purpose" 1')
    assert g.wait_for("[wm] notify post", 10), "the notification never arrived"
    time.sleep(0.6)
    mid = PPM(g.s.screendump(shots("focus-2-notified")))
    nb = (W - pt(CARD_W) - pt(CARD_MARGIN) - 4, 0, W, pt(CARD_TOP) + pt(CARD_H) * 2)
    assert find_in(mid, TILE_WARN, nb)[0] > 200, "the card is not actually on screen"

    g.s.typ("aaaaaa")
    time.sleep(1.0)
    two = PPM(g.s.screendump(shots("focus-3-typed-through")))
    d2 = ink_in(two, band) - ink_in(one, band)
    assert d2 > 40, "keystrokes STOPPED arriving while a notification was up (%d px vs %d)" % (d2, d1)
    # Same characters, same font: the two batches must cost about the same ink.
    lo, hi = d1 * 0.55, d1 * 1.75
    assert lo <= d2 <= hi, ("the six keystrokes after the notification drew %d px, "
                            "the six before drew %d -- some were swallowed" % (d2, d1))
    print("     six chars before the card: %d px of ink; six after: %d px" % (d1, d2))

    # ...and it is still the focused window, in the same place: the close button
    # is where it was. A notification that had taken focus would have moved it.
    n2, cb2 = find_in(two, CLOSE_RGB, (0, 0, W, H))
    assert cb2 == cb, "the focused window changed (%r -> %r)" % (cb, cb2)
    print("     the window still owns focus")


def t_dismiss(g, shots, W, H):
    """A click takes a card away, and the one below moves up."""
    g.sh("notify --burst 3")
    assert g.wait_for("[wm] notify post", 10)
    time.sleep(1.0)
    before = PPM(g.s.screendump(shots("dismiss-0-three")))
    box0 = card_box(0, W)
    box1 = card_box(1, W)
    top_before, _ = find_in(before, TILE_WARN, (box0[0], box0[1], box0[0] + box0[2], box0[1] + box0[3]))
    # Burst levels cycle 1,2,0 -- slot 0 is level 1 (warn), slot 1 is level 2.
    assert top_before > 200, "the top card is not the one expected (%d px of warn)" % top_before
    second, _ = find_in(before, TILE_ERROR, (box1[0], box1[1], box1[0] + box1[2], box1[1] + box1[3]))
    assert second > 200, "the second card is not where expected (%d px of error)" % second

    closes = g.log().count("[wm] notify close")
    g.s.click_at(box0[0] + box0[2] // 2, box0[1] + box0[3] // 2)
    assert g.wait_for("[wm] notify close", 10, after=closes), "the click did not dismiss anything"
    time.sleep(0.8)
    after = PPM(g.s.screendump(shots("dismiss-1-clicked")))
    promoted, _ = find_in(after, TILE_ERROR, (box0[0], box0[1], box0[0] + box0[2], box0[1] + box0[3]))
    assert promoted > 200, "the card below did not move up into slot 0 (%d px)" % promoted
    gone, _ = find_in(after, TILE_WARN, (box0[0], box0[1], box0[0] + box0[2], box0[1] + box0[3]))
    assert gone == 0, "the dismissed card is still there (%d px)" % gone
    print("     clicked slot 0: it went, and the one below took its place")
    time.sleep(NOTIFY_MS / 1000.0 + 2)


def t_stale(g, shots, W, H):
    """THE DAMAGE CHECK. Raise one, let it expire, compare with before.

    Zero differing pixels or the overlay lied about what it drew. There is no
    periodic full repaint in this compositor to heal an under-report, which is
    what makes this check possible at all -- and is why the negative control
    (NOTIFYLIE=1, the overlay reporting half its column) fails it."""
    skip = pt(24) + 2                      # the menu bar's clock is not our business
    time.sleep(1.5)
    a = PPM(g.s.screendump(shots("stale-0-before")))
    g.sh('notify "Round trip" "and then nothing should remain" 2')
    assert g.wait_for("[wm] notify post", 10)
    time.sleep(1.2)
    during = PPM(g.s.screendump(shots("stale-1-during")))
    nb = (W - pt(CARD_W) - pt(CARD_MARGIN) - 4, 0, W, pt(CARD_TOP) + pt(CARD_H) * 2)
    assert find_in(during, TILE_ERROR, nb)[0] > 200, "nothing was drawn, so nothing is being tested"
    dn, dbox = diff_pixels(a, during, skip)
    print("     while showing: %d pixels differ, in %r" % (dn, dbox))

    time.sleep(NOTIFY_MS / 1000.0 + 3.0)
    b = PPM(g.s.screendump(shots("stale-2-after")))
    n, box = diff_pixels(a, b, skip)
    assert n == 0, ("%d STALE PIXELS left behind, in %r -- the overlay reported "
                    "less damage than it caused" % (n, box))
    print("     after it expired: 0 differing pixels")


def sample_perf(g, tries=30):
    """Make the compositor print a perf line, and return it.

    wm_perf_report() stays QUIET on an idle desktop (`if (dm == 0 && dc <= 20)
    return`), which is right for a log and useless for a measurement: the state
    this needs to measure IS an idle desktop with a notification on it. So the
    pointer is nudged one pixel, which makes `dm` non-zero and unblocks the
    report.

    That nudge is deliberately the cheapest event in the system and it is why it
    was chosen: the pointer is a display PLANE on virtio-gpu, so motion does not
    damage anything and does not provoke a composite. It moves the reporting
    gate, not the thing being counted."""
    have = g.log().count("[wm] perf ")
    for i in range(tries):
        g.s.goto(g.s.cur[0] + (1 if i % 2 == 0 else -1), g.s.cur[1], settle=0.05)
        time.sleep(0.4)
        if g.log().count("[wm] perf ") > have:
            return perf(g.log())
    return None


def t_cost(g, shots, W, H):
    """What a notification costs the compositor, from its own counters.

    WITH A CONTROL, because neither half of the naive measurement is what it
    looks like:

      * an "idle" desktop is not idle -- the menu-bar clock damages a strip
        twice a second, about twenty composites over the ten seconds this takes;
      * and running `notify` from the shell FORKS AND EXECS A PROCESS, which
        costs the compositor a full-screen frame per command all by itself.
        The first version of this measurement showed three full-screen frames
        and would have blamed them on the notification.

    So the control runs the same number of commands, in the same shell, over the
    same span -- `true` instead of `notify` -- and the difference between the
    two intervals is the only figure quoted as the notification's cost."""
    span = NOTIFY_MS / 1000.0 + 5.0

    def interval(label, do):
        a = sample_perf(g)
        t0 = time.time()
        do()
        time.sleep(span)
        b = sample_perf(g)
        if not a or not b:
            return None
        d = {k: b[k] - a[k] for k in ("composites", "cpx", "fpx", "full", "rects")}
        d["secs"] = time.time() - t0
        print("       %-22s composites %3d (full %d)  cpx %9d  fpx %9d  over %.1fs"
              % (label, d["composites"], d["full"], d["cpx"], d["fpx"], d["secs"]))
        return d

    def raise_three():
        for t, lv in (("One", 0), ("Two", 1), ("Three", 2)):
            g.sh('notify "%s" "cost sample" %d' % (t, lv))
            time.sleep(0.4)

    def three_noops():
        for _ in range(3):
            g.sh("true")
            time.sleep(0.4)

    base = interval("control: 3x /bin/true", three_noops)
    quiesce(g)
    load = interval("3 notifications", raise_three)
    quiesce(g)
    if not base or not load:
        print("     could not get two comparable perf samples")
        return
    col_px = pt(CARD_W) * (pt(CARD_H) * NOTIFY_VISIBLE + pt(CARD_GAP) * (NOTIFY_VISIBLE - 1))
    print("     ATTRIBUTABLE TO THE NOTIFICATIONS (load - baseline):")
    print("       composites      %+d" % (load["composites"] - base["composites"]))
    print("       px recomposited %+d" % (load["cpx"] - base["cpx"]))
    print("       px presented    %+d" % (load["fpx"] - base["fpx"]))
    print("       full-screen frames %+d" % (load["full"] - base["full"]))
    print("     the overlay's damage column is %d device px = %.1f%% of the screen"
          % (col_px, 100.0 * col_px / (W * H)))


CHECKS = [("appear", t_appear), ("stack", t_stack), ("focus", t_focus),
          ("dismiss", t_dismiss), ("stale", t_stale), ("cost", t_cost)]


def main(argv):
    xres, yres = 1920, 1200
    iso = disk = None
    only = None
    keep = False
    i = 1
    while i < len(argv):
        if argv[i] == "--xres":   xres = int(argv[i + 1]); i += 2
        elif argv[i] == "--yres": yres = int(argv[i + 1]); i += 2
        elif argv[i] == "--iso":  iso = argv[i + 1]; i += 2
        elif argv[i] == "--disk": disk = argv[i + 1]; i += 2
        elif argv[i] == "--only": only = argv[i + 1].split(","); i += 2
        elif argv[i] == "--keep": keep = True; i += 1
        else:
            print("unknown arg %r" % argv[i]); return 2
    iso = iso or os.path.join(ROOT, "build", "logit.iso")
    disk = disk or os.path.join(ROOT, "build", "disk.img")

    check_constants()
    scale = configure(xres, yres)
    tmp = tempfile.mkdtemp(prefix="logit-notify-")
    shotdir = os.path.join(tmp, "shots")
    os.makedirs(shotdir, exist_ok=True)

    def shots(name):
        return os.path.join(shotdir, name + ".ppm")

    print("=== notifications: %dx%d device px (scale %d%%) ===  [timings are TCG]"
          % (xres, yres, scale))
    g = Guest(iso, disk, xres, yres, tmp)
    failed = []
    try:
        for name, fn in CHECKS:
            if only and name not in only:
                continue
            print("--- %s" % name)
            try:
                if not quiesce(g):
                    raise AssertionError("the overlay never emptied before this check")
                fn(g, shots, xres, yres)
                print("     ok")
            except AssertionError as e:
                print("     FAIL: %s" % e)
                failed.append(name)
    finally:
        g.stop()

    print("screenshots: %s" % shotdir)
    if failed:
        print("FAIL: %s" % ", ".join(failed))
        return 1
    print("PASS")
    if not keep:
        pass          # the frames are the evidence; they are left on purpose
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
