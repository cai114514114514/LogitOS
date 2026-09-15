#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run deliberate production-source mutations before the positive GUI gate."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-gui");a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];build=pathlib.Path(a.build);build.mkdir(parents=True,exist_ok=True)
gui=root/"c/kernel/gui"
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-DIME_LEARN_HOST"]
for d in ("c/kernel/gui","c/kernel/core","c/kernel/init","c/kernel/diag","c/kernel/sync","c/kernel/init","c/kernel/diag","c/kernel/sync","c/kernel/exec","c/kernel/exec/load","c/kernel/exec/signal","c/kernel/exec/fd","c/kernel/exec/load","c/kernel/exec/signal","c/kernel/exec/fd","c/kernel/mm","c/kernel/mm/phys","c/kernel/mm/virt","c/kernel/mm/cache","c/kernel/mm/reclaim","c/kernel/mm/phys","c/kernel/mm/virt","c/kernel/mm/cache","c/kernel/mm/reclaim","c/kernel/cpu","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/lib/ime","include","include/abi"):
    base+=["-I"+str(root/d)]
fixture=root/"tests/unit/bkl_gui_test.c"
def run(name,defs=(),ev=None,learn=None,expect=None):
    binary=build/name
    cmd=base+list(defs)+[str(fixture),str(gui/"clipboard.c"),str(ev or gui/"evq.c"),str(learn or gui/"ime_learn.c"),"-o",str(binary)]
    subprocess.run(cmd,check=True,cwd=root)
    r=subprocess.run([str(binary)],cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
    (build/(name+".log")).write_text(r.stdout)
    if expect:
        if r.returncode==0 or expect not in r.stdout:raise SystemExit("negative control did not fail at its intended assertion: "+name+"\n"+r.stdout)
        print("CONTROL",name,"caught:",expect)
    elif r.returncode:raise SystemExit(r.stdout)
    else:print(r.stdout,end="")
run("clip-no-pin",["-DGUI_CTL_CLIP_NO_PIN"],expect="heap-use-after-free")
ev=(gui/"evq.c").read_text()
ev=ev.replace('#include "evq.h"','#include "evq.h"\nvoid bkl_evq_pause(void);')
ev=ev.replace("    int nt = (q->tail + 1) % EVQ_N;","    int nt = (q->tail + 1) % EVQ_N;\n    bkl_evq_pause();")
ev="\n".join(line for line in ev.splitlines() if "gui_spin_lock(" not in line and "gui_spin_unlock(" not in line)+"\n"
ep=build/"evq-no-lock.c";ep.write_text(ev)
run("event-no-lock",ev=ep,expect="FAIL: event producers retain both unique events")
learn=(gui/"ime_learn.c").read_text()
learn=learn.replace('#include "ime_learn.h"','#include "ime_learn.h"\nvoid bkl_learn_pause(void);')
learn=learn.replace("\tg_commits += add;","\tuint32_t old_commits = g_commits;\n\tbkl_learn_pause();\n\tg_commits = old_commits + add;")
learn="\n".join(line for line in learn.splitlines() if "gui_spin_lock(" not in line and "gui_spin_unlock(" not in line)+"\n"
lp=build/"learn-no-lock.c";lp.write_text(learn)
run("learning-no-lock",learn=lp,expect="FAIL: learning preserves both commits")
run("gui-positive")

# Reuse the existing RAM-VFS apparatus, adding a source-stability observation at
# the actual vfs_write boundary. Merely checking final preferences would miss a
# concurrent serializer replacing the bytes while the first write sleeps.
old=(root/"tests/unit/settings_test.c").read_text()
old=old.replace("int main(int argc, char **argv)","int settings_original_main(int argc, char **argv)")
old=old.replace("int vfs_write(const char *p, const void *b, int size)\n{",
    "void bkl_settings_io_hold(const void *, int);\nint vfs_write(const char *p, const void *b, int size)\n{\n    bkl_settings_io_hold(b, size);")
old+=r"""
#include <pthread.h>
#include <sched.h>
static unsigned io_enabled, io_arrivals, io_changed;
void bkl_settings_io_hold(const void *bytes, int n) {
    if (!__atomic_load_n(&io_enabled,__ATOMIC_ACQUIRE)) return;
    char *saved=malloc(n?n:1); memcpy(saved,bytes,n);
    __atomic_add_fetch(&io_arrivals,1,__ATOMIC_SEQ_CST);
#ifdef BKL_SETTINGS_RACE
    while(__atomic_load_n(&io_arrivals,__ATOMIC_SEQ_CST)<2) sched_yield();
#else
    for(int i=0;i<1000;i++)sched_yield();
#endif
    if(memcmp(saved,bytes,n))__atomic_add_fetch(&io_changed,1,__ATOMIC_RELAXED);
    free(saved);
}
static void *parallel_setting(void *p) {
    const char *key=p;
    int rc=settings_set_str(key,key,1);
    return (void *)(intptr_t)rc;
}
int main(void) {
    setbuf(stdout,NULL);
    settings_load();
    __atomic_store_n(&io_enabled,1,__ATOMIC_RELEASE);
    pthread_t a,b;pthread_create(&a,0,parallel_setting,"app.one");
    while(__atomic_load_n(&io_arrivals,__ATOMIC_SEQ_CST)<1)sched_yield();
    pthread_create(&b,0,parallel_setting,"app.two");
    void *ra,*rb;pthread_join(a,&ra);pthread_join(b,&rb);
    int bad=ra!=0||rb!=0||io_changed!=0;
    if(io_changed)puts("FAIL: settings serializer changed during vfs_write");
    if(ra||rb)puts("FAIL: concurrent settings write failed");
    printf("BKL_SETTINGS source_changes=%u failures=%d\n",io_changed,bad);
    return bad?1:0;
}
"""
fixture_settings=build/"settings-fixture.c";fixture_settings.write_text(old)
settings=(root/"c/kernel/core/settings.c").read_text()
# Copied production sources resolve their original relative includes explicitly.
settings=settings.replace('#include "../gui/gui_sync.h"', '#include "'+str(gui/"gui_sync.h")+'"')
settings=settings.replace('#include "../../../include/weaksym.h"', '#include "'+str(root/"include/weaksym.h")+'"')
settings=settings.replace('#include "../../apps/coreutils/accounts.h"', '#include "'+str(root/"c/apps/coreutils/accounts.h")+'"')
for mutant in (True,False):
    source=build/("settings-no-lock.c" if mutant else "settings-locked.c")
    code=settings
    if mutant:
        code=code.replace("gui_mutex_lock(&settings_lock);","(void)settings_lock;").replace("gui_mutex_unlock(&settings_lock);","(void)settings_lock;")
    source.write_text(code)
    exe=build/("settings-no-lock" if mutant else "settings-positive")
    cmd=base+["-Ic/fs -Ic/fs/vfs -Ic/fs/logitfs -Ic/fs/cache -Ic/fs/ramfs -Ic/fs/ctl -Ic/fs/procfs","-Ic/drivers/block","-Wno-deprecated-declarations"]
    if mutant:cmd+=["-DBKL_SETTINGS_RACE"]
    subprocess.run(cmd+[str(fixture_settings),str(source),"c/drivers/block/crc32.c","-o",str(exe)],check=True,cwd=root)
    proc=subprocess.run([str(exe)],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=30)
    (build/(exe.name+".log")).write_text(proc.stdout)
    if mutant:
        if proc.returncode==0 or "FAIL: settings serializer changed during vfs_write" not in proc.stdout:
            raise SystemExit("settings control did not catch shared staging\n"+proc.stdout)
        print("CONTROL settings-no-lock caught: serializer changed during vfs_write")
    elif proc.returncode:raise SystemExit(proc.stdout)
    else:print(proc.stdout,end="")
