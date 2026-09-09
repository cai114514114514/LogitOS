#!/usr/bin/env python3
"""A hostile SSH-2 client, written from the RFCs with Python's `cryptography`
and hashlib -- ZERO code shared with c/net/ssh or sshd.c (the same
independence discipline as tests/unit/ssh_kex_gen.py: agreement between this
client and that server is evidence about the spec, not about one
implementation agreeing with itself).

It speaks enough of the protocol to get anywhere an attacker can get --
version exchange, KEXINIT, curve25519-sha256 KEX, NEWKEYS, aes128-ctr +
hmac-sha2-256 packets, SERVICE_REQUEST, password and publickey userauth,
session channels -- and then DEVIATES wherever the battery below wants to:
malformed name-lists, low-order points, wrong message order, keys derived
from the wrong session id, replayed cross-session signatures, double EOF,
window overruns, a second channel open, slot exhaustion.

Every attack names the behavior it REQUIRES of the server; checks marked
"RED until fix" are pinned-desired behavior for known defects (their pre-fix
failures are the finding, not a flake). After every attack a clean control
login must still succeed -- a server that survives an attack by dying is
not surviving it.

Usage: ssh_attack_client.py HOST PORT --user U --pw P [--pubkey keyfile]
Exit 0 iff every attack and every control passed.
"""
import argparse
import hashlib
import hmac as hmac_mod
import os
import socket
import sys
import time

from cryptography.hazmat.primitives.asymmetric import x25519, ed25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import serialization as ser

# ---- wire primitives (RFC 4251 5) -----------------------------------------
def u32(n):
    return n.to_bytes(4, "big")


def sshstring(b):
    return u32(len(b)) + b


def sshmpint(raw: bytes) -> bytes:
    raw = raw.lstrip(b"\x00")
    if raw and raw[0] & 0x80:
        raw = b"\x00" + raw
    return sshstring(raw)


def r_string(buf, off):
    n = int.from_bytes(buf[off:off + 4], "big")
    off += 4
    return buf[off:off + n], off + n


DISCONNECT, IGNORE, UNIMPLEMENTED, DEBUG = 1, 2, 3, 4
SERVICE_REQUEST, SERVICE_ACCEPT = 5, 6
KEXINIT, NEWKEYS, KEX_ECDH_INIT, KEX_ECDH_REPLY = 20, 21, 30, 31
USERAUTH_REQUEST, USERAUTH_FAILURE, USERAUTH_SUCCESS = 50, 51, 52
PK_OK = 60
CHANNEL_OPEN, OPEN_CONFIRM, OPEN_FAILURE = 90, 91, 92
WINDOW_ADJUST, CHANNEL_DATA, EXTENDED_DATA = 93, 94, 95
CHANNEL_EOF, CHANNEL_CLOSE, CHANNEL_REQUEST, CH_SUCCESS, CH_FAILURE = 96, 97, 98, 99, 100

DISC_KEY_EXCHANGE_FAILED = 3
DISC_AUTH_CANCELLED = 13

# The canonical low-order encodings (computed independently by
# tests/unit/ssh_attack_kex_gen.py; quoted here for the wire-level battery
# because this file cannot import that generator without coupling the two).
LOW_ORDER_POINTS = [
    bytes(32),
    b"\x01" + bytes(31),
    bytes.fromhex("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800"),
    bytes.fromhex("5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157"),
]


class Conn:
    """One TCP connection speaking SSH-2, happy path and hostile deviations."""

    def __init__(self, host, port, timeout=8.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.timeout = timeout
        self.buf = b""
        self.seq_c2s = 0
        self.seq_s2c = 0
        self.cipher_on = False
        self.ctr_c2s = None       # int, whole-128-bit running counter
        self.ctr_s2c = None
        self.enc_c2s = self.enc_s2c = None
        self.mac_c2s = self.mac_s2c = None
        self.v_c = b"SSH-2.0-AttackClient_1.0"
        self.v_s = None
        self.i_c = self.i_s = None
        self.session_id = None

    # --- raw socket ---------------------------------------------------------
    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_raw(self, b):
        self.sock.sendall(b)

    def recv_raw(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("server closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    # --- version exchange ----------------------------------------------------
    def expect_version(self):
        line = b""
        while not line.endswith(b"\n"):
            c = self.recv_raw(1)
            line += c
            if len(line) > 255:
                raise ValueError("server version line overran 255 bytes")
        self.v_s = line.rstrip(b"\r\n")
        return self.v_s

    def send_version(self):
        self.send_raw(self.v_c + b"\r\n")

    # --- packet layer (RFC 4253 6, RFC 4344) --------------------------------
    def _ctr_bytes(self, ctr):
        return ctr.to_bytes(16, "big")

    def send_packet(self, payload, pad_to_block=None, force_plain=False, mac_key=None):
        """Normally frames per RFC 4253; `force_plain`/`mac_key` let attacks
        send deliberately-wrong MACs. `pad_to_block` overrides padding
        discipline for framing attacks."""
        block = 16 if self.cipher_on else 8
        if pad_to_block:
            block = pad_to_block
        pad = block - ((5 + len(payload)) % block)
        if pad < 4:
            pad += block
        total = 4 + 1 + len(payload) + pad
        while total < 16:
            pad += block
            total += block
        pkt = u32(1 + len(payload) + pad) + bytes([pad]) + payload + os.urandom(pad)

        mk = mac_key if mac_key is not None else self.mac_c2s
        if self.cipher_on or mac_key is not None:
            mac = hmac_mod.new(mk, u32(self.seq_c2s) + pkt, hashlib.sha256).digest()
        else:
            mac = b""

        if self.cipher_on and not force_plain:
            enc = Cipher(algorithms.AES(self.enc_c2s), modes.CTR(self._ctr_bytes(self.ctr_c2s))).encryptor()
            wire_pkt = enc.update(pkt) + enc.finalize()
            self.ctr_c2s += (len(pkt) + 15) // 16
        else:
            wire_pkt = pkt
        self.send_raw(wire_pkt + mac)
        self.seq_c2s += 1

    def recv_packet(self):
        block = 16 if self.cipher_on else 8
        first = bytearray(self.recv_raw(block))
        if self.cipher_on:
            dec = Cipher(algorithms.AES(self.enc_s2c), modes.CTR(self._ctr_bytes(self.ctr_s2c))).decryptor()
            first = bytearray(dec.update(bytes(first)) + dec.finalize())
        packet_length = int.from_bytes(first[0:4], "big")
        total = 4 + packet_length
        rest = b""
        if total > block:
            need = total - block
            rest = bytearray(self.recv_raw(need))
            if self.cipher_on:
                dec2 = Cipher(algorithms.AES(self.enc_s2c), modes.CTR(self._ctr_bytes(self.ctr_s2c + 1))).decryptor()
                rest = bytearray(dec2.update(bytes(rest)) + dec2.finalize())
        pkt = bytes(first) + bytes(rest)
        if self.cipher_on:
            self.ctr_s2c += (total + 15) // 16
            mac = self.recv_raw(32)
            calc = hmac_mod.new(self.mac_s2c, u32(self.seq_s2c) + pkt, hashlib.sha256).digest()
            if not hmac_mod.compare_digest(calc, mac):
                raise ValueError("server MAC mismatch -- the server misbehaved")
        self.seq_s2c += 1
        padlen = pkt[4]
        return pkt[5:5 + packet_length - 1 - padlen]

    def send_ignore(self, data=b"x"):
        self.send_packet(bytes([IGNORE]) + sshstring(data))

    # --- kex -------------------------------------------------------------------
    def build_kexinit(self, kex=b"curve25519-sha256", hostkey=b"ssh-ed25519",
                      cipher=b"aes128-ctr", mac=b"hmac-sha2-256"):
        p = bytes([KEXINIT]) + os.urandom(16)
        for lst in (kex, hostkey, cipher, cipher, mac, mac, b"none", b"none", b"", b""):
            p += sshstring(lst)
        p += b"\x00" + u32(0)
        return p

    def send_kexinit(self, *a, **kw):
        self.i_c = self.build_kexinit(*a, **kw)
        self.send_packet(self.i_c)

    def recv_kexinit(self):
        p = self.recv_packet()
        if not p or p[0] != KEXINIT:
            raise ValueError(f"expected KEXINIT, got {p[0] if p else 'empty'}")
        self.i_s = p
        return p

    def kex(self, derive_from_session_id=None, qc_override=None):
        """Full honest KEX. `derive_from_session_id` derives the c2s keys from
        a DIFFERENT session id than the real one (attack A-wrongkeys);
        `qc_override` replaces the client public key bytes on the wire."""
        self.send_kexinit()
        self.recv_kexinit()

        priv = x25519.X25519PrivateKey.generate()
        qc = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        if qc_override is not None:
            qc = qc_override
        self.send_packet(bytes([KEX_ECDH_INIT]) + sshstring(qc))

        reply = self.recv_packet()
        if not reply or reply[0] != KEX_ECDH_REPLY:
            raise ValueError(f"expected KEX_ECDH_REPLY, got {reply[0] if reply else 'empty'}")
        ks, off = r_string(reply, 1)
        qs, off = r_string(reply, off)
        sig_blob, off = r_string(reply, off)
        if len(qs) != 32:
            raise ValueError("server Q_S is not 32 bytes")

        shared = priv.exchange(x25519.X25519PublicKey.from_public_bytes(qs))

        h = hashlib.sha256()
        h.update(sshstring(self.v_c))
        h.update(sshstring(self.v_s))
        h.update(sshstring(self.i_c))
        h.update(sshstring(self.i_s))
        h.update(sshstring(ks))
        h.update(sshstring(qc))
        h.update(sshstring(qs))
        h.update(sshmpint(shared))
        H = h.digest()
        self.session_id = H

        def kdf(letter):
            d = hashlib.sha256()
            d.update(sshmpint(shared))
            d.update(H)
            d.update(letter)
            d.update(derive_from_session_id if derive_from_session_id is not None else H)
            return d.digest()

        self.ctr_c2s = int.from_bytes(kdf(b"A")[:16], "big")
        self.ctr_s2c = int.from_bytes(kdf(b"B")[:16], "big")
        self.enc_c2s = kdf(b"C")[:16]
        self.enc_s2c = kdf(b"D")[:16]
        self.mac_c2s = kdf(b"E")
        self.mac_s2c = kdf(b"F")
        return ks, qs, sig_blob, H

    def newkeys(self):
        self.send_packet(bytes([NEWKEYS]))
        p = self.recv_packet()
        if not p or p[0] != NEWKEYS:
            raise ValueError(f"expected NEWKEYS, got {p[0] if p else 'empty'}")
        # switch AFTER both NEWKEYS crossed; the first encrypted packet the
        # server sends is SERVICE_ACCEPT or USERAUTH_*, so flip now and let
        # recv_packet use the s2c state
        self.cipher_on = True

    # --- userauth / channel (happy paths the attacks deviate from) -----------
    def service_request(self):
        self.send_packet(bytes([SERVICE_REQUEST]) + sshstring(b"ssh-userauth"))
        p = self.recv_packet()
        if not p or p[0] != SERVICE_ACCEPT:
            raise ValueError(f"expected SERVICE_ACCEPT, got {p[0] if p else 'empty'}")

    def userauth_password(self, user, pw):
        p = (bytes([USERAUTH_REQUEST]) + sshstring(user.encode()) + sshstring(b"ssh-connection")
             + sshstring(b"password") + b"\x00" + sshstring(pw.encode()))
        self.send_packet(p)
        r = self.recv_packet()
        return r[0] if r else -1

    def login(self, user, pw):
        self.send_version()
        self.expect_version()
        self.kex()
        self.newkeys()
        self.service_request()
        rc = self.userauth_password(user, pw)
        if rc != USERAUTH_SUCCESS:
            raise ValueError(f"control login refused (msg {rc})")

    def open_session(self):
        self.send_packet(bytes([CHANNEL_OPEN]) + sshstring(b"session") + u32(1) + u32(0) + u32(32768))
        p = self.recv_packet()
        if not p or p[0] != OPEN_CONFIRM:
            raise ValueError(f"expected OPEN_CONFIRMATION, got {p[0] if p else 'empty'}")
        return p

    def request_shell(self):
        self.send_packet(bytes([CHANNEL_REQUEST]) + u32(0) + sshstring(b"shell") + b"\x01")
        p = self.recv_packet()
        if not p or p[0] != CH_SUCCESS:
            raise ValueError(f"expected CHANNEL_SUCCESS, got {p[0] if p else 'empty'}")


# ---- battery -----------------------------------------------------------------
PASS, FAIL = "PASS", "FAIL"
results = []
ONLY = None  # set from --only: comma-separated attack-name prefixes


def record(name, ok, note=""):
    if ONLY and ONLY != name and not any(name.startswith(o) for o in ONLY.split(",")):
        return
    results.append((name, ok, note))
    print(f"ATTACK-RESULT {name} {PASS if ok else FAIL} {note}", flush=True)


class Closed(Exception):
    """The server dropped us -- often exactly what an attack wants."""


def dropped_within(conn, seconds=3.0):
    """True if the server closes (FIN/RST) or errors within `seconds`."""
    conn.sock.settimeout(seconds)
    try:
        while True:
            chunk = conn.sock.recv(4096)
            if not chunk:
                return True
            conn.buf += chunk   # may be a DISCONNECT; caller can inspect conn.buf
            if conn.buf.startswith(b"\x01"):
                return True     # SSH_MSG_DISCONNECT in the clear is also a close
    except (socket.timeout, TimeoutError):
        return False
    except (ConnectionResetError, EOFError, OSError):
        return True


def control_login(host, port, user, pw):
    c = Conn(host, port)
    try:
        c.login(user, pw)
        return True
    except Exception as e:
        print(f"CONTROL-LOGIN-FAILED: {type(e).__name__}: {e}", flush=True)
        return False
    finally:
        c.close()


def run_battery(host, port, user, pw, privkey_path):
    conns = []

    def fresh():
        c = Conn(host, port)
        conns.append(c)
        return c

    # ---- pre-KEX -----------------------------------------------------------
    c = fresh()
    c.expect_version()
    c.send_raw(b"A" * 300)  # no newline, over the RFC 4253 4.2 line bound
    record("version_oversize", dropped_within(c), "overlong version line must be dropped")
    c.close()

    c = fresh()
    c.expect_version()
    c.send_raw(b"XYZ-2.0-notssh\r\n")
    record("version_bad_prefix", dropped_within(c), "non-SSH- prefix must be dropped")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.recv_kexinit()               # the server's OWN KEXINIT arrives first --
    c.send_kexinit(kex=b"")        # empty kex name-list: nothing to negotiate
    p = c.recv_packet()
    got_disc = bool(p) and p[0] == DISCONNECT
    record("kexinit_empty_kex_list", got_disc,
           f"clean DISCONNECT for empty kex list (got msg {p[0] if p else 'none'}) -- "
           f"the close that follows a DISCONNECT is the server being correct")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.recv_kexinit()
    c.send_kexinit(kex=b"diffie-hellman-group14-sha256")
    p = c.recv_packet()
    got_disc = bool(p) and p[0] == DISCONNECT
    record("kexinit_no_common_algo", got_disc, f"clean DISCONNECT for no common kex (got msg {p[0] if p else 'none'})")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.i_c = c.build_kexinit()
    c.send_packet(c.i_c[: len(c.i_c) - 40])  # truncated mid-name-list
    record("kexinit_truncated", dropped_within(c), "truncated KEXINIT must be dropped")
    c.close()

    for i, low in enumerate(LOW_ORDER_POINTS):
        c = fresh()
        c.send_version()
        c.expect_version()
        c.send_kexinit()
        c.recv_kexinit()
        c.send_packet(bytes([KEX_ECDH_INIT]) + sshstring(low))
        record(f"ecdh_low_order_{i}", dropped_within(c, 3.0),
               "low-order point must never produce a KEXREPLY")
        c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.send_packet(bytes([KEX_ECDH_INIT]) + sshstring(bytes(31)))  # wrong length
    c.send_kexinit()
    record("ecdh_wrong_len", dropped_within(c, 3.0),
           "31-byte qc must be refused before any kex math")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    # USERAUTH in the clear, before any KEXINIT exists
    c.send_packet(bytes([USERAUTH_REQUEST]) + sshstring(b"root") + sshstring(b"ssh-connection")
                  + sshstring(b"none"))
    record("order_userauth_before_kex", dropped_within(c, 3.0),
           "cleartext USERAUTH_REQUEST before KEX must be dropped")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.send_kexinit()
    c.recv_kexinit()
    c.send_packet(bytes([NEWKEYS]))  # NEWKEYS where ECDH_INIT belongs
    record("order_newkeys_early", dropped_within(c, 3.0),
           "NEWKEYS before any ECDH must be dropped")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    # plaintext packet whose padding_length is 2 (< 4): u32(3) 02 'X' pad...
    # total 4+1+1+2 = 8, multiple of the plain block size
    c.send_raw(u32(3) + bytes([2]) + b"\x1e" + b"\x00\x00")
    record("packet_bad_padding", dropped_within(c, 3.0),
           "padding_length < 4 must be a format error")
    c.close()

    # ---- post-KEX ----------------------------------------------------------
    c = fresh()
    c.send_version()
    c.expect_version()
    c.kex(derive_from_session_id=bytes(32))  # keys from the WRONG session id
    c.newkeys()  # switches both directions per the honest flow; c2s keys are wrong
    # The server's s2c NEWKEYS echo was read under... the honest flow reads it
    # with the REAL s2c keys; then OUR next packet uses wrong c2s keys.
    c.send_packet(bytes([SERVICE_REQUEST]) + sshstring(b"ssh-userauth"),
                  mac_key=c.mac_c2s)
    record("packet_wrong_keys", dropped_within(c, 3.0),
           "packet under wrong-session-id keys must die on MAC, never process")
    c.close()

    c = fresh()
    c.send_version()
    c.expect_version()
    c.kex()
    c.newkeys()
    c.send_packet(bytes([KEXINIT]))  # a rekey ask, immediately
    p = c.recv_packet()
    okk = bool(p) and p[0] == DISCONNECT
    record("rekey_refused", okk and dropped_within(c, 1.0),
           f"KEXINIT after NEWKEYS must earn an honest DISCONNECT (got msg {p[0] if p else 'none'})")
    c.close()

    # MaxAuthTries: 6 wrong passwords then the connection must be cut
    c = fresh()
    c.send_version()
    c.expect_version()
    c.kex()
    c.newkeys()
    c.service_request()
    cut = False
    last_rc = None
    for i in range(6):
        try:
            last_rc = c.userauth_password(user, "definitely-wrong-" + str(i))
        except (EOFError, ConnectionResetError, OSError, ValueError):
            cut = True
            break
        if last_rc != USERAUTH_FAILURE:
            break
    record("auth_max_tries_disconnect", cut or dropped_within(c, 2.0),
           f"6 failures must DISCONNECT (last reply msg {last_rc})")
    c.close()

    # cross-session signature replay: sign in conn1, replay the very bytes in conn2
    if privkey_path:
        priv = ser.load_ssh_private_key(open(privkey_path, "rb").read(), password=None)
        sig_capture = {}

        def pk_auth(conn, sign=True):
            pub = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
            blob = sshstring(b"ssh-ed25519") + sshstring(pub)
            base = (bytes([USERAUTH_REQUEST]) + sshstring(user.encode())
                    + sshstring(b"ssh-connection") + sshstring(b"publickey")
                    + b"\x01" + sshstring(b"ssh-ed25519") + sshstring(blob))
            if not sign:
                conn.send_packet(base + sshstring(sig_capture["sig"]))
                return conn.recv_packet()
            # probe first WITHOUT the signature: a PK_OK reply proves the
            # account+key halves passed and isolates any later refusal to
            # the signature itself -- without this the happy path cannot
            # tell a key-matching failure from a signing-format failure.
            conn.send_packet(bytes([USERAUTH_REQUEST]) + sshstring(user.encode())
                             + sshstring(b"ssh-connection") + sshstring(b"publickey")
                             + b"\x00" + sshstring(b"ssh-ed25519") + sshstring(blob))
            probe = conn.recv_packet()
            if probe[0] != PK_OK:
                print(f"PK-DEBUG: probe (no-sig) reply msg {probe[0]} -- "
                      f"{'key not authorized/account miss' if probe[0] == USERAUTH_FAILURE else 'unexpected'}",
                      flush=True)
                return probe
            signdata = (sshstring(conn.session_id) + bytes([USERAUTH_REQUEST])
                        + sshstring(user.encode()) + sshstring(b"ssh-connection")
                        + sshstring(b"publickey") + b"\x01"
                        + sshstring(b"ssh-ed25519") + sshstring(blob))
            sig = priv.sign(signdata)
            sig_capture["sig"] = sshstring(b"ssh-ed25519") + sshstring(sig)
            conn.send_packet(base + sshstring(sig_capture["sig"]))
            return conn.recv_packet()

        c = fresh()
        c.send_version()
        c.expect_version()
        c.kex()
        c.newkeys()
        c.service_request()
        rc = pk_auth(c)
        rc0 = rc[0] if rc else -1
        record("pk_happy_path", rc0 == USERAUTH_SUCCESS,
               f"control: real publickey auth works (msg {rc0})")
        c.close()

        c = fresh()
        c.send_version()
        c.expect_version()
        c.kex()
        c.newkeys()
        c.service_request()
        rc = pk_auth(c, sign=False)  # the SAME signature, different session_id
        rc0 = rc[0] if rc else -1
        record("pk_replay_cross_session", rc0 == USERAUTH_FAILURE,
               f"a signature from ANOTHER session must fail (got msg {rc0})")
        c.close()

        c = fresh()
        c.send_version()
        c.expect_version()
        c.kex()
        c.newkeys()
        c.service_request()
        pub = priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
        blob = sshstring(b"ssh-ed25519") + sshstring(pub)
        junk_sig = sshstring(b"ssh-ed25519") + sshstring(bytes(63))  # rawsiglen != 64
        req = (bytes([USERAUTH_REQUEST]) + sshstring(user.encode())
               + sshstring(b"ssh-connection") + sshstring(b"publickey")
               + b"\x01" + sshstring(b"ssh-ed25519") + sshstring(blob))
        c.send_packet(req + sshstring(junk_sig))
        rc = c.recv_packet()
        rc0 = rc[0] if rc else -1
        record("pk_malformed_signature", rc0 == USERAUTH_FAILURE,
               f"63-byte raw signature must count a failure, not crash (msg {rc0})")
        c.close()

    # channel-level: full login first
    c = fresh()
    c.send_version()
    c.expect_version()
    c.kex()
    c.newkeys()
    c.service_request()
    if c.userauth_password(user, pw) != USERAUTH_SUCCESS:
        record("channel_battery_login", False, "could not log in for channel attacks")
    else:
        c.open_session()
        c.request_shell()

        # second CHANNEL_OPEN: must earn CHANNEL_OPEN_FAILURE (RED until fix:
        # today it is silently ignored and a real client hangs forever)
        c.send_packet(bytes([CHANNEL_OPEN]) + sshstring(b"session") + u32(2) + u32(0) + u32(32768))
        try:
            p = c.recv_packet()
            second = bool(p) and p[0] == OPEN_FAILURE
            record("second_channel_open_refused", second,
                   f"second CHANNEL_OPEN must be REFUSED not ignored (got msg {p[0] if p else 'none'})")
        except (socket.timeout, TimeoutError):
            record("second_channel_open_refused", False,
                   "RED until fix: second CHANNEL_OPEN silently ignored -- a client hangs forever")
        except (EOFError, ConnectionResetError, ValueError):
            record("second_channel_open_refused", True, "connection cut after second open: acceptable refusal")

        # WINDOW_ADJUST wrap: 2 x 0xFFFFFFFF must not corrupt state
        c.send_packet(bytes([WINDOW_ADJUST]) + u32(0) + u32(0xFFFFFFFF))
        c.send_packet(bytes([WINDOW_ADJUST]) + u32(0) + u32(0xFFFFFFFF))
        c.send_packet(bytes([CHANNEL_DATA]) + u32(0) + sshstring(b"echo WRAP_OK\n"))
        record("window_adjust_wrap_survives", True, "sent; liveness checked by the control below")

        # data on a channel we never opened: must be IGNORED (RED until fix:
        # today it is relayed to the child's stdin regardless of recipient)
        c.send_packet(bytes([CHANNEL_DATA]) + u32(0xDEAD) + sshstring(b"echo WRONGCHAN_7f3a\n"))
        time.sleep(3.0)   # QEMU/TCG round-trip: 1.5 s once read as a pass
                          # while the marker was still in flight -- the wait
                          # has to cover shell-exec + pump-back, not hope
        c.sock.settimeout(1.0)
        leaked = b""
        try:
            while True:
                chunk = c.sock.recv(65536)
                if not chunk:
                    break
                c.buf += chunk
                leaked += chunk
        except (socket.timeout, TimeoutError):
            pass
        c.sock.settimeout(c.timeout)
        record("channel_data_wrong_recipient_ignored", b"WRONGCHAN_7f3a" not in c.buf,
               "data for an unknown channel must never reach the shell"
               if b"WRONGCHAN_7f3a" not in c.buf else
               "RED until fix: wrong-recipient CHANNEL_DATA was relayed to the shell")

        # double EOF: the second must not upset the server (watched by control)
        c.send_packet(bytes([CHANNEL_EOF]) + u32(0))
        time.sleep(0.3)
        c.send_packet(bytes([CHANNEL_EOF]) + u32(0))
        record("double_eof_survives", True, "sent; liveness checked by the control below")

        # oversized exec command: refused, never silently TRUNCATED and run.
        # "Refused" includes the connection being dropped: sshd treats a
        # malformed/oversized exec as a protocol failure and returns -1 from
        # the setup loop (measured: the fixed server closes the connection
        # where the pre-fix one ran the truncated command AND replied
        # CHANNEL_SUCCESS -- serial line "sshd: EXEC cmd=echo EEE..." with
        # 511 E's, the truncation visible on the serial line itself).
        c.close()
        c = fresh()
        c.send_version()
        c.expect_version()
        c.kex()
        c.newkeys()
        c.service_request()
        c.userauth_password(user, pw)
        c.open_session()
        long_cmd = b"echo " + b"E" * 600
        c.send_packet(bytes([CHANNEL_REQUEST]) + u32(0) + sshstring(b"exec") + b"\x01"
                      + sshstring(long_cmd))
        try:
            p = c.recv_packet()
            refused = (not p) or p[0] in (CH_FAILURE, DISCONNECT)
            record("exec_oversize_refused", refused,
                   f"600-byte exec must be refused, not run truncated (got msg {p[0] if p else 'EOF'})")
        except (socket.timeout, TimeoutError):
            record("exec_oversize_refused", False, "no reply to oversized exec")
        except (EOFError, ConnectionResetError, OSError):
            record("exec_oversize_refused", True, "connection dropped on oversized exec: a refusal")
        c.close()

    # ---- availability: slot exhaustion --------------------------------------
    holders = []
    try:
        for _ in range(8):
            h = Conn(host, port, timeout=5.0)
            h.expect_version()   # a version and then NOTHING: a held slot
            h.send_raw(b"SSH-2.0-Slowloris_1.0\r\n")
            holders.append(h)
        time.sleep(0.5)
        ninth = Conn(host, port, timeout=5.0)
        try:
            v = ninth.expect_version()
            # slot refusal happens at accept: server closes immediately
            ninth.sock.settimeout(2.0)
            closed = dropped_within(ninth, 2.0)
            record("slots_refuse_ninth", closed, "at capacity the 9th connection is refused, not queued")
        except (EOFError, ConnectionResetError, OSError) as e:
            record("slots_refuse_ninth", True, f"9th connection refused at accept ({type(e).__name__})")
        finally:
            ninth.close()
    finally:
        for h in holders:
            h.close()

    # the survival control after every attack, plus the held slots released.
    # The settle delay is load-bearing: the eight holders' sockets were just
    # closed and the guest needs a moment to notice each FIN and run its
    # cleanup before a fresh login can get a slot (the first pre-fix run
    # failed this control on a capacity EOF that was gone three seconds
    # later).
    time.sleep(3)
    ok = control_login(host, port, user, pw)
    record("server_survives_battery", ok, "a clean login still succeeds after everything above")

    for c in conns:
        c.close()
    return all(r[1] for r in results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--user", required=True)
    ap.add_argument("--pw", required=True)
    ap.add_argument("--pubkey", default=None, help="OpenSSH-format private key for the publickey attacks")
    ap.add_argument("--only", default=None, help="comma-separated attack-name prefixes to run (debug)")
    args = ap.parse_args()
    global ONLY
    ONLY = args.only

    ok = run_battery(args.host, args.port, args.user, args.pw, args.pubkey)
    n_fail = sum(1 for r in results if not r[1])
    print(f"BATTERY: {len(results) - n_fail}/{len(results)} passed", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
