#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local JS -> DOM/CSS -> automatic SVG repaint on an independent frozen VM.

Identical fixture bytes run against current and old snapshots. An ordinary
mouse click mutates the scene after the first screenshot; neither phase reads
layout geometry. The static SVG and page anchor keep a failed load from being
mistaken for a useful negative control. Never attaches to another VM.
"""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/qmp"))
from qmp_ui import Session, configure
from PIL import Image

WHITE = (255, 255, 255)
GREEN = (0, 255, 0)
RED = (255, 0, 0)
BLUE = (0, 0, 255)
ANCHOR = (17, 231, 197)
CASES = [
    ("static-control", [(4, 4, GREEN), (28, 28, GREEN)]),
    ("createElementNS", [(4, 4, GREEN), (28, 28, GREEN)]),
    ("mutated-fill", [(4, 4, GREEN), (28, 28, GREEN)]),
    ("CSS-fill-cascade", [(4, 4, GREEN), (28, 28, GREEN)]),
    ("CSS-currentColor", [(4, 4, GREEN), (28, 28, GREEN)]),
    ("viewBox-case-update", [(4, 4, BLUE), (28, 28, BLUE)]),
    ("button-24-padding-zero", [(2, 2, GREEN), (21, 21, GREEN)]),
    ("button-12-padding-zero", [(2, 2, GREEN), (9, 9, GREEN)]),
]


def svg(inner, size=32, attrs=""):
    return '<svg width="%d" height="%d" viewBox="0 0 %d %d" %s>%s</svg>' % (size, size, size, size, attrs, inner)


def fixture():
    parts = ["""<!doctype html><meta charset="utf-8"><style>
html,body{margin:0;background:white}svg{display:block}p{margin:0;font:12px sans-serif}
#rule .shape{fill:red}#rule.changed .shape{fill:#00ff00}
.small{display:block;margin:0;padding:0;border:0;background:white}
</style><div style="position:absolute;left:20px;top:20px;width:4px;height:4px;background:#11e7c5"></div>"""]
    rect = '<rect width="32" height="32" fill="#00ff00"/>'
    scenes = [
        svg(rect),
        '<div id="created"></div>',
        svg('<rect id="mutated" width="32" height="32" fill="red"/>'),
        svg('<rect class="shape" width="32" height="32" fill="red"/>', attrs='id="rule"'),
        svg('<rect width="32" height="32" fill="currentColor"/>', attrs='id="current" style="color:red"'),
        '<svg id="view" width="32" height="32" viewBox="0 0 64 32"><rect x="32" width="32" height="32" fill="blue"/></svg>',
        '<button class="small" style="width:24px;height:24px">' + svg('<rect width="24" height="24" fill="#00ff00"/>', 24) + '</button>',
        '<button class="small" style="width:12px;height:12px">' + svg('<rect width="12" height="12" fill="#00ff00"/>', 12) + '</button>',
    ]
    for i, ((name, _), scene) in enumerate(zip(CASES, scenes)):
        x, y = 20 + (i % 4) * 180, 52 + (i // 4) * 110
        parts.append('<div style="position:absolute;left:%dpx;top:%dpx;width:160px;height:80px">%s<p>%s</p></div>' % (x, y, scene, name))
    parts.append("""<button id="apply" style="position:absolute;left:20px;top:280px;width:180px;height:40px">APPLY CHANGES</button>
<script>
var ns='http://www.w3.org/2000/svg';
var icon=document.createElementNS(ns,'svg');
icon.setAttribute('width','32');icon.setAttribute('height','32');icon.setAttribute('viewBox','0 0 32 32');
var shape=document.createElementNS(ns,'rect');shape.setAttribute('width','32');shape.setAttribute('height','32');shape.setAttribute('fill','#00ff00');
icon.appendChild(shape);document.getElementById('created').appendChild(icon);
document.getElementById('apply').addEventListener('click',function(){
 document.getElementById('mutated').setAttribute('fill','#00ff00');
 document.getElementById('rule').setAttribute('class','changed');
 document.getElementById('current').style.color='#00ff00';
 var view=document.getElementById('view');var count=view.getAttributeNames().length;
 view.setAttribute('viewBox','32 0 32 32');
 console.log('SVG-DOM CASE '+(view.getAttribute('viewBox')==='32 0 32 32'&&!view.hasAttribute('viewbox')&&view.getAttributeNames().length===count?'PASS':'FAIL'));
 console.log('SVG-DOM APPLIED');
});
setTimeout(function(){console.log('SVG-DOM READY')},1200);
</script>""")
    return "".join(parts).encode()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def inspect(path):
    im = Image.open(path).convert('RGB')
    im.save(path.with_suffix('.png'))
    px = im.load()
    anchors = [(x, y) for y in range(im.height) for x in range(im.width) if px[x, y] == ANCHOR]
    if len(anchors) != 16:
        raise RuntimeError('expected exactly 16 page anchor pixels, got %d' % len(anchors))
    ax, ay = min(x for x, y in anchors), min(y for x, y in anchors)
    samples = []
    for i, (name, expected) in enumerate(CASES):
        x, y = ax + (i % 4) * 180, ay + 32 + (i // 4) * 110
        got = [{'sample': [sx, sy], 'expected': list(rgb), 'actual': list(px[x + sx, y + sy])} for sx, sy, rgb in expected]
        samples.append({'name': name, 'pass': all(v['actual'] == v['expected'] for v in got), 'samples': got})
    return [ax, ay], samples


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
    (out / 'fixture.html').write_bytes(html)

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_GET(self):
            if self.path != '/':
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(html)))
            self.end_headers()
            self.wfile.write(html)

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial = out / 'serial.log'
    serial.write_text('')
    before_hashes = {name: digest(snapshot / name) for name in ('logit.iso', 'disk.img')}
    report = {'boundary': 'independent local-only frozen QEMU -snapshot; ordinary mouse click; no geometry reads or user VM access',
              'snapshot': str(snapshot), 'inputs_sha256': before_hashes,
              'fixture_sha256': hashlib.sha256(html).hexdigest(), 'expected_old': args.expect_old}
    try:
        with tempfile.TemporaryDirectory(prefix='svg-dom-qmp-', dir='/tmp') as tmp:
            socket = str(Path(tmp) / 'qmp.sock')
            cmd = ['qemu-system-x86_64', '-cpu', 'max', '-cdrom', str(snapshot / 'logit.iso'),
                   '-drive', 'file=' + str(snapshot / 'disk.img') + ',format=raw,if=none,id=hd0',
                   '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd', '-m', '1G', '-smp', '4',
                   '-accel', 'tcg,thread=multi', '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=900',
                   # restrict also blocks the usual 10.0.2.2 host route. One
                   # explicit guestfwd leaves only this local fixture reachable.
                   '-display', 'none', '-no-reboot', '-netdev',
                   'user,id=n0,restrict=on,guestfwd=tcp:10.0.2.100:18080-tcp:127.0.0.1:%d' % server.server_port,
                   '-device', 'e1000,netdev=n0',
                   '-serial', 'file:' + str(serial), '-qmp', 'unix:' + socket + ',server,nowait']
            with (out / 'qemu.log').open('w') as log:
                proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
                report['pid'] = proc.pid
                def wait(marker):
                    end = time.monotonic() + 180
                    while time.monotonic() < end:
                        if marker in serial.read_text(errors='replace'): return
                        if proc.poll() is not None: raise RuntimeError('guest exited')
                        time.sleep(.2)
                    raise RuntimeError('guest marker absent: ' + marker)
                try:
                    wait('desktop live')
                    ui = Session(socket, serial=str(serial))
                    ui.launch_app('browser')
                    time.sleep(2)
                    ui.key_mods(('ctrl',), 't', settle=.3)
                    ui.typ('http://10.0.2.100:18080/')
                    ui.key('ret')
                    wait('SVG-DOM READY')
                    time.sleep(2)
                    ui.screendump(str(out / 'before.ppm'))
                    anchor, initial = inspect(out / 'before.ppm')
                    report['anchor'] = anchor
                    report['initial_cases'] = initial
                    report['initial_mutation_is_red'] = initial[2]['samples'][0]['actual'] == list(RED)
                    target = (anchor[0] + 80, anchor[1] + 280)
                    ui.click_at_confirmed(str(out / 'pointer.ppm'), *target)
                    report['click'] = list(target)
                    wait('SVG-DOM APPLIED')
                    time.sleep(2)
                    ui.goto(10, 10)
                    ui.screendump(str(out / 'after.ppm'))
                finally:
                    proc.terminate()
                    try: proc.wait(timeout=15)
                    except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=10)
                    report['guest_stopped'] = proc.poll() is not None
            final_anchor, samples = inspect(out / 'after.ppm')
            report['final_anchor'] = final_anchor
            report['cases'] = samples
            report['passed'] = sum(c['pass'] for c in samples)
            report['failed'] = len(CASES) - report['passed']
            report['dom_case_pass'] = 'SVG-DOM CASE PASS' in serial.read_text(errors='replace')
            report['input_files_unchanged'] = before_hashes == {name: digest(snapshot / name) for name in before_hashes}
            guard = initial[0]['pass'] and samples[0]['pass'] and report['initial_mutation_is_red'] and anchor == final_anchor and report['input_files_unchanged']
            if args.expect_old:
                report['gate_pass'] = guard and not samples[1]['pass'] and not samples[2]['pass']
            else:
                report['gate_pass'] = guard and report['failed'] == 0 and report['dom_case_pass']
    except Exception as exc:
        report['error'] = str(exc)
        report['gate_pass'] = False
    finally:
        server.shutdown()
        (out / 'results.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report, indent=2))
    return 0 if report.get('gate_pass') else 1


if __name__ == '__main__':
    raise SystemExit(main())
