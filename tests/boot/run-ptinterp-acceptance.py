#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PT_INTERP controls, ordinary startup matrix and previous kernel regressions."""
import argparse, hashlib, json, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--apps',type=Path,default=ROOT/'build')
p.add_argument('--apps-disk',type=Path)
p.add_argument('--jobs',type=int,default=8)
a=p.parse_args();base=a.build.resolve();apps=base/'apps' if a.apps_disk else a.apps.resolve()
if base in (ROOT,apps) or base==ROOT/'build':raise SystemExit('Use an independent acceptance BUILD')
base.mkdir(parents=True,exist_ok=True);logs=base/'logs';logs.mkdir(exist_ok=True)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def sources():
    files=[]
    for directory in ['c/kernel','c/boot','c/drivers','c/fs','c/net','c/crypto','c/lib','include','rust/src']:
        files += [f for f in (ROOT/directory).rglob('*') if f.is_file() and f.suffix in ['.c','.h','.inc','.asm','.S','.ld','.sh','.rs']]
    for prefix in ['ptinterp','highheap','bkl','dma','physmap','wide','pie','block_dma','usb_process','serial_process','mm_','kheap','leak']:
        files += [f for f in (ROOT/'tests/unit').glob(prefix+'*') if f.is_file()]
    for directory in ['tests/unit/mmstub','tests/unit/pcistub','tests/unit/exechost']:
        files += [f for f in (ROOT/directory).rglob('*') if f.is_file()]
    # Hash reused runners/build fragments; source hashing is an explicit input
    # set, not a claim to snapshot the unrelated browser or all repository files.
    files += list((ROOT/'tests/boot').glob('*.py'))+list((ROOT/'tests/boot').glob('*.sh'))
    files += list((ROOT/'tests').glob('*.mk'))
    files += [ROOT/n for n in ['Makefile','linker.ld','tools/mkaex.py','tools/mkfs.py','tools/ksymbolize.py','c/apps/logit.h','c/apps/clib.h','c/apps/crt0_cli.asm','c/apps/coreutils/smptest.c','c/apps/coreutils/sigtest.c']]
    return {str(f.relative_to(ROOT)):digest(f) for f in sorted(set(files)) if f.is_file()}
report_path=base/'ptinterp-acceptance.json'
report=dict(started=time.time(),steps=[],source_sha256=sources(),scope='LogitOS kernel PT_INTERP; QEMU device paths and actual-source host tests; no full shared-library linker or physical-hardware claim')
def save():report_path.write_text(json.dumps(report,indent=2)+'\n')
def step(name,cmd):
    cmd=[str(x) for x in cmd];log=logs/(name+'.log')
    print('RUN',name,flush=True);start=time.monotonic()
    with log.open('w') as f:r=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
    report['steps'].append(dict(name=name,command=cmd,exit=r.returncode,seconds=round(time.monotonic()-start,3),log=str(log)));save()
    if r.returncode:
        print(log.read_text(errors='replace')[-7000:]);raise SystemExit('FAIL '+name+'; '+str(log))
    print('PASS',name,flush=True)
def make(name,build,*targets,flags=(),jobs=None):
    step(name,['make','-j'+str(jobs or a.jobs),'BUILD='+str(build),'WIDE_BASE_BUILD='+str(apps),'DMA_BASE_BUILD='+str(apps),
               'PTINTERP_CONTROL=','HIGHHEAPVERIFY=0','HIGHHEAP_CONTROL=','BKLVERIFY=0','BKLSERIAL=0','BKLIRQOFF=0','WIDEVERIFY=0','DMA_CAPTURE_VERIFY=0',*flags,*targets])
if a.apps_disk:
    step('recover-apps',['python3','tests/boot/stage-kernel-apps.py','--disk',a.apps_disk.resolve(),'--out',apps])
if (apps/'apps-snapshot.json').exists():report['apps_snapshot']=json.loads((apps/'apps-snapshot.json').read_text());save()
make('host',base/'host','test-ptinterp-host','test-pie','test-bkl-host','test-highheap-host','test-mk-wired',jobs=1)
normal=base/'ordinary';verify=base/'verify';regression=base/'regression'
make('ordinary-build',normal,'ptinterp-image','highheap-image')
disk=normal/'ptinterp-disk.img'
for control,assertion in [('areas','PTINTERP_RUNTIME_FAIL relro protect'),('metadata','PTINTERP_RUNTIME_FAIL metadata mmap exclusion'),('fork-fp','PTINTERP_FP_FAIL'),('stack','PTINTERP_RUNTIME_FAIL metadata image')]:
    d=base/('control-'+control)
    make('build-control-'+control,d,str(d/'logit.iso'),str(d/'esp.img'),flags=['PTINTERP_CONTROL='+control])
    step('control-'+control,['python3','tests/boot/run-ptinterp.py','--build',d,'--disk',disk,'--out',base/(control+'-results'),'--modes','bios','--ram','512M','--expect-failure',assertion])
step('interp-matrix',['python3','tests/boot/run-ptinterp.py','--build',normal,'--disk',disk,'--out',base/'interp-results'])
step('ordinary-bkl-structure',['python3','tests/unit/bkl_no_gate.py','--kernel',normal/'kernel.elf','--out',base/'ordinary-structure.json'])
make('verify-build',verify,'highheap-image','highheap-disk','bkl-image','bkl-disk',flags=['HIGHHEAPVERIFY=1','BKLVERIFY=1'])
heapdisk=verify/'highheap-disk.img';bkldisk=verify/'bkl-disk.img'
step('heap-matrix',['python3','tests/boot/run-highheap.py','--build',verify,'--disk',heapdisk,'--out',base/'heap-results'])
step('ordinary-module-matrix',['python3','tests/boot/run-highheap.py','--build',normal,'--disk',heapdisk,'--out',base/'module-results','--work','module'])
step('bkl-matrix',['python3','tests/boot/run-bkl.py','--build',verify,'--disk',bkldisk,'--out',base/'bkl-results'])
make('wide-pie-dma-regressions',regression,'test-dma-os',flags=['WIDEVERIFY=1','DMA_CAPTURE_VERIFY=1'],jobs=1)
step('ordinary-textedit-ime',['python3','tests/boot/run-dma-ime.py','--build',normal,'--disk',heapdisk,'--out',base/'ordinary-ime'])
for name,script,args in [('wait-smp','run-wait-smp.sh',['1','2','4','8']),('thread','run-thread-test.sh',[]),('signal','run-signal-test.sh',[]),('fork-storm','run-smp-fork-storm.sh',['120','4']),('heap-smp','run-smp-test.sh',[])]:
    step(name,['bash','tests/boot/'+script,normal/'logit.iso',bkldisk,*args])
images=[f for d in [normal,verify,regression]+[base/('control-'+c) for c in ['areas','metadata','fork-fp','stack']] for f in [d/'kernel.elf',d/'logit.iso',d/'esp.img']]
images += [disk,heapdisk,bkldisk,regression/'wide-disk.img']
report['image_sha256']={str(f.relative_to(base)):digest(f) for f in images}
report['source_unchanged']=report['source_sha256']==sources();report['finished']=time.time();report['passed']=report['source_unchanged'];save()
if not report['passed']:raise SystemExit('FAIL: declared inputs changed during acceptance; rebuild and rerun')
print('PTINTERP_ACCEPTANCE_PASS '+str(report_path),flush=True)
