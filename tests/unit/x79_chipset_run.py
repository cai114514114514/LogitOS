#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build C600/SPD host fixtures; negative variants must fail for one reason."""
import argparse
import json
import os
import subprocess
from pathlib import Path

P = argparse.ArgumentParser()
P.add_argument('--build', required=True, type=Path)
P.add_argument('--negative-only', action='store_true')
A = P.parse_args()
ROOT = Path(__file__).resolve().parents[2]
OUT = A.build.resolve()
OUT.mkdir(parents=True, exist_ok=True)
CC = os.environ.get('CC', 'clang')

COMMON = [
    CC, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-DLOGIT_HOST_TEST',
    '-I' + str(ROOT / 'tests/unit/x79chipstub'),
    '-I' + str(ROOT / 'tests/unit/pcistub'),
    '-I' + str(ROOT / 'c/drivers/platform'),
    '-I' + str(ROOT / 'c/drivers/core'),
    '-I' + str(ROOT / 'c/kernel/pci'),
]
SOURCES = [
    str(ROOT / 'tests/unit/x79_chipset_test.c'),
    str(ROOT / 'c/drivers/platform/ddr3_spd.c'),
    str(ROOT / 'c/drivers/platform/intel_c600_smbus.c'),
]


def build_run(name, mode, define=None, must_fail=None):
    exe = OUT / name
    cmd = COMMON[:]
    if define:
        cmd.append('-D' + define)
    subprocess.run(cmd + SOURCES + ['-o', str(exe)], check=True)
    run = subprocess.run([str(exe), mode], capture_output=True, text=True,
                         timeout=15)
    output = run.stdout + run.stderr
    (OUT / (name + '.log')).write_text(output)
    print(output, end='')
    if 'runtime error:' in output or 'AddressSanitizer' in output:
        raise SystemExit(name + ': sanitizer failure')
    if must_fail:
        if run.returncode != 1 or output.count('FAIL:') != 1 or must_fail not in output:
            raise SystemExit(f'{name}: control did not fail exactly as expected')
        print('negative control OK:', name)
    elif run.returncode != 0:
        raise SystemExit(f'{name}: positive fixture failed')


def inventory_check():
    run = subprocess.run([
        'python3', str(ROOT / 'tools/driver_inventory.py'), '--profile', 'x79',
        '--json', str(ROOT / 'tests/fixtures/x79-chipset-serial.log')
    ], capture_output=True, text=True, check=True, timeout=15)
    data = json.loads(run.stdout)
    dev = data['devices'][0]
    checks = [
        (data['boot_ok'], 'boot completion survives inventory parsing'),
        (dev['vendor'].lower() == '8086' and dev['device'].lower() == '1d22',
         'C600 identity survives inventory parsing'),
        (dev['category'] == 'SMBus' and dev['driver'] == 'c600-smbus',
         'SMBus category and binding are reported'),
        (any(x.startswith('[smbus]') for x in data['backend_markers']),
         'controller marker is retained'),
        (any('module_capacity_mib=16384' in x for x in data['backend_markers']),
         'module-capacity marker is retained'),
        (data['target_profile']['identified_by_log'] is False,
         'requested X79 profile is never promoted to detected hardware'),
    ]
    failed = [message for ok, message in checks if not ok]
    for message in failed:
        print('FAIL:', message)
    print(f'X79 inventory: {len(checks)} checks, {len(failed)} failed')
    if failed:
        raise SystemExit('driver inventory fixture failed')


if A.negative_only:
    build_run('neg-write-direction', 'readonly', 'X79_SPD_READ_BIT=0',
              'FAIL: SPD transactions remain read-only')
    build_run('neg-crc-coverage', 'crc', 'X79_SPD_NEG_FIXED_CRC126',
              'FAIL: byte0 bit7 selects 117-byte CRC coverage')
    build_run('neg-timeout-recovery', 'kill', 'X79_SMBUS_NEG_NO_KILL',
              'FAIL: owned timeout is killed, cleared and bounded')
    build_run('neg-command-readback', 'command', 'X79_SMBUS_NEG_SKIP_CMD_VERIFY',
              'FAIL: ignored I/O-decode write is refused before BAR access')
    build_run('neg-kill-readback', 'kill-ignored',
              'X79_SMBUS_NEG_SKIP_KILL_VERIFY',
              'FAIL: ignored KILL permanently quarantines without clearing or releasing ownership')
    build_run('neg-command-restore-readback', 'command-restore',
              'X79_SMBUS_NEG_SKIP_CMD_RESTORE_VERIFY',
              'FAIL: unconfirmed PCI Command restore quarantines before all BAR I/O')
else:
    build_run('positive', 'full')
    build_run('positive-kill-ignored', 'kill-ignored')
    build_run('positive-command-restore', 'command-restore')
    inventory_check()
