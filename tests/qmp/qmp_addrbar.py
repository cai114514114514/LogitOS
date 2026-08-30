#!/usr/bin/env python3
"""The Browser's address bar, DERIVED from what browser.c actually renders.

WHY THIS MODULE EXISTS. Nine drivers used to "click the address bar" at a
hardcoded (420, 145). That coordinate was right once, stopped being right the
day the desktop's window count or the Browser's chrome changed, and the
drivers kept passing anyway -- because the Browser boots with `editing = 1`
(browser.c: `redraw(1); int editing = 1;`), so the FIRST URL a driver types
lands in the bar with no click at all and the click was decoration. On this
tree, with the Finder owning cascade slot 0, (420, 145) lands 17 px into the
Browser's TAB STRIP, which hit-tests to "no tab" and no-ops silently. CLAUDE.md
describes the same click as "inside the window-manager TITLEBAR"; both are
the same defect at different window counts, and the correction is kept beside
the old claim here rather than over it.

The referee is the CARET. browser.c's draw_address_bar() paints, whenever
`editing` is true, a 2x16 bar in exactly rgb(90, 150, 240) at window-local
x = 14 + ucaret*8, y = TABH+7 .. TABH+23. On a fresh boot the bar is empty
(ucaret = 0), so the caret is a machine-checkable landmark for the window's
whole client origin. This module finds it, cross-checks it against every
position wm.c's cascade rule can put the window at, and refuses -- loudly,
with the screenshot path in the message -- to return a click coordinate
otherwise. A driver using focus() cannot silently type into the page.

WHAT IS DELIBERATELY NOT CHECKED. After a click the kernel does not repaint
(the click handler sets `editing` without requesting a frame), so a post-click
caret check is impossible without typing. Worse -- and this was MEASURED, not
assumed, by running this module's own sabotage control (QMP_ADDRBAR_LIE=1)
against a boot-fresh browser -- a first-navigation click is UNFALSIFIABLE from
inside the session: the bar boots focused, the retired (420, 145) lands on the
tab strip and hit-tests to nothing, and the typing lands in the bar anyway, so
a green gate proves nothing about the click. focus() therefore makes the click
load-bearing by unfocusing the bar first (one click into the blank boot
viewport) and letting typed_echo() -- dark glyph pixels in the field band,
checked after typing and BEFORE Enter -- be the verdict. If that control ever
goes green under QMP_ADDRBAR_LIE=1 again, the check has rotted and this
comment is the place to start.

All constants are quoted with their single source line in c/; they are
restated here, not derived, because the alternative is parsing C at test
time. When one of them changes, this file is the second door -- grep for its
name in tests/qmp/ after editing browser.c or wm.c chrome geometry.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmp_ui                      # noqa: E402  (pt(), SCREEN_W/H -- the geometry jar)

# --- one door per constant; each is the sole spelling used by the drivers ----
CARET_RGB = (90, 150, 240)   # browser.c draw_address_bar(): gui_rect(..., rgb(90,150,240))
TITLEBAR_PT = 30             # wm.c TITLEBAR_H ("points")
TABH_PT = 30                 # browser.c TABH -- the tab strip, ABOVE the address bar
BARH_PT = 30                 # browser.c BARH -- the address bar band itself
FIELD_X_PT = 10              # browser.c: gui_glass(10, TABH+5, win_w-20, 20, ...)
FIELD_DY_PT = (5, 25)        # the URL field's y INSIDE the bar: TABH+5 .. TABH+25
CARET_DY_PT = 7              # caret top inside the bar: gui_rect(14+ucaret*8, TABH+7, 2, 16)
CARET_H_PT = 16
TEXT_X_PT = 14               # gui_text(14, TABH+7, ...) -- glyph origin, dark rgb(40,40,48)
WINW_PT, WINH_PT = 1180, 620  # browser.c WINW/WINH -- only the FALLBACK born size; the
                              # real one is 88%/80% of the screen (pick_born_size) when
                              # nothing is remembered, so candidates cover both.
MENUBAR_PT = 24              # wm.c MENUBAR_H -- the cascade's y floor
BORN_FRAC = (88, 80)         # browser.c pick_born_size(): w = sw*88/100, h = sh*80/100


def die(msg):
    """Exit non-zero with the message. Kept tiny so every user reads the same."""
    print("qmp_addrbar: FAIL: " + msg)
    sys.exit(1)


def _born_sizes_pt():
    """The born client sizes pick_born_size() can choose, in POINTS.

    Two, not one, because the screen-derived default and the WINW/WINH
    fallback are both reachable (a screen smaller than WIN_MIN_* keeps the
    constants). Deduplicated; the order does not matter, locate() tries all."""
    sw = qmp_ui.SCREEN_W * 100 // max(1, qmp_ui.SCALE)
    sh = qmp_ui.SCREEN_H * 100 // max(1, qmp_ui.SCALE)
    sizes = set()
    if sw >= 480 and sh >= 320:
        w, h = sw * BORN_FRAC[0] // 100, sh * BORN_FRAC[1] // 100
        w = min(w, sw - 40)
        h = min(h, sh - 120)
        sizes.add((max(w, 480), max(h, 320)))
    sizes.add((WINW_PT, WINH_PT))
    return sorted(sizes)


def client_candidates():
    """Every device-pixel (x, y) the Browser's CLIENT AREA can start at.

    Mirrors wm.c's window placement: x = S(110 + cascade*28), y = S(70 +
    cascade*28), cascade 0..5, clamped on-screen and floored at the menu bar;
    the client area sits TITLEBAR_PT below the window origin. This is the same
    arithmetic qmp_fullsystem.py's window_slots() encodes for its own fallback
    click -- restated rather than imported because that file boots its own
    QEMU at import time (module-level side effects) and is not importable."""
    out = []
    for cw_pt, ch_pt in _born_sizes_pt():
        w = qmp_ui.pt(cw_pt)
        h = qmp_ui.pt(TITLEBAR_PT) + qmp_ui.pt(ch_pt)
        for c in range(6):
            x = qmp_ui.pt(110 + c * 28)
            y = qmp_ui.pt(70 + c * 28)
            if x + w > qmp_ui.SCREEN_W:
                x = qmp_ui.SCREEN_W - w
            if x < 0:
                x = 0
            if y + h > qmp_ui.SCREEN_H - qmp_ui.pt(4):
                y = qmp_ui.SCREEN_H - qmp_ui.pt(4) - h
            if y < qmp_ui.pt(MENUBAR_PT):
                y = qmp_ui.pt(MENUBAR_PT)
            p = (x, y + qmp_ui.pt(TITLEBAR_PT))
            if p not in out:
                out.append(p)
    return out


def _band_has_caret(ppm, cx, cy):
    """True if the caret's exact colour appears in this candidate's field band.

    Rows TABH+7 .. TABH+7+16, x from TEXT_X (an empty bar's caret sits at
    ucaret=0, x=14) to 590 pt in -- a restored session can leave text in the
    bar, which moves the caret right by 8 px per character, and 590 pt covers
    a 71-character URL without scanning into another window's content."""
    x0 = cx + qmp_ui.pt(TEXT_X_PT)
    x1 = min(cx + qmp_ui.pt(590), qmp_ui.SCREEN_W - 1)
    y0 = cy + qmp_ui.pt(TABH_PT + CARET_DY_PT)
    y1 = y0 + qmp_ui.pt(CARET_H_PT) - 1
    target = bytes(CARET_RGB)
    row = ppm.w * 3
    for y in range(max(0, y0), min(ppm.h, y1 + 1)):
        base = y * row
        seg = ppm.px[base + x0 * 3: base + (x1 + 1) * 3]
        k = seg.find(target)
        while k >= 0:
            if k % 3 == 0:
                return True
            k = seg.find(target, k + 1)
    return False


def field_point(origin):
    """The click target: the URL field's centre for a known client origin."""
    cx, cy = origin
    return (cx + qmp_ui.pt(300), cy + qmp_ui.pt(TABH_PT + (FIELD_DY_PT[0] + FIELD_DY_PT[1]) // 2))


def locate(ui, ppm_path=None, timeout=12.0):
    """Find the Browser's client origin from its boot caret, or die.

    Returns (x, y). Dying rather than returning None is the whole point: a
    driver that cannot say where the address bar is must not go on typing.
    Polls for `timeout` seconds first: the ~3 MB browser.aex paints seconds
    after the [wm] launched line, and a first-paint race is not the lie this
    module polices -- dying on it would make every gate using it flaky."""
    if ppm_path is None:
        fd, ppm_path = tempfile.mkstemp(prefix="qmp_addrbar-", suffix=".ppm")
        os.close(fd)
    from qmp_ui import PPM
    deadline = time.time() + timeout
    while True:
        ui.screendump(ppm_path, settle=0.4)
        ppm = PPM(ppm_path)
        for cx, cy in client_candidates():
            if _band_has_caret(ppm, cx, cy):
                print("qmp_addrbar: caret found -- Browser client origin "
                      "(%d, %d), URL field click at %r (%s)"
                      % (cx, cy, field_point((cx, cy)), ppm_path))
                return (cx, cy)
        if time.time() >= deadline:
            break
        time.sleep(1.0)
    die("the Browser's address-bar caret (browser.c draw_address_bar, "
        "rgb(90,150,240)) is not on screen at any of the %d client origins "
        "wm.c's cascade can produce after %.0fs -- either the Browser is not "
        "up, is not booted into editing mode, or the chrome geometry in this "
        "file has drifted from c/apps/browser/browser.c. Screendump kept at "
        "%s. Typing now would go to the page, not the bar."
        % (len(client_candidates()), timeout, ppm_path))


def focus(ui, origin=None, ppm_path=None, lie=False):
    """Click the address bar and return the client origin for later calls.

    THE CLICK IS MADE LOAD-BEARING. The Browser boots with `editing = 1`, so
    on a fresh boot ANY click -- the real one, the retired (420, 145), or no
    click at all -- is followed by typing that lands in the bar anyway; the
    click is decoration and no post-hoc pixel check can tell the three apart
    (measured, not assumed: the first version of this module's typed-echo
    control was run with QMP_ADDRBAR_LIE=1 and stayed GREEN on the retired
    coordinate, which is the lie demonstrated rather than refuted). So the
    FIRST focus() unfocuses the bar first -- one click into the blank boot
    viewport, which sets editing = 0 -- and only then clicks the field. If
    that click misses, the typing goes to the page and typed_echo() dies
    before Enter can be blamed. On later navigations Enter has already
    cleared editing, so the click is load-bearing with no help needed and
    the viewport is NOT clicked (a loaded page can have links).

    `origin`  a previously located origin, for navigations after the first
              (the caret is only painted while editing, so it cannot be
              re-found once a page has loaded; the window does not move).
    `lie`     THE NEGATIVE CONTROL: click the retired (420, 145) instead, so
              the typed_echo check can be watched dying on a coordinate this
              tree believed for months. Never set it in a real gate. Also
              armed by QMP_ADDRBAR_LIE=1 in the environment, which is how the
              control is run without editing the driver:
                  QMP_ADDRBAR_LIE=1 make test-webapi-page   # MUST fail"""
    lie = lie or os.environ.get("QMP_ADDRBAR_LIE") == "1"
    if origin is None:
        origin = locate(ui, ppm_path)
        # Unfocus first: one click into the blank new-tab viewport (dead
        # centre, ~200 pt below the bar) so the field click below has to be
        # the thing that restores focus. The boot page has no links to hit;
        # loaded pages never take this branch -- see the docstring.
        ui.click_at(origin[0] + qmp_ui.pt(300),
                    origin[1] + qmp_ui.pt(TABH_PT + BARH_PT + 200))
        time.sleep(0.4)
    x, y = (420, 145) if lie else field_point(origin)
    if lie:
        print("qmp_addrbar: SABOTAGE (negative control): clicking the retired "
              "(420, %d) instead of the field at %r" % (y, field_point(origin)))
    ui.click_at(x, y)
    time.sleep(0.3)
    return origin


def typed_echo(ui, origin, ppm_path=None, min_dark=30):
    """AFTER typing, BEFORE Enter: assert the glyphs are IN the field.

    This is the half the caret cannot cover (a click does not repaint, so
    focus is not visible until a key goes somewhere). gui_text paints the URL
    at window-local (14, TABH+7) in rgb(40, 40, 48); an empty band means the
    keystrokes never reached the bar. Honest only after the bar was cleared --
    every driver here clears with backspaces before typing, and backspace on
    an empty bar is a no-op, so the count is this typing's, not last URL's."""
    if ppm_path is None:
        fd, ppm_path = tempfile.mkstemp(prefix="qmp_addrbar-echo-", suffix=".ppm")
        os.close(fd)
    ui.screendump(ppm_path, settle=0.4)
    from qmp_ui import PPM
    ppm = PPM(ppm_path)
    cx, cy = origin
    # x span capped at 450 pt: WIN_MIN_W is 480, so the field's right edge is
    # never further left than that, and counting past it would read page ink.
    box = (cx + qmp_ui.pt(12), cy + qmp_ui.pt(TABH_PT + FIELD_DY_PT[0]),
           cx + qmp_ui.pt(450), cy + qmp_ui.pt(TABH_PT + FIELD_DY_PT[1]))
    dark = ppm.dark_pixels(box)
    if dark < min_dark:
        die("the typed URL is NOT in the address bar (%d dark glyph pixels in "
            "the field band, need >= %d) -- the keystrokes went somewhere "
            "other than the URL field. Screendump kept at %s"
            % (dark, min_dark, ppm_path))
    print("qmp_addrbar: typed-echo ok -- %d dark pixels in the field band" % dark)
    return dark
