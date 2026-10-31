/* js_frame.c -- a same-origin second browsing context: a fresh JSContext on
 * the page's own JSRuntime, adopted by an <iframe>'s already-parsed
 * DOMParser-backed document so a <script> inserted into it actually RUNS.
 *
 * THE SPECIMEN THIS CLOSES: Cloudflare's JS-detections bootstrap (injected
 * into every proxied response) does exactly this --
 *   var e = document.createElement("iframe");
 *   function n(){ var n = e.contentDocument || e.contentWindow.document;
 *     if(n){ var t = n.createElement("script"); t.innerHTML = "...loader...";
 *            n.getElementsByTagName("head")[0].appendChild(t) } }
 * -- an about:blank frame built to get a JavaScript environment the page has
 * not overridden. Before this file: createElement was undefined on a frame
 * document (js_domparser.c had no write surface) and even with it added, the
 * appended <script> was DATA -- js_platform.c's own header said so by name.
 * Both walls are closed here. Whether Cloudflare then issues a token is
 * Cloudflare's decision and nothing in this file or its callers may branch
 * on a hostname to influence that -- see js_platform.c's iframe comment and
 * CLAUDE.md's standing constraint: general capability, never site fitting.
 *
 * THE ARCHITECTURAL CHOICE, and it is the OPPOSITE of js_worker.c's on
 * purpose: JS_NewContext(rt) on the PAGE's EXISTING runtime, not
 * JS_NewRuntime(). Three independent reasons, in order of weight:
 *
 * 1. IT IS LITERALLY WHAT THE SPECIMEN IS ASKING FOR. "an about:blank frame
 *    ... to get a JavaScript environment the page has not overridden" is a
 *    description of fresh intrinsics, not of thread isolation. A second
 *    JSContext on one runtime has its own Object/Function/Array/JSON/RegExp
 *    prototypes, untouched by whatever the page monkey-patched -- delivered
 *    by the mechanism (JS_NewContext always builds a fresh global realm)
 *    rather than simulated.
 * 2. SAME-ORIGIN FRAMES ARE SUPPOSED TO BE MUTUALLY REACHABLE. QuickJS
 *    values do not cross RUNTIMES (js_worker.c's whole reason for a clone
 *    boundary), but they cross CONTEXTS of one runtime freely -- so
 *    `frame.contentDocument.body` and the parent's own handle to that same
 *    node are the SAME object, which is what same-origin access requires
 *    and a worker-style clone boundary would have gotten wrong.
 * 3. IT SIDESTEPS js_dom.c's SINGLETON ENTIRELY, WHICH IS WHY THIS FILE
 *    EXISTS AT ALL RATHER THAN A REAL SECOND DOCUMENT. js_dom.c:4364 calls
 *    `JS_NewClassID(&elem_cid)` UNGUARDED inside js_dom_init, with the
 *    comment above it explaining the old `if (!elem_cid)` guard was removed
 *    because js_page.c builds a fresh runtime per PAGE (plural pages,
 *    sequential). Calling js_dom_init a SECOND time -- which a real frame
 *    DOM would require -- does not leak the parent's state, it OVERWRITES
 *    it: g_root/g_ctx/g_document/elem_cid (js_dom.c:77,83,986,243) all
 *    become the frame's, and every js_dom.c entry point that reads those
 *    statics directly (getElementById, querySelector, createElement,
 *    the whole window-level EventTarget, ...) would then have the PARENT
 *    PAGE reading the untrusted child's tree while believing it reads its
 *    own -- a vulnerability shape, not a bug. That single measurement is
 *    also why the same-origin frame built here has NO DOM OF ITS OWN: a
 *    frame script that references `document` gets a ReferenceError, stated,
 *    not a silently wrong document. Building a real one is a de-singleton
 *    project against js_dom.c/js_dom_iface.inc that this pass does not
 *    take on (and that file is owned by other work as this was written).
 *
 *    CORRECTION 2026-09-25, kept beside the sentence above because it is the
 *    one somebody will arrive holding. "NO DOM OF ITS OWN" is still true --
 *    there is no second js_dom_init, for exactly the measured reason above.
 *    "A frame script that references `document` gets a ReferenceError" is
 *    no longer true, and it was never the honest answer it read as: the frame
 *    always HAD a document -- the DOMParser-backed one js_platform.c hands the
 *    parent as `contentDocument`, whose inserted <script>s this file runs. A
 *    script running for that document and unable to name it is not a stated
 *    limit, it is a broken Window. Measured on openai.com through the JS probe
 *    build: Cloudflare's JS-detections bootstrap reached this file, ran, and
 *    died on `ReferenceError: 'document' is not defined` at its first use of
 *    `document` -- its whole reason for building the frame. The frame's
 *    global is now a Window for that document: see frame_install_window()
 *    for every member and the Chromium measurement each one is held to.
 *    Nothing here builds a second DOM: `document` IS the object the parent
 *    already reads as contentDocument, which is also what same-origin access
 *    requires of it.
 *
 * WHAT THIS BUYS FOR FREE, MEASURED RATHER THAN ASSUMED:
 *   - THE WATCHDOG. JS_SetInterruptHandler is set on the RUNTIME
 *     (js_page.c:924, `JS_SetInterruptHandler(g_rt, slice_interrupt, 0)`),
 *     not the context, so a frame script running in this file's JSContext is
 *     ALREADY covered by js_page.c's g_slice_* budget -- no watchdog is
 *     installed below. The cost, stated rather than discovered: a runaway
 *     frame script spends the PAGE's slice, not a private one. A page that
 *     opens a frame and the frame's script both burn CPU compete for one
 *     budget; that is a real, load-bearing difference from js_worker.c
 *     (which keeps a private watchdog exactly so a lowered worker budget
 *     never lowers the page's), named here so nobody rediscovers it as a
 *     mystery timeout.
 *   - THE MICROTASK QUEUE. QuickJS's job queue is per-RUNTIME, not per
 *     context (js_dom_run_jobs / js_page.c's own drain already pumps
 *     `JS_ExecutePendingJob(g_rt, &ctx)`, which does not care which context
 *     enqueued the job). So a frame script that does
 *     `Promise.resolve().then(fn)` gets `fn` run by the PAGE's own existing
 *     drain loop, with zero code added here and zero edits to js_page.c.
 *     This is the one place js_worker.c's model (a private runtime, a
 *     private worker_drain_jobs) does NOT transfer, and transferring it
 *     anyway would have been dead code.
 *
 * WHAT IS DELIBERATELY NOT HERE -- THE CUT, NAMED:
 *   1. NO CROSS-ORIGIN FRAMES. js_platform.c's existing same-origin gate
 *      (`SAME-ORIGIN IS THE GATE, NOT A COURTESY`) stays exactly as it was.
 *      __frameAdopt is only ever reached for a `doc` js_platform.c's own
 *      settle() already decided is same-origin (or about:blank/srcdoc, which
 *      carry no origin to violate) -- this file adds no origin logic of its
 *      own because js_dom.c:4364 proves a REAL cross-origin boundary cannot
 *      be built on top of the DOM this browser has today (see point 3
 *      above); the Turnstile widget itself (challenges.cloudflare.com IN AN
 *      IFRAME, cross-origin by definition) stays unreachable through this
 *      path. Said plainly: this closes the Cloudflare BOOTSTRAP, not the
 *      CHECKBOX.
 *   2. NO PIXELS. A frame's document is never laid out or painted --
 *      layout.h's public surface is one display list for the whole process
 *      (layout_page/layout_items/layout_free take no document handle), and
 *      layout.c/browser_paint.c are owned by other work as this was
 *      written. The <iframe> element keeps the empty box layout already
 *      gives it. This is why the Cloudflare bootstrap (a detection sandbox
 *      that renders nothing by design) is exactly the shape this closes and
 *      a visible embed (Stripe's card field, a YouTube/bilibili player) is
 *      not.
 *   3. NO NESTED FRAMES. A frame document is not re-scanned for <iframe>;
 *      js_platform.c already logs this refusal by name and it is unchanged.
 *   4. ~~NO frame-side `document`, `fetch`, `XMLHttpRequest`, or DOM of any
 *      kind -- see point 3 of the architecture note. `console`, `window`
 *      and `self` only.~~ CORRECTED 2026-09-25 (see the correction under
 *      point 3): `document` is the frame's own document; `fetch`,
 *      `XMLHttpRequest` and a short list of other realm-independent services
 *      are the CREATOR's, reached under a stated rule (FRAME_SHARED_* below).
 *      Still true of the old sentence: no DOM interface hierarchy of the
 *      frame's own. Node/Element/HTMLElement/Document are deliberately NOT
 *      shared from the creator -- they describe the creator's js_dom.c nodes
 *      and would answer `instanceof` wrongly about this frame's DOMParser
 *      nodes, and a wrong answer is worse than the ReferenceError.
 *   5. ~~NO frame-side setTimeout/setInterval.~~ CORRECTED 2026-09-25: both
 *      exist now, and neither cost named below was paid. They are native
 *      functions of the FRAME's realm that schedule through the CREATOR's own
 *      setTimeout/setInterval/requestAnimationFrame -- JS values this file
 *      reaches through the creator's global object, so there is no second
 *      task list and no fourth pump hook in js_page.c -- with a callback
 *      wrapper that runs nothing once the frame is gone, and __frameRelease
 *      cancels whatever the frame still had queued. The page's rule for a
 *      string handler (refused, returns 0) is inherited by delegation, not
 *      restated. The retired text, kept for whoever holds it:
 *      NO frame-side setTimeout/setInterval. A frame script that schedules
 *      one gets ReferenceError, not a silently-dropped call -- adding a
 *      scheduler here would mean either a second flat task list this file
 *      owns in isolation (invisible to js_page.c's drain, the exact failure
 *      js_worker.h's header warns against) or reaching into js_page.c to
 *      register a fourth pump hook beside its three -- and js_page.c was
 *      being actively edited by another line of work as this was written.
 *      The Cloudflare bootstrap itself needs none of this: its loader runs
 *      synchronously off `t.innerHTML = ...; head.appendChild(t)`.
 *   6. NO SCRIPT `src`. An inline <script> (text content or innerHTML) runs;
 *      one with a `src` attribute is refused BY NAME (a console line, not
 *      silence) rather than half-built as a fetch this file would then have
 *      to make same-origin-safe on its own. bfetch is fetcher-safe for this
 *      (see the scope note bfetch_resolve/bfetch_sync already handle
 *      concurrent same-runtime callers with an explicit base), but wiring a
 *      frame script's `src` through it, INCLUDING the case where that
 *      changes which document.currentScript-shaped things a frame script
 *      can observe, is genuinely more file than this pass buys back.
 *      NOTE 2026-09-25: until today this refusal was unreachable from the
 *      commonest loader shape, `s = document.createElement('script');
 *      s.src = url; head.appendChild(s)` -- DOMParser elements had no `src`
 *      reflection, so the assignment made an expando and the script arrived
 *      here as an EMPTY INLINE one and was dropped without a word.
 *      js_domparser.c reflects `src` now, so that shape reaches this refusal
 *      by name. It is the next wall on the measured Cloudflare path, and it
 *      is left standing here deliberately rather than half-crossed.
 *
 * LIFECYCLE. Every adopted frame's JSContext is freed by js_frame_close_all,
 * called from js_platform_close() (js_platform.c), which js_page_close()
 * already calls BEFORE JS_FreeContext(g_ctx)/JS_FreeRuntime(g_rt) -- see
 * js_page.c's own comment above js_worker_close_all() for why that ordering
 * is load-bearing (JS_FreeRuntime asserts on live GC objects) and js_frame.h
 * for why the hook lives in js_platform.c and not a new js_page.c edit: that
 * file was being actively written by other work when this was built, and
 * js_platform_close() is an EXISTING hook js_page_close() already calls in
 * the right place, so no new door had to be cut into a contested file.
 * ~~There is deliberately no PER-FRAME free (e.g. when an <iframe> is
 * removed).~~ CORRECTION, kept beside the old claim: the 8-slot bound limits
 * simultaneously live frames, not the number a page may create over its
 * lifetime. WPT repeatedly creates one iframe, removes it, then creates the
 * next; before __frameRelease the ninth sequential frame was refused with
 * "too many live frames (8)" because js_platform.c's record still held the
 * DOMParser wrapper, so its finalizer could never reach frame_on_docfree.
 * __frameRelease destroys the browsing context at the DOM removal/navigation
 * boundary and makes the slot reusable. The document wrapper may remain in
 * author code, but it is deliberately no longer a script host once its frame
 * is gone. js_frame_close_all remains the page-close backstop. */
#include "quickjs.h"
#include "js/dom/frame.h"
#include "dom/tree.h"
#ifdef JS_PROBE_TRACE
#include "js/runtime/probe.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The other half of js_domparser.c's exported door -- see that file's own
 * comment beside js_domparser_doc_of. Declared here rather than pulled from
 * a shared header because neither file has one and this is the only other
 * consumer; matches this tree's existing practice (js_worker_install is
 * forward-declared the same way in js_page.c rather than given a header). */
struct dom_doc *js_domparser_doc_of(JSValueConst v);
void js_domparser_set_script_sink(void (*fn)(struct node *));
void js_domparser_set_docfree_sink(void (*fn)(struct dom_doc *));
void js_domparser_offer_scripts(JSValueConst v);

#define JSF_MAX_FRAMES 8
/* Eight auxiliary realms per creator, with a separate process admission
 * ceiling. Parent and network children must not free or consume each other's
 * eight slots. Exhaustion remains a visible failure, never a parent alias. */
#define JSF_TOTAL_FRAMES 64

/* One timer this frame scheduled through its creator. `seq` names the
 * callback wrapper (it cannot know the id the creator hands back until after
 * it was built); `id` is what clearTimeout/cancelAnimationFrame take. */
enum { FT_TIMEOUT = 0, FT_INTERVAL = 1, FT_RAF = 2 };
struct frame_timer { unsigned seq; int id; int kind; };

struct jsframe {
    int used;
    /* Bumped every time the slot is vacated and NEVER reset: a native
     * callback built for one occupant carries (slot, gen) and must find a
     * later occupant of the same slot to be a different frame. */
    unsigned gen;
    struct dom_doc *doc;     /* key: the frame document this context belongs to */
    JSContext *fctx;         /* the frame's own context, on the PAGE's runtime */
    JSContext *owner;
    int execute_scripts;
    struct frame_timer *timers;
    int ntimers, ctimers;
    unsigned timer_seq;
};

static struct jsframe g_frames[JSF_TOTAL_FRAMES];

static struct jsframe *find_by_doc(struct dom_doc *doc)
{
    if (!doc) return 0;
    for (int i = 0; i < JSF_TOTAL_FRAMES; i++)
        if (g_frames[i].used && g_frames[i].doc == doc) return &g_frames[i];
    return 0;
}

/* The one liveness test every callback that outlives its call site uses. */
static struct jsframe *frame_live(int slot, unsigned gen)
{
    if (slot < 0 || slot >= JSF_TOTAL_FRAMES) return 0;
    struct jsframe *f = &g_frames[slot];
    return (f->used && f->gen == gen) ? f : 0;
}

static int data_int(JSContext *ctx, JSValueConst v)
{
    int32_t i = -1;
    if (JS_ToInt32(ctx, &i, v)) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
    return i;
}

/* Cancel, through the creator, every timer a destroyed frame still had
 * queued. Only called while the creator's page is live (__frameRelease):
 * page teardown disposes its own task queue, and calling into a page that
 * has already invalidated its runtime is not something to do for tidiness. */
static void frame_cancel_ids(JSContext *owner, const struct frame_timer *t, int n)
{
    if (!owner || n <= 0) return;
    JSValue og = JS_GetGlobalObject(owner);
    JSValue ct = JS_GetPropertyStr(owner, og, "clearTimeout");
    JSValue cr = JS_GetPropertyStr(owner, og, "cancelAnimationFrame");
    for (int i = 0; i < n; i++) {
        JSValueConst fn = t[i].kind == FT_RAF ? cr : ct;
        if (!JS_IsFunction(owner, fn)) continue;
        JSValue id = JS_NewInt32(owner, t[i].id);
        JSValue r = JS_Call(owner, fn, og, 1, &id);
        if (JS_IsException(r)) JS_FreeValue(owner, JS_GetException(owner));
        JS_FreeValue(owner, r);
    }
    JS_FreeValue(owner, ct);
    JS_FreeValue(owner, cr);
    JS_FreeValue(owner, og);
}

/* Vacate a slot. THE ORDER IS LOAD-BEARING, and it changed when the frame's
 * global started holding its own document (the `document` getter's data):
 * JS_FreeContext can now drop the LAST reference to the DOMParser wrapper,
 * whose finalizer calls straight back into frame_on_docfree -> here. The old
 * order (free the context, then clear the slot) would find the slot still
 * occupied on that re-entry and free the same context twice. So everything
 * the release needs is copied out and the slot is cleared FIRST; only then
 * does anything run that can re-enter this file. */
static void frame_release_slot(struct jsframe *f, int cancel)
{
    JSContext *fctx = f->fctx, *owner = f->owner;
    struct frame_timer *timers = f->timers;
    int n = f->ntimers;
    unsigned gen = f->gen;
    memset(f, 0, sizeof *f);
    f->gen = gen + 1;
    if (cancel) frame_cancel_ids(owner, timers, n);
    free(timers);
    if (fctx) JS_FreeContext(fctx);
}

/* js_domparser.c's docfree sink -- see that file's own comment on why this
 * exists at all (a real UAF-shaped bug, found by frame_test.c, not assumed):
 * `dom_doc` pointers are reused by the allocator once freed, so a stale
 * table entry keyed on one is a live-looking hit for a completely unrelated,
 * later document. Called with the doc about to be destroyed; frees this
 * frame's JSContext right here rather than waiting for js_frame_close_all,
 * because a page that opens and discards many frames one at a time (not
 * just at navigation) must not exhaust JSF_MAX_FRAMES on documents nothing
 * references anymore.
 * CORRECTION 2026-09-25: an ADOPTED document can no longer reach this path
 * while its frame lives -- the frame's Window holds it, exactly as a browser
 * Window holds its Document -- so for adopted frames the removal/navigation
 * release (__frameRelease) and page close are the lifetime, and this sink is
 * the pointer-reuse guard it was first written as. */
static int frame_release_doc(struct dom_doc *doc, int cancel)
{
    struct jsframe *f = find_by_doc(doc);
    if (!f) return 0;
    frame_release_slot(f, cancel);
    return 1;
}

static void frame_on_docfree(struct dom_doc *doc)
{
    (void)frame_release_doc(doc, 0);
}

/* ---- the frame's console: its own, prefixed, so a harness can tell which
 * realm printed a line. Unchanged from before the Window below existed. */
static JSValue frame_console(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    printf("[frame] ");
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        if (i) printf(" ");
        printf("%s", s);
        JS_FreeCString(ctx, s);
    }
    printf("\n");
    return JS_UNDEFINED;
}

/* ===================== THE FRAME'S GLOBAL IS A WINDOW ======================
 * The target is not a reading of the specification alone; it is what
 * Chromium answers, measured 2026-09-25 inside a same-origin about:blank
 * <iframe> built exactly the way the Cloudflare bootstrap builds one
 * (createElement, appendChild, contentDocument.createElement('script'),
 * innerHTML, head.appendChild), with the script inside reading its own
 * Window back to the parent:
 *
 *   window === globalThis === self === frames      true
 *   document === window.document                   true (the frame's own)
 *   parent === top                                 true (the page's window)
 *   frameElement.tagName                           "IFRAME"
 *   location.href / String(location)               "about:blank"
 *   location.protocol / pathname / host            "about:" / "blank" / ""
 *   location.origin                                "null"  (the URL's origin)
 *   self.origin                                    the CREATOR's origin
 *   document.URL                                   "about:blank"
 *   name / length / closed / opener                "" / 0 / false / null
 *   own descriptors: window, document, top         get, enumerable, NOT configurable
 *                    location                      get+set, enumerable, NOT configurable
 *                    self, frames, parent, origin  get+set, enumerable, configurable
 *                    frameElement, closed          get, enumerable, configurable
 *   setTimeout                                     "function setTimeout() { [native code] }"
 *   navigator / Event / setTimeout                 NOT the parent's objects
 *   contentWindow === window                       true
 *
 * Every row above is answered below as measured EXCEPT the last two, and
 * those are named rather than hidden:
 *   - `contentWindow === window` is FALSE here. js_platform.c's contentWindow
 *     is a Proxy over this global (a WindowProxy stand-in that survives
 *     navigation and applies the cross-origin gate at read time); a QuickJS
 *     global object cannot itself be that Proxy. Reads and writes through
 *     either reach the same Window; identity comparisons between the two do
 *     not. The same split makes `document.defaultView === window` false
 *     inside the frame (defaultView is the parent-side proxy, deliberately:
 *     `contentDocument.defaultView === contentWindow` is the WPT idiom that
 *     one was fixed for).
 *   - Platform services are the CREATOR's objects, under the rule stated at
 *     FRAME_SHARED_* below -- not per-realm copies. The functions this file
 *     installs are native functions of the frame's OWN realm (toString says
 *     [native code], and `frame.setTimeout !== parent.setTimeout`), but what
 *     they return is made by the creator.
 *
 * Nothing here names a site, a vendor or a script; every value is the one a
 * same-origin frame's Window has. */

enum { FG_CONST, FG_TOP, FG_ORIGIN, FG_CLOSED, FG_LIVE };

/* One getter body for every accessor this file defines.
 *   FG_CONST  data[0]                      a value fixed at adoption
 *   FG_TOP    data[0] = creator's global   its `top` if that is an object, else it
 *   FG_ORIGIN data[0] = creator's global   its origin (about:blank inherits it)
 *   FG_CLOSED data[0..1] = slot, gen       true once the frame is discarded
 *   FG_LIVE   data[0] = creator's global, data[1] = name   read at each access */
static JSValue frame_get(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val; (void)argc; (void)argv;
    switch (magic) {
    case FG_CONST:
        return JS_DupValue(ctx, data[0]);
    case FG_TOP: {
        JSValue t = JS_GetPropertyStr(ctx, data[0], "top");
        if (JS_IsException(t) || JS_IsObject(t)) return t;
        JS_FreeValue(ctx, t);
        return JS_DupValue(ctx, data[0]);
    }
    case FG_ORIGIN: {
        JSValue o = JS_GetPropertyStr(ctx, data[0], "origin");
        if (JS_IsException(o) || JS_IsString(o)) return o;
        JS_FreeValue(ctx, o);
        JSValue loc = JS_GetPropertyStr(ctx, data[0], "location");
        if (JS_IsException(loc)) return loc;
        if (JS_IsObject(loc)) {
            o = JS_GetPropertyStr(ctx, loc, "origin");
            JS_FreeValue(ctx, loc);
            if (JS_IsException(o) || JS_IsString(o)) return o;
            JS_FreeValue(ctx, o);
        } else JS_FreeValue(ctx, loc);
        return JS_NewString(ctx, "null");
    }
    case FG_CLOSED:
        return JS_NewBool(ctx, !frame_live(data_int(ctx, data[0]), (unsigned)data_int(ctx, data[1])));
    case FG_LIVE: {
        JSAtom a = JS_ValueToAtom(ctx, data[1]);
        if (a == JS_ATOM_NULL) return JS_EXCEPTION;
        JSValue v = JS_GetProperty(ctx, data[0], a);
        JS_FreeAtom(ctx, a);
        return v;
    }
    }
    return JS_UNDEFINED;
}

/* WebIDL [Replaceable]: an assignment defines an own data property that
 * shadows the accessor, so `self = 1` or `var parent` in a frame script
 * behaves as it does in a browser instead of throwing. data[0] = the name. */
static JSValue frame_replace(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic;
    if (!JS_IsObject(this_val)) return JS_UNDEFINED;
    JSAtom a = JS_ValueToAtom(ctx, data[0]);
    if (a == JS_ATOM_NULL) return JS_EXCEPTION;
    int r = JS_DefinePropertyValue(ctx, this_val, a,
                                   JS_DupValue(ctx, argc ? argv[0] : JS_UNDEFINED), JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* A native function of the FRAME's realm, named the way the platform names
 * it, so Function.prototype.toString answers `function NAME() { [native
 * code] }` -- a frame that inspects its own globals sees built-ins, because
 * that is what they are. */
static JSValue frame_fn(JSContext *fctx, JSCFunctionData *fn, const char *name,
                        int length, int magic, int n, JSValueConst *data)
{
    JSValue f = JS_NewCFunctionData(fctx, fn, length, magic, n, data);
    if (!JS_IsException(f))
        JS_DefinePropertyValueStr(fctx, f, "name", JS_NewString(fctx, name), JS_PROP_CONFIGURABLE);
    return f;
}

static void def_accessor(JSContext *fctx, JSValueConst obj, const char *name, int magic,
                         int n, JSValueConst *data, JSValue setter, int flags)
{
    char nm[64];
    snprintf(nm, sizeof nm, "get %s", name);
    JSValue get = frame_fn(fctx, frame_get, nm, 0, magic, n, data);
    JSAtom a = JS_NewAtom(fctx, name);
    if (JS_DefinePropertyGetSet(fctx, obj, a, get, setter, flags) < 0)
        JS_FreeValue(fctx, JS_GetException(fctx));
    JS_FreeAtom(fctx, a);
}

static JSValue replaceable_setter(JSContext *fctx, const char *name)
{
    char nm[64];
    snprintf(nm, sizeof nm, "set %s", name);
    JSValue key = JS_NewString(fctx, name);
    JSValue s = frame_fn(fctx, frame_replace, nm, 1, 0, 1, &key);
    JS_FreeValue(fctx, key);
    return s;
}

/* ---- Location ------------------------------------------------------------
 * Built from the URL js_platform.c's settle() committed the frame to
 * (about:blank, about:srcdoc, or the same-origin URL it fetched) -- a URL the
 * WHATWG parser already normalised, so splitting it into components is
 * slicing, not parsing. Every member is an OWN property of the instance and
 * none is configurable: Location is [LegacyUnforgeable], and Chromium's
 * descriptors say exactly that. */
enum { FL_HREF, FL_ASSIGN, FL_REPLACE, FL_RELOAD, FL_PART };

/* data: [navigate, href, member name]. Navigation is the CREATOR's door
 * (js_platform.c passes the same function its own contentWindow.location
 * setter uses), so a frame navigating itself and a parent navigating the
 * frame decide it in one place. Component setters (hash=, search=, ...)
 * would be same-document navigations this file has no model of; they refuse
 * by name rather than reload the frame, which would be a different thing. */
static JSValue frame_loc_nav(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val;
    const char *member = JS_ToCString(ctx, data[2]);
    if (magic == FL_PART) {
        printf("[frame] refused: location.%s= inside a frame is not implemented -- "
               "only href/assign/replace/reload navigate (see js_frame.c)\n", member ? member : "?");
        if (member) JS_FreeCString(ctx, member);
        return JS_UNDEFINED;
    }
    if (!JS_IsFunction(ctx, data[0])) {
        printf("[frame] refused: location.%s -- this frame's creator supplied no navigation door, "
               "so the frame stays where it is\n", member ? member : "?");
        if (member) JS_FreeCString(ctx, member);
        return JS_UNDEFINED;
    }
    if (member) JS_FreeCString(ctx, member);
    JSValue target = magic == FL_RELOAD ? JS_DupValue(ctx, data[1])
                                        : JS_DupValue(ctx, argc ? argv[0] : JS_UNDEFINED);
    JSValue r = JS_Call(ctx, data[0], JS_UNDEFINED, 1, &target);
    JS_FreeValue(ctx, target);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

static JSValue frame_illegal_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static int is_scheme_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '-' || c == '.';
}

static void set_tostringtag(JSContext *fctx, JSValueConst obj, const char *tag)
{
    JSValue g = JS_GetGlobalObject(fctx);
    JSValue sym = JS_GetPropertyStr(fctx, g, "Symbol");
    JSValue tt = JS_IsObject(sym) ? JS_GetPropertyStr(fctx, sym, "toStringTag") : JS_UNDEFINED;
    JSAtom a = JS_IsSymbol(tt) ? JS_ValueToAtom(fctx, tt) : JS_ATOM_NULL;
    if (a != JS_ATOM_NULL) {
        JS_DefinePropertyValue(fctx, obj, a, JS_NewString(fctx, tag), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(fctx, a);
    }
    JS_FreeValue(fctx, tt);
    JS_FreeValue(fctx, sym);
    JS_FreeValue(fctx, g);
}

static JSValue frame_make_location(JSContext *fctx, JSValueConst g, const char *u, JSValueConst nav)
{
    int n = (int)strlen(u), i = 0;
    while (i < n && is_scheme_char(u[i])) i++;
    int scheme = i > 0 && i < n && u[i] == ':' &&
                 ((u[0] >= 'a' && u[0] <= 'z') || (u[0] >= 'A' && u[0] <= 'Z'));
    int pe = scheme ? i + 1 : 0;                   /* protocol = [0, pe), includes ':' */
    int hb = pe, he = pe, nb = pe, ne = pe, ob = pe, oe = pe, p = pe;
    if (scheme && p + 1 < n && u[p] == '/' && u[p + 1] == '/') {
        int ab = p + 2, ae = ab;
        while (ae < n && u[ae] != '/' && u[ae] != '?' && u[ae] != '#') ae++;
        hb = ab;
        for (int k = ab; k < ae; k++) if (u[k] == '@') hb = k + 1;   /* userinfo is not host */
        he = ae; nb = hb; ne = he; ob = oe = he;
        if (hb < he && u[hb] == '[') {                     /* [v6]:port */
            int k = hb;
            while (k < he && u[k] != ']') k++;
            ne = k < he ? k + 1 : he;
            if (ne < he && u[ne] == ':') { ob = ne + 1; oe = he; }
        } else {
            for (int k = he - 1; k >= hb; k--)
                if (u[k] == ':') { ne = k; ob = k + 1; oe = he; break; }
        }
        p = ae;
    }
    int qb = p;
    while (qb < n && u[qb] != '?' && u[qb] != '#') qb++;          /* pathname = [p, qb) */
    int hs = qb;
    while (hs < n && u[hs] != '#') hs++;                           /* search = [qb, hs) */
    /* "?" alone and "#" alone read back as "" -- the Location getters'
     * rule, and what Chromium prints. */
    int sb = hs - qb > 1 ? qb : hs, hhb = n - hs > 1 ? hs : n;

    char origin[512];
    static const char *const special[] = { "http:", "https:", "ws:", "wss:", "ftp:" };
    int sp = 0;
    for (unsigned k = 0; k < sizeof special / sizeof special[0]; k++)
        if ((int)strlen(special[k]) == pe && !strncmp(u, special[k], (size_t)pe)) sp = 1;
    if (sp && hb < he && pe + 2 + (he - hb) < (int)sizeof origin)
        snprintf(origin, sizeof origin, "%.*s//%.*s", pe, u, he - hb, u + hb);
    else
        snprintf(origin, sizeof origin, "null");

    JSValue proto = JS_NewObject(fctx);
    JSValue ctor = JS_NewCFunction2(fctx, frame_illegal_ctor, "Location", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(fctx, ctor, proto);
    set_tostringtag(fctx, proto, "Location");
    JS_DefinePropertyValueStr(fctx, g, "Location", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JSValue loc = JS_NewObjectProto(fctx, proto);
    JS_FreeValue(fctx, proto);

    JSValue href = JS_NewString(fctx, u);
    struct { const char *name; int b, e; int kind; } parts[] = {
        { "href",     0,   n,  FL_HREF },
        { "origin",   0,   0,  -1 },
        { "protocol", 0,   pe, FL_PART },
        { "host",     hb,  he, FL_PART },
        { "hostname", nb,  ne, FL_PART },
        { "port",     ob,  oe, FL_PART },
        { "pathname", p,   qb, FL_PART },
        { "search",   sb,  hs, FL_PART },
        { "hash",     hhb, n,  FL_PART },
    };
    for (unsigned k = 0; k < sizeof parts / sizeof parts[0]; k++) {
        JSValue v = parts[k].kind < 0 ? JS_NewString(fctx, origin)
                                      : JS_NewStringLen(fctx, u + parts[k].b,
                                            (size_t)(parts[k].e > parts[k].b ? parts[k].e - parts[k].b : 0));
        JSValue setter = JS_UNDEFINED;
        if (parts[k].kind >= 0) {
            char nm[48];
            snprintf(nm, sizeof nm, "set %s", parts[k].name);
            JSValue d[3] = { JS_DupValue(fctx, nav), JS_DupValue(fctx, href), JS_NewString(fctx, parts[k].name) };
            setter = frame_fn(fctx, frame_loc_nav, nm, 1, parts[k].kind, 3, d);
            for (int j = 0; j < 3; j++) JS_FreeValue(fctx, d[j]);
        }
        def_accessor(fctx, loc, parts[k].name, FG_CONST, 1, &v, setter, JS_PROP_ENUMERABLE);
        JS_FreeValue(fctx, v);
    }
    static const struct { const char *name; int kind; } ops[] = {
        { "assign", FL_ASSIGN }, { "replace", FL_REPLACE }, { "reload", FL_RELOAD },
    };
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        JSValue d[3] = { JS_DupValue(fctx, nav), JS_DupValue(fctx, href), JS_NewString(fctx, ops[k].name) };
        JSValue f = frame_fn(fctx, frame_loc_nav, ops[k].name, ops[k].kind == FL_RELOAD ? 0 : 1,
                             ops[k].kind, 3, d);
        for (int j = 0; j < 3; j++) JS_FreeValue(fctx, d[j]);
        JS_DefinePropertyValueStr(fctx, loc, ops[k].name, f, JS_PROP_ENUMERABLE);
    }
    JS_DefinePropertyValueStr(fctx, loc, "toString",
                              frame_fn(fctx, frame_get, "toString", 0, FG_CONST, 1, &href),
                              JS_PROP_ENUMERABLE);
    JS_FreeValue(fctx, href);
    return loc;
}

/* ---- timers --------------------------------------------------------------
 * The callback the creator's scheduler actually holds. data: [handler, slot,
 * gen, seq, the frame's global]; magic: kind. A discarded frame's callback
 * returns without running anything (HTML: a callback whose global's document
 * is not fully active does not run), and `this` is the frame's Window, not
 * the creator's.
 *
 * TWO MECHANISMS, and each is gated on its own (tests/frame_window.mk):
 * __frameRelease CANCELS the frame's queued timers through the creator, so a
 * discarded frame does not keep occupying the page's bounded task queue; and
 * the FENCE below refuses to run a callback whose frame is gone, which is what
 * holds on every path where cancelling cannot (page teardown, or a creator
 * whose clearTimeout an author script replaced). The handler's `this` is
 * carried in the data rather than read through the slot, so the fence is the
 * only line that depends on the slot being live. */
static int timer_find(struct jsframe *f, int by_seq, int key, int raf)
{
    for (int i = 0; i < f->ntimers; i++) {
        if (by_seq ? f->timers[i].seq != (unsigned)key : f->timers[i].id != key) continue;
        if (!by_seq && (f->timers[i].kind == FT_RAF) != raf) continue;
        return i;
    }
    return -1;
}
static void timer_drop(struct jsframe *f, int i)
{
    f->timers[i] = f->timers[--f->ntimers];
}
static void timer_push(struct jsframe *f, unsigned seq, int id, int kind)
{
    if (f->ntimers == f->ctimers) {
        int nc = f->ctimers ? f->ctimers * 2 : 8;
        struct frame_timer *nt = realloc(f->timers, (size_t)nc * sizeof *nt);
        if (!nt) return;   /* the wrapper's liveness check still fences it */
        f->timers = nt;
        f->ctimers = nc;
    }
    f->timers[f->ntimers].seq = seq;
    f->timers[f->ntimers].id = id;
    f->timers[f->ntimers].kind = kind;
    f->ntimers++;
}

static JSValue frame_timer_fire(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val;
    struct jsframe *f = frame_live(data_int(ctx, data[1]), (unsigned)data_int(ctx, data[2]));
    if (!f) return JS_UNDEFINED;   /* FENCE: a discarded frame's callback runs nothing */
    if (f && magic != FT_INTERVAL) {
        int i = timer_find(f, 1, data_int(ctx, data[3]), 0);
        if (i >= 0) timer_drop(f, i);
    }
    return JS_Call(ctx, data[0], data[4], argc, argv);
}

static const char *timer_creator_name(int kind)
{
    return kind == FT_RAF ? "requestAnimationFrame" : kind == FT_INTERVAL ? "setInterval" : "setTimeout";
}

/* setTimeout / setInterval / requestAnimationFrame. data: [slot, gen]. */
static JSValue frame_set_timer(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val;
    int slot = data_int(ctx, data[0]);
    unsigned gen = (unsigned)data_int(ctx, data[1]);
    struct jsframe *f = frame_live(slot, gen);
    if (!f) return JS_NewInt32(ctx, 0);
    JSValue og = JS_GetGlobalObject(f->owner);
    JSValue pfn = JS_GetPropertyStr(f->owner, og, timer_creator_name(magic));
    if (!JS_IsFunction(ctx, pfn)) {
        JS_FreeValue(ctx, pfn); JS_FreeValue(ctx, og);
        return JS_NewInt32(ctx, 0);
    }
    /* Not a function: hand it to the creator untouched, so the page's own
     * rule for a string handler (js_page.c refuses it and returns 0) is the
     * one answer, not a second one written here. */
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        JSValue r = JS_Call(ctx, pfn, og, argc, argv);
        JS_FreeValue(ctx, pfn); JS_FreeValue(ctx, og);
        return r;
    }
    unsigned seq = ++f->timer_seq;
    JSValue d[5] = { JS_DupValue(ctx, argv[0]), JS_NewInt32(ctx, slot),
                     JS_NewInt32(ctx, (int)gen), JS_NewInt32(ctx, (int)seq),
                     JS_GetGlobalObject(f->fctx) };
    JSValue w = JS_NewCFunctionData(f->fctx, frame_timer_fire, 0, magic, 5, d);
    for (int i = 0; i < 5; i++) JS_FreeValue(ctx, d[i]);
    if (JS_IsException(w)) { JS_FreeValue(ctx, pfn); JS_FreeValue(ctx, og); return w; }
    int na = magic == FT_RAF ? 1 : (argc > 2 ? argc : 2);
    JSValue stackargs[8], *a = na <= 8 ? stackargs : malloc((size_t)na * sizeof *a);
    if (!a) { JS_FreeValue(ctx, w); JS_FreeValue(ctx, pfn); JS_FreeValue(ctx, og); return JS_ThrowOutOfMemory(ctx); }
    a[0] = w;
    if (magic != FT_RAF) {
        a[1] = argc > 1 ? argv[1] : JS_UNDEFINED;
        for (int i = 2; i < na; i++) a[i] = argv[i];
    }
    JSValue r = JS_Call(ctx, pfn, og, na, a);
    if (a != stackargs) free(a);
    JS_FreeValue(ctx, w); JS_FreeValue(ctx, pfn); JS_FreeValue(ctx, og);
    if (JS_IsException(r)) return r;
    int id = data_int(ctx, r);
    JS_FreeValue(ctx, r);
    /* Re-check: the creator's scheduler is author-replaceable JS and may
     * have removed this very frame before returning. */
    f = frame_live(slot, gen);
    if (f && id > 0) timer_push(f, seq, id, magic);
    return JS_NewInt32(ctx, id);
}

/* clearTimeout / clearInterval (magic FT_TIMEOUT) and cancelAnimationFrame
 * (FT_RAF). Only ids THIS frame was handed are forwarded: timer ids are per
 * Window, so a frame must not be able to cancel its creator's timers by
 * guessing a number -- which one shared namespace would otherwise allow. */
static JSValue frame_clear_timer(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val;
    struct jsframe *f = frame_live(data_int(ctx, data[0]), (unsigned)data_int(ctx, data[1]));
    if (!f || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    int i = timer_find(f, 0, id, magic == FT_RAF);
    if (i < 0) return JS_UNDEFINED;
    struct frame_timer t = f->timers[i];
    timer_drop(f, i);
    frame_cancel_ids(f->owner, &t, 1);
    return JS_UNDEFINED;
}

/* queueMicrotask needs nothing from the creator: QuickJS's job queue is
 * per-runtime and the page's drain already runs it (header, "WHAT THIS BUYS
 * FOR FREE"). The job carries (slot, gen) so a discarded frame's microtask
 * does not run. */
static JSValue frame_microtask_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc;
    if (!frame_live(data_int(ctx, argv[1]), (unsigned)data_int(ctx, argv[2]))) return JS_UNDEFINED;
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}
static JSValue frame_queue_microtask(JSContext *ctx, JSValueConst this_val, int argc,
                                     JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val; (void)magic;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'queueMicrotask' on 'Window': "
                                      "parameter 1 is not of type 'Function'.");
    struct jsframe *f = frame_live(data_int(ctx, data[0]), (unsigned)data_int(ctx, data[1]));
    if (!f) return JS_UNDEFINED;
    JSValue a[3] = { argv[0], data[0], data[1] };
    if (JS_EnqueueJob(f->fctx, frame_microtask_job, 3, a) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* A frame-realm native function that calls the creator's. data: [fn, this]. */
static JSValue frame_forward(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv, int magic, JSValue *data)
{
    (void)this_val; (void)magic;
    return JS_Call(ctx, data[0], data[1], argc, argv);
}

/* ---- WHAT THE FRAME SHARES WITH ITS CREATOR, AND THE RULE -----------------
 * A browser gives every Window its own copy of every platform object. This
 * engine has one platform, installed into the creator's realm; building a
 * second is the js_dom.c de-singleton project the header names. The rule
 * for what a same-origin frame may nevertheless reach of the creator's:
 * a service whose answer does not depend on WHICH window asks. navigator,
 * screen, storage, crypto, fetch/XHR (whose relative URLs resolve against
 * the creator's base URL -- which for about:blank and srcdoc frames is the
 * frame's own, inherited), the event and error constructors.
 *
 * What fails the rule and is therefore ABSENT, not shared: history (a frame
 * pushState would rewrite the top-level URL), visualViewport/innerWidth/
 * matchMedia (the frame's viewport is its <iframe>'s box, not the page's),
 * getComputedStyle (takes the creator's elements), customElements (the
 * registry is per-window), Image/Audio/Option (create the creator's
 * elements), and the DOM interface objects (header, cut item 4). An absent
 * name is a ReferenceError a script can feature-test; a shared object that
 * answers about the wrong window is a lie it cannot.
 *
 * LIVE accessors are read from the creator at each access, INTERFACE
 * objects are captured at adoption. Both are the creator's objects, not
 * copies: `frame.navigator === parent.navigator` here and not in Chromium,
 * and a page that replaced one of these before creating the frame is seen
 * replaced inside it. Stated, not hidden. */
static const char *const FRAME_SHARED_LIVE[] = {
    "navigator", "screen", "performance", "crypto", "localStorage", "sessionStorage", "indexedDB",
};
static const struct { const char *name; int length; } FRAME_SHARED_CALL[] = {
    { "atob", 1 }, { "btoa", 1 }, { "fetch", 1 },
};
static const char *const FRAME_SHARED_VALUE[] = {
    "XMLHttpRequest", "Event", "CustomEvent", "MessageEvent", "ErrorEvent", "EventTarget",
    "DOMException", "URL", "URLSearchParams", "TextEncoder", "TextDecoder", "Blob", "File",
    "FileReader", "FormData", "Headers", "Request", "Response", "AbortController", "AbortSignal",
    "DOMParser", "Intl", "WebAssembly",
};

static int own_absent(JSContext *fctx, JSValueConst g, const char *name)
{
    JSAtom a = JS_NewAtom(fctx, name);
    int has = JS_HasProperty(fctx, g, a);
    JS_FreeAtom(fctx, a);
    return has == 0;
}

/* Build the frame's Window. `docv` is the frame's document (the exact object
 * the creator reads as contentDocument), `el` its <iframe> (or null when a
 * caller has none), `url` what the frame is showing, `nav` the creator's
 * navigation door for this frame (or undefined). */
static void frame_install_window(struct jsframe *f, JSContext *pctx, JSValueConst docv,
                                 JSValueConst el, const char *url, JSValueConst nav)
{
    JSContext *fctx = f->fctx;
    const int E = JS_PROP_ENUMERABLE, EC = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    JSValue g = JS_GetGlobalObject(fctx);
    JSValue pg = JS_GetGlobalObject(pctx);
    JSValue live[2] = { JS_NewInt32(fctx, (int)(f - g_frames)), JS_NewInt32(fctx, (int)f->gen) };
    JSValue fe = JS_IsObject(el) ? JS_DupValue(fctx, el) : JS_NULL;

    /* Identity and the browsing-context tree. */
    def_accessor(fctx, g, "window", FG_CONST, 1, &g, JS_UNDEFINED, E);
    def_accessor(fctx, g, "self", FG_CONST, 1, &g, replaceable_setter(fctx, "self"), EC);
    def_accessor(fctx, g, "frames", FG_CONST, 1, &g, replaceable_setter(fctx, "frames"), EC);
    def_accessor(fctx, g, "document", FG_CONST, 1, &docv, JS_UNDEFINED, E);
    def_accessor(fctx, g, "parent", FG_CONST, 1, &pg, replaceable_setter(fctx, "parent"), EC);
    def_accessor(fctx, g, "top", FG_TOP, 1, &pg, JS_UNDEFINED, E);
    def_accessor(fctx, g, "frameElement", FG_CONST, 1, &fe, JS_UNDEFINED, EC);
    def_accessor(fctx, g, "origin", FG_ORIGIN, 1, &pg, replaceable_setter(fctx, "origin"), EC);
    def_accessor(fctx, g, "closed", FG_CLOSED, 2, live, JS_UNDEFINED, EC);
    JS_DefinePropertyValueStr(fctx, g, "opener", JS_NULL, JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(fctx, g, "length", JS_NewInt32(fctx, 0), JS_PROP_C_W_E);
    /* The browsing context name comes from the <iframe>'s `name` attribute in
     * a browser. Reading it here would call author-replaceable DOM methods in
     * the middle of adoption; unnamed is what every frame this path has been
     * measured against is, and the gap is written down here instead. */
    JS_DefinePropertyValueStr(fctx, g, "name", JS_NewString(fctx, ""), JS_PROP_C_W_E);
    JSValue sec = JS_GetPropertyStr(pctx, pg, "isSecureContext");
    if (JS_IsBool(sec)) JS_DefinePropertyValueStr(fctx, g, "isSecureContext", sec, EC);
    else JS_FreeValue(pctx, sec);
    set_tostringtag(fctx, g, "Window");

    /* Location, on the Window AND on the document -- one object, two doors
     * (Chromium: document.location === location). */
    JSValue loc = frame_make_location(fctx, g, url, nav);
    {
        JSValue href = JS_GetPropertyStr(fctx, loc, "href");
        JSValue d[3] = { JS_DupValue(fctx, nav), JS_DupValue(fctx, href), JS_NewString(fctx, "href") };
        def_accessor(fctx, g, "location", FG_CONST, 1, &loc,
                     frame_fn(fctx, frame_loc_nav, "set location", 1, FL_HREF, 3, d), E);
        /* On the document these are configurable: a document outlives its
         * frame in author code, and nothing may make a later redefinition
         * throw. The values are the ones a frame document reports. */
        if (JS_IsObject(docv)) {
            def_accessor(fctx, docv, "location", FG_CONST, 1, &loc,
                         frame_fn(fctx, frame_loc_nav, "set location", 1, FL_HREF, 3, d), EC);
            def_accessor(fctx, docv, "URL", FG_CONST, 1, &href, JS_UNDEFINED, EC);
            def_accessor(fctx, docv, "documentURI", FG_CONST, 1, &href, JS_UNDEFINED, EC);
        }
        for (int j = 0; j < 3; j++) JS_FreeValue(fctx, d[j]);
        JS_FreeValue(fctx, href);
    }
    JS_FreeValue(fctx, loc);

    JSValue con = JS_NewObject(fctx);
    JSValue logf = JS_NewCFunction(fctx, frame_console, "log", 1);
    JS_SetPropertyStr(fctx, con, "log", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "info", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "debug", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "warn", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "error", logf);
    JS_SetPropertyStr(fctx, g, "console", con);

    /* Timers: installed only when the creator has the scheduler to forward
     * to, so a creator without one (a bare host context) leaves the name
     * absent -- the correct feature-detect answer -- rather than present and
     * silently inert. */
    static const struct { const char *name; int kind; int set; int length; } timers[] = {
        { "setTimeout", FT_TIMEOUT, 1, 1 },   { "setInterval", FT_INTERVAL, 1, 1 },
        { "clearTimeout", FT_TIMEOUT, 0, 0 }, { "clearInterval", FT_TIMEOUT, 0, 0 },
        { "requestAnimationFrame", FT_RAF, 1, 1 }, { "cancelAnimationFrame", FT_RAF, 0, 1 },
    };
    for (unsigned k = 0; k < sizeof timers / sizeof timers[0]; k++) {
        const char *need = timers[k].set ? timer_creator_name(timers[k].kind)
                         : timers[k].kind == FT_RAF ? "cancelAnimationFrame" : "clearTimeout";
        JSValue pfn = JS_GetPropertyStr(pctx, pg, need);
        int have = JS_IsFunction(pctx, pfn);
        JS_FreeValue(pctx, pfn);
        if (!have) continue;
        JS_DefinePropertyValueStr(fctx, g, timers[k].name,
            frame_fn(fctx, timers[k].set ? frame_set_timer : frame_clear_timer,
                     timers[k].name, timers[k].length, timers[k].kind, 2, live),
            JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(fctx, g, "queueMicrotask",
        frame_fn(fctx, frame_queue_microtask, "queueMicrotask", 1, 0, 2, live), JS_PROP_C_W_E);

    /* The Window is an EventTarget. The creator's constructible EventTarget
     * (js_events.c) keeps its listeners in a WeakMap keyed on `this`, so its
     * methods called with THIS global as `this` make the frame's Window a
     * target of its own: event.target is the frame's window, and nothing is
     * registered on the creator's. What still never arrives is a UA event
     * (load, message, resize) -- nothing dispatches one to a frame Window. */
    JSValue etc = JS_GetPropertyStr(pctx, pg, "EventTarget");
    JSValue etp = JS_IsFunction(pctx, etc) ? JS_GetPropertyStr(pctx, etc, "prototype") : JS_UNDEFINED;
    static const struct { const char *name; int length; } etm[] = {
        { "addEventListener", 2 }, { "removeEventListener", 2 }, { "dispatchEvent", 1 },
    };
    for (unsigned k = 0; JS_IsObject(etp) && k < sizeof etm / sizeof etm[0]; k++) {
        JSValue m = JS_GetPropertyStr(pctx, etp, etm[k].name);
        if (JS_IsFunction(pctx, m)) {
            JSValue d[2] = { m, g };
            JS_DefinePropertyValueStr(fctx, g, etm[k].name,
                frame_fn(fctx, frame_forward, etm[k].name, etm[k].length, 0, 2, d), JS_PROP_C_W_E);
        }
        JS_FreeValue(pctx, m);
    }
    JS_FreeValue(pctx, etp);
    JS_FreeValue(pctx, etc);

    for (unsigned k = 0; k < sizeof FRAME_SHARED_LIVE / sizeof FRAME_SHARED_LIVE[0]; k++) {
        const char *nm = FRAME_SHARED_LIVE[k];
        JSValue v = JS_GetPropertyStr(pctx, pg, nm);
        int have = JS_IsObject(v);
        JS_FreeValue(pctx, v);
        if (!have || !own_absent(fctx, g, nm)) continue;
        JSValue d[2] = { pg, JS_NewString(fctx, nm) };
        def_accessor(fctx, g, nm, FG_LIVE, 2, d, JS_UNDEFINED, EC);
        JS_FreeValue(fctx, d[1]);
    }
    for (unsigned k = 0; k < sizeof FRAME_SHARED_CALL / sizeof FRAME_SHARED_CALL[0]; k++) {
        const char *nm = FRAME_SHARED_CALL[k].name;
        JSValue fn = JS_GetPropertyStr(pctx, pg, nm);
        if (JS_IsFunction(pctx, fn) && own_absent(fctx, g, nm)) {
            JSValue d[2] = { fn, pg };
            JS_DefinePropertyValueStr(fctx, g, nm,
                frame_fn(fctx, frame_forward, nm, FRAME_SHARED_CALL[k].length, 0, 2, d), JS_PROP_C_W_E);
        }
        JS_FreeValue(pctx, fn);
    }
    for (unsigned k = 0; k < sizeof FRAME_SHARED_VALUE / sizeof FRAME_SHARED_VALUE[0]; k++) {
        const char *nm = FRAME_SHARED_VALUE[k];
        JSValue v = JS_GetPropertyStr(pctx, pg, nm);
        if (JS_IsObject(v) && own_absent(fctx, g, nm))
            JS_DefinePropertyValueStr(fctx, g, nm, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        else JS_FreeValue(pctx, v);
    }

    JS_FreeValue(fctx, fe);
    JS_FreeValue(fctx, live[0]);
    JS_FreeValue(fctx, live[1]);
    JS_FreeValue(pctx, pg);
    JS_FreeValue(fctx, g);
}

/* ---- gathering an inline <script>'s text, directly off `struct node` ----
 * The same shape js_domparser.c's dp_gather_text uses (that one is `static`
 * to its own file and there is no shared header to pull it from -- see the
 * file-header note on why js_domparser_doc_of/set_script_sink are declared
 * by hand rather than given one). N_TEXT/N_COMMENT children only; a
 * <script> built via createElement + appendChild(createTextNode(...)) or via
 * innerHTML= (which goes through the same fragment-import path) both leave
 * their content as ordinary text-node children, so one small walk covers
 * both doors the specimen and any script like it can use. */
struct fg_buf { char *p; size_t len, cap; };
static int fg_push(struct fg_buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n; b->p[b->len] = 0;
    return 1;
}
static void fg_gather(struct node *root, struct fg_buf *b)
{
    for (struct node *c = root->first_child; c; c = c->next) {
        if (c->type == N_TEXT && c->text) fg_push(b, c->text, (size_t)c->textlen);
    }
}

/* Called by js_domparser.c for EVERY <script> node it offers (any inserted
 * one, in any dp document -- see that file's own comment on the sink). The
 * no-op case -- a document nobody ever called __frameAdopt on, i.e. a plain
 * `new DOMParser().parseFromString(...)` result, or an iframe's document
 * before contentDocument/contentWindow was ever read -- is the common one
 * and costs one linear scan of an 8-entry table. */
void js_frame_offer_script(struct node *n)
{
    if (!n || n->type != N_ELEM) return;
    struct jsframe *f = find_by_doc(n->doc);
    if (!f) return;   /* not a live-adopted frame document -- data, as before.
                       * Deliberately NOT marked done: a document adopted later
                       * must still get its scripts, and this branch made no
                       * decision about the node, it declined to look at it. */
    if (!f->execute_scripts) return;

    /* Every path from here down has DECIDED about this node -- run, or refused
     * by name -- so it is marked before any of them, and the run-once flag
     * (dom.h's NF_SCRIPT_DONE, the same one js_dom.c uses) is what stops a
     * second `textContent =` or `innerHTML =` on an already-handled <script>
     * from executing it again. Marked FIRST rather than after JS_Eval on
     * purpose: the script being evaluated can mutate its own node, and a flag
     * set afterwards would be set on a node the eval may have re-offered,
     * which is a re-entrancy loop rather than a run-once guard. */
    dom_script_mark_done(n);

    const char *type = dom_attr(n, "type");
    if (type && type[0] &&
        strcmp(type, "text/javascript") != 0 &&
        strcmp(type, "application/javascript") != 0 &&
        strcmp(type, "application/ecmascript") != 0) {
        printf("[frame] refused: <script type=\"%s\"> is not a classic script -- not executed\n", type);
        return;
    }
    const char *src = dom_attr(n, "src");
    if (src && src[0]) {
        printf("[frame] refused: <script src=\"%s\"> -- external frame scripts are not fetched (see js_frame.c)\n", src);
        return;
    }

    struct fg_buf b = { 0, 0, 0 };
    fg_gather(n, &b);
    if (b.len == 0) { free(b.p); return; }   /* an empty <script> is not an error */

#ifdef JS_FRAME_NO_EXEC
    /* THE NEGATIVE CONTROL for test-frame-negctl: every DOM method above this
     * point (createElement, getElementsByTagName, appendChild, innerHTML) is
     * still fully present and correct -- only the actual JS_Eval is compiled
     * out, reproducing EXACTLY the failure this file exists to prevent: "a
     * contentWindow that exists and does nothing... the page sees the frame,
     * waits for its load event, and hangs" (js_platform.c:2570, quoted in the
     * brief this file was built against). test-frame-negctl must watch the
     * specimen's own assertion (window.__ran === 1, observed via
     * console.log) FAIL under this build and pass under the plain one. */
    free(b.p);
    printf("[frame] JS_FRAME_NO_EXEC: script received (%d bytes) and silently NOT run\n", (int)b.len);
    return;
#else
    /* The script may DESTROY ITS OWN FRAME before JS_Eval returns -- it can
     * reach `frameElement` and `parent` now, so `frameElement.remove()` or a
     * location assignment is one statement away, and either one releases this
     * slot (memset) from inside the eval. Everything after the eval therefore
     * uses a context reference taken before it, never `f`. Unreachable while
     * the frame's global had no way to name its creator; reachable from the
     * first commit that gave it one, which is this one. */
    JSContext *fctx = JS_DupContext(f->fctx);
    JSValue r = JS_Eval(fctx, b.p, b.len, "<frame script>", JS_EVAL_TYPE_GLOBAL);
    free(b.p);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(fctx);
        const char *m = JS_ToCString(fctx, e);
        printf("[frame] uncaught: %s\n", m ? m : "?");
        if (m) JS_FreeCString(fctx, m);
        JS_FreeValue(fctx, e);
    }
    JS_FreeValue(fctx, r);
    JS_FreeContext(fctx);
    /* No drain call here on purpose -- see the file header: QuickJS's job
     * queue is per-RUNTIME, and js_page.c's own microtask pump already
     * drains whatever this eval just enqueued the next time anything asks
     * the page to drain (js_page_pump / after the next js_page_eval or
     * js_page_run_due), same as any other synchronous JS entry on this
     * runtime. Draining here too would not be wrong, only redundant. */
#endif
}

/* __frameAdopt(doc): give a frame's already-parsed document a JSContext of
 * its own, so js_frame_offer_script has somewhere to run its <script>s.
 * Called from js_platform.c's iframe `settle()` once a real document
 * exists (about:blank, srcdoc, or a same-origin fetch that was never
 * blocked) -- never for a `blocked` result, which js_platform.c's own gate
 * already refused before this would ever see it. Idempotent: re-adopting a
 * document already in the table (a re-navigated iframe that kept the same
 * arena -- cannot happen today since every settle() gets a fresh DOMParser
 * document, but idempotence costs one comparison and is cheaper than a
 * second invariant to maintain) reuses the existing context rather than
 * leaking a second one.
 *
 * __frameAdopt(doc, frameElement, url, navigate) since 2026-09-25: the three
 * trailing arguments are what the frame's Window needs from the creator that
 * the document cannot say -- which element embeds it, which URL it is
 * showing (a DOMParser document has none of its own), and the creator's
 * navigation door for this frame. All three are optional: a missing element
 * is `frameElement === null`, a missing URL is about:blank (the only URL an
 * element-less caller can mean), a missing door makes location assignment a
 * named refusal. __frameGlobal takes the same arguments, so whichever of the
 * two doors creates the realm builds the same Window. */
static struct jsframe *frame_adopt(JSContext *pctx,JSValueConst value,int execute_scripts,
                                   JSValueConst el,const char *url,JSValueConst nav)
{
    struct dom_doc *doc = js_domparser_doc_of(value);
    if (!doc) return NULL;
    struct jsframe *existing=find_by_doc(doc);
    if(existing)return existing->owner==pctx?existing:NULL;

    int slot = -1,owned=0;
    for(int i=0;i<JSF_TOTAL_FRAMES;i++){
        if(!g_frames[i].used){if(slot<0)slot=i;}
        else if(g_frames[i].owner==pctx)owned++;
    }
    if (slot < 0 || owned>=JSF_MAX_FRAMES) {
        printf("[frame] refused: too many live frames (%d) -- this document's <script>s will not run\n",
               JSF_MAX_FRAMES);
        return NULL;
    }

    JSContext *fctx = JS_NewContext(JS_GetRuntime(pctx));
    if (!fctx) {
        printf("[frame] refused: could not create a JSContext for this frame (out of memory)\n");
        return NULL;
    }
    JS_SetStringCodeGenerationAllowed(fctx,JS_GetStringCodeGenerationAllowed(pctx));

    /* The slot goes live BEFORE the Window is built: its timers and
     * `closed` carry (slot, gen) and test liveness against this entry. */
    g_frames[slot].used = 1;
    g_frames[slot].doc = doc;
    g_frames[slot].fctx = fctx;
    g_frames[slot].owner = pctx;
    g_frames[slot].execute_scripts = execute_scripts;
    frame_install_window(&g_frames[slot], pctx, value, el, url && url[0] ? url : "about:blank", nav);
#ifdef JS_PROBE_TRACE
    /* The probe build names what a frame script reached for and did not get,
     * exactly as it does for the page -- the realm this file builds is where
     * the measured Cloudflare bootstrap runs, so it is where the next miss is. */
    js_probe_install(fctx, "frame.");
#endif

    /* Run whatever <script>s were ALREADY IN THIS DOCUMENT'S MARKUP when it
     * was parsed. The insertion sink above cannot see them -- nobody ever
     * inserts a node the parser built -- so without this line a frame whose
     * script came from `srcdoc="..."` or from a same-origin src's own response
     * body would be exactly the "the frame exists and nothing happens" shape
     * this file exists to prevent, while the specimen's create-then-insert
     * shape worked. Ordering is load-bearing and this is the only correct
     * place for it: the slot must be live first (js_frame_offer_script keys on
     * it and would refuse a document not yet in the table), and it must happen
     * before __frameAdopt returns, because js_platform.c's settle() fires the
     * element's `load` event immediately afterwards and a page is entitled to
     * observe the frame's own scripts' effects from its onload handler. */
    if(execute_scripts)js_domparser_offer_scripts(value);
    /* A markup script may have removed its own frame (see the eval above);
     * report what is live now, not what the slot held before it ran. */
    return find_by_doc(doc);
}

/* The optional trailing arguments of __frameAdopt/__frameGlobal. */
static struct jsframe *frame_adopt_args(JSContext *pctx, int argc, JSValueConst *argv, int exec)
{
    const char *url = argc > 2 && JS_IsString(argv[2]) ? JS_ToCString(pctx, argv[2]) : NULL;
    struct jsframe *f = frame_adopt(pctx, argv[0], exec, argc > 1 ? argv[1] : JS_NULL, url,
                                    argc > 3 ? argv[3] : JS_UNDEFINED);
    if (url) JS_FreeCString(pctx, url);
    return f;
}

static JSValue js__frameAdopt(JSContext *pctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;if(argc)frame_adopt_args(pctx,argc,argv,1);
    return JS_UNDEFINED;
}

/* Called only after the iframe's origin/sandbox gate has settled its parsed
 * document. JSValues may cross contexts on this SAME runtime; duplicating an
 * independent network frame's JSValue here would corrupt both runtimes.
 * It is deliberately not an API to look up other documents by URL or id. */
static JSValue js__frameGlobal(JSContext *ctx,JSValueConst t,int argc,JSValueConst *argv)
{
    (void)t;if(!argc)return JS_NULL;
    struct jsframe *f=frame_adopt_args(ctx,argc,argv,0);
    if(!f)return JS_ThrowInternalError(ctx,"Cannot create auxiliary frame realm");
    JS_SetStringCodeGenerationAllowed(f->fctx,JS_GetStringCodeGenerationAllowed(ctx));
    return JS_GetGlobalObject(f->fctx);
}

/* __frameRelease(doc): end the browsing context without destroying the
 * DOMParser document object. Removal and committed navigation have different
 * JS-visible document lifetimes, but the same context lifetime: author code
 * may keep a reference to the old document while scripts in it must stop
 * owning one of the eight live frame slots. Idempotence is load-bearing -- a
 * removal can be observed through both a subtree-replacing wrapper and a
 * later document finalizer, and the second call must be a no-op rather than a
 * double JS_FreeContext. This is also the one release path on which the
 * creator's page is certainly live, so it is the one that cancels the
 * frame's still-queued timers through the creator (frame_release_slot). */
static JSValue js__frameRelease(JSContext *pctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)pctx;
    if (argc < 1) return JS_UNDEFINED;
    struct dom_doc *doc = js_domparser_doc_of(argv[0]);
    if (doc) (void)frame_release_doc(doc, 1);
    return JS_UNDEFINED;
}

void js_frame_install_inert(JSContext *pctx)
{
    if (!pctx) return;
    js_domparser_set_script_sink(js_frame_offer_script);
    js_domparser_set_docfree_sink(frame_on_docfree);
    JSValue g = JS_GetGlobalObject(pctx);
    JS_SetPropertyStr(pctx, g, "__frameGlobal", JS_NewCFunction(pctx, js__frameGlobal, "__frameGlobal", 1));
    JS_SetPropertyStr(pctx, g, "__frameRelease", JS_NewCFunction(pctx, js__frameRelease, "__frameRelease", 1));
    JS_FreeValue(pctx, g);
}
void js_frame_install(JSContext *pctx)
{
    if(!pctx)return;
    js_frame_install_inert(pctx);
    JSValue g=JS_GetGlobalObject(pctx);
    JS_SetPropertyStr(pctx,g,"__frameAdopt",JS_NewCFunction(pctx,js__frameAdopt,"__frameAdopt",1));
    JS_FreeValue(pctx,g);
}

void js_frame_refresh_policy(JSContext *pctx)
{
    for(int i=0;i<JSF_TOTAL_FRAMES;i++)if(g_frames[i].used&&g_frames[i].owner==pctx)
        JS_SetStringCodeGenerationAllowed(g_frames[i].fctx,JS_GetStringCodeGenerationAllowed(pctx));
}
void js_frame_close_context(JSContext *pctx)
{
    for(int i=0;i<JSF_TOTAL_FRAMES;i++)if(g_frames[i].used&&g_frames[i].owner==pctx)
        frame_release_slot(&g_frames[i],0);
}

void js_frame_close_all(void)
{
    /* Per slot, not one memset: frame_release_slot keeps each slot's `gen`
     * moving forward, so a native callback that outlived this close can never
     * mistake a later occupant of its slot for its own frame. */
    for (int i = 0; i < JSF_TOTAL_FRAMES; i++)
        if (g_frames[i].used) frame_release_slot(&g_frames[i], 0);
    /* The sink is left registered: js_domparser.c is reinstalled fresh on
     * every js_page_open (a new set of dp_cid/prototypes), and the sink
     * function pointer itself is process-lifetime code, not page-lifetime
     * state -- calling js_frame_offer_script with an empty table (which is
     * exactly what this function just produced) is the documented no-op
     * path above, not a use-after-free. */
}
