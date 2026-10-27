#!/usr/bin/env python3
"""SYS_GUI_TEXT_RUN2 on a real guest: are aui's run2 labels actually drawn?

aui sends struct logit_run2 in 24.8 points (aui_text.inc: px * 256). A kernel
that reads x/y/px as integer points refuses every such run with OL_ARGUMENT and
the label is simply ABSENT -- the control is drawn, its text is not. Neither
host gate could see it (the aui harness answers run2 itself), so this looks at
the frame: it boots the test-aui disk, opens Gallery exactly as qmp_gallery.py
does, and counts the INK inside each label's rect. Ink is a pixel whose colour
is more than INK_DELTA (per channel) away from the rect's own dominant colour
-- the control's fill -- so a label-less segment, select or table cell counts
~0 and any real label counts well over MIN_INK.

  Gallery Controls   the three segmented labels (aui_text_in) and the select's
                     current item (aui_text_ellipsis)
  Gallery Shapes     the four badges (aui_text_in)
  Gallery Data       the list's and the table's first cells, and the table
                     headers (aui_text_ellipsis; these fit their columns, so
                     they take the legacy run and guard the page rather than
                     run2 -- only a CUT cell goes through run2)
  Gallery Overlays   the menu bar's first title and the modal sheet's title
                     (aui_text_in)
  Monitor Processes  the sort segmented control's three labels

Every rect is window-local points out of c/apps/gui/gallery/gallery.c and
c/apps/gui/monitor/monitor.c (worked through below), located through each
page's probe square exactly as qmp_gallery.py / qmp_monitor.py do.

Lines: `ok text-present <where> ...` / `FAIL text-present <where> ...` per label.
`ok harness ...` / `FAIL harness ...` for the things the measurement itself
depends on (the app opened on the right page; the LEGACY-path tab labels next
to the run2 labels have ink, so a zero count is about run2 and not about the
measuring). The negative control (a kernel with -DUI_RUN2_NEGCTL) must produce
`FAIL text-present` and no `FAIL harness`. Exit 1 on any FAIL.

Usage: qmp_ui_run2.py --iso ISO --disk DISK [--out DIR] [--xres N --yres N] [--keep]
"""
import collections
import os
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, pt, PPM  # noqa: E402
from qmp_gallery import PROBES as GALLERY_PROBES, ppm_to_png, which_page  # noqa: E402

MIN_INK = 20       # device px of ink a label must show (the spec's floor)
INK_DELTA = 48     # per-channel distance from the rect's dominant colour

MONITOR_PROBE = (255, 0, 128)    # monitor.c: the Processes tab's probe colour

# ---- Gallery, window-local points (gallery.c) ------------------------------
# frame(): x = AUI_PAD = 16, y = 92, w = WINW - 2*AUI_PAD = 720 - 32 = 688.
# page_controls(): col2 = 16 + 350 = 366. Column 1 ends at y = 334 (92 +20
# +28+8 +28+8 +44 +20 +26 +30 +30), so y2 = 334 - 200 = 134, and then
# +22 +30 +34 +18 +24 +42 +20 +36 +36 = 396 for aui_segmented(366, 396, 220, 28)
# and +38 = 434 for aui_select(366, 434, 220, ...).
SEG_X, SEG_Y, SEG_W, SEG_H, SEG_N = 366, 396, 220, 28, 3
SEG_LABELS = ("Day", "Week", "Year")
SEL_X, SEL_Y, SEL_W, SEL_H = 366, 434, 220, 28            # shows fruit[2] "Compositor"
# page_shapes(): y = 92 +24 +128 +22 +110 +22 = 398; badges at x 16, +84,
# +176, +250 (aui_badge: h = 12 + 8 + 2 = 22, w = text + 16).
BADGE_Y = 398
BADGES = (("success", 16), ("warning", 16 + 84), ("error", 16 + 176), ("accent", 16 + 250))
# page_data(): y = 92 + 20 = 112. aui_list(16, 112, 300, 240); aui_table(356,
# 112, 328, 240) with a 26-point header and column widths 150/96/68. Rows are
# list_body's ROWH = 26 inside aui_scroll_begin(x + 1, y + 1); cell text starts
# AUI_SP(2) = 8 in.
LIST_X, LIST_Y = 16, 112
TAB_X, TAB_Y, TAB_HDR = 356, 112, 26
COLW = (150, 96, 68)
HEADERS = ("Widget", "State", "Cost")
ROWS = (("Button", "hover", "9 calls"), ("Checkbox", "on", "7 calls"))
LIST_ROWS = ("Anchor", "Bitmap")
ROWH = 26
# page_overlay(): aui_menubar(16, 92, 648, ...) is 28 high and its first title
# "File" starts at 16 + AUI_SP(2) = 24 in a tw + 16 wide cell (text from ~32).
# "Open dialog" is aui_button_ex(16, 92 + 40 + 24 = 156, 150, 32). The sheet is
# aui_dialog_begin("Delete the volume?", 380, 170) in a 720 x 560 window:
# x = (720 - 380) / 2 = 170, y = (560 - 170 - 40) / 3 = 116, a 40-high title
# band with the 20-point title centred on x = 360.
MENU_FILE = (24 + 8, 92 + 6, 18, 16)
OPEN_DIALOG = (16 + 75, 156 + 16)
DIALOG_TITLE = (360 - 60, 116 + 10, 120, 20)
# aui_tabs(16, 44, ...): the LEGACY text path (aui_text_sz), the harness's own
# proof that ink is measurable. "Controls" starts at 16 + AUI_SP(3) = 28.
TABS_LABEL = (28, 44 + 8, 50, 18)

# ---- Activity Monitor (monitor.c) ------------------------------------------
# view_processes(16, 96, ...): aui_segmented(16 + 56, 96 - 6, 210, AUI_H_SM 22).
MSEG_X, MSEG_Y, MSEG_W, MSEG_H, MSEG_N = 72, 90, 210, 22, 3
MSEG_LABELS = ("PID", "名称", "文件数")


class Win:
    """A screendump plus a window origin from a probe colour."""

    def __init__(self, path, rgb):
        self.p = PPM(path)
        box = self.p.find_color(rgb)
        self.ok = box is not None
        self.ox = self.oy = 0
        if self.ok:
            self.ox, self.oy = box[0] - pt(4), box[1] - pt(4)

    def ink(self, x, y, w, h):
        """(ink pixel count, dominant colour) inside a window-local pt rect."""
        px = []
        for j in range(pt(y), pt(y + h)):
            for i in range(pt(x), pt(x + w)):
                X, Y = self.ox + i, self.oy + j
                if 0 <= X < self.p.w and 0 <= Y < self.p.h:
                    px.append(self.p.at(X, Y))
        if not px:
            return 0, None
        bg = collections.Counter(px).most_common(1)[0][0]
        n = sum(1 for c in px if max(abs(c[k] - bg[k]) for k in range(3)) > INK_DELTA)
        return n, bg


def main(argv):
    xres, yres, out, keep = 1280, 800, None, False
    iso = disk = None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--iso":    iso = argv[i + 1]; i += 2
        elif a == "--disk": disk = argv[i + 1]; i += 2
        elif a == "--out":  out = argv[i + 1]; i += 2
        elif a == "--xres": xres = int(argv[i + 1]); i += 2
        elif a == "--yres": yres = int(argv[i + 1]); i += 2
        elif a == "--keep": keep = True; i += 1
        else:
            print("unknown arg %r" % a); return 2
    if not iso or not disk:
        print(__doc__); return 2
    configure(xres, yres)
    slow = max(1.0, (xres * yres) / (1280.0 * 800.0))
    tmp = tempfile.mkdtemp(prefix="logit-run2-")
    if out:
        os.makedirs(out, exist_ok=True)
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
    fails = []

    def ck(cond, what, detail=""):
        print("%s %s%s" % ("ok" if cond else "FAIL", what, ("  " + detail) if detail else ""))
        if not cond:
            fails.append(what)
        return cond

    def keep_png(src, name):
        if out:
            ppm_to_png(src, os.path.join(out, name + ".png"))

    def label(win, where, rect):
        n, bg = win.ink(*rect)
        ck(n > MIN_INK, "text-present %s" % where, "[ink %d dev px > %d; fill %s]" % (n, MIN_INK, bg))

    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (xres, yres),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 180
        while time.time() < deadline:
            if os.path.exists(serial) and "desktop live" in open(serial, errors="replace").read():
                break
            if qemu.poll() is not None:
                print("FAIL harness qemu exited early"); return 1
            time.sleep(0.2)
        else:
            print("FAIL harness desktop never came up"); return 1
        time.sleep(3 * slow)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        # --- Gallery, opened exactly as qmp_gallery.py opens it ----------------
        ui.launch_app("gallery", probe=probe)
        time.sleep(6 * slow)
        ui.goto(xres - pt(60), pt(60))
        time.sleep(1.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        page = which_page(shot)
        if not ck(page == "controls", "harness Gallery opened on Controls", "[page=%r]" % page):
            keep_png(shot, "gallery-unknown")
            return 1
        keep_png(shot, "gallery-controls")
        g = Win(shot, GALLERY_PROBES["controls"])
        print("     Gallery content origin at device (%d,%d)" % (g.ox, g.oy))
        n, _ = g.ink(*TABS_LABEL)
        ck(n > MIN_INK, "harness legacy tab label has ink", "[ink %d]" % n)

        seg = SEG_W // SEG_N
        for k, name in enumerate(SEG_LABELS):
            label(g, "gallery-segmented-%d %s" % (k, name),
                  (SEG_X + k * seg + 6, SEG_Y + 5, seg - 12, SEG_H - 10))
        label(g, "gallery-select Compositor", (SEL_X + 8, SEL_Y + 5, SEL_W - 70, SEL_H - 10))

        # --- Shapes, then Data: the tab strip's own keyboard path --------------
        # (qmp_gallery.py's move: click the strip to focus it, then arrow right.)
        ui.click_at_confirmed(probe, g.ox + pt(50), g.oy + pt(60))
        time.sleep(1.0 * slow)
        ui.goto(g.ox + pt(700), g.oy + pt(20))
        time.sleep(1.0 * slow)
        ui.key("right"); time.sleep(1.6 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        page = which_page(shot)
        if ck(page == "shapes", "harness Gallery shows Shapes", "[page=%r]" % page):
            keep_png(shot, "gallery-shapes")
            s = Win(shot, GALLERY_PROBES["shapes"])
            for name, bx in BADGES:
                label(s, "gallery-badge %s" % name, (bx + 6, BADGE_Y + 4, 28, 14))

        ui.key("right"); time.sleep(1.6 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        page = which_page(shot)
        if ck(page == "data", "harness Gallery shows Data", "[page=%r]" % page):
            keep_png(shot, "gallery-data")
            d = Win(shot, GALLERY_PROBES["data"])
            cx = TAB_X + 8
            for c, name in enumerate(HEADERS):
                label(d, "gallery-table-header-%d %s" % (c, name),
                      (cx + 1, TAB_Y + 5, min(COLW[c] - 12, 44), TAB_HDR - 10))
                cx += COLW[c]
            body_y = TAB_Y + TAB_HDR + 1
            for r, row in enumerate(ROWS):
                cx = TAB_X + 1 + 8
                for c, name in enumerate(row):
                    label(d, "gallery-table-r%dc%d %s" % (r, c, name),
                          (cx + 1, body_y + r * ROWH + 5, min(COLW[c] - 12, 44), ROWH - 10))
                    cx += COLW[c]
            for r, name in enumerate(LIST_ROWS):
                label(d, "gallery-list-r%d %s" % (r, name),
                      (LIST_X + 1 + 8 + 1, LIST_Y + 1 + r * ROWH + 5, 40, ROWH - 10))

        ui.key("right"); time.sleep(1.6 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        page = which_page(shot)
        if ck(page == "overlay", "harness Gallery shows Overlays", "[page=%r]" % page):
            keep_png(shot, "gallery-overlay")
            o = Win(shot, GALLERY_PROBES["overlay"])
            label(o, "gallery-menubar File", MENU_FILE)
            ui.click_at_confirmed(probe, o.ox + pt(OPEN_DIALOG[0]), o.oy + pt(OPEN_DIALOG[1]))
            time.sleep(1.6 * slow)
            ui.goto(o.ox + pt(700), o.oy + pt(20))
            time.sleep(1.0 * slow)
            ui.screendump(shot, settle=1.0 * slow)
            keep_png(shot, "gallery-dialog")
            dlg = Win(shot, GALLERY_PROBES["overlay"])     # the scrim dims the probe:
            dlg.ok, dlg.ox, dlg.oy = True, o.ox, o.oy      # keep the pre-click origin
            label(dlg, "gallery-dialog-title Delete the volume?", DIALOG_TITLE)

        # --- Activity Monitor ----------------------------------------------------
        # Gallery is on Overlays now (probe 0,160,255), so the Processes probe
        # (255,0,128) can only be Monitor's.
        ui.launch_app("monitor", probe=probe)
        time.sleep(6 * slow)
        ui.goto(xres - pt(60), pt(60))
        time.sleep(1.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        m = Win(shot, MONITOR_PROBE)
        if ck(m.ok, "harness Monitor opened on Processes"):
            keep_png(shot, "monitor-processes")
            mseg = MSEG_W // MSEG_N
            for k, name in enumerate(MSEG_LABELS):
                label(m, "monitor-segmented-%d %s" % (k, name),
                      (MSEG_X + k * mseg + 6, MSEG_Y + 3, mseg - 12, MSEG_H - 6))
    except Exception as ex:          # a driver failure is never a text verdict
        traceback.print_exc()
        print("FAIL harness %s: %s" % (type(ex).__name__, ex))
        return 1
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        if keep:
            print("     artefacts in %s" % tmp)

    if fails:
        print("\n%d check(s) failed:" % len(fails))
        for f in fails:
            print("  - %s" % f)
        return 1
    print("\nall run2 text checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
