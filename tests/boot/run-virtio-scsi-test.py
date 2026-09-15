#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise virtio-scsi through the real filesystem and two fresh QEMU boots.

The input image is always copied. Boot 1 runs the existing on-device storage
byte assertions (write/rewrite/grow/shrink/positional write/fsync); boot 2 reads
its final image byte-for-byte. The positive machine has no ATA/NVMe/virtio-blk
root disk. Controller enumeration or a shell-command echo cannot satisfy it.
Negative controls run the SAME positive root assertion against a machine with
no SCSI controller and a controller carrying an unsupported 4Kn disk.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time


def require(log, marker):
    if marker not in log:
        raise AssertionError('missing guest evidence: ' + marker.decode())


def positive_root(log, expected=b'vscsi0'):
    if expected == b'vscsi0':
        require(log, b'[virtio-scsi] vscsi0 target=0 lun=0')
    roots = re.findall(rb'\[blk\][^\r\n]*root = ([a-z0-9]+)', log)
    if roots != [expected]:
        raise AssertionError('expected sole root ' + expected.decode() + ', got ' + repr(roots))
    require(log, b'[fs] mounted')
    require(log, b'LOGIT_BOOT_OK')


def run_boot(args, disk, extra, mode, boot, out):
    out.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    sock = None
    with tempfile.TemporaryDirectory(prefix='vscsi-serial-') as serialdir:
        serialpath = serialdir + '/serial'
        cmd = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max',
               '-m', '512M', '-smp', '4', '-accel', 'tcg,thread=multi',
               '-vga', 'none', '-device', 'virtio-gpu-pci,xres=1280,yres=800',
               '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
               '-display', 'none', '-no-reboot',
               '-chardev', f'socket,id=ser,path={serialpath},server=on,wait=on',
               '-serial', 'chardev:ser',
               '-drive', f'file={disk},format=raw,if=none,id=root,cache=writeback']
        if getattr(args, 'esp', None):
            # Firmware NVRAM and ESP are private per boot, just like root data.
            # OVMF can touch its variable store even when the guest never does.
            esp = out / 'esp.img'
            variables = out / 'vars.fd'
            shutil.copyfile(args.esp, esp)
            shutil.copyfile(os.environ.get('OVMF_VARS_SRC', '/opt/homebrew/share/qemu/edk2-i386-vars.fd'), variables)
            cmd += ['-drive', 'if=pflash,format=raw,readonly=on,file=' +
                    os.environ.get('OVMF_CODE', '/opt/homebrew/share/qemu/edk2-x86_64-code.fd'),
                    '-drive', f'if=pflash,format=raw,file={variables}',
                    '-drive', f'if=none,format=raw,id=esp,file={esp}',
                    '-device', 'ich9-ahci,id=bootahci',
                    '-device', 'ide-hd,drive=esp,bus=bootahci.0,bootindex=1']
        else:
            cmd += ['-cdrom', str(args.iso.resolve()), '-boot', 'd']
        rootname = b'nvme0' if mode == 'nvme-bridge' else b'ahci0' if mode.startswith('ahci-') else b'vscsi0'
        if mode == 'nvme-bridge':
            cmd += ['-machine', 'q35', '-device', 'pcie-root-port,id=rp1,chassis=1,slot=1',
                    '-device', 'nvme,drive=root,serial=storage-hardware,bus=rp1,max_ioqpairs=1']
        elif mode.startswith('ahci-'):
            blocksize = 4096 if mode == 'ahci-4kn' else 512
            cmd += ['-device', 'ich9-ahci,id=ahci0', '-device',
                    f'ide-hd,drive=root,bus=ahci0.0,logical_block_size={blocksize},physical_block_size=4096,discard_granularity=0']
        elif mode == 'absent':
            cmd += ['-device', 'virtio-blk-pci,drive=root']
        else:
            # disable-legacy=on tests modern PCI ID 0x1048. A second disk on
            # target 3 ensures discovery does not stop at the first target.
            ctl = 'virtio-scsi-pci,id=scsi0'
            if mode != 'transitional':
                ctl += ',disable-legacy=on'
            cmd += ['-device', ctl]
            scsidisk = 'scsi-hd,drive=root,bus=scsi0.0,scsi-id=0,lun=0'
            if mode == '4kn':
                scsidisk += ',logical_block_size=4096,physical_block_size=4096'
            cmd += ['-device', scsidisk]
            if mode in ('positive', 'transitional'):
                cmd += ['-drive', f'file={extra},format=raw,if=none,id=extra',
                        '-device', 'scsi-hd,drive=extra,bus=scsi0.0,scsi-id=3,lun=0']
        (out / 'command.json').write_text(json.dumps(cmd, indent=2))
        with (out / 'qemu.log').open('wb') as err:
            proc = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
            try:
                sock = socket.socket(socket.AF_UNIX)
                until = time.monotonic() + args.timeout
                while True:
                    try:
                        sock.connect(serialpath)
                        break
                    except OSError:
                        if proc.poll() is not None or time.monotonic() > until:
                            raise RuntimeError('QEMU failed before serial; see ' + str(out / 'qemu.log'))
                        time.sleep(.05)
                sock.settimeout(.1)

                def wait_for(marker):
                    until = time.monotonic() + args.timeout
                    while marker not in log:
                        if b'STORGATE-FAIL' in log:
                            raise AssertionError('guest storage byte assertion failed')
                        if proc.poll() is not None or time.monotonic() > until:
                            raise RuntimeError('guest timeout waiting for ' + repr(marker))
                        try:
                            data = sock.recv(65536)
                            if not data:
                                raise RuntimeError('serial closed')
                            log.extend(data)
                            (out / 'serial.log').write_bytes(log)
                        except socket.timeout:
                            pass

                if mode in ('4kn', 'ahci-4kn'):
                    wait_for(b'[fs] mount FAILED')
                else:
                    wait_for(b'LogitOS shell')
                if mode in ('absent', '4kn', 'ahci-4kn'):
                    if mode == '4kn':
                        require(log, b'target=0 rejected: logical-sector=4096')
                        if b'[virtio-scsi] vscsi0' in log:
                            raise AssertionError('unsupported 4Kn medium was registered')
                    if mode == 'ahci-4kn':
                        require(log, b'logical-sector=4096 unsupported (need 512)')
                        if b'[blk] ahci0:' in log:
                            raise AssertionError('AHCI registered unsupported 4Kn disk')
                    try:
                        positive_root(log, rootname)
                    except AssertionError as expected:
                        print(f'EXPECTED-FAIL {mode}: {expected}', flush=True)
                    else:
                        raise AssertionError('negative control satisfied positive root assertion')
                    result = {'pass': True, 'negative_control': mode,
                              'positive_assertion_rejected': True}
                else:
                    positive_root(log, rootname)
                    if getattr(args, 'esp', None):
                        require(log, b'[efi] ebs ok')
                        require(log, b'[efi] jump')
                    if mode in ('positive', 'transitional'):
                        require(log, b'[virtio-scsi] vscsi3 target=3 lun=0 sectors=16384')
                    elif mode == 'nvme-bridge':
                        require(log, b'[nvme] queue grant=1 pair(s)')
                        addresses = re.findall(rb'\[nvme\] PCI ([0-9a-f]+):([0-9a-f]+)\.([0-9]+)', log)
                        if len(addresses) != 1 or int(addresses[0][0], 16) == 0:
                            raise AssertionError('NVMe was not found beyond root PCI bus')
                    if boot == 1:
                        sock.sendall(b'mkdir /sg\nas /usr/as/examples/storgate.as run\n')
                        wait_for(b'STORGATE-RUN-DONE')
                        checks = [b'write-300 verified', b'rewrite-120 verified',
                                  b'ftruncate-grow-200 gap is zeros',
                                  b'ftruncate-shrink-60 verified', b'midfile-rewrite verified',
                                  b'seek-past-end gap reads as zeros', b'final-image written',
                                  b'readonly ftruncate refused']
                        for check in checks:
                            require(log, b'STORGATE-ok ' + check)
                    else:
                        sock.sendall(b'as /usr/as/examples/storgate.as verify\n')
                        wait_for(b'STORGATE-VERIFY-DONE')
                        require(log, b'STORGATE-ok image across reboot byte-exact')
                    if b'STORGATE-FAIL' in log:
                        raise AssertionError('guest storage failure')
                    result = {'pass': True, 'mode': mode, 'boot': boot,
                              'root': rootname.decode(),
                              'firmware': 'uefi' if getattr(args, 'esp', None) else 'bios',
                              'persistent_byte_check': boot == 2}
                    print(f'PASS {mode} boot {boot}: ' +
                          ('writes/flush/byte checks' if boot == 1 else 'persistent bytes after cold boot'), flush=True)
                (out / 'result.json').write_text(json.dumps(result, indent=2))
            except Exception as exc:
                (out / 'result.json').write_text(json.dumps({'pass': False, 'error': str(exc)}, indent=2))
                raise
            finally:
                if proc.poll() is None:
                    proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                if sock:
                    sock.close()
                (out / 'serial.log').write_bytes(log)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', required=True, type=Path)
    ap.add_argument('--disk', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--mode', choices=('positive', 'transitional', 'negctl'), default='positive')
    ap.add_argument('--timeout', type=int, default=150)
    args = ap.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    # Unique directories avoid one invocation replacing another's live image.
    with tempfile.TemporaryDirectory(prefix='disks-', dir=args.out) as disks:
        disk = Path(disks) / 'root.img'
        shutil.copyfile(args.disk, disk)
        extra = Path(disks) / 'extra.img'
        with extra.open('wb') as fp:
            fp.truncate(8 << 20)
        if args.mode == 'negctl':
            for mode in ('absent', '4kn'):
                run_boot(args, disk, extra, mode, 1, args.out / mode)
        else:
            for boot in (1, 2):
                run_boot(args, disk, extra, args.mode, boot, args.out / f'boot{boot}')


if __name__ == '__main__':
    main()
