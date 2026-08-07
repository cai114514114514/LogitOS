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
        if (p->max_reqs > 0 && p->v[i].reqs >= p->max_reqs) { slot_close(p, i); continue; }
        if (best < 0 || p->v[i].idle_since > p->v[best].idle_since) best = i;
    }
    if (best < 0) { p->misses++; return -1; }
    p->v[best].in_use = 1;
    p->v[best].reqs++;
    p->hits++;
    return best;
}

/* Oldest idle connection NOT belonging to `host:port:tls`, or -1. */
static int oldest_idle_other(const struct hpool *p, const char *host, int port, int tls)
{
    int best = -1;
    for (int i = 0; i < HP_MAX_CONNS; i++) {
        if (!p->v[i].used || p->v[i].in_use) continue;
        if (same_origin(&p->v[i], host, port, tls)) continue;
        if (best < 0 || p->v[i].idle_since < p->v[best].idle_since) best = i;
    }
    return best;
}

int hpool_may_open(struct hpool *p, const char *host, int port, int tls, int64_t now)
{
    if (!p || !host || (int)strlen(host) >= HP_HOST_MAX) return 0;
    hpool_expire(p, now);
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
    if (!p || !host) return -1;
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
        p->opened++;
        return i;
    }
    return -1;
}

void hpool_release(struct hpool *p, int slot, int reusable, int64_t now)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS || !p->v[slot].used) return;
    if (!reusable || (p->max_reqs > 0 && p->v[slot].reqs >= p->max_reqs)) {
        slot_close(p, slot);
        return;
    }
    p->v[slot].in_use = 0;
    p->v[slot].idle_since = now;
}

void hpool_drop(struct hpool *p, int slot)
{
    if (!p || slot < 0 || slot >= HP_MAX_CONNS) return;
    slot_close(p, slot);
}
