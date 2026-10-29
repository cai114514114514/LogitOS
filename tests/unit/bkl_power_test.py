#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse,os,pathlib,re,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-power");p.add_argument("--source-dir");a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];b=pathlib.Path(a.build);b.mkdir(parents=True,exist_ok=True)
src=pathlib.Path(a.source_dir) if a.source_dir else r/"c/kernel/core"
(b/"power.h").write_text((src/"power.h").read_text())
(b/"wait.h").write_text("#include <pthread.h>\nstruct mutex {pthread_mutex_t m;};\n#define MUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}\nstatic inline void mutex_lock(struct mutex *m){pthread_mutex_lock(&m->m);}\nstatic inline void mutex_unlock(struct mutex *m){pthread_mutex_unlock(&m->m);}\n")
(b/"io.h").write_text("#include <stdint.h>\nvoid outb(uint16_t,uint8_t);\nvoid outw(uint16_t,uint16_t);\n")
s=(src/"power.c").read_text()
# A host cannot reset its IDT or run x86 pause on arm64. Only the privileged
# leaf statements are replaced; preparation/cancellation/port order is real.
s,n=re.subn(r'__asm__ volatile\s*\([^;]*\);','(void)0;',s)
if n!=5:raise SystemExit('power privileged leaf count changed: '+str(n))
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-I"+str(b)]
for d in ("c/kernel/core","c/kernel/cpu","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/fs","c/fs/vfs","c/fs/logitfs","c/fs/cache","c/fs/ramfs","c/fs/ctl","c/fs/procfs","c/fs/vfs","c/fs/logitfs","c/fs/cache","c/fs/ramfs","c/fs/ctl","c/fs/procfs","c/drivers/block","include/abi"):base += ["-I"+str(r/d)]
def run(name,code,mode,defs=(),expect=None):
    source=b/(name+".c");source.write_text(code);exe=b/name
    subprocess.run(base+list(defs)+[str(r/"tests/unit/bkl_power_test.c"),str(source),"-o",str(exe)],check=True,cwd=r)
    q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if expect:
        if q.returncode==0 or expect not in out:raise SystemExit("control missed intended assertion: "+name+"\n"+out)
        print("CONTROL",name,"caught:",expect)
    elif q.returncode:raise SystemExit(out)
    else:print(out,end="")
none=s.replace('mutex_lock(&power_lock);','(void)power_lock;').replace('mutex_unlock(&power_lock);','(void)power_lock;')
run("power-no-owner",none,"sync",["-DPOWER_CTL_RACE"],"FAIL: power requests have one transition owner")
leak=s.replace('vfs_drain_end(token);','(void)token;')
run("power-no-rollback",leak,"sync",expect="FAIL: failed power action restores VFS admission")
for mode in ("sync","begin","hardware"):run("power-"+mode+"-positive",s,mode)
