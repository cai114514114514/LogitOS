#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual kheap/ticket-lock ownership gates; elapsed time is evidence, not a threshold."""
from pathlib import Path
import argparse,json,re,subprocess,tarfile,statistics,hashlib
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',default='/tmp/logitos-bkl-20260910/proc/kheap');p.add_argument('--iterations',type=int,default=10000);p.add_argument('--benchmark',action='store_true');p.add_argument('--baseline-tar');p.add_argument('--samples',type=int,default=3);a=p.parse_args()
if not 0<a.iterations<=10000000:raise SystemExit('iterations must be 1..10000000')
out=Path(a.build).resolve();out.mkdir(parents=True,exist_ok=True)
flags=['clang','-std=c11','-O2' if a.benchmark else '-O1','-g','-Wall','-Wextra','-Werror','-pthread','-DMM_HOSTTEST','-DMM_CONCURRENT','-DLOGIT_LOCK_HOST','-Itests/unit/mmstub','-Ic/kernel/mm -Ic/kernel/mm/phys -Ic/kernel/mm/virt -Ic/kernel/mm/cache -Ic/kernel/mm/reclaim']
if not a.benchmark:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
sources=['tests/unit/bkl_kheap_test.c','c/kernel/mm/phys/kheap.c','c/kernel/cpu/spinlock.c'];report=[]
for control in [True,False]:
 name='global-mag' if control else 'per-cpu';exe=out/name
 b=subprocess.run(flags+(['-DKHEAP_GLOBAL_MAG'] if control else [])+sources+['-o',str(exe)],cwd=root,text=True,capture_output=True)
 (out/(name+'-build.log')).write_text(b.stdout+b.stderr)
 if b.returncode:raise SystemExit('compile failed: '+str(out/(name+'-build.log')))
 r=subprocess.run([str(exe),str(a.iterations)],cwd=root,text=True,capture_output=True,timeout=60)
 text=r.stdout+r.stderr;(out/(name+'.log')).write_text(text);print(text,end='',flush=True)
 ok=(r.returncode==1 and 'FAIL magazine_cpus records eight independent active CPU caches' in text) if control else r.returncode==0
 m=re.search(r'wall_ns=(\d+)',text);report.append(dict(case=name,passed=ok,exit=r.returncode,wall_ns=int(m.group(1)) if m else None,log=str(out/(name+'.log'))))
 if not ok:raise SystemExit(name+' gate failed')
 print(name+': PASS'+(' (named ownership assertion rejected control)' if control else ''),flush=True)
(out/'results.json').write_text(json.dumps(dict(benchmark=a.benchmark,iterations=a.iterations,cases=report),indent=2)+'\n')

# Comparison is optional and runs only after strict positive/negative gates.
# Both allocators use the SAME current production ticket-lock implementation;
# this isolates allocator contention from scheduler/entry or lock changes.
if a.baseline_tar:
 if not 1<=a.samples<=15:raise SystemExit('samples must be 1..15')
 old=out/'before/c/kernel/mm/phys/kheap.c';old.parent.mkdir(parents=True,exist_ok=True)
 weak=out/'before/include/weaksym.h';weak.parent.mkdir(parents=True,exist_ok=True)
 with tarfile.open(a.baseline_tar,'r:gz') as archive:
  old.write_bytes(archive.extractfile('c/kernel/mm/phys/kheap.c').read())
 weak.write_bytes((root/'include/weaksym.h').read_bytes())
 measure_flags=['clang','-std=c11','-O2','-g','-Wall','-Wextra','-Werror','-pthread','-DMM_HOSTTEST','-DMM_CONCURRENT','-DLOGIT_LOCK_HOST','-DKHEAP_MEASURE_ONLY','-Itests/unit/mmstub','-Ic/kernel/mm -Ic/kernel/mm/phys -Ic/kernel/mm/virt -Ic/kernel/mm/cache -Ic/kernel/mm/reclaim']
 bins={}
 for name,allocator in [('before',old),('after',root/'c/kernel/mm/phys/kheap.c')]:
  exe=out/('measure-'+name);bins[name]=exe
  b=subprocess.run(measure_flags+['tests/unit/bkl_kheap_test.c',str(allocator),'c/kernel/cpu/spinlock.c','-o',str(exe)],cwd=root,text=True,capture_output=True)
  (out/('measure-'+name+'-build.log')).write_text(b.stdout+b.stderr)
  if b.returncode:raise SystemExit('baseline comparison compile failed: '+name)
 times={'before':[],'after':[]}
 for sample in range(a.samples):
  for name in (['before','after'] if sample%2==0 else ['after','before']):
   r=subprocess.run([str(bins[name]),str(a.iterations)],cwd=root,text=True,capture_output=True,timeout=60)
   text=r.stdout+r.stderr;(out/(f'measure-{name}-{sample}.log')).write_text(text);print(name+': '+text,end='',flush=True)
   if r.returncode:raise SystemExit('baseline byte workload failed: '+name)
   m=re.search(r'wall_ns=(\d+)',text)
   if not m:raise SystemExit('missing actual workload timing: '+name)
   times[name].append(int(m.group(1)))
 result=dict(iterations_per_cpu=a.iterations,cpus=8,samples=a.samples,scope='allocator source comparison with identical current ticket locks; not guest or BKL throughput',wall_ns=times,median_ns={k:statistics.median(v) for k,v in times.items()})
 result['median_ratio_before_over_after']=result['median_ns']['before']/result['median_ns']['after']
 result['source_sha256']={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in [('before',old),('after',root/'c/kernel/mm/phys/kheap.c'),('shared_ticket_lock',root/'c/kernel/cpu/spinlock.c')]}
 result['baseline_archive']=str(Path(a.baseline_tar).resolve())
 (out/'comparison.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
