#!/usr/bin/env python3
"""Launch installed Clock through Finder and check real UI pixels and input."""
import argparse
import hashlib
import json
import math
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure, pt
from qmp_window import win_by_title, drag, cmd_key
from owned_process import stop_owned

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--width', type=int, default=1280, choices=[1280, 1920])
args = parser.parse_args()
build = args.build.resolve()
out = build / ('clock-ui-' + str(args.width))
out.mkdir(exist_ok=True)
serial = out / 'serial.log'
serial.write_text('')
checks = []
ui = process = None
scale = configure(args.width, args.width * 5 // 8)

def log():
    return serial.read_text(errors='replace')

def record(complete=False):
    (out / 'result.json').write_text(json.dumps({
        'complete': complete, 'passed': bool(checks) and all(c['passed'] for c in checks),
        'checks': checks, 'scale': scale}, indent=2) + '\n')

def check(ok, name):
    checks.append({'name': name, 'passed': bool(ok)})
    record()
    print(('PASS ' if ok else 'FAIL ') + name, flush=True)
    if not ok:
        raise AssertionError(name)

def wait(predicate, name, timeout=90):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if process.poll() is not None or 'CLOCK ERROR' in log():
            raise RuntimeError('guest failure: ' + name)
        result = predicate()
        if result:
            return result
        time.sleep(.03)
    raise TimeoutError(name)

def window():
    return win_by_title(str(serial), 'Clock')

def frames():
    pattern = (r'CLOCK FRAME n=(\d+) size=(\d+)x(\d+) scale=(\d+) compact=(\d+) '
               r'seconds=(\d+) hour24=(\d+) hand=(-?\d+) active=(\d+) reduced=(\d+) ns=(\d+)')
    return [tuple(map(int, row)) for row in re.findall(pattern, log())]

def tap(key):
    for down in (True, False):
        ui._input([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': key}}}])
        time.sleep(.06)

def point(x, y):
    w = window()
    return w['x'] + pt(x), w['y'] + w['h'] - pt(w['ch']) + pt(y)

def shot(name):
    path = out / (name + '.ppm')
    ui.screendump(str(path), settle=.1)
    image = Image.open(path).convert('RGB')
    x, y = point(0, 0)
    w = window()
    image = image.crop((x, y, x + pt(w['cw']), y + pt(w['ch'])))
    image.save(out / (name + '.png'))
    return image

def changed(a, b):
    return ImageChops.difference(a, b).getbbox() is not None

def stable(name):
    old = None
    since = time.monotonic()
    end = since + 12
    while time.monotonic() < end:
        current = frames()
        if current != old:
            old, since = current, time.monotonic()
        if current and time.monotonic() - since > .65:
            return shot(name)
        time.sleep(.07)
    raise TimeoutError('idle ' + name)

record()
try:
    with tempfile.TemporaryDirectory(prefix='clock-ui-') as temporary, serial.open('wb') as stream:
        socket = str(Path(temporary) / 'qmp.sock')
        command = ['qemu-system-x86_64', '-cpu', 'max', '-smp', '4', '-m', '1G',
            '-accel', 'tcg,thread=multi', '-cdrom', str(build / 'logit.iso'),
            '-drive', f'file={build / "disk.img"},format=raw,if=none,id=d0',
            '-device', 'virtio-blk-pci,drive=d0', '-snapshot', '-boot', 'd', '-vga', 'none',
            '-device', f'virtio-gpu-pci,xres={args.width},yres={args.width * 5 // 8}',
            '-display', 'none', '-nic', 'none', '-rtc', 'base=2026-09-15T13:07:20',
            '-serial', 'stdio', '-qmp', f'unix:{socket},server=on,wait=off', '-no-reboot']
        (out / 'command.json').write_text(json.dumps(command, indent=2))
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
        wait(lambda: '[wm] launched Finder' in log(), 'desktop')
        ui = Session(socket, serial=str(serial))
        ui.launch_app('clock', title='Clock', probe=str(out / 'pointer.ppm'))
        wait(lambda: 'ready ui=2' in log() and frames(), 'new installed Clock')
        ui.goto(pt(1100), pt(40))
        time.sleep(1)
        initial = shot('wide')
        check(window()['cw'] == 620 and window()['ch'] == 400, 'normal launcher opens redesigned Clock')
        face = lambda image: image.crop((pt(28), pt(66), pt(282), pt(320)))
        time.sleep(1.2)
        tick = shot('tick')
        check(changed(face(initial), face(tick)), 'actual second hand changes displayed dial pixels')
        check(any(f[8] and f[7] % 1024 for f in frames()), 'SDK easing produces intermediate hand samples')
        check(any(not f[8] and f[7] % 1024 == 0 for f in frames()), 'animation publishes exact final tick')
        tap('s')
        wait(lambda: frames()[-1][5] == 0, 'hide seconds')
        stopped = stable('seconds-off')
        count = len(frames())
        time.sleep(.7)
        check(len(frames()) == count and not changed(stopped, shot('idle')),
              'seconds disabled stops redraw between minute changes')
        readout = lambda image: image.crop((pt(325), pt(100), pt(596), pt(240)))
        tap('h')
        wait(lambda: frames()[-1][6] == 0, '12-hour state')
        twelve = stable('twelve-hour')
        check(changed(readout(stopped), readout(twelve)), '12-hour action changes actual readout and AM/PM')
        # The pointer route returns the same semantic action as the H shortcut.
        ui.click_at_confirmed(str(out / 'pointer.ppm'), *point(222, 365))
        wait(lambda: frames()[-1][6] == 1, 'format button')
        stable('format-restored')
        check(True, 'mouse format button reaches the application state owner')
        ui.goto(pt(1100), pt(40))
        tap('s')
        wait(lambda: frames()[-1][5] == 1, 'show seconds')
        cmd_key(ui, 'm')
        wait(lambda: window()['min'] == 1, 'minimize')
        time.sleep(.5)
        count = len(frames())
        time.sleep(1)
        check(len(frames()) == count, 'minimized Clock performs no drawing')
        for _ in range(5):
            cmd_key(ui, 'tab')
            if window()['min'] == 0:
                break
        wait(lambda: window()['min'] == 0 and len(frames()) > count, 'restore')
        check(True, 'restoring Clock samples current time and repaints')
        tap('s')
        wait(lambda: frames()[-1][5] == 0, 'static resize test')
        stable('before-resize')
        w = window()
        x, y = w['x'] + w['w'] - 1, w['y'] + w['h'] - 1
        drag(ui, str(out / 'pointer.ppm'), x, y, x - pt(260), y + pt(40))
        wait(lambda: frames()[-1][4] == 1, 'compact layout')
        ui.goto(pt(1100), pt(40))
        compact = stable('compact')
        check(window()['cw'] < 520 and compact.width < initial.width,
              'normal resize reflows to stacked compact layout')
        check(len(set(compact.getdata())) > 150, 'resized device target contains antialiased dial and text')
        # Existing Settings control broadcasts reduced motion to running apps.
        ui.launch_app('settings', title='Settings', probe=str(out / 'pointer.ppm'))
        settings = wait(lambda: win_by_title(str(serial), 'Settings'), 'Settings')
        time.sleep(.4)
        ui.click_at_confirmed(str(out / 'pointer.ppm'), settings['x'] + pt(584),
            settings['y'] + settings['h'] - pt(settings['ch']) + pt(388))
        wait(lambda: frames()[-1][9] == 1, 'reduced motion setting')
        w = window()
        ui.click_at_confirmed(str(out / 'pointer.ppm'), w['x'] + w['w'] // 2, w['y'] + pt(15))
        tap('s')
        wait(lambda: frames()[-1][5] == 1, 'seconds with reduced motion')
        mark = len(frames())
        time.sleep(2)
        check(all(not f[8] and f[7] % 1024 == 0 for f in frames()[mark:]),
              'reduced motion keeps accurate ticks without intermediate animation')
        shot('reduced-motion')
        values = sorted(f[10] / 1e6 for f in frames()[8:] if f[1] == 620 and f[2] == 400)
        (out / 'timings.json').write_text(json.dumps({
            'scope': 'guest draw-to-present-request duration; not FPS or input-to-photon latency',
            'samples': len(values), 'median_ms': statistics.median(values),
            'p95_ms': values[math.ceil(len(values) * .95) - 1]}, indent=2))
        record(True)
        paths = [build / name for name in ['logit.iso', 'disk.img', 'clock.aex', 'clock.elf']]
        (out / 'artifacts.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in paths}, indent=2) + '\n')
finally:
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(process)
print(f'Clock UI: {len(checks)} guest checks passed at {scale}%', flush=True)
