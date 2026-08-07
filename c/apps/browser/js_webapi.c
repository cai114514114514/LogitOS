/* The Web APIs that are not the DOM: fetch, XHR, Storage, history, location,
 * URL, URLSearchParams, matchMedia.  See js_webapi.h for the contract and for
 * what is deliberately absent.
 *
 * Two rules shaped this file.
 *
 * NOTHING BLOCKS.  A fetch is a non-blocking socket plus an h1_conn stepped
 * from js_webapi_pump(), which the browser's main loop calls once a frame.
 * Between two steps of a 200 KB download the loop still drains input, fires
 * timers and repaints.  That is measured, not asserted: `make test-fetch-ui`
 * injects real clicks while a page's fetch() is mid-transfer and times how
 * long each takes to reach a ring-3 app.
 *
 * THE OBJECT PLUMBING IS IN JAVASCRIPT.  Headers, Response, XMLHttpRequest,
 * URL, URLSearchParams and MediaQueryList are defined by a prelude evaluated
 * at install time, which C hands four primitives to: __fetchStart (open a
 * request, get a Promise), __urlParse (c/net/http/url.c, the ONE URL parser in
 * this tree), __utf8 (bytes -> string) and __mediaMatch.  Writing Headers as
 * 200 lines of JS_SetPropertyStr would not have made it more correct, only
 * longer -- and every one of those classes is pure string shuffling, which is
 * exactly what a JS engine is for.  What stays in C is what JS cannot hold:
 * the sockets, and the state that has to SURVIVE the runtime (Storage).
 */

#include "quickjs.h"
#include "js_webapi.h"
#include "http1.h"
#include "url.h"
#include "logit_abi.h"          /* SOCK_F_* / SOCK_P_* -- pure #defines */
#include <string.h>
#include <stdlib.h>

#ifndef WEBAPI_HOST
#include "logit.h"              /* sock_* + monotonic_ms; ring 3 only */
#endif

int printf(const char *, ...);

#define WURL_MAX   2048         /* an absolute URL we are willing to carry */
#define WQ_MAX     1024         /* ?query -- deliberately NOT url.h's 512, see wurl_parse */
#define WH_MAX      256         /* #fragment */

/* ---- the transport ---------------------------------------------------- */

#ifndef WEBAPI_HOST
static int  d_open(const char *h, int p, int tls)
{ return sock_open(h, p, tls ? (SOCK_F_TLS | SOCK_F_ALPN_HTTP11) : 0); }
static int  d_poll(int fd) { return sock_poll(fd); }
static int  d_send(int fd, const void *b, int n) { return sock_send(fd, b, n); }
static int  d_recv(int fd, void *b, int n) { return sock_recv(fd, b, n); }
static void d_close(int fd) { sock_close(fd); }
static unsigned long long d_now(void) { return monotonic_ms(); }
static const struct webapi_net g_default_net = { d_open, d_poll, d_send, d_recv, d_close, d_now };
#else
static const struct webapi_net g_default_net = { 0, 0, 0, 0, 0, 0 };
#endif

static const struct webapi_net *g_net = &g_default_net;
void js_webapi_set_net(const struct webapi_net *n) { g_net = n ? n : &g_default_net; }
static unsigned long long now_ms(void) { return g_net && g_net->now_ms ? g_net->now_ms() : 0; }

/* ---- URLs -------------------------------------------------------------
 * Parsing goes through c/net/http/url.c, which is the parser the loader and
 * the resource fetcher already use -- a second one here would mean the address
 * bar and fetch() could disagree about what a URL means.
 *
 * Two things it does not do, done here instead:
 *   - it keeps the query and fragment inside `path`, bounded by URL_PATH_MAX
 *     (512).  An SPA's query string is routinely longer than its path, so the
 *     query is split off BEFORE parsing and carried separately; only the path
 *     is subject to url.c's bound.
 *   - a "?x=1" or "#frag" reference must keep the base's path.  url_resolve
 *     treats both as ordinary relative references and would replace the last
 *     path segment with them. */

struct wurl {
    int  https;
    char host[URL_HOST_MAX];
    int  port;
    char pathname[URL_PATH_MAX];
    char search[WQ_MAX];                /* "" or "?..." */
    char hash[WH_MAX];                  /* "" or "#..." */
};

static void scopy(char *d, const char *s, int max)
{ int i = 0; if (max <= 0) return; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

static int has_scheme(const char *s)
{ return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0; }

/* RFC 3986 5.2.4, which url.c's url_resolve does not do: collapse "." and ".."
 * segments. Bundlers emit "./chunk-4f2.js" and "../assets/x" constantly, and
 * without this they become literal path segments and the server answers 404 --
 * a failure that looks like a network bug and is not one. */
static void remove_dot_segments(char *p)
{
    char out[URL_PATH_MAX];
    int o = 0;
    const char *s = p;
    if (*s != '/') return;                       /* only absolute paths get here */
    while (*s == '/') {
        const char *seg = s + 1;
        int n = 0;
        while (seg[n] && seg[n] != '/') n++;
        int last = seg[n] == 0;
        if (n == 1 && seg[0] == '.') {
            if (last && o < URL_PATH_MAX - 1) out[o++] = '/';
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                      /* drop the '/' that led it */
            if (last && o < URL_PATH_MAX - 1) out[o++] = '/';
        } else {
            if (o < URL_PATH_MAX - 1) out[o++] = '/';
            for (int i = 0; i < n && o < URL_PATH_MAX - 1; i++) out[o++] = seg[i];
        }
        s = seg + n;
    }
    for (; *s && o < URL_PATH_MAX - 1; s++) out[o++] = *s;
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    memcpy(p, out, (size_t)o + 1);
}

static int wurl_parse(const char *in, const struct wurl *base, struct wurl *out)
{
    if (!in || !out) return -1;
    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') in++;

    if (base && in[0] == '#') {          /* fragment-only: everything else is the base's */
        *out = *base; scopy(out->hash, in, WH_MAX); return 0;
    }
    if (base && in[0] == '?') {          /* query-only: keeps the path, drops the fragment */
        *out = *base; out->hash[0] = 0;
        const char *h = strchr(in, '#');
        int n = h ? (int)(h - in) : (int)strlen(in);
        if (n >= WQ_MAX) n = WQ_MAX - 1;
        memcpy(out->search, in, (size_t)n); out->search[n] = 0;
        if (h) scopy(out->hash, h, WH_MAX);
        return 0;
    }
    if (base && in[0] == 0) { *out = *base; return 0; }

    /* Split off ?query and #fragment; only the path part goes to url.c. */
    char pathpart[URL_PATH_MAX + URL_HOST_MAX + 16];
    char query[WQ_MAX], frag[WH_MAX];
    query[0] = frag[0] = 0;
    {
        const char *q = 0, *f = 0;
        for (const char *p = in; *p; p++) {
            if (*p == '?' && !q && !f) q = p;
            else if (*p == '#' && !f) { f = p; break; }
        }
        const char *pend = q ? q : (f ? f : in + strlen(in));
        int n = (int)(pend - in);
        if (n >= (int)sizeof pathpart) return -1;
        memcpy(pathpart, in, (size_t)n); pathpart[n] = 0;
        if (q) {
            const char *qend = f ? f : in + strlen(in);
            int qn = (int)(qend - q);
            if (qn >= WQ_MAX) qn = WQ_MAX - 1;
            memcpy(query, q, (size_t)qn); query[qn] = 0;
        }
        if (f) scopy(frag, f, WH_MAX);
    }

    char abs[WURL_MAX];
    if (has_scheme(pathpart)) {
        scopy(abs, pathpart, (int)sizeof abs);
    } else {
        if (!base) return -1;
        struct url b;
        b.https = base->https; b.port = (uint16_t)base->port;
        scopy(b.host, base->host, URL_HOST_MAX);
        scopy(b.path, base->pathname, URL_PATH_MAX);      /* no query: see above */
        if (url_resolve(&b, pathpart[0] ? pathpart : base->pathname, abs, (int)sizeof abs) != 0)
            return -1;
    }

    struct url u;
    if (url_parse(abs, &u) != 0) return -1;
    out->https = u.https;
    out->port  = u.port;
    scopy(out->host, u.host, URL_HOST_MAX);
    scopy(out->pathname, u.path, URL_PATH_MAX);
    /* url_parse leaves an in-band query in path when the caller passed one --
     * it cannot happen here (we split first), but strip defensively. */
    { char *q = strchr(out->pathname, '?'); if (q) *q = 0; }
    remove_dot_segments(out->pathname);
    scopy(out->search, query, WQ_MAX);
    scopy(out->hash, frag, WH_MAX);
    return 0;
}

static int default_port(const struct wurl *u) { return u->https ? 443 : 80; }

static void num_str(int v, char *out)
{
    char t[12]; int n = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* scheme://host[:port] -- the origin, and the prefix of every href. */
static void wurl_origin(const struct wurl *u, char *out, int max)
{
    int o = 0;
    const char *s = u->https ? "https://" : "http://";
    for (; *s && o < max - 1; s++) out[o++] = *s;
    for (const char *p = u->host; *p && o < max - 1; p++) out[o++] = *p;
    if (u->port != default_port(u)) {
        char pn[12]; num_str(u->port, pn);
        if (o < max - 1) out[o++] = ':';
        for (const char *p = pn; *p && o < max - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}

static void wurl_href(const struct wurl *u, char *out, int max)
{
    wurl_origin(u, out, max);
    int o = (int)strlen(out);
    for (const char *p = u->pathname; *p && o < max - 1; p++) out[o++] = *p;
    for (const char *p = u->search;   *p && o < max - 1; p++) out[o++] = *p;
    for (const char *p = u->hash;     *p && o < max - 1; p++) out[o++] = *p;
    out[o] = 0;
}

/* The request-target: path + query, never the fragment (which is client-side
 * only and must not leave the machine). */
static void wurl_target(const struct wurl *u, char *out, int max)
{
    int o = 0;
    for (const char *p = u->pathname; *p && o < max - 1; p++) out[o++] = *p;
    if (o == 0 && max > 1) out[o++] = '/';
    for (const char *p = u->search; *p && o < max - 1; p++) out[o++] = *p;
    out[o] = 0;
}

/* ---- the document's location ------------------------------------------ */

static struct wurl g_loc;                    /* the current document's URL */
static int  g_loc_valid;
static char g_loc_raw[WURL_MAX] = "about:blank";   /* what we were handed, parseable or not */
static char g_pending_nav[WURL_MAX];
static int  g_have_pending_nav;

static void set_location(const char *url)
{
    scopy(g_loc_raw, url && *url ? url : "about:blank", WURL_MAX);
    g_loc_valid = (wurl_parse(g_loc_raw, 0, &g_loc) == 0);
    if (!g_loc_valid) memset(&g_loc, 0, sizeof g_loc);
}

int js_webapi_take_navigation(char *out, int max)
{
    if (!g_have_pending_nav) return 0;
    scopy(out, g_pending_nav, max);
    g_have_pending_nav = 0;
    return 1;
}

/* ---- Storage ----------------------------------------------------------
 * Real Storage semantics, in memory, keyed by origin. It lives in C rather
 * than JS for exactly one reason: it must survive js_page_close(), so a page
 * that navigates and comes back finds what it wrote.
 *
 * NOT PERSISTED TO DISK. LogitFS has a known cross-boot write-durability bug
 * (see CLAUDE.md), and every test harness boots with -snapshot, so a disk
 * backing would be both unreliable and unverifiable. localStorage therefore
 * lives as long as the browser process does; sessionStorage is identical
 * today, and differs only in that it is documented to be per-tab. */

#define ST_ORIGINS   8
#define ST_ITEMS   256
#define ST_BYTES (256*1024)      /* per store, keys + values */

struct st_item { char *k, *v; };
struct store {
    char  origin[URL_HOST_MAX + 16];
    int   session;                       /* 0 = localStorage, 1 = sessionStorage */
    int   used;
    int   n;
    long  bytes;
    struct st_item v[ST_ITEMS];
};
static struct store g_stores[ST_ORIGINS * 2];

static struct store *store_for(const char *origin, int session)
{
    for (int i = 0; i < ST_ORIGINS * 2; i++)
        if (g_stores[i].used && g_stores[i].session == session &&
            strcmp(g_stores[i].origin, origin) == 0) return &g_stores[i];
    for (int i = 0; i < ST_ORIGINS * 2; i++)
        if (!g_stores[i].used) {
            g_stores[i].used = 1; g_stores[i].session = session;
            g_stores[i].n = 0; g_stores[i].bytes = 0;
            scopy(g_stores[i].origin, origin, (int)sizeof g_stores[i].origin);
            return &g_stores[i];
        }
    return 0;                            /* 9th origin in one session: no store */
}

static int store_find(struct store *s, const char *k)
{ for (int i = 0; i < s->n; i++) if (strcmp(s->v[i].k, k) == 0) return i; return -1; }

static void store_erase(struct store *s, int i)
{
    s->bytes -= (long)strlen(s->v[i].k) + (long)strlen(s->v[i].v);
    free(s->v[i].k); free(s->v[i].v);
    for (int j = i; j + 1 < s->n; j++) s->v[j] = s->v[j + 1];
    s->n--;
}

static char *dupstr(const char *s)
{ size_t n = strlen(s) + 1; char *p = (char *)malloc(n); if (p) memcpy(p, s, n); return p; }

/* 0 ok, -1 out of memory, -2 quota. */
static int store_set(struct store *s, const char *k, const char *v)
{
    int i = store_find(s, k);
    long delta = (long)strlen(v) - (i >= 0 ? (long)strlen(s->v[i].v) : -(long)strlen(k));
    if (s->bytes + delta > ST_BYTES) return -2;
    if (i < 0 && s->n >= ST_ITEMS) return -2;
    char *nv = dupstr(v);
    if (!nv) return -1;
    if (i >= 0) {
        s->bytes += (long)strlen(v) - (long)strlen(s->v[i].v);
        free(s->v[i].v); s->v[i].v = nv;
        return 0;
    }
    char *nk = dupstr(k);
    if (!nk) { free(nv); return -1; }
    s->v[s->n].k = nk; s->v[s->n].v = nv; s->n++;
    s->bytes += (long)strlen(k) + (long)strlen(v);
    return 0;
}

/* ---- history ----------------------------------------------------------
 * The SAME-DOCUMENT history: pushState/replaceState and the back/forward that
 * walks between those entries, which is how every SPA router works. Entry 0 is
 * the document as loaded.
 *
 * Going back PAST entry 0 would be a real navigation to the previous document,
 * which the browser's own history owns (browser.c hist_go) -- back() at entry 0
 * therefore does nothing here rather than pretending. State objects are held by
 * reference, not structured-cloned: a page that mutates an object it pushed
 * will see the mutation later. */

#define HIST_MAX 64
struct hentry { char url[WURL_MAX]; JSValue state; };
static struct hentry g_hist[HIST_MAX];
static int g_hist_n, g_hist_i;
/* Both events are QUEUED and fired from the pump, never inline: the spec makes
 * them tasks, and firing popstate from inside history.back() would re-enter the
 * page's own call stack at a point it cannot expect. */
static int g_popstate_queued;
static int g_hashchange_queued;
static JSValue g_popstate_state;
static char g_hash_old[WURL_MAX], g_hash_new[WURL_MAX];

/* ---- the JS-side hooks the prelude hands back ------------------------- */

static JSValue g_mk_response = JS_UNDEFINED;   /* (status,statusText,pairs,buf,url,redirected) */
static JSValue g_viewport_changed = JS_UNDEFINED;

static int g_vw = 1180, g_vh = 572;            /* browser.c: WINW, WINH-BARH-18 */

/* ---- fetch ------------------------------------------------------------ */

#define WF_MAX        8            /* concurrent requests; TCP has 32 slots total */
#define WF_HOPS       10
#define WF_TIMEOUT 30000ull        /* ms per hop, connect included */
#define WF_STEPS      8            /* h1_conn_pump calls per frame: 8 x 4 KiB */

enum { WF_FREE = 0, WF_DIAL, WF_XFER };

struct wfetch {
    int   state;
    int   fd;
    int   started;                 /* h1_conn_start has run */
    struct h1_conn conn;
    struct wurl url;
    char  method[H1_METHOD_MAX];
    struct h1_headers hdr;         /* caller headers, re-sent on each hop */
    char *body; int body_len;      /* request body, owned */
    int   hops, redirected;
    unsigned long long deadline;
    JSValue resolve, reject;
};
static struct wfetch g_fetch[WF_MAX];
static int g_fetch_live;

/* h1_transport over a socket handle. `ctx` is the handle, cast through
 * intptr, because a transport is a vtable + one word and a socket IS one
 * word. */
static int tr_read(void *c, void *buf, int len)
{
    int fd = (int)(long)c;
    int n = g_net->recv(fd, buf, len);
    if (n > 0) return n;
    if (n == 0) return H1_AGAIN;
    return H1_EOF;                 /* the kernel reports "finished or dead" as < 0 */
}
static int tr_write(void *c, const void *buf, int len)
{
    int fd = (int)(long)c;
    int n = g_net->send(fd, buf, len);
    if (n >= 0) return n;          /* 0 = queue full = H1_AGAIN, which is what h1 wants */
    return H1_TERR;
}

static void fetch_release(JSContext *ctx, struct wfetch *f)
{
    if (f->fd >= 0) { g_net->close(f->fd); f->fd = -1; }
    if (f->started) h1_conn_free(&f->conn);
    else free(f->conn.out);
    memset(&f->conn, 0, sizeof f->conn);
    f->started = 0;
    h1_headers_free(&f->hdr);
    free(f->body); f->body = 0; f->body_len = 0;
    if (ctx) {
        JS_FreeValue(ctx, f->resolve);
        JS_FreeValue(ctx, f->reject);
    }
    f->resolve = JS_UNDEFINED; f->reject = JS_UNDEFINED;
    f->state = WF_FREE;
    if (g_fetch_live > 0) g_fetch_live--;
}

static void fetch_reject(JSContext *ctx, struct wfetch *f, const char *msg)
{
    char full[256];
    int o = 0;
    const char *pre = "fetch: ";
    for (const char *p = pre; *p && o < (int)sizeof full - 1; p++) full[o++] = *p;
    for (const char *p = msg; *p && o < (int)sizeof full - 1; p++) full[o++] = *p;
    full[o] = 0;
    printf("[webapi] %s\n", full);
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "TypeError"));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, full));
    JSValue r = JS_Call(ctx, f->reject, JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    fetch_release(ctx, f);
}

/* Serialize the current request and hand it to a fresh h1_conn. */
static int fetch_send_request(JSContext *ctx, struct wfetch *f)
{
    (void)ctx;
    struct h1_request q;
    char target[URL_PATH_MAX + WQ_MAX];
    wurl_target(&f->url, target, (int)sizeof target);
    if (h1_request_init(&q, f->method, target) != H1_OK) return -1;

    char hostport[URL_HOST_MAX + 8];
    scopy(hostport, f->url.host, (int)sizeof hostport);
    if (f->url.port != default_port(&f->url)) {
        int o = (int)strlen(hostport); char pn[12]; num_str(f->url.port, pn);
        if (o < (int)sizeof hostport - 1) hostport[o++] = ':';
        for (const char *p = pn; *p && o < (int)sizeof hostport - 1; p++) hostport[o++] = *p;
        hostport[o] = 0;
    }
    h1_request_set_header(&q, "Host", hostport);
    h1_request_set_header(&q, "User-Agent", "Mozilla/5.0 (LogitOS) Logit/1.0");
    h1_request_set_header(&q, "Accept", "*/*");
    h1_request_set_header(&q, "Accept-Encoding", h1_accept_encoding());
    /* One socket per request -- there is no pool on this path yet, and a
     * connection the server keeps open would just sit in the table. */
    h1_request_set_header(&q, "Connection", "close");
    for (int i = 0; i < f->hdr.n; i++)
        h1_request_add_header(&q, f->hdr.v[i].name, f->hdr.v[i].value);
    if (f->body && f->body_len > 0) h1_request_set_body(&q, f->body, f->body_len);

    char *raw = 0; int rawlen = 0;
    int rc = h1_request_build(&q, &raw, &rawlen);
    h1_request_free(&q);
    if (rc != H1_OK || !raw) { free(raw); return -1; }

    struct h1_transport t = { tr_read, tr_write, 0, (void *)(long)f->fd };
    if (h1_conn_start(&f->conn, &t, raw, rawlen) != H1_OK) { free(raw); return -1; }
    f->started = 1;                       /* the conn owns `raw` from here */
    return 0;
}

/* Dial the socket for f->url and arm the deadline. 0 ok. */
static int fetch_dial(struct wfetch *f)
{
    if (!g_net || !g_net->open) return -1;
    f->fd = g_net->open(f->url.host, f->url.port, f->url.https);
    if (f->fd < 0) return -1;
    f->state = WF_DIAL;
    f->started = 0;
    f->deadline = now_ms() + WF_TIMEOUT;
    return 0;
}

/* Build the Response and settle the promise. 1 if a JS callback ran. */
static int fetch_resolve(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    if (h1_decode_body(r) != H1_OK) {
        fetch_reject(ctx, f, "response body could not be decoded (Content-Encoding)");
        return 1;
    }

    JSValue pairs = JS_NewArray(ctx);
    for (int i = 0; i < r->hdr.n; i++) {
        JSValue p = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, p, 0, JS_NewString(ctx, r->hdr.v[i].name));
        JS_SetPropertyUint32(ctx, p, 1, JS_NewString(ctx, r->hdr.v[i].value));
        JS_SetPropertyUint32(ctx, pairs, (uint32_t)i, p);
    }
    static const uint8_t empty[1] = { 0 };
    JSValue buf = JS_NewArrayBufferCopy(ctx, r->body && r->body_len > 0 ? r->body : empty,
                                        r->body_len > 0 ? (size_t)r->body_len : 0);
    char href[WURL_MAX];
    wurl_href(&f->url, href, (int)sizeof href);

    JSValue argv[6];
    argv[0] = JS_NewInt32(ctx, r->code);
    argv[1] = JS_NewString(ctx, r->reason);
    argv[2] = pairs;
    argv[3] = buf;
    argv[4] = JS_NewString(ctx, href);
    argv[5] = JS_NewBool(ctx, f->redirected);

    JSValue resp = JS_Call(ctx, g_mk_response, JS_UNDEFINED, 6, (JSValueConst *)argv);
    for (int i = 0; i < 6; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(resp)) {
        JS_FreeValue(ctx, resp);
        JSValue e = JS_GetException(ctx);
        JS_FreeValue(ctx, e);
        fetch_reject(ctx, f, "could not build the Response");
        return 1;
    }
    JSValue rv = JS_Call(ctx, f->resolve, JS_UNDEFINED, 1, (JSValueConst *)&resp);
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, resp);
    fetch_release(ctx, f);
    return 1;
}

/* A 3xx with a Location: re-target and re-dial. 1 if the request continues. */
static int fetch_redirect(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    const char *loc = h1_headers_get(&r->hdr, "location");
    if (!loc || !*loc || f->hops >= WF_HOPS) return 0;

    struct wurl next;
    if (wurl_parse(loc, &f->url, &next) != 0) return 0;

    char m[H1_METHOD_MAX]; int drop = 0;
    if (h1_redirect_method(r->code, f->method, m, (int)sizeof m, &drop) != H1_OK) return 0;
    scopy(f->method, m, H1_METHOD_MAX);
    if (drop) {
        free(f->body); f->body = 0; f->body_len = 0;
        h1_headers_remove(&f->hdr, "content-type");
        h1_headers_remove(&f->hdr, "content-length");
    }

    g_net->close(f->fd); f->fd = -1;
    h1_conn_free(&f->conn);
    memset(&f->conn, 0, sizeof f->conn);
    f->started = 0;
    f->url = next;
    f->hops++;
    f->redirected = 1;
    if (fetch_dial(f) != 0) { fetch_reject(ctx, f, "redirect target could not be opened"); return 1; }
    return 1;
}

/* One frame's work for one request. Returns 1 if a JS callback ran. */
static int fetch_step(JSContext *ctx, struct wfetch *f)
{
    if (now_ms() > f->deadline) { fetch_reject(ctx, f, "timed out"); return 1; }

    int bits = g_net->poll(f->fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) { fetch_reject(ctx, f, "connection failed"); return 1; }
    if (!(bits & SOCK_P_CONNECTED)) return 0;             /* DNS/TCP/TLS still running */

    if (!f->started) {
        if (fetch_send_request(ctx, f) != 0) { fetch_reject(ctx, f, "request could not be built"); return 1; }
        f->state = WF_XFER;
    }

    for (int i = 0; i < WF_STEPS; i++) {
        int st = h1_conn_pump(&f->conn);
        if (st == H1_C_ERROR) {
            fetch_reject(ctx, f, h1_strerror(f->conn.err));
            return 1;
        }
        if (st == H1_C_DONE) {
            if (h1_is_redirect(f->conn.resp.code) && fetch_redirect(ctx, f)) return 0;
            return fetch_resolve(ctx, f);
        }
    }
    return 0;
}

/* ---- the C primitives the prelude is handed -------------------------- */

/* JS_GetArrayBuffer THROWS when the value is not an ArrayBuffer, and both
 * callers here legitimately accept a string instead -- so the pending
 * exception has to be swallowed, or the next unrelated call inherits it. */
static uint8_t *ab_bytes(JSContext *ctx, JSValueConst v, size_t *len)
{
    uint8_t *p = JS_GetArrayBuffer(ctx, len, v);
    if (!p) JS_FreeValue(ctx, JS_GetException(ctx));
    return p;
}

/* __fetchStart(url, method, headerPairs, body) -> Promise */
static JSValue js_fetch_start(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) return promise;

    struct wfetch *f = 0;
    for (int i = 0; i < WF_MAX; i++) if (g_fetch[i].state == WF_FREE) { f = &g_fetch[i]; break; }

    const char *msg = 0;
    struct wurl u;
    const char *us = argc > 0 ? JS_ToCString(ctx, argv[0]) : 0;

    if (!f) msg = "too many requests in flight";
    else if (!us) msg = "no URL";
    else if (wurl_parse(us, g_loc_valid ? &g_loc : 0, &u) != 0) msg = "invalid URL";

    if (!msg) {
        memset(f, 0, sizeof *f);
        f->fd = -1;
        f->url = u;
        h1_headers_init(&f->hdr);
        scopy(f->method, "GET", H1_METHOD_MAX);
        if (argc > 1) {
            const char *m = JS_ToCString(ctx, argv[1]);
            if (m && *m) scopy(f->method, m, H1_METHOD_MAX);
            if (m) JS_FreeCString(ctx, m);
        }
        if (argc > 2 && JS_IsArray(ctx, argv[2])) {
            uint32_t n = 0;
            JSValue len = JS_GetPropertyStr(ctx, argv[2], "length");
            JS_ToUint32(ctx, &n, len); JS_FreeValue(ctx, len);
            for (uint32_t i = 0; i < n && i < H1_MAX_HEADERS; i++) {
                JSValue p = JS_GetPropertyUint32(ctx, argv[2], i);
                JSValue kv = JS_GetPropertyUint32(ctx, p, 0);
                JSValue vv = JS_GetPropertyUint32(ctx, p, 1);
                const char *k = JS_ToCString(ctx, kv), *v = JS_ToCString(ctx, vv);
                /* h1_headers_add rejects a non-token name or a value with CR/LF,
                 * so header injection from a page's init dies here. */
                if (k && v) h1_headers_add(&f->hdr, k, -1, v, -1);
                if (k) JS_FreeCString(ctx, k);
                if (v) JS_FreeCString(ctx, v);
                JS_FreeValue(ctx, kv); JS_FreeValue(ctx, vv); JS_FreeValue(ctx, p);
            }
        }
        if (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
            size_t bl = 0;
            uint8_t *bp = ab_bytes(ctx, argv[3], &bl);
            if (bp) {
                if (bl > 0 && (f->body = (char *)malloc(bl)) != 0) {
                    memcpy(f->body, bp, bl); f->body_len = (int)bl;
                }
            } else {
                size_t sl = 0;
                const char *s = JS_ToCStringLen(ctx, &sl, argv[3]);
                if (s && sl > 0 && (f->body = (char *)malloc(sl)) != 0) {
                    memcpy(f->body, s, sl); f->body_len = (int)sl;
                }
                if (s) JS_FreeCString(ctx, s);
            }
        }
        if (fetch_dial(f) != 0) { h1_headers_free(&f->hdr); free(f->body); f->body = 0; msg = "could not open a socket"; }
    }
    if (us) JS_FreeCString(ctx, us);

    if (msg) {
        /* A rejected promise, not a throw: fetch() rejects, it does not raise. */
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "TypeError"));
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
        JSValue r = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
        if (f) f->state = WF_FREE;
        return promise;
    }

    f->resolve = rf[0];
    f->reject  = rf[1];
    g_fetch_live++;
    return promise;
}

/* __utf8(arrayBuffer) -> string */
static JSValue js_utf8(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_NewString(ctx, "");
    size_t len = 0;
    uint8_t *p = ab_bytes(ctx, argv[0], &len);
    if (!p) return JS_ToString(ctx, argv[0]);
    return JS_NewStringLen(ctx, (const char *)p, len);
}

/* __urlParse(input, base|undefined) -> {href, protocol, ...} or null */
static JSValue js_url_parse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_NULL;
    const char *in = JS_ToCString(ctx, argv[0]);
    if (!in) return JS_NULL;

    struct wurl base, out;
    const struct wurl *bp = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        const char *b = JS_ToCString(ctx, argv[1]);
        if (b && wurl_parse(b, 0, &base) == 0) bp = &base;
        if (b) JS_FreeCString(ctx, b);
        if (!bp) { JS_FreeCString(ctx, in); return JS_NULL; }
    }
    int rc = wurl_parse(in, bp, &out);
    JS_FreeCString(ctx, in);
    if (rc != 0) return JS_NULL;

    char buf[WURL_MAX];
    JSValue o = JS_NewObject(ctx);
    wurl_href(&out, buf, (int)sizeof buf);
    JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, buf));
    JS_SetPropertyStr(ctx, o, "protocol", JS_NewString(ctx, out.https ? "https:" : "http:"));
    wurl_origin(&out, buf, (int)sizeof buf);
    JS_SetPropertyStr(ctx, o, "origin", JS_NewString(ctx, buf));
    /* `host` carries the port when it is not the default; `hostname` never does. */
    { const char *h = strstr(buf, "//"); JS_SetPropertyStr(ctx, o, "host", JS_NewString(ctx, h ? h + 2 : buf)); }
    JS_SetPropertyStr(ctx, o, "hostname", JS_NewString(ctx, out.host));
    if (out.port != default_port(&out)) { char pn[12]; num_str(out.port, pn); JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, pn)); }
    else JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, o, "pathname", JS_NewString(ctx, out.pathname));
    JS_SetPropertyStr(ctx, o, "search", JS_NewString(ctx, out.search));
    JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, out.hash));
    return o;
}

/* ---- media queries ----------------------------------------------------
 * A small, honest evaluator: it answers width/height/orientation/resolution
 * style questions against the REAL viewport and answers `false` to anything it
 * does not understand, which is what the spec says an unknown feature must do.
 * It is not LibCSS's @media evaluator -- a page can therefore in principle see
 * matchMedia disagree with which stylesheet rules applied. Unifying them means
 * exposing LibCSS's parser here and is left undone deliberately. */

static void trim_lc(const char *s, int n, char *out, int max)
{
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    int o = 0;
    for (int i = 0; i < n && o < max - 1; i++) {
        char c = s[i];
        out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out[o] = 0;
}

/* "800px" / "50em" / "2" -> px. -1 if it is not a length we understand. */
static int mq_len(const char *v)
{
    int n = 0, any = 0;
    const char *p = v;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); any = 1; }
    if (!any) return -1;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   /* fractional px: floor */
    if (!strcmp(p, "px") || !*p) return n;
    if (!strcmp(p, "em") || !strcmp(p, "rem")) return n * 16;
    return -1;
}

static int mq_feature(const char *feat, const char *val)
{
    int has_val = val && *val;
    if (!strcmp(feat, "width") || !strcmp(feat, "min-width") || !strcmp(feat, "max-width")) {
        if (!has_val) return g_vw > 0;
        int px = mq_len(val); if (px < 0) return 0;
        if (feat[1] == 'i') return g_vw >= px;          /* min- */
        if (feat[1] == 'a') return g_vw <= px;          /* max- */
        return g_vw == px;
    }
    if (!strcmp(feat, "height") || !strcmp(feat, "min-height") || !strcmp(feat, "max-height")) {
        if (!has_val) return g_vh > 0;
        int px = mq_len(val); if (px < 0) return 0;
        if (feat[1] == 'i') return g_vh >= px;
        if (feat[1] == 'a') return g_vh <= px;
        return g_vh == px;
    }
    if (!strcmp(feat, "orientation"))
        return has_val && (!strcmp(val, g_vw >= g_vh ? "landscape" : "portrait"));
    if (!strcmp(feat, "prefers-color-scheme"))
        return has_val && !strcmp(val, "light");        /* the browser paints light */
    if (!strcmp(feat, "prefers-reduced-motion"))
        return !has_val ? 0 : !strcmp(val, "no-preference");
    if (!strcmp(feat, "pointer") || !strcmp(feat, "any-pointer"))
        return has_val && !strcmp(val, "fine");         /* PS/2 mouse */
    if (!strcmp(feat, "hover") || !strcmp(feat, "any-hover"))
        return has_val && !strcmp(val, "hover");
    if (!strcmp(feat, "display-mode"))
        return has_val && !strcmp(val, "browser");
    return 0;                                           /* unknown feature: no match */
}

/* One comma-free query: [not|only] [type] [and (feature)]* */
static int mq_one(const char *q, int len)
{
    char buf[256];
    trim_lc(q, len, buf, (int)sizeof buf);
    if (!buf[0]) return 0;
    int negate = 0;
    char *p = buf;
    if (!strncmp(p, "not ", 4)) { negate = 1; p += 4; }
    else if (!strncmp(p, "only ", 5)) p += 5;
    while (*p == ' ') p++;

    int ok = 1;
    /* an optional media type before the first '(' */
    if (*p && *p != '(') {
        char type[32]; int n = 0;
        while (*p && *p != ' ' && n < (int)sizeof type - 1) type[n++] = *p++;
        type[n] = 0;
        if (strcmp(type, "all") && strcmp(type, "screen")) ok = 0;
        while (*p == ' ') p++;
        if (!strncmp(p, "and", 3)) { p += 3; while (*p == ' ') p++; }
    }
    while (*p == '(') {
        const char *e = strchr(p, ')');
        if (!e) { ok = 0; break; }
        char inner[128];
        int n = (int)(e - p - 1); if (n < 0) n = 0;
        if (n > (int)sizeof inner - 1) n = (int)sizeof inner - 1;
        memcpy(inner, p + 1, (size_t)n); inner[n] = 0;
        char feat[64], val[64];
        char *colon = strchr(inner, ':');
        if (colon) {
            *colon = 0;
            trim_lc(inner, (int)strlen(inner), feat, (int)sizeof feat);
            trim_lc(colon + 1, (int)strlen(colon + 1), val, (int)sizeof val);
        } else {
            trim_lc(inner, (int)strlen(inner), feat, (int)sizeof feat);
            val[0] = 0;
        }
        if (!mq_feature(feat, val)) ok = 0;
        p = (char *)e + 1;
        while (*p == ' ') p++;
        if (!strncmp(p, "and", 3)) { p += 3; while (*p == ' ') p++; }
    }
    return negate ? !ok : ok;
}

static JSValue js_media_match(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_FALSE;
    const char *q = JS_ToCString(ctx, argv[0]);
    if (!q) return JS_FALSE;
    int m = 0;
    const char *start = q;
    for (const char *p = q;; p++) {
        if (*p == ',' || !*p) {
            if (mq_one(start, (int)(p - start))) m = 1;
            start = p + 1;
            if (!*p) break;
        }
    }
    JS_FreeCString(ctx, q);
    return JS_NewBool(ctx, m);
}

/* ---- location (live) -------------------------------------------------- */

static JSValue loc_get(JSContext *ctx, JSValueConst t, int magic)
{
    (void)t;
    char buf[WURL_MAX];
    if (!g_loc_valid) {
        /* about:blank or anything url.c will not parse: href is the raw string
         * and the components are empty, which is what a browser reports for an
         * opaque-origin document. */
        return JS_NewString(ctx, magic == 0 ? g_loc_raw : "");
    }
    switch (magic) {
    case 0: wurl_href(&g_loc, buf, (int)sizeof buf); return JS_NewString(ctx, buf);
    case 1: return JS_NewString(ctx, g_loc.https ? "https:" : "http:");
    case 2: wurl_origin(&g_loc, buf, (int)sizeof buf);
            { const char *h = strstr(buf, "//"); return JS_NewString(ctx, h ? h + 2 : buf); }
    case 3: return JS_NewString(ctx, g_loc.host);
    case 4: if (g_loc.port == default_port(&g_loc)) return JS_NewString(ctx, "");
            { char pn[12]; num_str(g_loc.port, pn); return JS_NewString(ctx, pn); }
    case 5: return JS_NewString(ctx, g_loc.pathname);
    case 6: return JS_NewString(ctx, g_loc.search);
    case 7: return JS_NewString(ctx, g_loc.hash);
    case 8: wurl_origin(&g_loc, buf, (int)sizeof buf); return JS_NewString(ctx, buf);
    }
    return JS_UNDEFINED;
}

/* Assigning to a location component. A hash-only change is a same-document
 * change: it updates in place. Anything else is a navigation request, which
 * (see js_webapi.h) is recorded and not yet acted on. */
/* Prefix `s` with `c` unless it is empty or already has it. */
static void with_prefix(char *dst, int max, char c, const char *s)
{
    if (!s[0]) { dst[0] = 0; return; }
    if (s[0] == c) { scopy(dst, s, max); return; }
    dst[0] = c;
    scopy(dst + 1, s, max - 1);
}

/* Do two URLs differ only in their fragment? */
static int same_document(const struct wurl *a, const struct wurl *b)
{
    struct wurl x = *a, y = *b;
    char sa[WURL_MAX], sb[WURL_MAX];
    x.hash[0] = y.hash[0] = 0;
    wurl_href(&x, sa, WURL_MAX);
    wurl_href(&y, sb, WURL_MAX);
    return strcmp(sa, sb) == 0;
}

static JSValue loc_set(JSContext *ctx, JSValueConst t, JSValueConst v, int magic)
{
    (void)t;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;

    struct wurl u = g_loc;
    int ok = 1;
    switch (magic) {
    case 0:  ok = wurl_parse(s, g_loc_valid ? &g_loc : 0, &u) == 0; break;      /* href */
    case 5:  ok = wurl_parse(s, g_loc_valid ? &g_loc : 0, &u) == 0; break;      /* pathname */
    case 6:  with_prefix(u.search, WQ_MAX, '?', s); break;                      /* search */
    case 7:  with_prefix(u.hash, WH_MAX, '#', s); break;                        /* hash */
    default: ok = 0; break;
    }
    JS_FreeCString(ctx, s);
    if (!ok) return JS_UNDEFINED;

    char want[WURL_MAX];
    wurl_href(&u, want, WURL_MAX);

    /* A change confined to the fragment is a same-document change: it must NOT
     * reload the page, and it is the one location write we can honour fully. */
    if (g_loc_valid && same_document(&g_loc, &u)) {
        int changed = strcmp(g_loc.hash, u.hash) != 0;
        if (changed) {
            wurl_href(&g_loc, g_hash_old, WURL_MAX);
            scopy(g_hash_new, want, WURL_MAX);
            g_hashchange_queued = 1;
        }
        g_loc = u;
        scopy(g_loc_raw, want, WURL_MAX);
        return JS_UNDEFINED;
    }
    scopy(g_pending_nav, want, WURL_MAX);
    g_have_pending_nav = 1;
    printf("[webapi] navigation requested: %s (the loader does not consume this yet)\n", want);
    return JS_UNDEFINED;
}

static JSValue loc_assign(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    return loc_set(ctx, t, argv[0], 0);
}
static JSValue loc_reload(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)t; (void)argc; (void)argv;
    scopy(g_pending_nav, g_loc_raw, WURL_MAX);
    g_have_pending_nav = 1;
    return JS_UNDEFINED;
}
static JSValue loc_tostring(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; return loc_get(ctx, t, 0); }

static const JSCFunctionListEntry loc_funcs[] = {
    JS_CGETSET_MAGIC_DEF("href", loc_get, loc_set, 0),
    JS_CGETSET_MAGIC_DEF("protocol", loc_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("host", loc_get, NULL, 2),
    JS_CGETSET_MAGIC_DEF("hostname", loc_get, NULL, 3),
    JS_CGETSET_MAGIC_DEF("port", loc_get, NULL, 4),
    JS_CGETSET_MAGIC_DEF("pathname", loc_get, loc_set, 5),
    JS_CGETSET_MAGIC_DEF("search", loc_get, loc_set, 6),
    JS_CGETSET_MAGIC_DEF("hash", loc_get, loc_set, 7),
    JS_CGETSET_MAGIC_DEF("origin", loc_get, NULL, 8),
    JS_CFUNC_DEF("assign", 1, loc_assign),
    JS_CFUNC_DEF("replace", 1, loc_assign),
    JS_CFUNC_DEF("reload", 0, loc_reload),
    JS_CFUNC_DEF("toString", 0, loc_tostring),
};

/* ---- Storage (JS side) ------------------------------------------------ */

static JSClassID storage_cid;
static JSClassDef storage_class = { "Storage", 0, 0, 0, 0 };

static struct store *store_of(JSContext *ctx, JSValueConst t)
{
    (void)ctx;
    return (struct store *)JS_GetOpaque(t, storage_cid);
}

static JSValue st_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct store *s = store_of(ctx, t);
    if (!s || argc < 1) return JS_NULL;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_NULL;
    int i = store_find(s, k);
    JS_FreeCString(ctx, k);
    return i < 0 ? JS_NULL : JS_NewString(ctx, s->v[i].v);
}
static JSValue st_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct store *s = store_of(ctx, t);
    if (!s || argc < 2) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    const char *v = JS_ToCString(ctx, argv[1]);          /* String() coercion, per spec */
    int rc = (k && v) ? store_set(s, k, v) : -1;
    if (k) JS_FreeCString(ctx, k);
    if (v) JS_FreeCString(ctx, v);
    if (rc == -2) return JS_ThrowRangeError(ctx, "QuotaExceededError: storage is full");
    if (rc == -1) return JS_ThrowOutOfMemory(ctx);
    return JS_UNDEFINED;
}
static JSValue st_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct store *s = store_of(ctx, t);
    if (!s || argc < 1) return JS_UNDEFINED;
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_UNDEFINED;
    int i = store_find(s, k);
    if (i >= 0) store_erase(s, i);
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}
static JSValue st_clear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct store *s = store_of(ctx, t);
    if (s) while (s->n) store_erase(s, s->n - 1);
    return JS_UNDEFINED;
}
static JSValue st_key(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct store *s = store_of(ctx, t);
    if (!s || argc < 1) return JS_NULL;
    int32_t i = 0;
    JS_ToInt32(ctx, &i, argv[0]);
    return (i < 0 || i >= s->n) ? JS_NULL : JS_NewString(ctx, s->v[i].k);
}
static JSValue st_length(JSContext *ctx, JSValueConst t)
{
    struct store *s = store_of(ctx, t);
    return JS_NewInt32(ctx, s ? s->n : 0);
}

static const JSCFunctionListEntry storage_proto[] = {
    JS_CFUNC_DEF("getItem", 1, st_get),
    JS_CFUNC_DEF("setItem", 2, st_set),
    JS_CFUNC_DEF("removeItem", 1, st_remove),
    JS_CFUNC_DEF("clear", 0, st_clear),
    JS_CFUNC_DEF("key", 1, st_key),
    JS_CGETSET_DEF("length", st_length, NULL),
};

/* ---- history (JS side) ------------------------------------------------ */

static void hist_reset(JSContext *ctx, const char *url)
{
    for (int i = 0; i < g_hist_n; i++) {
        if (ctx) JS_FreeValue(ctx, g_hist[i].state);
        g_hist[i].state = JS_NULL;
    }
    g_hist_n = 1; g_hist_i = 0;
    scopy(g_hist[0].url, url ? url : "", WURL_MAX);
    g_hist[0].state = JS_NULL;
}

static JSValue hist_push(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int replace)
{
    (void)t;
    char href[WURL_MAX];
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        const char *u = JS_ToCString(ctx, argv[2]);
        if (!u) return JS_EXCEPTION;
        struct wurl n;
        if (wurl_parse(u, g_loc_valid ? &g_loc : 0, &n) != 0) {
            JS_FreeCString(ctx, u);
            return JS_ThrowTypeError(ctx, "history: invalid URL");
        }
        JS_FreeCString(ctx, u);
        /* Same-origin only, exactly as the spec requires -- a page must not be
         * able to make the address bar claim another site. */
        if (g_loc_valid && (n.https != g_loc.https || strcmp(n.host, g_loc.host) || n.port != g_loc.port))
            return JS_ThrowTypeError(ctx, "history: cross-origin pushState");
        g_loc = n; g_loc_valid = 1;
        wurl_href(&g_loc, href, WURL_MAX);
        scopy(g_loc_raw, href, WURL_MAX);
    } else {
        scopy(href, g_loc_raw, WURL_MAX);
    }

    if (!replace) {
        for (int i = g_hist_i + 1; i < g_hist_n; i++) JS_FreeValue(ctx, g_hist[i].state);
        g_hist_n = g_hist_i + 1;
        if (g_hist_n >= HIST_MAX) {              /* drop the oldest entry */
            JS_FreeValue(ctx, g_hist[0].state);
            for (int i = 0; i + 1 < g_hist_n; i++) g_hist[i] = g_hist[i + 1];
            g_hist_n--; g_hist_i--;
        }
        g_hist_i = g_hist_n++;
        g_hist[g_hist_i].state = JS_NULL;
    }
    JS_FreeValue(ctx, g_hist[g_hist_i].state);
    g_hist[g_hist_i].state = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_NULL;
    scopy(g_hist[g_hist_i].url, href, WURL_MAX);
    return JS_UNDEFINED;
}

static JSValue js_pushState(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return hist_push(ctx, t, argc, argv, 0); }
static JSValue js_replaceState(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return hist_push(ctx, t, argc, argv, 1); }

/* Move within the same-document history. popstate is queued rather than fired
 * inline: the spec makes it a task, and firing it inside history.back() would
 * re-enter the page's own call stack. */
static void hist_move(JSContext *ctx, int delta)
{
    int want = g_hist_i + delta;
    if (want < 0 || want >= g_hist_n || delta == 0) return;
    g_hist_i = want;
    set_location(g_hist[g_hist_i].url);
    JS_FreeValue(ctx, g_popstate_state);
    g_popstate_state = JS_DupValue(ctx, g_hist[g_hist_i].state);
    g_popstate_queued = 1;
}

static JSValue js_hist_go(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t d = 0;
    if (argc > 0) JS_ToInt32(ctx, &d, argv[0]);
    hist_move(ctx, d);
    return JS_UNDEFINED;
}
static JSValue js_hist_back(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; (void)argv; hist_move(ctx, -1); return JS_UNDEFINED; }
static JSValue js_hist_fwd(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; (void)argv; hist_move(ctx, +1); return JS_UNDEFINED; }
static JSValue hist_get_len(JSContext *ctx, JSValueConst t)
{ (void)t; return JS_NewInt32(ctx, g_hist_n); }
static JSValue hist_get_state(JSContext *ctx, JSValueConst t)
{ (void)t; return JS_DupValue(ctx, g_hist[g_hist_i].state); }

static const JSCFunctionListEntry hist_funcs[] = {
    JS_CFUNC_DEF("pushState", 3, js_pushState),
    JS_CFUNC_DEF("replaceState", 3, js_replaceState),
    JS_CFUNC_DEF("back", 0, js_hist_back),
    JS_CFUNC_DEF("forward", 0, js_hist_fwd),
    JS_CFUNC_DEF("go", 1, js_hist_go),
    JS_CGETSET_DEF("length", hist_get_len, NULL),
    JS_CGETSET_DEF("state", hist_get_state, NULL),
};

/* ---- the prelude ------------------------------------------------------
 * Evaluated once per page. It is a function expression so that the four C
 * primitives arrive as ARGUMENTS rather than as globals a page could reach --
 * `__fetchStart` is not something script should be able to see or replace.
 * It returns the hooks C needs to call back into. */
static const char *PRELUDE =
"(function (__fetchStart, __utf8, __urlParse, __mediaMatch) {\n"
"'use strict';\n"
"var G = globalThis;\n"

/* ---- Headers ---- */
"G.Headers = function Headers(init) {\n"
"  this._l = [];\n"
"  if (init) {\n"
"    if (Array.isArray(init)) { for (var i = 0; i < init.length; i++) this.append(init[i][0], init[i][1]); }\n"
"    else if (init instanceof G.Headers) { for (var j = 0; j < init._l.length; j++) this.append(init._l[j][0], init._l[j][1]); }\n"
"    else if (typeof init.forEach === 'function') { var self = this; init.forEach(function (v, k) { self.append(k, v); }); }\n"
"    else { for (var k in init) this.append(k, init[k]); }\n"
"  }\n"
"};\n"
"G.Headers.prototype = {\n"
"  constructor: G.Headers,\n"
"  append: function (n, v) { this._l.push([String(n), String(v).trim()]); },\n"
"  set: function (n, v) { var k = String(n).toLowerCase();\n"
"    this._l = this._l.filter(function (p) { return p[0].toLowerCase() !== k; });\n"
"    this._l.push([String(n), String(v).trim()]); },\n"
"  delete: function (n) { var k = String(n).toLowerCase();\n"
"    this._l = this._l.filter(function (p) { return p[0].toLowerCase() !== k; }); },\n"
"  has: function (n) { var k = String(n).toLowerCase();\n"
"    return this._l.some(function (p) { return p[0].toLowerCase() === k; }); },\n"
   /* Several same-named headers join with ', ', which is what the spec says
      and what makes Set-Cookie the one header you must not read this way. */
"  get: function (n) { var k = String(n).toLowerCase();\n"
"    var v = this._l.filter(function (p) { return p[0].toLowerCase() === k; }).map(function (p) { return p[1]; });\n"
"    return v.length ? v.join(', ') : null; },\n"
"  forEach: function (fn, t) { var s = this; this._l.slice().forEach(function (p) { fn.call(t, p[1], p[0].toLowerCase(), s); }); },\n"
"  keys: function () { return this._l.map(function (p) { return p[0].toLowerCase(); })[Symbol.iterator](); },\n"
"  values: function () { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); },\n"
"  entries: function () { return this._l.map(function (p) { return [p[0].toLowerCase(), p[1]]; })[Symbol.iterator](); }\n"
"};\n"
"G.Headers.prototype[Symbol.iterator] = G.Headers.prototype.entries;\n"

/* ---- Response ---- */
"G.Response = function Response(body, init) {\n"
"  init = init || {};\n"
"  this._b = body === undefined ? null : body;\n"
"  this.status = init.status === undefined ? 200 : init.status | 0;\n"
"  this.statusText = init.statusText === undefined ? '' : String(init.statusText);\n"
"  this.headers = init.headers instanceof G.Headers ? init.headers : new G.Headers(init.headers);\n"
"  this.url = init.url || '';\n"
"  this.redirected = !!init.redirected;\n"
"  this.type = 'basic';\n"
"  this.ok = this.status >= 200 && this.status < 300;\n"
"  this.bodyUsed = false;\n"
"};\n"
"G.Response.prototype = {\n"
"  constructor: G.Response,\n"
"  _take: function () { if (this.bodyUsed) throw new TypeError('body already read'); this.bodyUsed = true; return this._b; },\n"
"  text: function () { var b; try { b = this._take(); } catch (e) { return Promise.reject(e); }\n"
"    return Promise.resolve(b === null ? '' : (typeof b === 'string' ? b : __utf8(b))); },\n"
"  json: function () { return this.text().then(function (t) { return JSON.parse(t); }); },\n"
"  arrayBuffer: function () { var b; try { b = this._take(); } catch (e) { return Promise.reject(e); }\n"
"    if (b === null) return Promise.resolve(new ArrayBuffer(0));\n"
"    if (typeof b === 'string') { var a = new Uint8Array(b.length);\n"
"      for (var i = 0; i < b.length; i++) a[i] = b.charCodeAt(i) & 255; return Promise.resolve(a.buffer); }\n"
"    return Promise.resolve(b); },\n"
"  blob: function () { return this.arrayBuffer(); },\n"
"  clone: function () { var r = new G.Response(this._b, { status: this.status, statusText: this.statusText,\n"
"    headers: this.headers, url: this.url, redirected: this.redirected }); return r; }\n"
"};\n"
"G.Response.error = function () { var r = new G.Response(null, { status: 0 }); r.type = 'error'; return r; };\n"

/* ---- fetch ---- */
"G.fetch = function fetch(input, init) {\n"
"  init = init || {};\n"
"  var url = (input && typeof input === 'object' && input.url) ? input.url : String(input);\n"
"  var method = String(init.method || (input && input.method) || 'GET').toUpperCase();\n"
"  var hs = new G.Headers(init.headers || (input && input.headers));\n"
"  var body = init.body;\n"
"  if (body !== undefined && body !== null) {\n"
"    if (G.URLSearchParams && body instanceof G.URLSearchParams) {\n"
"      if (!hs.has('content-type')) hs.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8');\n"
"      body = body.toString();\n"
"    } else if (ArrayBuffer.isView(body)) {\n"
"      body = body.buffer.slice(body.byteOffset, body.byteOffset + body.byteLength);\n"
"    } else if (!(body instanceof ArrayBuffer) && typeof body !== 'string') {\n"
"      body = String(body);\n"
"    }\n"
"  } else body = null;\n"
"  var pairs = []; hs.forEach(function (v, k) { pairs.push([k, v]); });\n"
"  return __fetchStart(url, method, pairs, body);\n"
"};\n"

/* ---- URLSearchParams ----
 * Kept as a serialize/parse pair over ONE string so that a params object taken
 * from a URL and the URL's own .search can never drift apart: every read
 * parses, every write serializes back through the owner. */
"function uspParse(s) {\n"
"  var out = [];\n"
"  s = String(s || '');\n"
"  if (s.charAt(0) === '?') s = s.slice(1);\n"
"  if (!s) return out;\n"
"  s.split('&').forEach(function (kv) {\n"
"    if (!kv) return;\n"
"    var i = kv.indexOf('=');\n"
"    var k = i < 0 ? kv : kv.slice(0, i), v = i < 0 ? '' : kv.slice(i + 1);\n"
"    out.push([uspDec(k), uspDec(v)]);\n"
"  });\n"
"  return out;\n"
"}\n"
"function uspDec(s) { try { return decodeURIComponent(String(s).replace(/\\+/g, ' ')); } catch (e) { return String(s); } }\n"
"function uspEnc(s) { return encodeURIComponent(String(s)).replace(/%20/g, '+'); }\n"
"function uspSer(l) { return l.map(function (p) { return uspEnc(p[0]) + '=' + uspEnc(p[1]); }).join('&'); }\n"
"G.URLSearchParams = function URLSearchParams(init) {\n"
"  this._owner = null;\n"
"  if (init instanceof G.URLSearchParams) this._s = init.toString();\n"
"  else if (Array.isArray(init)) this._s = uspSer(init.map(function (p) { return [p[0], p[1]]; }));\n"
"  else if (init && typeof init === 'object') { var l = []; for (var k in init) l.push([k, init[k]]); this._s = uspSer(l); }\n"
"  else this._s = String(init === undefined || init === null ? '' : init).replace(/^\\?/, '');\n"
"};\n"
"G.URLSearchParams.prototype = {\n"
"  constructor: G.URLSearchParams,\n"
"  _get: function () { return uspParse(this._owner ? this._owner.search : this._s); },\n"
"  _put: function (l) { var s = uspSer(l);\n"
"    if (this._owner) this._owner._setSearch(s ? '?' + s : ''); else this._s = s; },\n"
"  get: function (n) { n = String(n); var l = this._get();\n"
"    for (var i = 0; i < l.length; i++) if (l[i][0] === n) return l[i][1]; return null; },\n"
"  getAll: function (n) { n = String(n);\n"
"    return this._get().filter(function (p) { return p[0] === n; }).map(function (p) { return p[1]; }); },\n"
"  has: function (n) { return this.get(n) !== null; },\n"
"  append: function (n, v) { var l = this._get(); l.push([String(n), String(v)]); this._put(l); },\n"
"  set: function (n, v) { n = String(n); v = String(v); var l = this._get(), done = false, o = [];\n"
"    for (var i = 0; i < l.length; i++) { if (l[i][0] !== n) { o.push(l[i]); continue; }\n"
"      if (!done) { o.push([n, v]); done = true; } }\n"
"    if (!done) o.push([n, v]); this._put(o); },\n"
"  delete: function (n) { n = String(n);\n"
"    this._put(this._get().filter(function (p) { return p[0] !== n; })); },\n"
"  sort: function () { this._put(this._get().sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; })); },\n"
"  forEach: function (fn, t) { var s = this; this._get().forEach(function (p) { fn.call(t, p[1], p[0], s); }); },\n"
"  keys: function () { return this._get().map(function (p) { return p[0]; })[Symbol.iterator](); },\n"
"  values: function () { return this._get().map(function (p) { return p[1]; })[Symbol.iterator](); },\n"
"  entries: function () { return this._get()[Symbol.iterator](); },\n"
"  toString: function () { return uspSer(this._get()); }\n"
"};\n"
"G.URLSearchParams.prototype[Symbol.iterator] = G.URLSearchParams.prototype.entries;\n"
"Object.defineProperty(G.URLSearchParams.prototype, 'size', { get: function () { return this._get().length; } });\n"

/* ---- URL ---- */
"G.URL = function URL(input, base) {\n"
"  var p = __urlParse(String(input), base === undefined || base === null ? undefined : String(base));\n"
"  if (!p) throw new TypeError('Invalid URL: ' + input);\n"
"  this._p = p;\n"
"  this._sp = new G.URLSearchParams(); this._sp._owner = this;\n"
"};\n"
"G.URL.prototype = {\n"
"  constructor: G.URL,\n"
"  _reparse: function (href) { var p = __urlParse(href, undefined); if (p) this._p = p; },\n"
"  _setSearch: function (s) { this._p.search = s; this._p.href = this._p.origin + this._p.pathname + s + this._p.hash; },\n"
"  toString: function () { return this._p.href; },\n"
"  toJSON: function () { return this._p.href; }\n"
"};\n"
"['href','protocol','origin','host','hostname','port','pathname','search','hash'].forEach(function (k) {\n"
"  Object.defineProperty(G.URL.prototype, k, {\n"
"    get: function () { return this._p[k]; },\n"
"    set: function (v) {\n"
"      if (k === 'origin') return;\n"
"      var p = this._p, href;\n"
"      if (k === 'href') href = String(v);\n"
"      else if (k === 'search') { v = String(v); href = p.origin + p.pathname + (v && v.charAt(0) !== '?' ? '?' + v : v) + p.hash; }\n"
"      else if (k === 'hash') { v = String(v); href = p.origin + p.pathname + p.search + (v && v.charAt(0) !== '#' ? '#' + v : v); }\n"
"      else if (k === 'pathname') { v = String(v); href = p.origin + (v.charAt(0) === '/' ? v : '/' + v) + p.search + p.hash; }\n"
"      else if (k === 'protocol') { href = p.href.replace(/^[a-zA-Z]+:/, String(v).replace(/:*$/, ':')); }\n"
"      else if (k === 'host' || k === 'hostname') { href = p.href.replace(p.host, String(v)); }\n"
"      else if (k === 'port') { href = p.protocol + '//' + p.hostname + (v === '' ? '' : ':' + v) + p.pathname + p.search + p.hash; }\n"
"      else return;\n"
"      this._reparse(href);\n"
"    }\n"
"  });\n"
"});\n"
"Object.defineProperty(G.URL.prototype, 'searchParams', { get: function () { return this._sp; } });\n"

/* ---- XMLHttpRequest, over fetch ----
 * Async only. abort() stops the response being delivered but does not cancel
 * the transfer -- there is no cancellation in the socket ABI yet, and
 * pretending otherwise would be the kind of lie this file is trying not to
 * tell. There are no progress events for the same reason: the body is complete
 * before the promise resolves. */
"G.XMLHttpRequest = function XMLHttpRequest() {\n"
"  this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = '';\n"
"  this.response = ''; this.responseType = ''; this.responseURL = ''; this.timeout = 0;\n"
"  this.withCredentials = false; this.upload = {};\n"
"  this.onreadystatechange = null; this.onload = null; this.onerror = null;\n"
"  this.onloadend = null; this.onabort = null; this.ontimeout = null; this.onprogress = null;\n"
"  this._h = []; this._hdr = null; this._ev = {}; this._aborted = false;\n"
"};\n"
"G.XMLHttpRequest.UNSENT = 0; G.XMLHttpRequest.OPENED = 1; G.XMLHttpRequest.HEADERS_RECEIVED = 2;\n"
"G.XMLHttpRequest.LOADING = 3; G.XMLHttpRequest.DONE = 4;\n"
"G.XMLHttpRequest.prototype = {\n"
"  constructor: G.XMLHttpRequest,\n"
"  open: function (m, u) { this._m = String(m); this._u = String(u); this._rs(1); },\n"
"  setRequestHeader: function (n, v) { this._h.push([String(n), String(v)]); },\n"
"  overrideMimeType: function () {},\n"
"  getResponseHeader: function (n) { return this._hdr ? this._hdr.get(n) : null; },\n"
"  getAllResponseHeaders: function () { if (!this._hdr) return '';\n"
"    var s = ''; this._hdr.forEach(function (v, k) { s += k + ': ' + v + '\\r\\n'; }); return s; },\n"
"  addEventListener: function (t, f) { (this._ev[t] = this._ev[t] || []).push(f); },\n"
"  removeEventListener: function (t, f) { var l = this._ev[t]; if (!l) return;\n"
"    var i = l.indexOf(f); if (i >= 0) l.splice(i, 1); },\n"
"  abort: function () { this._aborted = true; this.readyState = 0; this._fire('abort'); },\n"
"  _fire: function (t) { var e = { type: t, target: this, currentTarget: this };\n"
"    var h = this['on' + t]; if (typeof h === 'function') h.call(this, e);\n"
"    (this._ev[t] || []).slice().forEach(function (f) { f.call(this, e); }, this); },\n"
"  _rs: function (s) { this.readyState = s; this._fire('readystatechange'); },\n"
"  send: function (body) {\n"
"    var self = this;\n"
"    G.fetch(this._u, { method: this._m || 'GET', headers: this._h, body: body })\n"
"      .then(function (r) {\n"
"        if (self._aborted) return null;\n"
"        self._hdr = r.headers; self.status = r.status; self.statusText = r.statusText;\n"
"        self.responseURL = r.url; self._rs(2);\n"
"        return r.text();\n"
"      })\n"
"      .then(function (t) {\n"
"        if (self._aborted || t === null) return;\n"
"        self.responseText = t;\n"
"        if (self.responseType === 'json') { try { self.response = JSON.parse(t); } catch (e) { self.response = null; } }\n"
"        else self.response = t;\n"
"        self._rs(3); self._rs(4);\n"
"        self._fire('load'); self._fire('loadend');\n"
"      })\n"
"      .catch(function (e) {\n"
"        if (self._aborted) return;\n"
"        self.status = 0; self._rs(4); self._fire('error'); self._fire('loadend');\n"
"      });\n"
"  }\n"
"};\n"

/* ---- matchMedia ---- */
"var mqls = [];\n"
"G.matchMedia = function matchMedia(q) {\n"
"  q = String(q);\n"
"  var m = {\n"
"    media: q, matches: __mediaMatch(q), onchange: null, _l: [],\n"
"    addListener: function (f) { if (typeof f === 'function') this._l.push(f); },\n"
"    removeListener: function (f) { var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1); },\n"
"    addEventListener: function (t, f) { if (t === 'change') this.addListener(f); },\n"
"    removeEventListener: function (t, f) { if (t === 'change') this.removeListener(f); },\n"
"    dispatchEvent: function () { return true; }\n"
"  };\n"
"  if (mqls.length < 256) mqls.push(m);\n"
"  return m;\n"
"};\n"

/* The hooks C calls back through. */
"return {\n"
"  mkResponse: function (status, statusText, pairs, buf, url, redirected) {\n"
"    return new G.Response(buf, { status: status, statusText: statusText, headers: pairs,\n"
"                                 url: url, redirected: redirected });\n"
"  },\n"
"  viewportChanged: function () {\n"
"    mqls.forEach(function (m) {\n"
"      var now = __mediaMatch(m.media);\n"
"      if (now === m.matches) return;\n"
"      m.matches = now;\n"
"      var e = { type: 'change', media: m.media, matches: now, target: m };\n"
"      if (typeof m.onchange === 'function') m.onchange(e);\n"
"      m._l.slice().forEach(function (f) { f.call(m, e); });\n"
"    });\n"
"  },\n"
   /* One entry point for the two events this file owns. `window.onpopstate =`
    * has to be called BY HAND: js_dom.c's on* table (the accessors that turn an
    * assignment into a registered listener) covers the DOM event names and not
    * these two, so the assignment lands as a plain data property that
    * dispatchEvent will never see. The descriptor test is what keeps that from
    * becoming a double-fire the day the table grows a popstate entry. */
"  fire: function (type, state, oldURL, newURL) {\n"
"    var ev;\n"
"    try { ev = new Event(type); } catch (e) { ev = { type: type }; }\n"
"    try { if (type === 'popstate') ev.state = state;\n"
"          else { ev.oldURL = oldURL; ev.newURL = newURL; } } catch (e2) {}\n"
"    if (typeof G.dispatchEvent === 'function') G.dispatchEvent(ev);\n"
"    var d = Object.getOwnPropertyDescriptor(G, 'on' + type);\n"
"    if (d && typeof d.value === 'function') d.value.call(G, ev);\n"
"  }\n"
"};\n"
"})\n";

static JSValue g_fire_fn = JS_UNDEFINED;   /* the prelude's popstate/hashchange dispatcher */

/* ---- install / close / pump ------------------------------------------ */

static JSValue make_storage(JSContext *ctx, const char *origin, int session)
{
    JSValue o = JS_NewObjectClass(ctx, (int)storage_cid);
    if (JS_IsException(o)) return o;
    JS_SetOpaque(o, store_for(origin, session));
    return o;
}

static int g_vp_dirty;

void js_webapi_set_viewport(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w == g_vw && h == g_vh) return;
    g_vw = w; g_vh = h;
    /* The listeners run from the pump, not from here: firing page script from
     * inside whatever resized the window would re-enter the embedder. */
    g_vp_dirty = 1;
}

void js_webapi_install(JSContext *ctx, const char *url)
{
    if (!ctx) return;
    set_location(url);
    hist_reset(0, g_loc_raw);
    g_popstate_state = JS_NULL;
    g_popstate_queued = g_hashchange_queued = 0;
    for (int i = 0; i < WF_MAX; i++) { g_fetch[i].state = WF_FREE; g_fetch[i].fd = -1; }
    g_fetch_live = 0;

    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue g = JS_GetGlobalObject(ctx);

    /* location -- js_page.c used to publish an href-only object here. */
    JSValue loc = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, loc, loc_funcs, (int)(sizeof loc_funcs / sizeof loc_funcs[0]));
    JS_SetPropertyStr(ctx, g, "location", JS_DupValue(ctx, loc));
    {   /* document.location is the same object; document belongs to js_dom.c,
         * which has already installed it by the time we run. */
        JSValue doc = JS_GetPropertyStr(ctx, g, "document");
        if (JS_IsObject(doc)) JS_SetPropertyStr(ctx, doc, "location", JS_DupValue(ctx, loc));
        JS_FreeValue(ctx, doc);
    }
    JS_FreeValue(ctx, loc);

    JSValue hist = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, hist, hist_funcs, (int)(sizeof hist_funcs / sizeof hist_funcs[0]));
    JS_SetPropertyStr(ctx, hist, "scrollRestoration", JS_NewString(ctx, "auto"));
    JS_SetPropertyStr(ctx, g, "history", hist);

    /* Storage. The origin is the document's; a document url.c cannot parse
     * (about:blank) gets its own store keyed by the raw string, so two such
     * pages share one -- which is what an opaque origin would NOT do. Named
     * because it is a real, if unreachable, deviation. */
    char origin[URL_HOST_MAX + 16];
    if (g_loc_valid) wurl_origin(&g_loc, origin, (int)sizeof origin);
    else scopy(origin, g_loc_raw, (int)sizeof origin);

    JS_NewClassID(&storage_cid);            /* per-runtime: js_page.c builds a new one per page */
    if (JS_NewClass(rt, storage_cid, &storage_class) >= 0) {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, storage_proto,
                                   (int)(sizeof storage_proto / sizeof storage_proto[0]));
        JS_SetClassProto(ctx, storage_cid, proto);
        JS_SetPropertyStr(ctx, g, "localStorage", make_storage(ctx, origin, 0));
        JS_SetPropertyStr(ctx, g, "sessionStorage", make_storage(ctx, origin, 1));
    }

    /* Viewport metrics, only if nothing else claimed them. */
    {
        static const char *names[] = { "innerWidth", "innerHeight", "outerWidth", "outerHeight" };
        int vals[4] = { g_vw, g_vh, g_vw, g_vh };
        for (int i = 0; i < 4; i++) {
            JSValue cur = JS_GetPropertyStr(ctx, g, names[i]);
            int absent = JS_IsUndefined(cur);
            JS_FreeValue(ctx, cur);
            if (absent) JS_SetPropertyStr(ctx, g, names[i], JS_NewInt32(ctx, vals[i]));
        }
        JSValue cur = JS_GetPropertyStr(ctx, g, "devicePixelRatio");
        int absent = JS_IsUndefined(cur);
        JS_FreeValue(ctx, cur);
        if (absent) JS_SetPropertyStr(ctx, g, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));
        JSValue scr = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, scr, "width", JS_NewInt32(ctx, g_vw));
        JS_SetPropertyStr(ctx, scr, "height", JS_NewInt32(ctx, g_vh));
        JS_SetPropertyStr(ctx, scr, "availWidth", JS_NewInt32(ctx, g_vw));
        JS_SetPropertyStr(ctx, scr, "availHeight", JS_NewInt32(ctx, g_vh));
        JS_SetPropertyStr(ctx, scr, "colorDepth", JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, scr, "pixelDepth", JS_NewInt32(ctx, 24));
        JS_SetPropertyStr(ctx, g, "screen", scr);
        JS_SetPropertyStr(ctx, g, "origin", JS_NewString(ctx, origin));
    }

    /* The prelude, with the four C primitives as arguments. */
    JSValue fn = JS_Eval(ctx, PRELUDE, strlen(PRELUDE), "<webapi>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[webapi] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, g);
        return;
    }
    JSValue args[4];
    args[0] = JS_NewCFunction(ctx, js_fetch_start, "__fetchStart", 4);
    args[1] = JS_NewCFunction(ctx, js_utf8, "__utf8", 1);
    args[2] = JS_NewCFunction(ctx, js_url_parse, "__urlParse", 2);
    args[3] = JS_NewCFunction(ctx, js_media_match, "__mediaMatch", 1);
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, 4, (JSValueConst *)args);
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(hooks)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[webapi] prelude call failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, hooks);
        JS_FreeValue(ctx, g);
        return;
    }
    g_mk_response      = JS_GetPropertyStr(ctx, hooks, "mkResponse");
    g_viewport_changed = JS_GetPropertyStr(ctx, hooks, "viewportChanged");
    g_fire_fn          = JS_GetPropertyStr(ctx, hooks, "fire");
    JS_FreeValue(ctx, hooks);
    JS_FreeValue(ctx, g);
}

void js_webapi_close(JSContext *ctx)
{
    for (int i = 0; i < WF_MAX; i++)
        if (g_fetch[i].state != WF_FREE) fetch_release(ctx, &g_fetch[i]);
    g_fetch_live = 0;
    hist_reset(ctx, "");
    if (ctx) {
        JS_FreeValue(ctx, g_popstate_state);
        JS_FreeValue(ctx, g_mk_response);
        JS_FreeValue(ctx, g_viewport_changed);
        JS_FreeValue(ctx, g_fire_fn);
    }
    g_popstate_state = JS_NULL;
    g_mk_response = g_viewport_changed = g_fire_fn = JS_UNDEFINED;
    g_popstate_queued = g_hashchange_queued = 0;
    g_hist_n = 0; g_hist_i = 0;
}

int js_webapi_pending(void)
{ return g_fetch_live > 0 || g_popstate_queued || g_hashchange_queued || g_vp_dirty; }

int js_webapi_pump(JSContext *ctx)
{
    if (!ctx) return 0;
    int ran = 0;

    if (g_vp_dirty) {
        g_vp_dirty = 0;
        if (JS_IsFunction(ctx, g_viewport_changed)) {
            JSValue r = JS_Call(ctx, g_viewport_changed, JS_UNDEFINED, 0, 0);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r);
            ran++;
        }
    }

    if ((g_popstate_queued || g_hashchange_queued) && JS_IsFunction(ctx, g_fire_fn)) {
        int pop = g_popstate_queued, hash = g_hashchange_queued;
        g_popstate_queued = g_hashchange_queued = 0;
        for (int k = 0; k < 2; k++) {
            if (k == 0 && !pop) continue;
            if (k == 1 && !hash) continue;
            JSValue a[4];
            a[0] = JS_NewString(ctx, k == 0 ? "popstate" : "hashchange");
            a[1] = k == 0 ? JS_DupValue(ctx, g_popstate_state) : JS_NULL;
            a[2] = JS_NewString(ctx, k == 1 ? g_hash_old : "");
            a[3] = JS_NewString(ctx, k == 1 ? g_hash_new : "");
            JSValue r = JS_Call(ctx, g_fire_fn, JS_UNDEFINED, 4, (JSValueConst *)a);
            if (JS_IsException(r)) {
                JSValue e = JS_GetException(ctx);
                const char *m = JS_ToCString(ctx, e);
                printf("[webapi] uncaught in %s: %s\n", k == 0 ? "popstate" : "hashchange",
                       m ? m : "?");
                if (m) JS_FreeCString(ctx, m);
                JS_FreeValue(ctx, e);
            }
            JS_FreeValue(ctx, r);
            for (int i = 0; i < 4; i++) JS_FreeValue(ctx, a[i]);
            ran++;
        }
    }

    for (int i = 0; i < WF_MAX; i++)
        if (g_fetch[i].state != WF_FREE) ran += fetch_step(ctx, &g_fetch[i]);
    return ran;
}
