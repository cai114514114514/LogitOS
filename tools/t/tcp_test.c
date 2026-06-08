/* Host unit test for the TCP out-of-order reassembly (white-box: #includes
 * tcp.c so it can drive tcp_input and inspect conns[] directly). Stubs the
 * kernel deps (ip_send captures the ACK we emit; timer is a virtual counter;
 * net_lock/poll are no-ops on the host). Build via `make test-tcp-host`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "net.h"     /* stub (tools/t/tcpstub): struct net_config, htons, net_lock... */

/* ---- kernel-dependency stubs (resolve tcp.c's externs) ---- */
struct net_config net_cfg = { 0x0A00020F };     /* 10.0.2.15 */
static uint32_t g_last_ack, g_last_win;
static uint8_t  g_last_flags;
static int      g_sends;
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    (void)dst; (void)proto;
    const uint8_t *p = payload;
    if (len >= 20) {
        g_last_ack = (uint32_t)p[8]<<24 | (uint32_t)p[9]<<16 | (uint32_t)p[10]<<8 | p[11];
        g_last_flags = p[13];
        g_last_win = (uint32_t)p[14]<<8 | p[15];
        g_sends++;
    }
    return 0;
}
static uint64_t g_ticks = 1;
uint64_t timer_ticks(void) { return g_ticks; }
void net_poll(void) {}
void net_idle(void) {}

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

/* Reset conns[0] into ESTABLISHED with the given initial receive sequence. */
static void setup(uint32_t isn)
{
    struct tcp_conn *c = &conns[0];
    memset(c, 0, sizeof *c);
    c->used = 1; c->state = ESTABLISHED;
    c->lport = LPORT; c->rport = RPORT; c->rip = RIP;
    c->rcv_nxt = c->read_seq = isn;
    c->snd_una = c->snd_nxt = 0xC0DE0000u;
}

/* Inject a data segment [seq, seq+n): payload byte i = pat(seq_off + i) where
 * seq_off is the stream offset (seq - base). base passed so pattern is absolute. */
static void inject(uint32_t base, uint32_t seq, int n, uint8_t flags)
{
    uint8_t buf[20 + 2048];
    memset(buf, 0, 20);
    buf[0] = (RPORT >> 8); buf[1] = RPORT & 0xFF;       /* sport = remote */
    buf[2] = (LPORT >> 8); buf[3] = LPORT & 0xFF;       /* dport = local  */
    buf[4]=seq>>24; buf[5]=seq>>16; buf[6]=seq>>8; buf[7]=seq;   /* seq */
    /* ack = 0 */
    buf[12] = (5 << 4);                                  /* data offset = 5 words */
    buf[13] = flags;
    for (int i = 0; i < n; i++) buf[20 + i] = pat((seq - base) + (uint32_t)i);
    tcp_input(RIP, buf, (uint16_t)(20 + n));
}

/* Drain all contiguous bytes; verify each equals its pattern. Returns total. */
static int drain_verify(uint32_t base, int expect_total)
{
    (void)base;                 /* pattern is absolute (offset from 0) */
    uint8_t out[200000];
    int total = 0, r;
    while ((r = tcp_recv(0, out + total, (int)sizeof out - total)) > 0) total += r;
    int ok = (total == expect_total);
    for (int i = 0; i < total; i++) if (out[i] != pat((uint32_t)i)) { ok = 0; break; }
    CHECK(ok, "drain: got %d bytes (want %d), content %s",
          total, expect_total, ok ? "ok" : "MISMATCH");
    return total;
}

int main(void)
{
    uint32_t B = 1000;

    /* 1) in-order */
    setup(B);
    inject(B, B+0, 4, ACK);
    inject(B, B+4, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+8, "in-order rcv_nxt=%u want %u", conns[0].rcv_nxt, B+8);
    CHECK(g_last_ack == B+8, "in-order ack=%u want %u", g_last_ack, B+8);
    drain_verify(B, 8);

    /* 2) out-of-order: second segment first, then the first fills the hole */
    setup(B);
    inject(B, B+4, 4, ACK);
    CHECK(conns[0].rcv_nxt == B, "ooo: rcv_nxt must NOT advance over hole (%u)", conns[0].rcv_nxt);
    CHECK(g_last_ack == B, "ooo: dup-ack should still point at the hole (%u)", g_last_ack);
    CHECK(conns[0].n_ooo == 1, "ooo: one buffered interval (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B+0, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+8, "ooo: rcv_nxt jumps after fill (%u want %u)", conns[0].rcv_nxt, B+8);
    CHECK(conns[0].n_ooo == 0, "ooo: interval absorbed (n_ooo=%d)", conns[0].n_ooo);
    drain_verify(B, 8);

    /* 3) three segments delivered middle, last, first */
    setup(B);
    inject(B, B+4, 4, ACK);
    inject(B, B+8, 4, ACK);
    CHECK(conns[0].n_ooo == 1, "3-seg: adjacent OOO merge into one (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B+0, 4, ACK);
    CHECK(conns[0].rcv_nxt == B+12, "3-seg rcv_nxt=%u want %u", conns[0].rcv_nxt, B+12);
    drain_verify(B, 12);

    /* 4) duplicate + overlap: resend an already-received range, and an overlap */
    setup(B);
    inject(B, B+0, 8, ACK);
    inject(B, B+0, 8, ACK);                 /* full duplicate */
    inject(B, B+4, 8, ACK);                 /* overlaps [4,8) already have, adds [8,12) */
    CHECK(conns[0].rcv_nxt == B+12, "overlap rcv_nxt=%u want %u", conns[0].rcv_nxt, B+12);
    drain_verify(B, 12);

    /* 5) beyond-window segment is clipped/dropped, no crash, no false advance */
    setup(B);
    inject(B, B + RXBUF + 100, 4, ACK);     /* entirely past the window */
    CHECK(conns[0].rcv_nxt == B, "beyond-window: rcv_nxt unchanged (%u)", conns[0].rcv_nxt);
    CHECK(conns[0].n_ooo == 0, "beyond-window: nothing buffered (n_ooo=%d)", conns[0].n_ooo);

    /* 6) THE BIG FLIGHT: 64 segments x 512B = 32 KiB delivered in REVERSE order,
     *    then the first segment fills the hole -> full 32 KiB reassembled. This
     *    is exactly the large-TLS-flight scenario that used to fail. */
    setup(B);
    int NSEG = 64, SS = 512;
    for (int i = NSEG - 1; i >= 1; i--)
        inject(B, B + (uint32_t)(i * SS), SS, ACK);
    CHECK(conns[0].rcv_nxt == B, "flight: no advance until hole filled (%u)", conns[0].rcv_nxt);
    CHECK(conns[0].n_ooo == 1, "flight: reverse-adjacent merge to ONE interval (n_ooo=%d)", conns[0].n_ooo);
    inject(B, B + 0, SS, ACK);              /* fill the front */
    CHECK(conns[0].rcv_nxt == B + (uint32_t)(NSEG * SS),
          "flight: rcv_nxt=%u want %u", conns[0].rcv_nxt, B + (uint32_t)(NSEG*SS));
    drain_verify(B, NSEG * SS);

    /* 7) multi-interval (non-adjacent) reassembly within NOOO: even blocks first
     *    (block 0 is in-order -> 7 disjoint OOO holes), then the odd blocks fill
     *    them and the stream becomes fully contiguous. */
    setup(B);
    for (int i = 0; i < 16; i += 2) inject(B, B + (uint32_t)(i*SS), SS, ACK);
    CHECK(conns[0].n_ooo == 7, "multi: 7 disjoint intervals (block0 in-order) (n_ooo=%d)", conns[0].n_ooo);
    for (int i = 1; i < 16; i += 2) inject(B, B + (uint32_t)(i*SS), SS, ACK);
    CHECK(conns[0].rcv_nxt == B + (uint32_t)(16*SS),
          "multi: rcv_nxt=%u want %u", conns[0].rcv_nxt, B + (uint32_t)(16*SS));
    CHECK(conns[0].n_ooo == 0, "multi: all absorbed (n_ooo=%d)", conns[0].n_ooo);
    drain_verify(B, 16 * SS);

    /* 8) sequence wraparound: base near 2^32 so seq wraps mid-flight */
    uint32_t W = 0xFFFFFF00u;
    setup(W);
    inject(W, W + 256, 256, ACK);           /* OOO across the 2^32 wrap */
    inject(W, W + 0, 256, ACK);
    CHECK(conns[0].rcv_nxt == W + 512, "wrap: rcv_nxt=%u want %u", conns[0].rcv_nxt, W + 512);
    /* drain_verify uses absolute pattern from base W */
    {
        uint8_t out[1024]; int total = 0, r;
        while ((r = tcp_recv(0, out + total, (int)sizeof out - total)) > 0) total += r;
        int ok = (total == 512);
        for (int i = 0; i < total; i++) if (out[i] != pat((uint32_t)i)) { ok = 0; break; }
        CHECK(ok, "wrap drain: %d bytes %s", total, ok ? "ok" : "MISMATCH");
    }

    printf("\nTCP reassembly: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
