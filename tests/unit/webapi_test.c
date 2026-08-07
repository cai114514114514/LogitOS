/* Host unit tests for c/apps/browser/js_webapi.c -- fetch, XHR, Storage,
 * history, location, URL, URLSearchParams, matchMedia.
 *
 * The point of this file is that NONE of it needs QEMU. js_webapi.c takes its
 * transport as a vtable, so the "server" below is a few hundred lines of C in
 * this process: it accepts a connection after a couple of polls (like a real
 * one), hands the response back a few bytes at a time (like a real one), and
 * answers a route table. That turns "a 302 to a second origin resolves the
 * right body, and the promise settles from the event loop" into an assertion
 * instead of a screenshot.
 *
 * What it deliberately cannot prove is the part that only a real machine can:
 * that the fetch does not freeze the UI. That is tests/qmp/qmp_fetch_ui.py.
 *
 *     make test-webapi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "js_webapi.h"
#include "logit_abi.h"

/* The Rust staticlib (linked for its inflater, which http1.c uses to undo
 * gzip) calls the kernel allocator and the image registry. On the host they
 * are malloc and a no-op -- same self-stubbing as the other pipeline tests. */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  img_register(void *d) { (void)d; }

/* ---- the harness ------------------------------------------------------ */

static JSRuntime *rt;
static JSContext *ctx;
static int checks, failures;

static void ck(int cond, const char *name)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", name); }
    else printf("ok  : %s\n", name);
}

static JSValue eval(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("      [js exception] %s\n         while evaluating: %s\n", m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    return v;
}

/* Evaluate an expression and require it to be truthy. */
static void ckjs(const char *expr, const char *name)
{
    char buf[4096];
    snprintf(buf, sizeof buf, "(%s)", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (!ok && !JS_IsException(v)) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

static void run(const char *src)
{
    JSValue v = eval(src);
    JS_FreeValue(ctx, v);
}

static void drain_jobs(void)
{
    JSContext *c;
    int n;
    do { n = JS_ExecutePendingJob(rt, &c); } while (n > 0);
}

/* One "frame" of the browser's loop: step the sockets, then run whatever
 * promise reactions that produced. */
static void settle(int frames)
{
    for (int i = 0; i < frames; i++) { js_webapi_pump(ctx); drain_jobs(); }
}

/* ---- the fake network -------------------------------------------------
 * Modelled on the real socket ABI's awkward parts on purpose: a connection is
 * not ready on the first poll, a send may be short, and a recv hands back a
 * slice rather than the whole response. A transport that behaved perfectly
 * would not test the state machine at all. */

#define FS_MAX 8
#define REQ_MAX 8192
#define RSP_MAX 65536

struct fakesock {
    int  used, closed;
    char host[128];
    int  port, tls;
    int  polls;                 /* poll() calls so far */
    int  ready_after;           /* polls before SOCK_P_CONNECTED */
    char req[REQ_MAX];
    int  req_len;
    int  answered;
    char rsp[RSP_MAX];
    int  rsp_len, rsp_off;
    int  slice;                 /* bytes handed back per recv */
    int  short_write;           /* accept only half of each send */
};
static struct fakesock fs[FS_MAX];
static unsigned long long fake_now;
static int fs_opened;                        /* sockets opened since the last reset */
static char last_req[REQ_MAX];               /* the most recent complete request */

static void fs_reset(void)
{
    memset(fs, 0, sizeof fs);
    fs_opened = 0;
    last_req[0] = 0;
}

static void rsp_add(struct fakesock *s, const char *txt)
{
    int n = (int)strlen(txt);
    if (s->rsp_len + n > RSP_MAX) return;
    memcpy(s->rsp + s->rsp_len, txt, (size_t)n);
    s->rsp_len += n;
}

static void rsp_body(struct fakesock *s, int code, const char *reason,
                     const char *ctype, const char *body)
{
    char hdr[512];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nX-Trace: one\r\nX-Trace: two\r\n"
             "Content-Length: %d\r\n\r\n", code, reason, ctype, (int)strlen(body));
    rsp_add(s, hdr);
    rsp_add(s, body);
}

/* Build the response for whatever request this socket has received. */
static void fs_answer(struct fakesock *s)
{
    char method[16], target[1024];
    method[0] = target[0] = 0;
    sscanf(s->req, "%15s %1023s", method, target);
    memcpy(last_req, s->req, (size_t)(s->req_len < REQ_MAX ? s->req_len : REQ_MAX - 1));
    last_req[s->req_len < REQ_MAX ? s->req_len : REQ_MAX - 1] = 0;

    if (!strcmp(target, "/hello")) {
        rsp_body(s, 200, "OK", "text/plain", "hello world");
    } else if (!strcmp(target, "/json")) {
        rsp_body(s, 200, "OK", "application/json", "{\"a\":1,\"b\":[2,3]}");
    } else if (!strcmp(target, "/notfound")) {
        rsp_body(s, 404, "Not Found", "text/plain", "nope");
    } else if (!strcmp(target, "/redirect")) {
        rsp_add(s, "HTTP/1.1 302 Found\r\nLocation: /hello\r\nContent-Length: 0\r\n\r\n");
    } else if (!strcmp(target, "/redirect-abs")) {
        rsp_add(s, "HTTP/1.1 301 Moved Permanently\r\n"
                   "Location: http://other.example/hello\r\nContent-Length: 0\r\n\r\n");
    } else if (!strcmp(target, "/loop")) {
        rsp_add(s, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n");
    } else if (!strcmp(target, "/chunked")) {
        rsp_add(s, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nchunk\r\n3\r\ned!\r\n0\r\n\r\n");
    } else if (!strcmp(target, "/echo")) {
        /* The body is the request verbatim, so a test can assert on the exact
         * bytes that left the client -- including the ones that should NOT be
         * there (header injection). */
        char hdr[128];
        snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                  "Content-Length: %d\r\n\r\n", s->req_len);
        rsp_add(s, hdr);
        if (s->rsp_len + s->req_len <= RSP_MAX) {
            memcpy(s->rsp + s->rsp_len, s->req, (size_t)s->req_len);
            s->rsp_len += s->req_len;
        }
    } else if (!strcmp(target, "/host")) {
        const char *h = strstr(s->req, "Host: ");
        char v[160]; v[0] = 0;
        if (h) sscanf(h + 6, "%159[^\r\n]", v);
        rsp_body(s, 200, "OK", "text/plain", v);
    } else {
        rsp_body(s, 404, "Not Found", "text/plain", "no route");
    }
    s->answered = 1;
}

static int f_open(const char *host, int port, int tls)
{
    if (!strcmp(host, "unreachable.example")) return -1;
    for (int i = 0; i < FS_MAX; i++) {
        if (fs[i].used) continue;
        memset(&fs[i], 0, sizeof fs[i]);
        fs[i].used = 1;
        snprintf(fs[i].host, sizeof fs[i].host, "%s", host);
        fs[i].port = port; fs[i].tls = tls;
        fs[i].ready_after = 2;          /* two polls of DNS/TCP/TLS, like the real thing */
        fs[i].slice = 7;                /* deliberately awkward: never a whole header */
        fs[i].short_write = 1;
        fs_opened++;
        return i;
    }
    return -1;
}

static int f_poll(int fd)
{
    if (fd < 0 || fd >= FS_MAX || !fs[fd].used) return SOCK_E_ARG;
    struct fakesock *s = &fs[fd];
    s->polls++;
    if (s->polls <= s->ready_after) return 0;
    int bits = SOCK_P_CONNECTED | SOCK_P_WRITABLE;
    if (s->answered && s->rsp_off < s->rsp_len) bits |= SOCK_P_READABLE;
    if (s->answered && s->rsp_off >= s->rsp_len) bits |= SOCK_P_EOF;
    return bits;
}

static int f_send(int fd, const void *buf, int len)
{
    if (fd < 0 || fd >= FS_MAX || !fs[fd].used) return SOCK_E_ARG;
    struct fakesock *s = &fs[fd];
    if (s->short_write && len > 1) len = len / 2 + 1;
    if (s->req_len + len > REQ_MAX) len = REQ_MAX - s->req_len;
    memcpy(s->req + s->req_len, buf, (size_t)len);
    s->req_len += len;
    s->req[s->req_len] = 0;
    /* A request is complete at the blank line plus whatever Content-Length
     * says; the routes that care are the ones that echo the body. */
    if (!s->answered) {
        char *end = strstr(s->req, "\r\n\r\n");
        if (end) {
            int hdr = (int)(end - s->req) + 4, want = 0;
            const char *cl = strstr(s->req, "Content-Length: ");
            if (cl && cl < end) want = atoi(cl + 16);
            if (s->req_len >= hdr + want) fs_answer(s);
        }
    }
    return len;
}

static int f_recv(int fd, void *buf, int max)
{
    if (fd < 0 || fd >= FS_MAX || !fs[fd].used) return SOCK_E_ARG;
    struct fakesock *s = &fs[fd];
    if (s->polls <= s->ready_after || !s->answered) return 0;
    if (s->rsp_off >= s->rsp_len) return -1;              /* EOF, as the kernel reports it */
    int n = s->rsp_len - s->rsp_off;
    if (n > max) n = max;
    if (s->slice > 0 && n > s->slice) n = s->slice;
    memcpy(buf, s->rsp + s->rsp_off, (size_t)n);
    s->rsp_off += n;
    return n;
}

static void f_close(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return;
    fs[fd].used = 0; fs[fd].closed = 1;
}

static unsigned long long f_now(void) { return fake_now += 5; }

static const struct webapi_net FAKE = { f_open, f_poll, f_send, f_recv, f_close, f_now };

/* ---- context lifecycle ------------------------------------------------ */

static void open_ctx(const char *url)
{
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    js_webapi_set_net(&FAKE);
    js_webapi_install(ctx, url);
}

static void close_ctx(void)
{
    js_webapi_close(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ctx = 0; rt = 0;
}

/* ---- 1. URL ----------------------------------------------------------- */

static void test_url(void)
{
    printf("\n-- URL --\n");
    run("var u = new URL('http://a.example:8080/p/q?x=1&y=2#frag');");
    ckjs("u.protocol === 'http:'", "URL.protocol");
    ckjs("u.hostname === 'a.example'", "URL.hostname");
    ckjs("u.port === '8080'", "URL.port");
    ckjs("u.host === 'a.example:8080'", "URL.host carries a non-default port");
    ckjs("u.pathname === '/p/q'", "URL.pathname excludes the query");
    ckjs("u.search === '?x=1&y=2'", "URL.search includes the '?'");
    ckjs("u.hash === '#frag'", "URL.hash includes the '#'");
    ckjs("u.origin === 'http://a.example:8080'", "URL.origin");
    ckjs("u.href === 'http://a.example:8080/p/q?x=1&y=2#frag'", "URL.href round-trips");
    ckjs("String(u) === u.href", "URL.toString");

    run("var d = new URL('https://b.example/x');");
    ckjs("d.port === ''", "a default port reports as the empty string");
    ckjs("d.origin === 'https://b.example'", "https origin omits :443");

    /* Relative resolution, which is the reason this goes through url.c. */
    ckjs("new URL('sub/page', 'http://a.example/dir/index.html').href === "
         "'http://a.example/dir/sub/page'", "relative reference resolves against the base");
    ckjs("new URL('/root', 'http://a.example/dir/index.html').href === "
         "'http://a.example/root'", "root-relative reference replaces the path");
    ckjs("new URL('?q=2', 'http://a.example/dir/index.html?q=1').href === "
         "'http://a.example/dir/index.html?q=2'",
         "a query-only reference keeps the path (url_resolve alone would not)");
    ckjs("new URL('#two', 'http://a.example/d/i.html?q=1#one').href === "
         "'http://a.example/d/i.html?q=1#two'",
         "a fragment-only reference keeps path AND query");
    ckjs("new URL('http://c.example/abs', 'http://a.example/dir/').href === "
         "'http://c.example/abs'", "an absolute reference ignores the base");

    /* Dot segments. A bundler emits './chunk.js' constantly and url.c's
     * url_resolve leaves it in the path, where it becomes a 404. */
    ckjs("new URL('./chunk.js', 'http://a.example/s/app.html').href === "
         "'http://a.example/s/chunk.js'", "'./' is removed");
    ckjs("new URL('../up.js', 'http://a.example/s/deep/app.html').href === "
         "'http://a.example/s/up.js'", "'..' walks up one segment");
    ckjs("new URL('../../../way/up', 'http://a.example/a/b/c.html').href === "
         "'http://a.example/way/up'", "'..' past the root clamps at the root");
    ckjs("new URL('http://a.example/x/./y/../z').pathname === '/x/z'",
         "dot segments in an absolute URL are removed too");
    ckjs("new URL('http://a.example/x/y/..').pathname === '/x/'",
         "a trailing '..' leaves the directory slash");
    ckjs("new URL('http://a.example/x/.').pathname === '/x/'",
         "a trailing '.' leaves the directory slash");

    /* Malformed input must throw, not silently produce something. */
    ckjs("(function(){ try { new URL('garbage'); return false; } catch (e) { return e instanceof TypeError; } })()",
         "a schemeless URL with no base throws TypeError");
    ckjs("(function(){ try { new URL(''); return false; } catch (e) { return true; } })()",
         "the empty string throws");
    ckjs("(function(){ try { new URL('ftp://h/x'); return false; } catch (e) { return true; } })()",
         "an unsupported scheme throws (we speak http and https)");
    ckjs("(function(){ try { new URL('http://'); return false; } catch (e) { return true; } })()",
         "a URL with no host throws");

    /* Setters. */
    run("var s = new URL('http://a.example/one?x=1#h');");
    run("s.pathname = '/two';");
    ckjs("s.href === 'http://a.example/two?x=1#h'", "setting pathname rewrites href");
    run("s.search = 'y=9';");
    ckjs("s.search === '?y=9' && s.href === 'http://a.example/two?y=9#h'",
         "setting search adds the '?' back");
    run("s.hash = 'bottom';");
    ckjs("s.hash === '#bottom'", "setting hash adds the '#'");
    run("s.href = 'https://z.example:9000/n';");
    ckjs("s.hostname === 'z.example' && s.port === '9000' && s.protocol === 'https:'",
         "setting href reparses everything");

    /* A long query is the normal case for an SPA and must not be truncated by
     * url.c's 512-byte path bound -- that is why the query is split off first. */
    run("var long = new URL('http://a.example/p?v=' + 'z'.repeat(700));");
    ckjs("long.search.length === 703", "a 700-byte query survives (url.c's path cap is 512)");
}

/* ---- 2. URLSearchParams ---------------------------------------------- */

static void test_usp(void)
{
    printf("\n-- URLSearchParams --\n");
    run("var p = new URLSearchParams('a=1&b=2&a=3');");
    ckjs("p.get('a') === '1'", "get returns the FIRST value");
    ckjs("JSON.stringify(p.getAll('a')) === '[\"1\",\"3\"]'", "getAll returns every value");
    ckjs("p.get('missing') === null", "a missing key is null, not undefined");
    ckjs("p.has('b') === true && p.has('zz') === false", "has");
    ckjs("p.size === 3", "size counts pairs, not keys");
    run("p.append('c', 'x y');");
    ckjs("p.toString() === 'a=1&b=2&a=3&c=x+y'", "a space serializes as '+'");
    run("p.set('a', '9');");
    ckjs("p.toString() === 'a=9&b=2&c=x+y'", "set replaces the first and drops the rest");
    run("p.delete('b');");
    ckjs("p.toString() === 'a=9&c=x+y'", "delete removes every match");

    ckjs("new URLSearchParams('x=%20%26%3D').get('x') === ' &='",
         "percent-escapes are decoded");
    ckjs("new URLSearchParams([['k','a=b']]).toString() === 'k=a%3Db'",
         "a value containing '=' is re-escaped");
    ckjs("new URLSearchParams({one:1,two:2}).toString() === 'one=1&two=2'",
         "constructed from an object");
    ckjs("new URLSearchParams('?lead=1').get('lead') === '1'",
         "a leading '?' is ignored");
    ckjs("new URLSearchParams('bare').get('bare') === ''", "a key with no '=' has an empty value");
    ckjs("new URLSearchParams('a=%ZZ').get('a') === '%ZZ'",
         "an invalid escape is left alone rather than throwing");
    ckjs("(function(){ var o = []; new URLSearchParams('a=1&b=2').forEach(function (v, k) { o.push(k + ':' + v); }); "
         "return o.join(',') === 'a:1,b:2'; })()", "forEach yields (value, key)");
    ckjs("Array.from(new URLSearchParams('a=1&b=2')).length === 2", "iterable");

    /* The link back to the URL: this is what breaks in every naive
     * implementation, where searchParams is a detached copy. */
    run("var lu = new URL('http://a.example/p?x=1');");
    ckjs("lu.searchParams.get('x') === '1'", "URL.searchParams reads the URL's query");
    run("lu.searchParams.set('x', '2'); lu.searchParams.append('n', '3');");
    ckjs("lu.search === '?x=2&n=3'", "mutating searchParams writes back to URL.search");
    ckjs("lu.href === 'http://a.example/p?x=2&n=3'", "...and to URL.href");
    run("lu.search = '?z=9';");
    ckjs("lu.searchParams.get('z') === '9' && lu.searchParams.get('x') === null",
         "...and the params see a later write to URL.search");
}

/* ---- 3. Storage ------------------------------------------------------- */

static void test_storage(void)
{
    printf("\n-- Storage --\n");
    run("localStorage.clear(); sessionStorage.clear();");
    ckjs("localStorage.length === 0", "a cleared store is empty");
    ckjs("localStorage.getItem('nope') === null", "a missing key is null");
    run("localStorage.setItem('k', 'v');");
    ckjs("localStorage.getItem('k') === 'v'", "setItem/getItem");
    ckjs("localStorage.length === 1", "length");
    ckjs("localStorage.key(0) === 'k'", "key(0)");
    ckjs("localStorage.key(9) === null", "key() past the end is null");
    run("localStorage.setItem('n', 42); localStorage.setItem('u', undefined); localStorage.setItem(7, 'seven');");
    ckjs("localStorage.getItem('n') === '42'", "values are coerced to strings");
    ckjs("localStorage.getItem('u') === 'undefined'", "undefined coerces to \"undefined\"");
    ckjs("localStorage.getItem('7') === 'seven'", "keys are coerced to strings");
    run("localStorage.setItem('k', 'v2');");
    ckjs("localStorage.getItem('k') === 'v2' && localStorage.length === 4",
         "overwriting does not grow the store");
    run("localStorage.removeItem('k');");
    ckjs("localStorage.getItem('k') === null && localStorage.length === 3", "removeItem");
    run("localStorage.removeItem('k');");
    ckjs("localStorage.length === 3", "removing a missing key is a no-op");

    ckjs("sessionStorage.getItem('n') === null",
         "sessionStorage is a DIFFERENT store from localStorage");
    run("sessionStorage.setItem('s', '1');");
    ckjs("localStorage.getItem('s') === null", "...in both directions");

    /* The quota has to be a real bound, not a comment. */
    ckjs("(function(){ try { localStorage.setItem('big', 'x'.repeat(300*1024)); return false; }"
         " catch (e) { return /Quota/.test(String(e)); } })()",
         "exceeding the quota throws QuotaExceededError");
    ckjs("localStorage.getItem('big') === null", "and the oversized value was not stored");
}

/* Storage must outlive the page: that is the only reason it lives in C. */
static void test_storage_survives_navigation(void)
{
    printf("\n-- Storage across a navigation --\n");
    run("localStorage.clear(); localStorage.setItem('token', 'abc123');");
    close_ctx();
    open_ctx("http://page.example/other.html");        /* same origin, new document */
    ckjs("localStorage.getItem('token') === 'abc123'",
         "localStorage survives js_page_close/open (a navigation)");

    close_ctx();
    open_ctx("http://elsewhere.example/x");            /* different origin */
    ckjs("localStorage.getItem('token') === null", "a different origin gets a different store");
    close_ctx();
    open_ctx("http://page.example/dir/index.html");    /* back home */
    ckjs("localStorage.getItem('token') === 'abc123'", "and the first origin still has its own");
}

/* ---- 4. fetch --------------------------------------------------------- */

static void test_fetch(void)
{
    printf("\n-- fetch --\n");
    fs_reset();
    run("var R = null, E = null;"
        "fetch('/hello').then(function (r) { return r.text().then(function (t) { R = { s: r.status, ok: r.ok, "
        "  st: r.statusText, t: t, ct: r.headers.get('content-type'), url: r.url, rd: r.redirected }; }); })"
        "  .catch(function (e) { E = e; });");
    ckjs("R === null", "fetch has not resolved before the loop pumps it");
    ck(js_webapi_pending(), "a fetch in flight makes the runtime 'pending'");
    settle(40);
    ckjs("E === null", "no error");
    ckjs("R && R.s === 200", "status 200");
    ckjs("R && R.ok === true", "ok");
    ckjs("R && R.st === 'OK'", "statusText from the status line");
    ckjs("R && R.t === 'hello world'", "text() gives the body");
    ckjs("R && R.ct === 'text/plain'", "headers.get is case-insensitive");
    ckjs("R && R.url === 'http://page.example/hello'",
         "a root-relative URL resolved against the document");
    ckjs("R && R.rd === false", "not redirected");
    ck(!js_webapi_pending(), "and the runtime is idle again once it settled");

    run("var J = null; fetch('/json').then(function (r) { return r.json(); }).then(function (j) { J = j; });");
    settle(40);
    ckjs("J && J.a === 1 && J.b[1] === 3", "json() parses the body");

    run("var N = null; fetch('/notfound').then(function (r) { N = { s: r.status, ok: r.ok, st: r.statusText }; });");
    settle(40);
    ckjs("N && N.s === 404 && N.ok === false && N.st === 'Not Found'",
         "a 404 RESOLVES with ok=false (fetch only rejects on a network error)");

    run("var C = null; fetch('/chunked').then(function (r) { return r.text(); }).then(function (t) { C = t; });");
    settle(60);
    ckjs("C === 'chunked!'", "a chunked response is de-chunked");

    run("var AB = null; fetch('/hello').then(function (r) { return r.arrayBuffer(); })"
        ".then(function (b) { AB = new Uint8Array(b); });");
    settle(40);
    ckjs("AB && AB.length === 11 && AB[0] === 104", "arrayBuffer() gives the raw bytes");

    run("var MH = null; fetch('/hello').then(function (r) { var o = []; r.headers.forEach(function (v, k) "
        "{ if (k === 'x-trace') o.push(v); }); MH = { j: r.headers.get('x-trace'), n: o.length }; });");
    settle(40);
    ckjs("MH && MH.j === 'one, two' && MH.n === 2",
         "repeated headers are kept apart and joined by get()");

    /* Redirects. */
    run("var RD = null; fetch('/redirect').then(function (r) { return r.text().then(function (t) "
        "{ RD = { t: t, url: r.url, rd: r.redirected, s: r.status }; }); });");
    settle(60);
    ckjs("RD && RD.t === 'hello world' && RD.s === 200", "a 302 is followed");
    ckjs("RD && RD.url === 'http://page.example/hello'", "response.url is the FINAL URL");
    ckjs("RD && RD.rd === true", "response.redirected");

    fs_reset();
    run("var XO = null; fetch('/redirect-abs').then(function (r) { return r.text().then(function (t) "
        "{ XO = { t: t, url: r.url }; }); });");
    settle(60);
    ckjs("XO && XO.t === 'hello world' && XO.url === 'http://other.example/hello'",
         "a redirect to another origin opens a new connection");
    ck(fs_opened == 2, "...and that took exactly two sockets");

    run("var LP = 'pending'; fetch('/loop').then(function () { LP = 'resolved'; })"
        ".catch(function () { LP = 'rejected'; });");
    settle(200);
    ckjs("LP === 'resolved' || LP === 'rejected'", "a redirect loop terminates");

    /* Network failure -> the promise REJECTS (this is the only case that does). */
    run("var F = 'pending'; fetch('http://unreachable.example/x').then(function () { F = 'resolved'; })"
        ".catch(function (e) { F = e.name; });");
    settle(20);
    ckjs("F === 'TypeError'", "an unopenable connection rejects with a TypeError");

    run("var BAD = 'pending'; fetch('::::not a url').catch(function (e) { BAD = e.name; });");
    settle(10);
    ckjs("BAD === 'TypeError'", "an unparseable URL rejects rather than throwing");
}

static void test_fetch_request(void)
{
    printf("\n-- fetch: what actually goes on the wire --\n");
    fs_reset();
    run("var P = null; fetch('/echo', { method: 'post', headers: { 'X-Token': 'abc' },"
        " body: 'name=value' }).then(function (r) { return r.text(); }).then(function (t) { P = t; });");
    settle(60);
    ckjs("P && /^POST \\/echo HTTP\\/1\\.1/.test(P)", "the method is sent, upper-cased");
    /* Case-insensitively: Headers lower-cases names on the way through, which
     * is what the spec says the normalized name is. */
    ckjs("P && /x-token: abc/i.test(P)", "a caller header is sent");
    ckjs("P && /Content-Length: 10/.test(P)", "Content-Length is computed from the body");
    ckjs("P && /\\r\\n\\r\\nname=value$/.test(P)", "the body follows the headers");
    ckjs("P && /Host: page\\.example/.test(P)", "Host comes from the URL, not the caller");

    run("var H = null; fetch('http://a.example:8080/host').then(function (r) { return r.text(); })"
        ".then(function (t) { H = t; });");
    settle(60);
    ckjs("H === 'a.example:8080'", "a non-default port is included in Host");

    /* Header injection: the value carries CRLF and must not become two headers. */
    run("var I = null; fetch('/echo', { headers: { 'X-A': 'good\\r\\nX-Evil: yes' } })"
        ".then(function (r) { return r.text(); }).then(function (t) { I = t; })"
        ".catch(function () { I = 'rejected'; });");
    settle(60);
    ckjs("I === 'rejected' || !/X-Evil/.test(I)",
         "a CRLF in a header value cannot inject a second header");

    /* A body given as a typed array. */
    run("var B = null; fetch('/echo', { method: 'PUT', body: new Uint8Array([65,66,67]) })"
        ".then(function (r) { return r.text(); }).then(function (t) { B = t; });");
    settle(60);
    ckjs("B && /\\r\\n\\r\\nABC$/.test(B)", "a Uint8Array body is sent as bytes");

    /* Concurrency: four requests in flight at once, all resolving. */
    fs_reset();
    run("var got = []; ['/hello','/json','/chunked','/hello'].forEach(function (u) {"
        "  fetch(u).then(function (r) { return r.text(); }).then(function (t) { got.push(t.length); }); });");
    ck(js_webapi_pending(), "four fetches are in flight");
    settle(80);
    ckjs("got.length === 4", "four concurrent requests all resolved");
    ck(fs_opened == 4, "...over four sockets");
}

/* ---- 5. XMLHttpRequest ------------------------------------------------ */

static void test_xhr(void)
{
    printf("\n-- XMLHttpRequest --\n");
    fs_reset();
    run("var states = [], X = null;"
        "var x = new XMLHttpRequest();"
        "x.onreadystatechange = function () { states.push(x.readyState); };"
        "x.addEventListener('load', function () { X = { s: x.status, t: x.responseText,"
        "  ct: x.getResponseHeader('Content-Type'), all: x.getAllResponseHeaders() }; });"
        "x.open('GET', '/hello'); x.setRequestHeader('X-Zed', '1'); x.send();");
    ckjs("states.length === 1 && states[0] === 1", "open() moves to readyState 1");
    settle(60);
    ckjs("X && X.s === 200", "status");
    ckjs("X && X.t === 'hello world'", "responseText");
    ckjs("X && X.ct === 'text/plain'", "getResponseHeader");
    ckjs("X && /content-length: 11/.test(X.all)", "getAllResponseHeaders");
    ckjs("JSON.stringify(states) === '[1,2,3,4]'", "readyState went 1,2,3,4");
    ck(strstr(last_req, "x-zed: 1") != 0, "setRequestHeader reached the wire");

    run("var XE = null; var y = new XMLHttpRequest();"
        "y.onerror = function () { XE = 'error'; };"
        "y.open('GET', 'http://unreachable.example/x'); y.send();");
    settle(30);
    ckjs("XE === 'error'", "a failed XHR fires onerror");

    run("var XJ = null; var z = new XMLHttpRequest(); z.responseType = 'json';"
        "z.onload = function () { XJ = z.response; }; z.open('GET', '/json'); z.send();");
    settle(60);
    ckjs("XJ && XJ.a === 1", "responseType='json' parses the body");
}

/* ---- 6. location + history ------------------------------------------- */

static void test_location(void)
{
    printf("\n-- location --\n");
    ckjs("location.href === 'http://page.example/dir/index.html'", "location.href");
    ckjs("location.protocol === 'http:'", "location.protocol");
    ckjs("location.host === 'page.example'", "location.host");
    ckjs("location.hostname === 'page.example'", "location.hostname");
    ckjs("location.port === ''", "location.port is empty for a default port");
    ckjs("location.pathname === '/dir/index.html'", "location.pathname");
    ckjs("location.search === ''", "location.search");
    ckjs("location.hash === ''", "location.hash");
    ckjs("location.origin === 'http://page.example'", "location.origin");
    ckjs("String(location) === location.href", "location.toString");
    /* There is no DOM in this binary; in the browser js_dom.c has installed
     * `document` before js_webapi_install runs and it gets .location too. */
    ckjs("typeof document === 'undefined' || document.location === location",
         "document.location is the same object as window.location");

    /* A fragment change is same-document and must be applied in place. */
    run("location.hash = 'sec2';");
    ckjs("location.hash === '#sec2'", "assigning location.hash applies immediately");
    ckjs("location.href === 'http://page.example/dir/index.html#sec2'",
         "...and shows up in href");
    {
        char nav[2048];
        ck(js_webapi_take_navigation(nav, sizeof nav) == 0,
           "a fragment change does NOT request a navigation");
    }
    /* A real navigation is recorded for the embedder rather than performed. */
    run("location.href = 'http://page.example/next.html';");
    {
        char nav[2048];
        int got = js_webapi_take_navigation(nav, sizeof nav);
        ck(got && strcmp(nav, "http://page.example/next.html") == 0,
           "assigning location.href records a pending navigation");
        ck(js_webapi_take_navigation(nav, sizeof nav) == 0, "...which is only reported once");
    }
    /* A fragment change fires hashchange, from the pump like popstate. */
    run("var hc = []; onhashchange = function (e) { hc.push([e.oldURL, e.newURL]); };"
        "location.hash = 'third';");
    ckjs("hc.length === 0", "hashchange is not fired synchronously");
    settle(1);
    ckjs("hc.length === 1 && /#sec2$/.test(hc[0][0]) && /#third$/.test(hc[0][1])",
         "the pump fired hashchange with oldURL and newURL");
    run("location.hash = 'third';");
    settle(1);
    ckjs("hc.length === 1", "assigning the SAME hash fires nothing");

    run("location.hash = '';");
}

static void test_history(void)
{
    printf("\n-- history --\n");
    ckjs("history.length === 1", "a fresh document has one history entry");
    ckjs("history.state === null", "and no state");

    run("history.pushState({ page: 1 }, '', '/app/one');");
    ckjs("location.pathname === '/app/one'", "pushState changes the URL without navigating");
    ckjs("history.length === 2", "...and grows the history");
    ckjs("history.state.page === 1", "history.state");
    {
        char nav[2048];
        ck(js_webapi_take_navigation(nav, sizeof nav) == 0,
           "pushState does NOT request a navigation (this is the SPA routing case)");
    }

    run("history.pushState({ page: 2 }, '', '/app/two?x=1');");
    ckjs("location.pathname === '/app/two' && location.search === '?x=1'",
         "a pushed URL with a query");
    ckjs("history.length === 3", "three entries");

    run("history.replaceState({ page: 22 }, '', '/app/two-b');");
    ckjs("history.length === 3 && history.state.page === 22 && location.pathname === '/app/two-b'",
         "replaceState overwrites instead of appending");

    /* popstate: queued, then delivered by the pump -- never inline. */
    run("var pops = []; onpopstate = function (e) { pops.push(e.state); };"
        "history.back();");
    ckjs("pops.length === 0", "back() does not fire popstate synchronously");
    ckjs("location.pathname === '/app/one'", "...but the URL is already back");
    settle(1);
    ckjs("pops.length === 1 && pops[0].page === 1", "the pump delivered popstate with its state");

    run("history.forward();");
    settle(1);
    ckjs("pops.length === 2 && pops[1].page === 22 && location.pathname === '/app/two-b'",
         "forward() walks the other way");

    run("history.go(-99);");
    settle(1);
    ckjs("pops.length === 2", "a go() past the start of the history does nothing");

    ckjs("(function(){ try { history.pushState({}, '', 'http://evil.example/x'); return false; }"
         " catch (e) { return true; } })()",
         "cross-origin pushState throws (a page must not fake the address bar)");
    ckjs("location.host === 'page.example'", "...and the location did not change");
}

/* ---- 7. matchMedia ---------------------------------------------------- */

static void test_media(void)
{
    printf("\n-- matchMedia --\n");
    js_webapi_set_viewport(1180, 572);
    ckjs("matchMedia('(min-width: 800px)').matches === true", "min-width below the viewport");
    ckjs("matchMedia('(min-width: 2000px)').matches === false", "min-width above it");
    ckjs("matchMedia('(max-width: 600px)').matches === false", "max-width below it");
    ckjs("matchMedia('(max-width: 1400px)').matches === true", "max-width above it");
    ckjs("matchMedia('(min-height: 500px)').matches === true", "min-height");
    ckjs("matchMedia('(orientation: landscape)').matches === true", "orientation");
    ckjs("matchMedia('(orientation: portrait)').matches === false", "the other orientation");
    ckjs("matchMedia('screen and (min-width: 800px)').matches === true", "media type + feature");
    ckjs("matchMedia('print').matches === false", "print does not match a screen");
    ckjs("matchMedia('(min-width: 2000px), (max-width: 1400px)').matches === true",
         "a comma is OR");
    ckjs("matchMedia('not all and (min-width: 2000px)').matches === true", "'not' inverts");
    ckjs("matchMedia('(prefers-color-scheme: light)').matches === true",
         "the browser paints light");
    ckjs("matchMedia('(prefers-color-scheme: dark)').matches === false", "...and not dark");
    ckjs("matchMedia('(min-resolution: 2dppx)').matches === false",
         "an unsupported feature reports false rather than guessing");
    ckjs("matchMedia('(min-width: 800px)').media === '(min-width: 800px)'", "media echoes the query");

    /* addListener is real: it fires when the viewport actually changes. */
    run("var mq = matchMedia('(min-width: 1000px)'); var fired = [];"
        "mq.addListener(function (e) { fired.push(e.matches); });"
        "mq.addEventListener('change', function (e) { fired.push('ev' + e.matches); });");
    ckjs("mq.matches === true", "matches before the change");
    js_webapi_set_viewport(600, 400);
    ck(js_webapi_pending(), "a viewport change is pending work");
    settle(1);
    ckjs("mq.matches === false", "the MediaQueryList updated");
    ckjs("JSON.stringify(fired) === '[false,\"evfalse\"]'",
         "both addListener and addEventListener('change') fired");
    js_webapi_set_viewport(1180, 572);
    settle(1);
    ckjs("mq.matches === true && fired.length === 4", "and again on the way back");
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    fake_now = 1000;
    fs_reset();
    open_ctx("http://page.example/dir/index.html");

    test_url();
    test_usp();
    test_storage();
    test_fetch();
    test_fetch_request();
    test_xhr();
    test_location();
    test_history();
    test_media();
    test_storage_survives_navigation();

    close_ctx();

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures) { printf("webapi_test: FAIL\n"); return 1; }
    printf("webapi_test: ALL PASS\n");
    return 0;
}
