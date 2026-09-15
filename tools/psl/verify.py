#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the compiled product matcher against raw PSL rules and upstream cases."""
import ast
import ctypes
import pathlib
import re
import sys
import unicodedata

ROOT = pathlib.Path(__file__).resolve().parents[2]
lib = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
lib.cookie_domain_is_public_suffix.argtypes = [ctypes.c_char_p]
lib.cookie_same_site.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
lib.cookie_psl_probe_domain.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]


def alabel(host):
    if host is None:
        return None
    return '.'.join(label if label.isascii() else 'xn--' + label.encode('punycode').decode()
                    for label in unicodedata.normalize('NFC', host.lower()).split('.'))


def b(host):
    return None if host is None else host.encode('ascii')


exact, wild, exceptions = set(), set(), set()
for line in (ROOT / 'tools/psl/public_suffix_list.dat').read_text().splitlines():
    line = line.strip()
    if not line or line.startswith('//'):
        continue
    if line.startswith('!'):
        exceptions.add(alabel(line[1:]))
    elif line.startswith('*.'):
        wild.add(alabel(line[2:]))
    else:
        exact.add(alabel(line))


def reference(host):
    """Independent set-based prevailing-rule calculation, no generated table."""
    labels = host.split('.')
    candidates = ['.'.join(labels[i:]) for i in range(len(labels))]
    exception = next((s for s in candidates if s in exceptions), None)
    if exception:
        n = len(exception.split('.')) - 1
    else:
        n = max([1] + [len(s.split('.')) for s in candidates if s in exact] +
                [len(s.split('.')) + 1 for s in candidates[1:] if s in wild])
    return host if len(labels) <= n else '.'.join(labels[-n-1:]), len(labels) <= n


checks = 0
for rule in sorted(exact | wild | exceptions):
    for candidate in [rule, 'unit.' + rule, 'a.unit.' + rule]:
        if len(candidate) >= 256:
            continue  # This browser's documented host storage limit.
        _, public = reference(candidate)
        assert bool(lib.cookie_domain_is_public_suffix(b(candidate))) == public, candidate
        checks += 1
        sibling = 'b.' + candidate
        if len(sibling) < 256:
            expected = reference(candidate)[0] == reference(sibling)[0]
            assert bool(lib.cookie_same_site(b(candidate), b(sibling))) == expected, (candidate, sibling)
            checks += 1

upstream = 0
for line in (ROOT / 'tools/psl/test_psl.txt').read_text().splitlines():
    match = re.match(r'checkPublicSuffix\((.*)\);', line)
    if not match:
        continue
    host, expected = ast.literal_eval('(' + match.group(1).replace('null', 'None') + ')')
    host, expected = alabel(host), alabel(expected)
    if expected is None:
        assert lib.cookie_domain_is_public_suffix(b(host)), repr(host)
    else:
        assert not lib.cookie_domain_is_public_suffix(b(expected)), expected
        assert lib.cookie_same_site(b(host), b(expected)), (host, expected)
        output = ctypes.create_string_buffer(256)
        assert lib.cookie_psl_probe_domain(b(host), output, len(output)) == 0, host
        actual = output.value.decode('ascii')
        assert actual == expected, (host, actual, expected)
    upstream += 1
print(f'cookie PSL: {len(exact)+len(wild)+len(exceptions)} rules; {checks} full-list checks; {upstream} upstream cases PASS')
