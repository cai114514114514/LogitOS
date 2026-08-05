#ifndef AETHER_ICMP_H
#define AETHER_ICMP_H

#include <stdint.h>

#define ICMP_DEST_UNREACH  3
#define ICMP_TIME_EXCEEDED 11
#define ICMP_UNREACH_PORT  3

/* Handle an ICMP message (L4 payload of an IPv4 packet). Answers echo
 * requests; matches echo replies to record the round-trip time; routes
 * destination-unreachable/time-exceeded errors to the transport layer whose
 * header they quote. */
void icmp_input(uint32_t src, const uint8_t *data, uint16_t len);

/* Send an ICMP error to `dst` (host order) quoting `quote` -- the offending
 * packet's IP header plus its first 8 L4 bytes, per RFC 792. */
void icmp_send_unreach(uint32_t dst, int type, int code,
                       const uint8_t *quote, uint16_t quote_len);

/* Send an ICMP echo request ("ping") to `dst` (host order). Returns 0 if it
 * went out, -1 if ARP is still resolving (caller retries). */
int  icmp_ping(uint32_t dst);

/* Round-trip time of the last matched reply, in PIT ticks (10 ms each); -1 if
 * no reply has arrived since the last ping. */
int  icmp_last_rtt(void);

#endif /* AETHER_ICMP_H */
