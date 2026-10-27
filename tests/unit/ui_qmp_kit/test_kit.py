#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the UI Python kit (ui-qmp-kit): qmp_aim, qmp_ui title alternates,
tools/ui_tour.py bookkeeping, tools/ui_shotdiff.py, tools/ui_glyph_check.py.

    python3 -I test_kit.py --root <worktree>            positive: prints `ok <check>`
    python3 -I test_kit.py --root <worktree> --negctl   control: feeds the three
        tools one deliberate violation each and relays their FAIL lines
        (`FAIL glyph U+1F9E0 ..`, `FAIL shot-diff ..`, `FAIL english Apply ..`);
        exits 1 like a failing gate. It exits 3 if a tool did NOT fail, so the
        Makefile can tell a dead control from a working one.

Runs under `python3 -I` (no cwd/script-dir imports); every path is explicit.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("--root", required=True)
ap.add_argument("--negctl", action="store_true")
ARGS = ap.parse_args()
ROOT = os.path.abspath(ARGS.root)
sys.path.insert(0, os.path.join(ROOT, "tests/qmp"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from PIL import Image  # noqa: E402

PY = sys.executable
GLYPH = [PY, "-I", os.path.join(ROOT, "tools/ui_glyph_check.py")]
SHOTDIFF = [PY, "-I", os.path.join(ROOT, "tools/ui_shotdiff.py")]
fails = []


def check(name, cond, detail=""):
    if cond:
        print("ok %s" % name)
    else:
        fails.append(name)
        print("FAIL %s %s" % (name, detail))


def run(cmd, cwd=None, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=env)
    return r.returncode, r.stdout + r.stderr


class FakeUI:
    """Just enough Session for Aims / window_of: a growing serial text."""

    def __init__(self, text=""):
        self.text = text

    def serial_text(self):
        return self.text

    def screendump(self, path, settle=0.0):
        Image.new("RGB", (self.w, self.h), self.fill).save(path, format="PPM")
        return path

    w, h, fill = 64, 40, (10, 20, 30)


# --------------------------------------------------------------------------
# qmp_aim: spliced lines, newest wins, timeouts, screen coordinates

def test_aims():
    import qmp_aim
    import qmp_ui
    text = (
        "[wm] launched Settings\n"
        "[settings] aim apply 100 200\n"
        "[wm] win 3 frame 400 100 800 644 content 720 560 pt zoom 0 min 0 Settings\n"
        # a WM line that interrupted an aim write: the aim lands after unrelated bytes
        "[wm] ptr 10 20[settings] aim apply 110 210\n"
        "[clock] aim ring 5 6\n"
        "[settings] aim navigation 84 146\n"
        "[wm] aim dock-2 640 760\n")
    ui = FakeUI(text)
    aims = qmp_aim.Aims(ui, scale=100)
    check("aim-newest-wins", aims.wait("settings", "apply", 1) == (110, 210),
          repr(aims.get("settings", "apply")))
    check("aim-per-app-key", aims.wait("clock", "ring", 1) == (5, 6))
    check("aim-wm", aims.wm("dock-2", 1) == (640, 760))
    check("aim-spliced-line-found", aims.get("settings", "navigation") == (84, 146))
    # screen(): origin = frame x, frame y + frame h - pt(content h); scale 100
    # frame h 644 - content 560 = 84... use the driver formula exactly
    sx, sy = aims.screen("settings", "apply", 1)
    check("aim-screen-100", (sx, sy) == (400 + 110, 100 + 644 - 560 + 210), repr((sx, sy)))
    # 150 %: content 720x560 pt -> frame 1080 wide, content area 840 px tall
    text150 = text.replace("frame 400 100 800 644", "frame 600 150 1080 900")
    aims150 = qmp_aim.Aims(FakeUI(text150), scale=150)
    sx, sy = aims150.screen("settings", "apply", 1)
    check("aim-screen-150", (sx, sy) == (600 + 110 * 150 // 100,
                                         150 + 900 - 560 * 150 // 100 + 210 * 150 // 100),
          repr((sx, sy)))
    # timeout names the app, the aim and the last 5 aim lines
    many = "".join("[a%d] aim n%d %d %d\n" % (i, i, i, i) for i in range(8))
    try:
        qmp_aim.Aims(FakeUI(many)).wait("settings", "ghost", 0.3, poll=0.05)
        check("aim-timeout-raises", False, "no exception")
    except qmp_aim.AimTimeout as e:
        msg = str(e)
        check("aim-timeout-names", "settings" in msg and "ghost" in msg, msg)
        check("aim-timeout-last5", "[a7] aim n7 7 7" in msg and "[a3] aim n3 3 3" in msg
              and "[a2] aim n2 2 2" not in msg, msg)
    # screen() with no window says so
    try:
        qmp_aim.Aims(FakeUI("[browser] aim go 1 2\n")).screen("browser", "go", 0.2)
        check("aim-screen-nowindow", False, "no exception")
    except qmp_aim.AimTimeout as e:
        check("aim-screen-nowindow", "browser" in str(e), str(e))


# --------------------------------------------------------------------------
# qmp_ui: title alternates and window_of

def test_titles():
    import qmp_ui
    t = qmp_ui.APP_TITLES
    check("titles-all-13", len(t) == 13 and all(isinstance(v, tuple) for v in t.values()),
          repr(sorted(t)))
    check("titles-english-first", t["files"][0] == "Finder" and t["files"][1] == "文件管理")
    check("title_of-element0", qmp_ui.title_of("files") == "Finder"
          and qmp_ui.title_of("terminal.aex") == "Terminal"
          and qmp_ui.title_of("assistant") == "Tasks"
          and qmp_ui.title_of("monitor") == "Monitor")
    check("titles_of-both", qmp_ui.titles_of("files") == ("Finder", "文件管理")
          and "活动监视器" in qmp_ui.titles_of("monitor")
          and "Activity Monitor" in qmp_ui.titles_of("monitor")
          and qmp_ui.titles_of("文件管理") == ("Finder", "文件管理"))
    check("titles_of-free-form", qmp_ui.titles_of("Sign in") == ("Sign in",))
    try:
        qmp_ui.title_of("nosuchapp")
        check("title_of-unknown-raises", False)
    except AssertionError:
        check("title_of-unknown-raises", True)

    def win(n, title, app=None, x=0):
        line = ("[wm] win %d frame %d 40 600 400 content 600 370 pt zoom 0 min 0 %s\n"
                % (n, x, title))
        return line + ("[wm] win %d app %s\n" % (n, app) if app else "")

    old = win(0, "Finder") + win(1, "Settings", x=100)
    new = win(0, "文件管理") + win(1, "设置", x=100)
    for label, txt in (("english", old), ("chinese", new)):
        check("window_of-%s-title" % label,
              (qmp_ui.window_of("files", txt) or {}).get("slot") == 0
              and (qmp_ui.window_of("settings", txt) or {}).get("slot") == 1)
    # both spellings match whatever the title is (rename one app at a time)
    mixed = win(0, "Finder") + win(1, "设置", x=100)
    check("window_of-mixed", qmp_ui.window_of("Finder", mixed)["slot"] == 0
          and qmp_ui.window_of("设置", mixed)["slot"] == 1
          and qmp_ui.window_of("settings", mixed)["slot"] == 1)
    # the app line wins over a misleading title
    app_line = win(0, "设置", app="files") + win(1, "Settings", app="settings", x=100)
    check("window_of-app-line-wins",
          qmp_ui.window_of("files", app_line)["slot"] == 0
          and qmp_ui.window_of("settings", app_line)["slot"] == 1)
    check("window_of-missing", qmp_ui.window_of("clock", old) is None)
    # skip and newest
    two = win(0, "Finder") + win(2, "Terminal", x=50) + win(4, "Terminal", x=80)
    check("window_of-first-vs-newest",
          qmp_ui.window_of("terminal", two)["slot"] == 2
          and qmp_ui.window_of("terminal", two, newest=True)["slot"] == 4)
    check("window_of-skip", qmp_ui.window_of("terminal", two, skip=("terminal",)) is None)
    gone = two + "[wm] win 2 gone\n"
    check("window_of-gone", qmp_ui.window_of("terminal", gone)["slot"] == 4)
    check("title_matches", qmp_ui.title_matches("设置", "settings")
          and qmp_ui.title_matches("Settings", "settings")
          and not qmp_ui.title_matches("Clock", "settings"))

    # qmp_window.win_by_title keeps its behaviour and gains the alternates
    import qmp_window
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "serial.log")
        with open(p, "w") as fh:
            fh.write(win(0, "文件管理") + win(1, "Terminal", x=30) + win(2, "未命名", x=60))
        check("win_by_title-alt", qmp_window.win_by_title(p, "Finder")["slot"] == 0
              and qmp_window.win_by_title(p, "文件管理")["slot"] == 0)
        check("win_by_title-prefix", qmp_window.win_by_title(p, "Term")["slot"] == 1)
        check("win_by_title-empty-skips-finder", qmp_window.win_by_title(p, "")["slot"] == 1)
        check("win_by_title-none", qmp_window.win_by_title(p, "Clock") is None)
        check("windows-keeps-fields",
              set(("x", "y", "w", "h", "cw", "ch", "zoom", "min", "title"))
              <= set(qmp_window.windows(p)[0]))


# --------------------------------------------------------------------------
# ui_tour: argument validation, naming, manifest, lock

def test_tour():
    import ui_tour
    import qmp_aim
    import qmp_ui

    def bad(argv):
        try:
            ui_tour.parse_args(argv)
        except SystemExit:
            return True
        return False
    check("tour-rejects-stage", bad(["--out", "/x", "--stages", "desktop,nope"]))
    check("tour-rejects-midshot", bad(["--out", "/x", "--midshot", "spin:100"])
          and bad(["--out", "/x", "--midshot", "open"]))
    a = ui_tour.parse_args(["--out", "/x", "--midshot", "open:120,theme:300",
                            "--stages", "desktop gallery", "--theme", "both"])
    check("tour-args", a.mid == [("open", 120), ("theme", 300)]
          and a.stages == ["desktop", "gallery"] and a.theme == "both"
          and a.jpeg == 88 and a.disk.endswith("build/tour.img"))
    check("tour-stage-list", ui_tour.STAGES == [
        "desktop", "menus", "about", "spotlight", "finder", "settings", "apps",
        "gallery", "widgets", "zoomed", "notify", "theme"], repr(ui_tour.STAGES))

    with tempfile.TemporaryDirectory() as d:
        a = ui_tour.parse_args(["--out", os.path.join(d, "o"), "--wt", ROOT,
                                "--xres", "2560", "--yres", "1600", "--rt",
                                "--reduce-motion"])
        t = ui_tour.Tour(a, qmp_ui, qmp_aim.Aims, Image)
        t.ui = FakeUI("[wm] win 1 frame 10 20 300 200 content 300 170 pt zoom 0 min 0 Clock\n")
        t.stage, t.theme = "desktop", "dark"
        t.shot()
        t.stage = "apps"
        t.shot("clock")
        names = sorted(f for f in os.listdir(a.out) if f.endswith(".jpg"))
        check("tour-shot-names", names == ["01-desktop-dark-2x-rt-rm.jpg",
                                           "02-apps-clock-dark-2x-rt-rm.jpg"], repr(names))
        t.write_manifest()
        rows = [l.split("\t") for l in open(os.path.join(a.out, "manifest.tsv")).read().splitlines()]
        import hashlib
        sha = hashlib.sha256(open(os.path.join(a.out, names[0]), "rb").read()).hexdigest()
        check("tour-manifest", rows[0] == ["name", "sha256", "xres", "yres", "theme", "flags",
                                           "git", "clockbox"]
              and rows[1][:7][1] == sha and rows[1][2:5] == ["2560", "1600", "dark"]
              and rows[1][5] == "2x,rt,rm" and len(rows) == 3
              and rows[1][7] == "10,20,300,200", repr(rows))
        t.kill()
        qmp_ui.configure(1280, 800)

    # the lock: takes the dir, releases it, and --no-lock touches nothing
    with tempfile.TemporaryDirectory() as d:
        ui_tour.LOCK_ROOT = d
        lk = ui_tour.Lock(True)
        lk.acquire()
        held = sorted(os.listdir(d))
        lk.release()
        check("tour-lock", held == [".qemu-lock", ".qemu-slot-1", ".qemu-slot-2", ".qemu-slot-3"]
              and os.listdir(d) == [], repr((held, os.listdir(d))))
        ui_tour.Lock(False).acquire()
        check("tour-no-lock", os.listdir(d) == [])


# --------------------------------------------------------------------------
# ui_shotdiff

def test_shotdiff():
    with tempfile.TemporaryDirectory() as d:
        def img(name, edits=(), size=(200, 120)):
            im = Image.new("RGB", size, (40, 90, 160))
            for (x, y, c) in edits:
                im.putpixel((x, y), c)
            p = os.path.join(d, name)
            im.save(p)
            return p
        a = img("a.png")
        same = img("same.png")
        one = img("one.png", [(50, 60, (40, 90, 175))])
        out = os.path.join(d, "out")
        rc, txt = run(SHOTDIFF + [a, same, "--out", out, "--expect-identical"])
        check("shotdiff-identical-0", rc == 0 and "changed=0" in txt and "FAIL" not in txt, txt)
        rc, txt = run(SHOTDIFF + [a, one, "--out", out, "--expect-identical"])
        check("shotdiff-one-pixel-1", rc == 1 and "changed=1 max=15 bbox=50,60,51,61" in txt
              and "FAIL shot-diff" in txt, txt)
        rc, txt = run(SHOTDIFF + [a, one, "--out", out])
        check("shotdiff-report-only-0", rc == 0 and "changed=1" in txt, txt)
        check("shotdiff-diff-image", os.path.exists(os.path.join(out, "one.jpg")))
        rc, txt = run(SHOTDIFF + [a, one, "--out", out, "--mask", "40,50,20,20",
                                  "--expect-identical"])
        check("shotdiff-mask", rc == 0 and "changed=0" in txt, txt)
        # --mask-clock: a change in the right 260x24 pt of the menu bar is hidden at 1x...
        sz = (1280, 800)
        c0 = img("c0.png", size=sz)
        c1 = img("c1.png", [(1100, 10, (255, 255, 255))], size=sz)
        c2 = img("c2.png", [(500, 10, (255, 255, 255))], size=sz)
        rc, txt = run(SHOTDIFF + [c0, c1, "--out", out, "--mask-clock", "--expect-identical"])
        check("shotdiff-mask-clock", rc == 0, txt)
        rc, txt = run(SHOTDIFF + [c0, c2, "--out", out, "--mask-clock", "--expect-identical"])
        check("shotdiff-mask-clock-not-everything", rc == 1 and "changed=1" in txt, txt)
        # ...and at 2x the strip is 520x48 device px
        big = (2560, 1600)
        b0 = img("b0.png", size=big)
        b1 = img("b1.png", [(2200, 40, (255, 255, 255))], size=big)
        rc, txt = run(SHOTDIFF + [b0, b1, "--out", out, "--mask-clock", "--expect-identical"])
        check("shotdiff-mask-clock-2x", rc == 0, txt)
        # directories: matched by stem, a missing partner is a difference, the manifest's clockbox masks a window
        da, db = os.path.join(d, "da"), os.path.join(d, "db")
        os.makedirs(da)
        os.makedirs(db)
        Image.new("RGB", (300, 200), (1, 2, 3)).save(os.path.join(da, "01-x.bmp"))
        im = Image.new("RGB", (300, 200), (1, 2, 3))
        im.putpixel((120, 90), (250, 250, 250))
        im.save(os.path.join(db, "01-x.png"))
        Image.new("RGB", (10, 10)).save(os.path.join(db, "02-only.png"))
        rc, txt = run(SHOTDIFF + [da, db, "--out", out, "--expect-identical"])
        check("shotdiff-dirs", rc == 1 and "01-x changed=1" in txt and "02-only only-in-B" in txt, txt)
        with open(os.path.join(db, "manifest.tsv"), "w") as fh:
            fh.write("name\tclockbox\n01-x.png\t100,80,50,30\n")
        os.unlink(os.path.join(db, "02-only.png"))
        rc, txt = run(SHOTDIFF + [da, db, "--out", out, "--mask-clock", "--expect-identical"])
        check("shotdiff-clockbox", rc == 0, txt)
        rc, txt = run(SHOTDIFF + [a, os.path.join(d, "nope.png"), "--out", out])
        check("shotdiff-missing-input-2", rc == 2, txt)


# --------------------------------------------------------------------------
# ui_glyph_check

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def test_glyph():
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "fix.c")
        write(src, '''#include "weird⌘.h"
/* aui_button(0, 0, "Apply ignored in comment", 1); */
void draw(void) {
    aui_label(10, 10, "⌘K ⌥ ⇧ ↩", AUI_TEXT);   // from text.ttf
    aui_label(10, 30, "设置", AUI_TEXT);                     // ui.ttf
    aui_label(10, 50, "LogitOS 12 KB 40 px UTF-8 %d%%", AUI_TEXT);
    aui_text(10, 60, 14, "file.txt /bin/ls my_var", AUI_TEXT);
    aui_button(10, 70, "应用", 1);
    strcmp(name, "Apply");
    printf("Hello world\\n");
    aui_label(10, 90, "\\xe2\\x8c\\x98", AUI_TEXT);
    char c = '"';
    aui_label(10, 100, "·", AUI_TEXT);
}
''')
        rc, txt = run(GLYPH + ["--english", src])
        lines = txt.splitlines()
        check("glyph-cmd-resolves-text-ttf", "U+2318 text.ttf" in lines, txt)
        check("glyph-cjk-resolves-ui-ttf", "U+8BBE ui.ttf" in lines and "U+7F6E ui.ttf" in lines, txt)
        check("glyph-escape-bytes", sum(1 for l in lines if l.startswith("U+2318")) == 1, txt)
        check("glyph-clean-fixture", rc == 0 and not any(l.startswith("FAIL") for l in lines), txt)
        check("english-allowlist-and-code", "FAIL english" not in txt, txt)

        bad = os.path.join(d, "bad.c")
        write(bad, 'void f(void){\n    aui_button(1, 2, "Apply", 1);\n    aui_label(1, 2, "\U0001f9e0 ok", 1);\n'
                   '    gui_create("Terminal", 1, 2);\n    aui_label(1, 2, "文字 Cancel", 1);\n}\n')
        rc, txt = run(GLYPH + ["--english", bad])
        check("english-flags-apply", rc == 1 and "FAIL english Apply %s:2" % bad in txt, txt)
        check("english-flags-gui_create", "FAIL english Terminal %s:4" % bad in txt, txt)
        check("english-flags-mixed", "FAIL english Cancel %s:5" % bad in txt, txt)
        check("glyph-flags-emoji", "FAIL glyph U+1F9E0 %s:3" % bad in txt, txt)
        rc, txt = run(GLYPH + [bad])
        check("glyph-without-english-flag", "FAIL english" not in txt and "FAIL glyph U+1F9E0" in txt, txt)

        # --diff REV: only literals on added lines, under c/
        repo = os.path.join(d, "repo")
        os.makedirs(os.path.join(repo, "c/apps"))
        write(os.path.join(repo, "c/apps/a.c"), 'void a(void){\n    aui_label(0,0,"Old English", 1);\n}\n')
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@t")
        run(["git", "init", "-q", repo], env=env)
        run(["git", "-C", repo, "add", "-A"], env=env)
        rc, txt = run(["git", "-C", repo, "commit", "-q", "-m", "base"], env=env)
        write(os.path.join(repo, "c/apps/a.c"),
              'void a(void){\n    aui_label(0,0,"Old English", 1);\n    aui_button(0,0,"Fresh", 1);\n}\n')
        rc, txt = run(GLYPH + ["--english", "--diff", "HEAD", "--root", repo])
        check("diff-only-added-lines", rc == 1 and "FAIL english Fresh c/apps/a.c:3" in txt
              and "Old" not in txt, txt)

        # NOTE: the spec's example U+1F600 is NOT a violation: DejaVuSans (text.ttf)
        # carries it, so the renderer draws it. The control uses U+1F9E0, which
        # no face in the chain has.
        # the chain is read from text.c: it contains ui.ttf and text.ttf in that order
        import ui_glyph_check
        names = [n for n, _ in ui_glyph_check.font_chain(ROOT)]
        check("glyph-chain-from-text.c", names[:2] == ["ui.ttf", "text.ttf"], repr(names))
        cm = ui_glyph_check.Cmap(dict(ui_glyph_check.font_chain(ROOT))["text.ttf"])
        check("cmap-reader", cm.has(0x2318) and cm.has(ord("A")) and not cm.has(0x1F9E0) and cm.has(0x1F600))


# --------------------------------------------------------------------------

def negctl():
    """One deliberate violation per tool; relay each FAIL line, exit 1."""
    out = []
    broken = []
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "neg.c")
        write(c, 'void f(void){\n    aui_label(1, 2, "\U0001f9e0", 1);\n    aui_button(1, 2, "Apply", 1);\n}\n')
        rc, txt = run(GLYPH + ["--english", c])
        if rc != 1:
            broken.append("ui_glyph_check passed on a U+1F9E0 / 'Apply' fixture (rc=%d)" % rc)
        out.append(txt)
        a = os.path.join(d, "a.png")
        b = os.path.join(d, "b.png")
        Image.new("RGB", (64, 48), (9, 9, 9)).save(a)
        im = Image.new("RGB", (64, 48), (9, 9, 9))
        im.putpixel((3, 4), (10, 9, 9))
        im.save(b)
        rc, txt = run(SHOTDIFF + [a, b, "--out", os.path.join(d, "o"), "--expect-identical"])
        if rc != 1:
            broken.append("ui_shotdiff --expect-identical passed on a one-pixel difference (rc=%d)" % rc)
        out.append(txt)
    sys.stdout.write("".join(out))
    for m in broken:
        print("NEGCTL BROKEN: " + m)
    return 3 if broken else 1


def main():
    if ARGS.negctl:
        return negctl()
    test_aims()
    test_titles()
    test_tour()
    test_shotdiff()
    test_glyph()
    print("ui-qmp-kit: %d check(s) failed" % len(fails) if fails else "ui-qmp-kit: all checks ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
