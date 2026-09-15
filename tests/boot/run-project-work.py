#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Directory = Project: native Finder/TextEdit, rename, reboot, stale identity.

Owns a new v5 image, never rewrites the source v4 image. The model is explicitly
simulated unless --gateway is supplied. Host negative controls precede this
runner in test-project; UI actions are ordinary QMP input, never synthetic RPC.
"""
import argparse,hashlib,importlib.util,json,re,time
from pathlib import Path
import signal
def interrupted(signum,frame):raise KeyboardInterrupt
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
import sys
sys.path.insert(0,str(ROOT/'tools'))
from project_volume import convert
gate=module('project_native',ROOT/'tests/boot/run-textedit-work.py');runtime=gate.runtime

def move(g,x,y):
 while max(abs(x-g.pointer[0]),abs(y-g.pointer[1]))>120:
  dx=max(-120,min(120,x-g.pointer[0]));dy=max(-120,min(120,y-g.pointer[1]))
  g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
  g.pointer[0]+=dx;g.pointer[1]+=dy;time.sleep(.16)
 g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':x-g.pointer[0]}},{'type':'rel','data':{'axis':'y','value':y-g.pointer[1]}}]});g.pointer=[x,y];time.sleep(.2)
def button(g,down,name='left'):
 g.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':name,'down':down}}]});time.sleep(.15)
def frame(g,title):
 # Finder can be created before the WM's initial geometry snapshot. Move its
 # actual logged animation frame to cause the ordinary settled-geometry report.
 if title=='Finder' and not re.search(rb'\[wm\] win .* pt zoom 0 min 0 Finder',bytes(g.log)):
  g.wait(rb'\[wm\] anim open win 0 .* home (\d+) (\d+) (\d+) (\d+)',30)
  m=re.findall(rb'\[wm\] anim open win 0 .* home (\d+) (\d+) (\d+) (\d+)',bytes(g.log))[-1]
  x,y,w,h=map(int,m);time.sleep(1);move(g,x+w//2,y+14);button(g,True);move(g,x+w//2+8,y+18);button(g,False);time.sleep(1)
 return gate.frame(g,title)
def main():
 signal.signal(signal.SIGTERM,interrupted)
 p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 p.add_argument('--gateway',type=Path);p.add_argument('--mode',choices=('bios','uefi'),default='bios');p.add_argument('--ram',default='512M');p.add_argument('--no-ui',action='store_true');a=p.parse_args()
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);out.chmod(0o700);b=a.build.resolve();g=model=None;checks=[]
 def check(name,value):
  if not value:raise AssertionError(name)
  checks.append(name);print('PROJECT_WORK_PASS',name,flush=True)
 def command(text):
  value=g.capture(text);check('command '+text.split()[0],g.last_capture_exit==0);return value
 def click(px,py):
  x,y=pos(px,py);move(g,x,y);button(g,True);button(g,False)
 def identity(path):
  value=command('/bin/agentctl ref '+path);m=re.search(r'volume=([a-f0-9]+) object=(\d+)',value);check('persistent identity '+path,bool(m));return m.groups()
 try:
  if a.gateway:gateway=a.gateway.resolve()
  else:model=gate.fault.FaultModel(out/'model');gateway=out/'model'
  source=runtime.prepare(b,gateway,out,extras=[f'{b}/{n}.aex:/bin/{n}' for n in ('mv','mkdir','rm','cp')])
  digest=hashlib.sha256(source.read_bytes()).digest();disk=out/'project.img';metadata=convert(source,disk,b/'lfs_snapshot')
  check('conversion preserves source image',hashlib.sha256(source.read_bytes()).digest()==digest)
  g=runtime.Guest(b,disk,out,a.mode,a.ram);g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
  if a.no_ui:command('/bin/mkdir /docs/Community')
  else:
   pos,W,H=frame(g,'Finder');click(750,23);click(295,24);g.type('Community');g.key('ret');time.sleep(1)
  project=identity('/docs/Community');check('empty directory is already a Project','tasks=0' in command('/bin/agentctl project /docs/Community'))
  command('/bin/cp /docs/source.md /docs/Community/source.md')
  if a.no_ui:check('Finder creates task','TASK_CREATED id=1' in command('/bin/agentctl create "Summarize the source with exact citations" /docs/Community /docs/Community/source.md'))
  else:
   # Source.md precedes the newly created directory in the actual directory list.
   move(g,*pos(310,90));button(g,True,'right');button(g,False,'right');click(332,102)
   g.wait(rb'PROJECT_VIEW path=/docs/Community tasks=0',30);time.sleep(.3)
   click(940,128);g.type('Write a concise report with exact source citations.');g.key('ret');time.sleep(1)
  g.complete(1);initial=command('/bin/agentctl document 1');check('report contains source facts and citations','4200' in initial.replace(',','') and '[S1:' in initial)
  doc=identity('/docs/Community/report-1.md');check('Project owns the created task','PROJECT_TASK id=1' in command('/bin/agentctl project /docs/Community'))
  check('disk bytes match report','DOCUMENT_VERIFIED' in command('/bin/agentctl verify 1'))
  if not a.no_ui:
   g.wait(rb'PROJECT_VIEW path=/docs/Community tasks=1 first=1 phase=6',30);time.sleep(.5)
   gate.screenshot(g,'finder-report');click(952,338);time.sleep(1)
   pos,W,H=frame(g,'/docs/Community/report-1.md');time.sleep(2)
   check('Finder opens TextEdit with the same task',gate.work(g)[5]==1)
   click(100,170);g.key('ctrl','end');g.type('\nhuman note keep this sentence\n');time.sleep(2)
   before=command('/bin/agentctl document 1');check('unsaved TextEdit edit is in task state','human note keep this sentence' in before)
   sw=min(360,max(290,W*31//100));dw=W-sw;x=dw+24;rw=sw-48
   click(x+80,H-49);g.type('Suggest next steps and preserve the exact sentence human note keep this sentence.');g.key('ret');time.sleep(1)
   g.complete(1,allow_conflict=True);v=gate.work(g);candidate=command('/bin/agentctl candidate 1')
   check('review retains user document',v[2]>0 and command('/bin/agentctl document 1')==before)
   gate.screenshot(g,'review-before-move');click(18,-15);time.sleep(.3)
  else:before=initial;v=candidate=None
  command('/bin/mv /docs/Community /docs/Library')
  check('Project identity survives rename',identity('/docs/Library')==project)
  check('file identity survives parent rename',identity('/docs/Library/report-1.md')==doc)
  check('renamed Project has same task','PROJECT_TASK id=1 artifact=/docs/Library/report-1.md' in command('/bin/agentctl project /docs/Library'))
  check('TextEdit lookup follows identity','DOCUMENT_TASK task=1' in command('/bin/agentctl lookup /docs/Library/report-1.md'))
  command('/bin/mkdir /docs/Archive')
  command('/bin/mv /docs/Library /docs/Archive/Library')
  check('Project identity survives reparenting',identity('/docs/Archive/Library')==project)
  check('reparented Project keeps the task','PROJECT_TASK id=1' in command('/bin/agentctl project /docs/Archive/Library'))
  g.close();g=None;reboot=out/'reboot';reboot.mkdir();g=runtime.Guest(b,disk,reboot,a.mode,a.ram);g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
  check('Project identity survives reboot',identity('/docs/Archive/Library')==project)
  check('task binding survives reboot','DOCUMENT_TASK task=1' in command('/bin/agentctl lookup /docs/Archive/Library/report-1.md'))
  check('confirmed document survives reboot',command('/bin/agentctl document 1')==before)
  if not a.no_ui:
   check('pending review survives rename and reboot',gate.work(g)==v and command('/bin/agentctl candidate 1')==candidate)
   command('/bin/agent-runtime-test open-path /docs/Archive/Library/report-1.md');pos,W,H=frame(g,'/docs/Archive/Library/report-1.md');time.sleep(2)
   click(x+rw//2,H-127);time.sleep(2)
   check('native Apply commits same proposal',command('/bin/agentctl document 1')==candidate and gate.work(g)[2]==0)
   check('accepted report stays inside renamed Project','artifact=/docs/Archive/Library/' in command('/bin/agentctl verify 1'))
   click(323,22);time.sleep(1);check('native Save keeps exact bytes','DOCUMENT_VERIFIED' in command('/bin/agentctl verify 1'))
   gate.screenshot(g,'applied-in-renamed-project')
  command('/bin/cp /docs/source.md /docs/Archive/Library/report-1.md')
  check('external edit retains the file identity',identity('/docs/Archive/Library/report-1.md')==doc)
  refused=g.capture('/bin/agentctl lookup /docs/Archive/Library/report-1.md');check('external content revision cannot restore an older checkpoint',g.last_capture_exit!=0 and 'DOCUMENT_TASK' not in refused)
  command('/bin/rm /docs/Archive/Library/report-1.md');command('/bin/cp /docs/source.md /docs/Archive/Library/report-1.md')
  check('replacement receives new identity',identity('/docs/Archive/Library/report-1.md')!=doc)
  denied=g.capture('/bin/agentctl lookup /docs/Archive/Library/report-1.md');check('same pathname cannot take over old task',g.last_capture_exit!=0 and 'DOCUMENT_TASK' not in denied)
  check('original source is unchanged',command('/bin/cat /docs/source.md')==(ROOT/'tests/fixtures/agent/source.md').read_text())
  (out/'result.json').write_text(json.dumps({'passed':True,'mode':a.mode,'ram':a.ram,'native_ui':not a.no_ui,'real_model':bool(a.gateway),'volume':metadata,'checks':checks},ensure_ascii=False,indent=2))
 except Exception:
  if g:
   try:gate.screenshot(g,'failure')
   except Exception:pass
   # Preserve interrupt state before teardown. A live shell can coexist with
   # a stopped PIT clock; then model and IPC deadlines never expire. Do not
   # dump memory here: real-provider guests contain ephemeral credentials.
   for name,query in (('registers','info registers -a'),('pic','info pic'),('irq','info irq')):
    try:(g.out/('failure-'+name+'.txt')).write_text(g.qmp('human-monitor-command',{'command-line':query}))
    except Exception:pass
  raise
 finally:
  if g:g.close()
  if model:model.close()
if __name__=='__main__':main()
