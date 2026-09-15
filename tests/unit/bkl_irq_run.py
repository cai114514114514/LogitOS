#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the real IRQ dispatch/retirement body with privileged leaves stubbed."""
from pathlib import Path
import argparse,os,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');a=ap.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
s=(r/'c/drivers/core/irq.c').read_text();s=s[s.index('struct irq_slot {'):].replace('__asm__ volatile ("cli");','/* host: no privileged CLI */')
if a.negative_only:
    needle='while (__atomic_load_n(&g_slot[i].active, __ATOMIC_ACQUIRE)) io_relax();'
    assert s.count(needle)==2
    s=s.replace(needle,'/* negative control: premature retirement */')
(b/'irq_body.c').write_text(s)
t=(r/'tests/unit/bkl_irq_test.c').read_text().replace('#include "irq_body.c"','#include "'+str(b/'irq_body.c')+'"');(b/'test.c').write_text(t)
exe=b/'test';subprocess.run([os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(b/'test.c'),'-o',str(exe)],check=True)
p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(b/'result.log').write_text(p.stdout+p.stderr)
if a.negative_only:assert p.returncode==1 and 'FAIL: retirement waits for an active callback' in p.stdout,(p.stdout,p.stderr)
else:assert p.returncode==0,(p.stdout,p.stderr)
print(('negative: ' if a.negative_only else 'positive: ')+p.stdout.strip().splitlines()[-1])
