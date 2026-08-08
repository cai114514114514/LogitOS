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
#include "cookies.h"
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
/* The wall clock, for cookie expiry. SYS_GET_TIME answers whole seconds off
 * the CMOS RTC in UTC; the civil-date arithmetic is Hinnant's, the same one
 * cookies.c uses to parse a date, so a round trip through both agrees. */
static long long d_now_unix(void)
{
    struct logit_time t;
    get_time(&t);
    long long y = t.year, m = t.month, d = t.day;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    y -= (m <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return ((days * 24 + t.hour) * 60 + t.minute) * 60 + t.second;
}
static const struct webapi_net g_default_net =
    { d_open, d_poll, d_send, d_recv, d_close, d_now, d_now_unix };
#else
static const struct webapi_net g_default_net = { 0, 0, 0, 0, 0, 0, 0 };
#endif

static const struct webapi_net *g_net = &g_default_net;
void js_webapi_set_net(const struct webapi_net *n) { g_net = n ? n : &g_default_net; }
static unsigned long long now_ms(void) { return g_net && g_net->now_ms ? g_net->now_ms() : 0; }
static long long now_unix(void) { return g_net && g_net->now_unix ? g_net->now_unix() : 0; }

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

/* ---- small string helpers used by the cookie and CORS rules ----------- */

static int lc_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (lc_c((unsigned char)*a) != lc_c((unsigned char)*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
static int ci_strneq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int x = lc_c((unsigned char)a[i]), y = lc_c((unsigned char)b[i]);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* Does a comma-separated list contain `tok` (case-insensitively)? */
static int list_has(const char *list, const char *tok)
{
    if (!list || !tok || !*tok) return 0;
    int tl = (int)strlen(tok), len = (int)strlen(list), b = 0;
    while (b <= len) {
        int e = b;
        while (e < len && list[e] != ',') e++;
        int tb = b, te = e;
        while (tb < te && (list[tb] == ' ' || list[tb] == '\t')) tb++;
        while (te > tb && (list[te-1] == ' ' || list[te-1] == '\t')) te--;
        if (te - tb == tl && ci_strneq(list + tb, tok, tl)) return 1;
        if (e >= len) break;
        b = e + 1;
    }
    return 0;
}

static int same_origin(const struct wurl *a, const struct wurl *b)
{ return a->https == b->https && a->port == b->port && ci_streq(a->host, b->host); }

/* ---- the cookie jar ---------------------------------------------------
 * c/net/http/cookies.c has been in this tree, tested and unreachable: nothing
 * built a request that could carry a Cookie header.  This is the wiring.  It
 * is a single process-wide jar, deliberately outliving js_page_close() for the
 * same reason Storage does -- a session that evaporated on every navigation
 * would not be a session.
 *
 * NOT PERSISTED TO DISK, for the reason given at Storage: LogitFS has a known
 * cross-boot write-durability bug and every harness boots with -snapshot.  So
 * cookies live as long as the browser process.  Persistent cookies still honour
 * their Expires/Max-Age within that lifetime; they just do not survive a
 * reboot.  Stated because "we have cookies" would otherwise imply more.
 *
 * WHO MAY SET WHAT is cookies.c's problem and it is solved there (RFC 6265
 * 5.1.3/5.1.4 domain- and path-match, the 6265bis Secure/HttpOnly/prefix
 * rules, and an approximate public-suffix rule that over-rejects on purpose).
 * What is decided HERE is the two things cookies.c cannot see: whether a
 * request is same-site (SameSite) and whether it is allowed to carry
 * credentials at all (CORS). */

static struct cookie_jar g_jar;
static int g_jar_ready;

static struct cookie_jar *jar(void)
{
    if (!g_jar_ready) { cookie_jar_init(&g_jar); g_jar_ready = 1; }
    return &g_jar;
}

static void ck_ctx(struct cookie_ctx *c, const struct wurl *u, int http_api)
{
    c->host = u->host;
    c->path = u->pathname[0] ? u->pathname : "/";
    c->secure = u->https;
    c->http_api = http_api;
}

/* ---- CORS -------------------------------------------------------------
 * The previous version of this file said there was "no origin boundary to
 * enforce in this browser -- everything a page fetches, it could fetch".  That
 * stopped being true the moment the line above wired up cookies: a request now
 * carries the user's session, so "any page may read any origin's response" is
 * "any page may read the user's mail".  A browser that ignores CORS is not more
 * capable than one that enforces it; it is one whose users have no security
 * boundary at all.
 *
 * So the real model, and the real refusals:
 *
 *   - A SIMPLE cross-origin request (GET/HEAD/POST, only CORS-safelisted
 *     author headers) goes straight out with an Origin header.  Its RESPONSE
 *     is refused unless Access-Control-Allow-Origin names our origin or is
 *     `*`.  Refused means the promise rejects and NOT ONE BODY BYTE reaches
 *     script -- the check runs at headers-done, before the stream exists.
 *   - Anything else (PUT/DELETE/PATCH, a custom header, a JSON content-type)
 *     is PREFLIGHTED: an OPTIONS carrying Access-Control-Request-Method and
 *     -Headers, whose answer must allow both.  Only then is the real request
 *     sent, on a fresh connection.  A successful preflight is cached per
 *     (origin, method, credentials) for Access-Control-Max-Age.
 *   - CREDENTIALS (cookies) go cross-origin only in `credentials: 'include'`
 *     mode, and then Access-Control-Allow-Credentials must be `true` AND
 *     Access-Control-Allow-Origin must name the origin exactly -- `*` is
 *     refused, because `*` means "any origin may read this" and combining that
 *     with a session cookie is the exact hole the spec closes.
 *   - RESPONSE HEADERS are filtered to the CORS-safelisted set plus whatever
 *     Access-Control-Expose-Headers names.  Set-Cookie is never exposed.
 *   - `mode: 'no-cors'` yields an OPAQUE response: status 0, no headers, no
 *     body.  It is honest rather than useful, which is what opaque means.
 *   - A cross-origin REDIRECT taints the request: every later hop sends
 *     `Origin: null`, and each hop's response is checked on its own.
 *
 * WHAT THIS IS NOT: there is no CORS on the page-loading path (browser.c's
 * bfetch), because that is a navigation and navigations are not CORS requests.
 * There is no Timing-Allow-Origin, and preflights do not vary the cache on the
 * request headers beyond a subset test. */

enum { WF_MODE_CORS = 0, WF_MODE_SAME_ORIGIN, WF_MODE_NO_CORS };
enum { WF_CRED_SAME_ORIGIN = 0, WF_CRED_OMIT, WF_CRED_INCLUDE };

/* fetch spec, "CORS-safelisted request-header".  Range is deliberately absent:
 * its safelisting carries value-syntax conditions, and a header wrongly called
 * safe is a request that skips the preflight it needed. */
static int cors_safe_req_header(const char *name, const char *value)
{
    if (ci_streq(name, "accept") || ci_streq(name, "accept-language") ||
        ci_streq(name, "content-language")) return 1;
    if (ci_streq(name, "content-type")) {
        char t[64]; int o = 0;
        for (const char *p = value; *p && *p != ';' && o < (int)sizeof t - 1; p++) {
            if (*p == ' ' || *p == '\t') continue;
            t[o++] = (char)lc_c((unsigned char)*p);
        }
        t[o] = 0;
        return !strcmp(t, "application/x-www-form-urlencoded") ||
               !strcmp(t, "multipart/form-data") || !strcmp(t, "text/plain");
    }
    return 0;
}

/* Header names script may not set at all: the ones the user agent owns, and
 * the ones that would let a page forge its own identity.  `Cookie` is in here,
 * which is the point -- the jar decides what rides, not the page. */
static int forbidden_req_header(const char *n)
{
    static const char *const f[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length", "cookie",
        "cookie2", "date", "dnt", "expect", "host", "keep-alive", "origin",
        "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade",
        "via", 0
    };
    for (int i = 0; f[i]; i++) if (ci_streq(n, f[i])) return 1;
    return ci_strneq(n, "proxy-", 6) || ci_strneq(n, "sec-", 4);
}

/* fetch spec, "CORS-safelisted response-header". */
static int cors_safe_resp_header(const char *n)
{
    return ci_streq(n, "cache-control") || ci_streq(n, "content-language") ||
           ci_streq(n, "content-length") || ci_streq(n, "content-type") ||
           ci_streq(n, "expires") || ci_streq(n, "last-modified") ||
           ci_streq(n, "pragma");
}

/* The preflight cache.  Without it a React app that PATCHes on every keystroke
 * pays two round trips per keystroke. */
#define PFC_MAX 16
struct pfcache {
    int  used, creds;
    char origin[URL_HOST_MAX + 16];
    char method[H1_METHOD_MAX];
    char headers[256];                  /* lowercase comma list the server allowed */
    unsigned long long expires_ms;
};
static struct pfcache g_pfc[PFC_MAX];

static void pfc_store(const char *origin, const char *method, int creds,
                      const char *allow_hdrs, int max_age_s)
{
    if (max_age_s <= 0) return;
    if (max_age_s > 86400) max_age_s = 86400;         /* the spec's own ceiling */
    struct pfcache *slot = 0;
    for (int i = 0; i < PFC_MAX; i++) {
        struct pfcache *p = &g_pfc[i];
        if (p->used && p->creds == creds && ci_streq(p->origin, origin) &&
            ci_streq(p->method, method)) { slot = p; break; }
    }
    if (!slot) for (int i = 0; i < PFC_MAX; i++) if (!g_pfc[i].used) { slot = &g_pfc[i]; break; }
    if (!slot) slot = &g_pfc[0];                      /* evict slot 0; the table is a cache */
    memset(slot, 0, sizeof *slot);
    slot->used = 1; slot->creds = creds;
    scopy(slot->origin, origin, (int)sizeof slot->origin);
    scopy(slot->method, method, (int)sizeof slot->method);
    scopy(slot->headers, allow_hdrs ? allow_hdrs : "", (int)sizeof slot->headers);
    slot->expires_ms = now_ms() + (unsigned long long)max_age_s * 1000ull;
}

/* 1 if a live cache entry covers this exact request. */
static int pfc_hit(const char *origin, const char *method, int creds,
                   const struct h1_headers *author)
{
    for (int i = 0; i < PFC_MAX; i++) {
        struct pfcache *p = &g_pfc[i];
        if (!p->used || p->creds != creds) continue;
        if (!ci_streq(p->origin, origin) || !ci_streq(p->method, method)) continue;
        if (now_ms() > p->expires_ms) { p->used = 0; continue; }
        if (list_has(p->headers, "*")) return 1;
        for (int k = 0; k < author->n; k++) {
            if (cors_safe_req_header(author->v[k].name, author->v[k].value)) continue;
            if (!list_has(p->headers, author->v[k].name)) return 0;
        }
        return 1;
    }
    return 0;
}

/* ---- fetch ------------------------------------------------------------ */

#define WF_MAX        8            /* concurrent requests; TCP has 32 slots total */
#define WF_HOPS       10
/* An IDLE timeout, not a total one, and the difference is the whole feature: a
 * chat stream is a connection that sits open for minutes and produces a token
 * every few seconds.  A total budget would cut the conversation off mid-answer
 * at exactly the 30-second mark and look like a server fault. */
#define WF_TIMEOUT 30000ull        /* ms since the last byte; connect included */
#define WF_STEPS      8            /* h1_conn_pump calls per frame: 8 x 4 KiB */
/* An SSE response never completes, so there is no "finished" moment at which
 * to stop reading -- the only thing that keeps a fast producer from growing the
 * JS queue without bound is refusing to read more while the page is behind.
 * Declining to pump lets the socket buffer fill and the TCP window close, which
 * is backpressure that reaches the server rather than a counter that does not. */
#define WF_HIGHWATER (512*1024)

/* The negative control, and the only reason this knob exists.  Built with
 * -DWEBAPI_NO_STREAM the fetch registers no body sink and does not settle until
 * the message is complete -- exactly the behaviour this change replaced.
 * tests/unit/stream_test.c compiles BOTH ways and requires the streaming
 * assertions to fail in this one, because "the tokens arrived" is a claim about
 * WHEN, and a test that only checks the final text passes against a fully
 * buffered implementation just as well. */
#ifdef WEBAPI_NO_STREAM
#  define WEBAPI_STREAMING 0
#else
#  define WEBAPI_STREAMING 1
#endif

enum { WF_FREE = 0, WF_DIAL, WF_XFER };
enum { WF_PH_ACTUAL = 0, WF_PH_PREFLIGHT };
/* What to do with body bytes arriving right now.  UNKNOWN can only hold for
 * the remainder of ONE h1_conn_pump: fetch_step inspects the headers after
 * every single pump, so `hold` never needs more than one read's worth. */
enum { WF_DEL_UNKNOWN = 0, WF_DEL_JS, WF_DEL_DROP };

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
    JSContext *ctx;                /* the sink runs inside js_webapi_pump(ctx) */

    int   gen;                     /* an abort handle is (slot, generation) */
    int   mode, creds;
    int   cross;                   /* THIS hop is cross-origin */
    int   tainted;                 /* a cross-origin redirect happened */
    int   phase;                   /* WF_PH_* */
    int   deliver;                 /* WF_DEL_* */
    int   resolved;                /* the promise has settled with a Response */
    JSValue push, fin, fail;       /* the stream controller mkResponse handed back */
    long  queued;                  /* bytes the JS queue is holding */
    /* JS-observable work done in the current fetch_step.  It is what the step
     * RETURNS, and the return value is what makes the embedder drain the
     * microtask queue and repaint -- see the note in fetch_step. */
    int   js_work;
    uint8_t hold[4096];
    int   hold_len;
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

/* Hand `n` bytes to the page's ReadableStream and remember how deep its queue
 * got, which is the number backpressure is decided on. */
static int wf_push(struct wfetch *f, const uint8_t *p, int n)
{
    JSContext *ctx = f->ctx;
    if (!ctx || !JS_IsFunction(ctx, f->push)) return H1_OK;
    JSValue ab = JS_NewArrayBufferCopy(ctx, p, (size_t)n);
    JSValue r = JS_Call(ctx, f->push, JS_UNDEFINED, 1, (JSValueConst *)&ab);
    JS_FreeValue(ctx, ab);
    if (JS_IsException(r)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        return H1_OK;              /* a throwing consumer is not a protocol error */
    }
    int32_t q = 0;
    JS_ToInt32(ctx, &q, r);
    JS_FreeValue(ctx, r);
    f->queued = q;
    /* Enqueuing settles a pending reader's promise, and a settled promise is
     * only a QUEUED job until someone drains the microtask queue.  The embedder
     * drains it when this step reports work, so a push that did not count would
     * leave the page's .then() sitting in the queue until the next step that
     * did -- which for a stream that goes quiet is the end of the response. */
    f->js_work++;
    return H1_OK;
}

/* http1's body sink: called from inside the parse that consumed the bytes. */
static int wf_sink(void *ctx, const uint8_t *p, int n)
{
    struct wfetch *f = (struct wfetch *)ctx;
    if (f->deliver == WF_DEL_DROP) return H1_OK;
    if (f->deliver == WF_DEL_JS) return wf_push(f, p, n);
    if (n > (int)sizeof f->hold - f->hold_len) return H1_E_TOOLARGE;
    memcpy(f->hold + f->hold_len, p, (size_t)n);
    f->hold_len += n;
    return H1_OK;
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
        JS_FreeValue(ctx, f->push);
        JS_FreeValue(ctx, f->fin);
        JS_FreeValue(ctx, f->fail);
    }
    f->resolve = f->reject = JS_UNDEFINED;
    f->push = f->fin = f->fail = JS_UNDEFINED;
    f->resolved = 0; f->hold_len = 0; f->queued = 0;
    f->deliver = WF_DEL_UNKNOWN;
    f->state = WF_FREE;
    f->gen++;                      /* any abort handle still held is now stale */
    if (g_fetch_live > 0) g_fetch_live--;
}

/* Fail the request.  Before the promise settled that is a rejection; after it
 * settled (streaming) the promise is long gone and the failure belongs to the
 * body stream, which is exactly what a browser does when a connection dies
 * mid-download. */
static void fetch_fail(JSContext *ctx, struct wfetch *f, const char *msg, const char *name)
{
    char full[256];
    int o = 0;
    const char *pre = "fetch: ";
    for (const char *p = pre; *p && o < (int)sizeof full - 1; p++) full[o++] = *p;
    for (const char *p = msg; *p && o < (int)sizeof full - 1; p++) full[o++] = *p;
    full[o] = 0;
    printf("[webapi] %s\n", full);

    if (f->resolved) {
        if (JS_IsFunction(ctx, f->fail)) {
            JSValue a[2];
            a[0] = JS_NewString(ctx, full);
            a[1] = JS_NewString(ctx, name ? name : "TypeError");
            JSValue r = JS_Call(ctx, f->fail, JS_UNDEFINED, 2, (JSValueConst *)a);
            if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, r); JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
        }
        fetch_release(ctx, f);
        return;
    }
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name ? name : "TypeError"));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, full));
    JSValue r = JS_Call(ctx, f->reject, JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    fetch_release(ctx, f);
}

static void fetch_reject(JSContext *ctx, struct wfetch *f, const char *msg)
{ fetch_fail(ctx, f, msg, "TypeError"); }

/* The document's origin as an Origin header value. */
static void doc_origin(char *out, int max)
{
    if (g_loc_valid) wurl_origin(&g_loc, out, max);
    else scopy(out, "null", max);
}

/* 1 if this request may carry the user's credentials. */
static int wf_creds(const struct wfetch *f)
{
    if (f->creds == WF_CRED_OMIT) return 0;
    if (f->creds == WF_CRED_INCLUDE) return 1;
    return !f->cross;                       /* 'same-origin', the default */
}

/* Does the request need an OPTIONS preflight before it may be sent? */
static int wf_needs_preflight(const struct wfetch *f)
{
    if (!f->cross || f->mode != WF_MODE_CORS) return 0;
    if (!ci_streq(f->method, "GET") && !ci_streq(f->method, "HEAD") &&
        !ci_streq(f->method, "POST")) return 1;
    for (int i = 0; i < f->hdr.n; i++)
        if (!cors_safe_req_header(f->hdr.v[i].name, f->hdr.v[i].value)) return 1;
    return 0;
}

/* Serialize the current request and hand it to a fresh h1_conn.
 *
 * NOTE ON POOLING.  Every request here opens its own socket and says
 * `Connection: close`.  c/net/hpool.c does pool, and browser_rt.c's bfetch
 * rides it (one wikipedia page went 14 handshakes to 4) -- but bfetch's
 * interface is a GET and a URL.  For fetch() to share that pool bfetch would
 * have to grow a method, a request header list, a request body, access to the
 * response headers, and a body SINK so a streamed response is not buffered
 * into its cache.  That is bfetch's file and its call; it is written here so
 * the next person does not conclude the pool was forgotten. */
static int fetch_send_request(JSContext *ctx, struct wfetch *f)
{
    (void)ctx;
    struct h1_request q;
    char target[URL_PATH_MAX + WQ_MAX];
    wurl_target(&f->url, target, (int)sizeof target);
    int preflight = (f->phase == WF_PH_PREFLIGHT);
    if (h1_request_init(&q, preflight ? "OPTIONS" : f->method, target) != H1_OK) return -1;

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
    h1_request_set_header(&q, "Connection", "close");

    /* Origin.  Sent on every cross-origin request, and on any same-origin
     * request that is not a simple read -- that is what browsers do, and a
     * server's CSRF check depends on it.  A cross-origin redirect taints the
     * request and the value becomes `null`, so a server cannot be told the
     * request came from somewhere it did not. */
    char origin[URL_HOST_MAX + 16];
    doc_origin(origin, (int)sizeof origin);
    int send_origin = f->cross || preflight ||
                      (!ci_streq(f->method, "GET") && !ci_streq(f->method, "HEAD"));
    if (send_origin)
        h1_request_set_header(&q, "Origin", f->tainted ? "null" : origin);

    if (preflight) {
        h1_request_set_header(&q, "Access-Control-Request-Method", f->method);
        char names[512]; int o = 0;
        for (int i = 0; i < f->hdr.n; i++) {
            if (cors_safe_req_header(f->hdr.v[i].name, f->hdr.v[i].value)) continue;
            const char *nm = f->hdr.v[i].name;
            int nl = (int)strlen(nm);
            if (o + nl + 2 >= (int)sizeof names) break;
            if (o) { names[o++] = ','; }
            for (int k = 0; k < nl; k++) names[o++] = (char)lc_c((unsigned char)nm[k]);
        }
        names[o] = 0;
        if (o) h1_request_set_header(&q, "Access-Control-Request-Headers", names);
        h1_request_set_header(&q, "Accept", "*/*");
    } else {
        /* Cookies.  cookies.c decides which entries match the host and path;
         * the two things it cannot see are decided here: whether this request
         * is allowed credentials at all (CORS), and whether it is same-site
         * (SameSite).  A fetch is never a top-level navigation, so a Lax or
         * unattributed cookie does not ride a cross-site one. */
        if (wf_creds(f)) {
            struct cookie_ctx cc;
            ck_ctx(&cc, &f->url, 1);
            int cross_site = g_loc_valid ? !cookie_same_site(g_loc.host, f->url.host)
                                         : CK_REQ_CROSS_SITE;
            char cookie[2048];
            int n = cookie_header_ex(jar(), &cc, cross_site ? CK_REQ_CROSS_SITE
                                                            : CK_REQ_SAME_SITE,
                                     now_unix(), cookie, (int)sizeof cookie);
            if (n > 0) h1_request_set_header(&q, "Cookie", cookie);
        }
        for (int i = 0; i < f->hdr.n; i++)
            h1_request_add_header(&q, f->hdr.v[i].name, f->hdr.v[i].value);
        if (f->body && f->body_len > 0) h1_request_set_body(&q, f->body, f->body_len);
    }

    char *raw = 0; int rawlen = 0;
    int rc = h1_request_build(&q, &raw, &rawlen);
    h1_request_free(&q);
    if (rc != H1_OK || !raw) { free(raw); return -1; }

    struct h1_transport t = { tr_read, tr_write, 0, (void *)(long)f->fd };
    if (h1_conn_start(&f->conn, &t, raw, rawlen) != H1_OK) { free(raw); return -1; }
    f->started = 1;                       /* the conn owns `raw` from here */
    h1_response_head(&f->conn.resp, !preflight && ci_streq(f->method, "HEAD"));
    if (WEBAPI_STREAMING) h1_response_sink(&f->conn.resp, wf_sink, f);
    f->deliver = preflight ? WF_DEL_DROP : WF_DEL_UNKNOWN;
    f->hold_len = 0;
    return 0;
}

/* Dial the socket for f->url and arm the deadline. 0 ok. */
static int fetch_dial(struct wfetch *f)
{
    if (!g_net || !g_net->open) return -1;
    f->cross = g_loc_valid ? !same_origin(&f->url, &g_loc) : 1;
    f->fd = g_net->open(f->url.host, f->url.port, f->url.https);
    if (f->fd < 0) return -1;
    f->state = WF_DIAL;
    f->started = 0;
    f->deadline = now_ms() + WF_TIMEOUT;
    return 0;
}

/* Store whatever Set-Cookie headers this response carried.  Only a request
 * that was allowed to SEND credentials may SET them -- otherwise a page could
 * plant a cookie in a third-party origin's jar without that origin's opt-in. */
static void fetch_take_cookies(struct wfetch *f)
{
    if (!wf_creds(f)) return;
    struct h1_response *r = &f->conn.resp;
    struct cookie_ctx cc;
    ck_ctx(&cc, &f->url, 1);
    long long now = now_unix();
    int n = h1_headers_count(&r->hdr, "set-cookie");
    for (int i = 0; i < n; i++) {
        const char *v = h1_headers_nth(&r->hdr, "set-cookie", i);
        if (v) cookie_set(jar(), &cc, v, now);
    }
}

/* The CORS check on a response.  Returns NULL if the response may be used, or
 * the reason it may not.  Runs at headers-done, BEFORE the body stream exists,
 * so a refusal cannot have leaked a byte. */
static const char *cors_check_response(struct wfetch *f)
{
    if (!f->cross || f->mode != WF_MODE_CORS) return 0;
    struct h1_response *r = &f->conn.resp;
    char origin[URL_HOST_MAX + 16];
    doc_origin(origin, (int)sizeof origin);

    if (h1_headers_count(&r->hdr, "access-control-allow-origin") > 1)
        return "the server sent several Access-Control-Allow-Origin headers";
    const char *acao = h1_headers_get(&r->hdr, "access-control-allow-origin");
    if (!acao || !*acao)
        return "cross-origin request blocked: no Access-Control-Allow-Origin";

    int creds = wf_creds(f);
    if (creds) {
        const char *acac = h1_headers_get(&r->hdr, "access-control-allow-credentials");
        if (!acac || !ci_streq(acac, "true"))
            return "credentialed cross-origin request blocked: "
                   "Access-Control-Allow-Credentials is not true";
        /* `*` plus credentials is the combination the spec forbids outright:
         * it would mean "any origin may read this response", said about a
         * response that was generated with the user's session. */
        if (!strcmp(acao, "*"))
            return "credentialed cross-origin request blocked: "
                   "Access-Control-Allow-Origin is '*'";
    }
    if (strcmp(acao, "*") != 0 && !ci_streq(acao, origin))
        return "cross-origin request blocked: Access-Control-Allow-Origin "
               "does not name this origin";
    return 0;
}

/* The CORS check on a PREFLIGHT response.  Same origin rules, plus the method
 * and header allowances the preflight exists to ask about. */
static const char *cors_check_preflight(struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    const char *why = cors_check_response(f);
    if (why) return why;
    if (r->code < 200 || r->code > 299) return "the CORS preflight was not successful";

    int creds = wf_creds(f);

    /* The method.  `*` is a wildcard only for an uncredentialed request; the
     * three CORS-safelisted methods need no naming.  Anything else must be
     * listed, which is the whole reason a DELETE is preflighted. */
    const char *acam = h1_headers_get(&r->hdr, "access-control-allow-methods");
    if (!acam) acam = "";
    int m_safelisted = ci_streq(f->method, "GET") || ci_streq(f->method, "HEAD") ||
                       ci_streq(f->method, "POST");
    if (!m_safelisted && !list_has(acam, f->method) && !(list_has(acam, "*") && !creds))
        return "the CORS preflight did not allow this method";

    /* Every non-safelisted author header must be named (or wildcarded, again
     * only without credentials). */
    const char *acah = h1_headers_get(&r->hdr, "access-control-allow-headers");
    if (!acah) acah = "";
    int star = list_has(acah, "*") && !creds;
    for (int i = 0; i < f->hdr.n; i++) {
        if (cors_safe_req_header(f->hdr.v[i].name, f->hdr.v[i].value)) continue;
        if (star) continue;
        if (!list_has(acah, f->hdr.v[i].name))
            return "the CORS preflight did not allow a request header";
    }

    char origin[URL_HOST_MAX + 16];
    wurl_origin(&f->url, origin, (int)sizeof origin);
    const char *ma = h1_headers_get(&r->hdr, "access-control-max-age");
    int secs = 0;
    if (ma) { for (const char *p = ma; *p >= '0' && *p <= '9'; p++) secs = secs * 10 + (*p - '0'); }
    pfc_store(origin, f->method, creds, acah, secs);
    return 0;
}

/* May script see this response header? */
static int header_visible(struct wfetch *f, const char *name, const char *expose)
{
    /* Set-Cookie is never readable, at any origin -- that is what HttpOnly is
     * for and Headers.get() joining duplicates would break it anyway. */
    if (ci_streq(name, "set-cookie") || ci_streq(name, "set-cookie2")) return 0;
    if (!f->cross || f->mode != WF_MODE_CORS) return 1;
    if (cors_safe_resp_header(name)) return 1;
    if (!expose) return 0;
    if (list_has(expose, "*") && !wf_creds(f)) return 1;
    return list_has(expose, name);
}

/* Headers are complete: settle the promise with a Response whose body is a
 * stream the network has not finished filling.  This is the point the fetch
 * spec resolves at, and the reason a token-by-token endpoint works at all. */
static int fetch_deliver_headers(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;

    fetch_take_cookies(f);

    const char *why = cors_check_response(f);
    if (why) { f->deliver = WF_DEL_DROP; fetch_reject(ctx, f, why); return 1; }

    int opaque = (f->cross && f->mode == WF_MODE_NO_CORS);
    const char *expose = h1_headers_get(&r->hdr, "access-control-expose-headers");

    JSValue pairs = JS_NewArray(ctx);
    uint32_t np = 0;
    if (!opaque) {
        for (int i = 0; i < r->hdr.n; i++) {
            if (!header_visible(f, r->hdr.v[i].name, expose)) continue;
            JSValue p = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, p, 0, JS_NewString(ctx, r->hdr.v[i].name));
            JS_SetPropertyUint32(ctx, p, 1, JS_NewString(ctx, r->hdr.v[i].value));
            JS_SetPropertyUint32(ctx, pairs, np++, p);
        }
    }

    char href[WURL_MAX];
    wurl_href(&f->url, href, (int)sizeof href);

    JSValue argv[7];
    argv[0] = JS_NewInt32(ctx, opaque ? 0 : r->code);
    argv[1] = JS_NewString(ctx, opaque ? "" : r->reason);
    argv[2] = pairs;
    argv[3] = JS_NewString(ctx, opaque ? "" : href);
    argv[4] = JS_NewBool(ctx, f->redirected);
    argv[5] = JS_NewString(ctx, opaque ? "opaque" : (f->cross ? "cors" : "basic"));
    argv[6] = JS_NewBool(ctx, opaque || r->no_body);
    JSValue hooks = JS_Call(ctx, g_mk_response, JS_UNDEFINED, 7, (JSValueConst *)argv);
    for (int i = 0; i < 7; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(hooks)) {
        JS_FreeValue(ctx, hooks);
        JS_FreeValue(ctx, JS_GetException(ctx));
        f->deliver = WF_DEL_DROP;
        fetch_reject(ctx, f, "could not build the Response");
        return 1;
    }

    JSValue resp = JS_GetPropertyStr(ctx, hooks, "r");
    f->push = JS_GetPropertyStr(ctx, hooks, "push");
    f->fin  = JS_GetPropertyStr(ctx, hooks, "close");
    f->fail = JS_GetPropertyStr(ctx, hooks, "error");
    JS_FreeValue(ctx, hooks);

    f->resolved = 1;
    f->deliver = opaque ? WF_DEL_DROP : WF_DEL_JS;

    /* Whatever the same read already parsed out of the body. */
    if (f->hold_len && f->deliver == WF_DEL_JS) wf_push(f, f->hold, f->hold_len);
    f->hold_len = 0;

    JSValue rv = JS_Call(ctx, f->resolve, JS_UNDEFINED, 1, (JSValueConst *)&resp);
    if (JS_IsException(rv)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, rv);
    JS_FreeValue(ctx, resp);
    f->js_work++;
    return 1;
}

/* The message finished.  A streamed body is already in the page's hands and
 * only needs closing; a buffered one (Content-Encoding: gzip, which cannot be
 * inflated incrementally) is decoded and handed over in one piece here. */
static int fetch_finish(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    if (!h1_response_streaming(r) && f->deliver == WF_DEL_JS) {
        if (h1_decode_body(r) != H1_OK) {
            fetch_fail(ctx, f, "response body could not be decoded (Content-Encoding)",
                       "TypeError");
            return 1;
        }
        if (r->body && r->body_len > 0) wf_push(f, r->body, r->body_len);
    }
    if (JS_IsFunction(ctx, f->fin)) {
        JSValue v = JS_Call(ctx, f->fin, JS_UNDEFINED, 0, 0);
        if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, v);
    }
    fetch_release(ctx, f);
    return 1;
}

/* Re-dial for the next hop (a redirect, or the real request after a
 * preflight). 0 ok. */
static int fetch_redial(struct wfetch *f)
{
    g_net->close(f->fd); f->fd = -1;
    h1_conn_free(&f->conn);
    memset(&f->conn, 0, sizeof f->conn);
    f->started = 0;
    f->hold_len = 0;
    f->deliver = WF_DEL_UNKNOWN;
    return fetch_dial(f);
}

/* A 3xx with a Location: re-target and re-dial. 1 if the request continues. */
static int fetch_redirect(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    const char *loc = h1_headers_get(&r->hdr, "location");
    if (!loc || !*loc || f->hops >= WF_HOPS) return 0;

    struct wurl next;
    if (wurl_parse(loc, &f->url, &next) != 0) return 0;

    /* A redirect that leaves the origin taints the request: from here on the
     * server is told `Origin: null`, so it cannot be led to believe the
     * request came from the origin it started at. */
    if (!same_origin(&next, &f->url)) f->tainted = 1;
    if (f->mode == WF_MODE_SAME_ORIGIN && g_loc_valid && !same_origin(&next, &g_loc)) {
        fetch_reject(ctx, f, "mode 'same-origin' forbids a cross-origin redirect");
        return 1;
    }

    char m[H1_METHOD_MAX]; int drop = 0;
    if (h1_redirect_method(r->code, f->method, m, (int)sizeof m, &drop) != H1_OK) return 0;
    scopy(f->method, m, H1_METHOD_MAX);
    if (drop) {
        free(f->body); f->body = 0; f->body_len = 0;
        h1_headers_remove(&f->hdr, "content-type");
        h1_headers_remove(&f->hdr, "content-length");
    }

    f->url = next;
    f->hops++;
    f->redirected = 1;
    f->phase = WF_PH_ACTUAL;
    if (fetch_redial(f) != 0) { fetch_reject(ctx, f, "redirect target could not be opened"); return 1; }
    /* The new hop may be cross-origin even though the first was not. */
    if (wf_needs_preflight(f) ) {
        char origin[URL_HOST_MAX + 16];
        wurl_origin(&f->url, origin, (int)sizeof origin);
        if (!pfc_hit(origin, f->method, wf_creds(f), &f->hdr)) f->phase = WF_PH_PREFLIGHT;
    }
    return 1;
}

/* One frame's work for one request.  Returns how much JS-observable work it
 * did, which is NOT bookkeeping: js_page_run_due() drains the microtask queue
 * and the embedder repaints only when this is non-zero.  A step that settles a
 * promise or enqueues a body chunk and then reports 0 leaves the page's
 * .then() queued -- and for a stream that goes quiet between tokens, "the next
 * step that reports work" is the end of the response.  That is exactly how this
 * streamed correctly at the C level and still delivered every token at once on
 * the device; the host tests could not see it because their loop drains jobs
 * unconditionally every frame. */
static int fetch_step(JSContext *ctx, struct wfetch *f)
{
    f->ctx = ctx;
    f->js_work = 0;
    if (now_ms() > f->deadline) { fetch_fail(ctx, f, "timed out", "TypeError"); return 1; }

    /* Backpressure: while the page is behind, stop reading.  The socket buffer
     * fills, the window closes, and the producer slows down. */
    if (f->resolved && f->queued > WF_HIGHWATER) return 0;

    int bits = g_net->poll(f->fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) { fetch_fail(ctx, f, "connection failed", "TypeError"); return 1; }
    if (!(bits & SOCK_P_CONNECTED)) return 0;             /* DNS/TCP/TLS still running */

    if (!f->started) {
        if (fetch_send_request(ctx, f) != 0) { fetch_reject(ctx, f, "request could not be built"); return 1; }
        f->state = WF_XFER;
    }

    for (int i = 0; i < WF_STEPS; i++) {
        if (f->state == WF_FREE) return 1;    /* released underneath us */
        int64_t before = f->conn.resp.body_seen + f->conn.resp.hdr_bytes;
        int st = h1_conn_pump(&f->conn);
        /* Any progress re-arms the idle timeout. */
        if (f->conn.resp.body_seen + f->conn.resp.hdr_bytes != before)
            f->deadline = now_ms() + WF_TIMEOUT;
        if (st == H1_C_ERROR) {
            fetch_fail(ctx, f, h1_strerror(f->conn.err), "TypeError");
            return 1;
        }

        /* Inspect the headers the moment they are complete -- and after EVERY
         * pump, which is what bounds `hold` to one read's worth. */
        if (WEBAPI_STREAMING &&
            f->phase == WF_PH_ACTUAL && !f->resolved && f->deliver == WF_DEL_UNKNOWN &&
            h1_response_headers_done(&f->conn.resp)) {
            struct h1_response *r = &f->conn.resp;
            int will_redirect = h1_is_redirect(r->code) &&
                                h1_headers_get(&r->hdr, "location") && f->hops < WF_HOPS;
            if (will_redirect) {
                fetch_take_cookies(f);       /* a login redirect sets its cookie here */
                f->deliver = WF_DEL_DROP;    /* the 3xx body is not the answer */
                f->hold_len = 0;
            } else if (fetch_deliver_headers(ctx, f)) {
                if (f->state == WF_FREE) return 1;      /* rejected and released */
            }
        }

        if (st == H1_C_DONE) {
            if (f->phase == WF_PH_PREFLIGHT) {
                const char *why = cors_check_preflight(f);
                if (why) { fetch_reject(ctx, f, why); return 1; }
                f->phase = WF_PH_ACTUAL;
                if (fetch_redial(f) != 0) { fetch_reject(ctx, f, "could not reopen after the preflight"); return 1; }
                return f->js_work;
            }
            if (h1_is_redirect(f->conn.resp.code) && fetch_redirect(ctx, f))
                return f->state == WF_FREE ? 1 : f->js_work;
            if (!f->resolved) {
                /* No body state was ever entered (a bodyless status we did not
                 * catch above): deliver now. */
                if (fetch_deliver_headers(ctx, f) && f->state == WF_FREE) return 1;
            }
            return fetch_finish(ctx, f);
        }
    }
    return f->js_work;
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

/* An abort handle names both the slot and the generation that occupied it, so
 * a handle held past the request's death cannot cancel whatever took the slot
 * next. */
static int wf_handle(const struct wfetch *f)
{ return (int)((f - g_fetch) * 4096 + (f->gen & 4095)); }

/* __fetchStart(url, method, headerPairs, body, opts) -> { p: Promise, h: id } */
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
        int gen = f->gen;                    /* the generation must survive the wipe */
        memset(f, 0, sizeof *f);
        f->gen = gen;
        f->fd = -1;
        f->url = u;
        f->resolve = f->reject = JS_UNDEFINED;
        f->push = f->fin = f->fail = JS_UNDEFINED;
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
                 * so header injection from a page's init dies here.  A FORBIDDEN
                 * name is dropped on top of that: the user agent owns Cookie,
                 * Host and Origin, and a page that could set them could forge
                 * its own identity to the server. */
                if (k && v && !forbidden_req_header(k)) h1_headers_add(&f->hdr, k, -1, v, -1);
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
        /* mode + credentials.  Unrecognised values fall back to the defaults
         * the spec gives them, which are the strict ones. */
        f->mode = WF_MODE_CORS; f->creds = WF_CRED_SAME_ORIGIN;
        if (argc > 4 && JS_IsObject(argv[4])) {
            JSValue mv = JS_GetPropertyStr(ctx, argv[4], "mode");
            const char *m = JS_IsUndefined(mv) ? 0 : JS_ToCString(ctx, mv);
            if (m) {
                if (!strcmp(m, "same-origin")) f->mode = WF_MODE_SAME_ORIGIN;
                else if (!strcmp(m, "no-cors")) f->mode = WF_MODE_NO_CORS;
                JS_FreeCString(ctx, m);
            }
            JS_FreeValue(ctx, mv);
            JSValue cv = JS_GetPropertyStr(ctx, argv[4], "credentials");
            const char *c = JS_IsUndefined(cv) ? 0 : JS_ToCString(ctx, cv);
            if (c) {
                if (!strcmp(c, "include")) f->creds = WF_CRED_INCLUDE;
                else if (!strcmp(c, "omit")) f->creds = WF_CRED_OMIT;
                JS_FreeCString(ctx, c);
            }
            JS_FreeValue(ctx, cv);
        }
        if (fetch_dial(f) != 0) { h1_headers_free(&f->hdr); free(f->body); f->body = 0; msg = "could not open a socket"; }
        else if (f->mode == WF_MODE_SAME_ORIGIN && f->cross) {
            g_net->close(f->fd); f->fd = -1;
            h1_headers_free(&f->hdr); free(f->body); f->body = 0;
            msg = "mode 'same-origin' forbids a cross-origin request";
        } else if (wf_needs_preflight(f)) {
            char origin[URL_HOST_MAX + 16];
            wurl_origin(&f->url, origin, (int)sizeof origin);
            if (!pfc_hit(origin, f->method, wf_creds(f), &f->hdr))
                f->phase = WF_PH_PREFLIGHT;
        }
    }
    if (us) JS_FreeCString(ctx, us);

    JSValue out = JS_NewObject(ctx);
    if (msg) {
        /* A rejected promise, not a throw: fetch() rejects, it does not raise. */
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, "TypeError"));
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
        JSValue r = JS_Call(ctx, rf[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, r); JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, rf[0]); JS_FreeValue(ctx, rf[1]);
        if (f) f->state = WF_FREE;
        JS_SetPropertyStr(ctx, out, "p", promise);
        JS_SetPropertyStr(ctx, out, "h", JS_NewInt32(ctx, -1));
        return out;
    }

    f->resolve = rf[0];
    f->reject  = rf[1];
    f->ctx = ctx;
    g_fetch_live++;
    JS_SetPropertyStr(ctx, out, "p", promise);
    JS_SetPropertyStr(ctx, out, "h", JS_NewInt32(ctx, wf_handle(f)));
    return out;
}

/* __fetchAbort(handle) -- a REAL cancellation.  bd1f53d gave the socket ABI a
 * close, so this closes the connection rather than merely refusing to look at
 * what arrives: the transfer stops on the wire and the slot is freed. */
static JSValue js_fetch_abort(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t h = -1;
    if (argc > 0) JS_ToInt32(ctx, &h, argv[0]);
    if (h < 0) return JS_FALSE;
    int slot = h / 4096, gen = h % 4096;
    if (slot < 0 || slot >= WF_MAX) return JS_FALSE;
    struct wfetch *f = &g_fetch[slot];
    if (f->state == WF_FREE || (f->gen & 4095) != gen) return JS_FALSE;
    f->ctx = ctx;
    fetch_fail(ctx, f, "aborted", "AbortError");
    return JS_TRUE;
}

/* ---- a delay the pump owns --------------------------------------------
 * EventSource has to wait `retry` milliseconds before reconnecting, and it has
 * to do so in the host unit tests too -- where js_page.c (and therefore
 * setTimeout) is not linked at all.  This is NOT a second event loop: the queue
 * is drained from js_webapi_pump, on the same clock the sockets are stepped
 * with, so a delay cannot fire between two steps of anything. */
#define WT_MAX 8
struct wtimer { int used; unsigned long long due; JSValue fn; };
static struct wtimer g_timers[WT_MAX];

static JSValue js_later(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_NewInt32(ctx, -1);
    int32_t ms = 0;
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;
    for (int i = 0; i < WT_MAX; i++) {
        if (g_timers[i].used) continue;
        g_timers[i].used = 1;
        g_timers[i].due = now_ms() + (unsigned long long)ms;
        g_timers[i].fn = JS_DupValue(ctx, argv[1]);
        return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

static JSValue js_cancel_later(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t id = -1;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= WT_MAX || !g_timers[id].used) return JS_FALSE;
    g_timers[id].used = 0;
    JS_FreeValue(ctx, g_timers[id].fn);
    g_timers[id].fn = JS_UNDEFINED;
    return JS_TRUE;
}

static int timers_run(JSContext *ctx)
{
    int ran = 0;
    unsigned long long t = now_ms();
    for (int i = 0; i < WT_MAX; i++) {
        if (!g_timers[i].used || g_timers[i].due > t) continue;
        JSValue fn = g_timers[i].fn;
        g_timers[i].used = 0;
        g_timers[i].fn = JS_UNDEFINED;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
        ran++;
    }
    return ran;
}

static int timers_live(void)
{ for (int i = 0; i < WT_MAX; i++) if (g_timers[i].used) return 1; return 0; }

static void timers_clear(JSContext *ctx)
{
    for (int i = 0; i < WT_MAX; i++) {
        if (!g_timers[i].used) continue;
        g_timers[i].used = 0;
        if (ctx) JS_FreeValue(ctx, g_timers[i].fn);
        g_timers[i].fn = JS_UNDEFINED;
    }
}

/* ---- document.cookie ---------------------------------------------------
 * The same jar the network uses, minus HttpOnly -- which is the whole point of
 * HttpOnly, and is enforced inside cookies.c by the http_api flag rather than
 * by filtering afterwards here. */
static JSValue js_cookie_get(JSContext *ctx, JSValueConst t)
{
    (void)t;
    if (!g_loc_valid) return JS_NewString(ctx, "");
    struct cookie_ctx cc;
    ck_ctx(&cc, &g_loc, 0);
    char buf[4096];
    int n = cookie_header(jar(), &cc, now_unix(), buf, (int)sizeof buf);
    return JS_NewString(ctx, n > 0 ? buf : "");
}

static JSValue js_cookie_set(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    (void)t;
    if (!g_loc_valid) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    struct cookie_ctx cc;
    ck_ctx(&cc, &g_loc, 0);
    cookie_set(jar(), &cc, s, now_unix());   /* a refusal is the normal outcome */
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
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

/* ---- non-special schemes ------------------------------------------------
 *
 * `struct wurl` carries `https` as a BOOL, so it can express http and https
 * and nothing else, and `new URL(x, "x:/")` therefore threw. That is not a
 * corner case -- it is in the runtime of every webpack 5 bundle that has an
 * asset module:
 *
 *     var u = new URL(s, "x:/"), a = {}; ... a.pathname = ...
 *
 * an idiom that uses a deliberately meaningless scheme to normalise a path
 * without a document base. MEASURED on the MDN fixture: five of its twelve
 * remaining exceptions were this one call, one per lazily-imported Web
 * Component, each reported by the page as "couldn't load code for <switch>".
 * Real Chrome throws nothing.
 *
 * The URL CONSTRUCTOR gains the general case; fetch does not, and must not.
 * A non-special URL has no host, no port and an opaque origin, so there is
 * nothing for a socket to connect to -- keeping this out of struct wurl is
 * what stops `fetch("x:/whatever")` from becoming reachable. */
static int scheme_of(const char *s, char *out, int max)
{
    int i = 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) return 0;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
           (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    if (s[i] != ':' || i >= max) return 0;
    for (int k = 0; k < i; k++) out[k] = (char)lc_c((unsigned char)s[k]);
    out[i] = 0;
    return i;
}

static int is_special_scheme(const char *sc)
{ return ci_streq(sc, "http") || ci_streq(sc, "https") || ci_streq(sc, "ftp") ||
         ci_streq(sc, "ws")   || ci_streq(sc, "wss")   || ci_streq(sc, "file"); }

/* Build the JS object for a non-special URL. Everything host-shaped is empty
 * and the origin is the string "null", which is what the platform reports for
 * an opaque origin -- not the empty string, and not the base's. */
static JSValue nonspecial_url(JSContext *ctx, const char *scheme, const char *rest)
{
    char path[WURL_MAX], search[WQ_MAX], hash[WH_MAX];
    search[0] = hash[0] = 0;
    const char *q = 0, *f = 0;
    for (const char *p = rest; *p; p++) {
        if (*p == '?' && !q && !f) q = p;
        else if (*p == '#' && !f) { f = p; break; }
    }
    const char *pend = q ? q : (f ? f : rest + strlen(rest));
    int n = (int)(pend - rest);
    if (n >= WURL_MAX) n = WURL_MAX - 1;
    memcpy(path, rest, (size_t)n); path[n] = 0;
    if (q) { const char *qe = f ? f : rest + strlen(rest);
             int qn = (int)(qe - q); if (qn >= WQ_MAX) qn = WQ_MAX - 1;
             memcpy(search, q, (size_t)qn); search[qn] = 0; }
    if (f) scopy(hash, f, WH_MAX);
    if (path[0] == '/') remove_dot_segments(path);

    char href[WURL_MAX];
    int o = 0;
    for (const char *s = scheme; *s && o < WURL_MAX - 2; s++) href[o++] = *s;
    href[o++] = ':';
    for (const char *s = path;   *s && o < WURL_MAX - 1; s++) href[o++] = *s;
    for (const char *s = search; *s && o < WURL_MAX - 1; s++) href[o++] = *s;
    for (const char *s = hash;   *s && o < WURL_MAX - 1; s++) href[o++] = *s;
    href[o] = 0;

    char proto[24];
    o = 0;
    for (const char *s = scheme; *s && o < 22; s++) proto[o++] = *s;
    proto[o++] = ':'; proto[o] = 0;

    JSValue ob = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ob, "href", JS_NewString(ctx, href));
    JS_SetPropertyStr(ctx, ob, "protocol", JS_NewString(ctx, proto));
    JS_SetPropertyStr(ctx, ob, "origin", JS_NewString(ctx, "null"));
    JS_SetPropertyStr(ctx, ob, "host", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, ob, "hostname", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, ob, "port", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, ob, "pathname", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, ob, "search", JS_NewString(ctx, search));
    JS_SetPropertyStr(ctx, ob, "hash", JS_NewString(ctx, hash));
    return ob;
}

/* Merge `ref` onto a non-special base path, RFC 3986 section 5.3. */
static JSValue nonspecial_resolve(JSContext *ctx, const char *scheme,
                                  const char *basepath, const char *ref)
{
    char merged[WURL_MAX];
    if (ref[0] == '/' || !basepath[0]) {
        scopy(merged, ref, WURL_MAX);
    } else if (!ref[0] || ref[0] == '?' || ref[0] == '#') {
        int o = 0;
        for (const char *s = basepath; *s && o < WURL_MAX - 1; s++) merged[o++] = *s;
        for (const char *s = ref;      *s && o < WURL_MAX - 1; s++) merged[o++] = *s;
        merged[o] = 0;
    } else {
        int keep = (int)strlen(basepath);
        while (keep > 0 && basepath[keep - 1] != '/') keep--;
        int o = 0;
        for (int i = 0; i < keep && o < WURL_MAX - 1; i++) merged[o++] = basepath[i];
        for (const char *s = ref; *s && o < WURL_MAX - 1; s++) merged[o++] = *s;
        merged[o] = 0;
    }
    return nonspecial_url(ctx, scheme, merged);
}

/* __urlParse(input, base|undefined) -> {href, protocol, ...} or null */
static JSValue js_url_parse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_NULL;
    const char *in = JS_ToCString(ctx, argv[0]);
    if (!in) return JS_NULL;

    /* Non-special schemes are decided BEFORE wurl_parse, on the input's own
     * scheme when it has one and otherwise on the base's, because that is the
     * order the URL standard resolves them in. */
    /* The negative control, and the only reason this knob exists. Built with
     * -DWEBAPI_NO_NONSPECIAL_URL the constructor behaves exactly as it did
     * before -- `new URL(x, "x:/")` throws -- and
     * tests/unit/webapi_test.c's four non-special assertions must FAIL.
     * `make test-webapi-url-negctl`. */
#ifndef WEBAPI_NO_NONSPECIAL_URL
    {
        char sc[24];
        if (scheme_of(in, sc, sizeof sc) && !is_special_scheme(sc)) {
            JSValue r = nonspecial_url(ctx, sc, in + strlen(sc) + 1);
            JS_FreeCString(ctx, in);
            return r;
        }
        if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
            const char *b = JS_ToCString(ctx, argv[1]);
            char bsc[24];
            if (b && scheme_of(b, bsc, sizeof bsc) && !is_special_scheme(bsc)) {
                /* strip the base's own query/fragment before merging */
                char bp[WURL_MAX];
                scopy(bp, b + strlen(bsc) + 1, WURL_MAX);
                for (char *p = bp; *p; p++) if (*p == '?' || *p == '#') { *p = 0; break; }
                JSValue r = nonspecial_resolve(ctx, bsc, bp, in);
                JS_FreeCString(ctx, b);
                JS_FreeCString(ctx, in);
                return r;
            }
            if (b) JS_FreeCString(ctx, b);
        }
    }
#endif

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

/* `new Storage()` is a TypeError on the platform: the interface object exists
 * to be referenced and to be the right-hand side of `instanceof`, not to be
 * constructed. Throwing is the behaviour, and it also means a page cannot make
 * an object that answers to `instanceof Storage` and is not one. */
static JSValue storage_illegal_ctor(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

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
"(function (__fetchStart, __utf8, __urlParse, __mediaMatch, __fetchAbort, __later, __cancelLater) {\n"
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

/* ---- streams ----
 * A real ReadableStream, because Response.body has to be one and because a
 * chat reply is a stream before it is a string. The queue is a plain array and
 * a byte count; the byte count is what C reads back from push() to decide
 * whether to keep reading the socket, so it is load-bearing rather than
 * decorative. */
"function chLen(c) { return c == null ? 0 : (c.byteLength !== undefined ? c.byteLength : (c.length || 0)); }\n"
"function rsPut(s, ch) {\n"
"  if (s._st !== 'readable') return;\n"
"  if (s._w.length) { s._w.shift()[0]({ value: ch, done: false }); return; }\n"
"  s._q.push(ch); s._qb += chLen(ch);\n"
"}\n"
"function rsEnd(s) {\n"
"  if (s._st !== 'readable') return;\n"
"  s._st = 'closed';\n"
"  while (s._w.length) s._w.shift()[0]({ value: undefined, done: true });\n"
"}\n"
"function rsErr(s, e) {\n"
"  if (s._st !== 'readable') { return; }\n"
"  s._st = 'errored'; s._e = e;\n"
"  while (s._w.length) s._w.shift()[1](e);\n"
"  s._q = []; s._qb = 0;\n"
"}\n"
"function rsPull(s) { if (typeof s._src.pull === 'function') { try { s._src.pull(s._c); } catch (e) { rsErr(s, e); } } }\n"
"function rsRead(s) {\n"
"  if (s._q.length) { var c = s._q.shift(); s._qb -= chLen(c); if (s._qb < 0) s._qb = 0;\n"
"    rsPull(s); return Promise.resolve({ value: c, done: false }); }\n"
"  if (s._st === 'closed') return Promise.resolve({ value: undefined, done: true });\n"
"  if (s._st === 'errored') return Promise.reject(s._e);\n"
"  return new Promise(function (res, rej) { s._w.push([res, rej]); rsPull(s); });\n"
"}\n"
"function rsCancel(s, reason) {\n"
"  if (s._st === 'readable') { s._st = 'closed';\n"
"    while (s._w.length) s._w.shift()[0]({ value: undefined, done: true }); }\n"
"  s._q = []; s._qb = 0;\n"
"  if (typeof s._src.cancel === 'function') { try { s._src.cancel(reason); } catch (e) {} }\n"
"  return Promise.resolve();\n"
"}\n"
"G.ReadableStream = function ReadableStream(src) {\n"
"  var s = this;\n"
"  s._src = src || {}; s._q = []; s._qb = 0; s._st = 'readable'; s._e = undefined;\n"
"  s._w = []; s._locked = false;\n"
"  s._c = {\n"
"    enqueue: function (ch) { rsPut(s, ch); },\n"
"    close: function () { rsEnd(s); },\n"
"    error: function (e) { rsErr(s, e); },\n"
"    bytes: function () { return s._qb; },\n"
"    get desiredSize() { return 65536 - s._qb; }\n"
"  };\n"
"  if (typeof s._src.start === 'function') s._src.start(s._c);\n"
"};\n"
"G.ReadableStream.prototype = {\n"
"  constructor: G.ReadableStream,\n"
"  get locked() { return this._locked; },\n"
"  getReader: function () {\n"
"    if (this._locked) throw new TypeError('ReadableStream is locked');\n"
"    var s = this; s._locked = true;\n"
"    return {\n"
"      read: function () { return rsRead(s); },\n"
"      cancel: function (r) { s._locked = false; return rsCancel(s, r); },\n"
"      releaseLock: function () { s._locked = false; },\n"
"      closed: new Promise(function () {})\n"
"    };\n"
"  },\n"
"  cancel: function (r) { return rsCancel(this, r); },\n"
"  tee: function () {\n"
"    var cs = [null, null];\n"
"    var out = [new G.ReadableStream({ start: function (c) { cs[0] = c; } }),\n"
"               new G.ReadableStream({ start: function (c) { cs[1] = c; } })];\n"
"    var rd = this.getReader();\n"
"    (function loop() {\n"
"      rd.read().then(function (r) {\n"
"        if (r.done) { cs[0].close(); cs[1].close(); return; }\n"
"        cs[0].enqueue(r.value); cs[1].enqueue(r.value); loop();\n"
"      }, function (e) { cs[0].error(e); cs[1].error(e); });\n"
"    })();\n"
"    return out;\n"
"  },\n"
"  pipeTo: function (w) {\n"
"    var rd = this.getReader(), wr = w.getWriter();\n"
"    return new Promise(function (res, rej) {\n"
"      (function loop() {\n"
"        rd.read().then(function (r) {\n"
"          if (r.done) { Promise.resolve(wr.close()).then(res, rej); return; }\n"
"          Promise.resolve(wr.write(r.value)).then(loop, rej);\n"
"        }, function (e) { try { wr.abort(e); } catch (x) {} rej(e); });\n"
"      })();\n"
"    });\n"
"  },\n"
"  pipeThrough: function (t) { this.pipeTo(t.writable); return t.readable; }\n"
"};\n"
"if (typeof Symbol !== 'undefined' && Symbol.asyncIterator) {\n"
"  G.ReadableStream.prototype[Symbol.asyncIterator] = function () {\n"
"    var rd = this.getReader();\n"
"    return { next: function () { return rd.read(); },\n"
"             'return': function () { rd.cancel(); return Promise.resolve({ done: true }); },\n"
"             '@@asyncIterator': function () { return this; } };\n"
"  };\n"
"}\n"
"G.WritableStream = function WritableStream(sink) { this._sink = sink || {}; this._locked = false; };\n"
"G.WritableStream.prototype = {\n"
"  constructor: G.WritableStream,\n"
"  get locked() { return this._locked; },\n"
"  getWriter: function () {\n"
"    var s = this; s._locked = true;\n"
"    return {\n"
"      write: function (c) { return Promise.resolve(s._sink.write ? s._sink.write(c) : undefined); },\n"
"      close: function () { return Promise.resolve(s._sink.close ? s._sink.close() : undefined); },\n"
"      abort: function (e) { return Promise.resolve(s._sink.abort ? s._sink.abort(e) : undefined); },\n"
"      releaseLock: function () { s._locked = false; },\n"
"      ready: Promise.resolve(), closed: Promise.resolve()\n"
"    };\n"
"  }\n"
"};\n"
"G.TransformStream = function TransformStream(t) {\n"
"  t = t || {};\n"
"  var rc = null;\n"
"  this.readable = new G.ReadableStream({ start: function (c) { rc = c; } });\n"
"  var tc = { enqueue: function (c) { rc.enqueue(c); },\n"
"             terminate: function () { rc.close(); },\n"
"             error: function (e) { rc.error(e); } };\n"
"  this.writable = new G.WritableStream({\n"
"    write: function (chunk) { return Promise.resolve(t.transform ? t.transform(chunk, tc) : tc.enqueue(chunk)); },\n"
"    close: function () { return Promise.resolve(t.flush ? t.flush(tc) : undefined).then(function () { rc.close(); }); },\n"
"    abort: function (e) { rc.error(e); }\n"
"  });\n"
"  if (typeof t.start === 'function') t.start(tc);\n"
"};\n"

/* ---- bytes <-> text ----
 * TextDecoder is INCREMENTAL, and that is not a nicety: a UTF-8 sequence split
 * across two TCP reads is the normal case for a token stream, and a decoder
 * that does not carry the tail turns a Chinese reply into replacement
 * characters at every read boundary. The actual decode is __utf8 (QuickJS's
 * own UTF-8 reader); what is written here is only the part that decides which
 * trailing bytes are not a whole character yet. */
"function toU8(x) {\n"
"  if (x === null || x === undefined) return new Uint8Array(0);\n"
"  if (x instanceof Uint8Array) return x;\n"
"  if (x instanceof ArrayBuffer) return new Uint8Array(x);\n"
"  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);\n"
"  if (typeof x === 'string') return new G.TextEncoder().encode(x);\n"
"  return new Uint8Array(0);\n"
"}\n"
"function u8ab(u) { return u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength); }\n"
"function joinParts(parts) {\n"
"  var a = [], n = 0, i;\n"
"  for (i = 0; i < parts.length; i++) { a.push(toU8(parts[i])); n += a[i].length; }\n"
"  var out = new Uint8Array(n), o = 0;\n"
"  for (i = 0; i < a.length; i++) { out.set(a[i], o); o += a[i].length; }\n"
"  return out.buffer;\n"
"}\n"
/* How many trailing bytes of `b` are an incomplete UTF-8 sequence? */
"function utf8Tail(b) {\n"
"  var n = b.length, i, need;\n"
"  for (i = 1; i <= 4 && i <= n; i++) {\n"
"    var c = b[n - i];\n"
"    if (c < 0x80) return 0;\n"
"    if ((c & 0xC0) === 0x80) continue;\n"
"    if ((c & 0xE0) === 0xC0) need = 2;\n"
"    else if ((c & 0xF0) === 0xE0) need = 3;\n"
"    else if ((c & 0xF8) === 0xF0) need = 4;\n"
"    else return 0;\n"
"    return i < need ? i : 0;\n"
"  }\n"
"  return 0;\n"
"}\n"
"G.TextEncoder = function TextEncoder() { this.encoding = 'utf-8'; };\n"
"G.TextEncoder.prototype.encode = function (s) {\n"
"  s = String(s === undefined ? '' : s);\n"
"  var out = [], i, c, d;\n"
"  for (i = 0; i < s.length; i++) {\n"
"    c = s.charCodeAt(i);\n"
"    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {\n"
"      d = s.charCodeAt(i + 1);\n"
"      if (d >= 0xDC00 && d <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i++; }\n"
"    }\n"
"    if (c < 0x80) out.push(c);\n"
"    else if (c < 0x800) out.push(0xC0 | (c >> 6), 0x80 | (c & 63));\n"
"    else if (c < 0x10000) out.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));\n"
"    else out.push(0xF0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));\n"
"  }\n"
"  return new Uint8Array(out);\n"
"};\n"
"G.TextDecoder = function TextDecoder(enc) { this.encoding = 'utf-8'; this._t = null; };\n"
"G.TextDecoder.prototype.decode = function (input, opts) {\n"
"  var stream = !!(opts && opts.stream);\n"
"  var b = toU8(input);\n"
"  if (this._t && this._t.length) {\n"
"    var m = new Uint8Array(this._t.length + b.length);\n"
"    m.set(this._t, 0); m.set(b, this._t.length); b = m; this._t = null;\n"
"  }\n"
"  var keep = stream ? utf8Tail(b) : 0;\n"
"  if (keep) { this._t = b.slice(b.length - keep); b = b.subarray(0, b.length - keep); }\n"
"  if (!b.length) return '';\n"
"  return __utf8(u8ab(b));\n"
"};\n"
"G.TextDecoderStream = function TextDecoderStream() {\n"
"  var dec = new G.TextDecoder();\n"
"  var t = new G.TransformStream({\n"
"    transform: function (chunk, c) { var s = dec.decode(chunk, { stream: true }); if (s) c.enqueue(s); },\n"
"    flush: function (c) { var s = dec.decode(new Uint8Array(0)); if (s) c.enqueue(s); }\n"
"  });\n"
"  this.readable = t.readable; this.writable = t.writable;\n"
"};\n"
"G.TextEncoderStream = function TextEncoderStream() {\n"
"  var enc = new G.TextEncoder();\n"
"  var t = new G.TransformStream({\n"
"    transform: function (chunk, c) { c.enqueue(enc.encode(chunk)); }\n"
"  });\n"
"  this.readable = t.readable; this.writable = t.writable;\n"
"};\n"

/* ---- AbortController ----
 * A real cancellation now that the socket ABI has a close: abort() shuts the
 * connection, it does not merely stop looking at what arrives. */
"function abortError() { var e = new Error('The operation was aborted.'); e.name = 'AbortError'; return e; }\n"
"G.AbortSignal = function AbortSignal() { this.aborted = false; this.reason = undefined;\n"
"  this.onabort = null; this._l = []; };\n"
"G.AbortSignal.prototype = {\n"
"  constructor: G.AbortSignal,\n"
"  addEventListener: function (t, f) { if (t === 'abort' && typeof f === 'function') this._l.push(f); },\n"
"  removeEventListener: function (t, f) { var i = this._l.indexOf(f); if (i >= 0) this._l.splice(i, 1); },\n"
"  throwIfAborted: function () { if (this.aborted) throw this.reason; },\n"
"  dispatchEvent: function () { return true; }\n"
"};\n"
"G.AbortController = function AbortController() { this.signal = new G.AbortSignal(); };\n"
"G.AbortController.prototype.abort = function (reason) {\n"
"  var s = this.signal;\n"
"  if (s.aborted) return;\n"
"  s.aborted = true;\n"
"  s.reason = reason !== undefined ? reason : abortError();\n"
"  var e = { type: 'abort', target: s };\n"
"  if (typeof s.onabort === 'function') s.onabort(e);\n"
"  s._l.slice().forEach(function (f) { f.call(s, e); });\n"
"};\n"

/* ---- Response ----
 * The body is a ReadableStream in every case, including the one where C
 * already had all the bytes: one shape means text()/json() cannot accidentally
 * work only for buffered responses. */
"function rsOf(chunk) { return new G.ReadableStream({ start: function (c) { c.enqueue(chunk); c.close(); } }); }\n"
"G.Response = function Response(body, init) {\n"
"  init = init || {};\n"
"  this.status = init.status === undefined ? 200 : init.status | 0;\n"
"  this.statusText = init.statusText === undefined ? '' : String(init.statusText);\n"
"  this.headers = init.headers instanceof G.Headers ? init.headers : new G.Headers(init.headers);\n"
"  this.url = init.url || '';\n"
"  this.redirected = !!init.redirected;\n"
"  this.type = init.type || 'basic';\n"
"  this.ok = this.status >= 200 && this.status < 300;\n"
"  this.bodyUsed = false;\n"
"  if (body === undefined || body === null) this.body = null;\n"
"  else if (body instanceof G.ReadableStream) this.body = body;\n"
"  else this.body = rsOf(toU8(body));\n"
"};\n"
"G.Response.prototype = {\n"
"  constructor: G.Response,\n"
"  _drain: function () {\n"
"    if (this.bodyUsed) return Promise.reject(new TypeError('body already read'));\n"
"    this.bodyUsed = true;\n"
"    var b = this.body;\n"
"    if (!b) return Promise.resolve([]);\n"
"    var rd = b.getReader(), parts = [];\n"
"    return new Promise(function (res, rej) {\n"
"      (function loop() {\n"
"        rd.read().then(function (r) {\n"
"          if (r.done) { res(parts); return; }\n"
"          parts.push(r.value); loop();\n"
"        }, rej);\n"
"      })();\n"
"    });\n"
"  },\n"
"  arrayBuffer: function () { return this._drain().then(joinParts); },\n"
"  text: function () { return this._drain().then(function (p) {\n"
"    var ab = joinParts(p); return ab.byteLength ? __utf8(ab) : ''; }); },\n"
"  json: function () { return this.text().then(function (t) { return JSON.parse(t); }); },\n"
"  blob: function () { return this.arrayBuffer(); },\n"
"  clone: function () {\n"
"    if (this.bodyUsed) throw new TypeError('body already read');\n"
"    var t = this.body ? this.body.tee() : [null, null];\n"
"    this.body = t[0];\n"
"    return new G.Response(t[1], { status: this.status, statusText: this.statusText,\n"
"      headers: this.headers, url: this.url, redirected: this.redirected, type: this.type });\n"
"  }\n"
"};\n"
"G.Response.error = function () { var r = new G.Response(null, { status: 0 }); r.type = 'error'; return r; };\n"
"G.Response.json = function (v, init) { init = init || {};\n"
"  var h = new G.Headers(init.headers); if (!h.has('content-type')) h.set('content-type', 'application/json');\n"
"  return new G.Response(JSON.stringify(v), { status: init.status, statusText: init.statusText, headers: h }); };\n"

/* ---- fetch ---- */
"function pairsOf(o) { var p = [], k; for (k in o) p.push([k, o[k]]); return p; }\n"
/* data: URLs. A fetch of one must never reach the socket: there is no host to
 * connect to, and before this it went through url parsing, came out as a
 * hostname of "text/plain;base64,..." and failed DNS -- which is a confusing
 * way to say "this URL contains its own answer".
 *
 * It matters beyond neatness because it is the transport for
 * URL.createObjectURL (js_platform.c): that returns a data: URL rather than a
 * blob: one precisely so that the thing it returns can be dereferenced, and a
 * fetch that could not read one would make that a lie.
 *
 * The response is a real Response, built through the same ReadableStream path
 * as a network one -- so `await (await fetch(u)).text()` behaves identically --
 * with status 200 and the declared Content-Type. RFC 2397: an omitted type is
 * text/plain;charset=US-ASCII, and ;base64 is the only supported encoding. */
"function dataURL(url) {\n"
"  var comma = url.indexOf(',');\n"
"  if (comma < 0) return null;\n"
"  var meta = url.slice(5, comma), payload = url.slice(comma + 1);\n"
"  var b64 = false;\n"
"  if (/;base64$/i.test(meta)) { b64 = true; meta = meta.slice(0, -7); }\n"
"  var type = meta || 'text/plain;charset=US-ASCII';\n"
"  var bytes;\n"
"  if (b64) {\n"
"    var tbl = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
"    var clean = payload.replace(/[^A-Za-z0-9+/]/g, ''), out = [], i, n, k;\n"
"    for (i = 0; i + 1 < clean.length; i += 4) {\n"
"      n = 0; k = 0;\n"
"      for (var j = 0; j < 4 && i + j < clean.length; j++) { n = (n << 6) | tbl.indexOf(clean[i + j]); k++; }\n"
"      n <<= (4 - k) * 6;\n"
"      out.push((n >> 16) & 255);\n"
"      if (k > 2) out.push((n >> 8) & 255);\n"
"      if (k > 3) out.push(n & 255);\n"
"    }\n"
"    bytes = new Uint8Array(out);\n"
"  } else {\n"
     /* percent-decoding then UTF-8 encoding, so a %C3%A9 in the payload is one
        character and two bytes, not two characters. */
"    var s;\n"
"    try { s = decodeURIComponent(payload); } catch (e) { s = payload; }\n"
"    bytes = new G.TextEncoder().encode(s);\n"
"  }\n"
"  var ctrl = null;\n"
"  var stream = new G.ReadableStream({ start: function (c) { ctrl = c; } });\n"
"  var r = new G.Response(stream, { status: 200, statusText: 'OK',\n"
"    headers: [['content-type', type], ['content-length', String(bytes.length)]],\n"
"    url: url, redirected: false, type: 'basic' });\n"
"  ctrl.enqueue(bytes);\n"
"  ctrl.close();\n"
"  return r;\n"
"}\n"
/* ---- the request queue ----
 * WHY: WF_MAX is 8, and before this a ninth concurrent fetch was REJECTED with
 * `TypeError: too many requests in flight`. That is not what a browser does
 * and it is not a limit a page can be expected to respect -- bing's own script
 * loader fires a burst of a dozen, and eight of them arriving and four failing
 * leaves the page in a state it has no code for. tests/qmp/qmp_bing.py is
 * where that showed up: the fixture served 13 resources and the serial log
 * filled with the rejection.
 *
 * So requests past the limit WAIT instead. The limit is 6 rather than WF_MAX
 * so a slot is always left for something the browser itself needs (a
 * stylesheet, an image) rather than a page's telemetry burst taking all of
 * them -- and because the kernel's socket table is the real scarce resource
 * underneath.
 *
 * The queue is FIFO, which is the only ordering a page can reason about; an
 * abort while queued rejects immediately and never dials. */
"var FQ_MAX = 6;\n"
"var fqLive = 0, fqQ = [];\n"
"function fqStart(e) {\n"
"  fqLive++;\n"
"  var st = __fetchStart(e.url, e.method, e.pairs, e.body, e.opts);\n"
"  if (e.sig && typeof e.sig.addEventListener === 'function')\n"
"    e.sig.addEventListener('abort', function () { __fetchAbort(st.h); });\n"
"  if (e.sig && e.sig.aborted) __fetchAbort(st.h);\n"
"  var done = function () { fqLive--; fqDrain(); };\n"
"  st.p.then(function (v) { done(); e.res(v); }, function (x) { done(); e.rej(x); });\n"
"}\n"
"function fqDrain() {\n"
"  while (fqQ.length && fqLive < FQ_MAX) {\n"
"    var e = fqQ.shift();\n"
"    if (e.cancelled) continue;\n"
"    fqStart(e);\n"
"  }\n"
"}\n"
"function fqEnqueue(url, method, pairs, body, opts, sig) {\n"
"  return new Promise(function (res, rej) {\n"
"    var e = { url: url, method: method, pairs: pairs, body: body, opts: opts,\n"
"              sig: sig, res: res, rej: rej, cancelled: false };\n"
"    if (fqLive < FQ_MAX) { fqStart(e); return; }\n"
"    if (sig && typeof sig.addEventListener === 'function')\n"
"      sig.addEventListener('abort', function () {\n"
"        if (e.cancelled) return;\n"
"        e.cancelled = true;\n"
"        rej(sig.reason || abortError());\n"
"      });\n"
"    fqQ.push(e);\n"
"  });\n"
"}\n"
"G.fetch = function fetch(input, init) {\n"
"  init = init || {};\n"
"  var url = (input && typeof input === 'object' && input.url) ? input.url : String(input);\n"
"  if (url.slice(0, 5).toLowerCase() === 'data:') {\n"
"    var dr = dataURL(url);\n"
"    return dr ? Promise.resolve(dr)\n"
"              : Promise.reject(new TypeError('Failed to fetch: malformed data: URL'));\n"
"  }\n"
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
"  var opts = { mode: init.mode, credentials: init.credentials };\n"
"  var sig = init.signal || (input && input.signal);\n"
"  if (sig && typeof sig === 'object' && sig.aborted)\n"
"    return Promise.reject(sig.reason || abortError());\n"
"  return fqEnqueue(url, method, pairs, body, opts, sig);\n"
"};\n"
"G.Request = function Request(input, init) {\n"
"  init = init || {};\n"
"  this.url = (input && typeof input === 'object' && input.url) ? input.url : String(input);\n"
"  this.method = String(init.method || (input && input.method) || 'GET').toUpperCase();\n"
"  this.headers = new G.Headers(init.headers || (input && input.headers));\n"
"  this.body = init.body === undefined ? null : init.body;\n"
"  this.mode = init.mode || 'cors'; this.credentials = init.credentials || 'same-origin';\n"
"  this.signal = init.signal || null;\n"
"};\n"

/* ---- EventSource + the text/event-stream framing ----
 * This is the reason the whole streaming path exists. The framing is small and
 * every one of its rules is one that a naive split-on-newline implementation
 * gets wrong: a field split across two reads, a `data:` with no space, several
 * data lines joined with \n rather than concatenated, the blank line being the
 * only thing that dispatches, and an event with an empty data buffer being
 * dropped rather than delivered. The parser therefore buffers across feeds and
 * holds a trailing CR back, because a CR at the end of a read may still turn
 * out to be the first half of a CRLF. */
"function sseParser(onEvent, es) {\n"
"  var buf = '', data = '', type = '', id = '';\n"
"  function endLine(l) {\n"
"    if (l === '') {\n"
"      if (es) es._lastId = id;\n"
"      if (data === '') { type = ''; return; }\n"
"      if (data.charAt(data.length - 1) === '\\n') data = data.slice(0, -1);\n"
"      onEvent({ type: type || 'message', data: data, lastEventId: id });\n"
"      data = ''; type = '';\n"
"      return;\n"
"    }\n"
"    if (l.charAt(0) === ':') return;\n"
"    var i = l.indexOf(':'), f, v;\n"
"    if (i < 0) { f = l; v = ''; }\n"
"    else { f = l.slice(0, i); v = l.slice(i + 1); if (v.charAt(0) === ' ') v = v.slice(1); }\n"
"    if (f === 'data') data += v + '\\n';\n"
"    else if (f === 'event') type = v;\n"
"    else if (f === 'id') { if (v.indexOf('\\u0000') < 0) id = v; }\n"
"    else if (f === 'retry') { if (/^[0-9]+$/.test(v) && es) es._retry = parseInt(v, 10); }\n"
"  }\n"
"  return { feed: function (text) {\n"
"    buf += text;\n"
"    for (;;) {\n"
"      var i = -1, j, ch;\n"
"      for (j = 0; j < buf.length; j++) { ch = buf.charAt(j); if (ch === '\\n' || ch === '\\r') { i = j; break; } }\n"
"      if (i < 0) break;\n"
"      var adv = 1;\n"
"      if (buf.charAt(i) === '\\r') {\n"
"        if (i + 1 >= buf.length) return;\n"
"        if (buf.charAt(i + 1) === '\\n') adv = 2;\n"
"      }\n"
"      var l = buf.slice(0, i);\n"
"      buf = buf.slice(i + adv);\n"
"      endLine(l);\n"
"    }\n"
"  } };\n"
"}\n"
"G.EventSource = function EventSource(url, cfg) {\n"
"  cfg = cfg || {};\n"
"  this.url = String(url);\n"
"  this.withCredentials = !!cfg.withCredentials;\n"
"  this.readyState = 0;\n"
"  this.onopen = null; this.onmessage = null; this.onerror = null;\n"
"  this._l = {}; this._retry = 3000; this._lastId = ''; this._h = -1; this._timer = -1;\n"
"  this._closed = false;\n"
"  this._connect();\n"
"};\n"
"G.EventSource.CONNECTING = 0; G.EventSource.OPEN = 1; G.EventSource.CLOSED = 2;\n"
"G.EventSource.prototype = {\n"
"  constructor: G.EventSource,\n"
"  addEventListener: function (t, f) { if (typeof f === 'function') (this._l[t] = this._l[t] || []).push(f); },\n"
"  removeEventListener: function (t, f) { var l = this._l[t]; if (!l) return;\n"
"    var i = l.indexOf(f); if (i >= 0) l.splice(i, 1); },\n"
"  dispatchEvent: function () { return true; },\n"
"  close: function () {\n"
"    this._closed = true; this.readyState = 2;\n"
"    if (this._h >= 0) { __fetchAbort(this._h); this._h = -1; }\n"
"    if (this._timer >= 0) { __cancelLater(this._timer); this._timer = -1; }\n"
"  },\n"
"  _emit: function (type, ev) {\n"
"    ev.type = type; ev.target = this; ev.currentTarget = this;\n"
"    var h = this['on' + type];\n"
"    if (typeof h === 'function') h.call(this, ev);\n"
"    (this._l[type] || []).slice().forEach(function (f) { f.call(this, ev); }, this);\n"
"  },\n"
   /* A dropped connection is not a failure: the spec says reconnect after the
      server's `retry` interval, carrying Last-Event-ID so the stream resumes
      where it stopped. A wrong content-type or a non-2xx IS a failure and
      must not retry, or a 404 becomes an infinite request loop. */
"  _retryLater: function () {\n"
"    if (this._closed) return;\n"
"    this.readyState = 0;\n"
"    this._emit('error', {});\n"
"    var self = this;\n"
"    this._timer = __later(this._retry, function () {\n"
"      self._timer = -1; if (!self._closed) self._connect(); });\n"
"  },\n"
"  _fatal: function () {\n"
"    this.readyState = 2; this._closed = true; this._h = -1;\n"
"    this._emit('error', {});\n"
"  },\n"
"  _connect: function () {\n"
"    var self = this;\n"
"    var hs = { 'Accept': 'text/event-stream' };\n"
"    if (this._lastId) hs['Last-Event-ID'] = this._lastId;\n"
"    var st = __fetchStart(this.url, 'GET', pairsOf(hs), null,\n"
"                          { credentials: this.withCredentials ? 'include' : 'same-origin' });\n"
"    this._h = st.h;\n"
"    st.p.then(function (r) {\n"
"      if (self._closed) return;\n"
"      var ct = String(r.headers.get('content-type') || '').toLowerCase();\n"
"      if (!r.ok || ct.indexOf('text/event-stream') < 0) { self._fatal(); return; }\n"
"      self.readyState = 1;\n"
"      self._emit('open', {});\n"
"      var origin = '';\n"
"      try { origin = new G.URL(self.url, G.location && G.location.href).origin; } catch (e) {}\n"
"      var parser = sseParser(function (ev) {\n"
"        self._lastId = ev.lastEventId;\n"
"        self._emit(ev.type, { data: ev.data, lastEventId: ev.lastEventId, origin: origin });\n"
"      }, self);\n"
"      if (!r.body) { self._h = -1; self._retryLater(); return; }\n"
"      var rd = r.body.getReader(), dec = new G.TextDecoder();\n"
"      (function loop() {\n"
"        rd.read().then(function (c) {\n"
"          if (self._closed) return;\n"
"          if (c.done) { self._h = -1; self._retryLater(); return; }\n"
"          parser.feed(dec.decode(c.value, { stream: true }));\n"
"          loop();\n"
"        }, function () { if (!self._closed) { self._h = -1; self._retryLater(); } });\n"
"      })();\n"
"    }, function () { if (!self._closed) { self._h = -1; self._retryLater(); } });\n"
"  }\n"
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
 * Async only. abort() is now a REAL abort: it aborts the AbortController the
 * send() started with, which closes the socket, so the transfer stops on the
 * wire rather than merely stopping being delivered. And because the body now
 * arrives in pieces, readyState 3 and `progress` are real events with real
 * partial responseText behind them, not a pair fired back to back once
 * everything had already been buffered. */
"G.XMLHttpRequest = function XMLHttpRequest() {\n"
"  this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = '';\n"
"  this.response = ''; this.responseType = ''; this.responseURL = ''; this.timeout = 0;\n"
"  this.withCredentials = false; this.upload = {};\n"
"  this.onreadystatechange = null; this.onload = null; this.onerror = null;\n"
"  this.onloadend = null; this.onabort = null; this.ontimeout = null; this.onprogress = null;\n"
"  this._h = []; this._hdr = null; this._ev = {}; this._aborted = false; this._ac = null;\n"
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
"  abort: function () { this._aborted = true;\n"
"    if (this._ac) { try { this._ac.abort(); } catch (e) {} }\n"
"    this.readyState = 0; this._fire('abort'); this._fire('loadend'); },\n"
"  _fire: function (t) { var e = { type: t, target: this, currentTarget: this };\n"
"    var h = this['on' + t]; if (typeof h === 'function') h.call(this, e);\n"
"    (this._ev[t] || []).slice().forEach(function (f) { f.call(this, e); }, this); },\n"
"  _rs: function (s) { this.readyState = s; this._fire('readystatechange'); },\n"
"  send: function (body) {\n"
"    var self = this;\n"
"    self._ac = new G.AbortController();\n"
"    G.fetch(this._u, { method: this._m || 'GET', headers: this._h, body: body,\n"
"                       signal: self._ac.signal,\n"
"                       credentials: this.withCredentials ? 'include' : 'same-origin' })\n"
"      .then(function (r) {\n"
"        if (self._aborted) return null;\n"
"        self._hdr = r.headers; self.status = r.status; self.statusText = r.statusText;\n"
"        self.responseURL = r.url; self._rs(2);\n"
"        if (!r.body) return '';\n"
"        var rd = r.body.getReader(), dec = new G.TextDecoder(), text = '';\n"
"        return new Promise(function (res, rej) {\n"
"          (function loop() {\n"
"            rd.read().then(function (c) {\n"
"              if (self._aborted) { res(null); return; }\n"
"              if (c.done) { text += dec.decode(new Uint8Array(0)); res(text); return; }\n"
"              text += dec.decode(c.value, { stream: true });\n"
"              self.responseText = text;\n"
"              if (self.responseType !== 'json') self.response = text;\n"
"              if (self.readyState !== 3) self._rs(3);\n"
"              self._fire('progress');\n"
"              loop();\n"
"            }, rej);\n"
"          })();\n"
"        });\n"
"      })\n"
"      .then(function (t) {\n"
"        if (self._aborted || t === null) return;\n"
"        self.responseText = t;\n"
"        if (self.responseType === 'json') { try { self.response = JSON.parse(t); } catch (e) { self.response = null; } }\n"
"        else self.response = t;\n"
"        if (self.readyState !== 3) self._rs(3);\n"
"        self._rs(4);\n"
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
   /* Called when the HEADERS arrive, not when the body does. C keeps the
      three functions handed back and drives the body through them, which is
      what makes the response a stream the page can read from while the
      network is still writing to it. */
"  mkResponse: function (status, statusText, pairs, url, redirected, type, nobody) {\n"
"    var ctrl = null;\n"
"    var stream = new G.ReadableStream({ start: function (c) { ctrl = c; } });\n"
"    var r = new G.Response(nobody ? null : stream, { status: status, statusText: statusText,\n"
"      headers: pairs, url: url, redirected: redirected, type: type });\n"
"    return { r: r,\n"
"      push: function (buf) { ctrl.enqueue(new Uint8Array(buf)); return ctrl.bytes(); },\n"
"      close: function () { ctrl.close(); },\n"
   /* The failure that arrives AFTER the promise settled belongs to the body
      stream -- which is what a browser does when a connection dies (or is
      aborted) mid-download. The name is carried through so an abort reads as
      an AbortError to the page and not as a generic network failure. */
"      error: function (m, n) { var e = new Error(m); e.name = n || 'TypeError';\n"
"        ctrl.error(e); } };\n"
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
        if (JS_IsObject(doc)) {
            JS_SetPropertyStr(ctx, doc, "location", JS_DupValue(ctx, loc));
            /* document.cookie: the same jar the network uses, minus HttpOnly.
             * An accessor pair rather than a data property, because a page
             * writes it as `document.cookie = "a=1"` and expects the write to
             * ADD to the jar, not replace the string. */
            JSAtom a = JS_NewAtom(ctx, "cookie");
            JSValue get = JS_NewCFunction2(ctx, (JSCFunction *)js_cookie_get, "get cookie",
                                           0, JS_CFUNC_getter, 0);
            JSValue set = JS_NewCFunction2(ctx, (JSCFunction *)js_cookie_set, "set cookie",
                                           1, JS_CFUNC_setter, 0);
            JS_DefinePropertyGetSet(ctx, doc, a, get, set, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, a);
        }
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
        JS_SetClassProto(ctx, storage_cid, JS_DupValue(ctx, proto));
        JS_SetPropertyStr(ctx, g, "localStorage", make_storage(ctx, origin, 0));
        JS_SetPropertyStr(ctx, g, "sessionStorage", make_storage(ctx, origin, 1));

        /* window.Storage, the INTERFACE OBJECT.  Two working stores and no
         * constructor is not a small omission -- it is how kimi.com stops.
         *
         * MEASURED (tests/unit/webapi_probe.c over the captured fixture, with
         * the module graph walked): kimi's entry module rejects with
         * `ReferenceError: 'Storage' is not defined`, thrown from useStorage
         * inside its framework chunk -- VueUse's useLocalStorage, whose
         * availability check is a bare reference to the interface. Real
         * headless Chrome throws nothing at all on the same bytes, so this was
         * ours and only ours. The whole application failed to boot over a name
         * that was never published.
         *
         * The prototype is the same object the instances already share, so
         * `localStorage instanceof Storage` is true -- which is the other half
         * of what feature detection does with it. Calling it throws, as every
         * interface object must: `new Storage()` is illegal on the platform. */
        JSValue ctor = JS_NewCFunction2(ctx, storage_illegal_ctor, "Storage", 0,
                                        JS_CFUNC_constructor, 0);
        JS_SetConstructor(ctx, ctor, proto);
        JS_SetPropertyStr(ctx, g, "Storage", ctor);
        JS_FreeValue(ctx, proto);
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
    JSValue args[7];
    args[0] = JS_NewCFunction(ctx, js_fetch_start, "__fetchStart", 5);
    args[1] = JS_NewCFunction(ctx, js_utf8, "__utf8", 1);
    args[2] = JS_NewCFunction(ctx, js_url_parse, "__urlParse", 2);
    args[3] = JS_NewCFunction(ctx, js_media_match, "__mediaMatch", 1);
    args[4] = JS_NewCFunction(ctx, js_fetch_abort, "__fetchAbort", 1);
    args[5] = JS_NewCFunction(ctx, js_later, "__later", 2);
    args[6] = JS_NewCFunction(ctx, js_cancel_later, "__cancelLater", 1);
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, 7, (JSValueConst *)args);
    for (int i = 0; i < 7; i++) JS_FreeValue(ctx, args[i]);
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
    timers_clear(ctx);
    /* The cookie jar and the preflight cache are NOT cleared: both outlive the
     * page for the same reason Storage does. A session that evaporated on
     * every navigation would not be a session. */
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
{ return g_fetch_live > 0 || g_popstate_queued || g_hashchange_queued || g_vp_dirty ||
         timers_live(); }

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

    /* EventSource's reconnect delay. Last, so a reconnection scheduled by a
     * stream that ended during THIS pump waits at least one more frame. */
    ran += timers_run(ctx);
    return ran;
}
