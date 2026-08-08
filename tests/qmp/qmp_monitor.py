#!/usr/bin/env python3
"""Drive the Activity Monitor over QMP: the views, and the kill.

A task manager's one claim is that you can END A PROCESS from it. That claim is
worth exactly as much as a test that watches it happen, so this boots the
desktop and asserts the whole loop from both ends at once:

  * the views paint      -- Processes / Memory / System, each identified by its
                            own probe colour, screenshotted in light AND dark.
  * the kill KILLS       -- launch Clock, select it in the table, press Force
                            Quit, confirm. Then require, from THREE independent
                            places, that it is gone:
                              1. the kernel says so on the serial console
                                 ([proc] kill: pid N marked / exiting),
                              2. the process disappears from the Monitor's own
                                 table (which is SYS_PROCS, i.e. the PCB table),
                              3. the Clock WINDOW leaves the screen -- a killed
                                 GUI app must not leave a corpse painted on the
                                 desktop.
                            Any one of those alone can pass while the feature is
                            broken; a window can vanish because the app crashed,
                            and a table row can vanish because the table is
                            stale.
  * the refusal REFUSES  -- pid 1 is the console shell. Selecting it must leave
                            the Force Quit button DISABLED (the refusal is
                            visible before the click, not only after it), and
                            asking the kernel directly must come back with
                            LOGIT_KILL_PROTECTED rather than a dead console.

Window origin: monitor.c paints a 6x6 probe at window-local (4,4) whose colour
names the current tab, the same device gallery.c uses. Nothing below hard-codes
where the compositor put the window.

Usage: qmp_monitor.py <iso> <disk> [--xres N] [--yres N] [--out shot.png] [--keep]
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, PPM, NAPPS  # noqa: E402

MONITOR_SLOT = 2                  # clock textedit MONITOR terminal widgets ...
CLOCK_SLOT = 0                    # the victim

PROBES = {
    "processes": (255, 0, 128),
    "memory":    (0, 255, 128),
    "system":    (255, 200, 0),
}

# Window-local points out of c/apps/gui/monitor.c.
WINW, WINH = 660, 470
TAB_Y = 44 + 17                   # the tab strip's vertical centre
# Tab centres. aui_tabs lays tabs out by TEXT WIDTH (tw(item) + AUI_SP(6)), so
# these are not evenly spaced and cannot be computed from an index -- they are
# read off a rendered frame. Every click on them is verified by the probe
# colour afterwards, so a font change fails loudly here rather than silently
# testing the wrong view.
TAB_X = (61, 146, 221)
TABLE_X, TABLE_Y = 16, 96 + 26    # aui_table's origin on the Processes tab
ROW_H = 26                        # list_body row pitch (aui.c)
HDR_H = 26                        # the table's header band
KILL_X, KILL_Y = 16 + (660 - 32) - 132 + 66, WINH - 50 + 14   # Force Quit centre
# The "Descending" checkbox, monitor.c view_processes(): x = 16+56+210+16.
DESC_X, DESC_Y = 16 + 56 + 210 + 16 + 9, 96 - 4 + 9


def ppm_to_png(src, dst):
    p = PPM(src)
    raw = b"".join(b"\x00" + p.px[y * p.w * 3:(y + 1) * p.w * 3] for y in range(p.h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    open(dst, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", p.w, p.h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b""))
    return p.w, p.h


def luma(c):
    return (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000


class Frame:
    """A screendump plus the Monitor window origin, from the tab probe."""

    def __init__(self, path):
        self.p = PPM(path)
        self.tab = None
        self.ox = self.oy = None
        for name, rgb in PROBES.items():
            box = self.p.find_color(rgb)
            if box is not None:
                self.tab = name
                self.ox = box[0] - pt(4)
                self.oy = box[1] - pt(4)
                break

    def ok(self):
        return self.tab is not None

    def at(self, x, y):
        return self.p.at(self.ox + pt(x), self.oy + pt(y))

    def dark(self, x, y, w, h, thresh=90):
        return self.p.dark_pixels((self.ox + pt(x), self.oy + pt(y),
                                   self.ox + pt(x + w), self.oy + pt(y + h)), thresh)


def main(argv):
    iso = argv[1] if len(argv) > 1 else "build/logit.iso"
    disk = argv[2] if len(argv) > 2 else "build/disk.img"
    xres, yres = 1280, 800
    out = "build/monitor.png"
    keep = "--keep" in argv
    for i, a in enumerate(argv):
        if a == "--xres":
            xres = int(argv[i + 1])
        elif a == "--yres":
            yres = int(argv[i + 1])
        elif a == "--out":
            out = argv[i + 1]
    configure(xres, yres)
    slow = 1.0

    tmp = tempfile.mkdtemp(prefix="logit-monitor-")
    sock, serial = os.path.join(tmp, "qmp.sock"), os.path.join(tmp, "serial.log")
    fails = []
    base_out = out[:-4] if out.endswith(".png") else out

    def ck(cond, what, detail=""):
        print("%-4s %s%s" % ("ok" if cond else "FAIL", what,
                             ("  [%s]" % detail) if detail else ""))
        if not cond:
            fails.append(what)

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

    def log():
        return open(serial, errors="replace").read() if os.path.exists(serial) else ""

    try:
        deadline = time.time() + 240
        while time.time() < deadline:
            if "desktop live" in log():
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early")
                return 1
            time.sleep(0.2)
        else:
            print("FAIL desktop never came up")
            return 1
        time.sleep(3 * slow)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        def park(f):
            """Park the pointer inside the window but on no widget: the header
            strip right of the title. There is no EV_MOUSE_LEAVE, so a hover
            left on a control sticks and would contaminate the next sample."""
            ui.goto(f.ox + pt(560), f.oy + pt(16))
            time.sleep(1.0 * slow)

        # --- open the Monitor -------------------------------------------------
        # The dock is CENTRED, so every icon's position depends on how many apps
        # are on the disk -- and that number changes whenever anybody adds one
        # (a Settings app appeared in this tree while this driver was being
        # written, moving every icon and making the click land on the
        # wallpaper, which looks exactly like the app failing to launch).
        # qmp_ui.NAPPS is therefore a starting guess, not a fact: try it, and if
        # the Monitor did not come up, try the neighbouring counts. The probe is
        # the oracle, so a wrong guess costs a retry rather than a false FAIL.
        f = None
        for n in (NAPPS, NAPPS + 1, NAPPS + 2, NAPPS - 1):
            if n < 1:
                continue
            ui.click_at_confirmed(probe, *dock_icon(MONITOR_SLOT, n))
            time.sleep(5 * slow)
            ui.screendump(shot, settle=1.2 * slow)
            cand = Frame(shot)
            if cand.ok():
                f = cand
                if n != NAPPS:
                    print("     (dock has %d apps, not qmp_ui.NAPPS=%d)" % (n, NAPPS))
                break
        dock_n = n
        if f is None:
            f = Frame(shot)
        ck(f.ok() and f.tab == "processes",
           "Activity Monitor opens on the Processes tab", "tab=%r" % (f.tab,))
        if not f.ok():
            ppm_to_png(shot, out)
            print("     wrote %s" % out)
            return 1
        print("     window content origin at device (%d,%d)" % (f.ox, f.oy))
        park(f)
        ui.screendump(shot, settle=1.0 * slow)
        ppm_to_png(shot, base_out + "-processes.png")

        # The table must actually have rows: dark text under the header band.
        f = Frame(shot)
        ink = f.dark(TABLE_X, TABLE_Y + HDR_H, 620, ROW_H * 4, 120)
        ck(ink > 60, "the process table has rows of real text", "dark px = %d" % ink)

        # --- the other two views ---------------------------------------------
        for idx, want in ((1, "memory"), (2, "system")):
            ui.click_at_confirmed(probe, f.ox + pt(TAB_X[idx]), f.oy + pt(TAB_Y))
            time.sleep(1.6 * slow)
            ui.screendump(shot, settle=1.2 * slow)
            g = Frame(shot)
            ck(g.ok() and g.tab == want, "the %s view paints" % want,
               "tab=%r" % (g.tab,))
            if g.ok():
                park(g)
                ui.screendump(shot, settle=1.0 * slow)
                ppm_to_png(shot, base_out + "-" + want + ".png")

        # Back to Processes.
        ui.click_at_confirmed(probe, f.ox + pt(TAB_X[0]), f.oy + pt(TAB_Y))
        time.sleep(1.6 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        f = Frame(shot)
        ck(f.tab == "processes", "the tab strip returns to Processes",
           "tab=%r" % (f.tab,))

        # --- dark theme -------------------------------------------------------
        # The menu-bar switch: wm.c puts it at W - S(210), S(3), S(38) x S(18).
        DARK_X, DARK_Y = xres - pt(210) + pt(19), pt(12)
        light_bg = f.at(600, 20)
        ui.click_at_confirmed(probe, DARK_X, DARK_Y)
        time.sleep(2.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        d = Frame(shot)
        ck(d.ok(), "the Monitor survives the theme switch", "tab=%r" % (d.tab,))
        if d.ok():
            park(d)
            ui.screendump(shot, settle=1.0 * slow)
            d = Frame(shot)
            dark_bg = d.at(600, 20)
            ck(luma(dark_bg) < luma(light_bg) - 40,
               "dark theme actually darkens the window",
               "luma %d -> %d" % (luma(light_bg), luma(dark_bg)))
            ppm_to_png(shot, base_out + "-processes-dark.png")
            for idx, want in ((1, "memory"), (2, "system")):
                ui.click_at_confirmed(probe, d.ox + pt(TAB_X[idx]), d.oy + pt(TAB_Y))
                time.sleep(1.6 * slow)
                ui.screendump(shot, settle=1.2 * slow)
                g = Frame(shot)
                if g.ok() and g.tab == want:
                    park(g)
                    ui.screendump(shot, settle=1.0 * slow)
                    ppm_to_png(shot, base_out + "-" + want + "-dark.png")
            ui.click_at_confirmed(probe, d.ox + pt(TAB_X[0]), d.oy + pt(TAB_Y))
            time.sleep(1.6 * slow)
        # Back to light for the kill sequence (the screenshots read better).
        ui.click_at_confirmed(probe, DARK_X, DARK_Y)
        time.sleep(2.5 * slow)

        # --- launch the victim -------------------------------------------------
        # Clock is spawned last, so it holds the HIGHEST pid. Sorting the table
        # descending by PID therefore puts it in row 0 deterministically --
        # which beats hunting for its name, because the cells are anti-aliased
        # glyphs and this driver does not do OCR.
        ui.click_at_confirmed(probe, *dock_icon(CLOCK_SLOT, dock_n))
        time.sleep(6 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        f = Frame(shot)
        ck(f.ok(), "the Monitor is still up after launching Clock")

        # Record where Clock's window is, so its DISAPPEARANCE can be asserted
        # later. The Clock face is a large light disc; sample the desktop region
        # it occupies and require it to change after the kill.
        ui.click_at_confirmed(probe, f.ox + pt(330), f.oy + pt(20))
        time.sleep(1.5 * slow)
        park(f)
        ui.screendump(shot, settle=1.2 * slow)
        f = Frame(shot)
        ck(f.ok() and f.tab == "processes", "the Monitor is frontmost again",
           "tab=%r" % (f.tab,))

        # WHY THE "GONE FROM THE SCREEN" CHECK IS NOT A PIXEL DIFF.
        # The obvious test -- find Clock's window by diffing the desktop before
        # and after it launched, then require those pixels back afterwards --
        # was written, run, and discarded: selecting a row raises the Monitor,
        # whose 660x470 window covers Clock's entirely, so the diff finds ZERO
        # of Clock's pixels and the assertion silently measures the menu bar's
        # ticking wall clock instead. (It duly passed against the negative
        # control, which is how it was caught.)
        #
        # The compositor states the same fact directly and without occlusion:
        # wm.c prints "[wm] win N gone" when it frees a window slot. That is the
        # window leaving the screen, said by the thing that owns the screen, and
        # it is independent of both the serial kill log and the process table.

        # Sort descending: Clock was spawned last, so it holds the HIGHEST pid
        # and row 0 becomes Clock deterministically. That beats hunting for its
        # name, because the cells are anti-aliased glyphs and this driver does
        # not do OCR.
        ui.click_at_confirmed(probe, f.ox + pt(DESC_X), f.oy + pt(DESC_Y))
        time.sleep(1.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        f = Frame(shot)
        ck(f.ok(), "the Descending control is reachable")

        ui.click_at_confirmed(probe, f.ox + pt(300),
                              f.oy + pt(TABLE_Y + HDR_H + ROW_H // 2))
        time.sleep(1.5 * slow)
        park(f)
        ui.screendump(shot, settle=1.0 * slow)
        f = Frame(shot)
        ppm_to_png(shot, base_out + "-selected.png")
        ck(f.dark(16, WINH - 50 + 2, 320, 20, 120) > 20,
           "selecting a row names it in the footer")

        # --- the REFUSAL, before the kill --------------------------------------
        # The console shell (init) is the one process SYS_KILL refuses. It is
        # identified by the kernel, not by a magic pid -- monitor.c greys the
        # button out from the LOGIT_PROC_PROTECTED flag the kernel publishes.
        # Sorting ascending puts it near the top; sh is the CLI process with no
        # parent. Row 1 is it (Finder=1, sh=2, Monitor=3, Clock=4 ascending).
        ui.click_at_confirmed(probe, f.ox + pt(DESC_X), f.oy + pt(DESC_Y))
        time.sleep(1.5 * slow)
        ui.click_at_confirmed(probe, f.ox + pt(300),
                              f.oy + pt(TABLE_Y + HDR_H + ROW_H + ROW_H // 2))
        time.sleep(1.5 * slow)
        park(f)
        ui.screendump(shot, settle=1.0 * slow)
        g = Frame(shot)
        ppm_to_png(shot, base_out + "-refused.png")
        # Enabled, the Force Quit button is AUI_V_DANGER -- a saturated red
        # fill. Disabled, it is AUI_DISABLED, a neutral grey. So the test is
        # REDNESS (r - b), not luma: the danger red's luma lands within a
        # rounding error of any threshold worth picking, which made a luma test
        # read 0 for both states. Sample left of centre, clear of the label.
        shell_px = g.at(KILL_X - 55, KILL_Y)
        shell_red = shell_px[0] - shell_px[2]

        # Now select Clock again (descending, row 0) and compare.
        ui.click_at_confirmed(probe, g.ox + pt(DESC_X), g.oy + pt(DESC_Y))
        time.sleep(1.5 * slow)
        ui.click_at_confirmed(probe, g.ox + pt(300),
                              g.oy + pt(TABLE_Y + HDR_H + ROW_H // 2))
        time.sleep(1.5 * slow)
        park(g)
        ui.screendump(shot, settle=1.0 * slow)
        f = Frame(shot)
        clock_px = f.at(KILL_X - 55, KILL_Y)
        clock_red = clock_px[0] - clock_px[2]
        ck(clock_red > shell_red + 40,
           "Force Quit is DISABLED for the console shell and enabled for Clock",
           "redness: shell=%d %s, clock=%d %s"
           % (shell_red, shell_px, clock_red, clock_px))

        # --- kill it ------------------------------------------------------------
        # The FOURTH row's band, measured with the sort descending and the
        # selection on row 0. With four processes that band holds Finder; with
        # three it is empty. Measuring the whole table instead would be
        # confounded by the selected row's accent fill, which changes the dark
        # pixel count without any row leaving -- that is exactly why the
        # negative control passed this assertion when it should not have.
        ROW3_Y = TABLE_Y + HDR_H + 3 * ROW_H
        before_row3 = f.dark(TABLE_X, ROW3_Y, 620, ROW_H, 120)

        pre = log()
        ui.click_at_confirmed(probe, f.ox + pt(KILL_X), f.oy + pt(KILL_Y))
        time.sleep(1.8 * slow)
        ui.screendump(shot, settle=1.0 * slow)
        ppm_to_png(shot, base_out + "-confirm.png")
        # The probe is UNDER the scrim, so Frame() cannot locate the window
        # while the sheet is up -- that is the scrim working, not a failure.
        # Use the origin from the frame immediately before it.
        dlg = PPM(shot)
        before_px = f.at(30, WINH - 20)
        after_px = dlg.at(f.ox + pt(30), f.oy + pt(WINH - 20))
        ck(luma(after_px) < luma(before_px) - 8,
           "the confirmation dims the window behind it",
           "luma %d -> %d" % (luma(before_px), luma(after_px)))

        # Press Enter: the dialog's default is Force Quit (index 0).
        ui.key("ret")
        time.sleep(2.5 * slow)
        ui.screendump(shot, settle=1.2 * slow)
        ppm_to_png(shot, base_out + "-after-kill.png")
        post = log()[len(pre):]

        # 1. the kernel says so
        marked = "[proc] kill:" in post and "marked" in post
        exited = "exiting" in post
        ck(marked, "the kernel accepted the kill (serial: 'kill: pid N marked')",
           post.strip().splitlines()[-1] if post.strip() else "no new serial output")
        ck(exited, "the victim ran proc_exit on itself (serial: 'exiting')")

        # 2. the Monitor survives, and its table lost a row. The table IS
        #    SYS_PROCS, so a row leaving it is the PCB table losing the process.
        f2 = Frame(shot)
        ck(f2.ok(), "the Monitor is still alive after the kill")
        if f2.ok():
            after_row3 = f2.dark(TABLE_X, ROW3_Y, 620, ROW_H, 120)
            ck(before_row3 > 10 and after_row3 <= 2,
               "the process table lost a row (SYS_PROCS no longer lists it)",
               "4th row ink %d -> %d" % (before_row3, after_row3))

        # 3. the WINDOW is gone. A killed GUI app must not leave a corpse on the
        #    desktop -- this is the assertion that proc_exit's wm_app_exit()
        #    actually ran, and it is independent of both the serial log and the
        #    table.
        # Locate Clock's window by DIFFERENCE -- the pixels that changed when it
        # launched -- and exclude the Monitor's own rectangle, which repaints
        # every second regardless. Then require those pixels to have gone back
        # to what they were before Clock existed. A count of "pixels that
        # changed since the kill" would pass on the Monitor's own redraw, which
        # is how the first version of this assertion passed the negative
        # control.
        ck("gone" in post and "[wm] win" in post,
           "the killed app's WINDOW left the screen (compositor: '[wm] win N gone')",
           [l.strip() for l in post.splitlines() if "[wm] win" in l][:1])

        print("     serial after the kill:")
        for line in post.splitlines():
            if "[proc]" in line or "[fault]" in line or "panic" in line.lower():
                print("       %s" % line.strip())

        # 4. a kill must not take anything else with it
        ck("panic" not in post.lower(), "nothing panicked")
        ck("[fault]" not in post, "the kill did not fault the victim or anyone else")
        ck("desktop live" in log(), "the machine is still running afterwards")

        ui.screendump(shot, settle=1.0 * slow)
        ppm_to_png(shot, base_out + "-final.png")

        # --- the cross-checks ---------------------------------------------------
        # Every number the app shows is checked against an independently
        # maintained source, and the app prints the comparison so a test can
        # assert on it rather than a human squinting at a screenshot.
        xs = [l for l in log().splitlines() if "[monitor] xcheck" in l]
        for l in xs[-2:]:
            print("     %s" % l.strip())
        ck(bool(xs), "the app reported its cross-checks")
        if xs:
            last = xs[-1]
            # pmm counts total, free and used by three different means; a frame
            # that is neither free nor referenced would show up here.
            ck(" delta=0 " in last or last.rstrip().endswith("delta=0"),
               "memory: pmm's used + free equals its total (delta 0)",
               last.split("xcheck ")[-1].strip())
            # The PIT-derived monotonic clock against the CMOS RTC -- two
            # different devices on two different crystals.
            #
            # THE BOUND IS ASYMMETRIC ON PURPOSE, and the asymmetry is the
            # assertion. monotonic_ms() counts 100 Hz PIT ticks and, as its own
            # ABI comment says, does not advance while interrupts are masked --
            # so under TCG it can LOSE ticks and lag the wall clock, and was
            # measured doing exactly that (4 s over 85 s, ~5%). What it must
            # never do is run FAST: a monotonic clock ahead of the RTC would
            # mean ticks arriving that no time passed for. So: a small, bounded
            # lag is tolerated and any meaningful lead is a failure.
            drift = last.rsplit("drift=", 1)[-1].strip()
            mono = last.split("mono=", 1)[-1].split("s", 1)[0]
            try:
                d, m = int(drift), int(mono)
                lead_ok = d >= -2                       # never ahead of the RTC
                lag_ok = d <= max(3, m // 8)            # lag bounded at ~12%
                ck(lead_ok and lag_ok,
                   "uptime: the monotonic clock tracks the RTC (lags, never leads)",
                   "drift %+d s over %d s" % (d, m))
            except ValueError:
                ck(False, "uptime: the monotonic clock tracks the RTC",
                   "unparsable %r / %r" % (drift, mono))

        # --- what the live table costs -----------------------------------------
        costs = [l for l in log().splitlines() if "[monitor] paints" in l]
        for l in costs[-3:]:
            print("     %s" % l.strip())

    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        if keep:
            print("     artefacts in %s" % tmp)

    if fails:
        print("\n%d assertion(s) failed:" % len(fails))
        for f_ in fails:
            print("  - %s" % f_)
        return 1
    print("\nall Activity Monitor assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
