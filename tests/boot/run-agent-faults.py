#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Deterministic failure tests against the real broker and restricted guest apps.

This deliberately simulated model is only a fault fixture. Real-model acceptance
is run separately by run-agent.py and must never be inferred from this gate.
"""
import argparse, http.server, importlib.util, json, pathlib, re, secrets, threading, time

HERE=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('agent_guest',HERE/'run-agent.py')
runtime=importlib.util.module_from_spec(spec);spec.loader.exec_module(runtime)

class FaultModel:
    def __init__(self,out):
        self.mode='good';self.calls=0;self.releases=[];self.lock=threading.Lock()
        self.token=secrets.token_urlsafe(32);self.records=[];owner=self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                if self.headers.get('Authorization')!='Bearer '+owner.token:self.send_error(403);return
                n=int(self.headers.get('Content-Length','0'))
                if not 0<n<=8*1024*1024:self.send_error(400);return
                body=json.loads(self.rfile.read(n))
                with owner.lock:
                    owner.calls+=1;call=owner.calls;mode=owner.mode;release=threading.Event()
                    owner.releases.append(release)
                    owner.records.append({'call':call,'mode':mode})
                if mode=='delay' and not release.wait(90):return
                if mode=='disconnect':self.close_connection=True;return
                span=re.search(r'\[S\d+:\d+-\d+\]',body['messages'][-1]['content'])
                if not span:self.send_error(400);return
                text='# Deterministic fault fixture\nBudget 4200 yuan; 120 books. '+span[0]+'\n'
                if mode=='invalid':text='Invented object [S999:0-100]'
                response=json.dumps({'choices':[{'message':{'content':text},'finish_reason':'stop'}]}).encode()
                try:
                    self.send_response(200);self.send_header('Content-Type','application/json')
                    self.send_header('Content-Length',str(len(response)));self.end_headers();self.wfile.write(response)
                except (BrokenPipeError,ConnectionResetError):pass
        self.server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        self.server.daemon_threads=True
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        out.mkdir(parents=True,exist_ok=True)
        (out/'agent.key').write_text(self.token+'\n');(out/'agent.key').chmod(0o600)
        (out/'agent.conf').write_text(f'host=10.0.2.2\nport={self.server.server_port}\ntls=0\nmodel=deepseek-flash\nmax_tokens=2048\nkey_file=/etc/agent.key\n')
        self.out=out
    def wait_call(self,number):
        end=time.monotonic()+60
        while self.calls<number and time.monotonic()<end:time.sleep(.05)
        if self.calls<number:raise RuntimeError('model request never arrived')
    def release(self):
        for event in self.releases:event.set()
    def close(self):
        self.release();self.server.shutdown();self.server.server_close()
        (self.out/'metrics.json').write_text(json.dumps({'simulated':True,'calls':self.calls,'records':self.records},indent=2))

def rows(guest):
    text=guest.capture('/bin/agentctl status')
    return {int(x[0]):x for x in re.findall(r'TASK id=(\d+) phase=(.*?) revision=(\d+) calls=(\d+) active=(\d+).*?output=(\S+)',text)}

def phase(guest,task,expected,timeout=60):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        current=rows(guest).get(task)
        if current and current[1]==expected:return current
        time.sleep(.2)
    raise RuntimeError(f'task {task}: expected {expected}, got {current}')

def create(guest,task):
    result=guest.capture('/bin/agentctl create "Summarize these plans with citations." /docs /docs/source.md')
    if f'TASK_CREATED id={task}' not in result:raise RuntimeError(result)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=pathlib.Path,required=True)
    parser.add_argument('--out',type=pathlib.Path,required=True)
    parser.add_argument('--publication-stage',type=int,choices=(1,2))
    parser.add_argument('--catalog',action='store_true',help='activate every owned application and test its real registered identity')
    args=parser.parse_args()
    args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=True);args.build=args.build.resolve()
    gateway=FaultModel(args.out/'model');guest=None;passed=[]
    def check(name,condition):
        if not condition:raise RuntimeError(name)
        passed.append(name);print('AGENT_FAULT_PASS',name,flush=True)
    try:
        disk=runtime.prepare(args.build,args.out/'model',args.out,args.catalog)
        guest=runtime.Guest(args.build,disk,args.out,'bios','512M');guest.wait(b'AGENTD_READY',180)
        if args.catalog:
            apps=json.loads((runtime.ROOT/'c/apps/agent/catalog.json').read_text())
            for index,app in enumerate(apps):
                target=f'/docs/capability-{index}.txt'
                guest.command(app['path']+' --aex-capabilities > '+target)
                result=guest.capture('/bin/agent-runtime-test caps '+target+' '+app['id'])
                check('activated '+app['id'],'AGENT_CAPABILITY_VERIFIED' in result)
            check('capability discovery does not request inference',gateway.calls==0)
        check('new app memory is empty','revision=1 bytes=0' in guest.capture('/bin/agentctl memory os.logit.textedit'))
        check('explicit app memory saved','revision=2' in guest.capture('/bin/agentctl memory os.logit.textedit 1 "Prefer concise reports."'))
        check('memory operation retry deduplicated','revision=2' in guest.capture('/bin/agentctl memory os.logit.textedit 1 "Prefer concise reports."'))
        check('memory same operation different data refused','refused' in guest.capture('/bin/agentctl memory os.logit.textedit 1 "Different preference."'))
        check('Finder does not inherit TextEdit memory','revision=1 bytes=0' in guest.capture('/files.aex --aex-memory'))
        check('TextEdit reads its own persistent memory','Prefer concise reports.' in guest.capture('/textedit.aex --aex-memory'))
        if args.publication_stage:
            create(guest,1)
            guest.wait(f'AGENT_PUBLICATION_CRASH task=1 stage={args.publication_stage}'.encode(),120)
            guest.wait(rb'AGENTD_READY[\s\S]*AGENTD_READY',120)
            done=guest.complete(1)
            check('service crash reconciles original task and version',done[2]=='2' and done[3]=='2')
            check('publication recovery uses no new model calls',gateway.calls==2)
            check('recovered public artifact byte matches committed snapshot','DOCUMENT_VERIFIED' in guest.capture('/bin/agentctl verify 1'))
            snapshot=rows(guest);guest.close();guest=None
            restart=args.out/'reboot';restart.mkdir()
            guest=runtime.Guest(args.build,disk,restart,'bios','512M');guest.wait(b'AGENTD_READY',180)
            check('publication recovery remains stable after reboot',rows(guest)==snapshot and gateway.calls==2)
            (args.out/'result.json').write_text(json.dumps({'simulated_model':True,'publication_stage':args.publication_stage,'checks':passed,'status':'passed'},indent=2))
            return
        gateway.mode='delay';create(guest,1);gateway.wait_call(1)
        guest.capture('/bin/agentctl pause 1');paused=phase(guest,1,'paused')
        check('pause revokes active worker',paused[4]=='0' and paused[2]=='1')
        gateway.mode='good';gateway.release();time.sleep(.4)
        check('late model response cannot commit paused task',rows(guest)[1]==paused)
        guest.capture('/bin/agentctl resume 1');done=guest.complete(1)
        check('pause resume uses original task',done[2]=='2')

        before=gateway.calls;gateway.mode='disconnect';create(guest,2);gateway.wait_call(before+1)
        waiting=phase(guest,2,'waiting for model')
        check('model disconnect retains revision',waiting[2]=='1')
        gateway.mode='good';guest.capture('/bin/agentctl resume 2');guest.complete(2)
        check('model reconnect resumes checkpoint',rows(guest)[2][2]=='2')

        before=gateway.calls;gateway.mode='delay';create(guest,3);gateway.wait_call(before+1)
        guest.capture('/bin/agentctl cancel 3');cancelled=phase(guest,3,'cancelled')
        gateway.mode='good';gateway.release();time.sleep(.4)
        guest.capture('/bin/agentctl resume 3')
        check('cancel terminal despite late completion and resume',rows(guest)[3]==cancelled and cancelled[4]=='0')

        before=gateway.calls;gateway.mode='delay';start=len(guest.log);create(guest,4);gateway.wait_call(before+1)
        match=re.search(rb'AGENT_WORKER task=4 pid=(\d+)',bytes(guest.log[start:]))
        check('actual worker PID observed',bool(match))
        guest.capture('/bin/agent-runtime-test stop '+match[1].decode())
        gateway.mode='good';gateway.release();guest.complete(4)
        check('worker crash resumes task without duplicate artifact',rows(guest)[4][2]=='2')

        gateway.mode='invalid';create(guest,5);waiting=phase(guest,5,'waiting for model')
        check('invented model source rejected',waiting[2]=='1')
        gateway.mode='good';guest.capture('/bin/agentctl resume 5');guest.complete(5)
        check('invalid candidate recoverable',rows(guest)[5][2]=='2')

        create(guest,6);guest.complete(6)
        for _ in range(15):
            guest.capture('/bin/agentctl revise 6 "Continue the report with source citations."');guest.complete(6)
        before=gateway.calls;guest.capture('/bin/agentctl revise 6 "Continue after budget."')
        exhausted=phase(guest,6,'budget exhausted')
        check('32-call budget stops before provider request',exhausted[3]=='32' and gateway.calls==before)
        guest.capture('/bin/agentctl budget 6');guest.complete(6)
        check('explicit budget extension resumes same task',rows(guest)[6][3]=='34')

        guest.capture('/files.aex --ask "Summarize this context." "Cedar plans need a concise report."');guest.complete(7)
        check('Finder text context keeps research and editor roles',rows(guest)[7][3]=='2')
        guest.capture('/bin/echo --ask "Explain the supplied text without running commands." "Explicit text context only; no files have been granted."');guest.complete(8)
        check('CLI domain worker completes a draft through the broker',rows(guest)[8][3]=='1')
        snapshot=rows(guest)
        for task in (1,2,4,5,6):
            check(f'task {task} artifact byte matches document','DOCUMENT_VERIFIED' in guest.capture(f'/bin/agentctl verify {task}'))
        calls=gateway.calls;guest.close();guest=None
        restart=args.out/'reboot';restart.mkdir()
        guest=runtime.Guest(args.build,disk,restart,'bios','512M');guest.wait(b'AGENTD_READY',180)
        check('restart preserves completed and cancelled states',rows(guest)==snapshot)
        check('restart does not repeat completed model work',gateway.calls==calls)
        check('memory survives system restart','Prefer concise reports.' in guest.capture('/textedit.aex --aex-memory'))
        (args.out/'result.json').write_text(json.dumps({'simulated_model':True,'checks':passed,'model_calls':gateway.calls,'status':'passed'},indent=2))
        print('AGENT_DETERMINISTIC_FAULTS_PASS',len(passed),flush=True)
    except Exception:
        if guest and guest.process.poll() is None:
            try:guest.freeze_failure(512*1024*1024)
            except Exception as error:
                (guest.out/'failure-dump.error').write_text(str(error))
        raise
    finally:
        if guest:guest.close()
        gateway.close()

if __name__=='__main__':main()
