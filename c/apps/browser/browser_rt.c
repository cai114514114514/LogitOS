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
#include "sockerr.h"        /* sock_why(): the ONE rendering of a SOCK_E_* */
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "img.h"
#include "url.h"
#include "http1.h"
#include "http2.h"
#include "hpack.h"
#include "hpool.h"
#include "bfetch.h"
/* For CK_HEADER_MAX only -- the ONE size of a Cookie: value, shared with the
 * two js_webapi.c call sites that used to disagree with this one by 8x. The
 * header declares no symbol this file links, so the cookieless build below
 * still holds. */
#include "cookies.h"

/* The cookie jar's two transport-side doors, owned by js_webapi.c and WEAK
 * here: a build that links neither js_webapi.c nor cookies.c (the loader
 * host tests) resolves both to NULL and runs cookieless. Contract at their
 * definitions. */
int  webapi_cookie_line(const char *host, const char *path, int secure,
                        int nav, char *out, int cap) __attribute__((weak));
void webapi_cookie_store_line(const char *host, const char *path, int secure,
                              const char *setcookie) __attribute__((weak));

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
    /* 1 = top-level navigation, 0 = subresource. Read only by the cookie line
     * (build_get), where it is the difference between suppressing SameSite=Lax
     * and sending it. ZERO IS THE SAFE VALUE and every constructor memsets the
     * slot, so a request shape added later without thinking about SameSite is
     * treated as a subresource -- the restrictive answer. It survives hops on
     * purpose: a redirect chain during a navigation is still that navigation,
     * and re-classifying hop 2 as a subresource would log the user out exactly
     * on the sites that redirect to their own login. */
    int   nav;
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

    /* ---- byte range (see bfetch.h) ----
     * rq_first < 0 means "no range asked", which is the memset-0 case only
     * because every constructor sets it explicitly -- do not rely on 0. */
    long long rq_first, rq_last;      /* what we asked for; rq_last < 0 = open */
    int       rres;                   /* BF_R_* */
    long long got_first, got_last, got_total;   /* what arrived; -1 unknown */
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

/* ---- byte ranges: the request side ---- */

/* Append a non-negative decimal. No libc: browser_rt.c has no printf in the
 * request path on purpose (build_get is a security boundary, not a formatter --
 * see h1_request_build). Returns the new offset. */
static int put_ll(char *b, int o, int cap, long long v)
{
    char t[24];
    int i = 0;
    if (v < 0) v = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i && o < cap - 1) b[o++] = t[--i];
    return o;
}

/* `bytes=first-last`, or `bytes=first-` when last < 0. Exactly one range, by
 * construction: there is no comma here and no way for a caller to introduce
 * one, which is the request half of the multipart/byteranges refusal. Returns
 * 0, or -1 if the ask is not a range at all. */
static int fmt_range(const struct breq *r, char *out, int cap)
{
    if (r->rq_first < 0) return -1;
    int o = 0;
    const char *p = "bytes=";
    while (*p && o < cap - 1) out[o++] = *p++;
    o = put_ll(out, o, cap, r->rq_first);
    if (o < cap - 1) out[o++] = '-';
    if (r->rq_last >= 0) o = put_ll(out, o, cap, r->rq_last);
    out[o] = 0;
    return 0;
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
    /* The Range, only when asked for -- an unconditional Range header changes
     * what caches and CDNs do to every request on the machine.
     *
     * ACCEPT-ENCODING IS FORCED TO IDENTITY WITH IT, and that is not caution,
     * it is the spec: a byte range names bytes of the SELECTED REPRESENTATION,
     * i.e. of the gzip stream, not of the file. Bytes 1000-1999 of a gzip
     * member are not independently inflatable, our inflater is one-shot (see
     * h1_decode_body), and the failure mode without this line is a 206 whose
     * body cannot be decoded at all -- or worse, one that decodes to something.
     * A range response that arrives content-encoded anyway is refused below by
     * name rather than guessed at. */
    if (r->rq_first >= 0) {
        char rv[64];
        if (fmt_range(r, rv, (int)sizeof rv) == 0) {
            h1_request_set_header(&q, "Range", rv);
            h1_request_set_header(&q, "Accept-Encoding", "identity");
        }
    }
    /* The entire point: no `Connection: close`. */
    h1_request_set_header(&q, "Connection", "keep-alive");
    /* Cookies, on EVERY transport request -- navigation, reload, and each
     * subresource -- not only the JS fetch()/XHR path. The jar lives in
     * js_webapi.c and is reached through a weak symbol so builds without
     * that TU (the loader host tests) link and run cookieless. See the
     * export comment in js_webapi.c for the WAF loop this closes. */
    if (webapi_cookie_line) {
        /* static, not automatic: CK_HEADER_MAX is 8 KiB and this file's own
         * history is a redirect chain that overflowed a stack into the page
         * tables. build_get() runs to completion synchronously and nothing
         * here is reentrant, so one buffer is one buffer. */
        static char ck[CK_HEADER_MAX];
        if (webapi_cookie_line(r->u.host, r->u.path, r->u.https, r->nav,
                               ck, (int)sizeof ck) > 0 && ck[0])
            h1_request_set_header(&q, "Cookie", ck);
    }
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
    if (fd < 0) {
        /* Which failure, not just that one happened: the ABI's SOCK_E_* codes
         * separate "bad argument" from "no DNS" from "table full", and folding
         * them into one string cost a real diagnosis once (a harness-wide
         * "sock_open failed" with the cause unreadable).
         *
         * The cause goes to `r->err` as well as to the serial log, which is the
         * half that was missing: bfetch_error() is what a PERSON is shown, and
         * it used to say "sock_open failed" no matter which of the four this
         * was. The decode is sock_why() rather than the four-way ternary that
         * used to be inlined here -- see include/abi/sockerr.h for why that
         * copy was the second of three. */
        printf("[bfetch] sock_open('%s', %d) = %d (%s)\n",
               r->u.host, r->u.port, fd, sock_why(fd));
        req_fail(r, sock_why(fd));
        return;
    }
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

/* ---- byte ranges: the response side ----
 *
 * This is where a range fetch is either honest or silently wrong, so every
 * branch below ends in either "the slice is exactly this" or a named refusal.
 * There is deliberately no branch that shrugs. */

static int ci_ch(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int ci_has(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && ci_ch((unsigned char)*a) == ci_ch((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* Bounded decimal. Returns the count of digits consumed (0 = none), and -1 on
 * overflow -- a Content-Range is attacker-controlled, and a wrapped offset is
 * how "write this slice at N" becomes a write somewhere else. */
static int rd_ll(const char *s, long long *out)
{
    long long v = 0;
    int n = 0;
    while (s[n] >= '0' && s[n] <= '9') {
        if (v > (0x7FFFFFFFFFFFFFFFLL - (s[n] - '0')) / 10) return -1;
        v = v * 10 + (s[n] - '0');
        n++;
        if (n > 19) return -1;
    }
    if (n) *out = v;
    return n;
}

/* RFC 9110 14.4:
 *     Content-Range = range-unit SP ( incl-range "/" (len / "*")
 *                                   | "*" "/" len )
 * Fills f/l with the inclusive bounds (-1/-1 for the unsatisfied "star slash
 * len" form) and *tot with the complete length (-1 for "*"). Returns 0, or -1 on
 * anything this does not understand -- including a comma, which is the only
 * way a multi-range answer could reach a single Content-Range line. */
static int parse_content_range(const char *v, long long *f, long long *l, long long *tot)
{
    *f = *l = *tot = -1;
    if (!v) return -1;
    while (*v == ' ' || *v == '\t') v++;
    /* unit; only "bytes" -- "none" and any extension unit are not slices. */
    const char *u = "bytes";
    int i = 0;
    while (u[i] && ci_ch((unsigned char)v[i]) == u[i]) i++;
    if (u[i] || (v[i] != ' ' && v[i] != '\t')) return -1;
    v += i;
    while (*v == ' ' || *v == '\t') v++;

    if (*v == '*') {
        v++;
        if (*v++ != '/') return -1;
        int n = rd_ll(v, tot);
        if (n <= 0 || v[n]) return -1;
        return 0;
    }
    int n = rd_ll(v, f);
    if (n <= 0) return -1;
    v += n;
    if (*v++ != '-') return -1;
    n = rd_ll(v, l);
    if (n <= 0) return -1;
    v += n;
    if (*v++ != '/') return -1;
    if (*v == '*') { *tot = -1; v++; }
    else { n = rd_ll(v, tot); if (n <= 0) return -1; v += n; }
    if (*v) return -1;                 /* trailing junk, or a second range */
    if (*f > *l) return -1;
    if (*tot >= 0 && *l >= *tot) return -1;
    return 0;
}

#ifdef BFETCH_NO_RANGE_CHECK
/* THE NEGATIVE CONTROL (tests/range.mk test-range-negctl). This is "add Range
 * support" done the quick way and it is what the whole response side above
 * exists not to be: send the header, parse a Content-Range if one turns up,
 * and otherwise believe that whatever came back is the slice that was asked
 * for. Every request-side check still passes under it -- the Range header is
 * byte-identical -- and the four response-side claims must fail. */
static const char *range_check(struct breq *r, struct h1_response *resp)
{
    r->rres = BF_R_NONE;
    r->got_first = r->got_last = r->got_total = -1;
    if (r->rq_first < 0) return 0;
    const char *cr = h1_headers_get(&resp->hdr, "content-range");
    if (cr) parse_content_range(cr, &r->got_first, &r->got_last, &r->got_total);
    r->rres = BF_R_PARTIAL;
    return 0;
}
#else

/* Decide what the response means for the range that was asked. Returns NULL to
 * accept, or the reason to fail with. Runs BEFORE h1_decode_body, because one
 * of the answers is "this must not be decoded". */
static const char *range_check(struct breq *r, struct h1_response *resp)
{
    r->rres = BF_R_NONE;
    r->got_first = r->got_last = r->got_total = -1;

    const char *cr = h1_headers_get(&resp->hdr, "content-range");

    if (r->rq_first < 0) {
        /* NOBODY ASKED. A 206 here is not a nicety to tolerate: bfetch_sync()
         * accepts any 2xx, so res_fetch() would file a partial body in the
         * whole-resource cache and an image decoder would call the file
         * corrupt. Buggy CDNs and intermediaries do send these. Refuse. */
        if (resp->code == 206)
            return "206 to a request that carried no Range";
        return 0;
    }

    if (resp->code == 416) {
        r->rres = BF_R_UNSATISFIABLE;
        if (cr) {
            long long f, l, t;
            if (parse_content_range(cr, &f, &l, &t) == 0) r->got_total = t;
        }
        return "416 range not satisfiable";
    }

    if (resp->code == 206) {
        /* multipart/byteranges: a MIME body with generated boundaries and a
         * Content-Range per part. We never ask for more than one range, so
         * this is a server doing something we did not request; the body is not
         * the slice and its first bytes are a boundary line. Named, not
         * guessed. */
        const char *ct = h1_headers_get(&resp->hdr, "content-type");
        if (ci_has(ct, "multipart/byteranges") || ci_has(ct, "multipart/x-byteranges"))
            return "206 multipart/byteranges is not supported";
        if (!cr)
            return "206 without Content-Range";
        if (h1_headers_count(&resp->hdr, "content-range") > 1)
            return "206 with more than one Content-Range";
        long long f, l, t;
        if (parse_content_range(cr, &f, &l, &t) != 0)
            return "206 Content-Range is malformed or not in bytes";
        if (f < 0)
            return "206 answered with an unsatisfied-range Content-Range";
        /* A server may CLAMP: asking 0-99999 of a 500-byte file legitimately
         * yields 0-499. It may not MOVE the start -- a caller places these
         * bytes at `first`, so a different origin is a silent misplacement. */
        if (f != r->rq_first)
            return "206 range does not start where we asked";
        if (r->rq_last >= 0 && l > r->rq_last)
            return "206 range extends past what we asked for";
        /* Content-Encoding on a range response: see the Accept-Encoding note in
         * build_get. We asked for identity; a server that encoded anyway has
         * given us bytes of a stream we cannot inflate a slice of. */
        const char *ce = h1_headers_get(&resp->hdr, "content-encoding");
        if (ce && ce[0] && !ci_has(ce, "identity"))
            return "206 body is content-encoded and a slice cannot be decoded";
        /* The framing cross-check, and the reason it is worth doing: http1.c
         * frames this body from Content-Length, which for a 206 is the length
         * of the SLICE. If the two headers disagree, one of them is lying about
         * where these bytes belong and there is no way to tell which. */
        if ((long long)resp->body_len != l - f + 1)
            return "206 body length disagrees with Content-Range";
        r->rres = BF_R_PARTIAL;
        r->got_first = f; r->got_last = l; r->got_total = t;
        return 0;
    }

    if (resp->code / 100 == 2) {
        /* THE SERVER IGNORED THE RANGE. Not an error -- the body is the whole
         * resource and is perfectly good -- but it is emphatically not the
         * slice, and a caller that assumes otherwise corrupts its buffer with
         * no symptom until playback. Reported three ways: the status stays
         * 200 (never rewritten to 206), the result is BF_R_IGNORED, and it is
         * printed, because the callers most likely to get this wrong are the
         * ones that never look. */
        r->rres = BF_R_IGNORED;
        r->got_first = 0;
        r->got_last = resp->body_len > 0 ? (long long)resp->body_len - 1 : -1;
        r->got_total = (long long)resp->body_len;
        char asked[64];
        if (fmt_range(r, asked, (int)sizeof asked) != 0) asked[0] = 0;
        printf("[bfetch] Range ignored: asked %s, server answered %d with the "
               "whole %d-byte body: %s\n", asked, resp->code, resp->body_len, r->url);
        return 0;
    }

    /* 404, 500, ... -- an ordinary error the caller reads from bfetch_status().
     * rres stays BF_R_NONE; see the enum comment. */
    return 0;
}
#endif /* BFETCH_NO_RANGE_CHECK */

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
        /* http1.c's error space stops at "the transport failed" -- it cannot
         * see WHY, because the transport is a socket handle to it. The socket
         * can, so ask before falling back to the protocol-level name. Guarded
         * on a NONZERO code: a SOCK_P_ERROR with an empty code byte is a
         * reporter that never encoded one (the host stubs in
         * tests/unit/h2stub do exactly that), and answering "unknown socket
         * error" there would be strictly less than h1_strerror already says. */
        int bits = r->fd >= 0 ? sock_poll(r->fd) : 0;
        if (bits > 0 && (bits & SOCK_P_ERROR) && SOCK_ERR_CODE(bits) < 0)
            req_fail(r, sock_why(SOCK_ERR_CODE(bits)));
        else
            req_fail(r, h1_strerror(r->c.err ? r->c.err : r->c.resp.err));
        return;
    }

    /* H1_C_DONE */
    struct h1_response *resp = &r->c.resp;
    r->status = resp->code;
    int keep = resp->keep_alive && !resp->must_close && r->c.spill_len == 0;

    /* Ingest Set-Cookie BEFORE the redirect branch: the challenge-and-reload
     * flow (and every login flow ever) sets its cookie on the 3xx/refresh
     * response itself, and following the hop without storing it is exactly
     * the loop this line exists to break. Multi-valued per RFC 6265, hence
     * nth, not get. */
    if (webapi_cookie_store_line) {
        int nck = h1_headers_count(&resp->hdr, "set-cookie");
        for (int ci = 0; ci < nck; ci++) {
            const char *sc = h1_headers_nth(&resp->hdr, "set-cookie", ci);
            if (sc) webapi_cookie_store_line(r->u.host, r->u.path, r->u.https, sc);
        }
    }

    if (h1_is_redirect(resp->code)) {
        const char *loc = h1_headers_get(&resp->hdr, "location");
        if (loc && r->hops < BF_HOPS) {
            char next[BF_URLMAX];
            if (bfetch_resolve(r->url, loc, next, sizeof next) == 0) {
                req_drop_conn(r, keep);
                r->hops++;
                r->retried = 0;
                r->t0 = monotonic_ms();       /* each hop gets its own budget */
                /* rq_first/rq_last are NOT reset: a redirected range request
                 * must ask the new location for the same slice, or the hop
                 * silently downgrades to a whole-resource fetch. The VERDICT
                 * is per hop and is cleared. */
                r->rres = BF_R_NONE;
                r->got_first = r->got_last = r->got_total = -1;
                int i = 0; while (next[i] && i < BF_URLMAX - 1) { r->url[i] = next[i]; i++; }
                r->url[i] = 0;
                if (url_parse(r->url, &r->u) != 0) { req_fail(r, "bad redirect target"); return; }
                r->state = RQ_QUEUED;
                return;
            }
        }
    }

    /* The range verdict, BEFORE h1_decode_body: one of its answers is "these
     * bytes must not be decoded", and the Content-Length/Content-Range
     * cross-check below is only meaningful against the body as it arrived. */
    {
        const char *why = range_check(r, resp);
        if (why) {
            printf("[bfetch] %s: %s\n", why, r->url);
            req_drop_conn(r, 0);
            r->err = why;
            r->state = RQ_FAIL;
            return;
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
                /* THIS IS WHERE A CERTIFICATE FAILURE LANDS, and for a while it
                 * was where one died. SOCK_P_CONNECTED means the transport is
                 * up INCLUDING the TLS handshake (logit_abi.h above SOCK_P_*),
                 * so everything the chain verifier can refuse -- an untrusted
                 * root, an expired leaf, a name that does not match -- reaches
                 * ring 3 as exactly this branch with SOCK_E_TLS in the code
                 * byte. c/net/core/sock.c:474 sets it, sock_poll_bits (:547)
                 * shifts it in, SOCK_ERR_CODE takes it back out.
                 *
                 * "socket error" / "connect failed" threw all four codes away
                 * one line from the end of a diagnosis that had survived the
                 * whole stack, so a bad certificate, an unresolvable name and a
                 * dead network were one sentence. */
                req_fail(r, sock_why(bits < 0 ? bits : SOCK_ERR_CODE(bits)));
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

static int bfetch_start_range_impl(const char *base, const char *ref,
                                   long long first, long long last, int nav)
{
    if (!g_pool_ready) bfetch_init();
    char abs[BF_URLMAX];
    if (bfetch_resolve(base, ref, abs, sizeof abs) != 0) return -1;
    /* An impossible ask is refused HERE, out loud, rather than sent and
     * answered 416 a round trip later -- and rather than sent as a header the
     * server is entitled to interpret however it likes. */
    if (first >= 0 && last >= 0 && last < first) {
        printf("[bfetch] refusing range bytes=%lld-%lld: last is before first: %s\n",
               first, last, abs);
        return -1;
    }
    for (int i = 0; i < BF_NREQ; i++) {
        struct breq *r = &g_req[i];
        if (r->state != RQ_FREE) continue;
        memset(r, 0, sizeof *r);
        r->fd = -1; r->pslot = -1;
        r->rq_first = first < 0 ? -1 : first;
        r->rq_last  = last  < 0 ? -1 : last;
        r->rres = BF_R_NONE;
        r->nav = nav ? 1 : 0;
        r->got_first = r->got_last = r->got_total = -1;
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

int bfetch_start_from(const char *base, const char *ref)
{ return bfetch_start_range_impl(base, ref, -1, -1, 0); }

int bfetch_start(const char *ref) { return bfetch_start_from(0, ref); }

/* The navigation door. It is a SEPARATE entry rather than a flag on
 * bfetch_start() so that the permissive classification has to be asked for by
 * name: every existing caller stays a subresource without being edited, and a
 * new one is a subresource until somebody decides otherwise. The reverse
 * default -- one parameter, forgotten once -- is the bug this whole change
 * exists to close, in the same shape one layer down (cookies.h's enum). */
int bfetch_start_nav(const char *ref)
{ return bfetch_start_range_impl(0, ref, -1, -1, 1); }

int bfetch_start_range_from(const char *base, const char *ref,
                            long long first, long long last)
{
    /* first < 0 would be a suffix range (`bytes=-N`), which this API does not
     * express -- see bfetch.h. Refused rather than quietly demoted to a
     * whole-resource GET, which is what passing it through would be. */
    if (first < 0) {
        printf("[bfetch] refusing range: first offset %lld is negative "
               "(suffix ranges are not supported): %s\n", first, ref ? ref : "");
        return -1;
    }
    /* A ranged GET is a media byte supply, never a navigation. */
    return bfetch_start_range_impl(base, ref, first, last, 0);
}

int bfetch_start_range(const char *ref, long long first, long long last)
{ return bfetch_start_range_from(0, ref, first, last); }

static struct breq *req_of(int id);

int bfetch_range_result(int id, long long *first, long long *last, long long *total)
{
    struct breq *r = req_of(id);
    if (!r) { if (first) *first = -1; if (last) *last = -1; if (total) *total = -1; return BF_R_NONE; }
    if (first) *first = r->got_first;
    if (last)  *last  = r->got_last;
    if (total) *total = r->got_total;
    return r->rres;
}

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

int bfetch_sync_range(const char *ref, long long first, long long last,
                      unsigned char **out, int *outlen, long long *total)
{
    if (total) *total = -1;
    int id = bfetch_start_range(ref, first, last);
    if (id < 0) return -1;
    bfetch_wait(id, g_tick);
    if (bfetch_state(id) != BF_DONE) {
        printf("[bfetch] ranged fetch failed: %s: %s\n", bfetch_error(id), ref);
        bfetch_release(id);
        return -1;
    }
    long long gf, gl, gt;
    int rres = bfetch_range_result(id, &gf, &gl, &gt);
    if (rres != BF_R_PARTIAL) {
        /* The 200 case ends up here, and ending up here is the point: this
         * entry point hands back a slice, and a whole body is not one. The
         * caller that can use it has bfetch_start_range(). */
        printf("[bfetch] ranged fetch refused: status %d, range result %d "
               "(1=partial 2=server ignored Range 3=416): %s\n",
               bfetch_status(id), rres, ref);
        bfetch_release(id);
        return -1;
    }
    if (total) *total = gt;
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
            /* THE CACHE HOLDS WHOLE RESOURCES ONLY (see bfetch.h). A prefetch
             * is started by bfetch_start(), which asks for no range, so this
             * cannot be a partial body -- and the guard is here rather than in
             * a comment because the cache is keyed by URL alone, so if that
             * ever stops being true the wrong bytes get served to the next
             * caller with no symptom at all. Cheap, local, and it prints. */
            if (st == BF_DONE && bfetch_range_result(e->id, 0, 0, 0) != BF_R_NONE) {
                printf("[bfetch] refusing to cache a partial body under a plain "
                       "URL: %s\n", e->url);
                bfetch_release(e->id);
                e->id = -1;
                continue;
            }
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

/* ===================== bxfer: HTTP/2 beneath the fetch ====================
 *
 * See bfetch.h for the contract and for why the protocol choice lives here
 * rather than in the fetch state machine. This half is the implementation:
 * a table of connections keyed by socket handle, an HPACK re-encoding of the
 * serialized HTTP/1.1 request, and the materialisation of an HTTP/2 stream
 * into the `struct h1_response` the caller already knows how to read.
 *
 * THE ONE STRUCTURAL DIFFERENCE from HTTP/1.1, and the reason this is not
 * just a parser swap: on HTTP/1.1 a connection belongs to one exchange, so
 * "open a socket" and "start a request" are the same event. On HTTP/2 a
 * connection belongs to the ORIGIN and carries every request to it at once.
 * So the socket outlives the exchange, is refcounted rather than owned, and
 * bxfer_open hands the same handle to every concurrent caller. That is the
 * whole of multiplexing, and it is why the fd had to come under this file's
 * control -- a caller that dials its own socket has already lost.
 *
 * -DBXFER_H1_ONLY is the negative control: h2 is never offered in ALPN and
 * bx_start_h2 is never reached, so the multiplexing measurement must collapse
 * to one connection per request while every HTTP/1.1 path is untouched. */

#define BX_MAXSESS 8            /* distinct connections bxfer may hold */
#define BX_MAXBIND 12           /* concurrent exchanges (fetch has WF_MAX = 8) */
#define BX_H2_BODY_MAX (32 * 1024 * 1024)

struct bx_sess {
    int   used;
    char  host[URL_HOST_MAX];
    int   port, tls;
    int   fd;
    int   proto;                /* HP_PROTO_H1 / H2 / PENDING */
    int   refs;                 /* outstanding bxfer_open handles */
    int   h1_taken;             /* an HTTP/1.1 exchange owns this socket */
    int   h2_live;              /* h2_conn_start has run */
    int   pslot;                /* hpool slot, or -1 */
    struct h2_conn c;
};

/* One exchange. `owner` is the caller's h1_conn, which is the only handle the
 * fetch state machine has -- so the binding is a side table keyed by that
 * pointer rather than a field, which keeps `struct h1_conn` http1.c's. */
struct bx_bind {
    struct h1_conn *owner;      /* NULL = free slot */
    int      sess;              /* index into g_sess, -1 = plain HTTP/1.1 */
    uint32_t sid;               /* our stream on that connection */
    int      hdr_done;
};

static struct bx_sess g_sess[BX_MAXSESS];
static struct bx_bind g_bind[BX_MAXBIND];
static int g_bx_conns, g_bx_h2conns, g_bx_streams, g_bx_peak;

void bxfer_stats(int *conns, int *h2conns, int *streams, int *peak)
{
    if (conns)   *conns   = g_bx_conns;
    if (h2conns) *h2conns = g_bx_h2conns;
    if (streams) *streams = g_bx_streams;
    if (peak)    *peak    = g_bx_peak;
}
void bxfer_reset_stats(void) { g_bx_conns = g_bx_h2conns = g_bx_streams = g_bx_peak = 0; }

static struct bx_sess *bx_sess_of(int fd)
{
    if (fd < 0) return 0;
    for (int i = 0; i < BX_MAXSESS; i++)
        if (g_sess[i].used && g_sess[i].fd == fd) return &g_sess[i];
    return 0;
}

static struct bx_bind *bx_bind_of(struct h1_conn *c)
{
    for (int i = 0; i < BX_MAXBIND; i++) if (g_bind[i].owner == c) return &g_bind[i];
    return 0;
}

/* ---- the h2 transport: the same three return values http1.c uses ---- */

static int bx_read(void *ctx, void *buf, int len)
{
    int fd = (int)(long)ctx;
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return H2_TERR;
    int n = sock_recv(fd, buf, len);
    if (n > 0) return n;
    if (n < 0) return H2_EOF;
    if (bits & SOCK_P_EOF) return H2_EOF;
    return H2_AGAIN;
}

static int bx_write(void *ctx, const void *buf, int len)
{
    int fd = (int)(long)ctx;
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return H2_TERR;
    if (!(bits & SOCK_P_CONNECTED)) return H2_AGAIN;   /* still handshaking */
    return sock_send(fd, buf, len);                     /* 0 = queue full = AGAIN */
}

/* ---- connections ---- */

static void bx_sess_drop(struct bx_sess *ss)
{
    if (!ss || !ss->used) return;
    if (ss->h2_live) {
        /* The line the device harness greps for. A multiplexed connection that
         * silently serialised would print streams=N peak=1, which is the whole
         * difference and is invisible from anywhere above this. */
        printf("[bxfer] h2 %s:%d closed: streams=%d peak=%d frames=%d/%d hpack_in=%lld\n",
               ss->host, ss->port, ss->c.streams_opened, ss->c.max_concurrent_seen,
               ss->c.frames_in, ss->c.frames_out, (long long)ss->c.bytes_in);
        h2_conn_free(&ss->c);
        ss->h2_live = 0;
    }
    if (ss->pslot >= 0) { hpool_drop(&g_pool, ss->pslot); ss->pslot = -1; ss->fd = -1; }
    if (ss->fd >= 0) sock_close(ss->fd);
    memset(ss, 0, sizeof *ss);
}

void bxfer_close_all(void)
{
    for (int i = 0; i < BX_MAXBIND; i++) memset(&g_bind[i], 0, sizeof g_bind[i]);
    for (int i = 0; i < BX_MAXSESS; i++) bx_sess_drop(&g_sess[i]);
}

int bxfer_open(const char *host, int port, int tls)
{
    if (!g_pool_ready) bfetch_init();
    if (!host || !host[0]) return -1;

#ifndef BXFER_H1_ONLY
    /* Join an existing connection to this origin if it is speaking HTTP/2, or
     * if its handshake has not reported yet and still might. The second case
     * is the one that matters: N fetches start in the same frame, long before
     * any ALPN answer exists, and a client that dials on each of them has six
     * connections to an origin that wanted one. The cost is a fallback if the
     * guess is wrong, which bxfer_start implements. */
    if (tls) {
        for (int i = 0; i < BX_MAXSESS; i++) {
            struct bx_sess *ss = &g_sess[i];
            if (!ss->used || ss->refs <= 0 || ss->port != port || ss->tls != tls) continue;
            if (strcmp(ss->host, host) != 0) continue;
            if (ss->proto == HP_PROTO_H1) continue;      /* one exchange at a time */
            if (ss->proto == HP_PROTO_H2 && !h2_conn_usable(&ss->c)) continue;
            int bits = sock_poll(ss->fd);
            if (bits < 0 || (bits & (SOCK_P_EOF | SOCK_P_ERROR))) continue;
            if (ss->pslot >= 0) hpool_acquire_mux(&g_pool, host, port, tls,
                                                  (int64_t)monotonic_ms());
            ss->refs++;
            return ss->fd;
        }
    }
#endif

    struct bx_sess *ss = 0;
    for (int i = 0; i < BX_MAXSESS; i++) if (!g_sess[i].used) { ss = &g_sess[i]; break; }
    if (!ss) return -1;

    int flags = 0;
    if (tls) {
        flags = SOCK_F_TLS | SOCK_F_ALPN_HTTP11;
#ifndef BXFER_H1_ONLY
        flags |= SOCK_F_ALPN_H2;        /* h2 is chosen HERE, in the handshake */
#endif
    }
    int fd = sock_open(host, port, flags);
    if (fd < 0) return -1;

    memset(ss, 0, sizeof *ss);
    ss->used = 1;
    ss->fd = fd;
    ss->port = port;
    ss->tls = tls;
    ss->refs = 1;
    /* Not yet known, and it cannot be until the handshake finishes. A plain
     * http:// socket has no ALPN at all, so it is HTTP/1.1 by construction. */
    ss->proto = tls ? HP_PROTO_PENDING : HP_PROTO_H1;
    int n = 0; while (host[n] && n < URL_HOST_MAX - 1) { ss->host[n] = host[n]; n++; }
    ss->host[n] = 0;
    /* Register with the shared pool so h1 sub-resource loading and h2 fetches
     * draw on ONE connection budget, and so the pool's own h2 counters are
     * real. A pool that will not take it is not a reason to fail the request. */
    ss->pslot = hpool_admit_proto(&g_pool, host, port, tls, fd, 0, ss->proto,
                                  (int64_t)monotonic_ms());
    g_bx_conns++;
    return fd;
}

void bxfer_close(int fd)
{
    struct bx_sess *ss = bx_sess_of(fd);
    if (!ss) { if (fd >= 0) sock_close(fd); return; }
    if (ss->refs > 0) ss->refs--;
    if (ss->refs > 0) {
        /* Other streams are still running on this connection: the socket is
         * emphatically not ours to close. This is the line that separates a
         * multiplexed connection from six serialised ones. */
        if (ss->pslot >= 0) hpool_release(&g_pool, ss->pslot, 1, (int64_t)monotonic_ms());
        return;
    }
    bx_sess_drop(ss);
}

/* ---- what ALPN said ---- */

static void bx_resolve_alpn(struct bx_sess *ss)
{
    if (ss->proto != HP_PROTO_PENDING) return;
    char a[24];
    int n = sock_alpn(ss->fd, a, (int)sizeof a - 1);
    if (n < 0) n = 0;
    if (n >= (int)sizeof a) n = (int)sizeof a - 1;
    a[n] = 0;
    ss->proto = (n == 2 && a[0] == 'h' && a[1] == '2') ? HP_PROTO_H2 : HP_PROTO_H1;
    if (ss->pslot >= 0) hpool_set_proto(&g_pool, ss->pslot, ss->proto);
    if (ss->proto == HP_PROTO_H2) g_bx_h2conns++;
}

/* ---- re-encoding the serialized request as HPACK ----
 *
 * fetch_send_request builds a complete HTTP/1.1 request -- method, target,
 * cookies, CORS headers, body -- and that assembly is where the Web-API
 * semantics live. Re-deriving any of it here would be a second implementation
 * of the same rules that could disagree with the first, so instead the bytes
 * are taken apart again: it is our own output, its shape is known exactly, and
 * the result is that h2 and h1 send provably the same request.
 *
 * The body is NOT copied. It is used in place out of `req`, which the
 * h1_conn owns until bxfer_free -- which is also exactly as long as http2.c
 * requires a borrowed request body to stay valid. */
static int bx_start_h2(struct bx_sess *ss, struct bx_bind *b, struct h1_conn *c,
                       char *req, int reqlen, const char *host, int port, int tls)
{
    (void)port;
    const char *p = req, *end = req + reqlen;

    /* request-line: METHOD SP target SP HTTP/1.1 CRLF */
    const char *sp1 = 0, *sp2 = 0, *eol = 0;
    for (const char *q = p; q + 1 < end; q++)
        if (q[0] == '\r' && q[1] == '\n') { eol = q; break; }
    if (!eol) return H1_E_SYNTAX;
    for (const char *q = p; q < eol; q++) if (*q == ' ') { sp1 = q; break; }
    if (!sp1) return H1_E_SYNTAX;
    for (const char *q = sp1 + 1; q < eol; q++) if (*q == ' ') { sp2 = q; break; }
    if (!sp2) return H1_E_SYNTAX;

    char method[H1_METHOD_MAX];
    int ml = (int)(sp1 - p);
    if (ml <= 0 || ml >= (int)sizeof method) return H1_E_SYNTAX;
    memcpy(method, p, (size_t)ml); method[ml] = 0;

    const char *path = sp1 + 1;
    int pl = (int)(sp2 - path);
    if (pl <= 0) return H1_E_SYNTAX;

    struct hpack_list extra;
    hpack_list_init(&extra);
    char authority[URL_HOST_MAX + 8];
    authority[0] = 0;

    int rc = H1_OK;
    const char *ln = eol + 2;
    while (ln + 1 < end) {
        if (ln[0] == '\r' && ln[1] == '\n') { ln += 2; break; }     /* end of headers */
        const char *le = 0;
        for (const char *q = ln; q + 1 < end; q++)
            if (q[0] == '\r' && q[1] == '\n') { le = q; break; }
        if (!le) { rc = H1_E_SYNTAX; break; }
        const char *colon = 0;
        for (const char *q = ln; q < le; q++) if (*q == ':') { colon = q; break; }
        if (!colon) { rc = H1_E_SYNTAX; break; }
        int nl = (int)(colon - ln);
        const char *v = colon + 1;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        int vl = (int)(le - v);

        char nm[96];
        if (nl > 0 && nl < (int)sizeof nm) {
            for (int k = 0; k < nl; k++) {
                char ch = ln[k];
                nm[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
            }
            nm[nl] = 0;
            /* :authority replaces Host; the rest of these have no meaning in
             * HTTP/2 and a server must reject a request carrying them. */
            if (!strcmp(nm, "host")) {
                int k = 0; while (k < vl && k < (int)sizeof authority - 1) { authority[k] = v[k]; k++; }
                authority[k] = 0;
            } else if (strcmp(nm, "connection") && strcmp(nm, "keep-alive") &&
                       strcmp(nm, "proxy-connection") && strcmp(nm, "transfer-encoding") &&
                       strcmp(nm, "upgrade")) {
                /* content-length is deliberately KEPT: it is legal in HTTP/2
                 * and it is the byte count the caller computed for a binary
                 * body, which is exactly the thing under test. */
                if (hpack_list_add(&extra, nm, nl, v, vl, 0) != HPACK_OK) { rc = H1_E_NOMEM; break; }
            }
        }
        ln = le + 2;
    }
    if (rc != H1_OK) { hpack_list_free(&extra); return rc; }

    if (!authority[0]) {
        int k = 0; while (host[k] && k < (int)sizeof authority - 1) { authority[k] = host[k]; k++; }
        authority[k] = 0;
    }

    const uint8_t *body = (const uint8_t *)ln;
    int blen = (int)(end - ln);
    if (blen < 0) blen = 0;

    if (!ss->h2_live) {
        struct h2_transport t;
        t.read = bx_read; t.write = bx_write; t.poll = 0; t.ctx = (void *)(long)ss->fd;
        if (h2_conn_start(&ss->c, &t) != H2_OK) { hpack_list_free(&extra); return H1_E_NOMEM; }
        ss->h2_live = 1;
    }

    /* :path must be NUL-terminated and is not, in `req`. The request line has
     * already been parsed, so the space before "HTTP/1.1" is free to take. */
    ((char *)path)[pl] = 0;

    int id = h2_request(&ss->c, method, tls ? "https" : "http", authority, path,
                        &extra, blen ? body : 0, blen);
    hpack_list_free(&extra);
    if (id < 0) return (id == H2_E_NOSLOT || id == H2_E_GOAWAY) ? H1_E_TRANSPORT : H1_E_NOMEM;

    b->sess = (int)(ss - g_sess);
    b->sid = (uint32_t)id;
    b->hdr_done = 0;
    h2_stream_limit(&ss->c, b->sid, BX_H2_BODY_MAX);

    c->out = req; c->out_len = reqlen; c->out_off = 0;   /* freed by bxfer_free */
    h1_response_init(&c->resp);
    c->state = H1_C_SEND;
    c->err = 0;

    g_bx_streams++;
    int live = h2_conn_active_streams(&ss->c);
    if (live > g_bx_peak) g_bx_peak = live;
    return H1_OK;
}

int bxfer_start(struct h1_conn *c, const struct h1_transport *t,
                char *req, int reqlen, int *fd,
                const char *host, int port, int tls)
{
    if (!c || !t || !req || reqlen <= 0 || !fd) return H1_E_ARG;

    struct bx_bind *b = bx_bind_of(c);
    if (!b) for (int i = 0; i < BX_MAXBIND; i++) if (!g_bind[i].owner) { b = &g_bind[i]; break; }
    if (!b) return H1_E_NOMEM;
    memset(b, 0, sizeof *b);
    b->owner = c;
    b->sess = -1;

    struct bx_sess *ss = bx_sess_of(*fd);
    if (ss) {
        bx_resolve_alpn(ss);
#ifndef BXFER_H1_ONLY
        if (ss->proto == HP_PROTO_H2)
            return bx_start_h2(ss, b, c, req, reqlen, host, port, tls);
#endif
        /* HTTP/1.1 after all. A connection carries one exchange at a time, so
         * a request that joined this socket speculatively has to be given its
         * own -- the alternative is two requests interleaved on one stream,
         * which is not a slow page, it is a corrupt one. */
        if (ss->h1_taken) {
            int nfd = bxfer_open(host, port, tls);
            if (nfd < 0) { b->owner = 0; return H1_E_TRANSPORT; }
            bxfer_close(*fd);
            *fd = nfd;
            ss = bx_sess_of(nfd);
            if (ss) ss->proto = HP_PROTO_H1;   /* it is a re-dial for h1; do not share it */
        }
        if (ss) ss->h1_taken = 1;
    }

    /* The transport the caller gave us names the fd it was built with, which
     * the re-dial above may have replaced. */
    struct h1_transport ht = *t;
    ht.ctx = (void *)(long)*fd;
    int rc = h1_conn_start(c, &ht, req, reqlen);
    if (rc != H1_OK) b->owner = 0;
    return rc;
}

/* ---- an HTTP/2 stream, seen as an h1_response ---- */

static int bx_err_map(int h2err)
{
    switch (h2err) {
    case H2_E_PROTO: case H2_E_COMPRESS: case H2_E_FRAMESIZE: return H1_E_SYNTAX;
    case H2_E_TOOLARGE:                                       return H1_E_TOOLARGE;
    case H2_E_NOMEM:                                          return H1_E_NOMEM;
    case H2_E_CLOSED: case H2_E_RESET: case H2_E_REFUSED:     return H1_E_TRUNC;
    default:                                                  return H1_E_TRANSPORT;
    }
}

static void bx_take_headers(struct bx_bind *b, struct bx_sess *ss, struct h1_conn *c)
{
    struct h2_stream *s = h2_stream_get(&ss->c, b->sid);
    if (!s || !s->headers_done || b->hdr_done) return;
    struct h1_response *r = &c->resp;

    r->code = s->status;
    r->minor = 1;
    for (int i = 0; i < s->hdr.n; i++) {
        const struct hpack_hdr *h = &s->hdr.v[i];
        if (h->nlen && h->name[0] == ':') continue;    /* pseudo-fields are framing */
        h1_headers_add(&r->hdr, h->name, h->nlen, h->value, h->vlen);
        r->hdr_bytes += h->nlen + h->vlen + 4;
    }
    /* An HTTP/2 connection outlives every stream on it: there is no
     * `Connection: close` to honour and nothing here forces a socket shut. */
    r->keep_alive = 1;
    r->must_close = 0;
    r->clen = -1;
    r->no_body = r->head_request || r->code == 204 || r->code == 304;

    /* Mirror http1.c's rule exactly: a sink only takes effect when the body
     * needs no whole-message transform, because our inflater is one-shot. A
     * gzip response is therefore buffered and delivered complete -- correct,
     * just not incremental -- and h1_response_streaming() reports which. */
    const char *ce = h1_headers_get(&r->hdr, "content-encoding");
    int transform = ce && ce[0] && strcmp(ce, "identity") != 0;
    r->streaming = (r->sink && !transform && !r->no_body) ? 1 : 0;

    /* Any state but STATUS/HEADER/ERROR makes h1_response_headers_done() true,
     * which is the moment fetch() is specified to resolve. The body runs until
     * the stream ends, which is what BODY_EOF means. */
    r->state = H1_ST_BODY_EOF;
    b->hdr_done = 1;
}

/* Hand over whatever DATA landed in this pump.  Taking the buffer each time is
 * what makes delivery incremental: http2.c accumulates a stream's body, and
 * draining it every pump means a token that arrived in this frame's DATA frame
 * reaches the page in this frame -- not when the stream closes, which for a
 * chat response is never. */
static int bx_drain(struct bx_bind *b, struct bx_sess *ss, struct h1_conn *c)
{
    struct h2_stream *s = h2_stream_get(&ss->c, b->sid);
    if (!s || s->body_len <= 0) return H1_OK;
    int n = 0;
    uint8_t *p = h2_stream_take_body(&ss->c, b->sid, &n);
    if (!p) return H1_OK;
    if (n <= 0) { free(p); return H1_OK; }

    struct h1_response *r = &c->resp;
    r->body_seen += n;
    int rc = H1_OK;
    if (r->streaming && r->sink) {
        rc = r->sink(r->sink_ctx, p, n);
        free(p);
        return rc;
    }
    /* THE BODY CAP, on the path that had lost it. http1.c's own append stops
     * at r->body_max (H1_BODY_MAX = 8 MiB, h1_response_limit to raise) -- but
     * this function is a SECOND accumulation site, and it enforced nothing:
     * a body that never terminates (a stream, a chunked decode that never
     * meets its terminator, a deliberate drip) grew here at the sender's pace
     * until the int-overflow check below or the machine ran out first.
     * qwen's 240 s TIMEOUT wears exactly that signature -- steady [mm] low
     * heartbeats, no completion, no error, nothing printed, because nothing
     * was WRONG by this path's own rules. Same knob as http1.c, ONE policy:
     * H1_E_TOOLARGE surfaces through h1_strerror ("response exceeds limit")
     * to req_fail, so a script hitting this prints as a named script LOST,
     * not a hang. */
    if (r->body_len + n < 0 || (r->body_max > 0 && r->body_len + n > r->body_max))
        { free(p); return H1_E_TOOLARGE; }
    int need = r->body_len + n + 1;
    if (need > r->body_cap) {
        int cap = r->body_cap ? r->body_cap : 4096;
        while (cap < need) cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(r->body, (size_t)cap);
        if (!nb) { free(p); return H1_E_NOMEM; }
        r->body = nb; r->body_cap = cap;
    }
    memcpy(r->body + r->body_len, p, (size_t)n);
    r->body_len += n;
    r->body[r->body_len] = 0;      /* http1.c's callers rely on this */
    free(p);
    return rc;
}

int bxfer_pump(struct h1_conn *c)
{
    struct bx_bind *b = bx_bind_of(c);
    if (!b || b->sess < 0) return h1_conn_pump(c);
    if (c->state == H1_C_DONE || c->state == H1_C_ERROR) return c->state;

    struct bx_sess *ss = &g_sess[b->sess];
    if (!ss->used || !ss->h2_live) { c->state = H1_C_ERROR; c->err = H1_E_TRANSPORT; return c->state; }

    /* Pumping the CONNECTION, not the stream: every stream on it advances,
     * whichever one the caller happens to be stepping. */
    h2_conn_pump(&ss->c, (int64_t)monotonic_ms());

    bx_take_headers(b, ss, c);
    int rc = bx_drain(b, ss, c);
    if (rc != H1_OK) { c->state = H1_C_ERROR; c->err = rc < 0 ? rc : H1_E_NOMEM; return c->state; }

    struct h2_stream *s = h2_stream_get(&ss->c, b->sid);
    if (!s) { c->state = H1_C_ERROR; c->err = H1_E_TRUNC; return c->state; }

    if (s->done) {
        if (s->err) { c->state = H1_C_ERROR; c->err = bx_err_map(s->err); return c->state; }
        if (!b->hdr_done) { c->state = H1_C_ERROR; c->err = H1_E_TRUNC; return c->state; }
        c->resp.state = H1_ST_DONE;
        c->state = H1_C_DONE;
        return c->state;
    }
    /* A connection-level failure kills every stream on it, and a stream that
     * has not been told yet would otherwise wait for the idle timeout. */
    if (h2_conn_state(&ss->c) == H2_C_ERROR || h2_conn_state(&ss->c) == H2_C_CLOSED) {
        c->state = H1_C_ERROR;
        c->err = bx_err_map(ss->c.err);
        return c->state;
    }
    c->state = b->hdr_done ? H1_C_RECV : H1_C_SEND;
    return c->state;
}

void bxfer_free(struct h1_conn *c)
{
    struct bx_bind *b = bx_bind_of(c);
    if (b && b->sess >= 0) {
        struct bx_sess *ss = &g_sess[b->sess];
        if (ss->used && ss->h2_live) h2_stream_release(&ss->c, b->sid);
        memset(b, 0, sizeof *b);
        /* c->out is the serialized request the stream borrowed its body from;
         * releasing the stream above is what makes it safe to free now. */
        h1_conn_free(c);
        return;
    }
    if (b) memset(b, 0, sizeof *b);
    h1_conn_free(c);
}

/* Sub-resource (image) fetch, the name net/layout.c calls.
 *
 * This used to be SYS_RES_FETCH into a 768 KiB static buffer: one blocking
 * kernel fetch per image, `Connection: close`, so eight images from one host
 * were eight TLS handshakes. Now it is one pooled request -- and the size cap
 * is gone with it, because the body buffer grows as bytes arrive rather than
 * being reserved up front. */
/* `data:` -- decoded HERE, because this is the one place a sub-resource's
 * BYTES are asked for, and a data URI is bytes that have already arrived.
 *
 * It used to go to bfetch_sync like any other src, which parsed it as a URL,
 * failed, and reported `fetch failed (status 404)` against a 900-character
 * "hostname". Measured on www.bing.com, whose page carries its icons inline;
 * the scoreboard recorded it as a sub-resource failure, which is exactly the
 * wrong diagnosis -- nothing failed, nothing should have been fetched.
 *
 * RFC 2397: data:[<mediatype>][;base64],<data>. The media type is not parsed
 * and does not need to be: img_decode() sniffs the bytes, as it does for a
 * fetched image, so a lying `image/png` on a JPEG is handled the same way
 * here as over HTTP. Only the `;base64` flag matters, because it decides how
 * the payload is read.
 *
 * Percent-decoding the non-base64 form is done too: `data:image/svg+xml,%3Csvg`
 * is how an SVG icon is written inline more often than the base64 form. */
static int is_data_uri(const char *s)
{
    static const char k[] = "data:";
    if (!s) return 0;
    for (int i = 0; i < 5; i++) {
        int c = s[i], d = k[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != d) return 0;
    }
    return 1;
}

static int b64v(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;                      /* '=' and whitespace both land here */
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int data_uri(const char *src, unsigned char **buf, int *len)
{
    if (!is_data_uri(src)) return -1;
    const char *comma = 0;
    for (const char *p = src; *p; p++) if (*p == ',') { comma = p; break; }
    if (!comma) return -1;                       /* no payload: not a data URI */
    int b64 = 0;
    for (const char *p = src + 5; p < comma - 5; p++)
        if ((p[0]=='b'||p[0]=='B') && (p[1]=='a'||p[1]=='A') &&
            (p[2]=='s'||p[2]=='S') && (p[3]=='e'||p[3]=='E') &&
            p[4]=='6' && p[5]=='4') { b64 = 1; break; }

    const char *d = comma + 1;
    int n = 0; while (d[n]) n++;
    /* An upper bound, never an exact size: base64 is 3 bytes per 4 characters
     * and percent-decoding only ever shrinks. */
    unsigned char *out = (unsigned char *)malloc((size_t)n + 4);
    if (!out) return -1;
    int o = 0;

    if (b64) {
        /* `acc` is MASKED BACK after every byte emitted, and unsigned. Without
         * the mask it accumulates every sextet ever seen and overflows a
         * signed int about seven characters in -- undefined behaviour that
         * happens to work on this compiler and was caught by UBSan in
         * test-h2mux-asan the first time this ran, not by any output being
         * wrong. After an emit at most 7 bits are still owed, so the mask is
         * exact rather than defensive. */
        unsigned acc = 0; int bits = 0;
        for (int i = 0; i < n; i++) {
            int v = b64v((unsigned char)d[i]);
            if (v < 0) continue;                 /* padding and whitespace */
            acc = (acc << 6) | (unsigned)v; bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[o++] = (unsigned char)((acc >> bits) & 0xFFu);
                acc &= (1u << bits) - 1u;
            }
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (d[i] == '%' && i + 2 < n) {
                int h = hexv((unsigned char)d[i+1]), l = hexv((unsigned char)d[i+2]);
                if (h >= 0 && l >= 0) { out[o++] = (unsigned char)((h << 4) | l); i += 2; continue; }
            }
            out[o++] = (unsigned char)(d[i] == '+' ? ' ' : d[i]);
        }
    }
    if (o <= 0) { free(out); return -1; }
    *buf = out; *len = o;
    return 0;
}

int res_fetch(const char *src, unsigned char **buf, int *len)
{
    if (data_uri(src, buf, len) == 0) return 0;
    char abs[BF_URLMAX];
    if (bfetch_resolve(0, src, abs, sizeof abs) == 0 && cache_take(abs, buf, len) == 0)
        return 0;
    return bfetch_sync(src, buf, len);
}
