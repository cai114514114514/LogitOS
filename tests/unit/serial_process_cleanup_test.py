#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise real serial-owner/script cleanup without starting a QEMU guest.

The receiver below is a real private child. Gate markers only end the actual
shell scripts; this measures process cleanup, never kernel correctness. Every
signal targets a Popen or a PID recorded by that Popen's own child.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--source-root',type=Path)
a=p.parse_args();r=(a.source_root or Path(__file__).resolve().parents[2]).resolve();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
helper=r/'tests/boot/serial-guest.py'
worker=b/'receiver.py'
worker.write_text('#!'+sys.executable+'''
import json,os,sys,time
from pathlib import Path
Path(os.environ['SERIAL_PID']).write_text(json.dumps([os.getpid(),os.getppid()]))
mode=os.environ.get('SERIAL_MODE','hold')
if mode=='bytes':
 Path(os.environ['SERIAL_BYTES']).write_bytes(sys.stdin.buffer.read());sys.exit(0)
if mode=='ack':
 Path(os.environ['SERIAL_BYTES']).write_bytes(sys.stdin.buffer.read(1))
marker=os.environ.get('SERIAL_MARKER','')
if marker:print(marker,flush=True)
if mode=='exit':sys.exit(1)
while True:time.sleep(10)
''');worker.chmod(0o700)

def alive(pid):
    try:os.kill(pid,0);return True
    except ProcessLookupError:return False

def ready(path):
    end=time.monotonic()+5
    while not path.exists() and time.monotonic()<end:time.sleep(.01)
    assert path.exists(),'private receiver never started'
    return json.loads(path.read_text())

def env_for(name,**values):
    d=b/name;d.mkdir(exist_ok=True);pf=d/'pid.json';pf.unlink(missing_ok=True)
    bf=d/'input.bin';bf.unlink(missing_ok=True)
    return dict(os.environ,QEMU=str(worker),SERIAL_PID=str(pf),SERIAL_BYTES=str(bf),**values),pf,bf

def kill_own(process):
    if process.poll() is None:process.kill()
    process.wait(timeout=5)

# Negative control: restoring an uninterruptible producer join must stall even
# though QEMU itself has already stopped. Kill only this fixture's owner after
# observing that exact fault; the receiver is confirmed dead first.
mutant=b/'negative/tests/boot/serial-guest.py';mutant.parent.mkdir(parents=True,exist_ok=True)
qmp=mutant.parent.parent/'qmp';qmp.mkdir(exist_ok=True)
(qmp/'owned_process.py').write_text((r/'tests/qmp/owned_process.py').read_text())
mutant.write_text(helper.read_text().replace('        cancel.set()','        pass # deliberately non-cancellable producer').replace('producer.join(timeout=1)','producer.join()'))
env,pf,_=env_for('negative',SERIAL_MODE='hold')
proc=subprocess.Popen([sys.executable,str(mutant),'--send','420','never','--',str(worker)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    child,_=ready(pf);proc.terminate()
    try:proc.wait(timeout=.4)
    except subprocess.TimeoutExpired:pass
    else:raise AssertionError('negative control did not wait for its sleeping producer')
    assert not alive(child),'negative control stopped for the wrong reason: receiver still alive'
    print('CONTROL uninterruptible producer caught: owner waits after child is dead',flush=True)
finally:kill_own(proc)

# The serial schedule itself preserves bytes and end-of-input, and cancellation
# in the 420-second tail is fast. No shell sleep process exists in either path.
for mode in ('bytes','ack'):
    env,pf,bf=env_for('schedule-'+mode,SERIAL_MODE=mode)
    args=['--lines','--send','.02','one','--send','.02','two','--linger','.02'] if mode=='bytes' else ['--send','0','x','--linger','420']
    proc=subprocess.Popen([sys.executable,str(helper)]+args+['--',str(worker)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    try:
        child,_=ready(pf)
        if mode=='ack':
            end=time.monotonic()+3
            while not bf.exists() and time.monotonic()<end:time.sleep(.01)
            assert bf.read_bytes()==b'x','producer never reached its long tail'
            start=time.monotonic();proc.terminate();proc.wait(timeout=3)
            assert time.monotonic()-start<3,'long producer tail delayed cleanup'
        else:
            assert proc.wait(timeout=3)==0
            assert bf.read_bytes()==b'one\ntwo\n','serial schedule changed input bytes'
        assert not alive(child),'serial owner left its receiver alive'
        print('PASS serial schedule '+mode,flush=True)
    finally:kill_own(proc)

# Execute all three real shell scripts. Their original success/failure paths
# must finish promptly and reclaim the actual serial owner and receiver.
for script,success,failure in (
 ('run-thread-test.sh','THREAD_TEST_OK','THREAD_TEST_FAIL'),
 ('run-smp-test.sh','SMP_TEST_OK','SMP_TEST_FAIL'),
 ('run-smp-fork-storm.sh','STORM-1-OK\nSTORM-2-OK\nSTORM-ALL-DONE','STORM-1-OK')):
    for passed in (True,False):
        name=script+('-success' if passed else '-failure')
        env,pf,_=env_for(name,SERIAL_MODE='exit' if script=='run-smp-fork-storm.sh' and not passed else 'hold',SERIAL_MARKER=success if passed else failure)
        start=time.monotonic()
        proc=subprocess.Popen(['bash',str(r/'tests/boot'/script),'unused.iso','unused.img','2','4'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        try:
            out=proc.communicate(timeout=6)[0];elapsed=time.monotonic()-start
            (b/(name+'.log')).write_bytes(out)
            assert proc.returncode==(0 if passed else 1),(name,out.decode())
            child,owner=ready(pf)
            assert not alive(child) and not alive(owner),name+' left a child or serial owner alive'
            print(f'PASS {name}: {elapsed:.3f}s, owner and receiver reaped',flush=True)
        finally:
            # Test errors can only target this private process tree. The script
            # handles TERM with its EXIT cleanup; kill is the bounded fallback.
            if proc.poll() is None:
                proc.terminate()
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:kill_own(proc)
            else:proc.wait()
print('SERIAL_PROCESS_CLEANUP_PASS: 1 control, 2 schedules, 6 actual script exits',flush=True)
