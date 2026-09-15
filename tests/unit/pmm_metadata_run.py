import argparse, os, subprocess
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--negative-only', action='store_true')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
variants = [('PMM_META_SINGLE_RANGE', 'metadata spans adjacent available descriptors'),
            ('PMM_META_FIXED_AFTER_KERNEL', 'metadata relocates beyond firmware hole')] if a.negative_only else [('', '')]
for macro, marker in variants:
    binary = a.build / (macro.lower() or 'positive')
    cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DMM_HOSTTEST',
           '-Ic/kernel/mm', '-Itests/unit/mmstub', 'tests/unit/pmm_metadata_test.c',
           'c/kernel/mm/pmm.c', '-o', str(binary)]
    if macro: cmd.insert(1, '-D' + macro)
    subprocess.run(cmd, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    binary.with_suffix('.log').write_text(result.stdout + result.stderr)
    if macro:
        assert result.returncode == 1 and 'FAIL: ' + marker in result.stdout, result.stdout + result.stderr
        assert 'runtime error:' not in result.stderr and 'AddressSanitizer' not in result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    print((macro or 'positive') + ': ' + result.stdout.strip().splitlines()[-1])
