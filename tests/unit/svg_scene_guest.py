#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Independent ordinary SVG image/inline pixel gate; never attaches to live VM.

Run a frozen ISO/disk with QEMU -snapshot and serve only local synthetic assets.
The uniquely coloured page anchor locates content pixels without assuming the
browser window border/header size. Positive/old runs consume identical bytes.
"""
import argparse, hashlib, http.server, json, subprocess, sys, tempfile, threading, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from qmp_ui import Session, configure
from PIL import Image
CASES = [
 ('static', '<rect width="16" height="16" fill="red"/>', [(4,4,(255,0,0)),(20,4,(255,255,255))]),
 ('currentColor', '<g color="#00ff00" fill="currentColor"><rect width="16" height="16"/></g>', [(4,4,(0,255,0))]),
 ('transform', '<g transform="translate(16 0)"><rect width="8" height="8" fill="red"/></g>', [(4,4,(255,255,255)),(20,4,(255,0,0))]),
 ('use', '<defs><rect id="shape" width="16" height="16"/></defs><use href="#shape" fill="red"/>', [(4,4,(255,0,0))]),
 ('clip', '<defs><clipPath id="clip"><rect width="8" height="16"/></clipPath></defs><rect width="16" height="16" fill="red" clip-path="url(#clip)"/>', [(4,4,(255,0,0)),(12,4,(255,255,255))]),
 ('boxclip', '<defs><clipPath id="clip" clipPathUnits="objectBoundingBox"><rect width=".5" height="1"/></clipPath></defs><rect x="8" y="8" width="16" height="16" fill="blue" clip-path="url(#clip)"/>', [(12,12,(0,0,255)),(20,12,(255,255,255))]),
 ('stroke', '<path d="M2 2v4" fill="none" stroke="red" stroke-width="2" transform="scale(3 2)"/>', [(4,6,(255,0,0)),(7,6,(255,0,0)),(2,6,(255,255,255))]),
 ('instance', '<defs fill="blue"><g id="shape"><rect width="8" height="8" fill="currentColor"/></g></defs><use href="#shape" color="red"/><use href="#shape" x="16" color="#00ff00"/>', [(4,4,(255,0,0)),(20,4,(0,255,0))]),
]
def wrapped(inner):
 return '<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">'+inner+'</svg>'
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--snapshot',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--expect-old',action='store_true');args=ap.parse_args()
 out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);configure(1280,900)
 assets={'/case%d.svg'%i:wrapped(v[1]).encode() for i,v in enumerate(CASES)}
 html='<!doctype html><meta charset="utf-8"><style>html,body{margin:0;background:white}img{display:block}p{margin:0;font:12px sans-serif}</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>'
 for i,(name,inner,expected) in enumerate(CASES):
  x=20+(i%4)*100;y=44+(i//4)*80
  html+='<div style="position:absolute;left:%dpx;top:%dpx;width:32px;height:32px"><img width="32" height="32" src="/case%d.svg"><p>%s</p></div>'%(x,y,i,name)
 html+='<script>setTimeout(function(){console.log("SVG-SCENE READY")},2000)</script>'
 assets['/']=html.encode();(out/'fixture.html').write_text(html)
 for name,data in assets.items():
  if name!='/':(out/name[1:]).write_bytes(data)
 class Handler(http.server.BaseHTTPRequestHandler):
  def log_message(self,*a):pass
  def do_GET(self):
   data=assets.get(self.path)
   if data is None:self.send_error(404);return
   self.send_response(200);self.send_header('Content-Type','text/html' if self.path=='/' else 'image/svg+xml');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
 serial=out/'serial.log';serial.write_text('');report={'boundary':'local synthetic assets on independent QEMU -snapshot; no account or user VM access','snapshot':str(args.snapshot.resolve()),'cases':[],'ready':False}
 for name in ('logit.iso','disk.img'):
  h=hashlib.sha256()
  with (args.snapshot/name).open('rb') as f:
   for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
  report[name+'_sha256']=h.hexdigest()
 try:
  with tempfile.TemporaryDirectory(prefix='svg-scene-qmp-',dir='/tmp') as tmp:
   socket=str(Path(tmp)/'qmp.sock')
   cmd=['qemu-system-x86_64','-cpu','max','-cdrom',str(args.snapshot/'logit.iso'),'-drive','file='+str(args.snapshot/'disk.img')+',format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-snapshot','-boot','d','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=900','-display','none','-no-reboot','-netdev','user,id=n0','-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+socket+',server,nowait']
   with (out/'qemu.log').open('w') as log:
    proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
    def wait(marker):
     end=time.monotonic()+180
     while time.monotonic()<end:
      if marker in serial.read_text(errors='replace'):return
      if proc.poll() is not None:raise RuntimeError('guest exited')
      time.sleep(.2)
     raise RuntimeError('guest marker absent: '+marker)
    try:
     wait('desktop live');ui=Session(socket,serial=str(serial));ui.launch_app('browser');time.sleep(2)
     ui.key_mods(('ctrl',),'t',settle=.3);ui.typ('http://10.0.2.2:%d/'%server.server_port);ui.key('ret');wait('SVG-SCENE READY');time.sleep(2)
     ui.screendump(str(out/'page.ppm'));report['ready']=True
    finally:
     proc.terminate()
     try:proc.wait(timeout=15)
     except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
  im=Image.open(out/'page.ppm').convert('RGB');im.save(out/'page.png');px=im.load()
  anchors=[(x,y) for y in range(im.height) for x in range(im.width) if px[x,y]==(17,231,197)]
  if not anchors:raise RuntimeError('page anchor missing')
  ax=min(x for x,y in anchors);ay=min(y for x,y in anchors);report['anchor']=[ax,ay]
  for i,(name,inner,expected) in enumerate(CASES):
   x=ax+(i%4)*100;y=ay+24+(i//4)*80
   got=[{'sample':[sx,sy],'expected':list(rgb),'actual':list(px[x+sx,y+sy])} for sx,sy,rgb in expected]
   report['cases'].append({'name':name,'pass':all(v['actual']==v['expected'] for v in got),'samples':got})
  report['passed']=sum(c['pass'] for c in report['cases']);report['failed']=len(CASES)-report['passed']
  if args.expect_old:report['gate_pass']=report['cases'][0]['pass'] and report['failed']>=5
  else:report['gate_pass']=report['failed']==0
 except Exception as exc:report['error']=str(exc);report['gate_pass']=False
 finally:
  server.shutdown();(out/'results.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
 return 0 if report.get('gate_pass') else 1
if __name__=='__main__':raise SystemExit(main())
