#ifndef AQUA_ICMP_H
#define AQUA_ICMP_H

#include <stdint.h>

/* Handle an ICMP message (L4 payload of an IPv4 packet). Answers echo
 * requests; matches echo replies to record the round-trip time. */
void icmp_input(uint32_t src, const uint8_t *data, uint16_t len);

/* Send an ICMP echo request ("ping") to `dst` (host order). Returns 0 if it
 * went out, -1 if ARP is still resolving (caller retries). */
int  icmp_ping(uint32_t dst);

/* Round-trip time of the last matched reply, in PIT ticks (10 ms each); -1 if
 * no reply has arrived since the last ping. */
int  icmp_last_rtt(void);

#endif /* AQUA_ICMP_H */
