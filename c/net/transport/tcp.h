#ifndef AETHER_TCP_H
#define AETHER_TCP_H

#include <stdint.h>

/* A compact active-open (client) TCP over IPv4. No listen/accept. Receive does
 * out-of-order reassembly over a seq-indexed 64 KiB ring. Send keeps one
 * outstanding segment, honors the peer MSS and advertised window, and uses
 * zero-window persist probes. Checksums, receive-sequence acceptability,
 * cumulative ACKs, retransmission, and challenge ACKs for in-window RSTs are
 * implemented. Congestion control, window scaling, SACK, timestamps, urgent
 * data, and a full RFC close/TIME-WAIT state machine are not. The public API is
 * blocking-ish (it pumps net_poll) and must run with interrupts enabled. */

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
