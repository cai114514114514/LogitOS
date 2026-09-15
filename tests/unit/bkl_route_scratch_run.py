#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real route_sync interrupted-task scratch regression, with old-call control first."""
import argparse
from pathlib import Path
import os,subprocess
p=argparse.ArgumentParser()
p.add_argument('--build',required=True,type=Path)
p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--dependency-root',type=Path)
a=p.parse_args();r=a.source_root.resolve();dep=(a.dependency_root or r).resolve();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
incs=['c/net/core','c/net/ip','c/net/link','c/drivers/net','c/drivers/core','c/drivers/timer','c/kernel/core','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/pci']
for negative in [True,False]:
 d=b/('negative' if negative else 'positive');d.mkdir(exist_ok=True)
 route=(r/'c/net/core/route.c').read_text()
 # Select the actual kernel scratch branch without enabling privileged io_lock
 # leaves: this is the only hosted/platform substitution in the route body.
 route=route.replace('#if __STDC_HOSTED__','#if 0')
 route=route.replace('"../../drivers/core/io_lock.h"','"'+str(dep/'c/drivers/core/io_lock.h')+'"')
 ip=(r/'c/net/ip/ip.c').read_text().replace('"../../../include/weaksym.h"','"'+str(dep/'include/weaksym.h')+'"')
 if negative:
  old='        struct route_entry row;\n        if (!route_at_copy(i, &row)) continue;\n        const struct route_entry *e = &row;'
  assert old in ip
  ip=ip.replace(old,'        const struct route_entry *e = route_at(i);\n        if (!e) continue;')
 (d/'route.c').write_text(route);(d/'ip.c').write_text(ip)
 (d/'route.h').write_bytes((r/'c/net/core/route.h').read_bytes())
 exe=d/'test'
 cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DLOGIT_NET_HOST','-I'+str(d)]+['-I'+str(dep/i) for i in incs]
 c=subprocess.run(cmd+[str(r/'tests/unit/bkl_route_scratch_test.c'),'-o',str(exe)],capture_output=True,text=True,timeout=60)
 (d/'compile.log').write_text(c.stdout+c.stderr);assert c.returncode==0,c.stderr
 q=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
 log=q.stdout+q.stderr;(d/'run.log').write_text(log)
 if negative:
  assert q.returncode==1 and 'FAIL: IRQ route_sync preserves occupied syscall scratch' in log,log
  print('caught old route_at control: actual route_sync clobbers syscall scratch')
 else:
  assert q.returncode==0,log
  print(log.strip())
