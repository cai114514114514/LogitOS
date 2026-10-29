#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A task made on v4 must keep its report through conversion and a v5 update."""
import argparse,json,importlib.util,sys
from pathlib import Path
import signal
def interrupted(signum,frame):raise KeyboardInterrupt
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from project_volume import convert
from disk_guard import image_guard
from agent_session import install
spec=importlib.util.spec_from_file_location('project_migration',ROOT/'tests/boot/run-project-work.py');gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);b=a.build.resolve();model=gate.gate.fault.FaultModel(out/'model');g=None
signal.signal(signal.SIGTERM,interrupted)
try:
 source=gate.runtime.prepare(b,out/'model',out,extras=[f'{b}/mv.aex:/bin/mv'])
 g=gate.runtime.Guest(b,source,out,'bios','512M');g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
 assert 'TASK_CREATED id=1' in g.capture('/bin/agentctl create "Summarize with exact citations" /docs /docs/source.md');g.complete(1)
 before=g.capture('/bin/agentctl document 1');g.close();g=None
 disk=out/'project.img';convert(source,disk,b/'lfs_snapshot')
 with image_guard(disk):install(disk,b/'lfs_snapshot',{'/bin/agentd':((b/'agentd.aex').read_bytes(),0o755),'/etc/agent.key':((out/'model/agent.key').read_bytes(),0o600)})
 reboot=out/'reboot';reboot.mkdir();g=gate.runtime.Guest(b,disk,reboot,'bios','512M');g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
 assert 'PROJECT_TASK id=1' in g.capture('/bin/agentctl project /docs'),'converted task binds directory identity'
 assert 'DOCUMENT_TASK task=1' in g.capture('/bin/agentctl lookup /docs/report-1.md'),'converted task binds document identity'
 assert g.capture('/bin/agentctl document 1')==before,'conversion retains confirmed content'
 g.capture('/bin/mv /docs /migrated');assert g.last_capture_exit==0
 assert 'DOCUMENT_TASK task=1' in g.capture('/bin/agentctl lookup /migrated/report-1.md'),'converted binding follows rename'
 assert 'artifact=/migrated/report-1.md' in g.capture('/bin/agentctl verify 1'),'converted report matches disk'
 (out/'result.json').write_text(json.dumps({'passed':True,'real_model':False,'checks':['v4 task creation','v5 copy conversion','identity-preserving install','task adoption','Project rename','disk equality']},indent=2));print('PROJECT_MIGRATION_PASS conversion, update and native task adoption',flush=True)
finally:
 if g:g.close()
 model.close()
