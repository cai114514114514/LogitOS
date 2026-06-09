#ifndef AETHER_TCP_H
#define AETHER_TCP_H

#include <stdint.h>

/* A minimal active-open (client) TCP: reliable byte stream over IPv4. No
 * listen/accept. Receive does full out-of-order reassembly (a sorted interval
 * set over a seq-indexed 64 KiB ring), so a reordered/lost segment mid-flight no
 * longer discards the rest -- large TLS handshake flights arrive reliably. Send
 * keeps a single outstanding segment but segments payloads > MSS (no truncation).
 * The public API is blocking-ish (it pumps net_poll) and must run with
 * interrupts enabled, exactly like dns_resolve(). */

/* RX entry (handed to ip_input's protocol dispatch). */
void tcp_input(uint32_t src, const uint8_t *data, uint16_t len);

/* Timer pump (called from net_poll): retransmit unacked data, advance timers. */
void tcp_poll(void);

/* Open a connection to dst:port (host order). Returns a connection id (>=0) on
 * ESTABLISHED, or -1 on failure/timeout. */
int  tcp_connect(uint32_t dst, uint16_t port);

/* Queue/send bytes on an established connection. Returns bytes sent, or -1. */
int  tcp_send(int id, const void *buf, int len);

/* Copy up to max received bytes into buf. Returns the count (0 if none yet,
 * -1 if the connection is closed and drained). */
int  tcp_recv(int id, void *buf, int max);

/* Begin an orderly close (send FIN). */
void tcp_close(int id);

/* 1 if the connection is still usable (not closed/reset). */
int  tcp_alive(int id);

#endif /* AETHER_TCP_H */
