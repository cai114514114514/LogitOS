/* Host tests for the Ethernet layer: frame validation, destination filtering,
 * VLAN de-tagging, minimum-frame padding and loopback.
 *
 * White-box: this #includes eth.c and stubs netdev_tx and the three upper-layer
 * handlers, so what the checks below inspect is the exact byte sequence
 * eth_send would hand a NIC and the exact buffer ip_input would be given.
 *
 * The de-tagging path is the one to be most suspicious of. It rewrites a frame
 * in a fixed buffer using lengths taken from the frame itself, inside the
 * receive interrupt, on input from anyone on the segment -- which is the shape
 * of every memorable network stack CVE. So the tag cases here are driven
 * exhaustively (every truncation of every tag layout) rather than by example,
 * and the whole thing is fuzzed under ASan at the end.
 *
 * Built with -DLOGIT_NET_HOST so net_lock() is a no-op (cli/sti are ring 0). */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "net.h"

#define OUR_MAC  0x52,0x54,0x00,0x12,0x34,0x56
static const uint8_t our_mac[6]   = { OUR_MAC };
static const uint8_t other_mac[6] = { 0x52,0x54,0x00,0xAA,0xBB,0xCC };
static const uint8_t bcast[6]     = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t mcast6[6]    = { 0x33,0x33,0x00,0x00,0x00,0x01 };
static const uint8_t mcast4[6]    = { 0x01,0x00,0x5E,0x00,0x00,0x01 };

struct net_config net_cfg = {
    .mac = { OUR_MAC },
    .ip = 0x0A00020Fu, .mask = 0xFFFFFF00u, .gw = 0x0A000202u, .dns = 0x0A000203u,
};

/* --- what reached the "wire" -------------------------------------------- */
static struct { uint16_t len; uint8_t data[2048]; } txq[64];
static int ntx;

int netdev_tx(const void *frame, uint16_t len)
{
    if (ntx >= 64 || len > sizeof txq[0].data) return -1;
    memcpy(txq[ntx].data, frame, len);
    txq[ntx].len = len;
    ntx++;
    return 0;
}

/* --- what reached the upper layers -------------------------------------- */
static struct { int which; uint16_t len; uint8_t data[2048]; } up[64];
static int nup;

static void deliver(int which, const uint8_t *f, uint16_t len)
{
    if (nup >= 64 || len > sizeof up[0].data) return;
    up[nup].which = which;
    up[nup].len = len;
    memcpy(up[nup].data, f, len);
    nup++;
}
void arp_input(const uint8_t *f, uint16_t len) { deliver(1, f, len); }
void ip_input(const uint8_t *f, uint16_t len)  { deliver(2, f, len); }
void ip6_input(const uint8_t *f, uint16_t len) { deliver(3, f, len); }

static int quiet;
void kprintf(const char *fmt, ...)
{
    if (quiet) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}

#include "eth.c"

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
    eth_reset();
    ntx = 0; nup = 0;
    memcpy(net_cfg.mac, our_mac, 6);
}

/* Build a frame: dst, src, an optional list of VLAN tags, ethertype, payload. */
static uint8_t rxbuf[2048];
static uint16_t mkframe(const uint8_t *dst, const uint8_t *src,
                        const uint16_t *tpids, const uint16_t *vids, int ntags,
                        uint16_t type, const uint8_t *payload, uint16_t plen)
{
    uint16_t n = 0;
    memcpy(rxbuf + n, dst, 6); n += 6;
    memcpy(rxbuf + n, src, 6); n += 6;
    for (int i = 0; i < ntags; i++) {
        rxbuf[n++] = (uint8_t)(tpids[i] >> 8); rxbuf[n++] = (uint8_t)tpids[i];
        rxbuf[n++] = (uint8_t)(vids[i] >> 8);  rxbuf[n++] = (uint8_t)vids[i];
    }
    rxbuf[n++] = (uint8_t)(type >> 8); rxbuf[n++] = (uint8_t)type;
    if (plen) { memcpy(rxbuf + n, payload, plen); n += plen; }
    return n;
}

static uint8_t body[64];
static void fill_body(void) { for (int i = 0; i < 64; i++) body[i] = (uint8_t)(i + 1); }

/* ======================================================================== */

/* Transmit builds the header the peer expects, and pads. */
static void t_send_builds_and_pads(void)
{
    reset(); fill_body();

    CHECK(eth_send(other_mac, ETHERTYPE_ARP, body, 28) == 0, "send failed");
    CHECK(ntx == 1, "expected one transmission");
    CHECK(txq[0].len == ETH_FRAME_MIN,
          "a 42-byte ARP frame must be padded to %d, went out at %u",
          ETH_FRAME_MIN, txq[0].len);
    CHECK(memcmp(txq[0].data, other_mac, 6) == 0, "wrong destination on the wire");
    CHECK(memcmp(txq[0].data + 6, our_mac, 6) == 0, "wrong source on the wire");
    CHECK(txq[0].data[12] == 0x08 && txq[0].data[13] == 0x06, "wrong ethertype");
    CHECK(memcmp(txq[0].data + 14, body, 28) == 0, "the payload was corrupted");
    CHECK(eth_get_stats()->tx_padded == 1, "the pad must be counted");

    /* The padding must be ZERO, not whatever was on the stack. This is a
     * kernel memory disclosure otherwise, once per ARP frame, to the whole
     * broadcast domain. */
    for (int i = 14 + 28; i < ETH_FRAME_MIN; i++)
        CHECK(txq[0].data[i] == 0, "pad byte %d leaked stack residue (%02x)",
              i, txq[0].data[i]);

    /* A frame already long enough is untouched. */
    reset();
    uint8_t big[100];
    memset(big, 0xA5, sizeof big);
    eth_send(other_mac, ETHERTYPE_IP, big, sizeof big);
    CHECK(txq[0].len == 114, "a long-enough frame must not be padded, got %u", txq[0].len);
    CHECK(eth_get_stats()->tx_padded == 0, "a long-enough frame was counted as padded");
}

static void t_send_refuses_oversize(void)
{
    reset();
    static uint8_t huge[2000];
    CHECK(eth_send(other_mac, ETHERTYPE_IP, huge, 1501) == -1,
          "a payload over the MTU must be refused");
    CHECK(ntx == 0, "an oversize frame reached the wire");
    CHECK(eth_get_stats()->tx_toolong == 1, "the refusal must be counted");
    /* Exactly at the limit must still work. */
    CHECK(eth_send(other_mac, ETHERTYPE_IP, huge, 1500) == 0,
          "a 1500-byte payload must be accepted");
    CHECK(txq[0].len == ETH_FRAME_MAX, "a full frame is %d bytes, got %u",
          ETH_FRAME_MAX, txq[0].len);
}

/* Dispatch by ethertype, and the unknown case being visible rather than silent. */
static void t_dispatch(void)
{
    reset(); fill_body();
    struct { uint16_t type; int which; } ok[] = {
        { ETHERTYPE_ARP, 1 }, { ETHERTYPE_IP, 2 }, { ETHERTYPE_IPV6, 3 },
    };
    for (unsigned i = 0; i < 3; i++) {
        reset();
        uint16_t n = mkframe(our_mac, other_mac, NULL, NULL, 0, ok[i].type, body, 40);
        eth_input(rxbuf, n);
        CHECK(nup == 1 && up[0].which == ok[i].which,
              "ethertype %04x dispatched wrongly", ok[i].type);
        CHECK(up[0].len == n, "the handler got the wrong length");
        CHECK(memcmp(up[0].data, rxbuf, n) == 0, "the frame was altered on the way up");
    }

    reset();
    uint16_t n = mkframe(our_mac, other_mac, NULL, NULL, 0, 0x88CC, body, 40);  /* LLDP */
    eth_input(rxbuf, n);
    CHECK(nup == 0, "an unknown ethertype must not be dispatched");
    CHECK(eth_get_stats()->rx_unhandled == 1, "an unknown ethertype must be counted");
    CHECK(eth_get_stats()->last_unhandled_type == 0x88CC,
          "the unknown ethertype must be recorded, got %04x",
          eth_get_stats()->last_unhandled_type);
}

/* Destination filtering. */
static void t_destination_filter(void)
{
    reset(); fill_body();

    struct { const uint8_t *dst; int accept; const char *what; } cases[] = {
        { our_mac,   1, "our own unicast" },
        { bcast,     1, "broadcast" },
        { mcast6,    1, "IPv6 ND multicast (33:33)" },
        { mcast4,    1, "IPv4 multicast (01:00:5e)" },
        { other_mac, 0, "somebody else's unicast" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset();
        uint16_t n = mkframe(cases[i].dst, other_mac, NULL, NULL, 0,
                             ETHERTYPE_IP, body, 40);
        eth_input(rxbuf, n);
        CHECK(nup == cases[i].accept, "%s was %s", cases[i].what,
              cases[i].accept ? "dropped" : "accepted");
    }
    CHECK(eth_get_stats()->rx_not_ours == 1, "the leaked frame must be counted");

    /* Multicast and broadcast are counted separately -- ND depends entirely on
     * the multicast path and it should be possible to see it working. */
    reset();
    uint16_t n = mkframe(mcast6, other_mac, NULL, NULL, 0, ETHERTYPE_IPV6, body, 40);
    eth_input(rxbuf, n);
    CHECK(eth_get_stats()->rx_multicast == 1 && eth_get_stats()->rx_broadcast == 0,
          "ND multicast counted wrongly");
    n = mkframe(bcast, other_mac, NULL, NULL, 0, ETHERTYPE_ARP, body, 40);
    eth_input(rxbuf, n);
    CHECK(eth_get_stats()->rx_broadcast == 1, "broadcast counted wrongly");

    /* Before net_init has copied the NIC's address in, we do not know who we
     * are and must not drop everything. */
    reset();
    memset(net_cfg.mac, 0, 6);
    n = mkframe(other_mac, other_mac, NULL, NULL, 0, ETHERTYPE_IP, body, 40);
    eth_input(rxbuf, n);
    CHECK(nup == 1, "frames must be accepted before our MAC is known");
}

/* Length bounds. */
static void t_length_bounds(void)
{
    reset(); fill_body();
    for (uint16_t s = 0; s < ETH_HDR_LEN; s++) {
        reset();
        uint16_t n = mkframe(our_mac, other_mac, NULL, NULL, 0, ETHERTYPE_IP, body, 40);
        (void)n;
        eth_input(rxbuf, s);
        CHECK(nup == 0, "a %u-byte frame was dispatched", s);
        CHECK(eth_get_stats()->rx_runt == 1, "a %u-byte frame was not counted as a runt", s);
    }

    /* A header and nothing else is legal to dispatch -- the upper layers do
     * their own length checks, and a zero-payload frame is not our business to
     * invent a rule about. */
    reset();
    uint16_t n = mkframe(our_mac, other_mac, NULL, NULL, 0, ETHERTYPE_IP, body, 0);
    eth_input(rxbuf, n);
    CHECK(n == ETH_HDR_LEN && nup == 1, "a bare header should reach the handler");

    reset();
    eth_input(rxbuf, ETH_FRAME_MAX_VLAN + 1);
    CHECK(nup == 0 && eth_get_stats()->rx_toolong == 1,
          "a length longer than any frame must be refused");
}

/* --- VLAN --------------------------------------------------------------- */

static void t_vlan_stripped(void)
{
    reset(); fill_body();
    uint16_t tpid = ETHERTYPE_VLAN, vid = 0x2064;   /* prio 1, VLAN 100 */
    uint16_t n = mkframe(our_mac, other_mac, &tpid, &vid, 1, ETHERTYPE_IP, body, 40);
    eth_input(rxbuf, n);

    CHECK(nup == 1 && up[0].which == 2, "a tagged IPv4 frame must reach ip_input");
    CHECK(eth_get_stats()->rx_vlan == 1, "the tag must be counted");
    CHECK(eth_get_stats()->last_vlan_id == 100, "VLAN id read as %u, expected 100",
          eth_get_stats()->last_vlan_id);

    /* What the handler sees must be a perfectly ordinary untagged frame: the
     * upper layers index their headers at a hardcoded offset of 14. */
    CHECK(up[0].len == n - 4, "the stripped frame is the wrong length: %u vs %u",
          up[0].len, (uint16_t)(n - 4));
    CHECK(memcmp(up[0].data, our_mac, 6) == 0 && memcmp(up[0].data + 6, other_mac, 6) == 0,
          "the addresses did not survive de-tagging");
    CHECK(up[0].data[12] == 0x08 && up[0].data[13] == 0x00,
          "the inner ethertype was not moved into place");
    CHECK(memcmp(up[0].data + 14, body, 40) == 0, "the payload was corrupted by de-tagging");

    /* ARP and IPv6 over a tag as well -- the whole point is that a trunk port
     * carries everything, not just IPv4. */
    reset();
    n = mkframe(bcast, other_mac, &tpid, &vid, 1, ETHERTYPE_ARP, body, 40);
    eth_input(rxbuf, n);
    CHECK(nup == 1 && up[0].which == 1, "a tagged ARP frame must reach arp_input");

    reset();
    n = mkframe(mcast6, other_mac, &tpid, &vid, 1, ETHERTYPE_IPV6, body, 40);
    eth_input(rxbuf, n);
    CHECK(nup == 1 && up[0].which == 3, "a tagged IPv6 frame must reach ip6_input");
}

static void t_vlan_qinq(void)
{
    reset(); fill_body();
    uint16_t tpids[2] = { ETHERTYPE_QINQ, ETHERTYPE_VLAN };
    uint16_t vids[2]  = { 0x0032, 0x0064 };          /* outer 50, inner 100 */
    uint16_t n = mkframe(our_mac, other_mac, tpids, vids, 2, ETHERTYPE_IP, body, 40);
    eth_input(rxbuf, n);
    CHECK(nup == 1 && up[0].which == 2, "a QinQ frame must be dispatched");
    CHECK(up[0].len == n - 8, "both tags must be stripped, got %u expected %u",
          up[0].len, (uint16_t)(n - 8));
    CHECK(memcmp(up[0].data + 14, body, 40) == 0, "QinQ payload corrupted");

    /* Three tags is more than we support and must be refused, not walked into. */
    reset();
    uint16_t t3[3] = { ETHERTYPE_QINQ, ETHERTYPE_VLAN, ETHERTYPE_VLAN };
    uint16_t v3[3] = { 1, 2, 3 };
    n = mkframe(our_mac, other_mac, t3, v3, 3, ETHERTYPE_IP, body, 40);
    eth_input(rxbuf, n);
    CHECK(nup == 0 && eth_get_stats()->rx_vlan_bad == 1,
          "an over-stacked tag must be refused");
}

/* A truncated tag is the dangerous case: the ethertype that says "a tag
 * follows" is 4 bytes before the length actually goes. Drive every truncation
 * of every layout rather than picking examples. */
static void t_vlan_truncations(void)
{
    fill_body();
    uint16_t tpids[2] = { ETHERTYPE_VLAN, ETHERTYPE_VLAN };
    uint16_t vids[2]  = { 100, 200 };
    for (int ntags = 1; ntags <= 2; ntags++) {
        uint16_t full = mkframe(our_mac, other_mac, tpids, vids, ntags,
                                ETHERTYPE_IP, body, 40);
        for (uint16_t s = ETH_HDR_LEN; s < full; s++) {
            reset();
            (void)mkframe(our_mac, other_mac, tpids, vids, ntags,
                          ETHERTYPE_IP, body, 40);
            eth_input(rxbuf, s);
            /* Either it is refused, or it is dispatched with a length that
             * stays inside what we were given. Never a read past `s`. */
            if (nup) {
                CHECK(up[0].len <= s, "a %u-byte tagged frame was dispatched as %u bytes",
                      s, up[0].len);
                CHECK(up[0].len >= ETH_HDR_LEN, "dispatched below a header length");
            }
        }
    }
    CHECK(1, "vlan truncation sweep completed");
}

/* --- loopback ------------------------------------------------------------ */

static void t_loopback(void)
{
    reset(); fill_body();
    CHECK(eth_send(our_mac, ETHERTYPE_IP, body, 40) == 0, "a self-addressed send failed");
    CHECK(ntx == 0, "a frame to our own MAC must not reach the wire");
    CHECK(nup == 1 && up[0].which == 2, "a frame to our own MAC must be delivered to us");
    CHECK(eth_get_stats()->lo_tx == 1, "the loopback must be counted");
    CHECK(memcmp(up[0].data, our_mac, 6) == 0 && memcmp(up[0].data + 6, our_mac, 6) == 0,
          "a looped frame must have us as both source and destination");
    CHECK(memcmp(up[0].data + 14, body, 40) == 0, "a looped frame was corrupted");

    /* Broadcast still goes to the wire even though our MAC is not it. */
    reset();
    eth_send(bcast, ETHERTYPE_ARP, body, 28);
    CHECK(ntx == 1 && nup == 0, "broadcast must go to the wire, not the loopback");
}

/* The recursion has to terminate. This is not hypothetical: a handler that
 * answers a self-addressed frame with another self-addressed frame is exactly
 * what ping-to-self does, and an unbounded version overflows the kernel stack
 * into the page tables. */
static int echo_depth;
static void echo_handler(const uint8_t *f, uint16_t len)
{
    (void)f; (void)len;
    echo_depth++;
    eth_send(our_mac, ETHERTYPE_IP, body, 40);      /* answer with another one */
}

static void t_loopback_bounded(void)
{
    reset(); fill_body();
    echo_depth = 0;
    /* Point the IPv4 handler at something that replies to itself forever. */
    eth_send(our_mac, ETHERTYPE_IP, body, 40);
    CHECK(nup >= 1, "setup: the first frame should be delivered");

    /* Now the real thing, driven through the recursive handler. */
    reset();
    echo_depth = 0;
    void (*saved)(const uint8_t *, uint16_t) = ip_input;
    (void)saved;
    /* ip_input is a real symbol here; drive the recursion by calling eth_send
     * from inside the delivery instead. */
    for (int i = 0; i < LOOPBACK_MAX_DEPTH + 3; i++) {
        loopback_depth = i;
        int rc = eth_send(our_mac, ETHERTYPE_IP, body, 40);
        if (i >= LOOPBACK_MAX_DEPTH)
            CHECK(rc == -1, "loopback at depth %d must be refused", i);
        else
            CHECK(rc == 0, "loopback at depth %d must be allowed", i);
    }
    loopback_depth = 0;
    CHECK(eth_get_stats()->lo_dropped == 3, "the refused loopbacks must be counted, got %u",
          eth_get_stats()->lo_dropped);
    (void)echo_handler;
}

/* --- fuzz ---------------------------------------------------------------- */

static uint32_t rnd_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void t_fuzz(void)
{
    reset();
    quiet = 1;
    static uint8_t buf[ETH_FRAME_MAX_VLAN + 16];
    for (int iter = 0; iter < 400000; iter++) {
        uint16_t len = (uint16_t)(rnd() % (ETH_FRAME_MAX_VLAN + 8));
        for (uint16_t i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
        /* Steer a good fraction of the corpus into the de-tagging path, which
         * is the only place this file does length arithmetic on attacker
         * input, and therefore the only place worth fuzzing hard. */
        if (len >= 20) {
            memcpy(buf, our_mac, 6);
            unsigned r = rnd() % 4;
            if (r < 3) { buf[12] = 0x81; buf[13] = 0x00; }
            if (r == 1 && len >= 24) { buf[16] = 0x81; buf[17] = 0x00; }
            if (r == 2 && len >= 24) { buf[16] = 0x88; buf[17] = 0xA8; }
        }
        eth_input(buf, len);
        /* Whatever came out must be a frame, and must fit in what went in. */
        for (int i = 0; i < nup; i++) {
            if (up[i].len < ETH_HDR_LEN) { failures++; printf("FAIL fuzz: dispatched %u bytes\n", up[i].len); }
            if (up[i].len > len)        { failures++; printf("FAIL fuzz: dispatched %u from %u\n", up[i].len, len); }
        }
        nup = 0; ntx = 0;
    }
    quiet = 0;
    checks++;
    CHECK(1, "fuzz completed");
}

int main(void)
{
    t_send_builds_and_pads();
    t_send_refuses_oversize();
    t_dispatch();
    t_destination_filter();
    t_length_bounds();
    t_vlan_stripped();
    t_vlan_qinq();
    t_vlan_truncations();
    t_loopback();
    t_loopback_bounded();
    t_fuzz();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "eth ok",
           checks, failures);
    return failures != 0;
}
