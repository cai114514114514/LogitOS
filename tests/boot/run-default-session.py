#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the real make run recipe with private QEMU transports and a disk.

Only hardware/transport arguments and artifact paths are overridden. make
still selects the ordinary session launcher, installs the live gateway's
configuration and starts the guest. The fixture disk is already built, so
-o prevents an unrelated full-system repack from replacing the fixture.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    if '--launch-make' in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument('--launch-make', action='store_true')
        parser.add_argument('--build', type=Path, required=True)
        parser.add_argument('--disk', type=Path, required=True)
        parser.add_argument('--env', type=Path, required=True)
        parser.add_argument('qemu', nargs=argparse.REMAINDER)
        args = parser.parse_args(); qemu = args.qemu[1:]
        if not qemu: parser.error('QEMU command required')
        command = ['make', '--no-print-directory', 'run', 'BUILD='+str(args.build),
                   'DISK='+str(args.disk), 'AGENT_ENV='+str(args.env), 'AGENT_SESSION_LIMIT=8',
                   'QEMU='+shlex.quote(qemu[0]), 'QEMU_RUN_ARGS='+shlex.join(qemu[1:]),
                   '-o', str(args.disk)]
        os.chdir(ROOT); os.execvp(command[0], command)
    # Reuse the existing end-to-end assertions: a real request, source facts,
    # citations, exact saved bytes and preservation of an arbitrary user path.
    spec = importlib.util.spec_from_file_location('default_session_gate', ROOT/'tests/boot/run-agent-session.py')
    gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
    gate.main(default_run=True)


if __name__ == '__main__': main()
