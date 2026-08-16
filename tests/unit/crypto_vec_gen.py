#!/usr/bin/env python3
# Regenerates tests/unit/crypto_vectors.h for tests/unit/crypto_vec_test.c
# (host-side crypto known-answer tests). Run: python3 tests/unit/crypto_vec_gen.py
import hashlib, hmac as hmac_mod, random

CURVES = {
 256: dict(
  p=int("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",16),
  a=int("ffffffff00000001000000000000000000000000fffffffffffffffffffffffc",16),
  b=int("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",16),
  n=int("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",16),
  Gx=int("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",16),
  Gy=int("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",16),
  nb=32),
 384: dict(
  p=int("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",16),
  a=int("fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000fffffffc",16),
  b=int("b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",16),
  n=int("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",16),
  Gx=int("aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",16),
  Gy=int("3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",16),
  nb=48),
}

def inv(x, m): return pow(x, -1, m)

def padd(P, Q, c):
    p, a = c['p'], c['a']
    if P is None: return Q
    if Q is None: return P
    x1, y1 = P; x2, y2 = Q
    if x1 == x2:
        if (y1 + y2) % p == 0: return None
        lam = (3*x1*x1 + a) * inv(2*y1, p) % p
    else:
        lam = (y2 - y1) * inv(x2 - x1, p) % p
    x3 = (lam*lam - x1 - x2) % p
    y3 = (lam*(x1 - x3) - y1) % p
    return (x3, y3)

def pmul(k, P, c):
    R = None
    while k:
        if k & 1: R = padd(R, P, c)
        P = padd(P, P, c)
        k >>= 1
    return R

def ecdsa_sign(cvid, d, k, hashbytes):
    c = CURVES[cvid]; n = c['n']; nb = c['nb']
    use = min(len(hashbytes), nb)
    e = int.from_bytes(hashbytes[:use], 'big')
    Q = pmul(d, (c['Gx'], c['Gy']), c)
    R = pmul(k, (c['Gx'], c['Gy']), c)
    r = R[0] % n
    s = (inv(k, n) * (e + r*d)) % n
    return Q, r, s

rng = random.Random(0xA37E)
def rnd_below(n): return rng.randrange(2, n-1)

out = []
def emit(name, bs):
    hexs = bs.hex()
    out.append('static const uint8_t ' + name + '[' + str(len(bs)) + '] = {' + ','.join('0x'+hexs[i:i+2] for i in range(0,len(hexs),2)) + '};')

ec_cases = []
for cvid, hf in [(256, hashlib.sha256), (256, hashlib.sha384), (384, hashlib.sha384), (384, hashlib.sha512)]:
    c = CURVES[cvid]; nb = c['nb']; n = c['n']
    msg = b"logit ecdsa audit message curve%d" % cvid
    h = hf(msg).digest()
    d = rnd_below(n); k = rnd_below(n)
    Q, r, s = ecdsa_sign(cvid, d, k, h)
    pub = Q[0].to_bytes(nb,'big') + Q[1].to_bytes(nb,'big')
    sig = r.to_bytes(nb,'big') + s.to_bytes(nb,'big')
    tag = "ec%d_%d" % (cvid, hf().digest_size*8)
    emit(tag + "_pub", pub); emit(tag + "_sig", sig); emit(tag + "_hash", h)
    ec_cases.append((cvid, len(h), tag))

emit("p256_n", CURVES[256]['n'].to_bytes(32,'big'))
Qbadx = (CURVES[256]['Gx']+1) % CURVES[256]['p']
emit("ec256_offcurve_pub", Qbadx.to_bytes(32,'big') + CURVES[256]['Gy'].to_bytes(32,'big'))
emit("p256_p", CURVES[256]['p'].to_bytes(32,'big'))

def is_prime(x, rounds=24):
    if x < 4: return x in (2,3)
    dd = x-1; rr = 0
    while dd % 2 == 0: dd//=2; rr+=1
    for _ in range(rounds):
        a = rng.randrange(2, x-2)
        v = pow(a, dd, x)
        if v in (1, x-1): continue
        for _ in range(rr-1):
            v = v*v % x
            if v == x-1: break
        else: return False
    return True

def gen_prime(bits):
    while True:
        x = rng.getrandbits(bits) | (1 << (bits-1)) | 1
        if is_prime(x): return x

pq = gen_prime(1024); qq = gen_prime(1024)
while qq == pq: qq = gen_prime(1024)
N = pq*qq; e = 65537
phi = (pq-1)*(qq-1)
d = inv(e, phi)
nlen = (N.bit_length()+7)//8
assert nlen == 256

def mgf1(seed, mlen, hf):
    outb = b''; c = 0
    while len(outb) < mlen:
        outb += hf(seed + c.to_bytes(4,'big')).digest()
        c += 1
    return outb[:mlen]

DI256 = bytes.fromhex("3031300d060960864801650304020105000420")
DI384 = bytes.fromhex("3041300d060960864801650304020205000430")
DI512 = bytes.fromhex("3051300d060960864801650304020305000440")

msg = b"logit rsa audit message"
h256 = hashlib.sha256(msg).digest()
h384 = hashlib.sha384(msg).digest()
h512 = hashlib.sha512(msg).digest()

def pkcs1_sign(h, di):
    T = di + h
    ps = b'\xff' * (nlen - len(T) - 3)
    em = b'\x00\x01' + ps + b'\x00' + T
    return pow(int.from_bytes(em,'big'), d, N).to_bytes(nlen,'big')

sig256 = pkcs1_sign(h256, DI256)
sig384 = pkcs1_sign(h384, DI384)
sig512 = pkcs1_sign(h512, DI512)

def pss_sign(h, hf):
    hlen = len(h); salt = bytes(range(hlen))
    emBits = N.bit_length()-1; emLen = (emBits+7)//8
    H = hf(b'\x00'*8 + h + salt).digest()
    PS = b'\x00' * (emLen - hlen - hlen - 2)
    DB = PS + b'\x01' + salt
    dbMask = mgf1(H, emLen - hlen - 1, hf)
    maskedDB = bytes(x^y for x,y in zip(DB, dbMask))
    maskedDB = bytes([maskedDB[0] & (0xff >> (8*emLen - emBits))]) + maskedDB[1:]
    em = maskedDB + H + b'\xbc'
    return pow(int.from_bytes(em,'big'), d, N).to_bytes(nlen,'big')

pss256 = pss_sign(h256, hashlib.sha256)
pss384 = pss_sign(h384, hashlib.sha384)
pss512 = pss_sign(h512, hashlib.sha512)

emit("rsa_n", N.to_bytes(nlen,'big')); emit("rsa_e", e.to_bytes(3,'big'))
emit("rsa_msg", msg)
emit("rsa_h256", h256); emit("rsa_h384", h384); emit("rsa_h512", h512)
emit("rsa_sig256", sig256); emit("rsa_sig384", sig384); emit("rsa_sig512", sig512)
emit("rsa_pss256", pss256); emit("rsa_pss384", pss384); emit("rsa_pss512", pss512)

T = DI256 + h256
em_short = b'\x00\x01' + b'\xff'*7 + b'\x00' + T + b'\x00'*(nlen - 2 - 7 - 1 - len(T))
assert len(em_short) == nlen
emit("rsa_sig_shortps", pow(int.from_bytes(em_short,'big'), d, N).to_bytes(nlen,'big'))
em_garbage = b'\x00\x01' + b'\xff'*8 + b'\x00' + T + b'\xde\xad'*( (nlen - 2 - 8 - 1 - len(T))//2 )
assert len(em_garbage) == nlen
emit("rsa_sig_garbage", pow(int.from_bytes(em_garbage,'big'), d, N).to_bytes(nlen,'big'))
em_pss = pow(int.from_bytes(pss256,'big'), e, N).to_bytes(nlen,'big')
em_badtr = em_pss[:-1] + b'\xbb'
emit("rsa_pss_badtrailer", pow(int.from_bytes(em_badtr,'big'), d, N).to_bytes(nlen,'big'))

def hkdf_extract(hf, salt, ikm): return hmac_mod.new(salt, ikm, hf).digest()
def hkdf_expand(hf, prk, info, L):
    okm = b''; t = b''; c = 1
    while len(okm) < L:
        t = hmac_mod.new(prk, t + info + bytes([c]), hf).digest()
        okm += t; c += 1
    return okm[:L]

ikm = bytes(range(0x30)); salt = bytes(range(0x40,0x70)); info = b"logit hkdf info"
prk384 = hkdf_extract(hashlib.sha384, salt, ikm)
okm384 = hkdf_expand(hashlib.sha384, prk384, info, 100)
emit("hk384_ikm", ikm); emit("hk384_salt", salt); emit("hk384_info", info)
emit("hk384_prk", prk384); emit("hk384_okm", okm384)

hkkey = bytes(((0xa0+i)&0xff) for i in range(131))
hkmsg = b"Test Using Larger Than Block-Size Key - Hash Key First"
emit("hmac384_key", hkkey); emit("hmac384_msg", hkmsg)
emit("hmac384_out", hmac_mod.new(hkkey, hkmsg, hashlib.sha384).digest())
hmac256_key = bytes([0xaa]*131)
emit("hmac256_key", hmac256_key)
emit("hmac256_tc7_msg", b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data")
emit("hmac256_tc7_out", hmac_mod.new(hmac256_key, b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data", hashlib.sha256).digest())

def expand_label(hf, secret, label, ctx, L):
    full = b"tls13 " + label
    info = L.to_bytes(2,'big') + bytes([len(full)]) + full + bytes([len(ctx)]) + ctx
    return hkdf_expand(hf, secret, info, L)
sec256 = bytes(range(32)); ctx256 = hashlib.sha256(b"transcript").digest()
emit("el256_secret", sec256); emit("el256_ctx", ctx256)
emit("el256_out", expand_label(hashlib.sha256, sec256, b"c hs traffic", ctx256, 32))
sec384 = bytes(range(48)); ctx384 = hashlib.sha384(b"transcript").digest()
emit("el384_secret", sec384); emit("el384_ctx", ctx384)
emit("el384_out", expand_label(hashlib.sha384, sec384, b"s ap traffic", ctx384, 48))
emit("el384_keyout", expand_label(hashlib.sha384, sec384, b"key", b"", 32))
emit("el256_ivout", expand_label(hashlib.sha256, sec256, b"iv", b"", 12))

# ---- HKDF-SHA-512 (hlen=64) -------------------------------------------------
# RFC 5869 has no SHA-512 appendix cases (only SHA-1/SHA-256), so these are
# self-computed against hashlib/hmac -- the same oracle, one width wider.
# L=136 > 2*64 exercises a third expand block.
ikm = bytes(range(0x00,0x50)); salt = bytes(range(0x60,0x80))
info = b"logit hkdf512 info"
prk512 = hkdf_extract(hashlib.sha512, salt, ikm)
okm512 = hkdf_expand(hashlib.sha512, prk512, info, 136)
emit("hk512_ikm", ikm); emit("hk512_salt", salt); emit("hk512_info", info)
emit("hk512_prk", prk512); emit("hk512_okm", okm512)
# null salt == 64 zero bytes (the TLS 1.3 early-secret pattern at width 64);
# written explicitly because hmac.new(None, ...) is a TypeError in python
emit("hk512_prk0", hkdf_extract(hashlib.sha512, b"\x00"*64, ikm))

# ---- PBKDF2-HMAC-SHA-512 (hlen=64) ------------------------------------------
# hashlib.pbkdf2_hmac is the OpenSSL PRF; the 1/2-iteration cases pin the
# loop logic cheaply, 4096 exercises the chain, and the long password/salt
# case crosses the HMAC block-size boundary in both operands.
pdf2_512 = [
    ("pb512a", b"password", b"salt", 1, 64),
    ("pb512b", b"password", b"salt", 2, 64),
    ("pb512c", b"password", b"salt", 4096, 64),
    ("pb512d", bytes([0x03]*64), bytes([0x07]*64), 5, 64),
    ("pb512e", bytes(range(0x40,0xc0)), bytes(range(0xc0,0x100)), 100, 48),
]
for tag, pw, slt, it, dklen in pdf2_512:
    emit(tag + "_pw", pw); emit(tag + "_salt", slt)
    out.append('static const uint32_t %s_iters = %du;' % (tag, it))
    emit(tag + "_dk", hashlib.pbkdf2_hmac("sha512", pw, slt, it, dklen))
# one SHA-384 and one SHA-256 case so the vec battery covers every hlen
emit("pb384a_pw", b"password"); emit("pb384a_salt", b"salt")
out.append('static const uint32_t pb384a_iters = 4096u;')
emit("pb384a_dk", hashlib.pbkdf2_hmac("sha384", b"password", b"salt", 4096, 48))
emit("pb224a_pw", b"password"); emit("pb224a_salt", b"salt")
out.append('static const uint32_t pb224a_iters = 4096u;')
emit("pb224a_dk", hashlib.pbkdf2_hmac("sha224", b"password", b"salt", 4096, 28))
emit("pb256a_pw", b"password"); emit("pb256a_salt", b"salt")
out.append('static const uint32_t pb256a_iters = 4096u;')
emit("pb256a_dk", hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 4096, 32))

import os
path = os.environ.get('VECOUT', 'vectors.h')
with open(path,'w') as f:
    f.write("/* generated by gen.py - reference vectors */\n")
    f.write('\n'.join(out))
    f.write('\n')
for cvid, hl, tag in ec_cases:
    print('ecdsa case: curve=%d hlen=%d tag=%s' % (cvid, hl, tag))
print("OK vectors written to", path)
