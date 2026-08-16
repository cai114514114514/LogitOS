#!/usr/bin/env python3
# crypto_diff_gen.py - random differential-test vector generator for Logit's
# hand-rolled crypto (c/crypto/). Writes a text vector file consumed by
# tests/unit/crypto_diff_test.c; the C implementation must reproduce every
# expected output byte-for-byte.
#
# Reference implementations here are pure Python (stdlib only) and are
# self-checked against published KATs BEFORE any generation happens:
#   X25519   - RFC 7748 ladder, checked against RFC 7748 sections 5.2 and 6.1
#   AES      - FIPS-197, all three key sizes, checked against the FIPS-197
#              Appendix B/C.1/C.3 vectors (openssl-confirmed)
#   AES-GCM  - SP 800-38D, 96-bit and arbitrary IVs, all key sizes, checked
#              against the NIST/McGrew-Viega vectors (TC1-20; the non-96-bit
#              cases TC5/6/11/12/17/18 were additionally confirmed against the
#              python `cryptography` AESGCM, which is OpenSSL)
#   AES-CTR  - SP 800-38A F.5.1/F.5.3/F.5.5 (openssl-confirmed), plus a
#              counter-wrap case that pins the full-128-bit increment
#   AES-CBC  - SP 800-38A F.2.1/F.2.3/F.2.5 + PKCS#7 (openssl-confirmed)
#   ChaCha20-Poly1305 - RFC 8439, checked against RFC 8439 A.5 and 2.8.2
#              (the same vectors crypto_vec_test.c uses)
#   HKDF     - RFC 5869, checked against RFC 5869 Case 1 and the SHA-384
#              vectors in tests/unit/crypto_vectors.h
#   HKDF-Expand-Label - RFC 8446 section 7.1, checked against the el256/el384
#              vectors in tests/unit/crypto_vectors.h
#   PBKDF2   - hashlib.pbkdf2_hmac directly (it IS the reference PRF)
#   SHA/HMAC - hashlib/hmac (authoritative), including sha512_224/sha512_256
#
# Usage: crypto_diff_gen.py <output-file>   (deterministic: seed 20260805)

import hashlib
import hmac as hmac_mod
import random
import sys

SEED = 20260805

# ---------------------------------------------------------------------------
# X25519 reference (RFC 7748 section 5)
# ---------------------------------------------------------------------------

P25519 = 2**255 - 19


def x25519_ref(scalar_bytes, point_bytes):
    k = bytearray(scalar_bytes)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    k = int.from_bytes(k, "little")
    u = int.from_bytes(point_bytes, "little") & ((1 << 255) - 1)
    x1 = u
    x2, z2 = 1, 0
    x3, z3 = u, 1
    swap = 0
    for t in range(254, -1, -1):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        A = (x2 + z2) % P25519
        AA = (A * A) % P25519
        B = (x2 - z2) % P25519
        BB = (B * B) % P25519
        E = (AA - BB) % P25519
        C = (x3 + z3) % P25519
        D = (x3 - z3) % P25519
        DA = (D * A) % P25519
        CB = (C * B) % P25519
        x3 = ((DA + CB) ** 2) % P25519
        z3 = (x1 * ((DA - CB) ** 2)) % P25519
        x2 = (AA * BB) % P25519
        z2 = (E * (AA + 121665 * E)) % P25519
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    out = (x2 * pow(z2, P25519 - 2, P25519)) % P25519
    return out.to_bytes(32, "little")


# ---------------------------------------------------------------------------
# AES reference (FIPS-197), all three key sizes, encryption direction only
# ---------------------------------------------------------------------------

_SBOX = []
_INV_SBOX = []


def _aes_init_tables():
    p = q = 1
    sbox = [0] * 256
    # generate S-box via GF(2^8) log/exp tables (FIPS-197 style)
    log = [0] * 256
    exp = [0] * 512
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        x ^= (x << 1) ^ (0x11B if x & 0x80 else 0)
        x &= 0xFF
    for i in range(255, 512):
        exp[i] = exp[i - 255]
    sbox[0] = 0x63
    for i in range(1, 256):
        inv = exp[255 - log[i]]
        s = inv
        for _ in range(4):
            inv = ((inv << 1) | (inv >> 7)) & 0xFF
            s ^= inv
        sbox[i] = s ^ 0x63
    inv_sbox = [0] * 256
    for i in range(256):
        inv_sbox[sbox[i]] = i
    return sbox, inv_sbox


_SBOX, _INV_SBOX = _aes_init_tables()

_RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _xtime(a):
    return ((a << 1) ^ (0x1B if a & 0x80 else 0)) & 0xFF


def _aes_expand_key(key):
    """FIPS-197 5.2 key schedule for nk = 4/6/8 (AES-128/192/256), flat bytes.
    Byte order matches c_key_expand() in aesgcm.c exactly, including the
    nk > 6 extra SubWord, so a schedule mismatch localises to one of the two."""
    nk = len(key) // 4
    total = 16 * (nk + 7)                       # 4*(nr+1) words, nr = nk+6
    w = list(key)
    rcon = 1
    for i in range(len(key), total, 4):
        t = w[i - 4:i]
        wid = i // 4
        if wid % nk == 0:
            t = [_SBOX[t[1]] ^ rcon, _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
            rcon = _xtime(rcon)                 # never wraps: max rcon is 0x80
        elif nk > 6 and wid % nk == 4:
            t = [_SBOX[b] for b in t]
        for j in range(4):
            w.append(w[i - len(key) + j] ^ t[j])
    return w


def aes_encrypt_block(key_sched, block):
    nr = len(key_sched) // 16 - 1
    # state[r][c] = block[r + 4c]
    s = [block[i] ^ key_sched[i] for i in range(16)]

    def shift_rows(st):
        # state index i = r + 4c (column-major); row r rotates left by r
        return [st[r + 4 * ((c + r) % 4)] for c in range(4) for r in range(4)]

    def mix_columns(st):
        out = [0] * 16
        for c in range(4):
            col = st[4 * c:4 * c + 4]
            out[4 * c + 0] = _xtime(col[0]) ^ (_xtime(col[1]) ^ col[1]) ^ col[2] ^ col[3]
            out[4 * c + 1] = col[0] ^ _xtime(col[1]) ^ (_xtime(col[2]) ^ col[2]) ^ col[3]
            out[4 * c + 2] = col[0] ^ col[1] ^ _xtime(col[2]) ^ (_xtime(col[3]) ^ col[3])
            out[4 * c + 3] = (_xtime(col[0]) ^ col[0]) ^ col[1] ^ col[2] ^ _xtime(col[3])
        return out

    for rnd in range(1, nr):
        s = [_SBOX[b] for b in s]
        s = shift_rows(s)
        s = mix_columns(s)
        s = [s[i] ^ key_sched[16 * rnd + i] for i in range(16)]
    s = [_SBOX[b] for b in s]
    s = shift_rows(s)
    s = [s[i] ^ key_sched[16 * nr + i] for i in range(16)]
    return bytes(s)


# --- AES-CTR (SP 800-38A 6.5): full 128-bit counter, not GCM's inc32 ---------

def aes_ctr_ref(key, iv, data):
    sched = _aes_expand_key(key)
    out = bytearray()
    cb = int.from_bytes(iv, "big")
    for i in range(0, len(data), 16):
        ks = aes_encrypt_block(sched, cb.to_bytes(16, "big"))
        blk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(blk, ks))
        cb = (cb + 1) & ((1 << 128) - 1)
    return bytes(out)


# --- AES-CBC + PKCS#7 (SP 800-38A 6.2 + RFC 5652 6.3) -------------------------

def _pkcs7(b):
    p = 16 - (len(b) % 16)
    return b + bytes([p]) * p


def aes_cbc_encrypt_ref(key, iv, pt):
    sched = _aes_expand_key(key)
    data = _pkcs7(pt)
    out = bytearray()
    chain = iv
    for i in range(0, len(data), 16):
        blk = bytes(a ^ b for a, b in zip(data[i:i + 16], chain))
        chain = aes_encrypt_block(sched, blk)
        out += chain
    return bytes(out)


# ---------------------------------------------------------------------------
# GCM reference (SP 800-38D), all key sizes, arbitrary IV lengths
# ---------------------------------------------------------------------------

_GHASH_R = 0xE1000000000000000000000000000000


def _gf128_mul(x, y):
    z = 0
    v = y
    for i in range(128):
        if (x >> (127 - i)) & 1:
            z ^= v
        v = (v >> 1) ^ _GHASH_R if v & 1 else v >> 1
    return z


def _ghash(h, data):
    y = 0
    for i in range(0, len(data), 16):
        y = _gf128_mul(y ^ int.from_bytes(data[i:i + 16], "big"), h)
    return y.to_bytes(16, "big")


def _pad16(b):
    return b + b"\x00" * ((16 - len(b) % 16) % 16)


def _gcm_j0(h, nonce):
    """SP 800-38D 5.2.1.1: fast path at 96 bits, GHASH construction beyond."""
    if len(nonce) == 12:
        return nonce + b"\x00\x00\x00\x01"
    data = _pad16(nonce) + b"\x00" * 8 + (len(nonce) * 8).to_bytes(8, "big")
    return _ghash(h, data)


def _gctr(sched, icb, data):
    out = bytearray()
    cb = int.from_bytes(icb, "big")
    for i in range(0, len(data), 16):
        ks = aes_encrypt_block(sched, cb.to_bytes(16, "big"))
        blk = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(blk, ks))
        cb = (cb & ~0xFFFFFFFF) | ((cb + 1) & 0xFFFFFFFF)  # inc32
    return bytes(out)


def aes_gcm_seal_ref(key, nonce, aad, pt):
    sched = _aes_expand_key(key)
    h = int.from_bytes(aes_encrypt_block(sched, b"\x00" * 16), "big")
    j0 = _gcm_j0(h, nonce)
    j0_inc = (int.from_bytes(j0, "big") & ~0xFFFFFFFF) | \
             ((int.from_bytes(j0, "big") + 1) & 0xFFFFFFFF)
    ct = _gctr(sched, j0_inc.to_bytes(16, "big"), pt)
    lens = (len(aad) * 8).to_bytes(8, "big") + (len(ct) * 8).to_bytes(8, "big")
    s = _ghash(h, _pad16(aad) + _pad16(ct) + lens)
    tag = bytes(a ^ b for a, b in zip(aes_encrypt_block(sched, j0), s))
    return ct, tag


# ---------------------------------------------------------------------------
# ChaCha20 / Poly1305 / AEAD reference (RFC 8439)
# ---------------------------------------------------------------------------

def _rotl32(v, n):
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFF


def _quarter_round(st, a, b, c, d):
    st[a] = (st[a] + st[b]) & 0xFFFFFFFF; st[d] = _rotl32(st[d] ^ st[a], 16)
    st[c] = (st[c] + st[d]) & 0xFFFFFFFF; st[b] = _rotl32(st[b] ^ st[c], 12)
    st[a] = (st[a] + st[b]) & 0xFFFFFFFF; st[d] = _rotl32(st[d] ^ st[a], 8)
    st[c] = (st[c] + st[d]) & 0xFFFFFFFF; st[b] = _rotl32(st[b] ^ st[c], 7)


def _chacha20_block(key, counter, nonce):
    const = b"expand 32-byte k"
    st = [int.from_bytes(const[i:i + 4], "little") for i in range(0, 16, 4)]
    st += [int.from_bytes(key[i:i + 4], "little") for i in range(0, 32, 4)]
    st.append(counter & 0xFFFFFFFF)
    st += [int.from_bytes(nonce[i:i + 4], "little") for i in range(0, 12, 4)]
    w = st[:]
    for _ in range(10):
        _quarter_round(w, 0, 4, 8, 12)
        _quarter_round(w, 1, 5, 9, 13)
        _quarter_round(w, 2, 6, 10, 14)
        _quarter_round(w, 3, 7, 11, 15)
        _quarter_round(w, 0, 5, 10, 15)
        _quarter_round(w, 1, 6, 11, 12)
        _quarter_round(w, 2, 7, 8, 13)
        _quarter_round(w, 3, 4, 9, 14)
    return b"".join(((w[i] + st[i]) & 0xFFFFFFFF).to_bytes(4, "little") for i in range(16))


def _chacha20_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = _chacha20_block(key, counter, nonce)
        blk = data[i:i + 64]
        out += bytes(a ^ b for a, b in zip(blk, ks))
        counter = (counter + 1) & 0xFFFFFFFF
    return bytes(out)


def _poly1305_mac(msg, key):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        blk = msg[i:i + 16]
        n = int.from_bytes(blk, "little") + (1 << (8 * len(blk)))
        acc = ((acc + n) * r) % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def chacha20_poly1305_seal_ref(key, nonce, aad, pt):
    otk = _chacha20_block(key, 0, nonce)[:32]
    ct = _chacha20_xor(key, 1, nonce, pt)
    mac_data = (_pad16(aad) + _pad16(ct) +
                len(aad).to_bytes(8, "little") + len(ct).to_bytes(8, "little"))
    return ct, _poly1305_mac(mac_data, otk)


# ---------------------------------------------------------------------------
# HKDF reference (RFC 5869) + HKDF-Expand-Label (RFC 8446 section 7.1)
# ---------------------------------------------------------------------------

def _hash_name(hlen):
    return {28: "sha224", 32: "sha256", 48: "sha384", 64: "sha512"}[hlen]


def hkdf_extract_ref(hlen, salt, ikm):
    if not salt:
        salt = b"\x00" * hlen
    return hmac_mod.new(salt, ikm, _hash_name(hlen)).digest()


def hkdf_expand_ref(hlen, prk, info, outlen):
    okm = b""
    t = b""
    counter = 1
    while len(okm) < outlen:
        t = hmac_mod.new(prk, t + info + bytes([counter]), _hash_name(hlen)).digest()
        okm += t
        counter += 1
    return okm[:outlen]


def hkdf_expand_label_ref(hlen, secret, label, ctx, outlen):
    full = b"tls13 " + label
    hkdf_label = (outlen.to_bytes(2, "big") + bytes([len(full)]) + full +
                  bytes([len(ctx)]) + ctx)
    return hkdf_expand_ref(hlen, secret, hkdf_label, outlen)


# ---------------------------------------------------------------------------
# Self-checks: every reference must pass its published KATs before we generate
# ---------------------------------------------------------------------------

def _h(s):
    return bytes.fromhex(s)


def self_check():
    fails = []

    def chk(name, got, want):
        ok = got == want
        print("selfcheck %-38s %s" % (name, "PASS" if ok else "FAIL"))
        if not ok:
            fails.append(name)

    # --- X25519: RFC 7748 section 5.2, the two single vectors ---
    chk("x25519 rfc7748 5.2 #1",
        x25519_ref(_h("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                   _h("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")),
        _h("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"))
    chk("x25519 rfc7748 5.2 #2",
        x25519_ref(_h("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                   _h("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493")),
        _h("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"))
    # iterated vector, 1 iteration (k = u = 09 followed by zeros)
    k = u = _h("0900000000000000000000000000000000000000000000000000000000000000")
    k, u = x25519_ref(k, u), k
    chk("x25519 rfc7748 iterated 1", k,
        _h("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"))
    # --- X25519: RFC 7748 section 6.1 ECDH ---
    a_priv = _h("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    a_pub = _h("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")
    b_priv = _h("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
    b_pub = _h("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f")
    k_ab = _h("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742")
    nine = b"\x09" + b"\x00" * 31
    chk("x25519 rfc7748 ecdh alice pub", x25519_ref(a_priv, nine), a_pub)
    chk("x25519 rfc7748 ecdh bob pub", x25519_ref(b_priv, nine), b_pub)
    chk("x25519 rfc7748 ecdh K (alice)", x25519_ref(a_priv, b_pub), k_ab)
    chk("x25519 rfc7748 ecdh K (bob)", x25519_ref(b_priv, a_pub), k_ab)

    # --- AES: FIPS-197 Appendix B (128) / C.1 (192) / C.3 (256) ---
    sched = _aes_expand_key(_h("2b7e151628aed2a6abf7158809cf4f3c"))
    chk("aes128 fips197 appB",
        aes_encrypt_block(sched, _h("3243f6a8885a308d313198a2e0370734")),
        _h("3925841d02dc09fbdc118597196a0b32"))
    sched = _aes_expand_key(_h("8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b"))
    chk("aes192 fips197 C.1",
        aes_encrypt_block(sched, _h("6bc1bee22e409f96e93d7e117393172a")),
        _h("bd334f1d6e45f25ff712a214571fa5cc"))
    sched = _aes_expand_key(_h("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"))
    chk("aes256 fips197 C.3",
        aes_encrypt_block(sched, _h("6bc1bee22e409f96e93d7e117393172a")),
        _h("f3eed1bdb5d2a03c064b5a7e3db181f8"))

    # --- AES-CTR: SP 800-38A F.5.1/F.5.3/F.5.5 (openssl-confirmed) ---
    PT4 = _h("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
             "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710")
    CTRIV = _h("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
    chk("ctr128 sp800-38a f.5.1",
        aes_ctr_ref(_h("2b7e151628aed2a6abf7158809cf4f3c"), CTRIV, PT4),
        _h("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
           "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"))
    chk("ctr192 sp800-38a f.5.3",
        aes_ctr_ref(_h("8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b"), CTRIV, PT4),
        _h("1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94"
           "1e36b26bd1ebc670d1bd1d665620abf74f78a7f6d29809585a97daec58c6b050"))
    chk("ctr256 sp800-38a f.5.5",
        aes_ctr_ref(_h("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
                    CTRIV, PT4),
        _h("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
           "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"))
    # counter wrap: IV ...fffffffe carries out of the low dword at block 3->4
    # (openssl-confirmed). inc32 here would replay block 2's keystream.
    chk("ctr128 full-carry wrap",
        aes_ctr_ref(_h("2b7e151628aed2a6abf7158809cf4f3c"),
                    _h("000000000000000000000000fffffffe"), PT4),
        _h("19349c288a689b7097ef8ead5f31d79f9decc4298cdb4779c055b775cfb1eb63"
           "5759b7d88cf209fea276cf653f4a4341837e18d6ab8113d3a67b557a0e27639f"))

    # --- AES-CBC+PKCS#7: SP 800-38A F.2.1/F.2.3/F.2.5 encrypt columns
    # (openssl-confirmed at the block level; PKCS#7 tail added by the ref) ---
    CBCIV = _h("000102030405060708090a0b0c0d0e0f")
    ct = aes_cbc_encrypt_ref(_h("2b7e151628aed2a6abf7158809cf4f3c"), CBCIV, PT4)
    chk("cbc128 sp800-38a f.2.1 head",
        ct[:64],
        _h("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
           "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"))
    ct = aes_cbc_encrypt_ref(_h("8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b"), CBCIV, PT4)
    chk("cbc192 sp800-38a f.2.3 head",
        ct[:64],
        _h("4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a"
           "571b242012fb7ae07fa9baac3df102e008b0e27988598881d920a9e64f5615cd"))
    ct = aes_cbc_encrypt_ref(_h("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
                             CBCIV, PT4)
    chk("cbc256 sp800-38a f.2.5 head",
        ct[:64],
        _h("f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
           "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"))

    # --- GCM 96-bit: the vectors tests/unit/crypto_vec_test.c validates C with ---
    ct, tag = aes_gcm_seal_ref(b"\x00" * 16, b"\x00" * 12, b"", b"")
    chk("gcm nist zero-key empty", tag, _h("58e2fccefa7e3061367f1d57a4e7455a"))
    ct, tag = aes_gcm_seal_ref(b"\x00" * 16, b"\x00" * 12, b"", b"\x00" * 16)
    chk("gcm nist zero-key 16B ct", ct, _h("0388dace60b6a392f328c2b971b2fe78"))
    chk("gcm nist zero-key 16B tag", tag, _h("ab6e47d42cec13bdf53a67b21257bddf"))
    key = _h("feffe9928665731c6d6a8f9467308308")
    iv = _h("cafebabefacedbaddecaf888")
    aad = _h("feedfacedeadbeeffeedfacedeadbeefabaddad2")
    pt = _h("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39")
    ct, tag = aes_gcm_seal_ref(key, iv, aad, pt)
    chk("gcm mcgrew-viega tc3 ct", ct,
        _h("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
           "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091"))
    chk("gcm mcgrew-viega tc3 tag", tag, _h("5bc94fbc3221a5db94fae95ae7121a47"))
    pt4 = pt + _h("1aafd255")
    ct, tag = aes_gcm_seal_ref(key, iv, aad, pt4)
    chk("gcm mcgrew-viega tc4 ct", ct,
        _h("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
           "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985"))
    chk("gcm mcgrew-viega tc4 tag", tag, _h("da80ce830cfda02da2a218a1744f4c76"))

    # --- GCM non-96-bit IVs: McGrew-Viega TC5/6 (128), TC11/12 (192),
    # TC17/18 (256). These pin J0 = GHASH_H(IV||0^s||0^64||[len]_64); they
    # were confirmed against python `cryptography` AESGCM before landing. ---
    IV60 = _h("9313225df88406e555909c5aff5269aa6a7a9538534f7da1e4c303d2a318a728"
              "c3c0c95156809539fcf0e2429a6b525416aedbf5a0de6a57a637b39b")
    IV8 = _h("cafebabefacedbad")
    K192 = _h("feffe9928665731c6d6a8f9467308308feffe9928665731c")
    K256 = _h("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
    for nm, k, ivv, want_ct, want_tag in (
        ("gcm tc5 iv8", key, IV8,
         "61353b4c2806934a777ff51fa22a4755699b2a714fcdc6f83766e5f97b6c7423"
         "73806900e49f24b22b097544d4896b424989b5e1ebac0f07c23f4598",
         "3612d2e79e3b0785561be14aaca2fccb"),
        ("gcm tc6 iv60", key, IV60,
         "8ce24998625615b603a033aca13fb894be9112a5c3a211a8ba262a3cca7e2ca7"
         "01e4a9a4fba43c90ccdcb281d48c7c6fd62875d2aca417034c34aee5",
         "619cc5aefffe0bfa462af43c1699d050"),
        ("gcm tc11 iv8", K192, IV8,
         "0f10f599ae14a154ed24b36e25324db8c566632ef2bbb34f8347280fc4507057"
         "fddc29df9a471f75c66541d4d4dad1c9e93a19a58e8b473fa0f062f7",
         "65dcc57fcf623a24094fcca40d3533f8"),
        ("gcm tc12 iv60", K192, IV60,
         "d27e88681ce3243c4830165a8fdcf9ff1de9a1d8e6b447ef6ef7b79828666e45"
         "81e79012af34ddd9e2f037589b292db3e67c036745fa22e7e9b7373b",
         "dcf566ff291c25bbb8568fc3d376a6d9"),
        ("gcm tc17 iv8", K256, IV8,
         "c3762df1ca787d32ae47c13bf19844cbaf1ae14d0b976afac52ff7d79bba9de0"
         "feb582d33934a4f0954cc2363bc73f7862ac430e64abe499f47c9b1f",
         "3a337dbf46a792c45e454913fe2ea8f2"),
        ("gcm tc18 iv60", K256, IV60,
         "5a8def2f0c9e53f1f75d7853659e2a20eeb2b22aafde6419a058ab4f6f746bf4"
         "0fc0c3b780f244452da3ebf1c5d82cdea2418997200ef82e44ae7e3f",
         "a44a8266ee1c8eb0c8b5d4cf5ae9f19a"),
    ):
        ct, tag = aes_gcm_seal_ref(k, ivv, aad, pt)
        chk("%s ct" % nm, ct, _h(want_ct))
        chk("%s tag" % nm, tag, _h(want_tag))

    # --- ChaCha20-Poly1305: RFC 8439 A.5 and 2.8.2 (same as crypto_vec_test.c) ---
    key = _h("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")
    nonce = _h("070000004041424344454647")
    aad = _h("50515253c0c1c2c3c4c5c6c7")
    pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you "
          b"only one tip for the future, sunscreen would be it.")
    ct, tag = chacha20_poly1305_seal_ref(key, nonce, aad, pt)
    chk("chacha rfc8439 a.5 ct", ct,
        _h("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
           "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
           "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
           "3ff4def08e4b7a9de576d26586cec64b6116"))
    chk("chacha rfc8439 a.5 tag", tag, _h("1ae10b594f09e26a7e902ecbd0600691"))
    key = _h("1c9240a5eb55d38af333888604f6b5f0473917c1402b80099dca5cbc207075c0")
    nonce = _h("000000000102030405060708")
    aad = _h("f33388860000000000004e91")
    pt = _h("496e7465726e65742d4472616674732061726520647261667420646f63756d656e74"
            "732076616c696420666f722061206d6178696d756d206f6620736978206d6f6e7468"
            "7320616e64206d617920626520757064617465642c207265706c616365642c206f72"
            "206f62736f6c65746564206279206f7468657220646f63756d656e74732061742061"
            "6e792074696d652e20497420697320696e617070726f70726961746520746f207573"
            "6520496e7465726e65742d447261667473206173207265666572656e6365206d6174"
            "657269616c206f7220746f2063697465207468656d206f74686572207468616e2061"
            "73202fe2809c776f726b20696e2070726f67726573732e2fe2809d")
    ct, tag = chacha20_poly1305_seal_ref(key, nonce, aad, pt)
    chk("chacha rfc8439 2.8.2 ct", ct,
        _h("64a0861575861af460f062c79be643bd5e805cfd345cf389f108670ac76c8cb24c"
           "6cfc18755d43eea09ee94e382d26b0bdb7b73c321b0100d4f03b7f355894cf332f"
           "830e710b97ce98c8a84abd0b948114ad176e008d33bd60f982b1ff37c8559797a0"
           "6ef4f0ef61c186324e2b3506383606907b6a7c02b0f9f6157b53c867e4b9166c76"
           "7b804d46a59b5216cde7a4e99040c5a40433225ee282a1b0a06c523eaf4534d7f8"
           "3fa1155b0047718cbc546a0d072b04b3564eea1b422273f548271a0bb2316053fa"
           "76991955ebd63159434ecebb4e466dae5a1073a6727627097a1049e617d91d3610"
           "94fa68f0ff77987130305beaba2eda04df997b714d6c6f2c29a6ad5cb4022b02709b"))
    chk("chacha rfc8439 2.8.2 tag", tag, _h("eead9d67890cbb22392336fea1851f38"))

    # --- HKDF: RFC 5869 Case 1 (SHA-256) ---
    prk = hkdf_extract_ref(32, _h("000102030405060708090a0b0c"), b"\x0b" * 22)
    chk("hkdf rfc5869 case1 extract", prk,
        _h("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"))
    chk("hkdf rfc5869 case1 expand",
        hkdf_expand_ref(32, prk, _h("f0f1f2f3f4f5f6f7f8f9"), 42),
        _h("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
           "34007208d5b887185865"))
    # --- HKDF-SHA384: vectors from tests/unit/crypto_vectors.h ---
    ikm = bytes(range(0x00, 0x30))
    salt = bytes(range(0x40, 0x70))
    info = b"logit hkdf info"
    prk = hkdf_extract_ref(48, salt, ikm)
    chk("hkdf384 vectors.h extract", prk,
        _h("412a5ab53e6945a1270d92e0ea5b62e79ce4133f99e2de74ee048e108939af43"
           "c72505856e960b29d4c06185ad2b08f2"))
    chk("hkdf384 vectors.h expand L=100", hkdf_expand_ref(48, prk, info, 100),
        _h("c3d1057ea66d94443012d5e52ae94bc367636553373b2c16e571eb52234d74bd48"
           "e0950e30599a9b59ab47c6505398dfd5a49a0d1a377833fae4d01e6c5f580af0117d85"
           "d4fb9996afe7732129612d76c2644abd0c344d9d96868776a60d25d7017750a8"))

    # --- HKDF-Expand-Label: vectors from tests/unit/crypto_vectors.h ---
    el256_secret = bytes(range(0x00, 0x20))
    el256_ctx = _h("54e6289e14c7b0e7ad9acc2dfc4c1e3d027d0eef7f5c4c3fe7c292761d0e06a6")
    chk("expand_label 256 c hs traffic",
        hkdf_expand_label_ref(32, el256_secret, b"c hs traffic", el256_ctx, 32),
        _h("29d5931544f5cd4dd517b0edce6417ae4459cff4a213154734aeec336dbf12df"))
    el384_secret = bytes(range(0x00, 0x30))
    el384_ctx = _h("520cde83e730ef1c04fe443dc399ded36c0f275993c190b6fc9fc11db7dae644"
                   "ae6073f4371fa061a7482e6cc2e90ed7")
    chk("expand_label 384 s ap traffic",
        hkdf_expand_label_ref(48, el384_secret, b"s ap traffic", el384_ctx, 48),
        _h("460b0d77dd84b7db0c5076a5d9f22578700bd2dc0f5102d4a0327620ca225601"
           "09d79200d76db0b6b2afc00777b9f022"))
    chk("expand_label 384 key empty ctx",
        hkdf_expand_label_ref(48, el384_secret, b"key", b"", 32),
        _h("6877d022f1c61d24ebb7487c16752d9a4798e40431c75b39320e537c90e23225"))
    chk("expand_label 256 iv empty ctx",
        hkdf_expand_label_ref(32, el256_secret, b"iv", b"", 12),
        _h("2f41c846a431a163814bcd71"))

    # --- SHA-224: FIPS 180-4 "abc". Pinned here as well as in the battery
    # because sha224 and sha512_224 produce the same LENGTH from different
    # cores, and a dispatch that confused them would still be 28 bytes. ---
    chk("sha224 abc", hashlib.new("sha224", b"abc").hexdigest(),
        "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7")
    chk("hmac224 rfc4231 tc1",
        hmac_mod.new(b"" * 20, b"Hi There", "sha224").hexdigest(),
        "896fb1128abbdf196832107cd49df33f47b4b1169912ba4f53684b22")

    # --- SHA-512/224 / SHA-512/256: FIPS 180-4 "abc" ---
    chk("sha512_224 abc", hashlib.new("sha512_224", b"abc").hexdigest(),
        "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa")
    chk("sha512_256 abc", hashlib.new("sha512_256", b"abc").hexdigest(),
        "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23")

    # --- HMAC-SHA-512: RFC 4231 TC1 (short key) and TC6 (key > block) ---
    chk("hmac512 rfc4231 tc1",
        hmac_mod.new(b"\x0b" * 20, b"Hi There", _hash_name(64)).hexdigest(),
        "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
        "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854")
    chk("hmac512 rfc4231 tc6",
        hmac_mod.new(b"\xaa" * 131,
                     b"Test Using Larger Than Block-Size Key - Hash Key First",
                     _hash_name(64)).hexdigest(),
        "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
        "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598")

    # --- HKDF-SHA-512: the vectors in tests/unit/crypto_vectors.h ---
    ikm = bytes(range(0x00, 0x50))
    salt = bytes(range(0x60, 0x80))
    info = b"logit hkdf512 info"
    prk = hkdf_extract_ref(64, salt, ikm)
    chk("hkdf512 vectors.h extract", prk,
        _h("4e17866827867f625e07fd684ca2e33ed8b4e15d2fa379ad985a4e10246b8172"
           "3bf9309fe8475af9b66088368cc8099bc1f8e32ce3126e411c6c3cd2a377ed12"))
    chk("hkdf512 vectors.h expand L=136", hkdf_expand_ref(64, prk, info, 136),
        _h("4683a671578b9222a035d755a8d5a1bea51769d2b8f2426fd710152e2596e3fc"
           "4c997b5fe9190a13c7631f1ac940ab3edb0ca4ff31237a47d76c46f10eafade9"
           "7ffb80e853e91da9b8b4641637b61f5e51c2fe56b3a2b10dafb2319a271c93f9"
           "b610630ecad7dc0ed119f9ce19e9f2d19f1121371d3c300f131f6af18daea5cc"
           "d0d64ebf22b5c91a"))

    if fails:
        print("SELF-CHECK FAILED (%d): %s" % (len(fails), ", ".join(fails)))
        return False
    print("selfcheck: all reference implementations verified")
    return True


# ---------------------------------------------------------------------------
# Vector generation
# ---------------------------------------------------------------------------

# NIST P-256 / P-384 field prime p and group order n (big-endian ints)
P256_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
P256_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
P384_P = int("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffe"
             "ffffffff0000000000000000ffffffff", 16)
P384_N = int("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf"
             "581a0db248b0a77aecec196accc52973", 16)


def _hx(b):
    return b.hex() if b else "-"


def _rand_bytes(rng, n):
    return bytes(rng.randrange(256) for _ in range(n))


def _be(n, nbytes):
    return n.to_bytes(nbytes, "big")


def generate(path):
    rng = random.Random(SEED)
    out = open(path, "w")
    counts = {}

    def emit(op, *fields):
        out.write(op + " " + " ".join(fields) + "\n")
        counts[op] = counts.get(op, 0) + 1

    # --- emul: ecdsa_modmul_test, out = a*b mod m (m = p or n of P-256/P-384) ---
    for curveid, (p, n, nbytes) in ((0, (P256_P, P256_N, 32)), (1, (P384_P, P384_N, 48))):
        for useorder in (0, 1):
            m = n if useorder else p
            for _ in range(25000):
                a = rng.randrange(m)
                b = rng.randrange(m)
                emit("emul", str(curveid), str(useorder),
                     _be(a, nbytes).hex(), _be(b, nbytes).hex(),
                     _be((a * b) % m, nbytes).hex())
            # edges: one operand in {0,1,m-1,m-2}, other random / also small
            edges = (0, 1, m - 1, m - 2)
            for _ in range(256):
                a = edges[rng.randrange(4)] if rng.randrange(2) else rng.randrange(m)
                b = edges[rng.randrange(4)] if rng.randrange(2) else rng.randrange(m)
                emit("emul", str(curveid), str(useorder),
                     _be(a, nbytes).hex(), _be(b, nbytes).hex(),
                     _be((a * b) % m, nbytes).hex())

    # --- rexp: rsa_modexp_be, out = base^e mod n ---
    widths = (512, 1024, 1536, 2048, 3072, 4096)
    for _ in range(5000):
        bits = widths[rng.randrange(len(widths))]
        nbytes = bits // 8
        n = rng.getrandbits(bits) | (1 << (bits - 1)) | 1   # odd, exact width
        e = rng.getrandbits(rng.randrange(3, 257)) | 1
        base = rng.randrange(n)
        bl = max(1, (base.bit_length() + 7) // 8)
        el = max(1, (e.bit_length() + 7) // 8)
        emit("rexp", _be(base, bl).hex(), _be(e, el).hex(), n.to_bytes(nbytes, "big").hex(),
             _be(pow(base, e, n), nbytes).hex())
    # rexp edges: degenerate bases/exponents, tiny moduli
    for _ in range(30):
        bits = widths[rng.randrange(len(widths))]
        nbytes = bits // 8
        n = rng.getrandbits(bits) | (1 << (bits - 1)) | 1
        base = (0, 1, n - 1, 2)[rng.randrange(4)]
        e = (1, 2, 3, 65537)[rng.randrange(4)]
        bl = max(1, (base.bit_length() + 7) // 8)
        emit("rexp", _be(base, bl).hex(), _be(e, 3 if e == 65537 else 1).hex(),
             n.to_bytes(nbytes, "big").hex(), _be(pow(base, e, n), nbytes).hex())

    # --- x255: x25519(out, scalar, point), all little-endian ---
    for _ in range(8000):
        scalar = _rand_bytes(rng, 32)
        point = _rand_bytes(rng, 32)
        emit("x255", scalar.hex(), point.hex(), x25519_ref(scalar, point).hex())
    # edges: special scalars x random points; special points x random scalars
    special_scalars = (b"\x00" * 32, b"\xff" * 32)
    for scalar in special_scalars:
        for _ in range(20):
            point = _rand_bytes(rng, 32)
            emit("x255", scalar.hex(), point.hex(), x25519_ref(scalar, point).hex())
    nine = b"\x09" + b"\x00" * 31
    low_points = [nine]
    for u in (0, 1, P25519 - 1, P25519, P25519 + 1):
        low_points.append((u & ((1 << 256) - 1)).to_bytes(32, "little"))
    for point in low_points:
        for _ in range(20):
            scalar = _rand_bytes(rng, 32)
            emit("x255", scalar.hex(), point.hex(), x25519_ref(scalar, point).hex())

    # --- gcm: aes128_gcm_seal/open, 96-bit nonce ---
    aad_lens = (0, 1, 15, 16, 17, 31, 32, 64, 255)
    pt_lens = (0, 1, 15, 16, 17, 63, 64, 65, 255, 256, 1024, 4096)
    for _ in range(4000):
        key = _rand_bytes(rng, 16)
        nonce = _rand_bytes(rng, 12)
        aad = _rand_bytes(rng, aad_lens[rng.randrange(len(aad_lens))])
        pt = _rand_bytes(rng, pt_lens[rng.randrange(len(pt_lens))])
        ct, tag = aes_gcm_seal_ref(key, nonce, aad, pt)
        emit("gcm", key.hex(), nonce.hex(), _hx(aad), _hx(pt), _hx(ct), tag.hex())

    # --- gcmx: arbitrary-length IVs, all key sizes (aes*_gcm_seal_iv) ---
    # IV lengths cross every GHASH block boundary 1..255, plus 1 (sub-block),
    # 16/17 (exact/one-over), 60 (the McGrew-Viega length) and 128/129.
    # IVs near 2^32 bits are not generated: SP 800-38D allows them but no
    # caller will hand this library a 512 MiB IV, and neither would the
    # vector file enjoy one.
    gcmx_ivlens = (1, 2, 8, 15, 16, 17, 31, 32, 60, 63, 64, 65, 127, 128, 129, 255)
    for i in range(3000):
        keylen = (16, 24, 32)[i % 3]
        key = _rand_bytes(rng, keylen)
        if i % 4 == 0:                      # edge IVs alongside random ones
            ivl = gcmx_ivlens[rng.randrange(len(gcmx_ivlens))]
            iv = bytes([0xff] * ivl) if i % 8 == 0 else bytes(ivl)
        else:
            ivl = gcmx_ivlens[rng.randrange(len(gcmx_ivlens))]
            iv = _rand_bytes(rng, ivl)
        aad = _rand_bytes(rng, aad_lens[rng.randrange(len(aad_lens))])
        pt = _rand_bytes(rng, pt_lens[rng.randrange(len(pt_lens))])
        ct, tag = aes_gcm_seal_ref(key, iv, aad, pt)
        emit("gcmx", str(keylen), key.hex(), iv.hex(), _hx(aad), _hx(pt),
             _hx(ct), tag.hex())

    # --- ctr: aes*_ctr, all key sizes, whole-block counter carry ---
    ctr_ivs = (
        b"\x00" * 16,
        b"\xff" * 16,
        _h("000000000000000000000000fffffffe"),
        _h("0000000000000000ffffffffffffffff"),
        _h("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"),
    )
    for i in range(3000):
        keylen = (16, 24, 32)[i % 3]
        key = _rand_bytes(rng, keylen)
        iv = ctr_ivs[i % 5] if i < 15 else _rand_bytes(rng, 16)
        data = _rand_bytes(rng, pt_lens[rng.randrange(len(pt_lens))])
        ctr_out = aes_ctr_ref(key, iv, data)
        emit("ctr", str(keylen), key.hex(), iv.hex(), _hx(data),
             ctr_out.hex() if ctr_out else "-")

    # --- cbc: aes*_cbc_encrypt/decrypt + PKCS#7, all key sizes ---
    for i in range(3000):
        keylen = (16, 24, 32)[i % 3]
        key = _rand_bytes(rng, keylen)
        iv = _rand_bytes(rng, 16)
        if i % 4 == 0:
            pt = bytes(rng.randrange(16))         # 0..15: pad-dominated cases
        else:
            pt = _rand_bytes(rng, pt_lens[rng.randrange(len(pt_lens))])
        ct = aes_cbc_encrypt_ref(key, iv, pt)
        emit("cbc", str(keylen), key.hex(), iv.hex(), _hx(pt), ct.hex())

    # --- aead: chacha20_poly1305_seal/open ---
    for _ in range(4000):
        key = _rand_bytes(rng, 32)
        nonce = _rand_bytes(rng, 12)
        aad = _rand_bytes(rng, aad_lens[rng.randrange(len(aad_lens))])
        pt = _rand_bytes(rng, pt_lens[rng.randrange(len(pt_lens))])
        ct, tag = chacha20_poly1305_seal_ref(key, nonce, aad, pt)
        emit("aead", key.hex(), nonce.hex(), _hx(aad), _hx(pt), _hx(ct), tag.hex())

    # --- hash: sha224/256/384/512 + sha512_224/sha512_256 ---
    must_lens = (55, 56, 63, 64, 65, 111, 112, 119, 120, 127, 128, 129)
    algos = ("sha224", "sha256", "sha384", "sha512", "sha512_224", "sha512_256")
    for i in range(4000):
        algo = algos[rng.randrange(len(algos))]
        mlen = must_lens[i % len(must_lens)] if i < len(must_lens) * len(algos) else rng.randrange(601)
        msg = _rand_bytes(rng, mlen)
        emit("hash", algo, _hx(msg), hashlib.new(algo, msg).hexdigest())

    # --- hmac: hlen 28/32/48/64, keys 0..131 bytes (incl. > block size), msg 0..300 ---
    for i in range(2500):
        hlen = (28, 32, 48, 64)[rng.randrange(4)]
        if i < 10:
            # the boundary keys for both block sizes: 0/1, block-1/block,
            # block+1, and 131 (the RFC 4231 long-key length, > 128)
            klen = (0, 1, 63, 64, 65, 127, 128, 129, 130, 131)[i]
        else:
            klen = rng.randrange(132)
        key = _rand_bytes(rng, klen)
        msg = _rand_bytes(rng, rng.randrange(301))
        tag = hmac_mod.new(key, msg, _hash_name(hlen)).digest()
        emit("hmac", str(hlen), _hx(key), _hx(msg), tag.hex())

    # --- hkdf: extract+expand, hlen 28/32/48/64 ---
    for _ in range(1200):
        hlen = (28, 32, 48, 64)[rng.randrange(4)]
        salt = _rand_bytes(rng, rng.randrange(33))
        ikm = _rand_bytes(rng, rng.randrange(65))
        info = _rand_bytes(rng, rng.randrange(33))
        outlen = rng.randrange(1, 137)     # up to 3 SHA-512 blocks
        prk = hkdf_extract_ref(hlen, salt, ikm)
        okm = hkdf_expand_ref(hlen, prk, info, outlen)
        emit("hkdf", str(hlen), _hx(salt), _hx(ikm), _hx(info), str(outlen),
             prk.hex(), okm.hex())

    # --- pdf2: pbkdf2 at every width; small iteration counts keep the
    # generator fast -- the loop logic is what matters here, and the
    # 4096/600000-iteration ends are pinned by crypto_vec_test.c/pwhash ---
    for i in range(800):
        hlen = (28, 32, 48, 64)[rng.randrange(4)]
        pw = _rand_bytes(rng, rng.randrange(73))
        salt = _rand_bytes(rng, rng.randrange(65))
        iters = (1, 1, 2, 3, 5, 10, 25)[rng.randrange(7)]
        dklen = rng.randrange(1, 81)
        dk = hashlib.pbkdf2_hmac(_hash_name(hlen), pw, salt, iters, dklen)
        emit("pdf2", str(hlen), _hx(pw), _hx(salt), str(iters), str(dklen), dk.hex())

    # --- exlb: HKDF-Expand-Label with real TLS 1.3 labels ---
    labels = (b"derived", b"c hs traffic", b"s hs traffic", b"c ap traffic",
              b"s ap traffic", b"finished", b"key", b"iv", b"res master",
              b"exp master", b"traffic upd")
    for _ in range(500):
        hlen = (32, 48)[rng.randrange(2)]
        secret = _rand_bytes(rng, hlen)
        label = labels[rng.randrange(len(labels))]
        ctx = b"" if rng.randrange(2) else _rand_bytes(rng, 32)
        outlen = (12, 16, 32, 44, 48)[rng.randrange(5)]
        emit("exlb", str(hlen), secret.hex(), label.hex(), _hx(ctx), str(outlen),
             hkdf_expand_label_ref(hlen, secret, label, ctx, outlen).hex())

    out.close()
    return counts


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: crypto_diff_gen.py <output-file>\n")
        return 2
    if not self_check():
        return 1
    counts = generate(sys.argv[1])
    total = sum(counts.values())
    for op in sorted(counts):
        print("gen %-6s %d" % (op, counts[op]))
    print("gen total  %d cases -> %s" % (total, sys.argv[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
