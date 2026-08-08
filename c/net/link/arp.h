#ifndef LOGIT_ARP_H
#define LOGIT_ARP_H

#include <stdint.h>
#include "eth.h"

/* IPv4 neighbour cache. The state machine, the constants and the reasoning are
 * in arp.c; this is the surface the rest of the stack sees.
 *
 * The states are the RFC 4861 ones, shared with c/net/ip/nd.c so that the v4
 * and v6 neighbour caches age and probe identically. Exposed because
 * arp_query_state() is how a test asserts a transition rather than inferring it
 * from packet counts. */
enum {
    ARP_FREE = 0,
    ARP_INCOMPLETE,
    ARP_REACHABLE,
    ARP_STALE,
    ARP_DELAY,
    ARP_PROBE,
    ARP_FAILED,
};

/* Every drop and every decision this layer makes, counted.
 *
 * A stack you cannot see is a stack you cannot debug, and until now the entire
 * link layer had exactly one counter (frames into eth_input). None of the
 * interesting events -- a poisoning attempt refused, an address conflict, a
 * packet dropped for want of a MAC -- was observable at all: they looked
 * identical to a working network from every vantage point the OS had. */
struct arp_stats {
    uint32_t rx;              /* ARP frames delivered to arp_input */
    uint32_t rx_bad;          /* malformed, implausible, or unparseable */
    uint32_t rx_req, rx_reply;
    uint32_t rx_gratuitous;   /* request where sender == target */
    uint32_t tx_req, tx_reply;
    uint32_t tx_announce;     /* RFC 5227 announcements + conflict defences */
    uint32_t conflicts;       /* another host claiming our address */
    uint32_t poison_blocked;  /* unsolicited attempts to replace a held binding */
    uint32_t queued;          /* packets held awaiting resolution */
    uint32_t queue_sent;      /* ... later transmitted */
    uint32_t queue_drop;      /* ... discarded (timeout, overflow, or failure) */
    uint32_t resolve_fail;    /* addresses that never answered */
    uint32_t evict, expire;   /* cache slots reclaimed under pressure / by age */
};

/* Handle an incoming ARP frame: RFC 826 merge rule, RFC 5227 conflict
 * detection, and replies for our own address. */
void arp_input(const uint8_t *frame, uint16_t len);

/* Resolve `ip` (host order) to a MAC. On a usable entry, copies it into `mac`
 * and returns 0; otherwise starts resolution if none is in flight and returns
 * -1. Unlike the version this replaces it does NOT transmit on every miss --
 * retransmission is paced by arp_poll(), and an address that has failed to
 * answer is negative-cached. */
int  arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN]);

/* Resolve, or hold the frame until resolution completes and send it then.
 * Returns 0 if the packet was sent OR accepted for later delivery, -1 if it
 * could not be taken at all.
 *
 * NOT YET WIRED: c/net/ip/ip.c still drops the datagram on a miss
 * (`if (arp_resolve(nexthop, mac) != 0) return -1;`). Switching that to
 * `return arp_output(nexthop, ETHERTYPE_IP, pkt, len)` after the header is
 * built is a one-line change, but ip.c belongs to the IP line. Until it lands,
 * the queue is exercised only by the host tests. */
int  arp_output(uint32_t nexthop, uint16_t ethertype, const void *payload, uint16_t len);

/* Advance the neighbour state machine: retransmit solicitations, age entries
 * REACHABLE -> STALE, probe, expire. Called from net_poll(). */
void arp_poll(void);

/* Announce our binding to the segment (RFC 5227 s3). Sent when an address is
 * taken -- after a DHCP lease, or at boot with a static address. */
void arp_announce(void);

/* Block (pumping net_poll) until `ip` is resolvable or `timeout` ticks pass.
 * Fetch context only. */
int  arp_warm(uint32_t ip, int timeout);

/* Visibility. */
const struct arp_stats *arp_get_stats(void);
int  arp_cache_entries(void);
int  arp_query_state(uint32_t ip);   /* one of the enum above; ARP_FREE if absent */
void arp_dump(void);
void arp_reset(void);

#endif /* LOGIT_ARP_H */
