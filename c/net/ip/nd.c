/* ICMPv6 and Neighbour Discovery: RFC 4443, RFC 4861, RFC 4862.
 *
 * This is where IPv6 stops resembling IPv4. ARP is a separate ethertype with a
 * flat cache and no state; Neighbour Discovery is ICMPv6 -- it runs OVER IP,
 * over multicast, with a five-state per-neighbour machine, and it carries the
 * host's entire address configuration as a side effect. One protocol here does
 * the job of ARP, ICMP redirects, router discovery and DHCP.
 *
 * The four conversations implemented:
 *
 *   Router Solicitation / Advertisement  -- "is there a router, and what prefix
 *      is on this link?" The RA's Prefix Information option with the A flag is
 *      SLAAC: the host forms its own address from the prefix and its interface
 *      identifier, with two lifetimes attached. The MTU option sets the link
 *      MTU. This replaces DHCP entirely.
 *
 *   Neighbour Solicitation / Advertisement -- address resolution, and also
 *      reachability confirmation, which ARP has no concept of. A neighbour is
 *      not "in the cache or not"; it is INCOMPLETE, REACHABLE, STALE, DELAY or
 *      PROBE, and the difference is what lets a host notice that a peer's MAC
 *      changed without waiting for a cache timeout.
 *
 *   Duplicate Address Detection -- every address, including the link-local one
 *      the host just derived from its own MAC, is TENTATIVE until it has been
 *      solicited for and nobody answered. RFC 4862 s5.4.5: an address that
 *      fails DAD MUST NOT be assigned. ip6_select_source() refuses tentative
 *      candidates, so this is enforced at the point of use and not only here.
 *
 *   Echo request/reply -- ICMPv6's, which unlike ICMPv4's carries a MANDATORY
 *      checksum over the pseudo-header. ip6_input verifies it before we see it.
 *
 * The hop-limit-255 check on every ND message is not decoration: it is the only
 * thing that makes ND off-link-unspoofable (RFC 4861 s11.2). A forged packet
 * from beyond the link cannot arrive with 255 hops left. */

#include <stdint.h>
#include <stddef.h>
#include "ip6.h"
#include "eth.h"
#include "net.h"
#include "pit.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* ---- message and option types ------------------------------------------- */

#define ICMP6_ECHO_REQUEST  128
#define ICMP6_ECHO_REPLY    129
#define ICMP6_RS            133
#define ICMP6_RA            134
#define ICMP6_NS            135
#define ICMP6_NA            136
#define ICMP6_REDIRECT      137

#define ND_OPT_SLLA         1
#define ND_OPT_TLLA         2
#define ND_OPT_PREFIX       3
#define ND_OPT_MTU          5

#define NA_ROUTER           0x80000000u
#define NA_SOLICITED        0x40000000u
#define NA_OVERRIDE         0x20000000u

/* RFC 4861 protocol constants, in 100 Hz ticks. */
#define RETRANS_TIMER       100     /* 1 s   */
#define REACHABLE_TIME      3000    /* 30 s  */
#define DELAY_FIRST_PROBE   500     /* 5 s   */
#define MAX_MCAST_SOLICIT   3
#define MAX_UCAST_SOLICIT   3
#define MAX_RTR_SOLICIT     3
#define RTR_SOLICIT_IVAL    400     /* 4 s   */

#define ND_CACHE   16
#define ND_PREFIXES 4
#define ND_ROUTERS  4

struct nd_entry {
    ip6_addr addr;
    uint8_t  mac[ETH_ALEN];
    uint8_t  used;
    uint8_t  state;
    uint8_t  is_router;
    uint8_t  probes;        /* solicitations sent in the current state */
    uint64_t timer;         /* when the current state expires */
};

struct nd_prefix {
    ip6_addr prefix;
    uint8_t  used, plen, onlink;
    uint64_t valid_until;
};

struct nd_router {
    ip6_addr addr;
    uint8_t  used;
    uint64_t valid_until;
};

static struct nd_entry  cache[ND_CACHE];
static struct nd_prefix prefixes[ND_PREFIXES];
static struct nd_router routers[ND_ROUTERS];

static int      rs_left;
static uint64_t rs_next;
static uint64_t reachable_time = REACHABLE_TIME;
static uint64_t retrans_timer  = RETRANS_TIMER;

static int      echo_seen;      /* echo replies received, for diagnostics */

/* ---- small helpers ------------------------------------------------------- */

static void fmt(const ip6_addr *a, char *out) { ip6_format(a, out, 48); }

static void unspecified(ip6_addr *a) { memset(a->b, 0, 16); }

static void all_routers(ip6_addr *a)
{
    static const uint8_t v[16] = { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x02 };
    memcpy(a->b, v, 16);
}

/* The source to use for an ND message we originate: the link-local address if
 * it has passed DAD, otherwise the unspecified address (legal, and what a host
 * bootstrapping from nothing must use -- RFC 4861 s4.1). */
static void nd_source(ip6_addr *out)
{
    for (int i = 0; i < IP6_NADDR; i++)
        if (ip6_addrs[i].used && ip6_addrs[i].state != IP6_TENTATIVE &&
            ip6_is_linklocal(&ip6_addrs[i].addr)) {
            *out = ip6_addrs[i].addr;
            return;
        }
    unspecified(out);
}

/* ---- the neighbour cache ------------------------------------------------- */

static struct nd_entry *cache_find(const ip6_addr *a)
{
    for (int i = 0; i < ND_CACHE; i++)
        if (cache[i].used && ip6_equal(&cache[i].addr, a))
            return &cache[i];
    return NULL;
}

static struct nd_entry *cache_alloc(const ip6_addr *a)
{
    struct nd_entry *e = cache_find(a);
    if (e) return e;
    for (int i = 0; i < ND_CACHE; i++)
        if (!cache[i].used) { e = &cache[i]; break; }
    if (!e) {
        /* Evict the least useful entry: an INCOMPLETE one (it has no data to
         * lose) before a STALE one, and never a REACHABLE one if avoidable. */
        int victim = 0, best = 9;
        static const int rank[] = { 0, 0, 3, 1, 2, 2 };
        for (int i = 0; i < ND_CACHE; i++) {
            int r = rank[cache[i].state <= ND_PROBE ? cache[i].state : 0];
            if (r < best) { best = r; victim = i; }
        }
        e = &cache[victim];
    }
    memset(e, 0, sizeof *e);
    e->used  = 1;
    e->addr  = *a;
    e->state = ND_INCOMPLETE;
    return e;
}

int nd_state(const ip6_addr *a)
{
    struct nd_entry *e = cache_find(a);
    return e ? e->state : ND_NONE;
}

void nd_reset(void)
{
    memset(cache, 0, sizeof cache);
    memset(prefixes, 0, sizeof prefixes);
    memset(routers, 0, sizeof routers);
    rs_left = 0;
    rs_next = 0;
    reachable_time = REACHABLE_TIME;
    retrans_timer  = RETRANS_TIMER;
    echo_seen = 0;
}

/* ---- sending ND messages ------------------------------------------------- */

/* Append a link-layer address option. */
static int put_lladdr_opt(uint8_t *o, int type)
{
    o[0] = (uint8_t)type;
    o[1] = 1;                               /* length in units of 8 bytes */
    memcpy(o + 2, net_cfg.mac, ETH_ALEN);
    return 8;
}

static void send_ns(const ip6_addr *target, const ip6_addr *dst,
                    const ip6_addr *src)
{
    uint8_t m[32];
    memset(m, 0, sizeof m);
    m[0] = ICMP6_NS;
    memcpy(m + 8, target->b, 16);
    int len = 24;
    /* RFC 4861 s4.3: a solicitation whose source is the unspecified address
     * (i.e. a DAD probe) MUST NOT carry a source link-layer address option --
     * there is no address for the peer to associate it with. */
    if (!ip6_is_unspecified(src))
        len += put_lladdr_opt(m + 24, ND_OPT_SLLA);
    uint16_t ck = ip6_pseudo_checksum(src, dst, IP6_NH_ICMP6, m, (uint32_t)len);
    m[2] = (uint8_t)(ck >> 8); m[3] = (uint8_t)ck;
    ip6_stats.ns_tx++;
    ip6_output(src, dst, IP6_NH_ICMP6, m, (uint16_t)len);
}

static void send_na(const ip6_addr *target, const ip6_addr *dst,
                    const ip6_addr *src, uint32_t flags)
{
    uint8_t m[32];
    memset(m, 0, sizeof m);
    m[0] = ICMP6_NA;
    m[4] = (uint8_t)(flags >> 24);
    memcpy(m + 8, target->b, 16);
    int len = 24 + put_lladdr_opt(m + 24, ND_OPT_TLLA);
    uint16_t ck = ip6_pseudo_checksum(src, dst, IP6_NH_ICMP6, m, (uint32_t)len);
    m[2] = (uint8_t)(ck >> 8); m[3] = (uint8_t)ck;
    ip6_stats.na_tx++;
    ip6_output(src, dst, IP6_NH_ICMP6, m, (uint16_t)len);
}

static void send_rs(void)
{
    ip6_addr src, dst;
    nd_source(&src);
    all_routers(&dst);
    uint8_t m[16];
    memset(m, 0, sizeof m);
    m[0] = ICMP6_RS;
    int len = 8;
    if (!ip6_is_unspecified(&src))
        len += put_lladdr_opt(m + 8, ND_OPT_SLLA);
    uint16_t ck = ip6_pseudo_checksum(&src, &dst, IP6_NH_ICMP6, m, (uint32_t)len);
    m[2] = (uint8_t)(ck >> 8); m[3] = (uint8_t)ck;
    ip6_stats.rs_tx++;
    ip6_output(&src, &dst, IP6_NH_ICMP6, m, (uint16_t)len);
}

void nd_start_rs(void)
{
    rs_left = MAX_RTR_SOLICIT;
    rs_next = timer_ticks();        /* first one immediately */
}

/* Solicit `target`: multicast to its solicited-node group while INCOMPLETE,
 * unicast to the cached MAC while PROBE (RFC 4861 s7.3.3). */
static void solicit(struct nd_entry *e, int unicast)
{
    ip6_addr src, dst;
    nd_source(&src);
    if (unicast) dst = e->addr;
    else         ip6_solicited_node(&e->addr, &dst);
    send_ns(&e->addr, &dst, &src);
}

/* ---- resolution ---------------------------------------------------------- */

int nd_resolve(const ip6_addr *a, uint8_t mac[ETH_ALEN])
{
    struct nd_entry *e = cache_find(a);
    if (e && (e->state == ND_REACHABLE || e->state == ND_STALE ||
              e->state == ND_DELAY || e->state == ND_PROBE)) {
        memcpy(mac, e->mac, ETH_ALEN);
        /* Sending to a STALE neighbour is allowed, but it starts the
         * reachability check that ARP has no equivalent of: DELAY gives the
         * upper layer five seconds to confirm the peer is alive (by any means)
         * before we spend a probe on it. */
        if (e->state == ND_STALE) {
            e->state = ND_DELAY;
            e->timer = timer_ticks() + DELAY_FIRST_PROBE;
            e->probes = 0;
        }
        return 0;
    }
    if (!e) {
        e = cache_alloc(a);
        e->probes = 1;
        e->timer  = timer_ticks() + retrans_timer;
        solicit(e, 0);
    }
    return -1;                              /* INCOMPLETE: caller retries */
}

/* ---- prefixes and routers ------------------------------------------------ */

int nd_onlink(const ip6_addr *a)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < ND_PREFIXES; i++) {
        if (!prefixes[i].used || !prefixes[i].onlink) continue;
        if (prefixes[i].valid_until != IP6_LIFETIME_INF &&
            now >= prefixes[i].valid_until) continue;
        if (ip6_prefix_eq(a, &prefixes[i].prefix, prefixes[i].plen)) return 1;
    }
    return 0;
}

int nd_default_router(ip6_addr *out)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < ND_ROUTERS; i++) {
        if (!routers[i].used) continue;
        if (routers[i].valid_until != IP6_LIFETIME_INF &&
            now >= routers[i].valid_until) continue;
        *out = routers[i].addr;
        return 1;
    }
    return 0;
}

static void router_update(const ip6_addr *a, uint32_t lifetime_secs)
{
    struct nd_router *slot = NULL;
    for (int i = 0; i < ND_ROUTERS; i++)
        if (routers[i].used && ip6_equal(&routers[i].addr, a)) { slot = &routers[i]; break; }
    if (lifetime_secs == 0) {               /* "I am no longer a router" */
        if (slot) slot->used = 0;
        return;
    }
    if (!slot)
        for (int i = 0; i < ND_ROUTERS; i++)
            if (!routers[i].used) { slot = &routers[i]; break; }
    if (!slot) slot = &routers[0];
    int is_new = !slot->used || !ip6_equal(&slot->addr, a);
    slot->used = 1;
    slot->addr = *a;
    slot->valid_until = timer_ticks() + (uint64_t)lifetime_secs * 100;
    if (is_new) {
        char t[48]; fmt(a, t);
        kprintf("[ip6] router %s lifetime %us\n", t, (unsigned)lifetime_secs);
    }
}

static void prefix_onlink_update(const ip6_addr *p, int plen, uint32_t valid)
{
    struct nd_prefix *slot = NULL;
    for (int i = 0; i < ND_PREFIXES; i++)
        if (prefixes[i].used && prefixes[i].plen == plen &&
            ip6_prefix_eq(&prefixes[i].prefix, p, plen)) { slot = &prefixes[i]; break; }
    if (!slot)
        for (int i = 0; i < ND_PREFIXES; i++)
            if (!prefixes[i].used) { slot = &prefixes[i]; break; }
    if (!slot) slot = &prefixes[0];
    slot->used   = 1;
    slot->prefix = *p;
    slot->plen   = (uint8_t)plen;
    slot->onlink = 1;
    slot->valid_until = (valid == 0xFFFFFFFFu)
                        ? IP6_LIFETIME_INF
                        : timer_ticks() + (uint64_t)valid * 100;
}

/* ---- SLAAC (RFC 4862 s5.5.3) --------------------------------------------- */

static void slaac_prefix(const ip6_addr *prefix, int plen,
                         uint32_t valid, uint32_t pref)
{
    /* s5.5.3 (c): a preferred lifetime greater than the valid lifetime is a
     * malformed advertisement and the option is ignored. This is a real attack
     * surface -- it is how a rogue RA would pin an address as "preferred"
     * forever -- so it is a hard reject, not a clamp. */
    if (pref > valid && valid != 0xFFFFFFFFu) return;
    if (valid == 0) return;
    /* Only a /64 can carry a 64-bit interface identifier (s5.5.3 (d)). */
    if (plen != 64) return;
    /* fe80::/10 must never be autoconfigured from an RA. */
    if (ip6_is_linklocal(prefix)) return;

    ip6_addr a = *prefix;
    for (int i = 8; i < 16; i++) a.b[i] = 0;
    ip6_eui64_into(net_cfg.mac, &a);

    int existed = ip6_addr_is_ours_any(&a);
    int slot = ip6_addr_add(&a, plen, 1, pref, valid);
    if (slot < 0) return;
    if (!existed) {
        char t[48]; fmt(&a, t);
        kprintf("[ip6] slaac %s/%d valid %us pref %us\n", t, plen,
                (unsigned)valid, (unsigned)pref);
    }
}

/* ---- DAD ----------------------------------------------------------------- */

static void dad_failed(struct ip6_ifaddr *e)
{
    char t[48]; fmt(&e->addr, t);
    kprintf("[ip6] dad failed %s -- address not assigned\n", t);
    ip6_stats.dad_fail++;
    e->used = 0;
}

/* Someone else claims `target`. If it is one of ours and still tentative, DAD
 * has failed; if it is already ours and assigned, the other node is the one at
 * fault and we defend by advertising (RFC 4862 s5.4.4). */
static void dad_conflict(const ip6_addr *target)
{
    for (int i = 0; i < IP6_NADDR; i++) {
        if (!ip6_addrs[i].used || !ip6_equal(&ip6_addrs[i].addr, target)) continue;
        if (ip6_addrs[i].state == IP6_TENTATIVE)
            dad_failed(&ip6_addrs[i]);
        return;
    }
}

static void dad_step(struct ip6_ifaddr *e, uint64_t now)
{
    if (e->state != IP6_TENTATIVE || now < e->dad_next) return;
    if (e->dad_left > 0) {
        ip6_addr src, dst;
        unspecified(&src);
        ip6_solicited_node(&e->addr, &dst);
        send_ns(&e->addr, &dst, &src);
        e->dad_left--;
        e->dad_next = now + retrans_timer;
        return;
    }
    /* Nobody objected within RetransTimer of the last probe. */
    e->state = (e->pref_until != IP6_LIFETIME_INF && now >= e->pref_until)
               ? IP6_DEPRECATED : IP6_PREFERRED;
    ip6_stats.dad_pass++;
    char t[48]; fmt(&e->addr, t);
    kprintf("[ip6] dad ok %s/%d\n", t, e->plen);
    /* The link-local address passing DAD is what makes a Router Solicitation
     * with a real source address possible, so re-arm the solicitation if no
     * router has been heard from yet. Without this the only RS ever sent is the
     * one from the unspecified address, and a router that ignores those (some
     * do) would leave the host with no global address at all. */
    ip6_addr gw;
    if (ip6_is_linklocal(&e->addr) && !nd_default_router(&gw)) {
        rs_left = MAX_RTR_SOLICIT;
        rs_next = now;
    }
}

/* ---- receive ------------------------------------------------------------- */

/* Walk the option list at `o[0..len)`. Returns 0 if well formed. Any option
 * with a zero length field is a protocol violation and the whole message is
 * discarded (RFC 4861 s4.6) -- otherwise a parser loops forever. */
static int opt_scan(const uint8_t *o, int len,
                    const uint8_t **slla, const uint8_t **tlla,
                    const uint8_t **prefix, const uint8_t **mtu)
{
    int i = 0;
    while (i + 2 <= len) {
        int olen = o[i + 1] * 8;
        if (olen == 0 || i + olen > len) return -1;
        switch (o[i]) {
        case ND_OPT_SLLA:   if (olen >= 8  && slla)   *slla   = o + i; break;
        case ND_OPT_TLLA:   if (olen >= 8  && tlla)   *tlla   = o + i; break;
        case ND_OPT_PREFIX: if (olen >= 32 && prefix) *prefix = o + i; break;
        case ND_OPT_MTU:    if (olen >= 8  && mtu)    *mtu    = o + i; break;
        default: break;                     /* unknown options are ignored */
        }
        i += olen;
    }
    return i == len ? 0 : -1;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Record a neighbour's link-layer address learned from a solicitation or
 * advertisement. `solicited`/`override` follow RFC 4861 s7.2.5. */
static void neighbour_update(const ip6_addr *a, const uint8_t *mac,
                             int solicited, int override, int is_router)
{
    struct nd_entry *e = cache_find(a);
    if (!e) {
        if (!mac) return;                   /* nothing to learn */
        e = cache_alloc(a);
        memcpy(e->mac, mac, ETH_ALEN);
        e->state = solicited ? ND_REACHABLE : ND_STALE;
        e->timer = timer_ticks() + reachable_time;
        e->is_router = (uint8_t)is_router;
        goto announce;
    }
    if (e->state == ND_INCOMPLETE) {
        if (!mac) return;
        memcpy(e->mac, mac, ETH_ALEN);
        e->state = solicited ? ND_REACHABLE : ND_STALE;
        e->timer = timer_ticks() + reachable_time;
        e->is_router = (uint8_t)is_router;
        goto announce;
    }
    int differs = 0;
    if (mac)
        for (int i = 0; i < ETH_ALEN; i++)
            if (e->mac[i] != mac[i]) { differs = 1; break; }
    if (mac && differs && !override) {
        /* A non-override advertisement that disagrees with what we hold does
         * not get to rewrite it; it only casts doubt, so the entry goes STALE
         * and the next send re-probes. */
        if (e->state == ND_REACHABLE) { e->state = ND_STALE; e->probes = 0; }
        return;
    }
    if (mac && differs) memcpy(e->mac, mac, ETH_ALEN);
    if (is_router) e->is_router = 1;
    if (solicited) {
        e->state = ND_REACHABLE;
        e->timer = timer_ticks() + reachable_time;
        e->probes = 0;
    } else if (mac && differs) {
        e->state = ND_STALE;
        e->probes = 0;
    }
    return;
announce:
    {
        char t[48]; fmt(a, t);
        kprintf("[ip6] nd %s -> %02x:%02x:%02x:%02x:%02x:%02x %s\n", t,
                e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                e->state == ND_REACHABLE ? "reachable" : "stale");
    }
}

static void handle_ns(const ip6_addr *src, const ip6_addr *dst,
                      const uint8_t *m, uint16_t len)
{
    if (len < 24) { ip6_stats.rx_bad++; return; }
    ip6_addr target;
    memcpy(target.b, m + 8, 16);
    if (ip6_is_multicast(&target)) { ip6_stats.rx_bad++; return; }

    const uint8_t *slla = NULL;
    if (opt_scan(m + 24, len - 24, &slla, NULL, NULL, NULL) != 0) {
        ip6_stats.rx_bad++;
        return;
    }
    ip6_stats.ns_rx++;

    int from_dad = ip6_is_unspecified(src);
    if (from_dad) {
        /* A DAD probe: it must be addressed to the target's solicited-node
         * group and must not carry a source link-layer address (s7.1.1). */
        ip6_addr sn;
        ip6_solicited_node(&target, &sn);
        if (!ip6_equal(dst, &sn) || slla) { ip6_stats.rx_bad++; return; }
        /* Somebody is probing for an address. If it is ours and tentative, we
         * both lose it; if it is ours and assigned, defend it. */
        dad_conflict(&target);
        if (ip6_addr_is_ours(&target)) {
            ip6_addr allnodes, me;
            static const uint8_t an[16] =
                { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01 };
            memcpy(allnodes.b, an, 16);
            me = target;
            send_na(&target, &allnodes, &me, NA_OVERRIDE);
        }
        return;
    }

    if (slla) neighbour_update(src, slla + 2, 0, 0, 0);

    if (!ip6_addr_is_ours(&target)) return;     /* not for us to answer */
    send_na(&target, src, &target, NA_SOLICITED | NA_OVERRIDE);
}

static void handle_na(const ip6_addr *src, const ip6_addr *dst,
                      const uint8_t *m, uint16_t len)
{
    if (len < 24) { ip6_stats.rx_bad++; return; }
    uint32_t flags = be32(m + 4);
    ip6_addr target;
    memcpy(target.b, m + 8, 16);
    if (ip6_is_multicast(&target)) { ip6_stats.rx_bad++; return; }
    /* s7.1.2: a multicast advertisement cannot be a solicited one. */
    if (ip6_is_multicast(dst) && (flags & NA_SOLICITED)) {
        ip6_stats.rx_bad++;
        return;
    }
    const uint8_t *tlla = NULL;
    if (opt_scan(m + 24, len - 24, NULL, &tlla, NULL, NULL) != 0) {
        ip6_stats.rx_bad++;
        return;
    }
    ip6_stats.na_rx++;
    (void)src;

    /* Someone else advertising an address we are still probing for means the
     * address is taken -- this is the ordinary DAD failure path. */
    dad_conflict(&target);

    neighbour_update(&target, tlla ? tlla + 2 : NULL,
                     (flags & NA_SOLICITED) != 0,
                     (flags & NA_OVERRIDE) != 0,
                     (flags & NA_ROUTER) != 0);
}

static void handle_ra(const ip6_addr *src, const uint8_t *m, uint16_t len)
{
    /* s6.1.2: the source of a Router Advertisement MUST be link-local. A router
     * that announces itself from a global address is not one we may trust with
     * this host's entire address configuration. */
    if (len < 16 || !ip6_is_linklocal(src)) { ip6_stats.rx_bad++; return; }

    const uint8_t *slla = NULL, *pio = NULL, *mtuopt = NULL;
    if (opt_scan(m + 16, len - 16, &slla, NULL, &pio, &mtuopt) != 0) {
        ip6_stats.rx_bad++;
        return;
    }
    ip6_stats.ra++;

    uint32_t rtr_life = ((uint32_t)m[6] << 8) | m[7];
    uint32_t reach    = be32(m + 8);
    uint32_t retrans  = be32(m + 12);

    if (slla) neighbour_update(src, slla + 2, 0, 1, 1);
    router_update(src, rtr_life);

    /* s6.3.4: a nonzero ReachableTime/RetransTimer in the RA replaces ours. */
    if (reach && reach <= 3600000u) reachable_time = reach / 10;    /* ms -> ticks */
    if (retrans && retrans <= 3600000u) retrans_timer = retrans / 10;
    if (reachable_time == 0) reachable_time = REACHABLE_TIME;
    if (retrans_timer == 0)  retrans_timer  = RETRANS_TIMER;

    if (mtuopt) ip6_set_mtu((uint16_t)be32(mtuopt + 4));

    if (pio) {
        int plen   = pio[2];
        int onlink = (pio[3] & 0x80) != 0;
        int autoc  = (pio[3] & 0x40) != 0;
        uint32_t valid = be32(pio + 4), pref = be32(pio + 8);
        ip6_addr prefix;
        memcpy(prefix.b, pio + 16, 16);
        if (plen > 128) { ip6_stats.rx_bad++; return; }
        if (onlink && !ip6_is_linklocal(&prefix))
            prefix_onlink_update(&prefix, plen, valid);
        if (autoc)
            slaac_prefix(&prefix, plen, valid, pref);
    }

    /* One RA answers the solicitation; stop soliciting. */
    rs_left = 0;
}

static void handle_echo(const ip6_addr *src, const ip6_addr *dst,
                        const uint8_t *m, uint16_t len)
{
    if (len > 1400) return;                 /* do not amplify */
    uint8_t r[1408];
    memcpy(r, m, len);
    r[0] = ICMP6_ECHO_REPLY;
    r[2] = r[3] = 0;
    /* The reply's source must be the address the request was sent TO, unless it
     * was multicast -- in which case selection picks one (RFC 4443 s4.2). */
    ip6_addr from;
    if (ip6_is_multicast(dst)) {
        const ip6_addr *s = ip6_source_for(src);
        if (!s) return;
        from = *s;
    } else {
        from = *dst;
    }
    uint16_t ck = ip6_pseudo_checksum(&from, src, IP6_NH_ICMP6, r, len);
    r[2] = (uint8_t)(ck >> 8); r[3] = (uint8_t)ck;
    ip6_output(&from, src, IP6_NH_ICMP6, r, len);
}

void icmp6_input(const ip6_addr *src, const ip6_addr *dst,
                 const uint8_t *data, uint16_t len, uint8_t hlim)
{
    if (len < 4) { ip6_stats.rx_bad++; return; }
    uint8_t type = data[0], code = data[1];

    switch (type) {
    case ICMP6_NS:
    case ICMP6_NA:
    case ICMP6_RA:
    case ICMP6_RS:
    case ICMP6_REDIRECT:
        /* RFC 4861 s11.2: every ND message must arrive with a hop limit of 255
         * and code 0. The hop limit is THE off-link forgery defence -- a router
         * decrements it, so a packet that still has 255 left cannot have
         * crossed one. Dropping here rather than trusting is the difference
         * between "we do ND" and "anyone on the Internet can reconfigure this
         * host's addresses". */
        if (hlim != 255 || code != 0) { ip6_stats.rx_bad++; return; }
        break;
    default:
        break;
    }

    switch (type) {
    case ICMP6_NS:  handle_ns(src, dst, data, len); break;
    case ICMP6_NA:  handle_na(src, dst, data, len); break;
    case ICMP6_RA:  handle_ra(src, data, len); break;
    case ICMP6_ECHO_REQUEST: handle_echo(src, dst, data, len); break;
    case ICMP6_ECHO_REPLY:   echo_seen++; break;
    case ICMP6_RS:  break;                  /* we are not a router */
    case ICMP6_REDIRECT: break;             /* single router; nothing to redirect to */
    default: break;                         /* errors: no action beyond counting */
    }
}

int nd_echo_count(void) { return echo_seen; }

/* ---- the pump ------------------------------------------------------------ */

void nd_poll(uint64_t now)
{
    /* Duplicate Address Detection for every tentative address. */
    for (int i = 0; i < IP6_NADDR; i++)
        if (ip6_addrs[i].used && ip6_addrs[i].state == IP6_TENTATIVE)
            dad_step(&ip6_addrs[i], now);

    /* Router Solicitation, until an RA answers (handle_ra clears rs_left). */
    if (rs_left > 0 && now >= rs_next) {
        send_rs();
        rs_left--;
        rs_next = now + RTR_SOLICIT_IVAL;
    }

    /* Neighbour cache state machine (RFC 4861 s7.3.3). */
    for (int i = 0; i < ND_CACHE; i++) {
        struct nd_entry *e = &cache[i];
        if (!e->used || now < e->timer) continue;
        switch (e->state) {
        case ND_INCOMPLETE:
            if (e->probes >= MAX_MCAST_SOLICIT) {
                e->used = 0;                /* unreachable; let the caller fail */
                break;
            }
            e->probes++;
            e->timer = now + retrans_timer;
            solicit(e, 0);
            break;
        case ND_REACHABLE:
            /* Reachability is a claim with an expiry date, which is the whole
             * difference from an ARP cache: we stop asserting it rather than
             * keep using a possibly-dead entry. */
            e->state = ND_STALE;
            e->probes = 0;
            break;
        case ND_DELAY:
            e->state = ND_PROBE;
            e->probes = 1;
            e->timer = now + retrans_timer;
            solicit(e, 1);
            break;
        case ND_PROBE:
            if (e->probes >= MAX_UCAST_SOLICIT) { e->used = 0; break; }
            e->probes++;
            e->timer = now + retrans_timer;
            solicit(e, 1);
            break;
        default:
            break;
        }
    }

    /* Expire routers and prefixes. */
    for (int i = 0; i < ND_ROUTERS; i++)
        if (routers[i].used && routers[i].valid_until != IP6_LIFETIME_INF &&
            now >= routers[i].valid_until)
            routers[i].used = 0;
    for (int i = 0; i < ND_PREFIXES; i++)
        if (prefixes[i].used && prefixes[i].valid_until != IP6_LIFETIME_INF &&
            now >= prefixes[i].valid_until)
            prefixes[i].used = 0;
}
