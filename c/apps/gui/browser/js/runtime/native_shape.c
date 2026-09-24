/* native_shape.c -- platform functions that look native.
 *
 * WHY. About forty JS preludes build this browser's platform ("<platform>",
 * "<select>", "<js_events>" ... -- every JS_Eval under c/apps/gui/browser/js),
 * and every function they hand the page used to be a PAGE-SHAPED function:
 * Function.prototype.toString returned its source text, it carried an own
 * `prototype`, `new` worked on it, and its name was often '' or 'value' (an
 * artifact of `{ value: function () {} }`). A browser's platform functions
 * are none of those things -- "function querySelector() { [native code] }",
 * no `prototype`, not a constructor, WebIDL's name -- and anti-tamper scripts
 * (Cloudflare's challenge among them) read exactly these properties. ECMA-262
 * lets the host give its built-in functions the NativeFunction form, and to
 * the page these ARE built-ins; the engine half of that is QuickJS's
 * JS_MarkNativeFunction (see quickjs.h), and this file is the pass that
 * decides what to mark.
 *
 * WHEN. Once per realm, at the end of js_page_open -- after js_anim_install,
 * the last installer, and before the JS_PROBE_TRACE probe snapshots the
 * shape -- and in a worker realm after its globals and before its script.
 * No page code has run in the realm at that moment, which is what makes the
 * first step safe:
 *
 * 1. JS_MarkNativeCode(ctx, JS_NATIVE_CODE_OPERATIONS): every function CODE
 *    the platform compiled in this realm so far is host code. The mark is on
 *    the code, so it also covers closures the platform creates later --
 *    per-instance methods, the wrappers it puts around page callbacks,
 *    whatever a getter hands out -- none of which a walk over today's objects
 *    could reach: they stringify as native code, and they are BORN with an
 *    operation's shape (no `prototype`, no [[Construct]]) unless a capital
 *    name says constructor (see quickjs.h, and tests/native_shape.mk for the
 *    convention that rests on). Code eval or the Function constructor
 *    compiled is excluded: the cssom shim compiles a page's
 *    <body onload="..."> with `new Function` at install, and that is page
 *    code (measured: without the exclusion the page's own functions read as
 *    native and lost [[Construct]]). This step changes no EXISTING object's
 *    shape; step 3 does that.
 *
 * 2. THE WALK. Which objects are the platform's is the same answer
 *    js/runtime/probe.c gives: a global name a bare QuickJS context does not
 *    have, its value, a constructor's .prototype, and -- because a walk that
 *    stopped there would miss document's and navigator's prototypes and
 *    every nested namespace -- everything reachable from those through own
 *    data properties and [[Prototype]] links, to a bounded depth. An Array
 *    is walked for its NAMED members only (navigator.plugins.item); its
 *    elements are data. The ES
 *    intrinsics are walked first and claimed as intrinsic, so the platform
 *    walk cannot mistake Object.prototype for a platform object; on an
 *    intrinsic object only BYTECODE functions count (QuickJS's own built-ins
 *    are all C, so a bytecode function there is a platform addition).
 *    The walk RUNS NOTHING: JS_GetOwnPropertyQuiet reads a descriptor
 *    without calling a getter, instantiating a lazy built-in, or entering a
 *    Proxy trap, and a Proxy is not walked at all.
 *
 * 3. THE MARKS, once the walk has seen every place a function lives, so a
 *    function reached first under a lowercase key and later as some
 *    prototype's `constructor` is classified on all the evidence:
 *      - an interface object (the `constructor` of an interface prototype, a
 *        key that starts with an uppercase letter, a C function built as a
 *        constructor): spelling only -- it keeps [[Construct]] and its
 *        `prototype`. Two kinds of evidence were measured to be FALSE and are
 *        not counted: the `constructor` link of the {constructor: f} object
 *        an ordinary function's `prototype` auto-creates (it exists for any
 *        function whose `prototype` something read), and a vendor prefix
 *        (`webkitMatchesSelector` is an operation) -- together they kept
 *        Element.prototype.matches constructible;
 *      - every other function-valued data property, and every getter and
 *        setter: the operation shape. QuickJS refuses to strip anything that
 *        was evidently used as a class, whatever this file asks. The verdict
 *        is recorded on the function's code as well, so the closures a
 *        factory makes after the pass follow the one it made before it.
 *      - names: '' / 'value' / 'get' / 'set' (what `{ value: function(){} }`
 *        and `{ get: function(){} }` produce) become KEY, 'get KEY' or
 *        'set KEY'; an accessor named bare 'KEY' (C getters made with
 *        JS_NewCFunction2(.., "width", ..)) becomes 'get KEY'. Any other
 *        name is left alone -- a function installed under two keys keeps the
 *        one it was given.
 *
 * WHAT IT DOES NOT DO: move members between prototypes (the shape pass, if
 * present, runs BEFORE this one -- see page.c), add or remove platform
 * members, or touch anything the page can create. Chrome-only identity
 * (window.chrome, a V8 stack format) is not a built-in's SHAPE and is not
 * this file's business.
 *
 * WHERE IT RUNS, AND WHERE IT DOES NOT. Every js_page_open realm (a network
 * iframe's included: frame/passive.c opens one) and every worker realm
 * (js/workers/worker.c). A js/dom/frame.c child realm needs nothing: it
 * installs C functions only. Host code the platform compiles AFTER the pass
 * is not marked -- today that is frame/window.inc's cross-origin
 * WindowProxy (`<embedded-window>`, made per iframe on first access), whose
 * getters still stringify as source. Per-instance methods made after the
 * pass by assignment (`arr.item = function (i) {...}`, js/dom/select.c) are
 * native and non-constructible but named '': their WebIDL home is a
 * prototype, which is member placement, not this file.
 *
 * -DNATIVE_SHAPE_NEGCTL_SKIP compiles the pass to a no-op that says so: the
 * negative control of test-native-shape. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quickjs.h"
#include "js/runtime/native_shape.h"

/* What the pass costs, printed on its one line: it runs on every page open. */
static double ns_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

#define NS_MAX_DEPTH_INTRINSIC 8
#define NS_MAX_DEPTH_PLATFORM  6
#define NS_MAX_OBJECTS         80000

enum { NS_INTRINSIC = 0, NS_PLATFORM = 1 };
enum { NS_METHOD = 0, NS_GET = 1, NS_SET = 2 };

struct ns_fn {
    JSValue fn;
    JSAtom key;
    unsigned char kind, ctor;
};

struct ns_item {
    JSValue obj;
    unsigned char mode, depth;
};

struct ns {
    JSContext *ctx;
    const void **seen;  size_t seen_cap, seen_n;
    struct ns_fn *fns;  size_t fn_cap, fn_n;
    size_t *fn_slot;    size_t fn_slot_cap;      /* ptr -> index+1 */
    struct ns_item *q;  size_t q_cap, q_head, q_n;
    JSAtom a_constructor, a_name;
    const void *object_proto;   /* this realm's Object.prototype */
    int oom;
    struct js_native_shape_report rep;
};

/* ---- pointer hashing -------------------------------------------------- */

static size_t ns_hash(const void *p, size_t mask)
{
    unsigned long long h = (unsigned long long)(size_t)p >> 4;
    h *= 11400714819323198485ULL;
    return (size_t)(h >> 20) & mask;
}

/* 1 if p was already in the set, 0 if it was added, -1 on OOM. */
static int ns_seen_add(struct ns *s, const void *p)
{
    if ((s->seen_n + 1) * 2 > s->seen_cap) {
        size_t ncap = s->seen_cap ? s->seen_cap * 2 : 4096;
        const void **n = calloc(ncap, sizeof *n);
        if (!n) return -1;
        for (size_t i = 0; i < s->seen_cap; i++) {
            if (!s->seen[i]) continue;
            size_t k = ns_hash(s->seen[i], ncap - 1);
            while (n[k]) k = (k + 1) & (ncap - 1);
            n[k] = s->seen[i];
        }
        free(s->seen);
        s->seen = n;
        s->seen_cap = ncap;
    }
    size_t k = ns_hash(p, s->seen_cap - 1);
    while (s->seen[k]) {
        if (s->seen[k] == p) return 1;
        k = (k + 1) & (s->seen_cap - 1);
    }
    s->seen[k] = p;
    s->seen_n++;
    return 0;
}

static struct ns_fn *ns_fn_find(struct ns *s, const void *p)
{
    if (!s->fn_slot_cap) return NULL;
    size_t k = ns_hash(p, s->fn_slot_cap - 1);
    while (s->fn_slot[k]) {
        struct ns_fn *f = &s->fns[s->fn_slot[k] - 1];
        if (JS_VALUE_GET_PTR(f->fn) == p) return f;
        k = (k + 1) & (s->fn_slot_cap - 1);
    }
    return NULL;
}

static int ns_fn_slot_put(struct ns *s, size_t idx)
{
    size_t k = ns_hash(JS_VALUE_GET_PTR(s->fns[idx].fn), s->fn_slot_cap - 1);
    while (s->fn_slot[k]) k = (k + 1) & (s->fn_slot_cap - 1);
    s->fn_slot[k] = idx + 1;
    return 0;
}

/* ---- the evidence ------------------------------------------------------- */

static int ns_ctor_key(JSContext *ctx, JSAtom key)
{
    int r = 0;
    JSValue kv = JS_AtomToValue(ctx, key);
    if (JS_IsString(kv)) {
        const char *k = JS_ToCString(ctx, kv);
        if (k) {
            /* Not the vendor prefixes: `webkitMatchesSelector` is an
             * operation (measured -- a webkit-prefix rule kept
             * Element.prototype.matches constructible), and a prefixed
             * interface alias (webkitURL) is the same object as its
             * unprefixed interface, found there, or has methods on its
             * `prototype`, which the engine refuses to strip anyway. */
            if (k[0] >= 'A' && k[0] <= 'Z') r = 1;
            JS_FreeCString(ctx, k);
        }
    }
    JS_FreeValue(ctx, kv);
    return r;
}

/* Record one sighting of a function. ctor votes are OR'd over every place
 * the function lives; the key and kind are the FIRST sighting's. */
static void ns_note_fn(struct ns *s, JSValueConst fn, JSAtom key, int kind, int ctor)
{
    struct ns_fn *f = ns_fn_find(s, JS_VALUE_GET_PTR(fn));
    if (f) { f->ctor |= (unsigned char)ctor; return; }
    if (s->fn_n == s->fn_cap) {
        size_t ncap = s->fn_cap ? s->fn_cap * 2 : 2048;
        struct ns_fn *n = realloc(s->fns, ncap * sizeof *n);
        if (!n) { s->oom = 1; return; }
        s->fns = n;
        s->fn_cap = ncap;
    }
    if ((s->fn_n + 1) * 2 > s->fn_slot_cap) {
        size_t ncap = s->fn_slot_cap ? s->fn_slot_cap * 2 : 4096;
        size_t *n = calloc(ncap, sizeof *n);
        if (!n) { s->oom = 1; return; }
        free(s->fn_slot);
        s->fn_slot = n;
        s->fn_slot_cap = ncap;
        for (size_t i = 0; i < s->fn_n; i++) ns_fn_slot_put(s, i);
    }
    f = &s->fns[s->fn_n];
    f->fn = JS_DupValue(s->ctx, fn);
    f->key = JS_DupAtom(s->ctx, key);
    f->kind = (unsigned char)kind;
    f->ctor = (unsigned char)ctor;
    ns_fn_slot_put(s, s->fn_n);
    s->fn_n++;
}

static void ns_push(struct ns *s, JSValueConst obj, int mode, int depth)
{
    if (!JS_IsObject(obj)) return;
    if (depth > (mode == NS_INTRINSIC ? NS_MAX_DEPTH_INTRINSIC : NS_MAX_DEPTH_PLATFORM)) return;
    if (s->q_n == s->q_cap) {
        size_t ncap = s->q_cap ? s->q_cap * 2 : 1024;
        struct ns_item *n = realloc(s->q, ncap * sizeof *n);
        if (!n) { s->oom = 1; return; }
        s->q = n;
        s->q_cap = ncap;
    }
    s->q[s->q_n].obj = JS_DupValue(s->ctx, obj);
    s->q[s->q_n].mode = (unsigned char)mode;
    s->q[s->q_n].depth = (unsigned char)depth;
    s->q_n++;
}

/* Is `holder` an interface prototype object, as opposed to the object a
 * function's own `prototype` auto-creates -- {constructor: f} over
 * Object.prototype and nothing else? The second exists for EVERY ordinary
 * function whose `prototype` anything has read, and its `constructor` link
 * is no evidence at all (measured: Element.prototype.matches kept its
 * `prototype` and [[Construct]] because something had read
 * matches.prototype before the pass, and the walk then found matches again
 * as matches.prototype.constructor). */
static int ns_holder_is_interface_proto(struct ns *s, JSValueConst holder, uint32_t nkeys)
{
    if (nkeys > 1) return 1;
    JSValue p = JS_GetPrototype(s->ctx, holder);
    if (JS_IsException(p)) { JS_FreeValue(s->ctx, JS_GetException(s->ctx)); return 1; }
    int r = !(JS_IsObject(p) && JS_VALUE_GET_PTR(p) == s->object_proto);
    JS_FreeValue(s->ctx, p);
    return r;
}

/* A function found as a data property's value under `key`. `ctor_link` is
 * 1 when the key is `constructor` on an interface prototype object. */
static void ns_function_value(struct ns *s, JSValueConst fn, JSAtom key, int mode, int depth,
                              int ctor_link)
{
    JSContext *ctx = s->ctx;
    int bc = JS_IsBytecodeFunction(ctx, fn);
    if (mode == NS_INTRINSIC && !bc) {
        /* QuickJS's own built-in: not ours to mark. Its statics and its
         * prototype are still walked, for platform additions hung on them. */
        ns_push(s, fn, NS_INTRINSIC, depth + 1);
        return;
    }
    if (JS_IsProxy(fn)) return;   /* a platform Proxy (HTMLElement): its target is marked where it lives */
    int ctor = ctor_link || (key != s->a_constructor && ns_ctor_key(ctx, key)) ||
               (!bc && JS_IsConstructor(ctx, fn));
    ns_note_fn(s, fn, key, NS_METHOD, ctor);
    ns_push(s, fn, NS_PLATFORM, depth + 1);
}

/* A canonical array index ("0", "17"): an Array's elements are data, and
 * only its named members (navigator.plugins.item) are platform surface. */
static int ns_is_index_key(JSContext *ctx, JSAtom a)
{
    const char *k = JS_AtomToCString(ctx, a);
    if (!k) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    int r = k[0] >= '0' && k[0] <= '9' && (k[0] != '0' || !k[1]);
    for (const char *c = k; r && *c; c++) if (*c < '0' || *c > '9') r = 0;
    JS_FreeCString(ctx, k);
    return r;
}

static void ns_accessor_fn(struct ns *s, JSValueConst fn, JSAtom key, int kind, int mode)
{
    if (!JS_IsFunction(s->ctx, fn) || JS_IsProxy(fn)) return;
    if (mode == NS_INTRINSIC && !JS_IsBytecodeFunction(s->ctx, fn)) return;
    ns_note_fn(s, fn, key, kind, 0);
}

static void ns_walk_one(struct ns *s, JSValueConst obj, int mode, int depth)
{
    JSContext *ctx = s->ctx;
    JSPropertyEnum *tab;
    uint32_t n;

    if (JS_IsProxy(obj)) return;
    int is_array = JS_IsArray(ctx, obj) > 0;
    if (s->rep.objects >= NS_MAX_OBJECTS) return;
    s->rep.objects++;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    int holder_iface = -1;   /* computed on the first `constructor` key only */
    for (uint32_t i = 0; i < n; i++) {
        JSPropertyDescriptor d;
        if (is_array && ns_is_index_key(ctx, tab[i].atom)) { JS_FreeAtom(ctx, tab[i].atom); continue; }
        int k = JS_GetOwnPropertyQuiet(ctx, &d, obj, tab[i].atom);
        if (k == JS_QUIET_DATA) {
            if (JS_IsFunction(ctx, d.value)) {
                int link = 0;
                if (tab[i].atom == s->a_constructor) {
                    if (holder_iface < 0) holder_iface = ns_holder_is_interface_proto(s, obj, n);
                    link = holder_iface;
                }
                ns_function_value(s, d.value, tab[i].atom, mode, depth, link);
            } else if (JS_IsObject(d.value))
                ns_push(s, d.value, mode, depth + 1);
        } else if (k == JS_QUIET_ACCESSOR) {
            ns_accessor_fn(s, d.getter, tab[i].atom, NS_GET, mode);
            ns_accessor_fn(s, d.setter, tab[i].atom, NS_SET, mode);
        }
        if (k == JS_QUIET_DATA || k == JS_QUIET_ACCESSOR) {
            JS_FreeValue(ctx, d.value);
            JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter);
        }
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    /* The prototype chain, at the same depth: chains are short, and the
     * seen-set ends every one of them at the first object already walked. */
    JSValue proto = JS_GetPrototype(ctx, obj);
    if (JS_IsException(proto)) JS_FreeValue(ctx, JS_GetException(ctx));
    else { ns_push(s, proto, mode, depth); JS_FreeValue(ctx, proto); }
}

static void ns_drain(struct ns *s)
{
    while (s->q_head < s->q_n && !s->oom) {
        struct ns_item it = s->q[s->q_head++];
        int r = ns_seen_add(s, JS_VALUE_GET_PTR(it.obj));
        if (r < 0) s->oom = 1;
        else if (r == 0) ns_walk_one(s, it.obj, it.mode, it.depth);
        JS_FreeValue(s->ctx, it.obj);
    }
}

/* ---- the global names a bare QuickJS context has ------------------------ */

static char (*g_intr)[48];
static int g_nintr = -1;

/* Asked once per process, of a bare context in a runtime of its OWN: the
 * names are plain strings, and building the context inside the page's runtime
 * can trigger a collection of the whole page heap (measured: the pass's first
 * run cost 13.4 ms instead of ~1 ms in test-cache's runtime). */
static void ns_intrinsic_names(void)
{
    if (g_nintr >= 0) return;
    g_nintr = 0;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return;
    JSContext *bare = JS_NewContext(rt);
    if (!bare) { JS_FreeRuntime(rt); return; }
    JSValue bg = JS_GetGlobalObject(bare);
    JSPropertyEnum *tab;
    uint32_t n;
    if (JS_GetOwnPropertyNames(bare, &tab, &n, bg, JS_GPN_STRING_MASK) == 0) {
        g_intr = calloc(n ? n : 1, sizeof *g_intr);
        for (uint32_t i = 0; i < n; i++) {
            const char *nm = JS_AtomToCString(bare, tab[i].atom);
            if (nm && g_intr && strlen(nm) < sizeof g_intr[0])
                memcpy(g_intr[g_nintr++], nm, strlen(nm) + 1);
            if (nm) JS_FreeCString(bare, nm);
            JS_FreeAtom(bare, tab[i].atom);
        }
        js_free(bare, tab);
    }
    JS_FreeValue(bare, bg);
    JS_FreeContext(bare);
    JS_FreeRuntime(rt);
    if (getenv("NATIVE_SHAPE_TRACE")) {
        printf("[native-shape] %d intrinsic global names:", g_nintr);
        for (int i = 0; i < g_nintr; i++) printf(" %s", g_intr[i]);
        printf("\n");
    }
}

static int ns_is_intrinsic(JSContext *ctx, JSAtom a)
{
    const char *nm = JS_AtomToCString(ctx, a);
    int r = 0;
    if (!nm) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    for (int i = 0; i < g_nintr && !r; i++) if (!strcmp(g_intr[i], nm)) r = 1;
    JS_FreeCString(ctx, nm);
    return r;
}

/* ---- names ---------------------------------------------------------------- */

/* WebIDL's name for a function installed under `key` as `kind`, or NULL if
 * its current name is a real one and should be left alone. Caller frees. */
static char *ns_wanted_name(struct ns *s, struct ns_fn *f)
{
    JSContext *ctx = s->ctx;
    char *out = NULL;
    const char *cur = NULL;
    JSPropertyDescriptor d;
    int k = JS_GetOwnPropertyQuiet(ctx, &d, f->fn, s->a_name);
    if (k == JS_QUIET_DATA && JS_IsString(d.value)) cur = JS_ToCString(ctx, d.value);
    if (k == JS_QUIET_DATA || k == JS_QUIET_ACCESSOR) {
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
    }
    if (k != JS_QUIET_DATA && k != JS_QUIET_ABSENT) goto done;   /* a name accessor: leave it */

    JSValue kv = JS_AtomToValue(ctx, f->key);
    char base[160];
    base[0] = 0;
    if (JS_IsString(kv)) {
        const char *ks = JS_ToCString(ctx, kv);
        if (ks) { snprintf(base, sizeof base, "%s", ks); JS_FreeCString(ctx, ks); }
    } else if (JS_IsSymbol(kv)) {
        /* SetFunctionName: a symbol key names the function "[description]". */
        const char *ds = JS_AtomToCString(ctx, f->key);
        if (ds) { snprintf(base, sizeof base, "[%s]", ds); JS_FreeCString(ctx, ds); }
        else JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, kv);
    if (!base[0]) goto done;

    int artifact = !cur || !cur[0] || !strcmp(cur, "value") || !strcmp(cur, "get") || !strcmp(cur, "set");
    if (f->kind != NS_METHOD && cur && !strcmp(cur, base)) artifact = 1;   /* a getter named bare 'width' */
    if (f->ctor && cur && cur[0]) artifact = 0;                             /* an interface keeps its name */
    if (!artifact) goto done;
    size_t len = strlen(base) + 5;
    out = malloc(len);
    if (out) snprintf(out, len, "%s%s", f->kind == NS_GET ? "get " : f->kind == NS_SET ? "set " : "", base);
    if (out && cur && !strcmp(out, cur)) { free(out); out = NULL; }
done:
    if (cur) JS_FreeCString(ctx, cur);
    return out;
}

static void ns_apply(struct ns *s)
{
    JSContext *ctx = s->ctx;
    const char *trace = getenv("NATIVE_SHAPE_TRACE");   /* host harnesses: every rename, one line */
    for (size_t i = 0; i < s->fn_n; i++) {
        struct ns_fn *f = &s->fns[i];
        char *nm = ns_wanted_name(s, f);
        if (nm) {
            if (trace) {
                JSValue old = JS_GetPropertyStr(ctx, f->fn, "name");
                const char *o = JS_ToCString(ctx, old);
                printf("[native-shape] rename %s%s -> %s\n", JS_IsBytecodeFunction(ctx, f->fn) ? "js " : "c ",
                       o ? o : "?", nm);
                if (o) JS_FreeCString(ctx, o);
                JS_FreeValue(ctx, old);
            }
            if (JS_DefinePropertyValue(ctx, f->fn, s->a_name, JS_NewString(ctx, nm),
                                       JS_PROP_CONFIGURABLE) > 0)
                s->rep.renamed++;
            else
                JS_FreeValue(ctx, JS_GetException(ctx));
            free(nm);
        }
        int r = JS_MarkNativeFunction(ctx, f->fn, f->ctor ? JS_NATIVE_FN_CONSTRUCTOR : 0);
        if (f->ctor) s->rep.interfaces++;
        else if (r == 1) s->rep.operations++;
        else if (r == 0 && JS_IsBytecodeFunction(ctx, f->fn)) s->rep.kept_class++;
    }
    s->rep.functions = (int)s->fn_n;
}

static void ns_free(struct ns *s)
{
    for (size_t i = s->q_head; i < s->q_n; i++) JS_FreeValue(s->ctx, s->q[i].obj);
    for (size_t i = 0; i < s->fn_n; i++) {
        JS_FreeValue(s->ctx, s->fns[i].fn);
        JS_FreeAtom(s->ctx, s->fns[i].key);
    }
    free(s->q); free(s->fns); free(s->fn_slot); free((void *)s->seen);
    JS_FreeAtom(s->ctx, s->a_constructor);
    JS_FreeAtom(s->ctx, s->a_name);
}

int js_native_shape_install(JSContext *ctx, struct js_native_shape_report *out)
{
    struct ns s;
    memset(&s, 0, sizeof s);
    s.ctx = ctx;
    if (out) memset(out, 0, sizeof *out);
#ifdef NATIVE_SHAPE_NEGCTL_SKIP
    printf("[native-shape] SKIPPED (-DNATIVE_SHAPE_NEGCTL_SKIP, the negative control)\n");
    return 0;
#endif
    double t0 = ns_now_ms();
    s.rep.code = JS_MarkNativeCode(ctx, JS_NATIVE_CODE_OPERATIONS);
    ns_intrinsic_names();
    s.a_constructor = JS_NewAtom(ctx, "constructor");
    s.a_name = JS_NewAtom(ctx, "name");
    {
        JSValue o = JS_NewObject(ctx);
        JSValue op = JS_GetPrototype(ctx, o);
        if (JS_IsObject(op)) s.object_proto = JS_VALUE_GET_PTR(op);
        JS_FreeValue(ctx, op);
        JS_FreeValue(ctx, o);
    }

    JSValue g = JS_GetGlobalObject(ctx);
    JSPropertyEnum *tab;
    uint32_t n;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, g, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        tab = NULL; n = 0;
    }
    /* Phase 1, the intrinsics, drained completely before any platform
     * object is looked at: whatever they reach is theirs. The global object
     * is claimed for phase 2 FIRST, because one intrinsic name --
     * `globalThis` -- is the global itself; walked as intrinsic, it would
     * hand every platform object to phase 1, where a C function is not
     * ours to name (measured: screen.width's getter kept its bare name). */
    if (ns_seen_add(&s, JS_VALUE_GET_PTR(g)) < 0) s.oom = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (!ns_is_intrinsic(ctx, tab[i].atom)) continue;
        JSPropertyDescriptor d;
        int k = JS_GetOwnPropertyQuiet(ctx, &d, g, tab[i].atom);
        if (k == JS_QUIET_DATA) {
            if (JS_IsFunction(ctx, d.value)) ns_function_value(&s, d.value, tab[i].atom, NS_INTRINSIC, 0, 0);
            else ns_push(&s, d.value, NS_INTRINSIC, 0);
        }
        if (k == JS_QUIET_DATA || k == JS_QUIET_ACCESSOR) {
            JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
        }
    }
    ns_drain(&s);
    /* Phase 2, the platform: the global object itself and, through it,
     * every non-intrinsic global. The global's intrinsic-named properties
     * are skipped here -- phase 1 took them. */
    s.rep.objects++;
    for (uint32_t i = 0; i < n && !s.oom; i++) {
        if (ns_is_intrinsic(ctx, tab[i].atom)) continue;
        JSPropertyDescriptor d;
        int k = JS_GetOwnPropertyQuiet(ctx, &d, g, tab[i].atom);
        if (k == JS_QUIET_DATA) {
            if (JS_IsFunction(ctx, d.value)) ns_function_value(&s, d.value, tab[i].atom, NS_PLATFORM, 0, 0);
            else ns_push(&s, d.value, NS_PLATFORM, 0);
        } else if (k == JS_QUIET_ACCESSOR) {
            ns_accessor_fn(&s, d.getter, tab[i].atom, NS_GET, NS_PLATFORM);
            ns_accessor_fn(&s, d.setter, tab[i].atom, NS_SET, NS_PLATFORM);
        }
        if (k == JS_QUIET_DATA || k == JS_QUIET_ACCESSOR) {
            JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
        }
    }
    {
        JSValue gp = JS_GetPrototype(ctx, g);
        if (JS_IsException(gp)) JS_FreeValue(ctx, JS_GetException(ctx));
        else { ns_push(&s, gp, NS_PLATFORM, 0); JS_FreeValue(ctx, gp); }
    }
    ns_drain(&s);
    for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, tab[i].atom);
    if (tab) js_free(ctx, tab);
    JS_FreeValue(ctx, g);

    /* Phase 3: the marks, on all the evidence. */
    if (!s.oom) ns_apply(&s);
    printf("[native-shape] code=%d functions=%d operations=%d interfaces=%d kept-class=%d renamed=%d objects=%d ms=%.1f%s%s\n",
           s.rep.code, s.rep.functions, s.rep.operations, s.rep.interfaces, s.rep.kept_class,
           s.rep.renamed, s.rep.objects, ns_now_ms() - t0, s.oom ? " (OUT OF MEMORY: walk incomplete)" : "",
           s.rep.objects >= NS_MAX_OBJECTS ? " (OBJECT CAP: walk incomplete)" : "");
    if (out) *out = s.rep;
    int oom = s.oom;
    ns_free(&s);
    return oom ? -1 : 0;
}
