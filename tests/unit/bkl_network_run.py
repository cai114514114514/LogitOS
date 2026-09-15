#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual socket queues and route table under eight concurrent host threads."""
from pathlib import Path
import argparse, os, subprocess
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');a=ap.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
for unit,source,marker in [('unix','c/net/core/unix.c','FAIL: 32000 duplex records preserve sequence and payload'),('route','c/net/core/route.c','FAIL eight CPUs read complete route publications')]:
    d=b/unit;d.mkdir(exist_ok=True)
    s=(r/source).read_text()
    for old,new in [('"../../drivers/core/io_domain.h"',r/'c/drivers/core/io_domain.h'),('"../../drivers/core/io_lock.h"',r/'c/drivers/core/io_lock.h'),('"../../../include/weaksym.h"',r/'include/weaksym.h')]:
        s=s.replace('#include '+old,'#include "'+str(new)+'"')
    if a.negative_only and unit=='unix':
        for name in ['unix_read','unix_write']:
            start=s.index('long '+name+'(');pos=s.index('    IO_DOMAIN_GUARD(&unix_owner);',start)
            s=s[:pos]+s[pos:].replace('    IO_DOMAIN_GUARD(&unix_owner);','    struct io_domain_guard domain_guard_ __attribute__((cleanup(io_domain_drop))) = {0};',1)
    (d/(unit+'.c')).write_text(s)
    t=(r/('tests/unit/'+unit+'_test.c')).read_text().replace('#include "'+unit+'.c"','#include "'+str(d/(unit+'.c'))+'"');(d/(unit+'_test.c')).write_text(t)
    t=(r/('tests/unit/bkl_'+unit+'_test.c')).read_text().replace('#include "'+unit+'_test.c"','#include "'+str(d/(unit+'_test.c'))+'"');(d/'parallel.c').write_text(t)
    cmd=[os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-Wno-ignored-attributes']
    if a.negative_only and unit=='route':cmd+=['-DIO_NO_LOCK']
    cmd+=['-I'+str(r/p) for p in ['tests/unit','tests/unit/unixstub','c/net/core','include/abi','c/fs']]
    exe=d/'parallel';subprocess.run(cmd+[str(d/'parallel.c'),'-o',str(exe)],check=True)
    p=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);(d/'result.log').write_text(p.stdout+p.stderr)
    if a.negative_only:assert p.returncode==1 and marker in p.stdout,(p.stdout,p.stderr)
    else:assert p.returncode==0,(p.stdout,p.stderr)
    print(unit+(' negative: ' if a.negative_only else ' positive: ')+p.stdout.strip().splitlines()[-1])
