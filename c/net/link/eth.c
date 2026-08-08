/* Ethernet: frame validation, VLAN, destination filtering, loopback, and the
 * counters that make all of it visible.
 *
 * The 47 lines this replaces did three things: increment a counter, check that
 * the frame was at least 14 bytes, and switch on the ethertype. Everything else
 * an Ethernet layer does was either absent or was being done for us by QEMU,
 * which is not the same as being done.
 *
 * WHAT WAS MISSING, and what each omission costs on a segment that is not
 * SLIRP:
 *
 *  - NO DESTINATION FILTER. Every frame handed up by the driver was dispatched,
 *    whoever it was addressed to. That is invisible while the NIC filters for
 *    us, and the NICs here do (e1000 sets BAM|MPE without unicast promiscuous;
 *    rtl8139 sets physical-match + multicast + broadcast). But it means the
 *    correctness of the whole stack rests on a register write in a driver, with
 *    no check at the layer whose job it is: put a card in promiscuous mode, mirror
 *    a switch port to it, or add a driver that gets RCR wrong, and the machine
 *    silently starts processing other hosts' TCP segments as its own.
 *
 *  - NO VLAN. A frame with ethertype 0x8100 fell off the end of the if-chain
 *    and was dropped without a trace. On any trunk port, any network with a
 *    voice or guest VLAN, any hypervisor doing VLAN-tagged guest networking --
 *    that is ALL of the traffic, and the symptom is a link that is plainly up
 *    with not one packet arriving. There is no partial version of this failure
 *    to warn you.
 *
 *  - NO UPPER LENGTH BOUND. eth_input trusted the driver's `len` completely.
 *    The drivers here do clamp, but the contract belongs at this boundary: a
 *    length longer than the frame is an out-of-bounds read in ring 0 performed
 *    on behalf of whoever sent the packet.
 *
 *  - NO MINIMUM FRAME ON TRANSMIT. An ARP frame is 14 + 28 = 42 bytes, and
 *    Ethernet's minimum is 60 (64 with FCS). Two of the four NIC drivers pad
 *    for us -- rtl8139 in software, e1000 via TCTL.PSP -- and the other two do
 *    not. So this worked by coincidence of which card was plugged in, and the
 *    failure on the others is that ARP specifically does not work, which
 *    presents as "no networking at all".
 *
 *  - NO LOOPBACK. A frame addressed to our own MAC went out of the NIC, and a
 *    NIC does not receive its own transmissions, so it was simply gone. With
 *    arp_resolve now answering for our own address, this is the other half of
 *    making the machine reachable from itself.
 *
 *  - ONE COUNTER for the entire layer, incremented before the validity check,
 *    so it counted frames that were then thrown away. Nothing else was
 *    observable: a machine dropping every frame for a wrong-VLAN or
 *    wrong-destination reason looked exactly like a machine on a quiet network.
 */

#include <stdint.h>
#include <stddef.h>
#include "eth.h"
#include "net.h"
#include "netdev.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
void  kprintf(const char *fmt, ...);

/* L2/L3 dispatch handlers (defined in arp.c / ip.c, added in later layers). */
void arp_input(const uint8_t *frame, uint16_t len) __attribute__((weak));
void ip_input(const uint8_t *frame, uint16_t len) __attribute__((weak));
void ip6_input(const uint8_t *frame, uint16_t len) __attribute__((weak));

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct eth_stats stats;

const struct eth_stats *eth_get_stats(void) { return &stats; }
uint32_t eth_rx_count(void) { return stats.rx; }

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, ETH_ALEN) == 0;
}

/* ---- transmit ----------------------------------------------------------- */

/* Loopback re-entry depth.
 *
 * eth_send -> eth_input -> ip_input -> icmp -> ip_send -> eth_send is a real
 * and useful path (it is what answering our own ping looks like), so the
 * recursion is not a bug to be forbidden -- but it does consume stack, and each
 * level costs this function's 1514-byte frame buffer plus ip_send's 1500-byte
 * one. A depth of two covers request-and-answer, which is every case anything
 * generates, and hard-bounds the stack cost at roughly 6 KiB of the 32 KiB
 * kernel stack. Anything deeper is a loop, and a loop here is a stack
 * overflow into the page tables (see the M11 stack note in CLAUDE.md), so it
 * is dropped and counted rather than followed. */
#define LOOPBACK_MAX_DEPTH 2
static int loopback_depth;

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype,
             const void *payload, uint16_t len)
{
    uint8_t frame[ETH_FRAME_MAX];
    if (len > ETH_FRAME_MAX - ETH_HDR_LEN) {
        stats.tx_toolong++;
        return -1;
    }
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst, ETH_ALEN);
    memcpy(h->src, net_cfg.mac, ETH_ALEN);
    h->ethertype = htons(ethertype);
    memcpy(frame + ETH_HDR_LEN, payload, len);

    uint16_t n = (uint16_t)(ETH_HDR_LEN + len);

    /* IEEE 802.3's minimum frame is 64 bytes including the 4-byte FCS, which
     * the NIC appends -- so 60 bytes of it are ours to provide. Padding here
     * rather than in each driver means it is done once, correctly, for every
     * card: the two drivers that already pad see frames that are long enough
     * and do nothing, and the two that do not are fixed. The pad must be zero
     * and not stack residue, which would otherwise leak kernel memory onto the
     * wire in every ARP frame the machine sends. */
#ifndef ETH_NEGCTL_LEGACY
    if (n < ETH_FRAME_MIN) {
        memset(frame + n, 0, (size_t)(ETH_FRAME_MIN - n));
        n = ETH_FRAME_MIN;
        stats.tx_padded++;
    }
#endif

    stats.tx++;
    stats.tx_bytes += n;

    /* A frame addressed to ourselves never reaches the wire: a NIC does not
     * receive its own transmissions, so handing this to the driver would simply
     * lose it. Deliver it to the receive path instead. This is what makes the
     * machine's own address usable from the machine -- see arp_resolve's self
     * case, which is what causes such a frame to be built in the first place. */
    if (mac_eq(dst, net_cfg.mac) && !mac_eq(dst, eth_broadcast)) {
        if (loopback_depth >= LOOPBACK_MAX_DEPTH) {
            stats.lo_dropped++;
            return -1;
        }
        loopback_depth++;
        stats.lo_tx++;
        eth_input(frame, n);
        loopback_depth--;
        return 0;
    }

    return netdev_tx(frame, n);
}

/* ---- receive ------------------------------------------------------------ */

/* Is this frame for us?
 *
 * Broadcast and multicast are accepted unconditionally: multicast is not
 * optional plumbing, it is how IPv6 Neighbour Discovery works at all (every
 * solicitation goes to a 33:33:... group address) and how IPv4 link-local
 * protocols reach us. Unicast must match our own address.
 *
 * The zero-MAC case is real and must accept: net_init copies the NIC's address
 * into net_cfg AFTER the driver has come up, and a frame that arrives in that
 * window would otherwise be dropped for not matching an address we do not have
 * yet. */
static int addressed_to_us(const struct eth_hdr *h)
{
    static const uint8_t zero[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };
#ifdef ETH_NEGCTL_LEGACY
    /* Negative control (tests/unit/eth_test.c): the behaviour this file
     * replaced -- dispatch every frame the driver hands up, whoever it was
     * addressed to, with no padding on transmit and no VLAN support. If the
     * suite still passes with this defined, none of it is testing the rewrite. */
    (void)h; (void)zero;
    return 1;
#endif
    if (h->dst[0] & 1) return 1;                     /* multicast (incl. broadcast) */
    if (mac_eq(net_cfg.mac, zero)) return 1;         /* we do not know who we are yet */
    return mac_eq(h->dst, net_cfg.mac);
}

/* De-tagging scratch.
 *
 * The upper layers all index their own headers at a hardcoded offset of 14
 * bytes (`frame + sizeof(struct eth_hdr)`), so the cheapest way to support
 * VLANs without touching ip.c, ip6.c and arp.c is to hand them an UNTAGGED
 * frame: copy the 12 address bytes and then the inner ethertype and payload,
 * which is exactly the original frame minus the 4-byte tag.
 *
 * A static buffer is safe here, and the reason is a contract worth stating: all
 * four NIC drivers (e1000, virtio-net, rtl8139, rtl8169) take net_lock() around
 * their receive drain and hold it across this callback, so eth_input never runs
 * concurrently with itself. The one re-entrant path is loopback, and a
 * loopback frame is one we built ourselves and is never tagged. */
static uint8_t detag[ETH_FRAME_MAX];

void eth_input(const uint8_t *frame, uint16_t len)
{
    stats.rx++;

    /* The length bounds are this layer's contract with the drivers, asserted
     * rather than assumed. Below the header there is nothing to dispatch on;
     * above the maximum the driver has handed us a length that does not
     * describe a frame, and every read past it is out of bounds. */
    if (len < ETH_HDR_LEN) { stats.rx_runt++; return; }
    if (len > ETH_FRAME_MAX_VLAN) { stats.rx_toolong++; return; }

    stats.rx_bytes += len;

    const struct eth_hdr *h = (const struct eth_hdr *)frame;

    if (!addressed_to_us(h)) {
        /* Not ours. On a correctly-filtering NIC this never fires, which is
         * why it is counted: a non-zero value here means the card is passing
         * traffic it should not, and that is worth knowing before it is
         * mistaken for a protocol bug. */
        stats.rx_not_ours++;
        return;
    }
    if (h->dst[0] & 1) {
        if (mac_eq(h->dst, eth_broadcast)) stats.rx_broadcast++;
        else stats.rx_multicast++;
    }

    uint16_t type = ntohs(h->ethertype);

    /* 802.1Q (and 802.1ad, whose outer tag has the same shape). The tag is
     * 4 bytes: 16 bits of TCI -- 3 priority, 1 drop-eligible, 12 VLAN id --
     * followed by the real ethertype. Stacked tags are handled by looping,
     * because a QinQ frame carries two and stopping after one leaves the inner
     * TPID looking like an ethertype. */
    int tags = 0;
#ifdef ETH_NEGCTL_LEGACY
    goto dispatch;                    /* the old code had no VLAN awareness */
#endif
    while (type == ETHERTYPE_VLAN || type == ETHERTYPE_QINQ) {
        if (++tags > ETH_MAX_VLAN_TAGS) { stats.rx_vlan_bad++; return; }
        /* Need the tag itself plus the ethertype that follows it. */
        if ((uint32_t)len < (uint32_t)ETH_HDR_LEN + 4u * (uint32_t)tags) {
            stats.rx_vlan_bad++;
            return;
        }
        const uint8_t *tag = frame + ETH_HDR_LEN + 4 * (tags - 1);
        stats.rx_vlan++;
        stats.last_vlan_id = (uint16_t)(((tag[0] << 8) | tag[1]) & 0x0FFF);
        type = (uint16_t)((tag[2] << 8) | tag[3]);
    }

    if (tags) {
        /* Rebuild the frame without its tags. len is already bounded above by
         * ETH_FRAME_MAX_VLAN and the strip only ever shortens it, so the copy
         * cannot exceed `detag`. */
        uint32_t stripped = 4u * (uint32_t)tags;
        uint32_t body = (uint32_t)len - ETH_HDR_LEN - stripped;
        if (ETH_HDR_LEN + body > sizeof detag) { stats.rx_vlan_bad++; return; }
        memcpy(detag, frame, 2 * ETH_ALEN);                 /* dst + src */
        detag[2 * ETH_ALEN]     = (uint8_t)(type >> 8);
        detag[2 * ETH_ALEN + 1] = (uint8_t)type;
        memcpy(detag + ETH_HDR_LEN, frame + ETH_HDR_LEN + stripped, body);
        frame = detag;
        len = (uint16_t)(ETH_HDR_LEN + body);
    }

#ifdef ETH_NEGCTL_LEGACY
dispatch:
#endif
    switch (type) {
    case ETHERTYPE_ARP:
        if (arp_input) { stats.rx_arp++; arp_input(frame, len); }
        else stats.rx_unhandled++;
        return;
    case ETHERTYPE_IP:
        if (ip_input) { stats.rx_ip++; ip_input(frame, len); }
        else stats.rx_unhandled++;
        return;
    case ETHERTYPE_IPV6:
        if (ip6_input) { stats.rx_ip6++; ip6_input(frame, len); }
        else stats.rx_unhandled++;
        return;
    default:
        /* Counted, not merely dropped. A frame the machine does not understand
         * is a fact about the network it is on, and until now there was no way
         * to tell "nothing is arriving" from "everything arriving is something
         * we ignore" -- which is precisely what an unsupported VLAN, an LLDP
         * segment, or a PPPoE link looks like from up here. */
        stats.rx_unhandled++;
        stats.last_unhandled_type = type;
        return;
    }
}

void eth_dump(void)
{
    kprintf("[eth] rx %u (%u B) arp %u ip %u ip6 %u vlan %u bcast %u mcast %u\n",
            stats.rx, stats.rx_bytes, stats.rx_arp, stats.rx_ip, stats.rx_ip6,
            stats.rx_vlan, stats.rx_broadcast, stats.rx_multicast);
    kprintf("[eth] rx drops: runt %u toolong %u not-ours %u vlan-bad %u "
            "unhandled %u (last type %x)\n",
            stats.rx_runt, stats.rx_toolong, stats.rx_not_ours, stats.rx_vlan_bad,
            stats.rx_unhandled, stats.last_unhandled_type);
    kprintf("[eth] tx %u (%u B) padded %u toolong %u  loopback %u dropped %u\n",
            stats.tx, stats.tx_bytes, stats.tx_padded, stats.tx_toolong,
            stats.lo_tx, stats.lo_dropped);
}

void eth_reset(void)
{
    memset(&stats, 0, sizeof stats);
    loopback_depth = 0;
}
