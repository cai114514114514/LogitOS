#!/usr/bin/env python3
"""Reproducible static-PIE fixture, actual-MMU host checks and semantic control.

Darwin runs the host harness as x86_64 (Rosetta on Apple Silicon): 4 KiB guest
pages need independently enforceable 4 KiB host pages. ASan reserves 1 TiB as
shadow, so its explicit PIE base is 32 TiB; ordinary host and guest use 1 TiB.
Neither deterministic base selection is described as ASLR.
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--build', type=Path, required=True)
p.add_argument('--fixtures-only', action='store_true')
p.add_argument('--negative-only', action='store_true')
p.add_argument('--positive-only', action='store_true')
a = p.parse_args()
r, b = a.root.resolve(), a.build.resolve()
b.mkdir(parents=True, exist_ok=True)
def run(cmd, **kw):
    print('+', ' '.join(map(str, cmd)), flush=True)
    return subprocess.run(list(map(str, cmd)), cwd=r, check=True, **kw)
cc, ld, nasm = os.environ.get('CC', 'clang'), os.environ.get('LD', 'ld.lld'), os.environ.get('ASM', 'nasm')
for tool in [cc, ld, nasm]:
    if not shutil.which(tool):
        raise SystemExit('SKIP: required tool unavailable: '+tool)
run([cc, '--target=x86_64-elf', '-ffreestanding', '-nostdlib', '-fPIE', '-ftls-model=local-exec', '-fno-stack-protector', '-mno-red-zone', '-O2', '-Ic/apps', '-Iinclude/abi', '-c', 'tests/unit/pie_program.c', '-o', b/'program.o'])
run([nasm, '-f', 'elf64', 'c/apps/crt0_cli.asm', '-o', b/'crt.o'])
run([nasm, '-f', 'elf64', 'tests/unit/pie_thread.asm', '-o', b/'thread.o'])
run([ld, '-pie', '--no-dynamic-linker', '-nostdlib', '-z', 'text', '-z', 'relro', '-z', 'now', '-e', '_start', '-o', b/'program.elf', b/'crt.o', b/'program.o', b/'thread.o'])
# Packaging uses the production tool; duplicating AEX generation here would
# let a test fixture work while developers still cannot package their PIE.
run(['python3', 'tools/mkaex.py', '--cli', b/'program.elf', b/'program.aex', 'pie', '-', '*', '150', '150', '150'])
if a.fixtures_only:
    raise SystemExit(0)
if platform.system() != 'Darwin' and platform.machine() not in ('x86_64', 'AMD64'):
    raise SystemExit('SKIP: mapped x86 execution requires an x86_64 host; run test-pie on x86_64 or Darwin/Rosetta')
flags = ['-arch', 'x86_64'] if platform.system() == 'Darwin' else []
base = [cc, *flags, '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-DLOGIT_HOSTTEST', '-Ic/kernel/exec', '-Itests/unit/exechost', '-Ic/crypto', '-Ic/crypto/trust', '-Ic/drivers/block']
src = ['tests/unit/pie_host.c', 'tests/unit/pie_space.c', 'c/kernel/exec/elf.c', 'c/drivers/block/crc32.c', 'c/crypto/hash/sha256.c']
for name, more in ([] if a.negative_only else [('host', []), ('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DELF_PIE_BASE=0x200000000000ull'])]):
    exe=b/('pie_'+name)
    run([*base, *more, *src, '-o', exe])
    result=run([exe,b/'program.elf'], env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'},capture_output=True,text=True)
    (b/(name+'.log')).write_text(result.stdout+result.stderr)
    print(result.stdout, end='')
if a.positive_only:
    raise SystemExit(0)
exe=b/'pie_neg'
run([*base, '-DELF_PIE_NEGCTL_NORELOC', *src, '-o',exe])
res=subprocess.run([exe,b/'program.elf'],cwd=r,capture_output=True,text=True)
(b/'negative.log').write_text(res.stdout+res.stderr)
if res.returncode != 1 or any('FAIL '+n not in res.stderr for n in ['global pointer resolves','function pointer resolves','TLS copies relocated pointer']):
    raise SystemExit('FAIL: disabled relocation did not fail the named semantic checks (a crash does not count)')
print('PASS: disabled relocation rejected by global/function/TLS pointer checks')
