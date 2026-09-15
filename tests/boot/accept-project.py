#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""BIOS/UEFI Project persistence matrix; models here are deterministic fixtures."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);rows=[]
for mode in ('bios','uefi'):
 for ram in ('512M','2G','8G'):
  name=mode+'-'+ram;target=out/name
  with (out/(name+'.log')).open('w') as log:
   subprocess.run([sys.executable,str(ROOT/'tests/boot/run-project-work.py'),'--build',str(a.build.resolve()),'--out',str(target),'--mode',mode,'--ram',ram,'--no-ui'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
  result=json.loads((target/'result.json').read_text());assert result['passed'];rows.append({'mode':mode,'ram':ram,'checks':len(result['checks'])});print('PROJECT_MATRIX_PASS',mode,ram,flush=True)
(out/'result.json').write_text(json.dumps({'passed':True,'real_model':False,'rows':rows},indent=2))
