#!/usr/bin/env python3
"""Adversarial X25519 vectors for the SSH kex: the low-order-point attack,
complete, plus the non-canonical-encoding variants of it.

The attack being pinned: a hostile client puts a small-order point (or a
non-canonical 32-byte encoding that decodes to one) into KEX_ECDH_INIT. If
the server's ladder does not reduce that to the all-zero shared secret --
or if the all-zero check does not fire -- the attacker has chosen the
session key and the "encrypted" channel is transparent to them. A sloppy
ladder can also disagree on NON-canonical encodings of ordinary points,
which is a spec violation worth catching even when it is not exploitable.

Oracles, in the house style of ssh_kex_gen.py (independence is the point):

  1. A pure-Python RFC 7748 X25519 ladder over big ints. Independent of BOTH
     c/crypto/pubkey/x25519.c and of OpenSSL: where python-`cryptography`
     refuses to speak (it errors on all-zero outputs and is entitled to its
     own opinions about non-canonical inputs), this ladder still can, because
     it implements exactly what RFC 7748 5 says: mask the high bit, do NOT
     reduce mod p, clamp the scalar.
  2. python-`cryptography` itself, cross-checking the ladder on random
     ordinary points, and confirming (by raising) on each low-order vector.

The 12-ish low-order encodings are COMPUTED, not quoted from a table: the
order-8 subgroup is generated in Edwards coordinates (T = [L]R for a random
full-order R), mapped to Montgomery u = (1+y)/(1-y), then extended with the
non-canonical variants (u+k*p that still fit, and the high-bit-set spellings)
that a sloppy ladder mishandles. A misremembered constant table could not
happen here; the generator asserts its subgroup contains u=0 and u=1.

Usage: ssh_attack_kex_gen.py <out-vectors-file>
Output, one record per line:  VEC <name> <qc-hex> <refuse 0|1> <k-hex-or-->
`refuse=1` means the ladder gives the all-zero secret (ssh_kex.c MUST return
-1); `refuse=0` vectors carry the exact expected shared secret, so the test
also pins the masking behavior on accepted non-canonical inputs.
"""
import sys
from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives import serialization as ser

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493  # prime order of the base point
M64 = (1 << 255) - 1                                 # mask off the 256th bit only


def clamp(k: int) -> int:
    k &= M64 & ~7          # clear bits 0..2 and the top bit
    return k | (1 << 254)  # set bit 254


def ladder(k: int, u: int) -> int:
    """RFC 7748 5 exactly: u masked (never reduced mod p), scalar clamped."""
    k = clamp(k)
    u &= M64
    x1, x2, z2, x3, z3 = u, 1, 0, u, 1
    swap = 0
    for t in range(254, -1, -1):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        a = (x2 + z2) % P; aa = a * a % P
        b = (x2 - z2) % P; bb = b * b % P
        e = (aa - bb) % P
        c = (x3 + z3) % P; d = (x3 - z3) % P
        da = d * a % P; cb = c * b % P
        x3 = (da + cb) * (da + cb) % P
        z3 = x1 * (da - cb) * (da - cb) % P
        x2 = aa * bb % P
        z2 = e * (aa + 121665 * e) % P
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    return x2 * pow(z2, P - 2, P) % P


def le(x: int, n: int = 32) -> bytes:
    return x.to_bytes(n, "little")  # wire byte order; every hex field below uses it


# --- Edwards25519 arithmetic, only to COMPUTE the order-8 subgroup ---------
# The birational companion of the Montgomery curve is the a=-1 twisted
# Edwards form -x^2 + y^2 = 1 + d x^2 y^2 (NOT the a=+1 form: writing the
# a=+1 addition here finds points of a DIFFERENT curve, and the assert on
# 8*T == identity below is what turns a slip here into a generator failure
# rather than a silently wrong vector file).
D = (-121665 * pow(121666, P - 2, P)) % P


def ed_add(p1, p2):
    x1, y1 = p1; x2, y2 = p2
    x3 = (x1 * y2 + y1 * x2) * pow(1 + D * x1 * x2 * y1 * y2, P - 2, P) % P
    y3 = (y1 * y2 + x1 * x2) * pow(1 - D * x1 * x2 * y1 * y2, P - 2, P) % P
    return (x3, y3)


def ed_mul(k: int, p1):
    r = (0, 1)
    while k:
        if k & 1:
            r = ed_add(r, p1)
        p1 = ed_add(p1, p1)
        k >>= 1
    return r


def sqrt_mod5(a: int):
    """Square root mod p for p % 8 == 5."""
    if a % P == 0:
        return 0
    r = pow(a, (P + 3) // 8, P)
    if r * r % P != a % P:
        r = r * pow(2, (P - 1) // 4, P) % P
    return r if r * r % P == a % P else None


def random_curve_point(rng):
    while True:
        y = rng() % P
        x2 = (y * y - 1) * pow((1 + D * y * y) % P, P - 2, P) % P
        x = sqrt_mod5(x2)
        if x is not None:
            return (x, y)


def low_order_u_values(rng):
    """All distinct Montgomery u of the order-8 subgroup, computed.

    Retried because the FIRST random R does not always do: the group is not
    cyclic, so [L]R lands on the order-8 subgroup with order 4 or 8 depending
    on R's cofactor component (the very first seed tried here landed on 4,
    yielding only {u=0, u=1} -- half the attack surface -- and the assert
    below on the count is what caught it)."""
    while True:
        r = random_curve_point(rng)
        t8 = ed_mul(L, r)
        if ed_mul(8, t8) == (0, 1) and ed_mul(4, t8) != (0, 1):
            break
    us = set()
    k = t8
    for _ in range(8):
        x, y = k
        if y != 1:                         # identity has no u
            us.add((1 + y) * pow((1 - y) % P, P - 2, P) % P)
        k = ed_add(k, t8)
    assert len(us) == 4, f"wanted the 4 canonical low-order u, got {len(us)}"
    return us


def main():
    out_path = sys.argv[1]

    # Deterministic RNG so the vector file is reproducible byte-for-byte.
    state = [0x243F6A8885A308D3]
    def rng():
        state[0] = (state[0] * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
        return state[0] >> 11

    server_priv = 0x9D61B19DEFFD5A60BA844AF492EC2CC44449C5697B326919703BAC031CAE7F60  # RFC 7748 k_(A)

    # --- cross-check the ladder against python-cryptography on ordinary points
    for i in range(16):
        kp = rng() & M64
        up = rng() & M64
        mine = ladder(kp, up)
        try:
            theirs = x25519.X25519PrivateKey.from_private_bytes(le(kp)).exchange(
                x25519.X25519PublicKey.from_public_bytes(le(up)))
            assert mine == int.from_bytes(theirs, "little"), f"ladder disagrees at {i}"
        except ValueError:
            # cryptography refuses exactly the all-zero results; the ladder
            # must agree that THIS input was a low-order hit.
            assert mine == 0, f"oracle disagreement on a zero: ladder={mine}"
    base9 = ladder(9, 9)  # RFC 7748 iterated-test seed, base point sanity
    assert base9 == int.from_bytes(
        x25519.X25519PrivateKey.from_private_bytes(le(clamp(9))).public_key()
        .public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw), "little"), "base-point derivation disagrees"

    # --- the hostile set --------------------------------------------------
    canonical = low_order_u_values(rng)
    assert 0 in canonical and 1 in canonical, "computed subgroup lost u=0/u=1"

    vectors = []  # (name, qc_int, refuse, expect_k)

    def add(name, u, force_refuse=None):
        k = ladder(server_priv, u)
        refuse = 1 if k == 0 else 0
        if force_refuse is not None:
            assert refuse == force_refuse, f"{name}: refuse={refuse}, expected {force_refuse}"
        vectors.append((name, u, refuse, None if refuse else k))

    # every canonical low-order u, plus every non-canonical spelling of it:
    # u+k*p still under 2^255, and the high-bit-set spellings. These are the
    # encodings a sloppy ladder turns into attacker-chosen secrets.
    for u in sorted(canonical):
        add(f"low_canonical_{u:064x}", u, force_refuse=1)
        for kp_ in (1, 2, 3):
            if u + kp_ * P < (1 << 255):
                add(f"low_plus_{kp_}p_{(u + kp_ * P) % P:064x}", u + kp_ * P, force_refuse=1)
        add(f"low_hibit_{u:064x}", u | (1 << 255), force_refuse=1)

    # ordinary points, canonical and not: MUST be accepted with the exact
    # masked-then-computed secret (pins the RFC 7748 masking behavior).
    for i in range(6):
        up = rng() & M64
        add(f"ordinary_{i}", up, force_refuse=0)
        add(f"ordinary_{i}_hibit", up | (1 << 255), force_refuse=0)  # same residue after mask
        for kp_ in (1, 2, 3):
            v = up + kp_ * P
            if v < (1 << 255):
                add(f"ordinary_{i}_plus_{kp_}p", v, force_refuse=0)  # same residue mod p

    # structural junk: p itself and p-1 (decode to residues 0 and -1)
    add("spelling_p", P, force_refuse=1)  # p = 0 in GF(p): the order-2 point again

    with open(out_path, "w") as f:
        f.write(f"SERVER_PRIV {le(server_priv).hex()}\n")
        for name, u, refuse, k in vectors:
            kh = "-" if k is None else le(k).hex()  # wire byte order, like the QC
            f.write(f"VEC {name} {le(u).hex()} {refuse} {kh}\n")
    n_ref = sum(1 for v in vectors if v[2])
    print(f"wrote {out_path}: {len(vectors)} vectors, {n_ref} expected-refused "
          f"({len(canonical)} canonical low-order u computed)")


if __name__ == "__main__":
    main()
