#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real native attachment clicks, retained realm, Blob export and disk bytes."""
import argparse, hashlib, http.server, json, mmap, os, re, shutil, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path
from qmp_ui import Session, browser_client_point
from owned_process import stop_owned
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/boot'))
import lfs_extract as lfs

def main():
 p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--iso');a=p.parse_args()
 build=Path(a.build).resolve();out=build/'download-keys-guest';out.mkdir(exist_ok=True)
 iso=Path(a.iso).resolve() if a.iso else build/'logit.iso'
 disk=out/'disk.img';shutil.copyfile(build/'disk.img',disk)
 data=bytes(i%251 for i in range(300123));requests=[]
 class Handler(http.server.BaseHTTPRequestHandler):
  def log_message(self,*_):pass
  def do_GET(self):
   requests.append(self.path)
   self.send_response(200)
   if self.path=='/attachment':
    body,mime=data,'application/octet-stream';self.send_header('Content-Disposition',"attachment; filename*=UTF-8'en'%E4%B8%8B%E8%BD%BD.bin");self.send_header('Cache-Control','max-age=600')
   elif self.path=='/plain':body,mime=b'ordinary text saved by the download attribute','text/plain'
   elif self.path in ('/key-vectors.js','/key-checks.js'):body,mime=(ROOT/'tests/fixtures/browser'/self.path[1:]).read_bytes(),'text/javascript'
   elif self.path=='/example':
    body=(ROOT/'examples/browser/signed-report.html').read_bytes()+b'''<script>setTimeout(async function(){try{var r=await createSignedReport('round-trip');var ok=await verifySignedReport(r);r.text+=' changed';var bad=!(await verifySignedReport(r));console.log('REPORT-CHECKS '+JSON.stringify({valid:ok,changed:bad}));var b=document.getElementById('sign').getBoundingClientRect();console.log('REPORT-RECT '+JSON.stringify({x:b.x+b.width/2,y:b.y+b.height/2}))}catch(e){console.log('REPORT-ERROR '+e)}},500)</script>''';mime='text/html'
   else:body,mime=(ROOT/'tests/fixtures/browser/download-keys.html').read_bytes(),'text/html'
   self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
 server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);threading.Thread(target=server.serve_forever,daemon=True).start()
 def sha(path):
  with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
 report={'artifacts':{n:sha(build/n) for n in ('logit.iso','disk.img','browser.aex')},'requests':requests,'boot_iso':str(iso),'boot_iso_sha256':sha(iso)}
 serial=out/'serial.log';serial.write_text('')
 try:
  with tempfile.TemporaryDirectory(prefix='dl-qmp-') as tmp:
   sock=tmp+'/qmp';cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-m','1G','-smp','4','-accel','tcg,thread=multi','-cdrom',str(iso),'-boot','d','-drive','file='+str(disk)+',format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-netdev','user,id=n0','-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+sock+',server,nowait']
   (out/'command.json').write_text(json.dumps(cmd,indent=2))
   with (out/'qemu.log').open('w') as log:
    proc=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
    def wait(pattern,seconds=180):
     end=time.monotonic()+seconds
     while time.monotonic()<end:
      text=serial.read_text(errors='replace');m=re.search(pattern,text)
      if m:return m
      if '[fault] app exception:' in text or 'LOGIT_PANIC' in text:raise RuntimeError('PRODUCT: guest fault')
      if proc.poll() is not None:raise RuntimeError('HARNESS: QEMU exited')
      time.sleep(.2)
     raise RuntimeError('Missing guest result: '+pattern)
    try:
     wait('desktop live');wait(r'\[wm\] win \d+ frame [^\r\n]* Finder');wait(r'\[wm\] ptr \d+ \d+');time.sleep(1);ui=Session(sock,serial=str(serial));ui.screendump(str(out/'desktop.ppm'));ui.launch_app('browser',probe=str(out/'launch.ppm')); wait(r'\[wm\] win \d+ frame [^\r\n]* Browser')
     ui.key_mods(('ctrl',),'l');ui.typ('http://10.0.2.2:%d/'%server.server_port);ui.key('ret')
     m=wait(r'KEY_RESULTS (\{[^\r\n]+\})');report['keys']=json.loads(m[1]);assert report['keys']['checks']==74 and not report['keys']['failed'],report['keys']
     time.sleep(2)
     # Native pointer events exercise browser.c's ordinary link navigation.
     def click(cx,cy):
      x,y=browser_client_point(serial.read_text(errors='replace'),cx,cy);ui.click_at(x,y)
     rects={name:json.loads(wait('DOWNLOAD-RECT '+name+r' (\{[^\r\n]+\})')[1]) for name in ('attachment','export')}
     point=rects['attachment'];click(point['x'],point['y']);wait(r'Downloaded: /download/下载.bin bytes=300123')
     click(point['x'],point['y']);wait(r'Downloaded: /download/下载 \(2\).bin bytes=300123')
     point=rects['export'];click(point['x'],point['y'])
     wait(r'Downloaded: /download/signed-report.json bytes=\d+')
     wait(r'Downloaded: /download/blob.bin bytes=6');wait(r'Downloaded: /download/empty.txt bytes=0');wait(r'Downloaded: /download/suggested.txt bytes=45')
     alive=wait(r'DOWNLOAD-PAGE-ALIVE ticks=(\d+) keys=74 failed=0 historyStable=true');assert int(alive[1])>0
     ui.screendump(str(out/'result.ppm'));
     ui.key_mods(('ctrl',),'l');ui.typ('http://10.0.2.2:%d/example'%server.server_port);ui.key('ret')
     report['example']=json.loads(wait(r'REPORT-CHECKS (\{[^\r\n]+\})')[1]);assert report['example']=={'valid':True,'changed':True}
     point=json.loads(wait(r'REPORT-RECT (\{[^\r\n]+\})')[1]);click(point['x'],point['y']);wait(r'Downloaded: /download/signed-report \(2\).json bytes=\d+');ui.screendump(str(out/'example.ppm'));time.sleep(1)
    finally:stop_owned(proc)
  # Read the real writable guest disk after QEMU exits; no snapshot overlay.
  with disk.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as img:
   geo=lfs.sb(img);entries=dict(lfs.readdir(img,geo,lfs.resolve(img,geo,'/download')))
   def read(name):
    blocks,n=lfs.blocks_of(img,geo,entries[name]);return b''.join(img[b*lfs.BS:(b+1)*lfs.BS] for b in blocks)[:n]
   expected={'下载.bin':data,'下载 (2).bin':data,'blob.bin':bytes([0,1,2,127,128,255]),'empty.txt':b'','suggested.txt':b'ordinary text saved by the download attribute'}
   for name,body in expected.items():assert read(name)==body,name
   signed=json.loads(read('signed-report.json'));assert signed['message']=='A useful signed report from LogitOS'
   # Independent OpenSSL-backed verifier, not the browser verifying itself.
   from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
   import base64
   Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(signed['publicKey']['x']+'==')).verify(bytes(signed['signature']),signed['message'].encode())
   example=json.loads(read('signed-report (2).json'));assert hashlib.sha256(example['text'].encode()).hexdigest()==example['sha256']
   Ed25519PublicKey.from_public_bytes(base64.urlsafe_b64decode(example['publicKey']['x']+'==')).verify(bytes.fromhex(example['signature']),example['text'].encode())
   assert all(not n.startswith('.pending-') for n in entries)
   report['files']={n:len(read(n)) for n in expected};report['signature_independently_verified']=True
  assert requests.count('/attachment')==1,'cache path did not run'
  assert report['artifacts']=={n:sha(build/n) for n in report['artifacts']},'HARNESS: source artifacts changed during guest'
  report['passed']=True;print(json.dumps(report,ensure_ascii=False,indent=2))
 finally:
  server.shutdown();server.server_close();(out/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
