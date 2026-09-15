#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real model smoke in a private guest; no generated text is substituted by the harness.

The guest config/key are produced by tools/agent_gateway.py. Only that gateway
holds the provider credential. Each run retains its writable disk and serial
evidence so a failed task can be inspected and rebooted without losing state.
"""
import argparse, importlib.util, json, os, re, shutil, socket, subprocess, tempfile, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

class Guest:
    def __init__(self, build, disk, out, mode, ram, launcher=()):
        self.out = out; self.disk = disk; self.log = bytearray(); self.sequence = 0
        self.capture_sequence = 0
        self.pointer = [640,400]
        self.tmp = tempfile.TemporaryDirectory(prefix='ag-qemu-')
        self.serial_path = self.tmp.name + '/serial'; self.qmp_path = self.tmp.name + '/qmp'
        command = [os.environ.get('QEMU', 'qemu-system-x86_64'), '-cpu', 'max', '-smp', '4',
                   '-m', ram, '-accel', 'tcg,thread=multi', '-vga', 'none', '-device',
                   'virtio-gpu-pci,xres=1280,yres=800', '-display', 'none', '-no-reboot',
                   '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
                   '-drive', f'file={disk},format=raw,if=none,id=hd0',
                   '-device', 'virtio-blk-pci,drive=hd0', '-chardev',
                   f'socket,id=ser0,path={self.serial_path},server=on,wait=on', '-serial', 'chardev:ser0',
                   '-qmp', f'unix:{self.qmp_path},server=on,wait=off']
        if mode == 'bios': command += ['-cdrom', str(build/'logit.iso'), '-boot', 'd']
        else:
            code = os.environ.get('OVMF_CODE', '/opt/homebrew/share/qemu/edk2-x86_64-code.fd')
            vars_src = os.environ.get('OVMF_VARS_SRC', '/opt/homebrew/share/qemu/edk2-i386-vars.fd')
            if not (out/'vars.fd').exists(): shutil.copyfile(vars_src, out/'vars.fd')
            if not (out/'esp.img').exists(): shutil.copyfile(build/'esp.img', out/'esp.img')
            command += ['-machine', 'q35', '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                        '-drive', f'if=pflash,format=raw,file={out}/vars.fd', '-device', 'ich9-ahci,id=ahci0',
                        '-drive', f'file={out}/esp.img,format=raw,if=none,id=esp0',
                        '-device', 'ide-hd,drive=esp0,bus=ahci0.0']
        command = list(launcher) + command
        (out/'command.json').write_text(json.dumps(command, indent=2))
        self.stderr = (out/'qemu.log').open('wb')
        self.process = subprocess.Popen(command, stdout=self.stderr, stderr=subprocess.STDOUT)
        self.serial = socket.socket(socket.AF_UNIX)
        try:
            for _ in range(300):
                try: self.serial.connect(self.serial_path); break
                except OSError:
                    if self.process.poll() is not None: raise RuntimeError('QEMU exited before serial')
                    time.sleep(.1)
            else: raise RuntimeError('serial socket unavailable')
            self.reader = threading.Thread(target=self.read, daemon=True); self.reader.start()
        except BaseException: self.close(); raise

    def read(self):
        try:
            while True:
                data = self.serial.recv(65536)
                if not data: break
                self.log.extend(data); (self.out/'serial.log').write_bytes(self.log)
        except OSError: pass

    def wait(self, pattern, timeout=120, start=0):
        until = time.monotonic() + timeout
        while time.monotonic() < until and self.process.poll() is None:
            match = re.search(pattern, bytes(self.log[start:]))
            if match: return bytes(self.log[start:])
            time.sleep(.1)
        raise RuntimeError('guest did not produce '+repr(pattern))

    def command(self, text, timeout=60):
        self.sequence += 1; token = f'AG_COMMAND_DONE_{self.sequence}'
        start = len(self.log)
        # Split quoting keeps the marker absent from the tty's command echo.
        # Kernel diagnostics can surround stdout mid-line, so a line anchor
        # would miss a completed command (observed during the kbench dump).
        marker_command='echo "AG_COMMAND_"DONE_'+str(self.sequence)
        self.serial.sendall((text+'\necho\n'+marker_command+'\n').encode())
        end=time.monotonic()+timeout
        while True:
            try:result=self.wait(token.encode(),min(8,max(.1,end-time.monotonic())),start);break
            except RuntimeError:
                if time.monotonic()>=end or self.process.poll() is not None:raise
                # stdout and kernel diagnostics can interleave inside a word.
                # Re-emit ONLY the harmless completion marker, never repeat the
                # original operation. The shell executes it after that command.
                self.serial.sendall((marker_command+'\n').encode())
        return result.decode(errors='replace')

    def capture(self, text, timeout=60):
        # Serial remains the command transport, but kernel messages may split
        # stdout inside a word. Read the actual closed output file instead.
        # Reuse a path outside Finder's source directory to keep its selection
        # unchanged and avoid exhausting a bounded guest directory with probes.
        self.capture_sequence += 1
        target='/state/agent-test-output.txt'; exit_file='/state/agent-test-exit.txt'
        self.command(text+' > '+target+'\n/bin/echo $? > '+exit_file,timeout)
        if not hasattr(Guest,'_filesystem_reader'):
            spec=importlib.util.spec_from_file_location('agent_capture_fs',ROOT/'tools/license_audit.py')
            reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
            Guest._filesystem_reader=reader
        self.qmp('stop',{})
        try:
            fs=Guest._filesystem_reader._LogitFS(self.disk)
            try:
                inode=fs.lookup(target);code=fs.lookup(exit_file)
                if inode is None or code is None:raise RuntimeError('guest command produced no captured file')
                data=fs.read_file(inode);status=int(fs.read_file(code).strip())
            finally:fs.close()
        finally:self.qmp('cont',{})
        stem=f'capture-{self.capture_sequence:04d}'
        (self.out/(stem+'.txt')).write_bytes(data)
        (self.out/(stem+'.json')).write_text(json.dumps({'command':text,'exit':status,'bytes':len(data)},indent=2))
        self.last_capture_exit=status
        return data.decode('utf-8')

    def complete(self, task=1, timeout=300, allow_conflict=False):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            status = self.capture('/bin/agentctl status')
            rows = re.findall(r'TASK id=(\d+) phase=(.*?) revision=(\d+) calls=(\d+).*?output=(\S+)', status)
            for row in rows:
                if int(row[0]) != task: continue
                print('GUEST_TASK', *row, flush=True)
                if row[1] == 'done': return row
                if row[1] == 'document conflict' and allow_conflict:return row
                if row[1] not in ('running','queued'): raise RuntimeError(status)
            time.sleep(2)
        raise RuntimeError('task completion deadline')

    def screenshot(self):
        with socket.socket(socket.AF_UNIX) as q:
            q.connect(self.qmp_path); stream=q.makefile('rw'); stream.readline()
            for request in ({'execute':'qmp_capabilities'}, {'execute':'screendump','arguments':{'filename':str(self.out/'desktop.ppm')}}):
                stream.write(json.dumps(request)+'\n');stream.flush()
                while True:
                    response=json.loads(stream.readline())
                    if 'error' in response: raise RuntimeError(str(response))
                    if 'return' in response: break

    def qmp(self, execute, arguments):
        with socket.socket(socket.AF_UNIX) as q:
            q.connect(self.qmp_path);stream=q.makefile('rw');stream.readline()
            for request in ({'execute':'qmp_capabilities'},{'execute':execute,'arguments':arguments}):
                stream.write(json.dumps(request)+'\n');stream.flush()
                while True:
                    reply=json.loads(stream.readline())
                    if 'error' in reply: raise RuntimeError(str(reply))
                    if 'return' in reply: break
            return reply['return']

    def freeze_failure(self, ram_bytes):
        # Preserve the state before a status command or reboot can wake a
        # blocked participant. Only deterministic fixtures request RAM dumps;
        # real-provider tests never dump guest credentials or document memory.
        self.qmp('stop',{})
        self.out.chmod(0o700)
        registers=self.qmp('human-monitor-command',{'command-line':'info registers -a'})
        (self.out/'failure-registers.txt').write_text(registers)
        memory=self.out/'failure-ram.bin'
        self.qmp('pmemsave',{'val':0,'size':ram_bytes,'filename':str(memory)})
        memory.chmod(0o600)

    def click(self,x,y):
        self.qmp('input-send-event',{'events':[
            {'type':'rel','data':{'axis':'x','value':x-self.pointer[0]}},
            {'type':'rel','data':{'axis':'y','value':y-self.pointer[1]}}]})
        self.pointer=[x,y];time.sleep(.15)
        for down in (True,False):
            self.qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.08)

    def key(self,*codes):
        self.qmp('send-key',{'keys':[{'type':'qcode','data':q} for q in codes],'hold-time':35});time.sleep(.08)

    def type(self,text):
        punctuation={' ':'spc','.':'dot',',':'comma','-':'minus','/':'slash','\n':'ret'}
        for c in text:
            if c.isupper(): self.key('shift',c.lower())
            else:self.key(punctuation.get(c,c))

    def gui_create(self):
        # Fresh desktop geometry is defined by WM's first two cascade slots.
        # These are real pointer/key events; protocol/serial assertions below
        # establish that the selected source actually reached the service.
        self.click(357,191);self.click(357,191);time.sleep(.4)
        self.click(357,191);self.click(685,526);time.sleep(1)
        self.click(250,234)
        self.type('summarize project cedar with source citations')
        self.click(225,300);time.sleep(.5);self.screenshot()

    def close(self):
        if self.process.poll() is None: self.process.terminate()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill();self.process.wait()
        self.serial.close(); self.stderr.close(); self.tmp.cleanup()

def prepare(build, gateway, out, catalog=False, extras=(), include_key=True):
    disk=out/'disk.img'
    if disk.exists(): raise RuntimeError('refusing to replace an existing guest disk')
    files=[]
    for name in ('agentd','agentctl','login','sh','cat','echo','agent-runtime-test'):
        files.append(f'{build}/{name}.aex:/bin/{name}')
    for name in ('files','textedit','assistant'): files.append(f'{build}/{name}.aex:/{name}.aex')
    if catalog:
        files=[f'{build}/agent-runtime-test.aex:/bin/agent-runtime-test']
        for app in json.loads((ROOT/'c/apps/agent/catalog.json').read_text()):files.append(f'{build}/{app["name"]}.aex:{app["path"]}')
    for name in ('ui','mono','ui-bold','mono-bold'): files.append(f'fsroot/fonts/{name}.ttf:/fonts/{name}.ttf')
    files += ['third_party/fonts/DejaVuSans.ttf:/fonts/text.ttf',
              'third_party/fonts/LICENSE-DejaVu.txt:/licenses/fonts/LICENSE-DejaVu.txt',
              'third_party/fonts/OFL-NotoSansSC.txt:/licenses/fonts/OFL-NotoSansSC.txt',
              'third_party/fonts/OFL-NotoSansMono.txt:/licenses/fonts/OFL-NotoSansMono.txt',
              'tests/fixtures/agent/source.md:/docs/source.md',
              f'{gateway}/agent.conf:/etc/agent.conf']
    # Ordinary images have configuration defaults but no installed credential.
    # Former acceptance always injected a key and masked the service boot loop.
    if include_key: files.append(f'{gateway}/agent.key:/etc/agent.key')
    # CLI regression adds immutable binary fixtures to the same catalog disk.
    # Keeping extras here avoids a second app/font manifest drifting separately.
    subprocess.run(['python3','tools/mkfs.py',str(disk),*files,*extras], cwd=ROOT, check=True,
                   stdout=(out/'mkfs.log').open('w'))
    return disk

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True)
    p.add_argument('--gateway',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--mode',choices=('bios','uefi'),default='bios');p.add_argument('--ram',default='512M')
    p.add_argument('--gui',action='store_true')
    p.add_argument('--reboot',action='store_true',help='power cycle the same writable disk and check committed state')
    a=p.parse_args();a.build=a.build.resolve();a.out=a.out.resolve();a.gateway=a.gateway.resolve();a.out.mkdir(parents=True,exist_ok=True)
    disk=prepare(a.build,a.gateway,a.out);guest=None
    try:
        guest=Guest(a.build,disk,a.out,a.mode,a.ram)
        guest.wait(b'LogitOS shell',180);guest.wait(b'AGENTD_READY',90)
        checked=guest.capture('/bin/agent-runtime-test')
        if 'AGENT_RUNTIME_PASS' not in checked or 'AGENT_RUNTIME_FAIL' in checked: raise RuntimeError(checked)
        if a.gui:guest.gui_create()
        else:
            created=guest.capture('/bin/agentctl create "Summarize Project Cedar in a concise report with source citations." /docs /docs/source.md')
            if 'TASK_CREATED id=1' not in created: raise RuntimeError(created)
        first=guest.complete();report=guest.capture('/bin/agentctl document 1')
        if '4200' not in report.replace(',','') or '120' not in report or not re.search(r'\[S1:\d+-\d+\]',report): raise RuntimeError('report facts/citations missing')
        artifact=guest.capture('/bin/cat '+first[4])
        if '4200' not in artifact.replace(',','') or '[S1:' not in artifact: raise RuntimeError('disk artifact missing report')
        original=guest.capture('/bin/cat /docs/source.md')
        if (ROOT/'tests/fixtures/agent/source.md').read_bytes() != original.encode('utf-8'): raise RuntimeError('original material changed')
        if a.gui:
            if 'objects=1' not in guest.capture('/bin/agentctl status'):raise RuntimeError('Finder selection widened')
            time.sleep(2);guest.click(650,650);time.sleep(2)
            guest.type('\nhuman note keep this sentence\n');time.sleep(1)
            if 'human note keep this sentence' not in guest.capture('/bin/agentctl document 1'):raise RuntimeError('unsaved TextEdit edits did not reach task')
        guest.capture('/bin/agentctl revise 1 "Keep the report facts and append a Next steps section with two concrete planned actions."')
        second=guest.complete(allow_conflict=a.gui)
        if a.gui and second[1]=='document conflict':
            if 'human note keep this sentence' not in guest.capture('/bin/agentctl document 1'):raise RuntimeError('conflict lost human draft')
            # Explicit user choice in the task center, then a precise follow-up.
            guest.click(300,110);time.sleep(.5);guest.click(260,690);time.sleep(.5)
            if 'phase=done' not in guest.capture('/bin/agentctl status'):raise RuntimeError('Keep current version did not resolve conflict')
            guest.capture('/bin/agentctl revise 1 "Append Next steps. Preserve the exact user sentence human note keep this sentence verbatim in the final document."')
            second=guest.complete()
        if int(second[2])<=int(first[2]) or second[4]==first[4]: raise RuntimeError('revision did not create a new version')
        if a.gui:
            if 'human note keep this sentence' not in guest.capture('/bin/agentctl document 1'):raise RuntimeError('revision lost manual changes')
            # A conflict review leaves Tasks focused. Activate TextEdit through
            # its actual dock tile before sending the editor's save shortcut.
            guest.click(640,752);time.sleep(2);guest.key('ctrl','s');time.sleep(1)
            saved=guest.capture('/bin/agentctl status')
            if '.manual.r' not in saved:raise RuntimeError('TextEdit did not save the managed document')
            guest.screenshot()
            shutil.copyfile(guest.out/'desktop.ppm',guest.out/'report.ppm')
            guest.click(182,141);guest.click(154,113);time.sleep(1)
            if 'phase=done' not in guest.capture('/bin/agentctl status'):raise RuntimeError('closing windows changed task')
            guest.screenshot()
        else:guest.screenshot()
        if a.reboot:
            before=guest.complete()
            verified=re.search(r'DOCUMENT_VERIFIED task=1[^\r\n]+',guest.capture('/bin/agentctl verify 1'))
            if not verified:raise RuntimeError('current artifact does not match the full document')
            checkpoint=guest.capture('/bin/agentctl status')
            guest.close();guest=None
            restart=a.out/'reboot';restart.mkdir(exist_ok=True)
            guest=Guest(a.build,disk,restart,a.mode,a.ram)
            guest.wait(b'LogitOS shell',180);guest.wait(b'AGENTD_READY',90)
            after=guest.complete()
            if after!=before:raise RuntimeError('reboot changed confirmed task revision, budget or artifact')
            reverified=re.search(r'DOCUMENT_VERIFIED task=1[^\r\n]+',guest.capture('/bin/agentctl verify 1'))
            if not reverified or verified[0]!=reverified[0]:raise RuntimeError('reboot changed committed document bytes')
            restored=guest.capture('/bin/cat '+after[4])
            if '4200' not in restored.replace(',','') or '120' not in restored:raise RuntimeError('reboot lost disk report')
            if a.gui and 'human note keep this sentence' not in guest.capture('/bin/agentctl document 1'):raise RuntimeError('reboot lost confirmed human edit')
            # Keep both snapshots for review of recovery latency and idle wakes.
            (a.out/'before-reboot.txt').write_text(checkpoint)
            (a.out/'after-reboot.txt').write_text(guest.capture('/bin/agentctl status'))
        (a.out/'result.json').write_text(json.dumps({'mode':a.mode,'ram':a.ram,'first':first,'revised':second,'gui':a.gui,'reboot':a.reboot,'provider':'DeepSeek','model':'deepseek-flash','status':'passed'},indent=2))
        print('AGENT_REAL_MODEL_GUEST_PASS',a.mode,a.ram,flush=True)
    finally:
        if guest:
            try:guest.screenshot()
            except Exception:pass
            guest.close()

if __name__=='__main__': main()
