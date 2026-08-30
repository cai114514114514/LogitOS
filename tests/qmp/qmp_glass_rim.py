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


def near_white_share(ppm, y0, y1):
    """Fraction of bright pixels in a horizontal band, sampled every 4th px.

    The Finder's file area is white-on-glass; the bare wallpaper under the
    dock is a dark gradient. A drag that landed on the wrong stripe (the
    tab strip, the menubar, nothing at all) leaves the dock band dark, and
    the whole photograph then proves nothing -- which is why this driver
    used to exit 0 whatever happened and was a check-test-liveness rule-1
    FAIL. Sampled rather than exhaustive because this is a precondition,
    not the measurement."""
    n = hit = 0
    for y in range(y0, y1, 2):
        for x in range(0, ppm.w, 4):
            r, g, b = ppm.at(x, y)
            n += 1
            if r > 200 and g > 200 and b > 200:
                hit += 1
    return hit / max(1, n)


def main():
    sock, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    ui = Session(sock)
    time.sleep(1.0)

    before = ui.screendump(os.path.join(out, "desk.ppm"))
    p = PPM(before)
    print("screen %dx%d" % (p.w, p.h))

    # The dock band: the same arithmetic qmp_ui.dock_icon() uses for its
    # bottom edge, restated here only as a band (the icon x positions do not
    # matter to "is there content under the rim").
    import qmp_ui
    band_y0 = p.h - qmp_ui.pt(50 + 20) - qmp_ui.pt(12)
    band_y1 = p.h - qmp_ui.pt(12)
    share_before = near_white_share(p, band_y0, band_y1)
    print("dock band bright share before the drag: %.3f" % share_before)

    # The Finder window opens centred-ish with its title bar near the top. Grab
    # it by the title bar (well clear of the three traffic lights on the left)
    # and pull it down until its white file area covers the dock.
    drag(ui, p.w // 2, 88, p.w // 2, 470)

    # Park the pointer off the dock so no tile is hovered -- a hover highlight
    # under the rim would be mistaken for the effect being measured.
    ui.goto(60, p.h // 2)

    after = ui.screendump(os.path.join(out, "rim.ppm"))
    q = PPM(after)
    share_after = near_white_share(q, band_y0, band_y1)
    print("dock band bright share after the drag:  %.3f" % share_after)
    # THE FAILING PATH this driver lacked: the drag is dead reckoning off a
    # title-bar coordinate, exactly the class of click that rots silently
    # (see qmp_addrbar.py's header for the address-bar instance). If the
    # white content is NOT under the dock, the rim photograph is of an empty
    # desktop and "I looked and saw nothing" says nothing at all.
    if share_after < share_before + 0.10:
        print("FAIL: the drag left the dock band's bright share at %.3f "
              "(was %.3f before) -- the Finder's content is not under the "
              "glass, so %s/rim.ppm is a photograph of nothing. Either the "
              "title-bar grab missed or wm.c's drag geometry moved."
              % (share_after, share_before, out))
        sys.exit(1)
    print("wrote %s/rim.ppm" % out)


main()
