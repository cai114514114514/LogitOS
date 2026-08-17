/* Host unit test for the cookie surface a page actually touches, driven by the
 * one page whose stack named it.
 *
 *     make test-logreporter          the tests
 *     make test-logreporter-negctl   the same file against js_webapi.c built
 *                                    with -DWEBAPI_NO_COOKIE_ENABLED: the
 *                                    navigator.cookieEnabled checks must FAIL
 *
 * WHAT THIS COVERS THAT test-cookie-cors DOES NOT. That file drives
 * document.cookie against a SYNTHETIC document -- tests/unit/stream_net.h:296
 * says so in its own words: "js_dom.c is not linked here, so there is no
 * document object; give the prelude one so document.cookie has somewhere to
 * live." It therefore cannot see anything about the two objects the browser
 * really publishes or about the ORDER they are published in. This file links
 * js_page.c + js_dom.c + js_webapi.c and calls js_page_open(), which is the
 * same call the browser makes, so:
 *
 *   - `document` is js_dom.c's document, not a bare JS_NewObject;
 *   - `navigator` is js_page.c's navigator, which sets cookieEnabled to FALSE
 *     at js_page.c:610 and is corrected by js_webapi_install twenty lines
 *     later at js_page.c:631. Whether that correction happens, and whether it
 *     survives js_platform.c's only-if-absent gap filling that runs after it,
 *     is a fact about the install order and about nothing else.
 *
 * THE PAGE. tests/fixtures/webapi/logreporter/ is a reduction of bilibili's
 * log-reporter.js -- getCurrentDomain and setCookie, the two frames in the
 * 2026-08-16 scoreboard's stack -- with the read half and the
 * navigator.cookieEnabled gate the same file's other real callers put around
 * it. See that directory's SOURCE. The fixture is unguarded exactly where the
 * original is: a browser that answers `undefined` from document.domain, or
 * from document.cookie, throws inside it rather than returning a wrong answer.
 *
 * THE THREE RULES CHECKED BY WATCHING THEM REFUSE, each paired with the
 * permitted case so a browser with no jar at all could not pass:
 *   1. an HttpOnly cookie set by the NETWORK is invisible to script, and is
 *      still sent on the next request;
 *   2. a page cannot set a cookie for a domain it does not control, and
 *      cannot clobber an HttpOnly one by omitting the attribute;
 *   3. an invalid write is IGNORED, never thrown -- a page that probes with a
 *      malformed cookie must survive the probe.
 *
 * NO FETCH IS EXERCISED HERE. The network side is reached through
 * webapi_cookie_line/webapi_cookie_store_line, which are the exact two entry
 * points browser_rt.c calls for the top-level navigation and every
 * subresource (declared weak at browser_rt.c:30-33), and they are the only
 * callers in the tree that pass http_api=1. Using them rather than a fake
 * server is not a shortcut: it puts the SCRIPT side and the NETWORK side of
 * the same jar side by side in one assertion, which is what HttpOnly is. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
#include "js_webapi.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
/* WEAK for the same reason webapi_platform_test.c gives: rust_host_shim.c is
 * in this link too and defines both. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }

/* js_webapi.c's two exports to the transport. Declared here rather than in
 * js_webapi.h because that is how browser_rt.c reaches them -- a local
 * declaration, weak there so a build without this TU still links. */
int  webapi_cookie_line(const char *host, const char *path, int secure,
                        int nav, char *out, int cap);
void webapi_cookie_store_line(const char *host, const char *path, int secure,
                              const char *setcookie);

/* ---- harness ---------------------------------------------------------- */
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
        printf("      [js exception] %s\n         while evaluating: %s\n",
               m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    return v;
}

static void ckjs(const char *expr, const char *name)
{
    char buf[4096];
    snprintf(buf, sizeof buf,
             "(function(){ try { return (%s); } catch (e) { return 'threw: ' + e; } })()", expr);
    JSValue v = eval(buf);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v) && !JS_IsString(v);
    if (!ok && !JS_IsException(v)) {
        const char *s = JS_ToCString(ctx, v);
        printf("      value was: %s\n", s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    ck(ok, name);
}

/* Run a statement and report whether it completed. A cookie write that a page
 * expects to be ignored must come back 1, not an exception. */
static int run_ok(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v);
    if (!ok) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("      [js exception] %s\n         while evaluating: %s\n",
               m ? m : "?", src);
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    js_page_pump();
    return ok;
}

/* ---- the transport, present only for its clock -------------------------
 * The jar needs a wall clock to decide whether an Expires= date has passed;
 * with none it behaves as though the epoch never advanced, which would make
 * the fixture's `expires=<one year out>` meaningless rather than wrong. The
 * socket half is left NULL on purpose -- nothing here fetches, and a stub
 * that answered would be a second transport nobody reads. */
static long long t_unix(void) { return (long long)time(NULL); }
static unsigned long long t_ms(void) { return 0; }
static const struct webapi_net CLOCK_ONLY = { 0, 0, 0, 0, 0, t_ms, t_unix };

/* ---- fixture loading -------------------------------------------------- */
static char *slurp(const char *dir, const char *name, int *len_out)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        printf("FAIL: cannot read %s\n", path); fclose(f); free(b); return NULL;
    }
    b[n] = 0;
    fclose(f);
    if (len_out) *len_out = (int)n;
    return b;
}

static struct node *open_page(const char *html, int html_len, const char *url)
{
    struct node *root = dom_parse(html, html_len);
    if (!root) { printf("FAIL: fixture did not parse\n"); return NULL; }
    js_page_set_location(url);
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return NULL; }
    ctx = js_page_ctx();
    return root;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures/webapi/logreporter";
    int html_len = 0, js_len = 0;
    char *html = slurp(dir, "page.html", &html_len);
    char *js   = slurp(dir, "log-reporter.js", &js_len);
    if (!html || !js) return 1;

    js_webapi_set_net(&CLOCK_ONLY);

    /* ==== 1. the page, as the browser opens it ==========================
     * Seeded BEFORE the page runs, through the network entry point, because
     * that is where an HttpOnly cookie comes from -- script cannot create one
     * (cookies.c:649) and a test that made one from script would be testing
     * the refusal, not the concealment. */
    webapi_cookie_store_line("www.bilibili.com", "/", 0, "sid=abc123; Path=/");
    webapi_cookie_store_line("www.bilibili.com", "/", 0,
                             "SESSDATA=zzz; Path=/; HttpOnly");

    struct node *root = open_page(html, html_len, "http://www.bilibili.com/index.html");
    if (!root) return 1;

    /* The bilibili row itself: "1 JS exception(s)" is the thing that has to
     * become zero, and js_page_eval returns 1 only when nothing escaped. */
    ck(js_page_eval(js, js_len, "log-reporter.js") == 1,
       "log-reporter.js runs to completion -- no uncaught exception");
    ckjs("REPORT.errors.length === 0", "...and caught none internally either");

    /* document.domain, the receiver that was undefined on 2026-08-16. */
    ckjs("typeof document.domain === 'string' && document.domain === 'www.bilibili.com'",
         "document.domain is the host, as a STRING");
    ckjs("REPORT.domain === 'bilibili.com'",
         "getCurrentDomain folded it to the registrable suffix -- the split worked");

    /* ---- navigator.cookieEnabled: the gate around everything above ----
     * js_page.c:610 published this as false. It is the only assertion in this
     * file that -DWEBAPI_NO_COOKIE_ENABLED changes, and the whole reason the
     * negative control exists. */
    ckjs("navigator.cookieEnabled === true",
         "navigator.cookieEnabled is TRUE on a page whose jar works");
    ckjs("REPORT.skipped === false",
         "...so the reporter did NOT take its no-cookies branch");

    /* ---- what the getter shows the page --------------------------------
     * Both halves in one place: the ordinary cookie is there, the HttpOnly one
     * is not, and neither statement means anything without the other. */
    ckjs("REPORT.names.indexOf('sid') >= 0",
         "document.cookie shows script the ordinary cookie the network set");
    ckjs("REPORT.names.indexOf('SESSDATA') < 0",
         "...and NOT the HttpOnly one -- which is the whole point of HttpOnly");
    ckjs("document.cookie.indexOf('SESSDATA') < 0",
         "...on a direct read too, not only through the fixture");

    /* ---- what the setter put in the jar -------------------------------- */
    ckjs("REPORT.wrote === true", "the reporter had no id and wrote one");
    ckjs("REPORT.id === 'B0F1E2D3C4B5A697'",
         "...and read its own write back out of document.cookie");

    /* The network side of the same jar, through the entry point browser_rt.c
     * uses. Two independent facts in one line: the page's write reaches the
     * wire, and the HttpOnly cookie the page cannot see is still sent. */
    {
        char line[1024];
        int n = webapi_cookie_line("www.bilibili.com", "/", 0, 0, line, (int)sizeof line);
        ck(n > 0 && strstr(line, "buvid3=B0F1E2D3C4B5A697") != NULL,
           "the next REQUEST carries the cookie the page wrote");
        ck(n > 0 && strstr(line, "SESSDATA=zzz") != NULL,
           "...and carries the HttpOnly cookie, which the network may see");
        printf("      Cookie: %s\n", n > 0 ? line : "(none)");
    }
    /* domain=bilibili.com, not www.bilibili.com: the fixture set it for the
     * registrable suffix, so a sibling host shares it. */
    {
        char line[1024];
        int n = webapi_cookie_line("m.bilibili.com", "/", 0, 0, line, (int)sizeof line);
        ck(n > 0 && strstr(line, "buvid3=") != NULL,
           "the domain= the split computed really widened it to a sibling host");
        ck(n > 0 && strstr(line, "sid=abc123") == NULL,
           "...while a host-only cookie stays on the host that set it");
    }

    /* ==== 2. the three refusals ========================================= */
    printf("\n-- what a page may NOT do --\n");

    /* A domain the page does not control. cookies.c has the rule (RFC 6265
     * 5.1.3, cookies.c:128) and it is not re-derived here; what is asserted is
     * that document.cookie goes THROUGH it. */
    ck(run_ok("document.cookie = 'evil=1; domain=evil.com; path=/';"),
       "setting a cookie for another domain does not throw");
    ckjs("document.cookie.indexOf('evil=1') < 0",
         "...and does not store it either");
    {
        char line[1024];
        int n = webapi_cookie_line("evil.com", "/", 0, 0, line, (int)sizeof line);
        ck(n <= 0 || strstr(line, "evil=1") == NULL,
           "...and evil.com never sees it on the wire");
    }

    /* A public suffix: `domain=com` would otherwise be a cookie for the whole
     * TLD (cookies.c:236). */
    ck(run_ok("document.cookie = 'tld=1; domain=com; path=/';"),
       "setting a cookie for a public suffix does not throw");
    ckjs("document.cookie.indexOf('tld=1') < 0", "...and does not store it");

    /* The subtle one: clobbering an HttpOnly cookie by leaving the attribute
     * off. Without cookies.c:666-669 the flag check never fires, because the
     * INCOMING cookie is not HttpOnly -- the existing one is. */
    ck(run_ok("document.cookie = 'SESSDATA=hijacked; path=/';"),
       "overwriting an HttpOnly cookie does not throw");
    {
        char line[1024];
        int n = webapi_cookie_line("www.bilibili.com", "/", 0, 0, line, (int)sizeof line);
        ck(n > 0 && strstr(line, "SESSDATA=zzz") != NULL &&
                    strstr(line, "hijacked") == NULL,
           "...and a same-name overwrite does not reach the wire either");
        /* DELIBERATELY NARROW WORDING. This used to read "the session the page
         * cannot read is still the one on the wire", which is a claim about
         * every route to that cookie and is checked along exactly one: the
         * same name, domain and path, which is the only shape cookies.c:666
         * refuses. An adversarial pass found two others and neither is a
         * failure of this line, only of what it said it proved:
         *   - PATH SHADOWING. From /a/b.html a script writes SESSDATA with no
         *     Path=, gets the default /a, misses jar_find's exact strcmp, and
         *     both live. The request then carries "SESSDATA=<script's>;
         *     SESSDATA=<real>" -- longer path first -- and most servers read
         *     the first. RFC 6265 8.6 admits this and Chrome and Firefox have
         *     it too, so it is recorded, not fixed.
         *   - LRU EVICTION, which WAS a defect and is fixed in cookies.c's
         *     evict_lru: see test-cookie-cors for the gate. */
    }

    /* An invalid write is IGNORED, not thrown -- pages probe with these. */
    ck(run_ok("document.cookie = 'nonsensewithnoequalssign';"),
       "a malformed cookie write is ignored, not thrown");
    ck(run_ok("document.cookie = '';"), "an empty cookie write is ignored, not thrown");
    ck(run_ok("document.cookie = '=novalue; path=/';"),
       "a nameless cookie write is ignored, not thrown");
    ckjs("typeof document.cookie === 'string'",
         "...and the getter still answers a string afterwards");

    js_page_close();
    dom_free(root);

    /* ==== 3. no usable origin ==========================================
     * about:blank. c/net/http/url.c:20-22 accepts only http/https, so
     * wurl_parse fails and g_loc_valid is 0: the getter answers "" and the
     * setter is a no-op.
     *
     * WHY "" IS NOT STUBBING TO SUCCESS HERE, and it was closer to it before
     * this change. "" alone is indistinguishable from an empty jar, which is
     * exactly the failure the brief names. It is distinguishable now, because
     * the property that answers the question directly answers it truthfully on
     * the same document: navigator.cookieEnabled is FALSE here and true on the
     * page above. A page therefore has one property that says "cookies do not
     * work on this document" and one that says "and here are none of them",
     * which is a coherent pair; removing document.cookie instead would throw
     * inside every reader that does `document.cookie.split('; ')` -- the exact
     * shape of the fixture, and the exact shape of the bug this all started
     * from. Refusing to answer and crashing the caller is not more honest. */
    printf("\n-- a document with no usable origin --\n");
    {
        struct node *root2 = dom_parse(html, html_len);
        if (!root2) { printf("FAIL: fixture did not re-parse\n"); return 1; }
        js_page_set_location("about:blank");
        if (!js_page_open(root2)) { printf("FAIL: js_page_open (about:blank)\n"); return 1; }
        ctx = js_page_ctx();

        ck(js_page_eval(js, js_len, "log-reporter.js") == 1,
           "log-reporter.js runs to completion on about:blank too");
        ckjs("navigator.cookieEnabled === false",
             "navigator.cookieEnabled is FALSE where cookies cannot work");
        ckjs("REPORT.skipped === true",
             "...and the page takes its own no-cookies branch, by its own choice");
        ckjs("document.cookie === ''", "document.cookie is an empty string, not undefined");
        ck(run_ok("document.cookie = 'x=1; path=/';"),
           "a write on an opaque origin does not throw");
        ckjs("document.cookie === ''", "...and stores nothing");
        js_page_close();
        dom_free(root2);
    }

    free(html);
    free(js);
    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("logreporter_test: ALL PASS\n");
    return failures ? 1 : 0;
}
