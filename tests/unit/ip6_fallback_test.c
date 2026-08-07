/* The dual-stack socket state machine (c/net/core/sock.c) against a model TCP.
 *
 * WHY THIS TEST EXISTS AND WHAT IT IS FOR.
 *
 * The dangerous thing about adding IPv6 to a working host is not IPv6 failing.
 * It is IPv6 being PREFERRED and then failing, because a client that reaches
 * for a v6 address it cannot use, and has no way back, is strictly worse than a
 * client with no IPv6 at all -- every dual-stack site it used to load simply
 * stops loading. So the two claims that matter here are the two this file
 * asserts, and neither can be observed from a packet capture of a healthy
 * network:
 *
 *   1. A PREFERRED-BUT-BROKEN IPv6 DESTINATION STILL FETCHES OVER IPv4, and
 *      does it in RFC 8305 time (a quarter second), not in TCP-timeout time
 *      (seven seconds). "It recovers eventually" is not the property; a
 *      seven-second stall on every page is a broken browser.
 *
 *   2. A v4-ONLY ANSWER BEHAVES EXACTLY AS IT DID BEFORE IPv6 EXISTED: one
 *      connection attempt, to the same address, with no second socket opened
 *      and no delay added. That is the no-regression claim stated as an
 *      assertion rather than as a hope, and it is checked by counting the
 *      connections the model TCP was asked for.
 *
 * The method is the one nd_test.c uses: #include the real sock.c and model
 * everything underneath it. The model TCP is scriptable per destination --
 * connects after N ticks, refuses after N ticks, or is a BLACK HOLE that never
 * answers at all, which is the shape of a real broken IPv6 path (no RST, no
 * ICMP, just silence). Nothing here is a mock of sock.c; sock.c is the code
 * under test, byte for byte as the kernel builds it. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* sock.c's SOCK_MAXDST, needed before sock.c is included; the #error after the
 * include keeps the two from drifting. */
#define SOCK_MAXDST_T 8

#include "logit_abi.h"
#include "net.h"
#include "eth.h"
#include "ip6.h"
#include "tcp.h"

/* ---- the world underneath sock.c ---------------------------------------- */

struct net_config net_cfg = {
    .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 },
    .ip = 0x0A00020Fu, .mask = 0xFFFFFF00u, .gw = 0x0A000202u, .dns = 0x0A000203u,
};

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }
int net_up(void) { return 1; }

#include "rtc.h"
void rtc_now(struct rtc_time *t)
{
    t->year = 2026; t->month = 8; t->day = 8;
    t->hour = 0; t->minute = 0; t->second = 0;
}

static char logbuf[8192];
static int  loglen;
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    loglen += vsnprintf(logbuf + loglen, sizeof logbuf - loglen - 1, fmt, ap);
    va_end(ap);
    if (loglen > (int)sizeof logbuf - 256) loglen = 0;
}

/* ARP: always warm, so the v4 path adds no delay of its own and any delay this
 * test measures is the socket layer's. */
int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN])
{ (void)ip; memset(mac, 0x11, ETH_ALEN); return 0; }
int arp_warm(uint32_t ip, int timeout) { (void)ip; (void)timeout; return 0; }
void arp_input(const uint8_t *f, uint16_t l) { (void)f; (void)l; }

/* ---- the model TCP ------------------------------------------------------- */

/* What a destination does when something connects to it. */
enum { D_CONNECTS, D_REFUSES, D_BLACKHOLE };

struct dest_policy {
    ip6_addr addr;
    int      behaviour;
    int      after;             /* ticks from the SYN until it happens */
};
#define NPOL 8
static struct dest_policy pol[NPOL];
static int npol;

static struct dest_policy *policy_for_addr(const ip6_addr *a)
{
    for (int i = 0; i < npol; i++)
        if (ip6_equal(&pol[i].addr, a)) return &pol[i];
    return NULL;
}

#define NMODEL 16
struct model_conn {
    int      used;
    ip6_addr dst;
    uint64_t opened;
    int      closed;
};
static struct model_conn mc[NMODEL];

/* Every connection the socket layer ever asked for, in order. This log is what
 * the "a v4-only answer opens exactly one connection" assertion counts. */
#define NLOG 32
static ip6_addr conn_log[NLOG];
static int      nconn_log;
static int      close_count;

static void model_reset(void)
{
    memset(mc, 0, sizeof mc);
    memset(pol, 0, sizeof pol);
    npol = 0;
    nconn_log = 0;
    close_count = 0;
    loglen = 0; logbuf[0] = 0;
}

int tcp_connect_start_addr(const struct tcp_addr *d, uint16_t port)
{
    (void)port;
    ip6_addr a;
    if (d->af == TCP_AF_INET) ip6_from_v4(d->a.v4, &a);
    else                      memcpy(a.b, d->a.v6, 16);
    if (nconn_log < NLOG) conn_log[nconn_log++] = a;
    for (int i = 0; i < NMODEL; i++)
        if (!mc[i].used) {
            mc[i].used = 1; mc[i].dst = a; mc[i].opened = ticks; mc[i].closed = 0;
            return i;
        }
    return -1;
}

int tcp_connect_start(uint32_t dst, uint16_t port)
{
    struct tcp_addr a; a.af = TCP_AF_INET; a.a.v4 = dst;
    return tcp_connect_start_addr(&a, port);
}

/* A refused connection keeps its model slot until tcp_close(), which is the
 * conservative half of what c/net/transport/tcp.c does: a RST in SYN_SENT
 * frees the slot there (conn_closed / the ICMP path both clear `used`), but a
 * connection that reached ESTABLISHED and was then reset reports -1 with the
 * slot STILL HELD. Modelling the second case is what makes "no connection is
 * left open" a real assertion about sock.c rather than about tcp.c. */
int tcp_connect_status(int id)
{
    if (id < 0 || id >= NMODEL || !mc[id].used || mc[id].closed) return -1;
    struct dest_policy *p = policy_for_addr(&mc[id].dst);
    if (!p) return -1;                          /* nothing scripted: refuse */
    if (p->behaviour == D_BLACKHOLE) return 0;  /* silence, forever */
    if ((int)(ticks - mc[id].opened) < p->after) return 0;
    return p->behaviour == D_CONNECTS ? 1 : -1;
}

void tcp_close(int id)
{
    if (id < 0 || id >= NMODEL || !mc[id].used) return;
    mc[id].closed = 1;
    mc[id].used = 0;
    close_count++;
}

/* The data path is not what this test is about; enough of it exists for
 * sock_pump()'s S_READY arm to run without special-casing. */
int tcp_send_nb(int id, const void *b, int n) { (void)id; (void)b; return n; }
int tcp_recv(int id, void *b, int max) { (void)id; (void)b; (void)max; return 0; }
int tcp_available(int id)
{ return (id >= 0 && id < NMODEL && mc[id].used) ? 0 : -1; }
int tcp_alive(int id) { return (id >= 0 && id < NMODEL && mc[id].used); }
void tcp_set_nodelay(int id, int on) { (void)id; (void)on; }
int tcp_send(int id, const void *b, int n) { (void)id; (void)b; return n; }
int tcp_connect(uint32_t d, uint16_t p) { (void)d; (void)p; return -1; }

/* TLS is not exercised (no socket here sets SOCK_F_TLS); the strong symbols
 * tls.h declares still have to resolve. */
int  tls_send(int s, const void *b, int n) { (void)s; (void)b; return n; }
int  tls_recv(int s, void *b, int n) { (void)s; (void)b; (void)n; return 0; }
void tls_close(int s) { (void)s; }

/* ---- the model resolver -------------------------------------------------- */

/* A scripted answer set, already in RFC 6724 order -- which is what
 * dns_query_addrs() hands sock.c in the kernel, and the ordering itself is
 * tested separately in ip6_addr_test.c. `pending_for` models a lookup that has
 * not finished yet, so the S_RESOLVE arm is exercised too. */
static ip6_addr answer[SOCK_MAXDST_T];
static int      nanswer;
static int      pending_for;        /* ticks the lookup stays pending */

/* A POOL, not one slot: two sockets resolve at the same time in the kernel and
 * a single-slot model would have the first socket's dns_query_free() make the
 * second socket's lookup fail -- which looks exactly like a fallback bug and is
 * not one. */
#define NQ 8
static struct { int used; uint64_t at; } mq[NQ];

int dns_query_start(const char *name)
{
    (void)name;
    for (int i = 0; i < NQ; i++)
        if (!mq[i].used) { mq[i].used = 1; mq[i].at = ticks; return i; }
    return -1;
}

void dns_query_free(int id) { if (id >= 0 && id < NQ) mq[id].used = 0; }

int dns_query_addrs(int id, ip6_addr *out, int max)
{
    if (id < 0 || id >= NQ || !mq[id].used) return -1;
    if ((int)(ticks - mq[id].at) < pending_for) return 0;
    if (nanswer == 0) return -1;
    int n = nanswer < max ? nanswer : max;
    for (int i = 0; i < n; i++) out[i] = answer[i];
    return n;
}

uint32_t dns_query_result(int id) { (void)id; return 0xFFFFFFFFu; }
void dns_poll(void) { }
uint32_t dns_resolve(const char *n) { (void)n; return 0; }

/* ---- the code under test ------------------------------------------------- */

#include "ip6_addr.c"
#include "sock.c"

/* sock.c defines SOCK_MAXDST after we needed it above; keep the two in step. */
#if SOCK_MAXDST != SOCK_MAXDST_T
#error "SOCK_MAXDST_T must match sock.c's SOCK_MAXDST"
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

static const char *F(const ip6_addr *a)
{
    static char b[4][64]; static int k;
    char *o = b[k++ & 3];
    ip6_format(a, o, 64);
    return o;
}

static void script(const char *addr, int behaviour, int after)
{
    pol[npol].addr = A(addr);
    pol[npol].behaviour = behaviour;
    pol[npol].after = after;
    npol++;
}

static void answers(const char *const *list, int n)
{
    nanswer = n;
    for (int i = 0; i < n; i++) answer[i] = A(list[i]);
}

/* A fresh world: no sockets, no connections, clock at a known point. */
static void reset_all(void)
{
    for (int i = 0; i < NSOCK; i++) sock_release(&socks[i]);
    memset(socks, 0, sizeof socks);
    model_reset();
    memset(mq, 0, sizeof mq);
    said_v6 = said_v4 = said_fb = 0;
    ticks = 1000;
    pending_for = 0;
    nanswer = 0;
}

/* Pump until the socket leaves the "still getting there" states, or `limit`
 * ticks pass. Returns the number of ticks it took. */
static int run_until_settled(int fd, int limit)
{
    uint64_t t0 = ticks;
    for (int i = 0; i < limit; i++) {
        sock_pump();
        int b = sock_poll_bits(fd, 1);
        if (b & (SOCK_P_CONNECTED | SOCK_P_ERROR)) return (int)(ticks - t0);
        ticks++;
    }
    sock_pump();
    return (int)(ticks - t0);
}

static int connected_addr(int fd, ip6_addr *out)
{
    if (fd < 0 || fd >= NSOCK || !socks[fd].used) return 0;
    if (socks[fd].state != S_READY) return 0;
    *out = socks[fd].dst[socks[fd].cur];
    return 1;
}

/* ---- 1. the no-regression claim ------------------------------------------ */

static void test_v4_only_unchanged(void)
{
    /* A name with only A records, which is every name on a v4-only network.
     * The socket must do exactly what it did before IPv6 existed: resolve,
     * ARP, ONE connection, ready -- no second family, no race, no extra tick
     * of delay. The connection count is the assertion; a "prefers IPv6"
     * implementation that opened a speculative second socket here would still
     * fetch the bytes and would still pass a naive download check. */
    reset_all();
    static const char *ans[] = { "::ffff:10.0.2.2" };
    answers(ans, 1);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);

    int fd = sock_open("example.test", 80, 0, 1);
    CHECK(fd >= 0, "socket opened (got %d)", fd);
    int took = run_until_settled(fd, 2000);

    ip6_addr got;
    CHECK(connected_addr(fd, &got), "a v4-only answer connects");
    CHECK(ip6_equal(&got, &answer[0]), "to the address the resolver gave (%s)", F(&got));
    CHECK(nconn_log == 1, "EXACTLY ONE connection was opened (got %d)", nconn_log);
    CHECK(took <= 5, "and with no added delay (%d ticks)", took);
    CHECK(strstr(logbuf, "via ipv4") != NULL, "reported as ipv4; log:\n%s", logbuf);
    CHECK(strstr(logbuf, "falling back") == NULL, "and nothing 'fell back'");

    /* Run it well past T_HAPPY: the race must never arm for a single family,
     * or a v4-only network would open a doomed second socket per connection
     * forever after. */
    for (int i = 0; i < 200; i++) { ticks++; sock_pump(); }
    CHECK(nconn_log == 1, "still one connection %d ticks later (got %d)",
          200, nconn_log);
    sock_close(fd, 1);
}

/* ---- 2. the claim that matters -------------------------------------------- */

static void test_v6_blackhole_falls_back(void)
{
    /* The real shape of a broken IPv6 path: the SYN goes out and NOTHING comes
     * back. No RST, no ICMP unreachable -- a firewall that drops, or a route
     * that does not exist. This is the case that hangs a naive client for the
     * full TCP timeout on every single connection. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "::ffff:10.0.2.2" };
    answers(ans, 2);
    script("2001:db8:1::2", D_BLACKHOLE, 0);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);

    int fd = sock_open("dual.test", 80, 0, 1);
    int took = run_until_settled(fd, 3000);

    ip6_addr got;
    CHECK(connected_addr(fd, &got), "a black-holed v6 destination still connects");
    CHECK(ip6_is_v4mapped(&got), "over IPv4 (got %s)", F(&got));
    /* T_HAPPY is 25 ticks (250 ms) and the v4 connect takes 2. Anything near
     * T_CONNECT (700) would mean the fallback was serial, not raced -- which is
     * the difference between a hiccup and a stall. */
    CHECK(took < 60, "in RFC 8305 time, not TCP-timeout time (%d ticks)", took);
    CHECK(took >= 25, "and not before the preferred family had its head start (%d)", took);
    CHECK(nconn_log == 2, "two attempts were made (got %d)", nconn_log);
    CHECK(!ip6_is_v4mapped(&conn_log[0]), "the v6 one first");
    CHECK(ip6_is_v4mapped(&conn_log[1]), "the v4 one second");
    /* The losing attempt must be torn down, or a page of sub-resources leaks a
     * TCP slot per fetch and the connection table is gone. */
    CHECK(close_count >= 1, "the losing v6 attempt was closed (%d closes)", close_count);
    sock_close(fd, 1);
}

static void test_v6_refused_falls_back(void)
{
    /* The other failure shape: the v6 listener is dead and answers with a RST
     * straight away. There is nothing to race here -- the primary fails before
     * T_HAPPY -- so this exercises advance_dst() rather than the race, and it
     * must be FASTER than the black-hole case, not slower. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "::ffff:10.0.2.2" };
    answers(ans, 2);
    script("2001:db8:1::2", D_REFUSES, 1);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);

    int fd = sock_open("dual.test", 80, 0, 1);
    int took = run_until_settled(fd, 3000);

    ip6_addr got;
    CHECK(connected_addr(fd, &got), "a refused v6 destination still connects");
    CHECK(ip6_is_v4mapped(&got), "over IPv4 (got %s)", F(&got));
    CHECK(took < 25, "immediately, without waiting out the race (%d ticks)", took);
    CHECK(strstr(logbuf, "falling back to ipv4") != NULL,
          "and it said so on the console; log:\n%s", logbuf);
    sock_close(fd, 1);
}

static void test_v6_refused_after_the_race_started(void)
{
    /* THE BUG THIS FILE FOUND, kept as the assertion that it stays fixed.
     *
     * The v6 destination does not refuse instantly and does not go silent
     * forever -- it refuses at t=30, five ticks AFTER the race has already
     * started the v4 attempt. So at the moment the primary fails there is a
     * healthy connection of the other family in flight.
     *
     * The rescued code tore that connection down and then called advance_dst(),
     * which SKIPS the alternate (it is already past cur in the list) and, on a
     * two-address answer, therefore found nothing left and failed the socket.
     * A dual-stack host with a v6 listener that RSTs a little slowly would have
     * been unable to reach that host at all -- over either family. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "::ffff:10.0.2.2" };
    answers(ans, 2);
    script("2001:db8:1::2", D_REFUSES, 30);     /* T_HAPPY is 25 */
    script("::ffff:10.0.2.2", D_CONNECTS, 40);

    int fd = sock_open("slowrst.test", 80, 0, 1);
    run_until_settled(fd, 3000);
    int bits = sock_poll_bits(fd, 1);
    CHECK((bits & SOCK_P_ERROR) == 0,
          "a v6 refusal mid-race does not fail the socket (bits %#x)", bits);
    ip6_addr got;
    CHECK(connected_addr(fd, &got), "the in-flight v4 attempt is promoted");
    CHECK(ip6_is_v4mapped(&got), "and it is the IPv4 one (got %s)", F(&got));
    /* And exactly two SYNs: promoting must not re-solicit the dead family. */
    CHECK(nconn_log == 2, "no extra attempt was started (got %d)", nconn_log);
    sock_close(fd, 1);
}

static void test_walks_the_whole_list(void)
{
    /* A name with several AAAA records and one A record, all the v6 ones dead.
     * The list is walked -- a host is not unreachable until every address it
     * published has been tried. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "2001:db8:1::3",
                                 "2001:db8:1::4", "::ffff:10.0.2.2" };
    answers(ans, 4);
    script("2001:db8:1::2", D_REFUSES, 1);
    script("2001:db8:1::3", D_REFUSES, 1);
    script("2001:db8:1::4", D_REFUSES, 1);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);

    int fd = sock_open("many.test", 80, 0, 1);
    run_until_settled(fd, 4000);
    ip6_addr got;
    CHECK(connected_addr(fd, &got), "the fourth address is reached");
    CHECK(ip6_is_v4mapped(&got), "and it is the IPv4 one (got %s)", F(&got));
    sock_close(fd, 1);
}

static void test_v4_first_falls_back_to_v6(void)
{
    /* Symmetry check. The fallback is not "v6 then v4" -- it is "the ordered
     * list", and RFC 6724 puts IPv4 first whenever the v6 prefix ranks lower
     * (which is what QEMU SLIRP's fec0::/10 does). So a dead IPv4 destination
     * must fall back to IPv6 by exactly the same machinery. A one-directional
     * fallback would pass every test written from the v6 point of view and
     * strand the host the moment the ordering came out the other way. */
    reset_all();
    static const char *ans[] = { "::ffff:10.0.2.2", "2001:db8:1::2" };
    answers(ans, 2);
    script("::ffff:10.0.2.2", D_BLACKHOLE, 0);
    script("2001:db8:1::2", D_CONNECTS, 2);

    int fd = sock_open("dual.test", 80, 0, 1);
    int took = run_until_settled(fd, 3000);
    ip6_addr got;
    CHECK(connected_addr(fd, &got), "a black-holed v4 destination falls back too");
    CHECK(!ip6_is_v4mapped(&got), "over IPv6 (got %s)", F(&got));
    CHECK(took < 60, "in race time (%d ticks)", took);
    sock_close(fd, 1);
}

/* ---- 3. failing is also a behaviour -------------------------------------- */

static void test_all_dead_fails_cleanly(void)
{
    /* Every address is dead. The socket must FAIL -- with an error the app can
     * see -- rather than sit in S_CONNECT forever holding a TCP slot. A
     * fallback chain with no end is its own kind of hang. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "2001:db8:1::3" };
    answers(ans, 2);
    script("2001:db8:1::2", D_REFUSES, 1);
    script("2001:db8:1::3", D_REFUSES, 1);

    int fd = sock_open("dead.test", 80, 0, 1);
    int took = run_until_settled(fd, 4000);
    int bits = sock_poll_bits(fd, 1);
    CHECK((bits & SOCK_P_ERROR) != 0, "an all-dead answer reports an error (bits %#x)", bits);
    CHECK(took < 200, "and does not wait out every timeout to say so (%d ticks)", took);
    /* Nothing may be left holding a connection. */
    int live = 0;
    for (int i = 0; i < NMODEL; i++) if (mc[i].used) live++;
    CHECK(live == 0, "no model connection is left open (%d live)", live);
    sock_close(fd, 1);
}

static void test_blackhole_everywhere_times_out(void)
{
    /* Silence on both families. This is the slow case by construction -- there
     * is nothing to learn from -- but it must still end, and end in an error,
     * within the socket layer's own deadline rather than never. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "::ffff:10.0.2.2" };
    answers(ans, 2);
    script("2001:db8:1::2", D_BLACKHOLE, 0);
    script("::ffff:10.0.2.2", D_BLACKHOLE, 0);

    int fd = sock_open("void.test", 80, 0, 1);
    int took = run_until_settled(fd, 5000);
    int bits = sock_poll_bits(fd, 1);
    CHECK((bits & SOCK_P_ERROR) != 0, "total silence eventually errors (bits %#x)", bits);
    CHECK(took < 2000, "bounded by the socket deadline (%d ticks)", took);
    sock_close(fd, 1);
}

/* ---- 4. the resolver arm -------------------------------------------------- */

static void test_pending_lookup(void)
{
    /* The lookup takes a while, then answers. Nothing may connect before it
     * does -- a socket that started a SYN against dst[0] while the answer set
     * was still being merged would be connecting to whatever was left in the
     * array from the previous socket. */
    reset_all();
    static const char *ans[] = { "::ffff:10.0.2.2" };
    answers(ans, 1);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);
    pending_for = 40;

    int fd = sock_open("slow.test", 80, 0, 1);
    for (int i = 0; i < 30; i++) { sock_pump(); ticks++; }
    CHECK(nconn_log == 0, "nothing connects while the lookup is pending (%d)", nconn_log);
    run_until_settled(fd, 2000);
    ip6_addr got;
    CHECK(connected_addr(fd, &got), "and it connects once the answer arrives");
    sock_close(fd, 1);
}

static void test_failed_lookup(void)
{
    reset_all();
    nanswer = 0;                                /* dns_query_addrs returns -1 */
    int fd = sock_open("nxdomain.test", 80, 0, 1);
    run_until_settled(fd, 1000);
    int bits = sock_poll_bits(fd, 1);
    CHECK((bits & SOCK_P_ERROR) != 0, "a name that does not resolve errors");
    CHECK(nconn_log == 0, "and opens no connection (%d)", nconn_log);
    sock_close(fd, 1);
}

/* ---- 5. several sockets at once ------------------------------------------ */

static void test_concurrent_sockets_independent(void)
{
    /* Two sockets with different answer sets must not tread on each other:
     * dst[] is per-socket, and the fallback bookkeeping (cur/alt/tcp_alt) is
     * the kind of state that is easy to accidentally make module-global. */
    reset_all();
    static const char *ans[] = { "2001:db8:1::2", "::ffff:10.0.2.2" };
    answers(ans, 2);
    script("2001:db8:1::2", D_BLACKHOLE, 0);
    script("::ffff:10.0.2.2", D_CONNECTS, 2);

    int a = sock_open("one.test", 80, 0, 1);
    int b = sock_open("two.test", 80, 0, 1);
    CHECK(a >= 0 && b >= 0 && a != b, "two sockets opened");
    for (int i = 0; i < 200; i++) { sock_pump(); ticks++; }
    ip6_addr ga, gb;
    CHECK(connected_addr(a, &ga) && ip6_is_v4mapped(&ga), "socket A fell back");
    CHECK(connected_addr(b, &gb) && ip6_is_v4mapped(&gb), "socket B fell back");
    CHECK(nconn_log == 4, "four attempts in total, two per socket (got %d)", nconn_log);
    sock_close(a, 1);
    sock_close(b, 1);
}

int main(void)
{
    test_v4_only_unchanged();
    test_v6_blackhole_falls_back();
    test_v6_refused_falls_back();
    test_v6_refused_after_the_race_started();
    test_walks_the_whole_list();
    test_v4_first_falls_back_to_v6();
    test_all_dead_fails_cleanly();
    test_blackhole_everywhere_times_out();
    test_pending_lookup();
    test_failed_lookup();
    test_concurrent_sockets_independent();
    printf("ip6 fallback: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
