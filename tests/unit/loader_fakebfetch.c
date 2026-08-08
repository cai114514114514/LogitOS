/* A bfetch that serves an in-memory site, for tests/unit/loader_test.c.
 *
 * c/apps/browser/browser_rt.c is the real one: sockets, TLS, an HTTP/1.1 state
 * machine and a connection pool. None of that is under test here -- what is
 * under test is what the LOADER does with the documents it gets back. So this
 * implements bfetch.h against a table of {url, body} and records every URL
 * asked for, which is what lets a test assert "the destination was fetched"
 * and "the sub-resources were discovered" as separate claims.
 *
 * Everything settles synchronously: bfetch_start() completes the request
 * immediately, so bfetch_wait() and bfetch_pump() have nothing to do. Transfer
 * concurrency belongs to browser_rt.c's own tests (tests/unit/http1_test.c,
 * hpool_test.c); reproducing it here would test the fake.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bfetch.h"
#include "loader_fakebfetch.h"

/* The image-decoder registration callbacks, WEAK.
 *
 * The Rust staticlib is one codegen unit, so a test that wants only the
 * inflater (http1.c's Content-Encoding) still drags png_register, bmp_register
 * and ico_register into the link, and therefore needs whatever they call. No
 * fixture here is an image, so recording a decoder is a no-op.
 *
 * WEAK, and that is the whole point of this comment. Which of these
 * tests/unit/rust_host_shim.c supplies has changed three times in one
 * afternoon, in both directions: with the shim silent this link failed on
 * `undefined reference to img_register`, and with the shim supplying it the
 * same link failed on `multiple definition of img_register`. Both failures are
 * in a file this test does not own and cannot pin. A weak definition is right
 * whichever way that lands -- the shim's strong one wins when it exists, this
 * one fills in when it does not -- so the loader test stops being collateral in
 * someone else's ABI.
 *
 * Signatures match c/lib/image/img.h so a real prototype in scope would agree,
 * even though this TU deliberately does not include it (img.h grows codec kinds
 * and a stub written against the header of the day stops compiling). */
__attribute__((weak)) void img_register(void *detect, void *decode);
__attribute__((weak)) void img_register(void *detect, void *decode)
{ (void)detect; (void)decode; }
__attribute__((weak)) void img_register_anim(void *detect, void *decode, void *anim);
__attribute__((weak)) void img_register_anim(void *detect, void *decode, void *anim)
{ (void)detect; (void)decode; (void)anim; }

#define NREQ  64
#define NROUTE 32

struct route { const char *url; const char *body; int status; };
static struct route g_routes[NROUTE];
static int g_nroutes;

/* Every URL bfetch_start was asked for, in order, including sub-resources. */
static char g_log[256][512];
static int  g_nlog;

struct req {
    int used, state, status;
    char url[512];
    unsigned char *body;
    int len;
};
static struct req g_req[NREQ];
static char g_base[512];
static int  g_dials, g_reuses, g_requests;
static void (*g_tick)(void);

/* ---- the fixture site ---- */
void fake_site_reset(void)
{
    g_nroutes = 0; g_nlog = 0;
    for (int i = 0; i < NREQ; i++) { free(g_req[i].body); }
    memset(g_req, 0, sizeof g_req);
    g_dials = g_reuses = g_requests = 0;
}
void fake_site_add(const char *url, const char *body)
{
    if (g_nroutes < NROUTE) {
        g_routes[g_nroutes].url = url;
        g_routes[g_nroutes].body = body;
        g_routes[g_nroutes].status = 200;
        g_nroutes++;
    }
}
void fake_site_clear_log(void) { g_nlog = 0; g_dials = g_reuses = g_requests = 0; }
int fake_site_dials(void) { return g_dials; }
int fake_site_requests(void) { return g_nlog; }
const char *fake_site_request(int i) { return (i >= 0 && i < g_nlog) ? g_log[i] : ""; }
int fake_site_fetched(const char *frag)
{
    int n = 0;
    for (int i = 0; i < g_nlog; i++) if (strstr(g_log[i], frag)) n++;
    return n;
}

/* ---- URL resolution: enough of RFC 3986 for the fixtures ---- */
static void resolve(const char *base, const char *ref, char *out, int max)
{
    if (strstr(ref, "://") ) { snprintf(out, max, "%s", ref); return; }
    if (ref[0] == '/') {
        /* scheme://host + ref */
        const char *p = strstr(base, "://");
        const char *slash = p ? strchr(p + 3, '/') : 0;
        int hostlen = slash ? (int)(slash - base) : (int)strlen(base);
        snprintf(out, max, "%.*s%s", hostlen, base, ref);
        return;
    }
    /* relative: strip the last path segment of base */
    const char *p = strstr(base, "://");
    const char *last = strrchr(p ? p + 3 : base, '/');
    int keep = last ? (int)(last - base) + 1 : (int)strlen(base);
    snprintf(out, max, "%.*s%s", keep, base, ref);
}

/* ---- bfetch.h ---- */
void bfetch_init(void) { }
void bfetch_set_base(const char *page_url)
{ snprintf(g_base, sizeof g_base, "%s", page_url ? page_url : ""); }

int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ resolve(base && base[0] ? base : g_base, ref, out, max); return 0; }

int bfetch_start_from(const char *base, const char *ref)
{
    int id = -1;
    for (int i = 0; i < NREQ; i++) if (!g_req[i].used) { id = i; break; }
    if (id < 0) return -1;
    struct req *r = &g_req[id];
    memset(r, 0, sizeof *r);
    r->used = 1;
    resolve(base && base[0] ? base : g_base, ref, r->url, sizeof r->url);
    if (g_nlog < 256) snprintf(g_log[g_nlog++], 512, "%s", r->url);
    g_requests++; g_dials++;
    for (int i = 0; i < g_nroutes; i++) {
        if (!strcmp(g_routes[i].url, r->url)) {
            r->len = (int)strlen(g_routes[i].body);
            r->body = malloc((size_t)r->len + 1);
            memcpy(r->body, g_routes[i].body, (size_t)r->len + 1);
            r->status = g_routes[i].status;
            r->state = BF_DONE;
            return id;
        }
    }
    r->status = 404; r->state = BF_FAILED;
    return id;
}
int bfetch_start(const char *ref) { return bfetch_start_from(0, ref); }

int bfetch_state(int id)  { return (id >= 0 && g_req[id].used) ? g_req[id].state : BF_FAILED; }
int bfetch_status(int id) { return (id >= 0 && g_req[id].used) ? g_req[id].status : 0; }
const char *bfetch_url(int id) { return (id >= 0 && g_req[id].used) ? g_req[id].url : ""; }
const char *bfetch_error(int id) { (void)id; return "not found"; }

const unsigned char *bfetch_body(int id, int *len)
{
    if (id < 0 || !g_req[id].used) { if (len) *len = 0; return 0; }
    if (len) *len = g_req[id].len;
    return g_req[id].body;
}
int bfetch_take(int id, unsigned char **out)
{
    if (id < 0 || !g_req[id].used || !g_req[id].body) { bfetch_release(id); return -1; }
    *out = g_req[id].body;
    int n = g_req[id].len;
    g_req[id].body = 0;
    memset(&g_req[id], 0, sizeof g_req[id]);
    return n;
}
void bfetch_release(int id)
{
    if (id < 0 || id >= NREQ) return;
    free(g_req[id].body);
    memset(&g_req[id], 0, sizeof g_req[id]);
}

int  bfetch_pump(void) { return 0; }                      /* everything is already settled */
void bfetch_wait(int id, void (*tick)(void)) { (void)id; if (tick) tick(); }
int  bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{
    int id = bfetch_start(ref);
    if (id < 0 || bfetch_state(id) != BF_DONE) { bfetch_release(id); return -1; }
    int n = bfetch_take(id, out);
    if (n < 0) return -1;
    *outlen = n;
    return 0;
}

void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
void bfetch_cache_clear(void) { }
/* The real one seeds the prefetch cache so layout's res_fetch() finds an image
 * without a connection. This test stubs res_fetch to fail outright (no fixture
 * is an image), so there is nothing for a cache to serve -- and a fake cache
 * here would be a fake serving a fake. What the test DOES assert about
 * re-hydration is the thing that matters and is real: the request LOG, which
 * this returning without doing anything cannot help. */
int bfetch_cache_put(const char *abs, const unsigned char *d, int len)
{ (void)abs; (void)d; (void)len; return -1; }
void bfetch_set_tick(void (*fn)(void)) { g_tick = fn; }
void bfetch_stats(int *d, int *r, int *q) { if (d) *d = g_dials; if (r) *r = g_reuses; if (q) *q = g_requests; }
void bfetch_pool_stats(int *h, int *e, int *c) { if (h) *h = 0; if (e) *e = 0; if (c) *c = 0; }
void bfetch_reset_stats(void) { g_dials = g_reuses = g_requests = 0; }
void bfetch_close_all(void) { }
