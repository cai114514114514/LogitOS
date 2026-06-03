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

static uint16_t ping_id = 0xA10A, ping_seq;
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
    msg.h.seq = htons(++ping_seq);
    for (int i = 0; i < 32; i++) msg.data[i] = (uint8_t)i;
    msg.h.checksum = htons(ip_checksum(&msg, sizeof msg));

    last_rtt = -1;
    ping_sent_tick = timer_ticks();
    return ip_send(dst, IP_PROTO_ICMP, &msg, sizeof msg);
}

void icmp_input(uint32_t src, const uint8_t *data, uint16_t len)
{
    if (len < sizeof(struct icmp_hdr))
        return;
    const struct icmp_hdr *in = (const struct icmp_hdr *)data;

    if (in->type == ICMP_ECHO_REQUEST) {
        /* Reply: echo the payload back with type 0, fresh checksum. */
        uint8_t buf[1500];
        if (len > sizeof buf) return;
        memcpy(buf, data, len);
        struct icmp_hdr *out = (struct icmp_hdr *)buf;
        out->type = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = htons(ip_checksum(buf, len));
        ip_send(src, IP_PROTO_ICMP, buf, len);
    } else if (in->type == ICMP_ECHO_REPLY) {
        if (ntohs(in->id) == ping_id) {
            int rtt = (int)(timer_ticks() - ping_sent_tick);
            last_rtt = rtt < 0 ? 0 : rtt;
        }
    }
}
