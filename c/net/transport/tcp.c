#include <stdint.h>
#include <stddef.h>
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "pit.h"
#include "rng.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* TCP flags */
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSE_WAIT, LAST_ACK,
       CLOSING, TIME_WAIT };

#define NCONN    8
#define RXBUF    65536      /* 64 KiB receive window: a full HTTP body / TLS flight
                            * fits in one window, so the sender streams it without
                            * zero-window stalls. Must be a power of two (ring &). */
#define TXBUF    1460     /* MSS: 1500 MTU - 20 IP - 20 TCP. Caps a segment so it
                          * fits ip_send's 1500-byte pkt[] (was 2048 -> >1480-byte
                          * payloads were silently dropped by ip_send). */
#define DEFAULT_MSS 536   /* RFC 9293 IPv4 send-MSS default when peer omits it */
#define NOOO     16        /* out-of-order reassembly intervals tracked per conn */

/* A received byte range [seq, end) that sits AHEAD of rcv_nxt (a hole precedes
 * it). The list is kept sorted by seq, merged, and non-overlapping. */
struct ooo_seg { uint32_t seq, end; };

struct tcp_conn {
    int      state;
    uint16_t lport, rport;
    uint32_t rip;               /* remote IP, host order */
    uint32_t snd_una, snd_nxt;  /* send sequence space */
    uint32_t snd_wnd;           /* peer-advertised window (no scaling negotiated) */
    uint32_t snd_wl1, snd_wl2;  /* SEG.SEQ/ACK of last accepted window update */
    uint16_t peer_mss;          /* MSS from SYN-ACK, or DEFAULT_MSS */
    uint32_t rcv_nxt;           /* next expected (contiguous) receive seq */
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
    int      adv_wnd;           /* window we last advertised (for drain updates) */

    /* single outstanding (data/SYN/FIN) segment for retransmit */
    uint8_t  tx[TXBUF];
    int      tx_len;            /* payload bytes pending ack (0 = none) */
    uint8_t  tx_flags;
    uint32_t tx_seq;
    uint64_t tx_tick;
    uint64_t rx_tick;           /* last valid inbound segment (FIN_WAIT backstop) */
    int      tx_retries;
    int      tx_probe;          /* persist retransmit while peer window is zero */
    int      close_pending;     /* send FIN after the one-slot TX queue drains */
    /* RFC 6298 RTT estimation, in ticks (fixed point via integer shifts). */
    uint64_t srtt;              /* smoothed RTT (0 = no sample yet) */
    uint64_t rttvar;            /* RTT variation */
    uint64_t rto;               /* current RTO, timeout backoff applied */
    int      tx_rexmitted;      /* outstanding segment was retransmitted (Karn:
                                 * its ACK must not produce an RTT sample) */
    int      dup_acks;          /* consecutive duplicate ACKs (fast retransmit) */
    uint64_t tw_tick;           /* entered TIME_WAIT (slot-release deadline) */
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

/* Ones-complement checksum over the TCP pseudo-header + segment. src/dst are
 * host order; folding (>>16)+(&0xFFFF) matches the on-wire big-endian words. */
static uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += (uint32_t)len;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (len & 1)
        sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Furthest sequence we have any buffered byte for (contiguous end, or the top of
 * the highest out-of-order interval). */
/* Receive window starts at rcv_nxt and ends at the fixed right edge of our
 * seq-indexed ring. OOO bytes already occupy their sequence slots, so they do
 * not move that edge. Without window scaling, the wire value is capped at
 * 65535 even though the ring itself holds 65536 bytes. */
static int recv_window(struct tcp_conn *c)
{
    uint32_t used = c->rcv_nxt - c->read_seq;
    if (used >= RXBUF) return 0;
    uint32_t win = RXBUF - used;
    return win > 65535 ? 65535 : (int)win;
}

/* RFC 9293 receive-window acceptability (SEG.LEN counts FIN). */
static int segment_acceptable(struct tcp_conn *c, uint32_t seq, uint32_t seglen)
{
    uint32_t win = (uint32_t)recv_window(c);
    if (seglen == 0) {
        if (win == 0) return seq == c->rcv_nxt;
        return seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + win);
    }
    if (win == 0) return 0;
    uint32_t last = seq + seglen - 1;
    return (seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + win)) ||
           (seq_le(c->rcv_nxt, last) && seq_lt(last, c->rcv_nxt + win));
}

/* Validate the option list and extract MSS from a SYN. Unknown options are
 * skipped as required; malformed lengths reject the segment. */
static int parse_options(const uint8_t *opt, int len, uint16_t *mss)
{
    *mss = 0;
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
        if (kind == 2) {
            if (olen != 4) return -1;
            *mss = (uint16_t)(((uint16_t)opt[i + 2] << 8) | opt[i + 3]);
            if (*mss == 0) return -1;
        }
        i += olen;
    }
    return 0;
}

/* Write n bytes at the ring slot for sequence s (caller guarantees the whole
 * [s, s+n) range lies within [read_seq, read_seq+RXBUF)). Handles the wrap. */
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
        /* a hole precedes [s,e): merge it into the sorted OOO list */
        int w = 0;
        for (int i = 0; i < c->n_ooo; i++) {
            uint32_t is = c->ooo[i].seq, ie = c->ooo[i].end;
            if (seq_lt(e, is) || seq_lt(ie, s)) {
                c->ooo[w++] = c->ooo[i];     /* disjoint (e==is / ie==s still merge) */
            } else {
                if (seq_lt(is, s)) s = is;   /* overlap/adjacent: grow [s,e) */
                if (seq_lt(e, ie)) e = ie;
            }
        }
        if (w < NOOO) {
            int j = w;
            while (j > 0 && seq_lt(s, c->ooo[j - 1].seq)) { c->ooo[j] = c->ooo[j - 1]; j--; }
            c->ooo[j].seq = s; c->ooo[j].end = e;
            c->n_ooo = w + 1;
        } else {
            c->n_ooo = w;                    /* table full: drop; sender resends */
        }
    }
    c->rx_len = (int)(c->rcv_nxt - c->read_seq);
}

/* Take an inbound data segment into the receive buffer (with reassembly). */
static void rx_data(struct tcp_conn *c, uint32_t seg_seq, const uint8_t *payload, int dlen)
{
    uint32_t s = seg_seq, e = seg_seq + (uint32_t)dlen;
    if (seq_lt(s, c->rcv_nxt)) { payload += (c->rcv_nxt - s); s = c->rcv_nxt; }  /* trim dup prefix */
    uint32_t win_end = c->rcv_nxt + (uint32_t)recv_window(c);
    if (seq_lt(win_end, e)) e = win_end;                                          /* trim beyond buffer */
    if (seq_le(e, s)) return;                                                     /* nothing new */
    ring_write(c, s, payload, (int)(e - s));
    reassemble(c, s, e);
}

/* Build and transmit one segment. `data`/`dlen` is the payload (may be 0). */
static void send_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                     const void *data, int dlen)
{
    uint8_t seg[sizeof(struct tcp_hdr) + 4 + TXBUF];
    struct tcp_hdr *h = (struct tcp_hdr *)seg;
    memset(h, 0, sizeof *h);
    h->sport = htons(c->lport);
    h->dport = htons(c->rport);
    h->seq = htonl(seq);
    h->ack = htonl(c->rcv_nxt);
    int hlen = (int)sizeof *h;
    if (flags & SYN) {
        /* We can reassemble a full Ethernet-MTU segment. Advertising MSS avoids
         * making the peer fall back to IPv4's conservative 536-byte default. */
        seg[hlen++] = 2; seg[hlen++] = 4;
        seg[hlen++] = (uint8_t)(TXBUF >> 8); seg[hlen++] = (uint8_t)TXBUF;
    }
    h->off = (uint8_t)((hlen / 4) << 4);
    h->flags = flags;
    int win = recv_window(c);
    h->window = htons((uint16_t)win);
    c->adv_wnd = win;                       /* remember what the peer now believes */
    if (dlen > 0)
        memcpy(seg + hlen, data, (size_t)dlen);
    h->checksum = 0;
    int total = hlen + dlen;
    h->checksum = htons(tcp_checksum(net_cfg.ip, c->rip, seg, total));
    ip_send(c->rip, IP_PROTO_TCP, seg, (uint16_t)total);
}

#define RTO_DEFAULT 50      /* 0.5 s before the first RTT sample */
#define RTO_MIN     10      /* RFC 6298 lower bound (100 ms at 100 Hz) */
#define RTO_MAX     6000    /* RFC 6298 upper bound (60 s) */

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

/* Fold one RTT sample into the estimators (Jacobson/Karels shifts). The caller
 * applies Karn's algorithm: only ACKs of never-retransmitted segments sample. */
static void rtt_sample(struct tcp_conn *c, uint64_t rtt)
{
    if (rtt == 0) rtt = 1;
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

/* Remember the segment that consumes sequence space, for retransmit. */
static void arm_retransmit(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                           const void *data, int dlen)
{
    c->tx_flags = flags;
    c->tx_seq = seq;
    c->tx_len = dlen;
    if (dlen > 0) memcpy(c->tx, data, (size_t)dlen);
    c->tx_tick = timer_ticks();
    c->tx_retries = 0;
    c->tx_probe = 0;
    c->rto = base_rto(c);
    c->tx_rexmitted = 0;
    c->dup_acks = 0;
}

static void start_fin(struct tcp_conn *c)
{
    uint32_t seq = c->snd_nxt;
    send_seg(c, FIN | ACK, seq, NULL, 0);
    arm_retransmit(c, FIN | ACK, seq, NULL, 0);
    if (c->snd_wnd == 0) c->tx_probe = 1;
    c->snd_nxt += 1;
    /* Active close waits for the peer's FIN; answering the peer's FIN waits
     * only for the ACK of ours. */
    c->state = (c->state == CLOSE_WAIT) ? LAST_ACK : FIN_WAIT;
}

/* Bytes the peer currently permits beyond data already in flight. */
static uint32_t send_available(struct tcp_conn *c)
{
    uint32_t flight = c->snd_nxt - c->snd_una;
    return c->snd_wnd > flight ? c->snd_wnd - flight : 0;
}

static void update_send_window(struct tcp_conn *c, uint32_t seg_seq,
                               uint32_t seg_ack, uint16_t seg_wnd)
{
    if (!seq_le(c->snd_una, seg_ack) || !seq_le(seg_ack, c->snd_nxt)) return;
    if (seq_lt(c->snd_wl1, seg_seq) ||
        (c->snd_wl1 == seg_seq && seq_le(c->snd_wl2, seg_ack))) {
        c->snd_wnd = seg_wnd;
        c->snd_wl1 = seg_seq;
        c->snd_wl2 = seg_ack;
        /* A persist probe becomes an ordinary outstanding byte as soon as the
         * window reopens. Make tcp_poll retransmit it promptly. */
        if (c->tx_probe && c->snd_wnd > 0) {
            c->tx_probe = 0;
            c->tx_retries = 0;
            uint64_t now = timer_ticks(), r = base_rto(c);
            c->tx_tick = now >= r ? now - r : 0;
        }
    }
}

static struct tcp_conn *find_conn(uint16_t lport, uint32_t rip, uint16_t rport)
{
    for (int i = 0; i < NCONN; i++)
        if (conns[i].used && conns[i].lport == lport &&
            conns[i].rip == rip && conns[i].rport == rport)
            return &conns[i];
    return NULL;
}

/* RFC 793: a segment that matches no connection gets a RST (unless it is one
 * itself -- RSTs never elicit RSTs). Connection-less send: no state, no
 * retransmit slot. */
static void send_reset(uint32_t rip, uint16_t lport, uint16_t rport,
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
    h.checksum = htons(tcp_checksum(net_cfg.ip, rip, (const uint8_t *)&h,
                                    (int)sizeof h));
    ip_send(rip, IP_PROTO_TCP, &h, (uint16_t)sizeof h);
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

void tcp_input(uint32_t src, const uint8_t *data, uint16_t len)
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
        if (tcp_checksum(src, net_cfg.ip, data, len) != 0) return;
        if (h->flags & RST) return;                 /* never RST a RST */
        if (h->flags & ACK) {
            send_reset(src, lport, rport, ntohl(h->ack), 0, 0);
        } else {
            uint32_t seglen0 = (uint32_t)(len - hlen0) +
                ((h->flags & SYN) ? 1u : 0u) + ((h->flags & FIN) ? 1u : 0u);
            send_reset(src, lport, rport, 0, ntohl(h->seq) + seglen0, 1);
        }
        return;
    }

    uint32_t seg_seq = ntohl(h->seq), seg_ack = ntohl(h->ack);
    uint8_t  flags = h->flags;
    int hlen = (h->off >> 4) * 4;
    if (hlen < (int)sizeof *h || hlen > len) return;
    /* Verify the pseudo-header + segment checksum; a valid segment's ones-
     * complement sum (checksum field included) folds to 0. */
    if (tcp_checksum(src, net_cfg.ip, data, len) != 0) return;
    uint16_t offered_mss;
    if (parse_options(data + sizeof *h, hlen - (int)sizeof *h, &offered_mss) != 0)
        return;
    const uint8_t *payload = data + hlen;
    int dlen = len - hlen;

    /* RFC 793: accept a RST only when its seq falls inside the receive window
     * (in SYN_SENT, only an RST that acks our SYN). A blind RST from off-path
     * must not tear down the connection. */
    if (c->state == SYN_SENT) {
        if (flags & RST) {
            if ((flags & ACK) && seg_ack == c->snd_nxt)
                { c->state = CLOSED; c->used = 0; }
            return;
        }
        if ((flags & (SYN | ACK)) == (SYN | ACK) && seg_ack == c->snd_nxt) {
            c->rcv_nxt = seg_seq + 1;
            c->read_seq = c->rcv_nxt;           /* align the receive ring base */
            c->snd_una = seg_ack;
            c->snd_wnd = ntohs(h->window);
            c->snd_wl1 = seg_seq; c->snd_wl2 = seg_ack;
            c->peer_mss = offered_mss ? offered_mss : DEFAULT_MSS;
            if (c->peer_mss > TXBUF) c->peer_mss = TXBUF;
            if (c->peer_mss == 0) c->peer_mss = 1;
            c->tx_len = 0; c->tx_flags = 0;     /* SYN acked */
            if (!c->tx_rexmitted)               /* handshake RTT is a valid sample */
                rtt_sample(c, timer_ticks() - c->tx_tick);
            c->state = ESTABLISHED;
            c->rx_tick = timer_ticks();
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
        }
        return;
    }

    uint32_t seglen = (uint32_t)dlen + ((flags & FIN) ? 1u : 0u);
    int acceptable = segment_acceptable(c, seg_seq, seglen);
    if (flags & RST) {
        /* RFC 5961 challenge ACK: only an exact RCV.NXT reset tears down an
         * established connection; an in-window non-exact RST gets challenged. */
        if (seg_seq == c->rcv_nxt) { c->state = CLOSED; c->used = 0; return; }
        if (acceptable) send_seg(c, ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (!acceptable) {
        send_seg(c, ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (flags & SYN) {                         /* unexpected SYN on this connection */
        send_seg(c, ACK, c->snd_nxt, NULL, 0);
        return;
    }
    if (!(flags & ACK)) return;                /* ACK is mandatory after handshake */

    c->rx_tick = timer_ticks();

    /* Established (and closing states): ack our outstanding data, take theirs.
     * Single outstanding segment, so snd_nxt is the end of everything we've sent
     * (data + the +1 for SYN/FIN); a cumulative ACK reaching it frees the slot. */
    if (flags & ACK) {
        if (seq_lt(c->snd_nxt, seg_ack)) {     /* acknowledges bytes never sent */
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
            return;
        }
        uint32_t prev_wnd = c->snd_wnd;
        update_send_window(c, seg_seq, seg_ack, ntohs(h->window));
        int advanced = 0;
        if (seq_lt(c->snd_una, seg_ack) && seq_le(seg_ack, c->snd_nxt)) {
            c->snd_una = seg_ack;
            advanced = 1;
        }
        /* RFC 5681 duplicate ACK: ack number unchanged, no data, no window
         * update, a segment outstanding. The third one retransmits that
         * segment at once -- exact under the single-outstanding model. The RTO
         * backoff is reset (not doubled, unlike a timeout) and the segment is
         * marked retransmitted so its eventual ACK cannot be sampled (Karn). */
        if (advanced) {
            c->dup_acks = 0;
        } else if (dlen == 0 && !(flags & (SYN | FIN)) && c->tx_flags &&
                   c->snd_wnd == prev_wnd && ++c->dup_acks >= 3) {
            c->dup_acks = 0;
            c->tx_tick = timer_ticks();
            c->tx_rexmitted = 1;
            c->rto = base_rto(c);
            send_seg(c, c->tx_flags, c->tx_seq, c->tx_len ? c->tx : NULL, c->tx_len);
        }
        /* Free the retransmit slot only when the ACK covers exactly the whole
         * outstanding segment; an ACK beyond snd_nxt is illegal and must not
         * silently drop an unacknowledged segment (hole in the byte stream). */
        if (c->tx_flags && seg_ack == c->snd_nxt) {
            if (!c->tx_rexmitted)
                rtt_sample(c, timer_ticks() - c->tx_tick);
            c->tx_len = 0; c->tx_flags = 0; c->tx_probe = 0;
            if (c->close_pending &&
                (c->state == ESTABLISHED || c->state == CLOSE_WAIT)) {
                c->close_pending = 0;
                start_fin(c);
            } else if (c->state == CLOSING) {
                c->state = TIME_WAIT;           /* simultaneous close completed */
                c->tw_tick = timer_ticks();
            } else if (c->state == LAST_ACK) {
                /* The app already closed and everything is acknowledged. */
                c->state = CLOSED; c->used = 0;
            }
        }
    }
    if (dlen > 0 && (c->state == ESTABLISHED || c->state == FIN_WAIT)) {
        rx_data(c, seg_seq, payload, dlen);
        send_seg(c, ACK, c->snd_nxt, NULL, 0);  /* cumulative ack (dup-ack on a hole) */
    }
    if (flags & FIN) {
        /* Accept the FIN only when it is in-order: everything up to its seq has
         * been received (after reassembly), so rcv_nxt == the FIN's sequence. */
        uint32_t fin_seq = seg_seq + (uint32_t)dlen;
        if (fin_seq == c->rcv_nxt) {
            c->rcv_nxt += 1;
            c->peer_fin = 1;
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
            if (c->state == ESTABLISHED) {
                /* Passive close: the app may keep draining (and sending) until
                 * it calls tcp_close, which moves us to LAST_ACK. */
                c->state = CLOSE_WAIT;
            } else if (c->state == FIN_WAIT) {
                if (c->tx_flags) {
                    c->state = CLOSING;         /* simultaneous close: our FIN
                                                 * is still unacknowledged */
                } else {
                    c->state = TIME_WAIT;
                    c->tw_tick = timer_ticks();
                }
            }
        } else if (c->state == CLOSE_WAIT || c->state == LAST_ACK ||
                   c->state == CLOSING || c->state == TIME_WAIT) {
            /* The peer retransmitted its FIN (our ACK was lost): re-ACK so it
             * can finish; in TIME_WAIT this also restarts the wait. */
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
            if (c->state == TIME_WAIT)
                c->tw_tick = timer_ticks();
        }
    }
}

void tcp_poll(void)
{
    uint64_t f = net_lock();            /* exclude the RX IRQ while we walk conns[] */
    uint64_t now = timer_ticks();
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used) continue;
        /* Persist probes keep their own exponential backoff (on top of the
         * dynamic base RTO); ordinary segments use the conn's current RTO,
         * which doubles on every timeout retransmission. */
        uint64_t rto;
        if (c->tx_probe) {
            int shift = c->tx_retries > 5 ? 5 : c->tx_retries;
            rto = base_rto(c) << shift;
            if (rto > RTO_MAX) rto = RTO_MAX;
        } else {
            rto = c->rto ? c->rto : RTO_DEFAULT;
        }
        if (c->tx_flags && now - c->tx_tick >= rto) {
            if (c->tx_probe) {
                if (c->tx_retries < 6) c->tx_retries++;
            } else if (++c->tx_retries > 8) {
                c->state = CLOSED; c->used = 0; continue;
            }
            send_seg(c, c->tx_flags, c->tx_seq, c->tx_len ? c->tx : NULL, c->tx_len);
            c->tx_tick = now;
            c->tx_rexmitted = 1;                /* Karn: no sample from its ACK */
            if (!c->tx_probe) {
                c->rto = rto * 2;               /* RFC 6298 backoff on timeout */
                if (c->rto > RTO_MAX) c->rto = RTO_MAX;
            }
        }
        /* Backstop: our FIN was acked but the peer's FIN never came -- don't leak
         * the slot (NCONN is small and a browser opens many connections). Only
         * fire after the peer has also gone quiet: a half-closed peer may still
         * be legally streaming data, which refreshes rx_tick on each segment. */
        if (c->state == FIN_WAIT && c->tx_flags == 0 && now - c->tx_tick > 200 &&
            now - c->rx_tick > 200)
            { c->state = CLOSED; c->used = 0; }
        /* TIME_WAIT: a shortened 2MSL (~10 s). alloc_lport() never immediately
         * reuses a local port, and with NCONN=8 slots a full 2MSL would leak
         * connection slots on a browser fetching many objects. */
        if (c->state == TIME_WAIT && now - c->tw_tick >= 1000)
            { c->state = CLOSED; c->used = 0; }
        /* LAST_ACK and CLOSING always hold our outstanding FIN, so the
         * retransmit cap above is their backstop; this sweep is defensive. */
        if ((c->state == LAST_ACK || c->state == CLOSING) && c->tx_flags == 0)
            { c->state = CLOSED; c->used = 0; }
    }
    net_unlock(f);
}

int tcp_connect(uint32_t dst, uint16_t port)
{
    int id = -1;
    for (int i = 0; i < NCONN; i++) if (!conns[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct tcp_conn *c = &conns[id];
    uint64_t f = net_lock();                    /* build the conn atomically vs the RX IRQ */
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->state = SYN_SENT;
    c->lport = alloc_lport();
    if (c->lport == 0) { c->used = 0; net_unlock(f); return -1; }
    c->rip = dst; c->rport = port;
    uint32_t iss;
    kernel_random_bytes((uint8_t *)&iss, sizeof iss);
    iss ^= (uint32_t)timer_ticks() * 2654435761u ^ dst ^
           ((uint32_t)c->lport << 16) ^ port ^ iss_counter++;
    c->snd_una = iss; c->snd_nxt = iss + 1;     /* SYN consumes one seq */
    c->peer_mss = DEFAULT_MSS;

    send_seg(c, SYN, iss, NULL, 0);
    arm_retransmit(c, SYN, iss, NULL, 0);
    net_unlock(f);

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 500) {        /* ~5 s */
        net_poll();
        if (c->state == ESTABLISHED) return id;
        if (!c->used) return -1;
        net_idle();                                  /* sleep to the next tick; don't peg the host CPU */
    }
    f = net_lock();             /* clear the slot under the same lock as setup */
    c->used = 0;
    net_unlock(f);
    return -1;
}

int tcp_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= NCONN || len < 0 || (!buf && len > 0)) return -1;
    struct tcp_conn *c = &conns[id];
    const uint8_t *p = (const uint8_t *)buf;
    int remaining = len, sent = 0;

    while (remaining > 0) {
        /* There is deliberately one retransmission slot. Never overwrite a
         * previous call's final segment or a close/persist segment. */
        uint64_t start = timer_ticks();
        while (c->used && c->tx_flags && timer_ticks() - start < 800) {
            net_poll();
            if (!c->tx_flags) break;
            net_idle();
        }
        if (!c->used || c->tx_flags) return sent > 0 ? sent : -1;

        uint64_t f = net_lock();
        if (!c->used || c->state != ESTABLISHED) { net_unlock(f); return sent > 0 ? sent : -1; }
        uint32_t avail = send_available(c);
        int chunk;
        if (avail == 0) {
            /* RFC 9293 zero-window probe: queue one byte of new data. It stays
             * in the retransmit slot with exponential persist backoff until the
             * peer reports a reopened window. */
            chunk = 1;
        } else {
            uint32_t cap = avail;
            if (cap > TXBUF) cap = TXBUF;
            if (cap > c->peer_mss) cap = c->peer_mss;
            chunk = remaining > (int)cap ? (int)cap : remaining;
        }
        uint32_t seq = c->snd_nxt;
        send_seg(c, PSH | ACK, seq, p, chunk);
        arm_retransmit(c, PSH | ACK, seq, p, chunk);
        if (avail == 0) c->tx_probe = 1;
        c->snd_nxt += (uint32_t)chunk;
        net_unlock(f);
        p += chunk; remaining -= chunk; sent += chunk;
    }

    return sent;
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
            /* Two-part ring drain (RXBUF is 2^16, so & (RXBUF-1) == % RXBUF). */
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
            if (c->state == ESTABLISHED && recv_window(c) - c->adv_wnd >= RXBUF / 4)
                send_seg(c, ACK, c->snd_nxt, NULL, 0);
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
        if (c->tx_flags) {
            c->close_pending = 1;         /* do not overwrite unacked payload */
        } else {
            start_fin(c);                 /* ESTABLISHED->FIN_WAIT, CLOSE_WAIT->LAST_ACK */
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
     * perspective the connection is over (same as the old direct-to-CLOSED
     * path); the slot lingers only to catch retransmitted FINs. */
    return conns[id].used && conns[id].state != CLOSED &&
           conns[id].state != TIME_WAIT;
}

/* icmp.c weak-hook target: an ICMP error quoting one of our segments. RFC
 * 1122 4.2.3.9 treats destination-unreachable codes 0 (net), 1 (host),
 * 3 (port) and 4 (fragmentation needed) as hard errors and aborts the
 * connection; time-exceeded and the soft codes are left to the retransmit
 * timer. Mirroring icmp.h's values locally keeps tcp.c self-contained (the
 * host white-box test compiles it without the ip-layer headers). */
#define ICMP_TYPE_DEST_UNREACH 3
void tcp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code)
{
    if (type != ICMP_TYPE_DEST_UNREACH)
        return;
    if (code != 0 && code != 1 && code != 3 && code != 4)
        return;
    struct tcp_conn *c = find_conn(lport, rip, rport);
    if (!c)
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
