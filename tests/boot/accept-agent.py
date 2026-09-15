#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and rerun AEX acceptance with isolated artifacts and host-only secrets."""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,sys,time
ROOT=pathlib.Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=pathlib.Path,required=True);p.add_argument('--out',type=pathlib.Path,required=True)
p.add_argument('--env',type=pathlib.Path,required=True,help='private host DeepSeek credential file; never packed into the guest')
p.add_argument('--skip-build',action='store_true',help='use a previously validated build; all runtime gates still run')
a=p.parse_args();a.build=a.build.resolve();a.out=a.out.resolve();a.env=a.env.resolve()
if a.out.exists() and any(a.out.iterdir()):raise SystemExit('Choose a new output directory; existing evidence is never overwritten.')
a.out.mkdir(parents=True,exist_ok=True);a.out.chmod(0o700)
if not a.env.is_file():raise SystemExit('Private credential file does not exist')
stages=[];gateway=None;gateway_log=None
def run(name,command):
    print('AGENT_ACCEPT_STAGE',name,flush=True)
    record={'stage':name,'command':command,'status':'running'};stages.append(record)
    (a.out/'progress.json').write_text(json.dumps(stages,indent=2))
    started=time.monotonic()
    with (a.out/(name+'.log')).open('w') as log:r=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    record.update(status='passed' if r.returncode==0 else 'failed',seconds=round(time.monotonic()-started,3),exit=r.returncode)
    (a.out/'progress.json').write_text(json.dumps(stages,indent=2))
    if r.returncode:raise RuntimeError(f'{name} failed; see {a.out}/{name}.log')
try:
    if not a.skip_build:
        run('build',['make',f'BUILD={a.build}','-j6','all',str(a.build/'esp.img'),'test-agent','test-agent-catalog-build',str(a.build/'agent-runtime-test.aex'),'test-mk-wired'])
    else:run('host-gates',['make',f'BUILD={a.build}','test-agent','test-mk-wired'])
    # Fault fixtures are prerequisites to real-model acceptance, never its substitute.
    run('unconfigured-startup',['make',f'BUILD={a.build}',f'AGENT_STARTUP_OUT={a.out}/unconfigured-startup','test-agent-startup'])
    run('faults',['python3','tests/boot/run-agent-faults.py','--build',str(a.build),'--out',str(a.out/'faults'),'--catalog'])
    run('legacy-cli',['python3','tests/boot/run-agent-cli.py','--build',str(a.build),'--out',str(a.out/'legacy-cli'),'--mode','bios','--ram','512M'])
    run('idle',['python3','tests/boot/run-unix-poll-idle.py','--build',str(a.build),'--out',str(a.out/'idle'),'--mode','bios','--ram','512M'])
    for stage in (1,2):
        build=a.out/f'publication-build-{stage}';build.mkdir()
        run(f'publication-build-{stage}',['make',f'BUILD={build}',f'AGENT_BROKER_FLAGS=-DAGENT_FAULT_STAGE={stage}','-j6',str(build/'agentd.aex')])
        for name in ('logit.iso','esp.img','agentctl.aex','agent-runtime-test.aex','login.aex','sh.aex','cat.aex','echo.aex','files.aex','textedit.aex','assistant.aex'):shutil.copy2(a.build/name,build/name)
        run(f'publication-{stage}',['python3','tests/boot/run-agent-faults.py','--build',str(build),'--out',str(a.out/f'publication-{stage}'),'--publication-stage',str(stage)])
    run('normal-session',['python3','tests/boot/run-agent-session.py','--build',str(a.build),'--out',str(a.out/'normal-session'),'--env',str(a.env)])
    gateway_dir=a.out/'gateway';gateway_log=(a.out/'gateway.log').open('w')
    gateway=subprocess.Popen([sys.executable,'tools/agent_gateway.py','--env',str(a.env),'--state',str(gateway_dir),'--limit','64'],cwd=ROOT,stdout=gateway_log,stderr=subprocess.STDOUT)
    for _ in range(100):
        if (gateway_dir/'ready.json').exists():break
        if gateway.poll() is not None:raise RuntimeError('gateway could not start')
        time.sleep(.1)
    else:raise RuntimeError('gateway readiness deadline')
    for mode in ('bios','uefi'):
        for ram in ('512M','2G','8G'):
            run(f'real-{mode}-{ram}',['python3','tests/boot/run-agent.py','--build',str(a.build),'--gateway',str(gateway_dir),'--out',str(a.out/f'real-{mode}-{ram}'),'--mode',mode,'--ram',ram,'--gui','--reboot'])
    fingerprints={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in a.build.glob('*.aex')}
    for name in ('logit.iso','esp.img'):fingerprints[name]=hashlib.sha256((a.build/name).read_bytes()).hexdigest()
    (a.out/'verified.json').write_text(json.dumps({'status':'passed','stages':stages,'sha256':fingerprints,'model':'deepseek-flash','provider':'DeepSeek','gateway_metrics':json.loads((gateway_dir/'metrics.json').read_text())},indent=2))
    print('AGENT_ACCEPTANCE_PASS',flush=True)
finally:
    if gateway:
        gateway.terminate()
        try:gateway.wait(timeout=10)
        except subprocess.TimeoutExpired:gateway.kill();gateway.wait()
    if gateway_log:gateway_log.close()
