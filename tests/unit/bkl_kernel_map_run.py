#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual shared kernel owners; deterministic race and partial-allocation controls run first."""
import argparse,json,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--build',default='/tmp/logitos-bkl-20260910/proc/kernel-map');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.build).resolve();out.mkdir(parents=True,exist_ok=True)
src=['tests/unit/bkl_kernel_map_test.c','tests/unit/mm_common.c']+['c/kernel/mm/'+s+'.c' for s in ['pmm','vmm','fault','vma','rmap','reclaim','swap','pcache','shm','oom']]+['c/kernel/cpu/spinlock.c']
flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-DMM_HOSTTEST','-DMM_CONCURRENT','-DLOGIT_LOCK_HOST','-DMM_KERNEL_MAP_TEST','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Itests/unit','-Itests/unit/mmstub','-Ic/kernel/mm -Ic/kernel/mm/phys -Ic/kernel/mm/virt -Ic/kernel/mm/cache -Ic/kernel/mm/reclaim']
report=[]
cases=[('no-kernel-owner','MM_NO_KERNEL_MAP_LOCK','concurrent kernel table publication preserves both leaf mappings'),('no-partial-publication','MM_NO_PARTIAL_KERNEL_PUBLISH','partial kernel table allocation failure publishes valid shared roots'),('positive',None,None)]
for name,macro,assertion in cases:
 control=macro is not None;exe=out/name
 b=subprocess.run(flags+(['-D'+macro] if macro else [])+src+['-o',str(exe)],cwd=root,text=True,capture_output=True);(out/(name+'-build.log')).write_text(b.stdout+b.stderr)
 if b.returncode:raise SystemExit('compile failed: '+str(out/(name+'-build.log')))
 r=subprocess.run([str(exe)],cwd=root,text=True,capture_output=True,timeout=30);text=r.stdout+r.stderr;(out/(name+'.log')).write_text(text);print(text,end='',flush=True)
 ok=(r.returncode==1 and ('FAIL: '+assertion) in text) if control else r.returncode==0
 report.append(dict(case=name,passed=ok,exit=r.returncode,log=str(out/(name+'.log'))))
 if not ok:raise SystemExit('kernel mapping gate failed: '+name)
 print(name+': PASS'+(' (named publication assertion caught)' if control else ''),flush=True)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
