/* SPDX-License-Identifier: MIT
 * tests/unit/frame_window_test.c -- the same-origin frame's Window, driven
 * through the REAL <iframe> path: js_page_open, the page's own
 * document.createElement('iframe'), js_platform.c's contentDocument /
 * contentWindow getters and settle(), and js_frame.c's frame_install_window.
 *
 * tests/frame.mk's specimen calls __frameAdopt by hand with the Window inputs
 * settle() is SUPPOSED to pass. This harness is the other half of that claim
 * (CLAUDE.md rule 4: linking a translation unit is not running it): it never
 * calls __frameAdopt at all, so every "[wire]" check below can only pass if
 * js_platform.c really hands js_frame.c the element, the URL and the
 * navigation door. It also owns the checks that need a creator with a real
 * scheduler, navigator and EventTarget -- the frame's timers, their
 * cancellation and fence, and the shared-service rule.
 *
 * The page script is the measured Cloudflare JS-detections bootstrap reduced
 * to its mechanism -- no hostname, no vendor path, nothing that could fit
 * this engine to one site. Transport is absent; the one navigation that
 * fetches resolves to the stand-in document settle() commits on failure.
 *
 * Controls (tests/frame_window.mk), each required to fail EXACTLY its own
 * tag and nothing else:
 *   [wire]   js_platform.c's settle() calling the old `__frameAdopt(doc)`
 *   [fence]  js_frame.c without the discarded-frame fence on timer callbacks
 *   [cancel] js_frame.c without the release-time timer cancellation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom/tree.h"
#include "js/runtime/page.h"
#include "js/dom/dom.h"
#include "css/style.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return 0; }
void bfetch_prefetch(const char *ref) { (void)ref; }
void bfetch_prefetch_wait(void) { }
int res_fetch(const char *ref, unsigned char **out, int *len)
{ (void)ref; (void)out; (void)len; return -1; }

static unsigned long long g_now = 1000000;
static unsigned long long fake_clock(void) { return g_now; }

static int checks, failed;
static JSContext *ctx;
static void check(const char *src, const char *name)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<frame-window-test>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v) && JS_ToBool(ctx, v);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("  (threw: %s)\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    checks++;
    if (!ok) failed++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}
static void run(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<frame-window-setup>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("setup threw: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    js_page_pump();
}
/* Let the page's own scheduler run: advance the injected clock in steps and
 * drain timers and microtasks, the way the browser's main loop does. */
static void advance(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        g_now += 5;
        js_page_run_due();
        js_page_pump();
    }
}

/* The bootstrap, statement for statement the measured one's shape. */
static const char BOOTSTRAP[] =
"var e = document.createElement('iframe');\n"
"e.height = 1; e.width = 1; e.style.position = 'absolute'; e.style.visibility = 'hidden';\n"
"document.body.appendChild(e);\n"
"var n = e.contentDocument || e.contentWindow.document;\n"
"var t = n.createElement('script');\n"
"t.nonce = '';\n"
"t.innerHTML = \"window['__boot$params']={r:'r0',t:'t0'};\" +\n"
"  \"var _s=document.createElement('script');_s.nonce='';_s.src='/boot-loader.js';\" +\n"
"  \"document.getElementsByTagName('head')[0].appendChild(_s);\" +\n"
"  \"window.parent.bootReport={doc:document,win:window,parent:parent,top:top,fe:frameElement,\" +\n"
"  \"href:location.href,origin:self.origin,ua:navigator.userAgent,\" +\n"
"  \"stNative:Function.prototype.toString.call(setTimeout).indexOf('[native code]')>0};\";\n"
"n.getElementsByTagName('head')[0].appendChild(t);\n";

int main(void)
{
    css_init();
    js_page_set_clock(fake_clock);
    const char html[] = "<!doctype html><html><head></head><body>frame window</body></html>";
    struct node *root = dom_parse(html, sizeof html - 1);
    js_page_set_location("http://frame.example/start.html");
    if (!root || !js_page_open(root)) { puts("FAIL: page initialization"); return 1; }
    ctx = js_page_ctx();

    run(BOOTSTRAP);
    check("typeof bootReport==='object' && bootReport.doc===e.contentDocument",
          "the bootstrap ran in the frame and its `document` is the <iframe>'s contentDocument");
    check("bootReport.parent===window && bootReport.top===window",
          "the frame's parent and top are the page's window");
    check("bootReport.fe===e",
          "[wire] frameElement is the page's own <iframe> element, handed over by settle()");
    check("bootReport.href==='about:blank' && e.contentWindow.location.href==='about:blank' && "
          "e.contentDocument.URL==='about:blank'",
          "location is about:blank from inside AND through contentWindow (which used to answer the PAGE's URL)");
    check("bootReport.origin==='http://frame.example' && e.contentWindow.location.origin==='null'",
          "self.origin inherits the creator's origin; location.origin is the about:blank URL's (\"null\")");
    check("e.contentWindow['__boot$params'].r==='r0' && typeof window['__boot$params']==='undefined'",
          "the page reads what the frame set on its own window, and nothing leaked onto the page's");
    check("n.getElementsByTagName('script').length===2 && n.getElementsByTagName('script')[1].getAttribute('src')==='/boot-loader.js'",
          "the loader the frame built landed in its own document with its src reflected");
    check("bootReport.ua===navigator.userAgent && e.contentWindow.navigator===navigator && bootReport.stNative && "
          "e.contentWindow.setTimeout!==setTimeout && typeof e.contentWindow.XMLHttpRequest==='function'",
          "realm-independent services are the page's; the frame's setTimeout is its own native function");
    check("e.contentWindow.history===undefined && e.contentWindow.getComputedStyle===undefined && "
          "e.contentWindow.innerWidth===undefined",
          "window-specific services that would answer about the PAGE are absent, not shared");

    /* Timers run on the page's scheduler with the frame's Window as `this`;
     * clearInterval inside the frame stops its own interval; a frame cannot
     * cancel the page's timer by guessing its id. */
    run("window.parentFired=false; window.pid=setTimeout(function(){parentFired=true;},20);"
        "var t2=n.createElement('script');"
        "t2.textContent=\"window.tick=0;setTimeout(function(){tick++;window.thisOk=(this===window);},5);"
        "var iv=setInterval(function(){window.ivc=(window.ivc||0)+1;if(window.ivc===3)clearInterval(iv);},1);"
        "clearTimeout(parent.pid);queueMicrotask(function(){window.mt=1;});"
        "addEventListener('ping',function(ev){window.pinged=(ev.target===window);});"
        "dispatchEvent(new Event('ping'));\";"
        "window.pageSawPing=false; addEventListener('ping',function(){pageSawPing=true;});"
        "n.body.appendChild(t2);");
    advance(60);
    check("e.contentWindow.tick===1 && e.contentWindow.thisOk===true",
          "a frame setTimeout fires once, with the frame's Window as `this`");
    check("e.contentWindow.ivc===3",
          "a frame setInterval is stopped by the frame's own clearInterval");
    check("parentFired===true",
          "a frame's clearTimeout cannot cancel the page's timer by its id");
    check("e.contentWindow.mt===1",
          "a frame queueMicrotask runs from the page's own microtask drain");
    check("e.contentWindow.pinged===true && pageSawPing===false",
          "the frame's Window is an EventTarget of its own: target is the frame, the page hears nothing");

    /* Removal: the frame's queued timer leaves the page's queue ([cancel]);
     * and when cancelling is defeated -- here, by a page that replaced
     * clearTimeout -- the discarded frame's callback still never runs ([fence]). */
    run("var fe1=document.createElement('iframe');document.body.appendChild(fe1);"
        "var s1=fe1.contentDocument.createElement('script');"
        "s1.textContent=\"setTimeout(function(){parent.lateFired=true;},100000);\";"
        "fe1.contentDocument.body.appendChild(s1);"
        "fe1.remove();");
    advance(10);   /* drain the zero-delay load-event tasks the insertions queued */
    {
        long long due = js_page_next_due();
        checks++;
        int ok = due < 0;
        if (!ok) failed++;
        printf("%s: [cancel] removing a frame takes its queued timer out of the page's queue (next due %lld)\n",
               ok ? "PASS" : "FAIL", due);
    }
    run("var realClear=clearTimeout; window.clearTimeout=function(){};"
        "var fe2=document.createElement('iframe');document.body.appendChild(fe2);"
        "var s2=fe2.contentDocument.createElement('script');"
        "s2.textContent=\"window.parent.fe2win=window;setTimeout(function(){parent.lateFired2=true;},10);\";"
        "fe2.contentDocument.body.appendChild(s2);"
        "fe2.remove(); window.clearTimeout=realClear;");
    advance(40);
    check("typeof lateFired2==='undefined'",
          "[fence] a discarded frame's timer callback runs nothing, even when its cancellation was defeated");
    check("fe2win.closed===true",
          "a removed frame's Window reports closed");

    /* Navigation from inside the frame goes through the page's one door. */
    run("var fe3=document.createElement('iframe');document.body.appendChild(fe3);"
        "window.doc3=fe3.contentDocument;"
        "var s3=fe3.contentDocument.createElement('script');"
        "s3.textContent=\"try{location.href='http://['}catch(x){parent.navErr=x.name}location.href='/next.html';\";"
        "fe3.contentDocument.body.appendChild(s3);");
    advance(40);
    check("navErr==='SyntaxError'",
          "[wire] an invalid URL assigned to the frame's location throws SyntaxError: the page's door decides, as through contentWindow");
    check("fe3.contentDocument!==doc3 && fe3.contentWindow.location.href==='http://frame.example/next.html' && "
          "fe3.contentWindow.location.pathname==='/next.html' && fe3.contentWindow.location.origin==='http://frame.example'",
          "[wire] location.href= inside the frame navigates it through the page's door (a new document at the new URL)");

    js_page_close();
    dom_free(root);
    printf("frame-window: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
