#ifndef LOGIT_IP6_H
#define LOGIT_IP6_H

#include <stdint.h>
#include "eth.h"

/* IPv6 for LogitOS.
 *
 * WHY THIS IS NOT "IPv4 WITH A WIDER ADDRESS". Almost nothing carries over:
 * an interface has SEVERAL addresses at once, each with a scope and two
 * lifetimes; address configuration is SLAAC off router advertisements rather
 * than a DHCP transaction; address resolution is Neighbour Discovery, which is
 * ICMPv6 -- ABOVE IP, not beside it like ARP -- so it needs a source address
 * before it can run, which is why the link-local address is formed first and
 * why it must survive Duplicate Address Detection before anything may use it;
 * the transport checksum's pseudo-header is a different shape; routers never
 * fragment; and picking which of our addresses to send from is an algorithm
 * with eight rules (RFC 6724), not `net_cfg.ip`.
 *
 * References: RFC 8200 (IPv6), RFC 4291 (addressing), RFC 4443 (ICMPv6),
 * RFC 4861 (Neighbour Discovery), RFC 4862 (SLAAC + DAD), RFC 6724 (source and
 * destination selection), RFC 5952 (text representation).
 *
 * Addresses are stored as 16 bytes in NETWORK order, always. There is no host
 * order for an IPv6 address in this stack, so there is nowhere to get the
 * byte-swapping wrong. */

typedef struct { uint8_t b[16]; } ip6_addr;

/* Next-header values we care about. */
#define IP6_NH_TCP      6
#define IP6_NH_UDP      17
#define IP6_NH_ICMP6    58
#define IP6_NH_NONE     59
/* Extension headers, skipped on receive (we originate none). */
#define IP6_NH_HOPOPT   0
#define IP6_NH_ROUTING  43
#define IP6_NH_FRAGMENT 44
#define IP6_NH_DSTOPTS  60

#define IP6_HDR_LEN     40
#define IP6_MIN_MTU     1280

/* RFC 4007 scopes, in the numeric order the selection rules compare with. */
#define IP6_SCOPE_IFACE   0x1
#define IP6_SCOPE_LINK    0x2
#define IP6_SCOPE_ADMIN   0x4
#define IP6_SCOPE_SITE    0x5
#define IP6_SCOPE_ORG     0x8
#define IP6_SCOPE_GLOBAL  0xE

/* RFC 4862 address states. TENTATIVE means DAD has not finished: such an
 * address MUST NOT be used as a source or matched as a destination, which is
 * enforced in ip6_select_source() and ip6_addr_is_ours(). */
enum { IP6_TENTATIVE = 0, IP6_PREFERRED = 1, IP6_DEPRECATED = 2 };

/* ---- text form (RFC 4291 / RFC 5952) ------------------------------------ */

/* Parse an IPv6 literal (with "::" compression and the trailing-dotted-quad
 * form). Returns 1 and fills *out on success, 0 if `s` is not one. A scope-id
 * suffix ("%eth0") is rejected: there is one interface. */
int ip6_parse(const char *s, ip6_addr *out);

/* RFC 5952 canonical text: lower-case hex, no leading zeros, the LONGEST run of
 * >= 2 zero groups compressed (leftmost on a tie), IPv4-mapped printed as
 * ::ffff:a.b.c.d. Writes at most `max` bytes including the NUL; returns the
 * length written (0 if it did not fit). */
int ip6_format(const ip6_addr *a, char *out, int max);

/* ---- classification ----------------------------------------------------- */

int ip6_equal(const ip6_addr *a, const ip6_addr *b);
int ip6_is_unspecified(const ip6_addr *a);      /* :: */
int ip6_is_loopback(const ip6_addr *a);         /* ::1 */
int ip6_is_multicast(const ip6_addr *a);        /* ff00::/8 */
int ip6_is_linklocal(const ip6_addr *a);        /* fe80::/10 */
int ip6_is_v4mapped(const ip6_addr *a);         /* ::ffff:0:0/96 */
int ip6_scope(const ip6_addr *a);

/* Bits shared by a and b, counted from the top. 0..128. */
int ip6_common_prefix_len(const ip6_addr *a, const ip6_addr *b);
int ip6_prefix_eq(const ip6_addr *a, const ip6_addr *b, int plen);

/* IPv4 lives in the v6 selection algorithms as ::ffff:a.b.c.d, which is how
 * RFC 6724 compares the two families against one policy table. */
void     ip6_from_v4(uint32_t v4, ip6_addr *out);   /* v4 in HOST order */
uint32_t ip6_to_v4(const ip6_addr *a);              /* host order, 0 if not mapped */

/* The multicast Ethernet address for an IPv6 multicast destination:
 * 33:33 followed by the low 32 bits (RFC 2464 s7). */
void ip6_multicast_mac(const ip6_addr *a, uint8_t mac[ETH_ALEN]);

/* ff02::1:ffXX:XXXX -- the solicited-node multicast group a node must join for
 * each of its addresses, and the destination of a Neighbour Solicitation. */
void ip6_solicited_node(const ip6_addr *a, ip6_addr *out);

/* fe80:: + modified EUI-64 of `mac` (RFC 4291 appendix A: insert fffe, flip the
 * universal/local bit). Also used for the SLAAC interface identifier. */
void ip6_linklocal_from_mac(const uint8_t mac[ETH_ALEN], ip6_addr *out);
void ip6_eui64_into(const uint8_t mac[ETH_ALEN], ip6_addr *a);   /* low 64 bits */

/* ---- RFC 6724 ----------------------------------------------------------- */

/* Default policy table lookup (longest matching prefix). */
int ip6_policy_precedence(const ip6_addr *a);
int ip6_policy_label(const ip6_addr *a);

/* A candidate source address, as the pure selection functions see it. Kept
 * separate from struct ip6_ifaddr so the algorithm can be unit-tested with a
 * hand-built table and no interface state at all. */
struct ip6_src_cand {
    ip6_addr addr;
    uint8_t  plen;      /* prefix length the address was configured with */
    uint8_t  state;     /* IP6_TENTATIVE / PREFERRED / DEPRECATED */
};

/* RFC 6724 section 5. Returns the index of the source address to use for `dst`,
 * or -1 if no candidate is usable. Rules implemented: 1 (prefer same address),
 * 2 (prefer appropriate scope), 3 (avoid deprecated), 5 (prefer matching
 * label), 8 (longest matching prefix). Rules 4 (home vs care-of address),
 * 5.5 (prefer the outgoing interface's address) and 7 (prefer temporary
 * addresses) are inapplicable: this stack has no Mobile IPv6, exactly one
 * interface, and no RFC 4941 privacy addresses. */
int ip6_select_source(const ip6_addr *dst,
                      const struct ip6_src_cand *cand, int ncand);

/* RFC 6724 section 6: order `dst[0..n)` most-preferred first, given the source
 * addresses available. A stable insertion sort, so destinations the rules
 * cannot separate keep the order the resolver returned them in (rule 10).
 * Rules 4 and 7 are again inapplicable; rule 3 (avoid deprecated source) and
 * rule 1 (avoid unusable destinations -- no source at all) are implemented. */
void ip6_sort_dests(ip6_addr *dst, int n,
                    const struct ip6_src_cand *cand, int ncand);

/* ---- the interface's address list --------------------------------------- */

#define IP6_NADDR 8

struct ip6_ifaddr {
    ip6_addr addr;
    uint8_t  used;
    uint8_t  plen;
    uint8_t  state;         /* IP6_TENTATIVE / PREFERRED / DEPRECATED */
    uint8_t  autoconf;      /* formed by SLAAC (so an RA may re-time it) */
    uint8_t  dad_left;      /* remaining DAD solicitations to send */
    uint64_t dad_next;      /* tick of the next DAD solicitation / of the verdict */
    uint64_t pref_until;    /* ticks; IP6_LIFETIME_INF = forever */
    uint64_t valid_until;
};

#define IP6_LIFETIME_INF  (~(uint64_t)0)

extern struct ip6_ifaddr ip6_addrs[IP6_NADDR];

/* Add (or refresh the lifetimes of) an address. `pref`/`valid` are SECONDS;
 * 0xFFFFFFFF means infinity. A new address starts TENTATIVE and DAD runs before
 * anything may use it. Returns the slot, or -1 when the table is full. */
int  ip6_addr_add(const ip6_addr *a, int plen, int autoconf,
                  uint32_t pref_secs, uint32_t valid_secs);
void ip6_addr_del(const ip6_addr *a);

/* 1 if `a` is one of ours AND usable (not tentative). */
int  ip6_addr_is_ours(const ip6_addr *a);
/* 1 if `a` is ours in any state -- what DAD needs, since a tentative address
 * still has to notice a defence. */
int  ip6_addr_is_ours_any(const ip6_addr *a);

/* The source address rule 5.x would pick for `dst`, or NULL. */
const ip6_addr *ip6_source_for(const ip6_addr *dst);

/* Fill `cand` with the current usable source addresses; returns the count. */
int  ip6_src_candidates(struct ip6_src_cand *cand, int max);

/* The same, plus the interface's IPv4 address as ::ffff:a.b.c.d -- the
 * candidate set RFC 6724 destination ordering needs in order to compare an A
 * record against a AAAA record at all. */
int  ip6_dual_candidates(struct ip6_src_cand *cand, int max);

/* Bring IPv6 up on the interface: form the link-local address from the MAC,
 * start DAD, and send a Router Solicitation. Idempotent. */
void ip6_init(void);

/* Forget every address and un-initialise, so a test can bring the interface up
 * again from nothing. Pairs with nd_reset(). */
void ip6_reset(void);

/* 1 once a link-local address has passed DAD (ND can run).
 * 2 once a routable address is configured and a default router is known --
 *   the point at which it is worth asking DNS for AAAA records. */
int  ip6_up(void);

/* Timer pump. Driven from ip_poll(), which net_poll() already calls. Runs DAD,
 * neighbour-cache state transitions, router/prefix/address lifetimes and
 * Router Solicitation retransmission. */
void ip6_poll(void);

/* ---- packets ------------------------------------------------------------ */

/* RX entry, handed the whole Ethernet frame (the link-layer source is needed:
 * ND options carry it and the neighbour cache learns from it). */
void ip6_input(const uint8_t *frame, uint16_t len);

/* Send an IPv6 packet. `src` may be NULL to let source selection choose (DAD
 * passes the unspecified address explicitly). Returns 0 on success, -1 if the
 * next hop is not resolved yet (a solicitation was sent; the caller retries),
 * if there is no route, or if the payload exceeds the link MTU. */
int  ip6_output(const ip6_addr *src, const ip6_addr *dst, uint8_t nh,
                const void *payload, uint16_t len);

/* --- the two symbols c/net/transport/tcp.c weak-links against --------------
 * The TCP line made the address family a parameter (struct tcp_addr) and
 * declared these; they are the whole of the contract between the two lines, so
 * the signatures below must match tcp.c's declarations byte for byte. */
int  ip6_send(const uint8_t dst[16], uint8_t proto,
              const void *payload, uint16_t len);
int  ip6_local_addr(const uint8_t dst[16], uint8_t out[16]);

/* Ones-complement checksum over the IPv6 pseudo-header (RFC 8200 s8.1) plus
 * `len` bytes of `data`. Returns the value to store, host order. */
uint16_t ip6_pseudo_checksum(const ip6_addr *src, const ip6_addr *dst,
                             uint8_t nh, const uint8_t *data, uint32_t len);

/* ---- diagnostics -------------------------------------------------------- */

/* Counters the boot test greps for; also the fastest way to see, from a serial
 * log, which half of the stack a page load actually used. */
struct ip6_stats {
    uint32_t rx, tx, rx_bad;
    uint32_t ra, ns_rx, na_rx, ns_tx, na_tx, rs_tx;
    uint32_t dad_pass, dad_fail;
    uint32_t icmp6_err;     /* ICMPv6 errors accepted as being about OUR traffic */
};
extern struct ip6_stats ip6_stats;

/* Neighbour cache states (RFC 4861 s7.3.2). */
enum { ND_NONE = 0, ND_INCOMPLETE, ND_REACHABLE, ND_STALE, ND_DELAY, ND_PROBE };

/* Look up (and if necessary start resolving) the link-layer address for the
 * on-link neighbour `a`. 0 + mac filled on a hit, -1 while resolving. */
int  nd_resolve(const ip6_addr *a, uint8_t mac[ETH_ALEN]);
/* State of the cache entry for `a`, or ND_NONE. Exposed for the tests. */
int  nd_state(const ip6_addr *a);
/* Drop every neighbour/prefix/router entry and reset the RS state (tests). */
void nd_reset(void);

/* 1 if a prefix learned from a Router Advertisement covers `a` and was flagged
 * on-link (RFC 4861 s6.3.4), so `a` is reached directly rather than via a
 * router. */
int  nd_onlink(const ip6_addr *a);
/* The current default router, if any. 1 + *out on success. */
int  nd_default_router(ip6_addr *out);
/* Begin (or restart) Router Solicitation. */
void nd_start_rs(void);
/* ND's share of ip6_poll(): DAD, cache states, RS retransmission, lifetimes. */
void nd_poll(uint64_t now);

/* ICMPv6 receive, called by ip6_input with the pseudo-header checksum already
 * verified. `hlim` is the received hop limit: Neighbour Discovery requires 255,
 * and that check is the only thing keeping ND unspoofable from off-link. Link-
 * layer addresses come from the messages' own options, never from the frame. */
void icmp6_input(const ip6_addr *src, const ip6_addr *dst,
                 const uint8_t *data, uint16_t len, uint8_t hlim);

/* ICMPv6 echo replies seen (the Network app / tests use it as a liveness probe). */
int  nd_echo_count(void);

/* The next hop for `dst`: the destination itself when an on-link prefix covers
 * it (or it is link-local/multicast), otherwise the default router. Returns 0
 * if there is no route. */
int  ip6_next_hop(const ip6_addr *dst, ip6_addr *out);

/* Link MTU learned from the RA's MTU option, clamped to [1280, 1500]. */
uint16_t ip6_mtu(void);
void     ip6_set_mtu(uint16_t mtu);

#endif /* LOGIT_IP6_H */
