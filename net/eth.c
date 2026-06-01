#include <stdint.h>
#include <stddef.h>
#include "eth.h"
#include "net.h"
#include "e1000.h"

void *memcpy(void *, const void *, size_t);

/* L2/L3 dispatch handlers (defined in arp.c / ip.c, added in later layers). */
void arp_input(const uint8_t *frame, uint16_t len) __attribute__((weak));
void ip_input(const uint8_t *frame, uint16_t len) __attribute__((weak));

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint32_t rx_count;

uint32_t eth_rx_count(void) { return rx_count; }

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t len)
{
    uint8_t frame[1514];
    if (len > sizeof frame - sizeof(struct eth_hdr))
        return -1;
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst, ETH_ALEN);
    memcpy(h->src, net_cfg.mac, ETH_ALEN);
    h->ethertype = htons(ethertype);
    memcpy(frame + sizeof *h, payload, len);
    return e1000_tx(frame, (uint16_t)(sizeof *h + len));
}

void eth_input(const uint8_t *frame, uint16_t len)
{
    rx_count++;
    if (len < sizeof(struct eth_hdr))
        return;
    const struct eth_hdr *h = (const struct eth_hdr *)frame;
    uint16_t type = ntohs(h->ethertype);
    if (type == ETHERTYPE_ARP && arp_input)
        arp_input(frame, len);
    else if (type == ETHERTYPE_IP && ip_input)
        ip_input(frame, len);
}
