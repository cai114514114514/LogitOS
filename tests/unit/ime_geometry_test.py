#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the same geometry reader used by the live candidate-click gate."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'boot'))
from ime_geometry import textedit_frames

launch = b'[wm] launched TextEdit\n'
frame = b'[wm] win 1 frame 138 98 520 390 content 520 360 pt zoom 0 min 0 '
for title in (b'untitled.txt', b'/untitled.txt'):
    assert textedit_frames(launch + frame + title + b'\r\n') == [(b'138', b'98')]
for title in (b'Clock', b'/home/untitled.txt', b'/untitled.txt.bak'):
    assert textedit_frames(launch + frame + title + b'\n') == []
assert textedit_frames(frame + b'/untitled.txt\n') == []
assert textedit_frames(frame + b'/untitled.txt\n' + launch) == []
assert textedit_frames(launch + frame + b'/untitled.txt\n' + launch) == []
print('IME_GEOMETRY_PASS checks=8')
