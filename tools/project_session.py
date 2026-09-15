#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Start the Project workspace on a persistent, independent image copy."""
import argparse,os,struct,sys
import mkfs
from pathlib import Path
from project_volume import convert
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);p.add_argument('--disk',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--env',type=Path,required=True);p.add_argument('--limit',type=int,default=32);p.add_argument('qemu',nargs=argparse.REMAINDER);a=p.parse_args()
build=a.build.resolve();disk=a.disk.absolute()
if disk.resolve()==a.source.resolve():raise SystemExit('Project workspace needs an independent disk path')
if not disk.exists():convert(a.source,disk,build/'lfs_snapshot')
with disk.open('rb') as stream:header=stream.read(8)
if len(header)!=8 or struct.unpack('<II',header)!=(mkfs.MAGIC,5):
    raise SystemExit('Existing Project disk must be v5; choose a new independent output path for conversion')
command=[sys.executable,str(ROOT/'tools/agent_session.py'),'--disk',str(disk),'--snapshot-helper',str(build/'lfs_snapshot'),'--env',str(a.env.resolve()),'--state-dir',str(build/'project-sessions'),'--limit',str(a.limit)]
for flag,name in [('broker','agentd'),('finder','files'),('textedit','textedit'),('assistant','assistant'),('agentctl','agentctl')]:command+=['--'+flag,str(build/(name+'.aex'))]
command+=a.qemu
os.execv(sys.executable,command)
