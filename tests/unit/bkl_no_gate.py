#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural gate complements the real four-CPU guest rendezvous."""
import argparse,json,re,struct
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--kernel',type=Path,required=True);p.add_argument('--baseline-kernel',type=Path);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
def symbols(path):
 b=path.read_bytes()
 assert b[:6]==b'\x7fELF\x02\x01','expected ELF64 little endian kernel'
 off=struct.unpack_from('<Q',b,40)[0];size,n=struct.unpack_from('<HH',b,58)
 sec=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*size) for i in range(n)];names=[]
 for s in sec:
  if s[1]!=2:continue
  strings=sec[s[6]];table=b[strings[4]:strings[4]+strings[5]]
  for at in range(s[4],s[4]+s[5],s[9]):
   start=struct.unpack_from('<I',b,at)[0];names.append(table[start:table.find(b'\0',start)].decode('utf8','replace'))
 assert names,'kernel must retain symbol table for gate'
 return names
def source_gate(s):
 s=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', ' ',s,flags=re.S)
 assert not re.search(r'\b(?:g_bkl\w*|syscall_is_bkl_free|bkl_hlt_wait)\b',s),'live BKL symbol or entry allow-list'
def kernel_gate(path):
 names=symbols(path)
 assert not any(re.search(r'(^|_)(?:g_bkl|syscall_is_bkl_free|bkl_hlt_wait)',x) for x in names),'compiled BKL symbol'
 assert 'bkl_verify_entry' not in names,'ordinary build must omit verification selector'
 return len(names)
paths=[*root.glob('c/kernel/**/*.c'),*root.glob('c/kernel/**/*.h'),*root.glob('c/drivers/**/*.c')]
source='\n'.join(x.read_text() for x in paths)
try:source_gate(source+'\nspinlock_t g_bkl;\n')
except AssertionError:pass
else:raise SystemExit('FAIL: structural negative control was accepted')
controls=['live-BKL-symbol']
if a.baseline_kernel:
 try:kernel_gate(a.baseline_kernel)
 except AssertionError as e:
  assert str(e)=='compiled BKL symbol',str(e);controls.append('actual-before-kernel')
 else:raise SystemExit('FAIL: before kernel did not contain expected BKL')
source_gate(source);count=kernel_gate(a.kernel)
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(dict(passed=True,controls=controls,source_files=len(paths),kernel_symbols=count),indent=2)+'\n');print('PASS BKL source/ordinary ELF gate; controls:',','.join(controls))
