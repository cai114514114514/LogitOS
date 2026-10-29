#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A shipping installer must retain FileIDs across v5 application updates."""
import argparse,struct,subprocess,sys,tempfile,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
import mkfs,agent_session,project_volume
from disk_profile import CheckedImage
from disk_guard import image_guard
from unittest.mock import patch
p=argparse.ArgumentParser();p.add_argument('--helper',type=Path,required=True);a=p.parse_args();helper=a.helper.resolve()
mkfs.TOTAL_BLOCKS=2048;mkfs.INODE_COUNT=128;mkfs.LOG_BLOCKS=16
for negative in (True,False):
 with tempfile.TemporaryDirectory(prefix='project-install-') as tmp:
  d=Path(tmp);source=d/'source.img';disk=d/'project.img';builder=mkfs.Builder()
  files={'/docs/Project/report.md':b'confirmed document','/browser.aex':b'opaque browser','/state/agents/task':b'checkpoint','/bin/agentd':b'old app'}
  for path,data in files.items():builder.add_file(path,data)
  builder.get_or_make_dir(['empty']);source.write_bytes(builder.serialize()[0]);digest=hashlib.sha256(source.read_bytes()).digest()
  project_volume.convert(source,disk,helper);before=CheckedImage(disk,allow_identity=True)
  def ref(fs,path):return struct.unpack_from('<QQ',fs.inode(fs.resolve(path))[2],100)
  identities={p:ref(before,p) for p in files};empty=ref(before,'/empty');uuid=before.data[52:76]
  try:
   with image_guard(disk):
    if negative:
     with patch('project_volume.preserve_identities',side_effect=lambda source,candidate,changed:candidate):agent_session.install(disk,helper,{'/bin/agentd':(b'new app',0o755),'/new/config':(b'private',0o600)})
    else:agent_session.install(disk,helper,{'/bin/agentd':(b'new app',0o755),'/new/config':(b'private',0o600)})
   after=CheckedImage(disk,allow_identity=True)
   assert after.sb[1]==5 and after.data[52:76]==uuid,'installer must retain the identity volume'
   for path,data in files.items():
    old_id,old_revision=identities[path];new_id,new_revision=ref(after,path)
    assert new_id==old_id,'installer must retain object identity'
    if path=='/bin/agentd':assert new_revision==old_revision+1 and after.payload(after.resolve(path))==b'new app'
    else:assert new_revision==old_revision and after.payload(after.resolve(path))==data,'unrequested bytes and revision remain identical'
   assert ref(after,'/empty')==empty,'empty Project remains the same object'
   assert ref(after,'/new/config')[0]>max(v[0] for v in identities.values()),'new identity cannot reuse old IDs'
   assert hashlib.sha256(source.read_bytes()).digest()==digest,'source image stays unchanged'
   if negative:raise RuntimeError('identity-loss control was not caught')
  except AssertionError as e:
   if not negative or str(e)!='installer must retain the identity volume':raise
   print('PROJECT_INSTALL_PASS lost-identity negative assertion',flush=True)
  else:print('PROJECT_INSTALL_PASS update preserves identities, revisions and untouched bytes',flush=True)
