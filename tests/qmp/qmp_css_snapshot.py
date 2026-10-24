#!/usr/bin/env python3
"""Capture a saved real HTML/CSS page; this is evidence, not a layout verdict.

Serve MANIFEST URL pathnames with their original bytes, retaining every script.
Unsaved resources return real 404s. Read-only javascript: geometry reporting
uses the existing Browser input path without inserting code into the HTML.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import urlsplit

from qmp_ui import PPM, Session, browser_client_point, configure
from qmp_site import write_png

ROOT = Path(__file__).resolve().parents[2]


def fingerprint(path):
    before = path.stat()
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    after = path.stat()
    assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), 'artifact changed during fingerprint: ' + str(path)
    return {'path': str(path), 'bytes': after.st_size, 'mtime_ns': after.st_mtime_ns, 'sha256': digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    for name in ('iso', 'disk', 'out', 'site'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    assert re.fullmatch(r'[A-Za-z0-9_-]+', args.site), 'site must name a saved corpus directory'
    directory = ROOT / 'tests/fixtures/cssweb' / args.site
    manifest = (directory / 'MANIFEST').read_text()
    original = next(line[2:] for line in manifest.splitlines() if line.startswith('# http'))
    entry = urlsplit(original).path or '/'
    assets = {entry: directory / 'index.html'}
    for line in manifest.splitlines():
        if not line or line.startswith('#'):
            continue
        name, size, url = line.split(maxsplit=2)
        path = directory / name
        assert path.stat().st_size == int(size), 'saved resource disagrees with MANIFEST: ' + name
        pathname = urlsplit(url).path
        assert pathname not in assets, 'ambiguous MANIFEST pathname: ' + pathname
        assets[pathname] = path
    payloads = {name: path.read_bytes() for name, path in assets.items()}
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'site': args.site, 'original_url': original, 'evidence': 'saved static HTML/CSS; live JS interaction unverified',
              'iso': fingerprint(Path(args.iso).resolve()), 'disk': fingerprint(Path(args.disk).resolve()),
              'capture_script': fingerprint(Path(__file__).resolve()),
              'manifest_sha256': hashlib.sha256(manifest.encode()).hexdigest(),
              'sources': {name: {'file': path.name, 'bytes': len(payloads[name]), 'sha256': hashlib.sha256(payloads[name]).hexdigest()} for name, path in assets.items()}}
    requests, reports = [], []

    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            name = urlsplit(self.path).path
            raw = payloads.get(name)
            requests.append({'path': self.path, 'status': 200 if raw is not None else 404,
                             'file': assets[name].name if raw is not None else None})
            if raw is None:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/css' if name.endswith('.css') else 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            if self.path != '/__snapshot_report' or not 0 < length < 65536:
                self.send_error(400)
                return
            reports.append(json.loads(self.rfile.read(length)))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial_path, qmp_path = out / 'serial.log', out / 'qmp.sock'
    configure(1280, 800)
    proc = subprocess.Popen([os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max', '-m', '1G', '-smp', '4',
        '-cdrom', result['iso']['path'], '-drive', 'file=%s,format=raw,if=none,id=hd0' % result['disk']['path'],
        '-device', 'virtio-blk-pci,drive=hd0', '-boot', 'd', '-snapshot', '-accel', 'tcg,thread=multi',
        '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none', '-no-reboot',
        '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0', '-serial', 'file:' + str(serial_path),
        '-qmp', 'unix:%s,server,nowait' % qmp_path], stdout=subprocess.DEVNULL, stderr=(out / 'qemu.log').open('w'))

    def serial():
        return serial_path.read_text(errors='replace') if serial_path.exists() else ''

    def wait(predicate, label, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            if proc.poll() is not None:
                raise AssertionError('QEMU exited before ' + label)
            time.sleep(.2)
        raise AssertionError('guest never reached ' + label)

    try:
        wait(lambda: 'LOGIT_BOOT_OK' in serial() and 'desktop live' in serial(), 'desktop', 240)
        time.sleep(3)
        ui = Session(str(qmp_path), serial=str(serial_path))
        ui.launch_app('browser')
        time.sleep(4)
        mark = ui.mark()
        ui.key_mods(['ctrl'], 'l')
        ui.typ('http://10.0.2.2:%d%s' % (server.server_port, entry))
        ui.key('ret')
        wait(lambda: '[load-complete]' in serial()[mark:], 'saved page load completion', 180)
        # A first paired capture served 200s but one guest lost a stylesheet
        # in transport. Its screenshot is not a same-input CSS comparison.
        # Require closure on BOTH sides: requests reaching this server and the
        # browser actually accepting the complete set without a transport miss.
        expected_css = {name for name in assets if name.endswith('.css')}
        served_css = {urlsplit(request['path']).path for request in requests
                      if request['status'] == 200 and urlsplit(request['path']).path.endswith('.css')}
        assert served_css == expected_css, 'saved stylesheet request set is incomplete'
        fetched = re.findall(r'\[css\] fetched: (\d+) ok, (\d+) empty, (\d+) http-error, (\d+) transport-error',
                             serial()[mark:])
        assert fetched, 'missing actual guest stylesheet load summary'
        result['guest_css_load'] = dict(zip(('ok', 'empty', 'http_error', 'transport_error'), map(int, fetched[-1])))
        assert list(result['guest_css_load'].values()) == [len(expected_css), 0, 0, 0], \
            'guest did not accept the complete saved stylesheet set: ' + str(result['guest_css_load'])
        probe = "javascript:(()=>{let b={};['header','main','body'].forEach(s=>{let e=document.querySelector(s),r=e&&e.getBoundingClientRect();b[s]=r?[r.left,r.top,r.width,r.height]:null});fetch('/__snapshot_report',{method:'POST',body:JSON.stringify({boxes:b,title:document.title,href:location.href,viewport:[innerWidth,innerHeight]})})})()"
        # The first probe omitted the outer arrow block's closing brace. Both
        # guests loaded all 17 sheets, then correctly reported SyntaxError;
        # waiting for a POST alone hid this apparatus defect until timeout.
        # Also derive the input bound instead of inventing a larger capacity.
        tabs = (ROOT / 'c/apps/gui/browser/app/tabs.h').read_text()
        capacity = int(re.search(r'^#define\s+TAB_URL\s+(\d+)', tabs, re.M).group(1))
        assert len(probe.encode()) < capacity, 'read-only probe must fit the existing address buffer'
        probe_mark = ui.mark()
        ui.key_mods(['ctrl'], 'l')
        ui.typ(probe)
        ui.key('ret')
        wait(lambda: bool(reports) or '[browser] JS exception:' in serial()[probe_mark:],
             'actual guest geometry report', 90)
        assert reports, 'read-only geometry probe failed: ' + serial()[probe_mark:][-1500:]
        assert len(reports) == 1 and urlsplit(reports[0]['href']).path == entry, reports
        result['guest'] = reports[0]
        for tag in ('header', 'main', 'body'):
            box = reports[0]['boxes'].get(tag)
            assert isinstance(box, list) and len(box) == 4, 'missing actual ' + tag + ' geometry'
        ui.screendump(str(out / 'screen.ppm'), settle=1)
        pixels = PPM(str(out / 'screen.ppm'))
        write_png(str(out / 'screen.png'), pixels.w, pixels.h, pixels.px)
        width, height = reports[0]['viewport']
        left, top = browser_client_point(serial(), 0, 0)
        right, bottom = browser_client_point(serial(), width - 1, height - 1)
        crop = b''.join(pixels.px[(y * pixels.w + left) * 3:(y * pixels.w + right + 1) * 3] for y in range(top, bottom + 1))
        write_png(str(out / 'viewport.png'), right - left + 1, bottom - top + 1, crop)
        result['screen_viewport'] = [left, top, right + 1, bottom + 1]
        result['captured'] = True
        print('static CSS snapshot captured:', args.site, reports[0]['boxes'], flush=True)
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        result['requests'] = requests
        result['missing_resources'] = [request['path'] for request in requests if request['status'] == 404]
        result['css_requests'] = [request for request in requests if urlsplit(request['path']).path.endswith('.css')]
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        (out / 'requests.json').write_text(json.dumps(requests, indent=2) + '\n')
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
