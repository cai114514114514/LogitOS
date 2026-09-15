#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real chunked HTTP/XHR gate on an independent localhost-only frozen guest.

The server keeps EOF closed until after an early screenshot. Host-observed JS
progress releases the next chunk, so a completed-response imitation cannot
pass the streaming oracle. Callback exceptions and MIME classification use
separate requests: a failed callback cannot conceal a working network control.
"""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure
from PIL import Image

GREEN = (0, 255, 0)
ANCHOR = (17, 231, 197)
NAMES = ['instance-constants', 'headers-hook', 'JSON-not-SSE', 'early-incremental-text',
         'final-progress-before-load', 'header-throw-isolated', 'progress-throw-isolated', 'terminal-throw-isolated']
CHUNKS = [b'{"first":"alpha",', b'"second":"beta",', b'"done":true}']


def fixture():
    html = '<!doctype html><meta charset="utf-8"><style>html,body{margin:0;background:white}p{font:14px sans-serif;margin:0}</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>'
    for i, name in enumerate(NAMES):
        html += '<div id="r%d" style="position:absolute;left:20px;top:%dpx;width:16px;height:16px;background:#999"></div><p style="position:absolute;left:44px;top:%dpx">%s</p>' % (i, 62 + i * 34, 60 + i * 34, name)
    for i, mode in enumerate(('normal', 'noisy')):
        html += '<div id="early-%s" style="position:absolute;left:20px;top:%dpx;width:16px;height:16px;background:#999"></div><p style="position:absolute;left:44px;top:%dpx">%s: second chunk visible before EOF</p>' % (mode, 360 + i * 30, 358 + i * 30, mode)
    html += '''<button id="start" style="position:absolute;left:20px;top:440px;width:180px;height:40px">START XHR</button>
<script>
var started=false, reported=false, records={};
var constantNames=['UNSENT','OPENED','HEADERS_RECEIVED','LOADING','DONE'];
var constants=constantNames.every(function(n,i){return (new XMLHttpRequest())[n]===i});
function maybeReport(force){
 if(reported||!records.normal||!records.noisy)return;
 var n=records.normal,b=records.noisy;if(!force&&(!n.ends||!b.ends))return;reported=true;
 var parsed=n.json&&n.json.first==='alpha'&&n.json.second==='beta'&&n.json.done===true;
 var final=n.events.slice(-3).join(',')==='progress,load,loadend'&&n.finalText===n.x.responseText&&n.finalBytes===n.x.responseText.length;
 var checks=[constants,n.headers===1&&n.headerStatus===200&&n.mime.indexOf('application/json')===0,
  !!parsed&&!n.stream&&n.sseFeeds===0,n.first&&n.second&&b.first&&b.second,
  final,b.headerThrown&&b.errors===0&&b.loads===1&&b.x.status===200,
  b.progressThrows>=2&&b.laterProgress===b.progressThrows&&b.loads===1&&b.errors===0,
  b.loads===1&&b.ends===1&&b.loadThrown&&b.errors===0&&b.x.readyState===b.x.DONE];
 for(var i=0;i<checks.length;i++)document.getElementById('r'+i).style.backgroundColor=checks[i]?'#00ff00':'#ff0000';
 function brief(v){return {headers:v.headers,status:v.x.status,readyState:v.x.readyState,loads:v.loads,ends:v.ends,errors:v.errors,
  first:v.first,second:v.second,progress:v.progress,progressThrows:v.progressThrows,laterProgress:v.laterProgress,bytes:v.x.responseText.length,events:v.events};}
 console.log('XHR-GUEST REPORT '+JSON.stringify({checks:checks,normal:brief(n),noisy:brief(b),forced:!!force}));
}
function request(mode){
 var x=new XMLHttpRequest(),s={x:x,headers:0,headerStatus:0,mime:'',stream:true,sseFeeds:0,json:null,events:[],
  first:false,second:false,progress:0,progressThrows:0,laterProgress:0,headerThrown:false,loadThrown:false,
  loads:0,ends:0,errors:0,finalText:'',finalBytes:0};records[mode]=s;
 x.open('GET',mode==='normal'?'/json':'/throw');
 if(mode==='normal'){
  function headers(){if(x.readyState===x.HEADERS_RECEIVED){s.headers++;s.headerStatus=x.status;s.mime=x.getResponseHeader('content-type')||'';s.stream=s.mime.indexOf('text/event-stream')>=0;}x.removeEventListener('readystatechange',headers);}
  x.addEventListener('readystatechange',headers);
 }else x.onreadystatechange=function(){if(x.readyState===2&&!s.headerThrown){s.headerThrown=true;throw new Error('fixture-header-handler');}};
 x.onprogress=function(e){
  s.progress++;s.events.push('progress');s.finalText=x.responseText;s.finalBytes=e.loaded;
  if(mode==='normal'&&s.stream)s.sseFeeds++;
  if(x.readyState===3&&!s.loads&&x.responseText.indexOf('alpha')>=0&&!s.first){s.first=true;console.log('XHR-GUEST FIRST '+mode);}
  if(x.readyState===3&&!s.loads&&x.responseText.indexOf('beta')>=0&&!s.second){s.second=true;document.getElementById('early-'+mode).style.backgroundColor='#00ff00';console.log('XHR-GUEST SECOND '+mode);}
  if(mode==='noisy'){s.progressThrows++;throw new Error('fixture-progress-handler');}
 };
 if(mode==='noisy'){
  x.addEventListener('progress',function(){throw new Error('fixture-progress-listener');});
  x.addEventListener('progress',function(){s.laterProgress++;});
 }
 x.onload=function(){s.loads++;s.events.push('load');if(mode==='normal'&&!s.stream)s.json=JSON.parse(x.responseText);if(mode==='noisy'){s.loadThrown=true;throw new Error('fixture-load-handler');}};
 x.onerror=function(){s.errors++;s.events.push('error');};
 x.onloadend=function(){s.ends++;s.events.push('loadend');maybeReport(false);};
 x.send();
}
document.getElementById('start').addEventListener('click',function(){if(started)return;started=true;request('normal');request('noisy');setTimeout(function(){maybeReport(true)},14000);console.log('XHR-GUEST STARTED');});
setTimeout(function(){console.log('XHR-GUEST READY')},1000);
</script>'''
    return html.encode()


class FixtureState:
    def __init__(self):
        self.html = fixture()
        self.first_seen = {n: threading.Event() for n in ('normal', 'noisy')}
        self.release = threading.Event()
        self.lock = threading.Lock()
        self.timeline = []
    def record(self, mode, phase):
        with self.lock:
            self.timeline.append({'mode': mode, 'phase': phase, 'time': time.monotonic()})


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        state = self.server.fixture_state
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(state.html)))
            self.end_headers()
            self.wfile.write(state.html)
            return
        if self.path not in ('/json', '/throw'):
            self.send_error(404)
            return
        mode = 'normal' if self.path == '/json' else 'noisy'
        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Transfer-Encoding', 'chunked')
        self.send_header('Connection', 'close')
        self.end_headers()
        state.record(mode, 'headers')
        try:
            for i, data in enumerate(CHUNKS):
                self.wfile.write(('%x\r\n' % len(data)).encode() + data + b'\r\n')
                self.wfile.flush()
                state.record(mode, 'chunk-%d' % (i + 1))
                if i == 0:
                    state.first_seen[mode].wait(7)
                    time.sleep(.3)
                elif i == 1:
                    state.release.wait(15)
            self.wfile.write(b'0\r\n\r\n')
            self.wfile.flush()
            state.record(mode, 'EOF')
        except (BrokenPipeError, ConnectionResetError):
            state.record(mode, 'client-disconnected')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''): h.update(block)
    return h.hexdigest()


def capture(ui, out, name):
    path = out / (name + '.ppm')
    ui.screendump(str(path))
    im = Image.open(path).convert('RGB')
    im.save(path.with_suffix('.png'))
    px = im.load()
    found = [(x, y) for y in range(im.height) for x in range(im.width) if px[x, y] == ANCHOR]
    if len(found) != 16: raise RuntimeError('fixture anchor absent or ambiguous')
    anchor = (min(x for x, y in found), min(y for x, y in found))
    colors = [list(px[anchor[0] + 8, anchor[1] + 50 + i * 34]) for i in range(len(NAMES))]
    early = [list(px[anchor[0] + 8, anchor[1] + 348 + i * 30]) for i in range(2)]
    return anchor, colors, early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--expect-old', action='store_true')
    args = ap.parse_args()
    snapshot, out = args.snapshot.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    configure(1280, 900)
    state = FixtureState()
    (out / 'fixture.html').write_bytes(state.html)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), FixtureHandler)
    server.fixture_state = state
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial = out / 'serial.log'
    serial.write_text('')
    hashes = {n: digest(snapshot / n) for n in ('logit.iso', 'disk.img')}
    report = {'boundary': 'local chunked HTTP on independent restricted QEMU; no site or account requests',
              'snapshot': str(snapshot), 'inputs_sha256': hashes, 'fixture_sha256': hashlib.sha256(state.html).hexdigest(), 'expected_old': args.expect_old}
    try:
        with tempfile.TemporaryDirectory(prefix='xhr-stream-qmp-', dir='/tmp') as tmp:
            socket = str(Path(tmp) / 'qmp.sock')
            # One forwarding process per connection is necessary for the HTML
            # navigation plus TWO simultaneous XHR responses; a single shared
            # chardev would test its serialization, not browser concurrency.
            forwarding = 'user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:18080-cmd:/usr/bin/nc 127.0.0.1 %d' % server.server_port
            cmd = ['qemu-system-x86_64', '-cpu', 'max', '-cdrom', str(snapshot / 'logit.iso'),
                   '-drive', 'file=' + str(snapshot / 'disk.img') + ',format=raw,if=none,id=hd0', '-device', 'virtio-blk-pci,drive=hd0',
                   '-snapshot', '-boot', 'd', '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
                   '-device', 'virtio-gpu-pci,xres=1280,yres=900', '-display', 'none', '-no-reboot', '-netdev', forwarding,
                   '-device', 'e1000,netdev=n0', '-serial', 'file:' + str(serial), '-qmp', 'unix:' + socket + ',server,nowait']
            with (out / 'qemu.log').open('w') as log:
                proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
                report['pid'] = proc.pid
                def wait(marker, seconds=180):
                    deadline = time.monotonic() + seconds
                    while time.monotonic() < deadline:
                        text = serial.read_text(errors='replace')
                        if marker in text: return text
                        if proc.poll() is not None: raise RuntimeError('guest exited')
                        time.sleep(.15)
                    raise RuntimeError('guest marker absent: ' + marker)
                try:
                    wait('desktop live')
                    ui = Session(socket, serial=str(serial))
                    ui.launch_app('browser')
                    time.sleep(2)
                    ui.key_mods(('ctrl',), 't', settle=.3)
                    ui.typ('http://10.0.2.100:18080/')
                    ui.key('ret')
                    wait('XHR-GUEST READY')
                    anchor, _, _ = capture(ui, out, 'before')
                    target = (anchor[0] + 80, anchor[1] + 440)
                    ui.click_at_confirmed(str(out / 'pointer.ppm'), *target)
                    report['click'] = list(target)
                    wait('XHR-GUEST STARTED', 20)
                    observed = set()
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        text = serial.read_text(errors='replace')
                        for mode in ('normal', 'noisy'):
                            for phase in ('FIRST', 'SECOND'):
                                key = phase + ' ' + mode
                                if key not in observed and 'XHR-GUEST ' + key in text:
                                    observed.add(key)
                                    state.record(mode, 'observed-' + phase.lower())
                                    if phase == 'FIRST': state.first_seen[mode].set()
                        if all('SECOND ' + m in observed for m in ('normal', 'noisy')): break
                        time.sleep(.08)
                    time.sleep(.4)
                    _, _, early_pixels = capture(ui, out, 'early')
                    report['early_pixels'] = early_pixels
                    report['observed_before_release'] = sorted(observed)
                    report['early_screenshot_before_EOF'] = not any(v['phase'] == 'EOF' for v in state.timeline)
                    state.record('host', 'early-screenshot-complete')
                    state.release.set()
                    text = wait('XHR-GUEST REPORT ', 25)
                    m = re.search(r'XHR-GUEST REPORT (\{[^\r\n]+\})', text)
                    if not m: raise RuntimeError('malformed synthetic guest report')
                    report['guest'] = json.loads(m.group(1))
                    time.sleep(.7)
                    ui.goto(10, 10)
                    final_anchor, colors, _ = capture(ui, out, 'after')
                    report['pixels'] = colors
                    report['anchor_stable'] = list(anchor) == list(final_anchor)
                finally:
                    state.release.set()
                    for e in state.first_seen.values(): e.set()
                    proc.terminate()
                    try: proc.wait(timeout=15)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=10)
                    report['guest_stopped'] = proc.poll() is not None
            report['input_files_unchanged'] = hashes == {n: digest(snapshot / n) for n in hashes}
            g = report['guest']
            report['passed'] = sum(g['checks'])
            report['failed'] = len(NAMES) - report['passed']
            base = g['normal']['status'] == 200 and g['normal']['loads'] == 1 and g['normal']['errors'] == 0 and report['input_files_unchanged'] and report['anchor_stable']
            if args.expect_old:
                report['gate_pass'] = base and not g['checks'][0] and not g['checks'][1] and not g['checks'][2] and not g['checks'][5]
            else:
                report['gate_pass'] = base and report['failed'] == 0 and report['early_screenshot_before_EOF'] and all(v == list(GREEN) for v in report['pixels'] + report['early_pixels']) and not g['forced']
    except Exception as exc:
        report['error'] = str(exc)
        report['gate_pass'] = False
    finally:
        state.release.set()
        server.shutdown()
        report['server_timeline'] = state.timeline
        (out / 'results.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report, indent=2))
    return 0 if report.get('gate_pass') else 1


if __name__ == '__main__': raise SystemExit(main())
