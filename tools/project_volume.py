#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a validated v5 Project-volume copy. Never change the input image.

v5 currently adds persistent identities; it does not claim the proposed long
names, wide sizes or multi-volume transactions. Independent copies get a new
volume UUID; an already-v5 image is refused to avoid silently rebinding tasks.
"""
import argparse, json, os, struct, subprocess, tempfile, uuid, zlib
from pathlib import Path
from disk_guard import image_guard
from disk_profile import atomic_write

def convert(source, target, helper):
    source=Path(source).absolute();target=Path(target).absolute()
    if source==target or target.exists():raise ValueError('output must be a new independent image')
    target.parent.mkdir(parents=True,exist_ok=True)
    with image_guard(source),image_guard(target),tempfile.TemporaryDirectory(prefix='project-volume-',dir=target.parent) as scratch:
        clean=Path(scratch)/'recovered.img'
        subprocess.run([str(helper),str(source),str(clean)],check=True)
        data=bytearray(clean.read_bytes());sb=struct.unpack_from('<13I',data)
        if sb[0]!=0x4c4f4749 or sb[1]!=4:raise ValueError('conversion requires a v4 volume')
        volume=uuid.uuid4().bytes
        struct.pack_into('<I',data,4,5)
        ext=struct.pack('<I',0x31444946)+volume
        data[52:76]=ext+struct.pack('<I',zlib.crc32(ext))
        next_id=0
        for ino in range(sb[4]):
            off=sb[7]*4096+ino*128
            if struct.unpack_from('<H',data,off)[0]:
                next_id+=1;struct.pack_into('<QQQ',data,off+100,next_id,1,0)
        struct.pack_into('<Q',data,sb[7]*4096+sb[10]*128+116,next_id)
        candidate=Path(scratch)/'candidate.img';candidate.write_bytes(data)
        checked=Path(scratch)/'checked.img'
        subprocess.run([str(helper),str(candidate),str(checked)],check=True)
        atomic_write(target,checked.read_bytes())
        return {'format':5,'objects':next_id,'volume':volume.hex(),'source_unchanged':True}

def preserve_identities(source, candidate, changed):
    """Repack identical paths without changing the identities referenced by tasks.

    The caller validates input and output with native fsck. Ordinary packers
    still reject v5, so forgetting this step cannot silently erase identities.
    """
    import mkfs
    from disk_profile import CheckedImage
    data=bytearray(candidate)
    # This reader has no open resources; use its normal validated-format prefix
    # and traversal for the candidate without creating a second parser.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='project-repack-') as tmp:
        image=Path(tmp)/'candidate.img';image.write_bytes(data);new=CheckedImage(image)
    def walk(fs,ino,path,rows,seen):
        if ino in seen:raise ValueError('multiply linked Project inode')
        seen.add(ino);kind,_,raw=fs.inode(ino);rows[path]=(ino,kind,raw)
        if kind==mkfs.T_DIR:
            for name,child in fs.directory(ino).items():walk(fs,child,path.rstrip('/')+'/'+name,rows,seen)
    old_rows={};new_rows={};walk(source,source.sb[10],'/',old_rows,set());walk(new,new.sb[10],'/',new_rows,set())
    if not old_rows.keys()<=new_rows.keys():raise ValueError('Project update would remove existing objects')
    oldroot=source.inode(source.sb[10])[2];counter=struct.unpack_from('<Q',oldroot,116)[0]
    for path,(ino,kind,_) in new_rows.items():
        if path in old_rows:
            oldino,oldkind,raw=old_rows[path]
            if oldkind!=kind:raise ValueError('Project update would change object type')
            identity,revision=struct.unpack_from('<QQ',raw,100)
            if kind==mkfs.T_DIR:
                modified=source.directory(oldino).keys()!=new.directory(ino).keys()
            else:
                original=source.payload(oldino);replacement=new.payload(ino);modified=original!=replacement
                if modified and path not in changed:raise ValueError('Project update changed an unrequested file')
            if modified:
                if revision==(1<<64)-1:raise ValueError('content revision exhausted')
                revision+=1
        else:
            if counter==(1<<64)-1:raise ValueError('object identity exhausted')
            counter+=1;identity=counter;revision=1
        struct.pack_into('<QQQ',data,new.sb[7]*mkfs.BS+ino*mkfs.INODE_SIZE+100,identity,revision,0)
    struct.pack_into('<Q',data,new.sb[7]*mkfs.BS+new.sb[10]*mkfs.INODE_SIZE+116,counter)
    struct.pack_into('<I',data,4,5);data[52:76]=source.data[52:76]
    return bytes(data)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--snapshot-helper',type=Path,required=True);a=p.parse_args()
    print(json.dumps(convert(a.source,a.output,a.snapshot_helper.resolve()),indent=2))
