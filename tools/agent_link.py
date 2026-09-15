#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Augment only catalogued app links, retaining their existing object/CRT lists.

Some apps use clib, some mini-libc, and media apps link their own codecs. An
archive supplies only unresolved SDK/libc members; copying those link recipes
would drift. A digest-bound sidecar lets mkaex refuse stale/unwired metadata.
"""
import hashlib,json,pathlib,subprocess,sys
from agent_catalog import entries,manifest
build,ld,*args=sys.argv[1:]
if '-o' not in args:raise SystemExit('agent linker needs -o')
out=pathlib.Path(args[args.index('-o')+1]); name=out.stem
app=next((x for x in entries() if x['name']==name),None)
extra=[]
if app:
    extra=['--start-group',build+'/agent/entry.o',build+'/agent/sdk.a',build+'/agent/libc.a','--end-group','-u','ag_gui_dispatch','-e','_agent_start']
subprocess.run([ld,*args,*extra],check=True)
if app:
    side=dict(id=app['id'],manifest=manifest(app),elf_sha256=hashlib.sha256(out.read_bytes()).hexdigest())
    out.with_suffix(out.suffix+'.agent.json').write_text(json.dumps(side)+'\n')
