#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Native TextEdit review workflow. QMP drives the actual document and sidebar.

--negative compiles the real broker with review bypassed and requires the
unchanged-document assertion to fail. Run it before the positive guest gate.
--gateway uses the existing real DeepSeek host service; absent that option,
the deterministic model is explicitly only lifecycle/failure evidence.
"""
import argparse, importlib.util, json, re, shlex, shutil, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
fault=module('work_fault',ROOT/'tests/boot/run-agent-faults.py');runtime=fault.runtime

def mutant(source,out):
    build=out/'build';build.mkdir()
    shutil.copytree(source/'agent',build/'agent',ignore=shutil.ignore_patterns('host','review-host','store-io'))
    for name in ('logit.iso','esp.img','agentctl.aex','login.aex','sh.aex','cat.aex','echo.aex','files.aex','textedit.aex','assistant.aex','agent-runtime-test.aex'):
        shutil.copy2(source/name,build/name)
    obj=build/'agent/c/apps/agent/agentd.o'
    planned=subprocess.run(['make','-Bn',f'BUILD={build}',str(obj)],cwd=ROOT,check=True,capture_output=True,text=True)
    commands=[shlex.split(line) for line in planned.stdout.splitlines() if ' -c c/apps/agent/agentd.c -o ' in line]
    if len(commands)!=1:raise RuntimeError('cannot derive broker build')
    with (out/'mutant-build.log').open('w') as log:
        subprocess.run(commands[0]+['-DAG_NEG_REVIEW_BYPASS'],cwd=ROOT,check=True,stdout=log,stderr=subprocess.STDOUT)
        subprocess.run(['make',f'BUILD={build}',str(build/'agentd.aex')],cwd=ROOT,check=True,stdout=log,stderr=subprocess.STDOUT)
    return build

def frame(g,title):
    pattern=rb'\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) pt zoom 0 min 0 '+re.escape(title.encode())
    g.wait(pattern,30);x,y,w,h,cw,ch=map(int,re.findall(pattern,bytes(g.log))[-1]);scale=w/cw
    return (lambda px,py:(round(x+px*scale),round(y+h-(ch-py)*scale))),cw,ch

def work(g,task=1):
    s=g.capture(f'/bin/agentctl work {task}')
    m=re.search(r'WORK task=(\d+) revision=(\d+) proposal=(\d+) base=(\d+) bytes=(\d+) review=(\d+) phase=(.+)',s)
    if not m:raise AssertionError('work view unavailable: '+s)
    return tuple(map(int,m.groups()[:6]))+(m[7],)

def screenshot(g,name):
    g.screenshot();target=g.out/(name+'.ppm');shutil.copyfile(g.out/'desktop.ppm',target)
    try:subprocess.run(['sips','-s','format','png',str(target),'--out',str(target.with_suffix('.png'))],check=True,capture_output=True)
    except FileNotFoundError:pass

def main():
    p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--gateway',type=Path);p.add_argument('--negative',action='store_true');p.add_argument('--mode',choices=('bios','uefi'),default='bios');p.add_argument('--ram',default='512M');a=p.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);out.chmod(0o700);build=a.build.resolve();g=model=None;checks=[]
    def check(label,value):
        if not value:raise AssertionError(label)
        checks.append(label);print('TEXTEDIT_WORK_PASS',label,flush=True)
    def denied(command):
        value=g.capture(command);return not value and g.last_capture_exit!=0
    try:
        if a.negative:build=mutant(build,out)
        if a.gateway:gateway=a.gateway.resolve()
        else:model=fault.FaultModel(out/'model');gateway=out/'model'
        disk=runtime.prepare(build,gateway,out)
        g=runtime.Guest(build,disk,out,a.mode,a.ram);g.wait(b'AGENTD_READY',180);g.wait(b'LogitOS shell',60)
        if a.negative:
            model.mode='delay';fault.create(g,1);model.wait_call(1);g.capture('/bin/agentctl work 1 review');model.mode='good';model.release()
            g.complete(1,allow_conflict=True)
            try:check('review does not change the document before approval',g.capture('/bin/agentctl document 1')=='')
            except AssertionError as exc:
                (out/'result.json').write_text(json.dumps({'status':'passed','negative_control':'AG_NEG_REVIEW_BYPASS','caught_assertion':str(exc)}));print('TEXTEDIT_WORK_NEGATIVE_CAUGHT',exc,flush=True);return
            raise RuntimeError('inconclusive: bypass was not caught')
        # The first report is an ordinary old-style Finder task; opening it opts
        # only this document into review, preserving the existing CLI workflow.
        created=g.capture('/bin/agentctl create "Write a concise Chinese report titled 社区图书角计划. Include 概况 and 下一步安排 headings and exact source citations. Distinguish plans from completed work." /docs /docs/source.md')
        check('Finder task created','TASK_CREATED id=1' in created)
        g.complete(1);initial=g.capture('/bin/agentctl document 1');check('report has actual facts and source references','4200' in initial.replace(',','') and '[S1:' in initial)
        g.capture('/bin/agent-runtime-test open-path /docs/report-1.md');pos,W,H=frame(g,'/docs/report-1.md');time.sleep(2)
        check('native document binds its own task',work(g)[5]==1)
        sw=min(360,max(290,W*31//100));dw=W-sw;x=dw+24;rw=sw-48
        check('work sidebar fits the native viewport',W>=760 and H>=640)
        def click(px,py):
            tx,ty=pos(px,py)
            # PS/2 relative motion can split a long jump into several packets.
            # Wait for bounded hops before pressing: the old 727-point jump
            # clicked the Dock while its last packet was still in flight.
            while max(abs(tx-g.pointer[0]),abs(ty-g.pointer[1]))>180:
                dx=max(-180,min(180,tx-g.pointer[0]));dy=max(-180,min(180,ty-g.pointer[1]))
                g.qmp('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':dx}},{'type':'rel','data':{'axis':'y','value':dy}}]})
                g.pointer[0]+=dx;g.pointer[1]+=dy;time.sleep(.18)
            g.click(tx,ty);time.sleep(.2)
        def submit(text):
            click(x+80,H-49);g.type(text);screenshot(g,'instruction-entered');g.key('ret')
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                if work(g)[6] not in ('done','paused'):return
                time.sleep(.2)
            raise AssertionError('native instruction was not submitted')
        click(100,170);g.key('ctrl','end');g.type('\nhuman note keep this sentence\n');time.sleep(2)
        before=g.capture('/bin/agentctl document 1');check('unsaved human edit reaches task','human note keep this sentence' in before)
        submit('add next steps and preserve the exact sentence human note keep this sentence')
        g.complete(1,allow_conflict=True);v=work(g);candidate=g.capture('/bin/agentctl candidate 1')
        check('candidate response is complete',g.last_capture_exit==0 and len(candidate.encode())==v[4] and bool(candidate))
        check('review does not change the document before approval',g.capture('/bin/agentctl document 1')==before and v[2]>0)
        check('review leaves saved artifact unchanged',g.capture('/bin/cat /docs/report-1.md')==initial)
        check('stale document decision refused',denied(f'/bin/agentctl decide 1 {v[1]-1} {v[2]} apply'))
        check('stale proposal decision refused',denied(f'/bin/agentctl decide 1 {v[1]} {v[2]-1} apply'))
        check('unknown source refused',denied('/bin/agentctl source 1 999 1 0'))
        check('wrong source version refused',denied('/bin/agentctl source 1 1 2 0'))
        check('source bytes match authorized immutable copy',g.capture('/bin/agentctl source 1 1 1 0')==(ROOT/'tests/fixtures/agent/source.md').read_text())
        click(x+80,278);time.sleep(.4);screenshot(g,'authorized-sources')
        click(W//2,(H-210-40)//3+40+67);g.wait(rb'TEXTEDIT_SOURCE task=1 object=1 revision=1 offset=0 bytes=368',20)
        check('native source row opens the authorized text',True);time.sleep(.4);screenshot(g,'source-text');click(dw-225,78);time.sleep(.3)
        click(dw-70,78);time.sleep(.5);screenshot(g,'candidate-review')
        # Close and power-cycle while approval is pending. No model request
        # should be needed to restore the same candidate or document binding.
        click(18,-15);time.sleep(.3);g.close();g=None
        reboot=out/'reboot';reboot.mkdir();g=runtime.Guest(build,disk,reboot,a.mode,a.ram);g.wait(b'AGENTD_READY',180)
        check('pending review survives system restart',work(g)==v and g.capture('/bin/agentctl candidate 1')==candidate and g.capture('/bin/agentctl document 1')==before)
        g.capture('/bin/agent-runtime-test open-path /docs/report-1.md');pos,W,H=frame(g,'/docs/report-1.md');time.sleep(2)
        click(dw-70,78);time.sleep(.4);click(x+rw//2,H-127);time.sleep(2)
        applied=work(g)
        check('native Apply commits exactly the reviewed proposal',applied[1]==v[1]+1 and applied[2]==0 and g.capture('/bin/agentctl document 1')==candidate)
        check('accepted report has matching disk artifact','DOCUMENT_VERIFIED' in g.capture('/bin/agentctl verify 1'))
        check('old completion cannot apply twice',denied(f'/bin/agentctl decide 1 {v[1]} {v[2]} apply') and work(g)==applied)
        click(323,22);time.sleep(.5);check('native Save retains complete document','DOCUMENT_VERIFIED' in g.capture('/bin/agentctl verify 1'))
        screenshot(g,'applied-document')
        submit('keep the report and suggest one more next step with citations');g.complete(1,allow_conflict=True);v2=work(g)
        current=g.capture('/bin/agentctl document 1');click(x+rw//2,H-90);time.sleep(1)
        check('native Keep preserves document and revision',work(g)[1]==v2[1] and work(g)[2]==0 and g.capture('/bin/agentctl document 1')==current)
        check('original material stays unchanged',g.capture('/bin/cat /docs/source.md')==(ROOT/'tests/fixtures/agent/source.md').read_text())
        (out/'result.json').write_text(json.dumps({'status':'passed','mode':a.mode,'ram':a.ram,'real_model':bool(a.gateway),'checks':checks,'initial_revision':v[1],'applied_revision':applied[1]},ensure_ascii=False,indent=2))
    except Exception:
        if g:
            try:screenshot(g,'failure')
            except Exception:pass
        raise
    finally:
        if g:g.close()
        if model:model.close()
if __name__=='__main__':main()
