#!/usr/bin/env python3
"""wm-shadow-adopt, guest half: the Finder window's drop shadow falloff.

The driver screendumps the desktop with the (focused) Finder window, minimises
the window, screendumps again, and recovers the shadow alpha beside the
window's left edge from the ratio of the two (the shadow is black, so
obs = 255 * (1 - with / without), averaged over rows). That is compared with
the host oracle (tests/unit/wm_shadow_extent_test --profile PCT DARK 1: the
real ol_display_shadow_ex at this scale, not clamped) at every distance
1..extent from the edge, within --tol (6/255). The theme is not assumed: the
closer of the dark and light oracle is used.

  falloff-<pct>   max |observed - oracle| <= tol over the profile, pct = the
                  backing scale of this mode (1280x800 -> 100, 2560x1600 -> 200)

The legacy clamped ol_display_shadow tightens the 200% blur (corner tile past
72 px), so the control kernel (-DWM_SHADOW_LEGACY) fails falloff-200.

Usage: qmp_wm_shadow.py --iso ISO --disk DISK --oracle BIN [--xres 1280 --yres 800] [--out DIR]
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
import qmp_ui                                       # noqa: E402
from qmp_ui import Session, configure, pt          # noqa: E402
from qmp_window import win_by_title, windows, cmd_key, aim  # noqa: E402
from owned_process import stop_owned                # noqa: E402
from PIL import Image                               # noqa: E402

TBH = 30           # title bar, points (wm.c TITLEBAR_H)

ap = argparse.ArgumentParser()
ap.add_argument('--iso', required=True, type=Path)
ap.add_argument('--disk', required=True, type=Path)
ap.add_argument('--xres', type=int, default=1280)
ap.add_argument('--yres', type=int, default=800)
ap.add_argument('--out', type=Path, default=None)
ap.add_argument('--oracle', required=True, type=Path)
ap.add_argument('--tol', type=int, default=6)
args = ap.parse_args()

out = (args.out or args.iso.parent / 'wmsh-guest').resolve()
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


def shot(name):
    path = out / (name + '.ppm')
    r = ui.cmd({'execute': 'screendump', 'arguments': {'filename': str(path)}})
    if 'error' in r:
        raise RuntimeError(r)
    im = Image.open(path).convert('RGB')
    im.save(out / (name + '.png'))
    return im


def luma(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


def oracle(pct, dark):
    r = subprocess.run([str(args.oracle), '--profile', str(pct), str(dark), '1'],
                       capture_output=True, text=True, check=True)
    lines = r.stdout.split('\n')
    ext = int(lines[0].split()[1])
    prof = {int(a): int(b) for a, b in (l.split() for l in lines[1:] if l.strip())}
    return ext, prof


def column_sum(im, x, y0, y1):
    return sum(luma(im.getpixel((x, y))) for y in range(y0, y1))


configure(args.xres, args.yres)
try:
    with tempfile.TemporaryDirectory(prefix='wmsh-') as temp, serial.open('wb') as stream:
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
        fw = wait(lambda: win_by_title(str(serial), 'Finder'), 'Finder window')
        time.sleep(3.0)
        aim(ui, str(out / 'aim.ppm'), fw['x'] + fw['w'] // 2, fw['y'] + pt(TBH) // 2)
        ui.click()                            # focus it: the FOCUSED preset
        time.sleep(1.5)
        fw = win_by_title(str(serial), 'Finder')
        others = [o['title'] for o in windows(str(serial)).values()
                  if not qmp_ui.title_matches(o['title'], 'files') and not o['min']]
        pct = qmp_ui.SCALE
        ext, _ = oracle(pct, 1)
        print(f'Finder at {fw["x"]},{fw["y"]} {fw["w"]}x{fw["h"]}, scale {pct}%, extent {ext}px, '
              f'other windows: {others or "none"}', flush=True)
        if fw['x'] - ext < 0 or others:
            raise RuntimeError('the left shadow is not over open wallpaper')
        with_win = shot('with')
        cmd_key(ui, 'm')
        wait(lambda: (win_by_title(str(serial), 'Finder') or {}).get('min') == 1, 'Finder minimised', 30)
        time.sleep(2.0)
        without = shot('without')

        ymid = fw['y'] + fw['h'] // 2
        y0, y1 = ymid - 40, ymid + 40
        obs = {}
        for i in range(1, ext + 1):
            x = fw['x'] - i
            a, b = column_sum(with_win, x, y0, y1), column_sum(without, x, y0, y1)
            obs[i] = 255.0 * (1.0 - a / b) if b > 0 else 0.0
        best = None
        for dark in (1, 0):
            _, prof = oracle(pct, dark)
            err = max(abs(obs[i] - prof[i]) for i in range(2, ext + 1))
            worst = max(range(2, ext + 1), key=lambda i: abs(obs[i] - prof[i]))
            if best is None or err < best[0]:
                best = (err, dark, worst, prof)
        err, dark, worst, prof = best
        verdict(err <= args.tol, f'falloff-{pct}',
                f'{"dark" if dark else "light"} oracle, max |observed - oracle| {err:.1f}/255 at {worst}px '
                f'(observed {obs[worst]:.1f}, oracle {prof[worst]}; tol {args.tol}); '
                f'at 20px observed {obs[min(20, ext)]:.1f} oracle {prof[min(20, ext)]}')
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
