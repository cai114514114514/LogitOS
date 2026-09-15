import pathlib, re, subprocess, sys
p = subprocess.run([sys.argv[1]], capture_output=True, text=True)
pathlib.Path(sys.argv[2]).write_text(p.stdout + p.stderr)
print(p.stdout, end='')
m = re.search(r'button-inner-flex: (\d+) checks, (\d+) failures', p.stdout)
assert m and int(m[1]) > 300
if sys.argv[3] == 'legacy':
    assert p.returncode == 1 and int(m[2]) > 10
    for label in ['inner flex intrinsic button width', 'row authored item gap', 'inner flex natural button height']:
        assert 'FAIL: ' + label in p.stdout
else:
    assert p.returncode == 0 and int(m[2]) == 0
