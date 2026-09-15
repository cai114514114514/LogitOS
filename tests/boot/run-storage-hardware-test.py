#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Physical storage protocol/topology paths, verified with emulated hardware.

NVMe is placed behind a PCIe root port, never on bus 0. AHCI is presented with
512-byte logical / 4096-byte physical media (must persist bytes) and with native
4Kn media (must refuse before registration). No physical PC is exercised here.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile

spec=importlib.util.spec_from_file_location('storage_guest',Path(__file__).with_name('run-virtio-scsi-test.py'))
guest=importlib.util.module_from_spec(spec);spec.loader.exec_module(guest)
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--iso',required=True,type=Path)
ap.add_argument('--disk',required=True,type=Path)
ap.add_argument('--esp',type=Path,help='private copy of this ESP boots through OVMF instead of BIOS')
ap.add_argument('--out',required=True,type=Path)
ap.add_argument('--mode',choices=('nvme-bridge','ahci-sector'),required=True)
ap.add_argument('--timeout',type=int,default=150)
a=ap.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='disks-',dir=a.out) as d:
    disk=Path(d)/'root.img';shutil.copyfile(a.disk,disk)
    extra=Path(d)/'unused.img'
    if a.mode=='ahci-sector':
        try:
            guest.run_boot(a,disk,extra,'ahci-4kn',1,a.out/'4kn-refusal')
        except RuntimeError:
            qlog=a.out/'4kn-refusal/qemu.log'
            if 'logical_block_size must be 512 for IDE' not in qlog.read_text():
                raise
            reason='QEMU ide-hd cannot model native 4Kn; AHCI 4Kn rejection is verified only by production IDENTIFY fixtures'
            (a.out/'4kn-refusal/result.json').write_text(json.dumps({'skipped':True,'reason':reason},indent=2))
            print('SKIP '+reason+'; settle on a native 4Kn SATA disk with this kernel',flush=True)
        mode='ahci-512e'
    else:mode='nvme-bridge'
    for boot in (1,2):
        guest.run_boot(a,disk,extra,mode,boot,a.out/f'boot{boot}')
