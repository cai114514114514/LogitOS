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
SCREEN_W, SCREEN_H = 1280, 800
DOCK_ISZ, DOCK_GAP = 50, 14           # wm.c: dock_isz, dock_gap
NAPPS = 9                             # *.aex at the LogitFS root, in scan_apps order:
BROWSER_SLOT = 8                      # clock textedit monitor terminal widgets
                                      # files preview studio browser


def dock_icon(i, n=NAPPS):
    """Centre of dock icon `i` (0-based), matching wm.c's draw_dock()."""
    dw = DOCK_GAP + n * (DOCK_ISZ + DOCK_GAP)
    x0 = (SCREEN_W - dw) // 2
    y0 = SCREEN_H - (DOCK_ISZ + 20) - 12
    return (x0 + DOCK_GAP + i * (DOCK_ISZ + DOCK_GAP) + DOCK_ISZ // 2,
            y0 + 10 + DOCK_ISZ // 2)


KMAP = {" ": "spc", ".": "dot", "\n": "ret", "-": "minus", "/": "slash",
        "_": "minus", "=": "equal", ",": "comma"}
SHIFT = {":": "semicolon", "_": "minus", "?": "slash"}


class Session:
    """A QMP connection plus a model of where the pointer is.

    The pointer position has to be tracked by us: QEMU's `rel` input events are
    deltas, and there is no way to ask the guest where its cursor ended up."""

    def __init__(self, sock_path, timeout=120.0):
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

    def goto(self, tx, ty, settle=0.2):
        self._input([{"type": "rel", "data": {"axis": "x", "value": tx - self.cur[0]}},
                     {"type": "rel", "data": {"axis": "y", "value": ty - self.cur[1]}}])
        self.cur[0], self.cur[1] = tx, ty
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
        for ch in text:
            if ch in SHIFT:
                self.key_shift(SHIFT[ch])
            else:
                self.key(KMAP.get(ch, ch))

    def screendump(self, path, settle=0.5):
        self.cmd({"execute": "screendump", "arguments": {"filename": path}})
        time.sleep(settle)
        return path


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
