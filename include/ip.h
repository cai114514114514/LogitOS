#ifndef AQUA_IP_H
#define AQUA_IP_H

#include <stdint.h>

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17

/* Internet checksum (ones-complement sum) over `len` bytes. */
uint16_t ip_checksum(const void *data, int len);

/* RX entry (handed to eth dispatch): parse + route to ICMP/UDP. */
void ip_input(const uint8_t *frame, uint16_t len);

/* Send an IPv4 packet to `dst` (host order) carrying `proto`. Resolves the
 * next-hop MAC via ARP (gateway if off-subnet). Returns 0 on success, -1 if
 * the next-hop MAC isn't known yet (ARP request sent; caller retries). */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);

/* Upper-layer receive hooks (defined in icmp.c / udp.c; weak so layers are
 * optional). Called with a pointer to the L4 payload and its length, plus the
 * source IP (host order). */
void icmp_input(uint32_t src, const uint8_t *data, uint16_t len);
void udp_input(uint32_t src, const uint8_t *data, uint16_t len);

#endif /* AQUA_IP_H */
