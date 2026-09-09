#!/usr/bin/env python3
"""An on-path attacker for the SSH boot test -- the ssh analogue of
tests/unit/tls12_tamper_proxy.py. Sits between a REAL OpenSSH client and the
guest's sshd, and exercises exactly the powers a network attacker has over
the unauthenticated part of an SSH-2 handshake.

CHARACTERIZATION, not enforcement: LogitOS deliberately refuses to offer
kex-strict-s-v00@openssh.com (c/net/ssh/ssh.h's own argument: half-
implementing strict kex is worse than not offering it, because the client
then believes the stricter contract is in force). These probes pin what that
decision costs, honestly, on the wire:

  kexreply-flip   flip one byte of the server's KEX_ECDH_REPLY (cleartext).
                  The exchange hash covers every byte of it and the host key
                  signs the hash, so the client MUST die at verification.
                  If login succeeds, host-key verification is broken.

  inject-ignore   inject a cleartext SSH_MSG_IGNORE between the server's
                  KEX_ECDH_REPLY and its NEWKEYS. First believed to pin a
                  SURVIVING login (non-strict tolerance); measured, it pins
                  a DEAD one instead (correction kept beside the belief):
                  the injected packet leaves the client's s2c sequence
                  count one ahead, and with no strict-kex reset the FIRST
                  post-NEWKEYS MAC fails -- fail-closed. The exposure the
                  missing strict-kex leaves on THIS server is handshake
                  availability only: one kex, one cipher, one MAC, nothing
                  an attacker could downgrade even with the counters
                  agreeing.

  drop-newkeys    swallow the client's NEWKEYS. The server blocks waiting
                  for a packet that will never come (pre-idle-timeout: for
                  ever; one slot held). Login must fail; the server must
                  survive for the next connection.

  flip-first-enc  flip one byte of the server's first post-NEWKEYS packet
                  (now under aes128-ctr + hmac-sha2-256). The client MUST
                  die on the MAC. If login succeeds, the MAC is decorative.

Usage: ssh_tamper_proxy.py <listen-port> <server-port> <mode>
"""
import socket
import sys
import threading


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("closed")
        buf += chunk
    return buf


def read_version(sock):
    """The RFC 4253 4.2 identification line (server's or client's)."""
    line = b""
    while not line.endswith(b"\n"):
        c = read_exact(sock, 1)
        line += c
    return line


def s2c_pump(client, server, mode, done):
    """Server->client. The server's plaintext flight is deterministic:
    version, KEXINIT, KEX_ECDH_REPLY, NEWKEYS -- then encryption, whose first
    packet we can still WALK (packet_length is inside the ciphertext, so after
    NEWKEYS we only ever split on the first block and forward the rest raw)."""
    def note(what):
        print(f"proxy-s2c: {what}", file=sys.stderr, flush=True)
    try:
        v = read_version(server)
        note(f"got server version {v!r}")
        client.sendall(v)                           # version line

        def plaintext_packet(i):
            """Read packet #i (0-based) from the server, return its bytes."""
            hdr = read_exact(server, 4)
            ln = int.from_bytes(hdr, "big")
            note(f"server packet #{i}: length {ln}")
            return hdr + read_exact(server, ln)

        p_kexinit = plaintext_packet(0)
        client.sendall(p_kexinit)      # FORWARD AS READ: holding the server's
                                       # KEXINIT until the KEXREPLY also
                                       # arrived deadlocked the whole triad
                                       # (client waits for KEXINIT, server
                                       # waits for the client's ECDH_INIT,
                                       # proxy waits for a KEXREPLY that
                                       # needs both) -- and every "dies"
                                       # outcome below then passed for the
                                       # DEADLOCK's reason, not the tamper's.
        p_reply = plaintext_packet(1)

        if mode == "kexreply-flip":
            b = bytearray(p_reply)
            b[10] ^= 0x20            # inside K_S / hash-covered territory
            p_reply = bytes(b)

        if mode == "inject-ignore":
            # a valid minimal plaintext SSH_MSG_IGNORE. packet_length counts
            # 1+payload+pad (RFC 4253 6) -- writing 4+1+payload+pad instead
            # made the client swallow the first 4 bytes of the real NEWKEYS
            # and die of framing corruption, which read as "the injection
            # kills the login" when it was only the injector being wrong.
            payload = bytes([2]) + (4).to_bytes(4, "big") + b"junk"
            pad = 8 - ((5 + len(payload)) % 8)
            if pad < 4:
                pad += 8
            ln = 1 + len(payload) + pad
            pkt = ln.to_bytes(4, "big") + bytes([pad]) + payload + bytes(pad)
            client.sendall(pkt)
        client.sendall(p_reply)
        client.sendall(plaintext_packet(2))              # NEWKEYS

        if mode == "flip-first-enc":
            first_block = read_exact(server, 16)         # encrypted length+start
            b = bytearray(first_block)
            b[3] ^= 0x01
            client.sendall(bytes(b))

        # everything else: raw both directions of this pump
        while True:
            chunk = server.recv(65536)
            if not chunk:
                break
            client.sendall(chunk)
    except (OSError, EOFError):
        pass
    finally:
        done.set()
        try:
            client.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def c2s_pump(client, server, mode, done):
    """Client->server. Plaintext flight: version, KEXINIT, KEX_ECDH_INIT,
    NEWKEYS. `drop-newkeys` swallows the 4th packet (the client's NEWKEYS) --
    the server then waits forever for keys that never arrive."""
    try:
        server.sendall(read_version(client))

        for i in range(4):
            hdr = read_exact(client, 4)
            ln = int.from_bytes(hdr, "big")
            pkt = hdr + read_exact(client, ln)
            if mode == "drop-newkeys" and i == 3:
                continue                                  # swallowed on purpose
            server.sendall(pkt)

        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            server.sendall(chunk)
    except (OSError, EOFError):
        pass
    finally:
        done.set()
        try:
            server.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def main():
    listen_port, server_port, mode = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    assert mode in ("kexreply-flip", "inject-ignore", "drop-newkeys", "flip-first-enc")

    front = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    front.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    front.bind(("127.0.0.1", listen_port))
    front.listen(4)
    print(f"tamper-proxy {mode} listening on {listen_port} -> {server_port}",
          file=sys.stderr, flush=True)

    while True:
        client, _ = front.accept()
        server = socket.create_connection(("127.0.0.1", server_port), timeout=8)
        done = threading.Event()
        threading.Thread(target=s2c_pump, args=(client, server, mode, done), daemon=True).start()
        threading.Thread(target=c2s_pump, args=(client, server, mode, done), daemon=True).start()


if __name__ == "__main__":
    main()
