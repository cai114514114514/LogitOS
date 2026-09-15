#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the production mkfs invocation for private, fresh test disk builders."""
import re
import shlex


def disk_recipe(text):
    text = re.sub(r'\\\r?\n[ \t]*', ' ', text)
    commands = [shlex.split(line) for line in text.splitlines() if 'tools/mkfs.py' in line]
    if len(commands) != 1:
        raise ValueError(f'expected one mkfs.py command, got {len(commands)}')
    command = commands[0]
    pos = command.index('tools/mkfs.py') + 1
    # Preservation applies to an existing product disk, not to a fresh test
    # image. These leading options each consume one value. Fail on a new option
    # rather than accidentally treating it or its value as a packaged file.
    while pos < len(command) and command[pos].startswith('--'):
        option = command[pos]
        if option not in ('--preserve', '--preserve-merge', '--snapshot-helper'):
            raise ValueError('unsupported mkfs recipe option: ' + option)
        if pos + 1 >= len(command) or command[pos + 1].startswith('--'):
            raise ValueError('missing value for mkfs recipe option: ' + option)
        pos += 2
    if pos >= len(command):
        raise ValueError('mkfs recipe has no output image')
    return command[pos], command[pos + 1:]
