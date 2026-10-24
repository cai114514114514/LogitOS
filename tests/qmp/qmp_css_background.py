#!/usr/bin/env python3
"""Read the same CSS background oracle as the host gate from real scanout.

The guest parses/cascades/layouts/paints the ordinary HTML fixture. Its actual
DOM rectangle arrives over HTTP; color assertions sample the QEMU screendump
at client points mapped by qmp_ui's real Browser frame/content geometry.
Host time is used only for bounded readiness, never a performance result.
"""
import argparse
import csv
import http.server
import json
import os
import hashlib
from pathlib import Path
import subprocess
import threading
import time
import tempfile
from urllib.parse import urlsplit

from qmp_ui import PPM, Session, browser_client_point, configure
from qmp_site import write_png

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'engine-expansion'


def read_oracle(case='background'):
    boxes, samples = [], []
    with (FIXTURES / ('css-%s-pixels.csv' % case)).open(newline='') as stream:
        for row in csv.reader(stream):
            if row[0] == 'box' and len(row) == 7:
                boxes.append({'id': row[1], 'expected': list(map(int, row[2:6])), 'label': row[6]})
            elif row[0] == 'pixel' and len(row) in (5, 6):
                value = int(row[3], 16)
                samples.append({'point': list(map(int, row[1:3])),
                                'expected': [(value >> shift) & 255 for shift in (16, 8, 0)],
                                'label': row[4],
                                'controls': row[5].split(';') if len(row) == 6 else []})
            else:
                raise ValueError('malformed CSS background oracle row: ' + repr(row))
    labels = [item['label'] for item in boxes + samples]
    expected={'background':26,'gradients':49,'layers':53}[case]
    assert len(labels) == expected and len(set(labels)) == expected, 'all independent assertions must remain present'
    return boxes, samples


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('iso')
    parser.add_argument('disk')
    parser.add_argument('--out', required=True)
    parser.add_argument('--case', choices=('background','gradients','layers'),default='background')
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--negative', choices=('old-box', 'no-base','no-rclip'))
    args = parser.parse_args()
    if args.negative and (args.case!='background' or args.baseline):parser.error('legacy fault tags belong to the background oracle')
    boxes, samples = read_oracle(args.case)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    requests, reports = [], []
    identifiers = json.dumps([box['id'] for box in boxes])
    # This report reads actual guest geometry. No host JS engine, eval hook or
    # alternate style/layout implementation supplies the observed rectangle.
    script = """<script>
addEventListener('load',function(){
    var boxes={};
    %s.forEach(function(id){
        var box=document.getElementById(id).getBoundingClientRect();
        boxes[id]=[box.left,box.top,box.width,box.height];
    });
    fetch('/report',{method:'POST',body:JSON.stringify({kind:'css-background',boxes:boxes})});
});
</script>""" % identifiers
    if args.case=='layers':
        script=script.replace("kind:'css-background',boxes:boxes",
            "kind:'css-background',boxes:boxes,backgroundColor:getComputedStyle(document.getElementById('shorthand')).backgroundColor")
    entry='/css-%s.html' % args.case
    original=(FIXTURES / entry[1:]).read_bytes()
    page = original + script.encode()
    sources={entry:hashlib.sha256(original).hexdigest()}
    assets={}
    if args.case=='layers':
        for name in ('css-layers.css','assets/stripes.bmp'):
            assets['/'+name]=(FIXTURES/name).read_bytes()
            sources['/'+name]=hashlib.sha256(assets['/'+name]).hexdigest()

    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            path=urlsplit(self.path).path
            if path==entry:body,mime=page,'text/html; charset=utf-8'
            elif path in assets:body,mime=assets[path],('text/css' if path.endswith('.css') else 'image/bmp')
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type',mime)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != '/report':
                self.send_error(404)
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length < 65536:
                self.send_error(400)
                return
            reports.append(json.loads(self.rfile.read(length)))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    # Darwin sockaddr_un permits fewer than 104 bytes. A descriptive evidence
    # directory plus its unique run ID can exceed that before QEMU even boots.
    # Keep persistent evidence in out and only the transient socket in /tmp.
    socket_dir=tempfile.TemporaryDirectory(prefix='css-qmp-',dir='/tmp')
    serial_path, qmp_path = out / 'serial.log', Path(socket_dir.name) / 'qmp.sock'
    configure(1280, 800)
    proc = subprocess.Popen([
        os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
        '-cdrom', str(Path(args.iso).resolve()),
        '-drive', 'file=%s,format=raw,if=none,id=hd0' % Path(args.disk).resolve(),
        '-device', 'virtio-blk-pci,drive=hd0', '-boot', 'd', '-snapshot',
        '-m', '1G', '-smp', '4', '-accel', 'tcg,thread=multi',
        '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800',
        '-display', 'none', '-no-reboot', '-netdev', 'user,id=n0',
        '-device', 'e1000,netdev=n0', '-serial', 'file:' + str(serial_path),
        '-object', 'filter-dump,id=packets,netdev=n0,file=' + str(out / 'network.pcap'),
        '-qmp', 'unix:%s,server,nowait' % qmp_path,
    ], stdout=subprocess.DEVNULL, stderr=(out / 'qemu.log').open('w'))
    result = {'negative': args.negative,'case':args.case,'baseline':args.baseline,
              'fixture_sha256':sources,'report_script_sha256':hashlib.sha256(script.encode()).hexdigest(),
              'checks': 0, 'passed': 0, 'failures': []}

    def serial():
        return serial_path.read_text(errors='replace') if serial_path.exists() else ''

    def wait_for(predicate, label, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            if proc.poll() is not None:
                raise AssertionError('QEMU exited while waiting for ' + label)
            time.sleep(.2)
        raise AssertionError('guest never reached ' + label)

    try:
        wait_for(lambda: 'LOGIT_BOOT_OK' in serial(), 'boot', 240)
        wait_for(lambda: 'desktop live' in serial(), 'desktop', 90)
        time.sleep(3)
        ui = Session(str(qmp_path), serial=str(serial_path))
        ui.launch_app('browser')
        time.sleep(4)
        load_mark=ui.mark()
        # A preserved user disk can restore a live tab. Typing then targets the
        # page, not the address bar; an empty scratch profile hid that apparatus
        # assumption. Use the browser's actual trusted address focus gesture.
        ui.key_mods(['ctrl'],'l')
        ui.typ('http://10.0.2.2:%d%s' % (server.server_port,entry))
        ui.key('ret')
        wait_for(lambda: bool(reports), 'actual guest geometry HTTP report', 160)
        assert len(reports) == 1 and reports[0].get('kind') == 'css-background', reports
        assert entry in requests, 'the real fixture was not requested'
        if args.case=='layers':assert '/css-layers.css' in requests,'external stylesheet never reached the guest'
        wait_for(lambda:'[load-complete]' in serial()[load_mark:],'actual guest resource drain',160)
        observed = reports[0]['boxes']
        records = []
        for box in boxes:
            actual = observed.get(box['id'])
            passed = isinstance(actual, list) and len(actual) == 4 and all(
                isinstance(a, (int, float)) and abs(a - b) < .01 for a, b in zip(actual, box['expected']))
            records.append({'label': box['label'], 'passed': passed, 'actual': actual, 'expected': box['expected']})
        if args.case=='layers':
            actual=reports[0].get('backgroundColor')
            records.append({'label':'computed background color matches rendered shorthand',
                            'passed':actual=='rgb(255, 0, 0)','actual':actual,'expected':'rgb(255, 0, 0)'})

        # Wait for TWO matching samples, not for expected colors. A broken
        # painter must settle to a stable failing frame instead of timing out
        # merely because the driver kept waiting for a green result.
        previous, stable = None, None
        for attempt in range(30):
            screenshot = out / ('frame-%02d.ppm' % attempt)
            ui.screendump(str(screenshot), settle=.4)
            image = PPM(str(screenshot))
            mapped = [browser_client_point(serial(), *sample['point']) for sample in samples]
            colors = [image.at(x, y) for x, y in mapped]
            if colors == previous:
                stable = (screenshot, mapped, colors)
                break
            previous = colors
        assert stable, 'CSS background pixels never settled'
        screenshot, mapped, colors = stable
        result['screenshot'] = screenshot.name
        write_png(str(out/'screen.png'),image.w,image.h,image.px)
        for sample, screen, actual in zip(samples, mapped, colors):
            passed = all(abs(a - b) <= 2 for a, b in zip(actual, sample['expected']))
            records.append({'label': sample['label'], 'passed': passed, 'client': sample['point'],
                            'screen': list(screen), 'actual': list(actual), 'expected': sample['expected']})
        result.update(checks=len(records), passed=sum(record['passed'] for record in records), records=records,
                      failures=[record['label'] for record in records if not record['passed']])
        expected_failures = {sample['label'] for sample in samples if args.negative in sample['controls']} if args.negative else set()
        assert result['checks']==len(boxes)+len(samples)+(args.case=='layers'),result
        if args.baseline:assert result['failures'],'baseline failed to expose the missing image capability'
        else:assert set(result['failures'])==expected_failures,result
        if args.case=='layers' and not args.baseline:
            assert '/assets/stripes.bmp?layer=front' in requests and '/assets/stripes.bmp?layer=back' in requests,'later image layer did not reach the server'
            assert not any('invalid=tail' in request for request in requests),'invalid shorthand leaked a URL prefix into IO'
        print('css-%s guest: %d passed, %d failures%s; actual scanout sampled' %
              (args.case,result['passed'], len(result['failures']), ' (baseline)' if args.baseline else ' (expected control)' if args.negative else ''), flush=True)
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        (out / 'reports.json').write_text(json.dumps(reports, indent=2) + '\n')
        (out / 'requests.json').write_text(json.dumps(requests, indent=2) + '\n')
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        server.shutdown()
        socket_dir.cleanup()


if __name__ == '__main__':
    main()
