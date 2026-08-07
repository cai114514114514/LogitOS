#include <stdint.h>
#include <stddef.h>
#include "dns.h"
#include "udp.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "rng.h"
#include "ip6.h"

/* A tiny DNS client: one A-query to the configured resolver (net_cfg.dns,
 * from DHCP or the static fallback), parse the first A answer. No compression
 * building (we only emit one QNAME); answer parsing skips compressed names
 * via the 0xC0 pointer form.
 *
 * IPv6 changes the SHAPE of a lookup, not just the record type. A name has an
 * A set and a AAAA set, they arrive in separate transactions that finish at
 * different times, and the caller wants ONE list in preference order -- which
 * is RFC 6724 destination ordering, not "v6 first". So the async pool below
 * runs both queries at once, merges the answers into a family-tagged list
 * (IPv4 carried as ::ffff:a.b.c.d, which is how RFC 6724 compares the two),
 * and hands the ordered list to sock.c.
 *
 * The transport stays IPv4. DNS runs over UDP and this stack has no UDP over
 * IPv6 (c/net/transport/ belongs to another line); asking an IPv4 resolver for
 * AAAA records is normal and complete -- what a stack must never do is assume
 * that no IPv6 transport means no IPv6 addresses.
 *
 * AAAA is only asked for when ip6_up() reports a routable address AND a default
 * router. On a v4-only network not one extra byte goes on the wire and this
 * file behaves exactly as it did before IPv6 existed. */

#define DNS_PORT   53
#define DNS_T_A    1
#define DNS_T_AAAA 28

/* Longest answer set kept per name. Eight covers every real round-robin set
 * worth trying before giving up on a host. */
#define DNS_MAXA   8

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

/* Build an A-query for `name` into q; returns its length, or -1. The
 * transaction id is generated here and handed back through *out_txid, so each
 * concurrent query in the async pool below keeps its own spoofing guard rather
 * than sharing one module-global. */
static int build_query_type(uint8_t *q, const char *name, int qtype,
                            uint16_t *out_txid)
{
    uint16_t id;
    kernel_random_bytes((uint8_t *)&id, sizeof id);
    id ^= (uint16_t)timer_ticks();
    *out_txid = id;
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)(id & 0xFF);
    q[2] = 0x01; q[3] = 0x00;
    q[4] = 0; q[5] = 1; q[6] = 0; q[7] = 0; q[8] = 0; q[9] = 0; q[10] = 0; q[11] = 0;
    int o = 12;
    int n = encode_qname(q + o, name);
    if (n < 0) return -1;
    o += n;
    q[o++] = (uint8_t)(qtype >> 8); q[o++] = (uint8_t)qtype;
    q[o++] = 0; q[o++] = 1;     /* QCLASS IN */
    return o;
}

static int build_query_id(uint8_t *q, const char *name, uint16_t *out_txid)
{
    return build_query_type(q, name, DNS_T_A, out_txid);
}

/* Collect every A and AAAA answer out of msg[0..rlen) into `out` (IPv4 records
 * become ::ffff:a.b.c.d). Returns the number appended, or -1 if the response is
 * not ours -- which is a different outcome from "ours, but empty": a NODATA
 * answer for AAAA is a definitive "this name has no IPv6 address" and must end
 * that half of the lookup rather than sit there retransmitting. */
static int collect_answers(const uint8_t *msg, int rlen, uint16_t want,
                           ip6_addr *out, int max, int *nout)
{
    if (rlen < 12) return -1;
    /* Reject a response whose transaction ID doesn't match our query (the
     * per-query txid set by build_query_type) -- a basic guard against off-path
     * spoofed answers landing in our receive slot. */
    if (msg[0] != (uint8_t)(want >> 8) || msg[1] != (uint8_t)(want & 0xFF))
        return -1;
    int added = 0;
    int ancount = (msg[6] << 8) | msg[7];

    /* Skip the question section (one QNAME + QTYPE + QCLASS). */
    int off = skip_name(msg, 12, rlen) + 4;
    for (int a = 0; a < ancount && off + 12 <= rlen; a++) {
        off = skip_name(msg, off, rlen);
        if (off + 10 > rlen) break;
        int type = (msg[off] << 8) | msg[off + 1];
        int rdlen = (msg[off + 8] << 8) | msg[off + 9];
        int rdata = off + 10;
        if (rdata + rdlen > rlen) break;                /* truncated record */
        if (type == DNS_T_A && rdlen == 4 && *nout < max) {
            ip6_from_v4(IPV4(msg[rdata], msg[rdata + 1],
                             msg[rdata + 2], msg[rdata + 3]), &out[*nout]);
            (*nout)++; added++;
        } else if (type == DNS_T_AAAA && rdlen == 16 && *nout < max) {
            /* A AAAA record that is not a usable unicast address is a
             * misconfiguration at best and a redirection primitive at worst:
             * a name resolving to ::1 or to a multicast group must not be
             * connected to. */
            ip6_addr v;
            for (int i = 0; i < 16; i++) v.b[i] = msg[rdata + i];
            if (!ip6_is_multicast(&v) && !ip6_is_loopback(&v) &&
                !ip6_is_unspecified(&v) && !ip6_is_v4mapped(&v)) {
                out[*nout] = v;
                (*nout)++; added++;
            }
        }
        off = rdata + rdlen;
    }
    return added;
}

/* Parse the first A answer out of msg[0..rlen); 0 if none. The legacy blocking
 * resolver's view, unchanged in behaviour. */
static uint32_t parse_answer_id(const uint8_t *msg, int rlen, uint16_t want)
{
    ip6_addr a[DNS_MAXA];
    int n = 0;
    if (collect_answers(msg, rlen, want, a, DNS_MAXA, &n) < 0) return 0;
    for (int i = 0; i < n; i++)
        if (ip6_is_v4mapped(&a[i])) return ip6_to_v4(&a[i]);
    return 0;
}

static int      dns_sock = -1;  /* UDP socket for the query in flight */

/* Random ephemeral source port (well clear of the DHCP client port 68). */
static uint16_t pick_lport(void)
{
    uint16_t r;
    kernel_random_bytes((uint8_t *)&r, sizeof r);
    return (uint16_t)(49152 + r % 10000);
}

/* Non-blocking: send the query, arm the receive socket, record the start time. */
void dns_start(const char *name)
{
    uint8_t q[512];
    int o = build_query_id(q, name, &txid);
    if (o < 0) { dns_sock = -1; return; }
    if (dns_sock < 0) {
        for (int tries = 0; tries < 16 && dns_sock < 0; tries++)
            dns_sock = udp_bind(pick_lport());
        if (dns_sock < 0) return;       /* no free socket: dns_result fails */
    }
    dns_started = timer_ticks();
    udp_send_to(dns_sock, net_cfg.dns, DNS_PORT, q, (uint16_t)o);
}

/* 0 = pending, 0xFFFFFFFF = timeout/ICMP error, else the resolved IP (host
 * order). A terminal answer releases the socket. */
uint32_t dns_result(void)
{
    if (dns_sock < 0)
        return 0xFFFFFFFFu;             /* dns_start could not arm a query */
    uint32_t src;
    uint16_t sport;
    int rlen = udp_recv(dns_sock, resp, sizeof resp, &src, &sport);
    uint32_t rc = 0;
    if (rlen < 0) {
        /* An ICMP error (e.g. port-unreachable) against the query port fails
         * the lookup immediately instead of riding out the 3 s timeout. */
        rc = 0xFFFFFFFFu;
    } else if (rlen > 0) {
        if (src == net_cfg.dns && sport == DNS_PORT) {
            uint32_t ip = parse_answer_id(resp, rlen, txid);
            rc = ip ? ip : 0xFFFFFFFFu;
        }
        /* else: someone else's datagram on our ephemeral port -- ignore it
         * and stay pending (the one-shot slot used to fail outright here). */
    } else if (timer_ticks() - dns_started > 300) {     /* ~3 s */
        rc = 0xFFFFFFFFu;
    }
    if (rc) {
        udp_close(dns_sock);
        dns_sock = -1;
    }
    return rc;
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

static void dns_cache_put(const char *name, uint32_t ip);   /* pool cache, below */

/* Deliberately WRITES the shared cache but never READS it. dns_resolve is the
 * blocking resolver on the legacy SYS_HTTP_GET path, which has to keep behaving
 * exactly as it did -- a cache hit there would change what that path does on a
 * stale record, and that path is the one every existing https test rides. So it
 * only warms the cache for the async sockets, which do read it. */
uint32_t dns_resolve(const char *name)
{
    uint32_t lit = parse_ip_literal(name);   /* skip DNS for "10.0.2.2" etc. */
    if (lit) return lit;
    arp_warm(net_cfg.dns, 30);                /* resolve the resolver's MAC first, so the */
    dns_start(name);                          /* query isn't dropped on a cold ARP cache */
    uint64_t start = timer_ticks(), last = start;
    while (timer_ticks() - start < 300) {
        net_poll();
        uint32_t r = dns_result();
        if (r == 0xFFFFFFFFu) return 0;
        if (r) { dns_cache_put(name, r); return r; }
        if (timer_ticks() - last >= 50) {       /* retransmit */
            last = timer_ticks();
            dns_start(name);
        }
        net_idle();                                  /* sleep to the next tick; don't peg the host CPU */
    }
    if (dns_sock >= 0) {                            /* overall timeout: release */
        udp_close(dns_sock);
        dns_sock = -1;
    }
    return 0;
}

/* ---- async resolver pool (the non-blocking socket path) -------------------
 *
 * dns_resolve() above owns the network for the ~200 ms a lookup takes: it spins
 * on net_poll() and cannot be called from a syscall or from net_poll() itself.
 * A connection pool needs the opposite -- several lookups outstanding at once,
 * each advanced a little on every net_poll() and never waited on. That is this.
 *
 * The slots are independent (own UDP socket, own transaction id, own timers), so
 * eight names resolve simultaneously; the legacy single-shot dns_start/
 * dns_result slot above is untouched and does not share state with them. */

#define DNS_NQ      8               /* concurrent lookups */
#define DNS_RETX    50             /* retransmit after ~500 ms */
#define DNS_GIVEUP  300            /* fail after ~3 s (matches dns_resolve) */

/* How long the lookup waits for the SECOND family after the first one answers.
 * RFC 8305 s3 ("Resolution Delay") puts this at 50 ms and applies it the other
 * way round -- wait for AAAA, then proceed on A. 500 ms here because the
 * resolver is a hop away over emulated hardware and because a lookup that ends
 * one family short costs a whole connection attempt to discover. It only ever
 * applies when both families were asked for, i.e. never on a v4-only network. */
#define DNS_2ND_GRACE 50

struct dns_query {
    int      used;
    int      done;                  /* 1 once the address list is final */
    int      failed;
    int      sock;                  /* UDP socket, -1 for a cache hit */
    uint16_t txid;                  /* the A transaction */
    uint16_t txid6;                 /* the AAAA transaction */
    uint8_t  want6;                 /* 1 if AAAA was asked for at all */
    uint8_t  got4, got6;            /* a definitive answer arrived for each */
    uint32_t ip;                    /* first A record, for the legacy uint32 API */
    ip6_addr addrs[DNS_MAXA];
    uint8_t  naddr;
    uint64_t t0, last, t_first;     /* t_first: when the first family answered */
    char     name[DNS_NAME_MAX];
};
static struct dns_query dq[DNS_NQ];

/* A tiny name cache. THIS IS A KNOWN SHORTCUT: the TTL is fixed rather than read
 * off the record, because we do not parse the answer's TTL field. 120 s is short
 * enough that a re-pointed name recovers within a page reload and long enough
 * that the thirty-odd sub-resources of one page cost one lookup per host. */
#define DNS_NCACHE  16
#define DNS_TTL     (100 * 120)     /* ticks (100 Hz) = 120 s */

struct dns_cent {
    char     name[DNS_NAME_MAX];
    uint32_t ip;                    /* first A record (the pre-IPv6 field) */
    ip6_addr addrs[DNS_MAXA];       /* the full mixed-family set */
    uint8_t  naddr;
    uint64_t exp;
};
static struct dns_cent dcache[DNS_NCACHE];
static int dcache_w;                /* round-robin victim: no LRU, 16 entries */

static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void name_copy(char *d, const char *s)
{
    int i = 0;
    for (; i < DNS_NAME_MAX - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

static struct dns_cent *dns_cache_find(const char *name)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < DNS_NCACHE; i++)
        if (dcache[i].naddr && dcache[i].exp > now && name_eq(dcache[i].name, name))
            return &dcache[i];
    return NULL;
}

/* Store the full answer set. The single-address entry point below keeps the
 * pre-IPv6 signature so dns_resolve()'s cache warming is untouched. */
static void dns_cache_put_set(const char *name, const ip6_addr *a, int n)
{
    if (n <= 0) return;
    if (n > DNS_MAXA) n = DNS_MAXA;
    uint64_t now = timer_ticks();
    struct dns_cent *e = NULL;
    for (int i = 0; i < DNS_NCACHE; i++)          /* refresh an existing entry */
        if (dcache[i].naddr && name_eq(dcache[i].name, name)) { e = &dcache[i]; break; }
    if (!e) {
        e = &dcache[dcache_w++ % DNS_NCACHE];
        name_copy(e->name, name);
    }
    e->naddr = (uint8_t)n;
    e->ip = 0;
    for (int i = 0; i < n; i++) {
        e->addrs[i] = a[i];
        if (!e->ip && ip6_is_v4mapped(&a[i])) e->ip = ip6_to_v4(&a[i]);
    }
    e->exp = now + DNS_TTL;
}

static void dns_cache_put(const char *name, uint32_t ip)
{
    if (!ip) return;
    ip6_addr a;
    ip6_from_v4(ip, &a);
    dns_cache_put_set(name, &a, 1);
}

static void dq_send(struct dns_query *q)
{
    uint8_t buf[512];
    int o = build_query_type(buf, q->name, DNS_T_A, &q->txid);
    if (o < 0) { q->done = 1; q->failed = 1; return; }
    q->last = timer_ticks();
    udp_send_to(q->sock, net_cfg.dns, DNS_PORT, buf, (uint16_t)o);
    if (!q->want6) return;
    o = build_query_type(buf, q->name, DNS_T_AAAA, &q->txid6);
    if (o > 0)
        udp_send_to(q->sock, net_cfg.dns, DNS_PORT, buf, (uint16_t)o);
}

/* Start a lookup. Returns a query id (>= 0) the caller polls with
 * dns_query_result() and MUST release with dns_query_free(), or -1 if the name
 * is too long or every slot is busy. An IP literal and a cache hit both return
 * an already-complete slot, so the caller's state machine has one shape. */
int dns_query_start(const char *name)
{
    if (!name || !*name) return -1;
    int n = 0; while (name[n]) n++;
    if (n >= DNS_NAME_MAX) return -1;

    int id = -1;
    for (int i = 0; i < DNS_NQ; i++) if (!dq[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct dns_query *q = &dq[id];
    for (unsigned i = 0; i < sizeof *q; i++) ((uint8_t *)q)[i] = 0;
    q->used = 1;
    q->sock = -1;
    name_copy(q->name, name);

    /* An IPv6 literal ("2001:db8::2", and the bracketless form a URL parser
     * hands over) resolves to itself, exactly as a dotted quad does. */
    ip6_addr lit6;
    if (ip6_parse(name, &lit6) && !ip6_is_multicast(&lit6)) {
        q->addrs[0] = lit6;
        q->naddr = 1;
        q->ip = ip6_to_v4(&lit6);       /* 0 for a real v6 literal */
        q->done = 1;
        return id;
    }

    uint32_t fast = parse_ip_literal(name);
    if (fast) {
        ip6_from_v4(fast, &q->addrs[0]);
        q->naddr = 1;
        q->ip = fast;
        q->done = 1;
        return id;
    }
    struct dns_cent *c = dns_cache_find(name);
    if (c) {
        for (int i = 0; i < c->naddr; i++) q->addrs[i] = c->addrs[i];
        q->naddr = c->naddr;
        q->ip = c->ip;
        q->done = 1;
        return id;
    }

    /* Ask for AAAA only when IPv6 could actually be used: a routable address
     * that has passed DAD, and a default router. Without that, a AAAA answer
     * would only produce destinations we cannot reach -- and on a v4-only
     * network this keeps the wire byte-identical to before IPv6 existed. */
    q->want6 = (ip6_up() == 2);

    for (int tries = 0; tries < 16 && q->sock < 0; tries++)
        q->sock = udp_bind(pick_lport());
    if (q->sock < 0) { q->used = 0; return -1; }
    /* Kick the resolver's ARP without waiting for it: arp_warm() blocks, and
     * nothing on this path may. A cold cache costs one DNS retransmit. */
    { uint8_t mac[ETH_ALEN]; arp_resolve(net_cfg.dns, mac); }
    q->t0 = timer_ticks();
    dq_send(q);
    return id;
}

/* 0 = still pending, 0xFFFFFFFF = failed/timed out, else the IPv4 address.
 * Non-destructive: the slot lives until dns_query_free().
 *
 * A name that resolved ONLY to IPv6 addresses reports 0xFFFFFFFF here: there is
 * no IPv4 address to return and pretending otherwise would be worse. Callers
 * that can speak both families use dns_query_addrs() instead. */
uint32_t dns_query_result(int id)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used) return 0xFFFFFFFFu;
    struct dns_query *q = &dq[id];
    if (!q->done) return 0;
    if (q->failed || !q->ip) return 0xFFFFFFFFu;
    return q->ip;
}

/* 0 = still pending, < 0 = failed, else the number of destinations written to
 * `out`, MOST PREFERRED FIRST.
 *
 * The ordering is RFC 6724 section 6, evaluated here rather than at answer time
 * because it depends on which of our own addresses are usable RIGHT NOW -- an
 * address that has just been deprecated, or a router whose lifetime expired,
 * changes the answer without the DNS records changing at all. */
int dns_query_addrs(int id, ip6_addr *out, int max)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used || max <= 0) return -1;
    struct dns_query *q = &dq[id];
    if (!q->done) return 0;
    if (q->failed || q->naddr == 0) return -1;

    int n = q->naddr;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = q->addrs[i];

    struct ip6_src_cand cand[IP6_NADDR + 1];
    int nc = ip6_dual_candidates(cand, IP6_NADDR + 1);
    ip6_sort_dests(out, n, cand, nc);
    return n;
}

void dns_query_free(int id)
{
    if (id < 0 || id >= DNS_NQ || !dq[id].used) return;
    if (dq[id].sock >= 0) udp_close(dq[id].sock);
    dq[id].sock = -1;
    dq[id].used = 0;
}

/* Pumped from net_poll(): receive answers, retransmit, time out. Every slot
 * advances on every call, which is what makes the lookups concurrent. */
void dns_poll(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < DNS_NQ; i++) {
        struct dns_query *q = &dq[i];
        if (!q->used || q->done || q->sock < 0) continue;

        /* Drain: with two transactions in flight on one socket, the A and AAAA
         * responses are usually both already queued by the time we look. */
        uint8_t buf[512];
        uint32_t src; uint16_t sport;
        int rlen;
        int died = 0;
        while ((rlen = udp_recv(q->sock, buf, sizeof buf, &src, &sport)) != 0) {
            if (rlen < 0) {                 /* ICMP error against our port */
                q->done = 1; q->failed = 1;
                died = 1;
                break;
            }
            if (src != net_cfg.dns || sport != DNS_PORT)
                continue;                   /* someone else's datagram */
            int n = q->naddr;
            /* Which transaction is this? Both are matched, so a spoofed or
             * stale response for the other family cannot be mistaken for ours. */
            if (collect_answers(buf, rlen, q->txid, q->addrs, DNS_MAXA, &n) >= 0) {
                q->got4 = 1;
            } else if (q->want6 &&
                       collect_answers(buf, rlen, q->txid6, q->addrs, DNS_MAXA, &n) >= 0) {
                q->got6 = 1;
            } else {
                continue;                   /* neither: ignore, stay pending */
            }
            if (n > q->naddr) {
                q->naddr = (uint8_t)n;
                if (!q->ip)
                    for (int k = 0; k < n; k++)
                        if (ip6_is_v4mapped(&q->addrs[k])) {
                            q->ip = ip6_to_v4(&q->addrs[k]);
                            break;
                        }
            }
            if (!q->t_first) q->t_first = now;
        }
        if (died) continue;

        /* Finished when both families have answered -- or when one has and the
         * other has had its grace period. A name with an A record and a slow
         * (or filtered) AAAA must not stall the connection behind it. */
        int both = q->got4 && (!q->want6 || q->got6);
        int graced = q->t_first && (now - q->t_first >= DNS_2ND_GRACE);
        if (both || (q->t_first && graced)) {
            q->done = 1;
            q->failed = (q->naddr == 0);
            if (q->naddr) dns_cache_put_set(q->name, q->addrs, q->naddr);
            continue;
        }
        if (now - q->t0 >= DNS_GIVEUP) {
            /* Out of time. Whatever arrived is still usable: a lookup that got
             * an A record and never heard about AAAA has an answer. */
            q->done = 1;
            q->failed = (q->naddr == 0);
            if (q->naddr) dns_cache_put_set(q->name, q->addrs, q->naddr);
            continue;
        }
        if (now - q->last >= DNS_RETX)      dq_send(q);
    }
}
