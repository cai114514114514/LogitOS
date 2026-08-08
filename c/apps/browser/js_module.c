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

static JSModuleDef *mod_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    (void)opaque;
    unsigned char *src = 0;
    int len = 0;
    if (bfetch_sync(module_name, &src, &len) != 0 || !src) {
        g_failed++;
        printf("[js] module fetch FAILED: %s\n", module_name);
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return NULL;
    }
    printf("[js] module loaded %d bytes: %s\n", len, module_name);
    g_loaded++;
    JSValue v = JS_Eval(ctx, (const char *)src, (size_t)len, module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
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
    ensure_installed(ctx);

    /* Compile first so import.meta.url exists before the body runs. The compile
     * step is also where LINKING happens (JS_EvalFunction below resolves the
     * imports), so a missing dependency surfaces here rather than as a
     * mysterious undefined. */
    JSValue fn = JS_Eval(ctx, src, (size_t)len, url,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
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
