#!/usr/bin/env python3
"""THE NEGATIVE CONTROL FOR test-dock-map's COUNT CHECK: can a LYING dock line
be caught, or does the gate just believe whatever number the guest prints?

    python3 tests/qmp/qmp_dock_negctl.py <negctl-iso> <disk.img>

WHAT -DDOCK_NEGCTL_PUBLISH_STALE DOES (wm.c: dock_publish(), see its comment).
scan_apps() is untouched -- it finds every real .aex, fills reg[] and nreg
correctly, and draw_dock() draws and hit-tests every one of them at its real,
nreg-wide centred position. Only the PRINTED line lies: dock_publish() reports
`nreg - 1` apps, and the report is otherwise perfectly well-formed -- N tokens
for a header of N, slots 0..N-1, nothing a parser would reject. It is the same
shape NAPPS used to be: a number that is internally consistent and simply
wrong, silently, forever, until something checks it against the machine that
has the actual answer.

THE CHECK THIS SCRIPT PROVES CAN CATCH IT. The published line says N apps.
Compute where slot N (one past the published end) WOULD be centred if the true
count were N+1 -- the same dock_icon() arithmetic every other driver uses,
just fed a hypothesis instead of a fact -- and click it. Under an HONEST
build this lands in the gap past the end of a real N-wide dock: nothing is
there, nothing launches. Under THIS build the real dock is N+1 wide (nreg =
N+1) and CENTRED on that width, so the hypothesis is exactly right and a real,
unpublished tile sits there. If the guest reports a launch, the published
count was proven wrong by an independent action, not by re-reading the same
line twice -- which is the only way a count check means anything (rule 5: a
control that cannot be watched failing is worse than no control).

THIS SCRIPT'S OWN EXIT CODE is test-dock-map-negctl's pass/fail. Finding the
extra tile is what SHOULD happen on this deliberately-broken build, and is
this control's PASS -- printed as "negctl red as expected", matching how
test-webaccel-negctl's own header reads, because the thing being demonstrated
is that the underlying assertion fails on the broken build, not that this
script's assertions all succeed. Finding nothing is the actual failure: it
means either the macro was not threaded into this ISO, or the click arithmetic
is wrong, or DOCK_NEGCTL_PUBLISH_STALE stopped working -- any of which means
the count check has no teeth, and this script says so and exits nonzero.

Env: QEMU (default qemu-system-x86_64), QEMU_CPU (default max).
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, dock_icon, parse_dock                 # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
XRES, YRES = 1280, 800     # qmp_ui's default mode: SCALE stays 100, pt == px

LAUNCH_RE = re.compile(r"\[wm\] launched (.+)")


def boot(disk):
    tmp = tempfile.mkdtemp(prefix="logit-docknegctl-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen(
        [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
         "-rtc", "base=localtime",
         "-vga", "none",
         "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES),
         "-serial", "file:" + serial, "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 300
    while True:
        if os.path.exists(serial):
            with open(serial, errors="replace") as fh:
                text = fh.read()
            if "desktop live" in text:
                time.sleep(2.0)      # let the Finder's open animation settle
                return proc, sock, serial
        if proc.poll() is not None:
            raise RuntimeError("qemu exited before the desktop came up (%s)" % disk)
        if time.time() > deadline:
            raise RuntimeError("guest never reported a live desktop (%s)" % disk)
        time.sleep(0.2)


def read(serial):
    with open(serial, errors="replace") as fh:
        return fh.read()


def main():
    proc, sock, serial = boot(DISK)
    fails = []
    s = None
    try:
        text = read(serial)
        dock = parse_dock(text)
        if dock is None:
            print("FAIL: no complete '[wm] dock N apps: ...' line in the serial "
                  "log -- was this ISO actually built with DOCKNEGCTL=1?")
            return 1
        n_pub = len(dock)
        print("guest's published dock: %d apps -- %s" %
              (n_pub, " ".join("%d=%s%s" % (e.slot, e.file, ",hidden" if e.hidden else "")
                                for e in dock)))
        if n_pub < 1:
            print("FAIL: published dock has %d apps -- nothing to hypothesise "
                  "an extra slot past" % n_pub)
            return 1

        # The hypothesis: the TRUE count is n_pub + 1, and slot n_pub (0-based,
        # i.e. the (n_pub+1)-th tile) is real. Position it with THAT width, not
        # the published one -- the dock is centred, so a wider real dock is not
        # just "one more icon appended", every tile including this one shifts.
        s = Session(sock, serial=serial)
        hx, hy = dock_icon(n_pub, n=n_pub + 1)
        print("hypothesis: a real, unpublished tile at slot %d, centred at "
              "(%d,%d) if the true count is %d" % (n_pub, hx, hy, n_pub + 1))

        mark = len(read(serial))
        s.click_at(hx, hy)
        time.sleep(4.0)
        launched = LAUNCH_RE.findall(read(serial)[mark:])

        if launched:
            print("negctl red as expected: published %d apps, but clicking the "
                  "slot the true count (%d) predicts launched %r -- the "
                  "published header was a lie the count check would have to "
                  "catch, and this click caught it" % (n_pub, n_pub + 1, launched))
            print("\ndock-negctl: ok -- the lying header is independently "
                  "detectable, so trusting it alone in test-dock-map would be "
                  "the bug this control exists to prevent")
            return 0

        print("FAIL: clicked where the true count predicts an unpublished tile "
              "(%d,%d) and nothing launched -- either this ISO was not really "
              "built with DOCKNEGCTL=1, or the click arithmetic is wrong, or "
              "dock_publish()'s under-report stopped happening. Any of those "
              "means the count check has no teeth." % (hx, hy))
        fails.append("no launch at the hypothesised extra slot")
        return 1
    finally:
        # Reuse the SAME connection `s` (when we got far enough to open one)
        # rather than opening a second one here: QEMU's QMP socket serves one
        # client, so a fresh Session() in a finally block would sit retrying
        # connect() against a socket the first Session still holds open, for
        # its full 120s default timeout, on every run -- exactly the kind of
        # apparatus-caused hang rule 1 warns about, and it did happen once
        # while this file was being written.
        if s is not None:
            try:
                s.cmd({"execute": "quit"})
            except Exception:
                pass
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
