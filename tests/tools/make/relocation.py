#!/usr/bin/env python3
"""Check incremental source relocation against the real Makefile graph.

A clean BUILD cannot reproduce this failure: an imported old .d adds the removed
primary source to a reused object, and -MP protects headers only. Old objects are
deliberately newer than the moved sources so dropping .d files alone cannot pass.
This is graph coverage, not a substitute for compiling or booting the product.
"""
import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


@dataclass(frozen=True)
class Case:
    program: str
    source: str
    old_source: str
    obj: str
    old_obj: str


def cases():
    result = [Case('clock', f'c/apps/gui/clock/{name}.c',
                   f'c/apps/clock/{name}.c', f'apps/gui/clock/{name}.o',
                   f'apps/clock/{name}.o') for name in ('main', 'model')]
    for source in sorted((ROOT / 'c/apps/gui/studio/core').glob('*.c')):
        result.append(Case('studio', str(source.relative_to(ROOT)),
                           f'c/apps/studio/{source.name}',
                           f'apps/gui/studio/core/{source.stem}.o',
                           f'apps/studio/{source.stem}.o'))
    moved_cli = {'httpd': 'c/apps/services/http', 'syslogd': 'c/apps/services/syslog',
                 'sftpd': 'c/apps/services/ssh', 'smptest': 'tests/guest/sched',
                 'socktest': 'tests/guest/net', 'sndtest': 'tests/guest/audio',
                 'crash': 'tests/guest/exec'}
    for name, directory in moved_cli.items():
        source = f'{directory}/{name}.c'
        result.append(Case(name, source, f'c/apps/coreutils/{name}.c',
                           f'cliobj/{directory}/{name}.o', f'apps/{name}.cli.o'))
    groups = [('pollobj', 'exec', 'polltest', ('', '-nopoll')),
              ('schedobj', 'sched', 'schedtest', ('',)),
              ('throbj', 'sched', 'thrtest',
               ('', '-serial', '-barrier', '-tls', '-c11', '-nolock', '-leak', '-noguard'))]
    for group, subsystem, source, suffixes in groups:
        for suffix in suffixes:
            name = source + suffix
            result.append(Case(name, f'tests/guest/{subsystem}/{source}.c',
                               f'c/apps/coreutils/{source}.c',
                               f'{group}/tests/guest/{subsystem}/{name}.o',
                               f'{group}/{name}.o'))
    return result


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def run_make(build, goals, override=None):
    command = ['make', '--no-print-directory', '-n', '-f', 'Makefile']
    if override:
        command += ['-f', str(override)]
    command += [f'BUILD={build}', 'POLL_NEGCTL=1', 'THR_NEGCTL=1', *goals]
    # A parent `make -n` or inherited jobserver must not change what this nested
    # probe asks. The child intentionally performs no recipes, only graph walks.
    env = {key: value for key, value in os.environ.items()
           if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES')}
    return subprocess.run(command, cwd=ROOT, env=env, text=True,
                          capture_output=True, timeout=60)


def compiles(output):
    # Join continuations before tokenizing, like the actual shell recipe.
    output = re.sub(r'\\\r?\n[ \t]*', ' ', output)
    found = set()
    for line in output.splitlines():
        if ' -c ' not in line or ' -o ' not in line:
            continue
        words = shlex.split(line)
        if '-c' in words and '-o' in words:
            found.add((words[words.index('-c') + 1], words[words.index('-o') + 1]))
    return found


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check(control):
    inventory = cases()
    with tempfile.TemporaryDirectory(prefix='logitos-build-relocation-') as temp:
        build = Path(temp) / 'build'
        # The moved .c files can be hours older than a freshly edited shared
        # header. Basing the sentinel on source mtimes alone made the supposed
        # idle object rebuild before this test changed its retained header.
        timestamp = max(time.time(), max((ROOT / case.source).stat().st_mtime
                                        for case in inventory)) + 3600
        saved = {}
        for case in inventory:
            require(not (ROOT / case.old_source).exists(),
                    f'fixture no longer represents a removed source: {case.old_source}')
            obj = build / case.old_obj
            dep = obj.with_suffix('.d')
            write(obj, 'stale object: must never be linked\n')
            os.utime(obj, (timestamp, timestamp))
            write(dep, f'{obj}: {case.old_source} \\\n  include/abi/logit_abi.h\ninclude/abi/logit_abi.h:\n')
            saved[obj] = (obj.read_bytes(), obj.stat().st_mtime_ns)
            saved[dep] = (dep.read_bytes(), dep.stat().st_mtime_ns)

        if control:
            # Restore only the legacy Clock object in the graph. This must fail
            # on the missing old source, not on a compiler/toolchain accident.
            override = Path(temp) / 'legacy.mk'
            old = build / 'apps/clock/main.o'
            write(override, f'CLOCK_UI_OBJ := {old}\n{build}/clock.elf: $(CLOCK_UI_OBJ)\n')
            result = run_make(build, [f'{build}/clock.elf'], override)
            require(result.returncode != 0 and 'No rule to make target' in result.stderr
                    and 'c/apps/clock/main.c' in result.stderr,
                    'legacy-object control did not reproduce the missing source:\n' + result.stderr)
            print('build-relocation negative control: legacy Clock object rejects removed main.c')
        else:
            goals = [f'{build}/{name}.elf' for name in sorted({c.program for c in inventory})]
            result = run_make(build, goals)
            require(result.returncode == 0, result.stderr)
            commands = compiles(result.stdout)
            for case in inventory:
                require((case.source, str(build / case.obj)) in commands,
                        f'no fresh compile for {case.source} -> {case.obj}')
                require(str(build / case.old_obj) not in result.stdout,
                        f'legacy object still appears in a recipe: {case.old_obj}')

            # Header tracking must survive the fix. A newer object first stays
            # idle, then a newer header from its .d alone must schedule a compile.
            clock = inventory[0]
            obj, header = build / clock.obj, build / 'retained-header.h'
            write(obj, 'current object sentinel\n')
            os.utime(obj, (timestamp, timestamp))
            write(header, '/* dependency sentinel */\n')
            os.utime(header, (timestamp - 1, timestamp - 1))
            write(obj.with_suffix('.d'), f'{obj}: {clock.source} {header}\n{header}:\n')
            idle = run_make(build, [str(obj)])
            require(idle.returncode == 0 and (clock.source, str(obj)) not in compiles(idle.stdout),
                    'current object was not idle before the header change: ' + idle.stderr)
            os.utime(header, (timestamp + 1, timestamp + 1))
            changed = run_make(build, [str(obj)])
            require(changed.returncode == 0 and (clock.source, str(obj)) in compiles(changed.stdout),
                    'a changed header in the retained .d no longer schedules compilation')
            print(f'build-relocation: {len(inventory)} moved objects compile at current paths; header tracking retained')

        for path, original in saved.items():
            require((path.read_bytes(), path.stat().st_mtime_ns) == original,
                    f'graph inspection altered an old build artifact: {path}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    try:
        check(args.control)
    except (AssertionError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f'build-relocation: FAIL: {error}')
