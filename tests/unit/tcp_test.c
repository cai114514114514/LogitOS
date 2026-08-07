/* Host unit test for the TCP state machines (white-box: #includes tcp.c so it
 * can drive tcp_input and inspect conns[] directly). Stubs the kernel deps
 * (ip_send captures every segment we emit; the timer is a virtual counter;
 * net_lock/poll are no-ops on the host). Build via `make test-tcp-host`.
 *
 * Everything here is deterministic: there is no real clock and no real peer,
 * so "cwnd halves on loss" is an exact number, not a trend.
 *
 * `make test-tcp-negctl` builds the same file with -DTCP_NEGATIVE_CONTROL,
 * which neutralises the congestion controller's response to loss at one
 * defined point (in THIS file -- tcp.c has no test hooks) and requires the run
 * to FAIL. A congestion-control test that still passes with the controller
 * switched off is not testing the controller. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "net.h"     /* stub (tests/unit/tcpstub): struct net_config, htons, ... */

/* ---- kernel-dependency stubs (resolve tcp.c's externs) ---- */
struct net_config net_cfg = { 0x0A00020F };     /* 10.0.2.15 */

#define NCAP 4096
struct cap {
    uint32_t seq, ack;
    uint16_t win;
    uint8_t  flags;
    int      hlen, dlen;
    uint8_t  raw[1600];
};
static struct cap g_cap[NCAP];
static int g_ncap;

static uint32_t rd32_(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    (void)dst; (void)proto;
    const uint8_t *p = payload;
    if (len >= 20 && g_ncap < NCAP) {
        struct cap *c = &g_cap[g_ncap++];
        c->seq = rd32_(p + 4);
        c->ack = rd32_(p + 8);
        c->hlen = (p[12] >> 4) * 4;
        c->flags = p[13];
        c->win = (uint16_t)(((uint16_t)p[14] << 8) | p[15]);
        c->dlen = len - c->hlen;
        memcpy(c->raw, p, len > sizeof c->raw ? sizeof c->raw : len);
    }
    return 0;
}
static uint64_t g_ticks = 1;
uint64_t timer_ticks(void) { return g_ticks; }
void net_poll(void) {}
void net_idle(void) { g_ticks++; }
void kernel_random_bytes(uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) out[i] = (uint8_t)(0xA5u + (unsigned)i);
}

/* macOS <string.h> makes memcpy/memset fortify macros; tcp.c re-prototypes them
 * as plain funcs (the kernel has no libc), so drop the macros first. */
#undef memcpy
#undef memset
#include "tcp.c"     /* white-box: pulls in conns[], tcp_input, tcp_recv, ... */

/* ---- test harness ---- */
static int passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { failed++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define LPORT 4242
#define RPORT 80
#define RIP   0x0A000202u           /* 10.0.2.2 */

static uint8_t pat(uint32_t off) { return (uint8_t)(off * 31u + 7u); }

static struct cap *last(void) { return g_ncap ? &g_cap[g_ncap - 1] : &g_cap[0]; }

/* Find an option in a captured segment. Returns its length, or 0. */
static int cap_opt(const struct cap *c, uint8_t kind, const uint8_t **val)
{
    const uint8_t *o = c->raw + 20;
    int len = c->hlen - 20;
    for (int i = 0; i < len;) {
        if (o[i] == 0) break;
        if (o[i] == 1) { i++; continue; }
        if (i + 1 >= len) break;
        int ol = o[i + 1];
        if (ol < 2 || i + ol > len) break;
        if (o[i] == kind) { if (val) *val = o + i + 2; return ol; }
        i += ol;
    }
    return 0;
}

/* ---- segment injection ---- */
struct injopt {
    int      mss;
    int      have_ws;  uint8_t ws;
    int      sack_perm;
    int      have_ts;  uint32_t tsval, tsecr;
    int      n_sack;   uint32_t sack[4][2];
};

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int build_inj(uint8_t *o, const struct injopt *s, int is_syn)
{
    int n = 0;
    if (!s) return 0;
    if (is_syn && s->mss) {
        o[n++] = 2; o[n++] = 4;
        o[n++] = (uint8_t)(s->mss >> 8); o[n++] = (uint8_t)s->mss;
    }
    if (is_syn && s->sack_perm) { o[n++] = 4; o[n++] = 2; }
    if (s->have_ts) {
        o[n++] = 8; o[n++] = 10;
        put32(o + n, s->tsval); n += 4;
        put32(o + n, s->tsecr); n += 4;
    }
    if (is_syn && s->have_ws) { o[n++] = 1; o[n++] = 3; o[n++] = 3; o[n++] = s->ws; }
    if (!is_syn && s->n_sack) {
        o[n++] = 1; o[n++] = 1;
        o[n++] = 5; o[n++] = (uint8_t)(2 + 8 * s->n_sack);
        for (int i = 0; i < s->n_sack; i++) {
            put32(o + n, s->sack[i][0]); n += 4;
            put32(o + n, s->sack[i][1]); n += 4;
        }
    }
    while (n & 3) o[n++] = 0;
    return n;
}

static uint16_t g_lport = LPORT;

/* General segment injector. `base` anchors the payload pattern. */
static void inject_full(uint32_t base, uint32_t seq, uint32_t ack, uint16_t window,
                        int n, uint8_t flags, const struct injopt *io,
                        int corrupt_checksum)
{
    uint8_t buf[60 + 4096];
    uint8_t opts[40];
    int optlen = build_inj(opts, io, (flags & SYN) != 0);
    int hlen = 20 + optlen;
    memset(buf, 0, (size_t)(hlen + n));
    buf[0] = (RPORT >> 8); buf[1] = RPORT & 0xFF;
    buf[2] = (uint8_t)(g_lport >> 8); buf[3] = (uint8_t)(g_lport & 0xFF);
    put32(buf + 4, seq); put32(buf + 8, ack);
    buf[12] = (uint8_t)((hlen / 4) << 4);
    buf[13] = flags;
    buf[14] = (uint8_t)(window >> 8); buf[15] = (uint8_t)window;
    if (optlen) memcpy(buf + 20, opts, (size_t)optlen);
    for (int i = 0; i < n; i++) buf[hlen + i] = pat((seq - base) + (uint32_t)i);
    uint16_t csum = tcp_checksum(RIP, net_cfg.ip, buf, hlen + n);
    buf[16] = (uint8_t)(csum >> 8); buf[17] = (uint8_t)csum;
    if (corrupt_checksum) buf[hlen + (n ? n - 1 : 0)] ^= 0x40;
    tcp_input(RIP, buf, (uint16_t)(hlen + n));
}

/* Inject a data segment [seq, seq+n) with our current snd_nxt as the ACK. */
static int g_id;
static void inject(uint32_t base, uint32_t seq, int n, uint8_t flags)
{
    inject_full(base, seq, conns[g_id].snd_nxt, 65535, n, flags, NULL, 0);
}

/* Inject a pure ACK of `ack` with the given window (+ optional options). */
static void inject_ack(uint32_t ack, uint16_t win, const struct injopt *io)
{
    inject_full(0, conns[g_id].rcv_nxt, ack, win, 0, ACK, io, 0);
}

/* Drain all contiguous bytes; verify each equals its pattern. */
static int drain_verify(int expect_total)
{
    static uint8_t out[300000];
    int total = 0, r;
    while ((r = tcp_recv(g_id, out + total, (int)sizeof out - total)) > 0) total += r;
    int ok = (total == expect_total);
    for (int i = 0; i < total; i++) if (out[i] != pat((uint32_t)i)) { ok = 0; break; }
    CHECK(ok, "drain: got %d bytes (want %d), content %s",
          total, expect_total, ok ? "ok" : "MISMATCH");
    return total;
}

/* ---- connection setup ------------------------------------------------- */

/* Reset conns[0] into ESTABLISHED with the given initial receive sequence,
 * bypassing the handshake (for the receive-path tests that predate options). */
static void setup(uint32_t isn)
{
    g_id = 0;
    struct tcp_conn *c = &conns[0];
    memset(c, 0, sizeof *c);
    c->used = 1; c->state = ESTABLISHED;
    c->lport = g_lport = LPORT; c->rport = RPORT;
    addr_v4(&c->raddr, RIP); addr_v4(&c->laddr, net_cfg.ip);
    c->rcv_nxt = c->read_seq = c->rcv_adv = c->last_ack_sent = isn;
    c->snd_una = c->snd_nxt = c->snd_max = c->snd_end = 0xC0DE0000u;
    c->snd_wnd = 65535; c->peer_mss = MSS_ADV; c->pmtu_mss = MSS_ADV;
    c->snd_wl1 = isn; c->snd_wl2 = c->snd_una;
    c->cwnd = 64 * 1024; c->ssthresh = 0xFFFFFFFFu;
    c->rto = RTO_DEFAULT;
    g_ncap = 0;
}

/* Full three-way handshake with a peer that offers exactly these options.
 * ws < 0 means "the peer refuses window scaling". Returns the conn id. */
static int handshake(int ws, int sack, int ts, int mss, uint16_t win, uint32_t peer_isn)
{
    for (int i = 0; i < NCONN; i++) conns[i].used = 0;
    g_ncap = 0;
    int id = tcp_connect_start(RIP, RPORT);
    g_id = id;
    g_lport = conns[id].lport;
    struct cap *syn = last();
    struct injopt o;
    memset(&o, 0, sizeof o);
    o.mss = mss;
    o.have_ws = ws >= 0; o.ws = (uint8_t)(ws < 0 ? 0 : ws);
    o.sack_perm = sack;
    o.have_ts = ts;
    o.tsval = 500000; o.tsecr = 0;
    if (ts) {
        const uint8_t *v;
        if (cap_opt(syn, 8, &v)) o.tsecr = rd32_(v);
    }
    inject_full(peer_isn, peer_isn, syn->seq + 1, win, 0, SYN | ACK, &o, 0);
    g_ncap = 0;
    return id;
}

/* RFC 7323 §2.2 again: the SYN-ACK window was unscaled, so the peer's real
 * window is only known once an ordinary segment arrives. The sender tests all
 * start from a settled window -- otherwise the first "duplicate" ACK carries a
 * window CHANGE and RFC 5681 rightly refuses to count it. */
static void settle(uint16_t win)
{
    inject_full(0, conns[g_id].rcv_nxt, conns[g_id].snd_nxt, win, 0, ACK, NULL, 0);
    g_ncap = 0;
}

/* Count data-carrying segments captured since index `from`, and their bytes. */
static int count_data(int from, int *bytes)
{
    int n = 0, b = 0;
    for (int i = from; i < g_ncap; i++)
        if (g_cap[i].dlen > 0) { n++; b += g_cap[i].dlen; }
    if (bytes) *bytes = b;
    return n;
}

/* ---- the negative control --------------------------------------------- */
#ifdef TCP_NEGATIVE_CONTROL
/* Emulate a sender that does not respond to loss: put the congestion state
 * back exactly as it was before the third duplicate ACK. */
static void negctl(struct tcp_conn *c, uint32_t cwnd0, uint32_t ssth0)
{ c->cwnd = cwnd0; c->ssthresh = ssth0; c->in_recovery = 0; }
#else
static void negctl(struct tcp_conn *c, uint32_t cwnd0, uint32_t ssth0)
{ (void)c; (void)cwnd0; (void)ssth0; }
#endif

int main(void)
{
    uint32_t B = 1000;
    uint8_t sendbuf[70000];
    for (unsigned i = 0; i < sizeof sendbuf; i++) sendbuf[i] = (uint8_t)i;
    int before;

    /* ================================================================
     * PART 1 -- the receive path (unchanged behaviour; must stay green)
     * ================================================================ */

    /* 1) in-order */
    setup(B);
    inject(B, B+0, 4, ACK);
    inject(B, B+4, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+8, "in-order rcv_nxt=%u want %u", conns[0].rcv_nxt, B+8);
    CHECK(last()->ack == B+8, "in-order ack=%u want %u", last()->ack, B+8);
    drain_verify(8);

    /* 2) out-of-order: second segment first, then the first fills the hole */
    setup(B);
    inject(B, B+4, 4, ACK);
    CHECK(conns[0].rcv_nxt == B, "ooo: rcv_nxt must NOT advance over hole (%u)", conns[0].rcv_nxt);
    CHECK(last()->ack == B, "ooo: dup-ack should still point at the hole (%u)", last()->ack);
    CHECK(conns[0].n_ooo == 1, "ooo: one buffered interval (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B+0, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+8, "ooo: rcv_nxt jumps after fill (%u want %u)", conns[0].rcv_nxt, B+8);
    CHECK(conns[0].n_ooo == 0, "ooo: interval absorbed (n_ooo=%d)", conns[0].n_ooo);
    drain_verify(8);

    /* 3) three segments delivered middle, last, first */
    setup(B);
    inject(B, B+4, 4, ACK);
    inject(B, B+8, 4, ACK);
    CHECK(conns[0].n_ooo == 1, "3-seg: adjacent OOO merge into one (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B+0, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+12, "3-seg rcv_nxt=%u want %u", conns[0].rcv_nxt, B+12);
    drain_verify(12);

    /* 4) duplicate + overlap */
    setup(B);
    inject(B, B+0, 8, ACK);
    inject(B, B+0, 8, ACK);                 /* full duplicate */
    inject(B, B+4, 8, ACK);                 /* overlaps [4,8), adds [8,12) */
    CHECK(conns[0].rcv_nxt == B+12, "overlap rcv_nxt=%u want %u", conns[0].rcv_nxt, B+12);
    drain_verify(12);

    /* 5) beyond-window segment is clipped/dropped, no crash, no false advance */
    setup(B);
    inject(B, B + RXBUF + 100, 4, ACK);
    CHECK(conns[0].rcv_nxt == B, "beyond-window: rcv_nxt unchanged (%u)", conns[0].rcv_nxt);
    CHECK(conns[0].n_ooo == 0, "beyond-window: nothing buffered (n_ooo=%d)", conns[0].n_ooo);

    /* 6) THE BIG FLIGHT: 64 x 512B = 32 KiB delivered in REVERSE order. */
    setup(B);
    int NSEG = 64, SS = 512;
    for (int i = NSEG - 1; i >= 1; i--)
        inject(B, B + (uint32_t)(i * SS), SS, ACK);
    CHECK(conns[0].rcv_nxt == B, "flight: no advance until hole filled (%u)", conns[0].rcv_nxt);
    CHECK(conns[0].n_ooo == 1, "flight: reverse-adjacent merge to ONE interval (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B + 0, SS, ACK);
    CHECK(conns[0].rcv_nxt == B + (uint32_t)(NSEG * SS),
          "flight: rcv_nxt=%u want %u", conns[0].rcv_nxt, B + (uint32_t)(NSEG*SS));
    drain_verify(NSEG * SS);

    /* 7) multi-interval (non-adjacent) reassembly within NOOO */
    setup(B);
    for (int i = 0; i < 16; i += 2) inject(B, B + (uint32_t)(i*SS), SS, ACK);
    CHECK(conns[0].n_ooo == 7, "multi: 7 disjoint intervals (n_ooo=%d)", conns[0].n_ooo);
    for (int i = 1; i < 16; i += 2) inject(B, B + (uint32_t)(i*SS), SS, ACK);
    CHECK(conns[0].rcv_nxt == B + (uint32_t)(16*SS),
          "multi: rcv_nxt=%u want %u", conns[0].rcv_nxt, B + (uint32_t)(16*SS));
    CHECK(conns[0].n_ooo == 0, "multi: all absorbed (n_ooo=%d)", conns[0].n_ooo);
    drain_verify(16 * SS);

    /* 8) sequence wraparound: base near 2^32 so seq wraps mid-flight */
    uint32_t W = 0xFFFFFF00u;
    setup(W);
    inject(W, W + 256, 256, ACK);
    inject(W, W + 0, 256, ACK);
    CHECK(conns[0].rcv_nxt == W + 512, "wrap: rcv_nxt=%u want %u", conns[0].rcv_nxt, W + 512);
    drain_verify(512);

    /* 9) bad TCP checksum is discarded before it can advance receive state. */
    setup(B);
    inject_full(B, B, conns[0].snd_nxt, 65535, 4, ACK, NULL, 1);
    CHECK(conns[0].rcv_nxt == B && conns[0].rx_len == 0,
          "checksum: corrupt segment must be discarded");

    /* ================================================================
     * PART 2 -- option negotiation (RFC 7323 / RFC 2018)
     * ================================================================ */

    /* 10) our SYN offers MSS, SACK-permitted, timestamps and window scale, in
     *     that order, in exactly 20 option bytes. */
    {
        for (int i = 0; i < NCONN; i++) conns[i].used = 0;
        g_ncap = 0;
        int id = tcp_connect_start(RIP, RPORT);
        struct cap *syn = last();
        const uint8_t *v;
        CHECK(syn->hlen == 40, "SYN: 40-byte header (20 option bytes), got %d", syn->hlen);
        CHECK(cap_opt(syn, 2, &v) == 4 && ((v[0] << 8) | v[1]) == MSS_ADV,
              "SYN: advertises MSS=%d", MSS_ADV);
        CHECK(cap_opt(syn, 4, NULL) == 2, "SYN: offers SACK-permitted");
        CHECK(cap_opt(syn, 8, NULL) == 10, "SYN: offers timestamps");
        CHECK(cap_opt(syn, 3, &v) == 3 && v[0] == conns[id].rcv_scale,
              "SYN: offers window scale");
        /* The receive ring is 128 KiB, so the shift has to be 2 for the window
         * to be expressible at all -- that is the point of the option. */
        CHECK(conns[id].rcv_scale == 2, "SYN: rcv_scale=%u want 2", conns[id].rcv_scale);
        CHECK(syn->win <= 65535, "SYN: the SYN window is never scaled (%u)", syn->win);
        tcp_close(id); conns[id].used = 0;
    }

    /* 11) a peer that accepts everything -> scaling, SACK and timestamps on. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x11110000u);
        struct tcp_conn *c = &conns[id];
        CHECK(c->state == ESTABLISHED, "hs: established (state=%d)", c->state);
        CHECK(c->snd_scale == 7 && c->rcv_scale == 2,
              "hs: scales snd=%u rcv=%u want 7/2", c->snd_scale, c->rcv_scale);
        CHECK(c->sack_ok && c->ts_ok, "hs: sack=%d ts=%d", c->sack_ok, c->ts_ok);
        CHECK(c->snd_wnd == 65535, "hs: SYN-ACK window is unscaled (%u)", c->snd_wnd);
        /* Post-handshake windows scale: a 1000 in the header means 128000. */
        inject_ack(c->snd_nxt, 1000, NULL);
        CHECK(c->snd_wnd == 128000u, "hs: scaled peer window %u want 128000", c->snd_wnd);
        /* And ours: 128 KiB of ring advertised as 32768 units of 4 bytes. */
        uint32_t R = 0x11110000u + 1;               /* first data seq: SYN took one */
        inject(R, R, 100, ACK);
        inject(R, R + 100, 100, ACK);               /* the second forces an ACK */
        CHECK(recv_window(c) == RXBUF - 200,
              "hs: our real window %u want %u", recv_window(c), RXBUF - 200);
        CHECK(last()->win == (RXBUF - 200) >> 2,
              "hs: advertised %u want %u (scaled by 4)", last()->win, (RXBUF - 200) >> 2);
        CHECK(((uint32_t)last()->win << c->rcv_scale) == RXBUF - 200,
              "hs: the wire value reconstructs to the real window");
    }

    /* 12) a peer that REFUSES every option -> clean fallback to plain TCP. */
    {
        int id = handshake(-1, 0, 0, 1460, 65535, 0x22220000u);
        struct tcp_conn *c = &conns[id];
        CHECK(c->snd_scale == 0 && c->rcv_scale == 0,
              "refuse-ws: both scales must be 0 (snd=%u rcv=%u)", c->snd_scale, c->rcv_scale);
        CHECK(!c->sack_ok && !c->ts_ok,
              "refuse: sack=%d ts=%d must both be off", c->sack_ok, c->ts_ok);
        inject_ack(c->snd_nxt, 1000, NULL);
        CHECK(c->snd_wnd == 1000, "refuse-ws: peer window unscaled (%u)", c->snd_wnd);
        uint32_t R = 0x22220000u + 1;
        inject(R, R, 100, ACK);
        inject(R, R + 100, 100, ACK);
        CHECK(last()->win == 65535,
              "refuse-ws: window must be capped at 65535, got %u", last()->win);
        CHECK(last()->hlen == 20, "refuse: no options on our segments (hlen=%d)", last()->hlen);
        /* Our whole 128 KiB ring is still usable, we just cannot announce it. */
        CHECK(recv_window(c) == RXBUF - 200, "refuse-ws: ring still 128 KiB (%u)", recv_window(c));
    }

    /* 13) SACK, receiver side: holes are reported, most recent block first. */
    {
        uint32_t P = 0x33330000u;
        int id = handshake(7, 1, 1, 1460, 65535, P);
        struct tcp_conn *c = &conns[id];
        inject(P, P + 200, 100, ACK);                  /* hole at [P, P+200) */
        const uint8_t *v;
        int ol = cap_opt(last(), 5, &v);
        CHECK(ol == 10 && rd32_(v) == P + 200 && rd32_(v + 4) == P + 300,
              "sack-rx: one block [%u,%u) len=%d", ol ? rd32_(v) : 0,
              ol ? rd32_(v + 4) : 0, ol);
        inject(P, P + 500, 100, ACK);                  /* a second, newer hole */
        ol = cap_opt(last(), 5, &v);
        CHECK(ol == 18 && rd32_(v) == P + 500 && rd32_(v + 4) == P + 600,
              "sack-rx: newest block first (len=%d first=%u)", ol, ol ? rd32_(v) : 0);
        CHECK(ol == 18 && rd32_(v + 8) == P + 200,
              "sack-rx: older block second (%u)", ol >= 18 ? rd32_(v + 8) : 0);
        CHECK(c->n_ooo == 2, "sack-rx: two intervals held (%d)", c->n_ooo);
    }

    /* 14) SACK refused: holes produce plain duplicate ACKs, no SACK option. */
    {
        uint32_t P = 0x44440000u;
        int id = handshake(7, 0, 1, 1460, 65535, P);
        (void)id;
        inject(P, P + 200, 100, ACK);
        CHECK(cap_opt(last(), 5, NULL) == 0,
              "sack-refused: must not emit SACK blocks");
        CHECK(last()->ack == P + 1,
              "sack-refused: still a dup ACK at the hole (%u want %u)", last()->ack, P + 1);
    }

    /* 15) PAWS (RFC 7323 §5.3): an old duplicate timestamp is rejected. */
    {
        uint32_t P = 0x55550000u;
        int id = handshake(7, 1, 1, 1460, 65535, P);
        struct tcp_conn *c = &conns[id];
        CHECK(c->ts_recent == 500000u, "paws: ts_recent seeded (%u)", c->ts_recent);
        struct injopt o; memset(&o, 0, sizeof o);
        o.have_ts = 1; o.tsval = 500010; o.tsecr = (uint32_t)g_ticks;
        inject_full(P, P, c->snd_nxt, 65535, 100, ACK, &o, 0);
        CHECK(c->rcv_nxt == P + 100 && c->ts_recent == 500010u,
              "paws: fresh timestamp accepted (rcv_nxt=%u ts=%u)", c->rcv_nxt, c->ts_recent);
        g_ncap = 0;
        o.tsval = 400000;                       /* older than ts_recent */
        inject_full(P, P + 100, c->snd_nxt, 65535, 100, ACK, &o, 0);
        CHECK(c->rcv_nxt == P + 100, "paws: stale segment must not be taken (%u)", c->rcv_nxt);
        CHECK(g_ncap == 1 && (last()->flags & ACK) && last()->ack == P + 100,
              "paws: stale segment draws a corrective ACK (n=%d ack=%u)", g_ncap, last()->ack);
        CHECK(c->ts_recent == 500010u, "paws: ts_recent unchanged (%u)", c->ts_recent);
        /* Same segment with a fresh timestamp is accepted -- PAWS is not just
         * dropping everything. */
        o.tsval = 500020;
        inject_full(P, P + 100, c->snd_nxt, 65535, 100, ACK, &o, 0);
        CHECK(c->rcv_nxt == P + 200, "paws: fresh retry accepted (%u)", c->rcv_nxt);
    }

    /* 16) sequence wraparound WITH options: the 2^32 fold must not confuse
     *     PAWS, the SACK blocks or the reassembly set. */
    {
        uint32_t P = 0xFFFFFF00u;
        int id = handshake(7, 1, 1, 1460, 65535, P - 1);   /* peer ISN, +1 = P */
        struct tcp_conn *c = &conns[id];
        struct injopt o; memset(&o, 0, sizeof o);
        o.have_ts = 1; o.tsval = 500001; o.tsecr = (uint32_t)g_ticks;
        inject_full(P, P + 256, c->snd_nxt, 65535, 256, ACK, &o, 0);   /* wraps 2^32 */
        const uint8_t *v;
        CHECK(cap_opt(last(), 5, &v) == 10 && rd32_(v) == P + 256,
              "wrap+opts: SACK block across the fold (%u want %u)",
              cap_opt(last(), 5, &v) ? rd32_(v) : 0, P + 256);
        o.tsval = 500002;
        inject_full(P, P, c->snd_nxt, 65535, 256, ACK, &o, 0);
        CHECK(c->rcv_nxt == P + 512, "wrap+opts: rcv_nxt=%u want %u", c->rcv_nxt, P + 512);
        CHECK(c->n_ooo == 0, "wrap+opts: hole absorbed (%d)", c->n_ooo);
        drain_verify(512);
        /* PAWS across the timestamp fold: TSval wraps 2^32 too and the compare
         * is signed, so 0xFFFFFFF0 -> 0x00000010 is FORWARD, not backward. */
        c->ts_recent = 0xFFFFFFF0u;
        g_ncap = 0;
        o.tsval = 0x00000010u;
        inject_full(P, P + 512, c->snd_nxt, 65535, 4, ACK, &o, 0);
        CHECK(c->rcv_nxt == P + 516, "wrap+opts: TSval wrap is forward (%u)", c->rcv_nxt);
    }

    /* ================================================================
     * PART 3 -- the sender: congestion control against real loss
     * ================================================================ */

    /* 17) slow start is exponential and the initial window is RFC 6928's. */
    uint32_t MSS;
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x66660000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        MSS = eff_mss(c);
        CHECK(MSS == 1448, "cc: eff MSS %u want 1448 (1460 - 12 timestamp bytes)", MSS);
        CHECK(c->cwnd == 10 * MSS && c->ssthresh == 0xFFFFFFFFu,
              "cc: IW=%u want %u, ssthresh=%u", c->cwnd, 10 * MSS, c->ssthresh);
        uint32_t base = c->snd_una;
        int n = tcp_send_nb(id, sendbuf, 32768);
        CHECK(n == 32768, "cc: 32 KiB accepted into the send ring (%d)", n);
        int bytes;
        int segs = count_data(0, &bytes);
        CHECK(segs == 10 && bytes == (int)(10 * MSS),
              "cc: initial burst %d segs / %d bytes, want 10 / %u", segs, bytes, 10 * MSS);
        CHECK(c->snd_max == base + 10 * MSS, "cc: snd_max=%u", c->snd_max - base);

        /* Slow start: each ACK of one segment opens the window by one segment,
         * so acknowledging one segment releases TWO. */
        g_ncap = 0;
        inject_ack(base + MSS, 65535, NULL);
        CHECK(c->cwnd == 11 * MSS, "cc: slow start cwnd=%u want %u", c->cwnd, 11 * MSS);
        segs = count_data(0, &bytes);
        CHECK(segs == 2 && bytes == (int)(2 * MSS),
              "cc: one ACK releases two segments (%d segs)", segs);
        g_ncap = 0;
        inject_ack(base + 2 * MSS, 65535, NULL);
        CHECK(c->cwnd == 12 * MSS, "cc: cwnd=%u want %u", c->cwnd, 12 * MSS);
        CHECK(count_data(0, NULL) == 2, "cc: and two more segments");

        /* Congestion avoidance: once cwnd >= ssthresh the growth is one MSS
         * per ROUND TRIP, not per ACK. */
        c->ssthresh = c->cwnd;                     /* force the transition */
        uint32_t cw = c->cwnd;
        c->ca_acked = 0;
        for (uint32_t acked = 0; acked < cw; acked += MSS)
            inject_ack(c->snd_una + MSS, 65535, NULL);
        CHECK(c->cwnd == cw + MSS,
              "cc: congestion avoidance +1 MSS per RTT (cwnd=%u want %u)", c->cwnd, cw + MSS);
    }

    /* 18) LOSS. Three duplicate ACKs -> fast retransmit + fast recovery, with
     *     the exact RFC 5681 §3.2 numbers, then a full ACK deflates. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x77770000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        uint32_t flight = c->snd_max - c->snd_una;
        CHECK(flight == 10 * MSS, "loss: 10 segments in flight (%u)", flight);
        uint32_t cwnd0 = c->cwnd, ssth0 = c->ssthresh;

        g_ncap = 0;
        inject_ack(base, 65535, NULL);          /* dup 1 */
        inject_ack(base, 65535, NULL);          /* dup 2 */
        CHECK(c->dup_acks == 2 && count_data(0, NULL) == 0,
              "loss: two dup ACKs do not retransmit (dups=%d sends=%d)",
              c->dup_acks, count_data(0, NULL));
        CHECK(c->cwnd == cwnd0, "loss: cwnd untouched before the third dup (%u)", c->cwnd);

        inject_ack(base, 65535, NULL);          /* dup 3 -> fast retransmit */
        negctl(c, cwnd0, ssth0);                /* the negative control bites here */

        uint32_t want_ssth = flight / 2;
        if (want_ssth < 2 * MSS) want_ssth = 2 * MSS;
        CHECK(c->ssthresh == want_ssth,
              "loss: ssthresh = FlightSize/2 = %u, got %u", want_ssth, c->ssthresh);
        CHECK(c->cwnd == want_ssth + 3 * MSS,
              "loss: cwnd = ssthresh + 3*MSS = %u, got %u", want_ssth + 3 * MSS, c->cwnd);
        CHECK(c->cwnd < cwnd0, "loss: the window must SHRINK (%u -> %u)", cwnd0, c->cwnd);
        CHECK(c->in_recovery && c->recover == c->snd_max,
              "loss: in fast recovery, recover=%u snd_max=%u", c->recover, c->snd_max);
        CHECK(g_ncap >= 1 && last()->seq == base && last()->dlen == (int)MSS,
              "loss: the lost segment is retransmitted at once (seq=%u len=%d)",
              g_ncap ? last()->seq - base : 0, g_ncap ? last()->dlen : -1);
        CHECK(c->n_fast_rexmit == 1, "loss: one fast-retransmit event (%u)", c->n_fast_rexmit);

        /* Each further duplicate inflates by one segment (RFC 5681 step 4). */
        uint32_t cw = c->cwnd;
        inject_ack(base, 65535, NULL);
        CHECK(c->cwnd == cw + MSS, "loss: dup 4 inflates cwnd (%u want %u)", c->cwnd, cw + MSS);

        /* A full ACK (>= recover) ends recovery and deflates to ssthresh. */
        uint32_t recover = c->recover;
        inject_ack(recover, 65535, NULL);
        CHECK(!c->in_recovery, "loss: full ACK leaves fast recovery");
        CHECK(c->cwnd == want_ssth,
              "loss: deflate to ssthresh %u, got %u", want_ssth, c->cwnd);
    }

    /* 19) NewReno partial ACK (RFC 6582): a second hole is retransmitted
     *     immediately and recovery is NOT exited. */
    {
        int id = handshake(7, 0, 1, 1460, 65535, 0x88880000u);   /* SACK off */
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        inject_ack(base, 65535, NULL);
        inject_ack(base, 65535, NULL);
        inject_ack(base, 65535, NULL);
        CHECK(c->in_recovery, "newreno: entered recovery");
        g_ncap = 0;
        inject_ack(base + 2 * MSS, 65535, NULL);    /* partial: recover is higher */
        CHECK(c->in_recovery, "newreno: partial ACK stays in recovery");
        CHECK(g_ncap >= 1 && g_cap[0].seq == base + 2 * MSS && g_cap[0].dlen == (int)MSS,
              "newreno: partial ACK retransmits the new head (seq=+%u len=%d)",
              g_ncap ? g_cap[0].seq - base : 0, g_ncap ? g_cap[0].dlen : -1);
        CHECK(c->n_fast_rexmit == 1,
              "newreno: a partial ACK is not a new loss event (%u)", c->n_fast_rexmit);
    }

    /* 20) SACK on the sender: the scoreboard stops us resending data the peer
     *     already holds. The same blocks with SACK unnegotiated are ignored. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x99990000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        struct injopt o; memset(&o, 0, sizeof o);
        o.have_ts = 1; o.tsval = 500100; o.tsecr = (uint32_t)g_ticks;
        o.n_sack = 1; o.sack[0][0] = base + 100; o.sack[0][1] = base + 4 * MSS;
        g_ncap = 0;
        inject_ack(base, 65535, &o);
        o.tsval++; inject_ack(base, 65535, &o);
        o.tsval++; inject_ack(base, 65535, &o);
        CHECK(c->n_sacked == 1 && c->sacked[0].seq == base + 100,
              "sack-tx: scoreboard holds one block (n=%d)", c->n_sacked);
        CHECK(sacked_bytes(c) == 4 * MSS - 100,
              "sack-tx: %u SACKed bytes", sacked_bytes(c));
        CHECK(g_ncap >= 1 && g_cap[0].seq == base && g_cap[0].dlen == 100,
              "sack-tx: retransmit stops at the SACK edge (len=%d want 100)",
              g_ncap ? g_cap[0].dlen : -1);
    }
    {
        int id = handshake(7, 0, 1, 1460, 65535, 0xAAAA0000u);   /* SACK refused */
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        struct injopt o; memset(&o, 0, sizeof o);
        o.have_ts = 1; o.tsval = 500100; o.tsecr = (uint32_t)g_ticks;
        o.n_sack = 1; o.sack[0][0] = base + 100; o.sack[0][1] = base + 4 * MSS;
        g_ncap = 0;
        inject_ack(base, 65535, &o);
        o.tsval++; inject_ack(base, 65535, &o);
        o.tsval++; inject_ack(base, 65535, &o);
        CHECK(c->n_sacked == 0,
              "sack-refused: unnegotiated blocks are ignored (n=%d)", c->n_sacked);
        CHECK(g_ncap >= 1 && g_cap[0].seq == base && g_cap[0].dlen == (int)MSS,
              "sack-refused: retransmit a whole segment (len=%d want %u)",
              g_ncap ? g_cap[0].dlen : -1, MSS);
    }

    /* 21) RTO collapses the window to one segment and retransmits from
     *     snd_una; the RTO itself doubles (RFC 6298 §5.5). */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0xBBBB0000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        uint32_t flight = c->snd_max - c->snd_una;
        uint64_t rto0 = c->rto ? c->rto : RTO_DEFAULT;
        g_ncap = 0;
        g_ticks += rto0 + 1;
        tcp_poll();
        uint32_t want_ssth = flight / 2; if (want_ssth < 2 * MSS) want_ssth = 2 * MSS;
        CHECK(c->cwnd == MSS, "rto: cwnd collapses to one segment (%u want %u)", c->cwnd, MSS);
        CHECK(c->ssthresh == want_ssth, "rto: ssthresh=%u want %u", c->ssthresh, want_ssth);
        CHECK(c->rto == rto0 * 2, "rto: backoff %llu -> %llu",
              (unsigned long long)rto0, (unsigned long long)c->rto);
        CHECK(g_ncap >= 1 && g_cap[0].seq == base && g_cap[0].dlen == (int)MSS,
              "rto: retransmit starts at snd_una (seq=+%u len=%d)",
              g_ncap ? g_cap[0].seq - base : 0, g_ncap ? g_cap[0].dlen : -1);
        CHECK(count_data(0, NULL) == 1,
              "rto: exactly one segment goes out under the collapsed window (%d)",
              count_data(0, NULL));
        CHECK(c->n_rto == 1, "rto: one timeout event (%u)", c->n_rto);
        /* And it recovers: ACK everything, slow start restarts from cwnd=MSS. */
        g_ncap = 0;
        inject_ack(base + MSS, 65535, NULL);
        CHECK(c->cwnd == 2 * MSS, "rto: slow start restarts (cwnd=%u want %u)",
              c->cwnd, 2 * MSS);
    }

    /* 22) THE WHOLE TRAJECTORY, printed, so the shape is visible and not just
     *     asserted: slow start, loss, recovery, congestion avoidance. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0xCCCC0000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        uint32_t traj[24]; int nt = 0;
        traj[nt++] = c->cwnd;
        for (int i = 1; i <= 5 && nt < 24; i++) {
            inject_ack(base + (uint32_t)i * MSS, 65535, NULL);
            traj[nt++] = c->cwnd;
        }
        uint32_t peak = c->cwnd;
        uint32_t una = c->snd_una;
        for (int i = 0; i < 3; i++) inject_ack(una, 65535, NULL);
        traj[nt++] = c->cwnd;
        int shrank = c->cwnd < peak;
        inject_ack(c->recover, 65535, NULL);
        traj[nt++] = c->cwnd;
        printf("  cwnd trajectory (MSS=%u):", MSS);
        for (int i = 0; i < nt; i++) printf(" %u", traj[i]);
        printf("\n");
        CHECK(shrank, "trajectory: loss must reduce cwnd (peak %u -> %u)", peak, c->cwnd);
        CHECK(traj[1] > traj[0] && traj[2] > traj[1],
              "trajectory: slow start must grow (%u %u %u)", traj[0], traj[1], traj[2]);
    }

    /* ================================================================
     * PART 4 -- Nagle, delayed ACK, PMTU
     * ================================================================ */

    /* 23) Nagle (RFC 896) holds a small write behind unacked data; the ACK
     *     releases it; TCP_NODELAY switches the whole thing off. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0xDDDD0000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        g_ncap = 0;
        tcp_send_nb(id, sendbuf, 100);
        CHECK(count_data(0, NULL) == 1 && last()->dlen == 100,
              "nagle: the first small write goes out at once (%d)", count_data(0, NULL));
        g_ncap = 0;
        tcp_send_nb(id, sendbuf, 100);
        CHECK(count_data(0, NULL) == 0, "nagle: the second is held (%d segs)",
              count_data(0, NULL));
        CHECK(c->snd_end == base + 200 && c->snd_nxt == base + 100,
              "nagle: it is queued, not lost (end=+%u nxt=+%u)",
              c->snd_end - base, c->snd_nxt - base);
        inject_ack(base + 100, 65535, NULL);
        CHECK(count_data(0, NULL) == 1 && last()->dlen == 100,
              "nagle: the ACK releases it (%d segs)", count_data(0, NULL));
    }
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0xDEDE0000u);
        g_id = id; settle(65535);
        tcp_set_nodelay(id, 1);
        g_ncap = 0;
        tcp_send_nb(id, sendbuf, 100);
        tcp_send_nb(id, sendbuf, 100);
        CHECK(count_data(0, NULL) == 2,
              "nodelay: both small writes go straight out (%d)", count_data(0, NULL));
    }
    {   /* Nagle must not hold the last bytes of a stream that is closing. */
        int id = handshake(7, 1, 1, 1460, 65535, 0xDFDF0000u);
        tcp_send_nb(id, sendbuf, 100);
        g_ncap = 0;
        tcp_send_nb(id, sendbuf, 50);
        CHECK(count_data(0, NULL) == 0, "nagle+close: held before the close");
        tcp_close(id);
        CHECK(count_data(0, NULL) >= 1 && (last()->flags & FIN),
              "nagle+close: the close flushes it (segs=%d flags=%02x)",
              count_data(0, NULL), last()->flags);
    }

    /* 24) Delayed ACK (RFC 1122 4.2.3.2): one segment waits, two do not, and
     *     out-of-order data is acknowledged at once. */
    {
        uint32_t P = 0xEEEE0000u;
        int id = handshake(7, 1, 1, 1460, 65535, P);
        struct tcp_conn *c = &conns[id];
        g_ncap = 0;
        inject(P, P, 100, ACK);
        CHECK(g_ncap == 0 && c->ack_due != 0,
              "delack: a single segment is not acknowledged at once (sends=%d)", g_ncap);
        uint64_t due = c->ack_due;
        g_ticks = due - 1; tcp_poll();
        CHECK(g_ncap == 0, "delack: nothing before the deadline (%d)", g_ncap);
        g_ticks = due; tcp_poll();
        CHECK(g_ncap == 1 && last()->ack == P + 100,
              "delack: the timer fires (%d sends, ack=%u)", g_ncap, last()->ack);
        CHECK(c->ack_due == 0, "delack: deadline cleared");
        g_ncap = 0;
        inject(P, P + 100, 100, ACK);
        inject(P, P + 200, 100, ACK);
        CHECK(g_ncap == 1 && last()->ack == P + 300,
              "delack: every second segment is acknowledged (%d)", g_ncap);
        g_ncap = 0;
        inject(P, P + 400, 100, ACK);       /* a hole */
        CHECK(g_ncap == 1 && last()->ack == P + 300,
              "delack: out-of-order data is acknowledged immediately (%d)", g_ncap);
    }

    /* 25) Path MTU discovery (RFC 1191): ICMP "fragmentation needed" must
     *     LOWER the segment size, not kill the connection -- and the blackhole
     *     case (the ICMP never arrives) is caught by the retransmit timer. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x12120000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        tcp_send_nb(id, sendbuf, 32768);
        g_ncap = 0;
        tcp_error(c->lport, RIP, RPORT, 3, 4);      /* frag needed, DF set */
        CHECK(c->used && c->state == ESTABLISHED,
              "pmtu: frag-needed must NOT abort (state=%d used=%d)", c->state, c->used);
        CHECK(c->pmtu_mss == 1400, "pmtu: stepped down to %u want 1400", c->pmtu_mss);
        CHECK(eff_mss(c) == 1388, "pmtu: eff MSS %u want 1388", eff_mss(c));
        CHECK(g_ncap >= 1 && g_cap[0].seq == base && g_cap[0].dlen == 1388,
              "pmtu: resend from snd_una at the new size (len=%d)",
              g_ncap ? g_cap[0].dlen : -1);
        /* The other unreachable codes still abort. */
        tcp_error(c->lport, RIP, RPORT, 3, 1);
        CHECK(c->state == CLOSED, "pmtu: host-unreachable is still fatal (%d)", c->state);
    }
    {   /* blackhole: no ICMP at all, three timeouts drop to a 508-byte MSS */
        int id = handshake(7, 1, 1, 1460, 65535, 0x13130000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        tcp_send_nb(id, sendbuf, 32768);
        for (int i = 0; i < 3; i++) {
            g_ticks += (c->rto ? c->rto : RTO_DEFAULT) + 1;
            tcp_poll();
        }
        CHECK(c->rtx_retries == 3 && c->pmtu_mss == 508,
              "blackhole: 3 timeouts -> MSS %u want 508 (retries=%d)",
              c->pmtu_mss, c->rtx_retries);
        CHECK(c->used, "blackhole: connection still alive");
    }

    /* ================================================================
     * PART 5 -- everything that was already green must stay green
     * ================================================================ */

    /* 26) newer window updates win; a reordered older update cannot shrink it. */
    setup(B);
    inject_full(B, B + 2, conns[0].snd_nxt, 1000, 0, ACK, NULL, 0);
    CHECK(conns[0].snd_wnd == 1000, "window: accepted newer update (%u)", conns[0].snd_wnd);
    inject_full(B, B + 1, conns[0].snd_nxt, 5, 0, ACK, NULL, 0);
    CHECK(conns[0].snd_wnd == 1000, "window: ignored reordered shrink (%u)", conns[0].snd_wnd);

    /* 27) in-window non-exact RST gets a challenge ACK; exact RCV.NXT resets. */
    setup(B);
    before = g_ncap;
    inject_full(B, B + 1, 0, 0, 0, RST, NULL, 0);
    CHECK(conns[0].used && g_ncap == before + 1 && (last()->flags & ACK),
          "RST: in-window non-exact reset challenged");
    inject_full(B, B, 0, 0, 0, RST, NULL, 0);
    CHECK(!conns[0].used, "RST: exact RCV.NXT reset accepted");

    /* 28) peer MSS and window cap a segment; a zero window enters persist mode
     *     rather than killing the connection. */
    setup(B);
    conns[0].peer_mss = 200; conns[0].snd_wnd = 300;
    {
        g_ncap = 0;
        int ns = tcp_send_nb(0, sendbuf, 500);
        CHECK(ns == 500, "send cap: all 500 bytes buffered (%d)", ns);
        CHECK(count_data(0, NULL) == 1 && last()->dlen == 200,
              "send cap: one 200-byte segment on the wire (%d segs, len=%d)",
              count_data(0, NULL), last()->dlen);
    }
    setup(B);
    conns[0].snd_wnd = 0;
    {
        int ns = tcp_send_nb(0, sendbuf, 1);
        CHECK(ns == 1 && conns[0].snd_end == conns[0].snd_una + 1,
              "zero window: the byte is queued, not sent (%d)", ns);
        g_ncap = 0;
        for (int i = 0; i < 12; i++) { tcp_poll(); g_ticks += 2000; }
        CHECK(conns[0].used && conns[0].persist_running,
              "zero window: persist probes do not abort the connection");
        CHECK(g_ncap >= 3 && last()->dlen == 1,
              "zero window: one-byte probes went out (%d)", g_ncap);
        /* Reopening the window releases the data. */
        g_ncap = 0;
        inject_full(B, B, conns[0].snd_una, 4096, 0, ACK, NULL, 0);
        CHECK(!conns[0].persist_running, "zero window: persist stops when it reopens");
    }

    /* 29) close: the FIN takes a sequence AFTER everything queued, so it can
     *     never overtake or overwrite unacknowledged payload. */
    setup(B);
    {
        struct tcp_conn *c = &conns[0];
        uint32_t base = c->snd_una;
        c->snd_wnd = 5;                       /* only 5 of the 10 bytes fit */
        tcp_send_nb(0, sendbuf, 10);
        g_ncap = 0;
        tcp_close(0);
        CHECK(c->fin_queued && c->state == ESTABLISHED && count_data(0, NULL) == 0,
              "close: FIN waits behind the window (state=%d)", c->state);
        inject_full(B, B, base + 5, 65535, 0, ACK, NULL, 0);   /* ack 5, open window */
        CHECK(c->state == FIN_WAIT, "close: FIN goes after the rest (state=%d)", c->state);
        CHECK((last()->flags & FIN) && last()->seq + (uint32_t)last()->dlen == base + 10,
              "close: FIN sits at snd_end (+%u want +10)",
              last()->seq + (uint32_t)last()->dlen - base);
    }

    /* 30) RFC 793: a segment that matches no connection gets a RST. */
    setup(B);
    before = g_ncap;
    g_lport = 9999;
    inject_full(B, 100, 0x12345678u, 0, 0, ACK, NULL, 0);
    CHECK(g_ncap == before + 1 && last()->flags == RST &&
          last()->seq == 0x12345678u && last()->hlen == 20,
          "no-conn ACK: RST seq=%08x flags=%02x", last()->seq, last()->flags);
    inject_full(B, 100, 0, 0, 10, 0, NULL, 0);
    CHECK(g_ncap == before + 2 && last()->flags == (RST | ACK) &&
          last()->seq == 0 && last()->ack == 110,
          "no-conn data: RST+ACK ack=%u flags=%02x", last()->ack, last()->flags);
    inject_full(B, 100, 0, 0, 0, SYN, NULL, 0);
    CHECK(last()->flags == (RST | ACK) && last()->ack == 101,
          "no-conn SYN: RST+ACK ack=%u want 101", last()->ack);
    inject_full(B, 100, 0, 0, 0, RST, NULL, 0);
    CHECK(g_ncap == before + 3, "no-conn RST must beget silence (%d)", g_ncap);
    inject_full(B, 100, 0x12345678u, 0, 4, ACK, NULL, 1);
    CHECK(g_ncap == before + 3, "corrupt segment must not draw a RST");
    CHECK(conns[0].used && conns[0].state == ESTABLISHED,
          "no-conn RSTs disturbed the real connection");
    g_lport = LPORT;

    /* 31) tcp_error: ICMP hard errors abort; soft ones and other tuples do not. */
    setup(B);
    tcp_error(LPORT, RIP, RPORT, 11, 0);        /* time exceeded */
    CHECK(conns[0].state == ESTABLISHED, "tcp_error: time-exceeded must be ignored");
    tcp_error(LPORT, RIP, RPORT, 3, 2);         /* protocol unreachable: soft */
    CHECK(conns[0].state == ESTABLISHED, "tcp_error: soft code must be ignored");
    tcp_error(9999, RIP, RPORT, 3, 3);          /* no matching connection */
    CHECK(conns[0].state == ESTABLISHED, "tcp_error: wrong tuple must not match");
    tcp_error(LPORT, RIP, RPORT, 3, 3);         /* port unreachable: hard */
    CHECK(conns[0].state == CLOSED && conns[0].used,
          "tcp_error: hard error must close (state=%d used=%d)",
          conns[0].state, conns[0].used);
    {
        uint8_t tmp[8];
        CHECK(tcp_recv(0, tmp, sizeof tmp) == -1 && !conns[0].used,
              "tcp_error: tcp_recv must report the error and free the slot");
    }
    {   /* SYN_SENT has no reader: the slot is freed so tcp_connect fails fast */
        struct tcp_conn *c = &conns[0];
        memset(c, 0, sizeof *c);
        c->used = 1; c->state = SYN_SENT;
        c->lport = LPORT; c->rport = RPORT;
        addr_v4(&c->raddr, RIP); addr_v4(&c->laddr, net_cfg.ip);
        tcp_error(LPORT, RIP, RPORT, 3, 1);     /* host unreachable: hard */
        CHECK(!c->used, "tcp_error: SYN_SENT abort must free the slot");
    }

    /* 32) RFC 6298 RTT estimation, now driven by RFC 7323 timestamps. */
    {
        int id = handshake(7, 1, 1, 1460, 65535, 0x14140000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        c->srtt = c->rttvar = 0;
        tcp_send_nb(id, sendbuf, 1000);
        struct injopt o; memset(&o, 0, sizeof o);
        o.have_ts = 1; o.tsval = 600000; o.tsecr = (uint32_t)g_ticks;
        g_ticks += 30;
        inject_full(0, c->rcv_nxt, base + 1000, 65535, 0, ACK, &o, 0);
        CHECK(c->srtt == 30 && c->rttvar == 15 && c->rto == 90,
              "rtt: timestamp sample srtt=%llu rttvar=%llu rto=%llu (want 30/15/90)",
              (unsigned long long)c->srtt, (unsigned long long)c->rttvar,
              (unsigned long long)c->rto);
        /* Upper clamp. */
        c->srtt = RTO_MAX; c->rttvar = RTO_MAX; c->rto = base_rto(c);
        CHECK(c->rto == RTO_MAX, "rtt: clamped at RTO_MAX (%llu)", (unsigned long long)c->rto);
        c->srtt = 1; c->rttvar = 0; c->rto = base_rto(c);
        CHECK(c->rto == RTO_MIN, "rtt: clamped at RTO_MIN (%llu)", (unsigned long long)c->rto);
    }
    {   /* No timestamps: one segment per window is timed, Karn still applies. */
        int id = handshake(7, 1, 0, 1460, 65535, 0x15150000u);
        struct tcp_conn *c = &conns[id];
        settle(65535);
        uint32_t base = c->snd_una;
        c->srtt = c->rttvar = 0;
        CHECK(!c->ts_ok && eff_mss(c) == 1460,
              "rtt-nots: no option bytes, full MSS (%u)", eff_mss(c));
        tcp_send_nb(id, sendbuf, 1000);
        CHECK(c->rtt_pending && c->rtt_seq == base + 1000, "rtt-nots: timing one segment");
        g_ticks += 25;
        inject_ack(base + 1000, 65535, NULL);
        CHECK(c->srtt == 25, "rtt-nots: sample taken (srtt=%llu)", (unsigned long long)c->srtt);
        /* Karn: a retransmitted segment's ACK must not be sampled. */
        tcp_send_nb(id, sendbuf, 1000);
        g_ticks += (c->rto ? c->rto : RTO_DEFAULT) + 1;
        tcp_poll();
        CHECK(!c->rtt_pending, "rtt-nots: Karn cancels the pending sample");
        g_ticks += 500;
        inject_ack(c->snd_max, 65535, NULL);
        CHECK(c->srtt == 25, "rtt-nots: no sample from a retransmit (srtt=%llu)",
              (unsigned long long)c->srtt);
    }

    /* 33) Passive close: peer FIN -> CLOSE_WAIT -> drain/EOF -> tcp_close ->
     *     LAST_ACK -> our FIN acked -> slot released. */
    setup(B);
    {
        struct tcp_conn *c = &conns[0];
        inject(B, B, 8, ACK);
        inject(B, B + 8, 0, FIN | ACK);
        CHECK(c->peer_fin && c->state == CLOSE_WAIT && c->rcv_nxt == B + 9,
              "pclose: peer FIN -> CLOSE_WAIT (state=%d rcv_nxt=%u)", c->state, c->rcv_nxt);
        drain_verify(8);
        CHECK(tcp_recv(0, sendbuf, 100) == -1 && c->used,
              "pclose: drained CLOSE_WAIT reports EOF, slot kept");
        tcp_close(0);
        CHECK(c->state == LAST_ACK && (last()->flags & FIN),
              "pclose: tcp_close sends our FIN -> LAST_ACK (state=%d)", c->state);
        inject_full(B, B + 9, c->snd_nxt, 65535, 0, ACK, NULL, 0);
        CHECK(!c->used, "pclose: FIN acked -> slot released (state=%d used=%d)",
              c->state, c->used);
    }

    /* 34) Active close: FIN_WAIT -> peer FIN -> TIME_WAIT -> timeout. */
    setup(B);
    {
        struct tcp_conn *c = &conns[0];
        tcp_close(0);
        CHECK(c->state == FIN_WAIT && c->fin_sent, "aclose: our FIN in flight -> FIN_WAIT");
        inject_full(B, B, c->snd_nxt, 65535, 0, ACK, NULL, 0);   /* ack our FIN */
        CHECK(c->state == FIN_WAIT && !c->rtx_running,
              "aclose: FIN acked, still FIN_WAIT (state=%d)", c->state);
        inject(B, B, 0, FIN | ACK);                              /* peer FIN */
        CHECK(c->state == TIME_WAIT && c->peer_fin && (last()->flags & ACK),
              "aclose: peer FIN -> TIME_WAIT (state=%d)", c->state);
        CHECK(!tcp_alive(0) && c->used, "aclose: TIME_WAIT is not alive but keeps the slot");
        before = g_ncap;
        inject(B, B, 0, FIN | ACK);
        CHECK(g_ncap == before + 1 && (last()->flags & ACK) && last()->ack == B + 1,
              "aclose: duplicate FIN re-ACKed (sends=%d ack=%u)", g_ncap, last()->ack);
        uint64_t t0 = g_ticks;
        g_ticks = t0 + 999; tcp_poll();
        CHECK(c->used, "aclose: TIME_WAIT held before 1000 ticks");
        g_ticks = t0 + 1001; tcp_poll();
        CHECK(!c->used, "aclose: TIME_WAIT frees the slot at the deadline");
    }

    /* 35) Simultaneous close -> CLOSING -> TIME_WAIT; plus the FIN_WAIT
     *     silence backstop. */
    setup(B);
    {
        struct tcp_conn *c = &conns[0];
        tcp_close(0);
        inject_full(B, B, c->snd_una, 65535, 0, FIN | ACK, NULL, 0);
        CHECK(c->state == CLOSING && c->peer_fin,
              "sclose: crossing FINs -> CLOSING (state=%d)", c->state);
        inject_full(B, B + 1, c->snd_nxt, 65535, 0, ACK, NULL, 0);
        CHECK(c->state == TIME_WAIT, "sclose: acked in CLOSING -> TIME_WAIT");
        g_ticks += 1001; tcp_poll();
        CHECK(!c->used, "sclose: TIME_WAIT frees the slot");
    }
    setup(B);
    {
        struct tcp_conn *c = &conns[0];
        tcp_close(0);
        inject_full(B, B, c->snd_nxt, 65535, 0, ACK, NULL, 0);   /* our FIN acked */
        g_ticks += 500;
        tcp_poll();
        CHECK(!c->used, "backstop: silent FIN_WAIT frees the slot");
    }

    /* 36) A window-scaled receive window really is bigger than 64 KiB -- the
     *     whole point of RFC 7323 for a downloading client. */
    {
        uint32_t P = 0x16160000u;
        int id = handshake(7, 1, 1, 1460, 65535, P);
        struct tcp_conn *c = &conns[id];
        CHECK(recv_window(c) == 131072u,
              "wscale: usable receive window %u want 131072", recv_window(c));
        CHECK(((uint32_t)wire_window(c) << c->rcv_scale) == 131072u,
              "wscale: the wire value reconstructs to %u",
              (uint32_t)wire_window(c) << c->rcv_scale);
        /* 80 KiB in flight, which a 65535 window could not have permitted. */
        uint32_t R = P + 1;
        for (int i = 0; i < 80; i++)
            inject(R, R + (uint32_t)i * 1024u, 1024, ACK);
        CHECK(c->rcv_nxt == R + 80u * 1024u,
              "wscale: 80 KiB accepted in one window (%u)", c->rcv_nxt - R);
        drain_verify(80 * 1024);
    }

    printf("\nTCP protocol tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
