#!/usr/bin/env python3
"""Stock sftp drives the product dispatcher through POSIX descriptors."""
import argparse, pathlib, subprocess, tempfile, os, struct
p=argparse.ArgumentParser();p.add_argument('binary');a=p.parse_args()
binary=str(pathlib.Path(a.binary).resolve())
# Ordinary protocol discovery, independent of the dispatcher's constants.
# Check the advertised limits before asking OpenSSH to choose its block size.
def packet(body): return struct.pack('>I',len(body))+body
name=b'limits@openssh.com'
query=b'\xc8'+struct.pack('>II',17,len(name))+name
r=subprocess.run([binary],input=packet(b'\x01'+struct.pack('>I',3))+packet(query),capture_output=True,timeout=10)
wire=r.stdout; first=struct.unpack('>I',wire[:4])[0]
reply=wire[4+first:]; n=struct.unpack('>I',reply[:4])[0]
assert r.returncode==0 and n==37 and reply[4]==201 and struct.unpack('>I',reply[5:9])[0]==17, 'SFTP limits reply failed'
assert struct.unpack('>QQQQ',reply[9:41])==(65540,65520,65511,8), 'SFTP limits do not match supported framing'
with tempfile.TemporaryDirectory(prefix='sftp-local-') as d:
    root=pathlib.Path(d);data=bytes(range(256))*16385+b'EOF marker'
    (root/'source.bin').write_bytes(data)
    (root/'batch').write_text('mkdir remote\nput source.bin remote/upload.bin\nchmod 640 remote/upload.bin\nls -l remote\nrename remote/upload.bin remote/renamed.bin\nget remote/renamed.bin copy.bin\n')
    r=subprocess.run(['sftp','-D',binary,'-b','batch'],cwd=d,capture_output=True,timeout=30)
    assert r.returncode==0, 'SFTP batch failed: '+r.stderr.decode()
    assert (root/'copy.bin').read_bytes()==data, 'download bytes differ'
    assert (root/'remote/renamed.bin').stat().st_mode&0o777==0o640, 'chmod lost'
    (root/'batch').write_text('ln -s renamed.bin remote/symlink.bin\nln remote/renamed.bin remote/hardlink.bin\nget remote/symlink.bin linked.bin\n')
    r=subprocess.run(['sftp','-D',binary,'-b','batch'],cwd=d,capture_output=True,timeout=30)
    assert r.returncode==0, 'SFTP links failed: '+r.stderr.decode()
    assert os.readlink(root/'remote/symlink.bin')=='renamed.bin', 'relative symlink target changed'
    assert (root/'linked.bin').read_bytes()==data, 'symlink download bytes differ'
    assert (root/'remote/renamed.bin').stat().st_ino==(root/'remote/hardlink.bin').stat().st_ino, 'hardlink does not share the inode'
    (root/'copy.bin').write_bytes(data[:16399])
    (root/'batch').write_text('reget remote/renamed.bin copy.bin\nrm remote/renamed.bin\nget remote/hardlink.bin retained.bin\nrm remote/hardlink.bin\nrm remote/symlink.bin\nrmdir remote\n')
    r=subprocess.run(['sftp','-D',binary,'-b','batch'],cwd=d,capture_output=True,timeout=30)
    assert r.returncode==0, 'SFTP resume failed: '+r.stderr.decode()
    assert (root/'copy.bin').read_bytes()==data, 'resume bytes differ'
    assert (root/'retained.bin').read_bytes()==data, 'hardlink lost after original unlink'
    assert not (root/'remote').exists(), 'remove/rmdir failed'
print('SFTP host integration: limits, transfer, chmod, listing, rename, resume, relative symlink, hardlink, unlink lifetime, cleanup passed')
