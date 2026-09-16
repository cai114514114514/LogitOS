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
 *   4. NO frame-side `document`, `fetch`, `XMLHttpRequest`, or DOM of any
 *      kind -- see point 3 of the architecture note. `console`, `window`
 *      and `self` only.
 *   5. NO frame-side setTimeout/setInterval. A frame script that schedules
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
#include "js_frame.h"
#include "dom.h"
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

struct jsframe {
    int used;
    struct dom_doc *doc;     /* key: the frame document this context belongs to */
    JSContext *fctx;         /* the frame's own context, on the PAGE's runtime */
    JSContext *owner;
    int execute_scripts;
};

static struct jsframe g_frames[JSF_TOTAL_FRAMES];

static struct jsframe *find_by_doc(struct dom_doc *doc)
{
    if (!doc) return 0;
    for (int i = 0; i < JSF_TOTAL_FRAMES; i++)
        if (g_frames[i].used && g_frames[i].doc == doc) return &g_frames[i];
    return 0;
}

/* js_domparser.c's docfree sink -- see that file's own comment on why this
 * exists at all (a real UAF-shaped bug, found by frame_test.c, not assumed):
 * `dom_doc` pointers are reused by the allocator once freed, so a stale
 * table entry keyed on one is a live-looking hit for a completely unrelated,
 * later document. Called with the doc about to be destroyed; frees this
 * frame's JSContext right here rather than waiting for js_frame_close_all,
 * because a page that opens and discards many frames one at a time (not
 * just at navigation) must not exhaust JSF_MAX_FRAMES on documents nothing
 * references anymore. */
static int frame_release_doc(struct dom_doc *doc)
{
    struct jsframe *f = find_by_doc(doc);
    if (!f) return 0;
    if (f->fctx) JS_FreeContext(f->fctx);
    memset(f, 0, sizeof *f);
    return 1;
}

static void frame_on_docfree(struct dom_doc *doc)
{
    (void)frame_release_doc(doc);
}

/* ---- the frame's own (minimal, DOM-less) global scope -- see the file
 * header's cut list, item 4. `window`/`self` alias globalThis, same shape
 * WORKER_SELF_JS uses for `self` (js_worker.c), so `window.__ran = 1` and
 * `self.foo` both reach the same object a probe reads back afterward. */
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

static void frame_install_globals(JSContext *fctx)
{
    JSValue g = JS_GetGlobalObject(fctx);
    JS_SetPropertyStr(fctx, g, "window", JS_DupValue(fctx, g));
    JS_SetPropertyStr(fctx, g, "self", JS_DupValue(fctx, g));
    JSValue con = JS_NewObject(fctx);
    JSValue logf = JS_NewCFunction(fctx, frame_console, "log", 1);
    JS_SetPropertyStr(fctx, con, "log", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "info", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "debug", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "warn", JS_DupValue(fctx, logf));
    JS_SetPropertyStr(fctx, con, "error", logf);
    JS_SetPropertyStr(fctx, g, "console", con);
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
    JSValue r = JS_Eval(f->fctx, b.p, b.len, "<frame script>", JS_EVAL_TYPE_GLOBAL);
    free(b.p);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(f->fctx);
        const char *m = JS_ToCString(f->fctx, e);
        printf("[frame] uncaught: %s\n", m ? m : "?");
        if (m) JS_FreeCString(f->fctx, m);
        JS_FreeValue(f->fctx, e);
    }
    JS_FreeValue(f->fctx, r);
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
 * leaking a second one. */
static struct jsframe *frame_adopt(JSContext *pctx,JSValueConst value,int execute_scripts)
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
    frame_install_globals(fctx);

    g_frames[slot].used = 1;
    g_frames[slot].doc = doc;
    g_frames[slot].fctx = fctx;
    g_frames[slot].owner = pctx;
    g_frames[slot].execute_scripts = execute_scripts;

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
    return &g_frames[slot];
}
static JSValue js__frameAdopt(JSContext *pctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;if(argc)frame_adopt(pctx,argv[0],1);
    return JS_UNDEFINED;
}

/* Called only after the iframe's origin/sandbox gate has settled its parsed
 * document. JSValues may cross contexts on this SAME runtime; duplicating an
 * independent network frame's JSValue here would corrupt both runtimes.
 * It is deliberately not an API to look up other documents by URL or id. */
static JSValue js__frameGlobal(JSContext *ctx,JSValueConst t,int argc,JSValueConst *argv)
{
    (void)t;if(!argc)return JS_NULL;
    struct jsframe *f=frame_adopt(ctx,argv[0],0);
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
 * double JS_FreeContext. */
static JSValue js__frameRelease(JSContext *pctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    struct dom_doc *doc = js_domparser_doc_of(argv[0]);
    if (doc) (void)frame_release_doc(doc);
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
        frame_release_doc(g_frames[i].doc);
}

void js_frame_close_all(void)
{
    for (int i = 0; i < JSF_TOTAL_FRAMES; i++) {
        if (!g_frames[i].used) continue;
        if (g_frames[i].fctx) JS_FreeContext(g_frames[i].fctx);
    }
    memset(g_frames, 0, sizeof g_frames);
    /* The sink is left registered: js_domparser.c is reinstalled fresh on
     * every js_page_open (a new set of dp_cid/prototypes), and the sink
     * function pointer itself is process-lifetime code, not page-lifetime
     * state -- calling js_frame_offer_script with an empty table (which is
     * exactly what this function just produced) is the documented no-op
     * path above, not a use-after-free. */
}
