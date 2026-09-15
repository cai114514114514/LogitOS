"""Stock OpenSSH forwarding and rekey consumers on the private server guest."""
import concurrent.futures
import re
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def exercise(g,tmp,out,ports,sshbase,known,run,check,http,free_port,stop_owned,blob):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Length',str(len(blob)))
            self.end_headers();self.wfile.write(blob)
        def log_message(self,*args):pass
    target=ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=target.serve_forever,daemon=True).start()
    control=tmp/'remote-control';remote=f'0:127.0.0.1:{target.server_port}'
    log=(out/'remote-forward.log').open('wb')
    master=subprocess.Popen(sshbase[:-1]+['-v','-o','BatchMode=yes','-M','-N','-S',str(control),sshbase[-1]],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=log)
    def operation(kind,*args):
        return run(sshbase[:-1]+['-o','BatchMode=yes','-o','ProxyCommand=false','-S',str(control),'-O',kind,*args,sshbase[-1]])
    try:
        end=time.monotonic()+15
        while not control.exists():
            if master.poll() is not None or time.monotonic()>end:raise RuntimeError('remote-forward master unavailable')
            time.sleep(.02)
        r=operation('forward','-R',remote)
        check(r.returncode==0 and r.stdout.strip().isdigit(),'OpenSSH remote forwarding allocates a guest loopback port')
        allocated=int(r.stdout.strip());local=free_port()
        # Both ends traverse one transport: host HTTP -> local -L -> guest
        # loopback listener -> remote -R -> owned host HTTP fixture. A second
        # channel must carry the return leg while the first remains active.
        r=operation('forward','-L',f'127.0.0.1:{local}:127.0.0.1:{allocated}')
        check(r.returncode==0,'local and remote forwarding coexist on one transport')
        status,head,body=http(local,'/payload')
        check(status==200 and body==blob,'remote forwarded-tcpip transfers the exact binary response')
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            replies=list(pool.map(lambda _:http(local,'/payload'),range(2)))
        check(all(s==200 and b==blob for s,h,b in replies),'two simultaneous remote transfers use four independent channels')
        r=operation('cancel','-R',remote)
        check(r.returncode==0,'OpenSSH cancels its dynamically allocated remote listener')
        # Rebinding the exact port proves cancellation freed the kernel
        # listener; accepting a cancel packet alone would not establish that.
        r=operation('forward','-R',f'{allocated}:127.0.0.1:{target.server_port}')
        check(r.returncode==0,'cancelled remote port can be bound again')
        status,head,body=http(local,'/payload')
        check(status==200 and body==blob,'rebound remote listener serves a new complete transfer')
    finally:
        stop_owned(master);log.close()
        target.shutdown();target.server_close()

    # Same guest identity, pinned from its console earlier, on a private
    # secondary listener. Client thresholds stay high so observed NEWKEYS
    # packets in these checks can only follow a server-initiated exchange.
    extra_known=tmp/'rekey-known'
    extra_known.write_text(known.read_text().replace(f'[127.0.0.1]:{ports[0]}',f'[127.0.0.1]:{ports[2]}'))
    secondary=list(sshbase[:-1]);secondary[secondary.index('-p')+1]=str(ports[2])
    secondary=[('UserKnownHostsFile='+str(extra_known)) if x.startswith('UserKnownHostsFile=') else x for x in secondary]
    def session(command,data=b'',threshold='1G 1h'):
        r=run(secondary+['-v','-o','BatchMode=yes','-o','RekeyLimit='+threshold,sshbase[-1],command],input=data,timeout=120)
        (out/'server-rekey.log').open('ab').write(r.stderr)
        return r
    def start(byte_limit,seconds):
        g.command('rm /state/ssh-service.stop')
        offset=len(g.data);g.command(f'/bin/ssh-service {byte_limit} {seconds} &')
        g.wait('SSHD_READY port=8081',start=offset)
    def stop():
        offset=len(g.data);g.command('touch /state/ssh-service.stop')
        g.wait('SSH_SERVICE_STOPPED',start=offset)
    start(0,0)
    try:
        r=session('/bin/ssh-hold --idle 5')
        count=r.stderr.count(b'SSH2_MSG_NEWKEYS received')
        control=r.returncode==0 and r.stdout==b'IDLE_DONE\n' and count==1
        if control:print('CONTROL_FAIL automatic idle rekey absent when disabled',flush=True)
        check(control,'rekey control observes only initial NEWKEYS with both triggers disabled')
        data=bytes(range(256))*2049+b'no-automatic-byte-rekey'
        r=session('cat',data)
        control=r.returncode==0 and r.stdout==data and r.stderr.count(b'SSH2_MSG_NEWKEYS received')==1
        if control:print('CONTROL_FAIL automatic byte rekey absent when disabled',flush=True)
        check(control,'byte rekey control transfers beyond the test budget without extra NEWKEYS')
    finally:stop()
    start(262144,2)
    try:
        r=session('/bin/ssh-hold --idle 5')
        check(r.returncode==0 and r.stdout==b'IDLE_DONE\n' and r.stderr.count(b'SSH2_MSG_NEWKEYS received')>=2,'server time limit rekeys an idle authenticated connection')
    finally:stop()
    # A time trigger must not cover up a missing byte counter on a slow run.
    start(262144,0)
    try:
        data=bytes(range(256))*8193+b'server-rekey-end'
        r=session('cat',data)
        check(r.returncode==0 and r.stdout==data and r.stderr.count(b'SSH2_MSG_NEWKEYS received')>=3,'server byte limit rekeys during exact bidirectional streaming')
        r=session('cat',data,threshold='512K 1h')
        check(r.returncode==0 and r.stdout==data and r.stderr.count(b'SSH2_MSG_NEWKEYS received')>=3,'client and server rekey limits coexist without losing channel bytes')
    finally:stop()
