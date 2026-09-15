#!/usr/bin/env python3
"""Real NIC acceptance: DHCP plus two exact 128 KiB transfers across ring wrap.

QEMU emulation proves the programmed interface, not physical-board operation.
The server sees actual GETs and the guest must report each payload's length and
FNV checksum. Logs are retained; counters alone cannot satisfy this check.
"""
import argparse
import hashlib
import http.server
from pathlib import Path
import re
import subprocess
import sys
import threading
import time


def fnv(data):
    n = 2166136261
    for x in data:
        n = ((n ^ x) * 16777619) & 0xffffffff
    return n


# Diagnostics may split separate user write calls; they cannot split one
# decimal number into two pieces and have the parser silently glue it back.
DIAG = r'(?:\[(?:mm|netlock|wm|time|sched|pcache|reclaim|rmap|swap|ip6|tcp|pcnet|e1000e)\] [^\r\n]*\r?\n)*'
RESULT = re.compile(r'^http bytes ' + DIAG + r'([0-9]+)' + DIAG +
                    r' fnv1a ' + DIAG + r'([0-9]+)' + DIAG + r'\r?$', re.M)


def check_transfer(text, data):
    records = RESULT.findall(text)
    digest = hashlib.sha256(data).hexdigest()
    sha = re.findall(r'^sha256 ' + DIAG + r'([0-9a-f]{64})' + DIAG + r'\r?$', text, re.M)
    if records != [(str(len(data)), str(fnv(data)))] or sha != [digest]:
        raise ValueError(f'expected exact {len(data)} bytes / FNV {fnv(data)} / SHA256 {digest}, got {records}, {sha}')


def self_test():
    data = bytes(range(256))
    good = f'http bytes {len(data)} fnv1a {fnv(data)}\nsha256 {hashlib.sha256(data).hexdigest()}\n'
    check_transfer(good, data)
    check_transfer(good.replace('bytes ', 'bytes [mm] diagnostic\n'), data)
    # Each bad specimen reaches the real acceptance function and must fail.
    bad = [good.replace('bytes 256', 'bytes 255'),
           good.replace(str(fnv(data)), str(fnv(data) ^ 1)),
           good.replace(hashlib.sha256(data).hexdigest(), '0' * 64),
           good.replace('256', '2[mm] diagnostic\n56'),
           good.replace('256', '2\n56'),
           good + good,
           good.replace('bytes ', 'bytes [unknown] diagnostic\n')]
    for index, text in enumerate(bad):
        try:
            check_transfer(text, data)
        except ValueError:
            continue
        raise RuntimeError(f'acceptance parser admitted negative specimen {index}')
    print('NIC_RESULT: 2 valid records accepted; 7 corrupt/split/ambiguous records rejected')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iso', required=True, type=Path)
    p.add_argument('--disk', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--device', default='pcnet')
    p.add_argument('--driver', default='pcnet')
    p.add_argument('--qemu', default='qemu-system-x86_64')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    payloads = {f'/probe{i}.bin': bytes((j * (31 + 2 * i) + 7 + i) & 255 for j in range(131072)) for i in range(2)}
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            data = payloads.get(self.path)
            if data is None:
                self.send_error(404)
                return
            requests.append(self.path)
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    log = a.output / 'serial.log'
    err = a.output / 'qemu.err'
    cmd = [a.qemu, '-cpu', 'max', '-cdrom', str(a.iso), '-drive',
           f'file={a.disk},format=raw,if=none,id=hd0,file.locking=off',
           '-device', 'virtio-blk-pci,drive=hd0', '-snapshot', '-boot', 'd',
           '-m', '512M', '-smp', '4', '-accel', 'tcg,thread=multi',
           '-vga', 'none', '-device', 'virtio-gpu-pci', '-netdev', 'user,id=n0',
           '-device', a.device + ',netdev=n0', '-serial', 'stdio',
           '-display', 'none', '-no-reboot']
    try:
        with log.open('wb') as out, err.open('wb') as errors:
            guest = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=out, stderr=errors)
            try:
                deadline = time.monotonic() + 65
                while time.monotonic() < deadline:
                    text = log.read_text(errors='replace')
                    if 'LOGIT_BOOT_OK' in text and '[dhcp] bound 10.0.2.15' in text:
                        break
                    if guest.poll() is not None:
                        raise RuntimeError('QEMU exited during boot')
                    time.sleep(0.1)
                else:
                    raise RuntimeError('no boot marker and DHCP lease within 65 s')
                if not re.search(r'\[net\] NIC bound: (?:[a-z0-9-]+ = )?' + re.escape(a.driver) + r'\b', text):
                    raise RuntimeError('the expected NIC driver was not bound')
                time.sleep(1)
                for i in range(2):
                    before = log.stat().st_size
                    guest.stdin.write(f'net get http://10.0.2.2:{server.server_port}/probe{i}.bin\n'.encode())
                    guest.stdin.flush()
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        text = log.read_bytes()[before:].decode(errors='replace')
                        if 'fnv1a' in text:
                            time.sleep(0.2)
                            text = log.read_bytes()[before:].decode(errors='replace')
                            break
                        if guest.poll() is not None:
                            raise RuntimeError('QEMU exited during HTTP transfer')
                        time.sleep(0.1)
                    data = payloads[f'/probe{i}.bin']
                    # The current ring-3 net client prints its digest only
                    # after HTTP framing/status have succeeded. Its former
                    # kernel '[http] get rc' diagnostic no longer exists.
                    try:
                        check_transfer(text, data)
                    except ValueError as e:
                        raise RuntimeError(f'transfer {i}: {e}') from e
                    if f'/probe{i}.bin' not in requests:
                        raise RuntimeError(f'transfer {i}: host did not observe the actual GET')
                    print(f'PASS[{a.driver}]: transfer={i} bytes={len(data)} fnv1a={fnv(data)}', flush=True)
                time.sleep(1.2)  # let the rate-limited completion report run
                text = log.read_text(errors='replace')
                if f'[{a.driver}] link: UP' not in text:
                    raise RuntimeError('no device-derived link-up report')
                route = re.search(r'\[net\] IRQ route: [a-z0-9]+ = ' + re.escape(a.driver) +
                                  r' vector ([0-9]+) via device model', text)
                irqs = re.findall(r'\[net\] rx path: frames [0-9]+ irq ([0-9]+)', text)
                if (route is None or not 96 <= int(route[1]) < 128 or
                        not irqs or max(map(int, irqs)) == 0):
                    raise RuntimeError('no dynamic device-model vector with actual NIC interrupt delivery')
                print(f'PASS[{a.driver}]: dynamic vector={route[1]} NIC IRQ schedules={max(map(int, irqs))}', flush=True)
                print(f'PASS[{a.driver}]: DHCP 10.0.2.15; two exact 128 KiB transfers; log={log}', flush=True)
            finally:
                guest.terminate()
                try:
                    guest.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    guest.kill()
                    guest.wait()
    finally:
        server.shutdown()


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['--self-test']:
            self_test()
        else:
            main()
    except (RuntimeError, OSError) as e:
        raise SystemExit('FAIL NIC guest: ' + str(e))
