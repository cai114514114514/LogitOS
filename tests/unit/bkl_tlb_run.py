#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production TLB transactions and ticket locks; named negative controls run first."""
import argparse,json,pathlib,subprocess
p=argparse.ArgumentParser();p.add_argument('--build',default='/tmp/logitos-bkl-20260910/proc/tlb');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];out=pathlib.Path(a.build).resolve();out.mkdir(parents=True,exist_ok=True)
flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DLOGIT_LOCK_HOST','-DLOGIT_TLB_HOST','-Ic/kernel/cpu']
source=root/'c/kernel/cpu/tlb.c';control=out/'tlb-timeout-return.c';text=source.read_text();needle='tlb_failstop(me,ack,others,n);'
if text.count(needle)!=1:raise SystemExit('timeout control no longer matches the production stop call')
control.write_text(text.replace(needle,'(void)tlb_failstop; return;'))
cases=[('no-send-lock',source,['-DTLB_NO_SEND_LOCK'],[],'simultaneous shootdown publishers never overlap shared ACK ownership'),('timeout-return',control,['-DTLB_WAIT_SPINS=128'],['timeout'],'timeout retains page ownership until fail-stop'),('concurrent',source,[],[],None),('timeout-stop',source,['-DTLB_WAIT_SPINS=128'],['timeout'],None)]
report=[]
for name,src,defs,args,expected in cases:
 exe=out/name;b=subprocess.run(flags+defs+['tests/unit/bkl_tlb_test.c',str(src),'c/kernel/cpu/spinlock.c','-o',str(exe)],cwd=root,text=True,capture_output=True)
 (out/(name+'-build.log')).write_text(b.stdout+b.stderr)
 if b.returncode:raise SystemExit('compile failed: '+str(out/(name+'-build.log')))
 r=subprocess.run([str(exe)]+args,cwd=root,text=True,capture_output=True,timeout=30);text=r.stdout+r.stderr;(out/(name+'.log')).write_text(text);print(text,end='',flush=True)
 ok=(r.returncode==1 and 'FAIL '+expected in text) if expected else r.returncode==0
 report.append(dict(case=name,passed=ok,exit=r.returncode,log=str(out/(name+'.log'))))
 if not ok:raise SystemExit('TLB gate failed: '+name)
 print(name+': PASS'+(' (named negative assertion caught)' if expected else ''),flush=True)
(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
