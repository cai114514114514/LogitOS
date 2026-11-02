# TLS and certificates

LogitOS provides TLS 1.2 and TLS 1.3 clients, a TLS 1.3 server, X.509 certificate path verification, OCSP stapling checks, and a compiled certificate authority store. The protocol engines live in `c/net/tls/` and use primitives from `c/crypto/`. The kernel socket layer advances client handshakes incrementally, while the HTTPS service compiles the server engine into a user process.

## Protocol organization

`c/net/tls/tls.c` owns version negotiation, the TLS 1.3 client handshake, and the public client API. `c/net/tls/tls12.c` implements the TLS 1.2 handshake and records. Both use the session storage, transport helpers, transcript hashes, and certificate interfaces declared in `c/net/tls/tls_int.h`. This division accommodates the different handshake messages, key schedules, and record formats of the two versions.

| Role and version | Cipher suites |
|---|---|
| TLS 1.3 client and server | AES-128-GCM with SHA-256, ChaCha20-Poly1305 with SHA-256, AES-256-GCM with SHA-384 |
| TLS 1.2 client | ECDHE with RSA or ECDSA authentication, using AES-128-GCM, AES-256-GCM, or ChaCha20-Poly1305 |

Classical key exchange supports X25519, X448, P-256, and P-384. TLS 1.3 also supports the X25519MLKEM768 hybrid group. The client initially supplies both hybrid and X25519 shares, allowing either group to be selected in the first response. A HelloRetryRequest can select another advertised group; the client rebuilds its transcript and generates the requested share. The server supports retries and prefers the hybrid group among matching supplied shares (`c/net/tls/tls.c`, `c/net/tls/tls_server.c`).

The ClientHello advertises TLS 1.3 before TLS 1.2. Version selection uses the server’s supported_versions extension when present and its legacy version field otherwise. The client checks downgrade sentinels when a server selects TLS 1.2 (`c/net/tls/tls.c`).

TLS 1.2 requests extended master secret. When negotiated, its derivation includes the handshake transcript through ClientKeyExchange. Otherwise, derivation uses the premaster secret and both handshake random values. The ECDHE and AEAD suite selection follows the rationale in `c/net/tls/tls_int.h`: forward secrecy and reduced exposure to RSA decryption oracles and CBC padding attacks.

These retry and ticket resumption paths supersede the older “no HelloRetryRequest, no resumption” claim in `TRANSPARENCY.md`.

## Driving the client

The API in `c/net/tls/tls.h` operates over an established TCP connection. `tls_start()` takes a TCP identifier, a hostname, an Application-Layer Protocol Negotiation (ALPN) offer listing protocols separated by commas, and time in Unix seconds. The hostname supplies Server Name Indication (SNI) and certificate name verification. The session retains the supplied time for certificate checks.

After obtaining a session identifier, advance the handshake with `tls_step()`:

| Result | Caller action |
|---|---|
| `TLS_DONE` | Begin application I/O. |
| `TLS_WANT_READ` | Arrange for further transport progress, then call again. |
| `TLS_WANT_WRITE` | Wait for transport output capacity, then call again. |
| Negative error code | Handle the failure and close the session. |

The socket broker follows this sequence and records the selected ALPN protocol before marking the socket ready (`c/net/core/sock.c`). The blocking HTTP path calls `tls_connect()`, which offers `http/1.1` and drives the handshake through network polling and idle waits (`c/net/http/http.c`, `c/net/tls/tls.c`).

`tls_send()` returns application bytes accepted into the session, potentially fewer than requested. The session retains sealed ciphertext until TCP accepts it. `tls_recv()` drains buffered plaintext and processes incoming records. A zero return indicates that the caller should try again after further progress. `tls_pending()` reports plaintext already buffered above TCP, which helps readiness polling detect data after the underlying receive buffer empties.

Call `tls_close()` for every returned session identifier, including after handshake failure. It wipes the session storage; the caller owns TCP teardown. `TLS_MAX_SESSIONS` defines 16 client slots. Both client and server handshakes use an absolute deadline so that successive fragments consume one overall time budget (`c/net/tls/tls.c`, `c/net/tls/tls_server.c`).

Handshake setup and server identity generation check `rng_strong()`. In the kernel, this query seeds the generator if needed and reports whether hardware entropy has entered its state (`c/kernel/core/rng.c`, `c/kernel/core/rng.h`).

## Peer authentication

A full client handshake combines certificate path verification with a signature that binds the peer’s key to the handshake.

TLS 1.3 verifies CertificateVerify and Finished. The flight verifier records successful CertificateVerify verification and requires it before completing certificate authentication. Supported CertificateVerify schemes are ECDSA with P-256/SHA-256, P-384/SHA-384, or P-521/SHA-512, and RSA-PSS with SHA-256, SHA-384, or SHA-512 (`c/net/tls/tls.c`).

TLS 1.2 verifies the ServerKeyExchange signature over the client random, server random, and ECDHE parameters. This authenticates the ephemeral exchange using the leaf certificate’s key. Signature verification accepts ECDSA, RSA PKCS#1 v1.5, and RSA-PSS with the supported SHA-2 hashes. The client subsequently verifies the server’s Finished value (`c/net/tls/tls12.c`).

Both versions use `tls_check_chain()` to invoke the X.509 verifier and report certificate failures.

## X.509 validation

`x509_parse()` reads a DER certificate into `struct cert`. Its pointers reference the input DER storage, which must outlive the parsed structure. The retained fields include the signed certificate bytes, public key, issuer and subject names, validity interval, common name (CN), Subject Alternative Name (SAN), BasicConstraints CA flag, and OCSP inputs (`c/net/tls/x509.h`, `c/net/tls/x509.c`).

Certificate signature verification supports ECDSA, RSA PKCS#1 v1.5, and RSA-PSS with SHA-256, SHA-384, or SHA-512, plus Ed25519. EC certificate keys support P-256, P-384, and P-521. Parsing requires the inner and outer certificate signature algorithm encodings to match.

`x509_verify_chain()` expects the leaf first and searches the remaining supplied certificates for parents. A qualifying parent has the BasicConstraints CA flag, a subject name whose DER matches the child’s issuer name, and a key that verifies the child’s signature. Tracking used entries allows the search to traverse duplicate or reordered intermediates.

The verifier compares each selected certificate’s parsed validity interval with the supplied time. Once the supplied certificates provide no further qualifying parent, it checks the compiled anchors. Trust succeeds when the current certificate’s public key matches an anchor or its signature verifies under an anchor key. On success, the verifier reorders the array into path order and reports the path length. This places the selected leaf issuer where the OCSP checker expects it (`c/net/tls/x509.c`).

Hostname verification searches SAN DNS names when the SAN value has a positive byte length. Otherwise, it falls back to CN. Matching ignores ASCII letter case and supports a leading wildcard covering one label. A recorded extension decoding error causes hostname rejection.

## PSK and session tickets

TLS 1.3 client resumption uses pre-shared keys (PSKs) derived from NewSessionTicket messages received after a completed handshake. `tls_recv()` processes these messages and passes ticket data to `c/net/tls/tls_psk.c`.

The resumption master secret includes the transcript through the client Finished. Each ticket nonce derives a PSK from that secret. The cache holds 16 entries, retains multiple tickets for a hostname, and selects them in arrival order. Taking a ticket copies its identity and PSK into the session and wipes the cache entry. This gives concurrent connections different cached credentials. Eviction and cache clearing also wipe stored secrets.

The next ClientHello offers a ticket identity and a binder covering the truncated ClientHello. After a retry, the binder includes the revised transcript. Resumption uses `psk_dhe_ke`, combining the PSK with fresh ephemeral exchange. The server’s Finished authenticates its possession of the resulting secrets. If the server declines the ticket, the client continues through certificate authentication and clears the remaining cached tickets for that hostname (`c/net/tls/tls.c`, `c/net/tls/tls_psk.c`).

Ticket lifetime is capped at seven days. Expiry comparisons use supplied Unix timestamps; the issuing timestamp comes from the session’s connection-start time. Wire ticket age uses elapsed milliseconds measured from ticket receipt.

`tls_resumed()` reports whether the server accepted resumption. `tls_tickets_count()` reports occupied cache entries, and `tls_tickets_clear()` wipes the cache.

## TLS server and HTTPS service

The server API in `c/net/tls/tls_server.h` follows the incremental client interface. `tlss_start()` allocates a session, `tlss_step()` advances the handshake, and `tlss_send()` and `tlss_recv()` transfer application data. Call `tlss_flush()` until queued ciphertext drains before shutdown. `tlss_close()` attempts a close notification for an established session and releases its state.

A `struct tls_ident` supplies a certificate chain and private EC scalar. The caller must provide the matching leaf first and retain the identity and DER storage for the lifetime of its sessions. The API accepts P-256, P-384, or P-521 as the identity curve and up to three certificates. `tlss_start()` checks the curve, certificate count, entry lengths, and `TLSS_CHAIN_BYTES`: a 12,288-byte bound including five bytes of overhead per certificate entry (`c/net/tls/tls_server.c`).

The server sends its chain, signs CertificateVerify with ECDSA, and verifies the client Finished. It chooses ALPN in configured preference order and exposes requested SNI through `tlss_sni()`. `TLSS_MAX_SESSIONS` defines four server slots.

`tlss_self_signed()` generates a P-256 key and certificate containing the supplied name in CN and SAN. Persistence is implemented by the HTTPS service in `c/apps/services/http/https_transport.h`. Its identity loader accepts a P-256 scalar, checks ownership and permission bits of the key file, and compares the derived public key with the parsed leaf. Optional DER intermediates allow an administrator to supply a chain issued by a CA.

`tests/servers.mk` compiles the server engine into the HTTPS user process. `c/apps/services/http/https_platform.c` maps transport operations to process file descriptors and obtains entropy through userland interfaces. The service forks a worker for each accepted connection, with four worker slots. Each worker has its own TLS pool state; `c/net/tls/tls_domain.h` adapts pool ownership for this process model (`c/apps/services/http/httpd.c`).

## OCSP stapling

The client requests Online Certificate Status Protocol (OCSP) stapling in ClientHello. TLS 1.2 extracts the response from CertificateStatus, while TLS 1.3 extracts it from the first CertificateEntry’s extensions. Both send extracted response bytes to `tls_check_staple()` (`c/net/tls/tls.c`, `c/net/tls/tls12.c`).

`ocsp_check()` verifies the response signature using the issuer’s key or a responder certificate included in the response. A delegated responder must carry the OCSP signing extended key usage, have a signature verified by the issuer, and pass validity checks with the supplied time tolerance.

A matching response identifies the leaf through its serial number and hashes of the issuer’s subject name and public key bits. CertID hashing supports SHA-1 and SHA-256. Response signature verification supports ECDSA and RSA PKCS#1 v1.5 with SHA-256, SHA-384, or SHA-512 (`c/net/tls/ocsp.c`).

A signed, matching `good` response with acceptable update times returns `OCSP_OK`. The verifier distinguishes revoked and unknown status, certificate mismatches, parsing failures, signer failures, and freshness errors. TLS supplies a 300-second tolerance and maps these results to the continuation policy described below.

## CA store and maintenance

The CA inputs live in `tools/roots/`. `tools/genroots.py` extracts their public keys into `c/crypto/trust/roots_bundle.inc`, which `c/crypto/trust/roots.c` includes. The current generated table contains 130 anchor entries and records zero skipped inputs.

`struct root_ca` represents EC keys, RSA modulus and exponent pairs, or Ed25519 keys. Companion arrays retain root names and descriptions of skipped inputs (`c/crypto/trust/roots.h`). `trust_banner()` prints the compiled counts and root names during kernel initialization (`c/net/tls/tls.c`, `c/kernel/init/kmain.c`).

After changing the PEM inputs, run `python3 tools/genroots.py` from the repository root to update the committed bundle. `make check-roots` generates a comparison bundle under the build directory and compares it with the committed file. The generator refuses an empty result by default and records unsupported or malformed inputs in its skip list (`tools/genroots.py`, `Makefile`).

`tools/roots/README.md` records the import history and a comparison with Mozilla certificate data fetched on 2026-08-21. `tools/roots/check_provenance.py` compares certificate DER fingerprints with the recorded ledgers. These records support tracing the inputs’ origin.

## Verification entry points

The following commands run from the repository root. Their coverage descriptions come from the harness sources; the commands were not run during this documentation review.

| Command | Harness coverage |
|---|---|
| `make test-tls-interop` | Client negotiation, authentication failures, OCSP, resumption, and OpenSSL interoperability; includes a prerequisite resumption control. |
| `make test-tls-server` | Server interoperability with OpenSSL and the project client; prerequisite controls alter the key schedule, CertificateVerify input, and hybrid support. |
| `make test-tls-psk` | Ticket ages, consumption, hostname scope, expiry, eviction, and binder inputs; prerequisite controls restore age and reuse defects. |
| `make test-tls-chain` | Captured chains, duplicate and reordered intermediates, signature mutations, and hostname rejection; includes a prerequisite trust-bypass control. |
| `make test-ocsp` | Parsing, delegated signers, certificate matching, status, freshness, and tampering. |
| `make test-ocsp-control` | Handshake assertions against a client built to bypass staple verification. |

Definitions are in `Makefile`, `tests/tlsx.mk`, `tests/tlsfix.mk`, and `tests/crypto.mk`. The principal harnesses are `tests/unit/run-tls-interop.sh`, `tests/unit/run-tls-server.sh`, `tests/unit/tls_psk_test.c`, and `tests/unit/ocsp_test.c`.

## Limitations

- TLS and cryptography are experimental implementations without an independent cryptographic audit or production security support (`SECURITY.md`, `TRANSPARENCY.md`).
- Client certificates, 0-RTT, renegotiation, and KeyUpdate are unsupported. The server also lacks TLS 1.2, ticket issuance, PSK resumption, RSA identity signing, and OCSP stapling. TLS 1.2 client sessions also lack resumption (`c/net/tls/tls.h`, `c/net/tls/tls_server.h`, `c/net/tls/tls12.c`).
- Client ticket resumption supports the two SHA-256 TLS 1.3 suites. Cache matching keys on the hostname alone (`c/net/tls/tls_psk.c`).
- X.509 path validation omits key usage, server authentication extended key usage, path length constraints, name constraints, and unknown critical extension rejection. Parent selection stops at the first qualifying certificate; missing intermediates require caller-supplied certificates. Hostname verification handles DNS names (`c/net/tls/x509.c`).
- Client handshake storage holds 16 KiB and up to eight successfully parsed certificates; certificate readers skip entries that fail parsing. Ed25519 verification covers certificate signatures and anchors, while TLS CertificateVerify accepts ECDSA or RSA-PSS. The TLS 1.3 flight verifier handles messages by dispatch and presence checks, so message ordering receives only partial enforcement (`c/net/tls/tls.c`, `c/net/tls/tls12.c`, `c/net/tls/tls_int.h`).
- OCSP works from stapled responses alone. Missing staples and paths lacking an issuer certificate permit continuation. `OCSP_E_STALE` also permits continuation after logging, including expired or missing nextUpdate and thisUpdate beyond the future tolerance. Other OCSP errors fail the handshake. This corrects the older expired-staple refusal policy documented in `c/net/tls/ocsp.h`; `tls_check_staple()` implements the current policy (`c/net/tls/tls.c`, `c/net/tls/ocsp.c`).
- Compiled anchors keep only public keys, dropping root certificate expiry and policy metadata. The dated store comparison records anchors outside Mozilla’s server authentication trust set; fingerprint provenance accounting does not establish current trust eligibility. Generated self-signed server certificates require client trust configuration (`c/crypto/trust/roots.h`, `tools/roots/README.md`, `c/net/tls/tls_server.h`).

## Source map

| File | What it contains |
|---|---|
| `c/net/tls/tls.h` | Public client API, results, and session capacity. |
| `c/net/tls/tls.c` | Negotiation, TLS 1.3 client, authentication, staple policy, records, and diagnostics. |
| `c/net/tls/tls12.c` | TLS 1.2 client handshake, signatures, key schedule, and records. |
| `c/net/tls/tls_int.h` | Shared session state, protocol constants, and buffer bounds. |
| `c/net/tls/tls_psk.c` | Ticket cache, PSK derivation, ticket ages, and binders. |
| `c/net/tls/tls_server.h`, `c/net/tls/tls_server.c` | Server API, identity representation, handshake, certificate generation, and application I/O. |
| `c/net/tls/x509.h`, `c/net/tls/x509.c` | Certificate parsing, signatures, hostname verification, and path construction. |
| `c/net/tls/ocsp.h`, `c/net/tls/ocsp.c` | OCSP interface, parsing, signer verification, status, and freshness checks. |
| `c/crypto/trust/roots.h`, `c/crypto/trust/roots.c`, `c/crypto/trust/roots_bundle.inc` | Anchor representation and compiled CA store. |
| `tools/genroots.py` | PEM extraction and bundle generation. |
| `tools/roots/README.md`, `tools/roots/check_provenance.py` | Import history, dated store comparison, and fingerprint accounting. |
| `c/net/core/sock.c`, `c/net/http/http.c` | Incremental socket and blocking HTTP client integrations. |
| `c/kernel/core/rng.c`, `c/kernel/core/rng.h` | Entropy status used by TLS setup. |
| `c/kernel/init/kmain.c` | Trust store reporting at boot. |
| `c/apps/services/http/httpd.c`, `c/apps/services/http/https_transport.h`, `c/apps/services/http/https_platform.c` | HTTPS workers, identity persistence, and process transport adapters. |
| `c/net/tls/tls_domain.h`, `tests/servers.mk` | Pool ownership adaptation and HTTPS service build. |
| `Makefile`, `tests/tlsx.mk`, `tests/tlsfix.mk`, `tests/crypto.mk` | Bundle comparison and verification targets. |
| `tests/unit/run-tls-interop.sh`, `tests/unit/run-tls-server.sh` | Client and server interoperability harnesses. |
| `tests/unit/tls_psk_test.c`, `tests/unit/ocsp_test.c` | Ticket cache, binder, and OCSP assertions. |
| `SECURITY.md`, `TRANSPARENCY.md` | Security status and rules for interpreting claims. |

## See also

- [Sockets](sockets.md)
- [HTTP](http.md)
- [Cryptography overview](../crypto/overview.md)
- [Trust roots and package signing](../crypto/trust-and-signing.md)
- [Security model](../architecture/security-model.md)
