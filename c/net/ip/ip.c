#include <stdint.h>
#include <stddef.h>
#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "net.h"
#include "reasm.h"
#include "route.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

/* The interface indices the routing table refers to.
 *
 * Weak on purpose, and for the reason the weak upper-layer hooks below already
 * establish: this file is compiled into host tests (tests/unit/ip_arp_test.c,
 * tests/unit/ip_route_test.c) that have no driver layer at all, and a hard
 * reference would drag c/drivers/net into a test about the neighbour cache.
 * Including netdev.h would be worse still -- it pulls in driver.h and the PCI
 * device model.
 *
 * When they are absent the constants from route.h stand in; netdev_init()
 * registers loopback first precisely so those constants are true by
 * construction, and netdev.c carries the _Static_assert that says so. */
int netdev_primary_ifindex(void) __attribute__((weak));
int netdev_loopback_ifindex(void) __attribute__((weak));

/* ---- net_cfg -> the routing table --------------------------------------- */

/* net_cfg is a single global ip/mask/gw read by 79 sites in 20 files. Moving
 * those is not this change; keeping the table honest about them is. This runs
 * on every send and is a comparison of four words in the common case, because
 * route_v4_iface memoises per interface and returns immediately when nothing
 * moved.
 *
 * WHY HERE and not in net.c where net_cfg is filled: net.c is not this
 * change's file, and there is more than one writer -- DHCP, the settings
 * store, and the static fallback -- so a call placed at one of them is a call
 * missing at the other two. A pull on use cannot be forgotten by a writer that
 * does not know the table exists. The cost of that choice is one comparison
 * per datagram; the cost of the alternative is a stale route nothing reports.
 * When net_cfg's owner adopts the table, this becomes route_v4_iface() at the
 * three configuration sites and this function is deleted. */
static void route_sync(void)
{
    uint32_t gen = route_generation();

    /* LOOPBACK FIRST, and unconditionally: a machine with no NIC and no lease
     * still has 127.0.0.1, and this is the one interface whose configuration
     * can never change, so it costs a memo comparison forever after.
     *
     * 127.0.0.1/8, not /32 -- the whole 127/8 block is loopback (RFC 1122
     * s3.2.1.3), and a /32 would leave 127.0.0.2 falling through to the
     * default route and onto the wire, which is the original bug moved one
     * address to the left rather than fixed. */
    int lo = netdev_loopback_ifindex ? netdev_loopback_ifindex() : RT_OIF_LO;
    if (lo > 0)
        route_v4_iface(lo, 0x7F000001u, 0xFF000000u, 0, RT_F_LOCAL);

    if (net_cfg.ip || net_cfg.mask) {           /* configured at all */
        int oif = netdev_primary_ifindex ? netdev_primary_ifindex() : RT_OIF_NIC0;
        if (oif > 0)
            route_v4_iface(oif, net_cfg.ip, net_cfg.mask, net_cfg.gw, 0);
    }

    if (route_generation() == gen) return;

    /* Print the table only when it actually moved. This is the boot evidence
     * that the routing decision is a table and not a ternary, and it is also
     * route_at()/route_count()'s consumer in the kernel. */
    for (int i = 0; i < RT_NROUTE; i++) {
        const struct route_entry *e = route_at(i);
        if (!e) continue;
        kprintf("[route] %u.%u.%u.%u/%u ", (e->dst >> 24) & 255,
                (e->dst >> 16) & 255, (e->dst >> 8) & 255, e->dst & 255, e->plen);
        if (e->nexthop)
            kprintf("via %u.%u.%u.%u ", (e->nexthop >> 24) & 255,
                    (e->nexthop >> 16) & 255, (e->nexthop >> 8) & 255,
                    e->nexthop & 255);
        else
            kprintf("onlink ");
        kprintf("dev %d src %u.%u.%u.%u metric %d%s\n", e->oif,
                (e->src >> 24) & 255, (e->src >> 16) & 255,
                (e->src >> 8) & 255, e->src & 255, e->metric,
                (e->flags & RT_F_LOCAL) ? " local" : "");
    }
}

/* Datagrams refused for want of a route. Counted rather than only returned:
 * "no route" and "the NIC would not take it" are both -1 to the caller, and
 * the difference is the first thing anybody debugging a dead destination wants
 * to know. Read by ip_no_route_count() -- see ip.h. */
static uint32_t ip_no_route;

uint32_t ip_no_route_count(void) { return ip_no_route; }

struct ip_hdr {
    uint8_t  ver_ihl;       /* 0x45: version 4, IHL 5 (20 bytes) */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;           /* network order */
    uint32_t dst;
} __attribute__((packed));

/* Upper-layer hooks are optional until their layers are linked in. */
void icmp_input(uint32_t, const uint8_t *, uint16_t) __attribute__((weak));
void udp_input(uint32_t, const uint8_t *, uint16_t,
               const uint8_t *) __attribute__((weak));
void tcp_input(uint32_t, const uint8_t *, uint16_t) __attribute__((weak));

uint16_t ip_checksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ip_id;

int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    /* Broadcasts are not ARP'd and are not routed: there is no single next hop
     * to resolve, and no prefix a broadcast address belongs to in the sense
     * the table means. Everything else goes out through arp_output, which
     * either sends now or HOLDS the datagram until the solicitation is
     * answered.
     *
     * The header has to be built before the decision, not after: arp_output
     * queues a finished frame payload.
     *
     * THE ROUTING DECISION USED TO BE THE TERNARY THAT IS NO LONGER HERE:
     *
     *     ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) ? dst : gw
     *
     * -- on-link goes direct, everything else goes to THE gateway. It could
     * not express a second interface, a host route, or "unreachable": an
     * address this machine has no path to came out of it as the gateway's
     * problem, which is how an ICMP echo addressed to 127.0.0.1 used to be
     * ARP'd for the gateway and put on the wire. */
    int bcast = ip_is_broadcast(dst);
    uint32_t nexthop = 0, src = net_cfg.ip;
    uint32_t rflags = 0;

#ifdef IP_NEGCTL_TERNARY
    /* NEGATIVE CONTROL (tests/route.mk test-ip-route-negctl): the routing
     * decision this file shipped with, restored verbatim. Not a broken
     * version -- the version that ran on this machine until today, which is
     * the only control worth having here, because everything it does looks
     * right: every packet still gets an interface, a next hop and a source
     * address, and the ones that go to the internet still arrive.
     *
     * What it cannot do is say "no route" (an unreachable destination comes
     * out as the gateway's problem) and it cannot put 127.0.0.1 anywhere but
     * on the wire. tests/unit/ip_route_test.c MUST fail against this build. */
    if (!bcast) {
        nexthop = ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask))
                  ? dst : net_cfg.gw;
        src = net_cfg.ip;
        rflags = 0;
    }
#else
    if (!bcast) {
        route_sync();
        struct route_res rr;
        if (route_lookup(dst, &rr) != RT_OK) {
            /* A DISTINGUISHABLE REFUSAL, which the ternary could not produce.
             * Handing an unroutable datagram to the gateway is not a fallback,
             * it is a leak: it puts the destination address on a wire that was
             * never going to carry it, and it makes every "why is this host
             * unreachable" question start at the wrong end of the network. */
            ip_no_route++;
            return -1;
        }
        nexthop = rr.nexthop;
        rflags = rr.flags;
        /* The source address is the route's, not net_cfg's. On a one-interface
         * machine these are the same value and nothing observable changes; on
         * a machine with two they are the difference between a reply that can
         * come back and one that cannot. */
        if (rr.src) src = rr.src;
    }
#endif

    uint8_t pkt[1500];
    if (sizeof(struct ip_hdr) + len > sizeof pkt)
        return -1;
    struct ip_hdr *h = (struct ip_hdr *)pkt;
    h->ver_ihl = 0x45;
    h->tos = 0;
    h->total_len = htons((uint16_t)(sizeof *h + len));
    h->id = htons(ip_id++);
    h->frag = htons(0x4000);                /* don't fragment */
    h->ttl = 64;
    h->proto = proto;
    h->checksum = 0;
    h->src = htonl(src);
    h->dst = htonl(dst);
    h->checksum = htons(ip_checksum(h, sizeof *h));
    memcpy(pkt + sizeof *h, payload, len);

    uint16_t total = (uint16_t)(sizeof *h + len);
    if (bcast)
        return eth_send(eth_broadcast, ETHERTYPE_IP, pkt, total);

    /* A route the table marked RT_F_LOCAL -- today that is the loopback
     * interface and 127/8. There is no neighbour to resolve: nothing on the
     * link owns 127.0.0.1, so a solicitation for it would be a broadcast
     * asking the LAN who holds an address the RFCs forbid it to hold.
     *
     * Delivery is eth_send() to our own MAC, which is not a special case
     * either: c/net/link/eth.c has had a loopback path since the link-layer
     * rewrite (a frame addressed to our own MAC is handed to eth_input rather
     * than to the card, with a depth bound), and reusing it means the datagram
     * goes through exactly the receive path a real frame would. */
    if (rflags & RT_F_LOCAL)
        return eth_send(net_cfg.mac, ETHERTYPE_IP, pkt, total);

#ifdef IP_NEGCTL_ARP_DROP
    /* Negative control (tests/link.mk test-ip-arp-negctl): exactly what this
     * file did before -- drop the datagram on a miss and make it the caller's
     * problem. The suite MUST fail with this defined. */
    {
        uint8_t mac[ETH_ALEN];
        if (arp_resolve(nexthop, mac) != 0) return -1;
        return eth_send(mac, ETHERTYPE_IP, pkt, total);
    }
#endif
    /* Was: `if (arp_resolve(nexthop, mac) != 0) return -1;` before the header
     * was built -- which threw the FIRST datagram to every cold next hop away
     * and made every caller's retry policy the substitute for a neighbour
     * queue. arp_output returns 0 for "sent OR accepted for later delivery",
     * so a cold cache now costs latency instead of a lost packet. */
    return arp_output(nexthop, ETHERTYPE_IP, pkt, total);
}

void ip_input(const uint8_t *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *h = (const struct ip_hdr *)(frame + sizeof(struct eth_hdr));
    if ((h->ver_ihl >> 4) != 4)
        return;
    int ihl = (h->ver_ihl & 0xF) * 4;
    uint32_t src = ntohl(h->src);
    uint16_t tot = ntohs(h->total_len);
    /* ihl must cover the fixed IP header (else l4 points inside it), and the
     * total length must fit the frame. Use 32-bit math: a uint16 cast of
     * (14 + tot) truncates and could wrap a huge tot under `len`. */
    if (ihl < (int)sizeof(struct ip_hdr) || tot < ihl ||
        (uint32_t)sizeof(struct eth_hdr) + tot > len)
        return;
    /* Drop corrupted headers; ihl (not sizeof *h) so IP options are covered. */
    if (ip_checksum(h, ihl) != 0)
        return;
    if (h->ttl == 0)
        return;
    /* The reserved flag must be zero; MF/nonzero-offset fragments go through
     * reassembly below (the DF flag is meaningless on receive and ignored). */
    uint16_t frag = ntohs(h->frag);
    if (frag & 0x8000u)
        return;
    uint32_t dst = ntohl(h->dst);
    /* No forwarding: only packets addressed to us reach the upper layers
     * (keeps off-subnet noise out of the one-shot UDP/DNS receive slot).
     * Broadcasts (limited or subnet-directed) are let through for UDP only --
     * they are how DHCP-class services reach us; TCP/ICMP stay unicast-only. */
    if (dst != net_cfg.ip && (!ip_is_broadcast(dst) || h->proto != IP_PROTO_UDP))
        return;
    /* This stack has no DHCP/bootstrap receive path, multicast membership, or
     * loopback-on-wire use. Reject source forms that cannot identify a remote
     * unicast peer in the supported configuration. */
    if (src == 0 || src == 0xFFFFFFFFu || (src >> 24) == 127 ||
        (src >> 28) >= 0xEu)
        return;

    const uint8_t *l4 = (const uint8_t *)h + ihl;
    uint16_t l4len = (uint16_t)(tot - ihl);
    const uint8_t *iph = (const uint8_t *)h;
    struct reasm_dgram g;

    if (frag & 0x3FFFu) {          /* MF set or nonzero offset: reassemble */
        if (!reasm_input(src, dst, h->proto, ntohs(h->id),
                         iph, (uint8_t)ihl,
                         (uint16_t)((frag & 0x1FFFu) << 3), (frag & 0x2000u) != 0,
                         l4, l4len, &g))
            return;                /* incomplete -- or poisoned and dropped */
        iph = g.iph;
        l4 = g.l4;
        l4len = g.l4len;
    }

    if (h->proto == IP_PROTO_ICMP && icmp_input)
        icmp_input(src, l4, l4len);
    else if (h->proto == IP_PROTO_UDP && udp_input)
        udp_input(src, l4, l4len, iph);
    else if (h->proto == IP_PROTO_TCP && tcp_input)
        tcp_input(src, l4, l4len);

    if (frag & 0x3FFFu)
        reasm_release(&g);
}
