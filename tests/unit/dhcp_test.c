/* Host unit test for the DHCP client (white-box: #includes dhcp.c). Stubs the
 * UDP socket layer with a fake server that answers DISCOVER->OFFER and
 * REQUEST->ACK/NAK, so the full state machine, option parsing, cookie/xid
 * validation, and T1 renewal run exactly as in the kernel. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "pit.h"
#include "rng.h"

struct net_config net_cfg = { .mac = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 } };

static uint64_t ticks;
uint64_t timer_ticks(void) { return ticks; }
void net_poll(void) {}
void net_idle(void) { ticks++; }        /* advance time so pump loops terminate */
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) out[i] = (uint8_t)(0x5Au + (unsigned)i);
}
void kprintf(const char *fmt, ...) { (void)fmt; }

/* ---- fake UDP socket layer + scripted server (bodies need dhcp.c's bootp
 * layout, so udp_send_to/udp_recv are defined after the include) ---- */
#define RXN 8
static uint8_t  rxq[RXN][1600];
static int      rxl[RXN], rxhead, rxtail;
static uint64_t rxdue[RXN];         /* replies arrive 1 tick after the request,
                                     * so one dhcp_step() cannot ping-pong */
static int      fake_server = 1;
static int      nak_on_request;
static uint32_t offer_ip   = 0x0A00020Fu;   /* 10.0.2.15 */
static uint32_t server_ip  = 0x0A000202u;   /* 10.0.2.2  */
static uint32_t dns_ip     = 0x0A000203u;   /* 10.0.2.3  */
static uint32_t offer_mask = 0xFFFFFF00u;
static uint32_t lease_secs = 3600;
static int      sends, sent_discover, sent_request;
static uint32_t last_dst;
static int      last_req_has_50;
static uint32_t last_req_ciaddr;
static uint8_t  saved_disc[300], saved_req[300];
static int      have_disc;

int udp_bind(uint16_t port) { (void)port; return 0; }
void udp_close(int sock) { (void)sock; }

/* Avoid fortified memcpy macros conflicting with the kernel prototypes. */
#undef memcpy
#undef memset
#include "dhcp.c"

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
    sends++;
    last_dst = dst;
    const struct bootp_hdr *m = (const struct bootp_hdr *)data;
    const uint8_t *opts = (const uint8_t *)data + sizeof *m;
    int olen = len - (int)sizeof *m;
    const uint8_t *t = opt_find(opts, olen, 53, 1);
    if (!t) return -1;
    if (*t == DHCP_DISCOVER) {
        sent_discover++;
        if (!have_disc) { memcpy(saved_disc, data, 300); have_disc = 1; }
        if (fake_server) push_reply(m, DHCP_OFFER);
    } else if (*t == DHCP_REQUEST) {
        sent_request++;
        memcpy(saved_req, data, 300);
        last_req_has_50 = opt_find(opts, olen, 50, 4) != NULL;
        last_req_ciaddr = ntohl(m->ciaddr);
        if (fake_server)
            push_reply(m, nak_on_request ? DHCP_NAK : DHCP_ACK);
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
    rxdue[rxtail] = ticks + 1;
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
    lease_secs = 3600;
    st = ST_IDLE;
    memset(&net_cfg.ip, 0, 16);         /* ip/mask/gw/dns all unconfigured */
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
    CHECK(st == ST_IDLE, "state after failed run is %d, want IDLE", st);

    /* 5) No server at all: retransmits, then gives up. */
    treset();
    fake_server = 0;
    CHECK(dhcp_run(300) == -1, "serverless run must fail");
    CHECK(sent_discover >= 2 && sent_request == 0,
          "expected retransmitted DISCOVERs (got %d) and no REQUEST",
          sent_discover);

    /* 6) Replies with a foreign xid or a bad magic cookie are ignored. */
    treset();
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

    printf("\nDHCP client tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
