#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build actual config code independently of the model transport and SDK."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(build, name, source, sanitize=False):
    directory = build / name
    directory.mkdir(parents=True, exist_ok=True)
    flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Ic/lib/agent']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    obj, exe = directory / 'config.o', directory / 'test'
    command = flags + ['-Dopen=ag_config_test_open', '-Dread=ag_config_test_read',
                       '-Dclose=ag_config_test_close', '-Dchmod=ag_config_test_chmod',
                       '-c', str(source), '-o', str(obj)]
    subprocess.run(command, cwd=ROOT, check=True)
    subprocess.run(flags + ['tests/unit/agent_model_config_test.c', str(obj), '-o', str(exe)],
                   cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='agent-config-') as fixture:
        result = subprocess.run([str(exe), fixture, str(ROOT / 'fsroot/etc/agent.conf')],
                                cwd=ROOT, capture_output=True, text=True)
    text = result.stdout + result.stderr
    (directory / 'test.log').write_text(text)
    return result.returncode, text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'c/lib/agent/model_config.c'
    source = original.read_text()
    if args.negative:
        start = source.index('static int decimal(')
        end = source.index('static int flag(', start)
        unchecked = ('static int decimal(const char *text,unsigned maximum,unsigned *out)\n'
                     '{(void)maximum;*out=(unsigned)strtoul(text,0,10);return 0;}\n')
        controls = [
            ('unchecked-number', '#include <stdlib.h>\n' + source[:start] + unchecked + source[end:],
             'numeric: entire bounded decimal port required'),
            ('host-separator', source.replace('if(!valid_host(config.host)||',
                                             'if((0&&!valid_host(config.host))||'),
             'header: host separator refused'),
            ('remote-plaintext', source.replace('if(!config.tls&&strcmp(', 'if(0&&!config.tls&&strcmp('),
             'plaintext: remote endpoint refused'),
            ('credential-space', source.replace('if((unsigned char)config.key[i]<33||',
                                               'if((unsigned char)config.key[i]<32||'),
             'credential: embedded whitespace refused'),
            ('credential-protection', source.replace('if(credential&&chmod(path,0600)<0)',
                                                    'if(0&&credential&&chmod(path,0600)<0)'),
             'credential: installed key becomes private before use'),
        ]
        for name, variant, marker in controls:
            path = build / (name + '.c')
            path.write_text(variant)
            code, text = run(build, name, path)
            if code != 1 or 'FAIL: ' + marker not in text:
                raise SystemExit('INCONCLUSIVE ' + name + '\n' + text)
            print('CONTROL DETECTED:', name, marker)
        return
    for name, sanitize in [('positive', False), ('asan', True)]:
        code, text = run(build, name, original, sanitize)
        print(name + ': ' + text, end='')
        if code:
            raise SystemExit(code)


if __name__ == '__main__':
    main()
