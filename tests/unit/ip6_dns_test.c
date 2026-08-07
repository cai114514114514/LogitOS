/* The dual-stack resolver (c/net/dns/dns.c) against a model UDP socket.
 *
 * WHY THIS FILE EXISTS. Adding AAAA to a resolver changes the SHAPE of a
 * lookup, not just the record type: one name now has two transactions that
 * finish at different times, and the caller wants a single list in preference
 * order. Every part of that is invisible from the outside until it goes wrong,
 * and two of the ways it goes wrong are severe:
 *
 *   - a AAAA transaction that never answers must not stall the A answer behind
 *     it. A resolver that waits for both is a resolver that hangs on every
 *     lookup the moment one family is filtered -- which is the normal condition
 *     on a lot of real networks;
 *
 *   - a AAAA record is a place a remote party writes 16 bytes that this host
 *     will then CONNECT to. ::1, a multicast group and a v4-mapped form are all
 *     things a name must not be allowed to resolve to, and rejecting them is a
 *     filter, not a nicety.
 *
 * And one that is not severe but is the whole no-regression claim: on a network
 * with no usable IPv6, this file must put exactly the same bytes on the wire as
 * it did before IPv6 existed -- one query, QTYPE=A -- which is asserted here by
 * counting the datagrams and reading their QTYPE, not by inspection.
 *
 * Method as elsewhere in this tree: #include the real dns.c and model UDP, the
 * clock and the RNG underneath it. dns.c is the code under test. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "net.h"
#include "eth.h"
#include "ip6.h"

struct net_config net_cfg = {
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = 0x0A00020Fu, .mask = 0xFFFFFF00u, .gw = 0x0A000202u, .dns = 0x0A000203u,
};
#define RESOLVER 0x0A000203u

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }
int net_up(void) { return 1; }

/* Deterministic "random": the transaction ids have to be predictable so a test
 * can forge a response with the RIGHT id and, more importantly, with a WRONG
 * one. Sequential is enough -- the property under test is that the id is
 * checked, not that it is unguessable (that is the RNG's problem). */
static uint16_t next_id = 0x1000;
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) out[i] = 0;
    if (len >= 2) { out[0] = (uint8_t)(next_id & 0xFF); out[1] = (uint8_t)(next_id >> 8); }
    next_id++;
}
int rng_strong(void) { return 0; }

/* Only the BLOCKING resolver (dns_resolve) reaches these, and nothing here
 * calls it: the async pool is the path sock.c uses and the path IPv6 changed. */
void net_poll(void) { }
void net_idle(void) { }

int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{ (void)ip; memset(mac, 0x22, ETH_ALEN); return 0; }
int arp_warm(uint32_t ip, int t) { (void)ip; (void)t; return 0; }
void arp_input(const uint8_t *f, uint16_t l) { (void)f; (void)l; }

/* ---- the model UDP socket ------------------------------------------------ */

#define NSOCK6 8
#define QMAX   8

struct sent { int sock; uint32_t dst; uint16_t dport; uint16_t len; uint8_t b[512]; };
static struct sent sent_log[64];
static int nsent;

struct rxq { int used, n, head; uint32_t src[QMAX]; uint16_t sport[QMAX];
             uint16_t len[QMAX]; uint8_t b[QMAX][512]; int err; };
static struct rxq socks6[NSOCK6];

int udp_bind(uint16_t port)
{
    (void)port;
    for (int i = 0; i < NSOCK6; i++)
        if (!socks6[i].used) { memset(&socks6[i], 0, sizeof socks6[i]);
                               socks6[i].used = 1; return i; }
    return -1;
}
void udp_close(int s) { if (s >= 0 && s < NSOCK6) socks6[s].used = 0; }

int udp_send_to(int s, uint32_t dst, uint16_t dport, const void *d, uint16_t len)
{
    if (nsent < 64 && len <= 512) {
        sent_log[nsent].sock = s; sent_log[nsent].dst = dst;
        sent_log[nsent].dport = dport; sent_log[nsent].len = len;
        memcpy(sent_log[nsent].b, d, len);
        nsent++;
    }
    return 0;
}
int udp_send(uint32_t d, uint16_t dp, uint16_t sp, const void *b, uint16_t l)
{ (void)d; (void)dp; (void)sp; (void)b; (void)l; return 0; }

int udp_recv(int s, void *buf, int max, uint32_t *src, uint16_t *sport)
{
    if (s < 0 || s >= NSOCK6 || !socks6[s].used) return 0;
    struct rxq *q = &socks6[s];
    if (q->err) { q->err = 0; return -1; }
    if (q->n == 0) return 0;
    int i = q->head;
    int n = q->len[i] < max ? q->len[i] : max;
    memcpy(buf, q->b[i], (size_t)n);
    if (src) *src = q->src[i];
    if (sport) *sport = q->sport[i];
    q->head = (q->head + 1) % QMAX;
    q->n--;
    return n;
}
uint32_t udp_drops(int s) { (void)s; return 0; }

/* Deliver a datagram to whichever socket dns.c most recently bound. */
static int last_sock(void)
{
    for (int i = NSOCK6 - 1; i >= 0; i--) if (socks6[i].used) return i;
    return -1;
}
static void deliver(const uint8_t *b, int len, uint32_t src, uint16_t sport)
{
    int s = last_sock();
    if (s < 0) return;
    struct rxq *q = &socks6[s];
    if (q->n >= QMAX) return;
    int i = (q->head + q->n) % QMAX;
    memcpy(q->b[i], b, (size_t)len);
    q->len[i] = (uint16_t)len; q->src[i] = src; q->sport[i] = sport;
    q->n++;
}

/* ---- IPv6 interface state, as dns.c sees it ------------------------------ */

/* dns.c asks ip6_up() whether IPv6 is worth querying for, and
 * ip6_dual_candidates() for the source addresses RFC 6724 ordering needs.
 * Both live in ip6.c, which drags the whole packet layer behind it; modelling
 * them here is what lets this test say "v4-only network" and "dual-stack
 * network" as a one-line switch. */
static int      v6_state;                   /* 0, 1 or 2, as ip6_up() returns */
static ip6_addr v6_src;                     /* our global address, when up */

int ip6_up(void) { return v6_state; }

int ip6_dual_candidates(struct ip6_src_cand *cand, int max)
{
    int n = 0;
    if (v6_state == 2 && n < max) {
        cand[n].addr = v6_src; cand[n].plen = 64; cand[n].state = IP6_PREFERRED; n++;
    }
    if (n < max) {
        ip6_from_v4(net_cfg.ip, &cand[n].addr);
        cand[n].plen = 96; cand[n].state = IP6_PREFERRED; n++;
    }
    return n;
}

#include "ip6_addr.c"
#include "dns.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL(%d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ip6_addr A6(const char *s)
{
    ip6_addr a; memset(&a, 0, sizeof a);
    if (!ip6_parse(s, &a)) { printf("FATAL: bad literal %s\n", s); failed++; }
    return a;
}
static const char *F(const ip6_addr *a)
{
    static char b[4][64]; static int k;
    char *o = b[k++ & 3]; ip6_format(a, o, 64); return o;
}

/* ---- building DNS responses ---------------------------------------------- */

static int put_qname(uint8_t *o, const char *name)
{
    int n = 0, lab = 0;
    for (;;) {
        const char *p = name;
        while (*p && *p != '.') p++;
        lab = (int)(p - name);
        o[n++] = (uint8_t)lab;
        for (int i = 0; i < lab; i++) o[n++] = (uint8_t)name[i];
        if (!*p) break;
        name = p + 1;
    }
    o[n++] = 0;
    return n;
}

/* A response for `name`, echoing `txid`, carrying `nrr` records. Each record is
 * (type, rdata, rdlen); rdlen_override != 0 lies about the length, which is how
 * the truncated-record cases are built. */
struct rr { int type; const uint8_t *rdata; int rdlen; int rdlen_override; };

static int build_resp(uint8_t *m, const char *name, uint16_t txid, int qtype,
                      const struct rr *rr, int nrr)
{
    int o = 0;
    m[o++] = (uint8_t)(txid >> 8); m[o++] = (uint8_t)txid;
    m[o++] = 0x81; m[o++] = 0x80;               /* QR + RD + RA, rcode 0 */
    m[o++] = 0; m[o++] = 1;                     /* QDCOUNT */
    m[o++] = (uint8_t)(nrr >> 8); m[o++] = (uint8_t)nrr;
    m[o++] = 0; m[o++] = 0;                     /* NSCOUNT */
    m[o++] = 0; m[o++] = 0;                     /* ARCOUNT */
    o += put_qname(m + o, name);
    m[o++] = (uint8_t)(qtype >> 8); m[o++] = (uint8_t)qtype;
    m[o++] = 0; m[o++] = 1;                     /* QCLASS IN */
    for (int i = 0; i < nrr; i++) {
        /* Compressed owner name, pointing back at the question -- the form a
         * real resolver uses, so skip_name's 0xC0 branch is on the path. */
        m[o++] = 0xC0; m[o++] = 12;
        m[o++] = (uint8_t)(rr[i].type >> 8); m[o++] = (uint8_t)rr[i].type;
        m[o++] = 0; m[o++] = 1;                 /* class IN */
        m[o++] = 0; m[o++] = 0; m[o++] = 0x01; m[o++] = 0x2C;   /* TTL 300 */
        int rl = rr[i].rdlen_override ? rr[i].rdlen_override : rr[i].rdlen;
        m[o++] = (uint8_t)(rl >> 8); m[o++] = (uint8_t)rl;
        memcpy(m + o, rr[i].rdata, (size_t)rr[i].rdlen);
        o += rr[i].rdlen;
    }
    return o;
}

/* The QTYPE of the n'th datagram dns.c sent, and its transaction id. */
static int sent_qtype(int i)
{
    const uint8_t *b = sent_log[i].b;
    int o = 12;
    while (b[o]) o += b[o] + 1;
    o++;
    return (b[o] << 8) | b[o + 1];
}
static uint16_t sent_txid(int i)
{ return (uint16_t)((sent_log[i].b[0] << 8) | sent_log[i].b[1]); }

static void reset_dns(void)
{
    memset(dq, 0, sizeof dq);
    memset(dcache, 0, sizeof dcache);
    dcache_w = 0;
    memset(socks6, 0, sizeof socks6);
    nsent = 0;
    next_id = 0x1000;
    ticks = 1000;
    v6_state = 0;
    v6_src = A6("2001:db8:1::5054:ff:fe12:3456");
}

static void tick_to(int n) { for (int i = 0; i < n; i++) { ticks++; dns_poll(); } }

/* ---- 1. a v4-only network is untouched ----------------------------------- */

static void test_v4_only_wire_unchanged(void)
{
    reset_dns();
    v6_state = 0;                               /* no routable IPv6 */

    int id = dns_query_start("example.test");
    CHECK(id >= 0, "lookup started");
    CHECK(nsent == 1, "EXACTLY ONE query goes on the wire (got %d)", nsent);
    CHECK(sent_qtype(0) == 1, "and it is QTYPE=A (got %d)", sent_qtype(0));
    CHECK(sent_log[0].dst == RESOLVER && sent_log[0].dport == 53, "to the resolver:53");

    static const uint8_t a1[4] = { 93, 184, 216, 34 };
    static const uint8_t a2[4] = { 93, 184, 216, 35 };
    struct rr rr[2] = { { 1, a1, 4, 0 }, { 1, a2, 4, 0 } };
    uint8_t m[512];
    int n = build_resp(m, "example.test", sent_txid(0), 1, rr, 2);
    deliver(m, n, RESOLVER, 53);
    dns_poll();

    CHECK(dns_query_result(id) == 0x5DB8D822u, "the legacy uint32 API returns the first A (%08x)",
          dns_query_result(id));
    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 2, "two destinations (got %d)", k);
    CHECK(ip6_is_v4mapped(&out[0]) && ip6_is_v4mapped(&out[1]), "both v4-mapped");
    /* Resolver order preserved: RFC 6724 rule 9 is deliberately NOT applied to
     * IPv4 pairs (see ip6_addr.c), so a round-robin answer stays round-robin. */
    CHECK(ip6_to_v4(&out[0]) == 0x5DB8D822u && ip6_to_v4(&out[1]) == 0x5DB8D823u,
          "in the resolver's order (%s, %s)", F(&out[0]), F(&out[1]));
    CHECK(nsent == 1, "and still only one datagram was ever sent (%d)", nsent);
    dns_query_free(id);
}

static void test_link_local_only_still_asks_no_aaaa(void)
{
    /* ip6_up() == 1 means a link-local address exists but nothing routable.
     * Asking for AAAA then would only produce destinations we cannot reach. */
    reset_dns();
    v6_state = 1;
    int id = dns_query_start("example.test");
    CHECK(nsent == 1, "link-local-only asks for A only (%d datagrams)", nsent);
    CHECK(sent_qtype(0) == 1, "QTYPE=A");
    dns_query_free(id);
}

/* ---- 2. the dual-stack lookup -------------------------------------------- */

static void test_dual_query_and_merge(void)
{
    reset_dns();
    v6_state = 2;

    int id = dns_query_start("dual.test");
    CHECK(nsent == 2, "a dual-stack host asks for both (got %d)", nsent);
    CHECK(sent_qtype(0) == 1 && sent_qtype(1) == 28, "A then AAAA (%d, %d)",
          sent_qtype(0), sent_qtype(1));
    CHECK(sent_txid(0) != sent_txid(1),
          "with SEPARATE transaction ids -- one id for both would let either "
          "answer be mistaken for the other");

    static const uint8_t a1[4] = { 93, 184, 216, 34 };
    ip6_addr g = A6("2001:db8:1::99");
    struct rr ra[1] = { { 1, a1, 4, 0 } };
    struct rr r6[1] = { { 28, g.b, 16, 0 } };
    uint8_t m[512];
    int n;

    n = build_resp(m, "dual.test", sent_txid(0), 1, ra, 1);
    deliver(m, n, RESOLVER, 53);
    n = build_resp(m, "dual.test", sent_txid(1), 28, r6, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();

    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 2, "both families in the answer set (got %d)", k);
    /* Our source is a global 2001:db8:1::/64 address, so the v6 destination has
     * precedence 40 against IPv4's 35 and sorts first. */
    CHECK(!ip6_is_v4mapped(&out[0]), "the global v6 destination first (%s)", F(&out[0]));
    CHECK(ip6_is_v4mapped(&out[1]), "IPv4 second (%s)", F(&out[1]));
    /* And the legacy API still answers with the A record, so every existing
     * IPv4-only caller sees what it always saw. */
    CHECK(dns_query_result(id) == 0x5DB8D822u, "the legacy API still returns the A record");
    dns_query_free(id);
}

static void test_no_usable_v6_source_demotes_v6(void)
{
    /* Same answer set, but our only IPv6 address is link-local (v6_state 2 with
     * a link-local "global" is the model's way of saying the path is broken).
     * ip6_select_source() then finds nothing usable for the v6 destination and
     * ordering must put IPv4 first. This is the resolver-level half of "a
     * broken v6 path falls back". */
    reset_dns();
    v6_state = 2;
    v6_src = A6("fe80::5054:ff:fe12:3456");

    int id = dns_query_start("dual.test");
    static const uint8_t a1[4] = { 93, 184, 216, 34 };
    ip6_addr g = A6("2606:2800:220:1:248:1893:25c8:1946");
    struct rr ra[1] = { { 1, a1, 4, 0 } };
    struct rr r6[1] = { { 28, g.b, 16, 0 } };
    uint8_t m[512];
    deliver(m, build_resp(m, "dual.test", sent_txid(0), 1, ra, 1), RESOLVER, 53);
    deliver(m, build_resp(m, "dual.test", sent_txid(1), 28, r6, 1), RESOLVER, 53);
    dns_poll();

    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 2, "both are still returned (got %d)", k);
    CHECK(ip6_is_v4mapped(&out[0]),
          "but IPv4 is tried FIRST when no v6 source is usable (got %s)", F(&out[0]));
    dns_query_free(id);
}

/* ---- 3. one family missing must not stall the other ---------------------- */

static void test_aaaa_never_answers(void)
{
    reset_dns();
    v6_state = 2;
    int id = dns_query_start("halfdead.test");
    static const uint8_t a1[4] = { 10, 0, 2, 2 };
    struct rr ra[1] = { { 1, a1, 4, 0 } };
    uint8_t m[512];
    deliver(m, build_resp(m, "halfdead.test", sent_txid(0), 1, ra, 1), RESOLVER, 53);
    dns_poll();

    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 0,
          "still pending while the AAAA half has time left");
    tick_to(DNS_2ND_GRACE + 2);
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1, "the A answer is delivered after the grace period (got %d)", k);
    CHECK(ip6_to_v4(&out[0]) == 0x0A000202u, "and it is the A record");
    /* The grace must be SHORT relative to the overall timeout, or a filtered
     * AAAA costs three seconds on every lookup instead of half of one. */
    CHECK(DNS_2ND_GRACE * 4 < DNS_GIVEUP,
          "the second-family grace (%d) is a small fraction of the timeout (%d)",
          DNS_2ND_GRACE, DNS_GIVEUP);
    dns_query_free(id);
}

static void test_a_never_answers(void)
{
    /* The other way round: only AAAA comes back. The name resolves -- to IPv6
     * addresses -- and the legacy uint32 API has to say "no IPv4 address"
     * rather than invent one. */
    reset_dns();
    v6_state = 2;
    int id = dns_query_start("v6only.test");
    ip6_addr g = A6("2001:db8:1::99");
    struct rr r6[1] = { { 28, g.b, 16, 0 } };
    uint8_t m[512];
    deliver(m, build_resp(m, "v6only.test", sent_txid(1), 28, r6, 1), RESOLVER, 53);
    dns_poll();
    tick_to(DNS_2ND_GRACE + 2);

    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1, "a AAAA-only name resolves (got %d)", k);
    CHECK(!ip6_is_v4mapped(&out[0]) && ip6_equal(&out[0], &g), "to its IPv6 address");
    CHECK(dns_query_result(id) == 0xFFFFFFFFu,
          "and the IPv4-only API reports failure rather than inventing an address");
    dns_query_free(id);
}

static void test_nothing_answers(void)
{
    reset_dns();
    v6_state = 2;
    int id = dns_query_start("void.test");
    ip6_addr out[8];
    tick_to(DNS_GIVEUP + 5);
    CHECK(dns_query_addrs(id, out, 8) < 0, "a lookup nobody answers fails");
    CHECK(dns_query_result(id) == 0xFFFFFFFFu, "and reports failure on the legacy API");
    dns_query_free(id);
}

/* ---- 4. what a name is not allowed to resolve to ------------------------- */

static void test_hostile_aaaa_records_rejected(void)
{
    /* Sixteen bytes chosen by whoever controls the name. Connecting to ::1
     * because a remote zone said so is a way to reach services that are bound
     * to loopback precisely so they cannot be reached; a multicast group is an
     * amplification primitive; and a v4-mapped AAAA is a way to smuggle an IPv4
     * destination past anything that checked the A records. */
    reset_dns();
    v6_state = 2;
    int id = dns_query_start("hostile.test");

    ip6_addr lo = A6("::1"), mc = A6("ff02::1"), un = A6("::"),
             v4m = A6("::ffff:127.0.0.1"), ok = A6("2001:db8:1::99");
    struct rr r6[5] = { { 28, lo.b, 16, 0 }, { 28, mc.b, 16, 0 },
                        { 28, un.b, 16, 0 }, { 28, v4m.b, 16, 0 },
                        { 28, ok.b, 16, 0 } };
    uint8_t m[512];
    deliver(m, build_resp(m, "hostile.test", sent_txid(1), 28, r6, 5), RESOLVER, 53);
    dns_poll();
    tick_to(DNS_2ND_GRACE + 2);

    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1, "four of the five AAAA records are refused (kept %d)", k);
    if (k >= 1)
        CHECK(ip6_equal(&out[0], &ok), "and the one kept is the legitimate one (%s)", F(&out[0]));
    dns_query_free(id);
}

static void test_malformed_answers(void)
{
    uint8_t m[512];
    ip6_addr out[8];

    /* A record whose RDLENGTH runs past the end of the message. */
    reset_dns(); v6_state = 2;
    int id = dns_query_start("trunc.test");
    ip6_addr g = A6("2001:db8:1::99");
    struct rr bad[1] = { { 28, g.b, 16, 400 } };            /* claims 400 bytes */
    deliver(m, build_resp(m, "trunc.test", sent_txid(1), 28, bad, 1), RESOLVER, 53);
    dns_poll(); tick_to(DNS_GIVEUP + 5);
    CHECK(dns_query_addrs(id, out, 8) < 0, "a record overrunning the message yields nothing");
    dns_query_free(id);

    /* A AAAA whose RDLENGTH is not 16 -- it is not an IPv6 address, whatever
     * the type field says. */
    reset_dns(); v6_state = 2;
    id = dns_query_start("shortrr.test");
    struct rr sh[1] = { { 28, g.b, 8, 0 } };
    deliver(m, build_resp(m, "shortrr.test", sent_txid(1), 28, sh, 1), RESOLVER, 53);
    dns_poll(); tick_to(DNS_GIVEUP + 5);
    CHECK(dns_query_addrs(id, out, 8) < 0, "a AAAA with rdlen != 16 is not an address");
    dns_query_free(id);

    /* A response too short to be a DNS header at all. */
    reset_dns(); v6_state = 2;
    id = dns_query_start("stub.test");
    uint8_t tiny[8] = { 0x10, 0x00, 0x81, 0x80, 0, 1, 0, 1 };
    deliver(tiny, 8, RESOLVER, 53);
    dns_poll();
    CHECK(dns_query_addrs(id, out, 8) == 0, "an 8-byte response is ignored, not parsed");
    dns_query_free(id);
}

static void test_spoofing_guards(void)
{
    uint8_t m[512];
    ip6_addr out[8];
    static const uint8_t a1[4] = { 6, 6, 6, 6 };
    struct rr ra[1] = { { 1, a1, 4, 0 } };

    /* Wrong transaction id. */
    reset_dns(); v6_state = 2;
    int id = dns_query_start("guard.test");
    deliver(m, build_resp(m, "guard.test", (uint16_t)(sent_txid(0) ^ 0x5A5A), 1, ra, 1),
            RESOLVER, 53);
    dns_poll();
    CHECK(dns_query_addrs(id, out, 8) == 0, "a response with the wrong txid is ignored");

    /* Right id, wrong source address. */
    deliver(m, build_resp(m, "guard.test", sent_txid(0), 1, ra, 1), 0x08080808u, 53);
    dns_poll();
    CHECK(dns_query_addrs(id, out, 8) == 0, "a response from the wrong server is ignored");

    /* Right id, wrong source port. */
    deliver(m, build_resp(m, "guard.test", sent_txid(0), 1, ra, 1), RESOLVER, 5353);
    dns_poll();
    CHECK(dns_query_addrs(id, out, 8) == 0, "a response from the wrong port is ignored");

    /* And the real one still lands, so the guards are not simply refusing
     * everything -- a test that only shows rejection proves nothing. */
    deliver(m, build_resp(m, "guard.test", sent_txid(0), 1, ra, 1), RESOLVER, 53);
    dns_poll(); tick_to(DNS_2ND_GRACE + 2);
    CHECK(dns_query_addrs(id, out, 8) == 1, "the genuine response is accepted");
    dns_query_free(id);
}

static void test_icmp_error_fails_the_lookup(void)
{
    reset_dns(); v6_state = 2;
    int id = dns_query_start("icmp.test");
    socks6[last_sock()].err = 1;                /* port unreachable */
    dns_poll();
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) < 0, "an ICMP error against our port fails the lookup");
    dns_query_free(id);
}

/* ---- 5. literals and the cache ------------------------------------------- */

static void test_literals(void)
{
    reset_dns(); v6_state = 2;

    int id = dns_query_start("10.0.2.2");
    CHECK(nsent == 0, "an IPv4 literal sends no DNS query (%d)", nsent);
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 1 && ip6_to_v4(&out[0]) == 0x0A000202u,
          "and resolves to itself");
    CHECK(dns_query_result(id) == 0x0A000202u, "on the legacy API too");
    dns_query_free(id);

    reset_dns(); v6_state = 2;
    id = dns_query_start("2001:db8::2");
    CHECK(nsent == 0, "an IPv6 literal sends no DNS query (%d)", nsent);
    CHECK(dns_query_addrs(id, out, 8) == 1 && ip6_equal(&out[0], &(ip6_addr){ .b = {
              0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,2 } }),
          "and resolves to itself (%s)", F(&out[0]));
    CHECK(dns_query_result(id) == 0xFFFFFFFFu,
          "with no IPv4 address to report on the legacy API");
    dns_query_free(id);

    /* A multicast literal is not a destination. */
    reset_dns(); v6_state = 2;
    id = dns_query_start("ff02::1");
    CHECK(id < 0 || dns_query_addrs(id, out, 8) != 1 || !ip6_is_multicast(&out[0]),
          "a multicast literal does not resolve to itself");
    if (id >= 0) dns_query_free(id);
}

static void test_cache_carries_both_families(void)
{
    reset_dns(); v6_state = 2;
    int id = dns_query_start("cached.test");
    static const uint8_t a1[4] = { 10, 0, 2, 2 };
    ip6_addr g = A6("2001:db8:1::99");
    struct rr ra[1] = { { 1, a1, 4, 0 } };
    struct rr r6[1] = { { 28, g.b, 16, 0 } };
    uint8_t m[512];
    deliver(m, build_resp(m, "cached.test", sent_txid(0), 1, ra, 1), RESOLVER, 53);
    deliver(m, build_resp(m, "cached.test", sent_txid(1), 28, r6, 1), RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 2, "first lookup resolved both families");
    dns_query_free(id);

    int before = nsent;
    int id2 = dns_query_start("cached.test");
    CHECK(nsent == before, "the second lookup sends nothing (%d new)", nsent - before);
    int k = dns_query_addrs(id2, out, 8);
    CHECK(k == 2, "and returns BOTH families from the cache (got %d)", k);
    CHECK(!ip6_is_v4mapped(&out[0]) && ip6_is_v4mapped(&out[1]),
          "still in RFC 6724 order");
    dns_query_free(id2);
}

static void test_more_records_than_fit(void)
{
    /* DNS_MAXA is 8. A zone that returns more must not write past the array --
     * this is a remote party choosing how many times to make us store 16 bytes. */
    reset_dns(); v6_state = 2;
    int id = dns_query_start("many.test");
    ip6_addr g[12];
    struct rr r6[12];
    char buf[64];
    for (int i = 0; i < 12; i++) {
        snprintf(buf, sizeof buf, "2001:db8:1::%d", i + 1);
        g[i] = A6(buf);
        r6[i].type = 28; r6[i].rdata = g[i].b; r6[i].rdlen = 16; r6[i].rdlen_override = 0;
    }
    uint8_t m[1024];
    int n = build_resp(m, "many.test", sent_txid(1), 28, r6, 12);
    deliver(m, n, RESOLVER, 53);
    dns_poll(); tick_to(DNS_2ND_GRACE + 2);

    ip6_addr out[16];
    int k = dns_query_addrs(id, out, 16);
    CHECK(k > 0 && k <= DNS_MAXA, "twelve records are capped at DNS_MAXA (got %d)", k);
    dns_query_free(id);
}

int main(void)
{
    test_v4_only_wire_unchanged();
    test_link_local_only_still_asks_no_aaaa();
    test_dual_query_and_merge();
    test_no_usable_v6_source_demotes_v6();
    test_aaaa_never_answers();
    test_a_never_answers();
    test_nothing_answers();
    test_hostile_aaaa_records_rejected();
    test_malformed_answers();
    test_spoofing_guards();
    test_icmp_error_fails_the_lookup();
    test_literals();
    test_cache_carries_both_families();
    test_more_records_than_fit();
    printf("ip6 dns: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
