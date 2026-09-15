#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Render a native review from a previously completed real-model task disk.

The caller supplies the running host gateway; this requests two more model
responses for task 1, verifies retained human text, and leaves its candidate
unapplied on a private copy. No provider credential enters this script.
"""
import sys,importlib.util,json,shutil,time,re,argparse
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
for key in ('build','source-disk','gateway','out'):p.add_argument('--'+key,type=Path,required=True)
p.add_argument('--existing-proposal',action='store_true',help='render a saved proposal without requesting new inference')
a=p.parse_args();build=a.build.resolve();out=a.out.resolve()
sys.path.insert(0,str(root/'tools'));import agent_session as session
spec=importlib.util.spec_from_file_location('work',root/'tests/boot/run-textedit-work.py');w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
out.mkdir(parents=True,exist_ok=False);disk=out/'disk.img';shutil.copyfile(a.source_disk.resolve(),disk)
replacements={path:((build/name).read_bytes(),0o755) for path,name in [('/textedit.aex','textedit.aex'),('/bin/agentd','agentd.aex'),('/bin/agentctl','agentctl.aex'),('/assistant.aex','assistant.aex')]}
for name in ('agent.conf','agent.key'):replacements['/etc/'+name]=((a.gateway/name).read_bytes(),0o600)
with session.image_guard(disk):session.install(disk,build/'lfs_snapshot',replacements)
g=None
try:
 g=w.runtime.Guest(build,disk,out,'bios','2G');g.wait(b'AGENTD_READY',180)
 before=g.capture('/bin/agentctl document 1');assert 'human note keep this sentence' in before
 if not a.existing_proposal:g.capture('/bin/agentctl revise 1 "补充下一步安排，保留我的批注。请逐字保留当前正文，只在末尾追加一个标题为后续安排的小节，包含三条具体计划。保留 human note keep this sentence 原样。保持来源引用真实，不宣称计划已完成。"')
 if not a.existing_proposal:assert g.last_capture_exit==0, 'task was not ready for a new instruction'
 g.complete(1,allow_conflict=True);v=w.work(g);candidate=g.capture('/bin/agentctl candidate 1');assert candidate and v[2]>0 and g.capture('/bin/agentctl document 1')==before
 assert 'human note keep this sentence' in candidate
 g.capture('/bin/agent-runtime-test open-path /docs/report-1.md');pos,W,H=w.frame(g,'/docs/report-1.md');time.sleep(2)
 sw=min(360,max(290,W*31//100));dw=W-sw
 def move(x,y):
  tx,ty=pos(x,y)
  while (tx,ty)!=tuple(g.pointer):
   dx=max(-120,min(120,tx-g.pointer[0]));dy=max(-120,min(120,ty-g.pointer[1]))
   g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]});g.pointer[0]+=dx;g.pointer[1]+=dy;time.sleep(.1)
 move(dw-70,78);g.click(*pos(dw-70,78));time.sleep(.4);w.screenshot(g,'native-top')
 move(420,400)
 for _ in range(64):
  for down in (True,False):g.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'wheel-down','down':down}}]})
  time.sleep(.15)
 time.sleep(.5);w.screenshot(g,'native-review')
 (out/'result.json').write_text(json.dumps({'status':'passed','mode':'bios','ram':'2G','new_model_request':not a.existing_proposal,'checks':['final app and broker accept real provider result','human note retained in candidate','current document unchanged before decision','native candidate rendered'],'work':v},ensure_ascii=False,indent=2))
finally:
 if g:g.close()
