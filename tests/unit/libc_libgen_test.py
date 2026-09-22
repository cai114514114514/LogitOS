#!/usr/bin/env python3
"""Compare every libgen case, retaining Logit's documented // root policy.

Darwin collapses dirname("//foo") to "/"; Logit deliberately follows GNU and
keeps "//". A byte-for-byte comparison treated that permitted choice as a
regression. Check our policy explicitly and compare all other fields exactly.
No case is dropped, including the basename field on special-root rows.
"""
import argparse
from pathlib import Path
import platform
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[2]
ROW = re.compile(r"(?P<id>\d+)\t\[(?P<path>.*)\]\tbasename=\[(?P<base>.*)\]\tdirname=\[(?P<dir>.*)\]")


def run(command):
    return subprocess.run(command, cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE).stdout


def compare(ours, reference):
    left, right = ours.splitlines(), reference.splitlines()
    failures = []
    if len(left) != len(right) or len(left) < 2:
        return ["case count differs or fixture produced no cases"]
    if left[0] != "NULL\tbasename=[.]\tdirname=[.]" or left[0] != right[0]:
        failures.append("NULL path contract")
    for index, (actual, expected) in enumerate(zip(left[1:], right[1:])):
        a, b = ROW.fullmatch(actual), ROW.fullmatch(expected)
        if not a or not b or a['id'] != str(index) or b['id'] != str(index):
            failures.append(f"row {index}: malformed output or missing case")
            continue
        path = a['path']
        if (path, a['base']) != (b['path'], b['base']):
            failures.append(f"row {index}: input/basename differs")
        # Only an exactly-two-slash root with at most one component returns
        # the implementation-defined root itself. Internal // stays subject
        # to the ordinary byte comparison, not a global slash normalization.
        special = (path.startswith('//') and not path.startswith('///')
                   and '/' not in path[2:].rstrip('/'))
        if special:
            if a['dir'] != '//' or b['dir'] not in ('/', '//'):
                failures.append(f"row {index}: double-slash root contract")
        elif a['dir'] != b['dir']:
            failures.append(f"row {index}: dirname differs")
    return failures


def build_common(cc, output):
    resource = run(cc + ['-print-resource-dir']).strip()
    flags = ['-std=c11', '-O1', '-fno-builtin']
    includes = ['-nostdinc', '-isystem', resource + '/include',
                '-Ic/apps/libc/include', '-Ic/apps/libc/include/uonly',
                '-Ic/apps/libc/src']
    strings = output / 'libgen_strings.o'
    bridge = output / 'libgen_runtime.o'
    run(cc + flags + includes + ['-include', 'tests/unit/libc_rename.h', '-c',
        'c/apps/libc/src/string.c', '-o', str(strings)])
    run(cc + flags + ['-c', 'tests/unit/libc_string_host.c', '-o', str(bridge)])
    reference = output / 'libgen_reference'
    run(cc + flags + ['tests/unit/libc_libgen_test.c', '-o', str(reference)])
    # err.c passes opaque FILE pointers to host vfprintf. Darwin exports the
    # stream as __stderrp, not stderr; redirect the symbol, never its layout.
    if platform.system() == 'Darwin':
        includes += ['-Dstderr=__stderrp']
    return flags + includes, [str(strings), str(bridge)], reference


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--controls', action='store_true')
    args = parser.parse_args()
    output = args.build.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(args.cc)
    flags, objects, reference = build_common(cc, output)
    reference_text = run([str(reference)])
    (output / 'libgen.reference.out').write_text(reference_text)
    source = ROOT / 'c/apps/libc/src/libgen.c'
    variants = [('ours', source, None)]
    if args.controls:
        variants = []
        mutations = [
            ('root-control', 'if (last_slash == path + 1)', 'if (0)',
             'double-slash root contract'),
            ('trailing-control', "memrchr(path, '/', (size_t)(runp - path))",
             "strrchr(path, '/')", 'row 6: dirname differs'),
        ]
        original = source.read_text()
        for name, before, after, marker in mutations:
            if original.count(before) != 1:
                raise RuntimeError('control anchor changed: ' + name)
            mutant = output / (name + '.c')
            mutant.write_text(original.replace(before, after))
            variants.append((name, mutant, marker))
    for name, implementation, marker in variants:
        binary = output / ('libgen_' + name)
        run(cc + flags + ['-Dmemrchr=mini_memrchr',
            'tests/unit/libc_libgen_test.c', str(implementation),
            'c/apps/libc/src/err.c', 'tests/unit/libc_host_errno_shim.c',
            *objects, '-o', str(binary)])
        text = run([str(binary)])
        (output / ('libgen.' + name + '.out')).write_text(text)
        failures = compare(text, reference_text)
        (output / (name + '.checks')).write_text('\n'.join(failures) + '\n')
        if marker:
            if not any(marker in failure for failure in failures):
                raise RuntimeError('control missed expected failure: ' + name)
            print(f'CONTROL PASS: {name}: {len(failures)} expected failures; {marker}')
        elif failures:
            raise RuntimeError('\n'.join(failures))
        else:
            print(f'PASS: libgen {len(text.splitlines())} cases; host comparison '
                  'and explicit double-slash contract')


if __name__ == '__main__':
    main()
