#!/usr/bin/env python3
"""Ordinary OpenSSH exec/file transfer and HTTP clients against shipped daemons.

Private keys/passwords live only in a temporary directory. QEMU owns a private
writable disk; the product image is never run writable. Pin the host key from
its trusted first-boot serial fingerprint, then verify it again after reboot.
"""
import argparse, hashlib, importlib.util, json, os, re, shlex, shutil, socket
import subprocess, sys, tempfile, threading, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/qmp'))
from owned_process import stop_owned
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--one-channel-control',type=Path,help='private one-channel daemon; the normal multiplex assertion must fail')
a=p.parse_args(); build=a.build.resolve(); base=build
out=build/('server-guest-one-channel' if a.one_channel_control else 'server-guest'); out.mkdir(exist_ok=True)
checks=[]
# Preparation can fail before the first guest assertion. Invalidate an older
# successful result first so missing build artifacts cannot leave a false pass.
(out/'result.json').write_text(json.dumps({'passed':False,'complete':False,'checks':checks},indent=2))
iso=out/'run.iso'; shutil.copyfile(build/'logit.iso',iso)
artifacts={'product_disk':str(build/'disk.img'),
    'iso_sha256':hashlib.sha256(iso.read_bytes()).hexdigest(),
    'sshd_sha256':hashlib.sha256((build/'sshd.aex').read_bytes()).hexdigest(),
    'httpd_sha256':hashlib.sha256((build/'httpd.aex').read_bytes()).hexdigest()}
def check(ok,label):
    checks.append({'name':label,'passed':bool(ok)})
    print(('PASS ' if ok else 'FAIL ')+label,flush=True)
    (out/'result.json').write_text(json.dumps({'passed':all(x['passed'] for x in checks),'checks':checks,'complete':False},indent=2))
    if not ok: raise AssertionError(label)
def run(cmd,**kw):
    return subprocess.run(cmd,cwd=ROOT,capture_output=True,timeout=kw.pop('timeout',90),**kw)
def require(cmd):
    r=run(cmd); (out/'prepare.log').open('ab').write(r.stdout+r.stderr)
    if r.returncode: raise RuntimeError(f'preparation failed: {cmd[0]}; see {out}/prepare.log')
    return r.stdout.decode()
def free_port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
def http(port,path,method='GET',headers=None):
    import http.client
    if method == 'HEAD':
        # http.client deliberately hides a HEAD response body. Inspect the
        # wire instead, or a broken server sending one passes this assertion.
        fields = ''.join(k+': '+v+'\r\n' for k,v in (headers or {}).items())
        request = f'HEAD {path} HTTP/1.1\r\nHost: localhost\r\n{fields}Connection: close\r\n\r\n'
        with socket.create_connection(('127.0.0.1',port),timeout=20) as sock:
            sock.sendall(request.encode()); wire=b''
            while True:
                part=sock.recv(65536)
                if not part: break
                wire+=part
        head,body=wire.split(b'\r\n\r\n',1)
        lines=head.decode().split('\r\n')
        return int(lines[0].split()[1]),dict((k.lower(),v.strip()) for k,v in (x.split(':',1) for x in lines[1:])),body
    c=http.client.HTTPConnection('127.0.0.1',port,timeout=20)
    try:
        c.request(method,path,headers=headers or {}); r=c.getresponse()
        return r.status,dict((k.lower(),v) for k,v in r.getheaders()),r.read()
    finally: c.close()
class Guest:
    def __init__(self,tmp,disk,number,ports):
        self.data=bytearray(); self.sock=socket.socket(socket.AF_UNIX); self.number=number
        self.log=(out/f'boot{number}.serial.log'); path=str(tmp/f'serial{number}')
        net=f'user,id=n0,hostfwd=tcp:127.0.0.1:{ports[0]}-:22,hostfwd=tcp:127.0.0.1:{ports[1]}-:8080,hostfwd=tcp:127.0.0.1:{ports[2]}-:8081,hostfwd=tcp:127.0.0.1:{ports[3]}-:8443'
        cmd=[os.environ.get('QEMU','qemu-system-x86_64'),'-cpu','max','-smp','4','-m','512M','-accel','tcg,thread=multi',
             '-cdrom',str(iso),'-boot','d','-drive',f'file={disk},format=raw,if=none,id=hd0',
             '-device','virtio-blk-pci,drive=hd0','-vga','none','-device','virtio-gpu-pci,xres=1280,yres=800',
             '-display','none','-no-reboot','-netdev',net,'-device','e1000,netdev=n0',
             '-chardev',f'socket,id=ser0,path={path},server=on,wait=on','-serial','chardev:ser0']
        (out/f'boot{number}.command.json').write_text(json.dumps(cmd,indent=2))
        self.qlog=(out/f'boot{number}.qemu.log').open('wb')
        self.process=subprocess.Popen(cmd,stdout=self.qlog,stderr=subprocess.STDOUT)
        end=time.monotonic()+20
        while True:
            try: self.sock.connect(path); break
            except OSError:
                if time.monotonic()>end or self.process.poll() is not None: self.close(); raise RuntimeError('no serial socket')
                time.sleep(.05)
        def reader():
            try:
                while True:
                    b=self.sock.recv(65536)
                    if not b: break
                    self.data.extend(b); self.log.write_bytes(self.data)
            except OSError: pass
        threading.Thread(target=reader,daemon=True).start()
    def wait(self,marker,timeout=90,start=0):
        marker=marker.encode() if isinstance(marker,str) else marker
        end=time.monotonic()+timeout
        while marker not in self.data[start:]:
            if time.monotonic()>end or self.process.poll() is not None: raise RuntimeError(f'missing {marker!r}; see {self.log}')
            time.sleep(.05)
    def send(self,line): self.sock.sendall((line+'\n').encode())
    def command(self,line):
        start=len(self.data); self.send(line); self.wait(' $ ',start=start); return self.data[start:].decode(errors='replace')
    def close(self):
        stop_owned(self.process); self.sock.close(); self.qlog.close(); self.log.write_bytes(self.data)

with tempfile.TemporaryDirectory(prefix='logit-servers-') as temp:
    tmp=Path(temp); key=tmp/'client'; known=tmp/'known_hosts'; password=os.urandom(20).hex()
    require(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(key)])
    from ssh_keys import prepare, exercise as exercise_keys, verify_reboot as verify_keys_reboot
    authorized,key_cases=prepare(tmp,key,require)
    ask=tmp/'askpass'; ask.write_text('#!/bin/sh\nprintf "%s\\n" "$LOGIT_TEST_PASSWORD"\n'); ask.chmod(0o700)
    manifest=require(['make','-n','-W','tools/mkfs.py','BUILD='+str(base),str(base/'disk.img')])
    joined=re.sub(r'\\\r?\n[ \t]*',' ',manifest)
    lines=[x for x in joined.splitlines() if x.startswith('python3 tools/mkfs.py ')]
    if len(lines)!=1: raise RuntimeError('cannot identify product disk recipe')
    args=shlex.split(lines[0])[2:]; flags=[]
    while args and args[0].startswith('--'):
        flags.extend(args[:2]); args=args[2:]
    specs=args[1:]
    if a.one_channel_control:
        specs=[str(a.one_channel_control.resolve())+':/bin/sshd' if x.endswith(':/bin/sshd') else x for x in specs]
    blob=bytes(range(256))*1201+b'final-data'
    payload=out/'payload.bin'; payload.write_bytes(blob)
    text=out/'encoded.txt'; text.write_text('encoded filename works\n')
    extras=[str(build/'server_probe.elf')+':/bin/server-probe',str(build/'pty_probe.elf')+':/bin/pty-probe',str(payload)+':/www/payload.bin',
            str(build/'inet_probe.elf')+':/bin/inet-probe',
            str(build/'ssh_hold.elf')+':/bin/ssh-hold',
            str(build/'ssh_service.elf')+':/bin/ssh-service',
            str(text)+':/www/report one.txt',str(text)+':/www/报告.txt',
            str(authorized)+':/server-test-authorized-keys']
    disk=out/'disk.img'
    # Remove a prior PRIVATE run disk only via overwrite by the builder; no
    # production accounts survive into a test's clean enrollment.
    require([sys.executable,'tools/mkfs.py',str(disk),*specs,*extras])
    ports=[free_port() for _ in range(4)]
    sshbase=['ssh','-F','/dev/null','-p',str(ports[0]),'-i',str(key),'-o','IdentitiesOnly=yes',
             '-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(known),'-o','ConnectTimeout=10',
             '-o','ConnectionAttempts=1','-o','LogLevel=ERROR','server@127.0.0.1']
    def ssh(command,stdin=b'',password_auth=False,extra=None):
        args=sshbase[:-1]+(extra or [])
        env=os.environ.copy()
        if password_auth:
            args+=['-o','PubkeyAuthentication=no','-o','PreferredAuthentications=password','-o','NumberOfPasswordPrompts=1']
            env.update(SSH_ASKPASS=str(ask),SSH_ASKPASS_REQUIRE='force',DISPLAY='logit-test',LOGIT_TEST_PASSWORD=password)
        else: args+=['-o','BatchMode=yes']
        r=run(args+[sshbase[-1],command],input=stdin,env=env,timeout=180)
        (out/'ssh.log').open('a').write(json.dumps({'command':command,'status':r.returncode,'stdout_bytes':len(r.stdout),'stdout_sha256':hashlib.sha256(r.stdout).hexdigest(),'stderr':r.stderr.decode(errors='replace')})+'\n')
        return r
    g=Guest(tmp,disk,1,ports)
    try:
        g.wait('LogitOS shell',120); g.command('echo SERVER_BOOT_READY')
        start=len(g.data); g.send('login -a server'); g.wait('New password:',start=start)
        start=len(g.data); g.send(password); g.wait('Retype password:',start=start)
        start=len(g.data); g.send(password); g.wait('ENROLLED server',120,start); g.wait(' $ ',start=start)
        g.command('mkdir /home/server/.ssh')
        g.command('cat /server-test-authorized-keys > /home/server/.ssh/authorized_keys')
        g.command('stat -c 700 /home/server/.ssh')
        g.command('stat -c 600 /home/server/.ssh/authorized_keys')
        g.command('touch /etc/sshd.enabled'); g.command('touch /etc/httpd.enabled')
        g.command('touch /etc/httpsd.enabled')
        g.command('touch /etc/sshd.forward.enabled')
        g.command('/bin/sshd &'); g.wait('SSHD_READY')
        g.command('/bin/httpd &'); g.wait('HTTPD_READY')
        g.command('/bin/httpsd &'); g.wait('HTTPSD_READY',120)
        # Keyscan is only discovery: compare against the fingerprint printed
        # inside this fresh guest before trusting the resulting known_hosts.
        r=run(['ssh-keyscan','-T','10','-p',str(ports[0]),'-t','ed25519','127.0.0.1'])
        known.write_bytes(r.stdout)
        fp=require(['ssh-keygen','-lf',str(known)]).split()[1]
        check(fp.encode() in g.data,'host identity pinned to guest console')
        r=ssh('/bin/server-probe')
        check(r.returncode==37 and r.stdout==b'SERVER_STDOUT\n' and r.stderr==b'SERVER_STDERR\n','exec stdout stderr and exit status')
        r=ssh('pwd')
        check(r.returncode==0 and r.stdout.strip()==b'/home/server','authenticated home directory')
        r=ssh('login -i')
        check(r.returncode==0 and b'ID uid=1000 gid=1000' in r.stdout,'command runs as enrolled uid and gid')
        r=ssh('echo PASSWORD_OK',password_auth=True)
        check(r.returncode==0 and r.stdout.strip()==b'PASSWORD_OK','password authentication')
        if not a.one_channel_control:
            exercise_keys(key_cases,sshbase,run,check,out)
            from ssh_extended import exercise
            exercise(g,tmp,out,ports,sshbase,known,run,check,http,free_port,stop_owned,blob)
        # ProxyCommand=false prevents a missing control socket from silently
        # creating a second authenticated connection and passing as multiplex.
        control=tmp/'ssh-control';before_auth=bytes(g.data).count(b'AUTH_OK user=server')
        master=subprocess.Popen(sshbase[:-1]+['-o','BatchMode=yes','-M','-N','-S',str(control),sshbase[-1]],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def mux(command,**kw):
            return run(sshbase[:-1]+['-o','BatchMode=yes','-o','ProxyCommand=false','-S',str(control),sshbase[-1],command],**kw)
        held=None;sender=None
        try:
            deadline=time.monotonic()+15
            while not control.exists():
                if master.poll() is not None or time.monotonic()>deadline:raise RuntimeError('no OpenSSH control socket')
                time.sleep(.02)
            pending=bytes(range(256))*2049+b'multiplex-end'
            start=len(g.data)
            held=subprocess.Popen(sshbase[:-1]+['-o','BatchMode=yes','-o','ProxyCommand=false','-S',str(control),sshbase[-1],'/bin/ssh-hold /home/server/release'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            held_result=[]
            def transfer():held_result.append(held.communicate(pending,timeout=70))
            sender=threading.Thread(target=transfer,daemon=True);sender.start()
            g.wait('/bin/ssh-hold loading',start=start)
            r=mux('echo MULTIPLEX_OK',timeout=20)
            check(r.returncode==0 and r.stdout.strip()==b'MULTIPLEX_OK' and held.poll() is None,'multiple commands share one SSH connection while one stdin is paused')
            r=mux('touch /home/server/release',timeout=20)
            check(r.returncode==0,'independent channel releases the paused consumer')
            sender.join(timeout=80)
            check(not sender.is_alive() and held.returncode==0 and held_result[0][0]==pending,'paused channel resumes with exact bytes across flow-control windows')
            import concurrent.futures
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                results=list(pool.map(lambda i:mux('echo CHANNEL_'+str(i)),range(3)))
            check(all(r.returncode==0 and r.stdout.strip()==f'CHANNEL_{i}'.encode() for i,r in enumerate(results)),'three simultaneous command channels complete independently')
            check(bytes(g.data).count(b'AUTH_OK user=server')==before_auth+1,'multiplex proof uses one authentication and one transport')
        finally:
            if held is not None:stop_owned(held)
            if sender is not None:sender.join(timeout=5)
            stop_owned(master);(out/'multiplex-master.log').write_bytes(master.stderr.read())
        if a.one_channel_control:raise AssertionError('one-channel control unexpectedly passed')
        r=ssh('/bin/inet-probe negative')
        check(r.returncode!=0 and b'INET_FAIL client connect' in r.stdout,'libc control detects missing socket byte-order conversion')
        r=ssh('/bin/inet-probe')
        check(r.returncode==0 and b'INET_FAILURES=0' in r.stdout,'libc IPv4 loopback client server accept names and half close')
        data=bytes(range(256))*12289+b'input-eof'
        r=ssh('cat > /home/server/upload.bin',data,extra=['-o','RekeyLimit=512K','-v'])
        check(r.returncode==0,'binary upload with stdin EOF')
        check(r.stderr.count(b'SSH2_MSG_NEWKEYS received')>=2,'OpenSSH rekeys during upload beyond the receive window')
        r=ssh('cat /home/server/upload.bin')
        check(r.returncode==0 and r.stdout==data,'binary download exact bytes')
        for i in range(10):
            r=ssh('echo RECONNECT_'+str(i))
            check(r.returncode==0 and r.stdout.strip()==f'RECONNECT_{i}'.encode(),f'worker reuse {i+1}')
        r=ssh('/bin/pty-probe negative')
        check(r.returncode!=0 and b'PTY_FAIL canonical echo' in r.stdout,'PTY control detects disabled echo')
        r=ssh('/bin/pty-probe')
        check(r.returncode==0 and b'failures=0' in r.stdout,'native PTY and libc termios raw echo window EOF')
        import pty, fcntl, termios, struct
        master,slave=pty.openpty()
        try:
            fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',41,119,0,0))
            env=os.environ.copy();env['TERM']='xterm-256color'
            r=run(sshbase[:-1]+['-o','BatchMode=yes','-tt',sshbase[-1],'/bin/pty-probe attached'],stdin=slave,env=env,timeout=60)
            check(r.returncode==0 and b'PTY_ATTACHED rows=41 cols=119' in r.stdout,'OpenSSH allocates a real terminal and sends its size')
        finally: os.close(master);os.close(slave)
        # An ordinary interactive shell must regain its prompt after Ctrl+C,
        # and a later command must see a live OpenSSH window-change update.
        import signal
        master,slave=pty.openpty(); terminal=bytearray()
        env=os.environ.copy();env['TERM']='xterm-256color'
        interactive=subprocess.Popen(sshbase[:-1]+['-o','BatchMode=yes','-tt',sshbase[-1]],stdin=slave,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        def terminal_reader():
            while True:
                part=os.read(interactive.stdout.fileno(),4096)
                if not part:break
                terminal.extend(part)
        reader=threading.Thread(target=terminal_reader,daemon=True);reader.start()
        def terminal_wait(marker,start=0):
            end=time.monotonic()+30
            while marker not in terminal[start:]:
                if time.monotonic()>end or interactive.poll() is not None:
                    raise RuntimeError('interactive terminal missing '+repr(marker)+'; '+repr(bytes(terminal)))
                time.sleep(.05)
        try:
            terminal_wait(b' $ ')
            start=len(g.data);os.write(master,b'sleep 30\r');g.wait('/bin/sleep loading',start=start)
            start=len(terminal);os.write(master,b'\x03');terminal_wait(b' $ ',start)
            start=len(terminal);os.write(master,b'echo PTY_AFTER_INTERRUPT\r');terminal_wait(b'\r\nPTY_AFTER_INTERRUPT\r\n',start)
            check(True,'OpenSSH Ctrl+C stops foreground command and shell remains usable')
            fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',50,132,0,0));interactive.send_signal(signal.SIGWINCH)
            start=len(terminal);os.write(master,b'/bin/pty-probe attached\r');terminal_wait(b'PTY_ATTACHED rows=50 cols=132',start)
            check(True,'OpenSSH live window change reaches the running terminal')
            os.write(master,b'exit\r');interactive.wait(timeout=30)
        finally:
            stop_owned(interactive);reader.join(timeout=2);os.close(master);os.close(slave)
            (out/'terminal.log').write_bytes(terminal)
        source=tmp/'sftp-source.bin';source.write_bytes(data)
        copied=tmp/'sftp-copy.bin';batch=tmp/'sftp.batch'
        batch.write_text(f'put {source} /home/server/sftp.bin\nchmod 640 /home/server/sftp.bin\nls -l /home/server\nrename /home/server/sftp.bin /home/server/sftp-renamed.bin\nget /home/server/sftp-renamed.bin {copied}\n')
        sftp=['sftp','-F','/dev/null','-P',str(ports[0]),'-i',str(key),'-o','IdentitiesOnly=yes','-o','BatchMode=yes',
              '-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(known),'-o','RekeyLimit=512K','-b',str(batch),'server@127.0.0.1']
        r=run(sftp,timeout=180)
        (out/'sftp-result.txt').write_bytes(r.stdout+r.stderr)
        check(r.returncode==0 and copied.read_bytes()==data,'SFTP put get listing chmod rename with rekey')
        copied.write_bytes(data[:12345]);batch.write_text(f'reget /home/server/sftp-renamed.bin {copied}\n')
        r=run(sftp,timeout=120)
        check(r.returncode==0 and copied.read_bytes()==data,'SFTP resumes an existing partial download')
        batch.write_text(f'ln -s sftp-renamed.bin /home/server/sftp-link.bin\nln /home/server/sftp-renamed.bin /home/server/sftp-hard.bin\nget /home/server/sftp-link.bin {copied}\n')
        r=run(sftp,timeout=120)
        (out/'sftp-links.txt').write_bytes(r.stdout+r.stderr)
        check(r.returncode==0 and copied.read_bytes()==data,'SFTP creates a relative symlink and downloads its target')
        batch.write_text(f'rm /home/server/sftp-renamed.bin\nget /home/server/sftp-hard.bin {copied}\n')
        r=run(sftp,timeout=120)
        check(r.returncode==0 and copied.read_bytes()==data,'SFTP hardlink retains file bytes after original unlink')
        start=len(g.data)
        client=subprocess.Popen(sshbase[:-1]+['-o','BatchMode=yes',sshbase[-1],'sleep 5'],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            g.wait('/bin/sleep loading',start=start)
        finally: stop_owned(client)
        r=ssh('echo AFTER_CANCEL')
        check(r.returncode==0 and r.stdout.strip()==b'AFTER_CANCEL','connection works after cancelling a running command')
        status,head,body=http(ports[1],'/')
        check(status==200 and b'/signed-report.html' in body,'default server page has real crypto consumer')
        status,head,body=http(ports[1],'/payload.bin')
        check(status==200 and body==blob and head.get('accept-ranges')=='bytes','HTTP streaming exact file')
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            responses=list(pool.map(lambda _: http(ports[1],'/payload.bin'),range(3)))
        check(all(s==200 and b==blob for s,h,b in responses),'concurrent HTTP clients receive independent complete files')
        local_port=free_port()
        tunnel=subprocess.Popen(sshbase[:-1]+['-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-o','ServerAliveInterval=2','-N','-L',f'127.0.0.1:{local_port}:127.0.0.1:8080',sshbase[-1]],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            end=time.monotonic()+15
            while True:
                try:status,head,body=http(local_port,'/payload.bin');break
                except ConnectionRefusedError:
                    if time.monotonic()>end or tunnel.poll() is not None:raise
                    time.sleep(.05)
            check(status==200 and body==blob,'OpenSSH local forwarding reaches guest HTTP via socket connect')
            for i in range(8):
                status,head,body=http(local_port,'/payload.bin')
                check(status==200 and body==blob and tunnel.poll() is None,f'forwarding sequential channel {i+2} including close overlap')
            with socket.create_connection(('127.0.0.1',local_port),timeout=20) as half:
                half.sendall(b'GET /payload.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')
                half.shutdown(socket.SHUT_WR);wire=b''
                while True:
                    part=half.recv(65536)
                    if not part:break
                    wire+=part
            check(wire.split(b'\r\n\r\n',1)[1]==blob,'forwarded stdin EOF preserves the destination response')
        finally:
            stop_owned(tunnel);(out/'forwarding.log').write_bytes(tunnel.stderr.read())
        import ssl
        from http.client import HTTPSConnection
        cert=ssh('cat /etc/httpsd.der')
        check(cert.returncode==0 and bool(cert.stdout),'persistent HTTPS certificate is readable')
        trust=ssl.create_default_context(cadata=ssl.DER_cert_to_PEM_cert(cert.stdout))
        def https_get(context):
            c=HTTPSConnection('localhost',ports[3],context=context,timeout=30)
            try:
                c.request('GET','/payload.bin');r=c.getresponse();return r.status,r.read()
            finally:c.close()
        try: https_get(ssl.create_default_context())
        except ssl.SSLCertVerificationError: rejected=True
        else: rejected=False
        check(rejected,'self-signed HTTPS requires explicit client trust')
        status,body=https_get(trust)
        check(status==200 and body==blob,'TLS verified HTTPS streams the exact file')
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            responses=list(pool.map(lambda _: https_get(trust),range(3)))
        check(all(s==200 and b==blob for s,b in responses),'concurrent HTTPS clients keep independent TLS state')
        for value,part in [('bytes=123-899',blob[123:900]),('bytes=300000-',blob[300000:]),('bytes=-17',blob[-17:])]:
            status,head,body=http(ports[1],'/payload.bin',headers={'Range':value})
            check(status==206 and body==part and int(head['content-length'])==len(part),'HTTP '+value)
        status,head,body=http(ports[1],'/payload.bin','HEAD',{'Range':'bytes=1-2'})
        check(status==200 and not body and int(head['content-length'])==len(blob),'HEAD ignores Range and returns full length')
        status,head,body=http(ports[1],'/missing','HEAD')
        check(status==404 and not body,'HEAD missing resource has no body')
        status,head,body=http(ports[1],'/_stat','HEAD')
        check(status==200 and not body,'HEAD counters have no body')
        status,head,body=http(ports[1],'/payload.bin',headers={'Range':'bytes=999999-'})
        check(status==416 and head.get('content-range')==f'bytes */{len(blob)}' and not body,'range past end reports representation length')
        for path in ('/report%20one.txt','/%E6%8A%A5%E5%91%8A.txt'):
            status,head,body=http(ports[1],path)
            check(status==200 and body==text.read_bytes(),'HTTP encoded name '+path)
        status,head,body=http(ports[1],'/signed-report.html')
        check(status==200 and b'crypto.subtle' in body,'WebCrypto report page served from guest')
        status,head,body=http(ports[1],'/ssh-keys.txt')
        check(status==200 and body==(ROOT/'fsroot/www/ssh-keys.txt').read_bytes(),'new SSH key guide is downloadable from guest HTTP')
        info=g.command('stat /etc/ssh_host_ed25519_key')
        check('mode=600' in info and 'uid=0 gid=0' in info,'private host key mode')
        # Guest download uses the host forward back into its own HTTP service,
        # exercising the real net consumer and /download storage path together.
        info=g.command('net download http://10.0.2.2:'+str(ports[1])+'/welcome.txt')
        check('/download/' in info,'net consumes guest HTTP and saves in download')
        info=g.command('net download http://10.0.2.2:'+str(ports[1])+'/ssh-keys.txt')
        check('/download/' in info,'net saves the new key guide in download')
    finally: g.close()
    # Rebuild the private product disk with exactly the production preservation
    # roots, then boot again. This catches identity/account/download loss on a
    # build as well as loss on a plain reboot. Fixture extras remain explicit.
    require([sys.executable,'tools/mkfs.py',*flags,str(disk),*specs,*extras])
    g=Guest(tmp,disk,2,ports)
    try:
        g.wait('LOGIN:',120)
        end=time.monotonic()+45
        while True:
            try:
                r=ssh('echo REBOOT_OK')
                if r.returncode==0: break
            except subprocess.TimeoutExpired: pass
            if time.monotonic()>end: raise RuntimeError('enabled SSH did not start on reboot')
            time.sleep(.5)
        check(r.stdout.strip()==b'REBOOT_OK','enabled SSH starts before console login after rebuild and reboot')
        r=ssh('cat /home/server/upload.bin')
        check(r.returncode==0 and r.stdout==data,'account key and home survive product rebuild')
        verify_keys_reboot(key_cases,sshbase,run,check,out)
        r=ssh('cat /home/server/sftp-hard.bin')
        check(r.returncode==0 and r.stdout==data,'SFTP hardlink data survives product rebuild and reboot')
        status,head,body=http(ports[1],'/')
        check(status==200,'enabled HTTP starts after reboot')
        status,body=https_get(trust)
        check(status==200 and body==blob,'HTTPS auto-starts after rebuild using the same trusted identity')
        r=ssh('cat /var/log/sshd.log')
        check(r.returncode==0 and fp.encode() in r.stdout,'same private host identity loaded on reboot')
        r=ssh('cat /download/welcome.txt')
        check(r.returncode==0 and r.stdout==(ROOT/'fsroot/www/welcome.txt').read_bytes(),'downloads survive rebuild and reboot')
        r=ssh('cat /download/ssh-keys.txt')
        check(r.returncode==0 and r.stdout==(ROOT/'fsroot/www/ssh-keys.txt').read_bytes(),'downloaded key guide survives product rebuild and reboot')
    finally: g.close()
    (out/'result.json').write_text(json.dumps({'passed':True,'complete':True,'checks':checks,**artifacts},indent=2))
    print(f'PASS {len(checks)} real guest server checks',flush=True)
