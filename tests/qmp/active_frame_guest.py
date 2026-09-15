#!/usr/bin/env python3
"""Real two-origin iframe pixels, native click, fetch, and window messages.

The click targets only our local fixture, never a public human challenge.
An optional public navigation is observation-only and can leave a visible,
owned guest running for the user. No JS injection or answer automation there.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
from qmp_ui import Session, browser_client_point


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--disk', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--public-url')
    ap.add_argument('--keep-open', action='store_true')
    ap.add_argument('--large-script', action='store_true')
    ap.add_argument('--workers', action='store_true')
    ap.add_argument('--network-worker', action='store_true')
    ap.add_argument('--ports', action='store_true', help='transfer a real Window port and pump native Worker channels')
    ap.add_argument('--tab-roundtrip', action='store_true', help='recreate embedded runtime after visiting a differently restricted tab')
    args = ap.parse_args()
    if args.ports:
        args.network_worker = True
    if args.network_worker:
        args.workers = True
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / 'serial.log'
    requests = []
    result = {'passed': False, 'requests': requests, 'public_search_accepted': False}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            path = self.path.split('?')[0]
            record = {'port': self.server.server_port, 'path': path}
            requests.append(record)
            parent_origin = 'http://10.0.2.2:%d' % parent_server.server_port
            child_origin = 'http://10.0.2.2:%d' % child_server.server_port
            if path == '/parent':
                text = """<!doctype html><meta charset=utf-8><title>Active inline frame</title>
<style>body{margin:24px;background:#eef2f8;font:18px sans-serif}iframe{display:block;width:340px;height:220px;border:3px solid #315591;margin-top:20px}</style>
<h1>FRAME-PARENT-RETAINED</h1><iframe id=f src='%s/child'></iframe>
<p id=status>Waiting for the embedded button.</p>
<script>var w=document.getElementById('f').contentWindow;
addEventListener('message',function(e){if(!e.isTrusted||e.origin!==%s||e.source!==w||e.data!=='clicked')return;
document.getElementById('status').textContent='PARENT-MESSAGE-PASS';
e.source.postMessage('reply',%s);console.log('FRAME-PARENT-MESSAGE-PASS');});</script>""" % (
                    child_origin, json.dumps(child_origin), json.dumps(child_origin))
                mime = 'text/html'
                if args.workers:
                    # The parent's Worker must stay live while the child
                    # creates/imports/fetches in a different native owner.
                    setup = """var pendingChild=null;
var parentWorkerURL=URL.createObjectURL(new Blob(["onmessage=function(e){postMessage(e.data)}"],{type:'text/javascript'}));
var parentWorker=new Worker(parentWorkerURL);URL.revokeObjectURL(parentWorkerURL);
parentWorker.onmessage=function(e){if(e.data!=='reply'||!pendingChild)throw Error('parent Worker reply');
pendingChild.postMessage('reply',%s);};
""" % json.dumps(child_origin)
                    text = text.replace('<script>var w=', '<script>' + setup + 'var w=')
                    text = text.replace("e.source.postMessage('reply',%s);" % json.dumps(child_origin),
                                        "pendingChild=e.source;parentWorker.postMessage('reply');")
                if args.ports:
                    text = text[:text.index('<script>')] + """<script>
var w=document.getElementById('f').contentWindow,remotePort=null,handoffs=0;
var u=URL.createObjectURL(new Blob(["onmessage=function(e){postMessage(e.data)}"],{type:'text/javascript'}));
var parentWorker=new Worker(u);URL.revokeObjectURL(u);
parentWorker.onmessage=function(e){if(e.data!=='reply'||!remotePort)throw Error('parent Worker reply');remotePort.postMessage('reply')};
addEventListener('message',function(e){
if(!e.isTrusted||e.origin!==%s||e.source!==w||e.data!=='port-handoff')return;
if(++handoffs!==1||e.ports.length!==1||!Object.isFrozen(e.ports)||!(e.ports[0] instanceof MessagePort))throw Error('port ownership');
remotePort=e.ports[0];remotePort.onmessage=function(e){
if(!e.isTrusted||e.target!==remotePort||e.data!=='clicked')throw Error('native port event');
document.getElementById('status').textContent='PARENT-MESSAGE-PASS';
console.log('FRAME-PARENT-MESSAGE-PASS');parentWorker.postMessage('reply')};
remotePort.postMessage('port-attached');console.log('FRAME-WINDOW-PORT-HANDOFF-PASS');
});</script>""" % json.dumps(child_origin)
            elif path == '/other':
                text, mime = '<!doctype html><title>Other policy tab</title><p>OTHER-POLICY-TAB</p>', 'text/html'
            elif path == '/child':
                text = """<!doctype html><style>body{margin:12px;background:white;font:18px sans-serif}#button{padding:16px;background:#9ee9a7;width:260px;height:90px;cursor:pointer}</style>
<div id=button role=button tabindex=0>FRAME-PENDING</div><script src='/child.js'></script>"""
                mime = 'text/html'
            elif path == '/child.js':
                text = """var button=document.querySelector('div[role=button]');
if(document.querySelectorAll('div[role]').length!==1||!button.matches('#button'))throw Error('child selectors');
fetch('/body.txt').then(function(r){return r.text()}).then(function(s){
if(s!=='owned-frame-response')throw Error('body mismatch');button.textContent='FRAME-READY';});
button.addEventListener('click',function(e){if(!e.isTrusted)throw Error('native click required');
button.textContent='FRAME-CLICKED';parent.postMessage('clicked',%s);});
addEventListener('message',function(e){if(!e.isTrusted||e.origin!==%s||e.source!==parent||e.data!=='reply')return;
button.textContent='FRAME-GUEST-PASS';console.log('FRAME-CHILD-MESSAGE-PASS');});""" % (
                    json.dumps(parent_origin), json.dumps(parent_origin))
                mime = 'text/javascript'
                if args.workers:
                    worker_code = ("importScripts(%s);fetch(%s).then(function(r){return r.text()})"
                                   ".then(function(s){if(s!=='owned-frame-response'||workerValue!==42)throw Error('worker resources');postMessage('ready')});"
                                   "onmessage=function(e){postMessage(e.data)};" %
                                   (json.dumps(child_origin + '/worker-lib.js'),
                                    json.dumps(child_origin + '/body.txt')))
                    setup = ("var workerReady=false,fetchReady=false;"
                             "function ready(){if(workerReady&&fetchReady)button.textContent='FRAME-READY'};"
                             "var workerURL=URL.createObjectURL(new Blob([%s],{type:'text/javascript'}));"
                             "var childWorker=new Worker(workerURL);URL.revokeObjectURL(workerURL);"
                             "childWorker.onmessage=function(e){if(e.data==='ready'){workerReady=true;ready()}"
                             "else if(e.data==='clicked')parent.postMessage('clicked',%s)};" %
                             (json.dumps(worker_code), json.dumps(parent_origin)))
                    if args.network_worker:
                        setup = setup.replace(
                            "var workerURL=URL.createObjectURL(new Blob([%s],{type:'text/javascript'}));" % json.dumps(worker_code),
                            "var workerURL='/worker-entry.js';")
                    text = text.replace("fetch('/body.txt')", setup + "fetch('/body.txt')")
                    text = text.replace("button.textContent='FRAME-READY';", "fetchReady=true;ready();")
                    # Leave ready()'s paint assignment intact (the replacement
                    # above also sees the inserted function's source).
                    text = text.replace("if(workerReady&&fetchReady)fetchReady=true;ready()",
                                        "if(workerReady&&fetchReady)button.textContent='FRAME-READY'")
                    text = text.replace("parent.postMessage('clicked',%s);});" % json.dumps(parent_origin),
                                        "childWorker.postMessage('clicked');});")
                if args.ports:
                    # Separate fixture mode: its painted result requires a
                    # transferred endpoint, not the old Window data fallback.
                    text = """var button=document.querySelector('div[role=button]');
var fetchReady=false,workerReady=false,portReady=false,channel=new MessageChannel();
function ready(){if(fetchReady&&workerReady&&portReady)button.textContent='FRAME-READY'}
channel.port1.onmessage=function(e){
if(!e.isTrusted||e.target!==channel.port1)throw Error('child native port event');
if(e.data==='port-attached'){portReady=true;ready()}
else if(e.data==='reply'){button.textContent='FRAME-GUEST-PASS';console.log('FRAME-CHILD-MESSAGE-PASS')}
else throw Error('unexpected port reply')};
parent.postMessage('port-handoff',%s,[channel.port2]);
var childWorker=new Worker('/worker-entry.js');
childWorker.onmessage=function(e){if(e.data==='ready'){workerReady=true;ready()}
else if(e.data==='clicked')channel.port1.postMessage('clicked');else throw Error('worker reply')};
fetch('/body.txt').then(r=>r.text()).then(function(s){if(s!=='owned-frame-response')throw Error('body');fetchReady=true;ready()});
button.addEventListener('click',function(e){if(!e.isTrusted)throw Error('native click');
button.textContent='FRAME-CLICKED';childWorker.postMessage('clicked')});""" % json.dumps(parent_origin)
                if args.large_script:
                    text = '/*' + 'x' * (1024 * 1024) + '*/' + text
            elif path == '/worker-entry.js':
                text = ("var denied=false;try{eval('1')}catch(e){denied=e.name==='EvalError'};"
                        "importScripts('./worker-lib.js');fetch('./body.txt').then(r=>r.text()).then(function(s){"
                        "if(!denied||s!=='owned-frame-response'||workerValue!==42)throw Error('network worker policy/resources');"
                        "postMessage('ready')});onmessage=function(e){postMessage(e.data)};")
                mime = 'text/javascript'
                if args.ports:
                    # Both startup and echo must traverse the Worker's native
                    # channel queue, so a constructor-only stub cannot pass.
                    text = ("var workerChannel=new MessageChannel();workerChannel.port2.onmessage=function(e){postMessage(e.data)};" +
                            text.replace("postMessage('ready')", "workerChannel.port1.postMessage('ready')")
                                .replace("onmessage=function(e){postMessage(e.data)}", "onmessage=function(e){workerChannel.port1.postMessage(e.data)}"))
            elif path == '/worker-lib.js':
                text, mime = 'var workerValue=42;', 'text/javascript'
            elif path == '/body.txt':
                text, mime = 'owned-frame-response', 'text/plain'
            else:
                self.send_error(404)
                return
            body = text.encode()
            record['response_bytes'] = len(body)
            self.send_response(200)
            self.send_header('Content-Type', mime)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            if args.tab_roundtrip and path == '/parent':
                self.send_header('Content-Security-Policy', 'frame-src ' + child_origin)
            if path == '/other':
                self.send_header('Content-Security-Policy', "frame-src 'none'")
            if path == '/child':
                self.send_header('Content-Security-Policy',
                                 "script-src 'self';worker-src 'self' blob:;connect-src 'self';style-src 'unsafe-inline';frame-ancestors " + parent_origin)
            if path == '/worker-entry.js':
                self.send_header('Content-Security-Policy', "script-src 'self';connect-src 'self'")
            self.end_headers()
            self.wfile.write(body)

    parent_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    child_server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    for server in (parent_server, child_server):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    # Persistent socket directory is reported for manual continuation. It is
    # not a shared VM's monitor; all commands below address this owned process.
    socket_dir = Path(tempfile.mkdtemp(prefix='active-frame-', dir='/tmp'))
    sock = str(socket_dir / 'qmp.sock')
    cmd = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
           '-cdrom', str(Path(args.iso).resolve()), '-drive',
           'file=' + str(Path(args.disk).resolve()) + ',format=raw,if=none,id=hd0',
           '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd',
           '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
           '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display',
           'cocoa' if args.keep_open else 'none', '-no-reboot', '-netdev',
           'user,id=n0', '-device', 'e1000,netdev=n0', '-serial', 'file:' + str(serial),
           '-qmp', 'unix:' + sock + ',server,nowait']
    (out / 'launch.json').write_text(json.dumps(cmd, indent=2))
    proc = None
    ui = None
    leave_running = False

    def log():
        return serial.read_text(errors='replace') if serial.exists() else ''

    def wait(pattern, seconds=120, since=0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            match = re.search(pattern, log()[since:], re.M)
            if match:
                return match
            if proc.poll() is not None:
                raise RuntimeError('owned guest exited')
            time.sleep(.2)
        raise RuntimeError('missing guest evidence: ' + pattern)

    def capture(ui, name):
        from PIL import Image
        ui.screendump(str(out / (name + '.ppm')))
        Image.open(out / (name + '.ppm')).save(out / (name + '.png'))

    try:
        with (out / 'qemu.log').open('w') as qlog:
            proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT,
                                    start_new_session=True)
        result.update(pid=proc.pid, qmp=sock)
        wait('desktop live')
        # The boot Finder launch follows "desktop live" asynchronously.
        # Let it finish before launch_app's evidence mark, or the helper
        # attributes that unrelated startup line to our verified dock click.
        wait(r'^\[wm\] launched Finder\r?$')
        ui = Session(sock, serial=str(serial))
        ui.launch_app('browser', probe=str(out / 'launch-pointer.ppm'))
        ui.key_mods(('ctrl',), 't', settle=.3)
        ui.typ('http://10.0.2.2:%d/parent' % parent_server.server_port)
        ui.key('ret')
        hit = wait(r'^\[dl\] (\d+),(\d+) FRAME-READY\r?$')
        capture(ui, 'before-click')
        point = browser_client_point(log(), int(hit[1]) + 12, int(hit[2]) - 60 + 8)
        result['fixture_click'] = point
        ui.click_at_confirmed(str(out / 'frame-pointer.ppm'), *point)
        # Kernel benchmark writers may interleave characters inside a [dl]
        # line. Our fixture never logs FRAME-GUEST-PASS from JavaScript. If
        # that painted marker arrived but its prefix was torn, resample the
        # existing display list and prove this extra key did NOT fix stale
        # pixels: the entire fixture viewport must be identical before/after.
        wait('FRAME-CHILD-MESSAGE-PASS')
        wait('FRAME-GUEST-PASS')
        if not re.search(r'^\[dl\] \d+,\d+ FRAME-GUEST-PASS\r?$', log(), re.M):
            from PIL import Image, ImageChops
            capture(ui, 'before-trace-resample')
            ui.key_mods(('ctrl', 'alt'), 'd', settle=.2)
            wait(r'^\[dl\] \d+,\d+ FRAME-GUEST-PASS\r?$')
            capture(ui, 'after-trace-resample')
            bounds = (*browser_client_point(log(), 0, 0),
                      *browser_client_point(log(), 600, 480))
            before = Image.open(out / 'before-trace-resample.png').crop(bounds)
            after = Image.open(out / 'after-trace-resample.png').crop(bounds)
            assert ImageChops.difference(before, after).getbbox() is None, 'input repaired stale fixture pixels'
            result['paint_trace_resampled_without_pixel_change'] = True
        wait(r'^\[dl\] \d+,\d+ FRAME-GUEST-PASS\r?$')
        wait(r'^\[dl\] \d+,\d+ PARENT-MESSAGE-PASS\r?$')
        wait('FRAME-PARENT-MESSAGE-PASS')
        wait('FRAME-CHILD-MESSAGE-PASS')
        capture(ui, 'embedded-pass')
        assert all(any(r['path'] == p for r in requests)
                   for p in ('/parent', '/child', '/child.js', '/body.txt'))
        if args.workers:
            assert sum(r['path'] == '/worker-lib.js' for r in requests) == 1
            assert sum(r['path'] == '/body.txt' for r in requests) == 2
            result['parent_child_workers_passed'] = True
            if args.network_worker:
                assert sum(r['path'] == '/worker-entry.js' for r in requests) == 1
                result['network_worker_response_policy_passed'] = True
        if args.ports:
            wait('FRAME-WINDOW-PORT-HANDOFF-PASS')
            result['window_port_transfer_and_worker_channel_passed'] = True
        if args.tab_roundtrip:
            # Do not let old painted lines satisfy the second load. The
            # separate log boundary is as important as clearing a host recorder.
            since = len(log())
            ui.key_mods(('ctrl',), 't', settle=.3)
            ui.typ('http://10.0.2.2:%d/other' % parent_server.server_port)
            ui.key('ret')
            wait(r'^\[dl\] \d+,\d+ OTHER-POLICY-TAB\r?$', since=since)
            # The first paint occurs inside synchronous load. That path only
            # consumes close/progress input, so switching tabs there can be
            # swallowed. Require its actual lifecycle completion before input.
            wait(r'^\[load-complete\]', since=since)
            since = len(log())
            ui.key_mods(('ctrl', 'shift'), 'tab', settle=.3)
            hit = wait(r'^\[dl\] (\d+),(\d+) FRAME-READY\r?$', since=since)
            capture(ui, 'restored-before-click')
            point = browser_client_point(log(), int(hit[1])+12, int(hit[2])-60+8)
            ui.click_at_confirmed(str(out/'restored-pointer.ppm'), *point)
            wait('FRAME-CHILD-MESSAGE-PASS', since=since)
            wait(r'^\[dl\] \d+,\d+ FRAME-GUEST-PASS\r?$', since=since)
            wait(r'^\[dl\] \d+,\d+ PARENT-MESSAGE-PASS\r?$', since=since)
            capture(ui, 'restored-embedded-pass')
            assert sum(r['path'] == '/parent' for r in requests) == 1, 'parent bytes were refetched, not hydrated'
            assert sum(r['path'] == '/child' for r in requests) == 2, 'new child response policy was not loaded'
            result['tab_roundtrip_parent_bytes_and_live_child_passed'] = True
        result['passed'] = True
        result['browser_sha256'] = hashlib.sha256(
            Path(args.disk).resolve().with_name('browser.aex').read_bytes()).hexdigest()
        if args.public_url:
            before = len(log())
            ui.key_mods(('ctrl',), 't', settle=.3)
            ui.typ(args.public_url)
            ui.key('ret')
            end = time.monotonic() + 90
            while time.monotonic() < end and proc.poll() is None:
                time.sleep(.5)
            capture(ui, 'public-observation')
            result['public_frame_status'] = re.findall(r'^\[iframe\][^\r\n]*', log()[before:], re.M)
        leave_running = args.keep_open and proc.poll() is None
        result['left_running'] = leave_running
    except Exception as exc:
        result['error'] = str(exc)
        if ui is not None and proc is not None and proc.poll() is None:
            try:
                capture(ui, 'failure-observation')
            except Exception:
                pass
        raise
    finally:
        if proc is not None and not leave_running and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
        for server in (parent_server, child_server):
            server.shutdown()
        (out / 'results.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
