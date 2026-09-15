#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the shipped net CLI; optionally verify X448 HTTPS using isolated test trust."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests/qmp'))
from owned_process import stop_owned

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--tls', action='store_true')
p.add_argument('--before-net', type=Path, help='optional earlier net ELF for the 128 KiB comparison')
a = p.parse_args()
build = a.build.resolve()
out = build / ('net-guest-tls' if a.tls else 'net-guest')
out.mkdir(parents=True, exist_ok=True)


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    with (out / 'preparation.log').open('a') as stream:
        stream.write('$ ' + ' '.join(cmd) + '\n' + result.stdout + result.stderr)
    if result.returncode: raise RuntimeError(f'preparation failed: {cmd}; see {out}/preparation.log')
    return result.stdout


spec = importlib.util.spec_from_file_location('net_fixtures', ROOT / 'tests/unit/net_consumers_run.py')
fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
server = fixture.http.server.ThreadingHTTPServer(('127.0.0.1', 0), fixture.Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
tls_server = None
tls_started = False
iso = build / 'logit.iso'
try:
    manifest = run(['make', '-n', '-W', 'tools/mkfs.py', 'BUILD=' + str(build), str(build / 'disk.img')])
    (out / 'disk.make').write_text(manifest)
    disk = out / 'disk.img'
    extras = [str(build / 'net-consumers-guest.elf') + ':/bin/net-consumers']
    if a.before_net: extras.append(str(a.before_net.resolve()) + ':/bin/net-before')
    run([sys.executable, 'tests/boot/mk-tcc-disk.py', '.', str(out / 'disk.make'), str(disk), *extras])
    tls_url = ''
    if a.tls:
        openssl = os.environ.get('OPENSSL', 'openssl')
        ca_dir = out / 'ca'; ca_dir.mkdir(exist_ok=True)
        ca = ca_dir / 'root.pem'; cakey = out / 'root.key'
        run([openssl, 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
             '-nodes', '-keyout', str(cakey), '-out', str(ca), '-days', '2', '-subj', '/CN=Net Consumer Test CA',
             '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign'])
        key = out / 'server.key'; csr = out / 'server.csr'; cert = out / 'server.pem'
        run([openssl, 'req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
             '-keyout', str(key), '-out', str(csr), '-subj', '/CN=10.0.2.2'])
        (out / 'server.ext').write_text('subjectAltName=IP:10.0.2.2,DNS:10.0.2.2\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n')
        run([openssl, 'x509', '-req', '-in', str(csr), '-CA', str(ca), '-CAkey', str(cakey),
             '-CAcreateserial', '-days', '2', '-sha256', '-extfile', str(out / 'server.ext'), '-out', str(cert)])
        run([sys.executable, 'tools/genroots.py', str(ca_dir), str(out / 'roots_bundle.inc')])
        (out / 'roots_test.c').write_bytes((ROOT / 'c/crypto/trust/roots.c').read_bytes())
        # Replace the root object only at this private link. Production source,
        # root bundle, normal kernel and normal ISO keep the ordinary trust set.
        trust_obj = out / 'roots_test.o'
        fragment = out / 'test-trust.mk'
        fragment.write_text(f'{trust_obj}: {out}/roots_test.c {out}/roots_bundle.inc\n'
            f'\t$(CC) $(CFLAGS) -c {out}/roots_test.c -o $@\n'
            f'OBJ := $(filter-out $(BUILD)/c/crypto/trust/roots.o,$(OBJ)) {trust_obj}\n'
            f'$(KERNEL): {trust_obj}\n')
        iso = out / 'logit.iso'
        run(['make', '-j4', '-f', 'Makefile', '-f', str(fragment), 'BUILD=' + str(build),
             'KERNEL=' + str(out / 'kernel.elf'), 'ISO=' + str(iso), 'ISO_DIR=' + str(out / 'iso'), str(iso)])
        tls_server = fixture.http.server.ThreadingHTTPServer(('127.0.0.1', 0), fixture.Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.set_ecdh_curve('X448')
        context.load_cert_chain(cert, key)
        tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
        threading.Thread(target=tls_server.serve_forever, daemon=True).start()
        tls_started = True
        tls_url = f' https://10.0.2.2:{tls_server.server_port}'

    def digest(path):
        with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()
    artifacts = {'iso': str(iso), 'iso_sha256': digest(iso),
        'disk_sha256': digest(disk), 'net_aex_sha256': digest(build / 'net.aex'),
        'test_only_trust': a.tls}
    if a.before_net:
        artifacts.update(before_net=str(a.before_net.resolve()), before_net_sha256=digest(a.before_net.resolve()))
    (out / 'artifacts.json').write_text(json.dumps(artifacts, indent=2))
    data = bytearray()
    with tempfile.TemporaryDirectory(prefix='net-guest-') as tmp:
        serial = tmp + '/serial'
        cmd = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max', '-smp', '4', '-m', '512M',
               '-accel', 'tcg,thread=multi', '-cdrom', str(iso), '-boot', 'd', '-snapshot',
               '-drive', f'file={disk},format=raw,if=none,id=hd0', '-device', 'virtio-blk-pci,drive=hd0',
               '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none', '-no-reboot',
               '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
               '-chardev', f'socket,id=ser0,path={serial},server=on,wait=on', '-serial', 'chardev:ser0']
        (out / 'command.json').write_text(json.dumps(cmd, indent=2))
        connection = socket.socket(socket.AF_UNIX)
        with (out / 'qemu.log').open('wb') as log:
            process = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(300):
                    try: connection.connect(serial); break
                    except OSError:
                        if process.poll() is not None: raise RuntimeError('QEMU exited before serial')
                        time.sleep(.05)
                def reader():
                    try:
                        while True:
                            part = connection.recv(65536)
                            if not part: break
                            data.extend(part); (out / 'serial.log').write_bytes(data)
                    except OSError: pass
                thread = threading.Thread(target=reader, daemon=True); thread.start()
                def wait(marker, timeout):
                    end = time.monotonic() + timeout
                    while marker not in data:
                        if process.poll() is not None or time.monotonic() > end:
                            raise RuntimeError(f'guest did not reach {marker!r}; see {out}/serial.log')
                        time.sleep(.1)
                wait(b'LogitOS shell', 120)
                if a.before_net:
                    connection.sendall(f'/bin/net-before get http://10.0.2.2:{server.server_port}/data\n'.encode())
                    wait(b'http bytes 131072 fnv1a ', 60)
                    time.sleep(.2)
                connection.sendall(f'/bin/net-consumers http://10.0.2.2:{server.server_port}{tls_url}\n'.encode())
                wait(b'NET_CONSUMERS_DONE', 180)
                wait(b'failures=', 5); time.sleep(.1)
                text = data.decode(errors='replace')
                expected = 22 if a.tls else 21
                assert f'NET_CONSUMERS_DONE checks={expected} failures=0' in text, 'guest command/byte assertion failed'
                assert 'NET_CASE_FAIL' not in text and 'LOGIT_PANIC' not in text
                if a.tls: assert 'HelloRetryRequest:' in text and 'x448' in text
                (out / 'result.json').write_text(json.dumps({'checks': expected, 'failures': 0,
                    'guest': 'BIOS, 512 MiB, 4 vCPU, e1000', 'tls_x448': a.tls,
                    'before_bytes': 131072 if a.before_net else None, 'after_bytes': 300123}, indent=2))
                print(f'PASS: {expected} real guest checks; 300123-byte downloads; exact saved files; TLS X448={a.tls}')
            finally:
                stop_owned(process); connection.close(); (out / 'serial.log').write_bytes(data)
finally:
    server.shutdown(); server.server_close()
    # shutdown() waits for serve_forever(); a failed SSL setup has never
    # started that loop and must only close its listening socket.
    if tls_started: tls_server.shutdown()
    if tls_server: tls_server.server_close()
