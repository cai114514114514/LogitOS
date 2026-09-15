#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded production Unix/FD ownership gates; temporary controls run first."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def body(source, name):
    match = re.search(r'^(?:int|void|struct file \*)\s*' + name +
                      r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError('missing production function ' + name)
    pos, depth = match.end(), 1
    # These FD helper bodies contain no braces inside literals/comments; the
    # complete extracted TU is compiled in both positive and control builds.
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('control no longer identifies one production check: ' + before)
    return source.replace(before, after, 1)


def relocated(source, original):
    # Only the temporary negative fixture moves directories. Resolve its
    # relative includes to the original providers, preserving the real TU.
    return re.sub(r'#include "(\.\./[^"\n]+)"',
                  lambda m: '#include "' + str((original.parent / m[1]).resolve()) + '"', source)


def command(args):
    result = subprocess.run([str(x) for x in args], cwd=ROOT,
                            capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def check(exe, label=None):
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=45)
    text = result.stdout + result.stderr
    exe.with_suffix('.log').write_text(text)
    if label:
        if result.returncode != 1 or 'FAIL: ' + label not in text:
            raise RuntimeError('INCONCLUSIVE ' + exe.name + '\n' + text)
        print(exe.name + ': ' + next(line for line in text.splitlines() if 'FAIL: ' + label in line))
    else:
        print(text, end='')
        if result.returncode:
            raise RuntimeError('positive gate failed: ' + exe.name)


def unix_gate(build, name, control=None):
    directory = build / name
    directory.mkdir(parents=True, exist_ok=True)
    flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-pthread',
             '-D_FORTIFY_SOURCE=0', '-fsanitize=address,undefined']
    if control:
        original = ROOT / 'c/net/core/unix.c'
        source = original.read_text()
        if control == 'peer':
            source = replace_once(source, 'live.uid==id->uid && live.gid==id->gid ? 0 : -1;',
                                  '1 ? 0 : -1;')
        else:
            source = replace_once(source, '            if (!owner_valid(s)) return LSK_E_PERM;', '')
            source = replace_once(source, '            if (!owner_valid(s)) return sent > 0 ? sent : LSK_E_PERM;', '')
        (directory / 'unix.c').write_text(relocated(source, original))
        flags += ['-I' + str(directory)]
    flags += ['-Itests/unit/unixstub', '-Ic/net/core', '-Iinclude/abi', '-Ic/fs']
    exe = directory / name
    command(flags + ['tests/unit/agent_unix_test.c', '-o', exe])
    labels = {'peer': 'identity: changed peer uid refused',
              'wait': 'wait identity: resumed read refused'}
    check(exe, labels.get(control))


def fd_gate(build, name, control=None):
    directory = build / name
    directory.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'c/kernel/exec/proc.c').read_text()
    names = ['proc_fd_alloc', 'proc_fd_acquire', 'proc_fd_close',
             'proc_fd_take_exclusive', 'proc_fd_dup2', 'proc_fd_clone', 'proc_fd_close_all']
    fd_source = '#include <stddef.h>\n#include "proc.h"\n#include "file.h"\n'
    fd_source += '\n'.join(body(source, name) for name in names)
    if control == 'alias':
        fd_source = replace_once(fd_source, 'file_refs_equal(f,2)', '(f!=NULL)')
    fd = directory / 'fd.c'
    fd.write_text(fd_source)

    # Existing storage model with its aborting scheduler stubs replaced by the
    # pollhost scheduler, exactly as the BKL FD gate links the real file TU.
    model = (ROOT / 'tests/unit/storhost/hostmodel.c').read_text()
    start = model.index('/* --- locks /')
    end = model.index('/* --- the VFS model', start)
    model = model[:start] + '''
void sched_poll_wait(void) { abort(); }
void ksig_tty_claim_fg(void) { abort(); }
int ksig_tty_getc(void) { abort(); }
int ksig_interrupted(void) { return 0; }
struct waitq *ksig_tty_waitq(void) { abort(); }
int ksig_tty_avail(void) { return 0; }
int ksig_post_current(int signo) { (void)signo; return 0; }
''' + model[end:]
    model_path = directory / 'model.c'
    model_path.write_text(model)
    file_source = ROOT / 'c/kernel/exec/file.c'
    if control == 'lock':
        source = file_source.read_text()
        original = body(source, 'file_refs_equal')
        changed = replace_once(original, 'uint64_t fl = spin_lock_irqsave(&g_file_lock);', '')
        changed = replace_once(changed, 'spin_unlock_irqrestore(&g_file_lock, fl);', '')
        source = replace_once(source, original, changed)
        temporary = directory / 'file.c'
        temporary.write_text(relocated(source, file_source))
        file_source = temporary
    flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
             '-Wno-unused-function', '-Wno-unused-variable', '-pthread',
             '-fsanitize=address,undefined']
    for include in ['c', 'c/kernel/exec', 'c/kernel/mm', 'c/kernel/core',
                    'c/kernel/cpu', 'c/kernel/sched', 'c/fs', 'c/drivers/char',
                    'c/drivers/timer', 'include/abi']:
        flags += ['-iquote', str(ROOT / include)]
    file_object = directory / 'file.o'
    command(flags + ['-Dspin_lock_irqsave=agent_file_lock', '-c', file_source, '-o', file_object])
    exe = directory / name
    command(flags + ['tests/unit/agent_fd_test.c', model_path, fd,
                     'tests/unit/pollhost/hostsched.c', 'c/kernel/core/wait.c',
                     'c/kernel/exec/kpoll.c', file_object, '-o', exe])
    labels = {'lock': 'references: query participates in file lock',
              'alias': 'handoff: temporary reference keeps parent descriptor'}
    check(exe, labels.get(control))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', required=True, type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--negative', action='store_true')
    mode.add_argument('--positive', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    if not args.positive:
        unix_gate(build, 'unix-peer-control', 'peer')
        unix_gate(build, 'unix-wait-control', 'wait')
        fd_gate(build, 'fd-lock-control', 'lock')
        fd_gate(build, 'fd-alias-control', 'alias')
    if not args.negative:
        unix_gate(build, 'unix-positive')
        fd_gate(build, 'fd-positive')


if __name__ == '__main__':
    main()
