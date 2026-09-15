#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ordinary echo/math Blob Worker fixture; never loads site worker algorithms."""
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
from qmp_ui import Session, configure
from xhr_stream_guest import digest
from PIL import Image

NAMES = ['HTTP-worker-control', 'Blob-parent-child-echo', 'Blob-creator-origin', 'URL-and-encoding-globals',
         'Blob-ordinary-importScripts', 'revoked-before-construction', 'foreign-Blob-refused', 'terminate-before-start']
ANCHOR = (17, 231, 197)
GREEN = (0, 255, 0)


def fixture():
    html = '<!doctype html><meta charset="utf-8"><style>html,body{margin:0;background:white}p{margin:0;font:14px sans-serif}</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>'
    for i, name in enumerate(NAMES):
        html += '<div id="r%d" style="position:absolute;left:20px;top:%dpx;width:16px;height:16px;background:#999"></div><p style="position:absolute;left:44px;top:%dpx">%s</p>' % (i, 60 + i * 34, 58 + i * 34, name)
    html += '''<button id="start" style="position:absolute;left:20px;top:370px;width:180px;height:40px">START WORKERS</button><script>
var started=false,reported=false,rec={};
['http','blob','pure','imported','revoked','foreign'].forEach(function(k){rec[k]={created:false,done:false,error:0,data:null}});
var cancelled=0,cancelReady=false,cancelConstructed=false,blobHref='';
function report(force){
 if(reported||!cancelReady)return;
 if(!force&&!Object.keys(rec).every(function(k){return rec[k].done}))return;reported=true;
 var b=rec.blob.data,p=rec.pure.data;
 var checks=[rec.http.data===42,!!b&&b.answer===42,!!b&&b.href===blobHref&&b.origin===location.origin&&b.locOrigin===location.origin&&b.noDOM,
  !!p&&p.url&&p.params&&p.encoding&&p.locationString,rec.imported.data===42,
  rec.revoked.created&&rec.revoked.error===1,rec.foreign.created&&rec.foreign.error===1,cancelConstructed&&cancelled===0];
 checks.forEach(function(ok,i){document.getElementById('r'+i).style.backgroundColor=ok?'#00ff00':'#ff0000'});
 console.log('WORKER-GUEST REPORT '+JSON.stringify({checks:checks,records:rec,forced:!!force}));
}
function worker(name,url,send){var r=rec[name];try{
 var w=new Worker(url);r.created=true;
 w.onmessage=function(e){r.data=e.data;r.done=true;w.terminate();report(false)};
 w.onerror=function(){r.error++;r.done=true;w.terminate();report(false)};
 if(send)w.postMessage({n:6});
 }catch(e){r.constructorError=e.name;r.done=true;report(false)}
}
document.getElementById('start').addEventListener('click',function(){
 if(started)return;started=true;
 var base=location.origin;
 worker('http',base+'/echo.js',true);
 var src="onmessage=function(e){postMessage({answer:e.data.n*7,href:location.href,origin:origin,locOrigin:location.origin,noDOM:typeof document==='undefined'})}";
 var u=URL.createObjectURL(new Blob([src],{type:'text/javascript'}));blobHref=u;
 worker('blob',u,true);URL.revokeObjectURL(u);
 worker('pure',base+'/pure.js',false);
 var imp="var base="+JSON.stringify(base+'/math.js')+";importScripts(String(new URL('./math.js',base)));postMessage(answer);";
 var iu=URL.createObjectURL(new Blob([imp],{type:'text/javascript'}));worker('imported',iu,false);URL.revokeObjectURL(iu);
 var ru=URL.createObjectURL(new Blob(['postMessage(1)']));URL.revokeObjectURL(ru);worker('revoked',ru,false);
 worker('foreign','blob:http://unowned.example/no-such-entry',false);
 var tu=URL.createObjectURL(new Blob(['postMessage(99)']));try{var tw=new Worker(tu);cancelConstructed=true;tw.onmessage=function(){cancelled++};tw.terminate()}catch(e){}URL.revokeObjectURL(tu);
 setTimeout(function(){cancelReady=true;report(false)},1500);setTimeout(function(){report(true)},8000);
});setTimeout(function(){console.log('WORKER-GUEST READY')},1000);
</script>'''
    return html.encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--expect-old', action='store_true')
    args = ap.parse_args()
    snapshot, out = args.snapshot.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    configure(1280, 900)
    html = fixture()
    assets = {'/': html, '/echo.js': b'onmessage=function(e){postMessage(e.data.n*7)};', '/math.js': b'self.answer=6*7;',
              '/pure.js': b"postMessage({url:new URL('child',location).pathname==='/child',params:new URLSearchParams('a=two+words').get('a')==='two words',encoding:new TextDecoder().decode(new TextEncoder().encode('hello'))==='hello',locationString:String(location)===location.href});"}
    (out / 'fixture.html').write_bytes(html)
    for path, data in assets.items():
        if path != '/': (out / path[1:]).write_bytes(data)
    requested = []
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        def log_message(self, *a): pass
        def do_GET(self):
            requested.append(self.path)
            data = assets.get(self.path)
            if data is None: self.send_error(404); return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html' if self.path == '/' else 'text/javascript')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers(); self.wfile.write(data)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial = out / 'serial.log'; serial.write_text('')
    hashes = {n: digest(snapshot / n) for n in ('logit.iso', 'disk.img')}
    report = {'boundary': 'isolated localhost-only QEMU with ordinary echo/math scripts; no site/account/challenge code',
              'snapshot': str(snapshot), 'inputs_sha256': hashes, 'fixture_sha256': hashlib.sha256(html).hexdigest(), 'expected_old': args.expect_old}
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
            forward = 'user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:18080-cmd:/usr/bin/nc 127.0.0.1 %d' % server.server_port
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
                    text=wait('WORKER-GUEST REPORT ',25)
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
            base=g['checks'][0] and report['anchor_stable'] and report['input_files_unchanged'] and not g['forced']
            if args.expect_old:report['gate_pass']=base and not g['checks'][1] and not g['checks'][3]
            else:report['gate_pass']=base and report['failed']==0 and all(c==list(GREEN) for c in colors)
    except Exception as exc:report['error']=str(exc);report['gate_pass']=False
    finally:
        server.shutdown();report['local_requests']=requested
        (out/'results.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    return 0 if report.get('gate_pass') else 1


if __name__=='__main__':raise SystemExit(main())
