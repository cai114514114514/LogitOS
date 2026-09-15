#!/usr/bin/env python3
"""Positive/negative apparatus fixtures; numbers alone cannot pass a NIC gate."""
import importlib.util
from pathlib import Path
p=Path(__file__).resolve().parents[1]/'boot/nic_http_result.py'
s=importlib.util.spec_from_file_location('nic_http_result',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
status='[http] get rc=0 status=2 t=0ms\n'
line='http bytes 32768 fnv1a 4213874117\n'
log='[mm] fork: cow=on, 1 forks, 0 bugs\n'
cases=[
 ('plain',status+line,True),
 ('kernel insert after prefix',status+'http bytes '+log+'32768 fnv1a 4213874117\n',True),
 ('kernel insert after complete length',status+'http bytes 32768'+log+' fnv1a 4213874117\n',True),
 ('kernel insert after label',status+'http bytes 32768 fnv1a '+log+'4213874117\n',True),
 ('kernel insert before final newline',status+'http bytes 32768 fnv1a 4213874117'+log+'\n',True),
 ('wrong hash',status+line.replace('4213874117','4213874116'),False),
 ('wrong length',status+line.replace('32768','32767'),False),
 ('split partial number',status+'http bytes 32'+log+'768 fnv1a 4213874117\n',False),
 ('split partial hash',status+'http bytes 32768 fnv1a 4213\n874117\n',False),
 ('arbitrary inserted line',status+'http bytes unrelated\n32768 fnv1a 4213874117\n',False),
 ('unknown bracket line',status+'http bytes [anything] x\n32768 fnv1a 4213874117\n',False),
 ('missing body',status,False),
 ('missing status',line,False),
 ('bad HTTP rc',status.replace('rc=0','rc=-1')+line,False),
 ('body predates successful status',line+status,False),
 ('missing final newline',status+line.rstrip(),False),
 ('echoed command',status+'echo '+line,False),
 ('ambiguous duplicate',status+line+line,False),
]
failed=[]
for name,text,want in cases:
 try:m.parse_result(text);got=True
 except ValueError:got=False
 if got!=want:failed.append(name);print('FAIL:',name)
print(f'nic-http-parser: {len(cases)} fixtures, {len(failed)} failed; wrong-hash/split-partial/missing negatives exercised')
raise SystemExit(bool(failed))
