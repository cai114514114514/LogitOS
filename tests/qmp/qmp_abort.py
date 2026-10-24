#!/usr/bin/env python3
"""Run an ordinary page in the shipped guest browser and inspect its pixels.

The delayed routes keep real sockets live at both cancellation boundaries.
The page must observe partial bytes, reject its reader with null, time out a
pending Fetch and run a dedicated Worker. A plain green console is insufficient:
the marker that only successful completion produces must reach the scanout.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading
import time

from qmp_ui import PPM, Session

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'cancellation'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('iso', nargs='?')
    parser.add_argument('disk', nargs='?')
    parser.add_argument('--out', required=True)
    parser.add_argument('--expect-order-failure', action='store_true')
    parser.add_argument('--serve-only', action='store_true')
    args = parser.parse_args()
    if not args.serve_only and (not args.iso or not args.disk):
        parser.error('guest verification requires iso and disk paths')
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    asked = []
    release_delayed = threading.Event()

    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(self.path)
            if self.path in ('/hold', '/stream'):
                if self.path == '/hold':
                    # The response stays pending until the test is over. A
                    # host-clock sleep can end before a contended TCG guest's
                    # timer has advanced, accidentally racing success vs abort.
                    release_delayed.wait()
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain')
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                try:
                    self.wfile.write(b'4\r\nseed\r\n')
                    self.wfile.flush()
                    release_delayed.wait()
                    self.wfile.write(b'4\r\nlate\r\n0\r\n\r\n')
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            path = FIXTURES / ('index.html' if self.path == '/' else self.path.lstrip('/'))
            if path.parent != FIXTURES or not path.is_file():
                self.send_error(404)
                return
            raw = path.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/javascript' if path.suffix == '.js' else 'text/html')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Fixture)
    print('fixture: http://127.0.0.1:%d/' % server.server_port, flush=True)
    if args.serve_only:
        try:
            server.serve_forever()
        finally:
            server.server_close()
        return
    threading.Thread(target=server.serve_forever, daemon=True).start()
    serial_path, qmp_path = out / 'serial.log', out / 'qmp.sock'
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

    def serial():
        return serial_path.read_text(errors='replace') if serial_path.exists() else ''

    def wait_for(needle, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if needle in serial():
                return
            if proc.poll() is not None:
                raise AssertionError('QEMU exited while waiting for ' + needle)
            time.sleep(0.25)
        raise AssertionError('guest did not print ' + needle)

    try:
        wait_for('LOGIT_BOOT_OK', 240)
        wait_for('desktop live', 90)
        # "desktop live" precedes the startup Finder launch. Without settling
        # that task, launch_app can attribute its delayed [wm] launched line
        # to our Browser click and reject the right dock coordinate as wrong.
        # This is an observation delay, never a guest performance measurement.
        time.sleep(3)
        ui = Session(str(qmp_path), serial=str(serial_path))
        ui.launch_app('browser')
        time.sleep(4)
        ui.typ('http://10.0.2.2:%d/' % server.server_port)
        ui.key('ret')
        wait_for('ABORT-DONE ', 120)
        text = serial()
        passed = [line for line in text.splitlines() if line.startswith('ABORT-PASS ')]
        failed = [line for line in text.splitlines() if line.startswith('ABORT-FAIL ')]
        if args.expect_order_failure:
            assert failed == ['ABORT-FAIL dependent states before source event'], failed
            assert len(passed) == 10, passed
        else:
            assert len(passed) == 11 and not failed, (passed, failed)
        assert '/stream' in asked and '/hold' in asked and '/worker.js' in asked, asked
        assert '/worker-hold' not in asked, 'pre-aborted Worker opened a socket'
        shot = out / 'screen.ppm'
        color = (192, 50, 50) if args.expect_order_failure else (19, 141, 89)
        # Console completion precedes the next frame. Session.screendump's
        # "settle" sleeps AFTER capture, so one capture can still show the old
        # yellow marker. Observe successive actual scanouts until the result
        # reaches pixels; never infer a painted frame from a JS log line.
        end = time.monotonic() + 20
        while True:
            ui.screendump(str(shot), settle=0.2)
            image = PPM(str(shot))
            marker_box = image.find_color(color)
            if marker_box is not None:
                left, top, right, bottom = marker_box
                dark_pixels = image.dark_pixels(marker_box)
                if (right - left) * (bottom - top) > 5000 and dark_pixels > 100:
                    break
            if proc.poll() is not None or time.monotonic() >= end:
                raise AssertionError('result marker was not painted')
        result = {'passed': passed, 'failed': failed, 'requests': asked,
                  'marker': color, 'marker_box': marker_box, 'dark_pixels': dark_pixels,
                  'negative_control': args.expect_order_failure}
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('guest cancellation: %d passed, %d expected failures; marker painted' % (len(passed), len(failed)))
    finally:
        release_delayed.set()
        (out / 'requests.json').write_text(json.dumps(asked) + '\n')
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        server.shutdown()


if __name__ == '__main__':
    main()
