#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Start work inside an existing TextEdit document and recover its binding."""
import argparse,importlib.util,json,re,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('work_gate',ROOT/'tests/boot/run-textedit-work.py');gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
build=a.build.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
model=gate.fault.FaultModel(out/'model');g=None;checks=[]
def check(name,condition):
    if not condition:raise AssertionError(name)
    checks.append(name);print('TEXTEDIT_DOCUMENT_PASS',name,flush=True)
try:
    disk=gate.runtime.prepare(build,out/'model',out);g=gate.runtime.Guest(build,disk,out,'bios','512M');g.wait(b'AGENTD_READY',180)
    original=g.capture('/bin/cat /docs/source.md');g.capture('/bin/agent-runtime-test open-path /docs/source.md');pos,W,H=gate.frame(g,'/docs/source.md');time.sleep(1)
    def click(px,py):
        tx,ty=pos(px,py)
        while max(abs(tx-g.pointer[0]),abs(ty-g.pointer[1]))>120:
            dx=max(-120,min(120,tx-g.pointer[0]));dy=max(-120,min(120,ty-g.pointer[1]))
            g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
            g.pointer[0]+=dx;g.pointer[1]+=dy;time.sleep(.15)
        g.click(tx,ty);time.sleep(.15)
    click(120,180);g.key('ctrl','end');g.type('\nunsaved note retained\n')
    sw=min(360,max(290,W*31//100));x=W-sw+24;rw=sw-48
    click(x+80,H-49);g.type('summarize this document and retain my note');g.key('ret')
    g.wait(rb'AGENT_WORKER task=1',30);g.complete(1,allow_conflict=True);v=gate.work(g)
    expected=original+'\nunsaved note retained\n'
    check('new native task seeds the complete unsaved document',g.capture('/bin/agentctl document 1')==expected and v[1]==1)
    check('source context contains the same unsaved version',g.capture('/bin/agentctl source 1 1 1 0')==expected)
    check('original disk source remains unchanged',g.capture('/bin/cat /docs/source.md')==original)
    candidate=g.capture('/bin/agentctl candidate 1');check('candidate waits for approval',bool(candidate) and v[2]>0)
    calls=model.calls;click(18,-15);g.close();g=None
    reboot=out/'reboot';reboot.mkdir();g=gate.runtime.Guest(build,disk,reboot,'bios','512M');g.wait(b'AGENTD_READY',180)
    g.capture('/bin/agent-runtime-test open-path /docs/source.md');pos,W,H=gate.frame(g,'/docs/source.md');time.sleep(2)
    check('reopening source restores its same task and proposal',gate.work(g)==v and g.capture('/bin/agentctl candidate 1')==candidate and model.calls==calls)
    click(x+rw//2,H-90);time.sleep(1)
    check('native Keep acts on the restored document binding',gate.work(g)[6]=='done' and g.capture('/bin/agentctl document 1')==expected)
    click(323,22);time.sleep(.4)
    check('Save creates a report without overwriting its source','DOCUMENT_VERIFIED' in g.capture('/bin/agentctl verify 1') and g.capture('/bin/cat /docs/source.md')==original)
    gate.screenshot(g,'restored-document')
    (out/'result.json').write_text(json.dumps({'status':'passed','simulated_model':True,'checks':checks},indent=2))
except Exception:
    if g:
        try:gate.screenshot(g,'failure')
        except Exception:pass
    raise
finally:
    if g:g.close()
    model.close()
