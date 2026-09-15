#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the real private-disk recipe reader, including quoted file paths."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'boot'))
from make_disk_recipe import disk_recipe

plain = 'python3 tools/mkfs.py build/disk.img fsroot/etc build/sh.aex:/bin/sh'
expected = ('build/disk.img', ['fsroot/etc', 'build/sh.aex:/bin/sh'])
assert disk_recipe(plain) == expected
options = ('python3 tools/mkfs.py --preserve /state --preserve-merge /etc '
           '--snapshot-helper build/lfs_snapshot \\\n  build/disk.img fsroot/etc build/sh.aex:/bin/sh')
assert disk_recipe(options) == expected
# Former argv[3:] behaviour must disagree: it packaged option values as files.
assert options.replace('\\\n', ' ').split()[3:] != expected[1]
assert disk_recipe('python3 tools/mkfs.py "a b/disk.img" "a b/file:/file"') == (
    'a b/disk.img', ['a b/file:/file'])
for invalid in ('', plain + '\n' + plain, 'python3 tools/mkfs.py --future value out.img',
                'python3 tools/mkfs.py --preserve', 'python3 tools/mkfs.py --preserve --state',
                'python3 tools/mkfs.py --preserve /state'):
    try:
        disk_recipe(invalid)
    except ValueError:
        continue
    raise AssertionError('invalid recipe accepted: ' + invalid)
print('KERNEL_DISK_RECIPE_PASS checks=10')
