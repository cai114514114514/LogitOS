#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The UI tour: boot the tour image and photograph the desktop and every app.

    ui_tour.py --wt <worktree> --iso build/logit.iso --disk build/tour.img \\
               --out <dir> [--stages S,S..] [--apps a,b..] [--theme light|dark|both]
               [--xres 1280 --yres 800] [--rt] [--reduce-motion]
               [--midshot action:ms[,..]] [--jpeg 88] [--no-lock]

Build the image first:  make BUILD=build all tour-disk   (release content +
Gallery + Widgets, no /etc/qmp-probes; Gallery and Widgets are not on the
product or release disk and never will be).

Stages (default: all, in this order):
  desktop menus about spotlight finder settings apps gallery widgets zoomed
  notify theme
`apps` photographs --apps (default: clock monitor preview studio assistant
terminal textedit browser). `theme` launches two windows and toggles the theme
UNDER them ("theme toggled after launch": everything must follow EV_THEME).

Shots: <out>/<NN>-<stage>[-<sub>]-<light|dark>[-2x][-rt][-rm].jpg  plus
<out>/manifest.tsv (name sha256 xres yres theme flags git clockbox). `clockbox`
is the Clock window's frame in device pixels when one was open (ui_shotdiff
--mask-clock masks it) and `-` otherwise.

--rt / --reduce-motion flip the Spotlight accessibility rows (Reduce
transparency / Reduce motion) before the first stage and confirm the change
from the serial log (`[spotlight] open kind=3`) or a pixel change.
--midshot takes a photo <ms> milliseconds after triggering the action, for
animations: theme open close minimise zoom spotlight.

Interactions use aims (`[app] aim <name> x y`, tests/qmp/qmp_aim.py) when the
guest prints them, else the survey coordinates (window-relative points).
Windows are found through qmp_ui.window_of, so Chinese and English titles both
match. A failed step prints `STEP FAILED <name> ...`, the run goes on, and the
exit status is 1 at the end. QEMU is always killed. Unless --no-lock the run
holds ~/LogitOS-wt/.qemu-lock AND the three .qemu-slot-N dirs (timing run).
"""
import argparse
import atexit
import csv
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
STAGES = ["desktop", "menus", "about", "spotlight", "finder", "settings", "apps",
          "gallery", "widgets", "zoomed", "notify", "theme"]
DEFAULT_APPS = ["clock", "monitor", "preview", "studio", "assistant", "terminal",
                "textedit", "browser"]
SLOW_APPS = ("browser", "studio")
MIDSHOTS = ("theme", "open", "close", "minimise", "zoom", "spotlight")
LOCK_ROOT = os.path.expanduser("~/LogitOS-wt")
DARK_TOGGLE_PT = (240, 19)      # design 8: W - S(240) + S(19), menu bar centre


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--wt", default=os.path.dirname(HERE))
    ap.add_argument("--iso")
    ap.add_argument("--disk")
    ap.add_argument("--xres", type=int, default=1280)
    ap.add_argument("--yres", type=int, default=800)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stages", default=",".join(STAGES))
    ap.add_argument("--apps", default=",".join(DEFAULT_APPS))
    ap.add_argument("--theme", choices=("light", "dark", "both"), default="light")
    ap.add_argument("--rt", action="store_true")
    ap.add_argument("--reduce-motion", action="store_true")
    ap.add_argument("--midshot", default="")
    ap.add_argument("--jpeg", type=int, default=88)
    ap.add_argument("--no-lock", action="store_true")
    a = ap.parse_args(argv)
    a.wt = os.path.abspath(os.path.expanduser(a.wt))
    a.iso = os.path.abspath(a.iso or os.path.join(a.wt, "build/logit.iso"))
    a.disk = os.path.abspath(a.disk or os.path.join(a.wt, "build/tour.img"))
    a.out = os.path.abspath(os.path.expanduser(a.out))
    a.stages = [s for s in a.stages.replace(" ", ",").split(",") if s]
    for s in a.stages:
        if s not in STAGES:
            ap.error("unknown stage %r (have: %s)" % (s, " ".join(STAGES)))
    a.apps = [s for s in a.apps.split(",") if s]
    a.mid = []
    for item in [m for m in a.midshot.split(",") if m]:
        act, _, ms = item.partition(":")
        if act not in MIDSHOTS or not ms.isdigit():
            ap.error("bad --midshot %r (want action:ms, action in %s)" % (item, "/".join(MIDSHOTS)))
        a.mid.append((act, int(ms)))
    return a


# --------------------------------------------------------------------------
# the QEMU lock (timing run: .qemu-lock plus all three functional slots)

class Lock:
    def __init__(self, enabled):
        self.enabled = enabled
        self.held = []

    def _take(self, path):
        while True:
            try:
                os.mkdir(path)
                self.held.append(path)
                return
            except FileExistsError:
                time.sleep(3)

    def acquire(self):
        if not self.enabled:
            return
        os.makedirs(LOCK_ROOT, exist_ok=True)
        self._take(os.path.join(LOCK_ROOT, ".qemu-lock"))
        for s in (1, 2, 3):
            self._take(os.path.join(LOCK_ROOT, ".qemu-slot-%d" % s))

    def release(self):
        while self.held:
            try:
                os.rmdir(self.held.pop())
            except OSError:
                pass


# --------------------------------------------------------------------------

class Tour:
    def __init__(self, a, qmp_ui, Aims, Image):
        self.a, self.ui_mod, self.Image = a, qmp_ui, Image
        self.scale = qmp_ui.configure(a.xres, a.yres)
        self.XR, self.YR = a.xres, a.yres
        self.slow = max(1.0, a.xres * a.yres / (1280.0 * 800))
        self.pt = qmp_ui.pt
        self.tmp = tempfile.mkdtemp(prefix="uitour-")
        self.seq = 0
        self.failed = []
        self.shots = []          # manifest rows
        self.stage_shots = {}
        self.theme = "light"
        self.stage = ""
        self.qemu = None
        self.stdin = None
        self.git = self._git_sha()
        self.Aims = Aims
        os.makedirs(a.out, exist_ok=True)

    # -- plumbing ----------------------------------------------------------
    def log(self, *x):
        print(time.strftime("%H:%M:%S"), *x, flush=True)

    def _git_sha(self):
        try:
            r = subprocess.run(["git", "-C", self.a.wt, "rev-parse", "--short=12", "HEAD"],
                               capture_output=True, text=True)
            return r.stdout.strip() or "unknown"
        except OSError:
            return "unknown"

    def boot(self):
        a = self.a
        sock = os.path.join(self.tmp, "qmp.sock")
        self.serial = os.path.join(self.tmp, "serial.log")
        self.serfh = open(self.serial, "wb")
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-cdrom", a.iso,
             "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % a.disk,
             "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
             "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
             "-rtc", "base=localtime", "-vga", "none",
             "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (a.xres, a.yres),
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-serial", "stdio", "-no-reboot", "-display", "none",
             "-qmp", "unix:%s,server,nowait" % sock],
            stdin=subprocess.PIPE, stdout=self.serfh, stderr=subprocess.DEVNULL)
        end = time.time() + 300
        while time.time() < end:
            if "desktop live" in self.serial_text():
                break
            if self.qemu.poll() is not None:
                raise RuntimeError("qemu exited early")
            time.sleep(0.25)
        else:
            raise RuntimeError("guest never reported a live desktop")
        self.log("desktop live; settling")
        time.sleep(25 * self.slow)
        self.ui = self.ui_mod.Session(sock, serial=self.serial)
        self.aims = self.Aims(self.ui)

    def kill(self):
        if self.qemu is not None and self.qemu.poll() is None:
            self.qemu.kill()
            try:
                self.qemu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.qemu = None

    def serial_text(self):
        try:
            with open(self.serial, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def sh(self, line):
        self.qemu.stdin.write((line + "\n").encode())
        self.qemu.stdin.flush()

    def step(self, name, fn):
        try:
            fn()
        except Exception as e:  # noqa: BLE001 -- the tour carries on and fails at the end
            self.failed.append(name)
            self.log("STEP FAILED", name, repr(e))
            traceback.print_exc()
            try:
                self.ui.key("esc")
            except Exception:  # noqa: BLE001
                pass

    # -- shots -------------------------------------------------------------
    def flags(self):
        f = []
        if self.scale == 200:
            f.append("2x")
        elif self.scale != 100:
            f.append("%dpct" % self.scale)
        if self.a.rt:
            f.append("rt")
        if self.a.reduce_motion:
            f.append("rm")
        return f

    def _clockbox(self):
        w = self.ui_mod.window_of("clock", self.ui, newest=True)
        if w and not w["min"]:
            return "%d,%d,%d,%d" % (w["x"], w["y"], w["w"], w["h"])
        return "-"

    def shot(self, sub="", settle=1.0):
        self.seq += 1
        parts = ["%02d" % self.seq, self.stage] + ([sub] if sub else []) + [self.theme] + self.flags()
        name = "-".join(parts)
        ppm = os.path.join(self.tmp, name + ".ppm")
        self.ui.screendump(ppm, settle=settle)
        box = self._clockbox()
        img = self.Image.open(ppm).convert("RGB")
        path = os.path.join(self.a.out, name + ".jpg")
        img.save(path, quality=self.a.jpeg)
        os.unlink(ppm)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        self.shots.append((name + ".jpg", digest, self.XR, self.YR, self.theme,
                           ",".join(self.flags()) or "-", self.git, box))
        self.stage_shots[self.stage] = self.stage_shots.get(self.stage, 0) + 1
        self.log("shot", name)
        return img

    def raw(self):
        """A throwaway screendump as a PIL image (not a tour shot)."""
        self.seq_raw = getattr(self, "seq_raw", 0) + 1
        ppm = os.path.join(self.tmp, "raw%d.ppm" % self.seq_raw)
        self.ui.screendump(ppm, settle=0.6)
        img = self.Image.open(ppm).convert("RGB")
        os.unlink(ppm)
        return img

    def write_manifest(self):
        with open(os.path.join(self.a.out, "manifest.tsv"), "w", newline="") as fh:
            w = csv.writer(fh, delimiter="\t", lineterminator="\n")
            w.writerow(["name", "sha256", "xres", "yres", "theme", "flags", "git", "clockbox"])
            w.writerows(self.shots)

    # -- interaction helpers ----------------------------------------------
    def rest(self):
        self.ui.goto(self.XR - self.pt(60), self.YR - self.pt(300), settle=0.4)

    def closeapp(self):
        self.ui.key_mods(("meta_l",), "w", settle=1.0)
        time.sleep(1.5)

    def launch(self, app, wait=6):
        self.ui.launch_app(app, timeout=120 * self.slow, allow_focus=True)
        time.sleep(wait * self.slow)
        self.rest()
        time.sleep(0.8)

    def win(self, app):
        w = self.ui_mod.window_of(app, self.ui, newest=True)
        if w is None:
            raise AssertionError("no window for %s in the serial log" % app)
        return w

    def click_aim_or(self, app, names, dx, dy, settle=0.4):
        """Click an aim of `app` (first of `names` the guest has printed), else
        the survey point (dx, dy) in points from the window frame's top-left."""
        for n in ([names] if isinstance(names, str) else names):
            if self.aims.get(app, n) is not None:
                x, y = self.aims.screen(app, n, timeout=2.0)
                self.ui.click_at(x, y, settle=settle)
                return "aim:" + n
        w = self.win(app)
        self.ui.click_at(w["x"] + self.pt(dx), w["y"] + self.pt(dy), settle=settle)
        return "survey"

    def menubar_sig(self, img):
        """Mean RGB of a menu-bar strip away from the clock and the logo."""
        x0, x1 = self.XR // 3, self.XR // 2
        y0, y1 = self.pt(4), self.pt(20)
        px = img.crop((x0, y0, x1, y1)).resize((1, 1), self.Image.BOX).getpixel((0, 0))
        return px

    def toggle_theme(self):
        before = self.menubar_sig(self.raw())
        self.ui.click_at(self.XR - self.pt(DARK_TOGGLE_PT[0]) + self.pt(DARK_TOGGLE_PT[1]),
                         (self.pt(24) - self.pt(18)) // 2 + self.pt(9), settle=0.5)
        time.sleep(3 * self.slow)
        self.rest()
        time.sleep(1)
        after = self.menubar_sig(self.raw())
        if max(abs(p - q) for p, q in zip(before, after)) < 6:
            raise AssertionError("theme toggle changed no pixel of the menu bar (%r -> %r)"
                                 % (before, after))
        self.theme = "dark" if self.theme == "light" else "light"

    def spot_open(self, wait=6):
        self.ui.key_mods(("meta_l",), "spc", settle=0.5)
        time.sleep(wait * self.slow)

    def spot_launch(self, q):
        self.spot_open()
        self.ui.typ(q)
        time.sleep(3)
        self.ui.key("ret")
        time.sleep(12 * self.slow)
        self.rest()

    def accessibility(self, query, what):
        """Flip a Spotlight toggle row; confirm from serial or a pixel change."""
        before_img = self.raw()
        mark = len(self.serial_text())
        self.spot_open()
        self.ui.typ(query)
        time.sleep(3)
        self.ui.key("ret")
        time.sleep(3 * self.slow)
        self.ui.key("esc")
        time.sleep(1.5)
        self.rest()
        time.sleep(1)
        got_serial = "[spotlight] open kind=3" in self.serial_text()[mark:]
        from PIL import ImageChops
        changed = ImageChops.difference(before_img, self.raw()).getbbox() is not None
        if not (got_serial or changed):
            raise AssertionError("%s: no [spotlight] open kind=3 line and no pixel change" % what)
        self.log("accessibility", what, "serial" if got_serial else "pixels")

    # -- stages ------------------------------------------------------------
    def st_desktop(self):
        self.step("desktop", lambda: (self.rest(), self.shot()))

    def menu_titles(self):
        t = {}
        for l in self.serial_text().splitlines():
            m = re.search(r"\[wm\] menu title (\d+) (\S+) x0 (-?\d+) y0 (-?\d+) x1 (-?\d+) y1 (-?\d+)", l)
            if m:
                t[int(m.group(1))] = (m.group(2),) + tuple(int(m.group(i)) for i in range(3, 7))
        return t

    def st_menus(self):
        titles = self.menu_titles()
        if not titles:
            raise AssertionError("the guest printed no [wm] menu title lines")
        for i in sorted(titles):
            def f(i=i):
                x0, y0, x1, y1 = titles[i][1:]
                self.ui.click_at((x0 + x1) // 2, (y0 + y1) // 2, settle=0.6)
                time.sleep(1)
                self.shot("%d" % i)
                self.ui.key("esc")
                time.sleep(0.8)
            self.step("menu%d" % i, f)

    def st_about(self):
        titles = self.menu_titles()
        x0, y0, x1, y1 = titles[0][1:]
        self.ui.click_at((x0 + x1) // 2, (y0 + y1) // 2, settle=0.6)
        time.sleep(1)
        self.ui.click_at(x0 + self.pt(40), y1 + self.pt(22), settle=0.6)
        time.sleep(2)
        self.rest()
        self.shot("panel")
        self.ui.key("esc")
        time.sleep(1)
        self.ui.click_at(self.XR // 2, self.YR - self.pt(40), settle=0.5)

    def st_spotlight(self):
        def empty():
            self.spot_open(8)
            self.shot("empty")
            self.ui.typ("set")
            time.sleep(3)
            self.shot("query")
            self.ui.key("esc")
            time.sleep(2)
        self.step("spotlight", empty)

        def nomatch():
            self.spot_open(5)
            self.ui.typ("zzqq")
            time.sleep(3)
            self.shot("nomatch")
            self.ui.key("esc")
            time.sleep(2)
        self.step("spotlight-nomatch", nomatch)

    def st_finder(self):
        self.launch("files", 5)
        self.shot("default")
        # survey points (window-relative pt); aim names are tried first
        clicks = [("sidebar-0", 56, 141), ("sidebar-1", 80, 197), ("sidebar-2", 80, 225),
                  ("row-0", 240, 95), ("view-list", 570, 56), ("view-icons", 636, 56)]
        for k, (aim, dx, dy) in enumerate(clicks):
            self.click_aim_or("files", aim, dx, dy)
            time.sleep(2.5 * self.slow)
            self.rest()
            self.shot("click%d" % k)
        self.closeapp()

    def st_settings(self):
        self.launch("settings", 5)
        self.shot("1")
        for i in range(1, 8):
            # panes are the sidebar rows; the survey stepped 26 pt per row
            self.ui.click_at(self.win("settings")["x"] + self.pt(84),
                             self.win("settings")["y"] + self.pt(146 + 26 * i), settle=0.4)
            time.sleep(2 * self.slow)
            self.rest()
            self.shot("%d" % (i + 1))
        self.closeapp()

    def st_apps(self):
        for app in self.a.apps:
            def f(app=app):
                self.launch(app, 20 if app in SLOW_APPS else 6)
                self.shot(app)
                self.closeapp()
            self.step("app-" + app, f)

    def hidden(self, query, sub):
        self.spot_launch(query)
        self.shot(sub)
        self.closeapp()

    def st_gallery(self):
        self.hidden("Gallery", "window")

    def st_widgets(self):
        self.hidden("Widgets", "window")

    def st_zoomed(self):
        def fin():
            self.launch("files", 5)
            self.click_aim_or("files", "zoom", 52, 15, settle=0.6)
            time.sleep(3 * self.slow)
            self.rest()
            self.shot("finder")
            self.closeapp()
        self.step("zoomed-finder", fin)

        def brw():
            self.launch("browser", 15)
            self.click_aim_or("browser", "zoom", 52, 15, settle=0.6)
            time.sleep(4 * self.slow)
            self.rest()
            self.shot("browser")
            self.closeapp()
        self.step("zoomed-browser", brw)

    def st_notify(self):
        for _ in range(10):
            self.sh("echo shell-is-up")
            end = time.time() + 12
            while time.time() < end and self.serial_text().count("shell-is-up") < 2:
                time.sleep(0.2)
            if self.serial_text().count("shell-is-up") >= 2:
                break
        else:
            raise AssertionError("the serial shell never answered")

        def posted(n):
            end = time.time() + 15
            while time.time() < end:
                if self.serial_text().count("[wm] notify post") > n:
                    return
                time.sleep(0.1)
            raise AssertionError("the kernel never saw the notification")

        seen = self.serial_text().count("[wm] notify post")
        self.sh('notify "下载完成" "logit.iso 已保存到下载文件夹" 0')
        posted(seen)
        time.sleep(0.8)
        self.shot("single")
        time.sleep(9)
        seen = self.serial_text().count("[wm] notify post")
        self.sh("notify --burst 4")
        posted(seen + 2)
        time.sleep(0.8)
        self.shot("stack")
        time.sleep(10)

    def st_theme(self):
        """Launch two windows, toggle the theme UNDER them, shoot, toggle back."""
        self.launch("files", 5)
        self.launch("settings", 5)
        self.toggle_theme()
        self.shot("toggled")
        self.toggle_theme()
        self.closeapp()
        self.closeapp()

    # -- midshots ----------------------------------------------------------
    def midshots(self):
        self.stage = "mid"
        for act, ms in self.a.mid:
            def f(act=act, ms=ms):
                sub = "%s-%dms" % (act, ms)
                if act == "theme":
                    self.ui.click_at(self.XR - self.pt(DARK_TOGGLE_PT[0]) + self.pt(DARK_TOGGLE_PT[1]),
                                     (self.pt(24) - self.pt(18)) // 2 + self.pt(9), settle=0.0)
                    time.sleep(ms / 1000.0)
                    self.shot(sub, settle=0.0)
                    time.sleep(3 * self.slow)
                    self.theme = "dark" if self.theme == "light" else "light"
                    self.rest()
                elif act == "open":
                    x, y = self.ui.dock_icon_of("settings")
                    self.ui.click_at(x, y, settle=0.0)
                    time.sleep(ms / 1000.0)
                    self.shot(sub, settle=0.0)
                    time.sleep(5 * self.slow)
                    self.rest()
                    self.closeapp()
                elif act in ("close", "minimise", "zoom"):
                    self.launch("settings", 4)
                    if act == "close":
                        self.ui.key_mods(("meta_l",), "w", settle=0.0)
                    elif act == "minimise":
                        self.ui.key_mods(("meta_l",), "m", settle=0.0)
                    else:
                        w = self.win("settings")
                        self.ui.click_at(w["x"] + self.pt(52), w["y"] + self.pt(15), settle=0.0)
                    time.sleep(ms / 1000.0)
                    self.shot(sub, settle=0.0)
                    time.sleep(3 * self.slow)
                    self.rest()
                    if act != "close":
                        self.closeapp()
                elif act == "spotlight":
                    self.ui.key_mods(("meta_l",), "spc", settle=0.0)
                    time.sleep(ms / 1000.0)
                    self.shot(sub, settle=0.0)
                    time.sleep(2)
                    self.ui.key("esc")
                    time.sleep(2)
            self.step("mid-" + act, f)

    # -- driver ------------------------------------------------------------
    def run_stages(self):
        for s in self.a.stages:
            self.stage = s
            self.log("== stage", s, self.theme)
            if s == "menus":
                self.step("menus", self.st_menus)
            elif s in ("desktop",):
                self.st_desktop()
            elif s == "spotlight":
                self.st_spotlight()
            elif s == "zoomed":
                self.st_zoomed()
            else:
                self.step(s, getattr(self, "st_" + s))
        if self.a.mid:
            self.midshots()

    def run(self):
        a = self.a
        self.boot()
        if a.rt:
            self.stage = "setup"
            self.step("rt", lambda: self.accessibility("transparency", "reduce transparency"))
        if a.reduce_motion:
            self.stage = "setup"
            self.step("reduce-motion", lambda: self.accessibility("motion", "reduce motion"))
        themes = ["light", "dark"] if a.theme == "both" else [a.theme]
        for t in themes:
            if self.theme != t:
                self.stage = "setup"
                self.step("theme-" + t, self.toggle_theme)
                # the first pass closed the boot Finder; the second pass starts
                # from the same one-window desktop
                self.step("reopen-finder", lambda: self.launch("files", 5))
            self.run_stages()
        self.write_manifest()
        # every requested stage must have produced a shot in every theme
        for s in a.stages:
            if self.stage_shots.get(s, 0) < len(themes):
                self.failed.append("stage-" + s)
                self.log("STEP FAILED", "stage-" + s,
                         "produced %d shot(s), expected at least %d"
                         % (self.stage_shots.get(s, 0), len(themes)))
        self.log("TOUR DONE: %d shots in %s, %d failed step(s)"
                 % (len(self.shots), a.out, len(self.failed)))
        return 1 if self.failed else 0


def main(argv=None):
    a = parse_args(argv if argv is not None else sys.argv[1:])
    for p, what in ((a.iso, "--iso"), (a.disk, "--disk")):
        if not os.path.exists(p):
            print("ui_tour: %s %s does not exist (make BUILD=build all tour-disk)" % (what, p),
                  file=sys.stderr)
            return 2
    qdir = os.path.join(a.wt, "tests/qmp")
    sys.path.insert(0, qdir)
    import qmp_ui                       # noqa: E402
    from qmp_aim import Aims            # noqa: E402
    from PIL import Image               # noqa: E402

    lock = Lock(not a.no_lock)
    tour = None

    def bye(*_):
        raise SystemExit(130)
    signal.signal(signal.SIGTERM, bye)
    signal.signal(signal.SIGINT, bye)
    atexit.register(lock.release)
    lock.acquire()
    rc = 1
    try:
        tour = Tour(a, qmp_ui, Aims, Image)
        rc = tour.run()
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001
        print("STEP FAILED tour %r" % (e,), flush=True)
        traceback.print_exc()
        rc = 1
    finally:
        if tour is not None:
            tour.kill()
            if tour.shots:
                tour.write_manifest()
            shutil.rmtree(tour.tmp, ignore_errors=True)
        lock.release()
    return rc


if __name__ == "__main__":
    sys.exit(main())
