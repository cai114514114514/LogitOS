#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual memory/task/store integration with semantic controls and real files."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def execute(build, name, source, defines=(), sanitize=False):
    out = build / name
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'test'
    command = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Ic/lib/agent', 'tests/unit/agent_memory_test.c', str(source),
               'c/lib/agent/task.c', 'c/lib/agent/store.c', 'c/drivers/block/crc32.c',
               '-o', str(exe)]
    command += ['-D' + value for value in defines]
    if sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='agm-', dir='/tmp') as directory:
        result = subprocess.run([str(exe), directory], cwd=ROOT, capture_output=True, text=True)
    text = result.stdout + result.stderr
    (out / 'test.log').write_text(text)
    return result.returncode, text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'c/lib/agent/memory.c'
    source = original.read_text()
    if args.negative:
        variants = [
            ('identity', source.replace('if(strcmp(t->app_id,id))return AG_E_SCOPE;', '(void)id;'), (),
             'identity: cross-AppID directory load refused'),
            ('capacity', source.replace('if(length>AG_MEMORY_MAX)return AG_E_LIMIT;',
                                        'if(length>AEX_AGENT_DOCUMENT_MAX)return AG_E_LIMIT;'), (),
             'capacity: sixty-four KiB plus one is refused before write'),
            ('revision', source, ('AG_NEG_REVISION',), 'CAS: stale revision cannot overwrite durable memory'),
            ('dedup', source, ('AG_NEG_DEDUP',), 'dedup: same operation and payload has one effect after restart'),
            ('checkpoint', source, ('AG_NEG_CHECKPOINT',), 'restart: explicit memory survives fresh load'),
        ]
        for name, text, defines, marker in variants:
            if not defines and text == source:
                raise SystemExit('mutation anchor missing: ' + name)
            variant = build / (name + '.c')
            variant.write_text(text)
            code, output = execute(build, name, variant, defines)
            if code != 1 or 'FAIL: ' + marker not in output:
                raise SystemExit('INCONCLUSIVE ' + name + '\n' + output)
            print('CONTROL DETECTED:', name, marker)
        return
    for name, sanitize in [('positive', False), ('asan', True)]:
        code, text = execute(build, name, original, sanitize=sanitize)
        print(name + ': ' + text, end='')
        if code:
            raise SystemExit(code)


if __name__ == '__main__':
    main()
