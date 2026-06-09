#include <stdint.h>
#include <stddef.h>
#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* TCP flags */
#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT, CLOSING, TIME_WAIT };

#define NCONN    8
#define RXBUF    65536      /* 64 KiB receive window: a full HTTP body / TLS flight
                            * fits in one window, so the sender streams it without
                            * zero-window stalls. Must be a power of two (ring &). */
#define TXBUF    1460     /* MSS: 1500 MTU - 20 IP - 20 TCP. Caps a segment so it
                          * fits ip_send's 1500-byte pkt[] (was 2048 -> >1480-byte
                          * payloads were silently dropped by ip_send). */
#define NOOO     16        /* out-of-order reassembly intervals tracked per conn */

/* A received byte range [seq, end) that sits AHEAD of rcv_nxt (a hole precedes
 * it). The list is kept sorted by seq, merged, and non-overlapping. */
struct ooo_seg { uint32_t seq, end; };

struct tcp_conn {
    int      state;
    uint16_t lport, rport;
    uint32_t rip;               /* remote IP, host order */
    uint32_t snd_una, snd_nxt;  /* send sequence space */
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
    int      tx_retries;
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
static uint32_t rx_furthest(struct tcp_conn *c)
{
    return c->n_ooo ? c->ooo[c->n_ooo - 1].end : c->rcv_nxt;
}

/* Free receive-buffer space = window we can advertise (accounts for OOO bytes
 * already parked in the ring). */
static int rx_free(struct tcp_conn *c)
{
    int occ = (int)(rx_furthest(c) - c->read_seq);
    int f = RXBUF - occ;
    return f < 0 ? 0 : f;
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
    uint32_t win_end = c->read_seq + RXBUF;
    if (seq_lt(win_end, e)) e = win_end;                                          /* trim beyond buffer */
    if (seq_le(e, s)) return;                                                     /* nothing new */
    ring_write(c, s, payload, (int)(e - s));
    reassemble(c, s, e);
}

/* Build and transmit one segment. `data`/`dlen` is the payload (may be 0). */
static void send_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                     const void *data, int dlen)
{
    uint8_t seg[sizeof(struct tcp_hdr) + TXBUF];
    struct tcp_hdr *h = (struct tcp_hdr *)seg;
    memset(h, 0, sizeof *h);
    h->sport = htons(c->lport);
    h->dport = htons(c->rport);
    h->seq = htonl(seq);
    h->ack = htonl(c->rcv_nxt);
    h->off = (uint8_t)((sizeof(struct tcp_hdr) / 4) << 4);
    h->flags = flags;
    int win = rx_free(c);
    if (win > 65535) win = 65535;
    h->window = htons((uint16_t)win);
    c->adv_wnd = win;                       /* remember what the peer now believes */
    if (dlen > 0)
        memcpy(seg + sizeof *h, data, (size_t)dlen);
    h->checksum = 0;
    int total = (int)sizeof *h + dlen;
    h->checksum = htons(tcp_checksum(net_cfg.ip, c->rip, seg, total));
    ip_send(c->rip, IP_PROTO_TCP, seg, (uint16_t)total);
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
}

static struct tcp_conn *find_conn(uint16_t lport, uint32_t rip, uint16_t rport)
{
    for (int i = 0; i < NCONN; i++)
        if (conns[i].used && conns[i].lport == lport &&
            conns[i].rip == rip && conns[i].rport == rport)
            return &conns[i];
    return NULL;
}

void tcp_input(uint32_t src, const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)data;
    uint16_t lport = ntohs(h->dport), rport = ntohs(h->sport);
    struct tcp_conn *c = find_conn(lport, src, rport);
    if (!c)
        return;

    uint32_t seg_seq = ntohl(h->seq), seg_ack = ntohl(h->ack);
    uint8_t  flags = h->flags;
    int hlen = (h->off >> 4) * 4;
    if (hlen < (int)sizeof *h || hlen > len) return;
    const uint8_t *payload = data + hlen;
    int dlen = len - hlen;

    if (flags & RST) { c->state = CLOSED; c->used = 0; return; }

    if (c->state == SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && seg_ack == c->snd_nxt) {
            c->rcv_nxt = seg_seq + 1;
            c->read_seq = c->rcv_nxt;           /* align the receive ring base */
            c->snd_una = seg_ack;
            c->tx_len = 0; c->tx_flags = 0;     /* SYN acked */
            c->state = ESTABLISHED;
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
        }
        return;
    }

    /* Established (and closing states): ack our outstanding data, take theirs.
     * Single outstanding segment, so snd_nxt is the end of everything we've sent
     * (data + the +1 for SYN/FIN); a cumulative ACK reaching it frees the slot. */
    if (flags & ACK) {
        if (seq_lt(c->snd_una, seg_ack) && seq_le(seg_ack, c->snd_nxt))
            c->snd_una = seg_ack;
        if (c->tx_flags && seq_le(c->snd_nxt, seg_ack)) { c->tx_len = 0; c->tx_flags = 0; }
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
            if (c->state == FIN_WAIT) { c->state = CLOSED; c->used = 0; }
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
        if (c->tx_flags && now - c->tx_tick >= 50) {   /* ~0.5 s RTO */
            if (++c->tx_retries > 8) { c->state = CLOSED; c->used = 0; continue; }
            send_seg(c, c->tx_flags, c->tx_seq, c->tx_len ? c->tx : NULL, c->tx_len);
            c->tx_tick = now;
        }
        /* Backstop: our FIN was acked but the peer's FIN never came -- don't leak
         * the slot (NCONN is small and a browser opens many connections). */
        if (c->state == FIN_WAIT && c->tx_flags == 0 && now - c->tx_tick > 200)
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
    c->lport = next_port++;
    if (next_port == 0) next_port = 49152;
    c->rip = dst; c->rport = port;
    uint32_t iss = (uint32_t)timer_ticks() * 2654435761u + iss_counter++;
    c->snd_una = iss; c->snd_nxt = iss + 1;     /* SYN consumes one seq */

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
    c->used = 0;
    return -1;
}

int tcp_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= NCONN || len < 0) return -1;
    struct tcp_conn *c = &conns[id];
    const uint8_t *p = (const uint8_t *)buf;
    int remaining = len, sent = 0;

    do {
        uint64_t f = net_lock();
        if (!c->used || c->state != ESTABLISHED) { net_unlock(f); return sent > 0 ? sent : -1; }
        int chunk = remaining > TXBUF ? TXBUF : remaining;
        if (chunk == 0) { net_unlock(f); break; }
        uint32_t seq = c->snd_nxt;
        send_seg(c, PSH | ACK, seq, p, chunk);
        arm_retransmit(c, PSH | ACK, seq, p, chunk);
        c->snd_nxt += (uint32_t)chunk;
        net_unlock(f);
        p += chunk; remaining -= chunk; sent += chunk;

        /* Single outstanding segment: before sending the next chunk, wait for
         * this one's ACK (pumping net_poll, IF=1). The final chunk returns
         * without waiting -- the caller's recv loop collects its ACK -- so the
         * common <=MSS request keeps its old one-shot fast path. */
        if (remaining > 0) {
            uint64_t start = timer_ticks();
            while (timer_ticks() - start < 800) {   /* ~8 s */
                net_poll();
                if (!c->used) return sent;
                if (c->tx_flags == 0) break;        /* acked */
                net_idle();
            }
            if (c->tx_flags) return sent;           /* timed out: partial send */
        }
    } while (remaining > 0);

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
            if (c->state == ESTABLISHED && rx_free(c) - c->adv_wnd >= RXBUF / 4)
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
    if (!c->used || c->state != ESTABLISHED) {
        if (c->used) c->used = 0;
    } else {
        uint32_t seq = c->snd_nxt;
        send_seg(c, FIN | ACK, seq, NULL, 0);
        arm_retransmit(c, FIN | ACK, seq, NULL, 0);
        c->snd_nxt += 1;
        c->state = FIN_WAIT;
    }
    net_unlock(f);
}

int tcp_alive(int id)
{
    if (id < 0 || id >= NCONN) return 0;
    return conns[id].used && conns[id].state != CLOSED;
}
