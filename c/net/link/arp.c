#include <stdint.h>
#include <stddef.h>
#include "arp.h"
#include "eth.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;               /* network order */
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;               /* network order */
} __attribute__((packed));

#define ARP_CACHE 16
static struct { uint32_t ip; uint8_t mac[ETH_ALEN]; int used; } cache[ARP_CACHE];

static void cache_put(uint32_t ip, const uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].used && cache[i].ip == ip) { memcpy(cache[i].mac, mac, ETH_ALEN); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!cache[i].used) { cache[i].used = 1; cache[i].ip = ip; memcpy(cache[i].mac, mac, ETH_ALEN); return; }
    cache[0].ip = ip; memcpy(cache[0].mac, mac, ETH_ALEN);   /* evict slot 0 */
}

static int cache_get(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].used && cache[i].ip == ip) { memcpy(mac, cache[i].mac, ETH_ALEN); return 0; }
    return -1;
}

static void send_arp(uint16_t op, const uint8_t *target_mac, uint32_t target_ip)
{
    struct arp_pkt p;
    p.htype = htons(ARP_HTYPE_ETH);
    p.ptype = htons(ARP_PTYPE_IP);
    p.hlen = ETH_ALEN; p.plen = 4;
    p.op = htons(op);
    memcpy(p.sha, net_cfg.mac, ETH_ALEN);
    p.spa = htonl(net_cfg.ip);
    memcpy(p.tha, target_mac, ETH_ALEN);
    p.tpa = htonl(target_ip);
    eth_send(target_mac, ETHERTYPE_ARP, &p, sizeof p);
}

void arp_input(const uint8_t *frame, uint16_t len)
{
    if (len < sizeof(struct eth_hdr) + sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *p = (const struct arp_pkt *)(frame + sizeof(struct eth_hdr));
    if (ntohs(p->ptype) != ARP_PTYPE_IP)
        return;

    /* Learn the sender, regardless of op. */
    cache_put(ntohl(p->spa), p->sha);

    if (ntohs(p->op) == ARP_OP_REQUEST && ntohl(p->tpa) == net_cfg.ip)
        send_arp(ARP_OP_REPLY, p->sha, ntohl(p->spa));   /* "that's me" */
}

int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{
    if (cache_get(ip, mac) == 0)
        return 0;
    send_arp(ARP_OP_REQUEST, eth_broadcast, ip);         /* who has `ip`? */
    return -1;
}

/* Block (polling) until `ip` is in the ARP cache, or `timeout` ticks elapse.
 * Warms the cache so the first real packet to a next-hop isn't silently dropped
 * on an ARP miss -- ip_send returns -1 there and the caller only re-sends on a
 * 250-500 ms protocol retransmit, which was adding ~0.5 s to every cold fetch.
 * The reply arrives in <1 ms, so this returns in a tick or two. Safe only from
 * the blocking fetch context (it pumps net_poll), NOT from the RX IRQ. */
int arp_warm(uint32_t ip, int timeout)
{
    uint8_t mac[ETH_ALEN];
    uint64_t start = timer_ticks();
    while (arp_resolve(ip, mac) != 0) {
        if ((int)(timer_ticks() - start) > timeout) return -1;
        net_poll();
        net_idle();
    }
    return 0;
}
