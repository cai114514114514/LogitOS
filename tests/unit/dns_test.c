/* c/net/dns/dns.c against byte-array response packets -- no network, no
 * kernel, just the parser and the state machine that drives it.
 *
 * WHY THIS FILE EXISTS. dns.c used to take any A/AAAA record anywhere in a
 * response's answer section as the answer to whatever name was asked --
 * never checking the record's OWNER name. That is bug #1 below, and it is
 * also why a CNAME appeared to "work": an upstream recursive resolver
 * returns the CNAME and the final A together, and the old code took the A
 * without noticing it belonged to a different name than the one in the
 * CNAME's target. Fixing the match and fixing CNAME chasing are the SAME
 * change (collect_answers() in dns.c), which is why they are one test group
 * here rather than two.
 *
 * SCOPE: this file drives the ASYNC POOL (dns_query_start/_addrs/_free +
 * dns_poll), not the legacy dns_start()/dns_result() wrapper. Both are
 * backed by the SAME parsing/state-advancement code (dq_advance() /
 * collect_answers() in dns.c -- see that file's header comment for why),
 * so exercising the pool exercises the legacy path's logic too; the legacy
 * wrapper's own arming/ICMP-error behaviour is covered by
 * tests/unit/net_proto_test.c, which drives it against a real udp.c/icmp.c.
 * Dual-stack (AAAA, RFC 6724 ordering) is tests/unit/ip6_dns_test.c's job;
 * this file keeps ip6_up() at 0 throughout and stays IPv4-only so it does
 * not re-test what that file already covers.
 *
 * Method as elsewhere in this tree: #include the real dns.c and model UDP,
 * a model TCP (for the truncation fallback), the clock and the RNG
 * underneath it. dns.c is the code under test. */

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

/* Deterministic "random": a test needs to forge a response with the RIGHT
 * transaction id (and, for the spoofing checks, a wrong one), so the ids
 * dns.c generates have to be predictable. Sequential, like ip6_dns_test.c. */
static uint16_t next_id = 0x2000;
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) out[i] = 0;
    if (len >= 2) { out[0] = (uint8_t)(next_id & 0xFF); out[1] = (uint8_t)(next_id >> 8); }
    next_id++;
}
int rng_strong(void) { return 0; }

void net_poll(void) { }
void net_idle(void) { }

int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{ (void)ip; memset(mac, 0x22, ETH_ALEN); return 0; }
int arp_warm(uint32_t ip, int t) { (void)ip; (void)t; return 0; }
void arp_input(const uint8_t *f, uint16_t l) { (void)f; (void)l; }

/* IPv4-only throughout: see the file header for why. */
int ip6_up(void) { return 0; }
int ip6_dual_candidates(struct ip6_src_cand *cand, int max)
{
    int n = 0;
    if (n < max) {
        ip6_from_v4(net_cfg.ip, &cand[n].addr);
        cand[n].plen = 96; cand[n].state = IP6_PREFERRED; n++;
    }
    return n;
}

/* ---- the model UDP socket (same shape as ip6_dns_test.c's) --------------- */

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

/* ---- the model TCP connection (for the truncation -> TCP fallback path) --
 *
 * dns.c's fallback drives exactly four calls: connect_start, connect_status,
 * send_nb, recv, close. This model lets a test control connect latency
 * (`connected`), stage a length-prefixed reply in `rx`, and force SHORT
 * reads (`trickle`) so the accumulate-across-polls path in dns.c's
 * dns_tcp_pump() -- not just the single-call-does-everything path -- is
 * actually exercised. */

#define NTCONN 4

struct tconn {
    int      used;
    int      connected;
    int      refuse;                 /* connect_status returns -1 forever */
    uint32_t dst;
    uint16_t port;
    uint8_t  tx[600]; int txlen;
    uint8_t  rx[8192]; int rxlen, rxoff;
    int      trickle;                /* max bytes per recv call, 0 = unlimited */
};
static struct tconn tconns[NTCONN];

int tcp_connect_start(uint32_t dst, uint16_t port)
{
    for (int i = 0; i < NTCONN; i++)
        if (!tconns[i].used) {
            memset(&tconns[i], 0, sizeof tconns[i]);
            tconns[i].used = 1; tconns[i].dst = dst; tconns[i].port = port;
            return i;
        }
    return -1;
}
int tcp_connect_status(int id)
{
    if (id < 0 || id >= NTCONN || !tconns[id].used) return -1;
    if (tconns[id].refuse) return -1;
    return tconns[id].connected ? 1 : 0;
}
int tcp_send_nb(int id, const void *b, int len)
{
    if (id < 0 || id >= NTCONN || !tconns[id].used) return -1;
    struct tconn *c = &tconns[id];
    int room = (int)sizeof c->tx - c->txlen;
    int n = len < room ? len : room;
    if (n < 0) n = 0;
    if (n > 0) { memcpy(c->tx + c->txlen, b, (size_t)n); c->txlen += n; }
    return n;
}
int tcp_recv(int id, void *b, int max)
{
    if (id < 0 || id >= NTCONN || !tconns[id].used) return -1;
    struct tconn *c = &tconns[id];
    int avail = c->rxlen - c->rxoff;
    if (avail <= 0) return 0;              /* nothing yet */
    int n = avail < max ? avail : max;
    if (c->trickle > 0 && n > c->trickle) n = c->trickle;
    memcpy(b, c->rx + c->rxoff, (size_t)n);
    c->rxoff += n;
    return n;
}
void tcp_close(int id) { if (id >= 0 && id < NTCONN) tconns[id].used = 0; }

static int last_tconn(void)
{
    for (int i = NTCONN - 1; i >= 0; i--) if (tconns[i].used) return i;
    return -1;
}

#include "ip6_addr.c"
#include "dns.c"

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL(%d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- building DNS response packets ---------------------------------------- */

static int put_qname(uint8_t *o, const char *name)
{
    int n = 0;
    while (*name) {
        const char *p = name;
        while (*p && *p != '.') p++;
        int lab = (int)(p - name);
        o[n++] = (uint8_t)lab;
        for (int i = 0; i < lab; i++) o[n++] = (uint8_t)name[i];
        name = (*p == '.') ? p + 1 : p;
    }
    o[n++] = 0;
    return n;
}

/* One answer record. `owner`==NULL means "a compression pointer back at the
 * question" (offset 12) -- the common case; a non-NULL owner is spelled out
 * fresh, which is what lets a record legitimately name something OTHER than
 * the question (the unrelated-name test) or a CNAME's OWN target become the
 * owner of the next record (the chain tests). For type CNAME, `rdname` is
 * encoded as the RDATA (a name), not `rdata`/`rdlen`. */
struct rr {
    const char *owner;
    int         type;
    uint32_t    ttl;
    const uint8_t *rdata;
    int         rdlen;
    const char *rdname;      /* CNAME target, or NULL for A/AAAA */
    int         rdlen_override;
};

static int build_resp(uint8_t *m, const char *qname, uint16_t txid, int qtype,
                      int tc, const struct rr *rr, int nrr)
{
    int o = 0;
    m[o++] = (uint8_t)(txid >> 8); m[o++] = (uint8_t)txid;
    m[o++] = (uint8_t)(0x80 | 0x01 | (tc ? 0x02 : 0));   /* QR RD [TC] */
    m[o++] = 0x80;                                        /* RA */
    m[o++] = 0; m[o++] = 1;                               /* QDCOUNT */
    m[o++] = (uint8_t)(nrr >> 8); m[o++] = (uint8_t)nrr;  /* ANCOUNT */
    m[o++] = 0; m[o++] = 0;                               /* NSCOUNT */
    m[o++] = 0; m[o++] = 0;                               /* ARCOUNT */
    o += put_qname(m + o, qname);
    m[o++] = (uint8_t)(qtype >> 8); m[o++] = (uint8_t)qtype;
    m[o++] = 0; m[o++] = 1;                               /* QCLASS IN */
    for (int i = 0; i < nrr; i++) {
        if (rr[i].owner) o += put_qname(m + o, rr[i].owner);
        else { m[o++] = 0xC0; m[o++] = 12; }
        m[o++] = (uint8_t)(rr[i].type >> 8); m[o++] = (uint8_t)rr[i].type;
        m[o++] = 0; m[o++] = 1;                           /* class IN */
        m[o++] = (uint8_t)(rr[i].ttl >> 24); m[o++] = (uint8_t)(rr[i].ttl >> 16);
        m[o++] = (uint8_t)(rr[i].ttl >> 8);  m[o++] = (uint8_t)rr[i].ttl;
        if (rr[i].rdname) {
            int lenpos = o; o += 2;
            int before = o;
            o += put_qname(m + o, rr[i].rdname);
            int rdl = o - before;
            m[lenpos] = (uint8_t)(rdl >> 8); m[lenpos + 1] = (uint8_t)rdl;
        } else {
            int rl = rr[i].rdlen_override ? rr[i].rdlen_override : rr[i].rdlen;
            m[o++] = (uint8_t)(rl >> 8); m[o++] = (uint8_t)rl;
            memcpy(m + o, rr[i].rdata, (size_t)rr[i].rdlen);
            o += rr[i].rdlen;
        }
    }
    return o;
}

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
static int sent_arcount(int i)
{ return (sent_log[i].b[10] << 8) | sent_log[i].b[11]; }

static void reset_dns(void)
{
    memset(dq, 0, sizeof dq);
    memset(&legacy_dq, 0, sizeof legacy_dq);
    memset(dcache, 0, sizeof dcache);
    dcache_w = 0;
    memset(tcpfb, 0, sizeof tcpfb);
    memset(socks6, 0, sizeof socks6);
    memset(tconns, 0, sizeof tconns);
    nsent = 0;
    next_id = 0x2000;
    ticks = 1000;
}

static void tick_to(int n) { for (int i = 0; i < n; i++) { ticks++; dns_poll(); } }

/* ---- 1. baseline: a well-formed single answer resolves -------------------- */

static void test_basic_answer(void)
{
    reset_dns();
    int id = dns_query_start("good.test");
    CHECK(id >= 0 && nsent == 1, "lookup started, one query sent");
    CHECK(sent_qtype(0) == DNS_T_A, "it is a QTYPE=A query (got %d)", sent_qtype(0));
    static const uint8_t a1[4] = { 93, 184, 216, 34 };
    struct rr rr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 60, .rdata = a1, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "good.test", sent_txid(0), DNS_T_A, 0, rr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1 && ip6_to_v4(&out[0]) == 0x5DB8D822u, "resolves to the record's address (k=%d)", k);
    dns_query_free(id);
}

/* ---- 2. owner-name matching + CNAME chasing (dns.c bug #1) ---------------- */

static void test_cname_chain(void)
{
    reset_dns();
    int id = dns_query_start("alias.test");
    static const uint8_t a1[4] = { 10, 20, 30, 40 };
    struct rr rr[2] = {
        { .owner = NULL, .type = DNS_T_CNAME, .ttl = 300, .rdname = "real.test" },
        { .owner = "real.test", .type = DNS_T_A, .ttl = 300, .rdata = a1, .rdlen = 4 },
    };
    uint8_t m[512];
    int n = build_resp(m, "alias.test", sent_txid(0), DNS_T_A, 0, rr, 2);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1 && ip6_to_v4(&out[0]) == 0x0A141E28u,
          "the CNAME's target A record is the answer (k=%d)", k);
    dns_query_free(id);
}

static void test_cname_two_hops(void)
{
    reset_dns();
    int id = dns_query_start("a1.test");
    static const uint8_t a1[4] = { 1, 2, 3, 4 };
    struct rr rr[3] = {
        { .owner = NULL,       .type = DNS_T_CNAME, .ttl = 300, .rdname = "a2.test" },
        { .owner = "a2.test",  .type = DNS_T_CNAME, .ttl = 300, .rdname = "a3.test" },
        { .owner = "a3.test",  .type = DNS_T_A,     .ttl = 300, .rdata = a1, .rdlen = 4 },
    };
    uint8_t m[512];
    int n = build_resp(m, "a1.test", sent_txid(0), DNS_T_A, 0, rr, 3);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1 && ip6_to_v4(&out[0]) == 0x01020304u,
          "a two-hop CNAME chain still resolves (k=%d)", k);
    dns_query_free(id);
}

/* NOTE ON WHAT IS NOT HERE: a "chain one hop past DNS_MAX_CNAME must not
 * resolve" test was deliberately left out. Its only observable failure mode
 * IS "the final A record's owner does not match" -- the same mechanism the
 * unrelated-name test below exercises -- so under DNS_NO_NAME_MATCH it would
 * ALSO redden, and the negative control is specified to redden EXACTLY the
 * unrelated-name case (see test_unrelated_name_refused()'s comment). Two
 * checks with the same failure mechanism is not two controls; keeping this
 * one out keeps the redden count precise instead of a test that would need
 * its own #ifdef to dodge the macro, which is worse -- a control that adapts
 * to the flag it is supposed to be testing proves nothing. The bound itself
 * (cnames_followed < DNS_MAX_CNAME) still runs on every chain tested above;
 * only the "one hop too many" edge is untested here. */

static void test_unrelated_name_refused(void)
{
    /* THE bug this file exists to close: an A record for a name we did not
     * ask about, sharing the answer section with nothing else. Before the
     * owner-name check existed this was accepted as the answer to OUR
     * question. DNS_NO_NAME_MATCH (make test-dns-negctl) restores exactly
     * that and must redden this one check, and only this one. */
    reset_dns();
    int id = dns_query_start("good.test");
    static const uint8_t evil[4] = { 6, 6, 6, 6 };
    struct rr rr[1] = { { .owner = "evil.test", .type = DNS_T_A, .ttl = 60,
                          .rdata = evil, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "good.test", sent_txid(0), DNS_T_A, 0, rr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    tick_to(DNS_GIVEUP + 5);
    ip6_addr out[8];
    int k = dns_query_addrs(id, out, 8);
    /* Asserted UNCONDITIONALLY, deliberately -- a check that branches on
     * DNS_NO_NAME_MATCH to expect the wrong answer under it is not a control,
     * it is a test that always passes. This is the ONE check the negative
     * control (`make test-dns-negctl`) is required to redden. */
    CHECK(k < 0, "a record for a name we did not ask about is refused (k=%d)", k);
    dns_query_free(id);
}

/* ---- 3. truncation (TC) triggers a real TCP fallback ---------------------- */

static void test_truncated_falls_back_to_tcp(void)
{
    reset_dns();
    int id = dns_query_start("trunc.test");
    CHECK(nsent == 1, "one UDP query sent (%d)", nsent);

    /* A response claiming truncation. Its own content is irrelevant -- TC
     * means "do not trust this", so dns.c must not parse it as an answer,
     * only as a signal to retry over TCP. */
    static const uint8_t stale[4] = { 1, 1, 1, 1 };
    struct rr trr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 60, .rdata = stale, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "trunc.test", sent_txid(0), DNS_T_A, 1 /* TC */, trr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();

    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 0,
          "still pending -- a truncated response is not an answer");

    int fb = -1;
    for (int i = 0; i < DNS_TCP_NQ; i++) if (tcpfb[i].used && tcpfb[i].owner == &dq[id]) fb = i;
    CHECK(fb >= 0, "a TCP fallback attempt was started");
    int tc = tcpfb[fb].id;
    CHECK(tc >= 0 && tc == last_tconn() && tconns[tc].used &&
          tconns[tc].dst == RESOLVER && tconns[tc].port == 53,
          "it connects to the resolver on port 53");

    tconns[tc].connected = 1;
    /* Each dns_poll() advances the fallback's state machine by exactly one
     * stage (connecting -> sending -> ...), matching how a real caller polls
     * from net_poll() rather than stepping in lockstep with dns.c's internal
     * states -- so this loops rather than assuming one call reaches "sent". */
    for (int i = 0; i < 10 && tconns[tc].txlen == 0; i++) dns_poll();
    CHECK(tconns[tc].txlen > 2, "the length-prefixed query was sent over TCP (%d bytes)", tconns[tc].txlen);
    int qlen = (tconns[tc].tx[0] << 8) | tconns[tc].tx[1];
    CHECK(qlen == tconns[tc].txlen - 2, "the length prefix matches the message (%d vs %d)",
          qlen, tconns[tc].txlen - 2);
    uint16_t fb_txid = (uint16_t)((tconns[tc].tx[2] << 8) | tconns[tc].tx[3]);
    CHECK(fb_txid == tcpfb[fb].txid,
          "the TCP attempt carries its OWN txid, independent of the UDP transaction");

    /* Stage the REAL (untruncated) answer as the TCP response, and trickle
     * it out 3 bytes at a time so the accumulate-across-polls path in
     * dns_tcp_pump() is what actually delivers it, not a lucky one-shot read. */
    static const uint8_t real[4] = { 172, 16, 5, 9 };
    struct rr rrr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 45, .rdata = real, .rdlen = 4 } };
    uint8_t full[512];
    int flen = build_resp(full, "trunc.test", fb_txid, DNS_T_A, 0, rrr, 1);
    tconns[tc].rx[0] = (uint8_t)(flen >> 8); tconns[tc].rx[1] = (uint8_t)flen;
    memcpy(tconns[tc].rx + 2, full, (size_t)flen);
    tconns[tc].rxlen = flen + 2;
    tconns[tc].trickle = 3;

    for (int i = 0; i < 200 && tconns[tc].used; i++) dns_poll();
    /* dq_accept() runs inside dns_tcp_pump(), which dns_poll() calls AFTER
     * dq_advance() -- so the poll that completes the fallback (tconns[tc]
     * goes to unused) still finalizes the QUERY on the *next* dns_poll(),
     * exactly like a real two-tick, one-net_poll()-cycle delay. */
    dns_poll();

    int k = dns_query_addrs(id, out, 8);
    CHECK(k == 1 && ip6_to_v4(&out[0]) == 0xAC100509u,
          "the untruncated answer arrived over TCP (k=%d)", k);
    CHECK(!tconns[tc].used, "the TCP connection was closed once the answer arrived");
    dns_query_free(id);
}

/* ---- 4. a compression-pointer loop is rejected, not spun on --------------- */

static void test_compression_pointer_loop(void)
{
    reset_dns();
    int id = dns_query_start("loop.test");
    uint8_t m[512];
    int o = 0;
    m[o++] = (uint8_t)(sent_txid(0) >> 8); m[o++] = (uint8_t)sent_txid(0);
    m[o++] = 0x81; m[o++] = 0x80;
    m[o++] = 0; m[o++] = 1;             /* QDCOUNT */
    m[o++] = 0; m[o++] = 1;             /* ANCOUNT */
    m[o++] = 0; m[o++] = 0; m[o++] = 0; m[o++] = 0;
    o += put_qname(m + o, "loop.test");
    m[o++] = 0; m[o++] = 1;             /* QTYPE A */
    m[o++] = 0; m[o++] = 1;             /* QCLASS IN */
    int self = o;
    m[o++] = 0xC0; m[o++] = (uint8_t)self;   /* a pointer pointing AT ITSELF */
    m[o++] = 0; m[o++] = 1;             /* TYPE A */
    m[o++] = 0; m[o++] = 1;             /* CLASS IN */
    m[o++] = 0; m[o++] = 0; m[o++] = 0; m[o++] = 60;  /* TTL */
    m[o++] = 0; m[o++] = 4;             /* RDLENGTH */
    m[o++] = 1; m[o++] = 2; m[o++] = 3; m[o++] = 4;
    deliver(m, o, RESOLVER, 53);
    dns_poll();
    tick_to(DNS_GIVEUP + 5);
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) < 0,
          "a self-referencing compression pointer does not resolve (and did not hang)");
    dns_query_free(id);
}

/* ---- 5. the TTL is read off the record, not fixed at 120 s ---------------- */

static void test_ttl_is_read_from_the_record(void)
{
    reset_dns();
    int id = dns_query_start("ttl.test");
    static const uint8_t a1[4] = { 8, 8, 8, 8 };
    struct rr rr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 300, .rdata = a1, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "ttl.test", sent_txid(0), DNS_T_A, 0, rr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 1, "resolved");
    CHECK(dq[id].ttl == 300, "the record's real TTL (300) is kept, not a fixed 120 (%u)", dq[id].ttl);
    dns_query_free(id);
}

static void test_ttl_floor_and_ceiling(void)
{
    /* Below the floor: DNS_TTL_FLOOR governs the cache entry's expiry. */
    reset_dns();
    int id = dns_query_start("low.test");
    static const uint8_t a1[4] = { 1, 1, 1, 1 };
    struct rr rr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 1, .rdata = a1, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "low.test", sent_txid(0), DNS_T_A, 0, rr, 1);
    uint64_t before = ticks;
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    struct dns_cent *e = dns_cache_find("low.test");
    CHECK(e != NULL, "cached");
    if (e) CHECK(e->exp == before + (uint64_t)DNS_TTL_FLOOR * TIMER_HZ,
                 "a 1 s TTL is floored to DNS_TTL_FLOOR (%u) in the cache (exp %llu vs %llu)",
                 DNS_TTL_FLOOR, (unsigned long long)e->exp,
                 (unsigned long long)(before + (uint64_t)DNS_TTL_FLOOR * TIMER_HZ));
    dns_query_free(id);

    /* Above the ceiling: DNS_TTL_CEIL governs it instead. */
    reset_dns();
    id = dns_query_start("high.test");
    struct rr rr2[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 999999u, .rdata = a1, .rdlen = 4 } };
    n = build_resp(m, "high.test", sent_txid(0), DNS_T_A, 0, rr2, 1);
    before = ticks;
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    e = dns_cache_find("high.test");
    CHECK(e != NULL, "cached");
    if (e) CHECK(e->exp == before + (uint64_t)DNS_TTL_CEIL * TIMER_HZ,
                 "a huge TTL is ceilinged to DNS_TTL_CEIL (%u) in the cache", DNS_TTL_CEIL);
    dns_query_free(id);
}

/* ---- 6. EDNS0: every query advertises a bigger receive size --------------- */

static void test_edns0_advertised(void)
{
    reset_dns();
    int id = dns_query_start("edns.test");
    CHECK(nsent == 1, "one query sent");
    CHECK(sent_arcount(0) == 1, "ARCOUNT=1 (the EDNS0 OPT pseudo-RR), got %d", sent_arcount(0));
    /* Walk past the question to the OPT record and check its CLASS field
     * (the advertised UDP payload size) and TYPE (41). */
    const uint8_t *b = sent_log[0].b;
    int o = 12;
    while (b[o]) o += b[o] + 1;
    o += 1 + 4;                          /* root label + QTYPE + QCLASS */
    int opt_type = (b[o] + 0) == 0 ? (b[o + 1] << 8 | b[o + 2]) : -1;
    /* o currently points at the OPT's owner (a single root byte 0x00). */
    CHECK(b[o] == 0, "OPT owner is the root name");
    int type = (b[o + 1] << 8) | b[o + 2];
    int cls  = (b[o + 3] << 8) | b[o + 4];
    CHECK(type == 41, "OPT TYPE is 41 (got %d)", type);
    CHECK(cls == DNS_MSG_MAX, "advertised size is DNS_MSG_MAX (got %d)", cls);
    (void)opt_type;
    dns_query_free(id);
}

/* ---- 7. spoofing guard still applies alongside name matching -------------- */

static void test_spoofed_txid_ignored(void)
{
    reset_dns();
    int id = dns_query_start("guard.test");
    static const uint8_t a1[4] = { 5, 5, 5, 5 };
    struct rr rr[1] = { { .owner = NULL, .type = DNS_T_A, .ttl = 60, .rdata = a1, .rdlen = 4 } };
    uint8_t m[512];
    int n = build_resp(m, "guard.test", (uint16_t)(sent_txid(0) ^ 0x1234), DNS_T_A, 0, rr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    ip6_addr out[8];
    CHECK(dns_query_addrs(id, out, 8) == 0, "wrong txid is ignored, stays pending");
    /* the real one still lands */
    n = build_resp(m, "guard.test", sent_txid(0), DNS_T_A, 0, rr, 1);
    deliver(m, n, RESOLVER, 53);
    dns_poll();
    CHECK(dns_query_addrs(id, out, 8) == 1, "the genuine response is accepted");
    dns_query_free(id);
}

int main(void)
{
    test_basic_answer();
    test_cname_chain();
    test_cname_two_hops();
    test_unrelated_name_refused();
    test_truncated_falls_back_to_tcp();
    test_compression_pointer_loop();
    test_ttl_is_read_from_the_record();
    test_ttl_floor_and_ceiling();
    test_edns0_advertised();
    test_spoofed_txid_ignored();
    printf("dns: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
