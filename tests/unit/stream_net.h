/* The fake network the streaming and cookie/CORS host tests share.
 *
 * js_webapi.c takes its transport as a vtable, so a "server" is a few hundred
 * lines of C in this process. This one differs from tests/unit/webapi_test.c's
 * in the way that matters for streaming: a response can be RELEASED IN PIECES.
 * fs_route() queues the bytes, fs_more() decides how many of them the client is
 * allowed to see yet, and until the route is finished a recv past that point
 * answers "nothing yet" rather than EOF.
 *
 * That is what turns "it streams" into an assertion. A test can pump the loop,
 * check that the page already holds the first token, verify the response has
 * NOT completed, then release more. A buffered implementation fails that at the
 * first check -- which is the point, and is what tests/unit/stream_test.c's
 * WEBAPI_NO_STREAM build asserts from the other side.
 */

#ifndef LOGIT_TEST_STREAM_NET_H
#define LOGIT_TEST_STREAM_NET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "js_webapi.h"
#include "logit_abi.h"

/* The Rust staticlib (linked for its inflater) calls the kernel allocator and
 * the image registry; on the host they are malloc and a no-op. */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  img_register(void *d) { (void)d; }

/* ---- checks ----------------------------------------------------------- */

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

static void run(const char *src) { JSValue v = eval(src); JS_FreeValue(ctx, v); }

static void ckjs(const char *expr, const char *name)
{
    char buf[8192];
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

/* The string value of a JS expression, into a static buffer. */
static const char *jsstr(const char *expr)
{
    static char out[4096];
    char buf[8192];
    snprintf(buf, sizeof buf, "String(%s)", expr);
    JSValue v = eval(buf);
    const char *s = JS_IsException(v) ? "<exception>" : JS_ToCString(ctx, v);
    snprintf(out, sizeof out, "%s", s ? s : "?");
    if (s && !JS_IsException(v)) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
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

/* ---- the fake sockets -------------------------------------------------- */

#define FS_MAX 8
#define REQ_MAX 8192
#define RSP_MAX 262144

struct fakesock {
    int  used, closed;
    char host[128];
    int  port, tls;
    int  polls, ready_after;
    char req[REQ_MAX];
    int  req_len;
    int  answered;
    char rsp[RSP_MAX];
    int  rsp_len, rsp_off;
    int  avail;                 /* how many response bytes the client may see */
    int  finished;              /* 1 = the rest is EOF, not "not yet" */
    int  slice;                 /* bytes handed back per recv (0 = as many as fit) */
    int  short_write;
};
static struct fakesock fs[FS_MAX];
static unsigned long long fake_now;
static long long fake_unix = 1700000000LL;      /* 2023-11-14T22:13:20Z */
static int fs_opened, fs_closed_count;
static char last_req[REQ_MAX];
static char all_reqs[16][REQ_MAX];
static int  all_req_n;

/* Every request line seen, so a test can assert an OPTIONS preflight went out
 * BEFORE the real request -- or that it did not. */
static const char *nth_req(int i) { return (i >= 0 && i < all_req_n) ? all_reqs[i] : ""; }
static int req_count(void) { return all_req_n; }

static void fs_reset(void)
{
    memset(fs, 0, sizeof fs);
    fs_opened = 0; fs_closed_count = 0;
    last_req[0] = 0;
    all_req_n = 0;
}

static void rsp_add(struct fakesock *s, const char *txt)
{
    int n = (int)strlen(txt);
    if (s->rsp_len + n > RSP_MAX) return;
    memcpy(s->rsp + s->rsp_len, txt, (size_t)n);
    s->rsp_len += n;
}

/* --- the routing table a test installs --------------------------------- */

/* A route answers a request. `s->finished` starts at 1 (the whole response is
 * there); a streaming route sets avail/finished itself. */
typedef void (*fs_router)(struct fakesock *s, const char *method, const char *target);
static fs_router g_router;
static void fs_set_router(fs_router r) { g_router = r; }

static void fs_answer(struct fakesock *s)
{
    char method[16], target[1024];
    method[0] = target[0] = 0;
    sscanf(s->req, "%15s %1023s", method, target);
    int n = s->req_len < REQ_MAX ? s->req_len : REQ_MAX - 1;
    memcpy(last_req, s->req, (size_t)n);
    last_req[n] = 0;
    if (all_req_n < 16) { memcpy(all_reqs[all_req_n], s->req, (size_t)n); all_reqs[all_req_n][n] = 0; all_req_n++; }

    s->avail = 0; s->finished = 1;
    if (g_router) g_router(s, method, target);
    /* A route that set nothing sends everything; one that set avail < 0 wants
     * the client to see NOTHING yet, not even the status line. */
    if (s->avail == 0) s->avail = s->rsp_len;
    else if (s->avail < 0) s->avail = 0;
    s->answered = 1;
}

/* Let the client see `n` more bytes of the queued response. */
static void fs_more(int fd, int n)
{
    if (fd < 0 || fd >= FS_MAX || !fs[fd].used) return;
    fs[fd].avail += n;
    if (fs[fd].avail > fs[fd].rsp_len) fs[fd].avail = fs[fd].rsp_len;
}
/* Append more bytes to a live response and release them. */
static void fs_push(int fd, const char *txt)
{
    if (fd < 0 || fd >= FS_MAX || !fs[fd].used) return;
    rsp_add(&fs[fd], txt);
    fs[fd].avail = fs[fd].rsp_len;
}
static void fs_finish(int fd) { if (fd >= 0 && fd < FS_MAX) fs[fd].finished = 1; }

/* The most recently opened, still-open socket -- which for a single in-flight
 * request is the one under test. */
static int fs_live(void)
{
    for (int i = FS_MAX - 1; i >= 0; i--) if (fs[i].used) return i;
    return -1;
}

/* How many bytes each recv hands back. 7 by default -- deliberately awkward, so
 * nothing ever arrives on a field, header or chunk boundary. Tests that care
 * about a particular split set it. */
static int g_next_slice = 7;

static int f_open(const char *host, int port, int tls)
{
    if (!strcmp(host, "unreachable.example")) return -1;
    for (int i = 0; i < FS_MAX; i++) {
        if (fs[i].used) continue;
        memset(&fs[i], 0, sizeof fs[i]);
        fs[i].used = 1;
        snprintf(fs[i].host, sizeof fs[i].host, "%s", host);
        fs[i].port = port; fs[i].tls = tls;
        fs[i].ready_after = 1;
        fs[i].slice = g_next_slice;
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
    if (s->answered && s->rsp_off < s->avail) bits |= SOCK_P_READABLE;
    if (s->answered && s->finished && s->rsp_off >= s->rsp_len) bits |= SOCK_P_EOF;
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
    if (s->rsp_off >= s->avail) return s->finished && s->rsp_off >= s->rsp_len ? -1 : 0;
    int n = s->avail - s->rsp_off;
    if (n > max) n = max;
    if (s->slice > 0 && n > s->slice) n = s->slice;
    memcpy(buf, s->rsp + s->rsp_off, (size_t)n);
    s->rsp_off += n;
    return n;
}

static void f_close(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return;
    if (fs[fd].used) fs_closed_count++;
    fs[fd].used = 0; fs[fd].closed = 1;
}

static unsigned long long f_now(void) { return fake_now += 5; }
static long long f_unix(void) { return fake_unix; }

static const struct webapi_net FAKE =
    { f_open, f_poll, f_send, f_recv, f_close, f_now, f_unix };

/* ---- context lifecycle ------------------------------------------------ */

static void open_ctx(const char *url)
{
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    js_webapi_set_net(&FAKE);
    /* js_dom.c is not linked here, so there is no document object; give the
     * prelude one so document.cookie has somewhere to live. */
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "document", JS_NewObject(ctx));
    JS_FreeValue(ctx, g);
    js_webapi_install(ctx, url);
}

static void close_ctx(void)
{
    js_webapi_close(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    ctx = 0; rt = 0;
}

/* Does the request on the wire carry this exact line? */
static int req_has(const char *req, const char *needle)
{ return strstr(req, needle) != 0; }

#endif /* LOGIT_TEST_STREAM_NET_H */
