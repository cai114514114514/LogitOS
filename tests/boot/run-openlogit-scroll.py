#!/usr/bin/env python3
"""Real wheel/drag input, AUI containers and displayed intermediate frames.

The instant-control binary shares the consumer, layout, event loop and backend;
it must fail the same intermediate-image oracle. Frame logs establish idle
submission behavior only, never displayed FPS or host performance.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure
from qmp_window import win_by_title
from owned_process import stop_owned

parser = argparse.ArgumentParser()
parser.add_argument('--build', required=True, type=Path)
args = parser.parse_args()
build = args.build.resolve()
out = build / 'scroll-guest'
out.mkdir(exist_ok=True)
serial = out / 'serial.log'
serial.write_text('')
checks, actions = [], []
guest = ui = None
mark = 0

def log():
    return serial.read_text(errors='replace')

def record(complete=False):
    (out / 'result.json').write_text(json.dumps({
        'passed': bool(checks) and all(c['passed'] for c in checks),
        'complete': complete, 'checks': checks}, indent=2) + '\n')
    (out / 'actions.json').write_text(json.dumps(actions, indent=2) + '\n')

def check(value, label):
    checks.append({'name': label, 'passed': bool(value)})
    record()
    print(('PASS ' if value else 'FAIL ') + label, flush=True)
    if not value:
        raise AssertionError(label)

def wait(fn, label, seconds=90):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if guest.poll() is not None:
            raise RuntimeError('guest exited: ' + label)
        value = fn()
        if value:
            return value
        time.sleep(.01)
    raise TimeoutError(label)

def send(line):
    actions.append({'shell': line})
    guest.stdin.write((line + '\n').encode())
    guest.stdin.flush()

def input_events(events):
    actions.append({'input': events})
    result = ui._input(events)
    if 'error' in result:
        raise RuntimeError(result)

def key(code):
    for down in (True, False):
        input_events([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}])
        time.sleep(.025)

def wheel(n):
    button = 'wheel-down' if n > 0 else 'wheel-up'
    for _ in range(abs(n)):
        input_events([{'type': 'btn', 'data': {'button': button, 'down': True}},
                      {'type': 'btn', 'data': {'button': button, 'down': False}}])
        time.sleep(.012)

def window():
    return win_by_title(str(serial), 'OpenLogit Scroll')

def point(x, y):
    w = window()
    return w['x'] + x, w['y'] + w['h'] - w['ch'] + y

def move(x, y):
    x, y = point(x, y)
    actions.append({'pointer': [x, y]})
    if ui.settle_pointer(str(out / 'pointer.ppm'), x, y, settle=.10) != (x, y):
        raise AssertionError('pointer did not settle before scroll input')

def shot(name):
    path = out / (name + '.ppm')
    result = ui.cmd({'execute': 'screendump', 'arguments': {'filename': str(path)}})
    if 'error' in result:
        raise RuntimeError(result)
    im = Image.open(path).convert('RGB')
    im.save(out / (name + '.png'))
    x, y = point(0, 0)
    return im.crop((x, y, x + 600, y + 350))

def offset(im, x=24, y=70):
    # Read a solid column away from glyphs. A row is 48 px high; its color
    # identifies its index, and the next color boundary gives sub-row offset.
    color = im.getpixel((x, y))
    row = next((i for i in range(20) if color == (40 + i*9, 100 + i*5, 190 - i*5)), None)
    if row is None:
        return -1
    boundary = next((d for d in range(1, 49) if im.getpixel((x, y+d)) != color), 48)
    return (row + 1) * 48 - boundary

def frames():
    return [tuple(map(int, match)) for match in re.findall(
        r'SCROLL FRAME n=(\d+) left=(\d+) outer=(\d+) inner=(\d+) active=(\d+) ms=(\d+)', log()[mark:])]

def settled(left=None, inner=None):
    f = frames()
    return f and f[-1][4] == 0 and (left is None or f[-1][1] == left) and (inner is None or f[-1][3] == inner)

def actual_intermediate(name, low, high, seconds=2):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        im = shot(name + '-probe')
        if low < offset(im) < high:
            im.save(out / (name + '-client.png'))
            return True
        time.sleep(.005)
    return False

record()
configure(1280, 800)
try:
    with tempfile.TemporaryDirectory(prefix='ol-scroll-') as temp, serial.open('wb') as stream:
        socket = str(Path(temp) / 'qmp.sock')
        command = ['qemu-system-x86_64', '-cpu', 'max', '-smp', '4', '-m', '1G',
                   '-accel', 'tcg,thread=multi', '-cdrom', str(build / 'logit.iso'),
                   '-drive', f'file={build / "disk.img"},format=raw,if=none,id=d0',
                   '-device', 'virtio-blk-pci,drive=d0', '-snapshot', '-boot', 'd',
                   '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800',
                   '-display', 'none', '-nic', 'none', '-serial', 'stdio',
                   '-qmp', f'unix:{socket},server=on,wait=off', '-no-reboot']
        (out / 'command.json').write_text(json.dumps(command, indent=2))
        guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
        wait(lambda: '[wm] launched Finder' in log(), 'desktop')
        ui = Session(socket, serial=str(serial))

        send('/bin/scroll-instant')
        wait(lambda: frames() and window(), 'negative consumer ready')
        time.sleep(.5)
        move(130, 175)
        check(offset(shot('negative-before')) == 0, 'instant control has the same initial displayed content')
        wheel(1)
        found = actual_intermediate('negative', 0, 48)
        wait(lambda: settled(48), 'instant endpoint')
        check(offset(shot('negative-after')) == 48, 'instant control receives wheel input and reaches the correct endpoint')
        print(('PASS ' if found else 'FAIL ') + 'wheel displays an actual intermediate frame [instant control]', flush=True)
        check(not found, 'instant control visibly fails the ordinary intermediate-frame assertion')
        key('esc')
        time.sleep(.3)

        mark = len(log())
        send('/bin/scroll-probe')
        wait(lambda: frames() and frames()[-1][0] >= 1, 'positive consumer ready')
        time.sleep(.5)
        move(130, 175)
        initial = shot('initial')
        check(offset(initial) == 0, 'normal consumer draws initial content through actual AUI')
        check(initial.getpixel((325,303)) == (232,59,167) and initial.getpixel((325,306)) != (232,59,167),
              'tiny scrolling viewport retains its clip when the scrollbar cannot fit')
        wheel(1)
        check(actual_intermediate('intermediate', 0, 48), 'wheel displays an actual intermediate frame')
        wait(lambda: settled(48), 'wheel endpoint')
        time.sleep(.15)
        final = shot('endpoint')
        check(offset(final) == 48 and ImageChops.difference(initial, final).getbbox() is not None,
              'wheel lands on the expected displayed row')
        n = len(frames())
        time.sleep(.7)
        check(len(frames()) == n and ImageChops.difference(final, shot('idle')).getbbox() is None,
              'completed scrolling stops application frames and leaves scanout stable')

        wheel(3)
        wait(lambda: settled(192), 'coalesced input target')
        time.sleep(.15)
        check(offset(shot('coalesced')) == 192, 'rapid wheel input preserves all three notches')
        wheel(-4)
        check(actual_intermediate('reverse', 0, 192), 'reverse scrolling displays an intermediate frame')
        wait(lambda: settled(0), 'reverse endpoint')
        time.sleep(.15)
        check(ImageChops.difference(initial, shot('returned')).getbbox() is None,
              'reverse scrolling restores the exact original content without trails')

        # Click directly on the lower scrollbar track. The content and thumb
        # must agree in that input frame; neither may drift after release.
        move(262, 235)
        wheel(1)
        wait(lambda: frames()[-1][4] > 0, 'wheel still running before drag')
        input_events([{'type': 'btn', 'data': {'button': 'left', 'down': True}}])
        wait(lambda: frames()[-1][1] > 400 and settled(), 'direct track input')
        time.sleep(.15)
        direct = frames()[-1][1]
        check(offset(shot('track-held')) == direct, 'scrollbar input and content agree while the mouse remains held')
        move(262, 282)
        wait(lambda: settled(740), 'held thumb dragged to bottom')
        time.sleep(.15)
        check(offset(shot('dragged')) == 740, 'held thumb drag reaches the content boundary in the same frame')
        direct = 740
        input_events([{'type': 'btn', 'data': {'button': 'left', 'down': False}}])
        time.sleep(.3)
        check(frames()[-1][1] == direct and settled(), 'scrollbar release leaves no old wheel target running')
        key('home')
        wait(lambda: settled(0), 'programmatic reset')
        key('c')
        move(130, 175)
        wheel(3)
        time.sleep(.4)
        check(settled(0), 'short content clamps offset and starts no animation')
        key('c')
        wait(lambda: settled(0), 'content restored')

        move(420, 180)
        wheel(1)
        wait(lambda: settled(inner=48), 'nested wheel endpoint')
        time.sleep(.15)
        nested = shot('nested')
        check(frames()[-1][2] == 0 and offset(nested, 332, 110) == 48,
              'nested scroller consumes wheel once and leaves its parent stationary')
        move(526, 180)
        wheel(1)
        wait(lambda: settled(inner=96), 'wheel over nested scrollbar gutter')
        check(frames()[-1][2] == 0, 'wheel over nested gutter still belongs to the inner container')
        wheel(3)
        wait(lambda: frames()[-1][4] > 0, 'inner animation running before hide')
        key('h')
        time.sleep(.3)
        hidden_position = frames()[-1][3]
        n = len(frames())
        time.sleep(.6)
        check(len(frames()) == n and settled(), 'hidden container requests no animation frames')
        key('h')
        time.sleep(.3)
        check(settled(inner=hidden_position), 'restored container retains its offset without resuming an old leg')

        ui.launch_app('settings', title='Settings', probe=str(out / 'pointer.ppm'))
        settings = wait(lambda: win_by_title(str(serial), 'Settings'), 'settings window')
        time.sleep(.4)
        ui.click_at_confirmed(str(out / 'pointer.ppm'), settings['x'] + 584,
                              settings['y'] + settings['h'] - settings['ch'] + 388)
        time.sleep(.4)
        w = window()
        ui.click_at_confirmed(str(out / 'pointer.ppm'), w['x'] + w['w']//2, w['y'] + 15)
        move(130, 175)
        key('home')
        wait(lambda: settled(0), 'reduced-motion initial state')
        start = len(frames())
        wheel(1)
        wait(lambda: settled(48), 'reduced-motion wheel endpoint')
        time.sleep(.2)
        positions = [f[1] for f in frames()[start:]]
        check(positions and all(p == 48 for p in positions) and offset(shot('reduced-motion')) == 48,
              'live Settings reduced-motion switch renders only the wheel endpoint')
        n = len(frames())
        time.sleep(.7)
        check(len(frames()) == n and settled(), 'reduced-motion wheel leaves no scheduled application frames')
        record(True)
        paths = [build / 'logit.iso', build / 'disk.img', build / 'scroll/scroll.elf', build / 'scroll/instant.elf',
                 ROOT / 'c/apps/gui/aui.c', ROOT / 'c/apps/gui/aui_scroll_motion.h',
                 ROOT / 'tests/fixtures/openlogit_scroll.c']
        (out / 'artifacts.json').write_text(json.dumps({str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                                      for p in paths}, indent=2) + '\n')
except Exception as exc:
    checks.append({'name': str(exc), 'passed': False})
    record()
    raise
finally:
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(guest)
print(f'OpenLogit scroll guest: {len(checks)} checks passed', flush=True)
