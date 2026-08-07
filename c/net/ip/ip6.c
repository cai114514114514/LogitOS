/* IPv6: the interface's address list, packet input/output, and the pumps.
 *
 * The shape that is NOT like IPv4:
 *
 *  - There is no `net_cfg.ip`. An interface holds SEVERAL addresses at once --
 *    always a link-local one, usually one or more routable ones -- each with a
 *    state (RFC 4862) and two lifetimes. Which of them sources a packet is
 *    decided per destination by RFC 6724, in ip6_addr.c.
 *
 *  - The link-local address is formed first and from the MAC alone, because
 *    Neighbour Discovery runs OVER ICMPv6 and therefore needs a source address
 *    before it can resolve anything. Bootstrapping is: link-local -> DAD ->
 *    Router Solicitation -> RA -> SLAAC global address -> DAD -> routable.
 *
 *  - Nothing on the path fragments. A datagram larger than the link MTU is the
 *    sender's problem, so ip6_output refuses it rather than truncating; TCP's
 *    RFC 1191 path-MTU logic then clamps and retries, which is the designed
 *    behaviour rather than a workaround.
 *
 *  - Multicast is load-bearing, not optional: the all-nodes group carries
 *    router advertisements and the solicited-node group carries the address
 *    resolution requests aimed at us. Both must be accepted by the NIC.
 *
 * IPv6 is brought up lazily from ip6_poll() -- which net_poll() already reaches
 * through ip_poll() -- rather than from net_init(), so this line needed no edit
 * to c/net/core/net.c. */

#include <stdint.h>
#include <stddef.h>
#include "ip6.h"
#include "ip.h"
#include "eth.h"
#include "net.h"
#include "pit.h"
#include "tcp.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

struct ip6_hdr {
    uint32_t vtcfl;     /* version(4) | traffic class(8) | flow label(20) */
    uint16_t plen;
    uint8_t  nh;
    uint8_t  hlim;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed));

struct ip6_ifaddr ip6_addrs[IP6_NADDR];
struct ip6_stats  ip6_stats;

static uint16_t link_mtu = 1500;
static int      inited;

uint16_t ip6_mtu(void) { return link_mtu; }

void ip6_set_mtu(uint16_t mtu)
{
    /* RFC 8200: 1280 is the IPv6 minimum link MTU and a router may not advertise
     * less. Larger than the Ethernet payload is meaningless here. */
    if (mtu < IP6_MIN_MTU || mtu > 1500) return;
    link_mtu = mtu;
}

/* ---- the address list --------------------------------------------------- */

static uint64_t lifetime_deadline(uint32_t secs)
{
    if (secs == 0xFFFFFFFFu) return IP6_LIFETIME_INF;
    return timer_ticks() + (uint64_t)secs * 100;    /* 100 Hz */
}

static struct ip6_ifaddr *find_ifaddr(const ip6_addr *a)
{
    for (int i = 0; i < IP6_NADDR; i++)
        if (ip6_addrs[i].used && ip6_equal(&ip6_addrs[i].addr, a))
            return &ip6_addrs[i];
    return NULL;
}

int ip6_addr_add(const ip6_addr *a, int plen, int autoconf,
                 uint32_t pref_secs, uint32_t valid_secs)
{
    struct ip6_ifaddr *e = find_ifaddr(a);
    if (e) {
        /* RFC 4862 s5.5.3: a repeated advertisement just re-times the address.
         * It does NOT re-run DAD, and it must not knock a preferred address
         * back to tentative -- that would break every connection using it. */
        e->pref_until  = lifetime_deadline(pref_secs);
        e->valid_until = lifetime_deadline(valid_secs);
        if (e->state == IP6_DEPRECATED && pref_secs)
            e->state = IP6_PREFERRED;
        return (int)(e - ip6_addrs);
    }
    for (int i = 0; i < IP6_NADDR; i++) {
        if (ip6_addrs[i].used) continue;
        e = &ip6_addrs[i];
        memset(e, 0, sizeof *e);
        e->addr        = *a;
        e->used        = 1;
        e->plen        = (uint8_t)plen;
        e->autoconf    = (uint8_t)!!autoconf;
        e->state       = IP6_TENTATIVE;
        e->dad_left    = 1;         /* RFC 4862 DupAddrDetectTransmits */
        e->dad_next    = timer_ticks();
        e->pref_until  = lifetime_deadline(pref_secs);
        e->valid_until = lifetime_deadline(valid_secs);
        return i;
    }
    return -1;
}

void ip6_addr_del(const ip6_addr *a)
{
    struct ip6_ifaddr *e = find_ifaddr(a);
    if (e) e->used = 0;
}

int ip6_addr_is_ours(const ip6_addr *a)
{
    struct ip6_ifaddr *e = find_ifaddr(a);
    return e && e->state != IP6_TENTATIVE;
}

int ip6_addr_is_ours_any(const ip6_addr *a)
{
    return find_ifaddr(a) != NULL;
}

int ip6_src_candidates(struct ip6_src_cand *cand, int max)
{
    int n = 0;
    for (int i = 0; i < IP6_NADDR && n < max; i++) {
        if (!ip6_addrs[i].used) continue;
        cand[n].addr  = ip6_addrs[i].addr;
        cand[n].plen  = ip6_addrs[i].plen;
        cand[n].state = ip6_addrs[i].state;
        n++;
    }
    return n;
}

/* The candidate set RFC 6724 actually wants when both families are in play:
 * our IPv6 addresses PLUS the IPv4 address as ::ffff:a.b.c.d. Selection and
 * destination ordering compare the two families against one policy table, so
 * they have to see one list. Kept separate from ip6_src_candidates() because
 * the pure IPv6 paths (ip6_output's source selection) must not be handed a
 * v4-mapped candidate they would then have to filter out again. */
int ip6_dual_candidates(struct ip6_src_cand *cand, int max)
{
    int n = ip6_src_candidates(cand, max);
    if (n < max && net_cfg.ip) {
        ip6_from_v4(net_cfg.ip, &cand[n].addr);
        cand[n].plen = 96;
        cand[n].state = IP6_PREFERRED;
        n++;
    }
    return n;
}

const ip6_addr *ip6_source_for(const ip6_addr *dst)
{
    struct ip6_src_cand cand[IP6_NADDR];
    int n = ip6_src_candidates(cand, IP6_NADDR);
    int i = ip6_select_source(dst, cand, n);
    if (i < 0) return NULL;
    /* Return a pointer into the real table, not into `cand` (a local). */
    for (int k = 0; k < IP6_NADDR; k++)
        if (ip6_addrs[k].used && ip6_equal(&ip6_addrs[k].addr, &cand[i].addr))
            return &ip6_addrs[k].addr;
    return NULL;
}

/* The TCP line's source-selection hook. */
int ip6_local_addr(const uint8_t dst[16], uint8_t out[16])
{
    ip6_addr d;
    memcpy(d.b, dst, 16);
    const ip6_addr *s = ip6_source_for(&d);
    if (!s) return -1;
    memcpy(out, s->b, 16);
    return 0;
}

/* ---- multicast membership ----------------------------------------------- */

/* The groups a host joins (RFC 4291 s2.8): the all-nodes group, and the
 * solicited-node group of EVERY address it holds -- including tentative ones,
 * since that is the group a DAD defence would arrive on. */
static int joined_group(const ip6_addr *a)
{
    static const uint8_t all_nodes[16] =
        { 0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01 };
    int eq = 1;
    for (int i = 0; i < 16; i++) if (a->b[i] != all_nodes[i]) { eq = 0; break; }
    if (eq) return 1;
    for (int i = 0; i < IP6_NADDR; i++) {
        if (!ip6_addrs[i].used) continue;
        ip6_addr sn;
        ip6_solicited_node(&ip6_addrs[i].addr, &sn);
        if (ip6_equal(a, &sn)) return 1;
    }
    return 0;
}

/* ---- checksums ---------------------------------------------------------- */

uint16_t ip6_pseudo_checksum(const ip6_addr *src, const ip6_addr *dst,
                             uint8_t nh, const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (int i = 0; i < 16; i += 2)
        sum += ((uint32_t)src->b[i] << 8) | src->b[i + 1];
    for (int i = 0; i < 16; i += 2)
        sum += ((uint32_t)dst->b[i] << 8) | dst->b[i + 1];
    sum += (len >> 16) & 0xFFFF;
    sum += len & 0xFFFF;
    sum += nh;                              /* zero-padded, per RFC 8200 s8.1 */
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (len & 1)
        sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- output ------------------------------------------------------------- */

int ip6_next_hop(const ip6_addr *dst, ip6_addr *out)
{
    if (ip6_is_multicast(dst) || ip6_is_linklocal(dst)) { *out = *dst; return 1; }
    if (nd_onlink(dst))                                 { *out = *dst; return 1; }
    return nd_default_router(out);
}

int ip6_output(const ip6_addr *src, const ip6_addr *dst, uint8_t nh,
               const void *payload, uint16_t len)
{
    ip6_addr from;
    if (src) {
        from = *src;
    } else {
        const ip6_addr *s = ip6_source_for(dst);
        if (!s) return -1;                  /* no usable source: unroutable */
        from = *s;
    }

    /* Routers do not fragment IPv6 (RFC 8200 s4.5), so an oversized datagram is
     * refused here rather than silently cut. TCP treats the failure as a lost
     * segment and its RFC 1191 clamp brings the segment size down. */
    if ((uint32_t)len + IP6_HDR_LEN > link_mtu)
        return -1;

    uint8_t mac[ETH_ALEN];
    if (ip6_is_multicast(dst)) {
        ip6_multicast_mac(dst, mac);
    } else {
        ip6_addr hop;
        if (!ip6_next_hop(dst, &hop))
            return -1;
        if (nd_resolve(&hop, mac) != 0)
            return -1;                      /* solicitation sent; caller retries */
    }

    uint8_t pkt[IP6_HDR_LEN + 1500];
    struct ip6_hdr *h = (struct ip6_hdr *)pkt;
    h->vtcfl = htonl(0x60000000u);
    h->plen  = htons(len);
    h->nh    = nh;
    /* 255 for everything: Neighbour Discovery REQUIRES it (RFC 4861 s11.2 --
     * an attacker off-link cannot forge a packet that still has 255 left), and
     * for ordinary traffic it is simply the default. */
    h->hlim  = 255;
    memcpy(h->src, from.b, 16);
    memcpy(h->dst, dst->b, 16);
    memcpy(pkt + IP6_HDR_LEN, payload, len);
    ip6_stats.tx++;
    return eth_send(mac, ETHERTYPE_IPV6, pkt, (uint16_t)(IP6_HDR_LEN + len));
}

/* The symbol c/net/transport/tcp.c weak-links against. */
int ip6_send(const uint8_t dst[16], uint8_t proto,
             const void *payload, uint16_t len)
{
    ip6_addr d;
    memcpy(d.b, dst, 16);
    return ip6_output(NULL, &d, proto, payload, len);
}

/* ---- input -------------------------------------------------------------- */

/* Walk past the extension headers we tolerate. Returns the transport next
 * header and advances the offset/remaining pair, or -1 to drop. We originate no extension
 * headers, so this only has to survive what a peer might send. */
static int skip_ext(const uint8_t *p, uint32_t *off, uint32_t *len, uint8_t nh)
{
    for (int guard = 0; guard < 8; guard++) {
        switch (nh) {
        case IP6_NH_HOPOPT:
        case IP6_NH_ROUTING:
        case IP6_NH_DSTOPTS: {
            if (*len < 8) return -1;
            uint32_t elen = ((uint32_t)p[*off + 1] + 1) * 8;
            if (elen > *len) return -1;
            /* A Routing header with segments-left > 0 asks us to forward. We
             * are a host: RFC 8200 s4.4 says discard. */
            if (nh == IP6_NH_ROUTING && p[*off + 3] != 0) return -1;
            nh = p[*off];
            *off += elen;
            *len -= elen;
            break;
        }
        case IP6_NH_FRAGMENT:
            /* Not reassembled. Nothing this stack sends provokes fragments (DNS
             * runs over IPv4, and TCP never exceeds the MTU), so dropping is
             * honest rather than pretending. Counted so it is visible. */
            ip6_stats.rx_bad++;
            return -1;
        default:
            return nh;
        }
    }
    return -1;                              /* header chain too long: drop */
}

void ip6_input(const uint8_t *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + IP6_HDR_LEN) { ip6_stats.rx_bad++; return; }
    const struct ip6_hdr *h =
        (const struct ip6_hdr *)(frame + sizeof(struct eth_hdr));
    if ((ntohl(h->vtcfl) >> 28) != 6) { ip6_stats.rx_bad++; return; }

    uint32_t plen = ntohs(h->plen);
    if ((uint32_t)sizeof(struct eth_hdr) + IP6_HDR_LEN + plen > len) {
        /* The payload length must fit the frame. 32-bit math on purpose: a
         * 16-bit sum would wrap and let a huge plen look small. */
        ip6_stats.rx_bad++;
        return;
    }

    ip6_addr src, dst;
    memcpy(src.b, h->src, 16);
    memcpy(dst.b, h->dst, 16);

    /* RFC 8200 s3: a multicast address must never appear as a source, and the
     * unspecified address is only legal as a source during DAD (whose messages
     * are ICMPv6 and validated again there). The loopback address must never
     * appear on a link. */
    if (ip6_is_multicast(&src) || ip6_is_loopback(&src) ||
        ip6_is_loopback(&dst) || ip6_is_unspecified(&dst)) {
        ip6_stats.rx_bad++;
        return;
    }

    /* No forwarding: only what is addressed to us, or to a group we joined. */
    if (!ip6_addr_is_ours_any(&dst) && !joined_group(&dst))
        return;

    ip6_stats.rx++;

    uint32_t off = sizeof(struct eth_hdr) + IP6_HDR_LEN;
    uint32_t rem = plen;
    int nh = skip_ext(frame, &off, &rem, h->nh);
    if (nh < 0) return;
    const uint8_t *l4 = frame + off;

    if (nh == IP6_NH_ICMP6) {
        if (rem < 4) { ip6_stats.rx_bad++; return; }
        /* ICMPv6's checksum is MANDATORY and covers the pseudo-header -- unlike
         * ICMPv4's, which covers only the message. Verifying it here is what
         * stops a corrupted Router Advertisement from reconfiguring the host. */
        if (ip6_pseudo_checksum(&src, &dst, IP6_NH_ICMP6, l4, rem) != 0) {
            ip6_stats.rx_bad++;
            return;
        }
        icmp6_input(&src, &dst, l4, (uint16_t)rem, h->hlim);
        return;
    }

    if (nh == IP6_NH_TCP) {
#ifdef TCP_AF_INET6
        /* The datagram was addressed to one of our addresses; TCP needs that
         * exact address for its pseudo-header, so it is passed through rather
         * than re-derived. tcp.c verifies the checksum itself.
         *
         * The #ifdef is a feature test on the TCP line's tcp.h: they made the
         * address family a parameter (struct tcp_addr / tcp_input_af). Until
         * that lands, IPv6 still configures itself and answers ND and pings --
         * it just carries no TCP -- rather than failing to build. */
        struct tcp_addr s, d;
        s.af = TCP_AF_INET6; memcpy(s.a.v6, src.b, 16);
        d.af = TCP_AF_INET6; memcpy(d.a.v6, dst.b, 16);
        tcp_input_af(&s, &d, l4, (uint16_t)rem);
#else
        ip6_stats.rx_bad++;
#endif
        return;
    }

    /* UDP over IPv6 is deliberately absent: the only UDP users are DNS and
     * DHCP, both of which keep running over IPv4 here (see dns.c). Anything
     * else is dropped rather than half-handled. */
    ip6_stats.rx_bad++;
}

/* ---- bring-up and the timer pump ---------------------------------------- */

void ip6_init(void)
{
    if (inited) return;
    inited = 1;
    ip6_addr ll;
    ip6_linklocal_from_mac(net_cfg.mac, &ll);
    /* A link-local address never expires and is never advertised: it is formed
     * from the MAC and lives as long as the interface. */
    ip6_addr_add(&ll, 64, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
    char t[48];
    ip6_format(&ll, t, sizeof t);
    kprintf("[ip6] link-local %s tentative (dad)\n", t);
    nd_start_rs();
}

void ip6_reset(void)
{
    memset(ip6_addrs, 0, sizeof ip6_addrs);
    memset(&ip6_stats, 0, sizeof ip6_stats);
    link_mtu = 1500;
    inited = 0;
}

int ip6_up(void)
{
    int have_ll = 0, have_routable = 0;
    for (int i = 0; i < IP6_NADDR; i++) {
        if (!ip6_addrs[i].used || ip6_addrs[i].state == IP6_TENTATIVE) continue;
        if (ip6_is_linklocal(&ip6_addrs[i].addr)) have_ll = 1;
        else have_routable = 1;
    }
    if (!have_ll) return 0;
    ip6_addr gw;
    if (have_routable && nd_default_router(&gw)) return 2;
    return 1;
}

/* Expire addresses whose lifetimes have run out (RFC 4862 s5.5.4): preferred ->
 * deprecated (still usable for existing connections, no longer chosen for new
 * ones -- which is exactly what selection rule 3 encodes) -> gone. */
static void age_addresses(uint64_t now)
{
    for (int i = 0; i < IP6_NADDR; i++) {
        struct ip6_ifaddr *e = &ip6_addrs[i];
        if (!e->used) continue;
        if (e->valid_until != IP6_LIFETIME_INF && now >= e->valid_until) {
            char t[48];
            ip6_format(&e->addr, t, sizeof t);
            kprintf("[ip6] expired %s\n", t);
            e->used = 0;
            continue;
        }
        if (e->state == IP6_PREFERRED && e->pref_until != IP6_LIFETIME_INF &&
            now >= e->pref_until)
            e->state = IP6_DEPRECATED;
    }
}

void ip6_poll(void)
{
    if (!net_up()) return;
    uint64_t f = net_lock();
    if (!inited) ip6_init();
    uint64_t now = timer_ticks();
    age_addresses(now);
    nd_poll(now);
    net_unlock(f);
}
