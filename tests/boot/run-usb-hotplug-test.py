#!/usr/bin/env python3
"""QMP root-port hotplug over xHCI and EHCI; this is emulated, not bare metal.

The controller starts with no USB function attached.  Only after LOGIT_BOOT_OK
does QMP attach a private MBR disk.  Guest USB/SCSI/partition logs must appear,
then QMP removes it and the guest class driver must offline the medium.
"""
import argparse,hashlib,json,re,socket,struct,subprocess,tempfile,time
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',required=True,type=Path);p.add_argument('--disk',required=True,type=Path)
p.add_argument('--out',required=True,type=Path);p.add_argument('--qemu',default='qemu-system-x86_64')
p.add_argument('--controller',choices=('both','xhci','ehci'),default='both')
p.add_argument('--timeout',type=int,default=150)
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1<<20),b''):h.update(block)
    return h.hexdigest()

def wait_text(path,needle,deadline,proc):
    while time.monotonic()<deadline:
        text=path.read_text(errors='replace') if path.exists() else ''
        if needle in text:return text
        if proc.poll() is not None:raise RuntimeError(f'QEMU exited while waiting for {needle!r}')
        time.sleep(.05)
    raise RuntimeError(f'timeout waiting for {needle!r}; see {path}')

def qmp_connect(path,deadline):
    while time.monotonic()<deadline:
        s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        try:s.connect(str(path));f=s.makefile('rw');json.loads(f.readline());return s,f
        except (FileNotFoundError,ConnectionRefusedError):s.close();time.sleep(.05)
    raise RuntimeError('QMP socket did not appear')

def qcmd(f,name,args=None):
    req={'execute':name}
    if args is not None:req['arguments']=args
    f.write(json.dumps(req)+'\n');f.flush()
    while True:
        line=f.readline()
        if not line:raise RuntimeError('QMP disconnected')
        value=json.loads(line)
        if 'error' in value:raise RuntimeError(f'QMP {name}: {value["error"]}')
        if 'return' in value:return value['return']

results=[]
controllers=('xhci','ehci') if a.controller=='both' else (a.controller,)
for controller in controllers:
    folder=a.out/controller;folder.mkdir(exist_ok=True)
    media=folder/'private-hotplug.img';serial=folder/'serial.log';qemu_log=folder/'qemu.log'
    # A rerun must never satisfy assertions from stale serial or result data.
    for old in (media,serial,qemu_log,folder/'result.json',folder/'failure.json'):
        old.unlink(missing_ok=True)
    with media.open('xb') as f:
        f.truncate(65536*512)
        mbr=bytearray(512);mbr[446+4]=0x83
        struct.pack_into('<II',mbr,446+8,2048,8192);mbr[510:512]=b'\x55\xaa';f.write(mbr)
    # Keep QMP's AF_UNIX pathname below the platform limit even when BUILD is
    # an intentionally descriptive, deeply nested directory.
    with tempfile.TemporaryDirectory(prefix='lusb-') as td:
        qsock=Path(td)/'qmp.sock'
        hcd='qemu-xhci,id=xhci' if controller=='xhci' else 'usb-ehci,id=ehci'
        cmd=[a.qemu,'-cpu','max','-m','1G','-smp','4','-accel','tcg,thread=multi',
             '-cdrom',str(a.iso.resolve()),'-boot','d','-display','none','-vga','none',
             '-device','virtio-gpu-pci',
             '-drive',f'file={a.disk.resolve()},format=raw,if=none,id=root,snapshot=on,file.locking=off',
             '-device','virtio-blk-pci,drive=root','-device',hcd,
             '-drive',f'file={media},format=raw,if=none,id=hotdisk,cache=writeback,file.locking=off',
             '-net','none','-serial',f'file:{serial}','-qmp',f'unix:{qsock},server=on,wait=off',
             '-no-reboot']
        (folder/'command.json').write_text(json.dumps(cmd,indent=2))
        with qemu_log.open('wb') as err:
            proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=err)
        sock=qfile=None
        try:
            deadline=time.monotonic()+a.timeout
            sock,qfile=qmp_connect(qsock,deadline);qcmd(qfile,'qmp_capabilities')
            text=wait_text(serial,'LOGIT_BOOT_OK',deadline,proc)
            if 'USB_READY devices=0' not in text:
                raise RuntimeError(f'{controller}: controller did not begin with an empty root bus')
            # Let QEMU select a speed-compatible receptacle, then assert the
            # exact root-port number reported by the guest on disconnect.
            qcmd(qfile,'device_add',{'driver':'usb-storage','id':'hotusb','drive':'hotdisk',
                                     'bus':controller+'.0'})
            usb_tree=qcmd(qfile,'human-monitor-command',{'command-line':'info usb'})
            (folder/'qemu-usb.txt').write_text(usb_tree)
            text=wait_text(serial,' connected result=online',deadline,proc)
            match=re.search(r'USB_HOTPLUG hc=0 port=(\d+) connected result=online',text)
            if not match:raise RuntimeError(f'{controller}: online record did not name a root port')
            root_port=int(match.group(1))
            required=("bound to driver 'usb-storage'",'[usb-msc] disk=usb0',
                      '[part] usb0: MBR','usb0p1')
            if any(x not in text for x in required):
                raise RuntimeError(f'{controller}: hot-added medium was not read and published')
            qcmd(qfile,'device_del',{'id':'hotusb'})
            text=wait_text(serial,f'USB_HOTPLUG hc=0 port={root_port} disconnected removed=1',deadline,proc)
            if '[usb-msc] interface=0 offline' not in text:
                raise RuntimeError(f'{controller}: disconnect did not offline USB block class')
            result={'controller':controller,'qemu_accel':'tcg','root_port':root_port,
                    'post_boot_attach':True,
                    'scsi_capacity_and_mbr_read':True,'partition_published':'usb0p1',
                    'disconnect_removed_devices':1,'block_class_offline':True,
                    'iso_sha256':digest(a.iso),'disk_sha256':digest(a.disk),
                    'private_media_sha256':digest(media)}
            (folder/'result.json').write_text(json.dumps(result,indent=2));results.append(result)
            print(f'PASS usb-hotplug {controller}: post-boot MBR disk online, then offline',flush=True)
        except Exception as exc:
            failure={'controller':controller,'qemu_accel':'tcg','passed':False,
                     'error':str(exc),'iso_sha256':digest(a.iso),'disk_sha256':digest(a.disk),
                     'serial_log':str(serial),'qemu_log':str(qemu_log)}
            (folder/'failure.json').write_text(json.dumps(failure,indent=2))
            raise
        finally:
            if qfile:qfile.close()
            if sock:sock.close()
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
(a.out/'result.json').write_text(json.dumps(results,indent=2))
print('PASS usb-hotplug: '+', '.join(x['controller'] for x in results)+' root-port lifecycle',flush=True)
