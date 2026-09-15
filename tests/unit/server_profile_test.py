#!/usr/bin/env python3
"""Preserving administrator configuration alongside newly packaged defaults."""
import argparse, struct, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import mkfs
from disk_profile import preserve
p=argparse.ArgumentParser();p.add_argument('--helper',required=True);p.add_argument('--negative',action='store_true');a=p.parse_args()
checks=0
def check(ok,label):
    global checks
    checks+=1
    if not ok: raise AssertionError(label)
def ino(b,path):
    parts=path.strip('/').split('/');return b.lookup(b.get_or_make_dir(parts[:-1]),parts[-1])
with tempfile.TemporaryDirectory(prefix='server-profile-') as d:
    old=mkfs.Builder();old.add_file('/etc/service.conf',b'administrator choice')
    old.add_file('/etc/host-key',b'private fixture identity')
    key=ino(old,'/etc/host-key')
    meta=bytearray(mkfs.OFF_GID+4-mkfs.OFF_ATIME)
    struct.pack_into('<I',meta,mkfs.OFF_XMODE-mkfs.OFF_ATIME,mkfs.MODE_SET|0o600)
    old.metadata[key]=meta
    disk=Path(d)/'disk.img';disk.write_bytes(old.serialize()[0])
    fresh=mkfs.Builder();fresh.add_file('/etc/service.conf',b'factory choice')
    fresh.add_file('/etc/new-service.conf',b'new factory default')
    count=preserve(fresh,disk,[],a.helper,[] if a.negative else ['/etc'])
    check(fresh.content[ino(fresh,'/etc/service.conf')]==b'administrator choice','administrator configuration retained')
    check(fresh.content[ino(fresh,'/etc/new-service.conf')]==b'new factory default','new packaged default retained')
    check(fresh.content[ino(fresh,'/etc/host-key')]==b'private fixture identity','host identity bytes retained')
    check(fresh.metadata[ino(fresh,'/etc/host-key')]==meta,'private identity metadata retained')
    check(count==3,'preserved inode count')
    strict=mkfs.Builder();strict.add_file('/etc/service.conf',b'factory')
    try: preserve(strict,disk,['/etc'],a.helper)
    except ValueError as e: check('reserved user-state' in str(e),'ordinary preservation stays strict')
    else: raise AssertionError('ordinary preservation must reject overlap')
    mismatch=mkfs.Builder();mismatch.add_file('/etc',b'file instead of directory')
    try: preserve(mismatch,disk,[],a.helper,['/etc'])
    except ValueError as e: check('type' in str(e),'configuration type mismatch refused')
    else: raise AssertionError('configuration type mismatch must fail')
print(f'SERVER_PROFILE checks={checks} failures=0')
