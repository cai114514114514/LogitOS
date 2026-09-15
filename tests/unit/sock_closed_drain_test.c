/* Production sock_open/send/close/pump, against a controllable lower TCP.
 * The resolver/connect fixture derives from ip6_fallback_test.c; unlike that
 * test, this one actually queues bytes and varies send progress after close.
 * No socket state is injected. Census reads only observe ownership BEFORE
 * cleanup. The old failure is permanent DEAD+shut handles, not a slow drain. */
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

/* Zero models transport backpressure, negative a terminal send failure, and
 * positive allows partial progress. sock.c owns and drains its real TX ring. */
static int send_result, send_calls, sent_bytes;
int tcp_send_nb(int id, const void *b, int n)
{
    (void)id; (void)b; send_calls++;
    if (send_result <= 0) return send_result;
    if (n > send_result) n = send_result;
    sent_bytes += n;
    return n;
}
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


static int live_handles(void)
{
    int n=0; for(int i=0;i<NSOCK;i++) n+=socks[i].used!=0; return n;
}
static int dial(void)
{
    int fd=sock_open("drain.example",80,0,1);
    if(fd>=0) {run_until_settled(fd,20); CHECK(sock_poll_bits(fd,1)&SOCK_P_CONNECTED,"connected");}
    return fd;
}
static void setup(void)
{
    reset_all();
    const char *a[]={"::ffff:192.0.2.1"}; answers(a,1); script(a[0],D_CONNECTS,0);
    send_calls=sent_bytes=0; send_result=0;
}
static void successful_drain(void)
{
    setup(); int fd=dial(); CHECK(fd>=0,"open");
    CHECK(sock_send(fd,"GET / HTTP/1.1\r\n\r\n",18,1)==18,"queued request");
    CHECK(sock_close(fd,1)==0,"close accepted");
    CHECK(live_handles()==1,"backpressure retains one handle temporarily");
    CHECK(sock_poll_bits(fd,1)==SOCK_E_ARG,"closed handle unavailable");
    send_result=3; sock_pump();
    CHECK(sent_bytes==18,"all queued bytes reached transport, got %d",sent_bytes);
    CHECK(live_handles()==0 && close_count==1,"successful drain releases once");
}
static void timeout_drain(void)
{
    setup(); int fd=dial(); sock_send(fd,"tail",4,1); sock_close(fd,1);
    ticks+=T_CONNECT+1; sock_pump();
    CHECK(live_handles()==0 && close_count==1,"blocked drain deadline releases");
}
static void open_error_remains_readable(void)
{
    setup(); int fd=dial(); send_result=-1; sock_send(fd,"tail",4,1);
    CHECK(live_handles()==1,"unclosed failed handle retained for owner");
    CHECK(sock_poll_bits(fd,1)&SOCK_P_ERROR,"owner can read transport failure");
    CHECK(sock_close(fd,1)==0 && live_handles()==0,"owner closes error handle");
}
static void closed_error_loop(void)
{
    setup(); int completed=0;
    for(int i=0;i<NSOCK+1;i++) {
        send_result=0; int fd=dial();
        if(fd<0) {printf("census: iteration=%d open=%d live=%d\n",i,fd,live_handles()); break;}
        CHECK(sock_send(fd,"tail",4,1)==4,"loop enqueues actual bytes");
        CHECK(sock_close(fd,1)==0,"loop closes owner handle");
        send_result=-1; sock_pump();
        ticks+=T_CONNECT+1; sock_pump();
        CHECK(live_handles()==0,"closed drain failure stranded handle iteration=%d live=%d",i,live_handles());
        completed++;
    }
    int dead_shut=0;
    for(int i=0;i<NSOCK;i++) dead_shut+=socks[i].used&&socks[i].shut&&socks[i].state==S_DEAD;
    printf("closed-drain census: completed=%d live=%d dead_shut=%d transport_closes=%d sends=%d\n",completed,live_handles(),dead_shut,close_count,send_calls);
    CHECK(completed==NSOCK+1,"17th transfer must start after 16 closes");
    /* Process teardown can reclaim the old leaked handles, but cannot turn
     * their pre-cleanup census into a pass for a long-lived browser process. */
    sock_close_owner(1);
    CHECK(live_handles()==0,"process cleanup");
}
int main(void)
{
    successful_drain(); timeout_drain(); open_error_remains_readable(); closed_error_loop();
    printf("sock-closed-drain: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
