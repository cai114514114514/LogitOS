#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two real HTTP origins, native navigation, shared Response assertions.

No injected JS or fake fetch response: the guest requests the same fixture the
host gate reads. Artifact hashes reject concurrent image changes. A completed
serial result is followed by a screenshot, not treated as paint evidence itself.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import subprocess
import threading
import time
from qmp_ui import Session

ROOT = pathlib.Path(__file__).resolve().parents[2]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--case', choices=('response', 'module-retry', 'stylesheets', 'images'), default='response')
    ap.add_argument('--page', help='ordinary standalone diagnostic HTML to serve')
    ap.add_argument('--marker')
    ap.add_argument('--expected-checks', type=int)
    args = ap.parse_args()
    build = pathlib.Path(args.build).resolve()
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    requests = []
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append({'port': self.server.server_port, 'path': self.path})
            module_sources = {
                '/entry1.js': "import './broken.js'; globalThis.brokenRan=true;",
                '/entry2.js': "import './broken.js'; globalThis.retryRan=true;",
                '/broken.js': "import './missing.js'; export const value=1;",
                '/entry3.js': "import {total} from './cycle-a.js'; globalThis.cyclic=total; globalThis.healthy=9;",
                '/cycle-a.js': "import {b} from './cycle-b.js'; export function a(){return 4}; export const total=b();",
                '/cycle-b.js': "import {a} from './cycle-a.js'; export function b(){return a()+3};",
            }
            if args.case == 'images':
                self.send_response(404 if self.path == '/missing.svg' else 200)
                if self.path == '/image.svg':
                    data, mime = b'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="16"><rect width="24" height="16" fill="#2458da"/></svg>', 'image/svg+xml'
                elif self.path in ('/missing.svg','/corrupt.svg'):
                    data, mime = b'Not an image', 'image/svg+xml'
                else:
                    data, mime = (ROOT/'tests/fixtures/browser/detached-images.html').read_bytes(), 'text/html'
            elif args.case == 'stylesheets':
                sheets = {'/first.css': '#target{width:52px}', '/second.css': '#target{width:94px}', '/empty.css': ''}
                self.send_response(404 if self.path == '/missing.css' else 200)
                if self.path in sheets:
                    data, mime = sheets[self.path].encode(), 'text/css'
                elif self.path == '/missing.css':
                    data, mime = b'Missing stylesheet', 'text/plain'
                else:
                    data, mime = (ROOT/'tests/fixtures/browser/dynamic-styles.html').read_bytes(), 'text/html'
            elif args.page:
                self.send_response(200)
                data, mime = pathlib.Path(args.page).read_bytes(), 'text/html'
            elif args.case == 'module-retry' and self.path in module_sources:
                self.send_response(200)
                data, mime = module_sources[self.path].encode(), 'text/javascript'
            elif args.case == 'module-retry' and self.path == '/missing.js':
                self.send_response(404)
                data, mime = b'Missing dependency', 'text/plain'
            elif args.case == 'module-retry':
                self.send_response(200)
                data = b'''<!doctype html><title>Module failure recovery</title>
<style>body{font:22px sans-serif;margin:48px}</style><h1>Module failure recovery</h1>
<p id=result>Waiting for module graph</p><script>
window.addEventListener('load',function(){var failures=0;
if(typeof brokenRan!=='undefined'||typeof retryRan!=='undefined')failures++;
if(globalThis.healthy!==9)failures++;if(globalThis.cyclic!==7)failures++;
var result='MODULE-RETRY-GUEST checks=3 failures='+failures;
document.getElementById('result').textContent=result;console.log(result);});
</script><script type=module src=/entry1.js></script><script type=module src=/entry2.js></script><script type=module src=/entry3.js></script>'''
                mime = 'text/html'
            elif self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/payload')
                data = b''
                mime = 'text/plain'
            elif self.path == '/payload':
                self.send_response(200)
                self.send_header('X-Secret', 'hidden')
                data, mime = b'secret payload', 'text/plain'
            elif self.path == '/response-boundary.js':
                self.send_response(200)
                data = (ROOT/'tests/fixtures/browser/response-boundary.js').read_bytes()
                mime = 'text/javascript'
            else:
                self.send_response(200)
                data = ('''<!doctype html><meta charset="utf-8"><title>Response boundaries</title>
<style>body{font:20px sans-serif;margin:48px;background:#f5f7fa;color:#132136}h1{font-size:30px}p{line-height:1.5}</style>
<h1>Response boundary regression</h1><p id="result">Waiting for network checks</p><p id="details"></p>
<script>var responseBoundaryOrigin='http://10.0.2.2:%d';</script>
<script src="/response-boundary.js"></script>''' % cross.server_port).encode()
                mime = 'text/html'
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        def log_message(self, *_):
            pass
    cross = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    main_server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Handler)
    for server in (cross, main_server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    def hashes():
        return {name: hashlib.file_digest(open(build/name, 'rb'), 'sha256').hexdigest()
                for name in ('logit.iso', 'disk.img', 'browser.aex')}
    artifacts = {'before': hashes()}
    serial = out/'serial.txt'
    sock = out/'qmp.sock'
    cmd = ['qemu-system-x86_64', '-cpu', 'max', '-cdrom', str(build/'logit.iso'),
           '-drive', 'file='+str(build/'disk.img')+',format=raw,if=none,id=hd0',
           '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd',
           '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
           '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none',
           '-no-reboot', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
           '-serial', 'file:'+str(serial), '-qmp', 'unix:'+str(sock)+',server,nowait']
    (out/'command.json').write_text(json.dumps(cmd, indent=2))
    proc = subprocess.Popen(cmd, stdout=open(out/'qemu.log', 'w'), stderr=subprocess.STDOUT)
    def wait(pattern, seconds=90):
        deadline = time.monotonic()+seconds
        while time.monotonic() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            match = re.search(pattern, text)
            if match:
                return match
            if '[fault] app exception:' in text:
                raise RuntimeError('PRODUCT: guest browser crashed: '+text[text.index('[fault] app exception:'):].splitlines()[0])
            if proc.poll() is not None:
                raise RuntimeError('HARNESS: QEMU exited before marker')
            time.sleep(.2)
        raise RuntimeError('HARNESS: guest marker missing: '+pattern)
    try:
        wait('desktop live')
        # The boot message precedes the first settled compositor/input frame;
        # immediate motion once launched Finder instead of the observed tile.
        time.sleep(3)
        u = Session(str(sock), serial=str(serial))
        u.launch_app('browser', probe=str(out/'launch-pointer.ppm'))
        # Launch acknowledges exec before Browser creates its native window.
        # Ctrl+L at that point goes to Finder; wait for the actual input target.
        wait(r'\[wm\] win \d+ frame [^\r\n]* Browser')
        u.key_mods(('ctrl',), 'l')
        u.typ('http://10.0.2.2:%d/' % main_server.server_port)
        u.key('ret')
        marker = args.marker or ('MODULE-RETRY-GUEST' if args.case == 'module-retry' else 'DYNAMIC-STYLES' if args.case == 'stylesheets' else 'DETACHED-IMAGES' if args.case == 'images' else 'RESPONSE-BOUNDARY')
        result = wait(marker+r' checks=(\d+) failures=(\d+)\r?\n')
        # Allow the pending invalidation to reach layout/paint. The saved image
        # is inspected separately; a console completion alone cannot certify it.
        time.sleep(2)
        u.screendump(str(out/'result.ppm'))
        from PIL import Image
        Image.open(out/'result.ppm').save(out/'result.png')
        record = {'checks': int(result[1]), 'failures': int(result[2]), 'requests': requests}
        (out/'result.json').write_text(json.dumps(record, indent=2))
        print(json.dumps(record), flush=True)
    finally:
        proc.terminate()
        proc.wait(timeout=15)
        for server in (cross, main_server):
            server.shutdown()
        artifacts['after'] = hashes()
        artifacts['unchanged'] = artifacts['before'] == artifacts['after']
        (out/'artifacts.json').write_text(json.dumps(artifacts, indent=2))
    if not artifacts['unchanged']:
        raise RuntimeError('HARNESS: guest artifacts changed')
    expected = args.expected_checks if args.expected_checks is not None else (3 if args.case == 'module-retry' else 13 if args.case == 'stylesheets' else 12 if args.case == 'images' else 25)
    return 0 if record['checks'] == expected and record['failures'] == 0 else 1

if __name__ == '__main__':
    raise SystemExit(main())
