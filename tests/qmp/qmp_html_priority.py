#!/usr/bin/env python3
"""Exercise an ordinary HTML page plus Worker through the rebuilt guest.

No host JS engine executes the acceptance assertions. Completion must reach
the real scanout, not just console output. Fault images run the same page and
must fail their own public assertions while the other groups remain healthy.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import parse_qsl, urlsplit

from qmp_ui import PPM, Session

FIXTURES = Path(__file__).resolve().parents[1] / 'fixtures' / 'html-priority'
CLONE_FAILURES = {
    'transfer detaches source ArrayBuffer',
    'transfer invalidates old typed array',
    'detached ArrayBuffer cannot be cloned or transferred again',
    'detached transfer validation follows graph getter serialization',
    'transfer captures bytes after graph getters finish',
    'commit revalidates all buffers after reentrant detachment',
    'transfer detaches listed buffer even when absent from the graph',
    'zero-length transferred ArrayBuffer is still detached',
}

# Phase boundaries are public assertions, not an API-presence probe. Keeping
# their counts here makes deleting a whole fixture (notably the 79 Streams
# checks in each realm) fail instead of still clearing a permissive threshold.
PAGE_GROUPS = (
    ('clone', 'clone creates independent objects', 57),
    ('forms', 'ValidityState has its interface and SameObject identity', 69),
    ('entries', 'FormData takes current native value', 28),
    ('streams', 'Writable desiredSize counts queued and in-flight strategy sizes', 79),
    ('timers', 'setTimeout rejects a Symbol timeout', 39),
    ('ui', 'guest required click fires a trusted non-bubbling cancelable invalid', 11),
)
WORKER_GROUPS = (PAGE_GROUPS[0], PAGE_GROUPS[3], ('timers', PAGE_GROUPS[4][1], 38))
TIMER_FAILURES = {
    'interval re-initialization increments nesting and applies the 4 ms minimum',
    "nested timeouts apply the minimum when the running task's nesting exceeds five",
    'timer callback checkpoint resets timer nesting for its microtask',
}


def check_groups(log, prefix, groups, negative):
    rows = []
    for line in log.splitlines():
        if line.startswith(prefix + 'PASS '):
            rows.append((True, line[len(prefix + 'PASS '):]))
        elif line.startswith(prefix + 'FAIL '):
            rows.append((False, line[len(prefix + 'FAIL '):]))
    labels = [label for _passed, label in rows]
    boundaries = []
    for name, first, _count in groups:
        # The broken validator omits invalid events; the UI deliberately
        # records the unexpected submit so the outer driver can still advance.
        if negative == 'forms' and name == 'ui':
            first = 'guest empty required click blocks submit'
        assert labels.count(first) == 1, (prefix, name, 'missing or repeated phase boundary')
        boundaries.append(labels.index(first))
    assert boundaries[0] == 0 and boundaries == sorted(boundaries), (prefix, boundaries)
    boundaries.append(len(rows))
    coverage = {}
    allowed = {'clone': {'clone'}, 'forms': {'forms', 'ui'}, 'timers': {'timers'}}.get(negative, set())
    for index, (name, _first, count) in enumerate(groups):
        observed = rows[boundaries[index]:boundaries[index + 1]]
        if negative == 'forms':
            # Three scripted invalid callbacks and one native invalid callback
            # disappear when validation is removed. All other checks must run.
            count -= {'forms': 3, 'ui': 1}.get(name, 0)
        failures = [label for passed, label in observed if not passed]
        assert len(observed) == count, (prefix, name, len(observed), count)
        assert name in allowed or not failures, (prefix, name, failures)
        coverage[name] = {'checks': len(observed), 'fail': len(failures)}
    return coverage


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('iso')
    parser.add_argument('disk')
    parser.add_argument('--out', required=True)
    parser.add_argument('--negative', choices=('clone', 'forms', 'timers', 'navigation'))
    parser.add_argument('--diagnose', action='store_true')
    parser.add_argument('--with-navigation', action='store_true')
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    requests = []
    navigation_entries = []
    reports = []

    class Fixture(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            if self.path != '/report':
                self.send_error(404)
                return
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length < 1024 * 1024:
                self.send_error(400)
                return
            reports.append(json.loads(self.rfile.read(length)))
            self.send_response(200)
            self.send_header('Content-Length', '0')
            self.end_headers()

        def do_GET(self):
            requests.append(self.path)
            if urlsplit(self.path).path == '/submitted':
                entries = parse_qsl(urlsplit(self.path).query, keep_blank_values=True)
                navigation_entries.append(entries)
                expected = [('typed', 'guest'), ('duplicate', 'one'), ('duplicate', 'two'),
                            ('notes', 'a\r\nb'), ('edited', 'listener✓'), ('go', 'send'),
                            ('external', 'outside'), ('added', 'tail')]
                ok = entries == expected and not urlsplit(self.path).fragment
                raw = ('<!doctype html><meta charset="utf-8"><title>Native GET result</title>'
                       '<style>body{margin:24px;font:24px sans-serif;}'
                       '#marker{width:340px;padding:24px;background:%s;}</style>'
                       '<h1>Native GET result</h1><div id="marker">%s</div>'
                       '<script>console.log("HTML_NAV_DONE %s");'
                       'console.log("HTML_NAV_HASH "+location.hash)</script>' %
                       ('#15865b' if ok else '#c52a2a',
                        'Native GET passed' if ok else 'Native GET failed',
                        'PASS' if ok else 'FAIL')).encode()
            else:
                leaf = 'index.html' if self.path == '/' else urlsplit(self.path).path.lstrip('/')
                path = FIXTURES / leaf
                if path.parent != FIXTURES or not path.is_file():
                    self.send_error(404)
                    return
                raw = path.read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/javascript' if urlsplit(self.path).path.endswith('.js') else 'text/html')
            self.send_header('Content-Length', str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_args):
            pass

    server = http.server.ThreadingHTTPServer(('0.0.0.0', 0), Fixture)
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

    def wait_for(needle, seconds, after=0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            current = serial()[after:]
            if needle in current:
                return
            # A missing script can finish the page before the input phase.
            # Waiting the full UI deadline then hides its useful failure behind
            # "READY never arrived"; keep the original assertion in the error.
            if needle.startswith('HTML_UI_') and 'HTML_PRIORITY pass=' in current:
                failures = [line for line in current.splitlines()
                            if line.startswith('HTML-FAIL ')]
                raise AssertionError('page ended before ' + needle + ': ' + repr(failures))
            if proc.poll() is not None:
                raise AssertionError('QEMU exited while waiting for ' + needle)
            time.sleep(0.2)
        raise AssertionError('guest did not print ' + needle)

    try:
        wait_for('LOGIT_BOOT_OK', 240)
        wait_for('desktop live', 90)
        time.sleep(3)  # the startup Finder launch follows desktop-live logging
        ui = Session(str(qmp_path), serial=str(serial_path))
        ui.launch_app('browser')
        time.sleep(4)
        ui.typ('http://10.0.2.2:%d/' % server.server_port)
        ui.key('ret')
        wait_for('HTML_UI_READY', 160)

        def click_control(color):
            # Locate the fixture's painted anchor, not a stale dock/window
            # coordinate. The actual control begins 20 px to its right.
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                shot=out/'input.ppm'
                ui.screendump(str(shot),settle=0.2)
                box=PPM(str(shot)).find_color(color)
                if box:
                    left,top,right,bottom=box
                    ui.click_at((left+right)//2+60,(top+bottom)//2)
                    return
                time.sleep(0.1)
            raise AssertionError('native control anchor was not painted: '+str(color))

        click_control((174,83,197))
        wait_for('HTML_UI_FIRST_DONE',20)
        click_control((12,117,183))
        ui.typ('guest')
        ui.key('ret')
        wait_for('HTML_UI_ENTER_DONE',20)
        click_control((174,83,197))
        wait_for('HTML_UI_BUTTON_DONE',20)
        wait_for('HTML_PRIORITY pass=', 160)
        assert len(reports) == 1, 'one complete guest report must arrive over the actual network'
        packet = reports[0]
        assert packet.get('kind') == 'html-priority', packet
        passed, failed = packet['pass'], packet['fail']
        # Serial lines are useful diagnostics, not atomic records: a kernel
        # printf inserted into 22 form assertions made a real 457/0 run look
        # like 47/69 form coverage. Preserve raw serial and validate the full
        # guest records, including the same Worker results, independently.
        lines = []
        for record in packet['records']:
            if record.startswith(('PASS ', 'FAIL ')):
                lines.append('HTML-' + record)
            elif record.startswith(('WORKER PASS ', 'WORKER FAIL ')):
                lines.append('HTML-WORKER ' + record.removeprefix('WORKER '))
        log = '\n'.join(lines)
        (out / 'report.json').write_text(json.dumps(packet, indent=2) + '\n')
        failures = [line.removeprefix('HTML-FAIL ') for line in log.splitlines() if line.startswith('HTML-FAIL ')]
        worker_failures = [line.removeprefix('HTML-WORKER FAIL ') for line in log.splitlines() if line.startswith('HTML-WORKER FAIL ')]
        assert failed == len(failures) + len(worker_failures), 'summary must count observed assertion failures'
        assert '/worker.js' in requests, 'dedicated Worker was not loaded'
        coverage = {'window': check_groups(log, 'HTML-', PAGE_GROUPS, args.negative),
                    'worker': check_groups(log, 'HTML-WORKER ', WORKER_GROUPS, args.negative)}
        observed = sum(group['checks'] for realm in coverage.values() for group in realm.values())
        assert passed + failed == observed, 'console summary must match all observed public checks'
        if not args.diagnose:
            if args.negative == 'clone':
                assert set(failures) == CLONE_FAILURES and set(worker_failures) == CLONE_FAILURES, (failures, worker_failures)
                assert failed == 16, 'both realms must observe every detachment control'
            elif args.negative == 'forms':
                assert 'required empty input is a candidate with valueMissing' in failures, failures
                assert 'invalid requestSubmit blocks submit before navigation' in failures, failures
                assert not worker_failures, worker_failures
                assert failed == 35, 'validation control must fail its full scripted/native boundary'
            elif args.negative == 'timers':
                page_failures = TIMER_FAILURES | {'DOM checkpoint entered inside a timer resets timer nesting for its microtask'}
                assert set(failures) == page_failures, failures
                assert set(worker_failures) == TIMER_FAILURES, worker_failures
                assert failed == 7, 'both task loops must detect all missing-clamp consequences'
            else:
                assert failed == 0, (failures, worker_failures)
        shot = out / 'screen.ppm'
        color = (197, 42, 42) if failed else (35, 161, 91)
        deadline = time.monotonic() + 25
        while True:
            ui.screendump(str(shot), settle=0.2)
            pixels = PPM(str(shot))
            marker = pixels.find_color(color)
            if marker:
                left, top, right, bottom = marker
                # The count is immediately below the marker. An empty marker
                # contains no text; inspect its adjacent rendered result too.
                count_box = (left, bottom, right, min(bottom + 35, pixels.h))
                if (right-left)*(bottom-top) > 5000 and pixels.dark_pixels(count_box) > 90:
                    break
            if proc.poll() is not None or time.monotonic() >= deadline:
                raise AssertionError('completion marker and result text did not reach scanout')
        result = {'pass': passed, 'fail': failed, 'failures': failures,
                  'worker_failures': worker_failures, 'requests': requests,
                  'marker': marker, 'negative': args.negative,
                  'diagnostic_run': args.diagnose, 'coverage': coverage}
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('html-priority guest: %d passed, %d failures; actual result frame captured' % (passed, failed), flush=True)
        if args.with_navigation:
            assert args.negative in (None, 'navigation') and not args.diagnose, 'navigation uses its own producer control'

            def open_submission(size):
                # A second document has the same console labels. Observe only
                # logs after this navigation, never reuse the previous READY.
                start = len(serial())
                navigation_entries.clear()
                ui.goto(500, 173)
                ui.click()
                ui.key_mods(['ctrl'], 'a')
                ui.typ('http://10.0.2.2:%d/submit.html?size=%d' % (server.server_port, size))
                ui.key('ret')
                wait_for('HTML_NAV_READY', 60, start)
                assert 'HTML_NAV_NAMESPACE checks=20 fail=0' in serial()[start:], 'getter-hidden namespace operations must not be constructors'
                click_control((174,83,197))
                wait_for('HTML_NAV_INVALID', 20, start)
                assert not navigation_entries, 'invalid native click must not reach the server'
                click_control((12,117,183))
                ui.typ('guest')
                ui.key('ret')
                return start

            if args.negative == 'navigation':
                controls = []
                for size in (800, 400):
                    start = open_submission(size)
                    wait_for('HTML_NAV_DONE FAIL', 60, start)
                    observed_log = serial()[start:]
                    assert observed_log.count('HTML_NAV_SUBMIT trusted=true submitter=send') == 1
                    assert observed_log.count('HTML_NAV_ENTRIES') == 1
                    assert len(navigation_entries) == 1, 'old producer must send the truncated request'
                    truncated = dict(navigation_entries[0]).get('oversize', '')
                    assert 0 < len(truncated) < size, 'control must lose actual bytes on the wire'
                    path = out / ('navigation-%d.ppm' % size)
                    for _attempt in range(50):
                        ui.screendump(str(path), settle=0.2)
                        pixels = PPM(str(path))
                        box = pixels.find_color((197,42,42))
                        if box and pixels.dark_pixels(box) > 90:
                            break
                    else:
                        raise AssertionError('truncated native GET did not paint its failing result')
                    controls.append({'size': size, 'sent': len(truncated), 'entries': navigation_entries[0], 'marker': box})
                (out / 'navigation.json').write_text(json.dumps({'negative_control': True, 'controls': controls}, indent=2) + '\n')
                print('native GET negative control: both URL capacities truncated real values on the wire', flush=True)
                return
            start = open_submission(800)
            for size in (800, 400):
                wait_for('HTML_NAV_LARGE_STAYED size=%d' % size, 20, start)
                assert not navigation_entries, 'oversized native GET must not reach the server with partial entries'
                ui.screendump(str(out / ('refusal-%d.ppm' % size)), settle=0.2)
                click_control((174,83,197))
            wait_for('HTML_NAV_DONE ', 60, start)
            final_log = serial()[start:]
            assert 'HTML_NAV_DONE PASS' in final_log, navigation_entries
            assert 'HTML_NAV_HASH #complete' in final_log, 'GET must retain the action fragment without sending it on the wire'
            assert final_log.count('HTML_NAV_SUBMIT trusted=true submitter=send') == 3, 'all real attempts retain their trusted submitter'
            assert final_log.count('HTML_NAV_ENTRIES') == 3 and len(navigation_entries) == 1, 'only the complete edited entry list may reach the server'
            nav_shot = out / 'navigation.ppm'
            for _attempt in range(50):
                ui.screendump(str(nav_shot), settle=0.2)
                nav_pixels = PPM(str(nav_shot))
                nav_marker = nav_pixels.find_color((21,134,91))
                if nav_marker and nav_pixels.dark_pixels(nav_marker) > 90:
                    break
            else:
                raise AssertionError('native GET result did not reach scanout')
            (out / 'navigation.json').write_text(json.dumps({'entries': navigation_entries[0], 'marker': nav_marker}, indent=2) + '\n')
            print('native GET: invalid and oversized submissions blocked; trusted button and edited entry list reached server and scanout', flush=True)
    finally:
        (out / 'reports.json').write_text(json.dumps(reports, indent=2) + '\n')
        (out / 'requests.json').write_text(json.dumps(requests) + '\n')
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
