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
 *      somebody's node_modules layout.
 *      Correction (2026-09-09): the import-map service below now resolves
 *      mapped bare names. Only names with no matching mapping are refused;
 *      map parsing, prefetch, static linking and dynamic import share it. */
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
static void im_clear(void);

/* The old scripts_execute phase includes recursively fetched dependencies:
 * GitHub Trending measured 44.27s there, without saying whether it was download
 * or compilation. Attribute at the real seams, including failure paths. A
 * parent resolve/evaluate encloses its child's loader, so independent start/end
 * sums would silently count that wait twice. Enter/leave transfers ownership of
 * the SAME guest-clock interval instead. No per-chunk diagnostic printing is
 * added inside the measured seams. This does not make the loader asynchronous. */
enum mod_phase { MOD_IDLE, MOD_COMPILE, MOD_PREFETCH, MOD_FETCH, MOD_LINK, MOD_EVAL };
static enum mod_phase g_mod_phase;
static unsigned long long g_mod_stamp;
static struct js_module_profile g_mod_profile;
static void mod_account(void)
{
    unsigned long long now=js_page_now_ms();
    unsigned long long dt=now>=g_mod_stamp?now-g_mod_stamp:0;
    switch(g_mod_phase) {
    case MOD_COMPILE:g_mod_profile.compile_ms+=dt;break;
    case MOD_PREFETCH:g_mod_profile.prefetch_ms+=dt;break;
    case MOD_FETCH:g_mod_profile.fetch_ms+=dt;break;
    case MOD_LINK:g_mod_profile.link_ms+=dt;break;
    case MOD_EVAL:g_mod_profile.eval_jobs_ms+=dt;break;
    default:break;
    }
    g_mod_stamp=now;
}
static enum mod_phase mod_enter(enum mod_phase next)
{
    enum mod_phase previous=g_mod_phase;mod_account();
#ifdef JS_MODULE_PROFILE_MISCHARGE_IO
    if(next==MOD_PREFETCH||next==MOD_FETCH)next=MOD_COMPILE;
#endif
    g_mod_phase=next;return previous;
}
static void mod_leave(enum mod_phase previous){mod_account();g_mod_phase=previous;}
void js_module_profile_get(struct js_module_profile *out)
{ if(out){mod_account();*out=g_mod_profile;} }
void js_module_profile_dump(void)
{
    struct js_module_profile p;js_module_profile_get(&p);
    printf("[module-perf] compile_ms=%llu prefetch_ms=%llu fetch_ms=%llu link_ms=%llu eval_jobs_ms=%llu compile_bytes=%llu compiles=%u fetches=%u prefetch_batches=%u\n",
        p.compile_ms,p.prefetch_ms,p.fetch_ms,p.link_ms,p.eval_jobs_ms,p.compile_bytes,p.compiles,p.fetches,p.prefetch_batches);
}

void js_module_stats(int *loaded, int *failed)
{ if (loaded) *loaded = g_loaded; if (failed) *failed = g_failed; }
void js_module_reset(void)
{
    im_clear();g_loaded=0;g_failed=0;g_installed=0;
    memset(&g_mod_profile,0,sizeof g_mod_profile);g_mod_phase=MOD_IDLE;g_mod_stamp=0;
}

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

int js_module_is_importmap_type(const char *type)
{
    char b[64];
    return type && ci_eq(trim_type(type, b, sizeof b), "importmap");
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

/* Textual inclusion keeps every existing host/guest module-loader consumer
 * on the same resolver. A separate TU would silently strand the hand-written
 * source lists this tree still has. No Web API constructor is fabricated. */
#include "js_importmap.inc"

static char *mod_normalize(JSContext *ctx, const char *base_name,
                           const char *name, void *opaque)
{
    (void)opaque;
    base_name = im_base(base_name);
    char abs[MOD_URLMAX];
    int mapped = im_resolve(ctx, base_name, name, abs, sizeof abs);
    if (mapped < 0) return NULL;
    if (mapped) return js_strdup(ctx, abs);
    if (is_bare_specifier(name)) {
        /* Say exactly what happened. This is the failure mode a real bundle
         * hits when it was built for a bundler rather than for the browser,
         * and "404" is not the same message at all. */
        printf("[js] bare module specifier '%s' (imported from %s) -- "
               "no matching import map entry\n", name, base_name ? base_name : "?");
        JS_ThrowTypeError(ctx, "bare module specifier '%s' is not resolvable "
                               "(relative or absolute URL required)", name);
        return NULL;
    }
    if (bfetch_resolve(base_name, name, abs, sizeof abs) != 0) {
        JS_ThrowTypeError(ctx, "cannot resolve module '%s' against '%s'",
                          name, base_name ? base_name : "?");
        return NULL;
    }
    if (!im_remember(ctx, base_name, abs)) return NULL;
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
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, im_base(url)), JS_PROP_C_W_E);
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
    g_mod_profile.compiles++;if(len>0)g_mod_profile.compile_bytes+=(unsigned)len;
    enum mod_phase compile_parent=mod_enter(MOD_COMPILE);
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
    JSValue v=JS_Eval(ctx, (const char *)src, (size_t)len, url,
                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    mod_leave(compile_parent);return v;
#else
    JSValue v = JS_Eval(ctx, (const char *)src, (size_t)len, url,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
                        JS_EVAL_FLAG_COMPILE_NO_RESOLVE);
    mod_leave(compile_parent);
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
            /* Prefetch and linking must use ONE resolution path. Otherwise
             * mapped imports fetch their old spelling speculatively and only
             * discover the mapping during linking. A failed speculative
             * resolution is left for JS_ResolveModule to report once. */
            char *abs = mod_normalize(ctx, url, spec, NULL);
            if (abs) {
#ifndef JS_MODULE_PREFETCH_LOADED
                    /* Correction to the tradeoff above (2026-09-11): an
                     * already compiled shared import/back edge was fetched
                     * again, but QuickJS never calls mod_loader for it. Those
                     * unclaimed bodies fill BF_NCACHE and turn later imports
                     * into serial fetches. The 48-branch cyclic fixture offered
                     * the shared chunk 49 times and the root 48 times before
                     * this guard; both now use the resolver's actual map.
                     * A second browser-side URL set would disagree after a
                     * failed compile or a runtime replacement. */
                    int present = JS_HasModule(ctx, abs);
                    if (present != 0) {
                        if (present < 0) {
                            JSValue error = JS_GetException(ctx);
                            JS_FreeValue(ctx, error);
                        }
                        js_free(ctx, abs);
                        JS_FreeCString(ctx, spec);
                        JS_FreeAtom(ctx, a);
                        continue;
                    }
#endif
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
                    js_free(ctx, abs);
            } else {
                JSValue error = JS_GetException(ctx);
                JS_FreeValue(ctx, error);
            }
            JS_FreeCString(ctx, spec);
        }
        JS_FreeAtom(ctx, a);
    }
    /* Only block here if something was actually queued: an empty page
     * (n==0, a leaf module) must not pay a pump-loop yield for nothing. */
    if (queued) {
        g_mod_profile.prefetch_batches++;
        enum mod_phase wait_parent=mod_enter(MOD_PREFETCH);
#ifndef JS_MODULE_NETWORK_TIME_OLD
        unsigned long long io = js_page_slice_io_begin();
#endif
        bfetch_prefetch_wait();
#ifndef JS_MODULE_NETWORK_TIME_OLD
        js_page_slice_io_end(io);
#endif
        mod_leave(wait_parent);
    }

    if (JS_ResolveModule(ctx, v) < 0) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
    return v;
#endif
}

static JSModuleDef *mod_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    (void)opaque;
    enum mod_phase parent=mod_enter(MOD_LINK);
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
    /* res_fetch also covers prefetch-cache misses, which synchronously dial.
     * Exclude retrieval only: compiling its returned bytes below stays charged.
     * Bracket failures too, so a failed dependency cannot leak an I/O scope. */
#ifndef JS_MODULE_NETWORK_TIME_OLD
    unsigned long long io = js_page_slice_io_begin();
#endif
    g_mod_profile.fetches++;
    enum mod_phase fetch_parent=mod_enter(MOD_FETCH);
    int fetch_rc = res_fetch(module_name, &src, &len);
    mod_leave(fetch_parent);
#ifndef JS_MODULE_NETWORK_TIME_OLD
    js_page_slice_io_end(io);
#endif
    if (fetch_rc != 0 || !src) {
        g_failed++;
        printf("[js] module fetch FAILED: %s\n", module_name);
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        mod_leave(parent);
        return NULL;
    }
    printf("[js] module loaded %d bytes: %s\n", len, module_name);
    g_loaded++;
    JSValue v = mod_compile_and_prefetch(ctx, src, len, module_name);
    free(src);
    if (JS_IsException(v)) { g_failed++; mod_leave(parent);return NULL; }
    set_import_meta(ctx, v, module_name);
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(v);
    JS_FreeValue(ctx, v);
    mod_leave(parent);
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

void js_module_prepare(void) { ensure_installed(js_page_ctx()); }

int js_module_importmap(const char *json, int len, const char *base_url)
{
    JSContext *ctx = js_page_ctx();
    if (!ctx || !json || len < 0 || !base_url) return 0;
    ensure_installed(ctx);
    if (im_parse(ctx, json, len, base_url)) return 1;
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    printf("[browser] import map rejected: %s\n", s ? s : "invalid map");
    js_page_note("[exception] import map rejected: ");
    if (s) { js_page_note(s); JS_FreeCString(ctx, s); }
    js_page_note("\n"); JS_FreeValue(ctx, e);
    return 0;
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
    enum mod_phase parent=mod_enter(MOD_LINK);
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
    if (JS_IsException(fn)) {
        report(ctx, url); JS_FreeValue(ctx, fn);
#ifndef JS_MODULE_NETWORK_TIME_OLD
        js_page_slice_end();
#endif
        mod_leave(parent);
        return 0;
    }
    set_import_meta(ctx, fn, url);

    enum mod_phase eval_parent=mod_enter(MOD_EVAL);
    JSValue v = JS_EvalFunction(ctx, fn);       /* consumes fn; links + evaluates */
    if (JS_IsException(v)) {
        mod_leave(eval_parent);
        report(ctx, url); JS_FreeValue(ctx, v);
#ifndef JS_MODULE_NETWORK_TIME_OLD
        js_page_slice_end();
#endif
        mod_leave(parent);
        return 0;
    }

    /* A module body is an async function: evaluation hands back a promise even
     * with no top-level await, and its rejection is delivered as a job. Drain
     * the queue before deciding whether the module succeeded, or a module whose
     * very first import threw would be reported as having run fine. */
    if (!js_page_cancel_requested()) js_dom_run_jobs(ctx);
    mod_leave(eval_parent);

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
#ifndef JS_MODULE_NETWORK_TIME_OLD
    /* A completed or pending module no longer owns this synchronous entry.
     * Retaining its deadline through idle time interrupts the next native
     * event/job after the module already returned. Same boundary as classic JS. */
    js_page_slice_end();
#endif
    mod_leave(parent);
    return ok;
}
