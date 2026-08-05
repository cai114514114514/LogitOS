#include <stdint.h>
#include <stddef.h>
#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "pit.h"
#include "rng.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* A minimal DHCPv4 client (RFC 2131/2132): one interface, broadcast-only
 * (this stack cannot receive unicast to an address it does not own yet, so
 * every message goes out with the BOOTP broadcast flag set). Lease renewal is
 * a T1 REQUEST; on NAK or repeated silence it falls back to DISCOVER. */

#define DHCP_SPORT 68
#define DHCP_DPORT 67
#define DHCP_MAGIC 0x63825363u

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_RETRY_TICKS 100        /* ~1 s between retransmits */
#define DHCP_MAX_RETRIES 4

struct bootp_hdr {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} __attribute__((packed));          /* 240 bytes; options follow on the wire */

enum { ST_IDLE, ST_SELECTING, ST_REQUESTING, ST_BOUND, ST_RENEWING };

static int      st = ST_IDLE;
static int      sock = -1;
static uint32_t xid;
static uint64_t st_tick;            /* start of the current wait */
static int      retries;
static int      initial;            /* inside dhcp_run(): failure ends the wait */
static uint32_t offered;            /* yiaddr from the OFFER being requested */
static uint32_t server_id;
static uint64_t renew_at;           /* T1: start renewing */

static uint32_t get32n(const uint8_t *p)    /* network-order 4 bytes -> host */
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Find option `code`; returns a pointer to its value (>= min_len bytes). */
static const uint8_t *opt_find(const uint8_t *o, int len, uint8_t code,
                               uint8_t min_len)
{
    for (int i = 0; i < len;) {
        if (o[i] == 0) { i++; continue; }           /* pad */
        if (o[i] == 255) break;                     /* end */
        if (i + 2 > len || i + 2 + o[i + 1] > len) break;
        if (o[i] == code && o[i + 1] >= min_len)
            return &o[i + 2];
        i += 2 + o[i + 1];
    }
    return NULL;
}

/* Build a client message into out (>= 300 bytes). msgtype is DISCOVER or
 * REQUEST; req_ip/sid (host order, 0 = omit) become options 50/54. */
static int build_msg(uint8_t *out, uint8_t msgtype,
                     uint32_t req_ip, uint32_t sid)
{
    struct bootp_hdr *m = (struct bootp_hdr *)out;
    memset(out, 0, 300);
    m->op = 1;                      /* BOOTREQUEST */
    m->htype = 1;                   /* Ethernet */
    m->hlen = 6;
    m->xid = htonl(xid);
    m->flags = htons(0x8000);       /* broadcast replies, please */
    if (st == ST_RENEWING)
        m->ciaddr = htonl(net_cfg.ip);
    memcpy(m->chaddr, net_cfg.mac, 6);
    m->magic = htonl(DHCP_MAGIC);
    uint8_t *o = out + sizeof *m;
    *o++ = 53; *o++ = 1; *o++ = msgtype;
    if (msgtype == DHCP_DISCOVER) {         /* parameter request list */
        *o++ = 55; *o++ = 4;
        *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    }
    if (req_ip) {
        uint32_t v = htonl(req_ip);
        *o++ = 50; *o++ = 4; memcpy(o, &v, 4); o += 4;
    }
    if (sid) {
        uint32_t v = htonl(sid);
        *o++ = 54; *o++ = 4; memcpy(o, &v, 4); o += 4;
    }
    *o++ = 255;
    return 300;                     /* RFC 2131 minimum BOOTP message size */
}

static void send_discover(void)
{
    uint8_t pkt[300];
    int n = build_msg(pkt, DHCP_DISCOVER, 0, 0);
    udp_send_to(sock, 0xFFFFFFFFu, DHCP_DPORT, pkt, (uint16_t)n);
}

static void send_request(void)
{
    uint8_t pkt[300];
    uint32_t req_ip = 0, sid = 0;
    if (st == ST_SELECTING || st == ST_REQUESTING) {
        req_ip = offered;           /* selecting: name the offer we take */
        sid = server_id;
    }                               /* renewing: ciaddr carries our address */
    int n = build_msg(pkt, DHCP_REQUEST, req_ip, sid);
    udp_send_to(sock, 0xFFFFFFFFu, DHCP_DPORT, pkt, (uint16_t)n);
}

static void kprint_ip(const char *tag, uint32_t ip)
{
    kprintf("%s%u.%u.%u.%u", tag,
            (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

/* Adopt an ACKed lease: fill net_cfg and arm the T1 renewal deadline. */
static void apply_lease(const struct bootp_hdr *h,
                        const uint8_t *opts, int olen)
{
    const uint8_t *v;
    net_cfg.ip = ntohl(h->yiaddr);
    if ((v = opt_find(opts, olen, 1, 4))) net_cfg.mask = get32n(v);
    if ((v = opt_find(opts, olen, 3, 4))) net_cfg.gw   = get32n(v);
    if ((v = opt_find(opts, olen, 6, 4))) net_cfg.dns  = get32n(v);
    uint32_t lease = 3600;                  /* default when option 51 is absent */
    if ((v = opt_find(opts, olen, 51, 4))) lease = get32n(v);
    uint64_t lt = (uint64_t)lease * 100;    /* seconds -> 100 Hz ticks */
    renew_at = timer_ticks() + lt / 2;

    int plen = 0;
    for (uint32_t m = net_cfg.mask; m & 0x80000000u; m <<= 1) plen++;
    kprint_ip("[dhcp] bound ", net_cfg.ip);
    kprintf("/%d", plen);
    kprint_ip(" gw ", net_cfg.gw);
    kprint_ip(" dns ", net_cfg.dns);
    kprintf("\n");
}

static void enter(int new_st)
{
    st = new_st;
    st_tick = timer_ticks();
    retries = 0;
}

static void dhcp_handle(const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(struct bootp_hdr))
        return;
    const struct bootp_hdr *h = (const struct bootp_hdr *)pkt;
    /* RFC 2131 4.1: a reply must echo our xid and chaddr, carry the magic
     * cookie, and be a BOOTREPLY. */
    if (h->op != 2 || ntohl(h->magic) != DHCP_MAGIC || ntohl(h->xid) != xid)
        return;
    for (int i = 0; i < 6; i++)
        if (h->chaddr[i] != net_cfg.mac[i]) return;
    const uint8_t *opts = pkt + sizeof *h;
    int olen = len - (int)sizeof *h;
    const uint8_t *t = opt_find(opts, olen, 53, 1);
    if (!t) return;

    if (*t == DHCP_OFFER && st == ST_SELECTING) {
        offered = ntohl(h->yiaddr);
        const uint8_t *v = opt_find(opts, olen, 54, 4);
        server_id = v ? get32n(v) : ntohl(h->siaddr);
        send_request();
        enter(ST_REQUESTING);
    } else if (*t == DHCP_ACK && (st == ST_REQUESTING || st == ST_RENEWING)) {
        apply_lease(h, opts, olen);
        st = ST_BOUND;
    } else if (*t == DHCP_NAK && st != ST_BOUND && st != ST_IDLE) {
        /* The server refused what we asked for: start over. */
        send_discover();
        enter(ST_SELECTING);
    }
}

/* One pump of the client: drain the socket, retransmit on silence, and move
 * BOUND -> RENEWING at T1. */
static void dhcp_step(void)
{
    if (st == ST_IDLE || sock < 0)
        return;
    uint8_t pkt[1500];
    int n;
    while ((n = udp_recv(sock, pkt, sizeof pkt, NULL, NULL)) > 0)
        dhcp_handle(pkt, n);

    uint64_t now = timer_ticks();
    if (st == ST_BOUND) {
        if (now >= renew_at) {
            enter(ST_RENEWING);     /* first, so build_msg sets ciaddr */
            send_request();         /* ciaddr renewal (RFC 2131 4.4.5) */
        }
        return;
    }
    if (now - st_tick < DHCP_RETRY_TICKS)
        return;
    if (++retries > DHCP_MAX_RETRIES) {
        if (initial) {
            st = ST_IDLE;           /* lets dhcp_run() report the failure */
        } else {
            /* Renewal keeps trying forever -- a dead server must not take
             * down a working config. After NAK/silence we re-DISCOVER. */
            send_discover();
            enter(ST_SELECTING);
        }
        return;
    }
    if (st == ST_SELECTING) send_discover();
    else                    send_request();
    st_tick = now;
}

int dhcp_run(int timeout_ticks)
{
    if (sock < 0) {
        sock = udp_bind(DHCP_SPORT);
        if (sock < 0)
            return -1;
    }
    kernel_random_bytes((uint8_t *)&xid, sizeof xid);
    xid ^= (uint32_t)timer_ticks();
    initial = 1;
    send_discover();
    enter(ST_SELECTING);

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < (uint64_t)timeout_ticks) {
        net_poll();
        dhcp_step();
        if (st == ST_BOUND) { initial = 0; return 0; }
        net_idle();
    }
    initial = 0;
    st = ST_IDLE;
    return -1;
}

void dhcp_poll(void)
{
    dhcp_step();
}
