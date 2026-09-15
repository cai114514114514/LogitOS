#!/usr/bin/env python3
"""Actual pixels and native input for stacking, on a caller-owned private disk.

The negative runs the SAME page with only the layout sort restored to flat-z.
Paint journals alone are insufficient: the old renderer issues text then paints
an opaque background over it. These assertions inspect the final framebuffer.
"""
import argparse
import hashlib
import http.server
import json
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from qmp_ui import Session, browser_client_point

PAGE = b"""<!doctype html><meta charset=utf-8><title>Stacking acceptance</title>
<style>
body{margin:0;background:#dddddd;color:black;font:20px sans-serif;line-height:30px}
.label{position:absolute;top:10px;left:20px}.label2{left:280px}
.panel{position:absolute;top:60px;width:220px;height:100px;background:white;z-index:8}
#bad{left:20px}#badchild{position:relative;z-index:1}#good{left:280px}#goodchild{position:relative}
#outer{position:absolute;top:200px;left:20px;z-index:1;width:220px;height:70px}
#trapped{position:absolute;inset:0;background:#d33245;z-index:999999}
#front{position:absolute;top:200px;left:20px;width:220px;height:70px;background:#14ac87;z-index:2}
</style>
<div class=label>Nested z 8 / z 1</div><div class="label label2">Control: child auto</div>
<div class=panel id=bad><div id=badchild>VISIBLE TEXT</div></div>
<div class=panel id=good><div id=goodchild>VISIBLE TEXT</div></div>
<div id=outer><div id=trapped>TRAPPED CHILD</div></div><div id=front>FRONT CONTEXT</div>
<script>
['bad','badchild','good','goodchild','trapped','front'].forEach(function(id){
document.getElementById(id).addEventListener('click',function(e){e.stopPropagation();console.log('STACK-CLICK:'+id)})});
setInterval(function(){console.log('STACK-GUEST-READY')},500);
</script>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--disk', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--expect-flat', action='store_true')
    args = ap.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / 'serial.log'
    serial.write_text('')
    (out / 'fixture.html').write_bytes(PAGE)
    result = {'passed': False, 'expected_flat': args.expect_flat,
              'fixture_sha256': hashlib.sha256(PAGE).hexdigest(), 'requests': []}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            result['requests'].append(self.path)
            if self.path != '/stack':
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(PAGE)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(PAGE)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    def log():
        return serial.read_text(errors='replace')

    try:
        with tempfile.TemporaryDirectory(prefix='stack-qmp-', dir='/tmp') as tmp:
            sock = str(Path(tmp) / 'qmp.sock')
            cmd = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
                   '-cdrom', str(Path(args.iso).resolve()), '-drive',
                   'file=' + str(Path(args.disk).resolve()) + ',format=raw,if=none,id=hd0',
                   '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd',
                   '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi', '-vga', 'none',
                   '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none',
                   '-no-reboot', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
                   '-serial', 'file:' + str(serial), '-qmp', 'unix:' + sock + ',server,nowait']
            (out / 'launch.json').write_text(json.dumps(cmd, indent=2))
            with open(out / 'qemu.log', 'w') as qlog:
                proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)

                def wait(marker, start=0, seconds=120):
                    end = time.monotonic() + seconds
                    while time.monotonic() < end:
                        if marker in log()[start:]:
                            return
                        if proc.poll() is not None:
                            raise RuntimeError('owned guest exited')
                        time.sleep(.2)
                    raise RuntimeError('missing guest marker: ' + marker)

                try:
                    wait('desktop live')
                    ui = Session(sock, serial=str(serial))
                    ui.launch_app('browser')
                    time.sleep(2)
                    ui.key_mods(('ctrl',), 't', settle=.3)
                    ui.typ('http://10.0.2.2:%d/stack' % server.server_port)
                    ui.key('ret')
                    wait('STACK-GUEST-READY')
                    time.sleep(2)
                    ui.screendump(str(out / 'page.ppm'))
                    from PIL import Image
                    img = Image.open(out / 'page.ppm').convert('RGB')
                    img.save(out / 'page.png')

                    def crop(x, y, w, h):
                        a = browser_client_point(log(), x, y)
                        b = browser_client_point(log(), x + w, y + h)
                        return img.crop((*a, *b))

                    dark = lambda region: sum(max(p) < 80 for p in region.getdata())
                    bad = dark(crop(20, 60, 215, 30))
                    good = dark(crop(280, 60, 215, 30))
                    covered = crop(30, 240, 180, 20)
                    green = sum(p == (20, 172, 135) for p in covered.getdata())
                    red = sum(p == (211, 50, 69) for p in covered.getdata())
                    result.update(nested_text_dark_pixels=bad, control_text_dark_pixels=good,
                                  nested_front_green_pixels=green, nested_escape_red_pixels=red)
                    assert good > 150, 'ordinary control text missing from final pixels'
                    if args.expect_flat:
                        assert bad == 0 and red > 1000 and green == 0, 'flat control did not visibly regress both scenes'
                    else:
                        assert bad == good and green > 1000 and red == 0, 'context text or ancestor boundary missing in final pixels'
                    targets = [(30, 70, 'bad' if args.expect_flat else 'badchild'),
                               (30, 245, 'trapped' if args.expect_flat else 'front')]
                    result['native_click_targets'] = []
                    for x, y, target in targets:
                        before = len(log())
                        ui.click_at(*browser_client_point(log(), x, y))
                        wait('STACK-CLICK:' + target, start=before, seconds=15)
                        result['native_click_targets'].append(target)
                    assert result['requests'].count('/stack') == 1, 'fixture was unexpectedly reloaded'
                    result['passed'] = True
                    print(json.dumps(result, indent=2))
                finally:
                    proc.terminate()
                    try:
                        proc.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=10)
    finally:
        server.shutdown()
        (out / 'results.json').write_text(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
