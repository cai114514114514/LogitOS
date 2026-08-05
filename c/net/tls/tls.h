#ifndef LOGIT_TLS_H
#define LOGIT_TLS_H

#include <stdint.h>

/* A minimal TLS 1.3 client over an established TCP connection (M10). Supports
 * X25519 key exchange, ChaCha20-Poly1305 + AES-128-GCM, SHA-256 transcript,
 * with strict certificate-chain verification (M12 L4). Blocking-ish: pumps
 * net_poll while waiting, must run with interrupts enabled (like dns_resolve). */

/* Perform the handshake over TCP connection `tcp_id` to `host` (for SNI + cert
 * name check), with `now` (unix-ish seconds) for validity checks. Returns a TLS
 * session id (>=0) on a fully verified ESTABLISHED, or <0 on any failure. */
int  tls_connect(int tcp_id, const char *host, int64_t now);

/* Send application data (encrypted record). Returns bytes sent, or -1. */
int  tls_send(int id, const void *buf, int len);

/* Receive decrypted application data into buf (up to max). Returns bytes (0 if
 * none yet, -1 if the session closed). */
int  tls_recv(int id, void *buf, int max);

/* Close the session (sends close_notify) and releases it. */
void tls_close(int id);

#define TLS_E_PROTO   -1
#define TLS_E_CERT    -2
#define TLS_E_CRYPTO  -3
#define TLS_E_TCP     -4

#endif /* LOGIT_TLS_H */
