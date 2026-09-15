#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ordinary dual-origin Worker Fetch fixture; all traffic stays on localhost."""
import argparse
import http.server
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import hashlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))

NAMES = ['HTTP-worker-echo-control', 'parent-fetch-control', 'HTTP-worker-arrayBuffer',
         'Blob-worker-absolute-fetch', 'worker-CORS-allowed', 'worker-CORS-denied',
         'Blob-relative-base-rejected', 'parent-fetch-and-URL-after-workers']
ANCHOR = (17, 231, 197)
GREEN = (0, 255, 0)
RED = (255, 0, 0)


def validate_report(report):
    """Pure evidence check: never opens files, starts a guest, or sends HTTP.

    The previous gate accepted any TypeError as CORS denial and checked only
    two red rows in the old control. Keep the observed matrix, pixels, support
    records and actual server requests mutually consistent. Legacy reports do
    not name both ephemeral ports: infer only ports witnessed in their routes,
    never invent the old run's unused second-server port.
    """
    assertions = []
    def check(name, passed):
        assertions.append({'name': name, 'passed': bool(passed)})
    valid_report = isinstance(report, dict)
    check('report-object', valid_report)
    if not valid_report:
        report = {}
    old = report.get('expected_old') is True
    check('explicit-expected-mode', type(report.get('expected_old')) is bool)
    guest = report.get('guest')
    guest = guest if isinstance(guest, dict) else {}
    values = guest.get('checks')
    shape = isinstance(values, list) and len(values) == len(NAMES) and all(type(x) is bool for x in values)
    expected = [True, True, False, False, False, False, False, True] if old else [True] * len(NAMES)
    check('eight-boolean-checks', shape)
    check('exact-old-3-pass-5-fail' if old else 'exact-current-8-pass', shape and values == expected)
    check('finished-without-timeout', guest.get('forced') is False)
    check('page-anchor-stable', report.get('anchor_stable') is True)
    check('input-files-unchanged', report.get('input_files_unchanged') is True)
    check('private-guest-stopped', report.get('guest_stopped') is True)
    check('no-harness-error', 'error' not in report)
    check('reported-counts-agree', shape and type(report.get('passed')) is int and
          type(report.get('failed')) is int and report['passed'] == sum(values) and
          report['failed'] == len(values) - sum(values))
    pixels = report.get('pixels')
    pixel_shape = isinstance(pixels, list) and len(pixels) == len(NAMES)
    for i, name in enumerate(NAMES):
        color = list(GREEN if expected[i] else RED)
        check('pixel-' + name, pixel_shape and isinstance(pixels[i], list) and
              all(type(channel) is int for channel in pixels[i]) and pixels[i] == color)
    records = guest.get('records')
    records = records if isinstance(records, dict) else {}
    http = records.get('http'); blob = records.get('blob')
    http = http if isinstance(http, dict) else {}
    blob = blob if isinstance(blob, dict) else {}
    check('worker-support-records-agree', http.get('supported') is (not old) and blob.get('supported') is (not old))
    if not old:
        check('worker-result-records-agree', http.get('bytes') == 10 and blob.get('text') == 'worker' and
              blob.get('allowed') == 'allowed' and blob.get('denied') is True and blob.get('relativeRejected') is True)

    requests = report.get('local_requests')
    def valid_port(port):
        return type(port) is int and 0 < port < 65536
    request_shape = isinstance(requests, list) and all(isinstance(r, dict) and
                    valid_port(r.get('port')) and isinstance(r.get('path'), str) for r in requests)
    check('local-request-records-valid', request_shape)
    requests = requests if request_shape else []
    main_paths = {'/', '/echo.js', '/parent', '/parent-after', '/fetch-worker.js', '/bytes', '/worker-text'}
    main_ports = {r['port'] for r in requests if r['path'] in main_paths}
    cross_ports = {r['port'] for r in requests if r['path'] in {'/allowed', '/denied'}}
    declared = report.get('server_ports')
    if declared is None:
        main_port = next(iter(main_ports)) if len(main_ports) == 1 else None
        cross_port = next(iter(cross_ports)) if len(cross_ports) == 1 else None
        port_source = 'inferred-from-recorded-routes'
        check('legacy-port-mapping-unambiguous', len(main_ports) == 1 and len(cross_ports) <= 1)
    else:
        declared = declared if isinstance(declared, dict) else {}
        main_port, cross_port = declared.get('main'), declared.get('cross_origin')
        port_source = 'declared-and-checked-against-routes'
        check('declared-server-ports-valid', valid_port(main_port) and valid_port(cross_port))
        main_port = main_port if valid_port(main_port) else None
        cross_port = cross_port if valid_port(cross_port) else None
        check('declared-ports-agree-with-routes', main_ports == {main_port} and
              (not cross_ports or cross_ports == {cross_port}))
    # A legacy old report never exercised cross-origin fetch: its unused port
    # is unknown. A new pass always requires both witnessed, distinct origins.
    check('distinct-server-origins', valid_port(main_port) and
          ((old and declared is None and cross_port is None) or
           (valid_port(cross_port) and cross_port != main_port)))
    for path in ('/echo.js', '/parent', '/parent-after'):
        check('main-http-request-' + path[1:], any(r['port'] == main_port and r['path'] == path for r in requests))
    if not old:
        for path in ('/bytes', '/worker-text'):
            check('main-worker-http-request-' + path[1:], any(r['port'] == main_port and r['path'] == path for r in requests))
        for path in ('/allowed', '/denied'):
            check('second-origin-http-request-' + path[1:], any(r['port'] == cross_port and r['path'] == path for r in requests))
    return {'gate_pass': all(a['passed'] for a in assertions), 'expected_old': old,
            'server_ports': {'main': main_port, 'cross_origin': cross_port},
            'port_evidence': port_source, 'assertions': assertions}


def revalidate_file(source, destination=None):
    """Write a separate assessment, retaining the original evidence bytewise."""
    source = Path(source).resolve()
    destination = Path(destination).resolve() if destination else source.with_name('revalidated.json')
    if source == destination:
        raise ValueError('revalidation output must not overwrite original results')
    original = source.read_bytes()
    report = json.loads(original)
    validation = validate_report(report)
    output = {'source_results': str(source),
              'source_results_sha256': hashlib.sha256(original).hexdigest(),
              'original_gate_pass': report.get('gate_pass') if isinstance(report, dict) else None,
              'offline_only': True, 'gate_pass': validation['gate_pass'], 'validation': validation}
    destination.write_text(json.dumps(output, indent=2) + '\n')
    return output


def fixture():
    html = '<!doctype html><meta charset="utf-8"><style>html,body{margin:0;background:white}p{margin:0;font:14px sans-serif}</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>'
    for i, name in enumerate(NAMES):
        html += '<div id="r%d" style="position:absolute;left:20px;top:%dpx;width:16px;height:16px;background:#999"></div><p style="position:absolute;left:44px;top:%dpx">%s</p>' % (i, 60 + i * 34, 58 + i * 34, name)
    html += '''<button id="start" style="position:absolute;left:20px;top:370px;width:180px;height:40px">START FETCH</button><script>
var started=false,reported=false,checks=[false,false,false,false,false,false,false,false],rec={};
var parentURL=new URL('/stable',location.href);
function report(force){if(reported)return;reported=true;
 checks.forEach(function(ok,i){document.getElementById('r'+i).style.backgroundColor=ok?'#00ff00':'#ff0000'});
 console.log('WORKER-GUEST REPORT '+JSON.stringify({checks:checks,records:rec,forced:!!force}));}
function worker(url,data){return new Promise(function(resolve){var w;
 try{w=new Worker(url);w.onmessage=function(e){w.terminate();resolve(e.data)};w.onerror=function(){w.terminate();resolve({error:true})};w.postMessage(data)}
 catch(e){resolve({constructorError:true})}})}
document.getElementById('start').addEventListener('click',async function(){
 if(started)return;started=true;setTimeout(function(){report(true)},20000);
 var base=location.origin,other='http://10.0.2.100:18081';
 try{
  checks[0]=(await worker(base+'/echo.js',{n:6}))===42;
  checks[1]=(await (await fetch(base+'/parent')).text())==='parent';
  var source=await (await fetch(base+'/fetch-worker.js')).text();
  var blob=URL.createObjectURL(new Blob([source],{type:'text/javascript'}));
  var result=await Promise.all([worker(base+'/fetch-worker.js',{kind:'http',base:base,other:other}),worker(blob,{kind:'blob',base:base,other:other})]);
  URL.revokeObjectURL(blob);var a=result[0],b=result[1];rec.http=a;rec.blob=b;
  checks[2]=!!a.supported&&a.bytes===10;
  checks[3]=!!b.supported&&b.text==='worker';
  checks[4]=!!b.supported&&b.allowed==='allowed';
  checks[5]=!!b.supported&&b.denied===true;
  checks[6]=!!b.supported&&b.relativeRejected===true;
  checks[7]=(await (await fetch(base+'/parent-after')).text())==='after'&&parentURL.pathname==='/stable';
 }catch(e){rec.parentError=e.name||'error'}
 report(false);
});setTimeout(function(){console.log('WORKER-GUEST READY')},1000);
</script>'''
    return html.encode()

WORKER_SCRIPT = b"""onmessage=async function(e){var d=e.data,r={supported:typeof fetch==='function'};
 if(!r.supported){postMessage(r);return;}
 try{if(d.kind==='http'){var a=new Uint8Array(await (await fetch('./bytes')).arrayBuffer());r.bytes=a[0]+a[1]+a[2]+a[3];}
 else{r.text=await (await fetch(d.base+'/worker-text')).text();r.allowed=await (await fetch(d.other+'/allowed')).text();
 try{await fetch(d.other+'/denied');r.denied=false}catch(x){r.denied=x instanceof TypeError}
 try{await fetch('./worker-text');r.relativeRejected=false}catch(x){r.relativeRejected=x instanceof TypeError}}
 }catch(x){r.error=x.name||'error'}postMessage(r);
};"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', type=Path)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--expect-old', action='store_true')
    ap.add_argument('--revalidate', type=Path, help='offline: validate existing results, with no VM or HTTP')
    ap.add_argument('--revalidated-out', type=Path, help='separate offline output; defaults to sibling revalidated.json')
    args = ap.parse_args()
    if args.revalidate:
        if args.snapshot or args.out or args.expect_old:
            ap.error('--revalidate uses the recorded mode and cannot start a guest')
        assessment = revalidate_file(args.revalidate, args.revalidated_out)
        print(json.dumps(assessment, indent=2))
        return 0 if assessment['gate_pass'] else 1
    if not args.snapshot or not args.out or args.revalidated_out:
        ap.error('guest mode requires --snapshot and --out; offline output requires --revalidate')
    # Offline validation has no GUI, image or guest-runtime dependencies.
    from qmp_ui import Session, configure
    from xhr_stream_guest import digest
    from PIL import Image
    snapshot, out = args.snapshot.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    configure(1280, 900)
    html = fixture()
    assets = {'/': html, '/echo.js': b'onmessage=function(e){postMessage(e.data.n*7)};', '/fetch-worker.js': WORKER_SCRIPT, '/parent': b'parent', '/parent-after': b'after', '/bytes': bytes([1,2,3,4]), '/worker-text': b'worker', '/allowed': b'allowed', '/denied': b'denied'}
    (out / 'fixture.html').write_bytes(html)
    for path, data in assets.items():
        if path != '/': (out / path[1:]).write_bytes(data)
    requested = []
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self, *a): pass
        def do_GET(self):
            requested.append({'port': self.server.server_port, 'path': self.path})
            data = assets.get(self.path)
            if data is None: self.send_error(404); return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html' if self.path == '/' else 'text/javascript')
            if self.path == '/allowed': self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers(); self.wfile.write(data)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    second = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=second.serve_forever, daemon=True).start()
    serial = out / 'serial.log'; serial.write_text('')
    hashes = {n: digest(snapshot / n) for n in ('logit.iso', 'disk.img')}
    report = {'boundary': 'isolated localhost-only QEMU with ordinary text/bytes fetch scripts; no site/account code',
              'snapshot': str(snapshot), 'inputs_sha256': hashes, 'fixture_sha256': hashlib.sha256(html).hexdigest(), 'expected_old': args.expect_old,
              'server_ports': {'main': server.server_port, 'cross_origin': second.server_port}}
    def capture(ui, name):
        ppm = out / (name + '.ppm');ui.screendump(str(ppm))
        im = Image.open(ppm).convert('RGB');im.save(ppm.with_suffix('.png'));px = im.load()
        points = [(x,y) for y in range(im.height) for x in range(im.width) if px[x,y] == ANCHOR]
        if len(points) != 16: raise RuntimeError('page anchor missing or ambiguous')
        a = (min(x for x,y in points),min(y for x,y in points))
        return a, [list(px[a[0]+8,a[1]+48+i*34]) for i in range(len(NAMES))]
    try:
        with tempfile.TemporaryDirectory(prefix='worker-blob-qmp-', dir='/tmp') as tmp:
            socket = str(Path(tmp) / 'qmp.sock')
            forward = 'user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:18080-cmd:/usr/bin/nc 127.0.0.1 %d,guestfwd=tcp:10.0.2.100:18081-cmd:/usr/bin/nc 127.0.0.1 %d' % (server.server_port,second.server_port)
            cmd = ['qemu-system-x86_64','-cpu','max','-cdrom',str(snapshot/'logit.iso'),'-drive','file='+str(snapshot/'disk.img')+',format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-snapshot','-boot','d','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=900','-display','none','-no-reboot','-netdev',forward,'-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+socket+',server,nowait']
            with (out/'qemu.log').open('w') as log:
                proc = subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT);report['pid']=proc.pid
                def wait(marker,seconds=180):
                    deadline=time.monotonic()+seconds
                    while time.monotonic()<deadline:
                        text=serial.read_text(errors='replace')
                        if marker in text:return text
                        if proc.poll() is not None:raise RuntimeError('guest exited')
                        time.sleep(.15)
                    raise RuntimeError('guest marker absent: '+marker)
                try:
                    # desktop-live precedes the asynchronous boot Finder launch.
                    # Under two TCG guests it arrived after launch_app's mark and
                    # looked like a wrong dock click, despite Browser launching.
                    # Drain that named boot event before marking our own click.
                    wait('[wm] launched Finder');ui=Session(socket,serial=str(serial))
                    ui.launch_app('browser',probe=str(out/'dock-pointer.ppm'));time.sleep(2)
                    ui.key_mods(('ctrl',),'t',settle=.3);ui.typ('http://10.0.2.100:18080/');ui.key('ret');wait('WORKER-GUEST READY')
                    anchor,_=capture(ui,'before');target=(anchor[0]+80,anchor[1]+370)
                    ui.click_at_confirmed(str(out/'pointer.ppm'),*target);report['click']=list(target)
                    text=wait('WORKER-GUEST REPORT ',45)
                    match=re.search(r'WORKER-GUEST REPORT (\{[^\r\n]+\})',text)
                    if not match:raise RuntimeError('malformed synthetic report')
                    report['guest']=json.loads(match.group(1));time.sleep(.8);ui.goto(10,10)
                    end_anchor,colors=capture(ui,'after');report['pixels']=colors;report['anchor_stable']=anchor==end_anchor
                finally:
                    proc.terminate()
                    try:proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
                    report['guest_stopped']=proc.poll() is not None
            g=report['guest'];report['passed']=sum(g['checks']);report['failed']=len(NAMES)-report['passed']
            report['input_files_unchanged']=hashes=={n:digest(snapshot/n) for n in hashes}
    except Exception as exc:report['error']=str(exc);report['gate_pass']=False
    finally:
        server.shutdown();second.shutdown();report['local_requests']=requested
        report['validation']=validate_report(report)
        report['gate_pass']=report['validation']['gate_pass']
        (out/'results.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    return 0 if report.get('gate_pass') else 1


if __name__=='__main__':raise SystemExit(main())
