/* Host unit test for the DHCP client (white-box: #includes dhcp.c). Stubs the
 * UDP socket layer with a fake server that answers DISCOVER->OFFER and
 * REQUEST->ACK/NAK, so the full state machine, option parsing, cookie/xid
 * validation, and T1 renewal run exactly as in the kernel. It also drives the
 * link (a NIC whose cable comes and goes) and the clock, to hold the client to
 * RFC 2131 4.1's backoff, to INIT-REBOOT on a link flap, and to "a lease that
 * arrives after the boot attempt gave up is taken" (X79-VG4, 2026-11-01).
 *
 * The REAL routing table (c/net/core/route.c) is included too, so that "a NAK
 * withdraws the refused address's routes" is read from route_lookup() and not
 * from a flag a stub set. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Defined below; dhcp.c declares them weak, and a white-box TU that provides
 * the real thing must say so (include/weaksym.h, SUPPRESSING ONE STUB). */
#define LOGIT_WEAK_LOCAL_netdev_link_up 1
#define LOGIT_WEAK_LOCAL_netdev_primary_ifindex 1
#define LOGIT_WEAK_LOCAL_arp_announce 1

#include "net.h"
#include "pit.h"
#include "rng.h"

struct net_config net_cfg = { .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 } };

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }
void net_poll(void) {}
void net_idle(void) { ticks++; }        /* advance time so pump loops terminate */
uint64_t time_mono_ns(void) { return ticks * 10000000ull; }   /* the TSC bound follows the same clock */

/* A small LCG rather than a constant: RFC 2131 4.1's +-1 s randomization has to
 * be seen to vary, or a client that forgot it would pass. */
static uint32_t prng_state = 0x2545F491u;
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        prng_state = prng_state * 1664525u + 1013904223u;
        out[i] = (uint8_t)(prng_state >> 24);
    }
}

/* Captured, so a test can assert on the lines the client owes the log. */
static char log_text[65536];
static size_t log_len;
void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (log_len < sizeof log_text - 1)
        log_len += (size_t)vsnprintf(log_text + log_len, sizeof log_text - log_len, fmt, args);
    va_end(args);
    if (log_len > sizeof log_text - 1)
        log_len = sizeof log_text - 1;
}

/* The NIC's link. A cable that is up from tick `link_up_at_tick`, or never. */
static uint64_t link_up_at_tick;
static int      link_forced_down;
static int      announces;

/* ---- fake UDP socket layer + scripted server (bodies need dhcp.c's bootp
 * layout, so udp_send_to/udp_recv are defined after the include) ---- */
#define RXN 8
static uint8_t  rxq[RXN][1600];
static int      rxl[RXN], rxhead, rxtail;
static uint64_t rxdue[RXN];         /* replies arrive 1 tick after the request,
                                     * so one dhcp_step() cannot ping-pong */
static int      fake_server = 1;
static int      nak_on_request;
static int      nak_requests_left;  /* NAK this many REQUESTs, then ACK again */
static unsigned reply_delay_ticks = 1;   /* a slow router answers late */
static unsigned request_reply_delay_ticks;   /* if nonzero: ACK/NAK delay, OFFERs keep reply_delay_ticks */
static int      drop_requests;           /* the server answers DISCOVERs and never a REQUEST */
static int      sends_into_dead_link;    /* transmissions attempted with the link down */
static uint64_t send_tick[256];     /* when each DISCOVER left, in ticks */
static uint32_t offer_ip   = 0x0A00020Fu;   /* 10.0.2.15 */
static uint32_t server_ip  = 0x0A000202u;   /* 10.0.2.2  */
static uint32_t dns_ip     = 0x0A000203u;   /* 10.0.2.3  */
static uint32_t offer_mask = 0xFFFFFF00u;
static uint32_t lease_secs = 3600;
static int      sends, sent_discover, sent_request;
static uint32_t last_dst;
static int      last_req_has_50;
static uint32_t last_req_ciaddr;
static int      discovers_with_ciaddr;      /* DISCOVERs that carried a nonzero ciaddr */
static uint32_t last_disc_xid, last_req_xid;
static uint8_t  saved_disc[300], saved_req[300];
static int      have_disc;

int udp_bind(uint16_t port) { (void)port; return 0; }
void udp_close(int sock) { (void)sock; }

/* Avoid fortified memcpy macros conflicting with the kernel prototypes. */
#undef memcpy
#undef memset
#include "dhcp.c"

/* The real routing table, included AFTER dhcp.c for the reason below: dhcp.c
 * calls route_flush_if() and this TU must be the one that defines it. Relative
 * to this file so the Makefile's narrower -I list still finds it. */
#include "../../c/net/core/route.c"

/* The NIC's interface index. Not RT_OIF_NIC0 on purpose: a client that flushed
 * the default index instead of asking the driver would leave these routes. */
#define TEST_NIC_OIF 3
static int nic_oif = TEST_NIC_OIF;

/* After the include, not before: dhcp.c declares these weak, and an attribute
 * may not follow a definition. */
int netdev_link_up(void) { return !link_forced_down && ticks >= link_up_at_tick; }
int netdev_primary_ifindex(void) { return nic_oif; }
void arp_announce(void) { announces++; }

static void put32n(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void push_reply(const struct bootp_hdr *req, uint8_t type);
static int udp_recv_pop(void *buf, int max);

int udp_send_to(int sock, uint32_t dst, uint16_t dport,
                const void *data, uint16_t len)
{
    (void)sock;
    if (len < sizeof(struct bootp_hdr) || dport != DHCP_DPORT)
        return -1;
    if (!netdev_link_up()) {
        sends_into_dead_link++;     /* netdev_tx_if() refuses a frame on a down link */
        return -1;
    }
    sends++;
    last_dst = dst;
    const struct bootp_hdr *m = (const struct bootp_hdr *)data;
    const uint8_t *opts = (const uint8_t *)data + sizeof *m;
    int olen = len - (int)sizeof *m;
    const uint8_t *t = opt_find(opts, olen, 53, 1);
    if (!t) return -1;
    if (*t == DHCP_DISCOVER) {
        if (sent_discover < (int)(sizeof send_tick / sizeof send_tick[0]))
            send_tick[sent_discover] = ticks;
        sent_discover++;
        last_disc_xid = ntohl(m->xid);
        if (m->ciaddr)
            discovers_with_ciaddr++;
        if (!have_disc) { memcpy(saved_disc, data, 300); have_disc = 1; }
        if (fake_server) push_reply(m, DHCP_OFFER);
    } else if (*t == DHCP_REQUEST) {
        sent_request++;
        memcpy(saved_req, data, 300);
        last_req_has_50 = opt_find(opts, olen, 50, 4) != NULL;
        last_req_ciaddr = ntohl(m->ciaddr);
        last_req_xid = ntohl(m->xid);
        int nak = nak_on_request;
        if (nak_requests_left > 0) {
            nak_requests_left--;
            nak = 1;
        }
        if (fake_server && !drop_requests)
            push_reply(m, nak ? DHCP_NAK : DHCP_ACK);
    }
    return 0;
}

int udp_recv(int sock, void *buf, int max, uint32_t *src, uint16_t *sport)
{
    (void)sock; (void)src; (void)sport;
    return udp_recv_pop(buf, max);
}

/* Queue a server reply (OFFER/ACK/NAK) echoing the request's xid/chaddr. */
static void push_reply(const struct bootp_hdr *req, uint8_t type)
{
    uint8_t *p = rxq[rxtail];
    memset(p, 0, 300);
    struct bootp_hdr *h = (struct bootp_hdr *)p;
    h->op = 2; h->htype = 1; h->hlen = 6;
    h->xid = req->xid;
    h->yiaddr = htonl(offer_ip);
    h->siaddr = htonl(server_ip);
    memcpy(h->chaddr, req->chaddr, 16);
    h->magic = htonl(DHCP_MAGIC);
    uint8_t *o = p + sizeof *h;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 54; *o++ = 4; put32n(o, server_ip); o += 4;
    *o++ = 1;  *o++ = 4; put32n(o, offer_mask); o += 4;
    *o++ = 3;  *o++ = 4; put32n(o, server_ip); o += 4;
    *o++ = 6;  *o++ = 4; put32n(o, dns_ip); o += 4;
    *o++ = 51; *o++ = 4; put32n(o, lease_secs); o += 4;
    *o++ = 255;
    rxl[rxtail] = 300;
    unsigned delay = reply_delay_ticks;
    if (type != DHCP_OFFER && request_reply_delay_ticks)
        delay = request_reply_delay_ticks;
    rxdue[rxtail] = ticks + delay;
    rxtail = (rxtail + 1) % RXN;
}

/* Queue a raw packet (for hand-corrupted offers). */
static void push_raw(const uint8_t *pkt, int len)
{
    memcpy(rxq[rxtail], pkt, (size_t)len);
    rxl[rxtail] = len;
    rxdue[rxtail] = ticks + 1;
    rxtail = (rxtail + 1) % RXN;
}

static int udp_recv_pop(void *buf, int max)
{
    if (rxhead == rxtail || rxdue[rxhead] > ticks)
        return 0;                       /* not arrived yet */
    int n = rxl[rxhead] > max ? max : rxl[rxhead];
    memcpy(buf, rxq[rxhead], (size_t)n);
    rxhead = (rxhead + 1) % RXN;
    return n;
}

static void treset(void)
{
    rxhead = rxtail = 0;
    sends = sent_discover = sent_request = 0;
    have_disc = 0;
    nak_on_request = 0;
    nak_requests_left = 0;
    reply_delay_ticks = 1;
    request_reply_delay_ticks = 0;
    drop_requests = 0;
    discovers_with_ciaddr = 0;
    last_disc_xid = last_req_xid = 0;
    nic_oif = TEST_NIC_OIF;
    route_flush();
    sends_into_dead_link = 0;
    link_up_at_tick = 0;
    link_forced_down = 0;
    announces = 0;
    log_len = 0;
    log_text[0] = 0;
    fake_server = 1;
    lease_secs = 3600;
    st = ST_IDLE;
    initial = 0;
    have_lease = 0;
    link_was_up = 1;
    memset(&net_cfg.ip, 0, 16);         /* ip/mask/gw/dns all unconfigured */
}

/* Let `seconds` of machine time pass with nothing but the periodic poll that
 * net_poll() gives the client after boot (no dhcp_run). */
static void pump_seconds(unsigned seconds)
{
    for (unsigned i = 0; i < seconds * 100; i++) {
        ticks++;
        dhcp_poll();
    }
}

/* Poll until `done()` or `limit_ticks` pass; returns ticks used. */
static unsigned pump_until_bound(unsigned limit_ticks)
{
    unsigned used = 0;
    while (used < limit_ticks && st != ST_BOUND) {
        ticks++;
        dhcp_poll();
        used++;
    }
    return used;
}

/* Whether a captured client message (300 bytes) carries option 55 asking for
 * the subnet mask (1), the router (3) and the resolver (6). */
static int asks_for_mask_router_dns(const uint8_t *message)
{
    const uint8_t *options = message + sizeof(struct bootp_hdr);
    int options_length = 300 - (int)sizeof(struct bootp_hdr);
    const uint8_t *list = opt_find(options, options_length, 55, 3);
    if (!list)
        return 0;
    int list_length = list[-1];
    int wants_mask = 0;
    int wants_router = 0;
    int wants_dns = 0;
    for (int i = 0; i < list_length; i++) {
        wants_mask |= list[i] == 1;
        wants_router |= list[i] == 3;
        wants_dns |= list[i] == 6;
    }
    return wants_mask && wants_router && wants_dns;
}

/* What c/net/ip/ip.c route_sync() does for the primary NIC before every send:
 * install the routes the configuration implies. Without this call the table
 * would stay empty and "the NAK removed them" would prove nothing. */
static void sync_routes_like_ip_c(void)
{
    if (net_cfg.ip || net_cfg.mask)
        route_v4_iface(netdev_primary_ifindex(), net_cfg.ip, net_cfg.mask, net_cfg.gw, 0);
}

/* Whether the real routing table sends `destination` out of the NIC. */
static int nic_routes_to(uint32_t destination)
{
    struct route_res result;
    return route_lookup(destination, &result) == RT_OK && result.oif == TEST_NIC_OIF;
}

/* Put a server reply of `type` on the wire carrying `reply_xid`, on the
 * router's own schedule: a NAK nobody provoked, or a late OFFER from an
 * exchange the client has already abandoned. */
static void inject_reply(uint8_t type, uint32_t reply_xid)
{
    struct bootp_hdr request_shape;
    memset(&request_shape, 0, sizeof request_shape);
    request_shape.xid = htonl(reply_xid);
    memcpy(request_shape.chaddr, net_cfg.mac, 6);
    push_reply(&request_shape, type);
}

static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* 1) Happy path: DISCOVER -> OFFER -> REQUEST -> ACK binds the lease. */
    treset();
    CHECK(dhcp_run(300) == 0, "dhcp_run happy path failed");
    CHECK(net_cfg.ip == offer_ip && net_cfg.mask == offer_mask &&
          net_cfg.gw == server_ip && net_cfg.dns == dns_ip,
          "lease: ip=%08x mask=%08x gw=%08x dns=%08x",
          net_cfg.ip, net_cfg.mask, net_cfg.gw, net_cfg.dns);
    CHECK(sent_discover == 1 && sent_request == 1 && last_dst == 0xFFFFFFFFu,
          "flow: %d discovers, %d requests, dst=%08x",
          sent_discover, sent_request, last_dst);
    CHECK(st == ST_BOUND, "state after ACK is %d, want BOUND", st);
    /* DISCOVER wire format: broadcast flag, our MAC, cookie, msgtype */
    {
        const struct bootp_hdr *d = (const struct bootp_hdr *)saved_disc;
        const uint8_t *o = saved_disc + sizeof *d;
        const uint8_t *t = opt_find(o, 300 - (int)sizeof *d, 53, 1);
        CHECK(d->op == 1 && ntohs(d->flags) == 0x8000 &&
              memcmp(d->chaddr, net_cfg.mac, 6) == 0 &&
              ntohl(d->magic) == DHCP_MAGIC && t && *t == DHCP_DISCOVER &&
              opt_find(o, 300 - (int)sizeof *d, 50, 4) == NULL,
              "DISCOVER wire format wrong");
    }
    /* REQUEST names the offer: requested-ip(50) + server-id(54), no ciaddr */
    {
        const struct bootp_hdr *r = (const struct bootp_hdr *)saved_req;
        const uint8_t *o = saved_req + sizeof *r;
        const uint8_t *v50 = opt_find(o, 300 - (int)sizeof *r, 50, 4);
        const uint8_t *v54 = opt_find(o, 300 - (int)sizeof *r, 54, 4);
        CHECK(ntohl(r->xid) == xid && r->ciaddr == 0 &&
              v50 && get32n(v50) == offer_ip && v54 && get32n(v54) == server_ip,
              "REQUEST must quote offer ip + server id");
        CHECK(asks_for_mask_router_dns(saved_disc), "DISCOVER must carry option 55 (mask, router, DNS)");
        CHECK(asks_for_mask_router_dns(saved_req), "REQUEST must carry option 55 (mask, router, DNS)");
    }

    /* 2) T1 renewal: REQUEST carries ciaddr (no option 50), ACK re-arms. */
    lease_secs = 60;                        /* renew_at = now + 30 s */
    treset();                               /* (also resets lease_secs...) */
    lease_secs = 60;
    CHECK(dhcp_run(300) == 0, "dhcp_run for renewal setup failed");
    uint64_t ra = renew_at;
    sent_request = 0;
    ticks = ra + 1;
    dhcp_poll();                            /* BOUND -> RENEWING, sends REQUEST */
    CHECK(sent_request == 1 && last_req_ciaddr == offer_ip && !last_req_has_50,
          "renewal REQUEST must use ciaddr and omit option 50");
    CHECK(asks_for_mask_router_dns(saved_req), "renewal REQUEST must carry option 55 (mask, router, DNS)");
    ticks++;
    dhcp_poll();                            /* drains the ACK */
    CHECK(st == ST_BOUND && renew_at > ra,
          "renewal re-armed (st=%d renew_at=%llu ra=%llu)",
          st, (unsigned long long)renew_at, (unsigned long long)ra);

    /* 3) NAK on renewal restarts from DISCOVER and recovers. */
    nak_on_request = 1;
    ticks = renew_at + 1;
    dhcp_poll();                            /* renewal REQUEST -> NAK queued */
    nak_on_request = 0;                     /* the NAK is already on the wire */
    int sd = sent_discover;
    ticks++;
    dhcp_poll();                            /* NAK -> DISCOVER (OFFER queued) */
    CHECK(st == ST_SELECTING && sent_discover > sd,
          "NAK must restart from DISCOVER (st=%d)", st);
    for (int i = 0; i < 16 && st != ST_BOUND; i++) { ticks++; dhcp_poll(); }
    CHECK(st == ST_BOUND, "no recovery after NAK (st=%d)", st);

    /* 4) Perpetual NAK during initial negotiation times out. */
    treset();
    nak_on_request = 1;
    CHECK(dhcp_run(300) == -1, "perpetual NAK must fail the run");
    /* The boot attempt ends, the client does not: it carries on in the
     * background (it used to go IDLE here, which is the machine that never got
     * an address on the X79-VG4). WHICH acquiring state it is in is fixed by
     * this harness, not by luck: the OFFER and the NAK each arrive one tick
     * after the message that provokes them, so the cycle is two ticks long and
     * the 300-tick boot window closes with the REQUEST of the last cycle
     * outstanding. A client whose boot window closes in REQUESTING stays there
     * (RFC 2131 3.1, test 20) instead of being forced to SELECTING; tests 8
     * and 19 pin the windows that close in SELECTING. */
    CHECK(st == ST_REQUESTING && !initial,
          "state after failed run is %d initial=%d, want background REQUESTING", st, initial);
    {
        int discovers_after_boot = sent_discover;
        pump_seconds(30);
        CHECK(sent_discover > discovers_after_boot && st != ST_IDLE && st != ST_BOUND,
              "perpetual NAK: the background client went quiet (state %d, %d new DISCOVERs)",
              st, sent_discover - discovers_after_boot);
    }

    /* 5) No server at all: retransmits, then gives up. */
    treset();
    fake_server = 0;
    CHECK(dhcp_run(300) == -1, "serverless run must fail");
    CHECK(sent_discover >= 2 && sent_request == 0,
          "expected retransmitted DISCOVERs (got %d) and no REQUEST",
          sent_discover);

    /* 6) Replies with a foreign xid or a bad magic cookie are ignored. */
    treset();
    fake_server = 0;
    {
        uint8_t bad[300];
        struct bootp_hdr *h = (struct bootp_hdr *)bad;
        memset(bad, 0, sizeof bad);
        h->op = 2; h->htype = 1; h->hlen = 6;
        h->xid = htonl(0xDEADBEEFu);        /* never our xid */
        h->yiaddr = htonl(offer_ip);
        memcpy(h->chaddr, net_cfg.mac, 6);
        h->magic = htonl(DHCP_MAGIC);
        uint8_t *o = bad + sizeof *h;
        *o++ = 53; *o++ = 1; *o++ = DHCP_OFFER; *o++ = 255;
        push_raw(bad, 300);                 /* wrong xid */
        h->magic = htonl(0x12345678u);
        push_raw(bad, 300);                 /* bad cookie */
    }
    CHECK(dhcp_run(300) == -1 && sent_request == 0,
          "bad xid/cookie offers must be ignored (requests=%d)", sent_request);

    /* 7) The link is down when the boot attempt starts and comes up after
     * 2.5 s -- the X79-VG4's gigabit autonegotiation. The boot attempt waits
     * for it (bounded) instead of spending its exchange on a dead wire, and
     * nothing is sent until the link is up. */
    treset();
    link_up_at_tick = ticks + 250;
    uint64_t t7 = ticks;
    CHECK(dhcp_run(300) == 0, "link-wait: lease after the link came up at 2.5 s");
    CHECK(sends_into_dead_link == 0, "link-wait: %d transmissions into a dead link",
          sends_into_dead_link);
    CHECK(send_tick[0] >= link_up_at_tick, "link-wait: first DISCOVER at tick %llu before link up at %llu",
          (unsigned long long)send_tick[0], (unsigned long long)link_up_at_tick);
    CHECK(ticks - t7 < 400, "link-wait: boot blocked %llu ticks", (unsigned long long)(ticks - t7));
    CHECK(net_cfg.ip == offer_ip, "link-wait: ip %08x", net_cfg.ip);

    /* 8) The link comes up AFTER the boot deadline (9 s). The boot attempt
     * returns inside its bound without having sent anything, the boot goes on,
     * and the lease arrives from dhcp_poll() the moment the link is up. This is
     * the X79-VG4 case. DHCP_NEGCTL_GIVE_UP_AFTER_BOOT must turn it red. */
    treset();
    link_up_at_tick = ticks + 900;
    uint64_t t8 = ticks;
    uint64_t t8_link = link_up_at_tick;
    CHECK(dhcp_run(300) == -1, "background-lease: boot attempt must fail with no link");
    CHECK(ticks - t8 <= 400, "background-lease: boot blocked %llu ticks (deadline 400)",
          (unsigned long long)(ticks - t8));
    CHECK(sent_discover == 0 && sends_into_dead_link == 0,
          "background-lease: sent %d DISCOVERs (%d into a dead link) with no link",
          sent_discover, sends_into_dead_link);
    CHECK(net_cfg.ip == 0, "background-lease: address must stay unconfigured, got %08x", net_cfg.ip);
    CHECK(st == ST_SELECTING && !initial,
          "background-lease: client must stay alive after the boot attempt (st=%d)", st);
    pump_until_bound(1000);             /* only dhcp_poll(): the boot has moved on */
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip,
          "background-lease: no late lease (st=%d ip=%08x)", st, net_cfg.ip);
    CHECK(ticks <= t8_link + 8, "background-lease: lease at tick %llu, link up at %llu",
          (unsigned long long)ticks, (unsigned long long)t8_link);
    CHECK(strstr(log_text, "[dhcp] lease 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3 after ") &&
          strstr(log_text, " ms (background)\n"),
          "background-lease: log line missing:\n%s", log_text);
    CHECK(announces == 1, "background-lease: %d ARP announcements, want 1", announces);

    /* 9) No server at all: the boot attempt fails, and the client keeps
     * retransmitting forever on RFC 2131 4.1's schedule -- 4, 8, 16, 32, 64, 64
     * seconds, each +-1 s -- and never invents an address. */
    treset();
    fake_server = 0;
    CHECK(dhcp_run(300) == -1, "backoff: serverless run must fail");
    int boot_discovers = sent_discover;
    uint64_t t9 = ticks;
    pump_seconds(400);
    {
        static const unsigned want_seconds[] = { 4, 8, 16, 32, 64, 64, 64 };
        CHECK(sent_discover - boot_discovers >= 7, "backoff: only %d background DISCOVERs in 400 s",
              sent_discover - boot_discovers);
        uint64_t previous = t9;
        for (unsigned i = 0; i < sizeof want_seconds / sizeof want_seconds[0]; i++) {
            uint64_t at = send_tick[boot_discovers + (int)i];
            uint64_t gap = at - previous;                   /* ticks of 10 ms */
            uint64_t low = (uint64_t)want_seconds[i] * 100 - 100 - 1;
            uint64_t high = (uint64_t)want_seconds[i] * 100 + 100 + 1;
            CHECK(gap >= low && gap <= high,
                  "backoff: gap %u was %llu ticks, want %u s +-1 s", i,
                  (unsigned long long)gap, want_seconds[i]);
            previous = at;
        }
    }
    CHECK(net_cfg.ip == 0, "backoff: address must stay unconfigured, got %08x", net_cfg.ip);
    CHECK(sent_request == 0, "backoff: %d REQUESTs with no OFFER", sent_request);
    {
        /* The jitter is real: not every gap is the same fraction of its base. */
        int distinct = 0;
        uint64_t first_gap = send_tick[boot_discovers + 4] - send_tick[boot_discovers + 3];
        uint64_t second_gap = send_tick[boot_discovers + 5] - send_tick[boot_discovers + 4];
        uint64_t third_gap = send_tick[boot_discovers + 6] - send_tick[boot_discovers + 5];
        distinct = (first_gap != second_gap) || (second_gap != third_gap);
        CHECK(distinct, "backoff: three 64 s gaps are identical (%llu): no randomization",
              (unsigned long long)first_gap);
    }

    /* 10) A server that answers SLOWLY: the OFFER to the boot attempt's own
     * DISCOVER arrives after the attempt gave up. The xid is kept, so it is
     * still a valid reply and the lease is taken. */
    treset();
    reply_delay_ticks = 350;
    CHECK(dhcp_run(300) == -1, "late-offer: boot attempt gives up on a 3.5 s router");
    pump_until_bound(2000);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip,
          "late-offer: a late OFFER was not taken (st=%d)", st);
    CHECK(strstr(log_text, "(background)\n") != NULL, "late-offer: no background lease line");

    /* 11) A link flap while BOUND restarts with INIT-REBOOT (RFC 2131 3.2):
     * one REQUEST naming the remembered address in option 50, no server id,
     * ciaddr zero -- and the lease is kept, not re-negotiated. */
    treset();
    CHECK(dhcp_run(300) == 0, "init-reboot: setup lease");
    uint32_t lease_before = net_cfg.ip;
    link_forced_down = 1;
    sent_discover = sent_request = 0;
    pump_seconds(5);
    CHECK(st == ST_BOUND && net_cfg.ip == lease_before,
          "init-reboot: lease dropped while the link was down (st=%d)", st);
    CHECK(sent_discover == 0 && sent_request == 0, "init-reboot: sent %d/%d into a dead link",
          sent_discover, sent_request);
    link_forced_down = 0;
    ticks++;
    dhcp_poll();                        /* the up edge */
    CHECK(sent_request == 1 && sent_discover == 0,
          "init-reboot: link up must send exactly one REQUEST (got %d REQUEST, %d DISCOVER)",
          sent_request, sent_discover);
    {
        const struct bootp_hdr *r = (const struct bootp_hdr *)saved_req;
        const uint8_t *o = saved_req + sizeof *r;
        const uint8_t *v50 = opt_find(o, 300 - (int)sizeof *r, 50, 4);
        CHECK(v50 && get32n(v50) == lease_before &&
              opt_find(o, 300 - (int)sizeof *r, 54, 4) == NULL && r->ciaddr == 0,
              "init-reboot: REQUEST must carry option 50 and no server id, ciaddr 0");
        CHECK(asks_for_mask_router_dns(saved_req), "init-reboot: REQUEST must carry option 55 (mask, router, DNS)");
    }
    pump_until_bound(50);
    CHECK(st == ST_BOUND && net_cfg.ip == lease_before && sent_discover == 0,
          "init-reboot: lease must be kept (st=%d discovers=%d)", st, sent_discover);
    CHECK(strstr(log_text, "INIT-REBOOT") != NULL, "init-reboot: not logged");

    /* 12) INIT-REBOOT answered with a NAK (the router was replaced): fall back
     * to DISCOVER and take the new lease. */
    treset();
    CHECK(dhcp_run(300) == 0, "init-reboot-nak: setup lease");
    link_forced_down = 1;
    pump_seconds(1);
    link_forced_down = 0;
    nak_requests_left = 1;
    sent_discover = sent_request = 0;
    ticks++;
    dhcp_poll();
    pump_until_bound(100);
    CHECK(st == ST_BOUND && sent_request >= 2 && sent_discover == 1,
          "init-reboot-nak: want NAK then DISCOVER then lease (st=%d req=%d disc=%d)",
          st, sent_request, sent_discover);

    /* 13) INIT-REBOOT met with silence: two REQUESTs, then DISCOVER, with the
     * old address still configured until a new lease replaces it. */
    treset();
    CHECK(dhcp_run(300) == 0, "init-reboot-silence: setup lease");
    uint32_t held = net_cfg.ip;
    link_forced_down = 1;
    pump_seconds(1);
    link_forced_down = 0;
    fake_server = 0;
    sent_discover = sent_request = 0;
    pump_seconds(40);
    CHECK(sent_request == DHCP_REQUEST_TRIES && sent_discover >= 1,
          "init-reboot-silence: %d REQUESTs, %d DISCOVERs after 40 s", sent_request, sent_discover);
    CHECK(net_cfg.ip == held, "init-reboot-silence: the held address was dropped");
    fake_server = 1;
    pump_until_bound(8000);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip, "init-reboot-silence: no lease once the server returned");

    /* 14) A down edge while still acquiring pauses the client (nothing sent
     * into a dead link) and the up edge restarts it at once with a fresh
     * backoff, not at the point the old one had reached. */
    treset();
    fake_server = 0;
    CHECK(dhcp_run(300) == -1, "restart: serverless run must fail");
    pump_seconds(100);                  /* deep into the backoff (64 s steps) */
    link_forced_down = 1;
    int before_down = sent_discover;
    pump_seconds(200);
    CHECK(sent_discover == before_down && sends_into_dead_link == 0,
          "restart: client transmitted %d times into a dead link",
          sent_discover - before_down);
    link_forced_down = 0;
    fake_server = 1;
    ticks++;
    dhcp_poll();
    CHECK(sent_discover == before_down + 1, "restart: link up must send a DISCOVER at once");
    pump_until_bound(50);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip, "restart: no lease after the up edge");

    /* 15) A NAK to INIT-REBOOT (the router was replaced while the cable was
     * out) means the remembered address is not valid on this network: RFC 2131
     * 3.2. The client stops using it -- configuration zeroed, the NIC's routes
     * removed from the REAL table, a fresh xid for the DISCOVER that follows --
     * instead of carrying a dead address and its routes into the next
     * negotiation. DHCP_NEGCTL_NAK_KEEPS_ADDRESS must turn this red. */
    treset();
    CHECK(dhcp_run(300) == 0, "nak-init-reboot: setup lease");
    sync_routes_like_ip_c();
    CHECK(nic_routes_to(IPV4(10, 0, 2, 99)) && nic_routes_to(IPV4(8, 8, 8, 8)),
          "nak-init-reboot: setup: the lease's routes are not in the table");
    link_forced_down = 1;
    pump_seconds(1);
    link_forced_down = 0;
    fake_server = 0;                    /* the NAK below is put on the wire by hand */
    sent_discover = sent_request = 0;
    ticks++;
    dhcp_poll();                        /* the up edge: INIT-REBOOT REQUEST */
    CHECK(st == ST_INIT_REBOOT && sent_request == 1,
          "nak-init-reboot: setup: state %d after the up edge, %d REQUESTs", st, sent_request);
    uint32_t refused_xid = xid;
    inject_reply(DHCP_NAK, refused_xid);
    ticks++;
    dhcp_poll();                        /* the NAK arrives */
    CHECK(net_cfg.ip == 0 && net_cfg.mask == 0 && net_cfg.gw == 0 && net_cfg.dns == 0,
          "nak-init-reboot: configuration kept after the NAK (ip=%08x mask=%08x gw=%08x dns=%08x)",
          net_cfg.ip, net_cfg.mask, net_cfg.gw, net_cfg.dns);
    sync_routes_like_ip_c();            /* what the next send does: nothing to install */
    CHECK(!nic_routes_to(IPV4(10, 0, 2, 99)) && !nic_routes_to(IPV4(8, 8, 8, 8)),
          "nak-init-reboot: the refused address's routes are still in the table");
    CHECK(st == ST_SELECTING && sent_discover == 1 && !have_lease,
          "nak-init-reboot: state %d, %d DISCOVERs, have_lease %d; want SELECTING, 1, 0",
          st, sent_discover, have_lease);
    CHECK(xid != refused_xid && last_disc_xid == xid,
          "nak-init-reboot: the DISCOVER reused the refused exchange's xid %08x", refused_xid);
    inject_reply(DHCP_OFFER, refused_xid);      /* a late OFFER from the refused exchange */
    ticks++;
    dhcp_poll();
    CHECK(st == ST_SELECTING && sent_request == 1,
          "nak-init-reboot: an OFFER with the refused xid was taken (state %d, %d REQUESTs)",
          st, sent_request);
    fake_server = 1;
    inject_reply(DHCP_OFFER, xid);              /* the router answers the new DISCOVER */
    ticks++;
    dhcp_poll();
    pump_until_bound(50);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip && announces == 1,
          "nak-init-reboot: no fresh lease afterwards (state %d, %d announcements)", st, announces);

    /* 16) The same for a NAK to a RENEWAL (RFC 2131 4.4.5): the address is
     * dropped, not kept on the strength of a lease the server just refused. */
    treset();
    CHECK(dhcp_run(300) == 0, "nak-renewing: setup lease");
    sync_routes_like_ip_c();
    fake_server = 0;
    ticks = renew_at + 1;
    dhcp_poll();                        /* BOUND -> RENEWING, sends the REQUEST */
    CHECK(st == ST_RENEWING, "nak-renewing: setup: state %d", st);
    refused_xid = xid;
    inject_reply(DHCP_NAK, refused_xid);
    ticks++;
    dhcp_poll();
    CHECK(net_cfg.ip == 0 && net_cfg.mask == 0 && net_cfg.gw == 0 && net_cfg.dns == 0 &&
          !nic_routes_to(IPV4(10, 0, 2, 99)) && !nic_routes_to(IPV4(8, 8, 8, 8)),
          "nak-renewing: address or routes kept after the NAK (ip=%08x)", net_cfg.ip);
    CHECK(st == ST_SELECTING && xid != refused_xid && last_disc_xid == xid,
          "nak-renewing: state %d, DISCOVER xid %08x (refused %08x)", st, last_disc_xid, refused_xid);

    /* 17) A NAK in SELECTING answers nothing -- only a DISCOVER is out -- and
     * is ignored (RFC 2131 4.3.2: NAK is a reply to a REQUEST). The client that
     * still holds its old address through an INIT-REBOOT silence (test 13)
     * keeps it, its routes, its xid and its pace. */
    treset();
    CHECK(dhcp_run(300) == 0, "nak-selecting: setup lease");
    sync_routes_like_ip_c();
    uint32_t held_ip = net_cfg.ip;
    link_forced_down = 1;
    pump_seconds(1);
    link_forced_down = 0;
    fake_server = 0;
    pump_seconds(40);
    CHECK(st == ST_SELECTING && net_cfg.ip == held_ip,
          "nak-selecting: setup: state %d ip %08x", st, net_cfg.ip);
    uint32_t selecting_xid = xid;
    int discovers_before_nak = sent_discover;
    inject_reply(DHCP_NAK, selecting_xid);
    ticks++;
    dhcp_poll();
    CHECK(net_cfg.ip == held_ip && net_cfg.mask == offer_mask && net_cfg.gw == server_ip &&
          net_cfg.dns == dns_ip,
          "nak-selecting: a NAK in SELECTING changed the configuration (ip=%08x)", net_cfg.ip);
    CHECK(nic_routes_to(IPV4(10, 0, 2, 99)) && nic_routes_to(IPV4(8, 8, 8, 8)),
          "nak-selecting: a NAK in SELECTING removed the routes");
    CHECK(st == ST_SELECTING && xid == selecting_xid && sent_discover == discovers_before_nak &&
          have_lease,
          "nak-selecting: state %d, xid changed %d, %d new DISCOVERs, have_lease %d",
          st, xid != selecting_xid, sent_discover - discovers_before_nak, have_lease);

    /* 18) ...and a NAK while BOUND, with nothing outstanding, likewise. */
    treset();
    CHECK(dhcp_run(300) == 0, "nak-bound: setup lease");
    sync_routes_like_ip_c();
    inject_reply(DHCP_NAK, xid);
    ticks++;
    dhcp_poll();
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip && nic_routes_to(IPV4(8, 8, 8, 8)),
          "nak-bound: a NAK with no REQUEST outstanding dropped the lease (state %d)", st);

    /* 19) ONE boot deadline covers the link wait AND the exchange. The link
     * comes up at each of these moments against a router that never answers;
     * the boot must come back by the 4 s deadline whatever the split, and must
     * not give up earlier than the exchange it was owed (3 s, or what is left
     * of the deadline). DHCP_NEGCTL_EXCHANGE_IGNORES_DEADLINE must turn this
     * red: its exchange starts when the link does and runs the full 3 s. */
    {
        static const unsigned link_up_ms[] = { 0, 1000, 2500, 3000, 3500, 3900, 3990 };
        for (unsigned i = 0; i < sizeof link_up_ms / sizeof link_up_ms[0]; i++) {
            treset();
            fake_server = 0;
            link_up_at_tick = ticks + link_up_ms[i] / 10;
            uint64_t started_tick = ticks;
            CHECK(dhcp_run(300) == -1, "boot-deadline[%u ms]: a silent router must fail the run",
                  link_up_ms[i]);
            unsigned long blocked_ticks = (unsigned long)(ticks - started_tick);
            unsigned long owed_ticks = (link_up_ms[i] + 3000 < 4000 ? link_up_ms[i] + 3000 : 4000) / 10;
            CHECK(blocked_ticks <= 400,
                  "boot-deadline[%u ms]: boot blocked %lu ticks, the deadline is 400",
                  link_up_ms[i], blocked_ticks);
            CHECK(blocked_ticks + 1 >= owed_ticks,
                  "boot-deadline[%u ms]: boot gave up after %lu ticks, the exchange was owed until %lu",
                  link_up_ms[i], blocked_ticks, owed_ticks);
            CHECK(st == ST_SELECTING && !initial,
                  "boot-deadline[%u ms]: no background client afterwards (state %d)", link_up_ms[i], st);
        }
    }
    /* A link that comes up 100 ms before the deadline still gets a lease from a
     * prompt router inside it, and from a slow one (answers after 3.5 s) the
     * background client takes the late OFFER to the boot DISCOVER. */
    treset();
    link_up_at_tick = ticks + 390;
    uint64_t t19 = ticks;
    CHECK(dhcp_run(300) == 0, "boot-deadline: no lease from a prompt router with the link up at 3.9 s");
    CHECK(ticks - t19 <= 400, "boot-deadline: prompt-router boot blocked %llu ticks",
          (unsigned long long)(ticks - t19));
    treset();
    reply_delay_ticks = 350;
    link_up_at_tick = ticks + 390;
    t19 = ticks;
    CHECK(dhcp_run(300) == -1, "boot-deadline: a 3.5 s router cannot fit in the last 100 ms");
    CHECK(ticks - t19 <= 400, "boot-deadline: slow-router boot blocked %llu ticks",
          (unsigned long long)(ticks - t19));
    pump_until_bound(2000);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip,
          "boot-deadline: the late OFFER was not taken by the background client (state %d)", st);

    /* 20) The boot window closes while the client waits for an ACK (the
     * router answers REQUESTs 3.5 s late). The client STAYS in REQUESTING
     * rather than broadcasting a DISCOVER at a server about to answer, so
     * that ACK -- a valid reply under the kept xid -- still binds the lease. */
    treset();
    request_reply_delay_ticks = 350;
    CHECK(dhcp_run(300) == -1, "requesting-window: a 3.5 s ACK cannot fit the boot window");
    CHECK(st == ST_REQUESTING && !initial,
          "requesting-window: the boot window closed in REQUESTING but the client moved to state %d", st);
    int discovers_at_handover = sent_discover;
    pump_until_bound(1000);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip,
          "requesting-window: the late ACK was not taken (state %d)", st);
    CHECK(sent_discover == discovers_at_handover,
          "requesting-window: %d DISCOVERs broadcast while an ACK was pending",
          sent_discover - discovers_at_handover);
    CHECK(strstr(log_text, " ms (background)\n") != NULL, "requesting-window: no background lease line");

    /* ...and a server that never ACKs is paced by the RFC 2131 4.1 backoff from
     * REQUESTING: nothing for the first step, the REQUEST repeated once, then a
     * fresh DISCOVER with a new xid. */
    treset();
    drop_requests = 1;
    CHECK(dhcp_run(300) == -1, "requesting-pace: a server that never ACKs must fail the run");
    CHECK(st == ST_REQUESTING && !initial, "requesting-pace: state %d after the boot window", st);
    int requests_at_handover = sent_request;
    discovers_at_handover = sent_discover;
    uint32_t xid_at_handover = xid;
    pump_seconds(2);
    CHECK(sent_request == requests_at_handover && sent_discover == discovers_at_handover,
          "requesting-pace: client transmitted %d REQUESTs and %d DISCOVERs inside the first backoff step",
          sent_request - requests_at_handover, sent_discover - discovers_at_handover);
    pump_seconds(4);                    /* the first step is 4 s +-1 s from the handover */
    CHECK(sent_request == requests_at_handover + 1 && sent_discover == discovers_at_handover &&
          st == ST_REQUESTING && xid == xid_at_handover,
          "requesting-pace: after the first step: %d new REQUESTs, %d new DISCOVERs, state %d",
          sent_request - requests_at_handover, sent_discover - discovers_at_handover, st);
    for (int i = 0; i < 2000 && st != ST_SELECTING; i++) {
        ticks++;
        dhcp_poll();
    }
    CHECK(st == ST_SELECTING && sent_request == requests_at_handover + 1 &&
          sent_discover == discovers_at_handover + 1 && xid != xid_at_handover,
          "requesting-pace: then DISCOVER: state %d, %d new REQUESTs, %d new DISCOVERs",
          st, sent_request - requests_at_handover, sent_discover - discovers_at_handover);
    drop_requests = 0;
    pump_until_bound(8000);
    CHECK(st == ST_BOUND && net_cfg.ip == offer_ip, "requesting-pace: no lease once the server ACKs again");

    /* 21) ciaddr is 0 in every DISCOVER (RFC 2131 4.3.6, Table 5), including
     * the one a renewal sends when it runs out of retries -- built while the
     * state is still RENEWING, which is what used to put our address there. */
    treset();
    CHECK(dhcp_run(300) == 0, "renew-ciaddr: setup lease");
    fake_server = 0;
    ticks = renew_at + 1;
    dhcp_poll();                        /* BOUND -> RENEWING, sends the REQUEST */
    CHECK(st == ST_RENEWING && last_req_ciaddr == offer_ip,
          "renew-ciaddr: setup: state %d, REQUEST ciaddr %08x", st, last_req_ciaddr);
    sent_discover = 0;
    discovers_with_ciaddr = 0;
    pump_seconds(10);
    CHECK(sent_discover >= 1, "renew-ciaddr: the exhausted renewal never re-discovered");
    CHECK(discovers_with_ciaddr == 0, "renew-ciaddr: %d of %d DISCOVERs carried a nonzero ciaddr",
          discovers_with_ciaddr, sent_discover);

    printf("\nDHCP client tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
