#!/usr/bin/env python3
"""Compare the EC public point openssl derived from a private scalar against
the one our own ecdh_keygen produced from the same scalar.

Called by tests/unit/run-ecdsa-sign.sh. It exists as a file rather than as a
heredoc because it is the check that keeps the openssl differential from being
circular, and a check buried in shell quoting is a check nobody reads.

The point is taken out of the DER SubjectPublicKeyInfo -- a BIT STRING whose
contents are the uncompressed point with a leading unused-bits octet -- rather
than scraped out of `openssl ec -text`, whose layout is a display format and
not an interface. Usage: ecdsa_pubcmp.py <builddir> <curve-id>
"""
import os
import sys


def bitstring_contents(der):
    """The contents of the last top-level BIT STRING in a SubjectPublicKeyInfo.

    SPKI is SEQUENCE { AlgorithmIdentifier, BIT STRING }, so walking the outer
    SEQUENCE's children and taking the BIT STRING is exact -- unlike searching
    for a 0x03 byte, which can match inside the algorithm OID or the point.
    """
    assert der[0] == 0x30, 'not a SEQUENCE'
    i, ln, hdr = 1, der[1], 2
    if ln & 0x80:
        k = ln & 0x7f
        ln = int.from_bytes(der[2:2 + k], 'big')
        hdr = 2 + k
    i = hdr
    end = hdr + ln
    while i < end:
        tag = der[i]
        j = i + 1
        n = der[j]
        if n & 0x80:
            k = n & 0x7f
            j += 1
            n = int.from_bytes(der[j:j + k], 'big')
            j += k
        else:
            j += 1
        if tag == 0x03:
            return der[j + 1:j + n]          # skip the unused-bits octet
        i = j + n
    raise AssertionError('no BIT STRING in the SPKI')


def main():
    build, cid = sys.argv[1], sys.argv[2]
    spki = open(os.path.join(build, 'c%s.pub.der' % cid), 'rb').read()
    theirs = bitstring_contents(spki)
    ours = open(os.path.join(build, 'c%s.pub' % cid), 'rb').read()
    if theirs != ours:
        print('  openssl point %s' % theirs.hex())
        print('  our point     %s' % ours.hex())
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
