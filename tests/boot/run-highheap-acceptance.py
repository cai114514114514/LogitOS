#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Complete high heap acceptance; controls precede real guest positives."""
import argparse,hashlib,json,statistics,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True)
p.add_argument('--apps',type=Path,default=ROOT/'build');p.add_argument('--apps-disk',type=Path)
p.add_argument('--baseline',type=Path);p.add_argument('--jobs',type=int,default=8);a=p.parse_args()
base=a.build.resolve();apps=(base/'apps') if a.apps_disk else a.apps.resolve()
if base in (ROOT,apps):raise SystemExit('Use an independent acceptance BUILD')
base.mkdir(parents=True,exist_ok=True);logs=base/'logs';logs.mkdir(exist_ok=True)
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def sources():
    files=[]
    for directory in ['c/kernel','c/boot','c/drivers','c/fs','c/net','c/crypto','c/lib','include']:
        files += [p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ['.c','.h','.inc','.asm','.S','.ld','.sh']]
    files += list((ROOT/'tests/unit').glob('highheap*'))
    files += [ROOT/'tests/boot'/n for n in ['run-highheap.py','run-highheap-acceptance.py','stage-kernel-apps.py']]
    files += [ROOT/'tests/highheap.mk']
    files += [ROOT/'Makefile',ROOT/'linker.ld']
    # Record reused harnesses and their build fragments too. This is the
    # declared input set, not a claim about every unrelated file in the tree.
    for prefix in ['bkl','dma','physmap','wide','pie','block_dma','usb_process','serial_process','mm_','kheap','leak']:
        files += [f for f in (ROOT/'tests/unit').glob(prefix+'*') if f.is_file()]
    files += [f for f in (ROOT/'tests/unit/mmstub').rglob('*') if f.is_file()]
    for pattern in ['run-bkl*','run-dma*','dma-*','run-wide*','run-block-dma*','*ime*.py','*ime*.sh','run-wait-smp.sh','run-thread-test.sh','run-signal-test.sh','run-smp*.sh','serial-guest.py','mk-*-disk.py']:
        files += list((ROOT/'tests/boot').glob(pattern))
    for name in ['bkl.mk','dma.mk','dma_drivers.mk','wide_memory.mk','pie.mk','thread.mk','smpstorm.mk']:
        files += [ROOT/'tests'/name]
    files += [ROOT/name for name in ['tools/mkaex.py','tools/mkfs.py','tools/license_audit.py','tools/ksymbolize.py','c/apps/clib.h','c/apps/crt0_cli.asm','c/apps/coreutils/smptest.c','c/apps/coreutils/sigtest.c']]
    files += list((ROOT/'rust/src').rglob('*.rs'))
    return {str(f.relative_to(ROOT)):digest(f) for f in sorted(files) if f.is_file()}
report=dict(started=time.time(),steps=[],source_sha256=sources(),scope='QEMU real kernel/device paths plus actual-source host tests; no physical hardware claim')
def save():(base/'highheap-acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
def step(name,cmd):
    cmd=[str(x) for x in cmd];log=logs/(name+'.log');start=time.monotonic();print('RUN',name,flush=True)
    with log.open('w') as f:run=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
    report['steps'].append(dict(name=name,command=cmd,exit=run.returncode,seconds=round(time.monotonic()-start,3),log=str(log)));save()
    if run.returncode:print(log.read_text(errors='replace')[-7000:]);raise SystemExit('FAIL '+name+'; '+str(log))
    print('PASS',name,flush=True)
def make(name,build,*targets,flags=(),jobs=None):
    step(name,['make','-j'+str(jobs or a.jobs),'BUILD='+str(build),'WIDE_BASE_BUILD='+str(apps),'DMA_BASE_BUILD='+str(apps),
               'HIGHHEAPVERIFY=0','HIGHHEAP_CONTROL=','BKLVERIFY=0','BKLSERIAL=0','BKLIRQOFF=0','WIDEVERIFY=0','DMA_CAPTURE_VERIFY=0',*flags,*targets])
if a.apps_disk:
    step('recover-accepted-apps',['python3','tests/boot/stage-kernel-apps.py','--disk',a.apps_disk.resolve(),'--out',apps])
    report['apps_snapshot']=json.loads((apps/'apps-snapshot.json').read_text());save()
make('host',base/'host','test-highheap-host','test-kheap','test-leak','test-bkl-host','test-mk-wired',jobs=1)
normal=base/'ordinary';verify=base/'verify';regression=base/'regression'
make('ordinary-build',normal,'highheap-image')
make('verify-build',verify,'highheap-image','highheap-disk','bkl-image','bkl-disk',flags=['HIGHHEAPVERIFY=1','BKLVERIFY=1'])
step('ordinary-bkl-structure',['python3','tests/unit/bkl_no_gate.py','--kernel',normal/'kernel.elf','--out',base/'ordinary-structure.json'])
# ELF symbols are retained by both builds. Require the actual test function
# in the instrumented binary and its absence in the ordinary binary.
symbol=b'\x00highheap_verify\x00'
report['test_selector_isolated']=symbol not in (normal/'kernel.elf').read_bytes() and symbol in (verify/'kernel.elf').read_bytes()
save()
if not report['test_selector_isolated']:raise SystemExit('FAIL: high heap verification leaked into ordinary kernel or is absent from test kernel')
heapdisk=verify/'highheap-disk.img';bkldisk=verify/'bkl-disk.img'
for control,ram,assertion in [('identity','512M','[highheap] FAIL ALIAS_ASSERT'),('low-pages','8G','[highheap] FAIL HIGH_HEAP_ASSERT'),('module-high','512M','HIGHHEAP_MODULE_FAIL -7')]:
    d=base/('control-'+control)
    make('build-control-'+control,d,'highheap-image',flags=['HIGHHEAPVERIFY=1','HIGHHEAP_CONTROL='+control])
    step('control-'+control,['python3','tests/boot/run-highheap.py','--build',d,'--disk',heapdisk,'--out',base/(control+'-results'),'--modes','bios','--ram',ram,'--expect-failure',assertion])
step('heap-matrix',['python3','tests/boot/run-highheap.py','--build',verify,'--disk',heapdisk,'--out',base/'heap-results'])
step('ordinary-module-matrix',['python3','tests/boot/run-highheap.py','--build',normal,'--disk',heapdisk,'--out',base/'module-results','--work','module'])
step('ordinary-high-stack-panic',['python3','tests/boot/run-highheap.py','--build',normal,'--disk',heapdisk,'--out',base/'panic-results','--ram','8G','--work','panic'])
degraded=base/'low-map-only'
make('low-map-only-build',degraded,'highheap-image',flags=['WIDENOMAP=1'])
step('low-map-only-panic',['python3','tests/boot/run-highheap.py','--build',degraded,'--disk',heapdisk,'--out',base/'low-map-only-results','--ram','8G','--work','panic'])
step('bkl-matrix',['python3','tests/boot/run-bkl.py','--build',verify,'--disk',bkldisk,'--out',base/'bkl-results'])
make('wide-pie-dma-regressions',regression,'test-dma-os',flags=['WIDEVERIFY=1','DMA_CAPTURE_VERIFY=1'],jobs=1)
step('ordinary-textedit-ime',['python3','tests/boot/run-dma-ime.py','--build',normal,'--disk',heapdisk,'--out',base/'ordinary-ime'])
for name,script,args in [('wait-smp','run-wait-smp.sh',['1','2','4','8']),('thread','run-thread-test.sh',[]),
                         ('signal','run-signal-test.sh',[]),('fork-storm','run-smp-fork-storm.sh',['120','4']),('heap-smp','run-smp-test.sh',[])]:
    step(name,['bash','tests/boot/'+script,normal/'logit.iso',bkldisk,*args])
if a.baseline:
    timings={'before':[],'after':[]}
    for sample in range(3):
        for name in (['before','after'] if sample%2==0 else ['after','before']):
            out=base/f'benchmark-{name}-{sample}';build=a.baseline.resolve() if name=='before' else normal
            step(f'benchmark-{name}-{sample}',['python3','tests/boot/run-bkl.py','--build',build,'--disk',bkldisk,'--out',out,'--modes','bios','--ram','8G','--bench'])
            timings[name].append(json.loads((out/'results.json').read_text())[0])
    report['benchmark']=dict(scope='identical guest MM/FS workload, BKL already removed in both kernels',samples=timings,
                             median_ms={n:statistics.median(x['work_ms'] for x in runs) for n,runs in timings.items()},
                             program_sha256=digest(verify/'bkl.aex'),baseline_kernel_sha256=digest(a.baseline/'kernel.elf'))
report['image_sha256']={str(f.relative_to(base)):digest(f) for d in [normal,verify,regression,degraded]+[base/('control-'+n) for n in ['identity','low-pages','module-high']] for f in [d/'kernel.elf',d/'logit.iso',d/'esp.img']}
report['source_unchanged']=report['source_sha256']==sources();report['finished']=time.time();report['passed']=report['source_unchanged'];save()
if not report['passed']:raise SystemExit('FAIL: source changed during acceptance; rebuild and rerun')
print('HIGHHEAP_ACCEPTANCE_PASS '+str(base/'highheap-acceptance.json'),flush=True)
