#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real LogitFS identities, with an inode-reuse mutant as the first gate."""
import argparse,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);a=p.parse_args();out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
base=['cc','-O1','-g','-Wall','-Wextra','-Wno-unused-function','-Wno-type-limits','-fsanitize=address,undefined','-Itests/unit/fsstub','-Ic/fs -Ic/fs/vfs -Ic/fs/logitfs -Ic/fs/cache -Ic/fs/ramfs -Ic/fs/ctl -Ic/fs/procfs','-Ic/drivers/block']
core=['c/fs/logitfs.c','c/fs/bcache.c','c/fs/fsck.c','c/drivers/block/crc32.c']
results=[]
for name,defs in [('reuse-negative',['-DLOGITFS_ID_REUSE_NEGCTL']),('identity',[])]:
 binary=out/name
 subprocess.run(base+defs+['tests/unit/fs_identity_test.c']+core+['-o',str(binary)],cwd=ROOT,check=True)
 r=subprocess.run([str(binary)],cwd=ROOT,text=True,capture_output=True);(out/(name+'.log')).write_text(r.stdout+r.stderr)
 if defs:
  assert r.returncode and 'FS_IDENTITY_FAIL deleted identity is never reused' in r.stderr,'inode-reuse mutant escaped the concrete assertion'
 else:assert not r.returncode,r.stdout+r.stderr
 results.append(name);print('PROJECT_IDENTITY_PASS',name,flush=True)
binary=out/'crash'
subprocess.run(base+['-DLOGITFS_SIM_IDENTITIES','tests/unit/fs_crash_test.c']+core+['-o',str(binary)],cwd=ROOT,check=True)
with (out/'crash.log').open('w') as log:subprocess.run([str(binary)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
results.append('v5 full device-write crash sweep');print('PROJECT_IDENTITY_PASS',results[-1],flush=True)
import tempfile
binary=out/'binding'
subprocess.run(base+['-Iinclude','-Iinclude/abi','-Ic/lib/gfx/include','tests/unit/project_binding_test.c','c/lib/agent/files.c','c/lib/agent/task.c','c/lib/agent/store.c','c/drivers/block/crc32.c','-o',str(binary)],cwd=ROOT,check=True)
with tempfile.TemporaryDirectory(prefix='project-binding-') as directory:subprocess.run([str(binary),directory],cwd=ROOT,check=True)
results.append('binding journal recovery')
(out/'result.json').write_text(json.dumps({'passed':True,'checks':results},indent=2))
