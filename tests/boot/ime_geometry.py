# SPDX-License-Identifier: MIT
"""Read only TextEdit's observed post-launch untitled document geometry."""
import re


def textedit_frames(log):
    # Old title: untitled.txt. Root-path document identity now reports
    # /untitled.txt. Accept those exact names, never a different app's frame or
    # a similarly named document, because this coordinate chooses candidate 9.
    if b'[wm] launched TextEdit' not in log:
        return []
    launched = log.rsplit(b'[wm] launched TextEdit', 1)[-1]
    return re.findall(
        rb'\[wm\] win \d+ frame (-?\d+) (-?\d+) \d+ \d+ content \d+ \d+ '
        rb'pt zoom \d+ min \d+ /?untitled\.txt(?=\r?\n|$)', launched)
