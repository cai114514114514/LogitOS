#!/usr/bin/env python3
"""Exercise production fanout, IRQ lifetime and IOAPIC register transactions."""
from pathlib import Path
import argparse
import os
import subprocess

ap = argparse.ArgumentParser()
ap.add_argument('--build', type=Path, required=True)
ap.add_argument('--negative-only', action='store_true')
ap.add_argument('--x2apic-only', action='store_true')
a = ap.parse_args()
root = Path(__file__).resolve().parents[2]
base = a.build.resolve()
irq = (root / 'c/drivers/core/irq.c').read_text()
irq = irq[irq.index('struct irq_slot {'):].replace('__asm__ volatile ("cli");', '/* host CLI leaf */')
ioapic = (root / 'c/kernel/cpu/irq/ioapic.c').read_text()
ioapic = ioapic[ioapic.index('int ioapic_present(void)'):]
variants = [('', None)] if not a.negative_only and not a.x2apic_only else [
    ('PCI_INTX_NEGCTL_FIRST_ONLY', 'one shared interrupt services both pending devices'),
    ('PCI_INTX_NEGCTL_SKIP_MASK', 'masked stale route cannot hit a reused vector'),
    ('PCI_INTX_NEGCTL_NO_MEMBER_DRAIN', 'non-last removal waits for its active callback'),
    ('LOGIT_X2APIC_NEGCTL_SKIP_ROUTE_READBACK', 'dropped IOAPIC route readback blocks source enable'),
    ('LOGIT_X2APIC_NEGCTL_SKIP_LEGACY_ROLLBACK', 'legacy route failure rolls back every earlier unmasked GSI'),
    ('LOGIT_X2APIC_NEGCTL_ALLOW_DUPLICATE_GSI', 'duplicate MADT legacy GSIs are rejected before any route is enabled'),
    ('LOGIT_X2APIC_NEGCTL_FREE_UNSAFE_INTX', 'unsafe first INTx route quarantines its destination vector'),
    ('LOGIT_X2APIC_NEGCTL_SKIP_INTX_SOURCE_READBACK',
     'PCI INTx command readback guards setup and release ownership'),
    ('IRQ_NEGCTL_EARLY_EOI', 'last-owner retirement waits for the old EOI'),
]
if a.x2apic_only:
    variants = [v for v in variants if v[0].startswith('LOGIT_X2APIC_')]
for macro, expected in variants:
    build = base / (macro.lower() if macro else 'positive')
    build.mkdir(parents=True, exist_ok=True)
    body = irq
    if macro == 'IRQ_NEGCTL_EARLY_EOI':
        needle = 'if (fn) fn(arg);'
        assert body.count(needle) == 1
        body = body.replace(needle, '''if (fn) {
            fn(arg);
            __atomic_fetch_sub(&g_slot[i].active, 1, __ATOMIC_RELEASE);
            borrowed = 0; /* negative: recycle before EOI */
        }''')
    (build / 'irq_body.c').write_text(body)
    (build / 'ioapic_body.c').write_text(ioapic)
    fixture = (root / 'tests/unit/pci_intx_test.c').read_text()
    for name in ('irq_body.c', 'ioapic_body.c'):
        fixture = fixture.replace(f'#include "{name}"', f'#include "{build / name}"')
    (build / 'test.c').write_text(fixture)
    cmd = [os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
           '-pthread', '-fsanitize=address,undefined', '-DLOGIT_HOST_TEST',
           '-Itests/unit/pcistub', '-Ic/drivers/core', '-Ic/kernel/pci', '-Ic/kernel/cpu -Ic/kernel/cpu/acpi -Ic/kernel/cpu/irq -Ic/kernel/cpu/smp',
           str(build / 'test.c'), 'c/kernel/pci/pci_msi.c',
           'c/kernel/cpu/irq/apic_model.c', '-o', str(build / 'test')]
    if macro.startswith('PCI_') or macro.startswith('LOGIT_'):
        cmd.insert(1, '-D' + macro)
    subprocess.run(cmd, cwd=root, check=True)
    result = subprocess.run([str(build / 'test')], capture_output=True, text=True, timeout=20)
    (build / 'result.log').write_text(result.stdout + result.stderr)
    if expected:
        assert result.returncode == 1 and f'FAIL: {expected}' in result.stdout, (macro, result.stdout, result.stderr)
        if a.x2apic_only:
            failed = [line for line in result.stdout.splitlines() if line.startswith('FAIL: ')]
            assert failed == ['FAIL: ' + expected], (macro, failed, result.stderr)
            assert 'PCI INTx: 1 checks, 1 failures' in result.stdout, result.stdout
    else:
        assert result.returncode == 0, (result.stdout, result.stderr)
    print((macro or 'positive') + ': ' + result.stdout.strip().splitlines()[-1])
