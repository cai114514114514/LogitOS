#!/usr/bin/env python3
"""Check real cache read batching and the real ptrace translation helper.

The ptrace function is extracted verbatim solely to avoid modeling unrelated
process tables/stop scheduling; the guest ptrace gate owns that integration.
Both controls restore the exact former identity-only operation, must compile,
and must fail a named assertion rather than crashing in the apparatus.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--ptrace-only', action='store_true', help='run the ptrace/VMM copy gate independently')
    args = ap.parse_args()
    root = args.root.resolve()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DMM_HOSTTEST',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             # c/kernel/{mm,exec} were split into subdirectories on 2026-09-15 and
             # this gate passes its own narrow include path, not INCDIRS.
             '-I' + str(root / 'tests/unit/mmstub'), '-I' + str(root / 'c/kernel/mm'),
             '-I' + str(root / 'c/kernel/mm/phys'), '-I' + str(root / 'c/kernel/mm/virt'),
             '-I' + str(root / 'c/kernel/mm/cache'), '-I' + str(root / 'c/kernel/mm/reclaim'),
             '-I' + str(root / 'c/kernel/exec'), '-I' + str(root / 'c/kernel/exec/load'),
             '-I' + str(root / 'c/kernel/exec/signal'), '-I' + str(root / 'c/kernel/exec/fd'),
             '-I' + str(build)]

    def check(name, sources, expected=None):
        binary = build / name
        subprocess.run([cc, *flags, *(str(s) for s in sources), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        (build / (name + '.log')).write_text(result.stdout + result.stderr)
        if expected:
            if result.returncode != 1 or ('FAIL: ' + expected) not in result.stdout:
                raise RuntimeError(name + ': old operation did not fail its specific assertion')
            if 'AddressSanitizer' in result.stderr or 'runtime error:' in result.stderr:
                raise RuntimeError(name + ': sanitizer abort is not a valid control')
            print('PASS: ' + name + ' fails its explicit integration assertion')
        else:
            if result.returncode:
                print(result.stdout + result.stderr)
                raise RuntimeError(name + ' failed')
            print(result.stdout.strip().splitlines()[-1])

    if not args.ptrace_only:
        cache = root / 'c/kernel/mm/cache/pcache.c'
        cache_test = root / 'tests/unit/physmap_consumers_test.c'
        check('cache_alias', [cache_test, cache])
        old_cache = build / 'pcache_old_alias.c'
        source = cache.read_text()
        boundary = ' &&\n               (frames[j] < PMM_LOW_LIMIT) == (frames[i] < PMM_LOW_LIMIT)'
        if source.count(boundary) != 1:
            raise RuntimeError('cache alias predicate changed; update the named control')
        old_cache.write_text(source.replace(boundary, ''))
        check('cache_old_alias', [cache_test, old_cache],
              'backend buffer never crosses low/direct-map alias boundary')

    source = (root / 'c/kernel/exec/ptrace.c').read_text()
    start = source.index('static int xlate(')
    end = source.index('\n}\n', start) + 3
    helper = source[start:end]
    include = build / 'physmap_ptrace_xlate.inc'
    include.write_text(helper)
    ptrace_test = root / 'tests/unit/physmap_ptrace_test.c'
    vmm = (root / 'c/kernel/mm/virt/vmm.c').read_text()
    start = vmm.index('int vmm_copy_in_space(')
    end = vmm.index('\n}\n', start) + 3
    copy = vmm[start:end]
    copy_include = build / 'physmap_vmm_copy.inc'
    translation = '(uint8_t *)mm_p2v(p)'
    if copy.count(translation) != 1:
        raise RuntimeError('VMM copy alias changed; update the named control')
    copy_include.write_text(copy.replace(translation, '(uint8_t *)(uintptr_t)p'))
    check('ptrace_old_alias', [ptrace_test],
          'ptrace high frame uses CPU alias rather than physical pointer')
    copy_include.write_text(copy)
    check('ptrace_alias', [ptrace_test])
    print('PASS: ptrace/VMM copy and former physical-pointer control' if args.ptrace_only else 'PASS: high-memory consumers and both former-behavior controls')


if __name__ == '__main__':
    main()
