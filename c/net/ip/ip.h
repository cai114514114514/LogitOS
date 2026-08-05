#ifndef LOGIT_IP_H
#define LOGIT_IP_H

#include <stdint.h>
#include "net.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* 1 if dst is the limited broadcast (255.255.255.255) or our subnet's
 * directed broadcast address (host part all ones). */
static inline int ip_is_broadcast(uint32_t dst)
{
    return dst == 0xFFFFFFFFu ||
           ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask) &&
            (dst | net_cfg.mask) == 0xFFFFFFFFu);
}

/* Internet checksum (ones-complement sum) over `len` bytes. */
uint16_t ip_checksum(const void *data, int len);

/* RX entry (handed to eth dispatch): validate, reassemble fragments, and
 * route to ICMP/UDP/TCP. Unicast to us only, except UDP broadcasts. */
void ip_input(const uint8_t *frame, uint16_t len);

/* Send an IPv4 packet to `dst` (host order) carrying `proto`. Resolves the
 * next-hop MAC via ARP (gateway if off-subnet); broadcast destinations go
 * straight to the broadcast MAC. Returns 0 on success, -1 if the next-hop
 * MAC isn't known yet (ARP request sent; caller retries). */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);

/* Upper-layer receive hooks (defined in icmp.c / udp.c / tcp.c; weak so
 * layers are optional). udp_input additionally gets a pointer to the
 * original IP header (needed for the pseudo-header destination -- which may
 * be a broadcast address -- and for ICMP error quoting). */
void icmp_input(uint32_t src, const uint8_t *data, uint16_t len);
void udp_input(uint32_t src, const uint8_t *data, uint16_t len,
               const uint8_t *iph);
void tcp_input(uint32_t src, const uint8_t *data, uint16_t len);

#endif /* LOGIT_IP_H */
