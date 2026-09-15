# SPDX-License-Identifier: MIT
"""Real MM/allocator locks under pthreads; mutation controls precede acceptance."""
import argparse, json, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--build',required=True);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2]
if not (root/'c/kernel/mm').is_dir(): root=pathlib.Path('/Users/wangzhe/system/LogitOS')
out=pathlib.Path(a.build).resolve();out.mkdir(parents=True,exist_ok=True)
src=['tests/unit/bkl_mm_test.c','tests/unit/mm_common.c']
src += ['c/kernel/mm/'+s+'.c' for s in ['pmm','vmm','fault','vma','rmap','reclaim','swap','pcache','shm','oom']]
src+=['c/kernel/cpu/spinlock.c','tests/unit/mmstub/mm_hoststub.c']
flags=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DMM_HOSTTEST','-DMM_CONCURRENT','-DLOGIT_LOCK_HOST','-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread','-Itests/unit','-Itests/unit/mmstub','-Ic/kernel/mm -Ic/kernel/mm/phys -Ic/kernel/mm/virt -Ic/kernel/mm/cache -Ic/kernel/mm/reclaim']
cases=[('no-as-lock','MM_NO_AS_LOCK','guarded physical usercopy has no lost updates'),('global-as-lock','MM_GLOBAL_AS_LOCK','independent address spaces execute concurrently'),('cache-borrow','PCACHE_NO_RETURN_REF','cache invalidation preserves caller reference'),('stale-fill','PCACHE_STALE_FILL','in-flight old read cannot refill invalidated cache'),('positive',None,None)]
report=[]
for name,macro,expected in cases:
 exe=out/name
 cmd=flags+(['-D'+macro] if macro else [])+src+['-o',str(exe)]
 b=subprocess.run(cmd,cwd=root,text=True,capture_output=True)
 (out/(name+'-build.log')).write_text(b.stdout+b.stderr)
 if b.returncode: raise SystemExit('compile failed: '+str(out/(name+'-build.log')))
 r=subprocess.run([str(exe)],cwd=root,text=True,capture_output=True,timeout=60)
 text=r.stdout+r.stderr;(out/(name+'.log')).write_text(text)
 ok=(r.returncode==0 if not macro else r.returncode!=0 and 'FAIL: '+expected in text)
 report.append(dict(case=name,exit=r.returncode,passed=ok,log=str(out/(name+'.log'))))
 print(name+': '+('PASS' if ok else 'FAIL'),flush=True)
 if not ok:
  print(text);raise SystemExit(1)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
