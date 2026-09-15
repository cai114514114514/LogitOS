#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Serial-driven heap/module/panic acceptance, owning only its private QEMU."""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--disk',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
p.add_argument('--modes',default='bios,uefi');p.add_argument('--ram',default='512M,2G,8G');p.add_argument('--work',choices=['heap','module','panic'],default='heap');p.add_argument('--expect-failure');p.add_argument('--timeout',type=int,default=180)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True);results=[]
for mode in a.modes.split(','):
 for ram in a.ram.split(','):
  d=a.out/f'{mode}-{ram}';d.mkdir(parents=True,exist_ok=True);log=bytearray()
  with tempfile.TemporaryDirectory(prefix='logit-highheap-') as tmp:
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
    if a.work=='panic':
     ser.sendall(b'echo panic highheap-stack-probe > /dev/ktrigger\n');waitfor([b'LOGIT_PANIC_END'])
     text=log.decode(errors='replace')
     ok='highheap-stack-probe' in text and bool(re.search(r'RSP 0xffff[89ab][0-9a-f]+',text)) and 'backtrace:' in text
     # Validate actual symbolic frames from the kernel panic chain, not merely
     # plausible hex addresses. The product symbol table supplies their names.
     import importlib.util
     spec=importlib.util.spec_from_file_location('symbols',Path(__file__).resolve().parents[2]/'tools/ksymbolize.py');symbols=importlib.util.module_from_spec(spec);spec.loader.exec_module(symbols)
     table=symbols.Table(symbols.load_map(a.build/'kernel.map'))
     cut=text[text.find('*** LOGIT_PANIC on cpu'):]
     frames=re.findall(r'^\s*#\d+ (0x[0-9a-f]+)',cut,re.M)
     # A noreturn panic call can end its caller's symbol exactly. These are
     # RETURN addresses, so the preceding instruction byte owns the frame.
     resolved=[table.lookup(int(f,16)-1) for f in frames]
     ok=ok and len(resolved)>=2 and resolved[0][0]=='kdiag_write' and any(n in ['file_write','file_close','file_release','vfs_write','syscall_dispatch','interrupt_handler'] for n,o in resolved[1:])
     (d/'frames.json').write_text(json.dumps(resolved,indent=2)+'\n')
    else:
     ser.sendall(b'/bin/highheap'+(b' module' if a.work=='module' else b'')+b'\n')
     waitfor([b'HIGHHEAP_GUEST_PASS',b'HIGHHEAP_GUEST_FAIL',b'HIGHHEAP_MODULE_PASS',b'HIGHHEAP_MODULE_FAIL'])
     text=log.decode(errors='replace')
     marker='HIGHHEAP_MODULE_PASS' if a.work=='module' else 'HIGHHEAP_GUEST_PASS'
     ok=marker in text and '[highheap-module] PASS' in text and '[highheap] FAIL' not in text
     if a.work=='heap' and not a.expect_failure:
      cpus=re.findall(r'\[highheap-cpu\] cpu=(\d+) stack=(0x[0-9a-f]+) phys=(0x[0-9a-f]+) failures=0',text)
      ok=ok and len(cpus)==4 and len({c[0] for c in cpus})==4
      if ram=='8G':ok=ok and all(int(c[2],16)>=1<<32 for c in cpus) and bool(re.search(r'\[highheap-capacity\] bytes=1342176960\b',text))
    if a.expect_failure:ok=a.expect_failure in text and 'HIGHHEAP_GUEST_PASS' not in text
    item=dict(mode=mode,ram=ram,work=a.work,passed=bool(ok),expected_failure=a.expect_failure,log=str(d/'serial.log'))
    results.append(item);(a.out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(('PASS' if ok else 'FAIL'),mode,ram,a.work,flush=True)
    if not ok:raise RuntimeError('assertion failed')
   except Exception as e:raise SystemExit(f'FAIL {mode} {ram}: {e}; logs {d}')
   finally:
    if q.poll() is None:q.terminate()
    try:q.wait(timeout=5)
    except subprocess.TimeoutExpired:q.kill();q.wait()
    if ser:ser.close()
    err.close();(d/'serial.log').write_bytes(log)
