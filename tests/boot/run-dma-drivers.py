#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""BIOS/UEFI 8 GiB driver gates, including poisoned real HDA capture buffers.

Build the image with DMA_CAPTURE_VERIFY=1, plus dma-driver-capture-disk. Ordinary
images are rejected: initially zero capture memory cannot establish DMA writes.
The supplied --disk drives NIC/USB/playback; BUILD/capture-disk.img adds only the
capture fixture to the product file list. Each run uses QEMU snapshot/private ESP.
"""
import argparse,json,os,re,subprocess,sys,threading,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"qmp"))
from owned_process import run_owned
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--disk',type=Path,required=True)
p.add_argument('--out',type=Path,required=True);p.add_argument('--modes',default='bios,uefi')
p.add_argument('--only',default='e1000,rtl8139,usb,hda,capture')
a=p.parse_args();r=Path(__file__).resolve().parents[2];a.build=a.build.resolve();a.disk=a.disk.resolve();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
if 'capture' in a.only.split(','):
 if b'HDA_CAPTURE_CANARY dma=' not in (a.build/'kernel.elf').read_bytes():
  raise SystemExit('FAIL: capture requires an image built with DMA_CAPTURE_VERIFY=1')
 if not (a.build/'capture-disk.img').exists():
  raise SystemExit('FAIL: build dma-driver-capture-disk first')
results=[]
for mode in a.modes.split(','):
 for kind in a.only.split(','):
  d=a.out/(mode+'-'+kind);d.mkdir(parents=True,exist_ok=True)
  env=dict(os.environ,QEMU=str(r/'tests/boot/dma-qemu-wrapper.py'),DMA_TEST_BOOT=mode,DMA_TEST_RAM='8G',DMA_TEST_BUILD=str(a.build),DMA_TEST_OUT=str(d))
  if kind in ('e1000','rtl8139'):
   cmd=['bash','tests/boot/run-nic-test.sh',str(a.build/'logit.iso'),str(a.disk),kind,kind]
  elif kind=='usb':
   # Execute the original assertions unchanged and retain their serial socket log.
   driver=r/'tests/qmp/qmp_usb.py';nsloader=d/'usb-instrument.py'
   nsloader.write_text('import os,sys\nns={"__file__":'+repr(str(driver))+'}\ntry:\n exec(compile(open('+repr(str(driver))+').read(),'+repr(str(driver))+',"exec"),ns)\nfinally:\n open('+repr(str(d/'usb-serial.log'))+',"w").write(ns.get("log",""))\n')
   cmd=['python3',str(nsloader),str(a.build/'logit.iso'),str(a.disk)]
  elif kind=='hda':cmd=['bash','tests/boot/run-audio-wav-test.sh',str(a.build/'logit.iso'),str(a.disk),'ramp']
  elif kind=='capture':
   cmd=[env['QEMU'],'-cpu',env.get('QEMU_CPU','max'),'-cdrom',str(a.build/'logit.iso'),'-drive',f'file={a.build}/capture-disk.img,format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-boot','d','-snapshot','-m','8G','-smp','4','-accel','tcg,thread=multi','-vga','none','-device','virtio-gpu-pci','-audiodev','none,id=snd0','-device','intel-hda','-device','hda-duplex,audiodev=snd0','-serial','stdio','-display','none','-no-reboot']
  else:raise SystemExit('unknown driver '+kind)
  if kind!='capture':
   with (d/'gate.log').open('wb') as f:
    # Let USB unwind its own Popen scope on timeout; SIGKILLing the Python
    # instrument loses its QEMU child. Other gate ownership is unchanged.
    runner=run_owned if kind=='usb' else subprocess.run
    result=runner(cmd,cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180)
   if result.returncode:raise SystemExit(f'FAIL: {mode} {kind}; see {d}/gate.log')
  else:
   log=bytearray();process=subprocess.Popen(cmd,cwd=r,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
   def read():
    while True:
     data=os.read(process.stdout.fileno(),65536)
     if not data:break
     log.extend(data);(d/'gate.log').write_bytes(log)
   thread=threading.Thread(target=read,daemon=True);thread.start()
   def wait(marker,seconds=90):
    until=time.monotonic()+seconds
    while marker not in log and time.monotonic()<until and process.poll() is None:time.sleep(.1)
    if marker not in log:raise RuntimeError('missing '+repr(marker))
   try:
    wait(b'LogitOS shell');process.stdin.write(b'/bin/dma-capture\n');process.stdin.flush()
    wait(b'DMA_CAPTURE_PASS',30)
    text=bytes(log)
    if b'DMA_CAPTURE_FAIL' in text or b'HDA_CAPTURE_STOP_FAIL' in text:raise RuntimeError('capture or stop assertion failed')
    if text.count(b'HDA_CAPTURE_STOP_PASS')!=2:raise RuntimeError('two normal-close hardware stops were not observed')
    if b'HDA_REMOVE_PASS' not in text:raise RuntimeError('normal HDA driver remove did not release DMA and reject opens')
    addresses=re.findall(rb'HDA_CAPTURE_CANARY dma=(0x[0-9a-f]+)',text)
    if len(addresses)!=2 or any(int(v,16)<=0xffffffff for v in addresses):raise RuntimeError('both capture runs must DMA above 4 GiB')
   finally:
    process.terminate();process.wait(timeout=10);thread.join(timeout=2)
  result={'boot':mode,'ram':'8G','driver':kind,'status':'PASS','artifacts':str(d)}
  results.append(result);print(json.dumps(result),flush=True)
  (a.out/'results.json').write_text(json.dumps(results,indent=2))
