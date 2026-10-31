#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the real IRQ dispatch/retirement body with privileged leaves stubbed."""
import argparse
import os
from pathlib import Path
import subprocess


def run_variant(root, base, macro, expected):
    """Require named runtime failures and completion, never compiler failures."""
    build = base.resolve() / (macro or 'positive')
    build.mkdir(parents=True, exist_ok=True)
    source = (root / 'c/drivers/core/irq.c').read_text()
    source = source[source.index('struct irq_slot {'):]
    source = source.replace('__asm__ volatile ("cli");', '/* host: no privileged CLI */')
    if macro == 'IRQ_TEST_PREMATURE_RETIREMENT':
        needle = 'while (__atomic_load_n(&g_slot[i].active, __ATOMIC_ACQUIRE)) io_relax();'
        assert source.count(needle) == 2
        source = source.replace(needle, '/* negative control: premature retirement */')
    (build / 'irq_body.c').write_text(source)
    fixture = (root / 'tests/unit/bkl_irq_test.c').read_text()
    fixture = fixture.replace('#include "irq_body.c"',
                              '#include "' + str(build / 'irq_body.c') + '"')
    (build / 'test.c').write_text(fixture)
    executable = build / 'test'
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g',
               '-pthread', '-fsanitize=address,undefined', str(build / 'test.c'),
               '-o', str(executable)]
    if macro.startswith('PRT_NEGCTL_'):
        command.insert(1, '-D' + macro)
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
    (build / 'result.log').write_text(result.stdout + result.stderr)
    assert 'BKL IRQ:' in result.stdout, (result.stdout, result.stderr)
    if expected:
        assert result.returncode == 1 and 'FAIL: ' + expected in result.stdout, (
            result.stdout, result.stderr)
        print('EXPECTED-FAIL: ' + expected)
    else:
        assert result.returncode == 0, (result.stdout, result.stderr)
    print((macro or 'positive') + ': ' + result.stdout.strip().splitlines()[-1])


def run():
    """Preserve callback lifetime controls and add PIC fallback liveness controls."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--negative-only', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    variants = [('', '')]
    if args.negative_only:
        variants = [
            ('IRQ_TEST_PREMATURE_RETIREMENT', 'retirement waits for an active callback'),
            ('PRT_NEGCTL_NO_CLEANUP_KICK',
             'PIC fallback BSP cleanup permits cross-CPU vector reuse'),
            ('PRT_NEGCTL_NO_LOCAL_CLEANUP',
             'late BSP delivery finishes quarantine without another LAPIC tick'),
            ('PRT_NEGCTL_NO_COMPLETION_KICK',
             'overlapping cleanup and old dispatch converge without timer ticks'),
        ]
    for macro, expected in variants:
        run_variant(root, args.build, macro, expected)


if __name__ == '__main__':
    run()
