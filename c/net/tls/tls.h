#ifndef LOGIT_TLS_H
#define LOGIT_TLS_H

#include <stdint.h>

/* A TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246) client over an established TCP
 * connection (M10). Both versions are offered in every ClientHello and the
 * server's choice is taken: 1.3 through the supported_versions extension, 1.2
 * through the legacy ServerHello version a 1.2 server answers with. 1.3 is
 * preferred and is the path that runs against everything modern; 1.2 exists
 * because a measurable slice of the web is still 1.2-only (sectigo.com,
 * www.mas.gov.sg, www.cbuae.gov.ae at the time of writing), and to those hosts
 * "we only speak 1.3" meant unreachable, not slow.
 *
 * Key exchange (both versions): x25519 (preferred) plus secp256r1 / secp384r1.
 * In 1.3 a group we did not lead with is reached through HelloRetryRequest; in
 * 1.2 the server simply names the curve in its ServerKeyExchange.
 *
 * Cipher suites:
 *   1.3  TLS_AES_128_GCM_SHA256, TLS_CHACHA20_POLY1305_SHA256
 *   1.2  ECDHE-{RSA,ECDSA} with AES-128-GCM, AES-256-GCM and
 *        ChaCha20-Poly1305. ECDHE ONLY -- no static-RSA key exchange and no
 *        CBC suites; see tls_int.h for why that is a security decision and not
 *        an omission.
 * Extended master secret (RFC 7627) is requested on every 1.2 handshake.
 * ALPN is offered and the selection reported. Certificate chains are verified
 * strictly against the built-in roots, and in 1.2 the ServerKeyExchange
 * signature is verified against the leaf -- it is the only thing that
 * authenticates the ephemeral key.
 *
 * Not implemented: TLS 1.1 and below, session resumption / PSK / tickets,
 * 0-RTT, client certificates (a CertificateRequest is declined politely),
 * renegotiation, KeyUpdate, post-quantum groups.
 *
 * --- Two ways to drive it ---
 *
 * 1. Stepped (non-blocking). This is the shape a non-blocking socket layer
 *    wants; nothing here ever sleeps, pumps the network, or spins.
 *
 *        int id = tls_start(tcp, host, "h2,http/1.1", now);
 *        for (;;) {
 *            int rc = tls_step(id);
 *            if (rc == TLS_DONE) break;
 *            if (rc < 0) { tls_close(id); ...fail... }
 *            // rc is TLS_WANT_READ or TLS_WANT_WRITE: wait for the socket to
 *            // become readable/writable, then step again.
 *        }
 *
 *    tls_send/tls_recv are non-blocking too: they move whatever the transport
 *    will take right now and tell you when it would block.
 *
 * 2. Blocking. tls_connect() is the original entry point, kept working on top
 *    of the stepped one: it pumps net_poll()/net_idle() until the handshake
 *    finishes. Like every blocking net call it must run with interrupts
 *    enabled (see SYS_HTTP_GET in wm.c).
 */

/* tls_step() results. The three non-negative values are the whole protocol
 * between TLS and the socket layer; anything negative is a terminal TLS_E_*. */
#define TLS_DONE        0    /* handshake complete; the session is usable */
#define TLS_WANT_READ   1    /* blocked: no more bytes available from TCP yet */
#define TLS_WANT_WRITE  2    /* blocked: TCP would not accept our pending bytes */

/* Begin a handshake over TCP connection `tcp_id`.
 *   host   SNI + certificate name check (<= 255 bytes)
 *   alpn   comma-separated protocol list to offer, e.g. "h2,http/1.1";
 *          NULL or "" omits the ALPN extension entirely
 *   now    unix-ish seconds, for certificate validity
 * Returns a session id (>= 0) or a negative TLS_E_*. The session slot is held
 * until tls_close(), including after a failed handshake -- callers must close
 * even on error, which is also how they read the failure back out. */
int  tls_start(int tcp_id, const char *host, const char *alpn, int64_t now);

/* Advance the handshake. Returns TLS_DONE / TLS_WANT_READ / TLS_WANT_WRITE, or
 * a negative TLS_E_* once the session has failed (repeat calls keep returning
 * the same error). Safe to call after TLS_DONE: it returns TLS_DONE. */
int  tls_step(int id);

/* Perform the whole handshake, blocking (pumps net_poll/net_idle). Returns a
 * session id (>= 0) on a fully verified ESTABLISHED, or a negative TLS_E_*.
 * On failure the session is already closed. Offers "http/1.1" via ALPN. */
int  tls_connect(int tcp_id, const char *host, int64_t now);

/* Send application data. Non-blocking: returns the number of bytes accepted
 * (which may be less than len), 0 if the transport cannot take anything right
 * now (retry later with the un-sent tail), or -1 if the session is dead. */
int  tls_send(int id, const void *buf, int len);

/* Receive decrypted application data. Non-blocking: returns bytes read, 0 if
 * nothing is available yet, or -1 if the session closed or failed. */
int  tls_recv(int id, void *buf, int max);

/* The ALPN protocol the server selected, as a NUL-terminated string ("" if the
 * server did not select one). Returns its length, or -1 for a bad id. */
int  tls_alpn(int id, char *out, int max);

/* The negotiated protocol version on the wire: 0x0304 (TLS 1.3), 0x0303
 * (TLS 1.2), or 0 before the ServerHello has been read / for a bad id. */
int  tls_version(int id);
#define TLS_VER_12 0x0303
#define TLS_VER_13 0x0304

/* Decrypted application bytes already buffered and returnable without touching
 * the transport. A readiness poll that only watched the socket would miss the
 * case where a whole record has been decrypted into the session while the wire
 * buffer went empty; this closes it. 0 for a bad or empty session. */
int  tls_pending(int id);

/* Release the session and wipe its key material. */
void tls_close(int id);

/* Sessions available concurrently. Sized to the socket layer above (16
 * sockets), not to TCP's NCONN, so a TLS socket can never be the thing that
 * runs out first. Each session carries a full-size receive record, a decrypt
 * buffer and a send buffer -- about 54 KiB, so this line costs ~870 KiB of
 * kernel BSS. That is the price of a connection pool; the previous value was
 * 2, which capped every concurrent fetch at two. */
#define TLS_MAX_SESSIONS 16

#define TLS_E_PROTO   -1
#define TLS_E_CERT    -2
#define TLS_E_CRYPTO  -3
#define TLS_E_TCP     -4

#endif /* LOGIT_TLS_H */
