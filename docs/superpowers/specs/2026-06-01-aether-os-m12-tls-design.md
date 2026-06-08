# Aether OS — M12: TLS 1.3 + strict certificate verification (HTTPS)

**Date:** 2026-06-01
**Status:** design approved; implementing
**Arc:** browser milestone 3 of 4 (M10 TCP ✅ · M11 HTTP+render ✅ · **M12 TLS** · M13 layout)

## Goal

Make `https://` actually open. A from-scratch TLS 1.3 client over the M10 TCP
stack, with **strict certificate-chain verification** against a built-in set of
root CAs, so `https://example.com` loads in the Browser app with a verified,
encrypted connection. This is the project's largest milestone (cryptography from
scratch).

## Scope (every choice confirmed by probing example.com)

The live example.com endpoint requires, and its chain uses:
- **TLS 1.3**, key exchange **X25519**, AEAD **ChaCha20-Poly1305**, hash **SHA-256**
  (`openssl s_client`: Server Temp Key ECDH X25519; Cipher AEAD-CHACHA20-POLY1305-SHA256).
- **4-cert chain**, signatures: leaf & intermediates **ECDSA-with-SHA256/384**,
  public keys **EC P-256 / P-384**; root self-sign is RSA-SHA256 (root trusted by
  presence, not re-verified).

Therefore M12 implements (user confirmed: do it all in one milestone, verify
everything strictly):

### Crypto primitives (from scratch, `crypto/`)
| Primitive | Why |
|-----------|-----|
| SHA-256, SHA-384 | transcript hash, HKDF, cert-signature digests |
| HMAC + HKDF (-Expand-Label) | TLS 1.3 key schedule (RFC 8446 §7) |
| ChaCha20-Poly1305 (RFC 8439) | record AEAD (negotiated) |
| AES-128-GCM | second AEAD (user-requested), AES + GHASH |
| X25519 (RFC 7748) | handshake ECDH |
| bignum (mod arith) | underpins EC point math + ECDSA |
| EC P-256 / P-384 + ECDSA-verify | chain signature verification |

Each primitive validated against published test vectors (RFC/NIST) before use.

### Cert verification (strict; user's hard requirement)
- `net/asn1.c` — minimal DER reader (SEQUENCE/INTEGER/OID/BIT STRING/…).
- `net/x509.c` — parse cert {tbs, subject CN, SAN, validity, SPKI, sig alg, sig};
  built-in **root CA public-key table** (includes SSL.com TLS ECC Root CA 2022,
  example.com's root) + a few common roots (e.g. ISRG/DigiCert).
- Chain check: for each link leaf←intermediate←…←root, verify the issuer's key
  ECDSA-signs the child's TBS (digest per the cert's sig alg); the top issuer
  must be in the trust store; check SNI/CN-or-SAN match and notBefore/notAfter.
  Any failure → abort the handshake, refuse the connection.

### TLS layer (`net/tls.c`)
- **Record layer:** read/write TLSPlaintext/TLSCiphertext (content type, version,
  length); post-handshake records are AEAD-sealed with per-direction keys + nonce
  (seq Xor iv).
- **Handshake state machine (client active open):**
  ClientHello (X25519 share, supported suites/groups/sig-algs, SNI) →
  ServerHello (pick suite, derive handshake secrets via HKDF over the transcript)
  → decrypt {EncryptedExtensions, Certificate, CertificateVerify, Finished} →
  **verify cert chain + CertificateVerify signature + server Finished MAC** →
  send client Finished → derive application traffic secrets → encrypted HTTP.
- **Key schedule:** early/handshake/master secrets; `Derive-Secret`,
  `HKDF-Expand-Label`; transcript hash across all handshake messages.

### HTTP integration (`net/http.c`)
The `cur.https` guard (currently rejecting https) becomes the branch: for https,
after `tcp_connect`, run `tls_connect(tcp_id, host)`; then `tls_send`/`tls_recv`
wrap the GET/response in encrypted records instead of raw tcp_send/recv. Plain
http keeps the existing path. The Browser app is unchanged — it just stops
erroring on https URLs (example.com's "Learn more" → iana.org now opens).

## Data flow
```
http_get(https://h/p): dns_resolve(h) -> tcp_connect(ip,443)
  -> tls_connect(id,h): ClientHello..Finished, verify chain+sigs+MAC
  -> tls_send(GET) ; loop tls_recv -> plaintext body -> html_render (M11)
```
All blocking + interrupt-on (M11 lesson: SYS_HTTP_GET already re-enables IF).

## Components / files
- `crypto/{sha256,sha384,hmac_hkdf,chacha20poly1305,aesgcm,x25519,ec,ecdsa}.c`
  + `include/crypto.h` (or per-file headers).
- `net/{asn1,x509,tls}.c` + `include/{tls.h,x509.h}`; built-in roots in
  `crypto/roots.c` (committed public keys, no runtime fetch).
- `net/http.c` https branch; Makefile builds `crypto/*.c`.

## Error handling
- Any verification failure (bad sig, untrusted root, name mismatch, expired,
  bad Finished MAC, AEAD tag mismatch) → handshake abort, `HTTP_ERR_*` to the app.
- Buffers sized for a 4-cert chain + handshake (static; bounded).
- Unsupported suite/group/version from the server → abort (we offer only what we
  implement).

## Verification (staged; vectors + pcap + openssl cross-check)
1. **Primitives:** kernel self-tests assert each against published vectors
   (SHA-256/384, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, ECDSA-verify) →
   `AETHER_CRYPTO_OK`.
2. **DER/X.509:** parse a committed copy of the example.com chain; verify it to
   the built-in root → `AETHER_X509_OK`.
3. **Handshake:** `tls_connect` to example.com:443 reaches "handshake complete";
   pcap shows ClientHello/ServerHello + encrypted records; serial `AETHER_TLS_OK`.
4. **End to end:** Browser loads `https://example.com` and renders the page
   (same text as the http page, now over TLS); QMP screenshot.
5. `make test` stays green (`AETHER_BOOT_OK`).

## Build order (each layer builds + boots; temp self-tests removed before final)
1. SHA-256/384 + HMAC/HKDF (vectors).
2. ChaCha20-Poly1305 + AES-128-GCM (vectors).
3. X25519 + bignum + EC P-256/P-384 + ECDSA-verify (vectors).
4. ASN.1/X.509 parse + chain verification to built-in roots (committed chain).
5. TLS record + handshake state machine + key schedule (handshake to example.com).
6. http_get https branch + Browser end-to-end + docs.

## Risks / notes
- Biggest risk: subtle crypto/transcript/key-schedule bugs. Mitigation: every
  primitive is vector-checked in isolation before the handshake is attempted;
  the handshake transcript is cross-checked against openssl `-keylogfile`/-msg
  where useful.
- bignum + EC are the heaviest new code; constant-time is **not** a goal (this is
  a client fetching public pages in an emulator, not a hardened library).
- Out of scope: TLS 1.2, session resumption / 0-RTT, client certs,
  renegotiation, OCSP/CRL revocation, RSA key-exchange (none needed here).
- M13 (HTML/CSS layout) remains the final browser milestone after this.

## Result — implemented & verified

All six layers shipped; **the Browser opens `https://example.com`**.
- **L1-L3 primitives** (`crypto/`): SHA-256/384, HMAC/HKDF, ChaCha20-Poly1305,
  AES-128-GCM, X25519, ECDSA P-256/P-384 — each checked in-kernel against
  published vectors (RFC 8439/7748, NIST, RFC 4231) and a real openssl-made
  ECDSA signature. ECDSA needed Jacobian coordinates (affine was ~800x too slow).
- **L4 X.509** (`net/x509.c` + `crypto/roots.c`): DER parse + strict chain
  verification (per-link ECDSA, root-in-trust-store, SAN/CN + validity), proven
  on the live example.com 4-cert chain; tampered signatures rejected.
- **L5 TLS** (`net/tls.c`): a TLS 1.3 client — ClientHello..Finished, HKDF key
  schedule over the SHA-256 transcript, AEAD records, cert + CertificateVerify +
  Finished verification — handshakes to the live example.com:443.
- **L6**: `http_get` layers `tls_connect` over TCP for https; the Browser app
  renders `https://example.com` ("Example Domain / ... / Learn more", status
  "loaded").

Key debugging finds (systematic, evidence-led): ECDSA performance (affine→
Jacobian); and reconfirming the M11 rule that blocking net ops from a syscall
need interrupts enabled (the int 0x80 gate clears IF).

Next: **M13 HTML/CSS layout** — the final browser milestone.
