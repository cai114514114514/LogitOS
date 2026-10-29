#!/usr/bin/env python3
"""Exercise real rebuild policy, journal checker, mkfs and metadata retention.

Joining recipe continuations matters: checking comments for '/docs' would pass
with the old destructive command still present. Only the real disk command
provides preservation arguments. The negative control removes /docs from it.
"""
import argparse,re,shlex,struct,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import mkfs
from disk_profile import CheckedImage
p=argparse.ArgumentParser();p.add_argument('--helper',required=True);p.add_argument('--negative',action='store_true');a=p.parse_args()
text=re.sub(r'\\\r?\n[ \t]*',' ',(ROOT/'Makefile').read_text())
recipes=[line for line in text.splitlines() if line.lstrip().startswith('python3 tools/mkfs.py ') and '$(DISK)' in line]
assert len(recipes)==1
words=shlex.split(recipes[0]);flags=[]
for i,w in enumerate(words):
 if w in ('--preserve','--preserve-merge'):
  if not(a.negative and words[i+1]=='/docs'):flags.extend([w,words[i+1]])
mkfs.TOTAL_BLOCKS=2048;mkfs.INODE_COUNT=128;mkfs.LOG_BLOCKS=16
checks=0
def check(value,label):
 global checks
 if not value:raise AssertionError(label)
 checks+=1
with tempfile.TemporaryDirectory(prefix='studio-rebuild-') as tmp:
 tmp=Path(tmp);disk=tmp/'disk.img';old=mkfs.Builder()
 values={'/docs/project/demo.as':'print("我的程序")\n'.encode(),'/docs/project/子目录/空文件.as':b'',
         '/docs/project/.studio/12345678.0':b'opaque\x00recovery\xffbytes',
         '/docs/readme.txt':b'user edited packaged readme','/etc/settings.conf':b'app.studio.tab.0 = /docs/project/demo.as\n'}
 for path,body in values.items():old.add_file(path,body)
 old.get_or_make_dir(['docs','project','empty'])
 old.add_file('/studio.aex',b'old app')
 ino=old.lookup(old.get_or_make_dir(['docs','project']),'demo.as')
 metadata=struct.pack('<qqqIII',10,20,30,mkfs.MODE_SET|0o640,123,456);old.metadata[ino]=metadata
 disk.write_bytes(old.serialize()[0]);(tmp/'app').write_bytes(b'new app');(tmp/'readme').write_bytes(b'factory readme');(tmp/'new').write_bytes(b'new shipped document')
 shim=tmp/'pack.py';shim.write_text('import sys\nsys.path.insert(0,'+repr(str(ROOT/'tools'))+')\nimport mkfs\nmkfs.TOTAL_BLOCKS=2048\nmkfs.INODE_COUNT=128\nmkfs.LOG_BLOCKS=16\nmkfs.main()\n')
 for iteration in range(2):
  r=subprocess.run([sys.executable,str(shim),*flags,'--snapshot-helper',str(Path(a.helper).resolve()),str(disk),str(tmp/'app')+':/studio.aex',str(tmp/'readme')+':/docs/readme.txt',str(tmp/'new')+':/docs/new.txt'],capture_output=True,text=True)
  check(r.returncode==0,'rebuild completed: '+r.stderr)
  image=CheckedImage(disk)
  check(image.resolve('/docs/project/demo.as') is not None,'project source survived rebuild')
  for path,body in values.items():check(image.payload(image.resolve(path))==body,'exact bytes: '+path)
  check(image.directory(image.resolve('/docs/project/empty'))=={},'empty folder survived')
  check(image.payload(image.resolve('/studio.aex'))==b'new app','application updated')
  check(image.payload(image.resolve('/docs/new.txt'))==b'new shipped document','new packaged sibling added')
  check(image.inode(image.resolve('/docs/project/demo.as'))[2][mkfs.OFF_ATIME:mkfs.OFF_GID+4]==metadata,'owner mode timestamps retained')
print(f'PASS {checks} Studio rebuild checks')
