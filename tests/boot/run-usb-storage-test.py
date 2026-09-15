#!/usr/bin/env python3
"""Two cold boots of actual usb-storage: partition writes, flush and byte equality.

Only a new private USB image is writable. Guest drives all payload I/O; host
supplies marker/partition metadata and observes guest evidence. --ram 16G also
requires high physical payloads and a reported actual HCD DMA address/path.
"""
import argparse,json,re,struct,subprocess,time,tempfile
from pathlib import Path
from guest_memory import high_ram

root=Path(__file__).resolve().parents[2]
header=(root/'tests/unit/usb_storage_fixture.h').read_text()
constants={k:int(v) for k,v in re.findall(r'^#define (USB_TEST_\w+) ([0-9]+)$',header,re.M)}
magic=re.search(r'^#define USB_TEST_MAGIC "([^"]+)"$',header,re.M)[1].encode()+b'\0'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',type=Path,required=True);p.add_argument('--disk',type=Path,required=True)
p.add_argument('--out',type=Path,required=True);p.add_argument('--controller',choices=('xhci','ehci','dual'),required=True)
p.add_argument('--ram',default='1G');p.add_argument('--cpu',default='max');p.add_argument('--qemu',default='qemu-system-x86_64');p.add_argument('--timeout',type=int,default=150)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
c=constants;controllers=['xhci','ehci'] if a.controller=='dual' else [a.controller]
media_dir=Path(tempfile.mkdtemp(prefix='private-media-',dir=a.out))
testdisks={name:media_dir/f'private-usb-{name}.img' for name in controllers}
# Exclusive creation prevents overwriting any existing image, including a
# previous interrupted run that a developer may need to inspect.
for identity,testdisk in enumerate(testdisks.values()):
 with testdisk.open('xb') as f:
  f.truncate(c['USB_TEST_SECTORS']*512)
  mbr=bytearray(512);mbr[446+4]=0x83
  struct.pack_into('<II',mbr,446+8,c['USB_TEST_PART_START'],c['USB_TEST_PART_SECTORS']);mbr[510:512]=b'\x55\xaa'
  f.write(mbr);f.seek(c['USB_TEST_PART_START']*512);f.write(magic)
  f.seek(c['USB_TEST_PART_START']*512+c['USB_TEST_ID_OFFSET']);f.write(bytes([identity]))
  f.seek((c['USB_TEST_PART_START']-1)*512);f.write(bytes([c['USB_TEST_GUARD_PRE']])*512)
  f.seek((c['USB_TEST_PART_START']+c['USB_TEST_DATA_START']+c['USB_TEST_DATA_SECTORS'])*512)
  f.write(bytes([c['USB_TEST_GUARD_POST']])*512)
results=[]
for boot in (1,2):
 folder=a.out/f'boot{boot}';folder.mkdir(exist_ok=True)
 cmd=[a.qemu,'-cpu',a.cpu,'-m',a.ram,'-smp','4','-accel','tcg,thread=multi',
      '-cdrom',str(a.iso.resolve()),'-boot','d','-display','none','-vga','none','-device','virtio-gpu-pci',
      '-drive',f'file={a.disk.resolve()},format=raw,if=none,id=root,snapshot=on,file.locking=off',
      '-device','virtio-blk-pci,drive=root','-net','none',
      '-serial','stdio','-no-reboot']
 for name,testdisk in testdisks.items():
  cmd+=['-drive',f'file={testdisk},format=raw,if=none,id=usb-{name},cache=writeback',
        '-device',('qemu-xhci,id=xhci' if name=='xhci' else 'usb-ehci,id=ehci'),
        '-device',f'usb-storage,bus={name}.0,drive=usb-{name}']
 (folder/'command.json').write_text(json.dumps(cmd,indent=2))
 with (folder/'serial.log').open('wb') as log,(folder/'qemu.log').open('wb') as err:
  guest=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=log,stderr=err)
  try:
   deadline=time.monotonic()+a.timeout
   while time.monotonic()<deadline:
    text=(folder/'serial.log').read_text(errors='replace')
    match=re.search(r'^USB_MSC_GUEST_RESULT phase=(\d+) checks=(\d+) failed=(\d+)\r?$',text,re.M)
    if match:
     if int(match[1])!=boot or int(match[3]) or ' FAIL' in '\n'.join(x for x in text.splitlines() if x.startswith('USB_MSC_GUEST_CHECK')):
      raise RuntimeError(f'guest boot {boot} rejected USB storage; see {folder}/serial.log')
     if 'LOGIT_BOOT_OK' in text:break
    if guest.poll() is not None:raise RuntimeError(f'QEMU exited before guest result; see {folder}')
    time.sleep(.1)
   else:raise RuntimeError(f'no complete USB guest evidence within {a.timeout}s; see {folder}')
   assert '[usb-msc] disk=usb0' in text and "bound to driver 'usb-storage'" in text
   disk_results=re.findall(r'USB_MSC_DISK_RESULT disk=(usb\d+) identity=(\d+) phase=(\d+) checks=(\d+) failed=(\d+)',text)
   assert len(disk_results)==len(controllers) and {int(x[1]) for x in disk_results}==set(range(len(controllers)))
   assert all(int(x[2])==boot and int(x[3])==15 and int(x[4])==0 for x in disk_results)
   if boot==2:assert text.count('USB_MSC_GUEST_CHECK cold-boot-retained-every-written-byte PASS')==len(controllers)
   payloads=re.findall(r'USB_MSC_DMA_BUFFER write=(0x[0-9a-f]+) read=(0x[0-9a-f]+) cpu_write=(0x[0-9a-f]+) bytes=(\d+) high_required=(\d+)',text)
   assert len(payloads)==len(controllers) and all(int(x[3])==c['USB_TEST_DATA_SECTORS']*512 for x in payloads)
   physical=[int(x[i],16) for x in payloads for i in (0,1)]
   if high_ram(a.ram):assert all(x[4]=='1' for x in payloads) and min(physical)>0xffffffff
   addresses={}
   if 'xhci' in controllers:
    dma=set(re.findall(r'USB_MSC_XHCI_DMA ep=\d+ dma=(0x[0-9a-f]+) mask=(0x[0-9a-f]+)',text))
    assert len(dma)==2 and all(int(addr,16)<=int(mask,16) for addr,mask in dma)
    addresses['xhci']=[int(x[0],16) for x in sorted(dma)]
   if 'ehci' in controllers:
    addrs=[int(x,16) for x in re.findall(r'USB_BULK_DMA hc=ehci dma=(0x[0-9a-f]+) bytes=\d+',text)]
    assert addrs and max(addrs)<=0xffffffff
    addresses['ehci']=addrs
   result={'boot':boot,'checks':int(match[2]),'bytes':c['USB_TEST_DATA_SECTORS']*512*len(controllers),'payload_phys':physical,
           'hcd_dma':addresses,'high_payload_required':high_ram(a.ram),'controller':a.controller,'ram':a.ram,'cpu':a.cpu,'media':disk_results,
           'private_images':{name:str(path) for name,path in testdisks.items()}}
   results.append(result);(folder/'result.json').write_text(json.dumps(result,indent=2))
   print(f'PASS usb-storage {a.controller} cold boot {boot}: {result["bytes"]} bytes, flush, partition guards, {result["checks"]} guest checks',flush=True)
  finally:
   guest.terminate()
   try:guest.wait(timeout=5)
   except subprocess.TimeoutExpired:guest.kill();guest.wait()
(a.out/'result.json').write_text(json.dumps(results,indent=2))
print(f'PASS usb-storage persistent two-cold-boot result: {a.out}',flush=True)
