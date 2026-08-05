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
static uint32_t g_last_seq;
static uint8_t  g_last_flags;
static uint8_t  g_last_seg[1600];
static int      g_last_len;
static int      g_sends;
int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    (void)dst; (void)proto;
    const uint8_t *p = payload;
    if (len >= 20) {
        g_last_ack = (uint32_t)p[8]<<24 | (uint32_t)p[9]<<16 | (uint32_t)p[10]<<8 | p[11];
        g_last_seq = (uint32_t)p[4]<<24 | (uint32_t)p[5]<<16 | (uint32_t)p[6]<<8 | p[7];
        g_last_flags = p[13];
        g_last_win = (uint32_t)p[14]<<8 | p[15];
        g_last_len = len;
        memcpy(g_last_seg, payload, len);
        g_sends++;
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

/* Reset conns[0] into ESTABLISHED with the given initial receive sequence. */
static void setup(uint32_t isn)
{
    struct tcp_conn *c = &conns[0];
    memset(c, 0, sizeof *c);
    c->used = 1; c->state = ESTABLISHED;
    c->lport = LPORT; c->rport = RPORT; c->rip = RIP;
    c->rcv_nxt = c->read_seq = isn;
    c->snd_una = c->snd_nxt = 0xC0DE0000u;
    c->snd_wnd = 65535; c->peer_mss = TXBUF;
    c->snd_wl1 = isn; c->snd_wl2 = c->snd_una;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* General segment injector. opts must be padded to a 4-byte boundary. */
static void inject_seg(uint32_t base, uint32_t seq, uint32_t ack, uint16_t window,
                       int n, uint8_t flags, const uint8_t *opts, int optlen,
                       int corrupt_checksum)
{
    uint8_t buf[60 + 2048];
    int hlen = 20 + optlen;
    memset(buf, 0, (size_t)(hlen + n));
    buf[0] = (RPORT >> 8); buf[1] = RPORT & 0xFF;
    buf[2] = (LPORT >> 8); buf[3] = LPORT & 0xFF;
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

/* Inject a data segment [seq, seq+n): payload byte i = pat(seq_off + i) where
 * seq_off is the stream offset (seq - base). base passed so pattern is absolute. */
static void inject(uint32_t base, uint32_t seq, int n, uint8_t flags)
{
    inject_seg(base, seq, conns[0].snd_nxt, 65535, n, flags, NULL, 0, 0);
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

/* Inject a segment to an arbitrary local port (for the no-connection RST
 * path); corrupt_checksum flips a bit after the checksum is computed. */
static void inject_to(uint16_t dport, uint32_t seq, uint32_t ack,
                      uint8_t flags, int n, int corrupt_checksum)
{
    uint8_t buf[20 + 2048];
    int hlen = 20;
    memset(buf, 0, (size_t)(hlen + n));
    buf[0] = (RPORT >> 8); buf[1] = RPORT & 0xFF;
    buf[2] = (dport >> 8); buf[3] = dport & 0xFF;
    put32(buf + 4, seq); put32(buf + 8, ack);
    buf[12] = (uint8_t)((hlen / 4) << 4);
    buf[13] = flags;
    uint16_t csum = tcp_checksum(RIP, net_cfg.ip, buf, hlen + n);
    buf[16] = (uint8_t)(csum >> 8); buf[17] = (uint8_t)csum;
    if (corrupt_checksum) buf[hlen + (n ? n - 1 : 0)] ^= 0x40;
    tcp_input(RIP, buf, (uint16_t)(hlen + n));
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

    /* 9) bad TCP checksum is discarded before it can advance receive state. */
    setup(B);
    inject_seg(B, B, conns[0].snd_nxt, 65535, 4, ACK, NULL, 0, 1);
    CHECK(conns[0].rcv_nxt == B && conns[0].rx_len == 0,
          "checksum: corrupt segment must be discarded");

    /* 10) SYN carries our MSS; SYN-ACK MSS and send window are learned. */
    setup(B);
    send_seg(&conns[0], SYN, conns[0].snd_nxt, NULL, 0);
    CHECK((g_last_seg[12] >> 4) == 6 && g_last_len == 24,
          "SYN: 24-byte header with MSS option (off=%u len=%d)",
          g_last_seg[12] >> 4, g_last_len);
    CHECK(g_last_seg[20] == 2 && g_last_seg[21] == 4 &&
          (((unsigned)g_last_seg[22] << 8) | g_last_seg[23]) == TXBUF,
          "SYN: advertises MSS=%d", TXBUF);

    {
        struct tcp_conn *c = &conns[0];
        memset(c, 0, sizeof *c);
        c->used = 1; c->state = SYN_SENT;
        c->lport = LPORT; c->rport = RPORT; c->rip = RIP;
        c->snd_una = 100; c->snd_nxt = 101; c->tx_flags = SYN;
        uint8_t mssopt[4] = { 2, 4, 0x04, 0xB0 };       /* 1200 */
        inject_seg(500, 500, 101, 4096, 0, SYN | ACK, mssopt, 4, 0);
        CHECK(c->state == ESTABLISHED && c->peer_mss == 1200 && c->snd_wnd == 4096,
              "SYN-ACK: state=%d mss=%u wnd=%u", c->state, c->peer_mss, c->snd_wnd);
    }

    /* 11) newer window updates win; a reordered older update cannot shrink it. */
    setup(B);
    inject_seg(B, B + 2, conns[0].snd_nxt, 1000, 0, ACK, NULL, 0, 0);
    CHECK(conns[0].snd_wnd == 1000, "window: accepted newer update (%u)", conns[0].snd_wnd);
    inject_seg(B, B + 1, conns[0].snd_nxt, 5, 0, ACK, NULL, 0, 0);
    CHECK(conns[0].snd_wnd == 1000, "window: ignored reordered shrink (%u)", conns[0].snd_wnd);

    /* 12) in-window non-exact RST gets a challenge ACK; exact RCV.NXT resets. */
    setup(B);
    int before = g_sends;
    inject_seg(B, B + 1, 0, 0, 0, RST, NULL, 0, 0);
    CHECK(conns[0].used && g_sends == before + 1 && (g_last_flags & ACK),
          "RST: in-window non-exact reset challenged");
    inject_seg(B, B, 0, 0, 0, RST, NULL, 0, 0);
    CHECK(!conns[0].used, "RST: exact RCV.NXT reset accepted");

    /* 13) peer MSS/window cap a queued segment; zero window enters persist
     * mode rather than killing the connection after ordinary retry count. */
    setup(B);
    conns[0].peer_mss = 200; conns[0].snd_wnd = 300;
    uint8_t sendbuf[500]; memset(sendbuf, 0x5A, sizeof sendbuf);
    int ns = tcp_send(0, sendbuf, sizeof sendbuf);
    CHECK(ns == 200 && conns[0].tx_len == 200 && g_last_len == 220,
          "send cap: queued=%d tx=%d wire=%d", ns, conns[0].tx_len, g_last_len);

    setup(B);
    conns[0].snd_wnd = 0;
    ns = tcp_send(0, sendbuf, 1);
    CHECK(ns == 1 && conns[0].tx_probe && conns[0].tx_len == 1,
          "zero window: one-byte persist probe queued");
    g_ticks = conns[0].tx_tick + 2000;
    for (int i = 0; i < 12; i++) { tcp_poll(); g_ticks += 2000; }
    CHECK(conns[0].used && conns[0].tx_probe,
          "zero window: persist retries do not abort connection");

    /* 14) close waits behind unacknowledged payload instead of replacing its
     * retransmit slot with FIN. The payload ACK then starts the FIN. */
    setup(B);
    send_seg(&conns[0], PSH | ACK, conns[0].snd_nxt, sendbuf, 10);
    arm_retransmit(&conns[0], PSH | ACK, conns[0].snd_nxt, sendbuf, 10);
    conns[0].snd_nxt += 10;
    tcp_close(0);
    CHECK(conns[0].close_pending && conns[0].state == ESTABLISHED &&
          conns[0].tx_flags == (PSH | ACK), "close: payload retained until ACK");
    inject_seg(B, B, conns[0].snd_nxt, 65535, 0, ACK, NULL, 0, 0);
    CHECK(conns[0].state == FIN_WAIT && conns[0].tx_flags == (FIN | ACK),
          "close: FIN starts after payload ACK");

    /* 15) RFC 793: a segment that matches no connection gets a RST.
     *    With ACK -> RST seq = inbound ack; without ACK -> RST+ACK with
     *    ack = inbound seq + seglen. A RST never elicits a RST, and a
     *    corrupt segment gets silence, not a RST. */
    setup(B);
    before = g_sends;
    inject_to(9999, 100, 0x12345678u, ACK, 0, 0);
    CHECK(g_sends == before + 1 && g_last_flags == RST &&
          g_last_seq == 0x12345678u && g_last_len == 20,
          "no-conn ACK: RST seq=%08x flags=%02x (sends=%d)",
          g_last_seq, g_last_flags, g_sends);
    inject_to(9999, 100, 0, 0, 10, 0);          /* 10 data bytes, no ACK */
    CHECK(g_sends == before + 2 && g_last_flags == (RST | ACK) &&
          g_last_seq == 0 && g_last_ack == 110,
          "no-conn data: RST+ACK ack=%u flags=%02x want ack=110",
          g_last_ack, g_last_flags);
    inject_to(9999, 100, 0, SYN, 0, 0);         /* SYN costs one sequence */
    CHECK(g_last_flags == (RST | ACK) && g_last_ack == 101,
          "no-conn SYN: RST+ACK ack=%u want 101", g_last_ack);
    inject_to(9999, 100, 0, RST, 0, 0);
    CHECK(g_sends == before + 3, "no-conn RST must beget silence (sends=%d)",
          g_sends);
    inject_to(9999, 100, 0x12345678u, ACK, 4, 1);   /* corrupt checksum */
    CHECK(g_sends == before + 3, "corrupt segment must not draw a RST");
    CHECK(conns[0].used && conns[0].state == ESTABLISHED,
          "no-conn RSTs disturbed the real connection");

    /* 16) tcp_error: ICMP hard errors abort the matching connection and the
     *    failure surfaces through tcp_recv; soft errors and non-matching
     *    tuples are ignored. */
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
        c->lport = LPORT; c->rport = RPORT; c->rip = RIP;
        tcp_error(LPORT, RIP, RPORT, 3, 1);     /* host unreachable: hard */
        CHECK(!c->used, "tcp_error: SYN_SENT abort must free the slot");
    }

    printf("\nTCP protocol tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
