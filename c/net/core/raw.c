#include <stdint.h>
#include <stddef.h>
#include "raw.h"
#include "ip.h"
#include "net.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

struct raw_sock {
    int      used;
    int      pid;
    uint32_t drops;                     /* queue-full drops, mirrors udp_sock */
    int      qhead;                     /* oldest datagram */
    int      qcount;
    uint16_t qlen[RAW_QUEUES];
    uint32_t qsrc[RAW_QUEUES];
    uint8_t  q[RAW_QUEUES][RAW_SLOT];
};

static struct raw_sock socks[RAW_MAX];

int raw_icmp_open(int pid)
{
    /* net_lock: raw_icmp_deliver() runs from the RX path (icmp_input, called
     * off net_poll() / the RX softirq), so allocating a slot has to be
     * mutually exclusive with delivery walking the table -- same reasoning
     * as udp_bind() in c/net/transport/udp.c. */
    uint64_t f = net_lock();
    int id = -1;
    for (int i = 0; i < RAW_MAX; i++)
        if (!socks[i].used) { id = i; break; }
    if (id >= 0) {
        memset(&socks[id], 0, sizeof socks[id]);
        socks[id].used = 1;
        socks[id].pid = pid;
    }
    net_unlock(f);
    return id;
}

void raw_icmp_close(int id)
{
    if (id < 0 || id >= RAW_MAX) return;
    uint64_t f = net_lock();
    socks[id].used = 0;
    net_unlock(f);
}

int raw_icmp_send(int id, uint32_t dst, const void *buf, int len)
{
    if (id < 0 || id >= RAW_MAX || !socks[id].used) return -1;
    if (len < 0 || (!buf && len > 0)) return -1;
    /* IP_HDRINCL IS REFUSED, ON PURPOSE, NOT MERELY UNIMPLEMENTED.
     *
     * A caller-built IP header is how a real raw socket forges a source
     * address -- the one thing that matters once a caller has already
     * cleared the privilege check in lsock_create(), because forging the
     * source is what turns "this process can read ICMP" into "this process
     * can make ICMP traffic that did not come from this machine look like it
     * did", which is a strictly bigger grant than the task asked for. This
     * machine also has exactly one source address (net_cfg.ip) and no
     * routing decision of its own to make, so HDRINCL would not even let a
     * LEGITIMATE caller pick a different exit interface the way it can on a
     * multi-homed box -- there is nothing behind the option to use.
     *
     * ip_send() builds the IP header exactly as every other sender in this
     * stack does (icmp_ping(), udp_send(), tcp.c's segments): the caller
     * supplies only the ICMP message -- type, code, checksum, id, seq,
     * payload -- computed in userland (see ping.c), same as any other raw
     * ICMP implementation asks of its caller when HDRINCL is off.
     *
     * ip_send()'s return is 0/-1 (queued or sent OK / ARP still resolving),
     * NOT a byte count -- see ip.h's own doc on it and icmp_ping()'s "Returns
     * 0 if it went out, -1 if ARP is still resolving". lsock_sendto() and
     * BSD sendto() both need bytes-sent on success, the same contract
     * lsock_sendto()'s UDP branch already returns, so that translation
     * happens HERE rather than leaking ip_send()'s different convention up
     * through the socket layer. (Caught by tests/unit/raw_test.c returning 0
     * instead of the message length before this comment existed.) */
    return ip_send(dst, IP_PROTO_ICMP, buf, (uint16_t)len) == 0 ? len : -1;
}

int raw_icmp_recv(int id, void *buf, int max, uint32_t *src)
{
    if (id < 0 || id >= RAW_MAX || max < 0 || (!buf && max > 0)) return -1;
    uint64_t f = net_lock();
    struct raw_sock *s = &socks[id];
    int rc = 0;
    if (!s->used) {
        rc = -1;
    } else if (s->qcount > 0) {
        int n = s->qlen[s->qhead] > (uint16_t)max ? max : s->qlen[s->qhead];
        memcpy(buf, s->q[s->qhead], (size_t)n);
        if (src) *src = s->qsrc[s->qhead];
        s->qhead = (s->qhead + 1) % RAW_QUEUES;
        s->qcount--;
        rc = n;
    }
    net_unlock(f);
    return rc;
}

static void queue_one(struct raw_sock *s, const uint8_t *data, uint16_t n,
                      uint32_t src)
{
    if (s->qcount == RAW_QUEUES) { s->drops++; return; }  /* newest loses (mirrors udp.c) */
    int tail = (s->qhead + s->qcount) % RAW_QUEUES;
    memcpy(s->q[tail], data, n);
    s->qlen[tail] = n;
    s->qsrc[tail] = src;
    s->qcount++;
}

void raw_icmp_deliver(uint32_t src, const uint8_t *data, uint16_t len)
{
    uint16_t n = len > RAW_SLOT ? RAW_SLOT : len;
#ifdef RAW_NEGCTL_FIRST_ONLY
    /* NEGATIVE CONTROL ONLY (make test-raw-negctl), never in the kernel
     * build: deliver to the first open socket and stop, as if the table were
     * one shared slot instead of a real fan-out. Every raw ICMP socket is
     * supposed to see its OWN copy of every inbound message --
     * tests/unit/raw_test.c's multi-socket fan-out check MUST fail against
     * this build. */
    for (int i = 0; i < RAW_MAX; i++) {
        if (!socks[i].used) continue;
        queue_one(&socks[i], data, n, src);
        return;
    }
#else
    for (int i = 0; i < RAW_MAX; i++)
        if (socks[i].used) queue_one(&socks[i], data, n, src);
#endif
}
