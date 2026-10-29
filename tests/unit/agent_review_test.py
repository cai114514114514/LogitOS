#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A real failed CAS assertion is a prerequisite of the positive review gate."""
import argparse, pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
for negative in (True,False):
    exe=a.build.resolve()/('negative-review-version' if negative else 'review')
    cmd=['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','tests/unit/agent_review_test.c','c/lib/agent/review.c','c/lib/agent/store.c','c/lib/agent/task.c','c/drivers/block/crc32.c','-o',str(exe)]
    if negative:cmd+=['-DAG_NEG_REVIEW_VERSION']
    subprocess.run(cmd,cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='textedit-review-') as directory:r=subprocess.run([str(exe),directory],capture_output=True,text=True)
    if negative:
        if r.returncode!=1 or 'FAIL: review version:' not in r.stderr:raise SystemExit('inconclusive review negative control\n'+r.stdout+r.stderr)
        print('AG_NEG_REVIEW_VERSION: rejected by stale document assertion')
    else:
        print(r.stdout+r.stderr,end='');r.check_returncode()
