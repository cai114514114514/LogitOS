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
/* The cross-navigation HTTP cache, textually: js_wasm.c's pattern for a TU
 * the frozen BROWSER_PIPE list cannot name this wave (see http_cache.h's own
 * comment for the full why). #include'd rather than linked so the cache's
 * statics are browser_rt.c's -- one translation unit, one symbol space, no
* chance of a second consumer half-linking it. */
#include "http_cache.c"
/* For CK_HEADER_MAX only -- the ONE size of a Cookie: value, shared with the
 * two js_webapi.c call sites that used to disagree with this one by 8x. The
 * header declares no symbol this file links, so the cookieless build below
 * still holds. */
#include "cookies.h"
#include "../../../include/weaksym.h"   /* the two weak doors below; read it first */

/* The cookie jar's two transport-side doors, owned by js_webapi.c and WEAK
 * here: a build that links neither js_webapi.c nor cookies.c (the loader
 * host tests) resolves both to NULL and runs cookieless. Contract at their
 * definitions. */
int webapi_cookie_line_request(const char *, const char *, int,
                              const struct cookie_request *, char *, int) LOGIT_WEAK;
void webapi_cookie_store_request(const char *, const char *, int,
                                const struct cookie_request *, const char *) LOGIT_WEAK;
LOGIT_WEAK_STUB(webapi_cookie_line_request);
LOGIT_WEAK_STUB(webapi_cookie_store_request);
int cookie_request_kind(const struct cookie_ctx *, const struct cookie_request *) LOGIT_WEAK;
LOGIT_WEAK_STUB(cookie_request_kind);

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

#define BF_SUBREQ  16
/* Navigation must start while the previous page still owns its requests. */
#define BF_NREQ    (BF_SUBREQ + 1)
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

/* ---- the FIRST-BYTE deadline: a separate clock from BF_REQ_MS, not a third
 * multiplying factor ----
 *
 * BF_REQ_MS's 60 s is an IDLE deadline and its comment right above defends a
 * real case: a load has long gaps in it between phases, and a request that
 * has already received part of a response should not be killed for a slow
 * remainder. That defence has nothing to do with a request that has sent its
 * headers and received not ONE response byte -- a server that accepted the
 * TCP/TLS handshake and then never writes (a proxy with no live backend, a
 * firewall's black hole, a load balancer's default route). qwen.ai's split is
 * the specimen, not the target: 17 of its 23 requests never got a response
 * byte and each one held its slot -- one of the origin's TWO -- for the full
 * 60 s idle timeout while the other 6 live requests queued behind them.
 *
 * WHY 12000: chosen from the two ends of the evidence this investigation
 * already has, not from taste. The floor: this tree's own fast loads answer
 * their first byte in well under a second (the [wa] timeline's per-module
 * stamps, ~170 ms apart on x.com, are round trips on a connection already
 * proven live). The ceiling: a subresource fetched against a host whose
 * document just answered has no plausible reason to still be silent at
 * 10-15 s -- DNS, TCP and (for https) the TLS handshake are already proven to
 * work against that exact host, because req_begin_exchange only runs after
 * SOCK_P_CONNECTED, which per sock.c's own comment means "the transport is up
 * INCLUDING the TLS handshake". 12 s splits that range and leaves an order of
 * magnitude of margin over the floor.
 *
 * NOT A THIRD FACTOR IN THE BF_HOPS x BF_REQ_MS TRAP (see t0's comment on
 * struct breq below): this clock only ever fires EARLIER than BF_REQ_MS's
 * existing idle check (12 s < 60 s), so on a dead request it can only
 * SHORTEN the time req_expired would eventually have spent anyway -- it
 * cannot lengthen anything, and every hop and retry is still bounded by the
 * same t_start / BF_TOTAL_MS check that already caps the product. A
 * _Static_assert says so rather than leaving it as a claim nobody re-checks
 * when either number moves. */
#define BF_FIRSTBYTE_MS 12000
_Static_assert(BF_FIRSTBYTE_MS < BF_REQ_MS,
              "the first-byte deadline must fire strictly before the idle "
              "deadline, or it adds a case req_expired did not already cover");

enum { RQ_FREE = 0, RQ_QUEUED, RQ_DIAL, RQ_XFER, RQ_DONE, RQ_FAIL };

struct breq {
    char content_type[128], content_disposition[768];
    char content_security_policy[4096], x_frame_options[256];
    int policy_known, embedded;
    bfetch_embedded_redirect embedded_redirect;
    void *embedded_owner;
    int   state;
    char  url[BF_URLMAX];
    struct url u;                    /* url parsed */
    int   hops;
    int   fd;                        /* socket handle, -1 when none */
    int   pslot;                     /* hpool slot, -1 when none */
    /* 1 = fd is a bxfer SESSION handle (offered "h2,http/1.1"; may be shared
     * with other concurrent requests to the same origin), not a plain socket.
     * Set by the dial in req_connect(), consumed by req_drop_conn() -- which
     * is the one place a connection is disposed of, so every path (fail,
     * redirect hop, retry, release) inherits the rule by construction. The
     * distinction matters because the h2 session owns both the fd AND its
     * pool slot: closing either directly would either double-close the
     * socket or strand the pool's accounting, and an h1 session dialed here
     * must be YIELDED to the plain keep-alive pool (bxfer_h1_yield) rather
     * than dropped, or HTTP/1.1 servers lose the reuse bfetch always had. */
    int   bx;
    /* 1 = the bxfer handle above was a FRESH dial by this request (not a
     * join): only then may a finished HTTP/1.1 exchange yield the socket to
     * the idle keep-alive pool. A joined handle is one ref among several
     * and its finish is not the connection's finish. */
    int   bx_fresh;
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
    struct cookie_request cookie_request;
    char initiator_host[URL_HOST_MAX]; /* cookie_request.site_host borrows THIS */
    /* ---- http_cache (revalidation) ----
     * A stale-cache hit arms this request as a CONDITIONAL GET: these hold
     * the stored entry's validators, build_get() turns them into
     * If-None-Match/If-Modified-Since, and a 304 back is answered from the
     * cache in req_step_xfer(). Both empty = an ordinary unconditional GET. */
    char  wv_etag[256];
    char  wv_lmod[64];
    /* THE COOKIE LINE THIS EXCHANGE ACTUALLY PUT ON THE WIRE, captured by
     * build_get() at the instant it built the Cookie header -- see
     * req_cookie_line's comment for why NOT caching it once for the whole
     * request (a hop retargets r->u). This is separate from that: it is
     * captured EVERY time build_get runs (once per attempt), and read back
     * by the STORE and 304 sites in req_step_xfer, which must NEVER call
     * req_cookie_line() fresh at response time.
     *
     * THE TRAP THIS FIELD EXISTS TO CLOSE, found on the guest (2026-09-02),
     * SILENT, and it is why "recompute fresh, it's just a function call" is
     * the wrong instinct here: req_step_xfer ingests a response's Set-Cookie
     * header into the jar BEFORE the store call runs (a few lines above --
     * ingesting a login/challenge cookie before following its own redirect
     * is the whole point of that ordering). A store site that called
     * req_cookie_line() fresh at that point would therefore read the JAR
     * AFTER this response's own Set-Cookie had already been folded into it
     * -- storing the challenge page under the cookie it is ABOUT TO CAUSE,
     * not the cookie the request that fetched it actually carried. That is
     * exactly the wrong key by one response: it makes the cache key equal
     * the NEXT request's cookie line, so the cookie-bearing re-navigation
     * hits the challenge's own entry again -- the douyin loop, reintroduced
     * one layer down from the bug this whole file exists to fix, and only
     * on the FIRST navigation of a session (every later store, where the
     * jar was not just mutated by this exact response, recomputes to the
     * same answer either way -- which is why this shipped once, in a unit
     * test that never involves a live Set-Cookie response, and only showed
     * up driving the real fixture server through QEMU). sent_ck is what
     * removes the ambiguity: it is fixed at send time, before any response
     * -- let alone this one -- can mutate the jar. */
    char  sent_ck[CK_HEADER_MAX];
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
    /* When THIS ATTEMPT's exchange was handed to h1_conn_start -- i.e. the
     * instant its headers were about to go out. Reset exactly where t0 is
     * (a fresh dial, a hop, a retry): BF_FIRSTBYTE_MS's clock, like t0's, has
     * to restart for each new connection, because a redirect target or a
     * fresh-connection retry is a different exchange and the silence-so-far
     * of the old one does not belong to it. Kept separate from t0 rather than
     * reusing it: t0 also covers DIAL (TCP connect + TLS handshake), which
     * bfetch_init's comment measures as costing real SECONDS on this host,
     * and folding that into a 12 s first-byte budget would fail slow-but-live
     * handshakes along with the dead servers it exists to catch. See
     * BF_FIRSTBYTE_MS's comment above for the rest of the argument. */
    unsigned long long t_xfer;

    /* ---- byte range (see bfetch.h) ----
     * rq_first < 0 means "no range asked", which is the memset-0 case only
     * because every constructor sets it explicitly -- do not rely on 0. */
    long long rq_first, rq_last;      /* what we asked for; rq_last < 0 = open */
    int       rres;                   /* BF_R_* */
    long long got_first, got_last, got_total;   /* what arrived; -1 unknown */
};

/* ONE FUNCTION for the wire and the http_cache key -- one jar, two doors
 * (AGENTS.md section 1). Every place that needs "the Cookie header for r's
 * current request" -- build_get's actual header, the cache lookup before a
 * connection exists, the cache store after a response arrives, the 304
 * re-arm -- calls THIS, never webapi_cookie_line() directly, so the bytes
 * that decide the cache key and the bytes that go out on the wire cannot
 * drift apart by one of them being computed a different way.
 *
 * DELIBERATELY NOT cached once on `r`: r->u (host/path/scheme) can change
 * mid-request -- a redirect hop retargets it (see the hop handling below,
 * which for the identical reason clears r->wv_etag/r->wv_lmod rather than
 * letting a stale validator survive a hop) -- and a cookie line computed for
 * hop A's host would be both the WRONG Cookie header for hop B's origin and
 * the wrong cache key for it. Computing fresh from r->u at each call site
 * costs one jar walk on cache-relevant paths only (never per poll pass), and
 * is what makes "same bytes, same function" true after a hop as well as
 * before one. */
static int req_cookie_line(const struct breq *r, char *out, int cap)
{
    out[0] = 0;
    if (LOGIT_HAVE(webapi_cookie_line_request))
        return webapi_cookie_line_request(r->u.host, r->u.path, r->u.https,
                                         &r->cookie_request, out, cap);
    return 0;
}

static struct breq g_req[BF_NREQ];
static struct hpool g_pool;
static int  g_pool_ready;
static char g_base[BF_URLMAX] = "about:blank";
/* A stylesheet/import base is a URL resolver, never the document's identity.
 * Requests snapshot this independent committed document before queueing. */
static char g_document[BF_URLMAX] = "about:blank";
static int  g_dials, g_reuses, g_reqs;
/* The cross-navigation cache's ONE OFF switch (bfetch_set_bypass). Zero for
 * every ordinary navigation; browser.c's ctrl+R SHOULD set it and does not
 * yet, because load() has no way to say "this is a reload" -- the one-line
 * diff is in the webaccel report. While it is unset, a reload reuses cached
 * subresources like any other revisit, which is what a heuristic-freshness
 * cache legitimately does. */
static int  g_wa_bypass;

/* ===================== [wa]: the open-time timeline stamps =================
 *
 * WHY THESE LINES EXIST (webaccel, 2026-08-30). "Page-open speed is too slow"
 * was the complaint, and the house rules forbid answering it with host wall
 * clock -- five sibling agents run QEMU on this host, so the only comparable
 * numbers are GUEST timestamps taken inside ONE boot. The browser app already
 * reads the kernel's monotonic clock (monotonic_ms, the same counter
 * tools/perf uses through /dev/kstat), and the phases this file can see are
 * exactly the network ones an accelerator has levers over: when the navigation
 * request was armed, when its document settled, and when the network last went
 * idle mid-load (which is where a subresource batch ended).
 *
 * THE ENGINE-SIDE PHASES (parse, css apply, layout, paint) are deliberately
 * NOT stamped from here: they happen in browser.c, and the driver gets them
 * from that file's own serial prints plus the [wm] perf t= line the window
 * manager prints every second -- ~1 s resolution, stated as such. What is
 * stamped from here is millisecond-exact because monotonic_ms() is.
 *
 * DELIBERATELY NOT a per-request log. Serial output is flow-controlled and a
 * heavy page runs 30+ subresource requests; stamping each would put harness
 * cost inside the thing being measured. Phase-level transitions only. */
static unsigned long long wa_last_stamp;
static void wa_stamp(const char *what)
{
    unsigned long long now = monotonic_ms();
    printf("[wa] t=%llu (+%llu) %s\n", now, now - wa_last_stamp, what);
    wa_last_stamp = now;
}
/* Track the pending-count edge so "network went idle" prints once per batch,
 * not once per pump pass. 0 = "was idle (or never pumped) last time we looked". */
static int wa_was_busy;
/* Set the first time a NAVIGATION request is armed. The loadend stamp below
 * rides bfetch_stats(), and the host range tests call bfetch_stats() without
 * ever arming a navigation -- without this guard their logs grow a [wa] line
 * per call and any output-comparing assertion sees a cache that was never
 * asked about. */
static int wa_saw_nav;

/* Defined with the bxfer session table below.  These declarations live up
 * here because bfetch owns the pool callback and request scheduler, while the
 * thing that must be kept coherent with them -- the HTTP/2 session -- is the
 * lower half of this file. */
static void bxfer_pool_closed(int fd);
static int bxfer_join_ready_h2(const char *host, int port, int tls, int wait_slot);
static int bxfer_join_pending(const char *host, int port, int tls, int pooled);
static int bxfer_h1_busy(int fd);

static void pool_closer(int fd, void *ctx, void *user)
{
    (void)ctx; (void)user;
    /* A plain h1 fd has no bxfer entry and this is a no-op.  An idle h2 fd has
     * BOTH a pool slot and a live HPACK/flow-control session; closing only the
     * fd used to leave that second owner pointing at a recycled descriptor.
     * Forget it in the same callback that closes the authoritative pool slot,
     * so expiry and cross-origin eviction cannot manufacture a stale session. */
    bxfer_pool_closed(fd);
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

void bfetch_set_document(const char *page_url)
{
    int i = 0;
    if (page_url) while (page_url[i] && i < BF_URLMAX - 1) {
        g_document[i] = page_url[i]; i++;
    }
    g_document[i] = 0;
}

#include "bfetch_url.inc"

int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ return browser_url_resolve(base && base[0] ? base : g_base, ref, out, max); }

/* ---- transport: h1_transport over the non-blocking socket ABI ----
 * ctx is the fd as a pointer-sized integer, NOT the breq: bxfer_start()
 * overwrites ht.ctx with the (possibly re-dialled) fd of its own choosing,
 * and these three only ever needed r->fd -- naming the fd directly is the
 * one form that is correct for both the plain and the bxfer path. */

static int tr_read(void *ctx, void *buf, int len)
{
    int fd = (int)(long)ctx;
    int n = sock_recv(fd, buf, len);
    if (n > 0) return n;
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return H1_TERR;
    /* Drain first, THEN believe EOF: the EOF bit can be set while bytes are
     * still sitting in the receive ring, and treating that as end-of-message
     * truncates the last response on every connection the server closes. */
    if (n < 0 || (bits & SOCK_P_EOF)) return H1_EOF;
    return H1_AGAIN;
}

static int tr_write(void *ctx, const void *buf, int len)
{
    int fd = (int)(long)ctx;
    int n = sock_send(fd, buf, len);
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
    int fd = (int)(long)ctx;
    if (!want_write) return 1;                   /* never trusted; read anyway */
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return -1;
    return (bits & SOCK_P_WRITABLE) ? 1 : 0;
}

/* ---- request lifecycle ---- */

static void req_drop_conn(struct breq *r, int reusable)
{
    /* bxfer_free, not h1_conn_free: it releases the h2 stream (or is exactly
     * h1_conn_free on the plain path) and frees the request buffer the
     * stream's body borrowed. Works for both, so there is one call. */
    if (r->c_live) { bxfer_free(&r->c); r->c_live = 0; }
    if (r->bx) {
        int ps = -1;
        if (reusable && r->bx_fresh && bxfer_h1_yield(r->fd, &ps) >= 0) {
            /* A finished HTTP/1.1 session becomes exactly the pooled
             * connection bfetch always dialled itself: fd and pool slot
             * transfer to the caller, the session entry dissolves, and the
             * ordinary keep-alive path below takes over. h2 connections
             * cannot take this path (their socket is not one exchange's to
             * give) and fall to bxfer_close, which is a refcount: the last
             * stream out used to drop the connection. Correction: a healthy
             * idle H2 session now remains pooled until expiry/eviction. */
            r->bx = 0;
            r->pslot = ps;
        } else {
            bxfer_close(r->fd);
            r->fd = -1;
            r->bx = 0;
            return;
        }
    }
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
    /* Revalidation conditionals, when a stale cross-navigation cache entry
     * armed this request (bfetch_start_range_impl copies the stored ETag /
     * Last-Modified into the breq). Empty strings mean an ordinary GET. Both
     * at once is normal and correct -- RFC 9111 13.1.4 says a cache SHOULD
     * send both and the server prefers ETag. */
    if (r->wv_etag[0])
        h1_request_set_header(&q, "If-None-Match", r->wv_etag);
    if (r->wv_lmod[0])
        h1_request_set_header(&q, "If-Modified-Since", r->wv_lmod);
    /* Cookies, on EVERY transport request -- navigation, reload, and each
     * subresource -- not only the JS fetch()/XHR path. The jar lives in
     * js_webapi.c and is reached through a weak symbol so builds without
     * that TU (the loader host tests) link and run cookieless. See the
     * export comment in js_webapi.c for the WAF loop this closes, and
     * req_cookie_line's own comment for why THIS is the one call site for
     * it rather than webapi_cookie_line() called inline.
     *
     * INTO r->sent_ck, NOT A LOCAL BUFFER: this is the one place the bytes
     * that go on the wire are decided, so this is the one place that gets
     * to freeze them. req_step_xfer's store/304 sites read r->sent_ck back
     * rather than recomputing -- see that field's own comment for the
     * silent wrong-key bug recomputing there caused. */
    if (req_cookie_line(r, r->sent_ck, (int)sizeof r->sent_ck) < 0) {
        /* A partial/empty fallback changes both authentication and the cache
         * key. Refuse the exchange before serializing any request bytes. */
        printf("[bfetch] cookie-header overflow\n");
        h1_request_free(&q);
        return 0;
    }
    if (r->sent_ck[0] && h1_request_set_header(&q, "Cookie", r->sent_ck) != H1_OK) {
        h1_request_free(&q); return 0;
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
#ifndef BFETCH_NO_PENDING_JOIN
    /* A speculative pending handle can resolve to an H1 connection another
     * caller started first. Direct bxfer callers may redial from bxfer_start;
     * bfetch must return through its own pool budget instead. This applies to
     * the initial dialer too: a later borrower may have started first. */
    if (r->bx && bxfer_h1_busy(r->fd)) {
        req_drop_conn(r, 0);
        r->bx_fresh = 0;
        r->reused = 0;
        r->state = RQ_QUEUED; /* same request/deadline, no HTTP bytes sent */
        return 0;
    }
#endif
    int len = 0;
    char *req = build_get(r, &len);
    if (!req) { req_fail(r, "could not build request"); return 0; }
    /* ctx names the fd (tr_read/tr_write/tr_poll take it directly): the
     * plain h1 path and bxfer_start's H1 branch both build the transport
     * from this same handle, and bxfer_start may REPLACE the fd under us
     * (the speculative-join-lost case -- see its comment) -- *fd by
     * pointer, r->fd updated after, or the transport would name a socket
     * the exchange no longer owns. */
    struct h1_transport t = { tr_read, tr_write, tr_poll, (void *)(long)r->fd };
    int rc;
    if (r->bx) {
        int fd = r->fd;
        rc = bxfer_start(&r->c, &t, req, len, &fd, r->u.host, r->u.port, r->u.https);
        if (fd != r->fd) {
            /* bxfer re-dialled: the old handle was closed inside and the new
             * one is ours even if protocol startup subsequently failed.  The
             * old success-only assignment made req_fail close the already
             * released handle while leaking the replacement session on that
             * rare error path. */
            r->fd = fd;
            r->bx_fresh = 1;
        }
    } else {
        rc = h1_conn_start(&r->c, &t, req, len);
    }
    if (rc == BXFER_START_WAIT) {
        free(req);
        r->state = RQ_DIAL; /* same deadline, new handshake; no exchange yet */
        return 0;
    }
    if (rc != H1_OK) {
        /* Ownership of `req` passes only on SUCCESS (h1_conn_start's
         * contract, and bxfer_start's two branches each hand it to the
         * exchange that accepted it) -- on failure neither has taken it,
         * on either path, so freeing here is right for both. */
        free(req);
        req_fail(r, "h1_conn_start failed");
        return 0;
    }
    r->c_live = 1;
    h1_response_limit(&r->c.resp,r->embedded ? 512*1024 : r->nav ? 64*1024*1024 : BROWSER_BUFFERED_BODY_MAX);
    r->state = RQ_XFER;
    r->t_xfer = monotonic_ms();     /* the first-byte deadline's own clock */
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
#ifndef BFETCH_NO_H2_JOIN
    /* JOIN BEFORE ASKING TO DIAL. hpool_may_open() correctly says "no" when
     * this origin already owns an h2 connection -- opening a second would
     * defeat multiplexing -- but that made bfetch wait forever because its
     * only next move used to be a new dial.  GitHub/Bilibili/Bing are merely
     * specimens of the general failure: any document/style/script/image batch
     * arriving after another consumer opened h2 could never take a stream.
     *
     * Measured by h2mux_test below the transport: with this branch removed,
     * the existing stream completes but bfetch remains BF_PENDING and the
     * server sees 1 stream; here it sees 2 concurrent streams on 1 socket and
     * both complete. -DBFETCH_NO_H2_JOIN is kept as that watched-red control. */
    int h2fd = bxfer_join_ready_h2(r->u.host, r->u.port, tls, 0);
    if (h2fd >= 0) {
        r->fd = h2fd;
        r->bx = 1;
        r->bx_fresh = 0;
        r->reused = 1;
        g_reuses++;
        req_begin_exchange(r);
        return;
    }
#endif
#ifndef BFETCH_NO_PENDING_JOIN
    /* A synchronous resource wait can pause the Fetch owner that opened this
     * same-origin TLS session. Its socket may already be connected while the
     * session stays PENDING until a caller starts HTTP. Joining only ready H2
     * therefore left the resource queued behind may_open for 60 seconds.
     * Borrow the existing bounded reservation; never dial through this door.
     * RQ_DIAL polls CONNECTED before bxfer_start can resolve ALPN, preserving
     * both an unfinished handshake and the normal HTTP/1.1 fallback. */
    int pending_fd = bxfer_join_pending(r->u.host, r->u.port, tls, 1);
    if (pending_fd >= 0) {
        r->fd = pending_fd;
        r->bx = 1;
        r->bx_fresh = 0;
        r->reused = 1;
        g_reuses++;
        r->state = RQ_DIAL;
        return;
    }
#endif
    if (!hpool_may_open(&g_pool, r->u.host, r->u.port, tls, now))
        return;                                   /* caps full: try again next pump */
    /* THE DIAL GOES THROUGH BXFER (2026-09-09): the offer is "h2,http/1.1",
     * so a page's sub-resources can land on one multiplexed HTTP/2
     * connection instead of one TCP+TLS handshake each -- this dial used to
     * pass SOCK_F_ALPN_HTTP11 alone, which made every page load HTTP/1.1 BY
     * CONSTRUCTION no matter what the server would have chosen. bxfer owns
     * the socket AND its pool slot from here (r->bx = 1); req_drop_conn()
     * gives it back. The hpool_may_open gate above still applies because
     * bxfer_open_ex registers the session with the same pool: one budget,
     * two protocols. sock_why() keeps the error mapping exactly as it was
     * (the ABI's SOCK_E_* names the cause; the kernel's refusal line split
     * table-full from no-source-address from port-exhaustion the same day). */
    int fresh = 0;
    int fd = bxfer_open_ex(r->u.host, r->u.port, tls, &fresh);
    if (fd < 0) {
        printf("[bfetch] sock_open('%s', %d) = %d (%s)\n",
               r->u.host, r->u.port, fd, sock_why(fd));
        req_fail(r, sock_why(fd));
        return;
    }
    r->fd = fd;
    r->bx = 1;
    r->bx_fresh = fresh;
    r->reused = fresh ? 0 : 1;
    if (fresh) {
        g_dials++;
        r->state = RQ_DIAL;
    } else {
        /* Defensive rather than expected: the join-before-gate branch above
         * owns negotiated h2, and a pending session makes may_open return 0.
         * Keeping the distinction here prevents a future bxfer join kind from
         * being counted as a dial or left waiting for a handshake it does not
         * own. */
        g_reuses++;
        req_begin_exchange(r);
    }
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

/* Keep every policy field, separated as independent policies. A cache hit has
 * no such metadata today and remains policy_known=0; missing data must never
 * become an embedding allow. Overflow likewise rejects at the consumer. */
static int req_policy_headers(const struct h1_headers *headers,const char *name,char *out,int cap)
{
    int used=0,n=h1_headers_count(headers,name);out[0]=0;
    for(int i=0;i<n;i++) {
        const char *v=h1_headers_nth(headers,name,i);if(!v)return 0;
        size_t len=strlen(v);if(len+(size_t)used+(i?1:0)>=(size_t)cap)return 0;
        if(i)out[used++]='\n';memcpy(out+used,v,len+1);used+=(int)len;
    }
    return 1;
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
        /* bxfer_pump, not h1_conn_pump: it IS h1_conn_pump for a plain
         * exchange and it pumps the whole h2 CONNECTION for a multiplexed
         * one -- every stream advances, whichever request happens to be
         * stepping here, which is the whole point of the dial change. */
        st = bxfer_pump(&r->c);
        if (st != H1_C_SEND && st != H1_C_RECV) break;
        if (h1_conn_progress(&r->c) == before) break;      /* nothing moved */
    }
    if (st == H1_C_SEND || st == H1_C_RECV) {
#ifndef BFETCH_NO_FIRSTBYTE_DEADLINE
        /* THE FIRST-BYTE DEADLINE (see BF_FIRSTBYTE_MS above for why 12 s and
         * why this cannot become a third multiplying factor). rx_bytes is
         * http1.c's own count of bytes this exchange has pulled off the
         * transport (h1_conn_pump: "c->rx_bytes += n" happens only on a real
         * read) -- zero means literally nothing has come back, not "nothing
         * interesting yet". A response that has started (even one header
         * byte) makes rx_bytes nonzero forever, which disarms this check for
         * the rest of the attempt: a slow-but-alive server that answers late
         * is exactly the case BF_REQ_MS's 60 s already exists to protect.
         *
         * THIS IS THE NEGATIVE CONTROL'S SWITCH (tests/fetchdl.mk,
         * -DBFETCH_NO_FIRSTBYTE_DEADLINE): compiled out, a request that never
         * gets a single byte falls through to the idle check below and lives
         * the full BF_REQ_MS -- which is the state that let 17 of qwen.ai's
         * 23 requests hold one of their origin's two connection slots in
         * total silence for a full minute apiece, with the six requests that
         * COULD have answered queued behind them. */
        if (r->c.rx_bytes == 0 &&
            monotonic_ms() - r->t_xfer > BF_FIRSTBYTE_MS) {
            printf("[browser] fetch stalled: no response after %d s: %s\n",
                   BF_FIRSTBYTE_MS / 1000, r->url);
            req_fail(r, "stalled: no response");
            return;
        }
#endif
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
    if (LOGIT_HAVE(webapi_cookie_store_request)) {
        int nck = h1_headers_count(&resp->hdr, "set-cookie");
        for (int ci = 0; ci < nck; ci++) {
            const char *sc = h1_headers_nth(&resp->hdr, "set-cookie", ci);
            if (sc) webapi_cookie_store_request(r->u.host, r->u.path, r->u.https,
                                                &r->cookie_request, sc);
        }
    }

    if (h1_is_redirect(resp->code)) {
        const char *loc = h1_headers_get(&resp->hdr, "location");
        if (loc && r->hops < BF_HOPS) {
            char next[BF_URLMAX];
            int resolved=r->embedded ? r->embedded_redirect &&
                r->embedded_redirect(r->embedded_owner,r->url,loc,next,sizeof next) :
                bfetch_resolve(r->url,loc,next,sizeof next)==0;
            if (resolved) {
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
                /* Validators do NOT survive a hop: they vouch for the STORED
                 * entry under this request's URL, and the hop's target is a
                 * different resource. Sending url-A's ETag to url-B asks B to
                 * 304 a body it never served -- silent wrong-bytes territory. */
                r->wv_etag[0] = 0;
                r->wv_lmod[0] = 0;
                int i = 0; while (next[i] && i < BF_URLMAX - 1) { r->url[i] = next[i]; i++; }
                r->url[i] = 0;
                struct url next;
                if (url_parse(r->url, &next) != 0) { req_fail(r, "bad redirect target"); return; }
                if (!r->cookie_request.browser_initiated && LOGIT_HAVE(cookie_request_kind)) {
                    /* A return hop cannot recover the original SameSite
                     * privilege after leaving an unrelated site's response. */
                    struct cookie_ctx previous = { r->u.host, r->u.path, r->u.https, 1 };
                    struct cookie_request next_site = { next.host, next.https, 0, 1, 0 };
                    if (cookie_request_kind(&previous, &next_site) != CK_REQ_SAME_SITE &&
                        cookie_request_kind(&previous, &r->cookie_request) != CK_REQ_SAME_SITE)
                        r->cookie_request.site_host = 0;
                }
                r->u = next;
                r->state = RQ_QUEUED;
                return;
            }
        }
    }

    /* ---- 304: the cache's answer, not an error --------------------------------
     *
     * This request was armed as a conditional GET because a stored entry was
     * stale; the server just said the stored bytes are still the bytes. The
     * caller sees status 200 and the stored body -- what a browser reports
     * for a validated cache hit, because that is what the user got. A 304
     * with NO armed validator is a server bug (nothing here asked "changed?")
     * and is failed BY NAME rather than handed to the caller as a bodyless
     * 2xx-looking success. */
    if (resp->code == 304) {
        /* r->sent_ck, NOT a fresh req_cookie_line() call: this response's own
         * headers have not been ingested into the jar yet at this point in
         * the function (Set-Cookie ingestion is below, past the redirect
         * branch), so a recompute would still be safe on THIS response --
         * but sent_ck is what build_get() actually put on the wire for the
         * conditional GET that produced this 304, which is the only bytes
         * that can be right by definition, and using it here rather than
         * "recompute, it should agree" is the same discipline the store
         * site below needs for real (see r->sent_ck's own comment for the
         * case where recomputing silently does NOT agree). */
        unsigned char *b = 0; int bl = 0;
        int have_val = r->wv_etag[0] || r->wv_lmod[0];
        if (have_val && wacache_body(r->url, r->sent_ck, &b, &bl) == 0) {
            wacache_response_get(r->url,r->sent_ck,r->content_type,sizeof r->content_type,r->content_disposition,sizeof r->content_disposition);
            wacache_refresh(r->url, r->sent_ck,
                            h1_headers_get(&resp->hdr, "cache-control"),
                            h1_headers_get(&resp->hdr, "expires"),
                            h1_headers_get(&resp->hdr, "date"));
            req_drop_conn(r, keep);
            r->body = b; r->blen = bl;
            r->status = 200;
            r->rres = BF_R_NONE;
            r->got_first = r->got_last = r->got_total = -1;
            r->state = RQ_DONE;
            return;
        }
        req_drop_conn(r, 0);
        r->err = "304 without a usable cached entry";
        r->state = RQ_FAIL;
        return;
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

    /* Navigation needs response metadata AFTER the connection/parser is
     * released. Previously only status/body survived, so an extensionless
     * Content-Disposition attachment was parsed as an empty/binary page. */
    const char *ct=h1_headers_get(&resp->hdr,"content-type");
    const char *cd=h1_headers_get(&resp->hdr,"content-disposition");
    if(ct){size_t n=strlen(ct);if(n<sizeof r->content_type)memcpy(r->content_type,ct,n+1);}
    if(cd){size_t n=strlen(cd);if(n<sizeof r->content_disposition)memcpy(r->content_disposition,cd,n+1);
        else if(!strncmp(cd,"attachment",10))memcpy(r->content_disposition,"attachment",11);}
    r->policy_known=req_policy_headers(&resp->hdr,"content-security-policy",r->content_security_policy,sizeof r->content_security_policy) &&
        req_policy_headers(&resp->hdr,"x-frame-options",r->x_frame_options,sizeof r->x_frame_options);

    /* Steal the body: h1_conn_free would otherwise take it with the response. */
    r->body = resp->body;
    r->blen = resp->body_len;
    resp->body = 0; resp->body_len = 0; resp->body_cap = 0;

    /* Store into the cross-navigation cache -- BEFORE req_drop_conn, which is
     * what frees the header list these reads come from. Whole-resource 2xx
     * GETs only: rq_first < 0 says no range was ASKED (a 206 or a
     * range-ignored 200 never reaches here with rq_first >= 0, and
     * range_check has already refused the malformed ones), and the cache is
     * keyed by (URL, Cookie header) so a slice under a plain URL is still
     * corruption regardless of cookie (bfetch.h's invariant). Store
     * REFUSALS are silent to the page: the entry is simply not cached and
     * the next visit refetches, which is exactly what a browser with no
     * cache does.
     *
     * THE KEY IS r->sent_ck, NOT A FRESH req_cookie_line() CALL, AND THAT
     * IS LOAD-BEARING, NOT STYLE: storing under the request's own header
     * (rather than the response's Set-Cookie) is what makes the very next
     * re-navigation -- now carrying that cookie -- miss this entry and
     * fetch for real (see http_cache.h's Set-Cookie paragraph). But "the
     * request's own header" means what build_get() PUT ON THE WIRE, and by
     * the time this line runs, the Set-Cookie ingestion a few lines above
     * (BEFORE the redirect branch, so a login/challenge cookie is captured
     * before its own redirect is followed) may have ALREADY folded THIS
     * response's Set-Cookie into the jar -- so a fresh req_cookie_line()
     * call right here would read the POST-ingestion jar and silently key
     * the challenge page to the cookie it is ABOUT to cause, reintroducing
     * the douyin loop one layer down on the very first navigation of a
     * session (found on the guest, 2026-09-02, invisible to the host unit
     * test because that test never ingests a live Set-Cookie response --
     * see r->sent_ck's own comment). r->sent_ck was frozen by build_get()
     * before this response, let alone its Set-Cookie, existed. */
    if (r->rq_first < 0 && r->status / 100 == 2 && r->body && r->blen > 0 &&
        !g_wa_bypass && !r->embedded) {
        int stored=wacache_store(r->url, r->sent_ck, r->body, r->blen,
                      h1_headers_get(&resp->hdr, "cache-control"),
                      h1_headers_get(&resp->hdr, "expires"),
                      h1_headers_get(&resp->hdr, "date"),
                      h1_headers_get(&resp->hdr, "last-modified"),
                      h1_headers_get(&resp->hdr, "etag"),
                      h1_headers_get(&resp->hdr, "vary"),
                      h1_headers_count(&resp->hdr, "set-cookie") > 0);
        if(stored==0)wacache_response_set(r->url,r->sent_ck,r->content_type,r->content_disposition);
    }

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
    /* RECOUNT after advancing: a request that settled during this pass was
     * counted busy at the top of its own iteration, so the pending count the
     * loop built is stale-high by exactly the requests that finished inside
     * it -- which are precisely the ones at the end of every batch. Without
     * the recount, the idle stamp below waits for a pump that never comes
     * (the batch's driver loop exits instead), and no batch end is ever
     * stamped. Found on the first measured run: zero idle lines. */
    pending = 0;
    for (int i = 0; i < BF_NREQ; i++) {
        int s = g_req[i].state;
        if (s == RQ_FREE || s == RQ_DONE || s == RQ_FAIL) continue;
        pending++;
    }
    /* The idle edge, once per batch: a subresource batch (stylesheets, scripts,
     * images -- browser.c's three res_fetch_all calls) ends when pending hits
     * zero and stays there until the next batch arms. Counters ride the line so
     * a batch's dial/reuse cost is readable without waiting for "load done". */
    if (pending == 0 && wa_was_busy) {
        unsigned long long now = monotonic_ms();   /* read once: two reads could straddle a tick */
        printf("[wa] t=%llu (+%llu) idle reqs=%d dials=%d reuses=%d\n",
               now, now - wa_last_stamp, g_reqs, g_dials, g_reuses);
        wa_last_stamp = now;
        wa_was_busy = 0;
    } else if (pending > 0) {
        wa_was_busy = 1;
    }
    return pending;
}

static int bfetch_start_range_context(const char *base, const char *ref,
                                   long long first, long long last, int nav,
                                   const char *initiator, int embedded,
                                   bfetch_embedded_redirect redirect,void *owner)
{
    if (!g_pool_ready) bfetch_init();
#ifdef BROWSER_COOKIE_BASE_IS_DOCUMENT
    if (!nav) initiator = base && base[0] ? base : g_base;
#endif
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
    for (int i = 0; i < (nav ? BF_NREQ : BF_SUBREQ); i++) {
        struct breq *r = &g_req[i];
        if (r->state != RQ_FREE) continue;
        memset(r, 0, sizeof *r);
        r->fd = -1; r->pslot = -1;
        r->rq_first = first < 0 ? -1 : first;
        r->rq_last  = last  < 0 ? -1 : last;
        r->rres = BF_R_NONE;
        r->nav = nav ? 1 : 0;
        r->embedded=embedded;
        r->embedded_redirect=redirect;r->embedded_owner=owner;
        struct url site;
        r->cookie_request.top_level_navigation = r->nav;
        r->cookie_request.safe_method = 1; /* this transport only sends GET */
        r->cookie_request.browser_initiated = nav && !initiator;
        if (initiator && url_parse(initiator, &site) == 0) {
            memcpy(r->initiator_host, site.host, sizeof r->initiator_host);
            r->cookie_request.site_host = r->initiator_host;
            r->cookie_request.site_secure = site.https;
        }
        r->got_first = r->got_last = r->got_total = -1;
        int n = 0; while (abs[n] && n < BF_URLMAX - 1) { r->url[n] = abs[n]; n++; }
        r->url[n] = 0;
        if (url_parse(r->url, &r->u) != 0) { r->state = RQ_FREE; return -1; }
        r->t0 = monotonic_ms();
        r->t_start = r->t0;                 /* the one clock no hop may reset */
        r->state = RQ_QUEUED;
        g_reqs++;
        /* nav=1 means browser.c's ONE navigation door armed this request -- the
         * moment the address bar's string became a fetch. The stamp is here
         * and not in browser.c because this file owns the clock's rendering;
         * the URL itself is on the "[browser] load:" line immediately before. */
        if (nav) { wa_stamp("nav"); wa_saw_nav = 1; }

        /* ---- the cross-navigation cache, consulted before a connection ----
         *
         * Whole-resource GETs only (first < 0): the cache is keyed by URL
         * alone and a ranged answer is a slice -- bfetch.h's invariant, and
         * the same rule the prefetch cache guards. Three outcomes:
         *   FRESH  -> the request settles HERE, no connection, no dial, no
         *             DNS. The caller's state machine sees the one shape it
         *             already knows (BF_DONE + body + status 200).
         *   STALE  -> the stored validators arm a conditional GET below, and
         *             a 304 is answered from the cache in req_step_xfer().
         *   MISS   -> an ordinary GET, byte-identical to before this existed.
         * Reload-bypass (g_wa_bypass) skips both -- the caller asked for the
         * network and gets it, stores included.
         *
         * THE KEY, and why no second bypass flag is needed for the script-
         * navigation case (douyin's challenge-and-reload): `ck` below is
         * THIS request's Cookie header, computed from r->u/r->nav which are
         * already set for THIS specific navigation -- a script-driven
         * re-navigation (location.reload(), or an href write to a new URL;
         * see js_webapi.c's loc_reload/loc_set) reaches here through the
         * ordinary bfetch_start_nav -> bfetch_start_range_impl path with a
         * freshly built r, so the re-navigation that follows a Set-Cookie
         * carries the new cookie and therefore computes a DIFFERENT `ck`,
         * therefore a different (url, ck) key, therefore a MISS against the
         * challenge page's entry -- correctly falling through to
         * req_connect below and dialling for real. GUEST-MEASURED, twice:
         * with the key change compiled out (-DWACACHE_NO_COOKIE_KEY,
         * tests/qmp/qmp_webaccel.py --chl) the challenge shell is served 11
         * times in a row (NAV_MAX_HOPS) and the real page never arrives;
         * with it, the shell serves exactly once and the real page exactly
         * once. The bug this closes was never "scripted navigations need a
         * bypass"; it was "the key could not see the one header value that
         * changed between the two navigations". Fixing the key fixes every
         * caller of this function, script-driven or not, which is the
         * general fix AGENTS.md's "never fit a site" rule asks for -- no
         * branch here knows or cares that douyin exists. */
        if (first < 0 && !g_wa_bypass && !embedded) {
            static char ck[CK_HEADER_MAX];
            if (req_cookie_line(r, ck, (int)sizeof ck) < 0) {
                req_fail(r, "cookie header exceeds capacity");
                return i;
            }
            unsigned char *cbody = 0; int clen = 0;
            if (wacache_lookup(abs, ck, &cbody, &clen) == 0) {
                wacache_response_get(abs,ck,r->content_type,sizeof r->content_type,r->content_disposition,sizeof r->content_disposition);
                r->body = cbody;
                r->blen = clen;
                r->status = 200;
                r->state = RQ_DONE;
                /* g_reqs already counted this request -- deliberately: the
                 * "load done: N requests" line stays comparable across cache
                 * on/off (it counts what the PAGE asked for), while dials and
                 * reuses are what show the network never ran. */
                return i;
            }
            (void)wacache_validators(abs, ck, r->wv_etag, (int)sizeof r->wv_etag,
                                     r->wv_lmod, (int)sizeof r->wv_lmod);
        }

        req_connect(r);                     /* a free slot dials immediately */
        return i;
    }
    return -1;
}

static int bfetch_start_range_impl(const char *base,const char *ref,long long first,long long last,int nav,const char *initiator)
{ return bfetch_start_range_context(base,ref,first,last,nav,initiator,0,0,0); }

int bfetch_start_embedded(const char *ref,const char *owner_url,const char *ancestor_url,
                          bfetch_embedded_redirect redirect,void *context_owner)
{
    struct url owner,ancestor;
    if(!owner_url || !ancestor_url || url_parse(owner_url,&owner) || url_parse(ancestor_url,&ancestor))return -1;
    struct cookie_ctx dst={owner.host,owner.path,owner.https,1};
    struct cookie_request site={ancestor.host,ancestor.https,0,1,0};
    /* The immediate owner's origin cannot erase a cross-site ancestor. In
     * this passive depth-one implementation the supplied ancestor is the top
     * document; extending depth requires checking the ENTIRE ancestor chain. */
    const char *initiator=LOGIT_HAVE(cookie_request_kind) &&
        cookie_request_kind(&dst,&site)==CK_REQ_SAME_SITE ? ancestor_url : "";
#ifdef BFETCH_EMBED_OWNER_ONLY
    initiator=owner_url; /* local negative control: lose the ancestor boundary */
#endif
    if(!redirect)return -1;
    return bfetch_start_range_context(owner_url,ref,-1,-1,0,initiator,1,redirect,context_owner);
}

int bfetch_start_from(const char *base, const char *ref)
{ return bfetch_start_range_impl(base, ref, -1, -1, 0, g_document); }

int bfetch_start(const char *ref) { return bfetch_start_from(0, ref); }

/* The navigation door. It is a SEPARATE entry rather than a flag on
 * bfetch_start() so that the permissive classification has to be asked for by
 * name: every existing caller stays a subresource without being edited, and a
 * new one is a subresource until somebody decides otherwise. The reverse
 * default -- one parameter, forgotten once -- is the bug this whole change
 * exists to close, in the same shape one layer down (cookies.h's enum). */
int bfetch_start_nav(const char *ref)
{ return bfetch_start_range_impl(0, ref, -1, -1, 1, g_document); }

int bfetch_start_nav_from(const char *ref, const char *initiator_url)
{ return bfetch_start_range_impl(0, ref, -1, -1, 1, initiator_url); }

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
    return bfetch_start_range_impl(base, ref, first, last, 0, g_document);
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

const char *bfetch_response_header(int id,const char *name)
{
    struct breq *r=req_of(id);if(!r||r->state!=RQ_DONE||!name)return NULL;
    if(!strcmp(name,"content-type"))return r->content_type;
    if(!strcmp(name,"content-disposition"))return r->content_disposition;
    if(!strcmp(name,"content-security-policy"))return r->content_security_policy;
    if(!strcmp(name,"x-frame-options"))return r->x_frame_options;
    return NULL;
}
int bfetch_response_policy_known(int id)
{ struct breq *r=req_of(id);return r && r->state==RQ_DONE && r->policy_known; }
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
#ifndef BFETCH_EMPTY_TAKE_LEGACY
    /* HTTP success does not require payload bytes. The response parser leaves
     * body=NULL for Content-Length:0; the old pointer test below turned a
     * completed empty script/style/document into a fetch failure (native
     * script-resource-events guest: empty:error after HTTP 200). Preserve the
     * take API's owned, NUL-terminated buffer contract even for zero bytes. */
    if(r&&r->state==RQ_DONE&&!r->body&&r->blen==0) {
        r->body=malloc(1);
        if(r->body)r->body[0]=0;
    }
#endif
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
        if (id >= 0) {
            if (bfetch_state(id) != BF_PENDING) {
                /* Only NAVIGATION documents stamp here: bfetch_sync() also
                 * waits on subresource ids (module loads, the image decode
                 * path), and those would double-stamp every batch. The nav
                 * flag is the request-table's own classification, not a
                 * guess from the call site. Status and byte count ride the
                 * line: "fetch done" without "what arrived" is not a phase
                 * boundary anyone can use. */
                struct breq *r = req_of(id);
                if (r && r->nav) {
                    unsigned long long now = monotonic_ms();  /* once: two reads could straddle a tick */
                    printf("[wa] t=%llu (+%llu) docdone status=%d len=%d\n",
                           now, now - wa_last_stamp, r->status, r->blen);
                    wa_last_stamp = now;
                }
                return;
            }
        }
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
    /* THE LOAD-END STAMP. browser.c calls this exactly once per load, on the
     * line before it prints "load done" -- i.e. after the last script ran,
     * with the whole load behind it. That makes it the one point where a
     * boot-clock timestamp of "open finished" exists in THIS file, and the
     * [wa] nav stamp ~5 s earlier is its partner: the pair brackets a whole
     * navigation in ONE clock (the guest's monotonic ms), which is what the
     * webaccel gate divides. The page's own performance.now() stamps cannot
     * serve: their epoch is js_page_open (js_page.c's g_t0), mid-load, so a
     * page stamp minus a nav stamp is not a duration at all -- the first
     * baseline run computed a NEGATIVE open time from exactly that mistake.
     * wacache counters ride along because "hits" is the number the whole
     * cache is for. */
    {
        int ents = 0, bytes = 0, hits = 0, rvs = 0;
        wacache_stats(&ents, &bytes, &hits, &rvs);
        if (wa_saw_nav)
            printf("[wa] t=%llu loadend reqs=%d dials=%d reuses=%d cache ents=%d "
                   "%dK hits=%d rvs=%d\n", monotonic_ms(), g_reqs, g_dials,
                   g_reuses, ents, bytes / 1024, hits, rvs);
    }
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

void bfetch_set_bypass(int on) { g_wa_bypass = on ? 1 : 0; }
void bfetch_http_cache_stats(int *entries, int *bytes, int *hits, int *revalidations)
{ wacache_stats(entries, bytes, hits, revalidations); }
void bfetch_http_cache_clear(void) { wacache_reset(); }

void bfetch_close_all(void)
{
    for (int i = 0; i < BF_NREQ; i++) if (g_req[i].state != RQ_FREE) bfetch_release(i);
    /* Healthy h2 sessions now deliberately survive their last resource handle.
     * Navigation/shutdown is the explicit lifetime boundary, so close those
     * sessions here BEFORE asking the pool to close what remains. bxfer's drop
     * path removes its own slot; hpool_close_all then sees only plain h1 fds. */
    bxfer_close_all();
    if (g_pool_ready) hpool_close_all(&g_pool);
}

/* ---- the prefetch cache ---- */

#define BF_NCACHE 32
#define BF_CACHE_QUEUED (-2)

struct bcache {
    char  url[BF_URLMAX];
    int   id;                        /* request handle, -1 terminal, -2 queued */
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
    struct url parsed;
    if (url_parse(abs, &parsed) != 0) return;
    for (int i = 0; i < BF_NCACHE; i++)
        if (g_cache[i].used && strcmp(g_cache[i].url, abs) == 0) return;   /* already have it */
    for (int i = 0; i < BF_NCACHE; i++) {
        if (g_cache[i].used) continue;
        int id = bfetch_start(abs);
#ifdef BFETCH_DROP_BUSY_PREFETCH
        if (id < 0) return;                      /* table full: the caller falls back to sync */
#else
        /* Request handles (16) and retained prefetch slots (32) are different
         * resources. Dropping an offer while all handles were busy prefetched
         * only 16 of the 24 URLs in the real HTTP/2 queue test; the tail fetched
         * synchronously later. Retain its URL and admit it as bodies release
         * handles, without increasing connection or body-memory limits. */
        if (id < 0) id = BF_CACHE_QUEUED;
#endif
        memcpy(g_cache[i].url, abs, strlen(abs) + 1);
        g_cache[i].id = id;
        g_cache[i].data = 0; g_cache[i].len = 0;
        g_cache[i].used = 1;
        return;
    }
}

void bfetch_prefetch_wait(void)
{
    for (;;) {
        int live = 0, deferred = 0, completed = 0;
        bfetch_pump();
        for (int i = 0; i < BF_NCACHE; i++) {
            struct bcache *e = &g_cache[i];
            if (!e->used) continue;
            if (e->id == BF_CACHE_QUEUED) {
                int id = bfetch_start(e->url);
                if (id < 0) { deferred++; continue; }
                e->id = id;
            }
            if (e->id < 0) continue;
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
                completed++;
                continue;
            }
            if (st == BF_DONE && bfetch_status(e->id) / 100 == 2) {
                e->len = bfetch_take(e->id, &e->data);
                if (e->len < 0) { e->len = 0; e->data = 0; }
            } else {
                bfetch_release(e->id);
            }
            e->id = -1;
            completed++;
        }
        /* A completed later slot can unblock an earlier queued slot on the
         * next pass. If NO prefetch owns a live handle and none was released,
         * the handles belong to other consumers: waiting here would deadlock
         * their caller. Leave the deferred URLs for a subsequent wait/take. */
        if (!live && deferred && completed) continue;
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
/* Concurrent exchanges. Was 12 when fetch() (WF_MAX = 8) was the only
 * consumer; bfetch's dial path went through bxfer_open() too (2026-09-09,
 * page loading over h2), and a page loading RES_INFLIGHT = 8 subresources
 * while its script runs 8 fetch()es needs 16 bindings at once -- at 12 the
 * 13th exchange took H1_E_NOMEM and the sub-resource failed as "could not
 * build request". 24 = both tables' worth of headroom. */
#define BX_MAXBIND 24           /* concurrent exchanges (bfetch 16 + fetch 8) */
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
    int      invalidate_target; /* unsafe/unknown method, until final headers */
    struct url target;         /* wire request, before redirect method changes */
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

/* Free protocol state without touching the socket or its pool slot.  There are
 * two owners at teardown: bx_sess_drop when an active session fails, and the
 * pool closer when an idle session expires/is evicted. Keeping the diagnostic
 * and h2_conn_free here makes those two doors byte-for-byte equivalent; doing
 * half at either door leaves an HPACK table alive beside a closed/reused fd. */
static void bx_sess_forget(struct bx_sess *ss)
{
    if (!ss || !ss->used) return;
    if (ss->h2_live) {
        /* The line the device harness greps for. A multiplexed connection that
         * silently serialised would print streams=N peak=1, which is the whole
         * difference and is invisible from anywhere above this. */
        printf("[bxfer] h2 %s:%d closed: streams=%d peak=%d frames=%d/%d hpack_in=%lld\n",
               ss->host, ss->port, ss->c.streams_opened, ss->c.max_concurrent_seen,
               ss->c.frames_in, ss->c.frames_out, (long long)ss->c.bytes_in);
        /* Counts alone cannot distinguish a peer GOAWAY from a local parser
         * failure. Preserve the first error before free clears the session;
         * the flag matters because GOAWAY code 0 is a real received message. */
        printf("[bxfer] h2 close-state state=%d error=%d sent_goaway=%u recv_goaway=%u received=%d last_stream=%u\n",
               ss->c.state,ss->c.err,ss->c.sent_goaway,ss->c.recv_goaway,
               ss->c.have_recv_goaway,ss->c.peer_last_stream);
        h2_conn_free(&ss->c);
    }
    memset(ss, 0, sizeof *ss);
}

/* hpool's closer calls this before sock_close.  It is intentionally keyed by
 * fd rather than by the opaque ctx: the pool also holds pre-bxfer h1 entries
 * with ctx == NULL, and fd is already the one identity shared by both tables. */
static void bxfer_pool_closed(int fd)
{
    struct bx_sess *ss = bx_sess_of(fd);
    if (ss) bx_sess_forget(ss);
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
    /* If the pool owns the fd, let its ONE closer close the socket and call
     * bxfer_pool_closed to free this session.  Closing here as well is not just
     * redundant: descriptors are small integers, so a double close after the
     * first one is recycled can terminate an unrelated connection silently. */
    if (ss->pslot >= 0 && hpool_fd(&g_pool, ss->pslot) == ss->fd) {
        int slot = ss->pslot;
        hpool_drop(&g_pool, slot);
        return;
    }
    int fd = ss->fd;
    bx_sess_forget(ss);
    if (fd >= 0) sock_close(fd);
}

/* Borrow one stream from an already-negotiated HTTP/2 session.  This function
 * is deliberately JOIN-ONLY: callers that are subject to hpool's connection
 * cap can try it before hpool_may_open, then ask to dial only after it fails.
 * Returning an unpooled session would bypass both max_streams and idle expiry,
 * so such a session is left to its existing direct caller and is not reused. */
static int bx_host_equal(const char *a, const char *b)
{
#ifdef BXFER_CASE_SENSITIVE_HOST
    return strcmp(a,b)==0;
#else
    /* Match the origin key used by hpool. Case-sensitive session matching
     * next to case-insensitive admission silently queued mixed-case hosts
     * until the pool's 60-second expiry. Ports and TLS remain exact. */
    while (*a && *b) {
        int x=(unsigned char)*a++, y=(unsigned char)*b++;
        if(x>='A'&&x<='Z')x+='a'-'A';
        if(y>='A'&&y<='Z')y+='a'-'A';
        if(x!=y)return 0;
    }
    return !*a && !*b;
#endif
}

static int bxfer_join_ready_h2(const char *host, int port, int tls, int wait_slot)
{
#ifndef BXFER_H1_ONLY
    if (!tls || !host || !host[0]) return -1;
    for (int i = 0; i < BX_MAXSESS; i++) {
        struct bx_sess *ss = &g_sess[i];
        if (!ss->used || ss->proto != HP_PROTO_H2 || ss->pslot < 0 ||
            ss->port != port || ss->tls != tls || !bx_host_equal(ss->host, host))
            continue;
        int bits = sock_poll(ss->fd);
        /* Read idle control frames before admitting HEADERS: a queued GOAWAY
         * is otherwise discovered only after sending the next request. Active
         * sessions are pumped by their owners, so their delivery stays there. */
        if (ss->refs == 0 && bits >= 0 && (bits & SOCK_P_READABLE))
            h2_conn_pump(&ss->c, (int64_t)monotonic_ms());
        if (!h2_conn_usable(&ss->c) || bits < 0 || (bits & (SOCK_P_EOF | SOCK_P_ERROR))) {
#ifndef BXFER_NO_DRAINING
            hpool_retire(&g_pool, ss->pslot);
#endif
            /* Previously this dropped even refs>0. Bindings still contain
             * the session index and callers still hold its fd: recycling
             * either here makes their later free/close hit the replacement.
             * Retain failed/draining sessions until the last owner releases. */
#ifdef BXFER_DROP_ACTIVE_SESSION
            bx_sess_drop(ss);
#else
            if (ss->refs == 0) bx_sess_drop(ss);
#endif
            continue;
        }
#ifndef BXFER_IGNORE_H2_STREAM_CAP
        /* A live h2 connection is not necessarily able to accept a stream in
         * this frame.  In particular, SETTINGS_MAX_CONCURRENT_STREAMS=1 is
         * common on throttled endpoints.  Reserving hpool/ref counts first and
         * discovering H2_E_NOSLOT in bx_start_h2 used to turn back-pressure
         * into a failed image/script request.  Leave it queued; the next pump
         * retries as soon as a stream completes.  The named macro is the
         * watched-red version of that bug in h2mux_test. */
        if (!wait_slot && !h2_conn_stream_available(&ss->c)) continue;
#endif
        int slot = hpool_acquire_mux(&g_pool, host, port, tls,
                                     (int64_t)monotonic_ms());
        if (slot < 0) continue;
        /* hpool's contract is one h2 connection per origin, but assert the
         * identity at the consumer boundary: if that invariant ever changes,
         * incrementing this session's refs for another session's pool stream
         * would corrupt both lifetimes. Give the mistaken stream straight back. */
        if (slot != ss->pslot || hpool_fd(&g_pool, slot) != ss->fd) {
            hpool_release(&g_pool, slot, 1, (int64_t)monotonic_ms());
            continue;
        }
        ss->refs++;
        return ss->fd;
    }
#else
    (void)host; (void)port; (void)tls; (void)wait_slot;
#endif
    return -1;
}

/* Shared with direct bxfer callers: one pending-origin reservation policy.
 * The capped resource loader additionally requires an actual pool slot, so
 * this join-only path cannot admit an unpooled connection behind its budget. */
static int bxfer_join_pending(const char *host, int port, int tls, int pooled)
{
#ifndef BXFER_H1_ONLY
    if (tls && host && host[0]) {
        for (int i = 0; i < BX_MAXSESS; i++) {
            struct bx_sess *ss = &g_sess[i];
            if (!ss->used || ss->refs <= 0 || ss->proto != HP_PROTO_PENDING ||
                ss->refs >= g_pool.max_streams || ss->port != port || ss->tls != tls ||
                (pooled && ss->pslot < 0)) continue;
            if (!bx_host_equal(ss->host, host)) continue;
            int bits = sock_poll(ss->fd);
            if (bits < 0 || (bits & (SOCK_P_EOF | SOCK_P_ERROR))) continue;
            ss->refs++;
            return ss->fd;
        }
    }
#else
    (void)host; (void)port; (void)tls; (void)pooled;
#endif
    return -1;
}

static int bxfer_h1_busy(int fd)
{
    struct bx_sess *ss = bx_sess_of(fd);
    return ss && ss->proto == HP_PROTO_H1 && ss->h1_taken;
}

void bxfer_close_all(void)
{
    for (int i = 0; i < BX_MAXBIND; i++) memset(&g_bind[i], 0, sizeof g_bind[i]);
    for (int i = 0; i < BX_MAXSESS; i++) bx_sess_drop(&g_sess[i]);
}

static int bxfer_dial(const char *host,int port,int tls,int h1_only,int *fresh);

int bxfer_open(const char *host, int port, int tls)
{
    return bxfer_open_ex(host, port, tls, 0);
}

/* Same, telling the caller whether it DIALLED (fresh) or JOINED an existing
 * or in-flight session. bfetch needs the distinction for keep-alive: only a
 * FRESH HTTP/1.1 dial may be yielded to the idle pool (bxfer_h1_yield),
 * because a joined handle is one ref among several and its "finish" is not
 * the connection's finish -- see req_drop_conn(). */
int bxfer_open_ex(const char *host, int port, int tls, int *fresh)
{
    if (fresh) *fresh = 0;
    if (!g_pool_ready) bfetch_init();
    if (!host || !host[0]) return -1;

#ifndef BXFER_H1_ONLY
    /* Join an existing connection to this origin if it is speaking HTTP/2. */
    /* Direct callers have no bfetch RQ_INIT queue: reserve a bounded handle
     * even if the peer's stream cap is full, then sid=0 waits in bxfer_pump. */
    int joined = bxfer_join_ready_h2(host, port, tls, 1);
    if (joined >= 0) return joined;

    /* Or join one whose handshake has not reported yet and still might. This
     * case
     * is the one that matters: N fetches start in the same frame, long before
     * any ALPN answer exists, and a client that dials on each of them has six
     * connections to an origin that wanted one. The cost is a fallback if the
     * guess is wrong, which bxfer_start implements. Pending joins are bounded
     * by the SAME max_streams that will apply if ALPN says h2; otherwise 24
     * callers could reserve refs which a 16-stream pool can never account. */
    int pending = bxfer_join_pending(host, port, tls, 0);
    if (pending >= 0) return pending;
#endif

    return bxfer_dial(host,port,tls,0,fresh);
}

static int bxfer_dial(const char *host,int port,int tls,int h1_only,int *fresh)
{
    struct bx_sess *ss = 0;
    for (int i = 0; i < BX_MAXSESS; i++) if (!g_sess[i].used) { ss = &g_sess[i]; break; }
    if (!ss) return -1;

    int flags = 0;
    if (tls) {
        flags = SOCK_F_TLS | SOCK_F_ALPN_HTTP11;
#ifndef BXFER_H1_ONLY
        if (!h1_only) flags |= SOCK_F_ALPN_H2;
#endif
    }
    int fd = sock_open(host, port, flags);
    if (fd < 0) return -1;
    if (fresh) *fresh = 1;

    memset(ss, 0, sizeof *ss);
    ss->used = 1;
    ss->fd = fd;
    ss->port = port;
    ss->tls = tls;
    ss->refs = 1;
    /* Not yet known, and it cannot be until the handshake finishes. A plain
     * http:// socket has no ALPN at all, so it is HTTP/1.1 by construction. */
    ss->proto = tls && !h1_only ? HP_PROTO_PENDING : HP_PROTO_H1;
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
    /* refs == 0 is a healthy IDLE h2 session, not a caller handle. A duplicate
     * close must not decrement its pool stream count or silently defeat idle
     * reuse; bxfer_close_all/bx_sess_drop are the explicit session doors. */
    if (ss->refs <= 0) return;
    ss->refs--;
    if (ss->proto == HP_PROTO_H2) {
        if (ss->pslot >= 0)
            hpool_release(&g_pool, ss->pslot, 1, (int64_t)monotonic_ms());
        if (ss->refs > 0) return;

#ifndef BXFER_DROP_IDLE_H2
        /* The request handle ended; the origin connection did not.  Keep a
         * healthy, pooled h2 session until the 60 s pool timeout, eviction, or
         * navigation. Pre-fix measurement: two serial bfetch resources opened
         * 2 TLS connections and left no session; now the server sees 2 streams
         * on 1 connection. The named macro is the watched-red old behaviour. */
        int bits = sock_poll(ss->fd);
        if (ss->pslot >= 0 && h2_conn_usable(&ss->c) && bits >= 0 &&
            !(bits & (SOCK_P_EOF | SOCK_P_ERROR)))
            return;
#endif
        bx_sess_drop(ss);
        return;
    }
    if (ss->refs > 0) {
        /* Other streams are still running on this connection: the socket is
         * emphatically not ours to close. This is the line that separates a
         * multiplexed connection from six serialised ones.
         *
         * THE RELEASE IS H2-ONLY. Only an h2 connection's pool slot is
         * stream-counted. A PENDING or H1 session's extra refs are
         * speculative JOINS that re-dialled their own connection when ALPN
         * landed http/1.1 (bxfer_start's h1_taken case): they never held
         * THIS socket, and releasing its slot here would set in_use = 0
         * while the dialer's HTTP/1.1 exchange is still mid-flight -- the
         * pool would hand the live socket to a second request and the two
         * responses would interleave on one stream. That is not a theory:
         * on 2026-09-09, bfetch's dial went through bxfer_open and
         * deepseek's CSS died exactly there (css__stylesheet_add_rule
         * walking response bytes as pointers, guest core; bisected to this
         * line via yield-disabled and H1-dial-only builds). js_webapi's
         * fetch() carries the same latent shape whenever two fetches share
         * a pending connection that lands H1, so the guard covers both
         * callers. */
        return;
    }
    bx_sess_drop(ss);
}

int bxfer_h1_yield(int fd, int *out_pslot)
{
    struct bx_sess *ss = bx_sess_of(fd);
    /* h2_live covers PENDING conns that never resolved: their ALPN answer
     * never came, so "finished HTTP/1.1" is not a claim this caller can
     * make about them. Only a resolved H1 session yields. */
    if (!ss || ss->proto != HP_PROTO_H1 || ss->h2_live) return -1;
    if (ss->refs != 1) return -1;   /* someone else still holds this handle */
    int pslot = ss->pslot;
    ss->pslot = -1;                 /* the slot survives; the entry does not */
    memset(ss, 0, sizeof *ss);      /* fd stays open, owned by the slot now */
    if (out_pslot) *out_pslot = pslot;
    return fd;
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
    printf("[bxfer] ALPN %s:%d fd=%d selected=%s\n",ss->host,ss->port,ss->fd,n?a:"(none)");
    ss->proto = (n == 2 && a[0] == 'h' && a[1] == '2') ? HP_PROTO_H2 : HP_PROTO_H1;
    if (ss->pslot >= 0) {
        hpool_set_proto(&g_pool, ss->pslot, ss->proto);
        /* hpool_set_proto accounts the dialler as stream one. Every other ref
         * joined while ALPN was pending, before hpool could call it an h2
         * stream, so materialise those reservations now. bxfer_open_ex bounds
         * refs by max_streams; with one pending connection per origin these
         * acquisitions cannot select another slot. If the pool invariant ever
         * changes, the mismatch is printed rather than silently undercounted. */
        if (ss->proto == HP_PROTO_H2) {
            for (int i = 1; i < ss->refs; i++) {
                int slot = hpool_acquire_mux(&g_pool, ss->host, ss->port, ss->tls,
                                             (int64_t)monotonic_ms());
                if (slot != ss->pslot) {
                    if (slot >= 0)
                        hpool_release(&g_pool, slot, 1, (int64_t)monotonic_ms());
                    printf("[bxfer] pending h2 stream accounting refused at %d/%d\n",
                           i + 1, ss->refs);
                    break;
                }
            }
        }
    }
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

    /* :path needs a temporary terminator; restore it even on NOSLOT because
     * a deferred request must be parsed again when peer capacity returns. */
    ((char *)path)[pl] = 0;

    int id = h2_request(&ss->c, method, tls ? "https" : "http", authority, path,
                        &extra, blen ? body : 0, blen);
    ((char *)path)[pl] = ' ';
    hpack_list_free(&extra);
#ifndef BXFER_IGNORE_H2_STREAM_CAP
    /* Speculative handles may have joined before ALPN/SETTINGS reported a
     * peer cap of one. Retain the serialized request as sid=0 and retry from
     * the ordinary pump. No HEADERS/body bytes have been submitted, so this
     * is admission back-pressure, not replay of an already-sent request. */
    if (id == H2_E_NOSLOT) { b->sess=(int)(ss-g_sess); return H1_OK; }
#endif
    if (id < 0) return (id == H2_E_NOSLOT || id == H2_E_GOAWAY) ? H1_E_TRANSPORT : H1_E_NOMEM;

    b->sess = (int)(ss - g_sess);
    b->sid = (uint32_t)id;
    b->hdr_done = 0;
    h2_stream_limit(&ss->c, b->sid, BX_H2_BODY_MAX);

    g_bx_streams++;
    int live = h2_conn_active_streams(&ss->c);
    if (live > g_bx_peak) g_bx_peak = live;
    return H1_OK;
}

/* RFC 9111 4.4: successful unsafe requests invalidate their target, including
 * methods whose safety is unknown. Capture BEFORE bx_start_h2 edits the request
 * buffer and BEFORE fetch can rewrite POST to GET while following a redirect.
 * Known safe methods are case-sensitive HTTP tokens, not case-folded strings.
 * Targets beyond the GET transport's URL capacity cannot match one of its
 * complete cache keys; do not truncate them into a different resource. */
static void bx_capture_target(struct bx_bind *b, const char *req, int len,
                              const char *host, int port, int tls)
{
    int m = 0;
    while (m < len && req[m] != ' ' && req[m] != '\r' && req[m] != '\n') m++;
    if (m == len || req[m] != ' ') return;
    if ((m == 3 && !memcmp(req,"GET",3)) || (m == 4 && !memcmp(req,"HEAD",4)) ||
        (m == 7 && !memcmp(req,"OPTIONS",7)) || (m == 5 && !memcmp(req,"TRACE",5))) return;
    int start = m + 1, end = start;
    while (end < len && req[end] != ' ' && req[end] != '\r' && req[end] != '\n') end++;
    int n = end - start;
    if (end == len || req[end] != ' ' || n <= 0 || n >= URL_PATH_MAX || req[start] != '/') return;
    if (!host || !host[0] || strlen(host) >= URL_HOST_MAX || port <= 0 || port > 65535) return;
    b->target.https = tls ? 1 : 0;
    b->target.port = (uint16_t)port;
    memcpy(b->target.host,host,strlen(host)+1);
    memcpy(b->target.path,req+start,(size_t)n); b->target.path[n] = 0;
    b->invalidate_target = 1;
}

static int bx_same_target(const char *cached, void *opaque)
{
    const struct url *target = opaque;
    struct url u;
    if (url_parse(cached,&u) != 0 || u.https != target->https || u.port != target->port) return 0;
    for (int i = 0;; i++) {
        int a = (unsigned char)u.host[i], b = (unsigned char)target->host[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
        if (!a) break;
    }
    /* Fragments identify a position in the representation, never another HTTP
     * resource. Query bytes remain significant; do not invalidate neighbours. */
    int i = 0;
    while (u.path[i] && u.path[i] != '#' && target->path[i] && target->path[i] != '#') {
        if (u.path[i] != target->path[i]) return 0;
        i++;
    }
    return (!u.path[i] || u.path[i] == '#') && (!target->path[i] || target->path[i] == '#');
}

static void bx_invalidate_response(struct bx_bind *b, const struct h1_response *r)
{
    if (!b || !b->invalidate_target || !h1_response_headers_done(r) || r->code < 200) return;
    b->invalidate_target = 0; /* once per response, never evict a later GET again */
    if (r->code >= 400) return;
    /* The old policy had wacache_invalidate but no production consumer. This
     * runs on final HEADERS, not body completion: redirects are cancelled at
     * that boundary and streaming bodies may never finish. RFC 9111 4.4 bases
     * invalidation on a non-error status, even if later body decoding fails.
     * Location/Content-Location invalidation is optional and deliberately not
     * performed here, so a response cannot purge another origin's cache. */
#ifndef BXFER_NO_CACHE_INVALIDATE
    wacache_invalidate_matching(bx_same_target,&b->target);
#endif
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
    bx_capture_target(b,req,reqlen,host,port,tls);

    struct bx_sess *ss = bx_sess_of(*fd);
    if (ss) {
        bx_resolve_alpn(ss);
#ifndef BXFER_H1_ONLY
        if (ss->proto == HP_PROTO_H2) {
#ifndef BXFER_NO_PRESEND_REPLACE
            /* A handle can be borrowed in one JS callback and started much
             * later. GOAWAY may arrive in that gap (guest 2026-09-10: config
             * GET failed before HEADERS after 14.47 s of script work). Read
             * queued control frames before allocating this stream; pumping
             * may advance other streams but their owners still deliver them.
             *
             * Only this pre-send boundary can replace the fd: no stream id
             * or request bytes exist yet. Do NOT retry from the error path of
             * an allocated stream, even for a POST whose response is absent.
             * WAIT returns ownership of req to the caller and preserves its
             * existing deadline; a new TLS handshake must not be misread as
             * "ALPN absent, use H1" by resolving it synchronously here. */
            int bits=sock_poll(ss->fd);
            if (ss->h2_live && bits>=0 && (bits&SOCK_P_READABLE))
                h2_conn_pump(&ss->c,(int64_t)monotonic_ms());
            if (ss->h2_live && (!h2_conn_usable(&ss->c) || bits<0 ||
                               (bits&(SOCK_P_ERROR|SOCK_P_EOF)))) {
                if(ss->pslot>=0)hpool_retire(&g_pool,ss->pslot);
                int oldfd=*fd;
                int nfd=bxfer_open(host,port,tls);
                b->owner=0;
                if(nfd<0)return H1_E_TRANSPORT;
                printf("[bxfer] pre-send replacement %s:%d fd=%d -> %d\n",host,port,oldfd,nfd);
                bxfer_close(oldfd);
                *fd=nfd;
                return BXFER_START_WAIT;
            }
#endif
            memset(c, 0, sizeof *c);
            h1_response_init(&c->resp);
            c->out = req; c->out_len = reqlen; c->state = H1_C_SEND;
            int rc = bx_start_h2(ss, b, c, req, reqlen, host, port, tls);
            /* A binding describes an exchange, not a connection reservation.
             * h2_request may refuse before allocating a stream (NOSLOT,
             * GOAWAY, malformed request, allocation failure); retaining owner
             * in that case leaves target/invalidation state attached to an
             * exchange that never existed. */
            if (rc != H1_OK) b->owner = 0;
            return rc;
        }
#endif
        /* HTTP/1.1 after all. A connection carries one exchange at a time, so
         * a request that joined this socket speculatively has to be given its
         * own -- the alternative is two requests interleaved on one stream,
         * which is not a slow page, it is a corrupt one. */
        if (ss->h1_taken) {
            /* The old replacement offered H2 again, then merely labelled
             * its session H1. If this handshake selected H2, raw HTTP/1
             * bytes became invalid frame headers. An H1 fallback dial must
             * offer H1 only and admit the pool slot with the same protocol. */
            int nfd = bxfer_dial(host, port, tls, 1, 0);
            if (nfd < 0) { b->owner = 0; return H1_E_TRANSPORT; }
            bxfer_close(*fd);
            *fd = nfd;
            ss = bx_sess_of(nfd);
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

static int bx_take_headers(struct bx_bind *b, struct bx_sess *ss, struct h1_conn *c)
{
    struct h2_stream *s = h2_stream_get(&ss->c, b->sid);
    if (!s || !s->headers_done || b->hdr_done) return H1_OK;
    struct h1_response *r = &c->resp;

    r->code = s->status;
    r->minor = 1;
    for (int i = 0; i < s->hdr.n; i++) {
        const struct hpack_hdr *h = &s->hdr.v[i];
        if (h->nlen && h->name[0] == ':') continue;    /* pseudo-fields are framing */
        /* A missing policy header is not an absent policy. Allocation or
         * validation failure must end the exchange before headers/body are
         * exposed, otherwise CSP/XFO completeness would be falsely asserted. */
        int added=h1_headers_add(&r->hdr,h->name,h->nlen,h->value,h->vlen);
#ifndef BXFER_PARTIAL_HEADERS_OK
        if(added!=H1_OK)return added;
#else
        (void)added;
#endif
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
    /* First-byte accounting for the h2 path: http1.c counts wire bytes into
     * c->rx_bytes as it reads them, and bfetch's BF_FIRSTBYTE_MS deadline
     * kills an exchange whose rx_bytes is still zero after 12 s -- a rule
     * that was about to execute every h2 sub-resource load as a stall,
     * because nothing on this path ever touched rx_bytes. Headers HAVE
     * arrived here, which is the deadline's actual question ("has literally
     * anything come back?"), so count their bytes once, at the one instant
     * it cannot be counted twice (b->hdr_done gates re-entry). */
    c->rx_bytes += (unsigned long long)(r->hdr_bytes > 0 ? r->hdr_bytes : 1);
    return H1_OK;
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
        {
            printf("[bxfer] limit=buffered-body host=%s sid=%u buffered=%d incoming=%d cap=%d\n",
                   ss->host, b->sid, r->body_len, n, r->body_max);
            free(p); return H1_E_TOOLARGE;
        }
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

static int bxfer_pump_inner(struct h1_conn *c)
{
    struct bx_bind *b = bx_bind_of(c);
    if (!b || b->sess < 0) return h1_conn_pump(c);
    if (c->state == H1_C_DONE || c->state == H1_C_ERROR) return c->state;

    struct bx_sess *ss = &g_sess[b->sess];
    if (!ss->used || !ss->h2_live) { c->state = H1_C_ERROR; c->err = H1_E_TRANSPORT; return c->state; }

    /* Pumping the CONNECTION, not the stream: every stream on it advances,
     * whichever one the caller happens to be stepping. */
    h2_conn_pump(&ss->c, (int64_t)monotonic_ms());
#ifndef BXFER_NO_DRAINING
    if (!h2_conn_usable(&ss->c) && ss->pslot >= 0) hpool_retire(&g_pool, ss->pslot);
#endif
    if (!b->sid) {
        int rc=bx_start_h2(ss,b,c,c->out,c->out_len,ss->host,ss->port,ss->tls);
        if(rc!=H1_OK){c->state=H1_C_ERROR;c->err=rc;return c->state;}
        if(!b->sid)return c->state;
    }

    int had_headers = b->hdr_done;
    int headers_rc=bx_take_headers(b,ss,c);
    if(headers_rc!=H1_OK){c->state=H1_C_ERROR;c->err=headers_rc;return c->state;}
#ifndef BXFER_HEADERS_WITH_BODY
    /* Give a sink's caller the final headers before invoking its body sink.
     * js_webapi decides redirect/CORS/Response delivery AFTER this pump returns.
     * The old "one read fits the 4096-byte hold" claim was true for h1, but
     * h2 pumps 16384 bytes and can also accumulate another stream's DATA. A
     * legal HEADERS + 8192-byte DATA batch therefore failed with TOOLARGE before
     * Response existed (test-fetch-header-order: 5 failures before this yield).
     * Leave bytes in the h2 stream, rather than enlarging a second JS-side
     * queue. Cancellation releases them; the next caller-approved pump drains
     * them under the existing backpressure and content-encoding rules. Buffered
     * bfetch callers have no sink and keep their existing completion behavior. */
    if (!had_headers && b->hdr_done && c->resp.sink) {
        c->state = H1_C_RECV;
        return c->state;
    }
#endif
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

int bxfer_pump(struct h1_conn *c)
{
    int state = bxfer_pump_inner(c);
    bx_invalidate_response(bx_bind_of(c),&c->resp);
    return state;
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
