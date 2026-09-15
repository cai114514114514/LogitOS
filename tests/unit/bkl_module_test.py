#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Publication and duplicate-load controls precede the real-loader positive."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-module");a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];b=pathlib.Path(a.build);b.mkdir(parents=True,exist_ok=True)
(b/"wait.h").write_text("#include <pthread.h>\nstruct mutex {pthread_mutex_t m;};\n#define MUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}\nstatic inline void mutex_lock(struct mutex *m){pthread_mutex_lock(&m->m);}\nstatic inline void mutex_unlock(struct mutex *m){pthread_mutex_unlock(&m->m);}\n")
s=(r/"c/kernel/module/modload.c").read_text()
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-I"+str(b)]
for d in ("c/kernel/module","c/drivers/core","c/kernel/core","c/kernel/mm","c/kernel/exec","c/kernel/cpu","c/fs","include/abi"):
    base += ["-I"+str(r/d)]
def run(name,src,mode,defs=(),expect=None):
    source=b/(name+".c");source.write_text(src);exe=b/name
    subprocess.run(base+list(defs)+[str(r/"tests/unit/bkl_module_test.c"),str(source),"-o",str(exe)],check=True,cwd=r)
    q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if expect:
        if q.returncode==0 or expect not in out:raise SystemExit("control did not reach intended assertion: "+name+"\n"+out)
        print("CONTROL",name,"caught:",expect)
    elif q.returncode:raise SystemExit(out)
    else:print(out,end="")
# Count is made visible before the sleeping probe, recreating early publication.
early=s.replace('    int bound = dev_probe_all();','    __atomic_store_n(&g_nmod, slot + 1, __ATOMIC_RELEASE);\n    int bound = dev_probe_all();')
run("module-early-publish",early,"publication",expect="FAIL: query hides the module until probe completes")
none=s.replace('    mutex_lock(&g_load_lock);','    (void)g_load_lock;').replace('    mutex_unlock(&g_load_lock);','    (void)g_load_lock;')
run("module-no-owner",none,"duplicates",["-DMOD_DUP_RACE"],expect="FAIL: duplicate concurrent load is rejected")
run("module-publish-positive",s,"publication")
run("module-duplicate-positive",s,"duplicates")
