/* ES modules for the page runtime -- see js_module.h for the measurement that
 * put this file here.
 *
 * Three things live in this file and nothing else does:
 *
 *   1. THE NORMALIZER. QuickJS hands the loader whatever string it was given
 *      after normalization, and its default normalizer does filesystem-style
 *      path joining. On the web the rule is URL resolution against the
 *      IMPORTING module's URL -- which is why the base name this module uses
 *      for every module is its absolute URL, not a filename. "./chunk.js",
 *      "../shared/x.js", "/abs.js" and "//cdn.example/x.js" all have to come
 *      out right, and dot segments have to be removed, or a bundle's own
 *      sibling imports 404.
 *
 *   2. THE LOADER. Fetch, compile with JS_EVAL_FLAG_COMPILE_ONLY, set
 *      import.meta.url, hand back the JSModuleDef. Compile-only matters: it is
 *      the only point at which import.meta can be populated, because after
 *      evaluation the module body has already read it.
 *
 *      WHAT COMPILE_ONLY DOES NOT DO -- measured, 2026-08-08, and worth
 *      knowing before reading a profile of a module page. It does not stop at
 *      compiling. js_parse_program calls js_resolve_module unconditionally
 *      (upstream's own comment there is "Could add a flag to avoid resolution
 *      if necessary"), so the JS_Eval below RECURSIVELY LOADS the entire
 *      static import graph before it returns: this loader re-enters itself,
 *      once per dependency, depth first, inside a single JS_Eval call. That
 *      is why a module page appears to freeze in one call rather than
 *      progressing chunk by chunk, and why the import.meta of a dependency is
 *      set after its own children have already been fetched (harmless --
 *      import.meta is read at evaluation, not at compile).
 *
 *      It costs nothing today because bfetch_sync is synchronous either way.
 *      It becomes the thing to fix the moment fetching can overlap: the graph
 *      is discovered one edge at a time inside the compiler, so no two of a
 *      page's chunks can ever be in flight at once. tests/unit/js_bench.c has
 *      to install a stub loader for exactly this reason -- without one, a real
 *      1.55 MB module fixture cannot be compiled at all.
 *
 *      THAT MOMENT ARRIVED (2026-09-02, the webaccel line's module-graph
 *      measurement: x.com's 405 modules, one 170ms-apart [wa]-adjacent round
 *      trip each). See mod_compile_and_prefetch() below for the fix -- it
 *      needed one small addition to quickjs.c itself, because the recursion
 *      this comment describes is not interruptible from outside: compiling a
 *      module and resolving (=loading) its children happen inside the SAME
 *      JS_Eval call with no seam the loader can insert concurrency into. The
 *      seam is JS_EVAL_FLAG_COMPILE_NO_RESOLVE, which is the flag upstream's
 *      own comment at that call site asked for ("Could add a flag to avoid
 *      resolution if necessary") and never added.
 *
 *   3. BARE SPECIFIERS ARE REFUSED, LOUDLY. `import "react"` has no meaning
 *      without an import map, and resolving it as a relative path would turn a
 *      diagnosable "bare specifier" into an undiagnosable 404 three hops into
 *      somebody's node_modules layout. */
#include "quickjs.h"
#include "js_module.h"
#include "js_dom.h"
#include "js_page.h"
#include "bfetch.h"
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

#define MOD_URLMAX 768

static JSRuntime *g_installed;          /* the runtime our loader is bound to */
static int g_loaded, g_failed;

void js_module_stats(int *loaded, int *failed)
{ if (loaded) *loaded = g_loaded; if (failed) *failed = g_failed; }
void js_module_reset(void) { g_loaded = 0; g_failed = 0; g_installed = 0; }

/* ---- <script type> classification ----
 *
 * The spec is a whitelist, not a blacklist: an unrecognised type means "this is
 * a data block, do not execute it". That matters in practice -- pages carry
 * <script type="application/json">, <script type="importmap"> and
 * <script type="text/template"> full of markup, and running any of them as
 * JavaScript is a syntax error that used to be reported as the page's. */
static int ci_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (;;) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
        a++; b++;
    }
}

static const char *trim_type(const char *t, char *buf, int max)
{
    if (!t) return 0;
    while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r' || *t == '\f') t++;
    int n = 0;
    while (t[n] && n < max - 1) n++;
    while (n > 0 && (t[n-1]==' '||t[n-1]=='\t'||t[n-1]=='\n'||t[n-1]=='\r'||t[n-1]=='\f')) n--;
    memcpy(buf, t, (size_t)n); buf[n] = 0;
    return buf;
}

int js_module_is_module_type(const char *type)
{
    char b[64];
    if (!type) return 0;
    return ci_eq(trim_type(type, b, sizeof b), "module");
}

int js_module_is_classic_type(const char *type)
{
    char b[64];
    if (!type || !type[0]) return 1;
    const char *t = trim_type(type, b, sizeof b);
    if (!t[0]) return 1;
    /* The HTML spec's "JavaScript MIME type essence" list, plus the legacy
     * spellings that are still in the wild. Parameters (";charset=utf-8") are
     * allowed, so compare only up to the first ';'. */
    char *semi = b; while (*semi && *semi != ';') semi++;
    char save = *semi; *semi = 0;
    static const char *ok[] = {
        "text/javascript", "application/javascript", "application/x-javascript",
        "text/ecmascript", "application/ecmascript", "text/jscript",
        "text/javascript1.5", "text/x-javascript", "javascript", 0
    };
    int hit = 0;
    for (int i = 0; ok[i]; i++) if (ci_eq(b, ok[i])) { hit = 1; break; }
    *semi = save;
    return hit;
}

/* ---- the module loader ---- */

static int is_bare_specifier(const char *s)
{
    if (!s || !s[0]) return 1;
    if (s[0] == '/' ) return 0;                        /* /abs or //host */
    if (s[0] == '.' && (s[1] == '/' || (s[1] == '.' && s[2] == '/'))) return 0;
    /* a scheme: letters/digits/+-. then ':' */
    int i = 0;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
           (s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '-' || s[i] == '.') i++;
    return !(i > 0 && s[i] == ':');
}

static char *mod_normalize(JSContext *ctx, const char *base_name,
                           const char *name, void *opaque)
{
    (void)opaque;
    if (is_bare_specifier(name)) {
        /* Say exactly what happened. This is the failure mode a real bundle
         * hits when it was built for a bundler rather than for the browser,
         * and "404" is not the same message at all. */
        printf("[js] bare module specifier '%s' (imported from %s) -- "
               "no import map, so this cannot be resolved\n", name, base_name ? base_name : "?");
        JS_ThrowTypeError(ctx, "bare module specifier '%s' is not resolvable "
                               "(relative or absolute URL required)", name);
        return NULL;
    }
    char abs[MOD_URLMAX];
    if (bfetch_resolve(base_name, name, abs, sizeof abs) != 0) {
        JS_ThrowTypeError(ctx, "cannot resolve module '%s' against '%s'",
                          name, base_name ? base_name : "?");
        return NULL;
    }
    return js_strdup(ctx, abs);
}

/* import.meta.url. Only settable between compile and evaluate, which is why
 * both the loader and js_module_eval go through COMPILE_ONLY. */
static void set_import_meta(JSContext *ctx, JSValueConst func_val, const char *url)
{
    if (JS_VALUE_GET_TAG(func_val) != JS_TAG_MODULE) return;
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta)) { JS_FreeValue(ctx, meta); return; }
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, url), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "main", JS_FALSE, JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta);
}

/* ---- module-graph prefetch --------------------------------------------
 *
 * Compile `src` as a module and, before letting QuickJS resolve (= fetch)
 * ITS OWN static imports, queue every one of them as a non-blocking prefetch
 * and drain that whole batch CONCURRENTLY -- bfetch_prefetch()/
 * bfetch_prefetch_wait(), the identical mechanism layout.c's <img> loop
 * already uses (see bfetch.h's "prefetch, for callers that fetch one
 * resource at a time"). Only THEN does JS_ResolveModule() run, and its
 * depth-first walk of req_module_entries finds every child already sitting
 * in the prefetch cache (or already in flight against the connection pool),
 * so mod_loader()'s res_fetch() call below takes it with zero additional
 * round trips instead of dialling one at a time.
 *
 * THE ONE-JAR RULE DECIDES WHERE THE SPECIFIER LIST COMES FROM: not a
 * second import parser (a regex would disagree with QuickJS about strings,
 * comments, export-from and dynamic import() -- see the file header), but
 * JS_GetModuleReqEntries*(), reading the exact list js_resolve_module()
 * itself is about to walk. That required one addition to quickjs.c
 * (JS_EVAL_FLAG_COMPILE_NO_RESOLVE) because there is no public way to stop
 * JS_Eval from resolving a module's children before it returns -- the
 * upstream code has said "Could add a flag to avoid resolution if
 * necessary" at that exact call site for longer than this fork has existed.
 *
 * BREADTH-FIRST LAYERED UNDER A DEPTH-FIRST LOADER, not a replacement for
 * it: this only ever sees ONE module's DIRECT children (its own
 * req_module_entries), because that is the deepest point at which the list
 * is knowable without patching further into the parser. Applied at every
 * level of the recursion, it turns each level's fan-out into one concurrent
 * wave (bounded by hpool's 6-total/2-per-origin caps) instead of a chain --
 * still O(depth) waves rather than O(1), but a page whose import graph is
 * wide and shallow (the common shape: an entry chunk pulling in dozens of
 * route/vendor chunks) gets most of the win.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: dedupe a specifier that TWO DIFFERENT
 * modules both request before either has been resolved. bfetch_prefetch()
 * dedupes WITHIN one call's own list (a module importing the same specifier
 * twice costs one fetch), and a shared chunk already fully loaded by an
 * earlier sibling is caught for free by QuickJS's own js_find_loaded_module
 * (mod_loader/mod_normalize are simply never re-invoked for an already-
 * resolved URL). What slips through is the narrow window where two not-yet-
 * resolved branches both prefetch the same not-yet-cached URL: one of the
 * two prefetch-cache entries is consumed by the real load, the other sits
 * unclaimed until bfetch_cache_clear() on the next navigation. Bounded
 * (single page load, cache capped at BF_NCACHE=32) and never wrong -- it
 * costs a duplicate fetch and some memory, never a corrupted module -- so it
 * is left as a known, named trade rather than solved with a global
 * in-flight-URL registry this file does not own. */
static JSValue mod_compile_and_prefetch(JSContext *ctx, const unsigned char *src,
                                        int len, const char *url)
{
#ifdef JS_MODULE_NO_PREFETCH
    /* THE CONTROL (tests/jsmodpf.mk test-jsmodpf-negctl): byte-identical to
     * this file before 2026-09-02, one request in flight at a time. Against
     * qmp_modprefetch.py's fixture (18 static imports split across 3 origins,
     * a fixed per-request delay) this build must take roughly N x delay; the
     * default build should take roughly ceil(N / 6) x delay, because hpool's
     * cap is 2 connections PER ORIGIN and the fixture spans 3 origins for
     * exactly that reason -- a single-origin graph would cap at 2x, not 6x.
     * If the two builds print the same guest-clock elapsed time, prefetch is
     * not happening. */
    return JS_Eval(ctx, (const char *)src, (size_t)len, url,
                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
#else
    JSValue v = JS_Eval(ctx, (const char *)src, (size_t)len, url,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
                        JS_EVAL_FLAG_COMPILE_NO_RESOLVE);
    if (JS_IsException(v)) return v;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_MODULE) return v;   /* classic script: nothing to walk */

    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    int n = JS_GetModuleReqEntriesCount(m);
    int queued = 0;
    for (int i = 0; i < n; i++) {
        JSAtom a = JS_GetModuleReqEntryName(ctx, m, i);
        if (a == JS_ATOM_NULL) continue;
        const char *spec = JS_AtomToCString(ctx, a);
        if (spec) {
            /* Same refusal mod_normalize applies, checked here too: a bare
             * specifier is not a URL bfetch_resolve can do anything with,
             * and starting a doomed prefetch for it would only cost a
             * table slot -- the real failure (and its diagnostic message)
             * still happens exactly where it does today, in mod_normalize,
             * when QuickJS actually asks for this entry below. */
            if (!is_bare_specifier(spec)) {
                char abs[MOD_URLMAX];
                if (bfetch_resolve(url, spec, abs, sizeof abs) == 0) {
                    bfetch_prefetch(abs);      /* queues, or silently does not
                                                 * if BF_NCACHE is full -- see
                                                 * bfetch_prefetch's own
                                                 * "table full" comment. Either
                                                 * way this is best-effort:
                                                 * mod_loader's res_fetch()
                                                 * falls straight back to a
                                                 * plain bfetch_sync() on a
                                                 * cache miss, which is
                                                 * exactly today's path. */
                    queued = 1;
                }
            }
            JS_FreeCString(ctx, spec);
        }
        JS_FreeAtom(ctx, a);
    }
    /* Only block here if something was actually queued: an empty page
     * (n==0, a leaf module) must not pay a pump-loop yield for nothing. */
    if (queued) bfetch_prefetch_wait();

    if (JS_ResolveModule(ctx, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
    return v;
#endif
}

static JSModuleDef *mod_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    (void)opaque;
    unsigned char *src = 0;
    int len = 0;
    /* res_fetch(), not bfetch_sync(): a child prefetched by ITS PARENT's own
     * mod_compile_and_prefetch() call is sitting in the prefetch cache under
     * this exact absolute URL (mod_normalize already resolved module_name
     * against the parent, the same resolution bfetch_resolve inside res_fetch
     * repeats and is idempotent on an already-absolute URL) -- res_fetch()
     * takes it with no network call. A cache miss (the page's very first
     * module, or the prefetch table was full) falls back to bfetch_sync()
     * unchanged. */
    if (res_fetch(module_name, &src, &len) != 0 || !src) {
        g_failed++;
        printf("[js] module fetch FAILED: %s\n", module_name);
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return NULL;
    }
    printf("[js] module loaded %d bytes: %s\n", len, module_name);
    g_loaded++;
    JSValue v = mod_compile_and_prefetch(ctx, src, len, module_name);
    free(src);
    if (JS_IsException(v)) { g_failed++; return NULL; }
    set_import_meta(ctx, v, module_name);
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    return m;
}

/* The runtime is created by js_page_open(), which this line does not own, so
 * the loader is installed lazily and re-installed whenever the runtime under us
 * has been replaced by a navigation. Comparing the runtime pointer is the whole
 * check: a fresh JS_NewRuntime has no loader, and reinstalling on the same one
 * is idempotent. */
static int ensure_installed(JSContext *ctx)
{
    if (!ctx) return 0;
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (rt == g_installed) return 1;
    JS_SetModuleLoaderFunc(rt, mod_normalize, mod_loader, 0);
    g_installed = rt;
    return 1;
}

/* Report an exception the way js_page.c does for classic scripts, so a module
 * failure is as visible on the serial log as a script failure. */
static void report(JSContext *ctx, const char *url)
{
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    printf("[browser] module exception in %s: %s\n", url, m ? m : "?");
    /* The GUI half. Serial-only reporting made a module that throws at top
     * level indistinguishable from a module that rendered nothing on purpose
     * -- the classic-script path (js_page.c:667) and the timer path both
     * note() their exceptions into the status bar, and this path was the one
     * of the three that did not. */
    js_page_note("[exception] "); if (m) js_page_note(m); js_page_note("\n");
    if (JS_IsError(ctx, e)) {
        JSValue st = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(st)) {
            const char *s = JS_ToCString(ctx, st);
            if (s && s[0]) printf("%s\n", s);
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, st);
    }
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
}

int js_module_eval(const char *src, int len, const char *url)
{
    JSContext *ctx = js_page_ctx();
    if (!ctx || !src || !url) return 0;
#ifdef FRMW_NEGCTL_NOMODULE
    /* The negative control for tests/fragmk.mk's guest gate, and the choice
     * of switch is the point: it does not stub the loader, or fetch and drop
     * the bytes, or mis-resolve one specifier -- it makes this function a
     * no-op, which is EXACTLY the state this file replaced (qmp_module_page.py
     * documents that era: every <script type=module> was evaluated as a
     * classic script, `import` at byte 0 was a SyntaxError, and the whole
     * class of module-shipped sites had zero JavaScript running while the
     * HTML-construction score said 1723/1818). With the switch on, the five
     * module-driven corpus apps mount nothing while webpack (defer) and next
     * (classic async) still mount, so the gate reads 2 of 7 against a bar of
     * 5 -- a control that takes the number BELOW the bar rather than to zero,
     * because a gate that can only fail to zero also passes any
     * partial-silence defect. Measured in the guest 2026-08-30: red build
     * mounts 2/7 (webpack + next -- the two classic-script apps), shipped
     * build 7/7. The reporter the gate reads is deliberately a CLASSIC
     * inline script for exactly this reason: a module-shaped reporter dies
     * with this switch and the control reads 0/7, which cannot be told apart
     * from a broken driver. */
    (void)ensure_installed; (void)mod_normalize; (void)mod_loader;
    (void)set_import_meta; (void)report; (void)mod_compile_and_prefetch;
    return 0;
#endif
    js_page_slice_begin();           /* a module body is one CPU slice too */
    ensure_installed(ctx);

    /* Compile first so import.meta.url exists before the body runs. The compile
     * step is also where LINKING happens (JS_EvalFunction below resolves the
     * imports), so a missing dependency surfaces here rather than as a
     * mysterious undefined. The root document's own top-level imports go
     * through the SAME prefetch-then-resolve path as every module mod_loader
     * fetches -- see mod_compile_and_prefetch()'s comment -- so a page whose
     * entry chunk fans out into a dozen route chunks gets the concurrency win
     * at the outermost level too, not just inside child modules. */
    JSValue fn = mod_compile_and_prefetch(ctx, (const unsigned char *)src, len, url);
    if (JS_IsException(fn)) { report(ctx, url); JS_FreeValue(ctx, fn); return 0; }
    set_import_meta(ctx, fn, url);

    JSValue v = JS_EvalFunction(ctx, fn);       /* consumes fn; links + evaluates */
    if (JS_IsException(v)) { report(ctx, url); JS_FreeValue(ctx, v); return 0; }

    /* A module body is an async function: evaluation hands back a promise even
     * with no top-level await, and its rejection is delivered as a job. Drain
     * the queue before deciding whether the module succeeded, or a module whose
     * very first import threw would be reported as having run fine. */
    js_dom_run_jobs(ctx);

    int ok = 1;
    if (JS_IsObject(v)) {
        JSPromiseStateEnum st = JS_PromiseState(ctx, v);
        if (st == JS_PROMISE_REJECTED) {
            JSValue r = JS_PromiseResult(ctx, v);
            const char *m = JS_ToCString(ctx, r);
            printf("[browser] module rejected %s: %s\n", url, m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, r);
            ok = 0;
        } else if (st == JS_PROMISE_PENDING) {
            /* Top-level await that is still waiting on a timer or a fetch. The
             * page's event loop keeps pumping jobs, so this is not an error --
             * it just is not finished yet. */
            printf("[js] module still pending (top-level await): %s\n", url);
        }
    }
    JS_FreeValue(ctx, v);
    return ok;
}
