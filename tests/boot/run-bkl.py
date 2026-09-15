#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fresh-boot real-kernel rendezvous and concurrent MM/FS; identical benchmark mode."""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--disk',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--modes',default='bios,uefi');p.add_argument('--ram',default='512M,2G,8G');p.add_argument('--bench',action='store_true');p.add_argument('--repeat',type=int,default=1);p.add_argument('--expect-serialized',action='store_true');p.add_argument('--timeout',type=int,default=180)
p.add_argument('--expect-irq-masked',action='store_true')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);results=[]
def host_load(own_pid):
 # Observe other emulator load without changing another task's processes.
 rows=subprocess.check_output(['ps','-Ao','pid,pcpu,comm'],text=True).splitlines()[1:]
 others=[]
 for row in rows:
  fields=row.split(None,2)
  if len(fields)==3 and 'qemu-system' in fields[2] and int(fields[0])!=own_pid:
   others.append(dict(pid=int(fields[0]),cpu_percent=float(fields[1])))
 return dict(load_average=os.getloadavg(),other_qemu=others)
for mode in a.modes.split(','):
 for ram in a.ram.split(','):
  for repeat in range(a.repeat):
   d=a.out/f'{mode}-{ram}-{repeat}';d.mkdir(parents=True,exist_ok=True)
   log=bytearray()
   with tempfile.TemporaryDirectory(prefix='logit-bkl-') as tmp:
    ser=tmp+'/serial';serial=None
    cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-smp','4','-m',ram,'-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-netdev','user,id=n0','-device','e1000,netdev=n0','-display','none','-no-reboot','-snapshot','-drive',f'file={a.disk.resolve()},format=raw,if=none,id=hd0,file.locking=off','-device','virtio-blk-pci,drive=hd0','-chardev',f'socket,id=ser0,path={ser},server=on,wait=on','-serial','chardev:ser0']
    if mode=='bios':cmd+=['-cdrom',str(a.build/'logit.iso'),'-boot','d']
    elif mode=='uefi':
     code=os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd');varsrc=os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd')
     var=Path(tmp)/'vars.fd';shutil.copyfile(varsrc,var)
     cmd+=['-machine','q35','-drive',f'if=pflash,format=raw,readonly=on,file={code}','-drive',f'if=pflash,format=raw,file={var}','-device','ich9-ahci,id=ahci0','-drive',f'file={a.build.resolve()}/esp.img,format=raw,if=none,id=esp0,file.locking=off','-device','ide-hd,drive=esp0,bus=ahci0.0']
    else:raise SystemExit('unsupported boot mode')
    (d/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    err=(d/'qemu.log').open('wb');q=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
    def waitfor(marker,timeout):
     until=time.monotonic()+timeout
     while marker not in log and q.poll() is None and time.monotonic()<until:time.sleep(.05)
     if marker not in log:raise RuntimeError('timeout waiting for '+repr(marker))
    try:
     serial=socket.socket(socket.AF_UNIX)
     for i in range(300):
      try:serial.connect(ser);break
      except OSError:
       if q.poll() is not None:raise RuntimeError('QEMU exited before serial')
       time.sleep(.1)
     def read():
      try:
       while True:
        b=serial.recv(65536)
        if not b:break
        log.extend(b);(d/'serial.log').write_bytes(log)
      except OSError:pass
     reader=threading.Thread(target=read,daemon=True);reader.start()
     waitfor(b'LogitOS shell',a.timeout);time.sleep(.5)
     load_start=host_load(q.pid)
     work_started=time.monotonic_ns()
     serial.sendall(b'/bin/bkl'+(b' bench' if a.bench else b'')+b'\n')
     until=time.monotonic()+a.timeout
     while not (b'BKL_GUEST_PASS' in log or b'BKL_GUEST_FAIL' in log) and q.poll() is None and time.monotonic()<until:time.sleep(.1)
     if a.expect_serialized:
      ok=b'BKL_GUEST_FAIL' in log and b'[bkluser] FAIL simultaneous kernel entry' in log
     elif a.expect_irq_masked:
      ok=b'BKL_GUEST_FAIL' in log and b'[bkluser] FAIL interruptible syscall' in log
     else:
      ok=b'BKL_GUEST_PASS' in log and b'BKL_GUEST_FAIL' not in log
      if not a.bench:
       irqs=re.findall(rb'\[bklirq\] cpu=(\d+) ticks=(\d+) context_stable=(\d+)',log)
       ok=ok and len(re.findall(rb'\[bklentry\] cpu=\d+ simultaneous=4 expected=4',log))==4
       ok=ok and len(irqs)==4 and len({x[0] for x in irqs})==4 and all(int(x[1])>=3 and x[2]==b'1' for x in irqs)
     times=re.findall(rb'BKL_WORK_MS (\d+)',log)
     item=dict(mode=mode,ram=ram,repeat=repeat,passed=bool(ok),work_ms=int(times[-1]) if times else None,external_ms=round((time.monotonic_ns()-work_started)/1e6,3),log=str(d/'serial.log'),host_load_start=load_start,host_load_end=host_load(q.pid))
     results.append(item);(a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
     print(('PASS' if ok else 'FAIL'),mode,ram,item['work_ms'],'ms',flush=True)
     if not ok:raise RuntimeError('guest acceptance failed')
    except Exception as e:raise SystemExit(f'FAIL {mode} {ram}: {e}; logs: {d}')
    finally:
     if q.poll() is None:q.terminate()
     try:q.wait(timeout=5)
     except subprocess.TimeoutExpired:q.kill();q.wait()
     if serial:serial.close()
     err.close();(d/'serial.log').write_bytes(log)
