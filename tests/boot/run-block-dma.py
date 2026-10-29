#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real persistent byte checks for NVMe/AHCI, with independent disk/firmware copies.
Requires a WIDEVERIFY kernel and the wide-memory and storgate programs in disk.
Two fresh QEMU machines share each private disk, never a snapshot overlay.
"""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
from guest_memory import high_ram, ram_bytes, require_capacity, require_high_payload
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--disk',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--modes',default='bios,uefi');ap.add_argument('--drivers',default='nvme,ahci');ap.add_argument('--ram',default='8G');ap.add_argument('--timeout',type=int,default=240)
ap.add_argument('--cpu',default='max',help='QEMU CPU profile; recorded in command.json')
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
ram_bytes(a.ram)
for driver in a.drivers.split(','):
 for mode in a.modes.split(','):
  if driver not in ('nvme','ahci') or mode not in ('bios','uefi'):raise SystemExit('unknown driver/mode')
  d=a.out/(driver+'-'+mode+'-'+a.ram);d.mkdir(parents=True,exist_ok=True)
  disk=d/'disk.img';shutil.copyfile(a.disk,disk)
  swap=d/'swap.img'
  with swap.open('wb') as f:f.truncate(64<<20)
  for boot in (1,2):
   bd=d/('boot'+str(boot));bd.mkdir(exist_ok=True);log=bytearray();serial=None
   with tempfile.TemporaryDirectory(prefix='logit-dma-') as tmp:
    ser=tmp+'/serial'
    cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu',a.cpu,'-smp','4','-m',a.ram,'-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-drive',f'file={disk},format=raw,if=none,id=hd0','-drive',f'file={swap},format=raw,if=none,id=swp','-chardev',f'socket,id=ser0,path={ser},server=on,wait=on','-serial','chardev:ser0']
    if driver=='nvme':cmd+=['-device','nvme,drive=hd0,serial=dma-root','-device','virtio-blk-pci,drive=swp']
    else:cmd+=['-device','ich9-ahci,id=ahci0','-device','ide-hd,drive=hd0,bus=ahci0.0','-device','ide-hd,drive=swp,bus=ahci0.1']
    if mode=='bios':cmd+=['-cdrom',str(a.build/'logit.iso'),'-boot','d']
    else:
     var=bd/'vars.fd';shutil.copyfile(os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd'),var)
     esp=bd/'esp.img';shutil.copyfile(a.build/'esp.img',esp)
     cmd+=['-machine','q35','-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd'),'-drive',f'if=pflash,format=raw,file={var}','-drive',f'file={esp},format=raw,if=none,id=esp0']
     if driver=='nvme':cmd+=['-device','ich9-ahci,id=ahci0']
     cmd+=['-device','ide-hd,drive=esp0,bus=ahci0.2']
    (bd/'command.json').write_text(json.dumps(cmd,indent=2));err=(bd/'qemu.log').open('wb');p=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
    def waitfor(marker,start=0):
     until=time.monotonic()+a.timeout
     while marker not in log[start:] and time.monotonic()<until and p.poll() is None:
      if b'STORGATE-FAIL' in log or b'WIDE_MEMORY_GUEST_FAIL' in log:raise RuntimeError('guest assertion failed')
      time.sleep(.1)
     if marker not in log[start:]:raise RuntimeError('timeout waiting for '+repr(marker))
    try:
     serial=socket.socket(socket.AF_UNIX)
     for i in range(300):
      try:serial.connect(ser);break
      except OSError:
       if p.poll() is not None:raise RuntimeError('QEMU exited before serial')
       time.sleep(.1)
     def read():
      try:
       while True:
        b=serial.recv(65536)
        if not b:break
        log.extend(b);(bd/'serial.log').write_bytes(log)
      except OSError:pass
     threading.Thread(target=read,daemon=True).start();waitfor(b'LogitOS shell');time.sleep(.5)
     if boot==1:
      serial.sendall(b'/bin/wide-memory\n');waitfor(b'WIDE_MEMORY_GUEST_PASS')
      if b'[widecheck] FAIL' in log:raise RuntimeError('wide memory failed')
      require_capacity(log, a.ram)
      if b'PASS DMA mappings pins and bounce resources restore baseline' not in log:raise RuntimeError('DMA resource baseline not checked')
      if high_ram(a.ram) and b'PASS device completed high-page direct write and read' not in log:raise RuntimeError('high DMA completion counters not checked')
      serial.sendall(b'mkdir /sg\nas /usr/as/examples/storgate.as run\n');waitfor(b'STORGATE-RUN-DONE')
      if b'STORGATE-ok readonly ftruncate refused' not in log:raise RuntimeError('write/refusal checks did not execute')
     else:
      serial.sendall(b'as /usr/as/examples/storgate.as verify\n');waitfor(b'STORGATE-VERIFY-DONE')
      if b'STORGATE-ok image across reboot byte-exact' not in log:raise RuntimeError('persistent byte check did not execute')
     if b'STORGATE-FAIL' in log:raise RuntimeError('storage assertions failed')
     # Check selected root, rather than accepting any discovered controller.
     aliases=re.findall(rb'\[dma\] alloc '+driver.encode()+rb' cpu=(0x[0-9a-f]+) dma=(0x[0-9a-f]+)',log)
     if not aliases or not all(int(cpu,16)!=int(bus,16) for cpu,bus in aliases):raise RuntimeError('coherent CPU/DMA aliases not distinct')
     if high_ram(a.ram) and not any(int(bus,16)>=0x100000000 for cpu,bus in aliases):raise RuntimeError('no coherent DMA above 4 GiB')
     if boot==1 and high_ram(a.ram) and b'PASS page-cache physical page above 4 GiB' not in log:raise RuntimeError('high physical root read not verified')
     roots=re.findall(rb'\[blk\][^\r\n]*root = ([a-z0-9]+)[^\r\n]*',log)
     if len(roots)!=1 or not roots[0].startswith(driver.encode()):raise RuntimeError('expected root driver absent: '+repr(roots))
     if boot==1:
      transfers=re.findall(rb'\[dmacheck\] backend='+re.escape(roots[0])+rb' cpu=(0x[0-9a-f]+) dma=(0x[0-9a-f]+) bytes=(\d+) completed_high=(\d+) result=PASS',log)
      if len(transfers)!=1 or int(transfers[0][2])!=524288:raise RuntimeError('root disk 512 KiB byte transfer did not execute')
      require_high_payload(a.ram, int(transfers[0][1],16), int(transfers[0][3]), 1048576)
     (bd/'result.json').write_text(json.dumps({'pass':True,'driver':driver,'mode':mode,'ram':a.ram,'boot':boot,'root_lines':[x.decode(errors='replace') for x in roots]},indent=2))
     print('PASS',driver,mode,a.ram,'boot',boot,flush=True)
    except Exception as e:
     (bd/'result.json').write_text(json.dumps({'pass':False,'error':str(e)},indent=2));raise SystemExit(f'FAIL {driver} {mode} boot {boot}: {e}; {bd}')
    finally:
     if p.poll() is None:p.terminate()
     try:p.wait(timeout=10)
     except subprocess.TimeoutExpired:p.kill();p.wait()
     if serial:serial.close()
     err.close();(bd/'serial.log').write_bytes(log)
