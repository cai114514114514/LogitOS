#ifndef LOGIT_ETH_H
#define LOGIT_ETH_H

#include <stdint.h>

#define ETH_ALEN       6
#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;     /* network order */
} __attribute__((packed));

extern const uint8_t eth_broadcast[ETH_ALEN];

/* Build an Ethernet frame into `out` (must hold 14 + len) and transmit it.
 * `ethertype` is host order. Returns 0 on success. */
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t len);

/* RX entry point (handed to e1000_rx_poll): dispatch by ethertype. */
void eth_input(const uint8_t *frame, uint16_t len);

/* Stats (for the Network app / debugging). */
uint32_t eth_rx_count(void);

#endif /* LOGIT_ETH_H */
