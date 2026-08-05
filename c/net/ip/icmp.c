#include <stdint.h>
#include <stddef.h>
#include "icmp.h"
#include "ip.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

/* The IP header quoted inside an ICMP error message. Only the fields needed
 * to route the error to a transport are read. */
struct icmp_orig_ip {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

/* Transport-layer error hooks (weak until udp.c / tcp.c are linked in). */
void udp_error(uint16_t, uint32_t, uint16_t, int, int) __attribute__((weak));
void tcp_error(uint16_t, uint32_t, uint16_t, int, int) __attribute__((weak));

static uint16_t ping_id = 0xA10A, ping_seq;
static uint16_t ping_wait_seq;
static uint32_t ping_dst;
static uint64_t ping_sent_tick;
static int last_rtt = -1;

int icmp_last_rtt(void) { return last_rtt; }

int icmp_ping(uint32_t dst)
{
    struct { struct icmp_hdr h; uint8_t data[32]; } msg;
    msg.h.type = ICMP_ECHO_REQUEST;
    msg.h.code = 0;
    msg.h.checksum = 0;
    msg.h.id = htons(ping_id);
    ping_wait_seq = ++ping_seq;
    msg.h.seq = htons(ping_wait_seq);
    for (int i = 0; i < 32; i++) msg.data[i] = (uint8_t)i;
    msg.h.checksum = htons(ip_checksum(&msg, sizeof msg));

    last_rtt = -1;
    ping_dst = dst;
    ping_sent_tick = timer_ticks();
    return ip_send(dst, IP_PROTO_ICMP, &msg, sizeof msg);
}

void icmp_send_unreach(uint32_t dst, int type, int code,
                       const uint8_t *quote, uint16_t quote_len)
{
    /* 8-byte ICMP header (id/seq double as the error messages' "unused"
     * field) + the quoted original IP header and first 8 L4 bytes. */
    uint8_t buf[sizeof(struct icmp_hdr) + 60 + 8];
    if (quote_len > sizeof buf - sizeof(struct icmp_hdr))
        quote_len = (uint16_t)(sizeof buf - sizeof(struct icmp_hdr));
    struct icmp_hdr *h = (struct icmp_hdr *)buf;
    h->type = (uint8_t)type;
    h->code = (uint8_t)code;
    h->checksum = 0;
    h->id = 0;
    h->seq = 0;
    memcpy(buf + sizeof *h, quote, quote_len);
    uint16_t len = (uint16_t)(sizeof *h + quote_len);
    h->checksum = htons(ip_checksum(buf, len));
    ip_send(dst, IP_PROTO_ICMP, buf, len);
}

/* Route a destination-unreachable / time-exceeded to the transport whose
 * header it quotes. The quote is the original IP header + 8 L4 bytes, which
 * for UDP and TCP suffices to recover both ports. */
static void icmp_error(const uint8_t *data, uint16_t len)
{
    const struct icmp_hdr *in = (const struct icmp_hdr *)data;
    if (len < sizeof(struct icmp_hdr) + sizeof(struct icmp_orig_ip) + 8)
        return;
    const struct icmp_orig_ip *oh =
        (const struct icmp_orig_ip *)(data + sizeof(struct icmp_hdr));
    if ((oh->ver_ihl >> 4) != 4)
        return;
    int oihl = (oh->ver_ihl & 0xF) * 4;
    if (oihl < (int)sizeof(struct icmp_orig_ip) ||
        (uint32_t)sizeof(struct icmp_hdr) + oihl + 8 > len)
        return;
    /* Only act on errors that quote a packet we actually sent. */
    if (ntohl(oh->src) != net_cfg.ip)
        return;
    uint32_t rip = ntohl(oh->dst);
    const uint8_t *ol4 = data + sizeof(struct icmp_hdr) + oihl;
    uint16_t lport = (uint16_t)(((uint16_t)ol4[0] << 8) | ol4[1]);
    uint16_t rport = (uint16_t)(((uint16_t)ol4[2] << 8) | ol4[3]);
    if (oh->proto == IP_PROTO_UDP && udp_error)
        udp_error(lport, rip, rport, in->type, in->code);
    else if (oh->proto == IP_PROTO_TCP && tcp_error)
        tcp_error(lport, rip, rport, in->type, in->code);
}

void icmp_input(uint32_t src, const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct icmp_hdr))
        return;
    if (ip_checksum(data, len) != 0)
        return;
    const struct icmp_hdr *in = (const struct icmp_hdr *)data;

    if (in->type == ICMP_ECHO_REQUEST && in->code == 0) {
        /* Reply: echo the payload back with type 0, fresh checksum. */
        uint8_t buf[1500];
        if (len > sizeof buf) return;
        memcpy(buf, data, len);
        struct icmp_hdr *out = (struct icmp_hdr *)buf;
        out->type = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = htons(ip_checksum(buf, len));
        ip_send(src, IP_PROTO_ICMP, buf, len);
    } else if (in->type == ICMP_ECHO_REPLY && in->code == 0) {
        if (src == ping_dst && ntohs(in->id) == ping_id &&
            ntohs(in->seq) == ping_wait_seq) {
            int rtt = (int)(timer_ticks() - ping_sent_tick);
            last_rtt = rtt < 0 ? 0 : rtt;
        }
    } else if (in->type == ICMP_DEST_UNREACH || in->type == ICMP_TIME_EXCEEDED) {
        icmp_error(data, len);
    }
}
