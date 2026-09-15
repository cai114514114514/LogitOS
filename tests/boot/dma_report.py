#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Collect completed guest evidence; refuse missing/failed runs, never infer PASS from allocation logs."""
import argparse,hashlib,json,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
a=p.parse_args();b=a.build.resolve();r=Path(__file__).resolve().parents[2]
result={'status':'PASS','build':str(b),'matrix':[],'block':[],'shutdown':[]}
logs=[]
for mode in ('bios','uefi'):
    for ram in ('512M','2G','8G'):
        f=b/'wide-results'/(mode+'-'+ram)/'serial.log';s=f.read_text(errors='replace');logs.append(f)
        assert 'WIDE_MEMORY_GUEST_PASS' in s and '[widecheck] FAIL' not in s,(mode,ram)
        assert s.count('PIE_PROGRAM_PASS')>=2 and 'PIE_PROGRAM_FAIL' not in s,(mode,ram,'PIE')
        result['matrix'].append({'boot':mode,'ram':ram,'status':'PASS','serial':str(f)})
    for driver in ('nvme','ahci'):
        for boot in (1,2):
            d=b/'dma-block-results'/f'{driver}-{mode}-8G'/f'boot{boot}'
            v=json.loads((d/'result.json').read_text());assert v['pass'],v
            result['block'].append(v);logs.append(d/'serial.log')
    d=b/'dma-shutdown-results'/(mode+'-8G');v=json.loads((d/'result.json').read_text());assert v['pass'] and len(v['removed'])==11,v
    s=(d/'serial.log').read_text(errors='replace');assert 'coherent=0 bytes=0 mappings=0 pins=0 direct=0 bounce=0 quarantine=0/0/0 audit=0 bugs=0' in s
    result['shutdown'].append(v);logs.append(d/'serial.log')
result['drivers']=json.loads((b/'dma-driver-results/results.json').read_text())
expected={(mode,driver) for mode in ('bios','uefi') for driver in ('e1000','rtl8139','usb','hda','capture')}
assert {(v['boot'],v['driver']) for v in result['drivers'] if v['status']=='PASS'}==expected
for v in result['drivers']:
    d=Path(v['artifacts'])
    for name in ('serial.log','usb-serial.log','gate.log'):
        if (d/name).exists():logs.append(d/name)
result['virtio']=json.loads((b/'dma-virtio-results/result.json').read_text())
assert len(result['virtio']['devices'])==2 and all(v['all_queue_dma_above_4g'] and v['cpu_differs_from_dma'] for v in result['virtio']['devices'])
result['ime']=json.loads((b/'dma-ime-results/results.json').read_text())
assert len(result['ime'])==2 and all(v['status']=='PASS' for v in result['ime'])
result['addresses']=[];result['resource_counts']=[]
for f in dict.fromkeys(logs):
    for line in f.read_text(errors='replace').splitlines():
        if '[dma] alloc ' in line or '[dmacheck] backend=' in line:
            result['addresses'].append({'log':str(f),'line':line})
        if '[dma] boot ' in line or '[dma] wide-memory ' in line or 'DMA_SHUTDOWN_COUNTS ' in line:
            result['resource_counts'].append({'log':str(f),'line':line})
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()
result['artifacts_sha256']={name:digest(b/name) for name in ('kernel.elf','logit.iso','esp.img','wide-disk.img','capture-disk.img','shutdown-disk.img')}
paths=set()
for sub in ('c/drivers','c/kernel/mm','c/kernel/audio'):
    paths.update(f for f in (r/sub).rglob('*') if f.suffix in ('.c','.h'))
paths.update(r/f for f in ('c/kernel/init/kmain.c','c/kernel/gui/fb/fb.c','c/kernel/gui/fb/fb.h'))
result['sources_sha256']={str(f.relative_to(r)):digest(f) for f in sorted(paths)}
result['limits']=['RTL8169 has driver-model coverage only; no QEMU model or physical hardware validation','Legacy PMM and kernel heap remain low; no IOMMU','Unconfirmed block DMA stop quarantines resources and fail-stops to protect caller buffers']
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print('DMA_ACCEPTANCE_PASS:',a.out)
