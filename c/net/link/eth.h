#ifndef LOGIT_ETH_H
#define LOGIT_ETH_H

#include <stdint.h>

#define ETH_ALEN       6
#define ETH_HDR_LEN    14

/* IEEE 802.3 frame sizes, excluding the 4-byte FCS the NIC appends and strips.
 *
 * ETH_FRAME_MIN is the one that has been silently missing: a frame shorter than
 * this is illegal on the wire, and an ARP packet (14 + 28 = 42) is shorter than
 * it. Two of the four NIC drivers happened to pad and two did not, so whether
 * ARP worked depended on which card was present. eth_send pads now. */
#define ETH_FRAME_MIN  60
#define ETH_FRAME_MAX  1514      /* 14 header + 1500 payload */
#define ETH_MAX_VLAN_TAGS 2      /* one 802.1Q tag, or a QinQ pair */
#define ETH_FRAME_MAX_VLAN (ETH_FRAME_MAX + 4 * ETH_MAX_VLAN_TAGS)

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_VLAN 0x8100    /* 802.1Q customer tag */
#define ETHERTYPE_QINQ 0x88A8    /* 802.1ad service tag */
#define ETHERTYPE_IPV6 0x86DD

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;     /* network order */
} __attribute__((packed));

extern const uint8_t eth_broadcast[ETH_ALEN];

/* Every frame in, every frame out, and every reason one was dropped.
 *
 * The layer used to have a single counter, incremented before the validity
 * check. Nothing else about it was observable, so a machine dropping 100% of
 * its traffic for a wrong-VLAN or wrong-destination reason was indistinguishable
 * from a machine on an idle network -- and those are opposite problems. */
struct eth_stats {
    uint32_t rx, rx_bytes;
    uint32_t rx_arp, rx_ip, rx_ip6;
    uint32_t rx_broadcast, rx_multicast, rx_vlan;
    uint32_t rx_runt;         /* shorter than a header */
    uint32_t rx_toolong;      /* longer than a frame can be */
    uint32_t rx_not_ours;     /* unicast to somebody else: the NIC filter leaked */
    uint32_t rx_vlan_bad;     /* truncated or over-stacked tag */
    uint32_t rx_unhandled;    /* ethertype we do not implement */
    uint16_t last_unhandled_type;
    uint16_t last_vlan_id;
    uint32_t tx, tx_bytes;
    uint32_t tx_padded;       /* frames extended to the 60-byte minimum */
    uint32_t tx_toolong;      /* refused: payload over the MTU */
    uint32_t lo_tx;           /* delivered to ourselves instead of the wire */
    uint32_t lo_dropped;      /* loopback recursion bound hit */
};

/* Build an Ethernet frame and transmit it. `ethertype` is host order. Pads to
 * the 60-byte minimum, and short-circuits to eth_input if `dst` is our own
 * address. Returns 0 on success. */
int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t len);

/* RX entry point (handed to the NIC drain): validate, filter by destination,
 * strip VLAN tags, dispatch by ethertype.
 *
 * CONTRACT: callers hold net_lock() for the duration, which all four NIC
 * drivers do. eth_input keeps a de-tagging buffer that relies on it. */
void eth_input(const uint8_t *frame, uint16_t len);

/* Stats (for the Network app / debugging). */
const struct eth_stats *eth_get_stats(void);
uint32_t eth_rx_count(void);
void     eth_dump(void);
void     eth_reset(void);

#endif /* LOGIT_ETH_H */
