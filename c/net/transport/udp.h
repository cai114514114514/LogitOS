#ifndef AETHER_UDP_H
#define AETHER_UDP_H

#include <stdint.h>

/* Handle an incoming UDP datagram (L4 payload of an IPv4 packet). iph points
 * at the original IP header (pseudo-header dst + ICMP error quoting). */
void udp_input(uint32_t src, const uint8_t *data, uint16_t len,
               const uint8_t *iph);

/* Send a UDP datagram to dst:dport from sport (all host order except data).
 * Returns 0 on success, -1 if ARP is still resolving (caller retries). */
int  udp_send(uint32_t dst, uint16_t dport, uint16_t sport,
              const void *data, uint16_t len);

/* Register a one-shot receive slot for `port`: the next datagram to that port
 * is copied into buf (up to max) and its length recorded. Poll udp_recv_len()
 * for >= 0. Re-arm by calling udp_recv_bind() again. */
void udp_recv_bind(uint16_t port, uint8_t *buf, int max);
int  udp_recv_len(void);        /* bytes received, or -1 if nothing yet */
int  udp_recv_err(void);        /* nonzero if an ICMP error hit the armed slot */
uint32_t udp_recv_src(void);    /* source IP of the received datagram */
uint16_t udp_recv_sport(void);  /* source port of the received datagram */

/* ICMP error hook (called from icmp.c, weak-linked): an error quoting one of
 * our datagrams arrived. Marks the armed receive slot as failed. */
void udp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code);

#endif /* AETHER_UDP_H */
