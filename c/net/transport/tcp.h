#ifndef LOGIT_TCP_H
#define LOGIT_TCP_H

#include <stdint.h>

/* A compact active-open (client) TCP. No listen/accept.
 *
 * Implemented: RFC 9293 core (checksums, receive-sequence acceptability,
 * cumulative ACKs, challenge ACKs for in-window RSTs, the full close/TIME-WAIT
 * state machine), out-of-order reassembly over a seq-indexed receive ring,
 * RFC 6298 RTT/RTO estimation, RFC 5681 congestion control (slow start,
 * congestion avoidance, fast retransmit/fast recovery) with the RFC 6582
 * NewReno partial-ACK rule, RFC 7323 window scaling + timestamps + PAWS,
 * RFC 2018 selective acknowledgement on BOTH sides, RFC 1122 delayed ACKs,
 * RFC 896 Nagle (with TCP_NODELAY), RFC 1122 silly-window avoidance, zero-
 * window persist probes, and RFC 1191 path-MTU discovery with a blackhole
 * backstop.
 *
 * Also implemented (server side): the passive open -- LISTEN, SYN_RCVD, the
 * three-way handshake from the receiving end, a per-listener accept backlog,
 * and half-close. See "the server API" at the bottom of this header.
 *
 * Not implemented: urgent data, TCP-AO/MD5, ECN, CUBIC (Reno is
 * the controller), and the full RFC 6675 pipe algorithm -- SACK informs which
 * bytes to retransmit, but the congestion window is driven by RFC 5681's
 * duplicate-ACK inflation rather than a scoreboard-walked pipe estimate.
 *
 * The public API is blocking-ish (it pumps net_poll) and must run with
 * interrupts enabled; the _nb entry points never block. */

/* --- address family ------------------------------------------------------
 * TCP itself is address-family agnostic: only the pseudo-header and the
 * datagram-send call differ. Everything internal carries a struct tcp_addr so
 * the IPv6 line can light up AF_INET6 without touching the state machine. The
 * uint32_t entry points below are IPv4 wrappers kept for existing callers. */
#define TCP_AF_INET   4
#define TCP_AF_INET6  6

struct tcp_addr {
    uint8_t af;
    union {
        uint32_t v4;        /* host order */
        uint8_t  v6[16];    /* network order, as it appears on the wire */
    } a;
};

/* RX entry (handed to ip_input's protocol dispatch). */
void tcp_input(uint32_t src, const uint8_t *data, uint16_t len);

/* Family-general RX entry. An IPv6 input path calls this with
 * src->af == TCP_AF_INET6; `dst` is the local address the datagram was
 * addressed to (needed for the pseudo-header checksum). */
void tcp_input_af(const struct tcp_addr *src, const struct tcp_addr *dst,
                  const uint8_t *data, uint16_t len);

/* TCP's TIMER WHEEL -- retransmission timeout, delayed-ACK flush, zero-window
 * persist, FIN_WAIT/TIME_WAIT reaping, and the drain that pushes queued bytes
 * when the peer's window reopens.
 *
 * NOT a poll, and no longer called from one. A 10 ms ktimer in c/net/core/net.c
 * raises SOFTIRQ_NET and this runs in the softirq handler -- the same interrupt
 * context, IF=0, BKL-held place tcp_input() has run in since the receive path
 * moved there. It used to be driven only by net_poll() from the window
 * manager's frame loop, so every deadline in this file ran at the compositor's
 * frame rate and stopped when the compositor did. net_poll() still calls it,
 * but only to discharge a pass the timer already owed. */
void tcp_poll(void);

/* Open a connection to dst:port (host order). Returns a connection id (>=0) on
 * ESTABLISHED, or -1 on failure/timeout. Blocking (pumps net_poll). */
int  tcp_connect(uint32_t dst, uint16_t port);

/* The same open, split so it does not block: _start sends the SYN and returns
 * the id at once, _status reports 1 established / 0 pending / -1 failed. Many
 * of these progress concurrently under tcp_poll(); tcp_connect() is just the
 * two of them with a wait loop between. */
int  tcp_connect_start(uint32_t dst, uint16_t port);
int  tcp_connect_start_addr(const struct tcp_addr *dst, uint16_t port);
int  tcp_connect_status(int id);

/* Queue bytes on an established connection. Returns bytes accepted, or -1.
 * Blocking (pumps net_poll): NOT callable from a syscall or from net_poll. */
int  tcp_send(int id, const void *buf, int len);

/* Copy bytes into the send buffer, never waits: bytes taken (0 = would block,
 * the buffer is full), or -1. What actually goes on the wire, and when, is up
 * to the congestion window, the peer's window and Nagle. */
int  tcp_send_nb(int id, const void *buf, int len);

/* RFC 896 Nagle escape hatch. on != 0 disables coalescing of small writes, so
 * an interactive protocol is not held for one RTT behind unacked data. */
void tcp_set_nodelay(int id, int on);

/* Bytes readable without consuming: >0 count, 0 none yet, -1 stream finished. */
int  tcp_available(int id);

/* Copy up to max received bytes into buf. Returns the count (0 if none yet,
 * -1 if the connection is closed and drained). */
int  tcp_recv(int id, void *buf, int max);

/* Begin an orderly close (send FIN once the queued data has drained). */
void tcp_close(int id);

/* 1 if the connection is still usable (not closed/reset). */
int  tcp_alive(int id);

/* Observability: a snapshot of the congestion/negotiation state. Returns 0 on
 * success, -1 for a dead id. Used by the on-device throughput probe. */
struct tcp_info {
    uint32_t cwnd, ssthresh, snd_wnd, rcv_wnd;
    uint32_t srtt_ticks, rto_ticks;
    uint32_t retransmits, fast_retransmits, rto_events;
    uint16_t mss;
    uint8_t  snd_scale, rcv_scale;
    uint8_t  sack_ok, ts_ok, in_recovery, state;
};
int  tcp_get_info(int id, struct tcp_info *out);

/* ====================================================================== the
 * server API -- the passive open.
 *
 * A LISTENER IS NOT A CONNECTION. It has its own small table (four ports), no
 * sequence space and no buffers, because a connection slot in this stack costs
 * ~161 KiB of send/receive rings and holding a port number should not cost that.
 * `tcp_listen` returns a LISTENER id; `tcp_accept` returns a CONNECTION id,
 * usable with every call above (tcp_recv/tcp_send_nb/tcp_close/...).
 *
 * THE BLOCKING MODEL. tcp_accept never waits: it pops a completed handshake or
 * says TCP_L_E_AGAIN. tcp_accept_wait parks the calling thread on the
 * listener's wait queue -- unlinked from the run ring, not spinning, not
 * holding the BKL -- until a handshake completes or the deadline passes. The
 * split matches what pipes already do in c/kernel/exec/file.c: O_NONBLOCK picks
 * the first, an ordinary descriptor the second.
 *
 * WHAT THE LIMITS ARE, AND THAT THEY ARE ENFORCED OUT LOUD. Four listeners; a
 * backlog of at most 8 per listener; and a passive open may never take the last
 * 8 connection slots, which are reserved so an inbound peer cannot make this
 * machine unable to fetch a page. Each refusal is a RST the peer can see AND a
 * counter in tcp_server_stats -- "the server stopped answering" and "the server
 * is at its limit" look identical from outside, and the counter is what tells
 * them apart. */

#define TCP_L_E_ARG   (-1)   /* not a listener, or port 0 */
#define TCP_L_E_INUSE (-2)   /* that port is already listening or in use */
#define TCP_L_E_FULL  (-3)   /* all listener slots are taken */
#define TCP_L_E_AGAIN (-4)   /* nothing to accept yet -- NOT an error */

int  tcp_listen(uint16_t port, int backlog, int pid);
int  tcp_accept(int lid);                       /* never waits */
int  tcp_accept_wait(int lid, unsigned ms);     /* parks; same returns */
int  tcp_listen_port(int lid);
void tcp_listen_close(int lid);
void tcp_listen_close_owner(int pid);

/* Peer address/port of an accepted connection (getpeername). 0 on success. */
int  tcp_peer(int id, uint32_t *ip, uint16_t *port);

/* Park the calling thread until the connection is readable/writable, or `ms`
 * elapses. tcp_wait_readable returns what tcp_available() would; the write side
 * returns 1 room / 0 timeout / -1 dead. Both are wait-queue parks, not spins --
 * but the deadline, not the wake, is what makes them correct (the receive state
 * lives under net_lock, not under the queue's lock), so a missed wake costs
 * bounded latency and never a hang. */
int  tcp_wait_readable(int id, unsigned ms);
int  tcp_wait_writable(int id, unsigned ms);

/* shutdown(fd, SHUT_WR): send the FIN, keep receiving. Unlike tcp_close this
 * does not give the connection up -- the peer's reply still arrives. */
void tcp_shutdown_write(int id);

struct tcp_server_stats {
    uint32_t syn_received, accepted;
    uint32_t refused_backlog, refused_slots, refused_noport;
    uint32_t free_conns, listeners;
};
void tcp_server_stats(struct tcp_server_stats *s);

#endif /* LOGIT_TCP_H */
