#!/usr/bin/env python3
"""Click one Dock icon and wait, so a serial-side profiler can bracket it.

This is deliberately the smallest possible QMP driver: it takes no screenshot
and asserts nothing. Its whole job is to deliver ONE launch at a known moment
while tests/boot/run-launch-profile.sh has kprof armed on the other channel --
anything else it did would be sampled too and would land in the histogram as if
it were part of the launch.

Usage: qmp_launch_click.py <qmp.sock> <slot|appname> <settle-seconds>
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, BROWSER_SLOT  # noqa: E402

sock_path = sys.argv[1]
which = sys.argv[2] if len(sys.argv) > 2 else "browser"
settle = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0

# The profile harness boots at 1920x1200, so the dock arithmetic has to be told:
# the icons are centred and scaled, and a coordinate computed for 1280x800 lands
# on the wallpaper, which does nothing and looks exactly like a slow launch.
configure(1920, 1200)

SLOTS = {"clock": 0, "textedit": 1, "monitor": 2, "terminal": 3,
         "widgets": 4, "files": 5, "preview": 6, "studio": 7,
         "browser": BROWSER_SLOT}
slot = SLOTS.get(which, None)
if slot is None:
    slot = int(which)

ui = Session(sock_path)
dx, dy = dock_icon(slot)
ui.click_at(dx, dy)
time.sleep(settle)
print("clicked slot %d at %d,%d" % (slot, dx, dy))
