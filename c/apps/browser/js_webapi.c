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
/* One persistence implementation, like storage_backend.c below. */
#include "cookie_persistence.c"
#include "url.h"
/* bxfer_*: the transport beneath this file, which chooses HTTP/1.1 or HTTP/2
 * from what ALPN returned. Drop-in for h1_conn_start/pump/free plus the socket
 * open/close; see bfetch.h. Under -DWEBAPI_HOST they are the h1_conn calls. */
#include "bfetch.h"
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
/* The dial and the release go through bxfer, not straight to the socket ABI,
 * because HTTP/2 makes a connection a property of the ORIGIN rather than of a
 * request: concurrent fetches to one host share one socket and many streams,
 * so the handle is refcounted and is not this file's to close. bxfer_open is
 * also where "h2,http/1.1" is offered -- the protocol is chosen in the TLS
 * handshake, before anyone here has a socket to have an opinion about. */
static int  d_open(const char *h, int p, int tls) { return bxfer_open(h, p, tls); }
static int  d_poll(int fd) { return sock_poll(fd); }
static int  d_send(int fd, const void *b, int n) { return sock_send(fd, b, n); }
static int  d_recv(int fd, void *b, int n) { return sock_recv(fd, b, n); }
static void d_close(int fd) { bxfer_close(fd); }
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
/* Serve GETs from a directory when WEBAPI_FILE_ROOT names one. Host only, and
 * off unless that variable is set -- see the header of js_filenet.inc for what
 * it is for and why it is not a fixture in a test file. */
#include "js_filenet.inc"
#endif

static const struct webapi_net *g_net = &g_default_net;

#ifdef WEBAPI_HOST
/* Chosen once, on the first use, so that a harness which installs its own net
 * before the first fetch still wins and nothing reads the environment twice. */
static int g_net_env_checked;
static void net_env_init(void)
{
    if (g_net_env_checked) return;
    g_net_env_checked = 1;
    if (g_net != &g_default_net) return;            /* someone installed one */
    const char *root = getenv("WEBAPI_FILE_ROOT");
    if (root && *root) { g_fnet_root = root; g_net = &g_fnet_net; }
}
#else
#define net_env_init() ((void)0)
#endif

void js_webapi_set_net(const struct webapi_net *n)
{
#ifdef WEBAPI_HOST
    g_net_env_checked = 1;                          /* an explicit net beats the env */
#endif
    g_net = n ? n : &g_default_net;
}
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
/* This service still has one live top-level realm (tabs dehydrate). Do not
 * silently overwrite it when a future frame caller tries to install another:
 * DOM, history and JS hooks are not yet per-realm. Refusal preserves the real
 * boundary; it is not a substitute for implementing multi-realm ownership. */
/* Correction: this is now the selected document slot. The guard still
 * refuses duplicate installation within one slot; sibling slots coexist. */
static JSContext *g_webapi_ctx;
static int  g_loc_valid;
static char g_loc_raw[WURL_MAX] = "about:blank";   /* what we were handed, parseable or not */
static char g_pending_nav[WURL_MAX];
static int  g_have_pending_nav;
/* The default slot uses its document origin. Embedded slots must carry an
 * explicit ancestor-derived site (opaque by default), never a mutable URL. */
static int g_embedded_site;
static char g_site_url[WURL_MAX];

#define WT_MAX 8
struct wtimer { int used; unsigned long long due; JSValue fn; };
/* One native policy engine, many owning realms. The page's location is not a
 * worker's base or authority: Blob has an opaque base but a creator origin.
 * No JSValue in this record is borrowed from a different runtime. */
struct fetch_realm {
    struct fetch_realm *next;
    JSContext *ctx;
    struct wurl base, origin, site;
    int base_valid, origin_valid, site_valid, stopped;
    unsigned identity;
    int (*connect_policy)(void *, const char *);
    void *connect_policy_opaque;
    JSValue mk_response, mk_error;
    struct wtimer timers[WT_MAX];
};
static struct fetch_realm *g_fetch_realms, *g_page_fetch;
static unsigned g_fetch_realm_serial;
static struct fetch_realm *fetch_realm_for(JSContext *ctx)
{
    for(struct fetch_realm *r=g_fetch_realms;r;r=r->next)
        if(r->ctx==ctx)return r;
    return NULL;
}
static struct fetch_realm *fetch_realm_new(JSContext *ctx, const char *base,
                                         const char *origin, const char *site)
{
    if(!ctx||fetch_realm_for(ctx))return NULL;
    if(g_fetch_realm_serial==~0u){JS_ThrowRangeError(ctx,"fetch realm identity exhausted");return NULL;}
    struct fetch_realm *r=calloc(1,sizeof *r);
    if(!r){JS_ThrowOutOfMemory(ctx);return NULL;}
    r->ctx=ctx;r->mk_response=r->mk_error=JS_UNDEFINED;
    r->identity=++g_fetch_realm_serial;
    r->base_valid=base&&wurl_parse(base,NULL,&r->base)==0;
    r->origin_valid=origin&&wurl_parse(origin,NULL,&r->origin)==0;
    r->site_valid=site&&wurl_parse(site,NULL,&r->site)==0;
    r->next=g_fetch_realms;g_fetch_realms=r;
    return r;
}

int js_webapi_set_connect_policy(JSContext *ctx, int (*allow)(void *, const char *), void *opaque)
{
    struct fetch_realm *r=fetch_realm_for(ctx);
    if(!r || r->stopped)return 0;
    r->connect_policy=allow;r->connect_policy_opaque=opaque;return 1;
}
static int fetch_policy_allows(struct fetch_realm *r, const struct wurl *url)
{
    if(!r->connect_policy)return 1;
    char href[WURL_MAX];wurl_href(url,href,sizeof href);
    return r->connect_policy(r->connect_policy_opaque,href);
}

static void set_location(const char *url)
{
    scopy(g_loc_raw, url && *url ? url : "about:blank", WURL_MAX);
    g_loc_valid = (wurl_parse(g_loc_raw, 0, &g_loc) == 0);
    if (!g_loc_valid) memset(&g_loc, 0, sizeof g_loc);
    if(g_page_fetch){
        g_page_fetch->base=g_loc;
        g_page_fetch->base_valid=g_loc_valid;
    }
}

int js_webapi_take_navigation(char *out, int max)
{
    if (!g_have_pending_nav) return 0;
    scopy(out, g_pending_nav, max);
    g_have_pending_nav = 0;
    return 1;
}

static void queue_navigation(const char *absolute_url)
{
    scopy(g_pending_nav, absolute_url, WURL_MAX);
    g_have_pending_nav = 1;
}

int js_webapi_request_navigation(const char *absolute_url)
{
    struct wurl parsed;
    if (!absolute_url || wurl_parse(absolute_url, 0, &parsed) != 0) return 0;
    char want[WURL_MAX];wurl_href(&parsed,want,sizeof want);
    /* Form submission must reload even an unchanged URL. Going through the
     * location.href setter would silently take its same-document fast path.
     * Use the SAME pending record, so callback form and location requests do
     * not race two independently drained navigation queues. */
    queue_navigation(want);
    return 1;
}

/* ---- Storage ----------------------------------------------------------
 * The old g_stores implementation lived here and keyed both localStorage and
 * sessionStorage by origin alone: "sessionStorage is identical today, and
 * differs only in that it is documented to be per-tab." Correction: the
 * shared backend now keys session areas by the embedder's stable tab id.
 * A navigation changes the JSContext, not the tab; no teardown here clears
 * its store. Explicit tab destruction drops only that tab's session areas.
 *
 * Still IN MEMORY ONLY. The old comment attributed the absence of disk
 * backing to a filesystem durability bug; that is not a current storage
 * capability check. This service simply has no disk commit/recovery backend,
 * and must not claim persistence until that backend is actually implemented.
 * Textual ownership keeps the existing partial host links on the same
 * implementation (see storage_backend.c); do not link a second copy. */
#include "storage_backend.c"
int js_webapi_set_storage_store(const struct bstore_ops *ops) { return storage_backend_set_store(ops); }
static unsigned long long g_storage_session;
void js_webapi_set_storage_session(unsigned long long id) { g_storage_session = id; }
void js_webapi_drop_storage_session(unsigned long long id) { storage_backend_drop_session(id); }

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

static JSValue g_mk_response = JS_UNDEFINED;   /* (status,statusText,pairs,url,redirected,type,nobody,handle) --
                                                   `handle` (argv[7]) is the abort handle wf_handle(f) also uses;
                                                   mkResponse wires it as the body stream's underlying-source
                                                   cancel, so a page cancelling the body reaches C. See
                                                   fetch_deliver_headers's own comment above the argv[] build. */
static JSValue g_viewport_changed = JS_UNDEFINED;
/* (name, message) -> an Error built through the REALM'S OWN constructor when
 * `name` names one (TypeError, RangeError, ...), so `e instanceof TypeError`
 * and `e.constructor === TypeError` both hold -- see mkError's own comment in
 * the prelude for why JS_NewError() plus a hand-set `.name` property cannot
 * produce that (it stays an Error whose constructor is Error). */
static JSValue g_mk_error = JS_UNDEFINED;

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

/* Correction 2026-09-10: the historical non-durable rationale above is retired.
 * Cookie persistence now commits each accepted mutation through a private
 * two-slot bstore; session cookies intentionally stay process-local. */
static struct cookie_jar g_jar;
static int g_jar_ready;

static struct cookie_jar *jar(void)
{
    if (!g_jar_ready) { cookie_jar_init(&g_jar); g_jar_ready = 1; }
    return &g_jar;
}

static struct cookie_persistence g_cookie_store;
int js_webapi_set_cookie_store(const struct bstore_ops *ops)
{ return cookie_persistence_open(&g_cookie_store, jar(), ops, now_unix()); }
int js_webapi_cookie_persistence_status(void)
{ return g_cookie_store.status; }
static void cookie_commit(int rc)
{
    if (rc == 0 && cookie_persistence_flush(&g_cookie_store, jar(), now_unix()) < 0)
        printf("[webapi] cookie persistence failed status=%d\n", g_cookie_store.status);
}

static void ck_ctx(struct cookie_ctx *c, const struct wurl *u, int http_api)
{
    c->host = u->host;
    c->path = u->pathname[0] ? u->pathname : "/";
    c->secure = u->https;
    c->http_api = http_api;
}

static struct cookie_request cookie_request_for(const struct wurl *site, int valid,
                                                int nav, int safe)
{
    struct cookie_request r = { valid ? site->host : 0,
                                valid ? site->https : 0,
                                nav, safe, 0 };
    return r;
}

int webapi_cookie_line_request(const char *host, const char *path, int secure,
                               const struct cookie_request *request, char *out, int cap)
{
    struct cookie_ctx c = { host, (path && path[0]) ? path : "/", secure, 1 };
#ifdef WEBAPI_COOKIE_ALWAYS_SAME_SITE
    (void)request;
    return cookie_header(jar(), &c, now_unix(), out, cap);
#else
    struct cookie_header_diagnostics diag;
    int n = cookie_header_with_diagnostics(jar(), &c, cookie_request_kind(&c, request),
                                          now_unix(), out, cap, &diag);
    if (n < 0) printf("[webapi] cookie-header error=%d required=%llu cap=%d count=%d\n",
                      n, (unsigned long long)diag.required_bytes, cap, diag.eligible_count);
    return n;
#endif
}

void webapi_cookie_store_request(const char *host, const char *path, int secure,
                                 const struct cookie_request *request, const char *value)
{
    struct cookie_ctx c = { host, (path && path[0]) ? path : "/", secure, 1 };
    cookie_commit(cookie_set_ex(jar(), &c, cookie_request_kind(&c, request), value, now_unix()));
}

/* ---- the jar's two exports to the TRANSPORT (browser_rt.c) --------------
 * Until these existed only the JS-visible half of the network carried
 * cookies: fetch()/XHR attached and stored them, but the fetches the BROWSER
 * makes -- the top-level navigation, location.reload(), every stylesheet/
 * script/image subresource -- went out bare and threw every Set-Cookie away.
 * A WAF that answers a first visit with a challenge that sets a cookie and
 * reloads (douyin's ByteDance PoW page, the BL diagnosis) therefore looped
 * forever: each reload arrived cookieless and got the challenge again, 11
 * reloads in a 9-second boot on the 2026-08-09 scoreboard.
 *
 * browser_rt.c calls these through WEAK references so the loader host build
 * (which links neither this TU nor cookies.c) still links and simply runs
 * cookieless -- the pre-fix behaviour, correct for a build with no jar.
 * http_api=1 on both: this is the network stack speaking, so HttpOnly
 * cookies are carried and storable per cookies.c's own rules. */
int webapi_cookie_line(const char *host, const char *path, int secure,
                       int nav, char *out, int cap)
{
    struct cookie_ctx c = { host, (path && path[0]) ? path : "/", secure, 1 };
    /* SameSite, computed HERE because this is the only file that knows what
     * document is asking: g_loc. browser_rt.c knows the request and not the
     * initiator, so it passes `nav` and nothing else.
     *
     * This call used to be cookie_header(), which is cookie_header_ex() with
     * CK_REQ_SAME_SITE wired in -- so every request the transport made was
     * declared same-site, including the ones that were not. Every
     * cross-origin <script src> and <link rel=stylesheet> a page named (and
     * res_fetch_all filters those for nothing but data:/javascript:) carried
     * the target's Secure+HttpOnly+SameSite=Strict session, and the reply was
     * then evaluated in the requesting page's realm. The fetch()/XHR path
     * fifty lines below had it right the whole time, which is what made this
     * an omission rather than a missing feature -- and test-cookie-cors
     * covers only that path, so the gate could not see it.
     *
     * !g_loc_valid means about:blank or a URL url.c will not parse -- no
     * initiator document, hence nothing to be cross-site TO, hence same-site.
     *
     * KNOWN AND DELIBERATE RESIDUAL, in the conservative direction: a
     * navigation typed into the address bar also has no initiator and should
     * likewise be same-site, but g_loc is still the PREVIOUS page when the
     * request goes out (set_location runs at js_page_open, after the bytes
     * arrive), so typing a URL while a different site is open is classified
     * CROSS_SITE_NAV and its SameSite=Strict cookies are withheld for that
     * one request. The page then loads, g_loc becomes it, and everything
     * after is same-site. Under-sending for one paint; the alternative is
     * threading a user-vs-page initiator flag through all ten callers of
     * load(), where the SAFE default is the one a caller forgets. */
#ifdef WEBAPI_COOKIE_ALWAYS_SAME_SITE     /* negctl: exactly the shipped bug */
    (void)nav;
    return cookie_header(jar(), &c, now_unix(), out, cap);
#else
    /* Compatibility door for existing direct embedders/tests. Product bfetch
     * now calls the explicit request door above; this global lookup MUST NOT
     * return to a queued/redirected resource path. The historical comment above
     * describes the retired product wiring, not a current context guarantee. */
    struct cookie_request request = cookie_request_for(&g_loc, g_loc_valid, nav, 1);
    if (!g_webapi_ctx && !g_loc_valid) request.browser_initiated = 1;
    return webapi_cookie_line_request(host, path, secure, &request, out, cap);
#endif
}

void webapi_cookie_store_line(const char *host, const char *path, int secure,
                              const char *setcookie)
{
    struct cookie_ctx c = { host, (path && path[0]) ? path : "/", secure, 1 };
    cookie_commit(cookie_set(jar(), &c, setcookie, now_unix()));
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
    char origin[WURL_MAX];             /* exact target URL, fragment excluded */
    char initiator[URL_HOST_MAX + 16];
    char method[H1_METHOD_MAX];
    char headers[256];                  /* lowercase comma list the server allowed */
    unsigned long long expires_ms;
};
static struct pfcache g_pfc[PFC_MAX];

static void pfc_store(const char *initiator, const char *origin, const char *method, int creds,
                      const char *allow_hdrs, int max_age_s)
{
    /* Opaque origins have identity beyond their serialized "null". Until the
     * cache can represent that identity, not caching them is the safe answer. */
    if (max_age_s <= 0 || !strcmp(initiator, "null")) return;
    if (max_age_s > 86400) max_age_s = 86400;         /* the spec's own ceiling */
    struct pfcache *slot = 0;
    for (int i = 0; i < PFC_MAX; i++) {
        struct pfcache *p = &g_pfc[i];
        if (p->used && p->creds == creds && !strcmp(p->initiator, initiator) &&
            !strcmp(p->origin, origin) &&
            ci_streq(p->method, method)) { slot = p; break; }
    }
    if (!slot) for (int i = 0; i < PFC_MAX; i++) if (!g_pfc[i].used) { slot = &g_pfc[i]; break; }
    if (!slot) slot = &g_pfc[0];                      /* evict slot 0; the table is a cache */
    memset(slot, 0, sizeof *slot);
    slot->used = 1; slot->creds = creds;
    scopy(slot->origin, origin, (int)sizeof slot->origin);
    scopy(slot->initiator, initiator, (int)sizeof slot->initiator);
    scopy(slot->method, method, (int)sizeof slot->method);
    scopy(slot->headers, allow_hdrs ? allow_hdrs : "", (int)sizeof slot->headers);
    slot->expires_ms = now_ms() + (unsigned long long)max_age_s * 1000ull;
}

/* 1 if a live cache entry covers this exact request. */
static int pfc_hit(const char *initiator, const char *origin, const char *method, int creds,
                   const struct h1_headers *author)
{
    for (int i = 0; i < PFC_MAX; i++) {
        struct pfcache *p = &g_pfc[i];
        if (!p->used || p->creds != creds) continue;
        if (strcmp(p->initiator, initiator) || strcmp(p->origin, origin) ||
            !ci_streq(p->method, method)) continue;
        if (now_ms() > p->expires_ms) { p->used = 0; continue; }
        if (!creds && list_has(p->headers, "*")) return 1;
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
/* A gap between successive fetch_steps longer than this is loop-blocked time,
 * not connection idleness -- see the block comment in fetch_step. 250 ms is
 * between the 16 ms frame the pump runs on and the multi-second script/module
 * compiles that actually block. */
#define WF_STEP_GAP 250ull
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
 * every single pump, so `hold` never needs more than one read's worth.
 * Correction: that old statement applies only to HTTP/1's 4096-byte read.
 * HTTP/2 may deliver 16384 bytes (or another stream's backlog); bxfer_pump now
 * yields its first final headers BEFORE draining DATA to this sink. Keep the
 * small h1 hold instead of silently turning it into another unbounded queue. */
enum { WF_DEL_UNKNOWN = 0, WF_DEL_JS, WF_DEL_DROP };

struct wfetch {
    struct fetch_realm *owner;
    int   state;
    int   fd;
    int   started;                 /* h1_conn_start has run */
    struct h1_conn conn;
    struct wurl url;
    /* Identity belongs to the request. Resolving a redirect, retrying a dial,
     * and accepting response cookies must never consult the active document
     * again: the page can have queued a navigation while this request waits. */
    struct wurl initiator;
    int initiator_valid;
    /* The document URL captured when fetch() was called. Referer is a user-
     * agent header: script cannot forge it, and consulting the active page
     * later would let a queued navigation change an already-created request. */
    struct wurl referrer;
    int referrer_valid;
    struct wurl cookie_site;
    int cookie_site_valid;
    int response_logged;
    char  method[H1_METHOD_MAX];
    struct h1_headers hdr;         /* caller headers, re-sent on each hop */
    char *body; int body_len;      /* request body, owned */
    int   hops, redirected, transport_retries;
    unsigned long long deadline;
    /* When fetch_step last ran for this request.  The idle deadline above is
     * charged for SERVICED time only -- see fetch_step. */
    unsigned long long last_step;
    int last_step_valid;            /* monotonic time zero is a valid baseline */
    JSValue resolve, reject;
    JSContext *ctx;                /* the sink runs inside js_webapi_pump(ctx) */

    int   gen;                     /* an abort handle is (slot, generation) */
    int   mode, creds;
    int   cross;                   /* THIS hop is cross-origin */
    int   tainted;                 /* Fetch redirect-taint is not same-origin */
    int   site_tainted;            /* redirect chain is cross-site */
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
    const char *failure_at;         /* exact local boundary, otherwise transport */
    int   failed_chunk;
};
static struct wfetch g_fetch[WF_MAX];
static int g_fetch_live;
static int wf_handle(const struct wfetch *f);
static int wf_creds(const struct wfetch *f);

static const char *fetch_trace_host(const struct wfetch *f)
{
    /* The transport URL splitter does not yet normalize every authority form.
     * In particular userinfo must never reach a metadata-only diagnostic. */
    const char *host = f->url.host;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' ||
              *p == ':' || *p == '[' || *p == ']')) return "redacted-host";
    return host;
}

static const char *fetch_trace_payload(const struct h1_response *r)
{
    /* Emit a fixed category, never an arbitrary response header value. */
    const char *v = h1_headers_get(&r->hdr, "content-type");
    if (!v) return "unspecified";
    while (*v == ' ' || *v == '\t') v++;
    static const struct { const char *mime, *label; } kinds[] = {
        { "text/event-stream", "event-stream" },
        { "application/json", "json" },
        { "text/html", "html" }
    };
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        int n = (int)strlen(kinds[i].mime);
        if (ci_strneq(v, kinds[i].mime, n) &&
            (!v[n] || v[n] == ';' || v[n] == ' ' || v[n] == '\t'))
            return kinds[i].label;
    }
    return "other";
}

static void fetch_trace_request(const struct wfetch *f)
{
#ifndef WEBAPI_NO_FETCH_DIAGNOSTICS
    /* Paths, headers and bodies may contain login material. Only parsed
     * transport metadata and local enums belong in the permanent serial log. */
    printf("[webapi] fetch-request id=%d method=%s host=%s port=%d tls=%d phase=%d hops=%d cross=%d creds=%d\n",
           wf_handle(f), f->phase == WF_PH_PREFLIGHT ? "OPTIONS" : f->method,
           fetch_trace_host(f), f->url.port, f->url.https, f->phase, f->hops,
           f->cross, wf_creds(f));
#else
    (void)f;
#endif
}

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
    if (n > (int)sizeof f->hold - f->hold_len) {
        f->failure_at = "pre-header-hold";
        f->failed_chunk = n;
        return H1_E_TOOLARGE;
    }
    memcpy(f->hold + f->hold_len, p, (size_t)n);
    f->hold_len += n;
    return H1_OK;
}

static void fetch_release(JSContext *ctx, struct wfetch *f)
{
    /* Caller identity cannot change the runtime that owns retained values. */
    ctx=f->ctx;
    if (f->fd >= 0) { g_net->close(f->fd); f->fd = -1; }
    if (f->started) bxfer_free(&f->conn);
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
    f->owner=NULL;f->ctx=NULL;
    f->gen++;                      /* any abort handle still held is now stale */
    if (g_fetch_live > 0) g_fetch_live--;
}

/* Build an Error through the realm's own constructor for `name` (mkError, in
 * the prelude) so `e instanceof TypeError` holds for script that checks it --
 * see g_mk_error's own comment for what JS_NewError() plus a hand-set `.name`
 * gets wrong. Falls back to the old shape if the prelude has not installed
 * the hook yet (should not happen in practice, but a fetch that fires before
 * js_webapi_install finished should not crash over it). */
static JSValue mk_error(JSContext *ctx, const char *name, const char *message)
{
    struct fetch_realm *owner=fetch_realm_for(ctx);
    JSValue constructor=owner?owner->mk_error:JS_UNDEFINED;
    if (JS_IsFunction(ctx, constructor)) {
        JSValue a[2];
        a[0] = JS_NewString(ctx, name);
        a[1] = JS_NewString(ctx, message);
        JSValue e = JS_Call(ctx, constructor, JS_UNDEFINED, 2, (JSValueConst *)a);
        JS_FreeValue(ctx, a[0]); JS_FreeValue(ctx, a[1]);
        if (!JS_IsException(e)) return e;
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, message));
    return err;
}

static void fetch_trace_detail(const struct wfetch *f)
{
#ifndef WEBAPI_NO_FETCH_DIAGNOSTICS
    const char *ce = h1_headers_get(&f->conn.resp.hdr, "content-encoding");
    printf("[webapi] fetch-detail id=%d state=%d error_code=%d phase=%d method=%s host=%s boundary=%s status=%d clen=%lld compressed=%d "
           "seen=%lld buffered=%d cap=%d streaming=%d hold=%d incoming=%d\n",
           wf_handle(f), f->state, f->conn.err ? f->conn.err : f->conn.resp.err,
           f->phase, f->method, fetch_trace_host(f),
           f->failure_at ? f->failure_at : "transport-or-policy",
           f->conn.resp.code, (long long)f->conn.resp.clen,
           ce && !ci_streq(ce, "identity"),
           (long long)f->conn.resp.body_seen, f->conn.resp.body_len,
           f->conn.resp.body_max, f->conn.resp.streaming, f->hold_len, f->failed_chunk);
#endif
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
    fetch_trace_detail(f);

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
    JSValue err = mk_error(ctx, name ? name : "TypeError", full);
    JSValue r = JS_Call(ctx, f->reject, JS_UNDEFINED, 1, (JSValueConst *)&err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    fetch_release(ctx, f);
}

static void fetch_reject(JSContext *ctx, struct wfetch *f, const char *msg)
{ fetch_fail(ctx, f, msg, "TypeError"); }

/* A redirect-tainted origin serializes identically on the wire, during CORS
 * checks, and in the preflight key. The old wire-only null conversion silently
 * rejected a correct ACAO:null response after following a cross-origin hop. */
static void fetch_origin(const struct wfetch *f, char *out, int max)
{
    if (f->initiator_valid && !f->tainted) wurl_origin(&f->initiator, out, max);
    else scopy(out, "null", max);
}

/* The Fetch default is strict-origin-when-cross-origin. This is not cosmetic:
 * media CDNs (including bilivideo) authorize an otherwise public signed URL
 * only when the browser supplies the embedding page's Referer. Same-origin
 * requests retain the path; cross-origin requests disclose only the origin;
 * an HTTPS document never leaks it to an HTTP target. */
static int fetch_referrer(const struct wfetch *f, char *out, int max)
{
    if (!f->referrer_valid || max <= 0 ||
        (f->referrer.https && !f->url.https)) return 0;
    if (same_origin(&f->referrer, &f->url)) {
        struct wurl u = f->referrer;
        u.hash[0] = 0;
        wurl_href(&u, out, max);
    } else {
        wurl_origin(&f->referrer, out, max);
        int n = (int)strlen(out);
        if (n + 1 >= max) return 0;
        out[n++] = '/';
        out[n] = 0;
    }
    return out[0] != 0;
}

static void fetch_cache_url(const struct wfetch *f, char *out, int max)
{
#ifdef WEBAPI_PREFLIGHT_UNPARTITIONED
    wurl_origin(&f->url, out, max);
#else
    struct wurl u = f->url;
    u.hash[0] = 0;
    wurl_href(&u, out, max);
#endif
}

static void fetch_preflight_origin(const struct wfetch *f, char *out, int max)
{
#ifdef WEBAPI_PREFLIGHT_UNPARTITIONED
    (void)f; scopy(out, "unpartitioned", max);
#else
    fetch_origin(f, out, max);
#endif
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
    fetch_origin(f, origin, (int)sizeof origin);
    int send_origin = f->cross || preflight ||
                      (!ci_streq(f->method, "GET") && !ci_streq(f->method, "HEAD"));
    if (send_origin)
        h1_request_set_header(&q, "Origin", origin);

    /* Referer is browser-controlled and therefore is deliberately absent
     * from f->hdr and from Access-Control-Request-Headers. Preflight describes
     * author headers; the actual request gets the navigation context. */
    if (!preflight) {
        char referer[WURL_MAX];
        if (fetch_referrer(f, referer, (int)sizeof referer))
            h1_request_set_header(&q, "Referer", referer);
    }

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
            struct cookie_request request = cookie_request_for(&f->cookie_site,
                                                        f->cookie_site_valid && !f->site_tainted, 0, 0);
            static char cookie[CK_HEADER_MAX];   /* 8 KiB: not on the stack */
            struct cookie_header_diagnostics diag;
            int n = cookie_header_with_diagnostics(jar(), &cc, cookie_request_kind(&cc, &request),
                                     now_unix(), cookie, (int)sizeof cookie, &diag);
            if (n < 0) {
                printf("[webapi] cookie-header error=%d required=%llu cap=%d count=%d\n",
                       n, (unsigned long long)diag.required_bytes, (int)sizeof cookie, diag.eligible_count);
                f->failure_at = "cookie-header"; h1_request_free(&q); return -1;
            }
            if (n > 0 && h1_request_set_header(&q, "Cookie", cookie) != H1_OK) {
                f->failure_at = "cookie-attach"; h1_request_free(&q); return -1;
            }
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
    /* bxfer picks the protocol from ALPN and may REPLACE f->fd: a request that
     * joined an origin's connection speculatively, before the handshake said
     * which protocol it was, has to be given its own socket if the answer
     * turns out to be HTTP/1.1. Hence the fd by pointer. */
    /* The old -1 merged serialization with protocol startup. A valid same-
     * origin GET on a closing multiplexed connection was consequently logged
     * as "request could not be built" (site-general guest, 2026-09-09). Keep
     * those boundaries distinct; this is not permission to retry a request
     * that might already have reached the peer. */
    int start_rc=bxfer_start(&f->conn, &t, raw, rawlen, &f->fd,
                            f->url.host, f->url.port, f->url.https);
    if(start_rc==BXFER_START_WAIT) {free(raw);return 1;}
    if(start_rc!=H1_OK) {
        printf("[webapi] fetch-start protocol_error=%d\n",start_rc);
        free(raw);return -2;
    }
    fetch_trace_request(f);
    f->started = 1;                       /* the conn owns `raw` from here */
    h1_response_limit(&f->conn.resp,BROWSER_BUFFERED_BODY_MAX);
    h1_response_head(&f->conn.resp, !preflight && ci_streq(f->method, "HEAD"));
    if (WEBAPI_STREAMING) h1_response_sink(&f->conn.resp, wf_sink, f);
    f->deliver = preflight ? WF_DEL_DROP : WF_DEL_UNKNOWN;
    f->hold_len = 0;
    return 0;
}

/* Dial the socket for f->url and arm the deadline. 0 ok. */
static int fetch_dial(struct wfetch *f)
{
    net_env_init();
    if (!g_net || !g_net->open) return -1;
    f->cross = f->cross || !f->initiator_valid || !same_origin(&f->url, &f->initiator);
    f->response_logged = 0;
    f->fd = g_net->open(f->url.host, f->url.port, f->url.https);
    if (f->fd < 0) { fetch_trace_request(f); return -1; }
    f->state = WF_DIAL;
    f->started = 0;
    unsigned long long now=now_ms();
    f->deadline = now + WF_TIMEOUT;
#ifdef WEBAPI_FETCH_NO_INITIAL_BASELINE
    f->last_step = 0;f->last_step_valid = 0;
#else
    /* The first pump may itself follow a long synchronous task. Establish
     * its observation baseline when the socket is opened, not after that
     * first pump has already tested and potentially expired the deadline. */
    f->last_step = now;f->last_step_valid = 1;
#endif
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
        if (v) {
            struct cookie_request request = cookie_request_for(&f->cookie_site,
                                                        f->cookie_site_valid && !f->site_tainted, 0, 0);
            cookie_commit(cookie_set_ex(jar(), &cc, cookie_request_kind(&cc, &request), v, now));
        }
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
    fetch_origin(f, origin, (int)sizeof origin);

    if (h1_headers_count(&r->hdr, "access-control-allow-origin") > 1)
        return "the server sent several Access-Control-Allow-Origin headers";
    const char *acao = h1_headers_get(&r->hdr, "access-control-allow-origin");
    if (!acao || !*acao)
        return "cross-origin request blocked: no Access-Control-Allow-Origin";

    int creds = wf_creds(f);
    if (creds) {
        const char *acac = h1_headers_get(&r->hdr, "access-control-allow-credentials");
        if (!acac || strcmp(acac, "true"))
            return "credentialed cross-origin request blocked: "
                   "Access-Control-Allow-Credentials is not true";
        /* `*` plus credentials is the combination the spec forbids outright:
         * it would mean "any origin may read this response", said about a
         * response that was generated with the user's session. */
        if (!strcmp(acao, "*"))
            return "credentialed cross-origin request blocked: "
                   "Access-Control-Allow-Origin is '*'";
    }
    if (strcmp(acao, "*") != 0 && strcmp(acao, origin))
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

    char origin[WURL_MAX], initiator[URL_HOST_MAX + 16];
    fetch_cache_url(f, origin, (int)sizeof origin);
    fetch_preflight_origin(f, initiator, (int)sizeof initiator);
    const char *ma = h1_headers_get(&r->hdr, "access-control-max-age");
    int secs = 0;
    if (ma) { for (const char *p = ma; *p >= '0' && *p <= '9'; p++) { if (secs < 86400) secs = secs * 10 + (*p - '0'); } }
    pfc_store(initiator, origin, f->method, creds, acah, secs);
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
/* Forward-declared: wf_handle's definition lives further down (with its own
 * comment) but fetch_deliver_headers below needs it to hand mkResponse a
 * cancel handle for the body stream it is about to build. */
static int wf_handle(const struct wfetch *f);

static int fetch_deliver_headers(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;

    fetch_take_cookies(f);

    const char *why = cors_check_response(f);
    if (why) { f->failure_at="cors-response"; f->deliver = WF_DEL_DROP; fetch_reject(ctx, f, why); return 1; }

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

    /* argv[7] is the abort handle for the body stream about to be built --
     * the same handle __fetchAbort() takes, and the same slot/gen encoding
     * (wf_handle's own comment explains why a handle cannot outlive the slot
     * it names). mkResponse threads it onto the ReadableStream's underlying
     * source as `cancel`, which was previously ABSENT: a page that called
     * `response.body.getReader().cancel()` (or let a `for await` `break` out,
     * which does the same thing) settled the JS-side stream and told C
     * nothing, so fetch_step kept pumping the socket to completion and
     * WF_HIGHWATER's queued-byte counter -- reset to 0 by the cancel it never
     * heard about -- could never trip again, so an abandoned multi-megabyte
     * transfer ran at full speed and held one of WF_MAX slots for its whole
     * duration. */
    JSValue argv[9];
    argv[0] = JS_NewInt32(ctx, opaque ? 0 : r->code);
    argv[1] = JS_NewString(ctx, opaque ? "" : r->reason);
    argv[2] = pairs;
    argv[3] = JS_NewString(ctx, opaque ? "" : href);
    /* An opaque filter has an empty URL list. Reporting its redirect bit
     * would reveal history that the URL/status/header filter already hid. */
    argv[4] = JS_NewBool(ctx, !opaque && f->redirected);
    argv[5] = JS_NewString(ctx, opaque ? "opaque" : (f->cross ? "cors" : "basic"));
    argv[6] = JS_NewBool(ctx, opaque || r->no_body);
    argv[7] = JS_NewInt32(ctx, wf_handle(f));
    int response_argc = 8;
#ifdef JS_RUNTIME_DIAGNOSTICS
    /* Private metadata for XHR diagnosis, never an added Response property. */
    argv[response_argc++] = JS_NewInt32(ctx, !opaque && ci_streq(fetch_trace_payload(r), "json"));
#endif
    JSValue hooks = JS_Call(ctx, f->owner->mk_response, JS_UNDEFINED, response_argc, (JSValueConst *)argv);
    for (int i = 0; i < response_argc; i++) JS_FreeValue(ctx, argv[i]);
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
            f->failure_at = "content-decoding";
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
#ifndef WEBAPI_NO_FETCH_DIAGNOSTICS
    /* Headers can succeed while the body subsequently fails. This boundary
     * reports protocol/body decoding completion, not application success. */
    printf("[webapi] fetch-complete id=%d status=%d payload=%s received=%lld delivery=%d\n",
           wf_handle(f), r->code, fetch_trace_payload(r),
           (long long)r->body_seen, f->deliver);
#endif
    fetch_release(ctx, f);
    return 1;
}

/* Re-dial for the next hop (a redirect, or the real request after a
 * preflight). 0 ok. */
static int fetch_redial(struct wfetch *f)
{
    g_net->close(f->fd); f->fd = -1;
    bxfer_free(&f->conn);
    memset(&f->conn, 0, sizeof f->conn);
    f->started = 0;
    f->hold_len = 0;
    f->deliver = WF_DEL_UNKNOWN;
    return fetch_dial(f);
}

static int fetch_retry_read(struct wfetch *f)
{
#ifndef WEBAPI_NO_FRESH_RETRY
    /* The pre-send GOAWAY check cannot see a frame that arrives AFTER HEADERS
     * were queued. bfetch already retries reads once; fetch() instead stranded
     * an otherwise usable SPA at its config GET (guest 2026-09-10: GOAWAY last
     * stream 1, GET streams 3/5 failed without response headers).
     *
     * Keep the boundary narrow: GET/HEAD only, one attempt over the ENTIRE
     * fetch including redirects, no Response/headers delivered, transport or
     * truncation failure only. POST/other methods are deliberately not retried
     * even when HTTP/2 REFUSED_STREAM might permit it: their replay contract
     * is not represented by this generic h1_response. Preserve the existing
     * deadline so a broken server cannot buy another timeout by closing. */
    if(f->transport_retries || f->resolved || h1_response_headers_done(&f->conn.resp) ||
       (strcmp(f->method,"GET")&&strcmp(f->method,"HEAD")) ||
       (f->conn.err!=H1_E_TRANSPORT&&f->conn.err!=H1_E_TRUNC))return 0;
    f->transport_retries++;
    unsigned long long deadline=f->deadline;
    int rc=fetch_redial(f);
    f->deadline=deadline;
    if(rc==0){printf("[webapi] retrying unanswered %s once\n",f->method);return 1;}
    f->conn.err=H1_E_TRANSPORT; /* redial cleared the old conn; never report "ok" */
#else
    (void)f;
#endif
    return 0;
}

/* A 3xx with a Location: re-target and re-dial. 1 if the request continues. */
static int fetch_redirect(JSContext *ctx, struct wfetch *f)
{
    struct h1_response *r = &f->conn.resp;
    const char *why = cors_check_response(f);
    if (why) { f->failure_at = "redirect-cors"; fetch_reject(ctx, f, why); return 1; }
    const char *loc = h1_headers_get(&r->hdr, "location");
    if (!loc || !*loc || f->hops >= WF_HOPS) return 0;

    struct wurl next;
    if (wurl_parse(loc, &f->url, &next) != 0) return 0;
    if (!fetch_policy_allows(f->owner, &next)) {
        fetch_reject(ctx, f, "redirect blocked by document connect policy");
        return 1;
    }

    /* A redirect that leaves the origin taints the request: from here on the
     * server is told `Origin: null`, so it cannot be led to believe the
     * request came from the origin it started at. */
    if (!same_origin(&next, &f->url)) {
        /* The first same-origin URL may redirect cross-origin without taint;
         * taint starts only when the PREVIOUS URL also differs from the
         * initiator (Fetch: compute redirect-taint). Keep it across return hops. */
        if (!f->initiator_valid || !same_origin(&f->initiator, &f->url))
            f->tainted = 1;
        struct cookie_ctx previous;
        ck_ctx(&previous, &f->url, 1);
        struct cookie_request next_site = cookie_request_for(&next, 1, 0, 0);
        struct cookie_request start_site = cookie_request_for(&f->cookie_site,
                                                             f->cookie_site_valid, 0, 0);
        if (cookie_request_kind(&previous, &next_site) != CK_REQ_SAME_SITE &&
            cookie_request_kind(&previous, &start_site) != CK_REQ_SAME_SITE)
            f->site_tainted = 1;
        /* Author Authorization belongs to its original origin. */
        h1_headers_remove(&f->hdr, "authorization");
    }
    if (f->mode == WF_MODE_SAME_ORIGIN &&
        (!f->initiator_valid || !same_origin(&next, &f->initiator))) {
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
        char origin[WURL_MAX], initiator[URL_HOST_MAX + 16];
        fetch_cache_url(f, origin, (int)sizeof origin);
        fetch_preflight_origin(f, initiator, (int)sizeof initiator);
        if (!pfc_hit(initiator, origin, f->method, wf_creds(f), &f->hdr)) f->phase = WF_PH_PREFLIGHT;
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
    if(ctx!=f->ctx||!f->owner||f->owner->stopped)return 0;
    f->js_work = 0;
    /* THE IDLE CLOCK ONLY TICKS WHILE WE CAN OBSERVE THE CONNECTION.
     *
     * MEASURED, 2026-08-30, in the guest, on the z.ai specimen: the page's
     * entry module is 3.2 MB; js_module.c's loader compiles the whole static
     * graph inside ONE JS_Eval (its own header documents why), which on TCG
     * blocked the page loop for ~50 s. The /api/config fetch this page's
     * inline script had started BEFORE the compile had a 30 s WF_TIMEOUT and
     * a socket that never got dialed -- fetch_step could not run, the wall
     * clock ran anyway, and the first step after the loop unblocked failed
     * the deadline check below: "fetch: timed out" for a request whose
     * server was 1 ms away and never consulted. The SPA then took its
     * config-missing branch and rendered /error instead of the app. A
     * network-fault report for a condition the network caused none of.
     *
     * The fix is to charge the deadline only for time the loop was actually
     * servicing requests: a gap between successive steps longer than
     * WF_STEP_GAP means the loop was BLOCKED (a synchronous script/module
     * compile, a long layout -- anything that owns the single thread), and
     * the connection cannot have idled during it because nobody polled it.
     * 250 ms sits two orders below the 45 s+ blocks this exists for and one
     * order above the 16 ms frame the pump normally runs on, so a merely
     * slow frame is never forgiven and a genuine block always is.
     *
     * What this deliberately does NOT do: pause the clock for DNS/TCP/TLS
     * handshakes on a live-but-dead peer. Those make progress returns of
     * `poll == not-connected` every frame with TINY gaps -- the loop is
     * servicing them -- so they still time out on schedule, which is the
     * behaviour the timeout exists for.
     *
     * ZAIBLANK_FETCH_DEADLINE_OLD compiles just this compensation out, which
     * is byte-for-byte the pre-fix behaviour; test-zaiblank-fetch-negctl
     * links it to watch check 2a go red (the fetch fails for blocked time)
     * while check 2b stays green -- proof the control measures this fix. */
#ifndef ZAIBLANK_FETCH_DEADLINE_OLD
    unsigned long long now = now_ms();
    if (f->last_step_valid && now > f->last_step + WF_STEP_GAP)
        f->deadline += now - f->last_step;
    f->last_step = now;
    f->last_step_valid = 1;
    if (now > f->deadline) { f->failure_at="timeout"; fetch_fail(ctx, f, "timed out", "TypeError"); return 1; }
#else
    if (now_ms() > f->deadline) { f->failure_at="timeout"; fetch_fail(ctx, f, "timed out", "TypeError"); return 1; }
#endif

    /* Backpressure: while the page is behind, stop reading.  The socket buffer
     * fills, the window closes, and the producer slows down.
     *
     * Re-arm the idle timeout on the way out: the connection is healthy by
     * construction here -- WE are the one declining to read, not a peer that
     * went quiet -- so the WF_TIMEOUT clock above must not keep running while
     * we are backpressured. Before this line existed, a page that held a
     * Response with more than WF_HIGHWATER queued and did not drain its
     * reader for 30s had its body stream errored with "fetch: timed out": a
     * network-fault report for a condition the network caused none of,
     * produced BY the mechanism that exists so the page is allowed to be
     * slow. (The other honest fix is a SEPARATE stall clock that only
     * advances while we are willing to read, for the case where the timeout
     * is meant to catch a peer that dies while we are backpressured -- that
     * needs a poll of the socket rather than the byte counter above, and
     * nothing here does that yet.) */
    if (f->resolved && f->queued > WF_HIGHWATER) { f->deadline = now_ms() + WF_TIMEOUT; return 0; }

    int bits = g_net->poll(f->fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) { f->failure_at="connect-or-socket"; fetch_fail(ctx, f, "connection failed", "TypeError"); return 1; }
    if (!(bits & SOCK_P_CONNECTED)) return 0;             /* DNS/TCP/TLS still running */

    if (!f->started) {
        int send_rc=fetch_send_request(ctx,f);
        if(send_rc>0)return 0; /* pre-send replacement: poll its TLS next frame */
        if(send_rc!=0) {
            fetch_reject(ctx,f,send_rc==-2?"request transport could not start":"request could not be built");
            return 1;
        }
        f->state = WF_XFER;
    }

    for (int i = 0; i < WF_STEPS; i++) {
        if (f->state == WF_FREE) return 1;    /* released underneath us */
        int64_t before = f->conn.resp.body_seen + f->conn.resp.hdr_bytes;
        int st = bxfer_pump(&f->conn);
        if (!f->response_logged && h1_response_headers_done(&f->conn.resp)) {
#ifndef WEBAPI_NO_FETCH_DIAGNOSTICS
            printf("[webapi] fetch-response id=%d status=%d phase=%d hops=%d payload=%s\n",
                   wf_handle(f), f->conn.resp.code, f->phase, f->hops,
                   fetch_trace_payload(&f->conn.resp));
#endif
            f->response_logged = 1;
        }
        /* Any progress re-arms the idle timeout. */
        if (f->conn.resp.body_seen + f->conn.resp.hdr_bytes != before)
            f->deadline = now_ms() + WF_TIMEOUT;
        if (st == H1_C_ERROR) {
            if(fetch_retry_read(f))return 0;
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
                if (why) { f->failure_at="cors-preflight"; fetch_reject(ctx, f, why); return 1; }
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

/* Forgiving base64 for data: URLs. Keeping this in native code matters on the
 * guest: Bilibili bootstraps its safety module from roughly half a megabyte
 * of base64, and QuickJS spent an entire watchdog slice in String.replace
 * before it decoded one byte. This implements the same whitespace/padding
 * rules as the Fetch data-URL algorithm and returns an ArrayBuffer, or null
 * for malformed input. */
static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_ws(unsigned char c)
{ return c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }

static JSValue js_base64_decode(JSContext *ctx, JSValueConst t,
                                int argc, JSValueConst *argv)
{
    (void)t;
    size_t inlen = 0;
    const char *in = argc ? JS_ToCStringLen(ctx, &inlen, argv[0]) : 0;
    if (!in) return JS_EXCEPTION;
    char *clean = (char *)malloc(inlen + 1);
    if (!clean) { JS_FreeCString(ctx, in); return JS_ThrowOutOfMemory(ctx); }
    size_t n = 0;
    for (size_t i = 0; i < inlen; i++)
        if (!b64_ws((unsigned char)in[i])) clean[n++] = in[i];
    JS_FreeCString(ctx, in);

    if (n % 4 == 0 && n && clean[n - 1] == '=') {
        n -= (n > 1 && clean[n - 2] == '=') ? 2 : 1;
    }
    if (n % 4 == 1) { free(clean); return JS_NULL; }
    for (size_t i = 0; i < n; i++) {
        if (b64_value((unsigned char)clean[i]) < 0) {
            free(clean); return JS_NULL;
        }
    }
    size_t outlen = n * 6 / 8;
    uint8_t *out = outlen ? (uint8_t *)malloc(outlen) : 0;
    if (outlen && !out) { free(clean); return JS_ThrowOutOfMemory(ctx); }
    size_t oi = 0;
    for (size_t i = 0; i < n; i += 4) {
        size_t rem = n - i;
        int a = b64_value((unsigned char)clean[i]);
        int b = rem > 1 ? b64_value((unsigned char)clean[i + 1]) : 0;
        int c = rem > 2 ? b64_value((unsigned char)clean[i + 2]) : 0;
        int d = rem > 3 ? b64_value((unsigned char)clean[i + 3]) : 0;
        out[oi++] = (uint8_t)((a << 2) | (b >> 4));
        if (rem >= 3) out[oi++] = (uint8_t)((b << 4) | (c >> 2));
        if (rem >= 4) out[oi++] = (uint8_t)((c << 6) | d);
    }
    free(clean);
    JSValue v = JS_NewArrayBufferCopy(ctx, out, outlen);
    free(out);
    return v;
}

/* An abort handle names both the slot and the generation that occupied it, so
 * a handle held past the request's death cannot cancel whatever took the slot
 * next. */
static int wf_handle(const struct wfetch *f)
{ return (int)((f - g_fetch) * 4096 + (f->gen & 4095)); }

/* How many `struct wfetch` slots are free RIGHT NOW.
 *
 * The JS request queue needs this because its own liveness counter measures
 * something else. `__fetchStart`'s promise settles when the RESPONSE HEADERS
 * arrive -- that is the spec, and it is what makes streaming work -- but the
 * slot stays occupied until the BODY is finished. So the queue released a
 * permit at header time while C was still holding the slot, admitted the next
 * request, and eventually asked for a ninth slot out of eight.
 *
 * MEASURED on the real machine, kimi.com over the real network:
 *   144 x [error] Uncaught (in promise) TypeError: too many requests in flight
 * with 30 sub-resources arriving out of 108 requests. The page painted its
 * shell and none of its data -- sidebar, logo and feature chips all need
 * calls that were among the 144 thrown away. The incomplete render and the
 * rejections were one bug.
 *
 * Admission gated on THIS number instead of on a JS-side count means the
 * queue is bounded by the real scarce resource. */
static JSValue js_fetch_slots(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
#ifdef WEBAPI_NO_SLOT_QUEUE
    /* The negative control. Admission then depends only on the JS permit
     * count, exactly as it did before -- and the permit is released at header
     * time while the slot is still held, which is the bug. `make
     * test-webapi-slots-negctl` builds this way and requires the queue
     * assertions to FAIL. */
    return JS_NewInt32(ctx, 1 << 20);
#else
    int n = 0;
    for (int i = 0; i < WF_MAX; i++) if (g_fetch[i].state == WF_FREE) n++;
    return JS_NewInt32(ctx, n);
#endif
}

/* __fetchStart(url, method, headerPairs, body, opts) -> { p: Promise, h: id }
 * or, when every slot is busy, { busy: 1 } and NO promise -- see below. */
static JSValue js_fetch_start(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    struct fetch_realm *owner=fetch_realm_for(ctx);
    if(!owner||owner->stopped)return JS_ThrowTypeError(ctx,"fetch realm is closed");

    struct wfetch *f = 0;
    for (int i = 0; i < WF_MAX; i++) if (g_fetch[i].state == WF_FREE) { f = &g_fetch[i]; break; }

    /* A FULL SLOT TABLE IS NOT A FAILED REQUEST, and returning a rejected
     * promise for it was wrong in a way no page can work around: `fetch()`
     * rejects with a TypeError only when the request could not be MADE, and
     * application code treats that as a network error it has no retry for.
     * Being busy is a "not yet". So this reports back to the queue, which
     * re-queues, and nothing is ever rejected for want of a slot. */
#ifndef WEBAPI_NO_SLOT_QUEUE
    if (!f) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "busy", JS_NewInt32(ctx, 1));
        return o;
    }
#endif

    JSValue rf[2];
    JSValue promise = JS_NewPromiseCapability(ctx, rf);
    if (JS_IsException(promise)) return promise;

    const char *msg = 0;
    struct wurl u;
    const char *us = argc > 0 ? JS_ToCString(ctx, argv[0]) : 0;

#ifdef WEBAPI_NO_SLOT_QUEUE
    if (!f) msg = "too many requests in flight";
    else
#endif
    if (!us) msg = "no URL";
    else if (wurl_parse(us, owner->base_valid ? &owner->base : 0, &u) != 0) msg = "invalid URL";
    else if (!fetch_policy_allows(owner, &u)) msg = "request blocked by document connect policy";

    if (!msg) {
        int gen = f->gen;                    /* the generation must survive the wipe */
        memset(f, 0, sizeof *f);
        f->gen = gen;
        f->fd = -1;
        f->owner=owner;f->ctx=ctx;
        f->url = u;
        f->initiator = owner->origin;
        f->initiator_valid = owner->origin_valid;
        f->referrer = owner->base;
        f->referrer_valid = owner->base_valid;
        f->cookie_site=owner->site;f->cookie_site_valid=owner->site_valid;
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
        if (fetch_dial(f) != 0) { f->failure_at = "connect-start"; f->conn.err = H1_E_TRANSPORT; fetch_trace_detail(f); h1_headers_free(&f->hdr); free(f->body); f->body = 0; msg = "could not open a socket"; }
        else if (f->mode == WF_MODE_SAME_ORIGIN && f->cross) {
            g_net->close(f->fd); f->fd = -1;
            h1_headers_free(&f->hdr); free(f->body); f->body = 0;
            msg = "mode 'same-origin' forbids a cross-origin request";
        } else if (wf_needs_preflight(f)) {
            char origin[WURL_MAX], initiator[URL_HOST_MAX + 16];
            fetch_cache_url(f, origin, (int)sizeof origin);
            fetch_preflight_origin(f, initiator, (int)sizeof initiator);
            if (!pfc_hit(initiator, origin, f->method, wf_creds(f), &f->hdr))
                f->phase = WF_PH_PREFLIGHT;
        }
    }
    if (us) JS_FreeCString(ctx, us);

    JSValue out = JS_NewObject(ctx);
    if (msg) {
        /* A rejected promise, not a throw: fetch() rejects, it does not raise. */
        JSValue err = mk_error(ctx, "TypeError", msg);
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
#ifndef WEBAPI_FETCH_NO_OWNER
    if(f->ctx!=ctx||f->owner!=fetch_realm_for(ctx)||f->owner->stopped)return JS_FALSE;
#endif
    fetch_fail(f->ctx, f, "aborted", "AbortError");
    return JS_TRUE;
}

#ifdef WEBAPI_FETCH_TEST_HOOKS
int js_webapi_fetch_test_handle(JSContext *ctx)
{
    for(int i=0;i<WF_MAX;i++)if(g_fetch[i].state!=WF_FREE&&
        (ctx?g_fetch[i].ctx==ctx:g_fetch[i].owner!=g_page_fetch))return wf_handle(&g_fetch[i]);
    return -1;
}
int js_webapi_fetch_test_abort(JSContext *ctx,int handle)
{
    JSValue h=JS_NewInt32(ctx,handle),v=js_fetch_abort(ctx,JS_UNDEFINED,1,(JSValueConst*)&h);
    int result=JS_ToBool(ctx,v);JS_FreeValue(ctx,h);JS_FreeValue(ctx,v);return result;
}
#endif

/* ---- a delay the pump owns --------------------------------------------
 * EventSource has to wait `retry` milliseconds before reconnecting, and it has
 * to do so in the host unit tests too -- where js_page.c (and therefore
 * setTimeout) is not linked at all.  This is NOT a second event loop: the queue
 * is drained from js_webapi_pump, on the same clock the sockets are stepped
 * with, so a delay cannot fire between two steps of anything. */

static JSValue js_later(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct fetch_realm *owner=fetch_realm_for(ctx);
    if(!owner||owner->stopped)return JS_NewInt32(ctx,-1);
    (void)t;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_NewInt32(ctx, -1);
    int32_t ms = 0;
    JS_ToInt32(ctx, &ms, argv[0]);
    if (ms < 0) ms = 0;
    for (int i = 0; i < WT_MAX; i++) {
        if (owner->timers[i].used) continue;
        owner->timers[i].used = 1;
        owner->timers[i].due = now_ms() + (unsigned long long)ms;
        owner->timers[i].fn = JS_DupValue(ctx, argv[1]);
        return JS_NewInt32(ctx, i);
    }
    return JS_NewInt32(ctx, -1);
}

static JSValue js_cancel_later(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct fetch_realm *owner=fetch_realm_for(ctx);
    if(!owner||owner->stopped)return JS_NewInt32(ctx,-1);
    (void)t;
    int32_t id = -1;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= WT_MAX || !owner->timers[id].used) return JS_FALSE;
    owner->timers[id].used = 0;
    JS_FreeValue(ctx, owner->timers[id].fn);
    owner->timers[id].fn = JS_UNDEFINED;
    return JS_TRUE;
}

static int timers_run(JSContext *ctx)
{
    struct fetch_realm *owner=fetch_realm_for(ctx);
    if(!owner||owner->stopped)return 0;
    int ran = 0;
    unsigned long long t = now_ms();
    for (int i = 0; i < WT_MAX; i++) {
        if (!owner->timers[i].used || owner->timers[i].due > t) continue;
        JSValue fn = owner->timers[i].fn;
        owner->timers[i].used = 0;
        owner->timers[i].fn = JS_UNDEFINED;
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, fn);
        ran++;
        if(owner->stopped)break;
    }
    return ran;
}

static int timers_live(JSContext *ctx)
{ struct fetch_realm *owner=fetch_realm_for(ctx);if(!owner||owner->stopped)return 0; for (int i = 0; i < WT_MAX; i++) if (owner->timers[i].used) return 1; return 0; }

static void timers_clear(JSContext *ctx)
{
    struct fetch_realm *owner=fetch_realm_for(ctx);if(!owner)return;
    for (int i = 0; i < WT_MAX; i++) {
        if (!owner->timers[i].used) continue;
        owner->timers[i].used = 0;
        if (ctx) JS_FreeValue(ctx, owner->timers[i].fn);
        owner->timers[i].fn = JS_UNDEFINED;
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
    static char buf[CK_HEADER_MAX];              /* 8 KiB: not on the stack */
    struct cookie_header_diagnostics diag;
    struct cookie_request request = cookie_request_for(
        g_page_fetch ? &g_page_fetch->site : NULL,
        g_page_fetch && g_page_fetch->site_valid, 0, 0);
    int n = cookie_header_with_diagnostics(jar(), &cc, cookie_request_kind(&cc, &request),
                                          now_unix(), buf, (int)sizeof buf, &diag);
    if (n < 0) {
        printf("[webapi] document-cookie error=%d required=%llu cap=%d count=%d\n",
               n, (unsigned long long)diag.required_bytes, (int)sizeof buf, diag.eligible_count);
        return JS_ThrowInternalError(ctx, "Cookie header exceeds capacity");
    }
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
    struct cookie_request request = cookie_request_for(
        g_page_fetch ? &g_page_fetch->site : NULL,
        g_page_fetch && g_page_fetch->site_valid, 0, 0);
    cookie_commit(cookie_set_ex(jar(), &cc, cookie_request_kind(&cc, &request), s, now_unix()));
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

/* The Encoding Standard's label table and single-byte indexes, plus the two
 * lookups the prelude's TextDecoder needs. Data, so it lives apart. */
#include "js_encoding.inc"

/* Workers need the same byte codecs as pages, but js_webapi_install owns ONE
 * page's retained callbacks, location and fetch slots. Calling that installer
 * in a worker would silently retarget the parent's state. This small prelude
 * closes over only immutable table lookups and its own realm's globals. */
/* Correction: full document slots now coexist too. Workers still use their
 * dedicated fetch/encoding realm rather than claiming a document slot. */
static const char ENCODING_REALM_PRELUDE[] =
"(function (__encLabel, __encIndex) { var G = globalThis;\n"
#include "js_encoding_prelude.inc"
"})\n";

int js_webapi_install_encoding(JSContext *ctx)
{
    JSValue fn = JS_Eval(ctx, ENCODING_REALM_PRELUDE,
                        sizeof ENCODING_REALM_PRELUDE - 1,
                        "<encoding realm>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) return -1;
    JSValue args[2] = {
        JS_NewCFunction(ctx, js_enc_label, "__encLabel", 1),
        JS_NewCFunction(ctx, js_enc_index, "__encIndex", 1)
    };
    if (JS_IsException(args[0]) || JS_IsException(args[1])) {
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, fn);
        return -1;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst *)args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, fn);
    int failed = JS_IsException(r);
    JS_FreeValue(ctx, r);
    return failed ? -1 : 0;
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
    if (!strcmp(feat, "prefers-reduced-motion")) {
        int reduced=0;
#ifndef WEBAPI_HOST
        reduced=setting_int("ui.reduce_motion",0)!=0;
#endif
        return !has_val ? reduced : !strcmp(val,reduced?"reduce":"no-preference");
    }
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

/* Optional for network-only host embeddings; the shipping browser always
 * links the cascade. Keep its evaluator authoritative without replacing the
 * live JS list/listeners with CSSOM's former snapshot object. */
extern int css_media_matches(const char *, int) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_media_matches);

static JSValue js_media_match(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_FALSE;
    const char *q = JS_ToCString(ctx, argv[0]);
    if (!q) return JS_FALSE;
#ifndef CSS_LIVE_NEGCTL_MEDIA_SCANNER
    if (LOGIT_HAVE(css_media_matches)) {
        int m = css_media_matches(q, -1);
        JS_FreeCString(ctx, q);
        return JS_NewBool(ctx, m);
    }
#endif
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

/* Correction 2026-09-09: screen.width/height were initialized from g_vw/g_vh,
 * i.e. the browser window. The cascade owns the measured display dimensions;
 * native accessors keep JS and device media queries on that same authority. */
extern int css_screen_width(void) LOGIT_WEAK;
extern int css_screen_height(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_screen_width);
LOGIT_WEAK_STUB(css_screen_height);
static JSValue screen_dimension(JSContext *ctx, JSValueConst t, int magic)
{
    (void)t;
    int n = magic ? (LOGIT_HAVE(css_screen_height) ? css_screen_height() : 0)
                  : (LOGIT_HAVE(css_screen_width) ? css_screen_width() : 0);
    return JS_NewInt32(ctx, n);
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

/* Forward-declared: defined with the rest of the history machinery below,
 * but loc_set (a fragment-only location write is itself a same-document
 * history entry -- see the call below) runs before that section textually. */
static void hist_commit(JSContext *ctx, const char *href, JSValueConst state, int replace);

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
            /* A fragment navigation is a same-document navigation like any
             * other and creates its own joint-session-history entry -- browsers
             * do not special-case this to a replace. Without this, history.go()
             * /back() had nothing to land on for a hash change: g_hist_n never
             * grew, so a page using only location.hash (the pre-pushState way
             * of doing exactly this) could never be traversed. State is always
             * null here -- only pushState attaches one. */
            hist_commit(ctx, want, JS_NULL, 0);
        }
        g_loc = u;
        scopy(g_loc_raw, want, WURL_MAX);
        return JS_UNDEFINED;
    }
    queue_navigation(want);
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
    queue_navigation(g_loc_raw);
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
struct storage_binding { struct storage_key key; char *origin; };
static void storage_finalizer(JSRuntime *rt, JSValue value)
{
    (void)rt;
    struct storage_binding *b = JS_GetOpaque(value, storage_cid);
    if (b) { free(b->origin); free(b); }
}
static JSClassDef storage_class = { "Storage", storage_finalizer, 0, 0, 0 };

static JSValue storage_illegal_ctor(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}
static struct storage_binding *store_of(JSContext *ctx, JSValueConst t)
{ return JS_GetOpaque2(ctx, t, storage_cid); }

static JSValue storage_io_error(JSContext *ctx, int rc)
{
    if(rc==STORAGE_NOMEM)return JS_ThrowOutOfMemory(ctx);
    return JS_Throw(ctx,mk_error(ctx,"InvalidStateError",rc==STORAGE_CORRUPT?
        "localStorage snapshots are corrupt; existing files were preserved":
        "localStorage commit failed; reopen the browser before retrying"));
}
static JSValue st_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct storage_binding *s = store_of(ctx, t);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getItem requires a key");
    size_t kn, vn;
    const char *k = JS_ToCStringLen(ctx, &kn, argv[0]);
    if (!k) return JS_EXCEPTION;
    int status=storage_backend_status(&s->key);
    if(status==STORAGE_CORRUPT || status==STORAGE_NOMEM){JS_FreeCString(ctx,k);return storage_io_error(ctx,status);}
    const char *v = storage_backend_get(&s->key, k, kn, &vn);
    JS_FreeCString(ctx, k);
    return v ? JS_NewStringLen(ctx, v, vn) : JS_NULL;
}
static JSValue st_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct storage_binding *s = store_of(ctx, t);
    if (!s) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setItem requires a key and value");
    size_t kn, vn;
    const char *k = JS_ToCStringLen(ctx, &kn, argv[0]);
    if (!k) return JS_EXCEPTION;
    const char *v = JS_ToCStringLen(ctx, &vn, argv[1]);
    if (!v) { JS_FreeCString(ctx, k); return JS_EXCEPTION; }
    int rc = storage_backend_set(&s->key, k, kn, v, vn);
    JS_FreeCString(ctx, k); JS_FreeCString(ctx, v);
    if (rc == STORAGE_QUOTA) {
#ifdef STORAGE_QUOTA_WRONG_CLASS
        return JS_ThrowRangeError(ctx, "QuotaExceededError: storage is full");
#else
        return JS_Throw(ctx, mk_error(ctx, "QuotaExceededError", "storage is full"));
#endif
    }
    if (rc == STORAGE_NOMEM) return JS_ThrowOutOfMemory(ctx);
    if (rc == STORAGE_IO || rc == STORAGE_CORRUPT) return storage_io_error(ctx,rc);
    if (rc != STORAGE_OK) return JS_ThrowTypeError(ctx, "invalid storage key");
    return JS_UNDEFINED;
}
static JSValue st_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct storage_binding *s = store_of(ctx, t);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "removeItem requires a key");
    size_t kn;
    const char *k = JS_ToCStringLen(ctx, &kn, argv[0]);
    if (!k) return JS_EXCEPTION;
    int rc=storage_backend_remove(&s->key, k, kn);
    JS_FreeCString(ctx, k);
    return rc==STORAGE_OK?JS_UNDEFINED:storage_io_error(ctx,rc);
}
static JSValue st_clear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct storage_binding *s = store_of(ctx, t);
    if (!s) return JS_EXCEPTION;
    int rc=storage_backend_clear(&s->key);
    return rc==STORAGE_OK?JS_UNDEFINED:storage_io_error(ctx,rc);
}
static JSValue st_key(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct storage_binding *s = store_of(ctx, t);
    if (!s) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "key requires an index");
    uint32_t i;
    if (JS_ToUint32(ctx, &i, argv[0]) < 0) return JS_EXCEPTION;
    size_t n;
    int status=storage_backend_status(&s->key);
    if(status==STORAGE_CORRUPT || status==STORAGE_NOMEM)return storage_io_error(ctx,status);
    const char *k = storage_backend_key(&s->key, i, &n);
    return k ? JS_NewStringLen(ctx, k, n) : JS_NULL;
}
static JSValue st_length(JSContext *ctx, JSValueConst t)
{
    struct storage_binding *s = store_of(ctx, t);
    if(!s)return JS_EXCEPTION;
    int status=storage_backend_status(&s->key);
    if(status==STORAGE_CORRUPT || status==STORAGE_NOMEM)return storage_io_error(ctx,status);
    return JS_NewInt32(ctx,(int)storage_backend_length(&s->key));
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

/* tab_hist_joint_extra(): the full-load half of history.length's joint count
 * (see tabs.h's comment on it and tabs.c's on tab_hist_behind). Weak because
 * tests/unit/webapi_test.c links this file WITHOUT tabs.c -- see
 * include/weaksym.h and js_dom.c's layout_count/layout_items for the same
 * idiom in the other direction. */
extern int tab_hist_joint_extra(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(tab_hist_joint_extra);

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

/* Push (replace == 0) or overwrite (replace == 1) the joint session history's
 * CURRENT same-document entry with `href`/`state`. The one place that touches
 * g_hist_n/g_hist_i/g_hist[g_hist_i], so pushState/replaceState (state comes
 * from script) and a plain fragment navigation (location.hash= and friends,
 * where the spec gives the new entry a null state -- only pushState attaches
 * one) cannot drift apart the way the same arithmetic spelled twice in two
 * places always eventually does in this tree (see CLAUDE.md, ONE JAR, TWO
 * DOORS). `state` may be JS_NULL; `ctx` may be NULL only when there is
 * nothing to duplicate (JS_NULL needs no runtime). */
static void hist_commit(JSContext *ctx, const char *href, JSValueConst state, int replace)
{
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
    g_hist[g_hist_i].state = ctx ? JS_DupValue(ctx, state) : JS_NULL;
    scopy(g_hist[g_hist_i].url, href, WURL_MAX);
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
        if (!g_loc_valid || n.https != g_loc.https || strcmp(n.host, g_loc.host) || n.port != g_loc.port)
            return JS_ThrowTypeError(ctx, "history: cross-origin pushState");
        g_loc = n; g_loc_valid = 1;
        wurl_href(&g_loc, href, WURL_MAX);
        scopy(g_loc_raw, href, WURL_MAX);
        set_location(href); /* Relative fetch base follows same-document history. */
    } else {
        scopy(href, g_loc_raw, WURL_MAX);
    }

    hist_commit(ctx, href, argc > 0 ? argv[0] : JS_NULL, replace);
    return JS_UNDEFINED;
}

static JSValue js_pushState(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return hist_push(ctx, t, argc, argv, 0); }
static JSValue js_replaceState(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return hist_push(ctx, t, argc, argv, 1); }

/* Move within the same-document history. popstate is queued rather than fired
 * inline: the spec makes it a task, and firing it inside history.back() would
 * re-enter the page's own call stack.
 *
 * A traversal that lands on an entry differing from where it started ONLY by
 * fragment fires hashchange TOO, alongside popstate -- WHATWG 7.2.4's
 * "traverse the history" always updates the document (popstate) for a
 * same-document entry, and separately runs the fragment-navigation steps
 * (hashchange) when the non-fragment parts of the two URLs match. Parsed
 * rather than string-compared: '#' inside an already-encoded path or query is
 * data, not the fragment delimiter, and same_document() (used by loc_set for
 * the same question) already does this correctly. */
static void hist_move(JSContext *ctx, int delta)
{
    int want = g_hist_i + delta;
    if (want < 0 || want >= g_hist_n || delta == 0) return;
    char oldurl[WURL_MAX];
    scopy(oldurl, g_hist[g_hist_i].url, WURL_MAX);
    g_hist_i = want;
    const char *newurl = g_hist[g_hist_i].url;

    struct wurl ou, nu;
    if (wurl_parse(oldurl, 0, &ou) == 0 && wurl_parse(newurl, 0, &nu) == 0 &&
        same_document(&ou, &nu) && strcmp(ou.hash, nu.hash) != 0) {
        scopy(g_hash_old, oldurl, WURL_MAX);
        scopy(g_hash_new, newurl, WURL_MAX);
        g_hashchange_queued = 1;
    }

    set_location(newurl);
    JS_FreeValue(ctx, g_popstate_state);
    g_popstate_state = JS_DupValue(ctx, g_hist[g_hist_i].state);
    g_popstate_queued = 1;
}

/* The browser-chrome direction of this seam -- see js_webapi_hist_step's
 * comment in js_webapi.h for the full argument. `delta` is usually the ±1 a
 * physical Back/Forward press means; any nonzero value is honoured the same
 * way history.go() honours one. */
int js_webapi_hist_step(JSContext *ctx, int delta, char *out, int max)
{
    int want = g_hist_i + delta;
    if (delta == 0 || want < 0 || want >= g_hist_n) return 0;
    hist_move(ctx, delta);
    if (out) scopy(out, g_hist[g_hist_i].url, max);
    return 1;
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
/* g_hist_n alone undercounts: it is the CURRENT document's own same-document
 * entries only, reset to 1 on every real navigation. The exact WHATWG number
 * is g_hist_n plus tab_hist_joint_extra() -- the full-load entries behind and
 * (in the narrow Back-then-not-yet-navigated window) ahead of this tab's
 * position, which live in tabs.c and cannot be duplicated here without the
 * two counters eventually disagreeing (see hist_commit's comment). */
static JSValue hist_get_len(JSContext *ctx, JSValueConst t)
{
    (void)t;
    int extra = LOGIT_HAVE(tab_hist_joint_extra) ? tab_hist_joint_extra() : 0;
    return JS_NewInt32(ctx, g_hist_n + extra);
}
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
#ifdef JS_RUNTIME_DIAGNOSTICS
#include "js_runtime_diagnostics.inc"
#endif
static const char *PRELUDE =
"(function (__fetchStart, __utf8, __urlParse, __mediaMatch, __fetchAbort, __later,\n"
"          __cancelLater, __fetchSlots, __encLabel, __encIndex, __base64Decode, __xhrDiag) {\n"
"'use strict';\n"
"var G = globalThis;\n"

#include "js_fetch_body_prelude.inc"

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

#include "js_fetch_object_url_prelude.inc"

/* ---- XMLHttpRequest, over fetch ----
 * Async only. abort() is now a REAL abort: it aborts the AbortController the
 * send() started with, which closes the socket, so the transfer stops on the
 * wire rather than merely stopping being delivered. And because the body now
 * arrives in pieces, readyState 3 and `progress` are real events with real
 * partial responseText behind them, not a pair fired back to back once
 * everything had already been buffered. */
/* Event callback exceptions belong to the page's error reporting channel.
 * They must not enter fetch's rejection chain (HEADERS_RECEIVED used to
 * become status=0), or strand the separate body Promise from a progress
 * callback. Isolate each listener so later listeners and completion run. */
#ifdef JS_RUNTIME_DIAGNOSTICS
"var xhrDiagRecords = new WeakMap(), xhrResponseDiag = new WeakMap(), xhrDiagNext = 0;\n"
"var xhrDiagGet = Function.prototype.call.bind(WeakMap.prototype.get), xhrDiagSet = Function.prototype.call.bind(WeakMap.prototype.set);\n"
"function xhrDiagEvent(t) { return t === 'readystatechange' ? 1 : t === 'progress' ? 2 : t === 'load' ? 3 : t === 'error' ? 4 : t === 'abort' ? 5 : t === 'loadend' ? 6 : 0; }\n"
#endif
"function xhrInvoke(self, fn, ev, event) {\n"
#ifndef XHR_CALLBACKS_PROPAGATE
"  try {\n"
#endif
"    if (typeof fn === 'function') fn.call(self, ev);\n"
"    else if (fn && typeof fn.handleEvent === 'function') fn.handleEvent(ev);\n"
#ifndef XHR_CALLBACKS_PROPAGATE
"  } catch (e) {\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"    var diag = xhrDiagGet(xhrDiagRecords, self);\n"
"    if (diag) __xhrDiag(1, diag.id, diag.fetch, xhrDiagEvent(event), e);\n"
#endif
"    try { if (typeof G.reportError === 'function') G.reportError(e);\n"
"      else if (G.console && typeof G.console.error === 'function') G.console.error(e); } catch (ignored) {}\n"
"  }\n"
#endif
"}\n"
/* A page may wrap readonly prototype getters before constructing an XHR.
 * Qwen's actual stat.js did so on 2026-09-13: the old constructor's assignment
 * to responseText threw before any request could start. Native response state
 * must be independent of those public descriptors; keep it in a private map
 * and publish real getters, also for updates after headers/body/abort. */
#ifdef XHR_PUBLIC_RESPONSE_LEGACY
"function xhrStateNew(self) { return self; }\n"
"function xhrStateGet(self) { return self; }\n"
#else
"var xhrStates = new WeakMap();\n"
"var xhrSlotGet = Function.prototype.call.bind(WeakMap.prototype.get);\n"
"var xhrSlotSet = Function.prototype.call.bind(WeakMap.prototype.set);\n"
"function xhrStateNew(self) { var s = {}; xhrSlotSet(xhrStates, self, s); return s; }\n"
"function xhrStateGet(self) { var s = xhrSlotGet(xhrStates, self); if (!s) throw new TypeError('Illegal XMLHttpRequest receiver'); return s; }\n"
#endif
"G.XMLHttpRequest = function XMLHttpRequest() {\n"
"  var state = xhrStateNew(this);\n"
"  state.readyState = 0; state.status = 0; state.statusText = ''; state.responseText = '';\n"
"  state.response = ''; state.responseURL = ''; state.responseType = ''; state.timeout = 0;\n"
"  state.withCredentials = false; state.upload = {};\n"
"  state.onreadystatechange = null; state.onload = null; state.onerror = null;\n"
"  state.onloadend = null; state.onabort = null; state.ontimeout = null; state.onprogress = null;\n"
"  this._h = []; this._hdr = null; this._ev = {}; this._aborted = false; this._ac = null;\n"
"};\n"
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
"    xhrStateGet(this).readyState = 0; this._fire('abort'); this._fire('loadend'); },\n"
"  _fire: function (t, loaded, total) { var e = { type: t, target: this, currentTarget: this,\n"
"      lengthComputable: total > 0, loaded: loaded || 0, total: total || 0 };\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"    var diag = xhrDiagGet(xhrDiagRecords, this);\n"
"    if (diag) { if (t === 'progress') diag.progress++; else if (t === 'load') diag.load++; else if (t === 'error') diag.error++; else if (t === 'abort') diag.abort++;\n"
"      if (t === 'loadend') __xhrDiag(4, diag.id, diag.fetch, diag.progress, diag.load, diag.error, diag.abort, diag.received, diag.chars); }\n"
#endif
"    xhrInvoke(this, this['on' + t], e, t);\n"
"    (this._ev[t] || []).slice().forEach(function (f) { xhrInvoke(this, f, e, t); }, this); },\n"
"  _rs: function (s) { xhrStateGet(this).readyState = s; this._fire('readystatechange'); },\n"
"  send: function (body) {\n"
"    var self = this, state = xhrStateGet(this);\n"
"    var received = 0, total = 0;\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"    var diag = {id: ++xhrDiagNext, fetch: -1, json: 0, progress: 0, load: 0, error: 0, abort: 0, received: 0, chars: 0};\n"
"    xhrDiagSet(xhrDiagRecords, self, diag);\n"
#endif
"    self._ac = new G.AbortController();\n"
"    G.fetch(this._u, { method: this._m || 'GET', headers: this._h, body: body,\n"
"                       signal: self._ac.signal,\n"
"                       credentials: state.withCredentials ? 'include' : 'same-origin' })\n"
"      .then(function (r) {\n"
"        if (self._aborted) return null;\n"
"        self._hdr = r.headers; state.status = r.status; state.statusText = r.statusText;\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"        var meta = xhrDiagGet(xhrResponseDiag, r);\n"
"        if (meta) { diag.fetch = meta.fetch; diag.json = meta.json; __xhrDiag(0, diag.id, diag.fetch, meta.status, diag.json); }\n"
#endif
"        var length = r.headers.get('content-length');\n"
"        if (length !== null && /^[0-9]+$/.test(length)) { var n = Number(length); if (n > 0 && n <= 9007199254740991) total = n; }\n"
"        state.responseURL = r.url; self._rs(2);\n"
"        if (!r.body) return state.responseType === 'arraybuffer' ? new ArrayBuffer(0) : '';\n"
"        var binary = state.responseType === 'arraybuffer';\n"
"        var rd = r.body.getReader(), dec = binary ? null : new G.TextDecoder(), text = '', chunks = binary ? [] : null;\n"
"        return new Promise(function (res, rej) {\n"
"          (function loop() {\n"
"            rd.read().then(function (c) {\n"
"              if (self._aborted) { res(null); return; }\n"
"              if (c.done) {\n"
"                if (binary) { var out = new Uint8Array(received), off = 0;\n"
"                  chunks.forEach(function (part) { out.set(part, off); off += part.byteLength; });\n"
"                  res(out.buffer); return; }\n"
"                text += dec.decode(new Uint8Array(0)); res(text); return; }\n"
"              received += c.value.byteLength;\n"
"              if (binary) chunks.push(new Uint8Array(c.value));\n"
"              else text += dec.decode(c.value, { stream: true });\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"              diag.received = received; diag.chars = text.length;\n"
#endif
"              if (!binary) { state.responseText = text;\n"
"                if (state.responseType !== 'json') state.response = text; }\n"
"              if (state.readyState !== 3) self._rs(3);\n"
"              self._fire('progress', received, total);\n"
"              loop();\n"
"            }, rej);\n"
"          })();\n"
"        });\n"
"      })\n"
"      .then(function (t) {\n"
"        if (self._aborted || t === null) return;\n"
"        if (state.responseType === 'arraybuffer') { state.responseText = ''; state.response = t; }\n"
"        else state.responseText = t;\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"        diag.chars = state.responseType === 'arraybuffer' ? t.byteLength : t.length;\n"
"        __xhrDiag(3, diag.id, diag.fetch, received, diag.chars, diag.json, diag.json && t.length <= 65536 ? t : undefined);\n"
#endif
"        if (state.responseType === 'json') { try { state.response = JSON.parse(t); } catch (e) { state.response = null; } }\n"
"        else if (state.responseType !== 'arraybuffer') state.response = t;\n"
"        if (state.readyState !== 3) self._rs(3);\n"
/* The decoder may only emit its final replacement/code point at EOF. XHR's
 * terminal progress event must expose that final responseText before DONE,
 * load and loadend, including an empty body; chunk callbacks alone miss it. */
#ifndef XHR_NO_FINAL_PROGRESS
"        self._fire('progress', received, total);\n"
"        if (self._aborted) return;\n"
#endif
"        self._rs(4);\n"
"        self._fire('load', received, total); self._fire('loadend', received, total);\n"
"      })\n"
"      .catch(function (e) {\n"
"        if (self._aborted) return;\n"
#ifdef JS_RUNTIME_DIAGNOSTICS
"        __xhrDiag(2, diag.id, diag.fetch, e);\n"
#endif
"        state.status = 0; self._rs(4); self._fire('error'); self._fire('loadend');\n"
"      });\n"
"  }\n"
"};\n"
#ifndef XHR_PUBLIC_RESPONSE_LEGACY
"['readyState','status','statusText','responseText','response','responseURL','upload'].forEach(function (key) {\n"
"  Object.defineProperty(G.XMLHttpRequest.prototype, key, {configurable: true, enumerable: true, get: function () { return xhrStateGet(this)[key]; }});\n"
"});\n"
"['responseType','timeout','withCredentials','onreadystatechange','onload','onerror','onloadend','onabort','ontimeout','onprogress'].forEach(function (key) {\n"
"  Object.defineProperty(G.XMLHttpRequest.prototype, key, {configurable: true, enumerable: true, get: function () { return xhrStateGet(this)[key]; }, set: function (value) { xhrStateGet(this)[key] = value; }});\n"
"});\n"
#endif
/* WebIDL constants live on BOTH the interface and its prototype. A normal
 * XHR adapter compares request.readyState with request.HEADERS_RECEIVED; the
 * old constructor-only assignments made that comparison silently false, so
 * its one-shot headers listener was removed without delivering the headers.
 * This does not by itself prevent independent progress/body listeners. */
"['UNSENT','OPENED','HEADERS_RECEIVED','LOADING','DONE'].forEach(function (name, value) {\n"
"  Object.defineProperty(G.XMLHttpRequest, name, {value: value, enumerable: true});\n"
#ifndef XHR_CONSTANTS_CONSTRUCTOR_ONLY
"  Object.defineProperty(G.XMLHttpRequest.prototype, name, {value: value, enumerable: true});\n"
#endif
"});\n"
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

#include "js_fetch_hooks_prelude.inc"

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

/* The worker shares the exact Fetch/Body/stream implementation; its native
 * URL is already installed. No document, viewport, storage or history surface
 * is evaluated here, and its object URLs have a separate realm-owned table. */
#define WEBAPI_FETCH_WORKER_PRELUDE
static const char *WORKER_FETCH_PRELUDE =
"(function (__fetchStart, __utf8, __urlParse, __mediaMatch, __fetchAbort, __later,\n"
"          __cancelLater, __fetchSlots, __encLabel, __encIndex, __base64Decode) {\n"
"'use strict'; var G = globalThis;\n"
#include "js_fetch_body_prelude.inc"
#include "js_fetch_object_url_prelude.inc"
#include "js_fetch_hooks_prelude.inc"
"};\n})\n";
#undef WEBAPI_FETCH_WORKER_PRELUDE

static JSValue g_fire_fn = JS_UNDEFINED;   /* the prelude's popstate/hashchange dispatcher */
static JSValue g_blob_fn = JS_UNDEFINED;
static char g_blob_origin[URL_HOST_MAX + 16];
static unsigned long long g_blob_generation;

/* The closure also owns a generation. A native embedder may close and reopen
 * the same JSContext while an old reader/abort closure is still reachable;
 * context equality alone would let it operate on the replacement realm. */
static JSValue fetch_bound_call(JSContext *ctx,JSValueConst self,int argc,
                                JSValueConst *argv,int magic,JSValue *data)
{
    uint32_t identity=0;JS_ToUint32(ctx,&identity,data[0]);
    struct fetch_realm *r=fetch_realm_for(ctx);
    if(!r||r->stopped||r->identity!=identity){
        if(magic==0)return JS_ThrowTypeError(ctx,"fetch realm is closed");
        return magic==2?JS_NewInt32(ctx,0):JS_NewInt32(ctx,-1);
    }
    switch(magic){
    case 0:return js_fetch_start(ctx,self,argc,argv);
    case 1:return js_fetch_abort(ctx,self,argc,argv);
    case 2:return js_fetch_slots(ctx,self,argc,argv);
    case 3:return js_later(ctx,self,argc,argv);
    default:return js_cancel_later(ctx,self,argc,argv);
    }
}
static JSValue fetch_binding(JSContext *ctx,int kind,int argc)
{
    struct fetch_realm *r=fetch_realm_for(ctx);
    JSValue token=JS_NewUint32(ctx,r->identity);
    JSValue fn=JS_NewCFunctionData(ctx,fetch_bound_call,argc,kind,1,(JSValueConst*)&token);
    JS_FreeValue(ctx,token);return fn;
}

/* Install the shared fetch surface into an already-created Worker context.
 * Its pure URL constructor and worker timers are installed by js_worker.c.
 * No parent callback is used as a proxy: all values originate in ctx. */
int js_webapi_fetch_install(JSContext *ctx,const char *base_url,
                           const char *origin_url,const char *site_url)
{
    struct fetch_realm *owner=fetch_realm_new(ctx,base_url,origin_url,site_url);
    if(!owner)return -1;
    JSValue fn=JS_Eval(ctx,WORKER_FETCH_PRELUDE,strlen(WORKER_FETCH_PRELUDE),
                       "<worker-fetch>",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(fn)){JS_FreeValue(ctx,fn);js_webapi_fetch_close(ctx);return -1;}
    JSValue args[11]={
        fetch_binding(ctx,0,5),
        JS_NewCFunction(ctx,js_utf8,"utf8",1),JS_UNDEFINED,JS_UNDEFINED,
        fetch_binding(ctx,1,1),
        fetch_binding(ctx,3,2),
        fetch_binding(ctx,4,1),
        fetch_binding(ctx,2,0),
        JS_NewCFunction(ctx,js_enc_label,"encodingLabel",1),
        JS_NewCFunction(ctx,js_enc_index,"encodingIndex",1),
        JS_NewCFunction(ctx,js_base64_decode,"base64Decode",1)};
    JSValue hooks=JS_Call(ctx,fn,JS_UNDEFINED,11,(JSValueConst*)args);
    for(int i=0;i<11;i++)JS_FreeValue(ctx,args[i]);JS_FreeValue(ctx,fn);
    if(JS_IsException(hooks)){JS_FreeValue(ctx,hooks);js_webapi_fetch_close(ctx);return -1;}
    owner->mk_response=JS_GetPropertyStr(ctx,hooks,"mkResponse");
    owner->mk_error=JS_GetPropertyStr(ctx,hooks,"mkError");
    char origin[URL_HOST_MAX+16],generation[24];
    if(owner->origin_valid)wurl_origin(&owner->origin,origin,sizeof origin);else strcpy(origin,"null");
    snprintf(generation,sizeof generation,"%llu",++g_blob_generation);
    JSValue configure=JS_GetPropertyStr(ctx,hooks,"blobConfigure");
    JSValue config[2]={JS_NewString(ctx,origin),JS_NewString(ctx,generation)};
    JSValue result=JS_Call(ctx,configure,JS_UNDEFINED,2,(JSValueConst*)config);
    JS_FreeValue(ctx,config[0]);JS_FreeValue(ctx,config[1]);JS_FreeValue(ctx,configure);
    JS_FreeValue(ctx,hooks);
    int failed=JS_IsException(result);JS_FreeValue(ctx,result);
    if(failed){js_webapi_fetch_close(ctx);return -1;}
    return 0;
}

void js_webapi_fetch_stop(JSContext *ctx)
{
    struct fetch_realm *r=fetch_realm_for(ctx);if(r)r->stopped=1;
}

void js_webapi_fetch_close(JSContext *ctx)
{
    struct fetch_realm *r=fetch_realm_for(ctx);if(!r)return;
    r->stopped=1;
    /* No JS callbacks during destruction. Release sockets and retained values
     * with their owning context before that runtime's GC tears it down. */
    for(int i=0;i<WF_MAX;i++)if(g_fetch[i].state!=WF_FREE&&g_fetch[i].owner==r)
        fetch_release(ctx,&g_fetch[i]);
    timers_clear(ctx);
    JS_FreeValue(ctx,r->mk_response);JS_FreeValue(ctx,r->mk_error);
    struct fetch_realm **link=&g_fetch_realms;
    while(*link&&*link!=r)link=&(*link)->next;
    if(*link)*link=r->next;
    if(g_page_fetch==r)g_page_fetch=NULL;
    free(r);
}

int js_webapi_fetch_pending(JSContext *ctx)
{
    struct fetch_realm *r=fetch_realm_for(ctx);if(!r||r->stopped)return 0;
    for(int i=0;i<WF_MAX;i++)if(g_fetch[i].state!=WF_FREE&&g_fetch[i].owner==r)return 1;
    return timers_live(ctx);
}

long long js_webapi_fetch_next_due(JSContext *ctx)
{
    struct fetch_realm *r=fetch_realm_for(ctx);if(!r||r->stopped)return -1;
    long long next=-1;
    for(int i=0;i<WF_MAX;i++)if(g_fetch[i].state!=WF_FREE&&g_fetch[i].owner==r){next=(long long)now_ms()+16;break;}
    for(int i=0;i<WT_MAX;i++)if(r->timers[i].used&&(next<0||(long long)r->timers[i].due<next))
        next=(long long)r->timers[i].due;
    return next;
}

int js_webapi_fetch_checkpoint(JSContext *ctx)
{
    struct fetch_realm *r=fetch_realm_for(ctx);if(!r||r->stopped)return 0;
    int ran=0;
    for(int i=0;i<WF_MAX&&!r->stopped;i++)if(g_fetch[i].state!=WF_FREE&&g_fetch[i].owner==r)
        ran+=fetch_step(ctx,&g_fetch[i]);
    return ran;
}

int js_webapi_fetch_pump(JSContext *ctx)
{
    int ran=js_webapi_fetch_checkpoint(ctx);
    struct fetch_realm *r=fetch_realm_for(ctx);
    if(r&&!r->stopped)ran+=timers_run(ctx);
    return ran;
}

int js_webapi_blob_snapshot(JSContext *ctx, const char *url,
    unsigned char **out, int *length, int max_bytes, char *origin, int origin_cap)
{
    if(out)*out=0;if(length)*length=0;if(origin&&origin_cap>0)origin[0]=0;
    if(!ctx||ctx!=g_webapi_ctx||!url||!JS_IsFunction(ctx,g_blob_fn))return 0;
    if(!out||!length||!origin||max_bytes<0||origin_cap<=(int)strlen(g_blob_origin))return -1;
    JSValue arg=JS_NewString(ctx,url);
    JSValue bytes=JS_Call(ctx,g_blob_fn,JS_UNDEFINED,1,(JSValueConst*)&arg);
    JS_FreeValue(ctx,arg);
    if(JS_IsException(bytes)){JS_FreeValue(ctx,JS_GetException(ctx));return -1;}
    if(JS_IsNull(bytes)||JS_IsUndefined(bytes)){JS_FreeValue(ctx,bytes);return 0;}
    size_t offset=0,len=0,unit=0,total=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,bytes,&offset,&len,&unit);
    if(JS_IsException(buffer)){JS_FreeValue(ctx,JS_GetException(ctx));JS_FreeValue(ctx,bytes);return -1;}
    unsigned char *data=JS_GetArrayBuffer(ctx,&total,buffer);
    int valid=unit==1&&len<=(size_t)max_bytes&&offset<=total&&len<=total-offset&&(data||!len);
    unsigned char *copy=valid?malloc(len+1):0;
    if(copy){if(len)memcpy(copy,data+offset,len);copy[len]=0;}
    JS_FreeValue(ctx,buffer);JS_FreeValue(ctx,bytes);
    if(!copy)return -1;
    *out=copy;*length=(int)len;scopy(origin,g_blob_origin,origin_cap);return 1;
}

/* ---- install / close / pump ------------------------------------------ */

static JSValue make_storage(JSContext *ctx, const char *origin, int session)
{
    JSValue o = JS_NewObjectClass(ctx, (int)storage_cid);
    if (JS_IsException(o)) return o;
    /* Wrappers keep an immutable partition key, never a recycled area slot.
     * Empty reads do not allocate backend areas, and clear/drop can release
     * their slots without redirecting a surviving wrapper to another origin. */
    struct storage_binding *b = calloc(1, sizeof *b);
    size_t n = strlen(origin);
    if (b) b->origin = malloc(n + 1);
    if (!b || !b->origin) { free(b); JS_FreeValue(ctx, o); return JS_ThrowOutOfMemory(ctx); }
    memcpy(b->origin, origin, n + 1);
    b->key.origin = b->origin;
    b->key.kind = session ? STORAGE_SESSION : STORAGE_LOCAL;
    b->key.session_id = g_storage_session;
    JS_SetOpaque(o, b);
    return o;
}

static int g_vp_dirty;
void js_webapi_media_changed(void) { g_vp_dirty = 1; }

void js_webapi_set_viewport(int w, int h)
{
    /* The embedder updates css_viewport first: listeners and stylesheet
     * matching must observe one viewport when the deferred pump runs. */
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
    if (g_webapi_ctx) {
        /* Refuse a second install in this slot. Other document slots own
         * separate hooks/history and stable fetch realms. */
        printf("[webapi] install refused: a realm is already active\n");
        return;
    }
    g_webapi_ctx = ctx;
    set_location(url);
    g_page_fetch=fetch_realm_new(ctx,url,url,g_embedded_site ? g_site_url : url);
    if(!g_page_fetch){g_webapi_ctx=NULL;return;}
    hist_reset(0, g_loc_raw);
    g_popstate_state = JS_NULL;
    g_popstate_queued = g_hashchange_queued = 0;

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
            /* document.domain = the document's hostname (a STRING, never
             * undefined). bilibili's log-reporter does
             * `document.domain.split('.')` unconditionally in getCurrentDomain,
             * and an undefined here threw "cannot read property 'split' of
             * undefined" and took the whole telemetry init with it. Reported
             * as the effective domain, which for our single-origin document is
             * just the host. A plain data property, so a page that narrows it
             * (`document.domain = 'bilibili.com'`, the legacy cross-frame
             * move) has the write STORED and reads its own value back -- which
             * is what a real browser does too, and here it affects nothing
             * because we have no frames. It affects no COOKIE either: ck_ctx()
             * builds every cookie context from g_loc.host, never from this
             * property, so writing it cannot widen what the page may set.
             * Empty string for an opaque-origin document, where `.split('.')`
             * gives [""], still not a throw. */
            JS_SetPropertyStr(ctx, doc, "domain",
                              JS_NewString(ctx, g_loc_valid ? g_loc.host : ""));
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

    /* navigator.cookieEnabled -- the property that ADVERTISES the accessor
     * above, and it was answering no while the jar worked.
     *
     * js_page.c:610 publishes it as false with the comment "no jar on this
     * path", and that comment is true OF THAT FILE: js_page.c is deliberately
     * host-linkable with no transport, and a build that drops this TU (the
     * weak js_webapi_install, see js_webapi.h) really has no jar and must keep
     * answering false. But js_page.c calls js_webapi_install twenty lines
     * later, so on the browser's own path the jar IS present and the answer
     * was still no. This file is the one that knows, so it answers here; the
     * false in js_page.c stays, and stays correct, for the build with no us.
     *
     * A lie in this direction is worse than an absent property, for exactly
     * the reason a cookie getter returning "" is worse than no getter: a page
     * CAN tell absent from false, and false is an affirmative answer it acts
     * on. Nothing throws, so no exception counter on the scoreboard can see
     * it -- the page renders correctly, minus a feature.
     *
     * MEASURED, both callers in tests/fixtures/webapi/ in this tree:
     *   baidureal/s010.js:104  `function close(){if(navigator.cookieEnabled){
     *                           document.cookie="su=0; domain=www.baidu.com"}}`
     *                          -- a document.cookie WRITE that never ran; and
     *                          :86 `!bds.se.sugStorage.isSupport()||
     *                          !navigator.cookieEnabled||...` skips the whole
     *                          suggestion store before it reads the cookie.
     *   bing/index.html:34     `navigator.cookieEnabled||r("COOKIEDISABLED")`,
     *                          and the dark-mode reload falls into
     *                          `f("dmnoreload_cookieenabled_"+...)` instead.
     *
     * The answer is g_loc_valid, not a constant 1, and that is the same
     * condition that decides whether document.cookie is a working accessor or
     * a pair of no-ops (js_cookie_get/js_cookie_set return ""/undefined when
     * it is 0). For a document whose URL url.c cannot parse -- about:blank,
     * anything that is not http(s) -- cookies genuinely do not work here, and
     * saying so is the truthful answer rather than the flattering one.
     *
     * Only if `navigator` already exists: js_page.c owns that object, and an
     * embedding that publishes no navigator gets no invented one from us.
     * js_platform.c runs after this and fills navigator gaps with an
     * only-if-absent `def`, so it cannot take the answer back. */
#ifndef WEBAPI_NO_COOKIE_ENABLED      /* the negative control: test-logreporter-negctl */
    {
        JSValue nav = JS_GetPropertyStr(ctx, g, "navigator");
        if (JS_IsObject(nav))
            JS_SetPropertyStr(ctx, nav, "cookieEnabled", JS_NewBool(ctx, g_loc_valid));
        JS_FreeValue(ctx, nav);
    }
#endif

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

    /* Finalizers in an inactive runtime still use this process-wide ID. */
    if (!storage_cid) JS_NewClassID(&storage_cid);
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

    /* THE SAME OMISSION AS Storage, SEVEN MORE TIMES, AND NOBODY GENERALISED IT.
     *
     * The Storage block above records why an interface object matters: kimi's
     * entire application failed to boot on `ReferenceError: 'Storage' is not
     * defined`, thrown by a bare reference in a feature check, over a name that
     * was never published. That was fixed -- for Storage, and separately for
     * Document, and for nothing else.
     *
     * MEASURED 2026-08-29, asking each singleton and its interface in one page:
     *     navigator   object  ->  Navigator    undefined
     *     location    object  ->  Location     undefined
     *     history     object  ->  History      undefined
     *     screen      object  ->  Screen       undefined
     *     performance object  ->  Performance  undefined
     *     console     object  ->  Console      undefined
     *     crypto      object  ->  Crypto       undefined
     * Seven instances a page can reach and seven interfaces it cannot. And it
     * is not hypothetical: douyin.com throws
     * `ReferenceError: 'Navigator' is not defined` on load and renders a
     * sidebar and nothing else -- 29 painted text runs over 2,276 changed
     * pixels, which is the shape of a page that started and stopped.
     *
     * DERIVED, NOT LISTED, and that is the point of doing it this way. Each
     * interface object takes its prototype from the instance that is already
     * installed, so `navigator instanceof Navigator` is true by construction
     * rather than by a second declaration that could disagree with the first.
     * A new singleton added above this loop gets its interface by adding one
     * row, and a singleton that is NOT installed is skipped rather than
     * publishing an interface for an object that does not exist -- an
     * interface with no instance would be a new way to lie about a capability.
     *
     * Calling any of them throws, as every interface object on the platform
     * must: `new Navigator()` is illegal.
     *
     * NOT DONE HERE, and named rather than left to be rediscovered:
     * CSSStyleDeclaration is also undefined, and it does not fit this loop
     * because it has no singleton -- its instances are element.style, which
     * needs a document that does not exist when this runs. */
    {
        static const struct { const char *inst, *iface; } singletons[] = {
            { "navigator",   "Navigator"   },
            { "location",    "Location"    },
            { "history",     "History"     },
            { "screen",      "Screen"      },
            { "performance", "Performance" },
            { "console",     "Console"     },
            { "crypto",      "Crypto"      },
        };
        for (unsigned i = 0; i < sizeof singletons / sizeof singletons[0]; i++) {
            JSValue have = JS_GetPropertyStr(ctx, g, singletons[i].iface);
            int already = !JS_IsUndefined(have);
            JS_FreeValue(ctx, have);
            if (already) continue;          /* somebody else published it */

            JSValue inst = JS_GetPropertyStr(ctx, g, singletons[i].inst);
            if (!JS_IsObject(inst)) { JS_FreeValue(ctx, inst); continue; }

            /* A FRESH PROTOTYPE, INSERTED. The obvious version of this loop
             * took the instance's EXISTING prototype and handed it to the
             * constructor -- and every one of these singletons is a plain
             * object whose prototype is Object.prototype, so that published
             * seven interfaces whose .prototype WAS Object.prototype.
             *
             * Measured, on the first build:
             *     ({}) instanceof Navigator          = true
             *     []   instanceof Navigator          = true
             *     document instanceof Navigator      = true
             *     navigator instanceof Location      = true
             * Every object in the page an instance of every interface. That is
             * far worse than the interfaces being absent, and it is the exact
             * present-and-wrong shape js_platform.h spends a page warning
             * about: absence makes a feature check take its fallback, a wrong
             * answer makes it take the branch it cannot follow.
             *
             * AND THE FIRST CONTROL PASSED. It asserted
             * `navigator instanceof Navigator`, which was true -- for the
             * wrong reason, because navigator is an object and Object.prototype
             * is in every object's chain. A control that cannot distinguish the
             * fix from the catastrophe is not a control. The gate now asserts
             * the NEGATIVE as well, which is the half that has teeth.
             *
             * So: mint a prototype per interface and splice it in between the
             * instance and Object.prototype. The chain is one link longer and
             * otherwise unchanged, so every property the instance already had
             * still resolves. */
            JSValue proto = JS_NewObject(ctx);
            if (!JS_IsObject(proto)) { JS_FreeValue(ctx, proto); JS_FreeValue(ctx, inst); continue; }
            JS_SetPrototype(ctx, inst, proto);
            JS_FreeValue(ctx, inst);

            JSValue ctor = JS_NewCFunction2(ctx, storage_illegal_ctor,
                                            singletons[i].iface, 0,
                                            JS_CFUNC_constructor, 0);
            JS_SetConstructor(ctx, ctor, proto);
            JS_SetPropertyStr(ctx, g, singletons[i].iface, ctor);
            JS_FreeValue(ctx, proto);
        }
    }

    /* Viewport metrics, only if nothing else claimed them. */
    {
        static const char *const names[] = { "innerWidth", "innerHeight", "outerWidth", "outerHeight" };
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
        const char *screen_names[] = { "width", "height" };
        for (int i = 0; i < 2; i++) {
            JSAtom a = JS_NewAtom(ctx, screen_names[i]);
            JS_DefinePropertyGetSet(ctx, scr, a,
                JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)screen_dimension,
                                     screen_names[i], 0, JS_CFUNC_getter_magic, i),
                JS_UNDEFINED, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, a);
        }
        /* availWidth/availHeight deliberately absent: the GUI screen syscall
         * does not report the work area reserved by desktop panels. Reporting
         * the window or full screen there would fabricate a different fact. */
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
    JSValue args[12];
    args[0] = fetch_binding(ctx,0,5);
    args[1] = JS_NewCFunction(ctx, js_utf8, "__utf8", 1);
    args[2] = JS_NewCFunction(ctx, js_url_parse, "__urlParse", 2);
    args[3] = JS_NewCFunction(ctx, js_media_match, "__mediaMatch", 1);
    args[4] = fetch_binding(ctx,1,1);
    args[5] = fetch_binding(ctx,3,2);
    args[6] = fetch_binding(ctx,4,1);
    args[7] = fetch_binding(ctx,2,0);
    args[8] = JS_NewCFunction(ctx, js_enc_label, "__encLabel", 1);
    args[9] = JS_NewCFunction(ctx, js_enc_index, "__encIndex", 1);
    args[10] = JS_NewCFunction(ctx, js_base64_decode, "__base64Decode", 1);
    int prelude_argc = 11;
#ifdef JS_RUNTIME_DIAGNOSTICS
    args[prelude_argc++] = make_xhr_diag_hook(ctx);
#endif
    JSValue hooks = JS_Call(ctx, fn, JS_UNDEFINED, prelude_argc, (JSValueConst *)args);
    for (int i = 0; i < prelude_argc; i++) JS_FreeValue(ctx, args[i]);
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
    g_mk_error         = JS_GetPropertyStr(ctx, hooks, "mkError");
    g_page_fetch->mk_response=JS_DupValue(ctx,g_mk_response);
    g_page_fetch->mk_error=JS_DupValue(ctx,g_mk_error);
    g_blob_fn          = JS_GetPropertyStr(ctx, hooks, "blobSnapshot");
    scopy(g_blob_origin,g_loc_valid?origin:"null",sizeof g_blob_origin);
    {
        char generation[24];
        snprintf(generation,sizeof generation,"%llu",++g_blob_generation);
        JSValue configure=JS_GetPropertyStr(ctx,hooks,"blobConfigure");
        JSValue config[2]={JS_NewString(ctx,g_blob_origin),JS_NewString(ctx,generation)};
        JSValue r=JS_Call(ctx,configure,JS_UNDEFINED,2,(JSValueConst*)config);
        if(JS_IsException(r))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,r);JS_FreeValue(ctx,config[0]);JS_FreeValue(ctx,config[1]);JS_FreeValue(ctx,configure);
    }
    JS_FreeValue(ctx, hooks);
    JS_FreeValue(ctx, g);
}

void js_webapi_close(JSContext *ctx)
{
    if (!g_webapi_ctx || ctx != g_webapi_ctx) return;
    /* A pagehide handler can enqueue a form/location request while the loader
     * is retiring this realm. It must not navigate the replacement document
     * on its first outer turn; the consumer already copied the chosen URL. */
    g_have_pending_nav = 0;g_pending_nav[0] = 0;
    js_webapi_fetch_close(ctx);
    /* The cookie jar and the preflight cache are NOT cleared: both outlive the
     * page for the same reason Storage does. A session that evaporated on
     * every navigation would not be a session. */
    hist_reset(ctx, "");
    if (ctx) {
        JS_FreeValue(ctx, g_popstate_state);
        JS_FreeValue(ctx, g_mk_response);
        JS_FreeValue(ctx, g_viewport_changed);
        JS_FreeValue(ctx, g_fire_fn);
        JS_FreeValue(ctx, g_mk_error);
        JS_FreeValue(ctx, g_blob_fn);
    }
    g_popstate_state = JS_NULL;
    g_mk_response = g_viewport_changed = g_fire_fn = g_mk_error = JS_UNDEFINED;
    g_blob_fn=JS_UNDEFINED;g_blob_origin[0]=0;
    g_popstate_queued = g_hashchange_queued = 0;
    g_hist_n = 0; g_hist_i = 0;
    g_webapi_ctx = 0;
    g_vp_dirty = 0;
    set_location("about:blank");
}

int js_webapi_pending(void)
{ return js_webapi_fetch_pending(g_webapi_ctx) || g_popstate_queued || g_hashchange_queued || g_vp_dirty; }

int js_webapi_pump(JSContext *ctx)
{
    if (!g_webapi_ctx || ctx != g_webapi_ctx) return 0;
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

    ran += js_webapi_fetch_pump(ctx);
    return ran;
}

/* These fields are a selected working image, not a second reference owner.
 * Inactive images own their JSValues; active values reside in the globals.
 * Request slots MUST NOT move: native HTTP callbacks retain their addresses.
 * The shared realm registry owns stable heap records and filters work by ctx.
 * Cookie/storage/preflight services and unique generation counters are also
 * intentionally shared, unlike document history and its private JS hooks. */
#define WEBAPI_CONTEXT_FIELDS(X) \
    X(g_loc) X(g_webapi_ctx) X(g_loc_valid) X(g_loc_raw) \
    X(g_pending_nav) X(g_have_pending_nav) X(g_embedded_site) X(g_site_url) \
    X(g_page_fetch) X(g_storage_session) X(g_hist) X(g_hist_n) X(g_hist_i) \
    X(g_popstate_queued) X(g_hashchange_queued) X(g_popstate_state) \
    X(g_hash_old) X(g_hash_new) X(g_mk_response) X(g_viewport_changed) \
    X(g_mk_error) X(g_vw) X(g_vh) X(g_vp_dirty) \
    X(g_fire_fn) X(g_blob_fn) X(g_blob_origin)

struct js_webapi_context {
#define WEBAPI_FIELD(n) __typeof__(n) n;
    WEBAPI_CONTEXT_FIELDS(WEBAPI_FIELD)
#undef WEBAPI_FIELD
};
static struct js_webapi_context webapi_default_context;
static struct js_webapi_context *webapi_active_context=&webapi_default_context;

struct js_webapi_context *js_webapi_context_create(const char *site_url)
{
    /* A truncated authority must not become a different valid site. */
    if (site_url && strlen(site_url)>=WURL_MAX) return NULL;
    struct js_webapi_context *s=calloc(1,sizeof *s);
    if (!s) return NULL;
    s->g_embedded_site=1;
    scopy(s->g_site_url,site_url ? site_url : "",WURL_MAX);
    s->g_storage_session=g_storage_session;
    s->g_vw=1180; s->g_vh=572;
    s->g_popstate_state=JS_NULL;
    s->g_mk_response=s->g_viewport_changed=s->g_mk_error=JS_UNDEFINED;
    s->g_fire_fn=s->g_blob_fn=JS_UNDEFINED;
    scopy(s->g_loc_raw,"about:blank",WURL_MAX);
    return s;
}

void js_webapi_context_activate(struct js_webapi_context *next)
{
    if (!next) next=&webapi_default_context;
    if (next==webapi_active_context) return;
#define WEBAPI_SAVE(n) memcpy(&webapi_active_context->n,&n,sizeof n);
    WEBAPI_CONTEXT_FIELDS(WEBAPI_SAVE)
#undef WEBAPI_SAVE
#define WEBAPI_LOAD(n) memcpy(&n,&next->n,sizeof n);
    WEBAPI_CONTEXT_FIELDS(WEBAPI_LOAD)
#undef WEBAPI_LOAD
    webapi_active_context=next;
}

int js_webapi_context_destroy(struct js_webapi_context *s)
{
    if (!s || s==&webapi_default_context) return 0;
    struct js_webapi_context *old=webapi_active_context;
    js_webapi_context_activate(s);
    int busy=g_webapi_ctx || g_page_fetch;
    js_webapi_context_activate(old==s && !busy ? NULL : old);
    if (busy) return 0;
    free(s);
    return 1;
}
