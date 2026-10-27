#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Aims: where the guest says its controls are (design P13, ui-qmp-kit).

Every app (and the WM) prints one line per control whenever the control moves:

    [<app>] aim <name> <x> <y>     window-local CONTENT points (the app's own)
    [wm] aim <name> <x> <y>        SCREEN points (the WM's controls)

A driver asks the guest instead of keeping a second copy of the layout:

    aims = Aims(ui)                      # ui: a Session (or anything with
                                         #     serial_text())
    x, y = aims.wait("settings", "apply", timeout=10)      # content pt
    px, py = aims.screen("settings", "apply")              # DEVICE px, ready
    ui.click_at(px, py)                                    #   for click_at
    mx, my = aims.wm("dock-0")                             # screen pt

Latest line per (app, name) wins -- an aim that moved is re-emitted, an aim
that did not is not. Numeric constants stay only for design-pinned geometry.

screen() returns device pixels because that is what a click is measured in.
The content origin is derived the way tests/boot/run-clock-ui.py always did
it: left = frame x, top = frame y + frame h - pt(content h), where the
`[wm] win N frame x y w h content cw ch pt` line gives the frame in device
pixels and the content size in points. Window selection prefers the optional
`[wm] win N app <stem>` line, else any title in qmp_ui.titles_of(app).
"""
import re
import time

import qmp_ui

# One write per line on the guest, but WM lines can still splice in front of
# it, so the pattern is searched, not anchored.
AIM_RE = re.compile(r"\[([A-Za-z0-9_.-]+)\] aim ([^\s]+) (-?\d+) (-?\d+)")


def parse_aims(text):
    """{(app, name): (x, y)} -- the latest line per key wins."""
    out = {}
    for line in text.splitlines():
        for m in AIM_RE.finditer(line):
            out[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
    return out


def aim_lines(text):
    """Every aim line in order, verbatim (for failure messages)."""
    return [m.group(0) for line in text.splitlines()
            for m in AIM_RE.finditer(line)]


class AimTimeout(AssertionError):
    pass


class Aims:
    def __init__(self, ui, scale=None):
        """`ui` needs serial_text(). `scale` is the percent scale used for
        screen(); default qmp_ui.SCALE at call time (configure())."""
        self.ui = ui
        self.scale = scale

    # -- raw reads ---------------------------------------------------------
    def _text(self):
        return self.ui.serial_text()

    def get(self, app, name):
        """Latest (x, y) or None."""
        return parse_aims(self._text()).get((app, name))

    def wait(self, app, name, timeout=10.0, poll=0.1):
        """Window-local (x, y) of `app`'s control `name`; on timeout raise
        naming the app, the aim and the last 5 aim lines seen."""
        end = time.time() + timeout
        while True:
            text = self._text()
            got = parse_aims(text).get((app, name))
            if got is not None:
                return got
            if time.time() >= end:
                last = aim_lines(text)[-5:]
                raise AimTimeout(
                    "no aim %r from app %r within %.1fs; last aim lines: %s"
                    % (name, app, timeout, last or "none"))
            time.sleep(poll)

    # -- screen coordinates -----------------------------------------------
    def _pt(self, v):
        scale = self.scale if self.scale is not None else qmp_ui.SCALE
        return v * scale // 100

    def origin(self, app):
        """Device-pixel top-left of `app`'s content, from the newest
        `[wm] win N frame` line of its window. Raises when it has none."""
        w = qmp_ui.window_of(app, self.ui, newest=True)
        if w is None:
            raise AimTimeout(
                "no window for app %r (titles %r) in the serial log; windows: %s"
                % (app, qmp_ui.titles_of(app),
                   [(v["slot"], v["title"]) for v in
                    qmp_ui.parse_windows(self._text()).values()]))
        return (w["x"], w["y"] + w["h"] - self._pt(w["ch"]))

    def screen(self, app, name, timeout=10.0):
        """Device-pixel screen coordinates of the control: content origin +
        pt(aim). Ready for Session.click_at()."""
        ax, ay = self.wait(app, name, timeout)
        ox, oy = self.origin(app)
        return (ox + self._pt(ax), oy + self._pt(ay))

    def wm(self, name, timeout=10.0):
        """`[wm] aim <name> x y`: SCREEN points, exactly as printed."""
        return self.wait("wm", name, timeout)

    def wm_px(self, name, timeout=10.0):
        x, y = self.wm(name, timeout)
        return (self._pt(x), self._pt(y))
