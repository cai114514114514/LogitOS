#!/usr/bin/env python3
"""Real BIOS/UEFI 8G virtio DMA acceptance, followed by the existing NIC HTTP gate.

Uses supplied ISO/ESP and an immutable source disk with QEMU snapshot writes.
Checks queue address separation, actual device operations and desktop scanout;
coherent high-page allocation is never presented as streaming high-byte completion.
All assertions read guest/device output, never the commands sent by this harness.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from nic_http_result import parse_result

ROOT = Path(__file__).resolve().parents[2]

def qmp_request(stream, request):
    stream.write(json.dumps(request)+'\n');stream.flush()
    while True:
        line=stream.readline()
        if not line: raise RuntimeError('QMP disconnected')
        reply=json.loads(line)
        if 'error' in reply: raise RuntimeError(str(reply['error']))
        if 'return' in reply: return reply['return']

def boot(a, mode):
    out=a.out/(mode+'-8G');out.mkdir(parents=True,exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    (out/'serial.log').write_bytes(b'')
    log=bytearray();proc=None;serial=None;reader=None
    with tempfile.TemporaryDirectory(prefix='dma-vq-') as td:
        serial_path=td+'/serial';qmp_path=td+'/qmp'
        cmd=[a.qemu,'-cpu','max','-accel','tcg,thread=multi','-smp','4','-m','8G',
             '-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800',
             '-display','none','-no-reboot','-snapshot',
             '-drive',f'file={a.disk},format=raw,if=none,id=hd0,file.locking=off',
             '-device','virtio-blk-pci,drive=hd0','-device','virtio-rng-pci',
             '-device','virtio-balloon-pci,id=bal0','-netdev','user,id=n0',
             '-device','virtio-net-pci,netdev=n0',
             '-chardev',f'socket,id=ser0,path={serial_path},server=on,wait=on',
             '-serial','chardev:ser0','-qmp',f'unix:{qmp_path},server=on,wait=off']
        if mode=='bios':cmd+=['-cdrom',str(a.build/'logit.iso'),'-boot','d']
        else:
            code=Path(os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd'))
            varsrc=Path(os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd'))
            if not code.is_file() or not varsrc.is_file():raise RuntimeError('OVMF unavailable: set OVMF_CODE and OVMF_VARS_SRC')
            var=out/'vars.fd';shutil.copyfile(varsrc,var)
            # QEMU ide-hd rejects a read-only backing node; supply a private ESP
            # copy rather than weakening ownership of the build's source image.
            esp=out/'esp.img';shutil.copyfile(a.build/'esp.img',esp)
            cmd+=['-machine','q35','-drive',f'if=pflash,format=raw,readonly=on,file={code}',
                  '-drive',f'if=pflash,format=raw,file={var}',
                  '-device','ich9-ahci,id=ahci0','-drive',f'file={esp},format=raw,if=none,id=esp0',
                  '-device','ide-hd,drive=esp0,bus=ahci0.0']
        (out/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
        with (out/'qemu.log').open('wb') as err:
            proc=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
            def wait(marker, seconds=None, start=0):
                end=time.monotonic()+(seconds or a.timeout)
                while marker not in log[start:] and time.monotonic()<end and proc.poll() is None:time.sleep(.1)
                if marker not in log[start:]:raise RuntimeError(f'{mode}: missing {marker!r}; see {out}')
            try:
                serial=socket.socket(socket.AF_UNIX)
                end=time.monotonic()+20
                while True:
                    try:serial.connect(serial_path);break
                    except OSError:
                        if proc.poll() is not None or time.monotonic()>end:raise RuntimeError(f'QEMU serial unavailable; see {out}/qemu.log')
                        time.sleep(.1)
                def read():
                    try:
                        while True:
                            data=serial.recv(65536)
                            if not data:break
                            log.extend(data);(out/'serial.log').write_bytes(log)
                    except OSError:pass
                reader=threading.Thread(target=read,daemon=True);reader.start()
                wait(b'LogitOS shell');time.sleep(.5)
                # /tmp on older packed disks is a virtual directory which refuses
                # create/chmod. Use a known ordinary file ONLY inside the snapshot;
                # the source disk and the next mode's starting data remain intact.
                marker=('DMA_VIRTIO_DISK_'+mode.upper()).encode()
                start=len(log)
                serial.sendall(b'echo '+marker+b' > /docs/readme.txt\ncat /docs/readme.txt\n')
                # Full output line excludes the shell's echoed redirect command.
                wait(b'\n'+marker+b'\r\n',30,start)
                with socket.socket(socket.AF_UNIX) as qp:
                    qp.connect(qmp_path)
                    with qp.makefile('rw') as stream:
                        stream.readline();qmp_request(stream,{'execute':'qmp_capabilities'})
                        qmp_request(stream,{'execute':'screendump','arguments':{'filename':str(out/'desktop.ppm')}})
                        balloon=qmp_request(stream,{'execute':'query-balloon'})
                text=log.decode(errors='replace')
                def need(condition,why):
                    if not condition:raise RuntimeError(f'{mode}: {why}; see {out}')
                need('LOGIT_BOOT_OK' in text,'no boot marker')
                queues=re.findall(r'\[virtio-dma\] (\S+) queue=(\d+) cpu=(0x[0-9a-f]+) dma=(0x[0-9a-f]+) avail=(0x[0-9a-f]+) used=(0x[0-9a-f]+)',text)
                need(len(queues)==8,f'expected eight queues, got {len(queues)}')
                need(all(int(cpu,16)!=int(desc,16) and all(int(v,16)>=1<<32 for v in (desc,av,used)) for _,_,cpu,desc,av,used in queues),'queue DMA must be >4G and differ from CPU')
                rng=re.search(r'VIRTIO_RNG_SELFTEST \S+ a=([0-9a-f]{32}) b=([0-9a-f]{32}) zero_a=0 zero_b=0 same=0',text)
                need(rng is not None and rng[1]!=rng[2] and int(rng[1],16)!=0 and int(rng[2],16)!=0,'RNG independent reads failed')
                bal=re.search(r'VIRTIO_BALLOON_SELFTEST \S+ inflated=(\d+) deflated=(\d+) free_before=(\d+) free_after_inflate=(\d+) free_after_deflate=(\d+)',text)
                need(bal is not None,'balloon self-test missing')
                inf,deff,before,during,after=map(int,bal.groups())
                need(inf==deff==4 and before-during==4 and before==after,'balloon physical-page ownership did not roundtrip')
                need(balloon.get('actual')==8<<30,'QMP balloon size did not return to 8G')
                need('cursor plane yes' in text,'GPU cursor resource absent')
                need('[virtio-net] up:' in text and '[virtio-net] link: UP' in text,'virtio NIC not live')
                need(not re.search(r'VIRTIO_\w+.*FAIL|\[panic\]|KERNEL PANIC',text),'guest device failure')
                dma=re.search(r'\[dma\] boot .*',text)
                need(dma is not None,'DMA completion diagnostics absent')
                need('quarantine=0/0/0' in dma[0],'healthy boot quarantined DMA storage')
                counters={k:int(v) for k,v in re.findall(r'(completed_direct|completed_high|high)=(\d+)',dma[0])}
                need('completed_high' in counters and counters['completed_direct']>0,'streaming completion counters missing')
                # A captured black frame is not a functioning scanout. Inspect
                # raster bytes, not file existence or the serial GPU-ready line.
                ppm=(out/'desktop.ppm').read_bytes();parts=ppm.split(b'\n',3)
                need(len(parts)==4 and parts[0]==b'P6' and len(set(parts[3]))>64,'desktop scanout is empty or flat')
                result={'boot':mode,'ram':'8G','queues':len(queues),'all_queue_dma_above_4g':True,
                        'cpu_differs_from_dma':True,'filesystem_write_read':True,'rng':True,
                        'balloon_roundtrip':True,'qmp_balloon_actual':balloon['actual'],
                        'gpu_scanout':True,'cursor_resource':True,'nic_initialized':True,'dma':counters}
                (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
                print(f'PASS {mode} 8G: all eight high DMA queues, disk write/read, scanout, cursor, RNG, balloon; {counters}',flush=True)
                return result
            finally:
                if proc.poll() is None:proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
                if serial:serial.close()
                if reader:reader.join(timeout=2)
                (out/'serial.log').write_bytes(log)

def nic(a, mode):
    out=a.out/('nic-'+mode+'-8G');out.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ)
    env.update(QEMU=str(ROOT/'tests/boot/dma-qemu-wrapper.py'),DMA_TEST_BOOT=mode,
               DMA_TEST_RAM='8G',DMA_TEST_BUILD=str(a.build),DMA_TEST_OUT=str(out),DMA_TEST_QEMU=a.qemu)
    cmd=['bash',str(ROOT/'tests/boot/run-nic-test.sh'),str(a.build/'logit.iso'),str(a.disk),'virtio-net-pci','virtio-net']
    r=subprocess.run(cmd,cwd=ROOT,env=env,text=True,capture_output=True,timeout=a.timeout)
    (out/'gate.log').write_text(r.stdout+r.stderr)
    print(r.stdout,end='',flush=True)
    if r.returncode:raise RuntimeError(f'{mode} virtio-net HTTP gate failed; see {out}/gate.log')
    # The existing gate checks length. Verify its guest-computed hash too, so a
    # broken payload cannot pass merely by delivering the expected byte count.
    text=(out/'serial.log').read_text(errors='replace')
    try: parsed=parse_result(text)
    except ValueError as e:raise RuntimeError(f'{mode}: {e}; see {out}') from e
    digest=parsed['fnv1a']
    result={'boot':mode,'ram':'8G','driver':'virtio-net','http_bytes':32768,'fnv1a':f'{digest:08x}'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True);p.add_argument('--disk',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--modes',default='bios,uefi')
    p.add_argument('--timeout',type=int,default=240);p.add_argument('--skip-nic',action='store_true')
    p.add_argument('--qemu',default=os.environ.get('DMA_TEST_QEMU','qemu-system-x86_64'))
    a=p.parse_args();a.build=a.build.resolve();a.disk=a.disk.resolve();a.out=a.out.resolve()
    a.out.mkdir(parents=True,exist_ok=True);modes=a.modes.split(',')
    (a.out/'result.json').unlink(missing_ok=True)
    if any(m not in ('bios','uefi') for m in modes):p.error('--modes accepts bios,uefi')
    results={'devices':[],'network':[]}
    for mode in modes:
        results['devices'].append(boot(a,mode))
        if not a.skip_nic:results['network'].append(nic(a,mode))
    (a.out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
    print('DMA_VIRTIO_GUEST_PASS',flush=True)
if __name__=='__main__':main()
