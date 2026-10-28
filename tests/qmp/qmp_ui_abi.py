#!/usr/bin/env python3
"""ui-wm-abi guest gate: the shell ABI on a real kernel.

Boots the isolated test image (tests/ui-wm-abi.mk installs the probes into
/bin), runs the probes from the serial shell and checks, each as one verdict
line `PASS <check> ...` / `FAIL <check> ...`:

  gui2-info      SYS_GUI2_INFO(0) advertises LOGIT_GUI2_F_ICON2 (1 << 10) and
                 not LOGIT_GUI2_F_BACKDROP_CACHE (reserved until W4)
  icon2-weight   SYS_GUI_ICON2 returns 0, and the same 64 pt symbol inks more
                 at weight 3 than at weight 1 on the screendump (>= 1.2x)
  titlebar       WINS_SET_TITLEBAR(TB_NO_SEP | #F2F2F6) -> 0 and is read back
                 from the serial (`[wm] win N app <stem> tb 01f2f2f6`) for the
                 probe's own window; an undefined flag bit -> -1, no line
  size-refused   WINS_SET_SIZE on a normal window -> -1
  dark-at        SYS_UI_DARK_AT(1, 200, 100) flips the theme (rc 1, EV_THEME),
                 the WM reports the reveal origin at exactly the window's
                 content origin + (200, 100) pt; (0, ...) flips back; on < 0
                 only queries
  panel-resize   WINS_SET_SIZE on a panel: 480x200 -> 640x400 -> 480x200 pt,
                 each frame horizontally centred with its top edge kept, and
                 after the shrink the whole screen equals the fresh composite
                 shot before the grow (0 stale px outside the menu-bar band,
                 whose clock moves)

--only sheet|panel runs one half; --panel names the panel probe (the gate's
negative control passes /bin/ui-abi-panel-neg, which never calls
WINS_SET_SIZE, and must fail exactly `panel-resize`). --shots DIR keeps the
review shots (sheet light/dark, panel states) as PNG. Exit 1 on any FAIL,
2 when the harness itself could not run (never a FAIL line).
"""
import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_ui import Session, configure, pt          # noqa: E402
from qmp_window import win_by_title, cmd_key, aim  # noqa: E402
from PIL import Image, ImageChops                  # noqa: E402

TBH = 30           # titlebar, points (TITLEBAR_H, design 8: kept)
MBH = 24           # menu bar, points
# the sheet probe's layout (tests/unit/ui_wm_abi_probe.c)
W1_X, W3_X, WP_Y, WP_PX = 712, 808, 420, 64
DARK_AT = (200, 100)
PANEL1, PANEL2 = (480, 200), (640, 400)

ap = argparse.ArgumentParser()
ap.add_argument('--iso', required=True, type=Path)
ap.add_argument('--disk', required=True, type=Path)
ap.add_argument('--xres', type=int, default=1280)
ap.add_argument('--yres', type=int, default=800)
ap.add_argument('--out', type=Path, default=None)
ap.add_argument('--only', choices=('sheet', 'panel'), default=None)
ap.add_argument('--probe', default='/bin/ui-abi-probe')
ap.add_argument('--panel', default='/bin/ui-abi-panel')
ap.add_argument('--shots', type=Path, default=None)
args = ap.parse_args()

out = (args.out or args.iso.parent / 'uwa-guest').resolve()
out.mkdir(parents=True, exist_ok=True)
if args.shots:
    args.shots.mkdir(parents=True, exist_ok=True)
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


def poll_for(fn, seconds):
    """wait(), but a timeout is an answer (None), not a harness error."""
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        v = fn()
        if v:
            return v
        time.sleep(.1)
    return None


def send(line):
    guest.stdin.write((line + '\n').encode())
    guest.stdin.flush()


def key(code):
    ui.key(code, settle=0.08)


def lines(prefix):
    return [l for l in log().splitlines() if prefix in l]


def kv(line, k):
    m = re.search(r'\b' + k + r'=(-?\w+)', line)
    return m.group(1) if m else None


def shot(name):
    path = out / (name + '.ppm')
    r = ui.cmd({'execute': 'screendump', 'arguments': {'filename': str(path)}})
    if 'error' in r:
        raise RuntimeError(r)
    im = Image.open(path).convert('RGB')
    im.save(out / (name + '.png'))
    if args.shots:
        im.save(args.shots / (name + '.png'))
    return im


def ink(im, x, y, size):
    """sum of (255 - luma) over a square: how much dark symbol is in it."""
    g = im.convert('L').crop((x, y, x + size, y + size))
    return sum(255 - v for v in g.getdata())


def focus(w):
    aim(ui, str(out / 'aim.ppm'), w['x'] + w['w'] // 2, w['y'] + pt(TBH) // 2)
    ui.click()
    time.sleep(0.5)


def sheet():
    send(args.probe)
    wait(lambda: lines('UIABI icon2'), 'sheet probe first frame')
    w = wait(lambda: win_by_title(str(serial), 'UI ABI Probe'), 'sheet probe window')
    time.sleep(1.5)
    slot = w['slot']
    top = w['y'] + w['h'] - pt(w['ch'])          # content origin, device px
    print(f'sheet window slot {slot} at {w["x"]},{w["y"]} {w["w"]}x{w["h"]}, content top {top}', flush=True)

    # gui2-info
    info = int(kv(lines('UIABI info=')[-1], 'info'), 16)
    verdict(bool(info & (1 << 10)) and not info & (1 << 11), 'gui2-info',
            f'SYS_GUI2_INFO(0) = {info:#x} (ICON2 bit 10 {"set" if info & 1024 else "CLEAR"}, '
            f'BACKDROP_CACHE bit 11 {"SET" if info & 2048 else "clear"})')

    # titlebar
    tb = lines('UIABI tb ')[-1]
    reads = [m for m in (re.search(r'\[wm\] win (\d+) app (\S+) tb ([0-9a-f]{8})', l) for l in log().splitlines()) if m]
    mine = [m for m in reads if int(m.group(1)) == slot]
    verdict(kv(tb, 'rc') == '0' and kv(tb, 'bad') == '-1' and len(mine) == 1 and mine[0].group(3) == '01f2f2f6',
            'titlebar', f'probe says "{tb.strip()}", WM read-back for win {slot}: '
            f'{[(m.group(2), m.group(3)) for m in mine]}')

    sz = lines('UIABI size-normal')[-1]
    verdict(kv(sz, 'rc') == '-1', 'size-refused', f'WINS_SET_SIZE on a normal window: rc {kv(sz, "rc")}')

    # icon2-weight
    ic = lines('UIABI icon2')[-1]
    im = shot('sheet-light')
    x0 = w['x']
    i1 = ink(im, x0 + pt(W1_X), top + pt(WP_Y), pt(WP_PX))
    i3 = ink(im, x0 + pt(W3_X), top + pt(WP_Y), pt(WP_PX))
    verdict(kv(ic, 'rc1') == '0' and kv(ic, 'rc3') == '0' and i1 > 0 and i3 >= 1.2 * i1, 'icon2-weight',
            f'rc {kv(ic, "rc1")}/{kv(ic, "rc3")}, ink weight 1 {i1}, weight 3 {i3} '
            f'({(i3 / i1 if i1 else 0):.2f}x, want >= 1.20x)')

    # dark-at
    focus(w)
    n_theme = len(lines('UIABI theme '))
    key('d')
    da = wait(lambda: lines('UIABI dark-at on=1'), 'dark-at reply', 60)[-1]
    org = wait(lambda: [m for m in (re.search(r'\[wm\] ui dark 1 at (-?\d+) (-?\d+)', l) for l in log().splitlines()) if m],
               'dark-at origin', 30)[-1]
    th = poll_for(lambda: [l for l in lines('UIABI theme ')[n_theme:] if 'dark=1' in l], 30)
    ex, ey = w['x'] + pt(DARK_AT[0]), top + pt(DARK_AT[1])
    ox, oy = int(org.group(1)), int(org.group(2))
    ok_on = kv(da, 'rc') == '1' and bool(th) and (ox, oy) == (ex, ey)
    time.sleep(2.0)
    shot('sheet-dark')
    n_dark = len(lines('[wm] ui dark '))
    key('q')
    dq = wait(lambda: lines('UIABI dark-query'), 'dark query', 30)[-1]
    q_ok = kv(dq, 'rc') == '1' and len(lines('[wm] ui dark ')) == n_dark
    n_theme = len(lines('UIABI theme '))
    key('l')
    db = wait(lambda: lines('UIABI dark-at on=0'), 'dark-at back', 60)[-1]
    back = poll_for(lambda: [l for l in lines('UIABI theme ')[n_theme:] if 'dark=0' in l], 30)
    verdict(ok_on and q_ok and kv(db, 'rc') == '0' and bool(back), 'dark-at',
            f'on: rc {kv(da, "rc")}, EV_THEME {"yes" if th else "NO"}, origin ({ox},{oy}) want ({ex},{ey}); '
            f'query rc {kv(dq, "rc")} {"silent" if q_ok else "NOT silent / wrong"}; '
            f'off: rc {kv(db, "rc")}, EV_THEME {"yes" if back else "NO"}')
    time.sleep(1.5)
    key('esc')
    wait(lambda: not win_by_title(str(serial), 'UI ABI Probe'), 'sheet probe exit', 60)
    time.sleep(1.5)


def frame_of(name):
    w = win_by_title(str(serial), name)
    return (w['x'], w['y'], w['w'], w['h']) if w else None


def panel():
    W = args.xres
    send(args.panel)
    wait(lambda: lines('UIABI panel frame'), 'panel first frame')
    first = wait(lambda: (lambda f: f if f and f[2] == pt(PANEL1[0]) and f[3] == pt(PANEL1[1]) else None)(frame_of('UI ABI Panel')),
                 'panel placed', 60)
    time.sleep(2.0)
    a = shot('panel-small')
    bad = []
    if first[0] != (W - first[2]) // 2:
        bad.append(f'small panel not centred: x {first[0]}, want {(W - first[2]) // 2}')

    key('g')
    wait(lambda: lines('UIABI panel grow'), 'grow reply', 60)
    want = (pt(PANEL2[0]), pt(PANEL2[1]))
    big = poll_for(lambda: (lambda f: f if f and (f[2], f[3]) == want else None)(frame_of('UI ABI Panel')), 20)
    if not big:
        bad.append(f'grow: frame stays {frame_of("UI ABI Panel")}, want {want[0]}x{want[1]}')
        verdict(False, 'panel-resize', '; '.join(bad))
        return
    if big[0] != (W - big[2]) // 2 or big[1] != first[1]:
        bad.append(f'grown panel at {big[0]},{big[1]}, want x {(W - big[2]) // 2} and the top kept at {first[1]}')
    poll_for(lambda: [l for l in lines('UIABI panel frame') if f'w={PANEL2[0]} h={PANEL2[1]}' in l], 20)
    time.sleep(2.0)
    shot('panel-grown')

    key('s')
    wait(lambda: lines('UIABI panel shrink'), 'shrink reply', 60)
    back = poll_for(lambda: (lambda f: f if f and f[2:] == first[2:] else None)(frame_of('UI ABI Panel')), 20)
    if not back:
        bad.append(f'shrink: frame {frame_of("UI ABI Panel")}, want {first}')
        verdict(False, 'panel-resize', '; '.join(bad))
        return
    if back != first:
        bad.append(f'shrunk panel at {back}, want exactly {first}')
    n = len(lines('UIABI panel frame'))
    poll_for(lambda: len(lines('UIABI panel frame')) >= n and
             lines('UIABI panel frame')[-1].find(f'w={PANEL1[0]} h={PANEL1[1]}') >= 0, 20)
    time.sleep(2.5)
    b = shot('panel-shrunk')
    band = pt(MBH)
    diff = ImageChops.difference(a.crop((0, band, a.width, a.height)), b.crop((0, band, b.width, b.height)))
    box = diff.getbbox()
    stale = 0
    if box:
        stale = sum(1 for p in diff.crop(box).getdata() if p != (0, 0, 0))
        Image.eval(diff, lambda v: 255 if v else 0).save(out / 'panel-stale.png')
        box = (box[0], box[1] + band, box[2], box[3] + band)
    if stale:
        bad.append(f'{stale} stale px vs the fresh composite in {box}')
    verdict(not bad, 'panel-resize',
            f'{first[2]}x{first[3]} -> {big[2]}x{big[3]} -> {back[2]}x{back[3]} at {back[0]},{back[1]}; '
            f'0 stale px' if not bad else '; '.join(bad))
    key('esc')


configure(args.xres, args.yres)
rc = 0
try:
    with tempfile.TemporaryDirectory(prefix='uwa-') as temp, serial.open('wb') as stream:
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
        time.sleep(2.0)
        # The Finder out of the way: the shots and the stale diff see only the
        # probes over the wallpaper.
        aim(ui, str(out / 'aim.ppm'), fw['x'] + fw['w'] // 2, fw['y'] + pt(TBH) // 2)
        ui.click()
        time.sleep(0.5)
        cmd_key(ui, 'm')
        wait(lambda: (win_by_title(str(serial), 'Finder') or {}).get('min') == 1, 'Finder minimised', 30)
        # park the pointer at the bottom-left, off every probe
        aim(ui, str(out / 'aim.ppm'), pt(8), args.yres - pt(120))
        time.sleep(1.0)
        shot('desktop')
        if args.only in (None, 'sheet'):
            sheet()
            aim(ui, str(out / 'aim.ppm'), pt(8), args.yres - pt(120))
        if args.only in (None, 'panel'):
            panel()
    rc = 1 if fails else 0
except Exception as e:                       # the harness, not a verdict
    print(f'ERROR harness: {type(e).__name__}: {e}', flush=True)
    rc = 2
finally:
    if guest and guest.poll() is None:
        guest.kill()
        guest.wait()
print(f'qmp_ui_abi: {"FAILED " + ", ".join(fails) if fails else ("ok" if rc == 0 else "harness error")}', flush=True)
sys.exit(rc)
