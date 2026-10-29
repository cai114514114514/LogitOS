#!/usr/bin/env python3
"""Owned passive iframe fixture: real child text and SVG pixels in QEMU.
The parent retains its document/URL and cannot access child DOM. An optional
public page observation uses ordinary navigation once; it performs no login,
SMS, click inside the document, or script injection into the public page.
"""
import argparse, hashlib, http.server, json, os, re, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from qmp_ui import Session


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--iso',required=True);ap.add_argument('--disk',required=True);ap.add_argument('--out',required=True);ap.add_argument('--expect-no-paint',action='store_true');ap.add_argument('--public-url');args=ap.parse_args()
    out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    serial=out/'serial.log';serial.write_text('');requests=[]
    result={'passed':False,'requests':requests,'expected_control':args.expect_no_paint}
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*a):pass
        def do_GET(self):
            path=self.path.split('?')[0];requests.append(path)
            if path=='/parent':
                body=("<!doctype html><meta charset=utf-8><title>Passive iframe acceptance</title><style>body{margin:24px;background:#eef2f8;font:18px sans-serif}iframe{display:block;width:240px;height:200px;border:3px solid #315591;margin-top:20px}</style><h1>FRAME-PARENT-RETAINED</h1><iframe id=f src='http://10.0.2.2:%d/child'></iframe><p>The colored SVG above belongs to a separate child document.</p><script>setInterval(function(){var isolated=false;try{isolated=document.getElementById('f').contentDocument===null}catch(e){isolated=true}console.log('FRAME-PARENT-CHECK url='+location.pathname+' isolated='+isolated+' childScript='+(typeof childRan==='undefined'))},600)</script>"%child_server.server_port).encode();mime='text/html'
            elif path=='/child':
                body=b"<!doctype html><link rel=stylesheet href='/child.css'><p id=label>FRAME-CHILD-PIXELS</p><img width=64 height=64 src='/image.svg'><script>childRan=true</script><script src='/must-not-run.js'></script><iframe src='/must-not-load'></iframe>";mime='text/html'
            elif path=='/child.css':body=b"body{margin:12px;background:white;color:#102030}#label{font:16px sans-serif;margin:0 0 12px}img{display:block}";mime='text/css'
            elif path=='/image.svg':body=b"<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'><rect width='64' height='64' fill='#14ac87'/><rect x='16' y='16' width='32' height='32' fill='#e825b4'/></svg>";mime='image/svg+xml'
            else:self.send_error(404);return
            self.send_response(200);self.send_header('Content-Type',mime);self.send_header('Content-Length',str(len(body)));self.send_header('Cache-Control','no-store')
            if path=='/child':self.send_header('Content-Security-Policy',"script-src 'none'; style-src http:; img-src http:; frame-ancestors http://10.0.2.2:*")
            self.end_headers();self.wfile.write(body)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);child_server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start();threading.Thread(target=child_server.serve_forever,daemon=True).start()
    def log():return serial.read_text(errors='replace')
    try:
        with tempfile.TemporaryDirectory(prefix='pf-qmp-',dir='/tmp') as tmp:
            sock=str(Path(tmp)/'qmp.sock')
            cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-cdrom',str(Path(args.iso).resolve()),'-drive','file='+str(Path(args.disk).resolve())+',format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-snapshot','-boot','d','-m','1G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-netdev','user,id=n0','-device','e1000,netdev=n0','-serial','file:'+str(serial),'-qmp','unix:'+sock+',server,nowait']
            (out/'launch.json').write_text(json.dumps(cmd,indent=2))
            with open(out/'qemu.log','w') as qlog:
                proc=subprocess.Popen(cmd,stdout=qlog,stderr=subprocess.STDOUT)
                def wait(marker,seconds=120,start=0):
                    end=time.monotonic()+seconds
                    while time.monotonic()<end:
                        if marker in log()[start:]:return
                        if proc.poll() is not None:raise RuntimeError('owned guest exited')
                        time.sleep(.2)
                    raise RuntimeError('missing guest marker: '+marker)
                try:
                    wait('desktop live');ui=Session(sock,serial=str(serial));ui.launch_app('browser');time.sleep(2)
                    ui.key_mods(('ctrl',),'t',settle=.3);ui.typ('http://10.0.2.2:%d/parent'%server.server_port);ui.key('ret')
                    wait('[iframe] passive document ready:');wait('FRAME-PARENT-CHECK url=/parent isolated=true childScript=true');time.sleep(2)
                    ui.screendump(str(out/'embedded.ppm'))
                    text=log();assert '/must-not-run.js' not in requests and '/must-not-load' not in requests,'child executable context started'
                    assert all(p in requests for p in ['/parent','/child','/child.css','/image.svg']),'required resources missing'
                    painted=bool(re.search(r'^\[dl\] \d+,\d+ FRAME-CHILD-PIXELS\r?$',text,re.M))
                    from PIL import Image
                    img=Image.open(out/'embedded.ppm').convert('RGB');img.save(out/'embedded.png')
                    green=sum(1 for p in img.getdata() if p==(20,172,135));pink=sum(1 for p in img.getdata() if p==(232,37,180))
                    notice='嵌入页面部分脚本不可用' in text
                    result.update(child_text_painted=painted,svg_green_pixels=green,svg_pink_pixels=pink,preview_notice=notice)
                    if args.expect_no_paint:assert not painted and green==0 and pink==0,'no-paint negative control did not fail visibly'
                    else:assert painted and green>=2500 and pink>=900 and notice,'real child text/image or native capability notice missing from guest pixels'
                    result['passed']=True
                    if args.public_url:
                        before=len(log());ui.key_mods(('ctrl',),'t',settle=.3);ui.typ(args.public_url);ui.key('ret')
                        # This is an observation, not a fake pass condition for
                        # the public application. Capture after ordinary load.
                        end=time.monotonic()+100
                        while time.monotonic()<end:
                            tail=log()[before:]
                            if '[iframe] passive document ready:' in tail or '[iframe] passive refused:' in tail:break
                            if proc.poll() is not None:raise RuntimeError('guest exited during public observation')
                            time.sleep(.5)
                        time.sleep(3);ui.screendump(str(out/'public.ppm'));Image.open(out/'public.ppm').save(out/'public.png')
                        result['public_frame_status']=re.findall(r'^\[iframe\] passive [^\r\n]+',log()[before:],re.M)
                    print(json.dumps(result,indent=2))
                finally:
                    proc.terminate()
                    try:proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=10)
    finally:
        server.shutdown();child_server.shutdown();(out/'results.json').write_text(json.dumps(result,indent=2))
if __name__=='__main__':main()
