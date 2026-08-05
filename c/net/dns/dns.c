#include <stdint.h>
#include <stddef.h>
#include "dns.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "rng.h"

/* A tiny DNS client: one A-query to the SLIRP resolver (10.0.2.3:53), parse the
 * first A answer. No compression building (we only emit one QNAME); answer
 * parsing skips compressed names via the 0xC0 pointer form. */

#define DNS_SERVER IPV4(10, 0, 2, 3)
#define DNS_PORT   53

/* Encode "a.b.c" as length-prefixed labels into out; returns bytes written. */
static int encode_qname(uint8_t *out, const char *name)
{
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int lbl = (int)(dot - p);
        if (lbl > 63) return -1;
        out[o++] = (uint8_t)lbl;
        for (int i = 0; i < lbl; i++) out[o++] = (uint8_t)p[i];
        p = (*dot == '.') ? dot + 1 : dot;
    }
    out[o++] = 0;       /* root label */
    return o;
}

/* Skip a (possibly compressed) DNS name starting at off; return new offset. */
static int skip_name(const uint8_t *msg, int off, int len)
{
    int depth = 0;
    while (off < len && depth < 64) {
        uint8_t l = msg[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2;     /* compression pointer */
        off += 1 + l;
        depth++;
    }
    return off;
}

static uint8_t resp[512];
static uint64_t dns_started;
static uint16_t txid;           /* of the query in flight (spoofing guard) */

/* Build an A-query for `name` into q; returns its length, or -1. */
static int build_query(uint8_t *q, const char *name)
{
    kernel_random_bytes((uint8_t *)&txid, sizeof txid);
    txid ^= (uint16_t)timer_ticks();
    q[0] = (uint8_t)(txid >> 8); q[1] = (uint8_t)(txid & 0xFF);
    q[2] = 0x01; q[3] = 0x00;
    q[4] = 0; q[5] = 1; q[6] = 0; q[7] = 0; q[8] = 0; q[9] = 0; q[10] = 0; q[11] = 0;
    int o = 12;
    int n = encode_qname(q + o, name);
    if (n < 0) return -1;
    o += n;
    q[o++] = 0; q[o++] = 1;     /* QTYPE A */
    q[o++] = 0; q[o++] = 1;     /* QCLASS IN */
    return o;
}

/* Parse the first A answer out of resp[0..rlen); 0 if none. */
static uint32_t parse_answer(int rlen)
{
    if (rlen < 12) return 0;
    /* Reject a response whose transaction ID doesn't match our query (the
     * per-query txid set by build_query) -- a basic guard against off-path
     * spoofed answers landing in our one-shot receive slot. */
    if (resp[0] != (uint8_t)(txid >> 8) || resp[1] != (uint8_t)(txid & 0xFF))
        return 0;
    int ancount = (resp[6] << 8) | resp[7];
    if (ancount < 1) return 0;

    /* Skip the question section (one QNAME + QTYPE + QCLASS). */
    int off = skip_name(resp, 12, rlen) + 4;
    for (int a = 0; a < ancount && off + 12 <= rlen; a++) {
        off = skip_name(resp, off, rlen);
        if (off + 10 > rlen) break;
        int type = (resp[off] << 8) | resp[off + 1];
        int rdlen = (resp[off + 8] << 8) | resp[off + 9];
        int rdata = off + 10;
        if (type == 1 && rdlen == 4 && rdata + 4 <= rlen)      /* A record */
            return IPV4(resp[rdata], resp[rdata + 1], resp[rdata + 2], resp[rdata + 3]);
        off = rdata + rdlen;
    }
    return 0;
}

/* Non-blocking: send the query, arm the receive slot, record the start time. */
void dns_start(const char *name)
{
    uint8_t q[512];
    int o = build_query(q, name);
    if (o < 0) return;
    udp_recv_bind(0x4444, resp, sizeof resp);
    dns_started = timer_ticks();
    udp_send(DNS_SERVER, DNS_PORT, 0x4444, q, (uint16_t)o);
}

/* 0 = pending, 0xFFFFFFFF = timeout, else the resolved IP (host order). */
uint32_t dns_result(void)
{
    int rlen = udp_recv_len();
    if (rlen >= 0) {
        if (udp_recv_src() != DNS_SERVER || udp_recv_sport() != DNS_PORT)
            return 0xFFFFFFFFu;
        uint32_t ip = parse_answer(rlen);
        return ip ? ip : 0xFFFFFFFFu;
    }
    if (timer_ticks() - dns_started > 300)      /* ~3 s */
        return 0xFFFFFFFFu;
    return 0;
}

/* Dotted-decimal IP literal "a.b.c.d" -> host-order IP, or 0 if not a literal. */
static uint32_t parse_ip_literal(const char *s)
{
    uint32_t ip = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, ndig = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++ndig > 3 || v > 255) return 0; }
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == 0 ? ip : 0;
}

uint32_t dns_resolve(const char *name)
{
    uint32_t lit = parse_ip_literal(name);   /* skip DNS for "10.0.2.2" etc. */
    if (lit) return lit;
    arp_warm(DNS_SERVER, 30);                 /* resolve the resolver's MAC first, so the */
    dns_start(name);                          /* query isn't dropped on a cold ARP cache */
    uint64_t start = timer_ticks(), last = start;
    while (timer_ticks() - start < 300) {
        net_poll();
        uint32_t r = dns_result();
        if (r == 0xFFFFFFFFu) return 0;
        if (r) return r;
        if (timer_ticks() - last >= 50) {       /* retransmit */
            last = timer_ticks();
            dns_start(name);
        }
        net_idle();                                  /* sleep to the next tick; don't peg the host CPU */
    }
    return 0;
}
