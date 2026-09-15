#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Require exact, scoped failures from deliberately disabled product rules."""
import argparse
import pathlib
import re
import shlex
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--cc',default='clang')
parser.add_argument('--build',required=True)
args=parser.parse_args()
build=pathlib.Path(args.build)
build.mkdir(parents=True,exist_ok=True)
controls=[('COOKIE_NO_SECURE_OVERLAY','secure',5),
          ('COOKIE_SCHEMELESS_CONTEXT','context',3),
          ('COOKIE_NO_CREATION_CONTEXT','creation',14),
          ('COOKIE_PSL_NO_WILDCARD','psl',4),
          ('COOKIE_PARTIAL_HEADER','overflow',7),
          ('COOKIE_NO_EVICT_PREFERENCE','eviction',3)]
for define,group,count in controls:
    exe=build / ('cookie-hardening-'+define)
    subprocess.run(shlex.split(args.cc)+['-O2','-Wall','-Wextra','-Wno-unused-parameter',
                   '-Ic/net/http','-D'+define,'tests/unit/cookie_hardening_test.c',
                   'c/net/http/cookies.c','-o',str(exe)],check=True)
    result=subprocess.run([str(exe.resolve())],capture_output=True,text=True)
    (build / (define+'.log')).write_text(result.stdout+result.stderr)
    failures=[line for line in result.stdout.splitlines() if line.startswith('FAIL ')]
    summary=re.search(r'cookie hardening: (\d+) checks, (\d+) failures',result.stdout)
    if result.returncode != 1 or len(failures) != count or not summary or \
       int(summary[2]) != count or any(not line.startswith('FAIL '+group+':') for line in failures):
        print(result.stdout+result.stderr)
        raise SystemExit(f'{define}: expected exactly {count} {group} failures, got rc={result.returncode}')
    print(f'{define}: observed {count} expected failures; {failures[0]}')
print('cookie hardening controls: all six rule removals rejected')
