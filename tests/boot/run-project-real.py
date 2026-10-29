#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Own one bounded provider gateway and run the native Project acceptance."""
import argparse,json,signal,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools'))
from agent_session import stop
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--env',type=Path,required=True);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);out.chmod(0o700)
def interrupted(signum,frame):raise KeyboardInterrupt
signal.signal(signal.SIGTERM,interrupted)
gateway=guest=None
try:
 with (out/'gateway.log').open('w') as log:
  gateway=subprocess.Popen([sys.executable,str(ROOT/'tools/agent_gateway.py'),'--env',str(a.env.resolve()),'--state',str(out/'gateway'),'--limit','8'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
  for _ in range(100):
   if (out/'gateway/ready.json').exists():break
   if gateway.poll() is not None:raise RuntimeError('model service did not start; see gateway.log')
   time.sleep(.1)
  else:raise RuntimeError('model service readiness deadline')
  guest=subprocess.Popen([sys.executable,str(ROOT/'tests/boot/run-project-work.py'),'--build',str(a.build.resolve()),'--out',str(out/'guest'),'--gateway',str(out/'gateway')],cwd=ROOT)
  if guest.wait():raise RuntimeError('native Project acceptance failed')
  result=json.loads((out/'guest/result.json').read_text());assert result['passed'] and result['real_model']
  (out/'result.json').write_text(json.dumps({'passed':True,'real_model':True,'checks':len(result['checks']),'provider':json.loads((out/'gateway/metrics.json').read_text())},indent=2));print('PROJECT_REAL_PASS native apps and real model',flush=True)
finally:
 stop(guest);stop(gateway)
