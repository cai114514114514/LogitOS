#include <stdint.h>
#include <stddef.h>
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "pit.h"
#include "rng.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* IPv6 output, provided by the IPv6 line. Weak: until that lands the symbol is
 * NULL and an AF_INET6 connection simply cannot be opened. Nothing else in this
 * file assumes a family. */
int ip6_send(const uint8_t dst[16], uint8_t proto, const void *payload,
             uint16_t len) __attribute__((weak));
/* Source-address selection for a given destination; 0 on success. */
int ip6_local_addr(const uint8_t dst[16], uint8_t out[16]) __attribute__((weak));

/* TCP flags */
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSE_WAIT, LAST_ACK,
       CLOSING, TIME_WAIT };

/* 32, up from 8. A connection POOL is the point of the async socket layer: a
 * real page pulls stylesheets/scripts/images from a handful of origins, and a
 * browser keeps ~6 sockets open per origin, so 8 slots could not hold even one
 * origin's pool plus the legacy blocking SYS_HTTP_GET path plus the slots that
 * linger in TIME_WAIT after each object.
 *
 * The cost is BSS: each conn carries its own seq-indexed receive ring and send
 * ring, so a slot is ~161 KiB and conns[] is ~5.2 MiB. That is affordable on
 * the 512 MiB the machine boots with, and the alternative -- kmalloc'ing the
 * rings per connect -- would put a failure path in the middle of the SYN and
 * break the white-box host test (tests/unit/tcp_test.c #includes this file with
 * no heap). */
#define NCONN    32

/* Receive ring. 128 KiB, which is the number that makes RFC 7323 window scaling
 * mean something: without scaling the advertised window is capped at 65535
 * whatever the buffer holds, so a 64 KiB ring made the option decorative. Must
 * be a power of two (ring &). */
#define RXBUF    131072
/* Send ring. Smaller on purpose -- this is a client stack: it sends requests
 * and TLS handshake flights, it does not serve bodies. 32 KiB is four times the
 * largest thing tls.c hands down in one go. Power of two. */
#define SNDBUF   32768

#define MSS_ADV     1460  /* MSS we advertise: 1500 MTU - 20 IP - 20 TCP. Per
                           * RFC 6691 this EXCLUDES options; our own sender
                           * subtracts its option bytes (see eff_mss). */
#define DEFAULT_MSS 536   /* RFC 9293 IPv4 send-MSS default when peer omits it */
#define MIN_MSS     216   /* floor for PMTU/blackhole clamping */
#define NOOO     16       /* out-of-order reassembly intervals tracked per conn */
#define NSACK    8        /* sender-side SACK scoreboard intervals */

#define MAXOPT   40       /* the TCP option area is 40 bytes, full stop */

/* A byte range [seq, end). Used for both the receive reassembly set and the
 * sender's SACK scoreboard; both are kept sorted, merged and non-overlapping. */
struct ooo_seg { uint32_t seq, end; };

struct tcp_conn {
    int      state;
    uint16_t lport, rport;
    struct tcp_addr raddr;      /* peer */
    struct tcp_addr laddr;      /* the local address this connection uses */

    /* --- send sequence space ---
     * snd_una  oldest unacknowledged byte
     * snd_nxt  next byte to transmit (rewinds to snd_una on an RTO)
     * snd_max  highest byte ever transmitted + 1 (never rewinds)
     * snd_end  one past the last byte the application has queued */
    uint32_t snd_una, snd_nxt, snd_max, snd_end;
    uint32_t snd_wnd;           /* peer window, ALREADY left-shifted by snd_scale */
    uint32_t snd_wl1, snd_wl2;  /* SEG.SEQ/ACK of last accepted window update */
    uint16_t peer_mss;          /* MSS from SYN-ACK, or DEFAULT_MSS */
    uint16_t pmtu_mss;          /* RFC 1191 clamp on top of peer_mss */
    uint8_t  snd[SNDBUF];       /* send ring, indexed by seq & (SNDBUF-1) */
    int      fin_queued;        /* the app closed; FIN occupies seq snd_end */
    int      fin_sent;          /* the FIN has been transmitted at least once */
    int      nodelay;           /* TCP_NODELAY: Nagle off */

    /* --- receive --- */
    uint32_t rcv_nxt;           /* next expected (contiguous) receive seq */
    uint32_t rcv_adv;           /* right edge we last advertised (never shrink) */
    uint32_t last_ack_sent;     /* rcv_nxt carried by our last ACK (PAWS) */
    int      peer_fin;          /* peer sent FIN (no more data coming) */

    /* Receive ring, indexed by sequence: byte seq s -> rx[(rx_head + (s -
     * read_seq)) & (RXBUF-1)]. read_seq is the seq of rx_head (first unread
     * byte); contiguous readable bytes = rcv_nxt - read_seq = rx_len. Bytes that
     * arrived out of order live in the ring too (at their seq slot) and become
     * readable when the preceding hole fills and rcv_nxt advances over them. */
    uint8_t  rx[RXBUF];
    int      rx_head;           /* ring index of first unread byte */
    int      rx_len;            /* contiguous unread bytes (rcv_nxt - read_seq) */
    uint32_t read_seq;          /* seq of rx_head */
    struct ooo_seg ooo[NOOO];   /* received-but-not-contiguous ranges, sorted */
    int      n_ooo;
    uint32_t ooo_recent;        /* start seq of the most recently touched OOO
                                 * block -- RFC 2018 wants it reported first */
    int      adv_wnd;           /* window we last advertised, in bytes */

    /* --- negotiated options (RFC 7323, RFC 2018) --- */
    int      sack_ok;           /* both sides sent SACK-permitted */
    int      ts_ok;             /* both sides sent a timestamp option */
    uint8_t  rcv_scale;         /* shift we asked the peer to apply to our window */
    uint8_t  snd_scale;         /* shift we apply to the peer's window */
    uint32_t ts_recent;         /* peer's TSval to echo (PAWS anchor) */

    /* --- sender-side SACK scoreboard: ranges above snd_una the peer holds --- */
    struct ooo_seg sacked[NSACK];
    int      n_sacked;

    /* --- RFC 5681 congestion control (bytes) --- */
    uint32_t cwnd, ssthresh;
    uint32_t ca_acked;          /* congestion-avoidance byte accumulator */
    uint32_t recover;           /* RFC 6582 NewReno recovery point */
    int      in_recovery;
    int      dup_acks;

    /* --- timers (ticks; 1 tick = 10 ms) --- */
    uint64_t rtx_tick;          /* when the retransmission timer was started */
    int      rtx_running;
    int      rtx_retries;
    uint64_t persist_tick;      /* zero-window persist */
    int      persist_running, persist_shift;
    uint64_t ack_due;           /* delayed-ACK deadline, 0 = no ACK owed */
    int      ack_segs;          /* full-sized segments received since our last ACK */
    uint64_t rx_tick;           /* last valid inbound segment (FIN_WAIT backstop) */
    uint64_t tw_tick;           /* entered TIME_WAIT (slot-release deadline) */

    /* --- RFC 6298 RTT estimation, in ticks --- */
    uint64_t srtt, rttvar, rto;
    int      rtt_pending;       /* timing one segment (no-timestamps fallback) */
    uint32_t rtt_seq;           /* its last sequence + 1 */
    uint64_t rtt_tick;

    /* --- counters, for tcp_get_info / tests --- */
    uint32_t n_rexmit, n_fast_rexmit, n_rto;

    int      used;
};

static struct tcp_conn conns[NCONN];
static uint16_t next_port = 49152;
static uint32_t iss_counter = 1;

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off;               /* data offset in high nibble (<<4) */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urg;
} __attribute__((packed));

/* Wraparound-safe sequence comparison (RFC 793 "SEQ < SEQ" via signed diff). */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

/* ------------------------------------------------------------------ address
 * family plumbing. Everything that differs between IPv4 and IPv6 lives here:
 * the pseudo-header, the send call, and local-address selection. */

static void addr_v4(struct tcp_addr *a, uint32_t ip)
{
    a->af = TCP_AF_INET; a->a.v4 = ip;
}

static int addr_eq(const struct tcp_addr *a, const struct tcp_addr *b)
{
    if (a->af != b->af) return 0;
    if (a->af == TCP_AF_INET) return a->a.v4 == b->a.v4;
    for (int i = 0; i < 16; i++) if (a->a.v6[i] != b->a.v6[i]) return 0;
    return 1;
}

/* Ones-complement sum of the transport pseudo-header. RFC 9293 for IPv4,
 * RFC 8200 §8.1 for IPv6 -- the layouts differ, the folding does not. */
static uint32_t pseudo_sum(const struct tcp_addr *src, const struct tcp_addr *dst,
                           uint32_t len)
{
    uint32_t sum = 0;
    if (src->af == TCP_AF_INET) {
        sum += (src->a.v4 >> 16) & 0xFFFF; sum += src->a.v4 & 0xFFFF;
        sum += (dst->a.v4 >> 16) & 0xFFFF; sum += dst->a.v4 & 0xFFFF;
        sum += IP_PROTO_TCP;
        sum += len;
    } else {
        for (int i = 0; i < 16; i += 2)
            sum += ((uint32_t)src->a.v6[i] << 8) | src->a.v6[i + 1];
        for (int i = 0; i < 16; i += 2)
            sum += ((uint32_t)dst->a.v6[i] << 8) | dst->a.v6[i + 1];
        sum += (len >> 16) & 0xFFFF; sum += len & 0xFFFF;
        sum += IP_PROTO_TCP;             /* next header, zero-padded */
    }
    return sum;
}

/* Ones-complement checksum over the pseudo-header + segment. */
static uint16_t tcp_checksum_af(const struct tcp_addr *src,
                                const struct tcp_addr *dst,
                                const uint8_t *seg, int len)
{
    uint32_t sum = pseudo_sum(src, dst, (uint32_t)len);
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (len & 1)
        sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* IPv4-only helper kept for the host test and send_reset. */
__attribute__((unused))
static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
    struct tcp_addr s, d;
    addr_v4(&s, src); addr_v4(&d, dst);
    return tcp_checksum_af(&s, &d, seg, len);
}

static int af_send(const struct tcp_addr *dst, const void *seg, uint16_t len)
{
    if (dst->af == TCP_AF_INET)
        return ip_send(dst->a.v4, IP_PROTO_TCP, seg, len);
    if (ip6_send)
        return ip6_send(dst->a.v6, IP_PROTO_TCP, seg, len);
    return -1;                          /* no IPv6 output path linked in */
}

/* The local address to use towards `dst`. */
static int af_local(const struct tcp_addr *dst, struct tcp_addr *out)
{
    if (dst->af == TCP_AF_INET) { addr_v4(out, net_cfg.ip); return 0; }
    if (!ip6_local_addr) return -1;
    out->af = TCP_AF_INET6;
    return ip6_local_addr(dst->a.v6, out->a.v6);
}

/* ------------------------------------------------------------------ windows */

/* Receive window in BYTES: from rcv_nxt to the fixed right edge of the ring.
 * OOO bytes already occupy their sequence slots, so they do not move the edge.
 * RFC 1122 4.2.3.3 silly-window avoidance collapses a useless sliver to zero;
 * the right edge is never retracted (RFC 9293 "do not shrink the window"). */
static uint32_t recv_window(struct tcp_conn *c)
{
    uint32_t used = c->rcv_nxt - c->read_seq;
    uint32_t win = used >= RXBUF ? 0 : (uint32_t)RXBUF - used;
    if (win < (uint32_t)RXBUF / 2 && win < MSS_ADV)
        win = 0;
    if (seq_lt(c->rcv_nxt, c->rcv_adv)) {
        uint32_t floor_ = c->rcv_adv - c->rcv_nxt;
        if (win < floor_) win = floor_;
    }
    return win;
}

/* The value that goes on the wire, i.e. the byte window right-shifted by the
 * scale the peer agreed to apply. Truncation is the safe direction. */
static uint16_t wire_window(struct tcp_conn *c)
{
    uint32_t win = recv_window(c) >> c->rcv_scale;
    return win > 65535 ? 65535 : (uint16_t)win;
}

/* RFC 9293 receive-window acceptability (SEG.LEN counts SYN/FIN). */
static int segment_acceptable(struct tcp_conn *c, uint32_t seq, uint32_t seglen)
{
    uint32_t win = recv_window(c);
    if (seglen == 0) {
        if (win == 0) return seq == c->rcv_nxt;
        return seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + win);
    }
    if (win == 0) return 0;
    uint32_t last = seq + seglen - 1;
    return (seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + win)) ||
           (seq_le(c->rcv_nxt, last) && seq_lt(last, c->rcv_nxt + win));
}

/* ------------------------------------------------------------------ options */

struct tcp_opts {
    uint16_t mss;
    int      have_ws;   uint8_t  ws;
    int      sack_perm;
    int      have_ts;   uint32_t tsval, tsecr;
    int      n_sack;    struct ooo_seg sack[NSACK];
};

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Validate the option list and extract everything we act on. Unknown options
 * are skipped as required; malformed lengths reject the segment. */
static int parse_options(const uint8_t *opt, int len, struct tcp_opts *o)
{
    memset(o, 0, sizeof *o);
    for (int i = 0; i < len;) {
        uint8_t kind = opt[i];
        if (kind == 0) {                      /* EOL; remaining bytes are zero pad */
            for (int j = i + 1; j < len; j++) if (opt[j] != 0) return -1;
            break;
        }
        if (kind == 1) { i++; continue; }     /* NOP */
        if (i + 1 >= len) return -1;
        int olen = opt[i + 1];
        if (olen < 2 || i + olen > len) return -1;
        switch (kind) {
        case 2:                                       /* MSS */
            if (olen != 4) return -1;
            o->mss = (uint16_t)(((uint16_t)opt[i + 2] << 8) | opt[i + 3]);
            if (o->mss == 0) return -1;
            break;
        case 3:                                       /* RFC 7323 window scale */
            if (olen != 3) return -1;
            o->have_ws = 1;
            /* "If a Window Scale option is received with a shift.cnt value
             * larger than 14, the value MUST be set to 14." */
            o->ws = opt[i + 2] > 14 ? 14 : opt[i + 2];
            break;
        case 4:                                       /* SACK-permitted */
            if (olen != 2) return -1;
            o->sack_perm = 1;
            break;
        case 5: {                                     /* SACK blocks */
            if (olen < 10 || ((olen - 2) % 8) != 0) return -1;
            int nb = (olen - 2) / 8;
            for (int b = 0; b < nb && o->n_sack < NSACK; b++) {
                const uint8_t *p = opt + i + 2 + b * 8;
                o->sack[o->n_sack].seq = rd32(p);
                o->sack[o->n_sack].end = rd32(p + 4);
                if (seq_lt(o->sack[o->n_sack].end, o->sack[o->n_sack].seq))
                    return -1;                        /* reversed block */
                o->n_sack++;
            }
            break;
        }
        case 8:                                       /* RFC 7323 timestamps */
            if (olen != 10) return -1;
            o->have_ts = 1;
            o->tsval = rd32(opt + i + 2);
            o->tsecr = rd32(opt + i + 6);
            break;
        default:
            break;
        }
        i += olen;
    }
    return 0;
}

/* Option bytes we put on a non-SYN segment that carries data. Kept out of the
 * MSS so a full-sized segment still fits the path MTU (RFC 6691). */
static uint32_t data_optlen(struct tcp_conn *c)
{
    return c->ts_ok ? 12u : 0u;     /* NOP NOP TSopt */
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Build the option area for one outbound segment. Returns its length (a
 * multiple of 4, <= MAXOPT). */
static int build_options(struct tcp_conn *c, uint8_t flags, uint8_t *o)
{
    int n = 0;
    if (flags & SYN) {
        /* Linux's ordering, which every middlebox on earth has seen:
         * MSS, SACK-permitted, timestamps, NOP, window scale = exactly 20. */
        o[n++] = 2; o[n++] = 4;
        o[n++] = (uint8_t)(MSS_ADV >> 8); o[n++] = (uint8_t)MSS_ADV;
        o[n++] = 4; o[n++] = 2;
        o[n++] = 8; o[n++] = 10;
        wr32(o + n, (uint32_t)timer_ticks()); n += 4;
        wr32(o + n, c->ts_recent); n += 4;
        o[n++] = 1;
        o[n++] = 3; o[n++] = 3; o[n++] = c->rcv_scale;
        return n;
    }
    if (c->ts_ok) {
        o[n++] = 1; o[n++] = 1;
        o[n++] = 8; o[n++] = 10;
        wr32(o + n, (uint32_t)timer_ticks()); n += 4;
        wr32(o + n, c->ts_recent); n += 4;
    }
    /* RFC 2018 receiver side: report the holes so the peer stops resending what
     * we already hold. The block covering the most recently received segment
     * goes first; the rest follow in sequence order. */
    if (c->sack_ok && c->n_ooo > 0 && (flags & ACK)) {
        int room = (MAXOPT - n - 4) / 8;              /* NOP NOP kind len + blocks */
        if (room > 4) room = 4;
        if (room > c->n_ooo) room = c->n_ooo;
        if (room > 0) {
            int order[NOOO], k = 0;
            for (int i = 0; i < c->n_ooo; i++)
                if (c->ooo[i].seq == c->ooo_recent) order[k++] = i;
            for (int i = 0; i < c->n_ooo && k < c->n_ooo; i++)
                if (c->ooo[i].seq != c->ooo_recent) order[k++] = i;
            o[n++] = 1; o[n++] = 1;
            o[n++] = 5; o[n++] = (uint8_t)(2 + 8 * room);
            for (int i = 0; i < room; i++) {
                wr32(o + n, c->ooo[order[i]].seq); n += 4;
                wr32(o + n, c->ooo[order[i]].end); n += 4;
            }
        }
    }
    while (n & 3) o[n++] = 0;                          /* EOL pad to a word */
    return n;
}

/* ------------------------------------------------------------ interval sets */

/* Insert [s,e) into a sorted, merged, non-overlapping set. Adjacent ranges
 * merge (e == is or ie == s). Returns the new count; a full set drops the
 * insertion (the sender will resend, or the peer will re-SACK). */
static int iv_insert(struct ooo_seg *set, int n, int cap, uint32_t s, uint32_t e)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        uint32_t is = set[i].seq, ie = set[i].end;
        if (seq_lt(e, is) || seq_lt(ie, s)) {
            set[w++] = set[i];                /* disjoint */
        } else {
            if (seq_lt(is, s)) s = is;        /* overlap/adjacent: grow [s,e) */
            if (seq_lt(e, ie)) e = ie;
        }
    }
    if (w < cap) {
        int j = w;
        while (j > 0 && seq_lt(s, set[j - 1].seq)) { set[j] = set[j - 1]; j--; }
        set[j].seq = s; set[j].end = e;
        return w + 1;
    }
    return w;
}

/* Drop everything at or below `una` (already cumulatively acknowledged). */
static int iv_trim(struct ooo_seg *set, int n, uint32_t una)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (seq_le(set[i].end, una)) continue;
        if (seq_lt(set[i].seq, una)) set[i].seq = una;
        set[w++] = set[i];
    }
    return w;
}

static uint32_t iv_bytes(const struct ooo_seg *set, int n)
{
    uint32_t t = 0;
    for (int i = 0; i < n; i++) t += set[i].end - set[i].seq;
    return t;
}

/* ------------------------------------------------------------------- rings */

/* Write n bytes at the receive-ring slot for sequence s (caller guarantees the
 * whole [s, s+n) range lies within [read_seq, read_seq+RXBUF)). */
static void ring_write(struct tcp_conn *c, uint32_t s, const uint8_t *src, int n)
{
    int pos = (c->rx_head + (int)(s - c->read_seq)) & (RXBUF - 1);
    int first = RXBUF - pos;
    if (n <= first) {
        memcpy(c->rx + pos, src, (size_t)n);
    } else {
        memcpy(c->rx + pos, src, (size_t)first);
        memcpy(c->rx, src + first, (size_t)(n - first));
    }
}

/* The send ring is indexed by absolute sequence, so it needs no base pointer:
 * byte seq s lives at snd[s & (SNDBUF-1)]. */
static void snd_write(struct tcp_conn *c, uint32_t s, const uint8_t *src, int n)
{
    int pos = (int)(s & (SNDBUF - 1));
    int first = SNDBUF - pos;
    if (n <= first) {
        memcpy(c->snd + pos, src, (size_t)n);
    } else {
        memcpy(c->snd + pos, src, (size_t)first);
        memcpy(c->snd, src + first, (size_t)(n - first));
    }
}

static void snd_read(struct tcp_conn *c, uint32_t s, uint8_t *dst, int n)
{
    int pos = (int)(s & (SNDBUF - 1));
    int first = SNDBUF - pos;
    if (n <= first) {
        memcpy(dst, c->snd + pos, (size_t)n);
    } else {
        memcpy(dst, c->snd + pos, (size_t)first);
        memcpy(dst + first, c->snd, (size_t)(n - first));
    }
}

/* Merge received range [s, e) into the reassembly set and advance rcv_nxt over
 * any contiguous data. Precondition: bytes are already in the ring and
 * rcv_nxt <= e, s <= read_seq+RXBUF (clipped by the caller). */
static void reassemble(struct tcp_conn *c, uint32_t s, uint32_t e)
{
    if (seq_le(s, c->rcv_nxt)) {
        /* touches the contiguous front: extend, then absorb adjacent OOO ranges */
        if (seq_lt(c->rcv_nxt, e)) c->rcv_nxt = e;
        int w = 0;
        for (int i = 0; i < c->n_ooo; i++) {
            if (seq_le(c->ooo[i].seq, c->rcv_nxt)) {
                if (seq_lt(c->rcv_nxt, c->ooo[i].end)) c->rcv_nxt = c->ooo[i].end;
            } else {
                c->ooo[w++] = c->ooo[i];     /* still a hole before this one */
            }
        }
        c->n_ooo = w;
    } else {
        c->n_ooo = iv_insert(c->ooo, c->n_ooo, NOOO, s, e);
        /* Remember which merged block the new bytes ended up in: RFC 2018
         * requires the first SACK block to report the most recent segment. */
        for (int i = 0; i < c->n_ooo; i++)
            if (seq_le(c->ooo[i].seq, s) && seq_lt(s, c->ooo[i].end))
                c->ooo_recent = c->ooo[i].seq;
    }
    c->rx_len = (int)(c->rcv_nxt - c->read_seq);
}

/* Take an inbound data segment into the receive buffer (with reassembly).
 * Returns 1 if the segment left a hole (i.e. it was out of order). */
static int rx_data(struct tcp_conn *c, uint32_t seg_seq, const uint8_t *payload, int dlen)
{
    uint32_t s = seg_seq, e = seg_seq + (uint32_t)dlen;
    if (seq_lt(s, c->rcv_nxt)) { payload += (c->rcv_nxt - s); s = c->rcv_nxt; }  /* trim dup prefix */
    uint32_t win_end = c->rcv_nxt + recv_window(c);
    if (seq_lt(win_end, e)) e = win_end;                                          /* trim beyond buffer */
    if (seq_le(e, s)) return c->n_ooo > 0;                                        /* nothing new */
    ring_write(c, s, payload, (int)(e - s));
    reassemble(c, s, e);
    return c->n_ooo > 0;
}

/* --------------------------------------------------------------- transmit */

/* Build and transmit one segment. `dlen` bytes of payload are taken from the
 * send ring starting at `seq`. */
static void send_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq, int dlen)
{
    uint8_t seg[sizeof(struct tcp_hdr) + MAXOPT + MSS_ADV];
    struct tcp_hdr *h = (struct tcp_hdr *)seg;
    memset(h, 0, sizeof *h);
    h->sport = htons(c->lport);
    h->dport = htons(c->rport);
    h->seq = htonl(seq);
    h->ack = htonl(c->rcv_nxt);
    int hlen = (int)sizeof *h;
    hlen += build_options(c, flags, seg + hlen);
    h->off = (uint8_t)((hlen / 4) << 4);
    h->flags = flags;
    /* RFC 7323 §2.2: the window in a SYN is never scaled -- the peer has not
     * agreed to a shift yet. Everything afterwards is. */
    uint16_t win;
    if (flags & SYN) {
        uint32_t w = recv_window(c);
        win = w > 65535 ? 65535 : (uint16_t)w;
    } else {
        win = wire_window(c);
    }
    h->window = htons(win);
    if (dlen > 0)
        snd_read(c, seq, seg + hlen, dlen);
    h->checksum = 0;
    int total = hlen + dlen;
    h->checksum = htons(tcp_checksum_af(&c->laddr, &c->raddr, seg, total));
    if (flags & ACK) {
        c->adv_wnd = (int)((uint32_t)win << c->rcv_scale);
        c->rcv_adv = c->rcv_nxt + ((uint32_t)win << c->rcv_scale);
        c->last_ack_sent = c->rcv_nxt;
        c->ack_due = 0;                 /* this segment carries the ACK we owed */
        c->ack_segs = 0;
    }
    af_send(&c->raddr, seg, (uint16_t)total);
}

/* An ACK we owe the peer, sent now rather than on the delayed-ACK timer. */
static void send_ack(struct tcp_conn *c)
{
    send_seg(c, ACK, c->snd_nxt, 0);
}

/* Every duration in this file is written in MILLISECONDS and converted, never
 * open-coded as a tick count. A tick is not a unit: the PIT ran at exactly
 * twice its programmed rate for a long time, so every timeout expressed in
 * ticks was silently half its stated value, and fixing the rate doubled them
 * all at once. Protocol timers are the worst place for that to happen quietly
 * -- an RTO that is really half of what the comment says is a stack that
 * retransmits into a healthy path. Rounding up keeps a sub-tick duration from
 * collapsing to zero. */
#define MS_TICKS(ms) ((uint64_t)(((ms) * (uint64_t)TIMER_HZ + 999) / 1000))

#define RTO_DEFAULT MS_TICKS(1000)    /* RFC 6298 §2.1: 1 s until the first sample */
#define RTO_MIN     MS_TICKS(100)     /* below the RFC's 1 s floor on purpose: this
                                       * is a LAN/VM stack and a 1 s minimum makes
                                       * every lost segment cost a second */
#define RTO_MAX     MS_TICKS(60000)   /* RFC 6298 §2.5 upper bound */
#define DELACK_MAX  MS_TICKS(200)     /* RFC 1122 4.2.3.2: MUST be < 500 ms */
#define TIME_WAIT_MS 10000            /* shortened 2MSL; see tcp_poll */
#define FINWAIT_QUIET_MS 2000         /* half-closed backstop */
#define CONNECT_MS  5000              /* tcp_connect's own deadline */
#define TXWAIT_MS   8000              /* how long tcp_send waits for ring space */

/* RFC 6298 RTO from the current estimators, in ticks (clock granularity
 * G = 1 tick = 10 ms). */
static uint64_t base_rto(struct tcp_conn *c)
{
    if (c->srtt == 0) return RTO_DEFAULT;
    uint64_t var = 4 * c->rttvar;
    uint64_t rto = c->srtt + (var > 1 ? var : 1);   /* SRTT + max(G, 4*RTTVAR) */
    if (rto < RTO_MIN) rto = RTO_MIN;
    if (rto > RTO_MAX) rto = RTO_MAX;
    return rto;
}

/* Fold one RTT sample into the estimators (Jacobson/Karels shifts). */
static void rtt_sample(struct tcp_conn *c, uint64_t rtt)
{
    if (rtt == 0) rtt = 1;
    if (rtt > RTO_MAX) return;                  /* nonsense: a stale echo */
    if (c->srtt == 0) {
        c->srtt = rtt;
        c->rttvar = rtt / 2;
    } else {
        uint64_t d = c->srtt > rtt ? c->srtt - rtt : rtt - c->srtt;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + rtt) / 8;
    }
    c->rto = base_rto(c);
}

/* The retransmission timer is per connection, not per segment (RFC 6298 §5):
 * it runs while anything is unacknowledged and restarts on every ACK that
 * advances snd_una. */
static void rtx_start(struct tcp_conn *c)
{
    c->rtx_tick = timer_ticks();
    c->rtx_running = 1;
}

static void rtx_stop(struct tcp_conn *c)
{
    c->rtx_running = 0;
    c->rtx_retries = 0;
}

/* The largest payload we may put in one segment: the peer's MSS, clamped by
 * the path MTU estimate, minus our own option bytes (RFC 6691). */
static uint32_t eff_mss(struct tcp_conn *c)
{
    uint32_t m = c->peer_mss;
    if (m > MSS_ADV) m = MSS_ADV;
    if (c->pmtu_mss && m > c->pmtu_mss) m = c->pmtu_mss;
    uint32_t opt = data_optlen(c);
    return m > opt + 1 ? m - opt : 1;
}

/* Bytes the peer is known to hold above snd_una (from its SACK blocks). */
__attribute__((unused))
static uint32_t sacked_bytes(struct tcp_conn *c)
{
    return c->sack_ok ? iv_bytes(c->sacked, c->n_sacked) : 0;
}

/* The end of the first hole at snd_una: retransmission never runs past what we
 * have already sent, and never into a range the peer selectively acknowledged.
 * This is the whole sender-side point of RFC 2018 -- without the scoreboard a
 * loss recovery resends data the receiver already holds. */
static uint32_t hole_end(struct tcp_conn *c)
{
    uint32_t end = c->snd_max;
    for (int i = 0; i < c->n_sacked; i++)
        if (seq_lt(c->snd_una, c->sacked[i].seq) && seq_lt(c->sacked[i].seq, end))
            end = c->sacked[i].seq;
    return end;
}

/* RFC 5681 initial window. RFC 6928 raised it to 10 segments, which is what
 * every deployed stack uses; the 14600-byte cap keeps a small-MSS path from
 * bursting more than the original 4380-byte rule scaled up. */
static uint32_t initial_cwnd(struct tcp_conn *c)
{
    uint32_t m = eff_mss(c);
    uint32_t iw = 10 * m;
    if (iw > 14600) iw = 14600;
    if (iw < 2 * m) iw = 2 * m;
    return iw;
}

static uint32_t cc_ssthresh_on_loss(struct tcp_conn *c, uint32_t flight)
{
    uint32_t m = eff_mss(c);
    uint32_t s = flight / 2;
    if (s < 2 * m) s = 2 * m;
    return s;
}

/* Send one segment if the windows, Nagle and the queue all allow it.
 * Returns 1 if something went out. */
static int output_one(struct tcp_conn *c)
{
    if (c->state != ESTABLISHED && c->state != CLOSE_WAIT &&
        c->state != FIN_WAIT && c->state != CLOSING && c->state != LAST_ACK)
        return 0;

    uint32_t avail = seq_lt(c->snd_nxt, c->snd_end) ? c->snd_end - c->snd_nxt : 0;
    uint32_t mss = eff_mss(c);

    if (avail == 0) {
        /* Nothing but a FIN left to send. A FIN goes out even against a closed
         * window: it carries no data, and holding it there is how a slot leaks
         * for the rest of the boot. */
        if (c->fin_queued && !c->fin_sent && c->snd_nxt == c->snd_end) {
            send_seg(c, FIN | ACK, c->snd_nxt, 0);
            c->snd_nxt += 1;
            if (seq_lt(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
            c->fin_sent = 1;
            if (c->state == ESTABLISHED)      c->state = FIN_WAIT;
            else if (c->state == CLOSE_WAIT)  c->state = LAST_ACK;
            if (!c->rtx_running) rtx_start(c);
            return 1;
        }
        return 0;
    }

    uint32_t wnd = c->snd_wnd < c->cwnd ? c->snd_wnd : c->cwnd;
    if (wnd == 0) return 0;                    /* persist timer's job */
    uint32_t off = c->snd_nxt - c->snd_una;
    if (off >= wnd) return 0;                  /* window full */
    uint32_t room = wnd - off;

    uint32_t len = avail < room ? avail : room;
    int full = (len >= mss);
    if (len > mss) len = mss;

    /* RFC 896 Nagle: a small segment waits until everything sent is
     * acknowledged. The escape hatches are TCP_NODELAY, a full-sized segment,
     * and a close -- holding the last bytes of a stream behind an ACK when the
     * FIN is already queued is exactly the 200 ms stall people complain about. */
    if (!full && !c->nodelay && seq_lt(c->snd_una, c->snd_max)) {
        int closes = c->fin_queued && (c->snd_nxt + len == c->snd_end);
        if (!closes) return 0;
    }
    /* RFC 1122 4.2.3.4 sender-side silly-window avoidance: when the PEER's
     * window (not the congestion window -- being cwnd-limited is normal) is
     * what caps this segment and it has opened less than a full segment, wait
     * for it to open properly rather than dribbling out fragments. */
    if (!full && seq_lt(c->snd_una, c->snd_max)) {
        uint32_t proom = c->snd_wnd > off ? c->snd_wnd - off : 0;
        if (proom <= len && proom < mss) return 0;
    }

    uint8_t fl = ACK | PSH;
    int fin = 0;
    if (c->fin_queued && !c->fin_sent && c->snd_nxt + len == c->snd_end &&
        room >= len + 1) {
        fl |= FIN; fin = 1;
    }
    send_seg(c, fl, c->snd_nxt, (int)len);

    /* Time one segment per RTT when the peer refused timestamps (Karn's
     * algorithm still applies: a retransmit cancels the sample). */
    if (!c->ts_ok && !c->rtt_pending && !seq_lt(c->snd_nxt, c->snd_max)) {
        c->rtt_pending = 1;
        c->rtt_seq = c->snd_nxt + len;
        c->rtt_tick = timer_ticks();
    }

    c->snd_nxt += len + (uint32_t)fin;
    if (seq_lt(c->snd_max, c->snd_nxt)) c->snd_max = c->snd_nxt;
    if (fin) {
        c->fin_sent = 1;
        if (c->state == ESTABLISHED)      c->state = FIN_WAIT;
        else if (c->state == CLOSE_WAIT)  c->state = LAST_ACK;
    }
    if (!c->rtx_running) rtx_start(c);
    return 1;
}

/* Drain the send queue as far as the windows permit. */
static void tcp_output(struct tcp_conn *c)
{
    int guard = 0;
    while (output_one(c) && ++guard < 4096)
        ;
}

/* Retransmit the first unacknowledged segment, skipping nothing (it is by
 * definition the hole). Used by fast retransmit and by NewReno partial ACKs. */
static void retransmit_head(struct tcp_conn *c)
{
    if (c->state == SYN_SENT) { send_seg(c, SYN, c->snd_una, 0); c->n_rexmit++; return; }
    uint32_t end = hole_end(c);
    if (!seq_lt(c->snd_una, end)) return;
    uint32_t mss = eff_mss(c);
    uint32_t len = end - c->snd_una;
    int fin = 0;
    if (c->fin_sent && c->snd_una + len == c->snd_end + 1) { len -= 1; fin = 1; }
    if (len > mss) { len = mss; fin = 0; }
    if (len == 0 && !fin) return;
    send_seg(c, (uint8_t)(ACK | PSH | (fin ? FIN : 0)), c->snd_una, (int)len);
    c->n_rexmit++;
    c->rtt_pending = 0;                 /* Karn: this window yields no sample */
    rtx_start(c);
}

/* ------------------------------------------------------- congestion control */

/* RFC 5681 §3.1: grow the window for `acked` newly acknowledged bytes. */
static void cc_on_ack(struct tcp_conn *c, uint32_t acked)
{
    uint32_t mss = eff_mss(c);
    if (c->cwnd < c->ssthresh) {
        uint32_t inc = acked < mss ? acked : mss;    /* slow start, RFC 3465 bound */
        c->cwnd += inc;
        if (c->cwnd > c->ssthresh) c->ca_acked = 0;
    } else {
        /* Congestion avoidance: one MSS per cwnd bytes acknowledged, i.e. one
         * MSS per round trip. Accumulate rather than use cwnd += mss*mss/cwnd,
         * which loses to integer truncation at large windows. */
        c->ca_acked += acked;
        while (c->ca_acked >= c->cwnd) {
            c->ca_acked -= c->cwnd;
            c->cwnd += mss;
        }
    }
    uint32_t cap = SNDBUF * 4u;
    if (c->cwnd > cap) c->cwnd = cap;
}

/* RFC 5681 §3.2 step 2 / RFC 6582: enter fast recovery. */
static void cc_enter_recovery(struct tcp_conn *c)
{
    uint32_t flight = c->snd_max - c->snd_una;
    c->ssthresh = cc_ssthresh_on_loss(c, flight);
    c->recover = c->snd_max;
    c->in_recovery = 1;
    c->cwnd = c->ssthresh + 3 * eff_mss(c);      /* inflate for the 3 dup ACKs */
    c->n_fast_rexmit++;
}

/* RFC 6298 §5.5-5.6 + RFC 5681 §3.1: a timeout collapses the window. */
static void cc_on_rto(struct tcp_conn *c)
{
    uint32_t flight = c->snd_max - c->snd_una;
    c->ssthresh = cc_ssthresh_on_loss(c, flight);
    c->cwnd = eff_mss(c);                        /* LW = 1 full-sized segment */
    c->ca_acked = 0;
    c->in_recovery = 0;
    c->dup_acks = 0;
    c->n_sacked = 0;                             /* RFC 6675 §5.1: clear on RTO */
    c->n_rto++;
}

/* ----------------------------------------------------------------- helpers */

static void update_send_window(struct tcp_conn *c, uint32_t seg_seq,
                               uint32_t seg_ack, uint16_t seg_wnd)
{
    if (!seq_le(c->snd_una, seg_ack) || !seq_le(seg_ack, c->snd_nxt)) return;
    if (seq_lt(c->snd_wl1, seg_seq) ||
        (c->snd_wl1 == seg_seq && seq_le(c->snd_wl2, seg_ack))) {
        uint32_t w = (uint32_t)seg_wnd << c->snd_scale;
        c->snd_wnd = w;
        c->snd_wl1 = seg_seq;
        c->snd_wl2 = seg_ack;
        if (c->persist_running && w > 0) {
            c->persist_running = 0;
            c->persist_shift = 0;
        }
    }
}

static struct tcp_conn *find_conn(uint16_t lport, const struct tcp_addr *rip,
                                  uint16_t rport)
{
    for (int i = 0; i < NCONN; i++)
        if (conns[i].used && conns[i].lport == lport &&
            conns[i].rport == rport && addr_eq(&conns[i].raddr, rip))
            return &conns[i];
    return NULL;
}

/* RFC 793: a segment that matches no connection gets a RST (unless it is one
 * itself -- RSTs never elicit RSTs). Connection-less send: no state, no
 * retransmit slot. */
static void send_reset(const struct tcp_addr *lip, const struct tcp_addr *rip,
                       uint16_t lport, uint16_t rport,
                       uint32_t seq, uint32_t ack, int with_ack)
{
    struct tcp_hdr h;
    memset(&h, 0, sizeof h);
    h.sport = htons(lport);
    h.dport = htons(rport);
    h.seq = htonl(seq);
    h.ack = htonl(ack);
    h.off = (uint8_t)((sizeof h / 4) << 4);
    h.flags = RST | (with_ack ? ACK : 0);
    h.checksum = htons(tcp_checksum_af(lip, rip, (const uint8_t *)&h,
                                       (int)sizeof h));
    af_send(rip, &h, (uint16_t)sizeof h);
}

static uint16_t alloc_lport(void)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port++;
        if (next_port < 49152) next_port = 49152;
        int busy = 0;
        for (int i = 0; i < NCONN; i++)
            if (conns[i].used && conns[i].lport == p) { busy = 1; break; }
        if (!busy) return p;
    }
    return 0;
}

/* ------------------------------------------------------------------ input */

/* Take the peer's SACK blocks into the scoreboard. Only ranges strictly above
 * snd_una and at or below snd_max mean anything. */
static void take_sack(struct tcp_conn *c, const struct tcp_opts *o)
{
    if (!c->sack_ok) return;                    /* not negotiated: ignore, RFC 2018 */
    for (int i = 0; i < o->n_sack; i++) {
        uint32_t s = o->sack[i].seq, e = o->sack[i].end;
        if (seq_le(e, c->snd_una)) continue;
        if (seq_lt(c->snd_max, e)) continue;    /* claims data we never sent */
        if (seq_lt(s, c->snd_una)) s = c->snd_una;
        if (seq_le(e, s)) continue;
        c->n_sacked = iv_insert(c->sacked, c->n_sacked, NSACK, s, e);
    }
}

static void conn_closed(struct tcp_conn *c)
{
    c->state = CLOSED;
    c->used = 0;
}

void tcp_input_af(const struct tcp_addr *src, const struct tcp_addr *dst,
                  const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)data;
    uint16_t lport = ntohs(h->dport), rport = ntohs(h->sport);
    struct tcp_conn *c = find_conn(lport, src, rport);
    if (!c) {
        /* No connection: validate before answering (a RST in response to
         * garbage is how RST storms start), then follow RFC 793's rules. */
        int hlen0 = (h->off >> 4) * 4;
        if (hlen0 < (int)sizeof *h || hlen0 > len) return;
        if (tcp_checksum_af(src, dst, data, len) != 0) return;
        if (h->flags & RST) return;                 /* never RST a RST */
        if (h->flags & ACK) {
            send_reset(dst, src, lport, rport, ntohl(h->ack), 0, 0);
        } else {
            uint32_t seglen0 = (uint32_t)(len - hlen0) +
                ((h->flags & SYN) ? 1u : 0u) + ((h->flags & FIN) ? 1u : 0u);
            send_reset(dst, src, lport, rport, 0, ntohl(h->seq) + seglen0, 1);
        }
        return;
    }

    uint32_t seg_seq = ntohl(h->seq), seg_ack = ntohl(h->ack);
    uint8_t  flags = h->flags;
    int hlen = (h->off >> 4) * 4;
    if (hlen < (int)sizeof *h || hlen > len) return;
    /* Verify the pseudo-header + segment checksum; a valid segment's ones-
     * complement sum (checksum field included) folds to 0. */
    if (tcp_checksum_af(src, dst, data, len) != 0) return;
    struct tcp_opts opt;
    if (parse_options(data + sizeof *h, hlen - (int)sizeof *h, &opt) != 0)
        return;
    const uint8_t *payload = data + hlen;
    int dlen = len - hlen;

    /* RFC 793: accept a RST only when its seq falls inside the receive window
     * (in SYN_SENT, only an RST that acks our SYN). A blind RST from off-path
     * must not tear down the connection. */
    if (c->state == SYN_SENT) {
        if (flags & RST) {
            if ((flags & ACK) && seg_ack == c->snd_nxt) conn_closed(c);
            return;
        }
        if ((flags & (SYN | ACK)) == (SYN | ACK) && seg_ack == c->snd_nxt) {
            c->rcv_nxt = seg_seq + 1;
            c->read_seq = c->rcv_nxt;           /* align the receive ring base */
            c->rcv_adv = c->rcv_nxt;
            c->snd_una = seg_ack;
            c->snd_end = seg_ack;               /* data starts after the SYN */
            c->snd_nxt = seg_ack;
            c->snd_max = seg_ack;
            /* RFC 7323 §2.2: the window in a SYN/SYN-ACK is NEVER scaled. Only
             * once BOTH sides have sent the option does scaling take effect. */
            c->snd_wnd = ntohs(h->window);
            c->snd_wl1 = seg_seq; c->snd_wl2 = seg_ack;
            /* Negotiation: an option we offered is in force only if the peer
             * echoed it. Anything absent falls back to plain RFC 9293. */
            if (opt.have_ws) {
                c->snd_scale = opt.ws;
            } else {
                c->snd_scale = 0;
                c->rcv_scale = 0;               /* we must not scale either */
            }
            c->sack_ok = opt.sack_perm ? 1 : 0;
            c->ts_ok = opt.have_ts ? 1 : 0;
            if (c->ts_ok) c->ts_recent = opt.tsval;
            c->peer_mss = opt.mss ? opt.mss : DEFAULT_MSS;
            if (c->peer_mss > MSS_ADV) c->peer_mss = MSS_ADV;
            if (c->peer_mss < 88) c->peer_mss = 88;
            c->state = ESTABLISHED;
            c->cwnd = initial_cwnd(c);
            c->ssthresh = 0xFFFFFFFFu;          /* RFC 5681: arbitrarily high */
            rtx_stop(c);
            if (c->ts_ok && opt.tsecr)
                rtt_sample(c, timer_ticks() - (uint64_t)opt.tsecr);
            else if (c->rtt_pending)
                rtt_sample(c, timer_ticks() - c->rtt_tick);
            c->rtt_pending = 0;
            c->rx_tick = timer_ticks();
            send_ack(c);
            tcp_output(c);
        }
        return;
    }

    /* RFC 7323 §5.3 PAWS: a segment whose timestamp predates ts_recent is an
     * old duplicate from a previous incarnation of the sequence space. Drop it
     * and re-announce where we are. RSTs are exempt. */
    if (c->ts_ok && opt.have_ts && !(flags & RST) &&
        (int32_t)(opt.tsval - c->ts_recent) < 0) {
        send_ack(c);
        return;
    }

    uint32_t seglen = (uint32_t)dlen + ((flags & FIN) ? 1u : 0u);
    int acceptable = segment_acceptable(c, seg_seq, seglen);
    if (flags & RST) {
        /* RFC 5961 challenge ACK: only an exact RCV.NXT reset tears down an
         * established connection; an in-window non-exact RST gets challenged. */
        if (seg_seq == c->rcv_nxt) { conn_closed(c); return; }
        if (acceptable) send_ack(c);
        return;
    }
    if (!acceptable) {
        send_ack(c);
        return;
    }
    if (flags & SYN) {                         /* unexpected SYN on this connection */
        send_ack(c);
        return;
    }
    if (!(flags & ACK)) return;                /* ACK is mandatory after handshake */

    /* RFC 7323 §4.3: adopt the peer's timestamp when it is not older than the
     * one we hold and the segment covers the sequence we last acknowledged. */
    if (c->ts_ok && opt.have_ts &&
        (int32_t)(opt.tsval - c->ts_recent) >= 0 &&
        seq_le(seg_seq, c->last_ack_sent))
        c->ts_recent = opt.tsval;

    c->rx_tick = timer_ticks();

    if (seq_lt(c->snd_nxt, seg_ack)) {         /* acknowledges bytes never sent */
        send_ack(c);
        return;
    }

    uint32_t prev_wnd = c->snd_wnd;
    update_send_window(c, seg_seq, seg_ack, ntohs(h->window));
    int had_sack = c->n_sacked;
    take_sack(c, &opt);
    int new_sack = (c->n_sacked != had_sack);

    uint32_t acked = 0;
    if (seq_lt(c->snd_una, seg_ack) && seq_le(seg_ack, c->snd_nxt)) {
        acked = seg_ack - c->snd_una;
        c->snd_una = seg_ack;
        c->n_sacked = iv_trim(c->sacked, c->n_sacked, c->snd_una);
        if (seq_lt(c->snd_nxt, c->snd_una)) c->snd_nxt = c->snd_una;
        /* RFC 6298 §5.3: restart the timer on an ACK that acknowledges new
         * data, or stop it when everything is acknowledged. */
        if (c->snd_una == c->snd_max) rtx_stop(c);
        else                          rtx_start(c);
        c->rtx_retries = 0;
        /* RTT sample. With timestamps every ACK samples (RFC 7323 §4.1);
         * without them, one segment per window is timed and Karn's algorithm
         * (rtt_pending cleared on retransmit) protects the estimator. */
        if (c->ts_ok && opt.have_ts && opt.tsecr)
            rtt_sample(c, timer_ticks() - (uint64_t)opt.tsecr);
        else if (c->rtt_pending && seq_le(c->rtt_seq, seg_ack)) {
            rtt_sample(c, timer_ticks() - c->rtt_tick);
            c->rtt_pending = 0;
        }
    }

    if (acked > 0) {
        c->dup_acks = 0;
        if (c->in_recovery) {
            if (seq_le(c->recover, seg_ack)) {
                /* Full ACK: RFC 5681 §3.2 step 6 -- deflate and leave. */
                c->in_recovery = 0;
                c->cwnd = c->ssthresh;
                c->ca_acked = 0;
            } else {
                /* RFC 6582 partial ACK: one loss repaired, another remains.
                 * Retransmit the new head immediately, deflate by what was
                 * acknowledged, and stay in recovery. */
                if (c->cwnd > acked) c->cwnd -= acked; else c->cwnd = eff_mss(c);
                c->cwnd += eff_mss(c);
                retransmit_head(c);
            }
        } else {
            cc_on_ack(c, acked);
        }
    } else if (dlen == 0 && !(flags & (SYN | FIN)) &&
               seq_lt(c->snd_una, c->snd_max) &&
               (c->snd_wnd == prev_wnd || new_sack)) {
        /* RFC 5681 duplicate ACK: same ack number, no data, no window update,
         * something outstanding. A SACK block that reports new data counts too
         * (RFC 6675) -- it is positive evidence a segment was lost. */
        c->dup_acks++;
        if (c->dup_acks == 3 && !c->in_recovery) {
            cc_enter_recovery(c);
            retransmit_head(c);
        } else if (c->dup_acks > 3 && c->in_recovery) {
            c->cwnd += eff_mss(c);          /* RFC 5681 §3.2 step 4 */
        }
    }

    if (dlen > 0 && (c->state == ESTABLISHED || c->state == FIN_WAIT)) {
        int hole = rx_data(c, seg_seq, payload, dlen);
        /* RFC 1122 4.2.3.2 delayed ACK: at most one full-sized segment may go
         * unacknowledged, and never for longer than DELACK_MAX. Out-of-order
         * data is acknowledged at once -- that dup ACK (with its SACK blocks)
         * is what drives the peer's fast retransmit. */
        c->ack_segs++;
        if (hole || c->ack_segs >= 2 || recv_window(c) == 0)
            send_ack(c);
        else if (c->ack_due == 0)
            c->ack_due = timer_ticks() + DELACK_MAX;
    }
    if (flags & FIN) {
        /* Accept the FIN only when it is in-order: everything up to its seq has
         * been received (after reassembly), so rcv_nxt == the FIN's sequence. */
        uint32_t fin_seq = seg_seq + (uint32_t)dlen;
        if (fin_seq == c->rcv_nxt) {
            c->rcv_nxt += 1;
            c->peer_fin = 1;
            send_ack(c);
            if (c->state == ESTABLISHED) {
                /* Passive close: the app may keep draining (and sending) until
                 * it calls tcp_close, which moves us to LAST_ACK. */
                c->state = CLOSE_WAIT;
            } else if (c->state == FIN_WAIT) {
                if (c->fin_queued && seq_le(c->snd_una, c->snd_end)) {
                    c->state = CLOSING;         /* our FIN is still unacked */
                } else {
                    c->state = TIME_WAIT;
                    c->tw_tick = timer_ticks();
                }
            }
        } else if (c->state == CLOSE_WAIT || c->state == LAST_ACK ||
                   c->state == CLOSING || c->state == TIME_WAIT) {
            /* The peer retransmitted its FIN (our ACK was lost): re-ACK so it
             * can finish; in TIME_WAIT this also restarts the wait. */
            send_ack(c);
            if (c->state == TIME_WAIT)
                c->tw_tick = timer_ticks();
        }
    }

    /* Our FIN acknowledged? It sits at seq snd_end and costs one sequence. */
    if (c->fin_queued && c->fin_sent && seq_lt(c->snd_end, c->snd_una)) {
        if (c->state == CLOSING) {
            c->state = TIME_WAIT;               /* simultaneous close completed */
            c->tw_tick = timer_ticks();
        } else if (c->state == LAST_ACK) {
            conn_closed(c);
            return;
        }
    }

    if (c->used) tcp_output(c);
}

void tcp_input(uint32_t src, const uint8_t *data, uint16_t len)
{
    struct tcp_addr s, d;
    addr_v4(&s, src);
    addr_v4(&d, net_cfg.ip);
    tcp_input_af(&s, &d, data, len);
}

/* ------------------------------------------------------------------ timers */

/* RFC 1191 plateau table, expressed as MSS (plateau MTU - 40). Used when an
 * ICMP "fragmentation needed" arrives without a usable next-hop MTU: step down
 * to the next plateau and resend from snd_una. */
static const uint16_t pmtu_plateau[] = { 1460, 1400, 1360, 1300, 1200, 1060,
                                         976, 812, 508, 468, 256, MIN_MSS };

static uint16_t pmtu_next_lower(uint16_t cur)
{
    for (unsigned i = 0; i < sizeof pmtu_plateau / sizeof pmtu_plateau[0]; i++)
        if (pmtu_plateau[i] < cur) return pmtu_plateau[i];
    return MIN_MSS;
}

void tcp_poll(void)
{
    uint64_t f = net_lock();            /* exclude the RX IRQ while we walk conns[] */
    uint64_t now = timer_ticks();
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used) continue;

        /* Delayed ACK expiry. */
        if (c->ack_due && now >= c->ack_due && c->state != CLOSED)
            send_ack(c);

        /* Zero-window persist (RFC 9293 §3.8.6.1): keep one byte knocking, with
         * its own exponential backoff, until the peer reopens the window. */
        if (c->snd_wnd == 0 && seq_lt(c->snd_nxt, c->snd_end) &&
            (c->state == ESTABLISHED || c->state == CLOSE_WAIT)) {
            if (!c->persist_running) {
                c->persist_running = 1;
                c->persist_shift = 0;
                c->persist_tick = now;
            } else {
                uint64_t p = base_rto(c) << (c->persist_shift > 5 ? 5 : c->persist_shift);
                if (p > RTO_MAX) p = RTO_MAX;
                if (now - c->persist_tick >= p) {
                    send_seg(c, ACK | PSH, c->snd_nxt, 1);
                    if (seq_lt(c->snd_max, c->snd_nxt + 1)) c->snd_max = c->snd_nxt + 1;
                    c->persist_tick = now;
                    if (c->persist_shift < 6) c->persist_shift++;
                }
            }
        } else if (c->persist_running && c->snd_wnd > 0) {
            c->persist_running = 0;
            c->persist_shift = 0;
        }

        /* Retransmission timeout. */
        uint64_t rto = c->rto ? c->rto : RTO_DEFAULT;
        if (c->rtx_running && now - c->rtx_tick >= rto) {
            /* A peer that advertises a zero window is not a lost segment: the
             * persist probes keep knocking forever and the retry cap must not
             * tear the connection down under them (RFC 9293 §3.8.6.1). */
            c->rtx_retries++;
            if (c->rtx_retries > 8) {
                if (!c->persist_running) { conn_closed(c); continue; }
                c->rtx_retries = 8;
            }
            /* RFC 4821-style blackhole backstop: if a path drops our full-sized
             * segments and swallows the ICMP that would have said so, three
             * timeouts in a row is enough evidence to try a smaller segment. */
            if (c->rtx_retries == 3 && c->pmtu_mss > 508)
                c->pmtu_mss = 508;
            if (!c->persist_running) cc_on_rto(c);
            c->rto = rto * 2;                   /* RFC 6298 §5.5 backoff */
            if (c->rto > RTO_MAX) c->rto = RTO_MAX;
            c->rtt_pending = 0;                 /* Karn */
            if (c->state == SYN_SENT) {
                send_seg(c, SYN, c->snd_una, 0);
                c->n_rexmit++;
            } else {
                /* Go back to snd_una and re-send from there under the collapsed
                 * window. fin_sent is cleared so the FIN is resent too. */
                c->snd_nxt = c->snd_una;
                if (c->fin_queued && seq_le(c->snd_una, c->snd_end))
                    c->fin_sent = 0;
                retransmit_head(c);
                if (c->snd_nxt == c->snd_una) {
                    uint32_t adv = hole_end(c) - c->snd_una;
                    uint32_t m = eff_mss(c);
                    if (adv > m) adv = m;
                    if (seq_lt(c->snd_una + adv, c->snd_end + 1))
                        c->snd_nxt = c->snd_una + adv;
                    else
                        c->snd_nxt = c->snd_max;
                }
            }
            c->rtx_tick = now;
        }

        if (c->used && c->state != TIME_WAIT) tcp_output(c);

        /* Backstop: our FIN was acked but the peer's FIN never came -- don't leak
         * the slot (NCONN is small and a browser opens many connections). Only
         * fire after the peer has also gone quiet: a half-closed peer may still
         * be legally streaming data, which refreshes rx_tick on each segment. */
        if (c->state == FIN_WAIT && !c->rtx_running && now - c->rtx_tick > MS_TICKS(FINWAIT_QUIET_MS) &&
            now - c->rx_tick > MS_TICKS(FINWAIT_QUIET_MS))
            conn_closed(c);
        /* TIME_WAIT: a shortened 2MSL (~10 s). alloc_lport() never immediately
         * reuses a local port, and with NCONN slots a full 2MSL would leak
         * connection slots on a browser fetching many objects. */
        if (c->state == TIME_WAIT && now - c->tw_tick >= MS_TICKS(TIME_WAIT_MS))
            conn_closed(c);
        /* LAST_ACK and CLOSING always hold our outstanding FIN, so the
         * retransmit cap above is their backstop; this sweep is defensive. */
        if ((c->state == LAST_ACK || c->state == CLOSING) && !c->rtx_running &&
            c->fin_sent && seq_lt(c->snd_end, c->snd_una))
            conn_closed(c);
    }
    net_unlock(f);
}

/* ------------------------------------------------------------------- API */

int tcp_connect_start_addr(const struct tcp_addr *dst, uint16_t port)
{
    int id = -1;
    uint64_t f = net_lock();                    /* build the conn atomically vs the RX IRQ */
    /* Slot search inside the lock: with a pool, two starts can race the same
     * free slot between the scan and the memset, and the loser would then
     * scribble over a connection that had already sent its SYN. */
    for (int i = 0; i < NCONN; i++) if (!conns[i].used) { id = i; break; }
    if (id < 0) { net_unlock(f); return -1; }
    struct tcp_conn *c = &conns[id];
    memset(c, 0, sizeof *c);
    c->raddr = *dst;
    if (af_local(dst, &c->laddr) != 0) { net_unlock(f); return -1; }
    c->used = 1;
    c->state = SYN_SENT;
    c->lport = alloc_lport();
    if (c->lport == 0) { c->used = 0; net_unlock(f); return -1; }
    c->rport = port;
    uint32_t iss;
    kernel_random_bytes((uint8_t *)&iss, sizeof iss);
    iss ^= (uint32_t)timer_ticks() * 2654435761u ^
           (dst->af == TCP_AF_INET ? dst->a.v4 : 0u) ^
           ((uint32_t)c->lport << 16) ^ port ^ iss_counter++;
    c->snd_una = iss; c->snd_nxt = iss + 1;     /* SYN consumes one seq */
    c->snd_max = iss + 1; c->snd_end = iss + 1;
    c->peer_mss = DEFAULT_MSS;
    c->pmtu_mss = MSS_ADV;
    c->cwnd = 2 * DEFAULT_MSS;
    c->ssthresh = 0xFFFFFFFFu;
    c->snd_wnd = DEFAULT_MSS;
    /* RFC 7323: ask for the largest shift our receive ring can use. The peer
     * either echoes a Window Scale option (scaling is on, both directions get
     * their own shift) or it does not, and we silently fall back to 65535. */
    c->rcv_scale = 0;
    while (((uint32_t)RXBUF >> c->rcv_scale) > 65535u && c->rcv_scale < 14)
        c->rcv_scale++;

    send_seg(c, SYN, iss, 0);
    rtx_start(c);
    c->rto = RTO_DEFAULT;
    c->rtt_pending = 1;
    c->rtt_seq = iss + 1;
    c->rtt_tick = timer_ticks();
    net_unlock(f);
    return id;
}

int tcp_connect_start(uint32_t dst, uint16_t port)
{
    struct tcp_addr a;
    addr_v4(&a, dst);
    return tcp_connect_start_addr(&a, port);
}

/* 1 = ESTABLISHED, 0 = still handshaking, -1 = the open failed (RST, ICMP hard
 * error, or tcp_poll gave up retransmitting the SYN and released the slot).
 * There is no timeout of its own: tcp_poll's retransmit cap is the timeout, and
 * a caller that wants a shorter deadline enforces it itself. */
int tcp_connect_status(int id)
{
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];
    if (!c->used || c->state == CLOSED) return -1;
    return c->state == SYN_SENT ? 0 : 1;
}

int tcp_connect(uint32_t dst, uint16_t port)
{
    int id = tcp_connect_start(dst, port);
    if (id < 0) return -1;
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < MS_TICKS(CONNECT_MS)) {
        net_poll();
        int st = tcp_connect_status(id);
        if (st > 0) return id;
        if (st < 0) return -1;
        net_idle();                              /* sleep to the next tick */
    }
    uint64_t f = net_lock();    /* clear the slot under the same lock as setup */
    conns[id].used = 0;
    net_unlock(f);
    return -1;
}

/* Copy bytes into the send ring and push whatever the windows allow. Never
 * waits and never calls net_poll(), which makes it legal inside a syscall (the
 * int 0x80 gate runs with IF=0) and inside net_poll() itself. Everything on the
 * non-blocking socket path sends through here, from sock_pump(). */
int tcp_send_nb(int id, const void *buf, int len)
{
    if (id < 0 || id >= NCONN || len < 0 || (!buf && len > 0)) return -1;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();
    if (!c->used || (c->state != ESTABLISHED && c->state != CLOSE_WAIT) ||
        c->fin_queued) { net_unlock(f); return -1; }
    if (len == 0) { net_unlock(f); return 0; }
    uint32_t queued = c->snd_end - c->snd_una;
    uint32_t space = queued >= SNDBUF ? 0 : (uint32_t)SNDBUF - queued;
    int n = len > (int)space ? (int)space : len;
    if (n > 0) {
        snd_write(c, c->snd_end, (const uint8_t *)buf, n);
        c->snd_end += (uint32_t)n;
        tcp_output(c);
    }
    net_unlock(f);
    return n;
}

int tcp_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= NCONN || len < 0 || (!buf && len > 0)) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    int remaining = len, sent = 0;

    while (remaining > 0) {
        int n = tcp_send_nb(id, p, remaining);
        if (n < 0) return sent > 0 ? sent : -1;
        if (n == 0) {
            /* The send ring is full: pump until the peer acknowledges enough
             * of it to take more. Report a short write rather than an error --
             * tls.c treats 0 as "would block" and retries. */
            uint64_t start = timer_ticks();
            while (n == 0 && timer_ticks() - start < MS_TICKS(TXWAIT_MS)) {
                net_poll();
                n = tcp_send_nb(id, p, remaining);
                if (n != 0) break;
                net_idle();
            }
            if (n < 0) return sent > 0 ? sent : -1;
            if (n == 0) return sent;
        }
        p += n; remaining -= n; sent += n;
    }
    return sent;
}

void tcp_set_nodelay(int id, int on)
{
    if (id < 0 || id >= NCONN) return;
    uint64_t f = net_lock();
    if (conns[id].used) {
        conns[id].nodelay = on ? 1 : 0;
        if (on) tcp_output(&conns[id]);
    }
    net_unlock(f);
}

/* Readable bytes waiting right now, without consuming any: >0 = that many,
 * 0 = nothing yet but the stream is open, -1 = finished (peer FIN drained, or
 * the connection is gone). */
int tcp_available(int id)
{
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();
    int rc;
    if (!c->used)          rc = -1;
    else if (c->rx_len > 0) rc = c->rx_len;
    else rc = (c->peer_fin || c->state == CLOSED) ? -1 : 0;
    net_unlock(f);
    return rc;
}

int tcp_recv(int id, void *buf, int max)
{
    if (max <= 0) return 0;
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();            /* exclude the RX IRQ's tcp_input */
    int rc;
    if (!c->used) {
        rc = -1;
    } else {
        int avail = c->rx_len;
        if (avail <= 0) {
            if (c->state == CLOSED) c->used = 0;
            rc = (c->peer_fin || c->state == CLOSED) ? -1 : 0;
        } else {
            int n = avail > max ? max : avail;
            uint8_t *out = buf;
            /* Two-part ring drain (RXBUF is a power of two). */
            int space = RXBUF - c->rx_head;          /* bytes before wrap */
            if (n <= space) {
                memcpy(out, c->rx + c->rx_head, (size_t)n);
            } else {
                memcpy(out, c->rx + c->rx_head, (size_t)space);
                memcpy(out + space, c->rx, (size_t)(n - space));
            }
            c->rx_head = (c->rx_head + n) & (RXBUF - 1);
            c->read_seq += (uint32_t)n;              /* keep read_seq == seq of rx_head */
            c->rx_len -= n;
            /* Window update: the sender learns our window only from ACKs (sent on
             * inbound data). After draining a burst, proactively ACK so it doesn't
             * stall on a stale small window. */
            if (c->state == ESTABLISHED &&
                (int)recv_window(c) - c->adv_wnd >= RXBUF / 4)
                send_ack(c);
            rc = n;
        }
    }
    net_unlock(f);
    return rc;
}

void tcp_close(int id)
{
    if (id < 0 || id >= NCONN) return;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();
    if (!c->used) {
        /* nothing to do */
    } else if (c->state == ESTABLISHED || c->state == CLOSE_WAIT) {
        if (!c->fin_queued) {
            c->fin_queued = 1;      /* the FIN takes sequence snd_end, i.e. it
                                     * goes out behind everything still queued */
            tcp_output(c);
        }
    } else {
        c->used = 0;
    }
    net_unlock(f);
}

int tcp_alive(int id)
{
    if (id < 0 || id >= NCONN) return 0;
    /* TIME_WAIT reports not-alive: both FINs are exchanged, so from the app's
     * perspective the connection is over; the slot lingers only to catch
     * retransmitted FINs. */
    return conns[id].used && conns[id].state != CLOSED &&
           conns[id].state != TIME_WAIT;
}

int tcp_get_info(int id, struct tcp_info *out)
{
    if (id < 0 || id >= NCONN || !out) return -1;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();
    int rc = -1;
    if (c->used) {
        memset(out, 0, sizeof *out);
        out->cwnd = c->cwnd;
        out->ssthresh = c->ssthresh;
        out->snd_wnd = c->snd_wnd;
        out->rcv_wnd = recv_window(c);
        out->srtt_ticks = (uint32_t)c->srtt;
        out->rto_ticks = (uint32_t)(c->rto ? c->rto : RTO_DEFAULT);
        out->retransmits = c->n_rexmit;
        out->fast_retransmits = c->n_fast_rexmit;
        out->rto_events = c->n_rto;
        out->mss = (uint16_t)eff_mss(c);
        out->snd_scale = c->snd_scale;
        out->rcv_scale = c->rcv_scale;
        out->sack_ok = (uint8_t)c->sack_ok;
        out->ts_ok = (uint8_t)c->ts_ok;
        out->in_recovery = (uint8_t)c->in_recovery;
        out->state = (uint8_t)c->state;
        rc = 0;
    }
    net_unlock(f);
    return rc;
}

/* icmp.c weak-hook target: an ICMP error quoting one of our segments. RFC
 * 1122 4.2.3.9 treats destination-unreachable codes 0 (net), 1 (host) and
 * 3 (port) as hard errors and aborts the connection; time-exceeded and the
 * soft codes are left to the retransmit timer.
 *
 * Code 4 (fragmentation needed, DF set) is NOT a hard error -- it is RFC 1191
 * path-MTU discovery, and killing the connection on it is how a stack becomes
 * the reason a whole tunnelled path "doesn't work". ip_send always sets DF, so
 * this is a live path. icmp.c does not hand us the next-hop MTU field, so we
 * step down the RFC 1191 plateau table and resend from snd_una. If the ICMP is
 * filtered entirely, tcp_poll's third timeout drops to a 508-byte MSS. */
#define ICMP_TYPE_DEST_UNREACH 3
void tcp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code)
{
    if (type != ICMP_TYPE_DEST_UNREACH)
        return;
    struct tcp_addr a;
    addr_v4(&a, rip);
    struct tcp_conn *c = find_conn(lport, &a, rport);
    if (!c)
        return;
    if (code == 4) {
        uint16_t next = pmtu_next_lower(c->pmtu_mss);
        if (next < c->pmtu_mss) {
            c->pmtu_mss = next;
            c->snd_nxt = c->snd_una;            /* everything in flight was too big */
            c->fin_sent = c->fin_queued ? 0 : c->fin_sent;
            c->n_sacked = 0;
            rtx_start(c);
            tcp_output(c);
        }
        return;
    }
    if (code != 0 && code != 1 && code != 3)
        return;
    int was_syn_sent = (c->state == SYN_SENT);
    c->state = CLOSED;
    /* Keep used=1 so a blocked tcp_recv() reports the failure and frees the
     * slot (same convention as the FIN_WAIT -> CLOSED path). In SYN_SENT
     * there is no reader to notify; free now so tcp_connect() fails fast
     * instead of riding out its handshake timeout. */
    if (was_syn_sent)
        c->used = 0;
}
