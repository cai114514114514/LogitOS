#!/usr/bin/env python3
"""Actual browser.aex delayed-resource workload; NOT live-site speed.

Server delay defines a repeatable fixture. Only performance.now() from the
guest measures completion. Each run boots an immutable snapshot independently.
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
    for name in ('iso', 'disk', 'out'):
        ap.add_argument('--' + name, required=True)
    ap.add_argument('--count', type=int, choices=(16, 96), default=16)
    ap.add_argument('--expect-loaded', type=int)
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    fixture = ROOT / 'tests/fixtures/browser/script-queue.html'
    payload = fixture.read_bytes()
    def hashes():
        result = {}
        for p in (pathlib.Path(args.iso), pathlib.Path(args.disk), fixture):
            with p.open('rb') as f:
                result[str(p)] = hashlib.file_digest(f, 'sha256').hexdigest()
        return result
    before = hashes(); requests = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            chunk = self.path.startswith('/chunk.js?')
            body = b'executed.push(+document.currentScript.id);' if chunk else payload
            if chunk:
                time.sleep(.2 if args.count == 16 else .005)
            self.send_response(200)
            self.send_header('Content-Type', 'text/javascript' if chunk else 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body)
        def log_message(self, *_args):
            pass
    srv = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    serial = out / 'serial.txt'
    with tempfile.TemporaryDirectory(prefix='sq-', dir='/tmp') as tmp:
        sock = tmp + '/qmp'
        cmd = ['qemu-system-x86_64', '-cpu', 'max', '-cdrom', args.iso,
            '-drive', f'file={args.disk},format=raw,if=none,id=hd0,file.locking=off',
            '-device', 'virtio-blk-pci,drive=hd0', '-boot', 'd', '-snapshot',
            '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
            '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none',
            '-no-reboot', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
            '-serial', 'file:' + str(serial), '-qmp', 'unix:' + sock + ',server,nowait']
        with (out / 'qemu.log').open('w') as log:
            proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            def wait(marker):
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if marker in text and '\n' in text[text.index(marker)+len(marker):]:
                        return text
                    if proc.poll() is not None:
                        raise RuntimeError('QEMU exited before ' + marker)
                    time.sleep(.15)
                raise RuntimeError('missing guest marker: ' + marker)
            try:
                wait('desktop live'); time.sleep(3)
                ui = Session(sock, serial=str(serial)); ui.launch_app('browser'); time.sleep(3)
                ui.key_mods(('ctrl',), 't')
                ui.typ(f'http://10.0.2.2:{srv.server_port}/script-queue.html?{args.count}'); ui.key('ret')
                wait('SCRIPT-QUEUE-START'); ui.key('a')
                text = wait('SCRIPT-QUEUE-DONE ')
                result = json.loads(re.search(r'SCRIPT-QUEUE-DONE (\{[^\r\n]+\})', text)[1])
                result.update(artifacts=before, server_requests=requests[:],
                              complete=result['loaded'] == args.count and result['ordered'] and not result['errors'])
                ui.screendump(str(out / 'final.ppm'), settle=.5)
                (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
                print(json.dumps(result), flush=True)
                expected = args.count if args.expect_loaded is None else args.expect_loaded
                assert result['loaded'] == expected and result['executed'] == expected
                assert result['ordered'] and result['errors'] == 0
                assert len([p for p in requests if p.startswith('/chunk.js?')]) == expected
                assert before == hashes(), 'measurement artifacts changed'
            finally:
                proc.terminate(); proc.wait(timeout=10); srv.shutdown()

if __name__ == '__main__':
    main()
