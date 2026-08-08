#!/usr/bin/env python3
"""Shared QMP desktop-driving helpers: dock geometry, pointer, keyboard,
screendumps, and PPM inspection.

This exists because the dock geometry silently rotted once already. The dock is
CENTRED, so adding one app moves every icon 32 px left and a stale coordinate
lands on the wallpaper -- which does nothing at all and looks exactly like the
app failing to open. One copy of that arithmetic, imported by every driver, is
the only version of this that stays true.
"""

import json
import socket
import time

# --- Dock geometry, mirrored from draw_dock() in c/kernel/gui/wm.c ---
#
# TWO UNITS LIVE HERE. wm.c's dock constants are POINTS; QMP's pointer works in
# DEVICE pixels. The compositor multiplies by a backing scale it derives from the
# mode (fb.c pick_scale), so this arithmetic has to derive the same scale from
# the same numbers -- otherwise every driver's coordinates silently rot the way
# they did when the app count changed, except now they rot when the RESOLUTION
# changes and nothing in a driver mentions resolution at all.
#
# The default is 1280x800, where the scale is 100 and a point IS a pixel, so
# every existing driver -- all of which boot QEMU at 1280x800 themselves -- gets
# byte-identical coordinates to before. A driver that boots at another mode calls
# configure() first.
DESIGN_W_PT, DESIGN_H_PT = 1280, 800  # fb.c: DESIGN_W_PT / DESIGN_H_PT
DOCK_ISZ_PT, DOCK_GAP_PT = 50, 14     # wm.c: DOCK_ISZ_PT, DOCK_GAP_PT
NAPPS = 10                            # *.aex at the LogitFS root, in scan_apps order:
BROWSER_SLOT = 8                      # clock textedit monitor terminal widgets
GALLERY_SLOT = 9                      # files preview studio browser gallery
# Gallery is packed LAST (Makefile: after $(BROWSER_AEX), not appended to APPS)
# precisely so BROWSER_SLOT keeps its value. NAPPS still has to change, because
# the dock is centred: one more app moves every icon 32 pt left.


def pick_scale(dev_w, dev_h):
    """The backing scale the kernel will choose for this mode -- fb.c pick_scale.

    Kept as one readable expression rather than a table because it is a claim
    about the kernel's behaviour, and a test that guesses at it proves nothing:
    qmp_scale.py asserts the guest's own reported scale equals this."""
    s = min(dev_w * 100 // DESIGN_W_PT, dev_h * 100 // DESIGN_H_PT)
    s = (s // 25) * 25
    return max(100, min(300, s))


SCREEN_W, SCREEN_H = DESIGN_W_PT, DESIGN_H_PT   # device pixels
SCALE = 100


def configure(dev_w, dev_h):
    """Point every geometry helper at a different display mode."""
    global SCREEN_W, SCREEN_H, SCALE
    SCREEN_W, SCREEN_H, SCALE = dev_w, dev_h, pick_scale(dev_w, dev_h)
    return SCALE


def pt(v):
    """Points -> device pixels, floored exactly as fb_pt() does."""
    return v * SCALE // 100


def dock_icon(i, n=NAPPS):
    """Centre of dock icon `i` (0-based) in DEVICE pixels, matching draw_dock()."""
    isz, gap = pt(DOCK_ISZ_PT), pt(DOCK_GAP_PT)
    dw = gap + n * (isz + gap)
    x0 = (SCREEN_W - dw) // 2
    y0 = SCREEN_H - (isz + pt(20)) - pt(12)
    return (x0 + gap + i * (isz + gap) + isz // 2,
            y0 + pt(10) + isz // 2)


# The cursor's outline colour, from draw_cursor_back() in c/kernel/gui/wm.c. The
# arrow's top-left cell is opaque outline, so the bounding box of this colour
# starts exactly at the pointer's hotspot.
#
# THIS ONLY WORKS WHEN THE POINTER IS IN THE FRAME. On virtio-gpu the kernel now
# puts the arrow on the device's cursor plane, so the scanout -- and therefore a
# screendump -- does not contain it at all, exactly like every other OS with a
# hardware cursor. That is not a regression to route around: it is the whole
# reason moving the mouse no longer recomposites the screen. The guest prints
# `[wm] ptr X Y` on the serial console whenever the pointer settles, and that is
# the authority now; the picture is the fallback for the software-cursor path.
CURSOR_RGB = (20, 20, 26)


def locate_cursor(ppm):
    """The pointer's (x, y) as the guest drew it, or None.

    None can mean "the pointer is on a display plane", not only "it is missing"
    -- see the note above. Prefer Session.guest_pointer() when a serial log is
    available."""
    box = ppm.find_color(CURSOR_RGB)
    return None if box is None else (box[0], box[1])


def parse_pointer(text):
    """The last `[wm] ptr X Y` in a serial log, or None."""
    got = None
    for line in text.splitlines():
        i = line.find("[wm] ptr ")
        if i < 0:
            continue
        parts = line[i + 9:].split()
        if len(parts) >= 2:
            try:
                got = (int(parts[0]), int(parts[1]))
            except ValueError:
                pass
    return got


KMAP = {" ": "spc", ".": "dot", "\n": "ret", "-": "minus", "/": "slash",
        "_": "minus", "=": "equal", ",": "comma"}
SHIFT = {":": "semicolon", "_": "minus", "?": "slash"}


class Session:
    """A QMP connection plus a model of where the pointer is.

    The pointer position has to be tracked by us: QEMU's `rel` input events are
    deltas, and there is no way to ask the guest where its cursor ended up."""

    def __init__(self, sock_path, timeout=120.0, serial=None):
        # `serial` is the guest's console log. Given one, the pointer is read
        # back from the guest's own report instead of guessed from deltas or
        # hunted for in a screenshot -- which is the only method that survives
        # the pointer moving to a display plane.
        self.serial = serial
        self.s = socket.socket(socket.AF_UNIX)
        deadline = time.time() + timeout
        while True:
            try:
                self.s.connect(sock_path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.f = self.s.makefile("rw")
        json.loads(self.f.readline())          # the greeting
        self.cmd({"execute": "qmp_capabilities"})
        # The WM centres the cursor on the screen at boot.
        self.cur = [SCREEN_W // 2, SCREEN_H // 2]

    def _recv(self):
        while True:
            line = self.f.readline()
            if not line:
                return None
            m = json.loads(line)
            if "return" in m or "error" in m:
                return m

    def cmd(self, d):
        self.f.write(json.dumps(d) + "\n")
        self.f.flush()
        return self._recv()

    def _input(self, events):
        return self.cmd({"execute": "input-send-event", "arguments": {"events": events}})

    # A PS/2 mouse packet carries a 9-bit signed delta, so one "rel" event cannot
    # express an arbitrary jump: past ~255 px per axis the movement is clamped and
    # the pointer stops SHORT of where the driver believes it is, silently. That
    # never bit anything while every driver worked on a 1280x800 screen and every
    # jump was small; on a 2560x1600 display the dock is 700 px below centre and
    # the click landed on the wallpaper -- which looks exactly like the app
    # failing to launch. Step the move instead. The endpoint is identical at every
    # resolution, so this changes nothing for the existing drivers.
    STEP = 128

    def goto(self, tx, ty, settle=0.2):
        while self.cur != [tx, ty]:
            dx = max(-self.STEP, min(self.STEP, tx - self.cur[0]))
            dy = max(-self.STEP, min(self.STEP, ty - self.cur[1]))
            self._input([{"type": "rel", "data": {"axis": "x", "value": dx}},
                         {"type": "rel", "data": {"axis": "y", "value": dy}}])
            self.cur[0] += dx
            self.cur[1] += dy
            time.sleep(0.01)
        time.sleep(settle)

    def click(self, hold=0.12):
        for down in (True, False):
            self._input([{"type": "btn", "data": {"button": "left", "down": down}}])
            time.sleep(hold)

    def click_at(self, x, y, settle=0.2):
        self.goto(x, y, settle)
        self.click()

    def key(self, qcode, settle=0.05):
        for down in (True, False):
            self._input([{"type": "key",
                          "data": {"key": {"type": "qcode", "data": qcode}, "down": down}}])
        time.sleep(settle)

    def key_shift(self, qcode, settle=0.05):
        self._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": True}},
                     {"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}])
        self._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}},
                     {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": False}}])
        time.sleep(settle)

    def typ(self, text):
        # Uppercase letters go through shift, like every other shifted key.
        # They used to fall through to `self.key("O")`, and QEMU's qcodes are
        # lowercase -- so the key name did not exist and the character was
        # dropped WITHOUT AN ERROR. Every URL with a capital in it therefore
        # loaded a different page than the test asked for:
        # ".../wiki/Operating_system" fetched ".../wiki/perating_system", which
        # is Wikipedia's "no article with this exact name" page. The screenshot
        # afterwards looks like a rendering bug and is not one.
        for ch in text:
            if ch in SHIFT:
                self.key_shift(SHIFT[ch])
            elif "A" <= ch <= "Z":
                self.key_shift(ch.lower())
            else:
                self.key(KMAP.get(ch, ch))

    def screendump(self, path, settle=0.5):
        self.cmd({"execute": "screendump", "arguments": {"filename": path}})
        time.sleep(settle)
        return path

    def guest_pointer(self):
        """Where the guest says its pointer is, or None."""
        if not self.serial:
            return None
        try:
            with open(self.serial, errors="replace") as fh:
                return parse_pointer(fh.read())
        except OSError:
            return None

    def settle_pointer(self, ppm_path, tx, ty, tries=8, settle=0.4):
        """Move to (tx,ty) and CONFIRM the guest's pointer got there.

        Dead reckoning off `rel` deltas is a guess, and on a 2560x1600 desktop it
        is a wrong one: the emulated PS/2 controller buffers a byte at a time, so
        a long move sent as a burst of packets loses some of them and the pointer
        stops short. Nothing reports that. The click then lands on whatever
        happens to be at the wrong place, which reads as a hit-testing bug in the
        thing being tested rather than a lie in the harness.

        So ask the guest instead of guessing: it prints `[wm] ptr X Y` when the
        pointer settles, which is true whether the arrow is composited into the
        frame or sitting on the display's cursor plane. Without a serial log the
        fallback is the picture, which only answers while the arrow is in it.
        Returns the confirmed position, or None if it never converged."""
        self.goto(tx, ty, settle)
        for _ in range(tries):
            got = self.guest_pointer()
            if got is None:
                self.screendump(ppm_path, settle=0.2)
                got = locate_cursor(PPM(ppm_path))
            if got is None:
                return None
            if got == (tx, ty):
                return got
            # Believe the guest, not the model, then re-aim.
            self.cur = [got[0], got[1]]
            self.goto(tx, ty, settle)
        return None

    def click_at_confirmed(self, ppm_path, x, y):
        """click_at, but only after the pointer is verified to be on the pixel."""
        got = self.settle_pointer(ppm_path, x, y)
        if got != (x, y):
            raise AssertionError("pointer would not settle at (%d,%d); it is at %r"
                                 % (x, y, got))
        self.click()
        return got


# ---- PPM inspection -------------------------------------------------------
# QEMU's screendump writes a binary P6 PPM. Reading it directly (rather than
# shelling out to an image library) keeps the harness dependency-free.

class PPM:
    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        if not data.startswith(b"P6"):
            raise ValueError(path + ": not a binary PPM")
        # header: P6 <w> <h> <maxval>, whitespace/comment separated
        fields, i = [], 2
        while len(fields) < 3:
            while i < len(data) and data[i:i + 1].isspace():
                i += 1
            if data[i:i + 1] == b"#":
                while i < len(data) and data[i] != 0x0A:
                    i += 1
                continue
            j = i
            while j < len(data) and not data[j:j + 1].isspace():
                j += 1
            fields.append(int(data[i:j]))
            i = j
        self.w, self.h, _maxv = fields
        self.px = data[i + 1:]

    def at(self, x, y):
        o = (y * self.w + x) * 3
        return (self.px[o], self.px[o + 1], self.px[o + 2])

    def find_color(self, rgb):
        """Bounding box of every pixel exactly equal to `rgb`, or None.

        Exact match on a deliberately odd colour is the point: it cannot be
        confused with the wallpaper gradient, the glass chrome or an icon."""
        r, g, b = rgb
        target = bytes((r, g, b))
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        row = self.w * 3
        for y in range(self.h):
            base = y * row
            start = 0
            while True:
                k = self.px.find(target, base + start, base + row)
                if k < 0:
                    break
                off = k - base
                if off % 3:                    # a straddling match, not a pixel
                    start = off + 1
                    continue
                x = off // 3
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
                start = off + 3
        if x1 < 0:
            return None
        return (x0, y0, x1, y1)

    def dark_pixels(self, box, thresh=90):
        """Count near-black pixels inside `box` -- i.e. how much TEXT is there.

        Two different strings on the same coloured block give two different
        counts, which is how a screenshot answers "did the text change?"
        without OCR."""
        x0, y0, x1, y1 = box
        n = 0
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            base = y * self.w * 3
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                o = base + x * 3
                if self.px[o] < thresh and self.px[o + 1] < thresh and self.px[o + 2] < thresh:
                    n += 1
        return n

    def first_dark(self, box, thresh=90):
        """The first near-black pixel in `box`, scanning rows top-down.

        Used to aim a click AT A GLYPH. The centre of a full-width block is
        usually blank space between words, and layout emits one box per word --
        so clicking the middle of the band can miss every text box on it."""
        x0, y0, x1, y1 = box
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            base = y * self.w * 3
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                o = base + x * 3
                if self.px[o] < thresh and self.px[o + 1] < thresh and self.px[o + 2] < thresh:
                    return (x, y)
        return None
