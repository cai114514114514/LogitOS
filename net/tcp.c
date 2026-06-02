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
#define RXBUF    16384
#define TXBUF    2048

struct tcp_conn {
    int      state;
    uint16_t lport, rport;
    uint32_t rip;               /* remote IP, host order */
    uint32_t snd_una, snd_nxt;  /* send sequence space */
    uint32_t rcv_nxt;           /* next expected receive seq */
    int      peer_fin;          /* peer sent FIN (no more data coming) */

    uint8_t  rx[RXBUF];
    int      rx_head, rx_tail;  /* consumed .. filled */

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

static int rx_free(struct tcp_conn *c) { return RXBUF - (c->rx_tail - c->rx_head); }

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
    h->window = htons((uint16_t)(rx_free(c) > 65535 ? 65535 : rx_free(c)));
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
            c->snd_una = seg_ack;
            c->tx_len = 0; c->tx_flags = 0;     /* SYN acked */
            c->state = ESTABLISHED;
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
        }
        return;
    }

    /* Established (and closing states): ack our outstanding data, take theirs. */
    if (flags & ACK) {
        if (seg_ack == c->snd_nxt) { c->snd_una = seg_ack; c->tx_len = 0; c->tx_flags = 0; }
    }
    if (dlen > 0 && c->state == ESTABLISHED) {
        if (seg_seq == c->rcv_nxt && rx_free(c) >= dlen) {
            for (int i = 0; i < dlen; i++) c->rx[(c->rx_tail++) % RXBUF] = payload[i];
            c->rcv_nxt += dlen;
        }
        send_seg(c, ACK, c->snd_nxt, NULL, 0);  /* ack in-order, or dup-ack to nudge */
    }
    if (flags & FIN) {
        if (seg_seq + dlen == c->rcv_nxt) {     /* in-order FIN */
            c->rcv_nxt += 1;
            c->peer_fin = 1;
            send_seg(c, ACK, c->snd_nxt, NULL, 0);
            /* If we already closed (client done), the slot can be released now;
             * skipping TIME_WAIT is fine for an outbound-only client. */
            if (c->state == FIN_WAIT) { c->state = CLOSED; c->used = 0; }
        }
    }
}

void tcp_poll(void)
{
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
}

int tcp_connect(uint32_t dst, uint16_t port)
{
    int id = -1;
    for (int i = 0; i < NCONN; i++) if (!conns[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct tcp_conn *c = &conns[id];
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

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 500) {        /* ~5 s */
        net_poll();
        if (c->state == ESTABLISHED) return id;
        if (!c->used) return -1;
        for (volatile int d = 0; d < 200000; d++) ;
    }
    c->used = 0;
    return -1;
}

int tcp_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];
    if (!c->used || c->state != ESTABLISHED) return -1;
    if (len > TXBUF) len = TXBUF;
    uint32_t seq = c->snd_nxt;
    send_seg(c, PSH | ACK, seq, buf, len);
    arm_retransmit(c, PSH | ACK, seq, buf, len);
    c->snd_nxt += len;
    return len;
}

int tcp_recv(int id, void *buf, int max)
{
    if (id < 0 || id >= NCONN) return -1;
    struct tcp_conn *c = &conns[id];
    if (!c->used) return -1;
    int avail = c->rx_tail - c->rx_head;
    if (avail <= 0)
        return (c->peer_fin || c->state == CLOSED) ? -1 : 0;
    int n = avail > max ? max : avail;
    uint8_t *out = buf;
    for (int i = 0; i < n; i++) out[i] = c->rx[(c->rx_head++) % RXBUF];
    return n;
}

void tcp_close(int id)
{
    if (id < 0 || id >= NCONN) return;
    struct tcp_conn *c = &conns[id];
    if (!c->used || c->state != ESTABLISHED) { if (c->used) { c->used = 0; } return; }
    uint32_t seq = c->snd_nxt;
    send_seg(c, FIN | ACK, seq, NULL, 0);
    arm_retransmit(c, FIN | ACK, seq, NULL, 0);
    c->snd_nxt += 1;
    c->state = FIN_WAIT;
}

int tcp_alive(int id)
{
    if (id < 0 || id >= NCONN) return 0;
    return conns[id].used && conns[id].state != CLOSED;
}
