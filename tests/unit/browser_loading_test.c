/* Real browser.c app_main, scripts, timers, DOM, layout and native mouse input.
 * The transport fixture alone withholds image completion; its readiness hooks
 * let the old blocking drain finish and record it without a wall-clock
 * timeout. Image bodies deliberately fail the existing fixture decoder: page
 * load must wait for a terminal image result, including an error. Codec and
 * real-network performance evidence belongs to browser_load.py in the guest. */
#define main loader_fixture_main
#include "loader_test.c"
#undef main

/* Reuse the fixture's routes, ownership and URL resolver. Only readiness and
 * wait observation differ; duplicating its bfetch ABI would drift silently. */
#define bfetch_state site_state
#define bfetch_wait site_wait
#define bfetch_release site_release
#define bfetch_take site_take
#define bfetch_pump site_pump
#define bfetch_start site_start
#define img_register site_img_register
#define img_register_anim site_img_register_anim
#include "loader_fakebfetch.c"
#undef bfetch_state
#undef bfetch_wait
#undef bfetch_release
#undef bfetch_take
#undef bfetch_pump
#undef bfetch_start
#undef img_register
#undef img_register_anim
#include "js_dom.h"
void app_main(void);
__attribute__((weak)) void img_register(img_detect_fn a, img_decode_fn b) { (void)a; (void)b; }
__attribute__((weak)) void img_register_anim(img_detect_fn a, img_decode_fn b, img_anim_fn c)
{ (void)a; (void)b; (void)c; }

static int slow_ready, blocked_waits, slow_taken, removed_cancelled, nav_cancelled;
static int stage, steps, script_seen, input_seen, load_seen, cancellation_seen;
static int click_x, click_y;
static int force_ready;

static int pending_image(int id)
{
    if (force_ready || id < 0 || id >= NREQ || !g_req[id].used) return 0;
    const char *u = g_req[id].url;
    return (strstr(u, "/slow.img") && !slow_ready) ||
           strstr(u, "/cancel.img") || strstr(u, "/nav.img");
}
int bfetch_state(int id) { return pending_image(id) ? BF_PENDING : site_state(id); }
int bfetch_start(const char *ref)
{
    char absolute[2048]; struct url parsed;
    /* Match the real fetcher's permanent start failure. The old generic fake
     * allocated a 404 request for malformed URLs, hiding retry-forever bugs. */
    if (bfetch_resolve(0, ref, absolute, sizeof absolute) || url_parse(absolute, &parsed)) return -1;
    return site_start(ref);
}
int bfetch_pump(void)
{
    /* res_fetch_all spins pump/state and even polls chrome through its
     * progress ticker, but has not created the page runtime yet. Let that
     * exact old ordering finish while recording the missing script/UI
     * opportunity. Counting poll_event alone would mistake the progress
     * ticker for a responsive page and turn the negative control green. */
    if (!js_page_live()) {
        for (int i = 0; i < NREQ; i++) if (pending_image(i)) {
            blocked_waits++; force_ready = slow_ready = 1; break;
        }
    }
    return site_pump();
}
void bfetch_wait(int id, void (*tick)(void))
{
    if (pending_image(id)) { blocked_waits++; slow_ready = 1; }
    site_wait(id, tick);
}
void bfetch_release(int id)
{
    if (pending_image(id)) {
        if (strstr(g_req[id].url, "/cancel.img")) removed_cancelled++;
        if (strstr(g_req[id].url, "/nav.img")) nav_cancelled++;
    }
    site_release(id);
}
int bfetch_take(int id, unsigned char **out)
{
    if (id >= 0 && id < NREQ && g_req[id].used && strstr(g_req[id].url, "/slow.img")) slow_taken++;
    return site_take(id, out);
}

static const char *ASYNC_PAGE =
"<!doctype html><link rel=stylesheet href='high_contrast.css'>"
"<style>body{margin:0}#action{position:absolute;left:20px;top:60px;width:220px}"
"#after{position:absolute;left:20px;top:130px}#themed{width:140px}#hook{width:31px}</style>"
"<link disabled rel=stylesheet href='high_contrast.css'>"
"<div id=themeOnly></div><div id=themed></div><div id=hook></div>"
"<a id=action href='#'>CLICK-WHILE-PENDING</a><div id=after>BEFORE-LOAD</div>"
"<img width=32 height=32 src='slow.img'>"
"<script>window.scriptRan=1;window.clicks=0;window.timerRan=0;window.loads=0;"
"window.cancelDone=0;window.trace=['script'];"
"document.getElementById('hook').style.width='93px';"
"window.cssHookWidth=document.getElementById('hook').getBoundingClientRect().width;"
"document.getElementById('action').addEventListener('click',function(e){e.preventDefault();clicks++});"
"document.addEventListener('DOMContentLoaded',function(){trace.push('dom')});"
"setTimeout(function(){timerRan++;trace.push('timer')},20);"
"window.addEventListener('load',function(){loads++;trace.push('load');"
"document.getElementById('after').textContent='LOAD-HANDLER-PAINT';"
"function image(src){var n=document.createElement('img');n.setAttribute('width','32');"
"n.setAttribute('height','32');n.setAttribute('src',src);document.body.appendChild(n);return n;}"
"var cancel=image('cancel.img');image('nav.img');"
/* Removal alone is not cancellation: detached new Image() must still load.
 * Explicitly clear src before removing it to exercise actual cancellation. */
"setTimeout(function(){cancel.setAttribute('src','');window.cancelAttr=cancel.getAttribute('src');cancel.parentNode.removeChild(cancel);cancelDone=1;"
"setTimeout(function(){location.href='bad.html'},60)},60);});</script>";
static const char *BAD_PAGE =
"<!doctype html><img src='ftp://invalid.test/image'><script>window.badRan=1;window.badLoads=0;"
"window.addEventListener('load',function(){badLoads++});</script>";

static int observed_int(const char *src)
{
    JSContext *ctx = js_page_ctx(); if (!ctx) return -999;
    JSValue v = JS_Eval(ctx, src, strlen(src), "<loading-observer>", JS_EVAL_TYPE_GLOBAL);
    int32_t n = -999;
    if (JS_IsException(v)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
    else JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v); return n;
}
static const struct paintop *loading_painted_text(const char *s)
{
    int len = (int)strlen(s);
    for (int i = paint_nops - 1; i >= 0; i--)
        if (paint_ops[i].kind == OP_TEXT && paint_ops[i].len == len &&
            !memcmp(paint_ops[i].text, s, (size_t)len)) return &paint_ops[i];
    return 0;
}
static void post_input(int type, int x, int y, int button)
{
    struct logit_event e = {0}; e.type = type; e.a = x; e.b = y; e.button = button;
    host_post_event(&e);
}
void loader_poll_hook(void)
{
    host_clock += 5;
    if (++steps > 800 && stage != 99) {
        printf("loading budget: stage=%d pending=%d cancel=%d nav=%d\n", stage,
               !slow_ready, removed_cancelled, nav_cancelled);
        CHECK(0, "browser loading phases finish within bounded event turns");
        stage = 99; post_input(EV_CLOSE, 0, 0, 0); return;
    }
    if (stage == 99 || !js_page_live()) return;
    if (stage == 0) {
        if (observed_int("typeof scriptRan==='number'?scriptRan:0") != 1) return;
        CHECK(blocked_waits == 0 && !slow_ready && slow_taken == 0,
              "script bootstrap does not wait for pending images");
        CHECK(observed_int("loads") == 0, "window load waits for delayed image terminal result");
        CHECK(observed_int("cssHookWidth") == 93,
              "app_main registers CSSOM reflow for synchronous style-to-geometry reads");
        CHECK(observed_int("document.getElementById('themeOnly').getBoundingClientRect().width") == 230,
              "ordinary stylesheet named high_contrast.css applies to real geometry");
        CHECK(observed_int("document.getElementById('themed').getBoundingClientRect().width") == 140,
              "disabled duplicate stylesheet never overrides later inline cascade");
        script_seen = 1; stage = 1;
    }
    if (stage == 1) {
        if (observed_int("timerRan") != 1) return;
        const struct paintop *p = loading_painted_text("CLICK-WHILE-PENDING"); if (!p) return;
        CHECK(!slow_ready && slow_taken == 0, "page timers run while image is pending");
        click_x = p->x + 8; click_y = p->y + 8;
        stage = 2; post_input(EV_MOUSE, click_x, click_y, EV_BTN_LEFT); return;
    }
    if (stage == 2) {
        stage = 3; post_input(EV_MOUSE_UP, click_x, click_y, EV_BTN_LEFT); return;
    }
    if (stage == 3) {
        if (observed_int("clicks") != 1) return;
        CHECK(!slow_ready && slow_taken == 0 && observed_int("loads") == 0,
              "native mouse click reaches page before image completion");
        input_seen = 1; paint_nops = 0; slow_ready = 1; stage = 4; return;
    }
    if (stage == 4) {
        if (observed_int("loads") != 1 || !loading_painted_text("LOAD-HANDLER-PAINT")) return;
        CHECK(slow_taken == 1, "image response is consumed exactly once before window load");
        CHECK(observed_int("trace.indexOf('script')<trace.indexOf('dom')&&"
                           "trace.indexOf('dom')<trace.indexOf('load')") == 1,
              "script then DOMContentLoaded then window load ordering");
        CHECK(fake_site_fetched("cancel.img") == 1 && fake_site_fetched("nav.img") == 1,
              "load handler DOM mutation paints and discovers new images without user input");
        load_seen = 1; stage = 5; return;
    }
    if (stage == 5) {
        if (observed_int("cancelDone") != 1 || !removed_cancelled) return;
        CHECK(observed_int("cancelAttr===''")==1,"cancellation actually cleared the DOM image source");
        CHECK(removed_cancelled == 1, "clearing image source cancels its pending transport exactly once");
        CHECK(observed_int("loads") == 1, "post-load images never refire page load");
        cancellation_seen = 1; stage = 6;
        /* Recorder text borrows DOM storage. Navigation frees it; discard old
         * operations before allowing the fixture's next navigation timer. */
        paint_nops = 0; return;
    }
    if (stage == 6) {
        if (!strstr(js_page_location(), "bad.html")) return;
        if (observed_int("typeof badLoads==='number'?badLoads:0") != 1) return;
        CHECK(nav_cancelled == 1, "navigation cancels the old page pending image exactly once");
        CHECK(observed_int("badRan") == 1, "unsupported image URL does not prevent new page script");
        CHECK(observed_int("badLoads") == 1, "unsupported image URL terminates instead of delaying load forever");
        stage = 99; post_input(EV_CLOSE, 0, 0, 0);
    }
}
int main(void)
{
    fake_site_reset();
    fake_site_add("http://fixture.test/async.html", ASYNC_PAGE);
    fake_site_add("http://fixture.test/bad.html", BAD_PAGE);
    fake_site_add("http://fixture.test/high_contrast.css", "#themeOnly{width:230px}#themed{width:240px}");
    fake_site_add("http://fixture.test/slow.img", "terminal image decode error");
    fake_site_add("http://fixture.test/cancel.img", "pending until removed");
    fake_site_add("http://fixture.test/nav.img", "pending until navigation");
    tabs_set_store(&memfs);
    const char *session = "logit-browser-session\t1\t0\n0\thttp://fixture.test/async.html\tfixture\t0\n";
    memfs_write(SESSION_PATH, session, (int)strlen(session));
    post_input(EV_KEY, '\n', 0, 0);
    if (setjmp(host_exit_jmp) == 0) app_main();
    CHECK(host_exited && host_exit_code == 0, "real browser event loop exits normally");
    CHECK(script_seen && input_seen && load_seen && cancellation_seen,
          "all asynchronous loading phases were actually observed");
    CHECK(blocked_waits == 0, "no image path blocks page bootstrap on transport");
    printf("browser-loading: %s (%d virtual polls, %d blocking image drains)\n",
           fail ? "FAIL" : "PASS", steps, blocked_waits);
    return fail;
}
