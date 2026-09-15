#!/usr/bin/env python3
"""Compile real BOT/class code; mutate production guards for observable controls."""
from pathlib import Path
import argparse,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True)
p.add_argument('--controls-only',action='store_true');p.add_argument('--positive-only',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
bot=(root/'c/drivers/usb/usb_bot.c').read_text();msc=(root/'c/drivers/usb/usb_storage.c').read_text()
variants=[('positive',None,None,'')]
controls=[
 ('tag','bot',' || le32(csw+4)!=tag','reject stale CSW tag'),
 ('signature','bot','le32(csw)!=0x53425355 || ','reject wrong CSW signature'),
 ('residue','bot',' || residue>len','reject residue greater than requested data'),
 ('csw-length','bot','n!=(int)sizeof csw || ','reject truncated CSW'),
 ('short-data','bot',' || relevant>(uint32_t)actual','reject fabricated full success after short data'),
 ('phase','bot','csw[12]>1 || ','phase error requires reset'),
 ('block-residue','msc',' ||\n            done!=chunk*BLK_SECTOR','block API rejects command-passed CSW with incomplete residue'),
 ('flush','msc','uint8_t cdb[10]={0x35};','block flush issues SYNCHRONIZE CACHE rather than success stub'),
]
if a.controls_only:variants=controls
elif not a.positive_only:variants+=controls
incs=['c/drivers/usb','c/drivers/block','c/drivers/core','c/kernel/core']
for name,kind,old,diagnostic in variants:
 d=out/name;d.mkdir(exist_ok=True);b=bot;m=msc
 if kind:
  txt=b if kind=='bot' else m
  assert txt.count(old)==1,(name,txt.count(old))
  txt=txt.replace(old,'uint8_t cdb[10]={0x00};' if name=='flush' else '')
  if kind=='bot':b=txt
  else:m=txt
 (d/'usb_bot.c').write_text(b);(d/'usb_msc_under_test.inc').write_text(m)
 suites=['usb_bot','usb_storage'] if not kind else (['usb_bot'] if kind=='bot' else ['usb_storage'])
 for suite in suites:
  cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-pthread',
       '-fsanitize=address,undefined','-I'+str(d)]
  cmd+=['-I'+str(root/i) for i in incs]
  exe=d/suite
  cmd +=[str(root/f'tests/unit/{suite}_test.c'),str(d/'usb_bot.c'),'-o',str(exe)]
  subprocess.run(cmd,check=True)
  run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
  (d/f'{suite}.log').write_text(run.stdout+run.stderr)
  if not kind:
   assert run.returncode==0,(name,suite,run.stdout,run.stderr)
   print(run.stdout.strip(),flush=True)
  else:
   assert run.returncode==1 and 'FAIL '+diagnostic in run.stdout,(name,run.returncode,run.stdout,run.stderr)
   assert 'AddressSanitizer' not in run.stderr and 'runtime error:' not in run.stderr,(name,run.stderr)
   print(f'NEGATIVE CONTROL {name}: real guard removed; "FAIL {diagnostic}" observed',flush=True)
