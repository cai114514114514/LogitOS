"""Put high-contrast content under a glass panel and photograph the rim.

WHY THIS DRIVER EXISTS. The dock's refraction and its chromatic dispersion are
invisible on an empty desktop, and that is not a defect -- the wallpaper is a
smooth gradient, so bending a sample by two pixels lands on almost the same
colour. A refractive material only shows itself across an EDGE. Every existing
screenshot in this tree is of the desktop as it boots, which is exactly the
scene in which this effect cannot appear, so "I looked and saw nothing" from
those shots says nothing at all.

So: drag the Finder window down until its white content sits under the dock,
then crop the dock's rim. Now there is contrast to refract, and R/B separation
has something to separate.

    qmp_glass_rim.py <qmp.sock> <out-dir>
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, configure  # noqa: E402


def drag(ui, x0, y0, x1, y1):
    """Press at (x0,y0), move to (x1,y1), release.

    Stepped like UI.goto for the same reason -- a PS/2 packet's 9-bit delta
    clamps past ~255 px -- and additionally because the window manager tracks a
    drag by successive motion events; one big jump with the button held is a
    different gesture than a drag and some WMs drop it.
    """
    ui.goto(x0, y0)
    ui._input([{"type": "btn", "data": {"button": "left", "down": True}}])
    time.sleep(0.2)
    while ui.cur != [x1, y1]:
        dx = max(-64, min(64, x1 - ui.cur[0]))
        dy = max(-64, min(64, y1 - ui.cur[1]))
        ui._input([{"type": "rel", "data": {"axis": "x", "value": dx}},
                   {"type": "rel", "data": {"axis": "y", "value": dy}}])
        ui.cur[0] += dx
        ui.cur[1] += dy
        time.sleep(0.03)
    time.sleep(0.3)
    ui._input([{"type": "btn", "data": {"button": "left", "down": False}}])
    time.sleep(0.5)


def main():
    sock, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    ui = Session(sock)
    time.sleep(1.0)

    before = ui.screendump(os.path.join(out, "desk.ppm"))
    p = PPM(before)
    print("screen %dx%d" % (p.w, p.h))

    # The Finder window opens centred-ish with its title bar near the top. Grab
    # it by the title bar (well clear of the three traffic lights on the left)
    # and pull it down until its white file area covers the dock.
    drag(ui, p.w // 2, 88, p.w // 2, 470)

    # Park the pointer off the dock so no tile is hovered -- a hover highlight
    # under the rim would be mistaken for the effect being measured.
    ui.goto(60, p.h // 2)

    ui.screendump(os.path.join(out, "rim.ppm"))
    print("wrote %s/rim.ppm" % out)


main()
