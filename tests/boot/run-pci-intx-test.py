#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two physical QEMU EDU sources sharing one guest-confirmed GSI.

The same guest assertions run in positive and intentionally broken kernels;
negative success requires a named failing assertion and a completed test, never
an absent marker, a timeout, a compilation error, or a QEMU launch failure.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--iso',required=True,type=Path)
ap.add_argument('--disk',required=True,type=Path)
ap.add_argument('--out',required=True,type=Path)
ap.add_argument('--expect-failure')
ap.add_argument('--timeout',type=int,default=150)
a=ap.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='disks-',dir=a.out) as tmp:
    disk=Path(tmp)/'root.img';shutil.copyfile(a.disk,disk)
    serial=a.out/'serial.log';serial.write_bytes(b'')
    cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-machine','pc','-cpu','max',
         '-m','512M','-smp','2','-accel','tcg,thread=multi','-cdrom',str(a.iso.resolve()),
         '-boot','d','-drive',f'file={disk},format=raw,if=none,id=root',
         '-device','virtio-blk-pci,drive=root','-vga','none','-device','virtio-gpu-pci',
         '-nic','none','-device','edu,addr=0x06','-device','edu,addr=0x0a',
         '-serial',f'file:{serial}','-display','none','-no-reboot']
    (a.out/'command.json').write_text(json.dumps(cmd,indent=2))
    with (a.out/'qemu.log').open('wb') as err:
        proc=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
        try:
            until=time.monotonic()+a.timeout
            log=b''
            while time.monotonic()<until and proc.poll() is None:
                log=serial.read_bytes()
                if b'PCI_INTX_GUEST_DONE ' in log:break
                time.sleep(.1)
            log=serial.read_bytes()
            routes=re.findall(rb'PCI_INTX_GUEST_ROUTE a=([^ ]+) pin=(\d+) line=(\d+) gsi=(\d+) b=([^ ]+) pin=(\d+) line=(\d+) gsi=(\d+)',log)
            if len(routes)!=1 or routes[0][3]!=routes[0][7]:
                raise RuntimeError('guest did not establish two functions on the same actual GSI')
            done=re.findall(rb'PCI_INTX_GUEST_DONE result=(PASS|FAIL) checks=(\d+) failed=(\d+)',log)
            if len(done)!=1:raise RuntimeError('guest did not complete the interrupt test')
            if a.expect_failure:
                marker=b'PCI_INTX_GUEST_CHECK '+a.expect_failure.encode()+b' FAIL'
                if done[0][0]!=b'FAIL' or marker not in log:
                    raise RuntimeError('negative control missed intended assertion '+a.expect_failure)
                print('EXPECTED-FAIL shared INTx: '+a.expect_failure,flush=True)
            else:
                if done[0][0]!=b'PASS' or int(done[0][2])!=0 or int(done[0][1])<15:
                    raise RuntimeError('guest assertions failed or did not run')
                required=['dual-a-payload','dual-b-payload','survivor-after-peer-unbind',
                          'last-owner-gsi-masked','released-vector-can-be-reused',
                          'retired-hardware-sources-reenabled-for-mask-test',
                          'masked-gsi-cannot-hit-reused-vector','no-retired-cookie-after-last-unbind']
                for check in required:
                    if b'PCI_INTX_GUEST_CHECK '+check.encode()+b' PASS' not in log:
                        raise RuntimeError('missing positive evidence '+check)
                print('PASS shared INTx: two EDU functions, actual GSI '+routes[0][3].decode()+
                      ', both delivered, survivor after unbind, final route masked and vector safely reused',flush=True)
            (a.out/'result.json').write_text(json.dumps({'pass':True,'negative_control':a.expect_failure,
                'gsi':int(routes[0][3]),'devices':[routes[0][0].decode(),routes[0][4].decode()],
                'guest_result':done[0][0].decode(),'checks':int(done[0][1]),'failed':int(done[0][2])},indent=2))
        except Exception as exc:
            (a.out/'result.json').write_text(json.dumps({'pass':False,'error':str(exc)},indent=2))
            raise
        finally:
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
