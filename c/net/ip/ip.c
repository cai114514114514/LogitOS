#include <stdint.h>
#include <stddef.h>
#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "net.h"
#include "reasm.h"

void *memcpy(void *, const void *, size_t);

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
    /* Broadcasts are not ARP'd: there is no single next hop to resolve.
     * Everything else goes out through arp_output, which either sends now or
     * HOLDS the datagram until the solicitation is answered.
     *
     * The header has to be built before the decision, not after: arp_output
     * queues a finished frame payload. That is the only structural change --
     * a resolved next hop takes exactly the path it took before. */
    int bcast = ip_is_broadcast(dst);
    uint32_t nexthop = bcast ? 0
        : (((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) ? dst : net_cfg.gw);

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
    h->src = htonl(net_cfg.ip);
    h->dst = htonl(dst);
    h->checksum = htons(ip_checksum(h, sizeof *h));
    memcpy(pkt + sizeof *h, payload, len);

    uint16_t total = (uint16_t)(sizeof *h + len);
    if (bcast)
        return eth_send(eth_broadcast, ETHERTYPE_IP, pkt, total);
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
