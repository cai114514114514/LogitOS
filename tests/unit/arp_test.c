/* Host tests for the IPv4 neighbour cache.
 *
 * White-box: this #includes arp.c and stubs only eth_send, the clock and the
 * console, so every byte checked below is a byte the kernel would really put on
 * the wire and every transition is the real one.
 *
 * What this is for, in order of how badly it would hurt to get wrong:
 *
 *  1. THE TRUST RULE. ARP has no authentication and never will; the only
 *     defence a host has is being careful about WHEN it believes a hardware
 *     address. The version of arp.c this replaces believed all of them, from
 *     anybody, unsolicited -- one broadcast frame from any host on the segment
 *     put that host in the middle of every connection the machine had. The
 *     poisoning and exhaustion cases here are the reason the file was rewritten
 *     and they are what the negative control switches off.
 *
 *  2. STATE OVER TIME. A cache with no clock is not a cache, it is a
 *     permanent assignment; the failure mode is a gateway that changed its MAC
 *     and a machine that transmits into the void until it is rebooted. Those
 *     transitions cannot be seen in a packet capture -- an entry that never
 *     leaves REACHABLE looks exactly like one that is being refreshed -- so
 *     they are stepped here against a controllable clock.
 *
 *  3. MALFORMED INPUT IN RING 0. arp_input consumes frames straight off the
 *     wire, inside the receive path, with no privilege boundary underneath it.
 *     Every shape below must be REJECTED rather than tolerated, and the fuzz
 *     case at the end feeds it several hundred thousand random frames.
 *
 * Built with -DLOGIT_NET_HOST so net_lock() is a no-op (cli/sti are ring 0). */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "net.h"
#include "eth.h"

#define OUR_IP   0x0A00020Fu       /* 10.0.2.15 */
#define GW_IP    0x0A000202u       /* 10.0.2.2  */
#define PEER_IP  0x0A000209u       /* 10.0.2.9  */

#define OUR_MAC  0x52,0x54,0x00,0x12,0x34,0x56
#define GW_MAC   0x52,0x55,0x0A,0x00,0x02,0x02
#define EVIL_MAC 0xDE,0xAD,0xBE,0xEF,0x00,0x01
#define PEER_MAC 0x52,0x55,0x0A,0x00,0x02,0x09

struct net_config net_cfg = {
    .mac = { OUR_MAC },
    .ip = OUR_IP, .mask = 0xFFFFFF00u, .gw = GW_IP, .dns = 0x0A000203u,
};

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static const uint8_t our_mac[6]  = { OUR_MAC };
static const uint8_t gw_mac[6]   = { GW_MAC };
static const uint8_t evil_mac[6] = { EVIL_MAC };
static const uint8_t peer_mac[6] = { PEER_MAC };

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }

/* --- captured transmissions --------------------------------------------- */
#define NWIRE 256
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

static int quiet;
void kprintf(const char *fmt, ...)
{
    if (quiet) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void net_poll(void) {}
void net_idle(void) {}

#include "arp.c"

/* --- harness ------------------------------------------------------------- */

static int failures, checks;
#define CHECK(cond, ...) do {                                       \
    checks++;                                                       \
    if (!(cond)) { failures++;                                      \
        printf("FAIL %s:%d: ", __func__, __LINE__);                 \
        printf(__VA_ARGS__); printf("\n"); }                        \
} while (0)

static void reset(void)
{
    arp_reset();
    nwire = 0;
    ticks = 1000;                 /* not 0: catches "uninitialised timer" reads */
    net_cfg.ip = OUR_IP;
    memcpy(net_cfg.mac, our_mac, 6);
}

/* Advance the clock, running arp_poll once per tick as net_poll would. */
static void advance(uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) { ticks++; arp_poll(); }
}

/* --- frame construction -------------------------------------------------- */

static uint8_t frame[64];

/* Build an ARP frame exactly as it appears on the wire, with every field
 * settable, so the malformed cases below can be built by changing one of
 * them. Returns the total frame length. */
static uint16_t build(uint16_t htype, uint16_t ptype, uint8_t hlen, uint8_t plen,
                      uint16_t op, const uint8_t *sha, uint32_t spa,
                      const uint8_t *tha, uint32_t tpa, const uint8_t *eth_dst)
{
    memset(frame, 0, sizeof frame);
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, eth_dst, 6);
    memcpy(e->src, sha, 6);
    e->ethertype = htons(ETHERTYPE_ARP);
    struct arp_pkt *p = (struct arp_pkt *)(frame + sizeof *e);
    p->htype = htons(htype);
    p->ptype = htons(ptype);
    p->hlen = hlen; p->plen = plen;
    p->op = htons(op);
    memcpy(p->sha, sha, 6);
    p->spa = htonl(spa);
    memcpy(p->tha, tha, 6);
    p->tpa = htonl(tpa);
    return (uint16_t)(sizeof *e + sizeof *p);
}

static const uint8_t zero_mac[6] = { 0, 0, 0, 0, 0, 0 };

/* A well-formed request, broadcast, from `sha`/`spa`, asking about `tpa`. */
static void rx_request(const uint8_t *sha, uint32_t spa, uint32_t tpa)
{
    uint16_t n = build(1, 0x0800, 6, 4, ARP_OP_REQUEST, sha, spa,
                       zero_mac, tpa, eth_broadcast);
    arp_input(frame, n);
}

/* A reply unicast to us -- i.e. one we solicited. */
static void rx_reply_to_us(const uint8_t *sha, uint32_t spa)
{
    uint16_t n = build(1, 0x0800, 6, 4, ARP_OP_REPLY, sha, spa,
                       our_mac, OUR_IP, our_mac);
    arp_input(frame, n);
}

/* A reply that is NOT for us: broadcast, target hardware address someone
 * else's. This is the shape a poisoning tool sends. */
static void rx_reply_unsolicited(const uint8_t *sha, uint32_t spa)
{
    uint16_t n = build(1, 0x0800, 6, 4, ARP_OP_REPLY, sha, spa,
                       zero_mac, spa, eth_broadcast);
    arp_input(frame, n);
}

/* --- wire inspection ----------------------------------------------------- */

static const struct arp_pkt *sent(int i)
{
    return (const struct arp_pkt *)wire[i].data;
}
static int n_requests(void)
{
    int n = 0;
    for (int i = 0; i < nwire; i++)
        if (ntohs(sent(i)->op) == ARP_OP_REQUEST) n++;
    return n;
}
static int n_replies(void)
{
    int n = 0;
    for (int i = 0; i < nwire; i++)
        if (ntohs(sent(i)->op) == ARP_OP_REPLY) n++;
    return n;
}
static int n_broadcast(void)
{
    int n = 0;
    for (int i = 0; i < nwire; i++)
        if (memcmp(wire[i].dst, eth_broadcast, 6) == 0) n++;
    return n;
}

/* ======================================================================== */

/* Resolution asks ONCE. The old arp_resolve broadcast on every call, so a
 * caller that retried in a loop broadcast in a loop -- sock.c carries a
 * hand-written rate limiter whose comment says exactly that. The pacing has to
 * be here, or every future caller has to remember to write its own. */
static void t_resolve_asks_once(void)
{
    reset();
    uint8_t mac[6];

    CHECK(arp_resolve(GW_IP, mac) == -1, "cold resolve must miss");
    CHECK(n_requests() == 1, "cold resolve should send exactly one request, sent %d",
          n_requests());
    CHECK(arp_query_state(GW_IP) == ARP_INCOMPLETE, "cold entry should be INCOMPLETE");
    CHECK(memcmp(wire[0].dst, eth_broadcast, 6) == 0, "first solicitation must broadcast");
    CHECK(ntohl(sent(0)->tpa) == GW_IP, "solicitation must ask about the target");
    CHECK(ntohl(sent(0)->spa) == OUR_IP, "solicitation must carry our address");
    CHECK(memcmp(sent(0)->sha, our_mac, 6) == 0, "solicitation must carry our MAC");

    /* Hammer it the way a retrying caller would. */
    for (int i = 0; i < 100; i++) (void)arp_resolve(GW_IP, mac);
    CHECK(n_requests() == 1, "100 resolves of an in-flight address sent %d requests",
          n_requests());
}

/* A reply we asked for makes the entry usable. */
static void t_reply_completes(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);

    CHECK(arp_query_state(GW_IP) == ARP_REACHABLE, "solicited reply should be REACHABLE");
    CHECK(arp_resolve(GW_IP, mac) == 0, "resolve after reply must hit");
    CHECK(memcmp(mac, gw_mac, 6) == 0, "resolved the wrong MAC");
}

/* The whole ageing ladder, which the old cache did not have at all. */
static void t_ageing_ladder(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);
    nwire = 0;

    advance(REACHABLE_TIME + 2);
    CHECK(arp_query_state(GW_IP) == ARP_STALE,
          "REACHABLE must age to STALE, got %d", arp_query_state(GW_IP));
    CHECK(nwire == 0, "ageing to STALE must not transmit (sent %d)", nwire);

    /* A stale entry is still USABLE -- refusing it would cost a round trip on
     * every binding that happened to age out between two fetches. */
    CHECK(arp_resolve(GW_IP, mac) == 0, "STALE entry must still resolve");
    CHECK(memcmp(mac, gw_mac, 6) == 0, "STALE entry returned the wrong MAC");
    CHECK(arp_query_state(GW_IP) == ARP_DELAY, "using a STALE entry must arm DELAY");
    CHECK(nwire == 0, "using a STALE entry must not transmit immediately");

    advance(DELAY_FIRST_PROBE + 2);
    CHECK(arp_query_state(GW_IP) == ARP_PROBE, "DELAY must become PROBE");
    CHECK(n_requests() == 1, "DELAY->PROBE should solicit once, sent %d", n_requests());
    CHECK(memcmp(wire[0].dst, gw_mac, 6) == 0,
          "a probe must be UNICAST to the address held, not broadcast");
    CHECK(n_broadcast() == 0, "probing an entry we hold must not broadcast");
}

/* An address that stops answering: probes exhaust, the dead binding is
 * forgotten, and we fall back to a broadcast search. This is the MAC-changed
 * case, and the old cache could never recover from it. */
static void t_probe_exhausts_to_broadcast(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);
    advance(REACHABLE_TIME + 2);
    (void)arp_resolve(GW_IP, mac);            /* STALE -> DELAY */
    advance(DELAY_FIRST_PROBE + 2);           /* -> PROBE */
    nwire = 0;

    advance(RETRANS_TIMER * (MAX_UCAST_SOLICIT + 1) + 4);
    CHECK(arp_query_state(GW_IP) == ARP_INCOMPLETE,
          "an unanswered probe must fall back to INCOMPLETE, got %d",
          arp_query_state(GW_IP));
    CHECK(n_broadcast() >= 1, "the fallback must broadcast to find the new owner");

    /* And the new owner is then adopted. */
    rx_reply_to_us(peer_mac, GW_IP);
    CHECK(arp_resolve(GW_IP, mac) == 0 && memcmp(mac, peer_mac, 6) == 0,
          "the new MAC must be adopted after the old one stopped answering");
}

/* An address nobody answers for must be given up on, and must then stay given
 * up on -- that negative cache is the only thing bounding how much broadcast a
 * hard-retrying caller can generate. */
static void t_unanswered_becomes_failed(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(PEER_IP, mac);
    advance(RETRANS_TIMER * (MAX_MCAST_SOLICIT + 1) + 4);

    CHECK(arp_query_state(PEER_IP) == ARP_FAILED,
          "an unanswered address must end in FAILED, got %d", arp_query_state(PEER_IP));
    CHECK(n_requests() == MAX_MCAST_SOLICIT,
          "expected %d solicitations before giving up, sent %d",
          MAX_MCAST_SOLICIT, n_requests());

    nwire = 0;
    for (int i = 0; i < 500; i++) CHECK(arp_resolve(PEER_IP, mac) == -1, "FAILED must miss");
    CHECK(nwire == 0, "500 resolves of a FAILED address transmitted %d frames", nwire);

    /* But it must not be permanent. */
    advance(FAILED_HOLD + 4);
    CHECK(arp_query_state(PEER_IP) == ARP_FREE, "FAILED must expire");
    CHECK(arp_resolve(PEER_IP, mac) == -1 && n_requests() == 1,
          "resolution must be retried once the hold expires");
}

/* ---- the trust rule ---------------------------------------------------- */

/* THE ONE THAT MATTERS. An established binding must not be replaced by an
 * unsolicited frame. */
static void t_poisoning_refused(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);
    CHECK(arp_query_state(GW_IP) == ARP_REACHABLE, "setup: gateway should be REACHABLE");
    nwire = 0;

    /* The attack, in both of the shapes it is sent in. */
    rx_reply_unsolicited(evil_mac, GW_IP);
    CHECK(arp_resolve(GW_IP, mac) == 0 && memcmp(mac, gw_mac, 6) == 0,
          "an unsolicited reply REPLACED the gateway's MAC");
    CHECK(arp_get_stats()->poison_blocked >= 1, "the refusal must be counted");

    reset();
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);
    rx_request(evil_mac, GW_IP, OUR_IP);       /* gratuitous-ish: a request claiming GW_IP */
    CHECK(arp_resolve(GW_IP, mac) == 0 && memcmp(mac, gw_mac, 6) == 0,
          "a broadcast request REPLACED the gateway's MAC");

    /* Refusing must not mean ignoring: the doubt has to be resolved, and the
     * genuine owner has to win. */
    CHECK(arp_query_state(GW_IP) == ARP_PROBE,
          "a conflicting claim must trigger re-verification, state %d",
          arp_query_state(GW_IP));
    rx_reply_to_us(gw_mac, GW_IP);
    CHECK(arp_query_state(GW_IP) == ARP_REACHABLE && memcmp(mac, gw_mac, 6) == 0,
          "the real owner's answer must restore the binding");
}

/* A MAC change we ASKED about is accepted -- the rule is about solicitation,
 * not about refusing all change, and a stack that never accepts a new address
 * is as broken as one that accepts every address. */
static void t_solicited_change_accepted(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);
    rx_reply_to_us(peer_mac, GW_IP);          /* solicited, different MAC */
    CHECK(arp_resolve(GW_IP, mac) == 0 && memcmp(mac, peer_mac, 6) == 0,
          "a solicited reply must be able to update the binding");
}

/* RFC 826's merge rule: a frame that is neither addressed to us nor about a
 * host we already know creates NOTHING. Without this the table is filled by
 * whatever is loudest on the segment. */
static void t_stranger_creates_nothing(void)
{
    reset();
    rx_request(peer_mac, PEER_IP, GW_IP);      /* a conversation between two others */
    CHECK(arp_query_state(PEER_IP) == ARP_FREE,
          "a request not addressed to us must not create an entry");
    CHECK(arp_cache_entries() == 0, "cache should be empty, has %d", arp_cache_entries());
    CHECK(n_replies() == 0, "we must not answer for an address that is not ours");

    /* But a request addressed to US both learns and answers. */
    rx_request(peer_mac, PEER_IP, OUR_IP);
    CHECK(arp_query_state(PEER_IP) == ARP_STALE,
          "a request addressed to us should learn the sender (state %d)",
          arp_query_state(PEER_IP));
    CHECK(n_replies() == 1, "a request for our address must be answered");
    CHECK(memcmp(wire[nwire - 1].dst, peer_mac, 6) == 0, "the reply must be unicast to the asker");
    CHECK(ntohl(sent(nwire - 1)->spa) == OUR_IP, "the reply must carry our address");
}

/* Sixteen forged frames used to evict every real binding in the table. */
static void t_cache_not_exhaustible(void)
{
    reset();
    uint8_t mac[6];
    (void)arp_resolve(GW_IP, mac);
    rx_reply_to_us(gw_mac, GW_IP);

    /* A flood of gratuitous ARPs from hosts we have never spoken to, four
     * times the size of the table. */
    for (int i = 0; i < ARP_CACHE * 4; i++) {
        uint8_t m[6] = { 0x02, 0x00, (uint8_t)(i >> 8), (uint8_t)i, 0x00, 0x01 };
        uint32_t ip = 0x0A000000u | (uint32_t)(i + 100);
        rx_request(m, ip, ip);                 /* gratuitous: sender == target */
    }

    CHECK(arp_resolve(GW_IP, mac) == 0 && memcmp(mac, gw_mac, 6) == 0,
          "a flood of strangers evicted the gateway binding");
    CHECK(arp_cache_entries() <= 1,
          "strangers should occupy no slots, cache holds %d", arp_cache_entries());
    CHECK(arp_get_stats()->evict == 0, "nothing should have been evicted, %u were",
          arp_get_stats()->evict);
}

/* RFC 5227: somebody else using our address. */
static void t_conflict_detected(void)
{
    reset();
    quiet = 1;
    rx_request(evil_mac, OUR_IP, GW_IP);       /* their spa is OUR address */
    quiet = 0;

    CHECK(arp_get_stats()->conflicts == 1, "the conflict must be counted, got %u",
          arp_get_stats()->conflicts);
    CHECK(n_replies() == 1, "a conflict must be defended with our own binding");
    CHECK(ntohl(sent(0)->spa) == OUR_IP && memcmp(sent(0)->sha, our_mac, 6) == 0,
          "the defence must assert OUR address and OUR MAC");
    CHECK(arp_query_state(OUR_IP) == ARP_FREE,
          "an impostor must not become the cache entry for our own address");

    /* And our own address must still resolve to us, not to them. */
    uint8_t mac[6];
    CHECK(arp_resolve(OUR_IP, mac) == 0 && memcmp(mac, our_mac, 6) == 0,
          "our own address resolved to the impostor's MAC");
}

static void t_announce(void)
{
    reset();
    arp_announce();
    CHECK(nwire == 1 && ntohs(sent(0)->op) == ARP_OP_REQUEST,
          "an announcement is a broadcast request");
    CHECK(memcmp(wire[0].dst, eth_broadcast, 6) == 0, "an announcement must broadcast");
    CHECK(ntohl(sent(0)->spa) == OUR_IP && ntohl(sent(0)->tpa) == OUR_IP,
          "an announcement has sender == target == our address");
}

/* ---- addressing without ARP -------------------------------------------- */

/* Our own address must be reachable from the machine itself. Before this it
 * was not: ip_send() to net_cfg.ip took the on-link path, asked ARP, broadcast
 * a request that no host answers (a NIC does not receive its own frames) and
 * never resolved. An OS that cannot address itself is one plenty of software
 * assumes it can. */
static void t_self_resolves(void)
{
    reset();
    uint8_t mac[6];
    CHECK(arp_resolve(OUR_IP, mac) == 0, "our own address must resolve");
    CHECK(memcmp(mac, our_mac, 6) == 0, "our own address must resolve to our own MAC");
    CHECK(nwire == 0, "resolving our own address must not put anything on the wire");
}

/* IPv4 multicast has a defined hardware address and is never ARP'd. ip_send
 * hands any non-broadcast destination here, so without this a multicast send
 * broadcast "who has 224.0.0.251" -- forever, on every packet. */
static void t_multicast_mapped(void)
{
    reset();
    uint8_t mac[6];
    static const uint8_t want[6] = { 0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB };
    CHECK(arp_resolve(0xE00000FBu, mac) == 0, "224.0.0.251 must resolve without ARP");
    CHECK(memcmp(mac, want, 6) == 0, "wrong multicast MAC mapping");
    CHECK(nwire == 0, "multicast must not generate an ARP request");

    /* The high bit of the second octet is NOT copied: 32 groups share a MAC. */
    static const uint8_t want2[6] = { 0x01, 0x00, 0x5E, 0x7F, 0xFF, 0xFA };
    CHECK(arp_resolve(0xEFFFFFFAu, mac) == 0 && memcmp(mac, want2, 6) == 0,
          "239.255.255.250 mapped wrongly (the 23-bit truncation)");
}

/* ---- the pending queue -------------------------------------------------- */

static void t_queue_holds_and_flushes(void)
{
    reset();
    uint8_t payload[100];
    for (int i = 0; i < 100; i++) payload[i] = (uint8_t)i;

    CHECK(arp_output(GW_IP, ETHERTYPE_IP, payload, sizeof payload) == 0,
          "a packet to an unresolved next hop must be accepted");
    CHECK(arp_get_stats()->queued == 1, "the packet should be queued");
    /* Only the solicitation is on the wire so far. */
    CHECK(n_requests() == 1 && nwire == 1, "queued packet was transmitted early");

    rx_reply_to_us(gw_mac, GW_IP);
    CHECK(arp_get_stats()->queue_sent == 1, "resolution must flush the queue");
    CHECK(nwire == 2 && wire[1].type == ETHERTYPE_IP && wire[1].len == 100,
          "the queued packet must go out as itself");
    CHECK(memcmp(wire[1].data, payload, 100) == 0, "the queued packet was corrupted");
    CHECK(memcmp(wire[1].dst, gw_mac, 6) == 0, "the queued packet went to the wrong MAC");
}

static void t_queue_keeps_the_newest(void)
{
    reset();
    uint8_t p[4];
    /* Overfill by one. RFC 1122: the packet to lose is the OLDEST. */
    for (int i = 0; i < ARP_QUEUE + 1; i++) {
        p[0] = (uint8_t)i;
        ticks++;                                 /* distinct queued_at */
        (void)arp_output(GW_IP, ETHERTYPE_IP, p, sizeof p);
    }
    CHECK(arp_get_stats()->queue_drop == 1, "overflow should drop exactly one");
    nwire = 0;
    rx_reply_to_us(gw_mac, GW_IP);
    CHECK(arp_get_stats()->queue_sent == ARP_QUEUE,
          "expected %d packets flushed, got %u", ARP_QUEUE, arp_get_stats()->queue_sent);
    int saw_first = 0, saw_last = 0;
    for (int i = 0; i < nwire; i++) {
        if (wire[i].data[0] == 0) saw_first = 1;
        if (wire[i].data[0] == ARP_QUEUE) saw_last = 1;
    }
    CHECK(!saw_first, "overflow discarded the NEWEST packet instead of the oldest");
    CHECK(saw_last, "the newest packet was not kept");
}

static void t_queue_expires_and_gives_up(void)
{
    reset();
    uint8_t p[4] = { 1, 2, 3, 4 };
    (void)arp_output(GW_IP, ETHERTYPE_IP, p, sizeof p);
    /* Nobody answers: the address fails, and the packets held for it must not
     * be delivered minutes later as duplicates. */
    advance(RETRANS_TIMER * (MAX_MCAST_SOLICIT + 1) + QUEUE_TTL + 8);
    CHECK(arp_get_stats()->queue_sent == 0, "a packet was delivered after resolution failed");
    CHECK(arp_get_stats()->queue_drop >= 1, "the abandoned packet must be counted as dropped");

    /* And a FAILED address must not accumulate more. */
    uint32_t before = arp_get_stats()->queued;
    (void)arp_output(PEER_IP, ETHERTYPE_IP, p, sizeof p);
    advance(RETRANS_TIMER * (MAX_MCAST_SOLICIT + 1) + 4);
    uint32_t mid = arp_get_stats()->queued;
    (void)arp_output(PEER_IP, ETHERTYPE_IP, p, sizeof p);
    CHECK(arp_get_stats()->queued == mid,
          "packets must not queue behind an address already known not to answer");
    (void)before;
}

/* ---- malformed input ---------------------------------------------------- */

static void t_malformed_rejected(void)
{
    uint16_t n;

    reset();
    n = build(1, 0x0800, 6, 4, ARP_OP_REQUEST, peer_mac, PEER_IP, zero_mac, OUR_IP, our_mac);
    for (uint16_t s = 0; s < n; s++) {          /* every truncation */
        arp_reset(); nwire = 0;
        arp_input(frame, s);
        CHECK(nwire == 0 && arp_cache_entries() == 0,
              "a %u-byte ARP frame produced output or state", s);
    }

    struct { const char *what; uint16_t htype, ptype; uint8_t hlen, plen, op; } bad[] = {
        { "htype != Ethernet",  2, 0x0800, 6, 4, ARP_OP_REQUEST },
        { "ptype != IPv4",      1, 0x86DD, 6, 4, ARP_OP_REQUEST },
        { "hlen != 6",          1, 0x0800, 8, 4, ARP_OP_REQUEST },
        { "plen != 4",          1, 0x0800, 6, 16, ARP_OP_REQUEST },
        { "hlen == 0",          1, 0x0800, 0, 4, ARP_OP_REQUEST },
        { "opcode 0",           1, 0x0800, 6, 4, 0 },
        { "opcode RARP",        1, 0x0800, 6, 4, 3 },
        { "opcode 255",         1, 0x0800, 6, 4, 255 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        reset();
        n = build(bad[i].htype, bad[i].ptype, bad[i].hlen, bad[i].plen, bad[i].op,
                  peer_mac, PEER_IP, zero_mac, OUR_IP, our_mac);
        arp_input(frame, n);
        CHECK(nwire == 0, "%s produced a transmission", bad[i].what);
        CHECK(arp_cache_entries() == 0, "%s created a cache entry", bad[i].what);
    }

    /* Sender hardware addresses that cannot belong to a host. Caching one makes
     * every packet to that binding a flood or a black hole. */
    static const uint8_t bcast_sha[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    static const uint8_t mcast_sha[6] = { 0x01,0x00,0x5E,0x00,0x00,0x01 };
    const uint8_t *bad_sha[] = { bcast_sha, mcast_sha, zero_mac };
    for (unsigned i = 0; i < 3; i++) {
        reset();
        n = build(1, 0x0800, 6, 4, ARP_OP_REQUEST, bad_sha[i], PEER_IP,
                  zero_mac, OUR_IP, our_mac);
        arp_input(frame, n);
        CHECK(arp_cache_entries() == 0, "a multicast/zero sender MAC was cached (case %u)", i);
        CHECK(nwire == 0, "a multicast/zero sender MAC was answered (case %u)", i);
    }

    /* Sender protocol addresses that cannot be a sender. The old code cached an
     * entry for 0.0.0.0 from every DHCP client on the segment. */
    uint32_t bad_spa[] = { 0x00000000u, 0xFFFFFFFFu, 0xE0000001u, 0x7F000001u };
    for (unsigned i = 0; i < 4; i++) {
        reset();
        n = build(1, 0x0800, 6, 4, ARP_OP_REPLY, peer_mac, bad_spa[i],
                  our_mac, OUR_IP, our_mac);
        arp_input(frame, n);
        CHECK(arp_cache_entries() == 0, "spa %08x was cached", bad_spa[i]);
    }

    /* ...but an RFC 5227 PROBE (spa == 0, a request) for our own address must
     * still be answered, or another host takes the address we are using. */
    reset();
    n = build(1, 0x0800, 6, 4, ARP_OP_REQUEST, peer_mac, 0, zero_mac, OUR_IP, eth_broadcast);
    arp_input(frame, n);
    CHECK(n_replies() == 1, "an address probe for our address must be answered");
    CHECK(arp_cache_entries() == 0, "an address probe must not create an entry");
}

/* Random frames into the receive path. This runs in ring 0 on untrusted input;
 * the bar is that nothing crashes, nothing is ever learned that should not be,
 * and the cache never overflows its bounds. */
static uint32_t rnd_state = 0x1234567u;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void t_fuzz(void)
{
    reset();
    quiet = 1;
    uint8_t buf[128];
    for (int iter = 0; iter < 300000; iter++) {
        uint16_t len = (uint16_t)(rnd() % sizeof buf);
        for (uint16_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        /* Half the time make it structurally valid so the fuzz reaches past the
         * header checks into the state machine, where the interesting bugs are. */
        if (len >= sizeof(struct eth_hdr) + sizeof(struct arp_pkt) && (rnd() & 1)) {
            struct arp_pkt *p = (struct arp_pkt *)(buf + sizeof(struct eth_hdr));
            p->htype = htons(1); p->ptype = htons(0x0800);
            p->hlen = 6; p->plen = 4;
            p->op = htons((rnd() & 1) ? ARP_OP_REQUEST : ARP_OP_REPLY);
            if (rnd() & 1) p->tpa = htonl(OUR_IP);
        }
        arp_input(buf, len);
        if ((iter & 0xFF) == 0) { ticks++; arp_poll(); }
        nwire = 0;
    }
    quiet = 0;

    /* Invariants that must hold no matter what was fed in. */
    CHECK(arp_cache_entries() <= ARP_CACHE, "the cache exceeded its own bound");
    uint8_t mac[6];
    CHECK(arp_resolve(OUR_IP, mac) == 0 && memcmp(mac, our_mac, 6) == 0,
          "fuzzing corrupted our own address binding");
    int qn = 0;
    for (int i = 0; i < ARP_QUEUE; i++) if (pendq[i].ip) qn++;
    CHECK(qn <= ARP_QUEUE, "the pending queue exceeded its own bound");
    for (int i = 0; i < ARP_CACHE; i++)
        CHECK(cache[i].state <= ARP_FAILED, "slot %d holds an invalid state %d",
              i, cache[i].state);
}

/* An entry that is USED stays; one that is not is reclaimed. The old eviction
 * always took slot 0, so slots 1..15 froze for the whole boot and every new
 * peer fought over the one remaining slot. */
static void t_eviction_is_least_recently_used(void)
{
    reset();
    uint8_t mac[6];
    /* Fill the table with real, learned entries (each addressed to us so the
     * merge rule creates them). */
    for (int i = 0; i < ARP_CACHE; i++) {
        uint8_t m[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, (uint8_t)i };
        uint32_t ip = 0x0A000000u | (uint32_t)(i + 20);
        rx_request(m, ip, OUR_IP);
    }
    CHECK(arp_cache_entries() == ARP_CACHE, "setup: table should be full, is %d",
          arp_cache_entries());

    /* Keep touching the FIRST one, so it is the most recently used. */
    uint32_t hot = 0x0A000000u | 20u;
    for (int i = 0; i < 5; i++) { ticks += 10; (void)arp_resolve(hot, mac); }

    /* Now force an eviction with a new, legitimate entry. */
    ticks += 10;
    rx_request(peer_mac, PEER_IP, OUR_IP);
    CHECK(arp_get_stats()->evict == 1, "expected exactly one eviction, got %u",
          arp_get_stats()->evict);
    CHECK(arp_query_state(hot) != ARP_FREE,
          "eviction took the MOST recently used entry");
    CHECK(arp_query_state(PEER_IP) != ARP_FREE, "the new entry was not installed");
}

int main(void)
{
    t_resolve_asks_once();
    t_reply_completes();
    t_ageing_ladder();
    t_probe_exhausts_to_broadcast();
    t_unanswered_becomes_failed();
    t_poisoning_refused();
    t_solicited_change_accepted();
    t_stranger_creates_nothing();
    t_cache_not_exhaustible();
    t_conflict_detected();
    t_announce();
    t_self_resolves();
    t_multicast_mapped();
    t_queue_holds_and_flushes();
    t_queue_keeps_the_newest();
    t_queue_expires_and_gives_up();
    t_malformed_rejected();
    t_eviction_is_least_recently_used();
    t_fuzz();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "arp ok",
           checks, failures);
    return failures != 0;
}
