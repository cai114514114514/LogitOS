#!/usr/bin/env python3
"""Shared QMP desktop-driving helpers: dock geometry, pointer, keyboard,
screendumps, and PPM inspection.

This exists because the dock geometry silently rotted once already. The dock is
CENTRED, so adding one app moves every icon 32 px left and a stale coordinate
lands on the wallpaper -- which does nothing at all and looks exactly like the
app failing to open. One copy of that arithmetic, imported by every driver, is
the only version of this that stays true.
"""

import collections
import json
import os
import re
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
# NAPPS, BROWSER_SLOT, GALLERY_SLOT and SETTINGS_SLOT lived here until
# 2026-08-30 -- four hand-maintained integers restating what wm.c's scan_apps()
# builds at boot. NAPPS went stale the day settings.aex was packed (10 -> 11,
# every dock coordinate half a slot off, every driver green or silent), which
# is why the parse_dock()/dock_icon_of() half of this file exists. Every caller
# now reads the slot AND the count off the guest's [wm] dock line; dock_icon()'s
# `n` is a REQUIRED argument so that nothing can quietly reintroduce a default.


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


def dock_icon(i, n):
    """Centre of dock icon `i` (0-based) in DEVICE pixels, matching draw_dock().

    THE ARITHMETIC HERE IS CORRECT AND STAYS -- it is draw_dock()'s. `n` is the
    app count, and it is REQUIRED on purpose: it used to default to NAPPS, a
    constant maintained by hand in step with a disk image, and that default is
    what went stale the day settings.aex was packed. Callers get `n` from
    len(dock) -- the guest's own [wm] dock line -- or via dock_icon_of(), which
    does both lookups in one call."""
    isz, gap = pt(DOCK_ISZ_PT), pt(DOCK_GAP_PT)
    dw = gap + n * (isz + gap)
    x0 = (SCREEN_W - dw) // 2
    y0 = SCREEN_H - (isz + pt(20)) - pt(12)
    return (x0 + gap + i * (isz + gap) + isz // 2,
            y0 + pt(10) + isz // 2)


# --- The dock, read from the guest instead of restated -----------------------
#
# The kernel publishes its app registry once, from the function that builds it
# (wm.c's dock_publish(), called inside scan_apps()):
#
#   [wm] dock 11 apps: 0=clock.aex,shown 1=textedit.aex,shown ... 9=gallery.aex,hidden
#
# One line, one token per slot, `slot=file,visibility`, no field containing a
# space. Everything below parses that and nothing below knows an app's index.
#
# WHY THIS EXISTS RATHER THAN ANOTHER CONSTANT. NAPPS/BROWSER_SLOT/
# GALLERY_SLOT/SETTINGS_SLOT above, a name->index dict in qmp_launch_click.py
# and a scatter of bare integers in a dozen drivers are ten spellings of one
# fact that lives in the guest -- CLAUDE.md rule 3 at ten doors. The failure is
# not a crash: the dock is CENTRED, so one app more or fewer moves every icon
# half a slot, and a stale coordinate either lands on the wallpaper (nothing
# happens, and it looks exactly like a slow launch) or -- worse -- lands on the
# NEIGHBOURING app and the driver goes on happily testing the wrong window.
DockEntry = collections.namedtuple("DockEntry", "slot file hidden")

_DOCK_RE = re.compile(r"\[wm\] dock (\d+) apps:([^\n]*)\n")


def parse_dock(text):
    """The guest's dock registry as a list of DockEntry, or None.

    None means "the guest has not said yet", not "the dock is empty" -- callers
    poll. The count the guest declares is checked against the number of tokens
    parsed, and the line must be newline-TERMINATED, because this is normally
    read out of a serial log QEMU is still appending to: half a line read
    mid-write would otherwise parse cleanly as a SHORTER dock, which is the one
    wrong answer that looks right (every icon shifts, nothing errors)."""
    entries = None
    for m in _DOCK_RE.finditer(text):          # the LAST one wins: a boot test
        want = int(m.group(1))                 # may reboot the same machine
        got = []
        for tok in m.group(2).split():
            slot, _, rest = tok.partition("=")
            name, _, vis = rest.partition(",")
            if not slot.isdigit() or not name or vis not in ("shown", "hidden"):
                got = None
                break
            got.append(DockEntry(int(slot), name, vis == "hidden"))
        if got is None or len(got) != want or [e.slot for e in got] != list(range(want)):
            continue
        entries = got
    return entries


def _dock_of(dock):
    """Accept a parsed dock or the raw serial text, so callers can pass either."""
    if isinstance(dock, str):
        parsed = parse_dock(dock)
        if parsed is None:
            raise AssertionError("no complete '[wm] dock N apps: ...' line in the "
                                 "serial log -- did the window manager come up?")
        return parsed
    return dock


def dock_entry(name, dock):
    """The guest's registry entry for an app, by name.

    `name` matches the .aex file the kernel named, with or without the
    extension, case-insensitively: 'files', 'files.aex' and 'Files' are the same
    app. It is the FILE name and not the display name on purpose -- 'Finder' is
    files.aex and 'Code Studio' has a space in it, and a field that can contain
    a space cannot be a token on a serial line."""
    dock = _dock_of(dock)
    key = name.lower()
    if not key.endswith(".aex"):
        key += ".aex"
    hits = [e for e in dock if e.file.lower() == key]
    if not hits:
        raise AssertionError("no dock app named %r; the guest listed %s"
                             % (name, " ".join(e.file for e in dock)))
    if len(hits) > 1:                          # cannot happen on LogitFS (one
        raise AssertionError("%r is ambiguous: %r" % (name, hits))  # name, one file)
    return hits[0]


def dock_slot(name, dock):
    """The slot index the GUEST gave this app. See dock_entry()."""
    return dock_entry(name, dock).slot


def dock_icon_of(name, dock):
    """Centre of an app's dock tile in DEVICE pixels, both numbers from the guest.

    Both, not one: the slot AND the app count come off the same line, so a disk
    with one more or one fewer app moves this coordinate correctly without any
    Python being edited. That is the whole point -- see the block comment
    above, and tests/qmp/qmp_dock_map.py, which watches it happen."""
    dock = _dock_of(dock)
    return dock_icon(dock_slot(name, dock), n=len(dock))


# --- The launch check: did that click launch the app I asked for? -------------
#
# A dock coordinate is an input; what the click DID is a fact the guest
# publishes on its own console:
#
#   [wm] launched Terminal
#
# printed by wm.c's launch path itself, off the display name in the app's .aex
# header, for every launch of every app. The alternative -- click, wait, and
# trust whatever happens to be on screen -- is the exact shape
# tools/check-test-liveness.py records verbatim: five drivers clicked a
# coordinate that had drifted into the WM titlebar and passed for months
# because the click was decoration and the field they wanted happened to start
# focused. A driver that cannot tell which app it opened is worse than a
# driver that fails: the red one gets fixed, the green one gets believed.
#
# THE TWO LINES NAME TWO DIFFERENT FIELDS, and the guest never joins them.
# dock_publish() prints the FILE (terminal.aex); the launch line prints the
# DISPLAY name (Terminal), which the Makefile's mkaex.py calls wrote into the
# .aex header. Joining them in wm.c's dock_publish() would close that gap at
# the source, but wm.c is another workflow's file, so the join lives here --
# ONE table, and every launch_app() call in tests/qmp/ goes through it. The
# table is still a jar-with-doors, but it is a different species from the slot
# constants it replaces: a stale row fails LOUDLY, the first time its app is
# launched, with both spellings in the message. A stale SLOT failed silently
# and clicked the neighbouring app forever.
#
# Derived from the same invocations that write the headers (Makefile, APP_RULE
# and the terminal/preview/browser/gallery_hidden rules):
APP_TITLES = {
    "clock": "Clock", "textedit": "TextEdit", "monitor": "Monitor",
    "terminal": "Terminal", "widgets": "Widgets", "files": "Finder",
    "preview": "Preview", "studio": "Code Studio", "browser": "Browser",
    "gallery": "Gallery", "settings": "Settings",
}

LAUNCH_RE = re.compile(r"\[wm\] launched (.+)")
# An app that is already running is FOCUSED, not launched, and that line names
# no app at all -- see launch_app()'s `allow_focus` for what that costs.
FOCUS_RE = re.compile(r"\[wm\] launch: already live, focusing")


def title_of(name):
    """The display name the guest will print when it launches `name`."""
    key = name.lower()
    if key.endswith(".aex"):
        key = key[:-4]
    if key not in APP_TITLES:
        raise AssertionError(
            "no display name for %r in qmp_ui.APP_TITLES -- add the row the "
            "Makefile's mkaex.py call writes (and qmp_dock_map.py clicks it "
            "once, so the row is checked against the machine)" % name)
    return APP_TITLES[key]


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

    def __init__(self, sock_path, timeout=120.0, serial=None,
                 serial_text_fn=None):
        # `serial` is the guest's console log. Given one, the pointer is read
        # back from the guest's own report instead of guessed from deltas or
        # hunted for in a screenshot -- which is the only method that survives
        # the pointer moving to a display plane.
        #
        # `serial_text_fn` is for drivers whose serial arrives through a SOCKET
        # pumped into memory (audio_term/rich_term/lm_stream/cjk_term): they
        # have no file to point at while the run is live -- their .serial.txt
        # is written only at exit, which is exactly when it stops being useful.
        # `serial` stays the human-readable name for error messages.
        self.serial = serial
        self._serial_text_fn = serial_text_fn
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
        """One shifted key. DELEGATES to key_mods, which is paced.

        THIS FUNCTION USED TO SEND FOUR SCANCODES IN TWO UNPACED BURSTS, and
        it cost this project 24% of the site scoreboard's corpus. The
        docstring below key_mods used to say key_shift "stays as it is -- it
        has callers and there is no reason to churn them". There was a
        reason. It is written down here rather than deleted, because the
        sentence was reasonable and the failure it caused was not visible
        from this file.

        WHAT IT LOOKED LIKE. The PS/2 controller this machine emulates has a
        ONE-BYTE buffer. Sent unpaced, the shift RELEASE is the byte that
        gets dropped, so everything typed after the first shifted key arrives
        shifted -- and the guest does exactly what it was told. From
        tests/scoreboard/0820-g4b/kimi.serial.txt:304, the harness typing
        http://10.0.2.2:5829/sb.html:

            [browser] load: http://10.0.2.2:%*@(?SB>HTML

        `%*@(?` are the shifted forms of `5 8 2 9 /`, `>` is shifted `.`, and
        `HTML` is shifted `html`. Nothing in the browser was wrong.

        WHY IT WAS EXPENSIVE OUT OF ALL PROPORTION. The corrupted URL was the
        harness's own self-test page, so the run was recorded as HARNESS with
        no metrics -- and sites_run.py's merge rule keeps the WORST of two
        runs, so a good measurement sitting in the same pass was discarded.
        Three sites (kimi, openai, weixin) published a row of dashes that way.
        A fourth, deepseek, lost two characters from the SITE url
        (www.deepseek.com -> www.deepseek.c) and was published as a browser
        FETCH-FAIL: an instrument fabricating a finding about the thing it
        exists to measure.

        THE NEGATIVE CONTROL IS AN ENVIRONMENT SWITCH, not a manual revert:
        QMP_UNPACED_SHIFT=1 restores exactly the code that shipped, so the
        corruption can be reproduced on demand by anyone, forever, instead of
        living in a commit message. Run it against the self-test page and the
        `[browser] load:` line comes back shifted.
        """
        if os.environ.get("QMP_UNPACED_SHIFT"):
            # The shipped-until-2026-08-25 version, kept runnable on purpose.
            self._input([{"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": True}},
                         {"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": True}}])
            self._input([{"type": "key", "data": {"key": {"type": "qcode", "data": qcode}, "down": False}},
                         {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": False}}])
            time.sleep(settle)
            return
        self.key_mods(("shift",), qcode, settle=settle)

    def key_mods(self, mods, qcode, settle=0.05):
        """A chord: hold every qcode in `mods`, tap `qcode`, release in reverse.

        key_shift() above IS this with mods=("shift",), and now calls it.
        This docstring used to say key_shift "stays as it is -- it has callers
        and there is no reason to churn them"; read key_shift's own comment
        for what that cost. Release order is
        reversed on purpose: a driver that lets go of ctrl before the letter
        can deliver the letter WITHOUT the modifier, which is a keystroke the
        page was never sent and is very hard to see afterwards.

        Everything here is one-byte PS/2 at heart, so the settle is not
        optional -- see the note in tests/qmp/qmp_term.py about the one-byte
        buffer.
        """
        # PACED, one scancode at a time. A chord is SIX bytes on the wire
        # (ctrl down, alt down, d down, d up, alt up, ctrl up) and the PS/2
        # controller this machine emulates has a ONE-BYTE buffer -- the same
        # fact tests/qmp/qmp_term.py's header records, and the same one that
        # makes a mistyped command look like a broken feature. Sent unpaced,
        # the first version of this helper delivered nothing at all and the
        # browser looked as though it had ignored the chord.
        step = 0.06
        for m in mods:
            self._input([{"type": "key",
                          "data": {"key": {"type": "qcode", "data": m}, "down": True}}])
            time.sleep(step)
        for down in (True, False):
            self._input([{"type": "key",
                          "data": {"key": {"type": "qcode", "data": qcode}, "down": down}}])
            time.sleep(step)
        for m in reversed(list(mods)):
            self._input([{"type": "key",
                          "data": {"key": {"type": "qcode", "data": m}, "down": False}}])
            time.sleep(step)
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

    def guest_dock(self, timeout=0.0):
        """The guest's own dock registry, or None. See parse_dock().

        `timeout` polls, because the line is printed in wm_init() and a driver
        that connects to QMP early can beat it to the log."""
        if not self.serial and self._serial_text_fn is None:
            return None
        deadline = time.time() + timeout
        while True:
            got = parse_dock(self.serial_text())
            if got is not None or time.time() >= deadline:
                return got
            time.sleep(0.2)

    def dock(self, timeout=30.0):
        """guest_dock(), but a missing line is an ERROR rather than None.

        A driver locating a tile has no fallback: without this line it can only
        guess an index, and a wrong guess CLICKS THE NEIGHBOURING APP and goes
        on testing the wrong window rather than failing. Refuse instead."""
        got = self.guest_dock(timeout)
        if got is None:
            raise AssertionError(
                "the guest never published '[wm] dock N apps: ...' (serial=%r). "
                "That line is wm.c dock_publish(); without it a dock coordinate "
                "is a guess." % (self.serial,))
        return got

    def dock_icon_of(self, name, timeout=30.0):
        """Centre of `name`'s dock tile, slot and app-count both from the guest."""
        return dock_icon_of(name, self.dock(timeout))

    def serial_text(self):
        """The whole serial log so far, or "" if this session has none."""
        if self._serial_text_fn is not None:
            return self._serial_text_fn()
        if not self.serial:
            return ""
        try:
            with open(self.serial, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def mark(self):
        """Byte offset of the serial log RIGHT NOW -- pass it to launch_app().

        Only needed by drivers that click tiles through some custom path and
        still want the launch verified; launch_app() takes its own mark."""
        return len(self.serial_text())

    def launched_since(self, mark, timeout=0.0):
        """Launch events the guest reported after `mark`, as display names.

        Polls up to `timeout` seconds for the first one, then returns whatever
        is on the log; a driver that wants its own retry loop around a manual
        click uses this instead of launch_app()."""
        deadline = time.time() + timeout
        while True:
            got = [m.group(1).strip()
                   for m in LAUNCH_RE.finditer(self.serial_text()[mark:])]
            if got or time.time() >= deadline:
                return got
            time.sleep(0.25)

    def launch_app(self, name, title=None, probe=None, timeout=90.0,
                   allow_focus=False):
        """Click `name`'s dock tile and REFUSE to return until the guest says
        the app that came up is `name`'s.

        This is the check that was missing when five drivers spent months
        clicking the window-manager titlebar and passing: the coordinate can
        be perfect and the launch still not happen, or -- the expensive case --
        a NEIGHBOURING app can come up and the driver goes on testing the
        wrong window, green. The guest's own `[wm] launched` line is the only
        referee that cannot lie, because it is printed by the launch path
        itself rather than read off pixels.

        `title`    expected display name; defaults to APP_TITLES[name].
        `probe`    screendump path -- verifies the pointer with
                   click_at_confirmed() before clicking, for drivers whose
                   dock clicks used that.
        `timeout`  how long the launch line may take. The browser under TCG
                   needs tens of seconds; qmp_freeze.py measured the same
                   wait at 90 before this existed.
        `allow_focus`  accept "[wm] launch: already live, focusing" instead of
                   a launch. WEAK BY CONSTRUCTION: that line names no app, so
                   a click that landed on a DIFFERENT already-running app
                   reads the same as the intended one being re-raised. Use it
                   only where the driver intends the single-instance re-raise
                   (the Finder is launched at boot; a Files driver's dock
                   click focuses it), never for a first launch.

        Returns the display name the guest printed. Raises AssertionError
        naming both spellings when a different app launched -- which is what
        a stale slot constant did silently, see the block comment above."""
        if not self.serial and self._serial_text_fn is None:
            raise AssertionError(
                "launch_app(%r) needs the guest's serial log to read the "
                "[wm] launched line -- construct Session(sock, serial=...) or "
                "Session(sock, serial_text_fn=...)" % name)
        dock = self.dock()
        x, y = dock_icon_of(name, dock)
        expected = title or title_of(name)
        mark = self.mark()
        if probe is not None:
            self.click_at_confirmed(probe, x, y)
        else:
            self.click_at(x, y)
        deadline = time.time() + timeout
        while True:
            text = self.serial_text()[mark:]
            launched = [m.group(1).strip() for m in LAUNCH_RE.finditer(text)]
            wrong = [n for n in launched if n != expected]
            if wrong:
                raise AssertionError(
                    "clicked the dock tile for %s (%s) at (%d,%d): the guest "
                    "launched %r -- the click landed on the WRONG APP, which "
                    "is what a stale slot constant did silently"
                    % (name, expected, x, y, wrong))
            if launched:
                return launched[0]
            if FOCUS_RE.search(text):
                if allow_focus:
                    return None
                raise AssertionError(
                    "clicked the dock tile for %s (%s): the guest reports it "
                    "was ALREADY LIVE and only focused it. If this driver "
                    "means the single-instance re-raise, pass allow_focus=True; "
                    "if it means a fresh launch, the app was started by "
                    "something else first." % (name, expected))
            if time.time() >= deadline:
                raise AssertionError(
                    "clicked the dock tile for %s (%s) at (%d,%d): no [wm] "
                    "launched line within %gs -- the click hit nothing (the "
                    "wallpaper or the gap between icons) and looked exactly "
                    "like a slow launch" % (name, expected, x, y, timeout))
            time.sleep(0.25)


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
