#!/usr/bin/env python3
"""wm-rounded-content, guest half: the app canvas is clipped to the window's
rounded shape, and a damage rect confined to a corner renders the same corner.

/bin/round-probe (tests/fixtures/wm_round_probe.c) paints its whole window one
solid colour. Two screendumps of the SAME window, one red and one blue, give the
corner coverage with no knowledge of what lies beneath (wallpaper, shadow):

    P_red  = under * (1 - c) + (255,0,0) * c
    P_blue = under * (1 - c) + (0,0,255) * c      =>  c = (P_red.R - P_blue.R) / 255

c is compared with the analytic area coverage of a quarter circle of radius
S(10) (a 16x16 supersample) at every pixel of both bottom corner squares.
Checks, each printed as PASS/FAIL, the first failure sets exit status 1:
  corner-coverage   |c - analytic| <= 8/255 everywhere in both bottom corners
  corner-outside    no red where the analytic coverage is 0 (outside the arc)
  corner-inside     solid red where the analytic coverage is 1
  corner-damage     after the probe repaints ONLY its two corner squares (a
                    small flush rect), the corner pixels equal the full-frame ones

Usage: qmp_round_corner.py --iso ISO --disk DISK [--xres 1280 --yres 800] [--out DIR]
"""
import argparse
import json
import math
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure, pt          # noqa: E402
from qmp_window import win_by_title                 # noqa: E402
from owned_process import stop_owned                # noqa: E402
from PIL import Image                               # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('--iso', required=True, type=Path)
ap.add_argument('--disk', required=True, type=Path)
ap.add_argument('--xres', type=int, default=1280)
ap.add_argument('--yres', type=int, default=800)
ap.add_argument('--out', type=Path, default=None)
ap.add_argument('--tol', type=int, default=8)
args = ap.parse_args()

out = (args.out or args.iso.parent / 'wmrc-guest').resolve()
out.mkdir(parents=True, exist_ok=True)
serial = out / 'serial.log'
serial.write_text('')
fails = []
guest = ui = None


def log():
    return serial.read_text(errors='replace')


def verdict(ok, name, detail=''):
    print(('PASS ' if ok else 'FAIL ') + name + (' ' + detail if detail else ''), flush=True)
    if not ok:
        fails.append(name)


def wait(fn, label, seconds=120):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if guest.poll() is not None:
            raise RuntimeError('guest exited: ' + label)
        v = fn()
        if v:
            return v
        time.sleep(.02)
    raise TimeoutError(label)


def send(line):
    guest.stdin.write((line + '\n').encode())
    guest.stdin.flush()


def key(code):
    for down in (True, False):
        r = ui._input([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        if 'error' in r:
            raise RuntimeError(r)
        time.sleep(.03)


def done_count():
    return log().count('ROUND DONE')


def press(code):
    n = done_count()
    key(code)
    wait(lambda: done_count() > n, 'probe flush after key ' + code)
    time.sleep(.5)                      # let the compositor present the frame


def shot(name):
    path = out / (name + '.ppm')
    r = ui.cmd({'execute': 'screendump', 'arguments': {'filename': str(path)}})
    if 'error' in r:
        raise RuntimeError(r)
    im = Image.open(path).convert('RGB')
    im.save(out / (name + '.png'))
    return im


def area(cx, cy, r, px, py, n=16):
    """Coverage of pixel (px,py) by the disc of radius r centred (cx,cy)."""
    hit = 0
    for j in range(n):
        for i in range(n):
            x = px + (i + .5) / n - cx
            y = py + (j + .5) / n - cy
            hit += (x * x + y * y) <= r * r
    return hit / (n * n)


def corner_pixels(w, r):
    """(px, py, analytic coverage) for the two bottom corner squares of window w."""
    res = []
    x0, x1, y1 = w['x'], w['x'] + w['w'], w['y'] + w['h']
    for dy in range(r):
        for dx in range(r):
            py = y1 - r + dy
            res.append((x0 + dx, py, area(x0 + r, y1 - r, r, x0 + dx, py)))
            res.append((x1 - r + dx, py, area(x1 - r, y1 - r, r, x1 - r + dx, py)))
    return res


configure(args.xres, args.yres)
r = pt(10)
try:
    with tempfile.TemporaryDirectory(prefix='wmrc-') as temp, serial.open('wb') as stream:
        sock = str(Path(temp) / 'qmp.sock')
        cmd = ['qemu-system-x86_64', '-cpu', 'max', '-smp', '4', '-m', '1G',
               '-accel', 'tcg,thread=multi', '-cdrom', str(args.iso),
               '-drive', f'file={args.disk},format=raw,if=none,id=d0',
               '-device', 'virtio-blk-pci,drive=d0', '-snapshot', '-boot', 'd',
               '-vga', 'none', '-device', f'virtio-gpu-pci,xres={args.xres},yres={args.yres}',
               '-display', 'none', '-nic', 'none', '-serial', 'stdio',
               '-qmp', f'unix:{sock},server=on,wait=off', '-no-reboot']
        guest = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
        wait(lambda: '[wm] launched Finder' in log(), 'desktop')
        ui = Session(sock, serial=str(serial))
        send('/bin/round-probe')
        wait(lambda: 'ROUND DONE' in log(), 'probe first frame')
        w = wait(lambda: win_by_title(str(serial), 'Round Probe'), 'probe window')
        time.sleep(.8)
        print(f'window {w["x"]},{w["y"]} {w["w"]}x{w["h"]} radius {r}px', flush=True)

        press('r')
        red = shot('red')
        press('b')
        blue = shot('blue')
        pix = corner_pixels(w, r)
        worst, bad_out, bad_in, nout, nin = 0.0, 0, 0, 0, 0
        for px, py, ref in pix:
            pr, pb = red.getpixel((px, py)), blue.getpixel((px, py))
            c = (pr[0] - pb[0]) / 255.0
            cb = (pb[2] - pr[2]) / 255.0
            err = max(abs(c - ref), abs(cb - ref)) * 255
            worst = max(worst, err)
            if ref == 0:
                nout += 1
                if abs(pr[0] - pb[0]) > args.tol:
                    bad_out += 1
            if ref == 1:
                nin += 1
                if pr[0] < 255 - args.tol:
                    bad_in += 1
        verdict(worst <= args.tol, 'corner-coverage', f'max error {worst:.1f}/255 over {len(pix)} px (tol {args.tol})')
        verdict(bad_out == 0, 'corner-outside', f'{bad_out} of {nout} px outside the arc carry window colour')
        verdict(bad_in == 0, 'corner-inside', f'{bad_in} of {nin} px inside the arc are not solid')

        press('k')
        kshot = shot('corner-flush')
        press('r')
        full = shot('red-again')
        # k painted red corner squares over a blue canvas and flushed only those:
        # inside the two corner squares the picture must equal the full red frame.
        diff = 0
        for px, py, _ in pix:
            a, b = kshot.getpixel((px, py)), full.getpixel((px, py))
            if max(abs(a[i] - b[i]) for i in range(3)) > 2:
                diff += 1
        verdict(diff == 0, 'corner-damage', f'{diff} of {len(pix)} corner px differ after a corner-only flush')
        key('esc')
except Exception as exc:
    print('FAIL harness', exc, flush=True)
    fails.append('harness')
    raise
finally:
    (out / 'result.json').write_text(json.dumps({'failed': fails}, indent=2) + '\n')
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(guest)
sys.exit(1 if fails else 0)
