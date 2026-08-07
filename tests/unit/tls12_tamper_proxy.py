#!/usr/bin/env python3
"""An on-path attacker for the TLS 1.2 interop test.

Sits between our client and `openssl s_server`, and corrupts exactly one byte of
the server's first flight before forwarding it. The point is that in TLS 1.2 the
whole first flight -- Certificate, ServerKeyExchange, ServerHelloDone -- travels
in the CLEAR, so this is not a contrived fault injection: it is precisely what a
network attacker can do, and the ServerKeyExchange signature is the only thing
in the protocol that notices.

Two modes, because they fail for different reasons and both must fail:

  sig     flip a byte of the ServerKeyExchange SIGNATURE. Everything else is
          untouched and genuine. A client that does not verify the signature at
          all completes the handshake happily.

  pubkey  flip a byte of the server's ECDHE PUBLIC KEY. This is the real attack
          shape -- substitute the ephemeral key so you can agree a secret the
          server never chose -- and it is caught because the signature is over
          the parameters, not just over the randoms.

  pointlen  set the ECDHE point's length byte to 0xFF, which claims 255 bytes
          inside a message that does not have them. This one is not about the
          signature at all: it is the bounds check, and the interop client is
          built with ASan, so a parser that trusted the length would abort here
          instead of failing cleanly.

Usage: tls12_tamper_proxy.py <listen-port> <server-port> <sig|pubkey|pointlen>
"""
import socket, struct, sys, threading

HS_SERVER_KX = 12
HS_SERVER_DONE = 14


def parse_records(buf):
    """Split a byte string into whole TLS records. Returns (records, leftover)."""
    out, i = [], 0
    while len(buf) - i >= 5:
        ln = struct.unpack('>H', buf[i + 3:i + 5])[0]
        if len(buf) - i < 5 + ln:
            break
        out.append(buf[i:i + 5 + ln])
        i += 5 + ln
    return out, buf[i:]


def tamper(flight, mode):
    """flight is the concatenated handshake-message stream. Flip one byte and
    return the new stream (same length, so record framing is untouched)."""
    b = bytearray(flight)
    i = 0
    while i + 4 <= len(b):
        mt = b[i]
        ml = int.from_bytes(b[i + 1:i + 4], 'big')
        body = i + 4
        if i + 4 + ml > len(b):
            break
        if mt == HS_SERVER_KX:
            # ECParameters: curve_type(1) namedcurve(2) then point len(1)||point,
            # then SignatureAndHashAlgorithm(2) and signature<2>.
            plen = b[body + 3]
            if mode == 'pointlen':
                b[body + 3] = 0xFF
                sys.stderr.write("proxy: point length byte -> 0xFF\n")
            elif mode == 'pubkey':
                # Flip inside the point. Not the leading 0x04 tag and not the
                # very first coordinate byte -- we want a point that is still
                # structurally a point (and, for x25519, any 32 bytes are), so
                # that the failure is the SIGNATURE check and not a parse error.
                pos = body + 4 + plen // 2
                b[pos] ^= 0x01
                sys.stderr.write("proxy: flipped ECDHE public key byte %d\n" % pos)
            else:
                siglen = int.from_bytes(b[body + 4 + plen + 2:body + 4 + plen + 4], 'big')
                pos = body + 4 + plen + 4 + siglen - 1
                b[pos] ^= 0x01
                sys.stderr.write("proxy: flipped signature byte %d\n" % pos)
            return bytes(b)
        i += 4 + ml
    sys.stderr.write("proxy: no ServerKeyExchange found -- forwarding untouched\n")
    return bytes(b)


def flight_done(flight):
    i = 0
    while i + 4 <= len(flight):
        ml = int.from_bytes(flight[i + 1:i + 4], 'big')
        if i + 4 + ml > len(flight):
            return False
        if flight[i] == HS_SERVER_DONE:
            return True
        i += 4 + ml
    return False


def pump(src, dst):
    try:
        while True:
            d = src.recv(65536)
            if not d:
                break
            dst.sendall(d)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def serve(listen_port, server_port, mode):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', listen_port))
    ls.listen(4)
    sys.stderr.write("proxy: listening on %d -> %d (%s)\n" % (listen_port, server_port, mode))
    while True:
        cli, _ = ls.accept()
        srv = socket.create_connection(('127.0.0.1', server_port))
        threading.Thread(target=handle, args=(cli, srv, mode), daemon=True).start()


def handle(cli, srv, mode):
    threading.Thread(target=pump, args=(cli, srv), daemon=True).start()
    # Server -> client: hold the ServerHello..ServerHelloDone flight, tamper, send.
    raw, flight, hdrs, done = b'', b'', [], False
    try:
        while not done:
            d = srv.recv(65536)
            if not d:
                break
            raw += d
            recs, raw = parse_records(raw)
            for r in recs:
                if r[0] != 22:                    # not handshake: pass through
                    cli.sendall(r)
                    continue
                hdrs.append(len(r) - 5)
                flight += r[5:]
            done = flight_done(flight)
        if flight:
            out = tamper(flight, mode)
            off = 0
            for n in hdrs:                        # re-emit with the same framing
                cli.sendall(b'\x16\x03\x03' + struct.pack('>H', n) + out[off:off + n])
                off += n
        if raw:
            cli.sendall(raw)
        pump(srv, cli)
    except OSError:
        pass
    finally:
        for s in (cli, srv):
            try:
                s.close()
            except OSError:
                pass


if __name__ == '__main__':
    serve(int(sys.argv[1]), int(sys.argv[2]), sys.argv[3])
