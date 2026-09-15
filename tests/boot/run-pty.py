#!/usr/bin/env python3
"""Boot one isolated LogitOS image and judge the ring-3 PTY probe marker."""
import argparse
import os
import shutil
import subprocess
import time
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True)
p.add_argument('--disk',type=Path,required=True)
p.add_argument('--command',default='/bin/pty-check')
p.add_argument('--expect-failure')
p.add_argument('--label',default='positive')
p.add_argument('--out',type=Path,required=True)
p.add_argument('--timeout',type=int,default=120)
a=p.parse_args()

qemu=os.environ.get('QEMU','qemu-system-x86_64')
if not shutil.which(qemu):
    print(f'SKIP: test-pty {a.label} -- cannot watch the guest assertion; {qemu} is not installed')
    raise SystemExit(77)
a.out.parent.mkdir(parents=True,exist_ok=True)
data=bytearray()

cmd=[qemu,'-cpu',os.environ.get('QEMU_CPU','max'),'-smp','4','-m','1G',
     '-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci',
     '-display','none','-no-reboot','-snapshot','-cdrom',str(a.iso.resolve()),
     '-boot','d','-drive',f'file={a.disk.resolve()},format=raw,if=none,id=hd0,file.locking=off',
     '-device','virtio-blk-pci,drive=hd0','-serial','stdio']
# A Unix-socket serial transport was tried first and QEMU's macOS sandbox
# refused bind() with EPERM under both /var/folders and /tmp. stdio is the
# existing boot-harness transport and still gives the driver exact guest bytes.
proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
try:
    os.set_blocking(proc.stdout.fileno(),False)
    def wait_for(marker,seconds):
        end=time.monotonic()+seconds
        while marker not in data:
            if proc.poll() is not None:raise RuntimeError('QEMU exited before '+marker.decode())
            if time.monotonic()>end:raise RuntimeError('timeout waiting for '+marker.decode())
            try:chunk=os.read(proc.stdout.fileno(),65536)
            except BlockingIOError:chunk=b''
            if chunk:data.extend(chunk);a.out.write_bytes(data)
            else:time.sleep(.02)
    wait_for(b'LogitOS shell',a.timeout)
    proc.stdin.write(a.command.encode()+b'\n');proc.stdin.flush()
    wait_for(b'PTY_RESULT ',a.timeout)
except Exception as error:
    a.out.write_bytes(data)
    print(f'FAIL: test-pty {a.label} -- {error}; log {a.out}')
    raise SystemExit(1)
finally:
    if proc.poll() is None:proc.terminate()
    try:proc.wait(timeout=5)
    except subprocess.TimeoutExpired:proc.kill();proc.wait()

text=data.decode(errors='replace').replace('\r','')
lines=[line for line in text.splitlines() if line.startswith(('PTY_PASS ','PTY_FAIL ','PTY_CHILD_','PTY_CHECKS=','PTY_RESULT '))]
print(f'PTY_{a.label.upper().replace("-","_")}_OUTPUT_BEGIN')
for line in lines:print(line)
print(f'PTY_{a.label.upper().replace("-","_")}_OUTPUT_END')
panic='LOGIT_PANIC' in text or 'EXCEPTION' in text
if a.expect_failure:
    ok=a.expect_failure in text and 'PTY_RESULT FAIL' in text and 'PTY_RESULT PASS' not in text and not panic
    if ok:print(f'PASS: pty {a.label} control observed "{a.expect_failure}"')
    else:print(f'FAIL: pty {a.label} control did not fail at the named assertion; log {a.out}')
else:
    ok='PTY_RESULT PASS' in text and 'PTY_FAIL ' not in text and not panic
    if ok:print('PASS: pty guest pair, exec, termios, session and isatty checks')
    else:print(f'FAIL: pty positive assertions; log {a.out}')
raise SystemExit(0 if ok else 1)
