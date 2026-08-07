/* Host tests for ICMPv6, Neighbour Discovery, Duplicate Address Detection and
 * SLAAC. White-box: it #includes ip6.c and nd.c and stubs only the Ethernet
 * send path, the timer, the console and TCP, so every byte checked here is a
 * byte the kernel would really put on the wire.
 *
 * The three things this is really for:
 *
 *  1. STATE. A neighbour cache with five states has transitions that no packet
 *     capture will show you failing -- an entry that never leaves REACHABLE, or
 *     one that probes forever, both "work" until they do not. The transitions
 *     are stepped here with a controllable clock.
 *
 *  2. DAD ACTUALLY REFUSING. It is easy to write DAD that sends the probe and
 *     then assigns the address regardless. The tests below make somebody else
 *     answer, and require the address to be gone.
 *
 *  3. MALFORMED INPUT. A Router Advertisement reconfigures the host's entire
 *     address set, so it is the most dangerous packet this OS parses. Every
 *     malformed shape here must be rejected, not tolerated -- including the
 *     zero-length option that turns a naive option walker into an infinite
 *     loop inside the receive interrupt.
 *
 * Built with -DLOGIT_NET_HOST so net_lock() is a no-op (cli/sti are ring 0). */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "net.h"
#include "eth.h"
#include "ip6.h"

#define OUR_MAC0 0x52, 0x54, 0x00, 0x12, 0x34, 0x56
struct net_config net_cfg = {
    .mac = { OUR_MAC0 },
    .ip = 0x0A00020Fu, .mask = 0xFFFFFF00u, .gw = 0x0A000202u, .dns = 0x0A000203u,
};

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }
int net_up(void) { return 1; }

/* --- captured transmissions --------------------------------------------- */
#define NWIRE 32
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

/* kprintf is captured rather than printed: the boot test greps its output, so
 * a test that asserts on the same strings keeps the two honest together. */
static char logbuf[8192];
static int  loglen;
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    loglen += vsnprintf(logbuf + loglen, sizeof logbuf - loglen - 1, fmt, ap);
    va_end(ap);
    if (loglen > (int)sizeof logbuf - 128) loglen = 0;
}

/* --- the code under test ------------------------------------------------- */
#undef memcpy
#undef memset
#include "ip6_addr.c"
#include "ip6.c"
#include "nd.c"

#ifdef TCP_AF_INET6
static int tcp_calls;
void tcp_input_af(const struct tcp_addr *src, const struct tcp_addr *dst,
                  const uint8_t *data, uint16_t len)
{ (void)src; (void)dst; (void)data; (void)len; tcp_calls++; }

/* The hook nd.c declares weak. Defining it here is what turns "we parse the
 * error" into "we hand the right connection and the right MTU to TCP". */
static int      err_calls;
static ip6_addr err_remote;
static uint16_t err_lport, err_rport;
static int      err_type, err_code;
static uint32_t err_mtu;
void tcp_error_af(const struct tcp_addr *remote, uint16_t lport, uint16_t rport,
                  int type, int code, uint32_t mtu)
{
    err_calls++;
    memcpy(err_remote.b, remote->a.v6, 16);
    err_lport = lport; err_rport = rport;
    err_type = type; err_code = code; err_mtu = mtu;
}
#endif

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL(%d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ip6_addr A(const char *s)
{
    ip6_addr a;
    memset(&a, 0, sizeof a);
    if (!ip6_parse(s, &a)) { printf("FATAL: bad literal %s\n", s); failed++; }
    return a;
}
/* A pointer to a parsed literal. The rotating buffer exists because C will not
 * let you take the address of a function's return value. */
static const ip6_addr *P(const char *s)
{
    static ip6_addr b[8]; static int k;
    ip6_addr *o = &b[k++ & 7];
    *o = A(s);
    return o;
}

static const char *F(const ip6_addr *a)
{
    static char b[4][64]; static int k;
    char *o = b[k++ & 3];
    ip6_format(a, o, 64);
    return o;
}

/* --- injecting a frame --------------------------------------------------- */

/* Build eth + IPv6 around `pl` and hand it to ip6_input, exactly as the RX
 * interrupt would. `hlim` is a parameter because the hop-limit-255 rule is one
 * of the things under test. */
static void rx6(const ip6_addr *src, const ip6_addr *dst, uint8_t nh,
                uint8_t hlim, const uint8_t *pl, int len, int corrupt_plen)
{
    uint8_t f[2048];
    memset(f, 0, sizeof f);
    memcpy(f, net_cfg.mac, 6);                  /* to us */
    f[6] = 0x52; f[7] = 0x55; f[8] = 0x0a; f[9] = 0x00; f[10] = 0x02; f[11] = 0x02;
    f[12] = 0x86; f[13] = 0xDD;
    uint8_t *h = f + 14;
    h[0] = 0x60;
    int p = corrupt_plen ? corrupt_plen : len;
    h[4] = (uint8_t)(p >> 8); h[5] = (uint8_t)p;
    h[6] = nh; h[7] = hlim;
    memcpy(h + 8, src->b, 16);
    memcpy(h + 24, dst->b, 16);
    memcpy(h + 40, pl, (size_t)len);
    ip6_input(f, (uint16_t)(14 + 40 + len));
}

/* An ICMPv6 message with its mandatory pseudo-header checksum filled in. */
static void rx_icmp6(const ip6_addr *src, const ip6_addr *dst,
                     uint8_t *m, int len, uint8_t hlim)
{
    m[2] = m[3] = 0;
    uint16_t ck = ip6_pseudo_checksum(src, dst, IP6_NH_ICMP6, m, (uint32_t)len);
    m[2] = (uint8_t)(ck >> 8); m[3] = (uint8_t)ck;
    rx6(src, dst, IP6_NH_ICMP6, hlim, m, len, 0);
}

static int build_na(uint8_t *m, const ip6_addr *target, uint32_t flags,
                    const uint8_t *tlla)
{
    memset(m, 0, 32);
    m[0] = 136;
    m[4] = (uint8_t)(flags >> 24);
    memcpy(m + 8, target->b, 16);
    if (!tlla) return 24;
    m[24] = 2; m[25] = 1;
    memcpy(m + 26, tlla, 6);
    return 32;
}

static int build_ns(uint8_t *m, const ip6_addr *target, const uint8_t *slla)
{
    memset(m, 0, 32);
    m[0] = 135;
    memcpy(m + 8, target->b, 16);
    if (!slla) return 24;
    m[24] = 1; m[25] = 1;
    memcpy(m + 26, slla, 6);
    return 32;
}

/* An RA with an optional prefix-information option and an optional MTU option. */
static int build_ra(uint8_t *m, uint32_t rtr_life, const uint8_t *slla,
                    const char *prefix, int plen, int flags,
                    uint32_t valid, uint32_t pref, uint32_t mtu)
{
    memset(m, 0, 96);
    m[0] = 134;
    m[4] = 64;                                  /* cur hop limit */
    m[6] = (uint8_t)(rtr_life >> 8); m[7] = (uint8_t)rtr_life;
    int n = 16;
    if (slla) { m[n] = 1; m[n + 1] = 1; memcpy(m + n + 2, slla, 6); n += 8; }
    if (mtu) {
        m[n] = 5; m[n + 1] = 1;
        m[n + 4] = (uint8_t)(mtu >> 24); m[n + 5] = (uint8_t)(mtu >> 16);
        m[n + 6] = (uint8_t)(mtu >> 8);  m[n + 7] = (uint8_t)mtu;
        n += 8;
    }
    if (prefix) {
        ip6_addr p = A(prefix);
        m[n] = 3; m[n + 1] = 4;
        m[n + 2] = (uint8_t)plen;
        m[n + 3] = (uint8_t)flags;              /* 0x80 on-link, 0x40 autonomous */
        m[n + 4] = (uint8_t)(valid >> 24); m[n + 5] = (uint8_t)(valid >> 16);
        m[n + 6] = (uint8_t)(valid >> 8);  m[n + 7] = (uint8_t)valid;
        m[n + 8] = (uint8_t)(pref >> 24);  m[n + 9] = (uint8_t)(pref >> 16);
        m[n + 10] = (uint8_t)(pref >> 8);  m[n + 11] = (uint8_t)pref;
        memcpy(m + n + 16, p.b, 16);
        n += 32;
    }
    return n;
}

/* --- reading what we sent ------------------------------------------------ */

/* Find the last transmitted ICMPv6 message of `type`; NULL if there is none. */
static const uint8_t *last_icmp6(int type, const uint8_t **hdr)
{
    for (int i = nwire - 1; i >= 0; i--) {
        if (wire[i].type != ETHERTYPE_IPV6 || wire[i].len < 44) continue;
        const uint8_t *h = wire[i].data;
        if (h[6] != IP6_NH_ICMP6) continue;
        if (h[40] != type) continue;
        if (hdr) *hdr = h;
        return h + 40;
    }
    return NULL;
}

static void wire_clear(void) { nwire = 0; }

static void boot(void)
{
    wire_clear();
    loglen = 0; logbuf[0] = 0;
    ticks = 1000;
    ip6_reset();
    nd_reset();
    ip6_poll();                     /* lazily initialises: link-local + DAD + RS */
}

/* Run the pump forward `n` ticks, a tick at a time, so timers really fire. */
static void advance(int n)
{
    for (int i = 0; i < n; i++) { ticks++; ip6_poll(); }
}

static const uint8_t ROUTER_MAC[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t OTHER_MAC[6]  = { 0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc };

/* ---- bring-up and DAD ---------------------------------------------------- */

static void test_bringup_dad(void)
{
    boot();
    ip6_addr ll = A("fe80::5054:ff:fe12:3456");
    CHECK(ip6_addr_is_ours_any(&ll), "link-local address formed from the MAC");
    CHECK(!ip6_addr_is_ours(&ll), "and it is TENTATIVE until DAD finishes");
    CHECK(ip6_up() == 0, "IPv6 is not up while the only address is tentative");

    /* The DAD probe: a Neighbour Solicitation FROM the unspecified address, TO
     * the target's solicited-node multicast group, with NO source link-layer
     * option (there is no source to associate). */
    const uint8_t *h = NULL;
    const uint8_t *ns = last_icmp6(135, &h);
    CHECK(ns != NULL, "a DAD solicitation was sent");
    if (ns) {
        ip6_addr src, dst, target, want;
        memcpy(src.b, h + 8, 16);
        memcpy(dst.b, h + 24, 16);
        memcpy(target.b, ns + 8, 16);
        ip6_solicited_node(&ll, &want);
        CHECK(ip6_is_unspecified(&src), "DAD source is :: (got %s)", F(&src));
        CHECK(ip6_equal(&dst, &want), "DAD destination is the solicited-node group (got %s)", F(&dst));
        CHECK(ip6_equal(&target, &ll), "DAD target is the tentative address");
        CHECK(h[7] == 255, "ND hop limit is 255 (got %d)", h[7]);
        /* 24 bytes = no options. An SLLA option here would be a protocol
         * violation and some stacks answer it with an abuse report. */
        uint16_t plen = (uint16_t)((h[4] << 8) | h[5]);
        CHECK(plen == 24, "DAD solicitation carries no options (plen %u)", plen);
        /* And the Ethernet destination is the solicited-node multicast MAC. */
        uint8_t mmac[6];
        ip6_multicast_mac(&want, mmac);
        int idx = -1;
        for (int i = 0; i < nwire; i++)
            if (wire[i].len >= 44 && wire[i].data[40] == 135) idx = i;
        CHECK(idx >= 0 && memcmp(wire[idx].dst, mmac, 6) == 0,
              "DAD frame goes to 33:33:ff:12:34:56");
    }

    /* Nobody objects. After RetransTimer the address becomes usable. */
    advance(120);
    CHECK(ip6_addr_is_ours(&ll), "DAD passed with no defence");
    CHECK(ip6_stats.dad_pass == 1, "one DAD pass counted (%u)", ip6_stats.dad_pass);
    CHECK(strstr(logbuf, "[ip6] dad ok fe80::5054:ff:fe12:3456/64") != NULL,
          "the boot-test marker was printed; log was:\n%s", logbuf);
    CHECK(ip6_up() == 1, "link-local only: up, but not routable (got %d)", ip6_up());

    /* And now a Router Solicitation goes out, to the all-routers group. */
    const uint8_t *rh = NULL;
    CHECK(last_icmp6(133, &rh) != NULL, "a Router Solicitation was sent");
    if (rh) {
        ip6_addr d;
        memcpy(d.b, rh + 24, 16);
        CHECK(strcmp(F(&d), "ff02::2") == 0, "RS goes to ff02::2 (got %s)", F(&d));
    }
}

static void test_dad_fails_on_defence(void)
{
    /* Somebody already owns the address and says so with an advertisement. The
     * address MUST NOT be assigned -- RFC 4862 s5.4.5. */
    boot();
    ip6_addr ll = A("fe80::5054:ff:fe12:3456");
    ip6_addr other = A("fe80::1");
    uint8_t m[64];
    int n = build_na(m, &ll, NA_OVERRIDE, OTHER_MAC);
    ip6_addr sn; ip6_solicited_node(&ll, &sn);
    rx_icmp6(&other, &sn, m, n, 255);

    CHECK(!ip6_addr_is_ours_any(&ll), "a defended address is dropped entirely");
    CHECK(ip6_stats.dad_fail == 1, "DAD failure counted");
    CHECK(strstr(logbuf, "dad failed") != NULL, "failure was reported");
    advance(300);
    CHECK(!ip6_addr_is_ours(&ll), "and it does not come back after the timer");
    CHECK(ip6_up() == 0, "with no link-local address, IPv6 is down");
}

static void test_dad_fails_on_competing_probe(void)
{
    /* The other case: another node is probing for the SAME address at the same
     * time. Its solicitation comes from :: -- both nodes must give up. */
    boot();
    ip6_addr ll = A("fe80::5054:ff:fe12:3456");
    ip6_addr un = A("::"), sn;
    ip6_solicited_node(&ll, &sn);
    uint8_t m[64];
    int n = build_ns(m, &ll, NULL);
    rx_icmp6(&un, &sn, m, n, 255);
    CHECK(!ip6_addr_is_ours_any(&ll), "a competing DAD probe also fails DAD");
}

static void test_dad_defence(void)
{
    /* Once OUR address is assigned, somebody else's DAD probe for it must be
     * answered, so that they give it up instead of us. */
    boot();
    advance(120);
    ip6_addr ll = A("fe80::5054:ff:fe12:3456");
    CHECK(ip6_addr_is_ours(&ll), "precondition: address assigned");
    wire_clear();
    ip6_addr un = A("::"), sn;
    ip6_solicited_node(&ll, &sn);
    uint8_t m[64];
    rx_icmp6(&un, &sn, m, build_ns(m, &ll, NULL), 255);
    const uint8_t *h = NULL;
    const uint8_t *na = last_icmp6(136, &h);
    CHECK(na != NULL, "we defend the address with an advertisement");
    if (na) {
        ip6_addr d;
        memcpy(d.b, h + 24, 16);
        CHECK(strcmp(F(&d), "ff02::1") == 0, "defence goes to all-nodes (got %s)", F(&d));
        CHECK((na[4] & 0x40) == 0, "an unsolicited defence has S clear");
        CHECK((na[4] & 0x20) != 0, "and O set, so the other node overwrites its cache");
    }
    CHECK(ip6_addr_is_ours(&ll), "our address survives -- we were here first");
}

/* ---- router advertisement + SLAAC --------------------------------------- */

static void bring_up_with_ra(const char *prefix, uint32_t valid, uint32_t pref,
                             uint32_t mtu)
{
    boot();
    advance(120);                                   /* link-local passes DAD */
    ip6_addr router = A("fe80::2");
    uint8_t m[128];
    int n = build_ra(m, 1800, ROUTER_MAC, prefix, 64, 0xC0, valid, pref, mtu);
    rx_icmp6(&router, P("ff02::1"), m, n, 255);
}

static void test_slaac(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 1400);
    ip6_addr g = A("2001:db8:1::5054:ff:fe12:3456");
    CHECK(ip6_addr_is_ours_any(&g), "SLAAC formed prefix + EUI-64 = %s", F(&g));
    CHECK(!ip6_addr_is_ours(&g), "the SLAAC address runs DAD too, before use");
    CHECK(ip6_mtu() == 1400, "link MTU taken from the RA option (got %u)", ip6_mtu());
    ip6_addr gw;
    CHECK(nd_default_router(&gw) && ip6_equal(&gw, P("fe80::2")), "default router recorded");
    CHECK(nd_onlink(P("2001:db8:1::99")), "the advertised prefix is on-link");
    CHECK(!nd_onlink(P("2001:db8:9::1")), "an unadvertised prefix is not");
    /* Note the spelling: RFC 5952 does NOT compress a single zero group, so the
     * canonical form keeps the ":0:" rather than collapsing it. The boot test
     * greps for this exact string, so pinning it here keeps the two in step. */
    CHECK(strstr(logbuf, "[ip6] slaac 2001:db8:1:0:5054:ff:fe12:3456/64") != NULL,
          "the SLAAC marker was printed; log was:\n%s", logbuf);

    advance(120);
    CHECK(ip6_addr_is_ours(&g), "SLAAC address usable after its own DAD");
    CHECK(ip6_up() == 2, "routable: address + router (got %d)", ip6_up());
    const ip6_addr *s = ip6_source_for(P("2606:2800::1"));
    CHECK(s && ip6_equal(s, &g), "and it is what a global destination sources from");

    /* Lifetimes: preferred expiring deprecates, valid expiring removes. */
    CHECK(ip6_addr_is_ours(&g), "still assigned before the lifetimes run");
}

static void test_lifetimes(void)
{
    /* 2 s preferred, 4 s valid: the address should go PREFERRED -> DEPRECATED
     * -> gone, which is the mechanism that makes renumbering possible at all. */
    bring_up_with_ra("2001:db8:1::", 4, 2, 0);
    advance(120);
    ip6_addr g = A("2001:db8:1::5054:ff:fe12:3456");
    CHECK(ip6_addr_is_ours(&g), "assigned");
    struct ip6_ifaddr *e = NULL;
    for (int i = 0; i < IP6_NADDR; i++)
        if (ip6_addrs[i].used && ip6_equal(&ip6_addrs[i].addr, &g)) e = &ip6_addrs[i];
    CHECK(e && e->state == IP6_PREFERRED, "preferred first");
    advance(150);
    CHECK(e && e->used && e->state == IP6_DEPRECATED,
          "deprecated once the preferred lifetime expires");
    advance(250);
    CHECK(!ip6_addr_is_ours_any(&g), "removed once the valid lifetime expires");
}

static void test_ra_rejects(void)
{
    uint8_t m[128];
    int n;

    /* A Router Advertisement from a GLOBAL source. RFC 4861 s6.1.2 requires
     * link-local, and this is the check that stops anyone on the Internet from
     * renumbering the host. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:1::", 64, 0xC0, 86400, 14400, 0);
    rx_icmp6(P("2001:db8:99::1"), P("ff02::1"), m, n, 255);
    CHECK(ip6_up() != 2, "an RA from a global source is ignored");
    CHECK(ip6_stats.ra == 0, "and not even counted");

    /* Hop limit 64: the packet crossed a router, so it is not on-link, so it is
     * not a neighbour, so it does not get to configure us. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:1::", 64, 0xC0, 86400, 14400, 0);
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 64);
    CHECK(ip6_up() != 2, "an RA with hop limit != 255 is ignored");

    /* An option with a zero length field. A naive walker loops forever on
     * this, inside the receive interrupt, and the machine stops. */
    boot(); advance(120);
    n = build_ra(m, 1800, NULL, NULL, 0, 0, 0, 0, 0);
    m[n] = 1; m[n + 1] = 0;                     /* SLLA, length 0 */
    n += 8;
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(ip6_stats.ra == 0, "a zero-length option discards the whole message");
    CHECK(ip6_stats.rx_bad > 0, "and is counted as malformed");

    /* An option that runs past the end of the message. */
    boot(); advance(120);
    n = build_ra(m, 1800, NULL, NULL, 0, 0, 0, 0, 0);
    m[n] = 3; m[n + 1] = 4;                     /* claims 32 bytes... */
    n += 8;                                     /* ...but only 8 are present */
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(ip6_stats.ra == 0, "an option overrunning the message is rejected");

    /* Preferred lifetime > valid lifetime: RFC 4862 s5.5.3(c) says ignore the
     * option. Accepting it would let a rogue RA pin an address as preferred
     * long after it stopped being valid. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:1::", 64, 0xC0, 100, 200, 0);
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(!ip6_addr_is_ours_any(P("2001:db8:1::5054:ff:fe12:3456")),
          "preferred > valid: no address configured");
    CHECK(ip6_stats.ra == 1, "but the RA itself was still processed");

    /* A prefix length that cannot carry a 64-bit interface identifier. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:1::", 48, 0xC0, 86400, 14400, 0);
    m[16 + 8 + 2] = 48;
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(!ip6_addr_is_ours_any(P("2001:db8:1::5054:ff:fe12:3456")),
          "a non-/64 prefix does not autoconfigure");

    /* A link-local prefix must never be autoconfigured from an RA. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "fe80::", 64, 0xC0, 86400, 14400, 0);
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(ip6_up() != 2, "an RA cannot hand us a link-local prefix");

    /* An MTU below the IPv6 minimum is not allowed to shrink the link. */
    boot(); advance(120);
    n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:1::", 64, 0xC0, 86400, 14400, 576);
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    CHECK(ip6_mtu() == 1500, "an MTU below 1280 is refused (got %u)", ip6_mtu());

    /* Router lifetime 0 withdraws the router. */
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    CHECK(ip6_up() == 2, "precondition: routable");
    n = build_ra(m, 0, ROUTER_MAC, NULL, 0, 0, 0, 0, 0);
    rx_icmp6(P("fe80::2"), P("ff02::1"), m, n, 255);
    ip6_addr gw;
    CHECK(!nd_default_router(&gw), "router lifetime 0 withdraws the router");
    CHECK(ip6_up() == 1, "and the host is no longer routable");
}

/* ---- neighbour cache ----------------------------------------------------- */

static void test_nd_resolution(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    wire_clear();

    ip6_addr peer = A("2001:db8:1::99");
    uint8_t mac[6];
    CHECK(nd_resolve(&peer, mac) == -1, "an unknown neighbour is not resolved yet");
    CHECK(nd_state(&peer) == ND_INCOMPLETE, "and the entry is INCOMPLETE");

    const uint8_t *h = NULL;
    const uint8_t *ns = last_icmp6(135, &h);
    CHECK(ns != NULL, "a solicitation went out");
    if (ns) {
        ip6_addr d, want, src;
        memcpy(d.b, h + 24, 16);
        memcpy(src.b, h + 8, 16);
        ip6_solicited_node(&peer, &want);
        CHECK(ip6_equal(&d, &want), "to the peer's solicited-node group (got %s)", F(&d));
        CHECK(ip6_is_linklocal(&src), "sourced from our link-local address (got %s)", F(&src));
        uint16_t plen = (uint16_t)((h[4] << 8) | h[5]);
        CHECK(plen == 32, "and it DOES carry a source link-layer option (plen %u)", plen);
    }

    /* The answer. Solicited + override with a target link-layer address. */
    uint8_t m[64];
    rx_icmp6(&peer, P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, &peer, NA_SOLICITED | NA_OVERRIDE, OTHER_MAC), 255);
    CHECK(nd_state(&peer) == ND_REACHABLE, "a solicited advertisement makes it REACHABLE");
    CHECK(nd_resolve(&peer, mac) == 0 && memcmp(mac, OTHER_MAC, 6) == 0,
          "and the MAC is what we resolve to");
    CHECK(strstr(logbuf, "-> 02:00:00:aa:bb:cc reachable") != NULL,
          "the resolution marker was printed");
}

static void test_nd_state_machine(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    ip6_addr peer = A("2001:db8:1::99");
    uint8_t mac[6], m[64];
    nd_resolve(&peer, mac);
    rx_icmp6(&peer, P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, &peer, NA_SOLICITED | NA_OVERRIDE, OTHER_MAC), 255);
    CHECK(nd_state(&peer) == ND_REACHABLE, "precondition: REACHABLE");

    /* REACHABLE is a claim with an expiry. This is the whole difference from an
     * ARP cache, which would keep asserting a dead entry until it was evicted. */
    advance(3100);
    CHECK(nd_state(&peer) == ND_STALE, "REACHABLE expires to STALE");

    /* Sending to a STALE neighbour is allowed and starts the confirmation. */
    CHECK(nd_resolve(&peer, mac) == 0, "a STALE entry still resolves (no stall)");
    CHECK(nd_state(&peer) == ND_DELAY, "and moves to DELAY");

    wire_clear();
    advance(520);
    CHECK(nd_state(&peer) == ND_PROBE, "DELAY expires to PROBE");
    const uint8_t *h = NULL;
    const uint8_t *ns = last_icmp6(135, &h);
    CHECK(ns != NULL, "PROBE solicits");
    if (ns) {
        ip6_addr d;
        memcpy(d.b, h + 24, 16);
        CHECK(ip6_equal(&d, &peer), "and a probe is UNICAST to the neighbour (got %s)", F(&d));
        int idx = -1;
        for (int i = 0; i < nwire; i++)
            if (wire[i].len >= 44 && wire[i].data[40] == 135) idx = i;
        CHECK(idx >= 0 && memcmp(wire[idx].dst, OTHER_MAC, 6) == 0,
              "to the cached MAC, not to multicast");
    }

    /* Nobody answers: the entry is eventually discarded rather than kept as a
     * lie. nd_resolve then starts over from INCOMPLETE. */
    advance(400);
    CHECK(nd_state(&peer) == ND_NONE, "an unanswered probe sequence deletes the entry");

    /* An INCOMPLETE entry that is never answered is also discarded. */
    ip6_addr ghost = A("2001:db8:1::dead");
    nd_resolve(&ghost, mac);
    CHECK(nd_state(&ghost) == ND_INCOMPLETE, "ghost starts INCOMPLETE");
    advance(500);
    CHECK(nd_state(&ghost) == ND_NONE, "and is dropped after MAX_MULTICAST_SOLICIT");
}

static void test_nd_override_rules(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    ip6_addr peer = A("2001:db8:1::99");
    uint8_t mac[6], m[64];
    nd_resolve(&peer, mac);
    rx_icmp6(&peer, P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, &peer, NA_SOLICITED | NA_OVERRIDE, OTHER_MAC), 255);

    /* A DIFFERENT MAC with the override bit CLEAR must not rewrite the entry.
     * It only casts doubt: RFC 4861 s7.2.5. Letting it through would be a
     * one-packet cache-poisoning primitive. */
    static const uint8_t evil[6] = { 0xde, 0xad, 0, 0, 0, 1 };
    rx_icmp6(&peer, P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, &peer, 0, evil), 255);
    nd_resolve(&peer, mac);
    CHECK(memcmp(mac, OTHER_MAC, 6) == 0, "a non-override advertisement cannot rewrite the MAC");

    /* With the override bit set, it may. */
    rx_icmp6(&peer, P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, &peer, NA_SOLICITED | NA_OVERRIDE, evil), 255);
    nd_resolve(&peer, mac);
    CHECK(memcmp(mac, evil, 6) == 0, "an override advertisement does");

    /* A solicited advertisement sent to a MULTICAST destination is illegal. */
    uint32_t bad = ip6_stats.rx_bad;
    rx_icmp6(&peer, P("ff02::1"), m,
             build_na(m, &peer, NA_SOLICITED, OTHER_MAC), 255);
    CHECK(ip6_stats.rx_bad > bad, "a solicited NA to a multicast address is rejected");

    /* A solicitation from a neighbour for OUR address is answered, and teaches
     * us their link-layer address at the same time. */
    wire_clear();
    ip6_addr ours = A("2001:db8:1::5054:ff:fe12:3456");
    rx_icmp6(&peer, &ours, m, build_ns(m, &ours, OTHER_MAC), 255);
    const uint8_t *h = NULL;
    const uint8_t *na = last_icmp6(136, &h);
    CHECK(na != NULL, "we answer a solicitation for our own address");
    if (na) {
        ip6_addr d;
        memcpy(d.b, h + 24, 16);
        CHECK(ip6_equal(&d, &peer), "unicast back to the solicitor (got %s)", F(&d));
        CHECK((na[4] & 0x40) != 0, "with the Solicited flag set");
    }
    CHECK(nd_state(&peer) != ND_NONE, "and we learned the solicitor from its SLLA option");

    /* A solicitation for an address that is not ours is not answered. */
    wire_clear();
    rx_icmp6(&peer, P("2001:db8:1::1234"), m,
             build_ns(m, P("2001:db8:1::1234"), OTHER_MAC), 255);
    CHECK(last_icmp6(136, NULL) == NULL, "we do not answer for addresses we do not hold");
}

/* ---- ip6_input validation ------------------------------------------------ */

static void test_input_validation(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    ip6_addr ours = A("2001:db8:1::5054:ff:fe12:3456");
    uint8_t pl[64];
    memset(pl, 0, sizeof pl);

    uint32_t rx0 = ip6_stats.rx;
    /* A multicast SOURCE address is forbidden by RFC 8200 s3 and is the shape a
     * reflection amplifier takes. */
    rx6(P("ff02::1"), &ours, IP6_NH_ICMP6, 255, pl, 8, 0);
    CHECK(ip6_stats.rx == rx0, "a multicast source is dropped");

    rx6(P("::1"), &ours, IP6_NH_ICMP6, 255, pl, 8, 0);
    CHECK(ip6_stats.rx == rx0, "a loopback source is dropped");

    rx6(P("fe80::2"), P("::"), IP6_NH_ICMP6, 255, pl, 8, 0);
    CHECK(ip6_stats.rx == rx0, "an unspecified destination is dropped");

    /* Not addressed to us and not a group we joined: no forwarding here. */
    rx6(P("fe80::2"), P("2001:db8:1::abcd"), IP6_NH_ICMP6, 255, pl, 8, 0);
    CHECK(ip6_stats.rx == rx0, "a packet for someone else is not processed");

    /* A payload length that claims more than the frame holds. */
    rx6(P("fe80::2"), &ours, IP6_NH_ICMP6, 255, pl, 8, 4000);
    CHECK(ip6_stats.rx == rx0, "a payload length past the end of the frame is dropped");

    /* A wrong ICMPv6 checksum. Unlike ICMPv4's, this one is mandatory and
     * covers the pseudo-header -- it is what stops a corrupted RA. */
    uint8_t m[128];
    int n = build_ra(m, 1800, ROUTER_MAC, "2001:db8:9::", 64, 0xC0, 86400, 14400, 0);
    m[2] = 0x00; m[3] = 0x01;                   /* deliberately wrong */
    rx6(P("fe80::2"), P("ff02::1"), IP6_NH_ICMP6, 255, m, n, 0);
    CHECK(!ip6_addr_is_ours_any(P("2001:db8:9::5054:ff:fe12:3456")),
          "an RA with a bad checksum configures nothing");

    /* A Routing header still asking to be forwarded (segments left > 0) must be
     * discarded by a host (RFC 8200 s4.4) rather than followed. */
    wire_clear();
    uint8_t ext[32];
    memset(ext, 0, sizeof ext);
    ext[0] = IP6_NH_ICMP6; ext[1] = 1; ext[2] = 0; ext[3] = 1;   /* segleft = 1 */
    ext[8] = 128;                                                /* an echo request */
    rx6(P("fe80::2"), &ours, IP6_NH_ROUTING, 255, ext, 24, 0);
    CHECK(last_icmp6(129, NULL) == NULL,
          "a routing header with segments left is dropped, not followed");

    /* Fragments are not reassembled; they must be dropped, not misread. */
    uint32_t bad = ip6_stats.rx_bad;
    memset(ext, 0, sizeof ext);
    ext[0] = IP6_NH_ICMP6;
    rx6(P("fe80::2"), &ours, IP6_NH_FRAGMENT, 255, ext, 16, 0);
    CHECK(ip6_stats.rx_bad > bad, "a fragment is dropped and counted");
}

static void test_echo(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    ip6_addr ours = A("2001:db8:1::5054:ff:fe12:3456");
    ip6_addr peer = A("2001:db8:1::99");

    /* Learn the peer's link-layer address first. Without a neighbour cache
     * entry the reply cannot be sent AT ALL -- ip6_output solicits and returns
     * -1, exactly as ip_send does on an ARP miss. That is correct, and it is
     * why a first ping to a cold cache is answered only on the retry. */
    uint8_t na[64];
    rx_icmp6(&peer, &ours, na, build_na(na, &peer, NA_OVERRIDE, OTHER_MAC), 255);
    wire_clear();

    uint8_t m[32];
    memset(m, 0, sizeof m);
    m[0] = 128;                                 /* echo request */
    m[4] = 0xAB; m[5] = 0xCD;                   /* id */
    m[6] = 0x00; m[7] = 0x07;                   /* sequence */
    for (int i = 8; i < 24; i++) m[i] = (uint8_t)i;
    rx_icmp6(&peer, &ours, m, 24, 64);          /* echo needs no hop limit 255 */

    const uint8_t *h = NULL;
    const uint8_t *r = last_icmp6(129, &h);
    CHECK(r != NULL, "an echo request is answered");
    if (r) {
        CHECK(r[4] == 0xAB && r[5] == 0xCD && r[7] == 7, "id and sequence are echoed");
        for (int i = 8; i < 24; i++)
            if (r[i] != (uint8_t)i) { CHECK(0, "payload byte %d not echoed", i); break; }
        ip6_addr s, d;
        memcpy(s.b, h + 8, 16);
        memcpy(d.b, h + 24, 16);
        CHECK(ip6_equal(&s, &ours) && ip6_equal(&d, &peer), "reply is addressed back");
        /* And the reply's own checksum must verify over the pseudo-header. */
        CHECK(ip6_pseudo_checksum(&s, &d, IP6_NH_ICMP6, r, 24) == 0,
              "the reply's checksum verifies");
    }
}

/* ---- output -------------------------------------------------------------- */

static void test_output(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);

    /* An on-link destination goes straight to the neighbour. */
    ip6_addr peer = A("2001:db8:1::99"), hop;
    CHECK(ip6_next_hop(&peer, &hop) && ip6_equal(&hop, &peer),
          "an on-link destination is its own next hop");

    /* An off-link one goes to the router. */
    ip6_addr far_ = A("2606:2800::1");
    CHECK(ip6_next_hop(&far_, &hop) && ip6_equal(&hop, P("fe80::2")),
          "an off-link destination goes via the router (got %s)", F(&hop));

    /* Teach the router's MAC, then send. */
    uint8_t m[64], mac[6];
    nd_resolve(P("fe80::2"), mac);
    rx_icmp6(P("fe80::2"), P("fe80::5054:ff:fe12:3456"), m,
             build_na(m, P("fe80::2"), NA_SOLICITED | NA_OVERRIDE | NA_ROUTER,
                      ROUTER_MAC), 255);
    wire_clear();
    uint8_t body[100];
    memset(body, 0x5A, sizeof body);
    CHECK(ip6_output(NULL, &far_, IP6_NH_TCP, body, sizeof body) == 0,
          "a packet to an off-link destination is sent");
    CHECK(nwire == 1 && memcmp(wire[0].dst, ROUTER_MAC, 6) == 0,
          "to the router's MAC");
    if (nwire == 1) {
        const uint8_t *h = wire[0].data;
        ip6_addr s;
        memcpy(s.b, h + 8, 16);
        CHECK(ip6_equal(&s, P("2001:db8:1::5054:ff:fe12:3456")),
              "sourced from the global address, not the link-local one (got %s)", F(&s));
        CHECK(h[6] == IP6_NH_TCP && h[7] == 255, "next header and hop limit");
        CHECK(((h[4] << 8) | h[5]) == 100, "payload length");
        CHECK(wire[0].type == ETHERTYPE_IPV6, "ethertype 0x86DD");
    }

    /* Nothing on the IPv6 path fragments, so an oversized datagram is refused
     * rather than truncated. TCP's PMTU clamp is what recovers. */
    static uint8_t huge[2000];
    CHECK(ip6_output(NULL, &far_, IP6_NH_TCP, huge, 1600) == -1,
          "a datagram larger than the link MTU is refused");

    /* No route at all -> refused, which is what makes a socket fail over. */
    boot();
    advance(120);
    CHECK(ip6_output(NULL, &far_, IP6_NH_TCP, body, 8) == -1,
          "with no router and no global address, output fails cleanly");
}

/* ---- ICMPv6 errors -------------------------------------------------------- */

#ifdef TCP_AF_INET6
/* Build an ICMPv6 error of `type`/`code` quoting a TCP packet we supposedly
 * sent from `qsrc`:`lport` to `qdst`:`rport`. `mtu` fills the type-specific
 * word (meaningful for Packet Too Big). `quote_len` lets a test truncate the
 * quoted packet to less than a stack needs to identify anything. */
static int build_icmp6_err(uint8_t *m, int type, int code, uint32_t mtu,
                           const ip6_addr *qsrc, const ip6_addr *qdst,
                           uint16_t lport, uint16_t rport, int nh, int quote_len)
{
    memset(m, 0, 96);
    m[0] = (uint8_t)type; m[1] = (uint8_t)code;
    m[4] = (uint8_t)(mtu >> 24); m[5] = (uint8_t)(mtu >> 16);
    m[6] = (uint8_t)(mtu >> 8);  m[7] = (uint8_t)mtu;
    uint8_t *q = m + 8;
    q[0] = 0x60;
    q[4] = 0; q[5] = 20;                        /* quoted payload length */
    q[6] = (uint8_t)nh; q[7] = 64;
    memcpy(q + 8, qsrc->b, 16);
    memcpy(q + 24, qdst->b, 16);
    q[40] = (uint8_t)(lport >> 8); q[41] = (uint8_t)lport;
    q[42] = (uint8_t)(rport >> 8); q[43] = (uint8_t)rport;
    return quote_len ? 8 + quote_len : 8 + 40 + 20;
}

static void test_icmp6_errors(void)
{
    bring_up_with_ra("2001:db8:1::", 86400, 14400, 0);
    advance(120);
    ip6_addr ours = A("2001:db8:1::5054:ff:fe12:3456");
    ip6_addr peer = A("2606:2800::1");
    ip6_addr rtr  = A("fe80::2");
    uint8_t m[128];
    int n;

    /* Packet Too Big. IPv6 routers never fragment, so this is the ONLY path-MTU
     * signal there is -- and unlike ICMPv4 frag-needed it always carries the
     * MTU, so nothing has to be guessed. */
    err_calls = 0;
    n = build_icmp6_err(m, 2, 0, 1400, &ours, &peer, 40000, 443, IP6_NH_TCP, 0);
    rx_icmp6(&rtr, &ours, m, n, 64);            /* hop limit 64: from a router */
    CHECK(err_calls == 1, "Packet Too Big is delivered to TCP (%d calls)", err_calls);
    if (err_calls) {
        CHECK(ip6_equal(&err_remote, &peer), "with the connection's REMOTE address (%s)",
              F(&err_remote));
        CHECK(err_lport == 40000 && err_rport == 443, "and its port pair (%u,%u)",
              err_lport, err_rport);
        CHECK(err_type == 2 && err_code == 0, "raw ICMPv6 type/code, not translated");
        CHECK(err_mtu == 1400, "and the MTU the router reported (%u)", err_mtu);
    }

    /* An MTU below the IPv6 minimum is clamped, not obeyed: a router may not
     * advertise less than 1280, and honouring 68 would mean sending tiny
     * segments forever on one attacker's say-so. */
    err_calls = 0;
    n = build_icmp6_err(m, 2, 0, 68, &ours, &peer, 40000, 443, IP6_NH_TCP, 0);
    rx_icmp6(&rtr, &ours, m, n, 64);
    CHECK(err_calls == 1 && err_mtu == IP6_MIN_MTU,
          "an MTU below 1280 is clamped to 1280 (got %u)", err_mtu);

    /* Destination Unreachable, and the MTU word must NOT be passed through as
     * an MTU -- for this type those four bytes are unused and a stack that read
     * them anyway would clamp the path on a message that says nothing about it. */
    err_calls = 0;
    n = build_icmp6_err(m, 1, 4, 0xDEADBEEF, &ours, &peer, 40000, 443, IP6_NH_TCP, 0);
    rx_icmp6(&rtr, &ours, m, n, 64);
    CHECK(err_calls == 1 && err_mtu == 0 && err_type == 1 && err_code == 4,
          "Destination Unreachable carries no MTU (got %u)", err_mtu);

    /* THE ONE THAT MATTERS. An error quoting a packet we did NOT send is a
     * forgery: ICMPv6 errors legitimately arrive from off-link, so the hop
     * limit cannot vouch for them and the quoted source address is the only
     * thing that can. Acting on this would let anyone who can reach the host
     * reset any connection, or pin its path MTU at 1280. */
    err_calls = 0;
    uint32_t bad0 = ip6_stats.rx_bad;
    ip6_addr notus = A("2001:db8:1::dead");
    n = build_icmp6_err(m, 2, 0, 1280, &notus, &peer, 40000, 443, IP6_NH_TCP, 0);
    rx_icmp6(&rtr, &ours, m, n, 64);
    CHECK(err_calls == 0, "an error quoting someone else's packet is refused");
    CHECK(ip6_stats.rx_bad > bad0, "and counted as bad");

    /* A quote too short to contain the port pair identifies nothing. */
    err_calls = 0;
    n = build_icmp6_err(m, 2, 0, 1400, &ours, &peer, 40000, 443, IP6_NH_TCP, 40);
    rx_icmp6(&rtr, &ours, m, n, 64);
    CHECK(err_calls == 0, "a quote without the transport ports is refused");

    /* A quoted protocol that is not TCP is not ours to act on. */
    err_calls = 0;
    n = build_icmp6_err(m, 1, 4, 0, &ours, &peer, 40000, 443, IP6_NH_UDP, 0);
    rx_icmp6(&rtr, &ours, m, n, 64);
    CHECK(err_calls == 0, "an error quoting a non-TCP packet is not delivered to TCP");

    /* And an error is NOT subject to the hop-limit-255 rule -- it comes from a
     * router, which decremented it. Requiring 255 here would silently discard
     * every path-MTU message the stack will ever receive. */
    err_calls = 0;
    n = build_icmp6_err(m, 2, 0, 1300, &ours, &peer, 1234, 80, IP6_NH_TCP, 0);
    rx_icmp6(&rtr, &ours, m, n, 3);             /* nearly expired: still valid */
    CHECK(err_calls == 1, "an error with a low hop limit is still accepted");
}
#endif

int main(void)
{
    test_bringup_dad();
    test_dad_fails_on_defence();
    test_dad_fails_on_competing_probe();
    test_dad_defence();
    test_slaac();
    test_lifetimes();
    test_ra_rejects();
    test_nd_resolution();
    test_nd_state_machine();
    test_nd_override_rules();
    test_input_validation();
    test_echo();
    test_output();
#ifdef TCP_AF_INET6
    test_icmp6_errors();
#endif
    printf("nd: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
