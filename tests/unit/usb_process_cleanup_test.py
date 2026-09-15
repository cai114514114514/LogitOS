#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual USB harness/wrapper cleanup, using sleeping children instead of a guest.

Only the post-Popen test body is interrupted. Production finally blocks,
SIGTERM handlers, escalation and waits execute unchanged. This gate neither
searches for QEMU processes nor sends a signal to a process it did not launch.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--source-root',type=Path)
a=p.parse_args();root=(a.source_root or Path(__file__).resolve().parents[2]).resolve()
b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root/'tests/qmp'))
from owned_process import stop_owned,run_owned
source=(root/'tests/qmp/qmp_usb.py').read_text()
needle='    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=qerr)\n'
assert source.count(needle)==1,'USB process ownership seam changed'
source=source.replace(needle,needle+'    cleanup_probe()\n')
worker=b/'child.py'
worker.write_text('#!'+sys.executable+'\nimport os,signal,time\nfrom pathlib import Path\n'
                  'if os.environ.get("CLEANUP_IGNORE_TERM"): signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                  'Path(os.environ["CLEANUP_PID"]).write_text(str(os.getpid()))\n'
                  'while True: time.sleep(10)\n')
worker.chmod(0o700)
env=dict(os.environ);original_argv=sys.argv[:]
os.environ.update(QEMU=str(root/'tests/boot/dma-qemu-wrapper.py'),DMA_TEST_QEMU=str(worker),
                  DMA_TEST_BOOT='bios',DMA_TEST_BUILD=str(b),DMA_TEST_RAM='512M')

def await_pid(path):
    deadline=time.monotonic()+5
    while not path.exists() and time.monotonic()<deadline:time.sleep(.01)
    assert path.exists(),'the wrapper never started its child'
    return int(path.read_text())

def alive(pid):
    try:os.kill(pid,0);return True
    except ProcessLookupError:return False

def reaped(process):
    if process.returncode is None:return False
    try:os.waitpid(process.pid,os.WNOHANG)
    except ChildProcessError:return True
    return False

def case(name,mode,control=False,ignore_term=False):
    d=b/name;d.mkdir(exist_ok=True);pidfile=d/'child.pid';pidfile.unlink(missing_ok=True)
    os.environ.update(DMA_TEST_OUT=str(d),CLEANUP_PID=str(pidfile))
    if ignore_term:os.environ['CLEANUP_IGNORE_TERM']='1'
    else:os.environ.pop('CLEANUP_IGNORE_TERM',None)
    ns={'__file__':str(root/'tests/qmp/qmp_usb.py')}
    child=None
    def probe():
        nonlocal child
        child=await_pid(pidfile)
        if mode=='failure':raise RuntimeError('injected USB assertion failure')
        if mode=='signal':os.kill(os.getpid(),signal.SIGTERM)
        raise SystemExit(0)
    ns['cleanup_probe']=probe
    code=source.replace('        stop_owned(proc)','        pass # missing ownership cleanup control') if control else source
    sys.argv=['qmp_usb.py','unused.iso','unused.img']
    try:
        try:exec(compile(code,ns['__file__'],'exec'),ns)
        except SystemExit as e:assert e.code==(143 if mode=='signal' else 0)
        except RuntimeError as e:assert mode=='failure' and str(e)=='injected USB assertion failure'
        owned=ns['proc'];clean=reaped(owned) and child is not None and not alive(child)
        if control:
            assert not clean,'missing-cleanup control did not expose a live child'
            print('CONTROL missing USB finally cleanup caught: owned wrapper/child still live',flush=True)
        else:
            assert clean,'USB '+name+' did not stop and reap its wrapper and child'
            print('PASS USB cleanup '+name,flush=True)
    finally:
        stop_owned(ns.get('proc'))
        if child is not None:assert not alive(child),'fixture failed to reclaim its own child'

try:
    # A no-cleanup mutant must fail this exact ownership assertion before any
    # positive runs. Its private Popen is then reclaimed by this outer fixture.
    case('negative','success',control=True)
    case('success','success')
    case('assertion-failure','failure')
    case('sigterm','signal')
    case('child-ignores-term','success',ignore_term=True)

    # Test the caller's timeout too: it must SIGTERM the instrument, whose
    # actual USB finally then stops the wrapper, which reaps its child.
    d=b/'parent-timeout';d.mkdir(exist_ok=True);pidfile=d/'child.pid';pidfile.unlink(missing_ok=True)
    os.environ.update(DMA_TEST_OUT=str(d),CLEANUP_PID=str(pidfile));os.environ.pop('CLEANUP_IGNORE_TERM',None)
    instrument=d/'instrument.py'
    instrument.write_text('import time,sys\nsys.argv=["qmp_usb.py","unused.iso","unused.img"]\n'
        'def cleanup_probe():\n while True: time.sleep(10)\n'
        'ns={"__file__":'+repr(str(root/'tests/qmp/qmp_usb.py'))+',"cleanup_probe":cleanup_probe}\n'
        'exec(compile('+repr(source)+',ns["__file__"],"exec"),ns)\n')
    try:run_owned([sys.executable,str(instrument)],timeout=1,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:pass
    else:raise AssertionError('parent timeout was not exercised')
    assert not alive(await_pid(pidfile)),'parent timeout stranded its QEMU child'
    print('PASS USB cleanup parent-timeout',flush=True)
finally:
    sys.argv=original_argv;os.environ.clear();os.environ.update(env)
print('USB_PROCESS_CLEANUP_PASS: 1 control, 5 real process cleanup paths',flush=True)
