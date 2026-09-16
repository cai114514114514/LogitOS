"""The consumer must preserve accounting while doing fewer locked operations."""
import re
import sys
from pathlib import Path
before, after = (Path(p).read_text() for p in sys.argv[1:])
def account(text):
    return re.search(r'^QJS_ACCOUNT count=\d+ size=\d+ result=\d+$', text, re.M)[0]
def locks(text):
    return int(re.search(r'^QJS_LOCKS (\d+)$', text, re.M)[1])
ok = account(before) == account(after) and locks(after) < locks(before) * .85
print(('ok: ' if ok else 'FAIL: ') + f'QuickJS allocator transactions: {locks(before)} -> {locks(after)}')
print('accounting equal:', account(before) == account(after))
sys.exit(0 if ok else 1)
