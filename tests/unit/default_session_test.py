#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the expanded default recipe and real launcher offline/error paths.

The negative control restores the old QEMU-only recipe. It must fail the
same assertion that checks the ordinary make target, not a separate model.
No provider request is made by this host test.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
APPS = ('agentd', 'agentctl', 'files', 'assistant', 'textedit')


def check_recipe(makefile, build):
    ignored = [build/'logit.iso', build/'disk.img', build/'lfs_snapshot']
    ignored += [build/(app+'.aex') for app in APPS]
    command = ['make', '--no-print-directory', '-n', '-f', str(makefile),
               'BUILD='+str(build), 'run']
    for path in ignored: command += ['-o', str(path)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=True)
    rows = [shlex.split(row) for row in result.stdout.splitlines() if row.startswith('python3 tools/')]
    assert len(rows) == 1 and rows[0][1] == 'tools/agent_session.py', 'default run starts the model session'
    row = rows[0]
    assert '--optional-env' in row and row[row.index('--env')+1] == '.env'
    assert row[row.index('--disk')+1] == str(build/'disk.img')
    assert row[row.index('--finder')+1] == str(build/'files.aex')
    assert '-qmp' in row and '-netdev' in row and '-audiodev' in row


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='default-session-') as tmp:
        directory = Path(tmp); build = directory/'build'; build.mkdir()
        if args.negative:
            original = (ROOT/'Makefile').read_text()
            anchor = '$(AGENT_SESSION_LAUNCH) --optional-env --disk $(DISK) -- $(QEMU) $(QEMU_RUN_ARGS)'
            assert original.count(anchor) == 1, 'default recipe mutation anchor changed'
            mutated = directory/'Makefile'
            mutated.write_text(original.replace(anchor, 'python3 tools/disk_guard.py $(DISK) -- $(QEMU) $(QEMU_RUN_ARGS)'))
            try: check_recipe(mutated, build)
            except AssertionError as error:
                assert str(error) == 'default run starts the model session'
                print('CONTROL DETECTED: old default recipe omits the model session')
            else: raise AssertionError('old default recipe was accepted')
            return
        check_recipe(ROOT/'Makefile', build)
        disk = directory/'disk.img'; disk.write_bytes(b'unchanged offline disk')
        broker = directory/'broker.aex'; broker.write_bytes(b'AEX1')
        env = directory/'model.env'; state = directory/'sessions'
        command = [sys.executable, str(ROOT/'tools/agent_session.py'), '--optional-env',
                   '--disk', str(disk), '--broker', str(broker), '--snapshot-helper', '/unused',
                   '--env', str(env), '--state-dir', str(state), '--limit', '1', '--',
                   sys.executable, '-c', "print('GUEST_STARTED');raise SystemExit(17)"]
        offline = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        assert offline.returncode == 17 and 'MODEL_SESSION_OFFLINE' in offline.stdout and 'GUEST_STARTED' in offline.stdout
        assert not state.exists() and disk.read_bytes() == b'unchanged offline disk'
        env.write_text('# deliberately missing the provider credential\n')
        invalid = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        assert invalid.returncode != 0 and 'GUEST_STARTED' not in invalid.stdout
        assert 'model gateway did not start' in invalid.stderr
        assert disk.read_bytes() == b'unchanged offline disk'
        print('DEFAULT_SESSION_PASS default recipe, offline boot, invalid configuration, preserved disk, exit status')


if __name__ == '__main__': main()
