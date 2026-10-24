#!/usr/bin/env python3
"""One ordinary external-CSS page, geometry and real scanout before/after.

The guest reports rectangles over HTTP; interleaved kernel/JS serial output is
diagnostic only. Bitmap checks additionally require the exact asset request and
its decoded colors on screen, so a failed navigation cannot pass by omission.
"""
import argparse
import http.server
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import threading
import time
import tempfile
import zlib

from qmp_ui import PPM, Session, browser_client_point

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'engine-expansion'


def png(ppm, path):
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    raw = bytearray()
    for y in range(ppm.h):
        raw.append(0)
        for x in range(ppm.w):
            raw.extend(ppm.at(x, y))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('!2I5B', ppm.w, ppm.h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('iso')
    parser.add_argument('disk')
    parser.add_argument('--out', required=True)
    parser.add_argument('--baseline', action='store_true')
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    reports, requests, checks = [], [], []

    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split('?', 1)[0]
            requests.append(path)
            relative = 'css-components.html' if path == '/' else path.lstrip('/')
            target = (FIXTURES / relative).resolve()
            if not target.is_relative_to(FIXTURES.resolve()) or not target.is_file():
                self.send_error(404)
                return
            raw = target.read_bytes()
            self.send_response(200)
            self.send_header('Content-Length', str(len(raw)))
            self.send_header('Content-Type', 'text/css' if target.suffix == '.css' else
                             'image/bmp' if target.suffix == '.bmp' else 'text/html')
            self.end_headers()
            self.wfile.write(raw)

        def do_POST(self):
            if self.path != '/report':
                self.send_error(404)
                return
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size < 65536:
                self.send_error(400)
                return
            reports.append(json.loads(self.rfile.read(size)))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    # Evidence paths plus UUIDs exceed Darwin's 104-byte UNIX socket limit.
    # Only the socket is transient; retain logs, source hashes and pixels in out.
    socket_dir=tempfile.TemporaryDirectory(prefix='css-comp-',dir='/tmp')
    serial_path, qmp_path = out / 'serial.log', Path(socket_dir.name) / 'qmp.sock'
    proc = subprocess.Popen([
        os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
        '-cdrom', str(Path(args.iso).resolve()),
        '-drive', 'file=%s,format=raw,if=none,id=hd0' % Path(args.disk).resolve(),
        '-device', 'virtio-blk-pci,drive=hd0', '-boot', 'd', '-snapshot',
        '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
        '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none',
        '-no-reboot', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
        '-serial', 'file:' + str(serial_path),
        '-qmp', 'unix:%s,server,nowait' % qmp_path,
    ], stdout=subprocess.DEVNULL, stderr=(out / 'qemu.log').open('w'))

    def serial():
        return serial_path.read_text(errors='replace') if serial_path.exists() else ''

    def wait(predicate, seconds, description):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            if proc.poll() is not None:
                raise AssertionError('QEMU exited during ' + description)
            time.sleep(.2)
        raise AssertionError('timed out waiting for ' + description)

    def check(condition, label):
        checks.append({'pass': bool(condition), 'label': label})
        print(('ok: ' if condition else 'FAIL: ') + label, flush=True)

    try:
        wait(lambda: 'LOGIT_BOOT_OK' in serial(), 240, 'boot')
        wait(lambda: 'desktop live' in serial(), 90, 'desktop')
        time.sleep(3)
        ui = Session(str(qmp_path), serial=str(serial_path))
        ui.launch_app('browser')
        time.sleep(4)
        ui.typ('http://10.0.2.2:%d/' % server.server_port)
        ui.key('ret')
        wait(lambda: len(reports) == 1, 160, 'complete guest geometry report')
        assert reports[0]['kind'] == 'css-components'
        boxes = reports[0]['boxes']
        check('/styles/components.css' in requests, 'external stylesheet reached the guest')
        check('/assets/stripes.bmp' in requests, 'CSS URL uses the external stylesheet base')
        check(boxes['ratio']['width'] == 240 and boxes['ratio']['height'] == 135,
              '16/9 card reserves its real 240x135 box')
        check(boxes['auto-col']['height'] == 150 and boxes['basis-a']['height'] == 60 and
              boxes['basis-b']['height'] == 90, 'auto column honors both flex bases')
        check(boxes['wrap-a']['x'] < boxes['wrap-b']['x'] < boxes['wrap-c']['x'] and
              boxes['wrap-a']['y'] == boxes['wrap-b']['y'] == boxes['wrap-c']['y'],
              'column wrap creates three distinct columns')
        check(boxes['normal-a']['width'] == 90 and boxes['start-a']['width'] == 20,
              'grid normal stretches while explicit start keeps intrinsic tracks')
        check(boxes['nested-leaf']['width'] == 80 and boxes['nested-leaf']['height'] == 16,
              'nested complex parent keeps selector-list specificity')

        shot = out / 'screen.ppm'
        deadline = time.monotonic() + (2 if args.baseline else 40)
        while True:
            ui.screendump(str(shot), settle=.2)
            pixels = PPM(str(shot))
            if pixels.find_color((31, 191, 103)) or time.monotonic() >= deadline:
                break
            time.sleep(.2)

        def sample(name, x, y, expected):
            b = boxes[name]
            point = browser_client_point(serial(), b['x'] + x, b['y'] + y)
            actual = pixels.at(*point)
            check(actual == expected, '%s pixel(%d,%d)=%s expected=%s' %
                  (name, x, y, actual, expected))

        for name in ('cover', 'contain', 'repeat'):
            b = boxes[name]
            check(b['width'] == 160 and b['height'] == 90, name + ' has its authored border box')
        sample('cover', 8, 40, (231, 41, 57))
        sample('cover', 80, 40, (31, 191, 103))
        sample('cover', 152, 40, (37, 73, 223))
        sample('contain', 80, 2, (222, 220, 113))
        sample('contain', 80, 40, (31, 191, 103))
        sample('contain', 80, 88, (222, 220, 113))
        sample('repeat', 5, 5, (231, 41, 57))
        sample('repeat', 45, 25, (231, 41, 57))
        sample('repeat', 25, 65, (31, 191, 103))
        png(pixels, out / 'screen.png')
        fixture_hashes = {str(path.relative_to(FIXTURES)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (FIXTURES/'css-components.html',FIXTURES/'styles/components.css',
                                       FIXTURES/'assets/stripes.bmp')}
        result = {'baseline': args.baseline, 'checks': checks, 'boxes': boxes,
                  'fixture_sha256': fixture_hashes,
                  'requests': requests, 'failures': sum(not c['pass'] for c in checks)}
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('css-components guest: %d checks, %d failures%s' %
              (len(checks), result['failures'], ' (before image)' if args.baseline else ''), flush=True)
        if args.baseline:
            assert result['failures'] > 0, 'before image must visibly discriminate the new work'
        else:
            assert not result['failures'], 'geometry or pixels differ from the independent CSS expectations'
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        server.shutdown()
        server.server_close()
        socket_dir.cleanup()


if __name__ == '__main__':
    main()
