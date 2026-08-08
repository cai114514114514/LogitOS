/* Ring-3 runtime shims for the render pipeline (M17 L1) + the resource fetcher.
 *
 * net/{dom,css,layout}.c are compiled into browser.aex unchanged; they reference
 * kernel symbols (kmalloc/kfree/text_measure/res_fetch/img_*). Map those onto the
 * app's mini-libc + the new render syscalls here.
 *
 * The second half of this file is `bfetch` -- see bfetch.h for why it exists.
 * The short version: every sub-resource used to go through SYS_RES_FETCH, which
 * is the kernel's one-request-at-a-time client with `Connection: close`, so a
 * page paid one full TLS handshake per resource and the machine could not poll
 * the network while it did. bfetch runs the same fetches over the non-blocking
 * socket ABI with a keep-alive connection pool, in ring 3. */
#include "logit.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "img.h"
#include "url.h"
#include "http1.h"
#include "hpool.h"
#include "bfetch.h"

void *malloc(size_t);
void  free(void *);
int   printf(const char *, ...);

void *kmalloc(unsigned long n) { return malloc((size_t)n); }
void  kfree(void *p) { free(p); }

/* Layout measures every word; route to the kernel font engine. */
int text_measure(const char *s, int len, int px, int mono)
{
    return text_measure_px(s, len, px, mono);
}

/* ====================== bfetch: the resource fetcher ====================== */

#define BF_NREQ    16          /* concurrent requests the table can hold */
#define BF_URLMAX 768          /* URL_HOST_MAX + URL_PATH_MAX + scheme + port */
#define BF_HOPS      8         /* redirect hops before giving up */
/* Wall-clock cap on ONE request (each redirect hop gets its own). 60 s, not 30:
 * a 216 KB gzipped stylesheet over TLS on an emulated CPU takes tens of seconds
 * when the host is busy, and a cap that expires mid-transfer turns a slow load
 * into a failed one -- which then looks like a protocol bug rather than a
 * scheduling one. */
#define BF_REQ_MS 60000

/* The whole request, redirects and retries included. BF_REQ_MS bounds one
 * ATTEMPT's silence; this bounds the product, which nothing did. Long enough
 * that a slow multi-hop page still lands, short enough that a user waits
 * rather than concluding the machine is dead -- which they will, because
 * load() is not reentrant and answers only its close button meanwhile. */
#define BF_TOTAL_MS 90000

enum { RQ_FREE = 0, RQ_QUEUED, RQ_DIAL, RQ_XFER, RQ_DONE, RQ_FAIL };

struct breq {
    int   state;
    char  url[BF_URLMAX];
    struct url u;                    /* url parsed */
    int   hops;
    int   fd;                        /* socket handle, -1 when none */
    int   pslot;                     /* hpool slot, -1 when none */
    int   reused;                    /* this attempt rode a pooled connection */
    int   retried;                   /* already re-dialled once after a dead conn */
    int   c_live;                    /* h1_conn needs freeing */
    struct h1_conn c;
    unsigned char *body;
    int   blen;
    int   status;
    const char *err;
    unsigned long long t0;
    /* When the whole request started, redirects and retries included. t0 is a
     * per-attempt IDLE deadline and every hop resets it, which is right for a
     * hop and wrong for the product: BF_HOPS is 8 and BF_REQ_MS is 60 s, so a
     * chain that keeps redirecting could legitimately live for eight minutes,
     * times the retries. https://bing.com is three hops on its own.
     *
     * That is the same shape as the kernel-side bug fixed in 7131e34 -- "every
     * layer has its own timeout and they MULTIPLY, with nothing bounding the
     * product" -- and it is worse here only because load() is not yet
     * reentrant, so every one of those minutes is a browser that answers its
     * close button and drops every other keystroke. */
    unsigned long long t_start;
};

static struct breq g_req[BF_NREQ];
static struct hpool g_pool;
static int  g_pool_ready;
static char g_base[BF_URLMAX] = "about:blank";
static int  g_dials, g_reuses, g_reqs;

static void pool_closer(int fd, void *ctx, void *user)
{
    (void)ctx; (void)user;
    if (fd >= 0) sock_close(fd);
}

void bfetch_init(void)
{
    if (g_pool_ready) return;
    hpool_init(&g_pool);
    /* TWO connections per host, not the four a desktop browser opens.
     *
     * This is a real trade and it goes the other way here. On hardware, more
     * connections per origin is faster because a handshake is cheap and
     * latency dominates. On an emulated CPU a TLS handshake is an RSA or ECDSA
     * chain verification done in software and costs SECONDS, so every extra
     * connection is a second of wall clock spent to overlap requests that were
     * going to take milliseconds each once the connection existed.
     *
     * Measured on en.wikipedia.org/wiki/Operating_system: four-per-host gave 4
     * to 10 handshakes run to run, and the spread was connections the CDN had
     * already closed being re-dialled. Total 6 leaves both origins their two
     * with room to spare, so nothing is evicted for want of a slot -- eviction
     * was the other half of that spread.
     *
     * The idle timeout is 60 s rather than 10: a load has long gaps in it (the
     * whole cascade and layout run between the stylesheet phase and the script
     * phase) and a short timeout means WE close connections that are still
     * perfectly good. Holding them longer is safe because req_connect polls a
     * pooled socket before spending a request on it. */
    hpool_config(&g_pool, 6, 2, 60000);
    hpool_set_closer(&g_pool, pool_closer, 0);
    for (int i = 0; i < BF_NREQ; i++) { g_req[i].state = RQ_FREE; g_req[i].fd = -1; g_req[i].pslot = -1; }
    g_pool_ready = 1;
}

void bfetch_set_base(const char *page_url)
{
    int i = 0;
    if (page_url) while (page_url[i] && i < BF_URLMAX - 1) { g_base[i] = page_url[i]; i++; }
    g_base[i] = 0;
}

/* RFC 3986 5.2.4 remove_dot_segments, applied to the path of an already
 * absolute URL. url.c's url_resolve concatenates instead, so "./chunk.js"
 * against ".../assets/index.js" comes out as ".../assets/./chunk.js" -- which
 * some servers normalise and some 404 on. A module graph cannot depend on which
 * kind it is talking to. */
static void norm_dots(char *url)
{
    /* find the start of the path: after "scheme://host[:port]" */
    char *p = url;
    while (*p && *p != ':') p++;
    if (p[0] != ':' || p[1] != '/' || p[2] != '/') return;
    p += 3;
    while (*p && *p != '/') p++;
    if (!*p) return;
    char *path = p;

    /* Dots inside a query or fragment are data, not path segments. */
    char *q = path;
    while (*q && *q != '?' && *q != '#') q++;
    int plen = (int)(q - path);
    if (plen <= 0 || plen >= URL_PATH_MAX) return;

    char in[URL_PATH_MAX], out[URL_PATH_MAX];
    memcpy(in, path, (size_t)plen); in[plen] = 0;

    int o = 0, i = 0;
    while (in[i]) {
        if (in[i] == '/' && in[i + 1] == '.' && (in[i + 2] == '/' || in[i + 2] == 0)) {
            i += 2;
            if (!in[i] && o < URL_PATH_MAX - 1) out[o++] = '/';
            continue;
        }
        if (in[i] == '/' && in[i + 1] == '.' && in[i + 2] == '.' &&
            (in[i + 3] == '/' || in[i + 3] == 0)) {
            i += 3;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                       /* drop the separator too */
            if (!in[i] && o < URL_PATH_MAX - 1) out[o++] = '/';
            continue;
        }
        if (o < URL_PATH_MAX - 1) out[o++] = in[i];
        i++;
        while (in[i] && in[i] != '/') { if (o < URL_PATH_MAX - 1) out[o++] = in[i]; i++; }
    }
    if (o == 0) out[o++] = '/';

    /* Normalisation only ever shortens, so the result fits where the old path
     * was; move the query/fragment down first, then lay the path over it. */
    int tl = 0; while (q[tl]) tl++;
    memmove(path + o, q, (size_t)tl + 1);
    memcpy(path, out, (size_t)o);
}

int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{
    if (!ref || !out || max < 2) return -1;
    const char *b = base && base[0] ? base : g_base;
    struct url bu;
    if (url_parse(b, &bu) != 0) {
        /* base is not an absolute http(s) URL (about:blank, or an inline
         * script's synthetic name): the reference has to stand alone. */
        struct url t;
        if (url_parse(ref, &t) != 0) return -1;
        int i = 0; while (ref[i] && i < max - 1) { out[i] = ref[i]; i++; }
        out[i] = 0;
        return 0;
    }
    if (url_resolve(&bu, ref, out, max) != 0) return -1;
    struct url chk;
    if (url_parse(out, &chk) != 0) return -1;
    norm_dots(out);
    return 0;
}

/* ---- transport: h1_transport over the non-blocking socket ABI ---- */

static int tr_read(void *ctx, void *buf, int len)
{
    struct breq *r = (struct breq *)ctx;
    int n = sock_recv(r->fd, buf, len);
    if (n > 0) return n;
    int bits = sock_poll(r->fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return H1_TERR;
    /* Drain first, THEN believe EOF: the EOF bit can be set while bytes are
     * still sitting in the receive ring, and treating that as end-of-message
     * truncates the last response on every connection the server closes. */
    if (n < 0 || (bits & SOCK_P_EOF)) return H1_EOF;
    return H1_AGAIN;
}

static int tr_write(void *ctx, const void *buf, int len)
{
    struct breq *r = (struct breq *)ctx;
    int n = sock_send(r->fd, buf, len);
    if (n > 0) return n;
    if (n < 0) return H1_TERR;
    return H1_AGAIN;
}

/* Write-side readiness only. http1.c never asks about reading -- SOCK_P_READABLE
 * is derived from tcp_available() and tls_pending(), neither of which can see a
 * complete TLS record already pulled off the wire and not yet decrypted, so it
 * reports "quiet" on a connection with a whole response sitting in it. The
 * writable bit has no such problem: it comes from the socket's own transmit
 * ring, which is the thing it describes. */
static int tr_poll(void *ctx, int want_write)
{
    struct breq *r = (struct breq *)ctx;
    if (!want_write) return 1;                  /* never trusted; read anyway */
    int bits = sock_poll(r->fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return -1;
    return (bits & SOCK_P_WRITABLE) ? 1 : 0;
}

/* ---- request lifecycle ---- */

static void req_drop_conn(struct breq *r, int reusable)
{
    if (r->c_live) { h1_conn_free(&r->c); r->c_live = 0; }
    if (r->pslot >= 0) {
        if (reusable) hpool_release(&g_pool, r->pslot, 1, (int64_t)monotonic_ms());
        else          hpool_drop(&g_pool, r->pslot);
        r->pslot = -1;
        r->fd = -1;                 /* the pool owns the socket now (or closed it) */
    } else if (r->fd >= 0) {
        sock_close(r->fd);
        r->fd = -1;
    }
}

static void req_fail(struct breq *r, const char *why)
{
    req_drop_conn(r, 0);
    r->err = why;
    r->state = RQ_FAIL;
}

/* Serialise the GET for r->u. Returns a malloc'd buffer h1_conn_start owns. */
static char *build_get(struct breq *r, int *outlen)
{
    struct h1_request q;
    if (h1_request_init(&q, "GET", r->u.path) != H1_OK) return 0;
    char hostport[URL_HOST_MAX + 8];
    int o = 0;
    for (const char *p = r->u.host; *p && o < (int)sizeof hostport - 8; p++) hostport[o++] = *p;
    int defport = r->u.https ? 443 : 80;
    if (r->u.port != defport) {
        hostport[o++] = ':';
        char t[6]; int i = 0; unsigned v = r->u.port;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) hostport[o++] = t[--i];
    }
    hostport[o] = 0;
    h1_request_set_header(&q, "Host", hostport);
    h1_request_set_header(&q, "User-Agent",
                          "Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0");
    h1_request_set_header(&q, "Accept", "*/*");
    h1_request_set_header(&q, "Accept-Encoding", h1_accept_encoding());
    /* The entire point: no `Connection: close`. */
    h1_request_set_header(&q, "Connection", "keep-alive");
    char *buf = 0; int len = 0;
    int rc = h1_request_build(&q, &buf, &len);
    h1_request_free(&q);
    if (rc != H1_OK) return 0;
    *outlen = len;
    return buf;
}

static int req_begin_exchange(struct breq *r)
{
    int len = 0;
    char *req = build_get(r, &len);
    if (!req) { req_fail(r, "could not build request"); return 0; }
    struct h1_transport t = { tr_read, tr_write, tr_poll, r };
    if (h1_conn_start(&r->c, &t, req, len) != H1_OK) {
        free(req);
        req_fail(r, "h1_conn_start failed");
        return 0;
    }
    r->c_live = 1;
    r->state = RQ_XFER;
    return 1;
}

/* Move a queued request onto a connection: reuse one from the pool if the
 * origin already has an idle one, else dial. */
static void req_connect(struct breq *r)
{
    int64_t now = (int64_t)monotonic_ms();
    int tls = r->u.https;
    int slot = hpool_acquire(&g_pool, r->u.host, r->u.port, tls, now);
    if (slot >= 0) {
        /* A connection the server closed while it sat idle looks exactly like a
         * live one to the pool, which only knows an fd. Ask the socket before
         * spending a request on it: this is a cheap check that turns "send,
         * get EOF, retry" into "dial", and it is the difference between a
         * failed sub-resource and a slower one when the retry budget is spent. */
        int fd = hpool_fd(&g_pool, slot);
        int bits = sock_poll(fd);
        if (bits < 0 || (bits & (SOCK_P_EOF | SOCK_P_ERROR))) {
            hpool_drop(&g_pool, slot);
        } else {
            r->pslot = slot;
            r->fd = fd;
            r->reused = 1;
            g_reuses++;
            req_begin_exchange(r);
            return;
        }
    }
    if (!hpool_may_open(&g_pool, r->u.host, r->u.port, tls, now))
        return;                                   /* caps full: try again next pump */
    int flags = tls ? (SOCK_F_TLS | SOCK_F_ALPN_HTTP11) : 0;
    int fd = sock_open(r->u.host, r->u.port, flags);
    if (fd < 0) { req_fail(r, "sock_open failed"); return; }
    slot = hpool_admit(&g_pool, r->u.host, r->u.port, tls, fd, 0, now);
    if (slot < 0) { sock_close(fd); return; }      /* raced another request; retry */
    r->pslot = slot;
    r->fd = fd;
    r->reused = 0;
    g_dials++;
    r->state = RQ_DIAL;
}

/* Repeat the request once, on a connection we dial ourselves.
 *
 * This started out as "only if the connection came from the pool", on the
 * reasoning that a pooled connection the server had already closed looks
 * exactly like a live one until you write to it. That was too narrow, and one
 * measured run showed why: a FRESH connection to en.wikipedia.org returned
 * `connection closed mid-message` on the main document, and because the
 * connection was not pooled there was no retry and the whole page load failed.
 *
 * A GET is idempotent. Replaying one costs a request and can save the page, so
 * the only condition worth keeping is that it happens at most once -- otherwise
 * a genuinely broken server becomes an infinite loop. */
static int req_retry_fresh(struct breq *r)
{
    if (r->retried) return 0;
    r->retried = 1;
    printf("[bfetch] retrying on a fresh connection: %s\n", r->url);
    req_drop_conn(r, 0);
    r->state = RQ_QUEUED;
    r->reused = 0;
    r->t0 = monotonic_ms();
    return 1;
}

/* t0 is this attempt s idle deadline; t_start is the age of the whole
 * request. A hop resets t0 on purpose -- a redirect really is a fresh
 * exchange -- and nothing may reset t_start, which is the point. */
static int req_expired(const struct breq *r)
{
    unsigned long long now = monotonic_ms();
    return (now - r->t0 > BF_REQ_MS) || (now - r->t_start > BF_TOTAL_MS);
}

static void req_step_xfer(struct breq *r)
{
    /* h1_conn_pump reads at most 4 KiB per call so that one fast server cannot
     * starve the others. That is the right rule per REQUEST, but calling it once
     * per yield would leave the kernel's 64 KiB receive ring full and the TCP
     * window shut for a 1.5 MB bundle -- so drain while bytes are actually
     * moving, bounded, and stop the moment they stop.
     *
     * 64 * 4 KiB = 256 KiB per pass, four times the receive ring. Anything less
     * and a slow pass (a repaint, a decode, another agent's QEMU on the same
     * host CPU) lets the ring overrun; wikipedia's 216 KB stylesheet is the
     * case that found this.
     *
     * The loop used to stop on `sock_poll` no longer reporting SOCK_P_READABLE,
     * and that was half of the stall this file existed to avoid. sock_poll sees
     * the WIRE: tls_rec_pull() drains the whole TCP receive buffer into the TLS
     * session's record buffer, so one read of one record leaves tcp_available()
     * and tls_pending() both at zero with several complete records still
     * undecrypted. The socket says quiet and it is not. Ask the exchange what it
     * moved instead -- that answer comes from the read itself and is true at
     * every layer. */
    int st = H1_C_RECV;
    for (int k = 0; k < 64; k++) {
        unsigned long long before = h1_conn_progress(&r->c);
        st = h1_conn_pump(&r->c);
        if (st != H1_C_SEND && st != H1_C_RECV) break;
        if (h1_conn_progress(&r->c) == before) break;      /* nothing moved */
    }
    if (st == H1_C_SEND || st == H1_C_RECV) {
        if (req_expired(r)) req_fail(r, "timed out");
        return;
    }
    if (st == H1_C_ERROR) {
        if (req_retry_fresh(r)) return;
        req_fail(r, h1_strerror(r->c.err ? r->c.err : r->c.resp.err));
        return;
    }

    /* H1_C_DONE */
    struct h1_response *resp = &r->c.resp;
    r->status = resp->code;
    int keep = resp->keep_alive && !resp->must_close && r->c.spill_len == 0;

    if (h1_is_redirect(resp->code)) {
        const char *loc = h1_headers_get(&resp->hdr, "location");
        if (loc && r->hops < BF_HOPS) {
            char next[BF_URLMAX];
            if (bfetch_resolve(r->url, loc, next, sizeof next) == 0) {
                req_drop_conn(r, keep);
                r->hops++;
                r->retried = 0;
                r->t0 = monotonic_ms();       /* each hop gets its own budget */
                int i = 0; while (next[i] && i < BF_URLMAX - 1) { r->url[i] = next[i]; i++; }
                r->url[i] = 0;
                if (url_parse(r->url, &r->u) != 0) { req_fail(r, "bad redirect target"); return; }
                r->state = RQ_QUEUED;
                return;
            }
        }
    }

    if (h1_decode_body(resp) != H1_OK) { req_drop_conn(r, 0); r->err = "bad Content-Encoding"; r->state = RQ_FAIL; return; }

    /* Steal the body: h1_conn_free would otherwise take it with the response. */
    r->body = resp->body;
    r->blen = resp->body_len;
    resp->body = 0; resp->body_len = 0; resp->body_cap = 0;

    req_drop_conn(r, keep);
    r->state = RQ_DONE;
}

int bfetch_pump(void)
{
    if (!g_pool_ready) bfetch_init();
    int pending = 0;
    for (int i = 0; i < BF_NREQ; i++) {
        struct breq *r = &g_req[i];
        if (r->state == RQ_FREE || r->state == RQ_DONE || r->state == RQ_FAIL) continue;
        pending++;
        if (req_expired(r)) { req_fail(r, "timed out"); continue; }
        if (r->state == RQ_QUEUED) { req_connect(r); continue; }
        if (r->state == RQ_DIAL) {
            int bits = sock_poll(r->fd);
            if (bits < 0 || (bits & SOCK_P_ERROR)) {
                req_fail(r, bits < 0 ? "socket error" : "connect failed");
                continue;
            }
            if (bits & SOCK_P_CONNECTED) req_begin_exchange(r);
            continue;
        }
        if (r->state == RQ_XFER) req_step_xfer(r);
    }
    hpool_expire(&g_pool, (int64_t)monotonic_ms());
    return pending;
}

int bfetch_start_from(const char *base, const char *ref)
{
    if (!g_pool_ready) bfetch_init();
    char abs[BF_URLMAX];
    if (bfetch_resolve(base, ref, abs, sizeof abs) != 0) return -1;
    for (int i = 0; i < BF_NREQ; i++) {
        struct breq *r = &g_req[i];
        if (r->state != RQ_FREE) continue;
        memset(r, 0, sizeof *r);
        r->fd = -1; r->pslot = -1;
        int n = 0; while (abs[n] && n < BF_URLMAX - 1) { r->url[n] = abs[n]; n++; }
        r->url[n] = 0;
        if (url_parse(r->url, &r->u) != 0) { r->state = RQ_FREE; return -1; }
        r->t0 = monotonic_ms();
        r->t_start = r->t0;                 /* the one clock no hop may reset */
        r->state = RQ_QUEUED;
        g_reqs++;
        req_connect(r);                     /* a free slot dials immediately */
        return i;
    }
    return -1;
}

int bfetch_start(const char *ref) { return bfetch_start_from(0, ref); }

static struct breq *req_of(int id)
{
    if (id < 0 || id >= BF_NREQ) return 0;
    if (g_req[id].state == RQ_FREE) return 0;
    return &g_req[id];
}

int bfetch_state(int id)
{
    struct breq *r = req_of(id);
    if (!r) return BF_FAILED;
    return r->state == RQ_DONE ? BF_DONE : r->state == RQ_FAIL ? BF_FAILED : BF_PENDING;
}

int bfetch_status(int id) { struct breq *r = req_of(id); return r ? r->status : 0; }
const char *bfetch_url(int id) { struct breq *r = req_of(id); return r ? r->url : ""; }
const char *bfetch_error(int id)
{ struct breq *r = req_of(id); return r && r->err ? r->err : "no error"; }

const unsigned char *bfetch_body(int id, int *len)
{
    struct breq *r = req_of(id);
    if (!r || r->state != RQ_DONE) { if (len) *len = 0; return 0; }
    if (len) *len = r->blen;
    return r->body;
}

int bfetch_take(int id, unsigned char **out)
{
    struct breq *r = req_of(id);
    if (!r || r->state != RQ_DONE || !r->body) { bfetch_release(id); return -1; }
    *out = r->body;
    int n = r->blen;
    r->body = 0; r->blen = 0;
    bfetch_release(id);
    return n;
}

void bfetch_release(int id)
{
    struct breq *r = req_of(id);
    if (!r) return;
    req_drop_conn(r, 0);
    free(r->body);
    memset(r, 0, sizeof *r);
    r->fd = -1; r->pslot = -1;
    r->state = RQ_FREE;
}

void bfetch_wait(int id, void (*tick)(void))
{
    for (;;) {
        int pending = bfetch_pump();
        if (id >= 0) { if (bfetch_state(id) != BF_PENDING) return; }
        else if (pending == 0) return;
        if (tick) tick();
        /* Yield so the WM thread runs net_poll(), which is what advances every
         * socket. Nothing above ever blocks in the kernel, so the desktop and
         * every other app keep running while this loop spins. */
        sys_yield();
    }
}

static void (*g_tick)(void);
void bfetch_set_tick(void (*fn)(void)) { g_tick = fn; }

int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{
    int id = bfetch_start(ref);
    if (id < 0) return -1;
    bfetch_wait(id, g_tick);
    if (bfetch_state(id) != BF_DONE || bfetch_status(id) / 100 != 2) {
        bfetch_release(id);
        return -1;
    }
    int n = bfetch_take(id, out);
    if (n < 0) return -1;
    *outlen = n;
    return 0;
}

void bfetch_stats(int *dials, int *reuses, int *requests)
{
    if (dials) *dials = g_dials;
    if (reuses) *reuses = g_reuses;
    if (requests) *requests = g_reqs;
}

/* The pool's own view, which answers a different question from the one above:
 * when a page dials more than it should have, `evicted` says it ran out of
 * slots and `closed` says a connection went away. Without these the two are
 * indistinguishable from the request side.
 *
 * hpool's `misses` counter is deliberately NOT surfaced: hpool_acquire is
 * called on every pump pass for every queued request, so it counts polling
 * attempts (tens of thousands) rather than requests, and printing it invites
 * exactly the wrong conclusion. */
void bfetch_pool_stats(int *hits, int *evicted, int *closed)
{
    if (hits) *hits = g_pool.hits;
    if (evicted) *evicted = g_pool.evicted;
    if (closed) *closed = g_pool.closed;
}
void bfetch_reset_stats(void) { g_dials = g_reuses = g_reqs = 0; }

void bfetch_close_all(void)
{
    for (int i = 0; i < BF_NREQ; i++) if (g_req[i].state != RQ_FREE) bfetch_release(i);
    if (g_pool_ready) hpool_close_all(&g_pool);
}

/* ---- the prefetch cache ---- */

#define BF_NCACHE 32

struct bcache {
    char  url[BF_URLMAX];
    int   id;                        /* in flight, or -1 */
    unsigned char *data;
    int   len;
    int   used;
};
static struct bcache g_cache[BF_NCACHE];

void bfetch_cache_clear(void)
{
    for (int i = 0; i < BF_NCACHE; i++) {
        if (g_cache[i].id >= 0 && g_cache[i].used) bfetch_release(g_cache[i].id);
        free(g_cache[i].data);
        memset(&g_cache[i], 0, sizeof g_cache[i]);
    }
}

void bfetch_prefetch(const char *ref)
{
    if (!g_pool_ready) bfetch_init();
    char abs[BF_URLMAX];
    if (bfetch_resolve(0, ref, abs, sizeof abs) != 0) return;
    for (int i = 0; i < BF_NCACHE; i++)
        if (g_cache[i].used && strcmp(g_cache[i].url, abs) == 0) return;   /* already have it */
    for (int i = 0; i < BF_NCACHE; i++) {
        if (g_cache[i].used) continue;
        int id = bfetch_start(abs);
        if (id < 0) return;                      /* table full: the caller falls back to sync */
        memcpy(g_cache[i].url, abs, sizeof g_cache[i].url);
        g_cache[i].id = id;
        g_cache[i].data = 0; g_cache[i].len = 0;
        g_cache[i].used = 1;
        return;
    }
}

void bfetch_prefetch_wait(void)
{
    for (;;) {
        int live = 0;
        bfetch_pump();
        for (int i = 0; i < BF_NCACHE; i++) {
            struct bcache *e = &g_cache[i];
            if (!e->used || e->id < 0) continue;
            int st = bfetch_state(e->id);
            if (st == BF_PENDING) { live++; continue; }
            if (st == BF_DONE && bfetch_status(e->id) / 100 == 2) {
                e->len = bfetch_take(e->id, &e->data);
                if (e->len < 0) { e->len = 0; e->data = 0; }
            } else {
                bfetch_release(e->id);
            }
            e->id = -1;
        }
        if (!live) return;
        if (g_tick) g_tick();
        sys_yield();
    }
}

/* Seed the cache with bytes the caller already has.
 *
 * This exists for TAB RE-HYDRATION. A background tab keeps the bytes every
 * sub-resource of its page arrived as (see tabs.h), and bringing it back to the
 * screen must not dial anything: layout.c asks for its images through
 * res_fetch(), which reads this cache, so the way to make a re-hydrate cost
 * zero handshakes is to put the tab's retained bytes back where res_fetch will
 * find them. It COPIES, because cache_take is destructive and the tab has to
 * still own its copy for the next switch.
 *
 * Returns 0 if the bytes are now in the cache (including "already were"). */
int bfetch_cache_put(const char *abs, const unsigned char *data, int len)
{
    if (!abs || !abs[0] || !data || len <= 0) return -1;
    if ((int)strlen(abs) >= BF_URLMAX) return -1;
    for (int i = 0; i < BF_NCACHE; i++)
        if (g_cache[i].used && strcmp(g_cache[i].url, abs) == 0) return 0;
    for (int i = 0; i < BF_NCACHE; i++) {
        if (g_cache[i].used) continue;
        unsigned char *copy = malloc((size_t)len + 1);
        if (!copy) return -1;
        memcpy(copy, data, (size_t)len);
        copy[len] = 0;
        memset(&g_cache[i], 0, sizeof g_cache[i]);
        memcpy(g_cache[i].url, abs, strlen(abs) + 1);
        g_cache[i].id = -1;              /* not in flight: it is already here */
        g_cache[i].data = copy;
        g_cache[i].len = len;
        g_cache[i].used = 1;
        return 0;
    }
    return -1;                            /* cache full: the caller re-fetches */
}

/* Take a prefetched body out of the cache, transferring ownership. */
static int cache_take(const char *abs, unsigned char **out, int *outlen)
{
    for (int i = 0; i < BF_NCACHE; i++) {
        struct bcache *e = &g_cache[i];
        if (!e->used || e->id >= 0 || strcmp(e->url, abs) != 0) continue;
        if (!e->data) { memset(e, 0, sizeof *e); return -1; }   /* it failed; don't retry */
        *out = e->data; *outlen = e->len;
        memset(e, 0, sizeof *e);
        return 0;
    }
    return -1;
}

/* Sub-resource (image) fetch, the name net/layout.c calls.
 *
 * This used to be SYS_RES_FETCH into a 768 KiB static buffer: one blocking
 * kernel fetch per image, `Connection: close`, so eight images from one host
 * were eight TLS handshakes. Now it is one pooled request -- and the size cap
 * is gone with it, because the body buffer grows as bytes arrive rather than
 * being reserved up front. */
int res_fetch(const char *src, unsigned char **buf, int *len)
{
    char abs[BF_URLMAX];
    if (bfetch_resolve(0, src, abs, sizeof abs) == 0 && cache_take(abs, buf, len) == 0)
        return 0;
    return bfetch_sync(src, buf, len);
}
