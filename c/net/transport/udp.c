#include <stdint.h>
#include <stddef.h>
#include "udp.h"
#include "ip.h"
#include "icmp.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint16_t checksum;      /* zero is accepted on IPv4; Aether always sends one */
} __attribute__((packed));

#define NUDP       8        /* sockets (DNS, DHCP, ...); port 68 is DHCP's */
#define UDP_QUEUES 4        /* received datagrams buffered per socket */
#define UDP_SLOT   1500     /* per-datagram buffer (full Ethernet MTU) */

struct udp_sock {
    int      used;
    uint16_t port;
    int      err;                       /* ICMP error pending report */
    uint32_t drops;                     /* queue-full drops */
    int      qhead;                     /* oldest datagram */
    int      qcount;
    uint16_t qlen[UDP_QUEUES];
    uint32_t qsrc[UDP_QUEUES];
    uint16_t qsport[UDP_QUEUES];
    uint8_t  q[UDP_QUEUES][UDP_SLOT];
};

static struct udp_sock socks[NUDP];

static uint16_t udp_checksum(uint32_t src, uint32_t dst,
                             const uint8_t *datagram, uint16_t len)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_PROTO_UDP;
    sum += len;
    for (uint16_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)datagram[i] << 8) | datagram[i + 1];
    if (len & 1) sum += (uint32_t)datagram[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static struct udp_sock *find_sock(uint16_t port)
{
    for (int i = 0; i < NUDP; i++)
        if (socks[i].used && socks[i].port == port)
            return &socks[i];
    return NULL;
}

int udp_bind(uint16_t port)
{
    uint64_t f = net_lock();        /* udp_input runs from the RX IRQ */
    int rc = -1, free = -1;
    for (int i = 0; i < NUDP; i++) {
        if (socks[i].used && socks[i].port == port)
            goto out;                       /* one owner per port */
        if (!socks[i].used && free < 0)
            free = i;
    }
    if (free >= 0) {
        memset(&socks[free], 0, sizeof socks[free]);
        socks[free].used = 1;
        socks[free].port = port;
        rc = free;
    }
out:
    net_unlock(f);
    return rc;
}

void udp_close(int sock)
{
    if (sock < 0 || sock >= NUDP)
        return;
    uint64_t f = net_lock();
    socks[sock].used = 0;
    net_unlock(f);
}

int udp_recv(int sock, void *buf, int max, uint32_t *src, uint16_t *sport)
{
    if (sock < 0 || sock >= NUDP || max < 0 || (!buf && max > 0))
        return -1;
    uint64_t f = net_lock();
    struct udp_sock *s = &socks[sock];
    int rc = 0;
    if (!s->used) {
        rc = -1;
    } else if (s->err) {
        rc = -1;
        s->err = 0;                 /* report an ICMP error exactly once */
    } else if (s->qcount > 0) {
        int n = s->qlen[s->qhead] > (uint16_t)max ? max : s->qlen[s->qhead];
        memcpy(buf, s->q[s->qhead], (size_t)n);
        if (src)   *src   = s->qsrc[s->qhead];
        if (sport) *sport = s->qsport[s->qhead];
        s->qhead = (s->qhead + 1) % UDP_QUEUES;
        s->qcount--;
        rc = n;
    }
    net_unlock(f);
    return rc;
}

int udp_send(uint32_t dst, uint16_t dport, uint16_t sport,
             const void *data, uint16_t len)
{
    uint8_t pkt[1480];
    if (sizeof(struct udp_hdr) + len > sizeof pkt)
        return -1;
    struct udp_hdr *h = (struct udp_hdr *)pkt;
    h->sport = htons(sport);
    h->dport = htons(dport);
    h->length = htons((uint16_t)(sizeof *h + len));
    h->checksum = 0;
    memcpy(pkt + sizeof *h, data, len);
    uint16_t total = (uint16_t)(sizeof *h + len);
    uint16_t sum = udp_checksum(net_cfg.ip, dst, pkt, total);
    /* In UDP/IPv4, a computed checksum of zero is transmitted as all ones;
     * an all-zero wire field means "checksum omitted". */
    h->checksum = htons(sum ? sum : 0xFFFFu);
    return ip_send(dst, IP_PROTO_UDP, pkt, total);
}

int udp_send_to(int sock, uint32_t dst, uint16_t dport,
                const void *data, uint16_t len)
{
    if (sock < 0 || sock >= NUDP || !socks[sock].used)
        return -1;
    return udp_send(dst, dport, socks[sock].port, data, len);
}

uint32_t udp_drops(int sock)
{
    if (sock < 0 || sock >= NUDP || !socks[sock].used)
        return 0;
    return socks[sock].drops;
}

/* icmp.c weak-hook target: an ICMP error quoting a datagram we sent. Sockets
 * record no peer, so the match is by local port only -- the consumer
 * (dns_result) re-validates the peer of whatever it receives. */
void udp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code)
{
    (void)rip; (void)rport;
    for (int i = 0; i < NUDP; i++)
        if (socks[i].used && socks[i].port == lport)
            socks[i].err = (type << 8) | code;
}

/* Rate limit for ICMP port-unreachable replies: at most one per target IP
 * per second (RFC 1122-style courtesy, and it blunts reflector abuse). */
static uint32_t unreach_ip;
static uint64_t unreach_tick;

/* A datagram arrived for a port no socket is bound to: tell the sender with
 * an ICMP port-unreachable quoting the original IP header + 8 L4 bytes. */
static void send_port_unreach(uint32_t src, const uint8_t *iph,
                              const uint8_t *data)
{
    uint64_t now = timer_ticks();
    if (src == unreach_ip && now - unreach_tick < 100)
        return;
    unreach_ip = src;
    unreach_tick = now;
    int ihl = (iph[0] & 0xF) * 4;
    uint8_t quote[60 + 8];
    memcpy(quote, iph, (size_t)ihl);
    memcpy(quote + ihl, data, sizeof(struct udp_hdr));
    icmp_send_unreach(src, ICMP_DEST_UNREACH, ICMP_UNREACH_PORT,
                      quote, (uint16_t)(ihl + sizeof(struct udp_hdr)));
}

void udp_input(uint32_t src, const uint8_t *data, uint16_t len,
               const uint8_t *iph)
{
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *h = (const struct udp_hdr *)data;
    uint16_t dport = ntohs(h->dport);
    /* Validate the wire length field against the IP-validated payload length
     * before deriving dlen: reject < header (underflow) and > received bytes. */
    uint16_t udp_len = ntohs(h->length);
    if (udp_len < sizeof *h || udp_len > len)
        return;
    /* The pseudo-header destination is the packet's real one -- which may be
     * a broadcast address we accepted -- not necessarily net_cfg.ip. */
    uint32_t dst = ((uint32_t)iph[16] << 24) | ((uint32_t)iph[17] << 16) |
                   ((uint32_t)iph[18] << 8) | iph[19];
    /* A zero UDP checksum is legal in IPv4. Any nonzero checksum is mandatory
     * to verify over the pseudo-header and the UDP length (not IP padding). */
    if (h->checksum != 0 && udp_checksum(src, dst, data, udp_len) != 0)
        return;
    uint16_t dlen  = (uint16_t)(udp_len - sizeof *h);
    const uint8_t *payload = data + sizeof *h;

    struct udp_sock *s = find_sock(dport);
    if (s) {
        if (s->qcount == UDP_QUEUES) {
            s->drops++;             /* newest loses; drain faster */
        } else {
            int tail = (s->qhead + s->qcount) % UDP_QUEUES;
            memcpy(s->q[tail], payload, dlen);
            s->qlen[tail] = dlen;
            s->qsrc[tail] = src;
            s->qsport[tail] = ntohs(h->sport);
            s->qcount++;
        }
    } else if (!ip_is_broadcast(dst)) {
        /* No receiver on this port. Never send ICMP errors about broadcasts
         * (RFC 1122 3.2.2) -- that way lie broadcast storms. */
        send_port_unreach(src, iph, data);
    }
}
