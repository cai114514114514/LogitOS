#!/usr/bin/env python3
"""wm-backdrop-regions, guest half: a SYS_GUI_BACKDROP sidebar shows the
DESKTOP BEHIND the window, frosted, with the app's text crisp on top.

/bin/backdrop-probe (tests/fixtures/wm_backdrop_probe.c) clears its canvas to
one flat colour, records one sidebar region (fill_rgb another flat colour) and
draws text inside it. The driver moves the window so the sidebar sits over open
wallpaper (it first minimises the boot Finder, whose 1000x610 window covers
most of a 1280x800 desktop, so no other window is beneath the sidebar),
screendumps, and checks:

  backdrop-call      the probe saw LOGIT_GUI2_F_BACKDROP in SYS_GUI2_INFO(0)
                     and SYS_GUI_BACKDROP returned OL_OK
  backdrop-variance  the sidebar interior (inset past the glass rim and below
                     the text) has the wallpaper's variance: luma stddev >=
                     --min-std. A canvas-only material (the control kernel,
                     -DWM_BACKDROP_SELF_ONLY) frosts the flat fill and is flat.
  backdrop-text      the text drawn inside the sidebar is crisp: its darkest
                     pixels reach the ink colour (16,16,20) within --ink-tol and
                     enough of them do -- glass drawn OVER the ink, or ink
                     frosted with it, would wash it out.

Usage: qmp_backdrop.py --iso ISO --disk DISK [--xres 1280 --yres 800] [--out DIR]
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
from qmp_window import win_by_title, windows, drag, cmd_key, aim  # noqa: E402
from owned_process import stop_owned                # noqa: E402
from PIL import Image                               # noqa: E402

SBW = 200          # sidebar width, points (the probe's SBW)
TBH = 30           # title bar, points (wm.c TITLEBAR_H)
INK = (16, 16, 20)

ap = argparse.ArgumentParser()
ap.add_argument('--iso', required=True, type=Path)
ap.add_argument('--disk', required=True, type=Path)
ap.add_argument('--xres', type=int, default=1280)
ap.add_argument('--yres', type=int, default=800)
ap.add_argument('--out', type=Path, default=None)
ap.add_argument('--min-std', type=float, default=2.0)
ap.add_argument('--ink-tol', type=int, default=40)
args = ap.parse_args()

out = (args.out or args.iso.parent / 'wmbd-guest').resolve()
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
        time.sleep(.05)
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


def done_lines():
    return [l for l in log().splitlines() if 'BACKDROP DONE' in l]


def shot(name):
    path = out / (name + '.ppm')
    r = ui.cmd({'execute': 'screendump', 'arguments': {'filename': str(path)}})
    if 'error' in r:
        raise RuntimeError(r)
    im = Image.open(path).convert('RGB')
    im.save(out / (name + '.png'))
    return im


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def luma(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


configure(args.xres, args.yres)
try:
    with tempfile.TemporaryDirectory(prefix='wmbd-') as temp, serial.open('wb') as stream:
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
        # Minimise the Finder: click its title bar (focus), then Cmd+M.
        fw = wait(lambda: win_by_title(str(serial), 'Finder'), 'Finder window')
        time.sleep(2.0)
        aim(ui, str(out / 'aim.ppm'), fw['x'] + fw['w'] // 2, fw['y'] + pt(TBH) // 2)
        ui.click()
        time.sleep(0.5)
        cmd_key(ui, 'm')
        wait(lambda: (win_by_title(str(serial), 'Finder') or {}).get('min') == 1, 'Finder minimised', 30)
        time.sleep(1.0)
        send('/bin/backdrop-probe')
        wait(lambda: done_lines(), 'probe first frame')
        w = wait(lambda: win_by_title(str(serial), 'Backdrop Probe'), 'probe window')
        time.sleep(1.0)
        last = done_lines()[-1]
        info = int(last.split('info=')[1].split()[0], 16)
        rc = int(last.split('rc=')[1].split()[0])
        verdict(bool(info & (1 << 8)) and rc == 0, 'backdrop-call',
                f'SYS_GUI2_INFO(0)={info:#x} (LOGIT_GUI2_F_BACKDROP {"set" if info & 256 else "CLEAR"}), '
                f'SYS_GUI_BACKDROP rc={rc}')

        # Put the sidebar over open wallpaper: the window flush with the right
        # edge of the screen, below the menu bar, clear of every other window.
        others = [o for o in windows(str(serial)).values()
                  if not o['title'].startswith('Backdrop Probe') and not o['min']]
        tx = args.xres - w['w'] - pt(16)
        ty = pt(60)
        grab = (w['x'] + w['w'] // 2, w['y'] + pt(TBH) // 2)
        drag(ui, str(out / 'aim.ppm'), grab[0], grab[1], grab[0] + (tx - w['x']), grab[1] + (ty - w['y']))
        n = len(done_lines())
        key('r')                               # redraw (and re-record) after the move
        wait(lambda: len(done_lines()) > n, 'probe redraw')
        time.sleep(1.0)
        w = win_by_title(str(serial), 'Backdrop Probe')
        cy = w['y'] + w['h'] - pt(w['ch'])     # content top
        side = (w['x'], cy, w['x'] + pt(SBW), w['y'] + w['h'])
        busy = [o['title'] for o in others if overlaps(side, (o['x'] - pt(40), o['y'] - pt(40),
                                                              o['x'] + o['w'] + pt(40), o['y'] + o['h'] + pt(40)))]
        print(f'window {w["x"]},{w["y"]} {w["w"]}x{w["h"]} content top {cy}; sidebar {side}; '
              f'windows beneath: {busy or "none"}', flush=True)
        if busy:
            raise RuntimeError('the sidebar is not over open wallpaper: ' + ', '.join(busy))
        im = shot('backdrop')

        # backdrop-variance: inset past the rim (edge width <= 22 px, halo) and
        # below the three text lines (24..96 pt)
        x0, x1 = side[0] + pt(30), side[2] - pt(30)
        y0, y1 = cy + pt(130), side[3] - pt(30)
        vals = [luma(im.getpixel((x, y))) for y in range(y0, y1, 2) for x in range(x0, x1, 2)]
        mean = sum(vals) / len(vals)
        std = math.sqrt(sum((v - mean) ** 2 for v in vals) / len(vals))
        verdict(std >= args.min_std, 'backdrop-variance',
                f'sidebar interior ({x0},{y0})-({x1},{y1}): luma mean {mean:.1f} stddev {std:.2f} '
                f'(min {args.min_std}) over {len(vals)} samples')

        # backdrop-text: the three labels' box
        tx0, tx1 = side[0] + pt(12), side[0] + pt(150)
        ty0, ty1 = cy + pt(20), cy + pt(100)
        dark = 0
        best = 999
        for y in range(ty0, ty1):
            for x in range(tx0, tx1):
                p = im.getpixel((x, y))
                d = max(abs(p[i] - INK[i]) for i in range(3))
                best = min(best, d)
                dark += d <= args.ink_tol
        verdict(best <= 12 and dark >= 60, 'backdrop-text',
                f'{dark} px within {args.ink_tol} of the ink colour in the text box, closest {best}/255')
        key('esc')
except Exception as exc:
    print('FAIL harness', exc, flush=True)
    fails.append('harness')
finally:
    (out / 'result.json').write_text(json.dumps({'failed': fails}, indent=2) + '\n')
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(guest)
sys.exit(1 if fails else 0)
