/* Host unit tests for the zaiblank package's two general fixes, both found on
 * the z.ai specimen (2026-08-30, the guest replay in the package report):
 *
 *   1. BroadcastChannel (js_platform.c) -- the SPA's entry module rejected at
 *      evaluation on `ReferenceError: 'BroadcastChannel' is not defined`
 *      because the multi-tab "active-tab" lock idiom instantiates the channel
 *      at module init. Spec semantics: same-name exchange in one context,
 *      sender excluded, clone at call time, silent no-op after close.
 *
 *   2. The fetch idle deadline (js_webapi.c) -- the page loop is single and
 *      can be BLOCKED for tens of seconds (js_module.c compiles a module
 *      graph inside one JS_Eval). A fetch whose deadline is charged for that
 *      blocked time dies as "timed out" without its socket ever being
 *      consulted. The fix charges the deadline only for SERVICED time, and
 *      test 2b below is its negative control in BOTH directions:
 *        2a  a blocked loop (60 s gap between pumps) must NOT time out;
 *        2b  a live loop pumping frames with a silent server MUST still time
 *            out after 30 s of serviced time -- the fix must not have
 *            bought liveness by breaking the honest timeout.
 *
 * The file lives under tests/fixtures/zaiblank/ (not tests/unit/) because the
 * package's own-list ends at tests/fixtures/zaiblank/** -- the same placement
 * rule the frameworks package applied to its guest_mount.py driver. Moving it
 * under tests/unit/ is a one-line change for whoever owns that directory.
 *
 * Control build: -DZAIBLANK_BC_ABSENT compiles the BroadcastChannel block out
 * of js_platform.c (the JS_DOCWRITE_NO_INSTALL idiom) and --control inverts
 * every check -- the BroadcastChannel section must then fail, the fetch
 * section must still pass (it is a different feature), and the run says so
 * per section rather than failing opaquely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_webapi.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* WEAK, same reason as webapi_platform_test.c: rust_host_shim.c may supply
 * these too, and a strong duplicate breaks whichever link changes last. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

/* ---- harness: the page world, the same call the browser makes ----------- */
static JSContext *ctx;
static int checks, failures, inverted;

static void ck(int cond, const char *name)
{
    checks++;
    int good = inverted ? !cond : cond;
    if (!good) { failures++; printf("FAIL: %s%s\n", name, inverted ? "  (control: should NOT have worked)" : ""); }
    else printf("ok  : %s\n", name);
}

static JSValue eval(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (!inverted) printf("      [js exception] %s\n         while evaluating: %.60s\n",
                              m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void ckjs(const char *expr, const char *name)
{
    char buf[8192];
    snprintf(buf, sizeof buf, "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) && !JS_IsString(v);
    if (!ok && !JS_IsException(v) && !inverted) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

static void run(const char *src) { JS_FreeValue(ctx, eval(src)); js_page_pump(); }

/* One clock drives both the page timers and the fetch stack's now_ms: a test
 * that moved one and not the other would measure a fiction. */
static unsigned long long g_clock;
static unsigned long long clock_fn(void) { return g_clock; }

/* Advance SERVICED time: this is what a live page loop does. */
static void tick(int ms)
{
    g_clock += (unsigned long long)ms;
    js_page_run_due();
    js_page_pump();
}

/* Advance BLOCKED time: the clock moves, the loop does not run at all --
 * the shape of js_module.c's synchronous compile, a long layout, or any
 * other owner of the single thread. */
static void blocked(int ms) { g_clock += (unsigned long long)ms; }

/* ---- the in-memory net: a server that connects and then says nothing ------
 * Deliberately the simplest honest peer: TCP up, zero bytes ever. That is
 * exactly the case WF_TIMEOUT exists for, with none of TLS's machinery in
 * the way. */
static int g_sock_alive;
static int net_open(const char *host, int port, int tls)
{
    (void)host; (void)port; (void)tls;
    g_sock_alive = 1;
    return 7;                                  /* one fixed fd */
}
static int net_poll(int fd)
{
    (void)fd;
    return g_sock_alive ? SOCK_P_CONNECTED : 0;
}
static int net_send(int fd, const void *buf, int len) { (void)fd; (void)buf; return len; }
static int net_recv(int fd, void *buf, int max) { (void)fd; (void)buf; (void)max; return 0; }
static void net_close(int fd) { (void)fd; g_sock_alive = 0; }
static unsigned long long net_now(void) { return g_clock; }
static long long net_unix(void) { return 1700000000ll; }
static const struct webapi_net g_net_mock = {
    net_open, net_poll, net_send, net_recv, net_close, net_now, net_unix
};

static const char *PAGE =
"<!doctype html><html><head><title>t</title></head><body id='b'>"
"<div id='wrap'></div>"
"</body></html>";

/* ---- section 1: BroadcastChannel ---------------------------------------- */
static void section_broadcast(void)
{
    printf("-- BroadcastChannel (js_platform.c)\n");
    ckjs("typeof BroadcastChannel === 'function'", "BroadcastChannel exists");
    ckjs("typeof MessageEvent === 'function'", "MessageEvent exists (the delivery event)");

    run("try { new BroadcastChannel(); __bc0 = 'no-throw'; } catch (e) { __bc0 = e.name; }");
    ckjs("__bc0 === 'TypeError'", "constructor with no argument throws TypeError");

    /* Exchange: two same-name channels, sender excluded. The delivery is a
     * task, so the check runs AFTER a tick -- the same discipline as the
     * MessagePort tests, and the thing scheduler-shaped code depends on. */
    run("__a = new BroadcastChannel('t1'); __b = new BroadcastChannel('t1');"
        "__got = []; __b.onmessage = function (e) { __got.push(e.data + '@' + (e.origin !== undefined)); };"
        "__a.onmessage = function (e) { __got.push('self:' + e.data); };");
    run("__a.postMessage('hello')");
    ckjs("__got.length === 0", "delivery is asynchronous (a task, not a call)");
    tick(1);
    ckjs("__got.length === 1 && __got[0] === 'hello@true'",
         "peer receives the message with data and an origin; sender does not receive its own");

    run("__c = new BroadcastChannel('other'); __cgot = [];"
        "__c.onmessage = function (e) { __cgot.push(e.data); };");
    run("__a.postMessage('x2')");
    tick(1);
    ckjs("__cgot.length === 0 && __got.length === 2", "different names do not cross");

    /* Clone at call time: mutate after post, receiver must see the value as
     * of the post. The single-copy alternative is the bug the window.postMessage
     * comment in js_platform.c already documents from a live page. */
    run("__obj = { v: 1 }; __seen = -1;"
        "__b.onmessage = function (e) { __seen = e.data.v; };");
    run("__a.postMessage(__obj); __obj.v = 2;");
    tick(1);
    ckjs("__seen === 1", "payload is cloned at postMessage time, not shared by reference");

    /* addEventListener path (the only other observation form). */
    run("__l = []; __b._l.length = 0;"  /* fresh listeners */
        "__b.onmessage = null;"
        "__b.addEventListener('message', function (e) { __l.push(e.data); });");
    run("__a.postMessage('via-listener')");
    tick(1);
    ckjs("__l.length === 1 && __l[0] === 'via-listener'", "addEventListener('message') receives too");

    /* close(): idempotent, silent afterwards. */
    run("__closed = 'unset';"
        "try { __a.close(); __a.postMessage('after'); __closed = 'no-throw'; }"
        " catch (e) { __closed = 'threw ' + e.name; }");
    tick(1);
    ckjs("__closed === 'no-throw' && __got.length === 2",
         "postMessage after close is a silent no-op (spec: return, not throw)");

    run("__name = (new BroadcastChannel('nm')).name;");
    ckjs("__name === 'nm'", "name is the constructor string");

    /* The lone-channel case, verbatim from the specimen's idiom: a page with
     * ONE channel and no peers posts and nothing may happen -- no event, no
     * error. A broadcast that echoed to the sender would wake the page's own
     * "stand down" branch in a browser it never runs in. */
    run("__solo = new BroadcastChannel('active-tab-channel');"
        "__soloEvents = 0;"
        "__solo.onmessage = function () { __soloEvents++; };"
        "__solo.postMessage('active');");
    tick(1);
    ckjs("__soloEvents === 0", "a lone channel does not receive its own broadcast");
}

/* ---- section 2: the fetch idle deadline ---------------------------------- */
static void section_fetch_deadline(void)
{
    printf("-- fetch idle deadline vs blocked loop time (js_webapi.c)\n");
    /* 2b runs FIRST and is the fix's own negative control: with a live loop,
     * the honest 30 s timeout must still fire. If the fix ever over-forgives,
     * THIS check goes red first, before any liveness claim below. */
    run("__st = null;"
        "fetch('http://dead.test/x').then(function (r) { __st = 'ok ' + r.status; },"
        "                              function (e) { __st = String(e); });");
    for (int i = 0; i < 2100 && (g_clock < 31000ull); i++) tick(16);
    ckjs("__st === 'TypeError: fetch: timed out'",
         "serviced idle time still times out at ~30s (the honest timeout, kept)");

    /* 2a: the same silent server, but the loop is BLOCKED for 60 s -- the
     * measured shape of the z.ai replay (module compile ~50 s, then the
     * first pump failed the fetch that had never been serviced). */
    run("__st2 = null;"
        "fetch('http://dead2.test/x').then(function (r) { __st2 = 'ok ' + r.status; },"
        "                               function (e) { __st2 = String(e); });");
    tick(16);                       /* the loop services the fetch once: dial */
    blocked(60000);                 /* the loop is owned by something else */
    tick(16);                       /* first pump after the block */
    ckjs("__st2 === null",
         "a fetch is NOT failed for loop-blocked time it could not be serviced in");
    /* And it must still be OPERATIONAL, not merely un-failed: with serviced
     * frames from here, it times out 30 s of real service later -- i.e. the
     * deadline moved, exactly once, by the blocked gap. */
    for (int i = 0; i < 2100 && (g_clock < 91000ull + 31000ull); i++) tick(16);
    ckjs("__st2 === 'TypeError: fetch: timed out'",
         "after the block, serviced idle time still expires the deadline (moved by the gap, not removed)");
}

int main(int argc, char **argv)
{
    (void)argc;
    int fetch_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--control")) inverted = 1;
        else if (!strcmp(argv[i], "--fetch-only")) fetch_only = 1;
    }

    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("FAIL: fixture did not parse\n"); return 1; }

    js_webapi_set_net(&g_net_mock);
    js_page_set_clock(clock_fn);
    js_page_set_location("http://fixture.test/dir/page.html?a=1");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    ctx = js_page_ctx();

    if (inverted) {
        /* The control build drops ONLY BroadcastChannel; the fetch section
         * must still pass there, so it is skipped in control mode rather
         * than inverted -- stated per section, the ckjs_dom precedent. */
        section_broadcast();
        printf("-- fetch section skipped in control mode (different feature; "
               "its own negative control is check 2b)\n");
    } else {
        if (!fetch_only) section_broadcast();
        section_fetch_deadline();
    }

    printf("%s: %d checks, %d failure(s)\n",
           inverted ? "zaiblank-control" : "zaiblank", checks, failures);
    return failures ? 1 : 0;
}
