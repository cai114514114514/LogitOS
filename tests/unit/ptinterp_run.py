#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build independent PT_INTERP fixtures and actual-source semantic controls."""
import argparse,json,os,platform,re,shutil,struct,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--fixtures-only',action='store_true');a=p.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
def run(cmd):subprocess.run(list(map(str,cmd)),cwd=r,check=True)
cc=os.environ.get('CC','clang');ld=os.environ.get('LD','ld.lld');nasm=os.environ.get('ASM','nasm')
flags=['--target=x86_64-elf','-ffreestanding','-nostdlib','-fPIE','-ftls-model=local-exec','-fno-stack-protector','-mno-red-zone','-O2','-Ic/apps','-Iinclude/abi','-Ic/kernel/exec -Ic/kernel/exec/load -Ic/kernel/exec/signal -Ic/kernel/exec/fd']
for src,name in [('ptinterp_program.c','program'),('ptinterp_runtime.c','runtime'),('ptinterp_driver.c','driver'),('ptinterp_gui.c','gui')]:run([cc,*flags,'-c','tests/unit/'+src,'-o',b/(name+'.o')])
for src,name in [('c/apps/crt0_cli.asm','crt'),('tests/unit/pie_thread.asm','thread'),('tests/unit/ptinterp_start.asm','start')]:run([nasm,'-f','elf64',src,'-o',b/(name+'.o')])
fork_nr=re.search(r'^#define SYS_FORK\s+(\d+)',(r/'include/abi/logit_abi.h').read_text(),re.M).group(1)
run([nasm,'-f','elf64','-DSYS_FORK='+fork_nr,'tests/unit/ptinterp_fork_fp.asm','-o',b/'fp.o'])
run([ld,'-pie','--no-dynamic-linker','-nostdlib','-z','text','-z','relro','-z','now','-e','_start','-o',b/'ld-logit-test.so',b/'start.o',b/'runtime.o'])
run([ld,'-pie','--dynamic-linker=/lib/ld-logit-test.so','-nostdlib','-z','text','-z','relro','-z','now','-e','_start','-o',b/'program.elf',b/'crt.o',b/'program.o',b/'thread.o'])
run(['python3','tools/mkaex.py','--cli',b/'program.elf',b/'program.aex','ptinterp','-','*','150','150','150'])
run([ld,'--image-base=0x50000000','--dynamic-linker=/lib/ld-logit-test.so','--export-dynamic','-nostdlib','-z','relro','-e','_start','-o',b/'fixed.elf',b/'crt.o',b/'program.o',b/'thread.o'])
run([ld,'--image-base=0x50000000','-nostdlib','-e','_start','-o',b/'driver.elf',b/'crt.o',b/'driver.o',b/'fp.o'])
run([ld,'-pie','--dynamic-linker=/lib/ld-logit-test.so','-nostdlib','-z','relro','-e','_start','-o',b/'gui.elf',b/'crt.o',b/'gui.o'])
run(['python3','tools/mkaex.py',b/'gui.elf',b/'gui.aex','Interp GUI','-','*','90','150','180','--stack-pages','1024','--category','test'])
run(['python3','tools/mkaex.py',b/'gui.elf',b/'tiny.aex','Interp Tiny','-','*','90','150','180','--stack-pages','1','--category','test'])
program=(b/'program.elf').read_bytes()
for name,path in [('missing','none'),('denied','deny'),('nested','loop'),('junk','junk')]:
 data=program.replace(b'/lib/ld-logit-test.so',('/lib/ld-logit-'+path+'.so').encode());assert len(data)==len(program)
 (b/(name+'.elf')).write_bytes(data)
(b/'junk.so').write_bytes(b'unsupported interpreter format'.ljust(128,b'!'))
if a.fixtures_only:raise SystemExit(0)
arch=['-arch','x86_64'] if platform.system()=='Darwin' else []
host=[cc,*arch,'-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-DLOGIT_HOSTTEST','-Ic/kernel/exec -Ic/kernel/exec/load -Ic/kernel/exec/signal -Ic/kernel/exec/fd','-Itests/unit/exechost','-Ic/fs -Ic/fs/vfs -Ic/fs/logitfs -Ic/fs/cache -Ic/fs/ramfs -Ic/fs/ctl -Ic/fs/procfs','-Ic/kernel/mm -Ic/kernel/mm/phys -Ic/kernel/mm/virt -Ic/kernel/mm/cache -Ic/kernel/mm/reclaim','-Ic/crypto','-Ic/crypto/trust','-Ic/drivers/block']
sources=['tests/unit/ptinterp_host.c','tests/unit/pie_space.c','c/kernel/exec/load/interp.c','c/drivers/block/crc32.c','c/crypto/hash/sha256.c']
original=(r/'c/kernel/exec/load/elf.c').read_text()
controls={
 'start':('out->start_entry = interp.entry;','out->start_entry = out->entry;','START_ASSERT'),
 'base':('out->interp_base = interp.load_bias;','out->interp_base = 0;','BASE_ASSERT'),
 'defer':('int deferred = interpreter || out->interp_path[0];','int deferred = interpreter;','DEFER_ASSERT'),
 'relro':('if (relro && !deferred) {','if (relro) {','RELRO_ASSERT')}
results=[]
for name,change in list(controls.items())+[('positive',None),('asan',None)]:
 source=r/'c/kernel/exec/load/elf.c'
 if change:
  old,new,assertion=change;assert original.count(old)==1
  tree=b/name;source=tree/'c/kernel/exec/load/elf.c';source.parent.mkdir(parents=True,exist_ok=True)
  source.write_text(original.replace(old,new));shutil.copy2(r/'c/kernel/exec/load/elf.h',source.with_name('elf.h'))
  (tree/'c/kernel/mm').mkdir(parents=True,exist_ok=True);shutil.copy2(r/'c/kernel/mm/mm.h',tree/'c/kernel/mm/mm.h')
  (tree/'include').mkdir(exist_ok=True);shutil.copy2(r/'include/weaksym.h',tree/'include/weaksym.h')
  (tree/'include/abi').mkdir(exist_ok=True);shutil.copy2(r/'include/abi/aex_agent.h',tree/'include/abi/aex_agent.h')
 exe=b/('host-'+name);more=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-DELF_PIE_BASE=0x200000000000ull'] if name=='asan' else []
 run([*host,*more,*sources,source,'-o',exe])
 z=subprocess.run([exe,b/'program.elf',b/'ld-logit-test.so'],cwd=r,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
 (b/(name+'.log')).write_text(z.stdout+z.stderr)
 ok=(z.returncode==1 and 'FAIL '+assertion in z.stderr) if change else z.returncode==0
 results.append(dict(name=name,passed=ok,exit=z.returncode));print(('PASS' if ok else 'FAIL'),name,z.stdout.strip(),flush=True)
 if not ok:print(z.stderr[-4000:]);raise SystemExit(1)
(b/'results.json').write_text(json.dumps(results,indent=2)+'\n')
