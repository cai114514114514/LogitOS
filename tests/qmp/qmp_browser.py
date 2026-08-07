#!/usr/bin/env python3
"""Drive the Browser app over QMP: launch from the Dock, load the default URL
(then optionally type one passed in), screenshot.
Usage: qmp_browser.py <qmp.sock> <out.ppm> [url] [wait] [x,y] [scrolls]
When scrolls > 0, also captures out-scroll<N>.ppm after N PageDowns each.

The dock geometry and the input helpers moved to qmp_ui.py so that every QMP
driver derives the icon position from the same app count -- the coordinate rots
silently otherwise (the dock is centred, so one more app shifts every icon).
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, dock_icon, BROWSER_SLOT       # noqa: E402

sock_path, out = sys.argv[1], sys.argv[2]
url = sys.argv[3] if len(sys.argv) > 3 else None
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 11   # post-Enter wait before screendump

ui = Session(sock_path)

# Launch Browser from the Dock (the globe, currently rightmost).
# The 5th arg still overrides, for driving a build with a different app set.
dx, dy = (map(int, sys.argv[5].split(","))) if len(sys.argv) > 5 else dock_icon(BROWSER_SLOT)
ui.click_at(dx, dy)
time.sleep(2.5)          # the .aex is ~2.7 MB off virtio-blk, then ELF load + first paint

# Capture the window BEFORE any URL is typed. Without this, "the dock click
# missed" and "the page failed to render" produce the same blank final image
# and there is nothing to tell them apart after the fact.
base = out.rsplit(".", 1)
ui.screendump(base[0] + "-launch." + (base[1] if len(base) > 1 else "ppm"), settle=0.4)

if url:
    # focus the address bar, clear it, type the new URL
    ui.click_at(420, 145)
    for _ in range(60):
        ui.key("backspace")
    ui.typ(url)

ui.key("ret")                       # load
time.sleep(wait)                    # DNS + TCP (+TLS) + parse + external CSS + layout
ui.screendump(out)

scrolls = int(sys.argv[6]) if len(sys.argv) > 6 else 0
for i in range(1, scrolls + 1):
    for _ in range(3):
        ui.key("pgdn")              # ~3 viewport pages per capture
    time.sleep(1.0)
    ui.screendump(base[0] + "-scroll" + str(i) + "." + (base[1] if len(base) > 1 else "ppm"))
print("done")
