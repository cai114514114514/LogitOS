/* The probe: what did a page's scripts ask this engine for that it does not
 * have? (Diagnostic builds only -- the whole file is empty without
 * -DJS_PROBE_TRACE; build one with `make BUILD=build-probe PROBE=1`.)
 *
 * WHY IT EXISTS. Cloudflare's challenge (orchestrate/chl_page) is 236 KB of
 * obfuscated script that feature-tests the platform inside try/catch and then
 * reports "Browser not supported" or dies on `TypeError: not an object` in a
 * function named `Db`. Neither message names anything. A checklist of what
 * other engines were missing (Ladybird issue #226) is a guess about THIS
 * script; the probe is the measurement: every lookup that walked a platform
 * object's whole prototype chain and found nothing, and every error the
 * engine created -- including the ones the script caught and swallowed.
 *
 * WHAT COUNTS AS A PLATFORM OBJECT. A miss on a script's own `{}` is noise,
 * so a receiver is reported only if it, or something on its prototype chain,
 * is one of: the global object ("window"); a global the BROWSER installed --
 * i.e. a global name a bare QuickJS context does not also have -- or, when
 * that global is a constructor, its .prototype. The ES intrinsics (Object,
 * Array, Promise ...) are excluded by that subtraction, so Object.prototype
 * misses never appear.
 *
 * THE WALK RUNS NO PAGE CODE. Prototypes are followed with JS_ProbeRawProto
 * (no reference, no Proxy trap), property names are printed from atoms, and
 * the hook never calls a getter. A probe that executed the page's code while
 * observing it would change what it measures.
 *
 * OUTPUT (serial): the FIRST time a (receiver, name, kind) or a throw is seen
 * it prints one line --
 *     [probe] miss Navigator.pdfViewerEnabled get
 *     [probe] throw TypeError: not an object [js_object_getPrototypeOf] in Db
 * -- and js_probe_report() (called at page close) prints every entry with
 * its count, so a harness that navigates away gets the totals. */
#ifdef JS_PROBE_TRACE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "js/runtime/probe.h"

#define PROBE_OBJS   4096      /* platform objects tracked (pointer set) */
#define PROBE_SEEN   8192      /* distinct misses/throws remembered */
#define PROBE_LABEL  64

struct probe_obj { void *p; char label[PROBE_LABEL]; JSValue keep; };
static struct probe_obj g_objs[PROBE_OBJS];
static int g_nobjs;

struct probe_seen { unsigned long h; char text[160]; unsigned count; };
static struct probe_seen g_seen[PROBE_SEEN];
static int g_nseen, g_dropped;
static int g_building;
static JSContext *g_owner;     /* the context whose objects g_objs keeps alive */

static unsigned long hash_str(const char *s)
{
    unsigned long h = 1469598103934665603UL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211UL; }
    return h;
}

static int slot_for(void *p)
{
    unsigned long h = ((unsigned long)p >> 4) * 11400714819323198485UL;
    for (int i = 0; i < PROBE_OBJS; i++) {
        int k = (int)((h + (unsigned long)i) & (PROBE_OBJS - 1));
        if (!g_objs[k].p || g_objs[k].p == p) return k;
    }
    return -1;
}

static const char *label_of(void *p)
{
    int k = slot_for(p);
    return (k >= 0 && g_objs[k].p == p) ? g_objs[k].label : NULL;
}

static void add_obj(JSContext *ctx, JSValueConst v, const char *label)
{
    if (!JS_IsObject(v) || g_nobjs >= PROBE_OBJS * 3 / 4) return;
    void *p = JS_VALUE_GET_PTR(v);
    int k = slot_for(p);
    if (k < 0 || g_objs[k].p) return;           /* full, or already labelled */
    g_objs[k].p = p;
    snprintf(g_objs[k].label, sizeof g_objs[k].label, "%s", label);
    g_objs[k].keep = JS_DupValue(ctx, v);       /* a freed-and-reused address must not be misnamed */
    g_nobjs++;
}

/* Count one event; print it the first time it is seen. */
static void note(const char *text)
{
    unsigned long h = hash_str(text);
    for (int i = 0; i < PROBE_SEEN; i++) {
        int k = (int)((h + (unsigned long)i) & (PROBE_SEEN - 1));
        if (!g_seen[k].count) {
            if (g_nseen >= PROBE_SEEN * 3 / 4) { g_dropped++; return; }
            g_seen[k].h = h;
            snprintf(g_seen[k].text, sizeof g_seen[k].text, "%s", text);
            g_seen[k].count = 1;
            g_nseen++;
            printf("[probe] %s\n", text);
            return;
        }
        if (g_seen[k].h == h && !strcmp(g_seen[k].text, text)) { g_seen[k].count++; return; }
    }
}

static void on_miss(JSContext *ctx, JSValueConst obj, JSAtom prop, int kind, void *opaque)
{
    (void)opaque;
    if (g_building || !JS_IsObject(obj)) return;
    const char *label = NULL;
    void *p = JS_VALUE_GET_PTR(obj);
#ifdef PROBE_NEGCTL_NO_WALK
    const int max_depth = 1;     /* the control: look at the receiver only */
#else
    const int max_depth = 24;
#endif
    for (int depth = 0; p && depth < max_depth && !label; depth++) {
        label = label_of(p);
        p = JS_ProbeRawProto(p);
    }
    if (!label) return;
    const char *name = JS_AtomToCString(ctx, prop);
    if (!name) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    char text[160];
    static const char *const kinds[] = { "get", "has", "ref" };
    snprintf(text, sizeof text, "miss %s.%s %s", label, name, kinds[kind & 3]);
    JS_FreeCString(ctx, name);
    note(text);
}

static void on_throw(JSContext *ctx, int error_num, const char *msg,
                     const char *where, const char *fn, void *opaque)
{
    (void)ctx; (void)opaque;
    static const char *const names[] = { "EvalError", "RangeError", "ReferenceError",
        "SyntaxError", "TypeError", "URIError", "InternalError", "AggregateError" };
    const char *en = (error_num >= 0 && error_num < 8) ? names[error_num] : "Error";
    char text[160];
    snprintf(text, sizeof text, "throw %s: %.80s%s%s%s in %s", en, msg,
             where ? " [" : "", where ? where : "", where ? "]" : "", fn && *fn ? fn : "?");
    note(text);
}

static void on_note(JSContext *ctx, const char *text, void *opaque)
{
    (void)ctx; (void)opaque;
    note(text);
}

/* The global names a bare context has: the ES intrinsics, never reported. */
static int is_intrinsic(char names[][48], int n, const char *s)
{
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return 1;
    return 0;
}

void js_probe_install(JSContext *ctx, const char *prefix)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    static char intrinsic[512][48];
    static int nintrinsic = -1;
    g_building = 1;
    if (nintrinsic < 0) {
        nintrinsic = 0;
        JSContext *bare = JS_NewContext(rt);
        if (bare) {
            JSValue bg = JS_GetGlobalObject(bare);
            JSPropertyEnum *tab; uint32_t n;
            if (JS_GetOwnPropertyNames(bare, &tab, &n, bg, JS_GPN_STRING_MASK) == 0) {
                for (uint32_t i = 0; i < n; i++) {
                    const char *s = JS_AtomToCString(bare, tab[i].atom);
                    if (s && nintrinsic < 512) snprintf(intrinsic[nintrinsic++], 48, "%s", s);
                    JS_FreeCString(bare, s);
                    JS_FreeAtom(bare, tab[i].atom);
                }
                js_free(bare, tab);
            }
            JS_FreeValue(bare, bg);
            JS_FreeContext(bare);
        }
    }
    if (!g_owner) g_owner = ctx;
    JSValue g = JS_GetGlobalObject(ctx);
    char lab[PROBE_LABEL];
    snprintf(lab, sizeof lab, "%swindow", prefix ? prefix : "");
    add_obj(ctx, g, lab);
    JSPropertyEnum *tab; uint32_t n;
    int platform = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, g, JS_GPN_STRING_MASK) == 0) {
        for (uint32_t i = 0; i < n; i++) {
            const char *s = JS_AtomToCString(ctx, tab[i].atom);
            if (s && !is_intrinsic(intrinsic, nintrinsic, s)) {
                JSValue v = JS_GetProperty(ctx, g, tab[i].atom);
                if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
                else if (JS_IsObject(v)) {
                    snprintf(lab, sizeof lab, "%s%s", prefix ? prefix : "", s);
                    if (JS_IsFunction(ctx, v)) {
                        JSValue proto = JS_GetPropertyStr(ctx, v, "prototype");
                        if (JS_IsException(proto)) JS_FreeValue(ctx, JS_GetException(ctx));
                        else { add_obj(ctx, proto, lab); JS_FreeValue(ctx, proto); }
                    }
                    add_obj(ctx, v, lab);
                    platform++;
                }
                JS_FreeValue(ctx, v);
            }
            JS_FreeCString(ctx, s);
            JS_FreeAtom(ctx, tab[i].atom);
        }
        js_free(ctx, tab);
    }
    /* WHERE THE PLATFORM PUT ITS MEMBERS. WebIDL puts operations and
     * attributes on the interface prototype object; in a real browser
     * Object.getOwnPropertyNames(document) is ["location"] and navigator,
     * screen, performance, history and a fresh <div> own NOTHING. Anything
     * printed here beyond that is a member installed on the instance, which a
     * script reading Interface.prototype.member finds missing -- the shape of
     * Cloudflare's `new Proxy(Document.prototype.querySelectorAll, ...)`. */
    static const char *const owners[] = { "document", "navigator", "screen", "performance",
        "history", "document.createElement('div')", "document.body", "window.visualViewport" };
    for (unsigned oi = 0; oi < sizeof owners / sizeof owners[0]; oi++) {
        JSValue o = JS_Eval(ctx, owners[oi], strlen(owners[oi]), "<probe>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(o)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        if (JS_IsObject(o)) {
            JSPropertyEnum *ot; uint32_t on;
            if (JS_GetOwnPropertyNames(ctx, &ot, &on, o, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) == 0) {
                char line[2048]; int used = snprintf(line, sizeof line, "[probe] own %s (%u):", owners[oi], on);
                for (uint32_t i = 0; i < on; i++) {
                    const char *nm = JS_AtomToCString(ctx, ot[i].atom);
                    if (nm && used < (int)sizeof line - 64) used += snprintf(line + used, sizeof line - used, " %s", nm);
                    JS_FreeCString(ctx, nm);
                    JS_FreeAtom(ctx, ot[i].atom);
                }
                js_free(ctx, ot);
                printf("%s%s\n", line, used >= (int)sizeof line - 64 ? " ..." : "");
            }
        }
        JS_FreeValue(ctx, o);
    }
    JS_FreeValue(ctx, g);
    JS_SetProbeHooks(rt, on_miss, on_throw, NULL);
    JS_SetProbeNote(rt, on_note);
    g_building = 0;
    printf("[probe] installed %s: %d platform globals, %d intrinsic names excluded, %d objects tracked\n",
           prefix && *prefix ? prefix : "page", platform, nintrinsic, g_nobjs);
}

void js_probe_report(void)
{
    if (!g_nseen) return;
    printf("[probe] summary: %d distinct events%s\n", g_nseen,
           g_dropped ? " (table full -- some dropped)" : "");
    for (int i = 0; i < PROBE_SEEN; i++)
        if (g_seen[i].count) printf("[probe] n=%u %s\n", g_seen[i].count, g_seen[i].text);
    printf("[probe] summary end\n");
}

void js_probe_reset(JSContext *ctx)
{
    /* Called before the context that owns the tracked objects is freed. */
    if (ctx != g_owner) return;
    js_probe_report();
    for (int i = 0; i < PROBE_OBJS; i++)
        if (g_objs[i].p) { JS_FreeValue(ctx, g_objs[i].keep); g_objs[i].p = NULL; }
    g_nobjs = 0;
    memset(g_seen, 0, sizeof g_seen);
    g_nseen = g_dropped = 0;
    g_owner = NULL;
}

#endif /* JS_PROBE_TRACE */
