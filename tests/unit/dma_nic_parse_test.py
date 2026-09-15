#!/usr/bin/env python3
"""Positive/negative apparatus fixtures; numbers alone cannot pass a NIC gate."""
import importlib.util
from pathlib import Path
p=Path(__file__).resolve().parents[1]/'boot/nic_http_result.py'
s=importlib.util.spec_from_file_location('nic_http_result',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
line='http bytes 32768 fnv1a 4213874117\n'
sha='sha256 611253a4531dea3d840789b4f11a1ad9c4329fbbf85ee1634f2ae601e6da6db0\n'
log='[mm] fork: cow=on, 1 forks, 0 bugs\n'
cases=[
 ('plain',line+sha,True),
 ('kernel insert after prefix','http bytes '+log+'32768 fnv1a 4213874117\n'+sha,True),
 ('kernel insert after complete length','http bytes 32768'+log+' fnv1a 4213874117\n'+sha,True),
 ('kernel insert after label','http bytes 32768 fnv1a '+log+'4213874117\n'+sha,True),
 ('kernel insert before final newline','http bytes 32768 fnv1a 4213874117'+log+'\n'+sha,True),
 ('wrong fnv',line.replace('4213874117','4213874116')+sha,False),
 ('wrong length',line.replace('32768','32767')+sha,False),
 ('wrong sha',line+sha.replace('611253a4','011253a4'),False),
 ('missing sha',line,False),
 ('sha predates result',sha+line,False),
 ('split partial number','http bytes 32'+log+'768 fnv1a 4213874117\n'+sha,False),
 ('split partial fnv','http bytes 32768 fnv1a 4213\n874117\n'+sha,False),
 ('arbitrary inserted line','http bytes unrelated\n32768 fnv1a 4213874117\n'+sha,False),
 ('unknown bracket line','http bytes [anything] x\n32768 fnv1a 4213874117\n'+sha,False),
 ('missing body',sha,False),
 ('missing result newline',line.rstrip()+sha,False),
 ('echoed command','echo '+line+sha,False),
 ('ambiguous duplicate',line+sha+line+sha,False),
 ('ambiguous sha',line+sha+sha,False),
]
failed=[]
for name,text,want in cases:
 try:m.parse_result(text);got=True
 except ValueError:got=False
 if got!=want:failed.append(name);print('FAIL:',name)
print(f'nic-http-parser: {len(cases)} fixtures, {len(failed)} failed; wrong-digest/split-partial/missing negatives exercised')
raise SystemExit(bool(failed))
