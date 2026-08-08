/* Host test for the one thing ip_send() does that nothing else can: hand a
 * datagram to the neighbour cache instead of throwing it away.
 *
 * arp_output() has been built, tested and documented since the link-layer
 * rewrite, and ip.c did not call it -- `if (arp_resolve(nexthop, mac) != 0)
 * return -1;`. The consequence is not subtle and it is not rare: the FIRST
 * datagram to any next hop whose MAC is not already cached was destroyed, on
 * every boot, for every protocol. UDP hid it because DNS retries; TCP hid it
 * because SYN retransmits; what neither hides is the extra round-trip time on
 * every cold connection, which is the retry timer, not the network.
 *
 * White-box in the same shape as tests/unit/arp_test.c: it #includes arp.c and
 * ip.c and stubs only what is genuinely below them, so the bytes checked here
 * are the bytes the kernel would put on the wire.
 *
 * Built with -DLOGIT_NET_HOST so net_lock() is a no-op (cli/sti are ring 0).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "net.h"
#include "eth.h"

#define OUR_IP   0x0A00020Fu       /* 10.0.2.15 */
#define GW_IP    0x0A000202u       /* 10.0.2.2, off-subnet next hop */
#define PEER_IP  0x0A000209u       /* 10.0.2.9, on-subnet  */
#define FAR_IP   0x08080808u       /* 8.8.8.8, routed via the gateway */

#define OUR_MAC  0x52,0x54,0x00,0x12,0x34,0x56
#define GW_MAC   0x52,0x55,0x0A,0x00,0x02,0x02
#define PEER_MAC 0x52,0x55,0x0A,0x00,0x02,0x09

struct net_config net_cfg = {
    .mac = { OUR_MAC },
    .ip = OUR_IP, .mask = 0xFFFFFF00u, .gw = GW_IP, .dns = 0x0A000203u,
};

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const uint8_t our_mac[6]  = { OUR_MAC };
static const uint8_t gw_mac[6]   = { GW_MAC };
static const uint8_t peer_mac[6] = { PEER_MAC };

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }

/* --- captured transmissions --------------------------------------------- */
#define NWIRE 64
static struct { uint8_t dst[6]; uint16_t type, len; uint8_t data[1600]; } wire[NWIRE];
static int nwire;

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type,
             const void *payload, uint16_t len)
{
    if (nwire >= NWIRE || len > sizeof wire[0].data) return -1;
    memcpy(wire[nwire].dst, dst, 6);
    wire[nwire].type = type;
    wire[nwire].len = len;
    memcpy(wire[nwire].data, payload, len);
    nwire++;
    return 0;
}

void kprintf(const char *fmt, ...) { (void)fmt; }
void net_poll(void) {}
void net_idle(void) {}

/* ip.c hands fragments to reasm and upper layers; neither is under test here.
 * This test only drives ip_send, which never reaches either. */
#include "reasm.h"
int reasm_input(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                const uint8_t *iph, uint8_t ihl,
                uint16_t off, int more, const uint8_t *data, uint16_t dlen,
                struct reasm_dgram *out)
{ (void)src;(void)dst;(void)proto;(void)id;(void)iph;(void)ihl;
  (void)off;(void)more;(void)data;(void)dlen;(void)out; return 0; }
void reasm_release(struct reasm_dgram *g) { (void)g; }

#include "arp.c"
#include "ip.c"

/* ------------------------------------------------------------------------ */

static int fails, checks;
#define CHECK(cond, msg) do { checks++; \
    if (cond) printf("ok   %s\n", (msg)); \
    else { printf("FAIL %s (%s:%d)\n", (msg), __FILE__, __LINE__); fails++; } } while (0)

static void reset(void)
{
    nwire = 0;
    ticks = 1000;
    arp_reset();
}

/* Count the frames of a given ethertype currently captured. */
static int count_type(uint16_t type)
{
    int n = 0;
    for (int i = 0; i < nwire; i++) if (wire[i].type == type) n++;
    return n;
}

/* The IP datagram we sent, if one went out. Returns its index or -1. */
static int find_ip_to(const uint8_t mac[6], uint32_t dst)
{
    for (int i = 0; i < nwire; i++) {
        if (wire[i].type != ETHERTYPE_IP) continue;
        if (memcmp(wire[i].dst, mac, 6) != 0) continue;
        if (wire[i].len < 20) continue;
        uint32_t d;
        memcpy(&d, wire[i].data + 16, 4);
        if (ntohl(d) == dst) return i;
    }
    return -1;
}

/* Feed arp_input an ARP reply from `ip`/`mac` addressed to us. */
static void deliver_reply(uint32_t ip, const uint8_t mac[6])
{
    uint8_t f[64];
    memset(f, 0, sizeof f);
    memcpy(f, our_mac, 6);              /* eth dst = us */
    memcpy(f + 6, mac, 6);              /* eth src */
    f[12] = 0x08; f[13] = 0x06;         /* ETHERTYPE_ARP */
    uint8_t *a = f + 14;
    a[0] = 0; a[1] = 1;                 /* htype ethernet */
    a[2] = 0x08; a[3] = 0x00;           /* ptype IPv4 */
    a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = 2;                 /* opcode reply */
    memcpy(a + 8, mac, 6);
    a[14] = (uint8_t)(ip >> 24); a[15] = (uint8_t)(ip >> 16);
    a[16] = (uint8_t)(ip >> 8);  a[17] = (uint8_t)ip;
    memcpy(a + 18, our_mac, 6);
    a[24] = (uint8_t)(OUR_IP >> 24); a[25] = (uint8_t)(OUR_IP >> 16);
    a[26] = (uint8_t)(OUR_IP >> 8);  a[27] = (uint8_t)OUR_IP;
    arp_input(f, 42);
}

int main(void)
{
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    /* ---- 1. A COLD NEXT HOP. The whole point of the change. -------------
     * ip_send must be ACCEPTED (0), must not put an IP frame on the wire yet,
     * and must have caused exactly one ARP solicitation. */
    reset();
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "cold next hop: ip_send is accepted, not refused");
    CHECK(count_type(ETHERTYPE_ARP) == 1,
          "cold next hop: exactly one solicitation went out");
    CHECK(count_type(ETHERTYPE_IP) == 0,
          "cold next hop: the datagram is held, not transmitted to nobody");

    /* ...and when the reply arrives, the HELD datagram is delivered. This is
     * the assertion that fails without the wiring: the old code had already
     * destroyed it, so no amount of later resolution could produce it. */
    deliver_reply(PEER_IP, peer_mac);
    int idx = find_ip_to(peer_mac, PEER_IP);
    CHECK(idx >= 0, "cold next hop: the held datagram is sent on the ARP reply");
    if (idx >= 0) {
        CHECK(wire[idx].len == 20 + (int)sizeof payload,
              "held datagram: length is header + payload");
        CHECK(memcmp(wire[idx].data + 20, payload, sizeof payload) == 0,
              "held datagram: payload survives the queue byte for byte");
        uint32_t src;
        memcpy(&src, wire[idx].data + 12, 4);
        CHECK(ntohl(src) == OUR_IP, "held datagram: source address intact");
        CHECK(wire[idx].data[9] == IP_PROTO_UDP,
              "held datagram: protocol intact");
        CHECK(ip_checksum(wire[idx].data, 20) == 0,
              "held datagram: header checksum still verifies");
    }

    /* ---- 2. A WARM NEXT HOP IS UNCHANGED. -------------------------------
     * The change must be a strict improvement at the call site: once the entry
     * is REACHABLE, ip_send transmits immediately and synchronously. */
    nwire = 0;
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "warm next hop: ip_send returns 0");
    CHECK(count_type(ETHERTYPE_ARP) == 0,
          "warm next hop: no solicitation");
    CHECK(find_ip_to(peer_mac, PEER_IP) >= 0,
          "warm next hop: transmitted in the same call");

    /* ---- 3. OFF-SUBNET GOES TO THE GATEWAY'S MAC, not the destination's. */
    reset();
    CHECK(ip_send(FAR_IP, IP_PROTO_TCP, payload, sizeof payload) == 0,
          "off-subnet: accepted");
    deliver_reply(GW_IP, gw_mac);
    CHECK(find_ip_to(gw_mac, FAR_IP) >= 0,
          "off-subnet: held datagram goes to the gateway MAC, dst is 8.8.8.8");

    /* ---- 4. BROADCAST IS NEVER ARP'd. ------------------------------------
     * There is no single next hop to resolve, so it must go out immediately
     * with no solicitation at all -- this is the path DHCP DISCOVER takes and
     * queueing it would deadlock the lease. */
    reset();
    CHECK(ip_send(0xFFFFFFFFu, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "broadcast: sent");
    CHECK(count_type(ETHERTYPE_ARP) == 0,
          "broadcast: no solicitation");
    CHECK(find_ip_to(eth_broadcast, 0xFFFFFFFFu) >= 0,
          "broadcast: transmitted to ff:ff:ff:ff:ff:ff immediately");

    /* ---- 5. A NEXT HOP THAT NEVER ANSWERS still terminates. --------------
     * Queueing must not become an unbounded hold: after the solicitations are
     * exhausted the entry fails, the queue is dropped, and a later ip_send is
     * refused rather than queued behind a dead address. */
    reset();
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "silent next hop: first datagram accepted");
    /* Three polls is exactly MAX_MCAST_SOLICIT: the entry reaches ARP_FAILED
     * and has not yet aged out of the negative cache. Polling further would
     * free it and the next send would legitimately start a fresh resolution --
     * which is correct behaviour, and would make this check assert the
     * opposite of what it says. */
    for (int i = 0; i < 3; i++) { ticks += 1000; arp_poll(); }
    CHECK(count_type(ETHERTYPE_IP) == 0,
          "silent next hop: nothing was ever transmitted to it");
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == -1,
          "silent next hop: a send during the negative cache is refused, not queued");
    /* The bound that makes queueing safe: solicitation is capped by
     * MAX_MCAST_SOLICIT, so a held datagram cannot turn ip_send into an
     * unbounded broadcast source. Long after the address has given up, the
     * total is still a handful -- not one per poll. */
    int solicits = count_type(ETHERTYPE_ARP);
    CHECK(solicits > 0 && solicits <= 5,
          "silent next hop: solicitation is bounded, not one per poll");

    printf("\nip_arp_test: %d checks, %d failed\n", checks, fails);
    if (fails) { printf("ip_arp_test: FAIL\n"); return 1; }
    printf("ip_arp_test: ALL PASS\n");
    return 0;
}
