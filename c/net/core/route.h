#ifndef LOGIT_ROUTE_H
#define LOGIT_ROUTE_H

#include <stdint.h>

/* The IPv4 forwarding table.
 *
 * WHAT IT REPLACES, exactly: one ternary in c/net/ip/ip.c that was the entire
 * routing decision this machine could make --
 *
 *     nexthop = ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask))
 *               ? dst : net_cfg.gw;
 *
 * on-link goes direct, everything else goes to THE gateway. That expression
 * cannot express a second subnet, a second card, a host route, a blackhole, or
 * a loopback interface, and it cannot say "no route" -- an unreachable
 * destination came out of it as the gateway's problem, which is why 127.0.0.1
 * used to be ARP'd to the gateway and put on the wire.
 *
 * DESIGN, and the one thing to know before reading further: THIS FILE KNOWS
 * ONLY INTEGERS. No device, no lock, no clock, no kprintf, nothing from
 * c/drivers. An interface is an `int oif` and nothing else. That is deliberate
 * and copied from c/net/ip/ip6_addr.c, which is the half of the IPv6 stack
 * that is all rules and no I/O and is therefore the half that can be
 * exhaustively unit-tested on the host -- tests/unit/route_test.c drives every
 * function below with a hand-built table and no kernel at all.
 *
 * The rejected alternative was `struct netif *oif`. It costs nothing in the
 * kernel and costs the gate everything: route.c would then include netdev.h,
 * which includes driver.h, and the host test would be linking the PCI device
 * model to ask which of two prefixes is longer.
 *
 * A DEFAULT ROUTE IS NOT A SPECIAL CASE. It is prefix length 0, matched and
 * ranked by the same loop as every other route. Making it a field
 * (`net_cfg.gw`) is precisely how this tree ended up with a routing decision
 * that had exactly one answer.
 *
 * WHERE net_cfg FITS. `struct net_config` (c/net/core/net.h) is a single
 * global ip/mask/gw read by 79 sites across 20 files, and it is not this
 * change's job to move them. It stays, and it stays authoritative for the
 * primary interface; route_v4_iface() is the one-way bridge that turns an
 * address + mask + gateway into the two table entries they imply (a connected
 * route and, if there is a gateway, a default route). ip_send() calls it on
 * every send and it is a no-op unless the configuration actually moved, so the
 * table cannot go stale behind DHCP. When net_cfg's owner is ready, that call
 * becomes route_add() at the two places that configure an address and this
 * paragraph goes away.
 */

/* Table size. 16 is not a guess about the internet: this machine has at most
 * NETIF_MAX interfaces, each contributing a connected route and possibly a
 * default, so 16 leaves room for roughly twice what the interfaces generate
 * plus hand-installed host routes. A full table refuses (RT_EFULL) rather than
 * evicting -- silently dropping a route is how a machine becomes unreachable
 * in a way nothing reports. */
#define RT_NROUTE 16

/* Highest interface index route.c will remember a configuration for. Must be
 * >= NETIF_MAX in netdev.h; the two are checked against each other by a
 * _Static_assert in netdev.h rather than being wished at each other here. */
#define RT_NIF 8

/* Interface indices netdev_init() assigns, named here because c/net/ip/ip.c
 * needs them when the driver layer is not linked (a host test), and because a
 * bare 1 and 2 in three files is three chances to disagree. netdev_init()
 * registers the loopback interface FIRST so that RT_OIF_LO holds by
 * construction rather than by comment. */
#define RT_OIF_LO    1
#define RT_OIF_NIC0  2

/* Route flags. */
#define RT_F_LOCAL  0x1u   /* deliver on this interface; do NOT resolve a
                            * neighbour. The loopback interface's flag: there
                            * is no MAC to ARP for 127.0.0.1, and asking would
                            * put a solicitation for it on the wire. */

/* Return codes. Every one of them is distinguishable, which is the point of
 * the whole file: RT_ENOROUTE is NOT "use the gateway". */
#define RT_OK         0
#define RT_ENOROUTE (-1)   /* nothing in the table matches */
#define RT_EFULL    (-2)   /* no free slot */
#define RT_EINVAL   (-3)   /* bad prefix length, interface index, or netmask */

/* One row. Passed BY VALUE to route_add so a caller -- and above all the
 * table-driven gate -- can write a designated initialiser and leave every
 * field it does not care about at zero:
 *
 *     route_add((struct route_entry){ .dst = IPV4(10,0,2,0), .plen = 24,
 *                                     .oif = RT_OIF_NIC0 });
 *
 * 24 bytes by value costs a register pair's worth of copying on a path that
 * runs once per configuration change, not per packet. */
struct route_entry {
    uint32_t dst;       /* network prefix, HOST order; masked to plen on insert */
    uint32_t nexthop;   /* 0 == on-link: the next hop IS the destination */
    uint32_t src;       /* preferred source address (0 = caller decides) */
    uint32_t flags;     /* RT_F_* */
    int      oif;       /* interface index, must be > 0 */
    int      metric;    /* lower wins between routes of EQUAL prefix length */
    uint8_t  plen;      /* 0..32; 0 is the default route and nothing more */
};

/* What a lookup answers. `nexthop` is RESOLVED -- for an on-link route it is
 * the destination itself, so the caller never has to re-derive it and cannot
 * get that derivation wrong in a fifth place. */
struct route_res {
    int      oif;
    uint32_t nexthop;
    uint32_t src;
    uint32_t flags;
    int      metric;
    uint8_t  plen;      /* which prefix length won: what a test asserts on */
};

/* 0xFFFFFFFF << (32-plen), except that plen 0 must yield 0 and the shift count
 * would be 32, which is undefined. That one case is why this is a function. */
uint32_t route_plen_mask(int plen);

/* Contiguous netmask -> prefix length, or -1 for a mask with a hole in it
 * (0xFFFF00FF). Refused rather than "corrected": a caller that configured
 * 255.255.0.255 meant something, and guessing which half is a fabricated
 * answer to a question the caller got wrong. */
int route_mask_plen(uint32_t mask);

/* Install (or update in place) a route. Two rows are THE SAME ROUTE when
 * dst/plen/oif/nexthop all match -- then src, flags and metric are overwritten,
 * which is what makes reconfiguration idempotent. Two rows differing only in
 * metric are two routes on purpose (that is the whole point of a metric).
 * Returns RT_OK / RT_EINVAL / RT_EFULL. */
int route_add(struct route_entry r);

/* Remove routes matching dst/plen; oif <= 0 means "on any interface".
 * Returns the number removed, or RT_ENOROUTE when that is zero. */
int route_del(uint32_t dst, int plen, int oif);

/* THE LOOKUP. Longest prefix wins; among equal prefix lengths the lower metric
 * wins; among equal metrics the earlier-installed route wins, so the answer is
 * stable and a test can assert on it. Returns RT_OK or RT_ENOROUTE. */
int route_lookup(uint32_t dst, struct route_res *out);

/* Everything an address on an interface implies, in one call: a connected
 * route for (addr & mask), and -- when gw is nonzero -- a default route via it.
 * Both carry `addr` as their preferred source and `flags` as their flags.
 *
 * Idempotent and CHEAP: it memoises the last (addr, mask, gw, flags) per
 * interface and returns immediately when nothing moved, which is what lets
 * ip_send() call it on every packet instead of every layer above it having to
 * remember to re-sync after DHCP. When something DID move, the interface's
 * previous rows are withdrawn first, so a lease change cannot leave the old
 * subnet in the table.
 *
 * Returns RT_OK (whether or not anything changed -- compare route_generation()
 * to find out), or RT_EINVAL / RT_EFULL. */
int route_v4_iface(int oif, uint32_t addr, uint32_t mask, uint32_t gw,
                   uint32_t flags);

/* Bumped by every insertion, update and removal. A caller that wants to log
 * the table only when it changes compares this instead of diffing rows. */
uint32_t route_generation(void);

/* Withdraw everything (route_flush_if: everything on one interface, including
 * its memoised configuration so the next route_v4_iface reinstalls). */
void route_flush(void);
void route_flush_if(int oif);

/* Enumeration, for whoever prints the table. route_at() returns NULL for an
 * empty slot; the index space is the raw slot space, so callers walk
 * 0..RT_NROUTE and skip NULLs rather than being handed a compacted view that
 * would change under them. */
const struct route_entry *route_at(int slot);
int route_count(void);

#endif /* LOGIT_ROUTE_H */
