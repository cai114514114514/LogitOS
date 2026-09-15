#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Isolated BKL acceptance: controls first, real guests, sequential benchmarks."""
import argparse,hashlib,json,os,statistics,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--apps',type=Path,default=ROOT/'build')
p.add_argument('--baseline-build',type=Path);p.add_argument('--baseline-tar',type=Path);p.add_argument('--samples',type=int,default=3);p.add_argument('--jobs',type=int,default=8)
a=p.parse_args();base=a.build.resolve();apps=a.apps.resolve()
if base==ROOT or base==apps:raise SystemExit('Use an independent acceptance BUILD, not the source/application directory')
if not 1<=a.samples<=15:raise SystemExit('samples must be 1..15')
base.mkdir(parents=True,exist_ok=True);logs=base/'logs';logs.mkdir(exist_ok=True)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def sources():
 paths=[]
 for directory in ['c/kernel','c/boot','c/drivers','c/fs','c/net','c/crypto','c/lib','include']:
  paths += [p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ['.c','.h','.inc','.asm']]
 paths += [ROOT/'c/apps/coreutils/smptest.c',ROOT/'tests/bkl.mk']
 paths += list((ROOT/'tests/unit').glob('bkl_*.c'))+list((ROOT/'tests/unit').glob('bkl_*.py'))
 paths += [ROOT/'tests/boot'/name for name in ['run-bkl.py','run-bkl-acceptance.py','mk-bkl-disk.py','serial-guest.py','run-smp-test.sh','run-thread-test.sh','run-smp-fork-storm.sh']]
 return {str(p.relative_to(ROOT)):digest(p) for p in sorted(paths)}
report={'started':time.time(),'steps':[],'source_sha256':sources(),'scope':'QEMU device models and real-source host fixtures; no physical-hardware claim'}
def save(): (base/'bkl-acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
def step(name,cmd):
 cmd=[str(x) for x in cmd];log=logs/(name+'.log');started=time.monotonic();print('RUN',name,flush=True)
 with log.open('w') as f:r=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
 report['steps'].append(dict(name=name,command=cmd,exit=r.returncode,seconds=round(time.monotonic()-started,3),log=str(log)));save()
 if r.returncode:
  print(log.read_text(errors='replace')[-6000:]);raise SystemExit('FAIL '+name+'; report: '+str(base/'bkl-acceptance.json'))
 print('PASS',name,flush=True)
def make(name,build,*targets,flags=()):
 step(name,['make','-j'+str(a.jobs),'BUILD='+str(build),'WIDE_BASE_BUILD='+str(apps),'DMA_BASE_BUILD='+str(apps),'BKLVERIFY=0','BKLSERIAL=0','BKLIRQOFF=0','WIDEVERIFY=0','DMA_CAPTURE_VERIFY=0',*flags,*targets])
normal=base/'ordinary';positive=base/'parallel';negative=base/'serialized';irqnegative=base/'irq-masked';regression=base/'regression'
make('host',base/'host','test-bkl-host')
make('ordinary-build',normal,'bkl-image')
cmd=['python3','tests/unit/bkl_no_gate.py','--kernel',normal/'kernel.elf','--out',base/'structure.json']
if a.baseline_build:cmd+=['--baseline-kernel',a.baseline_build.resolve()/'kernel.elf']
step('structure',cmd)
make('parallel-build',positive,'bkl-image','bkl-disk',flags=['BKLVERIFY=1'])
make('serialized-build',negative,'bkl-image',flags=['BKLVERIFY=1','BKLSERIAL=1'])
make('irq-masked-build',irqnegative,'bkl-image',flags=['BKLVERIFY=1','BKLIRQOFF=1'])
disk=positive/'bkl-disk.img'
step('serialized-control',['python3','tests/boot/run-bkl.py','--build',negative,'--disk',disk,'--out',base/'serialized-results','--modes','bios','--ram','512M','--expect-serialized'])
step('irq-masked-control',['python3','tests/boot/run-bkl.py','--build',irqnegative,'--disk',disk,'--out',base/'irq-masked-results','--modes','bios','--ram','512M','--expect-irq-masked'])
step('parallel-matrix',['python3','tests/boot/run-bkl.py','--build',positive,'--disk',disk,'--out',base/'parallel-results'])
# Serial make for device guests: they include their own controls, and QEMU
# timing/temporary resources must not compete across test targets.
oldjobs=a.jobs;a.jobs=1
make('wide-pie-dma-regressions',regression,'test-dma-os',flags=['WIDEVERIFY=1','DMA_CAPTURE_VERIFY=1'])
a.jobs=oldjobs
for name,script,args in [
 ('wait-smp','run-wait-smp.sh',['1','2','4','8']),
 ('signal','run-signal-test.sh',[]),
 ('thread','run-thread-test.sh',[]),
 ('fork-storm-1','run-smp-fork-storm.sh',['120','1']),
 ('fork-storm-4','run-smp-fork-storm.sh',['120','4']),
 ('heap-guest','run-smp-test.sh',[])]:
 step(name,['bash','tests/boot/'+script,normal/'logit.iso',disk,*args])
# Optional historical comparison uses one identical new program/disk on both
# old and ordinary kernels. Never invent a baseline from current source.
if a.baseline_build:
 paths={'before':a.baseline_build.resolve(),'after':normal};timings={'before':[],'after':[]}
 for sample in range(a.samples):
  for name in (['before','after'] if sample%2==0 else ['after','before']):
   out=base/f'benchmark-{name}-{sample}'
   step(f'benchmark-{name}-{sample}',['python3','tests/boot/run-bkl.py','--build',paths[name],'--disk',disk,'--out',out,'--modes','bios','--ram','8G','--bench'])
   timings[name].append(json.loads((out/'results.json').read_text())[0])
 medians={name:{field:statistics.median(x[field] for x in runs) for field in ['work_ms','external_ms']} for name,runs in timings.items()}
 report['benchmark']={'samples':timings,'medians':medians,'guest_ratio_before_over_after':medians['before']['work_ms']/medians['after']['work_ms'],'external_ratio_before_over_after':medians['before']['external_ms']/medians['after']['external_ms'],'identical_program_sha256':digest(positive/'bkl.aex'),'baseline_kernel_sha256':digest(paths['before']/'kernel.elf')};save()
else:report['benchmark']={'status':'not measured; supply --baseline-build to compare historical kernel'}
if a.baseline_tar:
 step('heap-source-comparison',['python3','tests/unit/bkl_kheap_run.py','--build',base/'heap-comparison','--iterations','100000','--baseline-tar',a.baseline_tar.resolve(),'--samples',a.samples])
 report['heap_comparison']=json.loads((base/'heap-comparison/comparison.json').read_text())
report['image_sha256']={str(path.relative_to(base)):digest(path) for d in [normal,positive,negative,irqnegative,regression] for path in [d/'kernel.elf',d/'logit.iso',d/'esp.img']}
report['source_unchanged']=report['source_sha256']==sources()
report['finished']=time.time();report['passed']=report['source_unchanged'];save()
if not report['passed']:raise SystemExit('FAIL: kernel source changed during acceptance; rebuild and rerun')
print('BKL_ACCEPTANCE_PASS '+str(base/'bkl-acceptance.json'),flush=True)
