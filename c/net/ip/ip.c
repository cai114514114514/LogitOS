#include <stdint.h>
#include <stddef.h>
#include "ip.h"
#include "eth.h"
#include "arp.h"
#include "net.h"

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
void udp_input(uint32_t, const uint8_t *, uint16_t) __attribute__((weak));
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
    /* Next hop: the destination if it shares our subnet, else the gateway. */
    uint32_t nexthop = ((dst & net_cfg.mask) == (net_cfg.ip & net_cfg.mask))
                       ? dst : net_cfg.gw;
    uint8_t mac[ETH_ALEN];
    if (arp_resolve(nexthop, mac) != 0)
        return -1;                          /* ARP pending; caller retries */

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
    return eth_send(mac, ETHERTYPE_IP, pkt, (uint16_t)(sizeof *h + len));
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
    /* Fragment reassembly is not implemented. Passing a first fragment to a
     * transport parser as if it were a complete segment is worse than an
     * explicit drop, so reject MF, nonzero offsets, and the reserved flag. */
    uint16_t frag = ntohs(h->frag);
    if (frag & 0xBFFFu)                  /* allow only the DF flag (0x4000) */
        return;
    /* No forwarding: only packets addressed to us reach the upper layers
     * (keeps off-subnet noise out of the one-shot UDP/DNS receive slot). */
    if (ntohl(h->dst) != net_cfg.ip)
        return;
    /* This stack has no DHCP/bootstrap receive path, multicast membership, or
     * loopback-on-wire use. Reject source forms that cannot identify a remote
     * unicast peer in the supported configuration. */
    if (src == 0 || src == 0xFFFFFFFFu || (src >> 24) == 127 ||
        (src >> 28) == 0xEu)
        return;

    const uint8_t *l4 = (const uint8_t *)h + ihl;
    uint16_t l4len = (uint16_t)(tot - ihl);

    if (h->proto == IP_PROTO_ICMP && icmp_input)
        icmp_input(src, l4, l4len);
    else if (h->proto == IP_PROTO_UDP && udp_input)
        udp_input(src, l4, l4len);
    else if (h->proto == IP_PROTO_TCP && tcp_input)
        tcp_input(src, l4, l4len);
}
