#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a fresh, ordered SDK archive before replacing the old artifact.

Updating an existing archive retains historical member order. That changed
ELF layout and authenticated AEX image hashes between clean and ordinary
builds even when every object was byte-identical. A fresh temporary archive
also prevents removed members from surviving source-list changes.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ar, output, *objects = sys.argv[1:]
target = Path(output)
target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix=target.name+'.', dir=target.parent) as directory:
    pending = Path(directory)/target.name
    subprocess.run([ar, 'rcs', str(pending), *objects], check=True)
    os.replace(pending, target)
