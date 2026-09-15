#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual registry/device callback concurrency with prerequisite controls."""
import argparse,os,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument("--build",default="/tmp/logitos-bkl-device");p.add_argument("--source-dir");a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];b=pathlib.Path(a.build);b.mkdir(parents=True,exist_ok=True)
src=pathlib.Path(a.source_dir) if a.source_dir else r/"c/drivers/core"
(b/"driver.h").write_text((src/"driver.h").read_text())
s=(src/"device.c").read_text()
base=[os.environ.get("CC","clang"),"-std=c11","-O1","-g","-pthread","-fsanitize=address,undefined","-DLOGIT_HOST_TEST","-I"+str(b),"-I"+str(r/"c/drivers/core"),"-I"+str(r/"c/kernel/pci"),"-I"+str(r/"tests/unit/pcistub")]
def run(name,code,mode,expect=None):
    source=b/(name+".c");source.write_text(code);exe=b/name
    subprocess.run(base+[str(r/"tests/unit/bkl_device_test.c"),str(source),"-o",str(exe)],check=True,cwd=r)
    q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if expect:
        if q.returncode==0 or expect not in out:raise SystemExit("control missed intended assertion: "+name+"\n"+out)
        print("CONTROL",name,"caught:",expect)
    elif q.returncode:raise SystemExit(out)
    else:print(out,end="")
no_registry=s.replace('#include "driver.h"','#include "driver.h"\nvoid bkl_add_pause(void);').replace('    int index = dev_count();','    int index = dev_count();\n    bkl_add_pause();').replace('IO_GUARD(&registry_lock);','(void)registry_lock;')
run("device-no-registry",no_registry,"add","FAIL: device publication reserves distinct complete slots")
start=s.index('    unsigned idle = 0;',s.index('static int dev_binding_try'));end=s.index('\n}',start)
no_owner=s[:start]+'    (void)d; return 1;'+s[end:]
run("device-no-owner",no_owner,"probe","FAIL: same-device probe has one owner while other devices progress")
run("device-publish-positive",s,"add")
run("device-owner-positive",s,"probe")

# The NIC priority pass precedes the scheduler. Re-entry after that point must
# not bypass per-device runtime ownership; exercise the real netdev.c fixture.
net=(r/"c/drivers/net/netdev.c").read_text()
fixture=b/"netif-fixture.c";fixture.write_text((r/"tests/unit/netif_test.c").read_text())
start=net.index('int netdev_init(void)\n{');end=net.index('\nint netdev_present(void)',start)
for mutant in (True,False):
    code=net[:start]+'int netdev_init(void) { return netdev_init_boot(); }\n'+net[end:] if mutant else net
    (b/"netdev.c").write_text(code)
    name="netdev-repeat-init" if mutant else "netdev-init-positive";exe=b/name
    subprocess.run(base+["-I"+str(r/"c/drivers/net"),"-I"+str(r/"c/net/core"),str(fixture),"-o",str(exe)],check=True,cwd=r)
    q=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);out=q.stdout+q.stderr;(b/(name+".log")).write_text(out)
    if mutant:
        if q.returncode==0 or "FAIL late init cannot repeat the raw boot probe or duplicate interfaces" not in out:
            raise SystemExit("NIC re-entry control missed intended assertion\n"+out)
        print("CONTROL netdev-repeat-init caught: late boot-probe re-entry")
    elif q.returncode:raise SystemExit(out)
    else:print("BKL_NETDEV: existing 44-check interface gate passed with repeat-init guard")
