#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run real TextEdit keyboard/IME/save assertions on both firmware at 8 GiB."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for key in ('build','disk','out'):p.add_argument('--'+key,type=Path,required=True)
p.add_argument('--modes',default='bios,uefi');a=p.parse_args()
r=Path(__file__).resolve().parents[2];a.build=a.build.resolve();a.disk=a.disk.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
results=[]
for mode in a.modes.split(','):
    out=a.out/mode;out.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,QEMU=str(r/'tests/boot/dma-qemu-wrapper.py'),IME_TEST_RAM='8G',DMA_TEST_BOOT=mode,DMA_TEST_RAM='8G',DMA_TEST_BUILD=str(a.build),DMA_TEST_OUT=str(out/'boot'))
    with (out/'gate.log').open('w') as f:
        result=subprocess.run(['python3',str(r/'tests/boot/run-ime-usability.py'),str(a.build/'logit.iso'),str(a.disk),str(out)],cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=1080)
    if result.returncode:raise SystemExit('FAIL '+mode+' IME: '+str(out/'gate.log'))
    results.append({'boot':mode,'ram':'8G','status':'PASS','checks':['ASCII control','Chinese candidate','TextEdit UTF-8 saved bytes'],'artifacts':str(out)})
    print('PASS',mode,'8G TextEdit/IME',flush=True)
(a.out/'results.json').write_text(json.dumps(results,indent=2))
