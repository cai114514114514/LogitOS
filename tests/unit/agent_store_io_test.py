#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);a=p.parse_args();a.build=a.build.resolve();a.build.mkdir(parents=True,exist_ok=True)
source=(ROOT/'c/lib/agent/store.c').read_text()
variants=[('negative-fsync',source.replace('if(!r && fsync(fd)<0)r=AG_E_IO;','(void)fsync(fd);'),'fsync failure'),
          ('negative-close',source.replace('if(close(fd)<0)r=AG_E_IO;','(void)close(fd);'),'close failure'),('positive',source,None)]
for name,text,label in variants:
    if label and text==source:raise SystemExit('mutation anchor missing')
    local=a.build/(name+'.inc');local.write_text(text);exe=a.build/name
    subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'c/lib/agent'),'-DAG_STORE_SOURCE="'+str(local)+'"','tests/unit/agent_store_io_test.c','c/lib/agent/task.c','c/drivers/block/crc32.c','-o',str(exe)],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='agent-store-') as directory:r=subprocess.run([str(exe),directory],capture_output=True,text=True)
    if label:
        if r.returncode!=1 or 'FAIL: '+label not in r.stderr:raise SystemExit('inconclusive control '+name+'\n'+r.stdout+r.stderr)
        print(name+': '+r.stderr.strip())
    else:r.check_returncode();print(r.stdout,end='')
