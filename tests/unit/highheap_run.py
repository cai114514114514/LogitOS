#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutate actual allocation/domain paths, require named failures, then positive."""
import argparse,json,subprocess,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
s=(r/'c/kernel/mm/phys/kheap.c').read_text();report=[]
variants=[('ordinary-low','pmm_alloc_contig_masked(frames, UINT64_MAX, FRAME_SIZE, 0)','pmm_alloc_contig(frames)','HIGH_HEAP_ASSERT'),
          ('low-cache','alloc_domain(size, 1)','alloc_domain(size, 0)','LOW_DOMAIN_ASSERT'),
          ('split-domain','rest_size | (b->size & F_LOW)','rest_size','SPLIT_DOMAIN_ASSERT'),
          ('low-drains-caches','if (!low) mag_drain_all_locked();','mag_drain_all_locked();','LOW_CACHE_ASSERT'),('positive',None,None,None)]
for name,old,new,assertion in variants:
    d=b/name;d.mkdir(exist_ok=True)
    # Preserve quoted includes and relative weaksym path without editing the
    # shared tree. Only the single named production expression is mutated.
    for rel in ['c/kernel/mm','include/weaksym.h']:
        dst=d/rel;dst.parent.mkdir(parents=True,exist_ok=True)
        if (r/rel).is_dir():shutil.copytree(r/rel,dst,dirs_exist_ok=True)
        else:shutil.copy2(r/rel,dst)
    if old and s.count(old)!=1:raise SystemExit('control expression drifted: '+name)
    (d/'c/kernel/mm/phys/kheap.c').write_text(s.replace(old,new) if old else s)
    exe=d/'check'
    cmd=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-DMM_HOSTTEST','-fsanitize=address,undefined','-fno-sanitize-recover=all',
         '-I'+str(r/'tests/unit/mmstub'),'-I'+str(d/'c/kernel/mm'),str(r/'tests/unit/highheap_test.c'),str(d/'c/kernel/mm/phys/kheap.c'),str(r/'c/kernel/mm/phys/pmm.c'),'-o',str(exe)]
    subprocess.run(cmd,check=True,cwd=r)
    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60);text=run.stdout+run.stderr;(d/'run.log').write_text(text)
    ok=(run.returncode==1 and 'FAIL: '+assertion in text) if assertion else run.returncode==0
    ok=ok and 'ERROR: AddressSanitizer' not in text and 'runtime error:' not in text
    report.append(dict(case=name,passed=ok,exit=run.returncode,log=str(d/'run.log')))
    (b/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(('PASS ' if ok else 'FAIL ')+name,flush=True)
    if not ok:raise SystemExit(text)
