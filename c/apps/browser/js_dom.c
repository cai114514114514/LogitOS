/* M17 L4: JavaScript <-> DOM bindings.
 *
 * The page DOM (net/dom.c `struct node`) and QuickJS run in the same ring-3
 * address space, so JS manipulates the live DOM directly. Exposes:
 *   document.getElementById(id) / .querySelector(sel) / .body / .documentElement
 *         / .createElement(tag)
 *   Element.textContent (get/set), .innerHTML (get), .tagName, .id,
 *           .getAttribute(n), .setAttribute(n,v),
 *           .appendChild(c), .removeChild(c), .addEventListener(type,fn),
 *           .classList.add/remove/toggle/contains
 *   console.log/warn/error (only if the caller didn't install its own console)
 * Mutations set a dirty flag so the browser re-styles + re-lays-out + repaints. */
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include <string.h>
#include <stdlib.h>

void dom_free(struct node *);          /* net/dom.c */

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static struct node *g_root;
static int g_dirty;

int  js_dom_dirty(void) { return g_dirty; }
void js_dom_clear_dirty(void) { g_dirty = 0; }

static JSClassID elem_cid;

/* Wrappers don't hold a bare struct node*: textContent= frees whole subtrees
 * while scripts may still hold wrappers into them (UAF / double free). Each
 * wrapper owns a {node, epoch} handle; freeing children bumps g_dom_epoch so
 * every stale wrapper fails node_of() and reads as NULL instead. */
struct elem_handle { struct node *n; unsigned long epoch; };
static unsigned long g_dom_epoch;

static void elem_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    struct elem_handle *h = JS_GetOpaque(val, elem_cid);
    if (h) free(h);
}

/* ---- helpers ---- */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ieq(const char *a, const char *b)
{ while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; } return lc((unsigned char)*a) == lc((unsigned char)*b); }

static struct node *node_of(JSValueConst v)
{
    struct elem_handle *h = JS_GetOpaque(v, elem_cid);
    if (!h || h->epoch != g_dom_epoch) return 0;    /* stale wrapper: subtree was freed */
    return h->n;
}

static JSValue wrap(JSContext *ctx, struct node *n)
{
    if (!n) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, (int)elem_cid);
    if (JS_IsException(o)) return o;
    struct elem_handle *h = malloc(sizeof *h);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    h->n = n; h->epoch = g_dom_epoch;
    JS_SetOpaque(o, h);
    return o;
}

static int gather_text(struct node *n, char *out, int o, int max)
{
    if (n->type == N_TEXT && n->text)
        for (int i = 0; i < n->textlen && o < max - 1; i++) out[o++] = n->text[i];
    for (struct node *c = n->first_child; c; c = c->next) o = gather_text(c, out, o, max);
    return o;
}

static void free_children(struct node *n)
{
    struct node *c = n->first_child;
    if (!c) return;                           /* nothing freed: don't invalidate wrappers */
    while (c) { struct node *nx = c->next; dom_free(c); c = nx; }
    n->first_child = n->last_child = 0;
    g_dom_epoch++;                            /* invalidate wrappers into the freed subtree */
}

static void set_text(struct node *n, const char *s)
{
    free_children(n);
    int len = 0; while (s[len]) len++;
    char *buf = malloc(len + 1); if (!buf) return; memcpy(buf, s, len + 1);
    struct node *t = malloc(sizeof *t); if (!t) { free(buf); return; }
    memset(t, 0, sizeof *t);
    t->type = N_TEXT; t->text = buf; t->textlen = len; t->parent = n;
    n->first_child = n->last_child = t;
}

static void copy_into(char *dst, const char *src, int cap)
{ int j = 0; while (src[j] && j < cap - 1) { dst[j] = src[j]; j++; } dst[j] = 0; }

static void set_attr(struct node *n, const char *name, const char *val)
{
    for (int i = 0; i < n->nattr; i++)
        if (ieq(n->attrs[i].name, name)) { copy_into(n->attrs[i].val, val, 256); return; }
    struct attr *na = malloc((n->nattr + 1) * sizeof(struct attr));
    if (!na) return;
    if (n->attrs) { memcpy(na, n->attrs, n->nattr * sizeof(struct attr)); free(n->attrs); }
    n->attrs = na;
    struct attr *a = &n->attrs[n->nattr];
    int j = 0; while (name[j] && j < 31) { a->name[j] = (char)lc((unsigned char)name[j]); j++; } a->name[j] = 0;
    copy_into(a->val, val, 256);
    n->nattr++;
}

/* whitespace-separated word membership test ("a b c" contains "b") */
static int word_has(const char *cls, const char *w)
{
    if (!cls) return 0;
    int wl = 0; while (w[wl]) wl++;
    const char *p = cls;
    while (*p) { while (*p == ' ') p++; const char *s = p; while (*p && *p != ' ') p++;
        if (p - s == wl && memcmp(s, w, (size_t)wl) == 0) return 1; }
    return 0;
}

/* selector match: "#id" | ".class" | "tag" */
static int matches(struct node *n, const char *sel)
{
    if (n->type != N_ELEM) return 0;
    if (sel[0] == '#') { const char *id = dom_attr(n, "id"); return id && ieq(id, sel + 1); }
    if (sel[0] == '.') return word_has(dom_attr(n, "class"), sel + 1);
    return ieq(n->tag, sel);
}
static struct node *find_sel(struct node *n, const char *sel)
{
    if (matches(n, sel)) return n;
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find_sel(c, sel); if (r) return r; }
    return 0;
}
static struct node *find_id(struct node *n, const char *id)
{
    if (n->type == N_ELEM) { const char *v = dom_attr(n, "id"); if (v && ieq(v, id)) return n; }
    for (struct node *c = n->first_child; c; c = c->next) { struct node *r = find_id(c, id); if (r) return r; }
    return 0;
}

/* ---- document methods ---- */
static JSValue doc_getById(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 1 || !g_root) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    struct node *n = find_id(g_root, id); JS_FreeCString(ctx, id);
    return wrap(ctx, n);
}
static JSValue doc_qs(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 1 || !g_root) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]); if (!sel) return JS_NULL;
    struct node *n = 0;
    for (struct node *c = g_root->first_child; c && !n; c = c->next) n = find_sel(c, sel);
    JS_FreeCString(ctx, sel);
    return wrap(ctx, n);
}

/* ---- element accessors ---- */
static JSValue el_get_text(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t); if (!n) return JS_UNDEFINED;
    static char buf[8192]; int len = gather_text(n, buf, 0, sizeof buf); buf[len] = 0;
    return JS_NewString(ctx, buf);
}
static JSValue el_set_text(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t); if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (s) { set_text(n, s); JS_FreeCString(ctx, s); g_dirty = 1; }
    return JS_UNDEFINED;
}
static JSValue el_get_tag(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? JS_NewString(ctx, n->tag) : JS_UNDEFINED; }
static JSValue el_get_id(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); const char *v = n ? dom_attr(n, "id") : 0; return JS_NewString(ctx, v ? v : ""); }
static JSValue el_get_html(JSContext *ctx, JSValueConst t) { return el_get_text(ctx, t); }
static JSValue el_getattr(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]); if (!nm) return JS_NULL;
    const char *v = dom_attr(n, nm); JS_FreeCString(ctx, nm);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}
static JSValue el_setattr(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 2) return JS_UNDEFINED;
    const char *nm = JS_ToCString(ctx, argv[0]);
    const char *vl = JS_ToCString(ctx, argv[1]);
    if (nm && vl) { set_attr(n, nm, vl); g_dirty = 1; }
    if (nm) JS_FreeCString(ctx, nm);
    if (vl) JS_FreeCString(ctx, vl);
    return JS_UNDEFINED;
}

/* ---- tree mutation: createElement / appendChild / removeChild ---- */
static JSValue doc_createElement(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]); if (!tag) return JS_NULL;
    struct node *n = malloc(sizeof *n);
    if (n) {
        memset(n, 0, sizeof *n);
        n->type = N_ELEM;
        int j = 0; while (tag[j] && j < 15) { n->tag[j] = (char)lc((unsigned char)tag[j]); j++; } n->tag[j] = 0;
    }
    JS_FreeCString(ctx, tag);
    return wrap(ctx, n);
}

static void unlink_node(struct node *c)
{
    struct node *p = c->parent; if (!p) return;
    struct node **pp = &p->first_child;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    if (p->last_child == c) {
        p->last_child = 0;
        for (struct node *k = p->first_child; k; k = k->next) p->last_child = k;
    }
    c->parent = 0; c->next = 0;
}

static JSValue el_appendChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c || c == n) return JS_NULL;
    unlink_node(c);                            /* move semantics: detach from any old parent */
    c->parent = n; c->next = 0;
    if (n->last_child) n->last_child->next = c; else n->first_child = c;
    n->last_child = c;
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);          /* like the DOM: return the appended child */
}

static JSValue el_removeChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c || c->parent != n) return JS_NULL;
    unlink_node(c);
    dom_free(c);
    g_dom_epoch++;                             /* invalidate wrappers into the freed subtree */
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);
}

/* ---- classList (DOMTokenList over the class attribute) ---- */
static JSClassID token_cid;

static JSValue el_get_classlist(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t); if (!n) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, (int)token_cid);
    if (JS_IsException(o)) return o;
    struct elem_handle *h = malloc(sizeof *h);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    h->n = n; h->epoch = g_dom_epoch;
    JS_SetOpaque(o, h);
    return o;
}

/* shared arg decode: token list receiver + class name from argv[0] */
static struct node *token_args(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                               const char **name)
{
    *name = 0;
    if (argc < 1) return 0;
    struct elem_handle *h = JS_GetOpaque(t, token_cid);
    if (!h || h->epoch != g_dom_epoch) return 0;
    *name = JS_ToCString(ctx, argv[0]);
    return *name ? h->n : 0;
}

static JSValue cl_contains(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_FALSE;
    int has = word_has(dom_attr(n, "class"), nm);
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, has);
}
static JSValue cl_add(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_UNDEFINED;
    const char *cls = dom_attr(n, "class");
    if (!word_has(cls, nm)) {
        char buf[256];
        int j = 0;
        if (cls) for (int i = 0; cls[i] && j < 253; i++) buf[j++] = cls[i];
        if (j) buf[j++] = ' ';
        for (int i = 0; nm[i] && j < 255; i++) buf[j++] = nm[i];
        buf[j] = 0;
        set_attr(n, "class", buf); g_dirty = 1;
    }
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}
static JSValue cl_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_UNDEFINED;
    const char *cls = dom_attr(n, "class");
    if (word_has(cls, nm)) {
        char buf[256]; int j = 0; int wl = 0; while (nm[wl]) wl++;
        const char *p = cls;
        while (*p) {
            while (*p == ' ') p++;
            const char *s = p; while (*p && *p != ' ') p++;
            if (!(p - s == wl && memcmp(s, nm, (size_t)wl) == 0)) {
                if (j) buf[j++] = ' ';
                for (const char *k = s; k < p && j < 255; k++) buf[j++] = *k;
            }
        }
        buf[j] = 0;
        set_attr(n, "class", buf); g_dirty = 1;
    }
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}
static JSValue cl_toggle(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int has = JS_ToBool(ctx, cl_contains(ctx, t, argc, argv));
    if (has) cl_remove(ctx, t, argc, argv); else cl_add(ctx, t, argc, argv);
    return JS_NewBool(ctx, !has);
}

static const JSCFunctionListEntry token_proto[] = {
    JS_CFUNC_DEF("add", 1, cl_add),
    JS_CFUNC_DEF("remove", 1, cl_remove),
    JS_CFUNC_DEF("toggle", 1, cl_toggle),
    JS_CFUNC_DEF("contains", 1, cl_contains),
};

static JSClassDef token_class = { "DOMTokenList", elem_finalizer };

/* ---- addEventListener: record-only (no event dispatch yet) ----
 * Listeners are kept so scripts attaching handlers don't throw. Each entry
 * holds a JS_DupValue'd ref on the handler, so js_dom_cleanup() MUST run
 * before the page's context/runtime is freed (JS_FreeRuntime asserts on
 * live GC objects). */
#define MAX_LISTENERS 64
static struct { struct node *n; unsigned long epoch; char type[32]; JSValue fn; }
    g_listeners[MAX_LISTENERS];
static int g_nlisteners;

static JSValue el_addEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    const char *ty = JS_ToCString(ctx, argv[0]); if (!ty) return JS_UNDEFINED;
    if (g_nlisteners < MAX_LISTENERS) {
        int i = g_nlisteners++;
        g_listeners[i].n = n; g_listeners[i].epoch = g_dom_epoch;
        int j = 0; while (ty[j] && j < 31) { g_listeners[i].type[j] = ty[j]; j++; }
        g_listeners[i].type[j] = 0;
        g_listeners[i].fn = JS_DupValue(ctx, argv[1]);
    }
    JS_FreeCString(ctx, ty);
    return JS_UNDEFINED;
}
/* queryable from tests / future event dispatch */
int js_dom_listener_count(void) { return g_nlisteners; }

/* Free the dup'd listener handler refs. Call before JS_FreeContext on the
 * context js_dom_init() was last run with. */
void js_dom_cleanup(JSContext *ctx)
{
    for (int i = 0; i < g_nlisteners; i++) JS_FreeValue(ctx, g_listeners[i].fn);
    g_nlisteners = 0;
}

static const JSCFunctionListEntry elem_proto[] = {
    JS_CGETSET_DEF("textContent", el_get_text, el_set_text),
    JS_CGETSET_DEF("innerHTML", el_get_html, NULL),
    JS_CGETSET_DEF("tagName", el_get_tag, NULL),
    JS_CGETSET_DEF("id", el_get_id, NULL),
    JS_CGETSET_DEF("classList", el_get_classlist, NULL),
    JS_CFUNC_DEF("getAttribute", 1, el_getattr),
    JS_CFUNC_DEF("setAttribute", 2, el_setattr),
    JS_CFUNC_DEF("appendChild", 1, el_appendChild),
    JS_CFUNC_DEF("removeChild", 1, el_removeChild),
    JS_CFUNC_DEF("addEventListener", 2, el_addEventListener),
};

static JSClassDef elem_class = { "Element", elem_finalizer };

/* ---- console.log/warn/error ----
 * Installed only when the caller hasn't provided its own `console` (browser.c's
 * run_js installs a richer one that also mirrors into the status bar). Output
 * goes to printf -- the serial console in the OS, stdout in host tests. */
int printf(const char *, ...);

static JSValue con_log(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) continue;
        printf("%s%s", i ? " " : "", s);
        JS_FreeCString(ctx, s);
    }
    printf("\n");
    return JS_UNDEFINED;
}
static JSValue con_warn(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ printf("[warn] "); return con_log(ctx, t, argc, argv); }
static JSValue con_error(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ printf("[error] "); return con_log(ctx, t, argc, argv); }

static void maybe_install_console(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue existing = JS_GetPropertyStr(ctx, g, "console");
    if (JS_IsUndefined(existing)) {
        JSValue con = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, con, "log", JS_NewCFunction(ctx, con_log, "log", 1));
        JS_SetPropertyStr(ctx, con, "warn", JS_NewCFunction(ctx, con_warn, "warn", 1));
        JS_SetPropertyStr(ctx, con, "error", JS_NewCFunction(ctx, con_error, "error", 1));
        JS_SetPropertyStr(ctx, g, "console", con);
    }
    JS_FreeValue(ctx, existing);
    JS_FreeValue(ctx, g);
}

/* document.body / .documentElement are getters, not captured wrappers: a wrapper
 * made at init goes stale as soon as any subtree free bumps g_dom_epoch (e.g.
 * after one textContent=, document.body would read as NULL). Resolving per
 * access keeps them valid for the page's lifetime. */
static JSValue doc_get_body(JSContext *ctx, JSValueConst t)
{
    (void)t;
    struct node *body = 0;
    if (g_root) for (struct node *c = g_root->first_child; c && !body; c = c->next) body = find_sel(c, "body");
    return wrap(ctx, body);
}
static JSValue doc_get_docel(JSContext *ctx, JSValueConst t)
{
    (void)t;
    struct node *docel = 0;
    if (g_root) {
        for (struct node *c = g_root->first_child; c && !docel; c = c->next) docel = find_sel(c, "html");
        if (!docel)                       /* no <html>: documentElement = first top-level element */
            for (struct node *c = g_root->first_child; c && !docel; c = c->next)
                if (c->type == N_ELEM) docel = c;
    }
    return wrap(ctx, docel);
}

static const JSCFunctionListEntry doc_funcs[] = {
    JS_CFUNC_DEF("getElementById", 1, doc_getById),
    JS_CFUNC_DEF("querySelector", 1, doc_qs),
    JS_CFUNC_DEF("createElement", 1, doc_createElement),
    JS_CGETSET_DEF("body", doc_get_body, NULL),
    JS_CGETSET_DEF("documentElement", doc_get_docel, NULL),
};

void js_dom_init(JSContext *ctx, struct node *root)
{
    g_root = root; g_dirty = 0; g_nlisteners = 0;
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* class ids index per-runtime arrays: run_js() builds a fresh JSRuntime for
     * every page, so the class must be registered on each init. The old
     * `if (!elem_cid)` guard reused the first runtime's id -> out-of-bounds
     * access on ctx->class_proto[] from the second page on. */
    JS_NewClassID(&elem_cid);
    if (JS_NewClass(rt, elem_cid, &elem_class) < 0) return;
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, elem_proto, countof(elem_proto));
    JS_SetClassProto(ctx, elem_cid, proto);

    JS_NewClassID(&token_cid);
    if (JS_NewClass(rt, token_cid, &token_class) >= 0) {
        JSValue tp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, tp, token_proto, countof(token_proto));
        JS_SetClassProto(ctx, token_cid, tp);
    }

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, doc, doc_funcs, countof(doc_funcs));
    JS_SetPropertyStr(ctx, g, "document", doc);
    JS_FreeValue(ctx, g);
    maybe_install_console(ctx);
}
