#!/usr/bin/env python3
"""Click one Dock icon and wait, so a serial-side profiler can bracket it.

This is deliberately the smallest possible QMP driver: it takes no screenshot
and its only assertion is the one at the bottom -- that the click launched the
app it names. Its whole job is to deliver ONE launch at a known moment while
tests/boot/run-launch-profile.sh has kprof armed on the other channel --
anything else it did would be sampled too and would land in the histogram as if
it were part of the launch.

Usage: qmp_launch_click.py <qmp.sock> <slot|appname> <settle-seconds>
Env:   QMP_SERIAL_LOG=<path>  the guest's serial log, for the dock line and the
        launch check. run-launch-profile.sh sets it (its -serial stdio log IS
        the serial). Without it the dock tile cannot be looked up by name and
        the launch cannot be verified, so only a raw integer slot works, and
        the profile it produces is UNVERIFIED -- printed as such.

The verify is not decoration: a profile of the wrong app is worse than no
profile, because it gets read as a profile of the named one. A stale
coordinate once aimed this driver's SLOTS dict at whatever the pack list had
drifted under; the histogram would have dutifully sampled the neighbour.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, dock_icon_of, title_of  # noqa: E402

sock_path = sys.argv[1]
which = sys.argv[2] if len(sys.argv) > 2 else "browser"
settle = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
serial_path = os.environ.get("QMP_SERIAL_LOG")

# The profile harness boots at 1920x1200, so the dock arithmetic has to be told:
# the icons are centred and scaled, and a coordinate computed for 1280x800 lands
# on the wallpaper, which does nothing and looks exactly like a slow launch.
configure(1920, 1200)

if which.isdigit():
    if serial_path is None:
        print("clicked raw slot %s -- no QMP_SERIAL_LOG: UNVERIFIED launch" % which)
    slot = int(which)
    named = None
else:
    named = which
    slot = None

ui = Session(sock_path, serial=serial_path)

if named is not None:
    # Slot AND app count from the guest's own [wm] dock line. The SLOTS dict
    # this replaces held the packing order as literals, "browser": BROWSER_SLOT
    # included -- the same fact in a second place, silent when it moved.
    dx, dy = ui.dock_icon_of(named)
    ui.click_at(dx, dy)
else:
    # A raw integer slot still needs the app COUNT for the centred geometry,
    # and that comes off the same dock line. No line, no click -- guessing a
    # count is what rotted every coordinate in this tree once already.
    dock = ui.dock()
    dx, dy = dock_icon(slot, n=len(dock))
    ui.click_at(dx, dy)
time.sleep(settle)

if named is not None:
    got = re.findall(r"\[wm\] launched (.+)", ui.serial_text())
    tail = got[-1].strip() if got else "(no launch line at all)"
    if not got or got[-1].strip() != title_of(named):
        print("FAIL: clicked %s's dock tile at (%d,%d): the guest launched %s"
              % (named, dx, dy, tail))
        sys.exit(1)
    print("clicked %s's dock tile at (%d,%d); guest launched %r"
          % (named, dx, dy, got[-1].strip()))
else:
    print("clicked slot %d at %d,%d" % (slot, dx, dy))
