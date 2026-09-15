#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Own one QEMU and a cancellable serial-input schedule.

A background shell producer | QEMU is one shell job: wait "$QPID" can keep
waiting for the producer's sleep 420 even after QEMU exits. Scheduled input
uses one thread here, with no sleep child to orphan. The test shell still owns
one PID; SIGTERM cancels input and stops/reaps only this owner's Popen child.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'qmp'))
from owned_process import stop_owned


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--send',nargs=2,action='append',default=[],metavar=('DELAY','TEXT'))
    parser.add_argument('--lines',action='store_true')
    parser.add_argument('--linger',type=float,default=0)
    parser.add_argument('command',nargs=argparse.REMAINDER)
    args=parser.parse_args()
    command=args.command[1:] if args.command[:1]==['--'] else args.command
    if not command:parser.error('missing QEMU command after --')
    stages=[(float(delay),(text+('\n' if args.lines else '')).encode()) for delay,text in args.send]
    if args.linger<0 or any(delay<0 for delay,_ in stages):parser.error('negative delay')
    cancel=threading.Event();process=None;producer=None
    def terminate(signum,frame):
        raise SystemExit(128+signum)
    previous=signal.signal(signal.SIGTERM,terminate)
    try:
        process=subprocess.Popen(command,stdin=subprocess.PIPE,bufsize=0)
        def produce():
            try:
                for delay,data in stages:
                    if cancel.wait(delay):return
                    view=memoryview(data)
                    while view:
                        if cancel.is_set():return
                        n=os.write(process.stdin.fileno(),view)
                        view=view[n:]
                if cancel.wait(args.linger):return
                process.stdin.close()
            except (BrokenPipeError,OSError):
                # QEMU can close its serial input before the schedule ends.
                # The parent keeps the guest result and the test's assertions.
                pass
        producer=threading.Thread(target=produce,daemon=True)
        producer.start()
        return process.wait()
    finally:
        cancel.set()
        # Close QEMU's pipe reader first so a blocked writer can also return.
        stop_owned(process)
        if producer is not None:producer.join(timeout=1)
        if process is not None:process.stdin.close()
        signal.signal(signal.SIGTERM,previous)


if __name__=='__main__':
    sys.exit(main())
