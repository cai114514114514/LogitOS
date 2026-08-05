#ifndef LOGIT_UDP_H
#define LOGIT_UDP_H

#include <stdint.h>

/* Handle an incoming UDP datagram (L4 payload of an IPv4 packet). iph points
 * at the original IP header (pseudo-header dst + ICMP error quoting). */
void udp_input(uint32_t src, const uint8_t *data, uint16_t len,
               const uint8_t *iph);

/* Socket API: NUDP sockets, each with a small FIFO of received datagrams. */
int  udp_bind(uint16_t port);           /* socket id (>=0), or -1 (in use/full) */
void udp_close(int sock);

/* Pop the oldest queued datagram. Returns its length (truncated to max), 0 if
 * the queue is empty, or -1 once if an ICMP error quoted this socket's port
 * (the error is cleared by reporting it). src/sport may be NULL. */
int  udp_recv(int sock, void *buf, int max, uint32_t *src, uint16_t *sport);

/* Send from the socket's bound port. Same return convention as udp_send. */
int  udp_send_to(int sock, uint32_t dst, uint16_t dport,
                 const void *data, uint16_t len);

/* Socket-less raw send (explicit source port). Returns 0 on success, -1 if
 * ARP is still resolving (caller retries). */
int  udp_send(uint32_t dst, uint16_t dport, uint16_t sport,
              const void *data, uint16_t len);

/* Datagrams dropped because the socket's receive queue was full. */
uint32_t udp_drops(int sock);

/* ICMP error hook (called from icmp.c, weak-linked): an error quoting one of
 * our datagrams arrived. Marks the matching socket (by local port). */
void udp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code);

#endif /* LOGIT_UDP_H */
