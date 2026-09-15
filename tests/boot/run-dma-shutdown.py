#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Terminal real DMA device removal gate, with private disks and a serial result.
Requires the WIDEVERIFY selector 7 and /bin/dma-shutdown. GPU and root disk are
removed, so PASS is printed by the kernel and QEMU is ended without another task.
"""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--disk',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--modes',default='bios,uefi');ap.add_argument('--timeout',type=int,default=240)
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
for mode in a.modes.split(','):
 d=a.out/(mode+'-8G');d.mkdir(parents=True,exist_ok=True)
 disk=d/'disk.img';shutil.copyfile(a.disk,disk)
 for name in ('nvme','ahci'):
  with (d/(name+'.img')).open('wb') as f:f.truncate(64<<20)
 log=bytearray();serial=None
 with tempfile.TemporaryDirectory(prefix='logit-stop-') as tmp:
  ser=tmp+'/serial'
  cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-smp','4','-m','8G','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-drive',f'file={disk},format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-drive',f'file={d}/nvme.img,format=raw,if=none,id=nv0','-device','nvme,drive=nv0,serial=shutdown-scratch','-device','ich9-ahci,id=ahci0','-drive',f'file={d}/ahci.img,format=raw,if=none,id=ah0','-device','ide-hd,drive=ah0,bus=ahci0.0','-device','qemu-xhci,id=xhci','-device','usb-kbd,bus=xhci.0','-device','usb-mouse,bus=xhci.0','-audiodev','none,id=snd0','-device','intel-hda','-device','hda-duplex,audiodev=snd0','-object','rng-random,filename=/dev/urandom,id=rng0','-device','virtio-rng-pci,rng=rng0','-device','virtio-balloon-pci','-chardev',f'socket,id=ser0,path={ser},server=on,wait=on','-serial','chardev:ser0']
  for n,model in enumerate(('e1000','rtl8139','virtio-net-pci')):cmd+=['-netdev',f'user,id=n{n},restrict=on','-device',f'{model},netdev=n{n}']
  if mode=='bios':cmd+=['-cdrom',str(a.build/'logit.iso'),'-boot','d']
  elif mode=='uefi':
   var=d/'vars.fd';shutil.copyfile(os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd'),var)
   esp=d/'esp.img';shutil.copyfile(a.build/'esp.img',esp)
   cmd+=['-machine','q35','-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd'),'-drive',f'if=pflash,format=raw,file={var}','-drive',f'file={esp},format=raw,if=none,id=esp0','-device','ide-hd,drive=esp0,bus=ahci0.1']
  else:raise SystemExit('unknown mode '+mode)
  (d/'command.json').write_text(json.dumps(cmd,indent=2));err=(d/'qemu.log').open('wb');p=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
  def waitfor(marker):
   until=time.monotonic()+a.timeout
   while marker not in log and time.monotonic()<until and p.poll() is None:
    if b'DMA_SHUTDOWN_FAIL' in log:raise RuntimeError('kernel shutdown assertions failed')
    time.sleep(.1)
   if marker not in log:raise RuntimeError('timeout waiting for '+repr(marker))
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
      log.extend(b);(d/'serial.log').write_bytes(log)
    except OSError:pass
   threading.Thread(target=read,daemon=True).start();waitfor(b'LogitOS shell');time.sleep(.5)
   serial.sendall(b'/bin/dma-shutdown\n');waitfor(b'DMA_SHUTDOWN_PASS')
   removed=re.findall(rb'DMA_SHUTDOWN_REMOVE device=\S+ driver=(\S+) stage=(\d)',log)
   required={b'xhci',b'hda',b'e1000',b'rtl8139',b'virtio-net',b'virtio-rng',b'virtio-balloon',b'virtio-gpu',b'virtio-blk',b'nvme',b'ahci'}
   missing=required-{name for name,stage in removed}
   if missing:raise RuntimeError('required live DMA drivers not removed: '+repr(missing))
   order=[int(stage) for name,stage in removed]
   if order!=sorted(order):raise RuntimeError('remove stages out of order')
   if b'DMA_SHUTDOWN_FAIL' in log:raise RuntimeError('kernel shutdown failure')
   (d/'result.json').write_text(json.dumps({'pass':True,'mode':mode,'ram':'8G','removed':[name.decode() for name,stage in removed]},indent=2))
   print('PASS',mode,'8G DMA normal shutdown',flush=True)
  except Exception as e:
   (d/'result.json').write_text(json.dumps({'pass':False,'error':str(e)},indent=2));raise SystemExit(f'FAIL {mode}: {e}; {d}')
  finally:
   if p.poll() is None:p.terminate()
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:p.kill();p.wait()
   if serial:serial.close()
   err.close();(d/'serial.log').write_bytes(log)
