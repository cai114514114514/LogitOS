#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the actual run-project launcher and reopen its persistent volume."""
import argparse,json,importlib.util,sys
from pathlib import Path
import signal
def interrupted(signum,frame):raise KeyboardInterrupt
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('project_launcher',ROOT/'tests/boot/run-project-work.py');gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--env',type=Path,required=True);a=p.parse_args();b=a.build.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);disk=out/'project.img';g=None
launcher=[sys.executable,str(ROOT/'tools/project_session.py'),'--source',str(a.source.resolve()),'--disk',str(disk),'--build',str(b),'--env',str(a.env.resolve()),'--limit','1','--']
signal.signal(signal.SIGTERM,interrupted)
try:
 for run in range(2):
  target=out/('boot'+str(run));target.mkdir();g=gate.runtime.Guest(b,disk,target,'bios','512M',launcher=launcher);g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
  if not run:g.capture('/bin/mkdir /docs/Launcher');assert g.last_capture_exit==0
  text=g.capture('/bin/agentctl ref /docs/Launcher');assert g.last_capture_exit==0 and 'FILE_REF' in text
  if not run:
   identity=text;pos,_,_=gate.frame(g,'Finder');gate.move(g,*pos(790,90))
   gate.button(g,True,'right');gate.button(g,False,'right');gate.move(g,*pos(700,102));gate.button(g,True);gate.button(g,False)
   g.wait(rb'PROJECT_VIEW path=/docs/Launcher tasks=0',30)
   gate.move(g,*pos(190,23));gate.button(g,True);gate.button(g,False)
   g.wait(rb'PROJECT_VIEW path=/docs tasks=0',30)
   gate.move(g,*pos(310,90))
   for _ in range(2):gate.button(g,True);gate.button(g,False)
   g.wait(rb'PROJECT_VIEW path=/docs/Launcher tasks=0',30)
  else:assert text==identity,'launcher update preserves Project identity and revision'
  assert 'PROJECT object=' in g.capture('/bin/agentctl project /docs/Launcher')
  g.close();g=None
 (out/'result.json').write_text(json.dumps({'passed':True,'provider_calls':0,'checks':['actual Project launcher boot','persistent Project creation','native edge context menu enters Project','native double-click enters Project','second launch preserves identity and revision']},indent=2));print('PROJECT_SESSION_PASS launcher, repeat update and persistent directory identity',flush=True)
finally:
 if g:g.close()
