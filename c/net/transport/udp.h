#ifndef AETHER_UDP_H
#define AETHER_UDP_H

#include <stdint.h>

/* Handle an incoming UDP datagram (L4 payload of an IPv4 packet). */
void udp_input(uint32_t src, const uint8_t *data, uint16_t len);

/* Send a UDP datagram to dst:dport from sport (all host order except data).
 * Returns 0 on success, -1 if ARP is still resolving (caller retries). */
int  udp_send(uint32_t dst, uint16_t dport, uint16_t sport,
              const void *data, uint16_t len);

/* Register a one-shot receive slot for `port`: the next datagram to that port
 * is copied into buf (up to max) and its length recorded. Poll udp_recv_len()
 * for >= 0. Re-arm by calling udp_recv_bind() again. */
void udp_recv_bind(uint16_t port, uint8_t *buf, int max);
int  udp_recv_len(void);        /* bytes received, or -1 if nothing yet */
uint32_t udp_recv_src(void);    /* source IP of the received datagram */

#endif /* AETHER_UDP_H */
