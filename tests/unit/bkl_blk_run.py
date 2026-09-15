#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Four real host threads drive the production block request engine."""
from pathlib import Path
import argparse,os,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');a=ap.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
name='no-medium-lock' if a.negative_only else 'positive';exe=b/name
cmd=[os.environ.get('CC','clang'),'-O1','-g','-pthread','-fsanitize=address,undefined','-DBLK_HOSTTEST']
if a.negative_only:cmd+=['-DIO_NO_LOCK']
cmd+=['-I'+str(r/p) for p in ['tests/unit','c/drivers/block','c/drivers/virtio','c/kernel/core','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/mm','c/kernel/mm/phys','c/kernel/mm/virt','c/kernel/mm/cache','c/kernel/mm/reclaim','c/kernel/mm/phys','c/kernel/mm/virt','c/kernel/mm/cache','c/kernel/mm/reclaim']]
cmd += [str(r/'tests/unit/bkl_blk_test.c'),str(r/'c/drivers/block/blkdev.c'),'-o',str(exe)]
subprocess.run(cmd,check=True)
p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);(b/(name+'.log')).write_text(p.stdout+p.stderr)
if a.negative_only:assert p.returncode==1 and 'FAIL: one medium callback at a time on four CPUs' in p.stdout,(p.stdout,p.stderr)
else:assert p.returncode==0,(p.stdout,p.stderr)
print(name+': '+p.stdout.strip().splitlines()[-1])
