#include <stdint.h>
#include <stddef.h>
#include "udp.h"
#include "ip.h"
#include "icmp.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint16_t checksum;      /* zero is accepted on IPv4; Aether always sends one */
} __attribute__((packed));

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

/* A single one-shot receive slot (enough for the DNS demo). */
static struct {
    uint16_t  port;
    uint8_t  *buf;
    int       max;
    int       len;          /* -1 until a datagram arrives */
    int       err;          /* nonzero once an ICMP error quotes this port */
    uint32_t  src;
    uint16_t  sport;
} slot = { .len = -1 };

void udp_recv_bind(uint16_t port, uint8_t *buf, int max)
{
    slot.port = port;
    slot.buf = (buf && max >= 0) ? buf : NULL;
    slot.max = max >= 0 ? max : 0;
    slot.len = -1; slot.err = 0; slot.src = 0; slot.sport = 0;
}
int      udp_recv_len(void) { return slot.len; }
int      udp_recv_err(void) { return slot.err; }
uint32_t udp_recv_src(void) { return slot.src; }
uint16_t udp_recv_sport(void) { return slot.sport; }

/* icmp.c weak-hook target: an ICMP error quoting a datagram we sent. The
 * one-shot slot records no peer, so the match is by local port only -- the
 * consumer (dns_result) re-validates the peer of whatever it receives. */
void udp_error(uint16_t lport, uint32_t rip, uint16_t rport, int type, int code)
{
    (void)rip; (void)rport;
    if (slot.len < 0 && slot.buf && slot.port == lport)
        slot.err = (type << 8) | code;
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

/* Rate limit for ICMP port-unreachable replies: at most one per target IP
 * per second (RFC 1122-style courtesy, and it blunts reflector abuse). */
static uint32_t unreach_ip;
static uint64_t unreach_tick;

/* A datagram arrived for a port nobody is receiving on: tell the sender with
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

    if (slot.len < 0 && slot.buf && dport == slot.port) {
        int n = dlen > slot.max ? slot.max : dlen;
        memcpy(slot.buf, payload, n);
        slot.len = n;
        slot.src = src;
        slot.sport = ntohs(h->sport);
    } else if (!ip_is_broadcast(dst)) {
        /* No receiver on this port. Never send ICMP errors about broadcasts
         * (RFC 1122 3.2.2) -- that way lie broadcast storms. */
        send_port_unreach(src, iph, data);
    }
}
