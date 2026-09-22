# SPDX-License-Identifier: MIT
"""Compile the real draft model and prove the stale-snapshot control fails."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
negative = '--negative' in sys.argv
with tempfile.TemporaryDirectory(prefix='settings-draft-') as temp:
    temp = pathlib.Path(temp)
    header = (root / 'c/apps/gui/settings/model.h').read_text()
    if negative:
        header = header.replace('(edited & (1u << i))', '(1)')
    (temp / 'model.h').write_text(header)
    source = (root / 'tests/unit/settings_model_test.c').read_text().replace('../../c/apps/gui/settings/model.h', 'model.h')
    (temp / 'test.c').write_text(source)
    subprocess.run(['clang', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-I' + str(root / 'include/abi'), str(temp / 'test.c'), '-o', str(temp / 'test')], check=True)
    run = subprocess.run([str(temp / 'test')], capture_output=True, text=True)
    print(run.stdout, end='')
    if negative:
        if run.returncode != 1 or 'FAIL writes == 1' not in run.stdout:
            sys.exit('FAIL negative control did not detect stale writes')
        print('PASS negative control: stale snapshot writes were rejected')
    elif run.returncode:
        sys.exit('FAIL settings draft')
