/* THE CALL SITE. tests/unit/route_test.c proves the table answers correctly;
 * this proves ip_send() asks it.
 *
 * That distinction is the one this tree keeps re-learning -- the WPT runner
 * linked layout.c and never called layout_page(), the cookie jar's rule was
 * gated at one of its two callers -- so a routing table with no test of the
 * one function that consults it would be a table nothing routes through.
 *
 * White-box, in the shape of tests/unit/ip_arp_test.c: it #includes route.c
 * and ip.c and stubs the two things directly below them (arp_output, which is
 * where a routed datagram goes, and eth_send, which is where a broadcast or a
 * loopback datagram goes). arp.c itself is NOT linked -- the neighbour cache
 * is ip_arp_test's subject, and mixing the two would make a routing failure
 * look like an ARP failure.
 *
 * THE HEADLINE CASE is 127.0.0.1. Before the table, ip.c's routing decision
 * was one ternary over net_cfg.mask, 127.0.0.1 failed its on-subnet test, and
 * the datagram was therefore ARP'd for the DEFAULT GATEWAY and put on the
 * wire. There is a check below that would have caught that, and it is the
 * cheapest possible proof that the table is really being consulted.
 *
 * Built with -DLOGIT_NET_HOST so net_lock() is a no-op (cli/sti are ring 0).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "net.h"
#include "eth.h"
#include "arp.h"
#include "route.h"

#define OUR_IP   0x0A00020Fu       /* 10.0.2.15 */
#define GW_IP    0x0A000202u       /* 10.0.2.2  */
#define PEER_IP  0x0A000209u       /* 10.0.2.9, on-subnet */
#define FAR_IP   0x08080808u       /* 8.8.8.8 */
#define LOOP1    0x7F000001u       /* 127.0.0.1 */
#define LOOP2    0x7F000002u       /* 127.0.0.2 */
#define ETH1_IP  0xAC140005u       /* 172.20.0.5 */
#define ETH1_PEER 0xAC140009u      /* 172.20.0.9 */
#define ETH1_IF  3

#define OUR_MAC  0x52,0x54,0x00,0x12,0x34,0x56

struct net_config net_cfg = {
    .mac = { OUR_MAC },
    .ip = OUR_IP, .mask = 0xFFFFFF00u, .gw = GW_IP, .dns = 0x0A000203u,
};

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t our_mac[6] = { OUR_MAC };

/* --- what went where ----------------------------------------------------- */

#define NCAP 32
static struct { uint32_t nexthop; uint16_t type, len; uint8_t data[1600]; } arps[NCAP];
static int narp;
static struct { uint8_t dst[6]; uint16_t type, len; uint8_t data[1600]; } eths[NCAP];
static int neth;

int arp_output(uint32_t nexthop, uint16_t ethertype, const void *payload, uint16_t len)
{
    if (narp >= NCAP || len > sizeof arps[0].data) return -1;
    arps[narp].nexthop = nexthop; arps[narp].type = ethertype;
    arps[narp].len = len; memcpy(arps[narp].data, payload, len);
    narp++;
    return 0;
}

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type, const void *payload, uint16_t len)
{
    if (neth >= NCAP || len > sizeof eths[0].data) return -1;
    memcpy(eths[neth].dst, dst, 6);
    eths[neth].type = type; eths[neth].len = len;
    memcpy(eths[neth].data, payload, len);
    neth++;
    return 0;
}

void kprintf(const char *fmt, ...) { (void)fmt; }
void net_poll(void) {}
void net_idle(void) {}

/* ip.c hands fragments to reasm; nothing here is a fragment. */
#include "reasm.h"
int reasm_input(uint32_t src, uint32_t dst, uint8_t proto, uint16_t id,
                const uint8_t *iph, uint8_t ihl, uint16_t off, int more,
                const uint8_t *data, uint16_t dlen, struct reasm_dgram *out)
{ (void)src;(void)dst;(void)proto;(void)id;(void)iph;(void)ihl;
  (void)off;(void)more;(void)data;(void)dlen;(void)out; return 0; }
void reasm_release(struct reasm_dgram *g) { (void)g; }

/* netdev_primary_ifindex / netdev_loopback_ifindex are WEAK in ip.c and are
 * deliberately left undefined here: this test is the case the weak
 * declarations exist for -- ip.c compiled with no driver layer at all -- so it
 * is also the thing that proves the fallback constants in route.h are the ones
 * ip.c actually uses. */

#include "route.c"
#include "ip.c"

/* ------------------------------------------------------------------------ */

static int checks, fails;
#define CHECK(cond, msg) do { checks++; \
    if (cond) printf("ok   %s\n", (msg)); \
    else { printf("FAIL %s (%s:%d)\n", (msg), __FILE__, __LINE__); fails++; } } while (0)

static const uint8_t payload[25] = "routing-table-test-paylo";

static void reset(void) { narp = 0; neth = 0; }

static uint32_t hdr_src(const uint8_t *pkt)
{
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    return ntohl(h->src);
}
static uint32_t hdr_dst(const uint8_t *pkt)
{
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    return ntohl(h->dst);
}

int main(void)
{
    route_flush();

    /* ---- on-link ------------------------------------------------------- */
    reset();
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "on-subnet destination is accepted");
    CHECK(narp == 1 && arps[0].nexthop == PEER_IP,
          "on-subnet destination is its own next hop");
    CHECK(narp == 1 && hdr_src(arps[0].data) == OUR_IP,
          "source address is the route's, and it is 10.0.2.15");
    CHECK(neth == 0, "an on-link datagram does not bypass the neighbour cache");

    /* ---- default route ------------------------------------------------- */
    reset();
    CHECK(ip_send(FAR_IP, IP_PROTO_UDP, payload, sizeof payload) == 0,
          "off-subnet destination is accepted");
    CHECK(narp == 1 && arps[0].nexthop == GW_IP,
          "off-subnet destination takes the default route via the gateway");
    CHECK(narp == 1 && hdr_dst(arps[0].data) == FAR_IP,
          "the datagram still carries the real destination, not the gateway");

    /* ---- LOOPBACK: the case the ternary could not express ---------------- */
    reset();
    CHECK(ip_send(LOOP1, IP_PROTO_ICMP, payload, sizeof payload) == 0,
          "127.0.0.1 is accepted");
    CHECK(narp == 0,
          "127.0.0.1 is NOT handed to the neighbour cache (no MAC can own it)");
    CHECK(neth == 1 && memcmp(eths[0].dst, our_mac, 6) == 0,
          "127.0.0.1 is delivered through eth.c's loopback path, to our own MAC");
    CHECK(neth == 1 && hdr_src(eths[0].data) == LOOP1,
          "a loopback datagram's source is 127.0.0.1, not the card's address");

    reset();
    ip_send(LOOP2, IP_PROTO_ICMP, payload, sizeof payload);
    CHECK(narp == 0 && neth == 1,
          "the whole 127/8 block loops back, not just 127.0.0.1");

    /* THE REGRESSION THIS FILE EXISTS FOR, stated as its own check: under the
     * old ternary both of the above went to arp_output(GW_IP) and onto the
     * wire. Assert the negation directly so the failure names itself. */
    reset();
    ip_send(LOOP1, IP_PROTO_ICMP, payload, sizeof payload);
    CHECK(!(narp == 1 && arps[0].nexthop == GW_IP),
          "a loopback datagram is never ARP'd for the default gateway");

    /* ---- broadcast is still not routed ---------------------------------- */
    reset();
    ip_send(0xFFFFFFFFu, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(neth == 1 && memcmp(eths[0].dst, eth_broadcast, 6) == 0,
          "the limited broadcast goes straight to ff:ff:ff:ff:ff:ff");
    reset();
    ip_send(0x0A0002FFu, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(neth == 1 && memcmp(eths[0].dst, eth_broadcast, 6) == 0,
          "a subnet-directed broadcast is broadcast, not routed");

    /* ---- the source address comes from the ROUTE ------------------------ */
    reset();
    route_add((struct route_entry){ .dst = 0xAC140000u, .plen = 16,
                                    .nexthop = 0, .src = ETH1_IP,
                                    .oif = ETH1_IF, .metric = 0 });
    ip_send(ETH1_PEER, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(narp == 1 && arps[0].nexthop == ETH1_PEER,
          "a second interface's subnet is on-link on that interface");
    CHECK(narp == 1 && hdr_src(arps[0].data) == ETH1_IP,
          "and the datagram is sourced from THAT interface's address, "
          "not net_cfg.ip");
    route_del(0xAC140000u, 16, ETH1_IF);

    /* ---- no route at all ------------------------------------------------ */
    reset();
    uint32_t before = ip_no_route_count();
    route_flush();                 /* also clears route_sync's per-if memo */
    net_cfg.gw = 0;                /* a machine with an address and no gateway */
    CHECK(ip_send(FAR_IP, IP_PROTO_UDP, payload, sizeof payload) == -1,
          "with no default route an off-subnet destination is REFUSED");
    CHECK(narp == 0 && neth == 0,
          "and nothing was put on the wire on the way to that refusal");
    CHECK(ip_no_route_count() == before + 1,
          "the refusal is counted as a routing failure, not a device failure");

    /* Discriminating: the same machine still reaches what it does have a
     * route to. A table that refuses everything would pass the three checks
     * above and is exactly what this one rules out. */
    reset();
    CHECK(ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload) == 0 &&
          narp == 1 && arps[0].nexthop == PEER_IP,
          "the on-subnet destination still resolves with no default route");
    reset();
    CHECK(ip_send(LOOP1, IP_PROTO_ICMP, payload, sizeof payload) == 0 && neth == 1,
          "loopback still works on a machine with no gateway");
    net_cfg.gw = GW_IP;

    /* ---- the per-send sync is free after the first ---------------------- */
    reset();
    ip_send(FAR_IP, IP_PROTO_UDP, payload, sizeof payload);
    uint32_t g = route_generation();
    for (int i = 0; i < 50; i++)
        ip_send(FAR_IP, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(route_generation() == g,
          "50 further datagrams did not touch the table (the memo holds)");

    /* ---- a lease change is picked up without anyone announcing it -------- */
    reset();
    net_cfg.ip = 0xC0A80132u;      /* 192.168.1.50 */
    net_cfg.mask = 0xFFFFFF00u;
    net_cfg.gw = 0xC0A80101u;      /* 192.168.1.1 */
    ip_send(0xC0A80109u, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(narp == 1 && arps[0].nexthop == 0xC0A80109u,
          "after a lease change the new subnet is on-link");
    reset();
    ip_send(PEER_IP, IP_PROTO_UDP, payload, sizeof payload);
    CHECK(narp == 1 && arps[0].nexthop == 0xC0A80101u,
          "and the OLD subnet is no longer on-link -- it takes the new default");

    printf("\nip_route_test: %d checks, %d failed\n", checks, fails);
    if (fails) { printf("ip_route_test: FAILURES\n"); return 1; }
    printf("ip_route_test: ALL PASS\n");
    return 0;
}
