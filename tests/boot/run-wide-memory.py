#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot the actual LogitOS ABI tests with private disks and bounded deadlines.
Require an opt-in WIDEVERIFY kernel, a disk containing /bin/wide-memory,
/bin/pie and /bin/pie-aex, and BIOS ISO plus UEFI ESP from the same build.
"""
import argparse,json,os,re,shutil,socket,subprocess,tempfile,threading,time
from pathlib import Path
from guest_memory import ram_bytes, require_capacity
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build',type=Path,required=True);ap.add_argument('--disk',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--modes',default='bios,uefi');ap.add_argument('--ram',default='512M,2G,8G');ap.add_argument('--skip-pie',action='store_true')
ap.add_argument('--cpu',default='max',help='QEMU CPU profile; recorded in command.json')
ap.add_argument('--expect-failure',action='store_true');ap.add_argument('--timeout',type=int,default=240)
a=ap.parse_args();a.out.mkdir(parents=True,exist_ok=True)
for ram in a.ram.split(','): ram_bytes(ram)
for mode in a.modes.split(','):
 for ram in a.ram.split(','):
    d=a.out/(mode+'-'+ram);d.mkdir(parents=True,exist_ok=True)
    disk=d/'disk.img';shutil.copyfile(a.disk,disk)
    swap=d/'swap.img'
    with swap.open('wb') as f:f.truncate(64<<20)
    log=bytearray();qp=None;serial=None
    with tempfile.TemporaryDirectory(prefix='logit-mm-') as tmp:
      ser=tmp+'/serial';qmp=tmp+'/qmp'
      cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu',a.cpu,'-smp','4','-m',ram,'-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800','-display','none','-no-reboot','-drive',f'file={disk},format=raw,if=none,id=hd0','-device','virtio-blk-pci,drive=hd0','-drive',f'file={swap},format=raw,if=none,id=swp','-device','nvme,drive=swp,serial=wide-swap','-chardev',f'socket,id=ser0,path={ser},server=on,wait=on','-serial','chardev:ser0','-qmp',f'unix:{qmp},server,nowait']
      if mode=='bios':cmd+=['-cdrom',str(a.build/'logit.iso'),'-boot','d']
      elif mode=='uefi':
        code=os.environ.get('OVMF_CODE','/opt/homebrew/share/qemu/edk2-x86_64-code.fd')
        varsrc=os.environ.get('OVMF_VARS_SRC','/opt/homebrew/share/qemu/edk2-i386-vars.fd')
        var=d/'vars.fd';shutil.copyfile(varsrc,var)
        esp=d/'esp.img';shutil.copyfile(a.build/'esp.img',esp)
        cmd+=['-machine','q35','-drive',f'if=pflash,format=raw,readonly=on,file={code}','-drive',f'if=pflash,format=raw,file={var}','-device','ich9-ahci,id=ahci0','-drive',f'file={esp},format=raw,if=none,id=esp0','-device','ide-hd,drive=esp0,bus=ahci0.0']
      else:raise SystemExit('unknown mode '+mode)
      (d/'command.json').write_text(json.dumps(cmd,indent=2))
      err=(d/'qemu.log').open('wb');p=subprocess.Popen(cmd,stdout=err,stderr=subprocess.STDOUT)
      def waitfor(marker,seconds):
        until=time.monotonic()+seconds
        while marker not in log and time.monotonic()<until:
          if p.poll() is not None:break
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
              log.extend(b)
              (d/'serial.log').write_bytes(log)
          except OSError:pass
        threading.Thread(target=read,daemon=True).start()
        waitfor(b'LogitOS shell',a.timeout);time.sleep(.5)
        serial.sendall(b'/bin/wide-memory\n')
        # Match final marker, never the echoed command or a boot capacity line.
        until=time.monotonic()+a.timeout
        while b'WIDE_MEMORY_GUEST_PASS' not in log and b'WIDE_MEMORY_GUEST_FAIL' not in log and time.monotonic()<until and p.poll() is None:time.sleep(.1)
        failed=b'WIDE_MEMORY_GUEST_FAIL' in log
        if a.expect_failure:
          if not failed or b'[widecheck] FAIL anonymous physical page above' not in log:raise RuntimeError('high-map control did not fail the real physical-page assertion')
        else:
          if b'WIDE_MEMORY_GUEST_PASS' not in log or b'[widecheck] FAIL' in log:raise RuntimeError('wide-memory acceptance failed')
          capacity = require_capacity(log, ram)
          (d/'capacity.json').write_text(json.dumps(capacity, indent=2)+'\n')
          if not a.skip_pie:
            for program in ('pie','pie-aex'):
              start=len(log);serial.sendall(('export PIE_TEST=yes\n/bin/'+program+'\n').encode())
              until=time.monotonic()+a.timeout
              while b'PIE_PROGRAM_PASS' not in log[start:] and b'PIE_PROGRAM_FAIL' not in log[start:] and time.monotonic()<until and p.poll() is None:time.sleep(.1)
              part=bytes(log[start:]);biases=re.findall(rb'PIE_BIAS (0x[0-9a-f]+)',part)
              if b'PIE_PROGRAM_PASS' not in part or b'PIE_CHILD_PASS' not in part or b'PIE_CAP_PASS' not in part or len(set(biases))<2:raise RuntimeError(program+' two-base/TLS execution failed')
          serial.sendall(b'echo LEGACY_CLI_PASS\n/bin/true\n')
          waitfor(b'LEGACY_CLI_PASS\r\n',20)
        # Retain a desktop capture as a boot/regression artifact.
        qp=socket.socket(socket.AF_UNIX);qp.connect(qmp);qf=qp.makefile('rw');qf.readline()
        for request in ({'execute':'qmp_capabilities'},{'execute':'screendump','arguments':{'filename':str((d/'desktop.ppm').resolve())}}):
          qf.write(json.dumps(request)+'\n');qf.flush()
          while True:
            reply=json.loads(qf.readline())
            if 'return' in reply or 'error' in reply:break
          if 'error' in reply:raise RuntimeError(str(reply))
        print('PASS',mode,ram,'control' if a.expect_failure else 'memory'+('' if a.skip_pie else '+PIE'),flush=True)
      except Exception as e:
        raise SystemExit(f'FAIL {mode} {ram}: {e}; logs: {d}')
      finally:
        if qp:qp.close()
        if p.poll() is None:p.terminate()
        try:p.wait(timeout=10)
        except subprocess.TimeoutExpired:p.kill();p.wait()
        if serial:serial.close()
        err.close();(d/'serial.log').write_bytes(log)
    disk.unlink();swap.unlink()
