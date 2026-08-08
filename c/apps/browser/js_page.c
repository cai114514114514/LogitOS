/* The page's JavaScript runtime: lifetime, timers, the console, `window`.
 *
 * See js_page.h for the contract. The short version: the runtime is opened with
 * the document and closed on navigation, so a callback registered during load
 * is still callable when the user clicks a minute later. Everything in this
 * file exists because something has to survive `JS_Eval` returning. */
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"
/* fetch/Storage/history/URL live in js_webapi.c, which the browser links and
 * the host tests of THIS file do not. Every entry point is weak, so a build
 * without it links and simply has no network API -- see js_webapi.h. */
#define JS_WEBAPI_OPTIONAL
#include "js_webapi.h"
/* Timing, the document lifecycle, message queues, the observers -- js_platform.c,
 * weak for the same reason: the host tests of THIS file link without it. */
#define JS_PLATFORM_OPTIONAL
#include "js_platform.h"
/* MediaSource / SourceBuffer / HTMLMediaElement -- js_media.c, weak for the
 * same reason as the two above: the host tests of THIS file link without it and
 * simply come up with no media. */
#define JS_MEDIA_OPTIONAL
#include "js_media.h"
/* CSSOM-View geometry, document.styleSheets, the CSS namespace object --
 * js_cssom.c, weak for the same reason as the three above. */
#define JS_CSSOM_OPTIONAL
#include "js_cssom.h"
/* The Event constructor hierarchy, a constructible EventTarget and the rest of
 * addEventListener's options surface -- js_events.c, layered over the native
 * dispatcher js_dom.c owns. Weak for the same reason as the three above. */
#define JS_EVENTS_OPTIONAL
#include "js_events.h"
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

/* ---- the console buffer ----
 * Bounded on purpose: a page in a console.log loop must not be able to grow the
 * browser's heap without limit, and only the first line is ever displayed. Once
 * full, output is dropped rather than rotated -- the FIRST messages are the ones
 * that explain a page, not the ten-thousandth. */
#define OUTCAP 4096
static char g_out[OUTCAP];
static int  g_outlen;

/* An optional observer on everything that goes into the buffer above.
 *
 * The buffer is capped at 4 KiB on purpose and that cap is right for the
 * browser -- only the first line is ever displayed, and a page in a
 * console.log loop must not be able to grow the heap. It is wrong for an
 * INSTRUMENT: tests/unit/webapi_probe.c counts a page's uncaught exceptions,
 * kimi produces more than 4 KiB of them, and the probe was silently reading a
 * truncated list and reporting the truncated count. Raising OUTCAP would fix
 * the measurement by changing the thing measured; a sink does not.
 *
 * Fragments, not lines: note() is called several times per message. The
 * observer assembles them, which keeps this end free of a line buffer. */
static void (*g_note_sink)(const char *frag);
void js_page_set_note_sink(void (*fn)(const char *frag)) { g_note_sink = fn; }

static void note(const char *s)
{
    if (!s) return;
    if (g_note_sink) g_note_sink(s);
    while (*s && g_outlen < OUTCAP - 1) g_out[g_outlen++] = *s++;
    g_out[g_outlen] = 0;
}

const char *js_page_output(void)     { return g_out; }
int         js_page_output_len(void) { return g_outlen; }
void        js_page_output_clear(void) { g_outlen = 0; g_out[0] = 0; }

/* ---- the clock ---- */
static unsigned long long (*g_clock)(void);
void js_page_set_clock(unsigned long long (*fn)(void)) { g_clock = fn; }
static unsigned long long now_ms(void) { return g_clock ? g_clock() : 0; }

static JSRuntime *g_rt;
static JSContext *g_ctx;
static unsigned long long g_t0;              /* page start, for performance.now() */
static char g_location[600] = "about:blank";

int  js_page_live(void) { return g_ctx != 0; }
JSContext *js_page_ctx(void) { return g_ctx; }
void js_page_set_location(const char *url)
{
    int i = 0;
    if (url) while (url[i] && i < (int)sizeof g_location - 1) { g_location[i] = url[i]; i++; }
    g_location[i] = 0;
}

/* ---- timers ----
 *
 * One queue for setTimeout, setInterval and requestAnimationFrame, because they
 * differ only in when they are re-armed. It is a plain linked list scanned
 * linearly: a page holds a handful of timers, and a heap would cost more in
 * code than it saves in comparisons.
 *
 * `seq` is the tiebreaker. Two timers with the same deadline -- which is the
 * COMMON case here, since the clock only advances in 10 ms steps -- must fire in
 * the order they were created, or `setTimeout(a,0); setTimeout(b,0)` becomes a
 * coin flip. */
struct jstimer {
    struct jstimer *next;
    int id;
    int raf;                       /* requestAnimationFrame callback */
    int interval_ms;               /* >0: re-arm after firing (setInterval) */
    unsigned long long due;
    unsigned long long seq;
    JSValue fn;
    JSValue *argv; int argc;       /* extra setTimeout(fn, ms, ...args) arguments */
};

static struct jstimer *g_timers;
static int g_next_id = 1;                    /* setTimeout/setInterval handles */
static int g_next_raf_id = 1;                /* rAF has its own handle space, per spec */
static unsigned long long g_seq;
static unsigned long long g_frame_due;       /* the next animation-frame boundary */

/* 60 Hz is the target; the monotonic clock advances in 10 ms steps, so the real
 * cadence is 20 ms. Asking for 16 and letting the clock round up is honest --
 * claiming 16 would just mean firing every tick, i.e. a 100 Hz busy loop. */
#define FRAME_MS 16

static void timer_free(JSContext *ctx, struct jstimer *t)
{
    JS_FreeValue(ctx, t->fn);
    for (int i = 0; i < t->argc; i++) JS_FreeValue(ctx, t->argv[i]);
    free(t->argv);
    free(t);
}

static void timers_clear(JSContext *ctx)
{
    while (g_timers) { struct jstimer *t = g_timers; g_timers = t->next; timer_free(ctx, t); }
    g_frame_due = 0;
}

static void timer_unlink(struct jstimer *t)
{
    for (struct jstimer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; return; }
}

int js_page_pending(void)
{
    if (g_timers) return 1;
    /* A fetch in flight also needs the loop to call js_page_run_due(), which is
     * where its socket is stepped. */
    return js_webapi_pending && js_webapi_pending();
}

long long js_page_next_due(void)
{
    long long best = -1;
    for (struct jstimer *t = g_timers; t; t = t->next)
        if (best < 0 || (long long)t->due < best) best = (long long)t->due;
    return best;
}

int js_page_pump(void) { return js_dom_run_jobs(g_ctx); }

int js_page_run_due(void)
{
    if (!g_ctx) return 0;
    int ran = 0;

    /* In-flight fetches first: they are the thing that must make progress on
     * EVERY pass of the loop, timers or no timers. A resolved promise queues
     * its reactions, so drain the microtask queue before returning -- the
     * embedder repaints on a non-zero return and the .then() that writes the
     * DOM has to have run by then. */
    if (js_webapi_pump) {
        int n = js_webapi_pump(g_ctx);
        if (n > 0) { js_dom_run_jobs(g_ctx); ran += n; }
    }
    if (!g_timers) return ran;

    unsigned long long now = now_ms();
    /* Snapshot the sequence counter: a callback that schedules another timer for
     * "now" must wait for the next pass. Without this, `setTimeout(f, 0)` calling
     * itself would spin inside this loop and the browser would never repaint.
     * (A re-armed setInterval takes a fresh seq for the same reason.) */
    unsigned long long limit = g_seq;

    for (;;) {
        struct jstimer *best = 0;
        for (struct jstimer *t = g_timers; t; t = t->next) {
            if (t->due > now || t->seq > limit) continue;
            if (!best || t->due < best->due || (t->due == best->due && t->seq < best->seq))
                best = t;
        }
        if (!best) break;

        /* Take our own references before calling: the callback may clear this
         * very timer (clearInterval(self) is idiomatic), which would otherwise
         * free the JSValue we are about to invoke. */
        JSValue fn = JS_DupValue(g_ctx, best->fn);
        int nargs = best->argc;
        JSValue *args = 0;
        if (best->raf) {
            nargs = 1;
            args = malloc(sizeof *args);
            if (args) args[0] = JS_NewFloat64(g_ctx, (double)(now - g_t0));
            else nargs = 0;
        } else if (nargs > 0) {
            args = malloc((size_t)nargs * sizeof *args);
            if (args) for (int i = 0; i < nargs; i++) args[i] = JS_DupValue(g_ctx, best->argv[i]);
            else nargs = 0;
        }

        if (best->interval_ms > 0) {
            /* Re-arm from NOW, not from the old deadline: catching up on missed
             * ticks after a slow page load would fire a burst of callbacks the
             * page never asked for. */
            best->due = now + (unsigned long long)best->interval_ms;
            best->seq = ++g_seq;
        } else {
            timer_unlink(best);
            timer_free(g_ctx, best);
        }

        JSValue r = JS_Call(g_ctx, fn, JS_UNDEFINED, nargs, (JSValueConst *)args);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(g_ctx);
            const char *m = JS_ToCString(g_ctx, e);
            printf("[js] uncaught in timer: %s\n", m ? m : "?");
            note("[exception] "); if (m) note(m); note("\n");
            if (m) JS_FreeCString(g_ctx, m);
            JS_FreeValue(g_ctx, e);
        }
        JS_FreeValue(g_ctx, r);
        JS_FreeValue(g_ctx, fn);
        for (int i = 0; i < nargs; i++) JS_FreeValue(g_ctx, args[i]);
        free(args);
        js_dom_run_jobs(g_ctx);          /* a timer that resolves a promise: run its reactions now */
        ran++;
    }

    /* The frame boundary has passed; the next rAF starts a new frame. */
    if (g_frame_due && g_frame_due <= now) g_frame_due = 0;
    return ran;
}

static int timer_add(JSContext *ctx, JSValueConst fn, long delay, int interval,
                     int raf, int argc, JSValueConst *argv)
{
    struct jstimer *t = calloc(1, sizeof *t);
    if (!t) return 0;
    unsigned long long now = now_ms();
    if (raf) {
        /* Every rAF callback registered before the boundary runs in the same
         * frame with the same timestamp -- that is what lets a page schedule
         * several animations and have them stay in step. */
        if (!g_frame_due || g_frame_due <= now) g_frame_due = now + FRAME_MS;
        t->due = g_frame_due;
        t->id = g_next_raf_id++;
        t->raf = 1;
    } else {
        if (delay < 0) delay = 0;
        t->due = now + (unsigned long long)delay;
        t->id = g_next_id++;
        t->interval_ms = interval ? (delay > 0 ? (int)delay : 1) : 0;
    }
    t->seq = ++g_seq;
    t->fn = JS_DupValue(ctx, fn);
    if (argc > 0) {
        t->argv = malloc((size_t)argc * sizeof *t->argv);
        if (t->argv) { for (int i = 0; i < argc; i++) t->argv[i] = JS_DupValue(ctx, argv[i]); t->argc = argc; }
    }
    t->next = g_timers;
    g_timers = t;
    return t->id;
}

static void timer_cancel(JSContext *ctx, int id, int raf)
{
    for (struct jstimer **pp = &g_timers; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id && (*pp)->raf == raf) {
            struct jstimer *t = *pp;
            *pp = t->next;
            timer_free(ctx, t);
            return;
        }
}

/* setTimeout(fn, delay, ...args). A string first argument (the eval form) is
 * deliberately not supported -- it is deprecated, and supporting it would mean
 * carrying a second code path that can only ever compile untrusted text. */
static JSValue js_setTimeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], ms, 0, 0,
                                      argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : 0));
}
static JSValue js_setInterval(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int32_t ms = 0;
    if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], ms, 1, 0,
                                      argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : 0));
}
static JSValue js_clearTimer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    timer_cancel(ctx, id, 0);
    return JS_UNDEFINED;
}
static JSValue js_raf(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, timer_add(ctx, argv[0], 0, 0, 1, 0, 0));
}
static JSValue js_caf(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    timer_cancel(ctx, id, 1);
    return JS_UNDEFINED;
}
static JSValue js_perf_now(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)(now_ms() - g_t0));
}

/* ---- console ----
 * Richer than js_dom.c's fallback: this one also captures into the buffer the
 * browser's status bar reads, which is the only place a headless screenshot
 * test can see that a script ran. */
static JSValue con_out(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) { note(" "); printf(" "); }
        note(s); printf("%s", s);
        JS_FreeCString(ctx, s);
    }
    note("\n"); printf("\n");
    return JS_UNDEFINED;
}
static JSValue con_warn(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ note("[warn] "); printf("[warn] "); return con_out(ctx, t, argc, argv); }
static JSValue con_error(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ note("[error] "); printf("[error] "); return con_out(ctx, t, argc, argv); }

/* ---- document.currentScript -------------------------------------------
 *
 * MEASURED on two unrelated sites, and it is not a page quirk -- it is the
 * mechanism bundlers use to find their own chunks and the mechanism inline
 * scripts use to find themselves:
 *
 *   nodejs.org   the webpack/Next.js chunk loader derives its base URL from
 *                document.currentScript.src, finds nothing, checks whether it
 *                is in a Worker, and throws its OWN error: "chunk path empty
 *                but not in a worker". Every script after it on the page then
 *                fails too -- one undefined property, the whole page.
 *   x.com        four uncaught exceptions, all `cannot read property 'remove'
 *                of undefined`, from the standard document.currentScript
 *                .remove() idiom where an inline script deletes its own tag.
 *
 * WHY IT IS AN INDEX AND NOT A NODE. js_dom.c's node-to-JSValue wrapper is
 * static to that file, so this file cannot hand a JSValue for a node to
 * anybody. What it CAN do is say which <script> element, by position in
 * document order -- and js_select.c already publishes document.scripts in
 * exactly that order. So the C side publishes __currentScriptIndex() and
 * js_platform.c defines the property as document.scripts[i]. The alternative
 * was a js_dom.c export, which is another line's file.
 *
 * WHICH SCRIPT IS RUNNING is worked out here rather than passed in, because
 * the embedder (browser.c, another line's file) calls js_page_eval with a
 * filename and no node. An external script is matched by its src attribute
 * against that filename; an inline one takes the next inline <script> in
 * document order. Both consume from the same cursor, so a page that
 * interleaves them stays aligned. */
static int g_cur_script = -1;
static int g_script_used[256];

static int script_index_for(struct node *n, const char *filename, int *idx, int *found)
{
    if (!n) return 0;
    if (n->type == N_ELEM && n->tag && !strcmp(n->tag, "script")) {
        int i = (*idx)++;
        if (*found < 0 && i < 256 && !g_script_used[i]) {
            const char *src = dom_attr(n, "src");
            int match;
            if (src && src[0]) {
                /* The filename the embedder passes is the ABSOLUTE url; the
                 * attribute is whatever the author wrote. A suffix test is
                 * enough to pair them and needs no URL resolver here. */
                int sl = (int)strlen(src), fl = filename ? (int)strlen(filename) : 0;
                match = filename && fl >= sl && !strcmp(filename + fl - sl, src);
            } else {
                match = !filename || !strchr(filename, ':');   /* an inline one */
            }
            if (match) { *found = i; g_script_used[i] = 1; }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        script_index_for(c, filename, idx, found);
    return *found;
}

static JSValue js_cur_script_index(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(ctx, g_cur_script);
}

/* Exported because js_page_eval is not the only caller that runs a page's
 * classic script: tests/unit/webapi_probe.c evaluates each one itself so it
 * can keep the exception object rather than the printed message. Without
 * these it measured a document.currentScript that was null for every script
 * on every page -- an instrument reporting the feature as broken because it
 * had not turned it on. */
void js_page_begin_script(const char *filename)
{
    int idx = 0, found = -1;
    script_index_for(js_dom_root(), filename, &idx, &found);
    g_cur_script = found;
}
void js_page_end_script(void) { g_cur_script = -1; }

int js_page_open(struct node *root)
{
    js_page_close();
    g_rt = JS_NewRuntime();
    if (!g_rt) return 0;
    /* The browser's ring-3 user stack is 8 MiB (wm.c gives it double the normal
     * app stack). Bound QuickJS's overflow guard well under that so a deeply
     * recursive script throws a catchable "stack overflow" RangeError instead of
     * overrunning the real stack -- and, critically, so the THROW itself
     * (JS_ThrowError2 builds an Error + backtrace, ~2 MiB) has ample room below
     * the guard. 2 MiB limit -> the guard fires with ~6 MiB still free. */
    JS_SetMaxStackSize(g_rt, 2 * 1024 * 1024);
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx) { JS_FreeRuntime(g_rt); g_rt = 0; return 0; }

    g_t0 = now_ms();
    g_seq = 0; g_next_id = 1; g_next_raf_id = 1; g_frame_due = 0;
    g_cur_script = -1;
    memset(g_script_used, 0, sizeof g_script_used);

    JSValue g = JS_GetGlobalObject(g_ctx);
    JSValue con = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, con, "log",   JS_NewCFunction(g_ctx, con_out, "log", 1));
    JS_SetPropertyStr(g_ctx, con, "info",  JS_NewCFunction(g_ctx, con_out, "info", 1));
    JS_SetPropertyStr(g_ctx, con, "debug", JS_NewCFunction(g_ctx, con_out, "debug", 1));
    JS_SetPropertyStr(g_ctx, con, "warn",  JS_NewCFunction(g_ctx, con_warn, "warn", 1));
    JS_SetPropertyStr(g_ctx, con, "error", JS_NewCFunction(g_ctx, con_error, "error", 1));
    JS_SetPropertyStr(g_ctx, g, "console", con);
    JS_SetPropertyStr(g_ctx, g, "print", JS_NewCFunction(g_ctx, con_out, "print", 1));
    JS_SetPropertyStr(g_ctx, g, "alert", JS_NewCFunction(g_ctx, con_out, "alert", 1));

    JS_SetPropertyStr(g_ctx, g, "setTimeout",  JS_NewCFunction(g_ctx, js_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(g_ctx, g, "setInterval", JS_NewCFunction(g_ctx, js_setInterval, "setInterval", 2));
    JS_SetPropertyStr(g_ctx, g, "clearTimeout",  JS_NewCFunction(g_ctx, js_clearTimer, "clearTimeout", 1));
    JS_SetPropertyStr(g_ctx, g, "clearInterval", JS_NewCFunction(g_ctx, js_clearTimer, "clearInterval", 1));
    JS_SetPropertyStr(g_ctx, g, "requestAnimationFrame",
                      JS_NewCFunction(g_ctx, js_raf, "requestAnimationFrame", 1));
    JS_SetPropertyStr(g_ctx, g, "cancelAnimationFrame",
                      JS_NewCFunction(g_ctx, js_caf, "cancelAnimationFrame", 1));

    JSValue perf = JS_NewObject(g_ctx);
    JS_SetPropertyStr(g_ctx, perf, "now", JS_NewCFunction(g_ctx, js_perf_now, "now", 0));
    JS_SetPropertyStr(g_ctx, g, "performance", perf);

    /* The bridge js_platform.c turns into document.currentScript. Not a
     * property of `document` here because js_dom.c owns that object and
     * installs it below; see the comment on script_index_for. */
    JS_SetPropertyStr(g_ctx, g, "__currentScriptIndex",
                      JS_NewCFunction(g_ctx, js_cur_script_index, "__currentScriptIndex", 0));

    /* `location` normally comes from js_webapi_install below, parsed into
     * components and writable. This href-only stand-in is what a build without
     * js_webapi.c gets -- the surface it had before -- so dropping that file
     * from a link takes away fetch, not the page's own address. */
    if (!js_webapi_install) {
        JSValue loc = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, loc, "href", JS_NewString(g_ctx, g_location));
        JS_SetPropertyStr(g_ctx, g, "location", loc);
    }
    JSValue nav = JS_NewObject(g_ctx);
    /* A Mozilla/5.0 prefix because a startling amount of shipped JS branches on
     * it before it will render anything at all. The rest is truthful. */
    JS_SetPropertyStr(g_ctx, nav, "userAgent",
                      JS_NewString(g_ctx, "Mozilla/5.0 (LogitOS; x86_64) Logit/1.0"));
    JS_SetPropertyStr(g_ctx, nav, "appName", JS_NewString(g_ctx, "Netscape"));
    JS_SetPropertyStr(g_ctx, nav, "platform", JS_NewString(g_ctx, "LogitOS x86_64"));
    JS_SetPropertyStr(g_ctx, nav, "language", JS_NewString(g_ctx, "en-US"));
    { JSValue langs = JS_NewArray(g_ctx);
      JS_SetPropertyUint32(g_ctx, langs, 0, JS_NewString(g_ctx, "en-US"));
      JS_SetPropertyStr(g_ctx, nav, "languages", langs); }
    JS_SetPropertyStr(g_ctx, nav, "onLine", JS_NewBool(g_ctx, 1));
    JS_SetPropertyStr(g_ctx, nav, "cookieEnabled", JS_NewBool(g_ctx, 0));   /* no jar on this path */
    JS_SetPropertyStr(g_ctx, nav, "hardwareConcurrency", JS_NewInt32(g_ctx, 1));
    JS_SetPropertyStr(g_ctx, nav, "maxTouchPoints", JS_NewInt32(g_ctx, 0));
    JS_SetPropertyStr(g_ctx, g, "navigator", nav);

    /* `window === globalThis`, so `window.foo = 1; foo` works and the mountain
     * of feature detection that starts with `typeof window` sees a browser. */
    JS_SetPropertyStr(g_ctx, g, "window", JS_DupValue(g_ctx, g));
    JS_SetPropertyStr(g_ctx, g, "self", JS_DupValue(g_ctx, g));

    js_dom_set_note(note);
    js_dom_init(g_ctx, root);            /* installs document + Element + Event */
    js_dom_bind_event_target(g_ctx, g);  /* window.addEventListener + window.on* */
    /* AFTER the DOM: js_webapi publishes document.location and dispatches
     * popstate through window.dispatchEvent, both of which js_dom.c owns. */
    if (js_webapi_install) js_webapi_install(g_ctx, g_location);
    /* LAST. js_platform.c fills gaps in what the two above publish (document,
     * navigator, performance, localStorage) and every one of its installs is
     * conditional on the property being absent -- which only means anything
     * once everyone who owns one has had their turn. */
    if (js_select_install) js_select_install(g_ctx);
    if (js_intl_install) js_intl_install(g_ctx);
    /* AFTER js_dom_init (it takes the Element prototype) and after the platform
     * fills in `document`; MediaSource has no dependency on either, but the
     * HTMLMediaElement members are installed on the element prototype. */
    if (js_media_install) js_media_install(g_ctx);
    if (js_platform_install) js_platform_install(g_ctx);
    /* AFTER all of the above, and the ordering is not a preference. js_cssom.c
     * takes the Element prototype js_dom.c published, and it deliberately
     * REPLACES two bindings older files install: getBoundingClientRect (its
     * version flushes a pending layout first) and matchMedia (its version is
     * the cascade's own media evaluator, which closes the divergence css.h
     * names). Installing it earlier means those two get overwritten again. */
    if (js_cssom_install) js_cssom_install(g_ctx);
    /* LAST of the last. The event layer needs js_dom.c's native Event classes
     * to wrap, js_webapi.c's AbortSignal for the `signal` option, and it
     * deliberately REPLACES two placeholders js_platform.c installs when
     * nobody better has (EventTarget, PromiseRejectionEvent) -- so unlike
     * js_platform.c's "only if absent" rule, this one has to run after the
     * placeholder exists in order to take it over. */
    if (js_events_install) js_events_install(g_ctx);
    JS_FreeValue(g_ctx, g);
    return 1;
}

void js_page_close(void)
{
    if (!g_ctx) { g_rt = 0; return; }
    /* Order is load-bearing. Everything holding a JSValue must let go before
     * JS_FreeRuntime, which asserts on live GC objects -- and js_dom_cleanup
     * also clears the DOM's weak wrapper slots, so no node is left pointing at
     * a JSObject in a runtime that is about to stop existing. */
    timers_clear(g_ctx);
    if (js_platform_close) js_platform_close(g_ctx);  /* unhooks the rejection tracker */
    if (js_webapi_close) js_webapi_close(g_ctx);   /* aborts fetches, drops promise resolvers */
    if (js_media_close) js_media_close(g_ctx);     /* stops playback, frees the DPBs */
    if (js_cssom_close) js_cssom_close(g_ctx);     /* drops the node lookup cache */
    js_dom_cleanup(g_ctx);
    js_dom_set_note(0);
    JS_FreeContext(g_ctx);
    JS_FreeRuntime(g_rt);
    g_ctx = 0; g_rt = 0;
}

int js_page_eval(const char *src, int len, const char *filename)
{
    if (!g_ctx || !src) return 0;
    js_page_begin_script(filename);
    JSValue v = JS_Eval(g_ctx, src, (size_t)len, filename ? filename : "<page>",
                        JS_EVAL_TYPE_GLOBAL);
    /* Cleared before the microtask drain: currentScript is null everywhere
     * except a classic script's own synchronous execution, and a .then()
     * queued by the script runs after it, not during it. */
    js_page_end_script();
    int ok = !JS_IsException(v);
    if (!ok) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("[browser] JS exception: %s\n", m ? m : "?");
        /* AND THE STACK, because the message alone is not a diagnosis.
         *
         * `TypeError: cannot read property 'charAt' of undefined` names the
         * operation and nothing else -- not the function, not the file, not
         * which of a bundle's thousands of call sites. Every real page that
         * failed today failed with a line of exactly that shape, and each one
         * cost a separate investigation to place.
         *
         * The stack was always here; this printed the message and dropped it.
         * That is the same defect the engine had until a QuickJS patch landed
         * today -- build_backtrace() emitted frames with no message header, so
         * every reporter on every page printed a stack with no error in it.
         * Fixing one half and leaving the other prints two useless halves.
         *
         * Defensive on purpose: a thrown value need not be an Error, `stack`
         * may be absent or a getter, and this runs while an exception is
         * already in flight -- so an accessor that threw would have nowhere to
         * go. Read it only when it is a plain string. */
        if (JS_IsObject(e)) {
            JSValue st = JS_GetPropertyStr(g_ctx, e, "stack");
            if (JS_IsString(st)) {
                const char *s = JS_ToCString(g_ctx, st);
                if (s && *s) printf("%s", s);
                if (s) JS_FreeCString(g_ctx, s);
            }
            JS_FreeValue(g_ctx, st);
        }
        note("[exception] "); if (m) note(m); note("\n");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
    }
    JS_FreeValue(g_ctx, v);
    /* Promise reactions queued by the script run now, not "eventually": a page
     * whose whole body is `main().then(render)` has to have rendered by the time
     * the loader repaints. */
    js_dom_run_jobs(g_ctx);
    return ok;
}
