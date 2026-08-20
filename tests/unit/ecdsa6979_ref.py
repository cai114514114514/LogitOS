#!/usr/bin/env python3
"""An INDEPENDENT reference for RFC 6979 deterministic ECDSA over P-256/384/521.

Why this exists rather than a table of hex constants copied into the C test:
this tree's rule is that every number is measured or derived, never remembered,
and a mistyped digit in a 132-hex-digit P-521 vector is indistinguishable from
a bug in the signer -- it fails in exactly the same place with exactly the same
shape. Nothing here is transcribed except the curve parameters, which are
checkable in one line (see the assertions at the bottom).

It is independent of c/crypto/pubkey/ecdsa.c in the way that matters: Python's
arbitrary-precision integers and a plain affine double-and-add, against the C's
13-limb Barrett reduction and Jacobian coordinates. The two share no
arithmetic, no representation and no code, so agreeing on r and s for a
deterministic k is evidence, not a tautology.

It is NOT independent of the RFC: both this and the C are implementations of
the same document, so a misreading of the document would be reproduced here.
That gap is what the openssl half of the gate covers -- openssl never sees this
file and verifies the C's signature against the public key alone.

Usage:  ecdsa6979_ref.py            -> one line per case, space separated:
                                       curve hlen priv_hex msg_hex r_hex s_hex
        Every field is hex (the message included) so the consumer needs no
        quoting rules and a message containing a space cannot split a field.
"""
import hashlib
import hmac
import sys

# ---------------------------------------------------------------- curves ----
# p, a, b, Gx, Gy, n. FIPS 186-4 / SEC 2. Every one of these is asserted
# against the curve equation at the bottom of this file, so a mistyped digit
# stops the generator instead of producing a wrong expectation.
CURVES = {
    256: dict(
        p=0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff,
        a=0xffffffff00000001000000000000000000000000fffffffffffffffffffffffc,
        b=0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b,
        gx=0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
        gy=0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5,
        n=0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551,
        flen=32, h='sha256'),
    384: dict(
        p=0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff,
        a=0xfffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc,
        b=0xb3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef,
        gx=0xaa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7,
        gy=0x3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f,
        n=0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973,
        flen=48, h='sha384'),
    521: dict(
        p=0x01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff,
        a=0x01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffc,
        b=0x0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00,
        gx=0x00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66,
        gy=0x011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650,
        n=0x01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409,
        flen=66, h='sha512'),
}


def pt_add(P, Q, p, a):
    """Affine addition on y^2 = x^3 + ax + b over F_p. None is infinity."""
    if P is None:
        return Q
    if Q is None:
        return P
    if P[0] == Q[0] and (P[1] + Q[1]) % p == 0:
        return None
    if P == Q:
        lam = (3 * P[0] * P[0] + a) * pow(2 * P[1], -1, p) % p
    else:
        lam = (Q[1] - P[1]) * pow(Q[0] - P[0], -1, p) % p
    x = (lam * lam - P[0] - Q[0]) % p
    return (x, (lam * (P[0] - x) - P[1]) % p)


def pt_mul(k, P, p, a):
    R = None
    while k:
        if k & 1:
            R = pt_add(R, P, p, a)
        P = pt_add(P, P, p, a)
        k >>= 1
    return R


def bits2int(b, qlen):
    v = int.from_bytes(b, 'big')
    excess = len(b) * 8 - qlen
    return v >> excess if excess > 0 else v


def rfc6979_k(x, h1, n, qlen, hname, rlen):
    """RFC 6979 3.2, written straight from the section headings."""
    hlen = hashlib.new(hname).digest_size
    x_oct = x.to_bytes(rlen, 'big')
    h1_oct = (bits2int(h1, qlen) % n).to_bytes(rlen, 'big')   # bits2octets
    V = b'\x01' * hlen
    K = b'\x00' * hlen
    K = hmac.new(K, V + b'\x00' + x_oct + h1_oct, hname).digest()
    V = hmac.new(K, V, hname).digest()
    K = hmac.new(K, V + b'\x01' + x_oct + h1_oct, hname).digest()
    V = hmac.new(K, V, hname).digest()
    while True:
        T = b''
        while len(T) < rlen:
            V = hmac.new(K, V, hname).digest()
            T += V
        yield bits2int(T[:rlen], qlen)
        K = hmac.new(K, V + b'\x00', hname).digest()
        V = hmac.new(K, V, hname).digest()


def sign(curve, priv, msg):
    C = CURVES[curve]
    p, a, n, rlen = C['p'], C['a'], C['n'], C['flen']
    qlen = n.bit_length()
    h1 = hashlib.new(C['h'], msg).digest()
    e = bits2int(h1, qlen) % n
    for k in rfc6979_k(priv, h1, n, qlen, C['h'], rlen):
        if not (1 <= k < n):
            continue
        R = pt_mul(k, (C['gx'], C['gy']), p, a)
        r = R[0] % n
        if r == 0:
            continue
        s = pow(k, -1, n) * (e + r * priv) % n
        if s == 0:
            continue
        return r, s


def main():
    # Self-check the transcribed parameters before emitting anything: G must
    # lie on the curve and n*G must be the point at infinity. A digit typed
    # wrong fails one of these, here, instead of becoming a wrong expectation
    # that reads like a signer bug.
    for cid, C in CURVES.items():
        p, a, b, gx, gy, n = C['p'], C['a'], C['b'], C['gx'], C['gy'], C['n']
        assert (gy * gy - (gx * gx * gx + a * gx + b)) % p == 0, f'G not on P-{cid}'
        assert pt_mul(n, (gx, gy), p, a) is None, f'n*G != O on P-{cid}'

    # Two messages per curve, and a private key derived from the curve id so
    # nothing here is a constant somebody has to trust.
    for cid, C in CURVES.items():
        rlen = C['flen']
        priv = int.from_bytes(hashlib.sha512(b'logitos-ecdsa-key-%d' % cid).digest()
                              * 2, 'big') % (C['n'] - 1) + 1
        for msg in (b'sample', b'test', b'logitos tls server certificate verify'):
            r, s = sign(cid, priv, msg)
            print(cid, hashlib.new(C['h']).digest_size,
                  priv.to_bytes(rlen, 'big').hex(),
                  msg.hex(),
                  r.to_bytes(rlen, 'big').hex(),
                  s.to_bytes(rlen, 'big').hex())


if __name__ == '__main__':
    sys.exit(main())
