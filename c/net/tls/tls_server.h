#ifndef LOGIT_TLS_SERVER_H
#define LOGIT_TLS_SERVER_H

#include <stdint.h>

/* A TLS 1.3 (RFC 8446) SERVER over an established TCP connection.
 *
 * Until this file, c/net/tls could place a call and could not answer one:
 * `grep tls_accept` found nothing, and the httpd in c/apps/coreutils could
 * serve plaintext only. The client and the server are two halves of the same
 * protocol but they are NOT the same program -- the client proves nothing
 * about itself and verifies everything about its peer; the server proves
 * everything about itself and (here) verifies nothing about its peer.
 *
 * WHAT IS SHARED, and this is deliberate: the record layer, the transcript,
 * the AEAD dispatch, the ephemeral key handling and the session struct all
 * come from tls_int.h and are the SAME code the client runs. What is NOT
 * shared is the TLS 1.3 key schedule and the record seal/open, because those
 * are `static` inside tls.c -- see the SHARING note at the top of
 * tls_server.c, which names the exact five functions and what changing them
 * would cost. Two copies of a key schedule is how they come to disagree, so
 * that duplication is recorded as debt with a removal plan, not accepted.
 *
 * WHAT THIS SERVER DOES:
 *   versions   TLS 1.3 only. A ClientHello without supported_versions naming
 *              0x0304 is refused with a protocol_version alert rather than
 *              answered in 1.2 -- c/net/tls/tls12.c is a CLIENT and there is
 *              no 1.2 server here, so pretending otherwise would mean a
 *              handshake that starts and cannot finish.
 *   groups     x25519, secp256r1, secp384r1, with HelloRetryRequest when the
 *              client's key_share offers none of them but its supported_groups
 *              does. That path is not decoration: a 2026 Chrome or Firefox
 *              leads with X25519MLKEM768, which we do not have, and without
 *              HRR every modern browser would be unable to reach this server.
 *   suites     TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256,
 *              TLS_AES_256_GCM_SHA384 -- the same three the client offers, so
 *              there is exactly one list of suites in this tree.
 *   auth       an EC certificate chain and an ECDSA CertificateVerify.
 *
 * WHAT IT DOES NOT DO, each because of a specific missing piece rather than a
 * shortage of enthusiasm:
 *   - CLIENT CERTIFICATES. Not free: a CertificateRequest is easy, but
 *     verifying the client's CertificateVerify means the signature-checking
 *     block inside tls.c's verify_flight(), which is static and welded to a
 *     client-shaped flight walk. Rather than fork it (the same mistake as
 *     forking the key schedule), this is left undone and named.
 *   - SESSION RESUMPTION / NewSessionTicket. tls_psk.c is a client-side ticket
 *     CACHE; the server half is a ticket ISSUER with a rotating key, which is
 *     a different program.
 *   - 0-RTT, KeyUpdate, post-handshake auth, OCSP stapling (we have no
 *     responder to staple from), TLS 1.2, RSA certificates (no RSA signer
 *     exists in this tree; see ecdsa_sign in c/crypto/pubkey/ecdsa.c for why
 *     the EC one had to be written first).
 *
 * --- Driving it. Same shape as the client: nothing here ever blocks. ---
 *
 *     int id = tlss_start(tcp, &ident, "http/1.1", now);
 *     for (;;) {
 *         int rc = tlss_step(id);
 *         if (rc == TLS_DONE) break;
 *         if (rc < 0) { tlss_close(id); ...fail... }
 *         // TLS_WANT_READ / TLS_WANT_WRITE: wait for the socket, step again.
 *     }
 *     tlss_recv(id, buf, sizeof buf);  tlss_send(id, reply, n);
 */

#include "tls.h"        /* TLS_DONE / TLS_WANT_* / TLS_E_* are shared */

/* Longest chain a server identity may carry. Three is a leaf, an intermediate
 * and a cross-signed intermediate, which is what the public web actually uses;
 * the self-signed identity this file can generate is one. */
#define TLSS_CHAIN_MAX 3

/* The whole flight (ServerHello + the encrypted EE/Certificate/
 * CertificateVerify/Finished) is staged in the session's 16 KiB handshake
 * buffer, so this is what bounds an identity rather than any wire limit. */
#define TLSS_CHAIN_BYTES 12288

/* Concurrent server sessions. Four, not the client's sixteen: a struct
 * tls_sess is about 54 KiB (a full-size receive record, a decrypt buffer and a
 * send buffer), so sixteen would be 870 KiB of kernel BSS for a machine whose
 * only server today is an httpd. Raising it is one line and costs 54 KiB a
 * session, which is the number to weigh rather than a vague "more". */
#define TLSS_MAX_SESSIONS 4

/* What the server presents. `chain[0]` is the leaf and must match `key`.
 *
 * The DER lives in the caller's memory and must outlive the session -- like
 * `struct cert`, and for the same reason: copying a chain into every session
 * would put 12 KiB x TLSS_MAX_SESSIONS of duplicate certificate in BSS to hold
 * bytes that never change. */
struct tls_ident {
    const uint8_t *chain[TLSS_CHAIN_MAX];
    int            chainlen[TLSS_CHAIN_MAX];
    int            nchain;
    int            key_curve;               /* 256 / 384 / 521 */
    uint8_t        key[66];                 /* private scalar, big-endian, flen bytes */
};

/* --- where the certificate comes from -------------------------------------
 *
 * Generate one. Not a build-time constant, not a file in fsroot: BOTH of those
 * put a TLS server's PRIVATE KEY in a git repository, where it is a secret
 * every reader of the repository already has -- and a server whose key is
 * public is not a server, it is a demonstration that looks like one. The
 * failure is silent and permanent: nothing about a handshake changes when the
 * key is known to the attacker.
 *
 * So the key is made here, on the machine, from kernel_random_bytes, and never
 * touches a disk. The costs, stated plainly rather than buried:
 *   - the certificate is SELF-SIGNED, so no client trusts it by default. That
 *     is not a shortcoming of the design, it is the true state of this
 *     machine: there is no CA here and no way to obtain a publicly trusted
 *     certificate, so a certificate that LOOKED trusted would be lying.
 *   - it does not survive a reboot. A caller that needs a stable identity
 *     fills in `struct tls_ident` itself from a chain it obtained some other
 *     way; that is the whole reason the struct is public.
 *   - generation costs one P-256 keygen plus one ECDSA signature over the
 *     TBSCertificate: 1,546 us on the host, measured over 100 iterations by
 *     `make bench-tls-selfcert`. That is a HOST number and the target under
 *     QEMU's TCG will be some multiple of it -- it is quoted because the
 *     argument only needs the order of magnitude: one-off milliseconds at
 *     first use, against a private key that ships in a git repository.
 *
 * `cn` is both the Subject/Issuer CN and the SAN dNSName, so the name check in
 * x509.c passes for exactly that name and no other. `days` is the validity
 * window from `now`. `buf`/`buflen` receive the DER, and `id` is filled to
 * point INTO buf -- so buf must outlive every session using the identity.
 * Returns the DER length, or -1. */
int tlss_self_signed(struct tls_ident *id, const char *cn, int64_t now, int days,
                     uint8_t *buf, int buflen);

/* Begin a server handshake on an accepted TCP connection.
 *   ident  the certificate chain and key to present; must outlive the session
 *   alpn   comma-separated protocols we are willing to speak, most preferred
 *          first ("h2,http/1.1"); NULL or "" selects nothing and sends no ALPN
 *   now    unix-ish seconds (unused by the handshake itself -- the server
 *          checks no certificate -- but kept so a future client-auth path has
 *          the same clock the client uses)
 * Returns a session id (>= 0) or a negative TLS_E_*. The slot is held until
 * tlss_close(), including after failure, which is how the reason is read. */
int  tlss_start(int tcp_id, const struct tls_ident *ident, const char *alpn, int64_t now);

/* Advance the handshake. TLS_DONE / TLS_WANT_READ / TLS_WANT_WRITE, or a
 * negative TLS_E_* once it has failed (repeat calls return the same error). */
int  tlss_step(int id);

/* Application data, both non-blocking, same contracts as tls_send/tls_recv:
 * send returns bytes accepted (possibly 0, possibly < len) or -1; recv returns
 * bytes read, 0 if nothing is available yet, or -1 on close/failure. */
int  tlss_send(int id, const void *buf, int len);
int  tlss_recv(int id, void *buf, int max);

/* The ALPN protocol selected ("" if none), its length, or -1 for a bad id. */
int  tlss_alpn(int id, char *out, int max);

/* The SNI the client asked for, NUL-terminated ("" if it sent none). A server
 * that cannot report this cannot do virtual hosting, and a caller that has to
 * re-parse the ClientHello to find it would be parsing attacker input twice. */
int  tlss_sni(int id, char *out, int max);

/* Negotiated version on the wire (0x0304), or 0 before the ClientHello. */
int  tlss_version(int id);

/* Decrypted bytes already buffered and returnable without touching TCP. */
int  tlss_pending(int id);

/* Send a close_notify (best effort), release the session, wipe its keys. */
void tlss_close(int id);

#endif /* LOGIT_TLS_SERVER_H */
