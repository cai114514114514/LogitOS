#!/usr/bin/env python3
"""Independent oracle for the curve25519-sha256 kex + RFC 4253 7.2 KDF.

Written from the RFC text (4253 sections 6 [string/mpint], 7.2 [KDF], 8
[exchange hash] and 8731 [curve25519-sha256]) using Python's `cryptography`
library for X25519/Ed25519 and hashlib for SHA-256 -- NONE of c/net/ssh's
code is imported or reused here. That independence is the whole point: this
script and ssh_kex.c were built from the same public spec by two different
paths, so agreement between them is evidence about the spec, not about one
implementation agreeing with itself.

Usage: ssh_kex_gen.py <out-vectors-file>
"""
import sys, hashlib
from cryptography.hazmat.primitives.asymmetric import x25519, ed25519
from cryptography.hazmat.primitives import serialization as ser

def u32(n):
    return n.to_bytes(4, "big")

def sshstring(b):
    return u32(len(b)) + b

def sshmpint(raw: bytes):
    raw = raw.lstrip(b"\x00")
    if raw and raw[0] & 0x80:
        raw = b"\x00" + raw
    return sshstring(raw)

def x25519_raw_priv(b32: bytes):
    return x25519.X25519PrivateKey.from_private_bytes(b32)

def x25519_pub_bytes(pk):
    return pk.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)

def main():
    out_path = sys.argv[1]

    # Fixed, arbitrary-but-deterministic inputs -- every byte below is chosen
    # by this script, not read from anywhere, so the vector is reproducible.
    V_C = b"SSH-2.0-OpenSSH_9.9"
    V_S = b"SSH-2.0-LogitOS_1.0"
    # Synthetic but well-formed KEXINIT payloads (msg type 20 + 16-byte
    # cookie + a namelist) -- their CONTENT does not need to be a real
    # negotiation for this vector; the hash just needs SOME I_C/I_S bytes,
    # exactly as it does on the wire.
    I_C = bytes([20]) + bytes(range(16)) + sshstring(b"curve25519-sha256")
    I_S = bytes([21]) + bytes(range(16, 32)) + sshstring(b"ssh-ed25519")

    host_seed = bytes(range(32))
    host_priv = ed25519.Ed25519PrivateKey.from_private_bytes(host_seed)
    host_pub = host_priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)

    client_priv_b = bytes([(i * 7 + 3) & 0xFF for i in range(32)])
    server_priv_b = bytes([(i * 13 + 5) & 0xFF for i in range(32)])
    client_priv = x25519_raw_priv(client_priv_b)
    server_priv = x25519_raw_priv(server_priv_b)
    Q_C = x25519_pub_bytes(client_priv)
    Q_S = x25519_pub_bytes(server_priv)

    K = client_priv.exchange(x25519.X25519PublicKey.from_public_bytes(Q_S))
    K2 = server_priv.exchange(x25519.X25519PublicKey.from_public_bytes(Q_C))
    assert K == K2, "ECDH did not agree with itself"

    K_S = sshstring(b"ssh-ed25519") + sshstring(host_pub)

    h = hashlib.sha256()
    h.update(sshstring(V_C))
    h.update(sshstring(V_S))
    h.update(sshstring(I_C))
    h.update(sshstring(I_S))
    h.update(sshstring(K_S))
    h.update(sshstring(Q_C))
    h.update(sshstring(Q_S))
    h.update(sshmpint(K))
    H = h.digest()

    sig = host_priv.sign(H)  # raw Ed25519 signature over H itself, not a re-hash

    session_id = H  # first kex

    def kdf(letter: bytes):
        d = hashlib.sha256()
        d.update(sshmpint(K))
        d.update(H)
        d.update(letter)
        d.update(session_id)
        return d.digest()

    iv_c2s = kdf(b"A")[:16]
    iv_s2c = kdf(b"B")[:16]
    enc_c2s = kdf(b"C")[:16]
    enc_s2c = kdf(b"D")[:16]
    mac_c2s = kdf(b"E")
    mac_s2c = kdf(b"F")

    def hx(b):
        return b.hex()

    with open(out_path, "w") as f:
        f.write(f"VC {hx(V_C)}\n")
        f.write(f"VS {hx(V_S)}\n")
        f.write(f"IC {hx(I_C)}\n")
        f.write(f"IS {hx(I_S)}\n")
        f.write(f"HOST_SEED {hx(host_seed)}\n")
        f.write(f"HOST_PUB {hx(host_pub)}\n")
        f.write(f"CLIENT_PRIV {hx(client_priv_b)}\n")
        f.write(f"CLIENT_PUB {hx(Q_C)}\n")
        f.write(f"SERVER_PRIV {hx(server_priv_b)}\n")
        f.write(f"SERVER_PUB {hx(Q_S)}\n")
        f.write(f"K {hx(K)}\n")
        f.write(f"H {hx(H)}\n")
        f.write(f"SIG {hx(sig)}\n")
        f.write(f"IV_C2S {hx(iv_c2s)}\n")
        f.write(f"IV_S2C {hx(iv_s2c)}\n")
        f.write(f"ENC_C2S {hx(enc_c2s)}\n")
        f.write(f"ENC_S2C {hx(enc_s2c)}\n")
        f.write(f"MAC_C2S {hx(mac_c2s)}\n")
        f.write(f"MAC_S2C {hx(mac_s2c)}\n")
    print(f"wrote {out_path}")

if __name__ == "__main__":
    main()
