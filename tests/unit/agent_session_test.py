#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the real session installer with native fsck and private test disks."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
import mkfs
from disk_guard import image_guard
from disk_profile import CheckedImage


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--negative', action='store_true')
    args = parser.parse_args()
    mkfs.TOTAL_BLOCKS = 2048; mkfs.INODE_COUNT = 128; mkfs.LOG_BLOCKS = 16
    original = (ROOT/'tools/agent_session.py').read_text()
    changes = {
        'lost-document': ('source.copy_tree(builder, child, \'/\'+name)',
                          "None if name == 'untitled.txt' else source.copy_tree(builder, child, '/'+name)",
                          'all existing bytes and metadata retained'),
        'readable-credential': ('mkfs.MODE_SET | mode, 0, 0)',
                                'mkfs.MODE_SET | 0o644, 0, 0)',
                                'installed credential is private'),
    }
    variants = list(changes) if args.negative else ['positive']
    for variant in variants:
        text = original
        if variant in changes:
            old, new, expected = changes[variant]
            if text.count(old) != 1: raise AssertionError('installer mutation anchor changed')
            text = text.replace(old, new)
        namespace = {'__name__': 'session_test_subject', '__file__': str(ROOT/'tools/agent_session.py')}
        exec(compile(text, str(ROOT/'tools/agent_session.py'), 'exec'), namespace)
        with tempfile.TemporaryDirectory(prefix='agent-session-test-') as tmp:
            disk = Path(tmp)/'disk.img'
            builder = mkfs.Builder()
            values = {'/untitled.txt': b'unsaved-before-reboot-now-saved',
                      '/docs/source.md': b'original sources', '/state/agents/task': b'checkpoint',
                      '/browser.aex': b'untouched browser image', '/browser/profile': b'opaque state',
                      '/bin/agentd': b'old-broker', '/etc/agent.conf': b'old-config',
                      '/textedit.aex': b'old-editor', '/assistant.aex': b'old-tasks'}
            for path, data in values.items(): builder.add_file(path, data)
            builder.get_or_make_dir(['empty'])
            metadata = struct.pack('<qqqIII', 10, 20, 30, mkfs.MODE_SET | 0o700, 123, 456)
            for ino in range(builder.next_ino): builder.metadata[ino] = metadata
            disk.write_bytes(builder.serialize()[0])

            def snapshot():
                image = CheckedImage(disk); result = {}
                def visit(ino, path):
                    kind, _, raw = image.inode(ino)
                    result[path] = (kind, image.payload(ino) if kind == mkfs.T_FILE else None,
                                    raw[mkfs.OFF_ATIME:mkfs.OFF_GID+4])
                    if kind == mkfs.T_DIR:
                        for name, child in image.directory(ino).items(): visit(child, path.rstrip('/')+'/'+name)
                visit(image.sb[10], '/'); return result

            before = snapshot()
            replacements = {'/bin/agentd': (b'new-broker', 0o755),
                            '/textedit.aex': (b'new-editor', 0o755), '/assistant.aex': (b'new-tasks', 0o755),
                            '/etc/agent.conf': (b'session-config', 0o600),
                            '/etc/agent.key': (b'ephemeral-test-capability', 0o600)}
            try:
                with image_guard(disk): namespace['install'](disk, args.helper.resolve(), replacements)
                after = snapshot()
                assert all(after.get(p) == value for p, value in before.items() if p not in replacements), 'all existing bytes and metadata retained'
                assert after['/etc/agent.key'][2][24:28] == struct.pack('<I', mkfs.MODE_SET | 0o600), 'installed credential is private'
                assert all(after[p][1] == data for p, (data, _) in replacements.items()), 'only requested replacements installed'
                stable = disk.read_bytes()
                with disk.open('rb'):
                    try:
                        with image_guard(disk): raise AssertionError('open image accepted')
                    except RuntimeError as error: assert 'open by PID' in str(error)
                assert disk.read_bytes() == stable, 'active guest image unchanged'
                with patch('os.fsync', side_effect=OSError('fixture fsync failure')):
                    try:
                        with image_guard(disk): namespace['install'](disk, args.helper.resolve(), replacements)
                    except OSError: pass
                    else: raise AssertionError('fsync failure ignored')
                assert disk.read_bytes() == stable, 'failed install retains original image'
                try:
                    with image_guard(disk): namespace['install'](disk, args.helper.resolve(), {'/empty': (b'file', 0o600)})
                except ValueError: pass
                else: raise AssertionError('directory replaced by file')
                assert disk.read_bytes() == stable, 'type mismatch retains original image'
            except AssertionError as error:
                if variant not in changes or str(error) != expected: raise
                print('CONTROL DETECTED:', variant, expected, flush=True)
            else:
                if args.negative: raise AssertionError('installer control was not detected')
                print('AGENT_SESSION_INSTALL_PASS preservation permissions active-disk fsync type-conflict', flush=True)


if __name__ == '__main__': main()
