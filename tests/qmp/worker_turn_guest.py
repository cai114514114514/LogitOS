#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Finite local Worker work: observe parent delivery before the next worker starts."""
import argparse, hashlib, http.server, json, re, subprocess, sys, tempfile, threading, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
NAMES=['both-finite-results','parent-message-before-second-worker','ordinary-parent-input-event']
ANCHOR=(17,231,197)
WORKER=b'''onmessage=function(e){var started=Date.now(),sum=0;
for(var i=0;i<200000;i++)sum=(sum+(i&255))|0;
postMessage({id:e.data,started:started,done:Date.now(),value:20+22,sum:sum});};'''
def fixture():
    s='<!doctype html><meta charset="utf-8"><style>body{margin:0;background:white}p{margin:0;font:14px sans-serif}</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>'
    for i,n in enumerate(NAMES):
        s+='<div id="r%d" style="position:absolute;left:20px;top:%dpx;width:16px;height:16px;background:#999"></div><p style="position:absolute;left:44px;top:%dpx">%s</p>'%(i,60+i*34,58+i*34,n)
    s+='''<input id="input" style="position:absolute;left:20px;top:200px;width:220px;height:32px"><button id="start" style="position:absolute;left:20px;top:260px;width:180px;height:40px">START FINITE WORK</button><script>
var begun=false,reported=false,rec={},events=0;
var input=document.getElementById('input');
input.addEventListener('input',function(){events++});
function report(force){if(reported)return;reported=true;
 var a=rec[1],b=rec[2],expected=25493856;
 var checks=[!!a&&!!b&&a.value===42&&b.value===42&&a.sum===expected&&b.sum===expected,
 !!a&&!!b&&a.parentAt<=b.started&&a.done-a.started>=8,
 events===1&&input.value==='42'];
 checks.forEach(function(v,i){document.getElementById('r'+i).style.backgroundColor=v?'#00ff00':'#ff0000'});
 console.log('WORKER-TURN REPORT '+JSON.stringify({checks:checks,records:rec,inputEvents:events,forced:!!force}));}
document.getElementById('start').addEventListener('click',function(){
 if(begun)return;begun=true;setTimeout(function(){report(true)},30000);
 var a=new Worker('/work.js'),b=new Worker('/work.js');
 function receive(e){var r=e.data;r.parentAt=Date.now();rec[r.id]=r;
 if(r.id===1){input.value='42';input.dispatchEvent(new Event('input'));a.terminate()}
 else b.terminate();if(rec[1]&&rec[2])report(false)}
 a.onmessage=receive;b.onmessage=receive;a.postMessage(1);b.postMessage(2);
});setTimeout(function(){console.log('WORKER-TURN READY')},1000);
</script>'''
    return s.encode()
def validate_report(report):
    # Two workers may share the cached script; each actual result remains required.
    g=report.get('guest',{});expected=[True,False,True] if report.get('expected_old') is True else [True]*3
    rec=g.get('records',{});a=rec.get('1',{});b=rec.get('2',{})
    numeric=all(type(r.get(k)) is int for r in (a,b) for k in ('started','done','parentAt','sum','value'))
    ordinary=numeric and all(r['started']<=r['done']<=r['parentAt'] and r['value']==42 and r['sum']==25493856 for r in (a,b))
    order=numeric and (a['parentAt']>b['started'] if report.get('expected_old') is True else a['parentAt']<=b['started']) and a['done']-a['started']>=8
    report['gate_pass']=bool('error' not in report and g.get('checks')==expected and g.get('forced') is False and ordinary and order and g.get('inputEvents')==1 and report.get('pixels')==[list((0,255,0) if x else (255,0,0)) for x in expected] and report.get('anchor_stable') and report.get('input_files_unchanged') and report.get('guest_stopped') and 1<=report.get('requests',[]).count('/work.js')<=2 and set(report.get('requests',[]))=={'/','/work.js'})
    return report['gate_pass']

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--snapshot',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--expect-old',action='store_true');args=ap.parse_args()
    from qmp_ui import Session,configure
    from PIL import Image
    configure(1280,900)
    snapshot=args.snapshot.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    assets={'/':fixture(),'/work.js':WORKER};requests=[]
    for name,body in assets.items():(out/('fixture.html' if name=='/' else name[1:])).write_bytes(body)
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*a):pass
        def do_GET(self):
            requests.append(self.path);body=assets.get(self.path)
            if body is None:self.send_error(404);return
            self.send_response(200);self.send_header('Content-Type','text/html' if self.path=='/' else 'text/javascript');self.send_header('Content-Length',str(len(body)));self.send_header('Connection','close');self.end_headers();self.wfile.write(body)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
    serial=out/'serial.log';serial.write_text('')
    def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
    initial={n:digest(snapshot/n) for n in ('disk.img','logit.iso')}
    report={'boundary':'isolated localhost, finite integer loops, ordinary parent synthetic input event; no keyboard-latency or native-preemption claim','snapshot':str(snapshot),'inputs_sha256':initial,'expected_old':args.expect_old,'fixture_sha256':digest(out/'fixture.html'),'worker_sha256':digest(out/'work.js')}
    def capture(ui,name):
        p=out/(name+'.ppm');ui.screendump(str(p));im=Image.open(p).convert('RGB');im.save(p.with_suffix('.png'));px=im.load()
        points=[(x,y) for y in range(im.height) for x in range(im.width) if px[x,y]==ANCHOR]
        if len(points)!=16:raise RuntimeError('page anchor missing or ambiguous')
        a=min(x for x,y in points),min(y for x,y in points)
        return a,[list(px[a[0]+8,a[1]+48+i*34]) for i in range(len(NAMES))]
    try:
        with tempfile.TemporaryDirectory(prefix='worker-turn-',dir='/tmp') as tmp:
            sock=str(Path(tmp)/'qmp.sock')
            network='user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:18080-cmd:/usr/bin/nc 127.0.0.1 %d'%server.server_port
            cmd=['qemu-system-x86_64','-cpu','max','-cdrom',str(snapshot/'logit.iso'),'-drive','file='+str(snapshot/'disk.img')+',format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-snapshot','-boot','d','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=900','-display','none','-no-reboot','-netdev',network,'-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+sock+',server,nowait']
            with (out/'qemu.log').open('w') as log:
                proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT);report['pid']=proc.pid
                def wait(marker,seconds=180):
                    deadline=time.monotonic()+seconds
                    while time.monotonic()<deadline:
                        text=serial.read_text(errors='replace')
                        if marker in text:return text
                        if proc.poll() is not None:raise RuntimeError('guest exited')
                        time.sleep(.15)
                    raise RuntimeError('guest marker absent: '+marker)
                try:
                    wait('[wm] launched Finder');ui=Session(sock,serial=str(serial));ui.launch_app('browser',probe=str(out/'dock-pointer.ppm'));time.sleep(2)
                    ui.key_mods(('ctrl',),'t',settle=.3);ui.typ('http://10.0.2.100:18080/');ui.key('ret');wait('WORKER-TURN READY')
                    anchor,_=capture(ui,'before');target=anchor[0]+80,anchor[1]+260
                    ui.click_at_confirmed(str(out/'pointer.ppm'),*target)
                    text=wait('WORKER-TURN REPORT ',45);m=re.search(r'WORKER-TURN REPORT (\{[^\r\n]+\})',text)
                    if not m:raise RuntimeError('malformed finite report')
                    report['guest']=json.loads(m.group(1));time.sleep(.8);ui.goto(10,10)
                    end,report['pixels']=capture(ui,'after');report['anchor_stable']=anchor==end
                finally:
                    proc.terminate()
                    try:proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
                    report['guest_stopped']=proc.poll() is not None
        report['input_files_unchanged']=initial=={n:digest(snapshot/n) for n in initial}
    except Exception as e:report['error']=str(e)
    finally:
        server.shutdown();report['requests']=requests
        validate_report(report)
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if report['gate_pass'] else 1
if __name__=='__main__':raise SystemExit(main())
