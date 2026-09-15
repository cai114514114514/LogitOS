#!/usr/bin/env python3
"""Compile the real PMM; prove high RAM/holes/permissions and break the map.

The negative build must compile and fail explicit high-RAM assertions. A linker
failure or sanitizer abort is an apparatus failure, never a passing control.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def run(cmd, **kwargs):
    return subprocess.run([str(x) for x in cmd], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             # c/kernel/mm split into subdirectories on 2026-09-15; this gate passes
             # its own narrow include path, so the subdirectories have to be named.
             '-I' + str(root / 'c/kernel/mm'),
             '-I' + str(root / 'c/kernel/mm/phys'),
             '-I' + str(root / 'c/kernel/mm/virt'),
             '-I' + str(root / 'c/kernel/mm/cache'),
             '-I' + str(root / 'c/kernel/mm/reclaim'),
             '-I' + str(root / 'tests/unit/mmstub')]
    src = root / 'tests/unit/physmap_test.c'
    pmm = root / 'c/kernel/mm/phys/pmm.c'
    for name, defs in [('physmap', []), ('physmap_no_nx', ['-DPHYSMAP_TEST_NO_NX']),
                       ('physmap_no_high', ['-DPHYS_MAP_DISABLE_HIGH']),
                       ('physmap_low_control', ['-DPHYS_MAP_DISABLE_HIGH', '-DPHYSMAP_TEST_LOW_ONLY'])]:
        binary = build / name
        run([cc, *flags, '-DMM_HOSTTEST', *defs, src, pmm, '-o', binary])
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        (build / (name + '.log')).write_text(result.stdout + result.stderr)
        if name == 'physmap_no_high':
            if result.returncode != 1 or 'FAIL: all and only high AVAILABLE pages released' not in result.stdout:
                raise RuntimeError('map-removal control did not fail the high RAM assertion')
            if 'AddressSanitizer' in result.stderr or 'runtime error:' in result.stderr:
                raise RuntimeError('sanitizer failure is not a valid negative control')
            print('PASS: disabling high mappings fails explicit high RAM assertions')
        else:
            if result.returncode:
                print(result.stdout + result.stderr)
                raise RuntimeError(name + ' failed')
            print(result.stdout.strip().splitlines()[-1])
    alias = build / 'physmap_alias'
    run([cc, *flags, root / 'tests/unit/physmap_alias_test.c', '-o', alias])
    run([alias])
    run([sys.executable, root / 'tests/unit/physmap_consumers_test.py',
         '--root', root, '--build', build / 'consumers'])
    print('PASS: physmap and allocation zones (including negative control)')


if __name__ == '__main__':
    main()
