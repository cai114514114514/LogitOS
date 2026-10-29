#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PT_INTERP CLI, AEX, fixed ELF and GUI acceptance on the ordinary kernel."""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--disk',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--modes',default='bios,uefi');p.add_argument('--ram',default='512M,2G,8G');p.add_argument('--timeout',type=int,default=180)
p.add_argument('--expect-failure',help='A named guest assertion, never a crash or timeout')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);results=[]
for mode in a.modes.split(','):
 for ram in a.ram.split(','):
  d=a.out/f'{mode}-{ram}';d.mkdir(parents=True,exist_ok=True);log=bytearray()
  with tempfile.TemporaryDirectory(prefix='logit-ptinterp-') as tmp:
   path=tmp+'/serial';ser=None
   cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-smp','4','-m',ram,'-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-netdev','user,id=n0','-device','e1000,netdev=n0','-display','none','-no-reboot','-snapshot','-drive',f'file={a.disk.resolve()},format=raw,if=none,id=hd0,file.locking=off','-device','virtio-blk-pci,drive=hd0','-chardev',f'socket,id=ser0,path={path},server=on,wait=on','-serial','chardev:ser0']
   if mode=='bios':cmd+=['-cdrom',str(a.build.resolve()/'logit.iso'),'-boot','d']
   elif mode=='uefi':
    var=Path(tmp)/'vars.fd';shutil.copyfile(os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd'),var)
    cmd+=['-machine','q35','-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd'),'-drive',f'if=pflash,format=raw,file={var}','-device','ich9-ahci,id=ahci0','-drive',f'file={a.build.resolve()}/esp.img,format=raw,if=none,id=esp0,file.locking=off','-device','ide-hd,drive=esp0,bus=ahci0.0']
   else:raise SystemExit('unsupported firmware '+mode)
   (d/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
   err=(d/'qemu.log').open('wb');q=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
   try:
    ser=socket.socket(socket.AF_UNIX)
    for i in range(300):
     try:ser.connect(path);break
     except OSError:
      if q.poll() is not None:raise RuntimeError('QEMU exited before serial')
      time.sleep(.1)
    def read():
     try:
      while True:
       b=ser.recv(65536)
       if not b:break
       log.extend(b);(d/'serial.log').write_bytes(log)
     except OSError:pass
    reader=threading.Thread(target=read,daemon=True);reader.start()
    def waitfor(markers):
     end=time.monotonic()+a.timeout
     while not any(m in log for m in markers) and q.poll() is None and time.monotonic()<end:time.sleep(.1)
     if not any(m in log for m in markers):raise RuntimeError('timeout '+repr(markers))
    waitfor([b'LogitOS shell']);time.sleep(.3)
    ser.sendall(b'/bin/ptinterp-check\n')
    waitfor([b'PTINTERP_GUEST_PASS',b'PTINTERP_GUEST_FAIL',b'PTINTERP_RUNTIME_FAIL',b'PTINTERP_MAIN_FAIL',b'PTINTERP_FP_FAIL'])
    if a.expect_failure:
     waitfor([a.expect_failure.encode()])
    elif b'PTINTERP_GUEST_PASS' in log:
     waitfor([b'PTINTERP_GUI_PASS',b'PTINTERP_GUI_FAIL'])
     waitfor([b'PTINTERP_GUI_TINY_PASS',b'PTINTERP_GUI_FAIL'])
    text=log.decode(errors='replace')
    counts={n:text.count(n) for n in ['PTINTERP_RUNTIME_PASS','PTINTERP_MAIN_PASS','PIE_PROGRAM_PASS','PIE_CHILD_PASS','PIE_CAP_PASS','PTINTERP_GUI_PASS','PTINTERP_GUI_TINY_PASS','PTINTERP_FP_PASS']}
    ok='PTINTERP_GUEST_PASS' in text and all(counts[k]>=v for k,v in dict(PTINTERP_RUNTIME_PASS=10,PTINTERP_MAIN_PASS=9,PIE_PROGRAM_PASS=3,PIE_CHILD_PASS=3,PIE_CAP_PASS=3,PTINTERP_GUI_PASS=1,PTINTERP_GUI_TINY_PASS=1,PTINTERP_FP_PASS=1).items()) and not re.search(r'PTINTERP_\w+_FAIL|LOGIT_PANIC',text)
    biases=[int(v,16) for v in re.findall(r'PIE_BIAS (0x[0-9a-f]+)',text)]
    ok=ok and 0 in biases and len(set(v for v in biases if v))>=2
    if a.expect_failure:ok=a.expect_failure in text and 'LOGIT_PANIC' not in text
    item=dict(mode=mode,ram=ram,passed=bool(ok),expected_failure=a.expect_failure,counts=counts,main_biases=biases,log=str(d/'serial.log'))
    results.append(item);(a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(('PASS' if ok else 'FAIL'),mode,ram,flush=True)
    if not ok:raise RuntimeError('assertion failed')
   except Exception as e:raise SystemExit(f'FAIL {mode} {ram}: {e}; logs {d}')
   finally:
    if q.poll() is None:q.terminate()
    try:q.wait(timeout=5)
    except subprocess.TimeoutExpired:q.kill();q.wait()
    if ser:ser.close()
    err.close();(d/'serial.log').write_bytes(log)
