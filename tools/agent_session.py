#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run an existing QEMU disk with the host-only DeepSeek credential gateway.

Unlike a system repack, this preserves EVERY guest path and its metadata,
including user documents outside predefined /home and /state roots. Only the
broker and its two configuration files are replaced, under the existing disk
guard. The provider key never crosses into the guest or command line.
"""
import argparse
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time

import mkfs
from disk_guard import image_guard
from disk_profile import CheckedImage, atomic_write

ROOT = Path(__file__).resolve().parents[1]


def install(disk, helper, replacements):
    """Caller holds image_guard throughout install and the subsequent VM."""
    with tempfile.TemporaryDirectory(prefix='.agent-update-', dir=disk.parent) as tmp:
        checked = Path(tmp)/'checked.img'
        subprocess.run([str(helper), str(disk), str(checked)], check=True)
        source = CheckedImage(checked, allow_identity=True)
        builder = mkfs.Builder()
        root = source.sb[10]
        for name, child in source.directory(root).items():
            source.copy_tree(builder, child, '/'+name)
        builder.metadata[builder.root] = source.inode(root)[2][mkfs.OFF_ATIME:mkfs.OFF_GID+4]
        for path, (data, mode) in replacements.items():
            parts = path.strip('/').split('/')
            parent = builder.get_or_make_dir(parts[:-1])
            ino = builder.lookup(parent, parts[-1])
            if ino is None:
                builder.add_file(path, data)
                ino = builder.lookup(parent, parts[-1])
            elif builder.itype[ino] != mkfs.T_FILE:
                raise ValueError('agent destination is not a regular file: '+path)
            else:
                builder.content[ino] = data
            now = int(time.time())
            builder.metadata[ino] = struct.pack('<qqqIII', now, now, now, mkfs.MODE_SET | mode, 0, 0)
        data, _ = builder.serialize()
        if source.sb[1] == 5:
            from project_volume import preserve_identities
            data = preserve_identities(source, data, replacements)
        candidate = Path(tmp)/'candidate.img'; candidate.write_bytes(data)
        verified = Path(tmp)/'verified.img'
        subprocess.run([str(helper), str(candidate), str(verified)], check=True)
        atomic_write(disk, verified.read_bytes())


def stop(process):
    if process is None or process.poll() is not None: return
    process.terminate()
    try: process.wait(timeout=10)
    except subprocess.TimeoutExpired: process.kill(); process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--broker', type=Path, required=True)
    parser.add_argument('--finder', type=Path, help='matching native Project workspace')
    parser.add_argument('--agentctl', type=Path, help='matching task inspection commands')
    parser.add_argument('--assistant', type=Path, help='matching task center for reviewed candidates')
    parser.add_argument('--textedit', type=Path, help='install the native document workspace with its matching broker')
    parser.add_argument('--snapshot-helper', type=Path, required=True)
    parser.add_argument('--env', type=Path, required=True)
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--limit', type=int, default=32)
    parser.add_argument('qemu', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.qemu[1:] if args.qemu[:1] == ['--'] else args.qemu
    if not command: parser.error('QEMU command is required after --')
    if args.limit < 1: parser.error('limit must be positive')
    # Do not resolve disk symlinks before the shared guard can reject them.
    disk = args.disk.absolute()
    if not disk.is_file(): raise RuntimeError('build the initial disk before starting an agent session')
    broker = args.broker.resolve().read_bytes()
    if broker[:4] != b'AEX1': raise RuntimeError('broker is not an AEX image')
    agentctl = args.agentctl.resolve().read_bytes() if args.agentctl else None
    if agentctl is not None and agentctl[:4] != b'AEX1': raise RuntimeError('agentctl is not an AEX image')
    assistant = args.assistant.resolve().read_bytes() if args.assistant else None
    if assistant is not None and assistant[:4] != b'AEX1': raise RuntimeError('assistant is not an AEX image')
    editor = args.textedit.resolve().read_bytes() if args.textedit else None
    if editor is not None and editor[:4] != b'AEX1': raise RuntimeError('TextEdit is not an AEX image')
    finder = args.finder.resolve().read_bytes() if args.finder else None
    if finder is not None and finder[:4] != b'AEX1': raise RuntimeError('Finder is not an AEX image')
    args.state_dir.mkdir(parents=True, exist_ok=True)
    gateway = guest = None; log = None

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        with image_guard(disk) as lock:
            state = Path(tempfile.mkdtemp(prefix='session-', dir=args.state_dir.resolve()))
            log = (state/'gateway.log').open('w')
            gateway = subprocess.Popen([sys.executable, str(ROOT/'tools/agent_gateway.py'),
                '--env', str(args.env.resolve()), '--state', str(state), '--limit', str(args.limit)],
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            for _ in range(100):
                if (state/'ready.json').exists(): break
                if gateway.poll() is not None: raise RuntimeError('model gateway did not start; see '+str(state/'gateway.log'))
                time.sleep(.1)
            else: raise RuntimeError('model gateway readiness deadline')
            replacements = {
                '/bin/agentd': (broker, 0o755),
                '/etc/agent.conf': ((state/'agent.conf').read_bytes(), 0o600),
                '/etc/agent.key': ((state/'agent.key').read_bytes(), 0o600)}
            if finder is not None: replacements['/files.aex'] = (finder, 0o755)
            if editor is not None: replacements['/textedit.aex'] = (editor, 0o755)
            if assistant is not None: replacements['/assistant.aex'] = (assistant, 0o755)
            if agentctl is not None: replacements['/bin/agentctl'] = (agentctl, 0o755)
            install(disk, args.snapshot_helper.resolve(), replacements)
            print('AGENT_SESSION_READY model=deepseek-flash limit='+str(args.limit)+' state='+str(state), flush=True)
            guest = subprocess.Popen(command, pass_fds=(lock,))
            try:
                while guest.poll() is None:
                    if gateway.poll() is not None:
                        # Preserve the live desktop and unsaved text. The task
                        # service handles lost inference as a recoverable wait.
                        print('Model gateway stopped; pending tasks can wait. Session: '+str(state), flush=True)
                        return guest.wait()
                    time.sleep(.5)
                return guest.returncode
            finally:
                # Close the guest before dropping its inherited disk lock.
                stop(guest)
    finally:
        stop(gateway)
        if log: log.close()


if __name__ == '__main__':
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit('agent session: '+str(exc))
