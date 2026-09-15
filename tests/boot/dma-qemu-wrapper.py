#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Adapter for existing guest gates. Required: DMA_TEST_BUILD (ISO/ESP directory),
# DMA_TEST_OUT (private artifacts). Optional: DMA_TEST_BOOT=bios|uefi,
# DMA_TEST_RAM=8G, DMA_TEST_QEMU, OVMF_CODE, OVMF_VARS_SRC. Set QEMU to this file.
# Hardware assertions remain in the original gate; this only changes boot/RAM
# parameters and retains serial, QEMU arguments, stderr, and WAV artifacts.
import os,sys,subprocess,signal,json,shutil
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"qmp"))
from owned_process import stop_owned
d=Path(os.environ['DMA_TEST_OUT']).resolve();d.mkdir(parents=True,exist_ok=True)
build=Path(os.environ['DMA_TEST_BUILD']).resolve()
a=sys.argv[1:];a[a.index('-m')+1]=os.environ.get('DMA_TEST_RAM','8G')
if os.environ.get('DMA_TEST_BOOT','bios')=='uefi':
 for opt in ['-cdrom','-boot']:
  if opt in a:i=a.index(opt);del a[i:i+2]
 if '-machine' in a:
  i=a.index('-machine');a[i+1]=a[i+1].replace('pc,','q35,')
 else:a+=['-machine','q35']
 shutil.copyfile(os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd'),d/'vars.fd')
 shutil.copyfile(build/'esp.img',d/'esp.img')
 a+=['-drive','if=pflash,format=raw,readonly=on,file='+os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd'),'-drive',f'if=pflash,format=raw,file={d}/vars.fd','-device','ich9-ahci,id=ahci_dma','-drive',f'file={d}/esp.img,format=raw,if=none,id=esp_dma','-device','ide-hd,drive=esp_dma,bus=ahci_dma.0']
cmd=[os.environ.get('DMA_TEST_QEMU',shutil.which('qemu-system-x86_64') or '/opt/homebrew/bin/qemu-system-x86_64')]+a
(d/'command.json').write_text(json.dumps(cmd,indent=2))
f=(d/'serial.log').open('wb');err=(d/'qemu.log').open('wb')
p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=err)
def stop(sig,frame):
 # Finish our child before the outer USB owner escalates against us.
 stop_owned(p,timeout=3,sig=sig)
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
try:
 while True:
  b=os.read(p.stdout.fileno(),65536)
  if not b:break
  f.write(b);f.flush()
  try:os.write(sys.stdout.fileno(),b)
  except BrokenPipeError:pass
 rc=p.wait()
finally:
 stop_owned(p,timeout=3)
 p.stdout.close();f.close();err.close()
if '-audiodev' in a:
 desc=a[a.index('-audiodev')+1]
 for arg in desc.split(','):
  if arg.startswith('path=') and Path(arg[5:]).exists():shutil.copyfile(arg[5:],d/'audio.wav')
sys.exit(rc)
