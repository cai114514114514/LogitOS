#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile real accounting helpers; a deterministic shared-shard control precedes PASS."""
import argparse
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', required=True, type=Path)
p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
a = p.parse_args()
root = a.source_root.resolve()
build = a.build.resolve()
build.mkdir(parents=True, exist_ok=True)
header = (root / 'c/kernel/sched/kbench.h').read_text()
test = root / 'tests/unit/bkl_kbench_test.c'
cc = shutil.which('clang') or shutil.which('cc')
assert cc, 'host C compiler unavailable'
anchor = '    struct kb_sys_cpu *s = &g_kb_sys[cpu];'
assert header.count(anchor) == 1, 'shared-shard mutation anchor changed'
control = header.replace(anchor, '    cpu = 0; /* negative control: shared counter shard */\n' + anchor)
for name, source in [('shared-cpu0-negative', control), ('positive', header)]:
    case = build / name
    case.mkdir(exist_ok=True)
    (case / 'kbench.h').write_text(source)
    exe = case / 'kbench-test'
    compile_result = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                    '-pthread', '-I', str(case), str(test), '-o', str(exe)],
                                   text=True, capture_output=True, timeout=60)
    (case / 'compile.log').write_text(compile_result.stdout + compile_result.stderr)
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=30)
    log = result.stdout + result.stderr
    (case / 'run.log').write_text(log)
    if name.endswith('negative'):
        assert result.returncode == 1 and 'FAIL: each CPU owns its own syscall counters' in log, log
        print('caught shared CPU0 negative: per-CPU ownership assertion')
    else:
        assert result.returncode == 0 and '320000 exact syscall records, PASS' in log, log
        print(log.strip())
# Integration contracts around the real tested helper, not timing measurements.
report = (root / 'c/kernel/sched/kbench.c').read_text()
dispatch = (root / 'c/kernel/exec/syscall.c').read_text()
entry = (root / 'c/kernel/cpu/interrupts.c').read_text()
assert 'snapshot.n[best] = 0' in report
assert 'g_kb_sys_n' not in report and 'g_kb_sys_cyc' not in report
assert '__atomic_store_n(&g_kb_stat' in report
assert 'kb_sys_record((unsigned)this_cpu()->index' in dispatch
assert 'kb_entry_record(' in entry and 'kb_stat_enabled()' in entry
print('bkl kbench integration: private report snapshot, atomic flag, exit CPU accounting PASS')
