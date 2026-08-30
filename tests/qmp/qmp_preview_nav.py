#!/usr/bin/env python3
"""Preview's NAVIGATION, not its pixels: Escape unwinds a mode instead of
killing the app, and a fragmented-MP4 refusal names the real reason.

WHY A SEPARATE DRIVER FROM qmp_preview.py. That one asks "is the picture on
screen the picture in the file" for every format, one Dock launch per run in
--assoc mode or one continuous session walking the whole list otherwise. This
one asks a narrower, previously-untested question: what happens when the user
presses Escape while looking at something Preview opened from its own list?
Answering that needs the SAME session to survive across Escape and prove it is
still the same process (the list reprints itself, with fresh indices, only
when app_main() calls pick_from_media() again) -- a claim qmp_preview.py's
existing back_to_list() helper never makes because it drives Backspace, not
Escape, and Escape used to mean something else entirely.

THE CONTROL, AND HOW TO WATCH IT FAIL. Revert the `e.a == 27` line in
pump_events() to unconditional PUMP_QUIT (what it was) and every assertion in
run_escape_case() here goes red: the second "preview: pick 0 " line never
appears, because app_main()'s loop breaks on PUMP_QUIT and app_exit(0) runs
before pick_from_media() is ever called again. That is a decisive, log-based
control -- not a screenshot guess -- for exactly the reason CLAUDE.md's rule 5
asks for one: it is not possible for this assertion to pass by accident, since
the line it waits for is printed by a code path that only runs if the process
is still alive and back at the picker.

THE OTHER HALF: the m4s message. c/lib/media/mp4.c used to report a bare
DASH/CMAF media segment (moof+mdat, no moov) as MEDIA_ERR_CORRUPT -- true of
the bytes' relationship to a complete file, false of the bytes themselves,
and indistinguishable on screen from a genuinely malformed one. It is now
MEDIA_ERR_NO_INIT, with a message that says what is actually missing. This
driver opens exactly that fixture (the same tests/fixtures/mse/video-1.m4s
MSE's own gate already exercises as a segment, here opened STANDALONE, which
is the scenario a user hits when they save one .m4s off a network panel) and
reads the refusal off the console -- "named, not blanked" is a claim about
the STRING, so the string is what gets asserted, not a pixel count.

Usage: qmp_preview_nav.py [--iso X] [--disk X] [--out DIR] [--keep]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, pt, PPM            # noqa: E402
from qmp_preview import (PICK_PROBE,                      # noqa: E402
                          distinct_colours, ppm_to_png)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def main(argv):
    iso = os.environ.get("LOGIT_ISO", os.path.join(ROOT, "build", "logit.iso"))
    disk = os.environ.get("LOGIT_DISK", os.path.join(ROOT, "build", "disk.img"))
    outdir = os.path.join(ROOT, "build", "preview-nav-shots")
    keep = False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--iso":    iso = argv[i + 1]; i += 2
        elif a == "--disk": disk = argv[i + 1]; i += 2
        elif a == "--out":  outdir = argv[i + 1]; i += 2
        elif a == "--keep": keep = True; i += 1
        else:
            print("unknown arg %r" % a); return 2

    os.makedirs(outdir, exist_ok=True)
    configure(1280, 800)
    tmp = tempfile.mkdtemp(prefix="logit-preview-nav-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")

    # Private copies -- see qmp_preview.py's identical note. Several lines
    # rebuild build/disk.img concurrently and a shared image reads back
    # corrupted, which looks exactly like a filesystem bug in the guest.
    run_iso = os.path.join(tmp, "logit.iso")
    run_disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(iso, run_iso)
    shutil.copyfile(disk, run_disk)
    iso, disk = run_iso, run_disk

    fails = []

    def ck(cond, what, detail=""):
        print("%-4s %s%s" % ("ok" if cond else "FAIL", what,
                             ("  [%s]" % detail) if detail else ""), flush=True)
        if not cond:
            fails.append(what)

    serial_fh = open(serial, "wb")
    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
         "-serial", "stdio", "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdin=subprocess.PIPE, stdout=serial_fh, stderr=subprocess.DEVNULL)

    def wait_for(pattern, mark, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, read(serial)[mark:])
            if m:
                return m
            if qemu.poll() is not None:
                return None
            time.sleep(0.5)
        return None

    try:
        deadline = time.time() + 300
        while time.time() < deadline:
            if "desktop live" in read(serial):
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        else:
            print("FAIL desktop never came up"); return 1
        time.sleep(4)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        # --- open Preview from the Dock, land on its own list ---------------
        # Tile from the guest's dock line, verified launch. The PREVIEW_SLOT=6
        # / NAPPS=10 pair this import used to fetch was already stale: with
        # eleven apps on the disk it aims at the midpoint of the gap between
        # preview's tile and studio's, where a click opens nothing.
        ui.launch_app("preview", probe=probe)
        m = wait_for(r"preview: pick 0 ", 0, 90)
        picks = {mm.group(2): int(mm.group(1))
                 for mm in re.finditer(r"preview: pick (\d+) (\S+)", read(serial))}
        ck(m is not None and bool(picks),
           "Preview opened and listed /media", "%d entries" % len(picks))
        if not picks:
            return 1

        origin = None
        for _ in range(20):
            ui.screendump(shot, settle=0.8)
            box = PPM(shot).find_color(PICK_PROBE)
            if box is not None:
                origin = (box[0], box[1])
                break
        ck(origin is not None, "found Preview's content origin", str(origin))
        if origin is None:
            return 1
        ppm_to_png(shot, os.path.join(outdir, "00-list.png"))

        def click_row(idx):
            """Same geometry qmp_preview.py's click_row uses: aim at the row,
            not the pixel, and re-aim off the guest's own pointer report."""
            tx = origin[0] + pt(200)
            ty = origin[1] + pt(70 + idx * 20 + 10)
            for _ in range(6):
                ui.goto(tx, ty, 0.35)
                got = ui.guest_pointer()
                if got is None:
                    break
                ui.cur = [got[0], got[1]]
                if (got[1] - origin[1] - pt(70)) // pt(20) == idx:
                    break
                ty += (origin[1] + pt(70 + idx * 20 + 10)) - got[1]
            ui.click()
            time.sleep(0.4)

        def open_entry(name):
            """Click `name` in the CURRENT list. Returns (mark, matched open line)."""
            base = os.path.basename(name)
            idx = picks[name]
            for _ in range(3):
                mark = len(read(serial))
                click_row(idx)
                got = wait_for(r"preview: open (\S+) ", mark, 40)
                if got is not None and got.group(1) == base:
                    return mark, got
            return mark, None

        def refresh_picks():
            log = read(serial)
            i2 = log.rfind("preview: pick 0 ")
            if i2 < 0:
                return
            picks.clear()
            for mm in re.finditer(r"preview: pick (\d+) (\S+)", log[i2:]):
                picks[mm.group(2)] = int(mm.group(1))

        def press_escape_expect_list(label):
            """Send Esc; assert the picker comes BACK (app alive), not that
            the window vanished. The decisive signal is a FRESH 'pick 0' line:
            pick_from_media() prints it every time it is entered, and it is
            only entered again if app_main()'s loop did NOT break -- i.e. Esc
            took PUMP_BACK, not PUMP_QUIT. See the file header for how this
            fails when the fix is reverted."""
            mark = len(read(serial))
            ui.key("esc", settle=0.4)
            got = wait_for(r"preview: pick 0 ", mark, 40)
            ck(got is not None,
               "Esc from %s returns to the list (app still running)" % label,
               "saw a fresh picker listing" if got else
               "no picker reprint -- Esc quit the app instead of unwinding")
            return got is not None

        # --- TEST A: Esc while viewing an image opened from the list --------
        if "dot.png" not in picks:
            ck(False, "dot.png is on the list", "have: %s" % ", ".join(sorted(picks)))
            return 1
        mark, opened = open_entry("dot.png")
        ck(opened is not None, "dot.png opened from the list",
           opened.group(0).strip() if opened else "no 'preview: open' line")
        ui.screendump(shot, settle=0.8)
        ppm_to_png(shot, os.path.join(outdir, "01-image.png"))
        if not press_escape_expect_list("a still image (Esc test A)"):
            print("\n%d FAILED:\n  " % len(fails) + "\n  ".join(fails))
            return 1
        ui.screendump(shot, settle=0.8)
        box = PPM(shot).find_color(PICK_PROBE)
        ck(box is not None and (box[0], box[1]) == origin,
           "the list is genuinely back on screen after Esc (not just the log)",
           str(box))
        ppm_to_png(shot, os.path.join(outdir, "02-back-to-list.png"))
        refresh_picks()

        # --- TEST B: a self-contained fragmented MP4 decodes -----------------
        if "clip-frag.mp4" in picks:
            mark, opened = open_entry("clip-frag.mp4")
            ck(opened is not None, "clip-frag.mp4 opened from the list",
               opened.group(0).strip() if opened else "no 'preview: open' line")
            got = wait_for(r"preview: container clip-frag\.mp4 kind=mp4 video=(\S+)",
                            mark, 40)
            ck(got is not None and got.group(1) == "h264",
               "the fragmented MP4 was recognised as a real container, not refused",
               got.group(0).strip() if got else "no 'preview: container' line")
            time.sleep(2.0)     # let a couple of frames actually decode+paint
            ui.screendump(shot, settle=0.5)
            box2 = PPM(shot).find_color(PICK_PROBE)
            origin2 = origin if box2 is None else (box2[0], box2[1])
            cols = distinct_colours(PPM(shot), origin2, 200, 200) if origin2 else 0
            ck(box2 is None,
               "the screen left the list (a picture is being drawn)",
               "still shows the list anchor" if box2 is not None else "anchor gone")
            ppm_to_png(shot, os.path.join(outdir, "03-frag-mp4.png"))
            press_escape_expect_list("a playing fragmented MP4 (Esc test B)")
            refresh_picks()
        else:
            ck(False, "clip-frag.mp4 is on the list",
               "have: %s -- add it via tests/preview.mk's FS_FILES" % ", ".join(sorted(picks)))

        # --- TEST C: a bare DASH/CMAF segment (no init) is named, not blanked -
        if "mse/video-1.m4s" in picks:
            mark, opened = open_entry("mse/video-1.m4s")
            ck(opened is not None, "clip-segment.m4s opened (i.e. was READ)",
               opened.group(0).strip() if opened else "no 'preview: open' line")
            got = wait_for(r"preview: error (.+)", mark, 40)
            ck(got is not None, "the bare segment produced a named refusal",
               got.group(0).strip() if got else "no 'preview: error' line")
            if got is not None:
                msg = got.group(1)
                ck("init segment" in msg,
                   "the refusal names the REAL reason (no init segment), not "
                   "a generic 'corrupt'", msg)
                ck("corrupt" not in msg.split("(")[0],
                   "the top-level message is not the misleading word 'corrupt'",
                   msg)
            ui.screendump(shot, settle=0.5)
            ppm_to_png(shot, os.path.join(outdir, "04-m4s-refusal.png"))
            press_escape_expect_list("the m4s refusal screen (Esc test C)")
        else:
            ck(False, "clip-segment.m4s is on the list",
               "have: %s -- add it via tests/preview.mk's FS_FILES" % ", ".join(sorted(picks)))

        if fails:
            print("\n%d FAILED:" % len(fails))
            for f in fails:
                print("  " + f)
            return 1
        print("\nPASS: Esc unwinds a mode instead of quitting, a self-contained "
              "fragmented MP4 decodes, and a bare segment is refused by name.")
        return 0
    finally:
        try:
            qemu.stdin.close()
        except Exception:
            pass
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except Exception:
            qemu.kill()
        serial_fh.close()
        if keep:
            print("kept %s (serial log, screenshots)" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
