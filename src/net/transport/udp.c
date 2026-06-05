#include <stdint.h>
#include <stddef.h>
#include "udp.h"
#include "ip.h"
#include "net.h"

void *memcpy(void *, const void *, size_t);

static uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += IP_PROTO_UDP;
    sum += (uint32_t)len;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
    if (len & 1)
        sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint16_t checksum;      /* 0 = not computed (allowed for IPv4) */
} __attribute__((packed));

/* A single one-shot receive slot (enough for the DNS demo). */
static struct {
    uint16_t  port;
    uint8_t  *buf;
    int       max;
    int       len;          /* -1 until a datagram arrives */
    uint32_t  src;
} slot = { .len = -1 };

void udp_recv_bind(uint16_t port, uint8_t *buf, int max)
{
    slot.port = port; slot.buf = buf; slot.max = max; slot.len = -1; slot.src = 0;
}
int      udp_recv_len(void) { return slot.len; }
uint32_t udp_recv_src(void) { return slot.src; }

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
    h->checksum = 0;                    /* optional for IPv4; receivers accept 0 */
    memcpy(pkt + sizeof *h, data, len);
    return ip_send(dst, IP_PROTO_UDP, pkt, (uint16_t)(sizeof *h + len));
}

void udp_input(uint32_t src, const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *h = (const struct udp_hdr *)data;
    uint16_t ulen = ntohs(h->length);
    if (ulen < sizeof *h || ulen > len)
        return;
    if (h->checksum && udp_checksum(src, net_cfg.ip, data, ulen) != 0)
        return;
    uint16_t dport = ntohs(h->dport);
    uint16_t dlen  = (uint16_t)(ulen - sizeof *h);
    const uint8_t *payload = data + sizeof *h;

    if (slot.len < 0 && slot.buf && dport == slot.port) {
        int n = dlen > slot.max ? slot.max : dlen;
        memcpy(slot.buf, payload, n);
        slot.len = n;
        slot.src = src;
    }
}
