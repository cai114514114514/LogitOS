/* hpool.c -- origin-keyed HTTP connection pool. See hpool.h for why.
 *
 * The only subtle rule: the key is (host, port, tls) and ALL THREE must match.
 * Reusing an https connection for an http request, or example.com's connection
 * for evil.com because they resolved to the same address, sends the request --
 * headers, Cookie and all -- to the wrong peer. Origin identity is a string
 * comparison here on purpose; it is never inferred from the socket.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "hpool.h"

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int host_eq(const char *a, const char *b)
{
    while (*a && *b) { if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static int same_origin(const struct hp_conn *c, const char *host, int port, int tls)
{
    return c->used && c->port == port && c->tls == tls && host_eq(c->host, host);
}

void hpool_init(struct hpool *p)
{
    if (!p) return;
    memset(p, 0, sizeof *p);
    /* 6, not 8: the TCP stack has 8 slots total and a DNS retry or a redirect
     * to a new origin must still be able to get one.  A pool that can consume
     * every slot deadlocks the page it is trying to speed up. */
    p->max_total = 6;
    p->max_per_host = 4;
    p->idle_ms = 10000;
    p->max_reqs = 100;
    p->max_streams = 16;           /* matches H2_MAX_STREAMS in http2.h */
}

void hpool_config(struct hpool *p, int max_total, int max_per_host, int64_t idle_ms)
{
    if (!p) return;
    if (max_total > 0) p->max_total = max_total > HP_MAX_CONNS ? HP_MAX_CONNS : max_total;
    if (max_per_host > 0) p->max_per_host = max_per_host;
    if (idle_ms > 0) p->idle_ms = idle_ms;
}

void hpool_set_closer(struct hpool *p, void (*fn)(int fd, void *ctx, void *user), void *user)
{
    if (!p) return;
    p->closer = fn;
    p->user = user;
}

static void slot_close(struct hpool *p, int i)
{
    if (!p->v[i].used) return;
    if (p->closer) p->closer(p->v[i].fd, p->v[i].ctx, p->user);
    memset(&p->v[i], 0, sizeof p->v[i]);
    p->closed++;
}

int hpool_count(const struct hpool *p)
{
    int n = 0;
    if (!p) return 0;
    for (int i = 0; i < HP_MAX_CONNS; i++) if (p->v[i].used) n++;
    return n;
}

int hpool_idle_count(const struct hpool *p)
{
    int n = 0;
    if (!p) return 0;
    for (int i = 0; i < HP_MAX_CONNS; i++) if (p->v[i].used && !p->v[i].in_use) n++;
    return n;
}

int hpool_count_origin(const struct hpool *p, const char *host, int port, int tls)
{
    int n = 0;
    if (!p || !host) return 0;
    for (int i = 0; i < HP_MAX_CONNS; i++) if (same_origin(&p->v[i], host, port, tls)) n++;
    return n;
}

int hpool_fd(const struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return -1;
    return p->v[slot].fd;
}

void *hpool_ctx(const struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return NULL;
    return p->v[slot].ctx;
}

int hpool_expire(struct hpool *p, int64_t now)
{
    int n = 0;
    if (!p) return 0;
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (!p->v[i].used || p->v[i].in_use) continue;
        if (now - p->v[i].idle_since >= p->idle_ms) { slot_close(p, i); n++; }
    }
    p->evicted += n;
    return n;
}

void hpool_close_all(struct hpool *p)
{
    if (!p) return;
    for (int i = 0; i < HP_MAX_CONNS; i++) slot_close(p, i);
}

int hpool_acquire(struct hpool *p, const char *host, int port, int tls, int64_t now)
{
    if (!p || !host) return -1;
    hpool_expire(p, now);
    /* Most-recently-idled first: it is the one the server is least likely to
     * have reaped, so it is the one least likely to fail on first write. */
    int best = -1;
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (!same_origin(&p->v[i], host, port, tls) || p->v[i].in_use) continue;
        /* h2 and still-undecided connections are not HTTP/1.1 connections. An
         * HTTP/1.1 request written onto an h2 socket is not a request, it is a
         * malformed frame header. */
        if (p->v[i].proto != HP_PROTO_H1) continue;
        if (p->max_reqs > 0 && p->v[i].reqs >= p->max_reqs) { slot_close(p, i); continue; }
        if (best < 0 || p->v[i].idle_since > p->v[best].idle_since) best = i;
    }
    if (best < 0) { p->misses++; return -1; }
    p->v[best].in_use = 1;
    p->v[best].reqs++;
    p->hits++;
    return best;
}

int hpool_acquire_mux(struct hpool *p, const char *host, int port, int tls, int64_t now)
{
    if (!p || !host) return -1;
    hpool_expire(p, now);
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (!same_origin(&p->v[i], host, port, tls)) continue;
        if (p->v[i].proto != HP_PROTO_H2) continue;
        if (p->max_streams > 0 && p->v[i].streams >= p->max_streams) continue;
        p->v[i].streams++;
        if (p->v[i].streams > p->v[i].peak_streams) p->v[i].peak_streams = p->v[i].streams;
        p->v[i].in_use = 1;
        p->v[i].reqs++;
        p->hits++;
        p->mux_hits++;
        return i;
    }
    p->misses++;
    return -1;
}

void hpool_set_proto(struct hpool *p, int slot, int proto)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return;
    if (proto != HP_PROTO_H1 && proto != HP_PROTO_H2 && proto != HP_PROTO_PENDING) return;
    if (proto == HP_PROTO_H2 && p->v[slot].proto != HP_PROTO_H2) p->h2_conns++;
    p->v[slot].proto = proto;
    /* The dial that discovered h2 counts as this connection's first stream;
     * anything else double-counts the request that is already running on it. */
    if (proto == HP_PROTO_H2 && p->v[slot].in_use && p->v[slot].streams == 0) {
        p->v[slot].streams = 1;
        if (p->v[slot].peak_streams < 1) p->v[slot].peak_streams = 1;
    }
}

int hpool_proto(const struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return -1;
    return p->v[slot].proto;
}

int hpool_streams(const struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return -1;
    return p->v[slot].streams;
}

int hpool_count_mux(const struct hpool *p, const char *host, int port, int tls)
{
    int n = 0;
    if (!p || !host) return 0;
    for (int i = 0; i < HP_MAX_CONNS; i++)
        if (same_origin(&p->v[i], host, port, tls) &&
            (p->v[i].proto == HP_PROTO_H2 || p->v[i].proto == HP_PROTO_PENDING)) n++;
    return n;
}

/* Oldest idle connection NOT belonging to `host:port:tls`, or -1.
 *
 * HTTP/1.1 connections are sacrificed first. An idle h2 connection is worth
 * more than an idle h1 one: it is the ONLY connection its origin has, it holds
 * a warm HPACK context that took several requests to build, and re-dialling it
 * costs a TLS handshake plus that context. So an h2 connection is evicted only
 * when there is no h1 connection to take instead. */
static int oldest_idle_kind(const struct hpool *p, const char *host, int port, int tls, int h2)
{
    int best = -1;
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (!p->v[i].used || p->v[i].in_use) continue;
        if (same_origin(&p->v[i], host, port, tls)) continue;
        if ((p->v[i].proto == HP_PROTO_H2) != (h2 != 0)) continue;
        if (best < 0 || p->v[i].idle_since < p->v[best].idle_since) best = i;
    }
    return best;
}

static int oldest_idle_other(const struct hpool *p, const char *host, int port, int tls)
{
    int v = oldest_idle_kind(p, host, port, tls, 0);
    if (v >= 0) return v;
    return oldest_idle_kind(p, host, port, tls, 1);
}

int hpool_may_open(struct hpool *p, const char *host, int port, int tls, int64_t now)
{
    if (!p || !host || (int)strlen(host) >= HP_HOST_MAX) return 0;
    hpool_expire(p, now);
    /* One connection per origin once h2 is in play -- and while a dial to this
     * origin is still deciding, because a caller that opens six connections
     * during the handshake window ends up with six h2 connections carrying one
     * stream each: six HPACK contexts, six sets of windows, and none of the
     * multiplexing that was the reason to speak h2 at all. */
    if (hpool_count_mux(p, host, port, tls) > 0) return 0;
    if (hpool_count_origin(p, host, port, tls) >= p->max_per_host) return 0;
    if (hpool_count(p) < p->max_total) return 1;
    /* At the global cap: an idle connection to some other origin is a
     * legitimate thing to sacrifice, an in-use one is not. */
    int victim = oldest_idle_other(p, host, port, tls);
    if (victim < 0) return 0;
    slot_close(p, victim);
    p->evicted++;
    return 1;
}

int hpool_admit(struct hpool *p, const char *host, int port, int tls,
                int fd, void *ctx, int64_t now)
{
    return hpool_admit_proto(p, host, port, tls, fd, ctx, HP_PROTO_H1, now);
}

int hpool_admit_proto(struct hpool *p, const char *host, int port, int tls,
                      int fd, void *ctx, int proto, int64_t now)
{
    if (!p || !host) return -1;
    if (proto != HP_PROTO_H1 && proto != HP_PROTO_H2 && proto != HP_PROTO_PENDING) return -1;
    int hl = (int)strlen(host);
    if (hl <= 0 || hl >= HP_HOST_MAX) return -1;
    if (!hpool_may_open(p, host, port, tls, now)) return -1;
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (p->v[i].used) continue;
        memset(&p->v[i], 0, sizeof p->v[i]);
        for (int k = 0; k < hl; k++) p->v[i].host[k] = (char)lc((unsigned char)host[k]);
        p->v[i].host[hl] = 0;
        p->v[i].port = port;
        p->v[i].tls = tls;
        p->v[i].fd = fd;
        p->v[i].ctx = ctx;
        p->v[i].used = 1;
        p->v[i].in_use = 1;
        p->v[i].reqs = 1;
        p->v[i].idle_since = now;
        p->v[i].proto = proto;
        if (proto == HP_PROTO_H2) { p->v[i].streams = 1; p->v[i].peak_streams = 1; p->h2_conns++; }
        p->opened++;
        return i;
    }
    return -1;
}

void hpool_release(struct hpool *p, int slot, int reusable, int64_t now)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return;
    if (!reusable) { slot_close(p, slot); return; }

    if (p->v[slot].proto == HP_PROTO_H2) {
        /* One stream finished. The connection stays in use while its siblings
         * are still running, and max_reqs deliberately does NOT recycle it --
         * closing an h2 connection cancels every stream on it, so "retire
         * after N requests" would be a bug rather than hygiene. */
        if (p->v[slot].streams > 0) p->v[slot].streams--;
        if (p->v[slot].streams == 0) {
            p->v[slot].in_use = 0;
            p->v[slot].idle_since = now;
        }
        return;
    }
    if (p->max_reqs > 0 && p->v[slot].reqs >= p->max_reqs) { slot_close(p, slot); return; }
    p->v[slot].in_use = 0;
    p->v[slot].idle_since = now;
}

void hpool_drop(struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS) return;
    slot_close(p, slot);
}
