#!/usr/bin/env python3
"""Fixed rendering workload, not a live-site benchmark. All timing is guest rAF.

The host only serves immutable bytes, drives native input and bounds observation.
One independent VM per run; snapshot storage; hash the actual input artifacts.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--disk', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = ROOT / 'tests/fixtures/browser/animation-refresh.html'
    payload = fixture.read_bytes()
    def hashes():
        return {str(p): hashlib.file_digest(open(p, 'rb'), 'sha256').hexdigest()
                for p in (pathlib.Path(args.iso), pathlib.Path(args.disk), fixture)}
    before = hashes()
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        def log_message(self, *_args):
            pass
    srv = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    serial = out / 'serial.txt'
    with tempfile.TemporaryDirectory(prefix='ar-', dir='/tmp') as tmp:
        sock = tmp + '/qmp'
        cmd = ['qemu-system-x86_64', '-cpu', 'max', '-cdrom', args.iso,
               '-drive', f'file={args.disk},format=raw,if=none,id=hd0,file.locking=off',
               '-device', 'virtio-blk-pci,drive=hd0', '-boot', 'd', '-snapshot',
               '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi',
               '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800',
               '-display', 'none', '-no-reboot', '-netdev', 'user,id=n0',
               '-device', 'e1000,netdev=n0', '-serial', 'file:' + str(serial),
               '-qmp', 'unix:' + sock + ',server,nowait']
        with (out / 'qemu.log').open('w') as qlog:
            proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)
            def wait(marker):
                until = time.monotonic() + 180  # observation budget, never performance
                while time.monotonic() < until:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if marker in text and '\n' in text[text.index(marker) + len(marker):]:
                        return text
                    if proc.poll() is not None:
                        raise RuntimeError('QEMU exited before ' + marker)
                    time.sleep(.15)
                raise RuntimeError('missing guest marker: ' + marker)
            try:
                wait('desktop live')
                time.sleep(3)
                ui = Session(sock, serial=str(serial))
                ui.launch_app('browser')
                time.sleep(3)
                ui.key_mods(('ctrl',), 't')
                ui.typ(f'http://10.0.2.2:{srv.server_port}/animation-refresh.html')
                ui.key('ret')
                wait('ANIMATION-START')
                ui.key('a')
                text = wait('ANIMATION-DONE ')
                result = json.loads(re.search(r'ANIMATION-DONE (\{[^\r\n]+\})', text)[1])
                assert result['frames'] == 48 and result['geometry_same']
                assert result['inputs'] > 0, 'native input did not arrive during animation'
                result['layout_builds_observed'] = text.count('[layout-perf]')
                result['artifacts'] = before
                ui.screendump(str(out / 'final.ppm'), settle=.5)
                (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
                print(json.dumps(result), flush=True)
            finally:
                proc.terminate()
                proc.wait(timeout=10)
                srv.shutdown()
    assert before == hashes(), 'input artifacts changed during measurement'


if __name__ == '__main__':
    main()
