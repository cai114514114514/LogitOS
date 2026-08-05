/* M17 L4: JavaScript <-> DOM bindings.
 *
 * The page DOM (net/dom.c `struct node`) and QuickJS run in the same ring-3
 * address space, so JS manipulates the live DOM directly. Exposes:
 *   document.getElementById(id) / document.querySelector(sel) / document.body
 *   Element.textContent (get/set), .innerHTML (get), .tagName, .id,
 *           .getAttribute(n), .setAttribute(n,v)
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

/* selector match: "#id" | ".class" | "tag" */
static int matches(struct node *n, const char *sel)
{
    if (n->type != N_ELEM) return 0;
    if (sel[0] == '#') { const char *id = dom_attr(n, "id"); return id && ieq(id, sel + 1); }
    if (sel[0] == '.') {
        const char *cls = dom_attr(n, "class"); if (!cls) return 0;
        int wl = 0; while (sel[1 + wl]) wl++;
        const char *p = cls;
        while (*p) { while (*p == ' ') p++; const char *s = p; while (*p && *p != ' ') p++;
            if (p - s == wl && memcmp(s, sel + 1, (size_t)wl) == 0) return 1; }
        return 0;
    }
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

static const JSCFunctionListEntry elem_proto[] = {
    JS_CGETSET_DEF("textContent", el_get_text, el_set_text),
    JS_CGETSET_DEF("innerHTML", el_get_html, NULL),
    JS_CGETSET_DEF("tagName", el_get_tag, NULL),
    JS_CGETSET_DEF("id", el_get_id, NULL),
    JS_CFUNC_DEF("getAttribute", 1, el_getattr),
    JS_CFUNC_DEF("setAttribute", 2, el_setattr),
};

static JSClassDef elem_class = { "Element", elem_finalizer };

void js_dom_init(JSContext *ctx, struct node *root)
{
    g_root = root; g_dirty = 0;
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

    struct node *body = 0;
    if (root) for (struct node *c = root->first_child; c && !body; c = c->next) body = find_sel(c, "body");

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "getElementById", JS_NewCFunction(ctx, doc_getById, "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "querySelector", JS_NewCFunction(ctx, doc_qs, "querySelector", 1));
    JSValue bw = wrap(ctx, body);
    if (JS_IsException(bw)) bw = JS_NULL;     /* don't hand an exception value to SetPropertyStr */
    JS_SetPropertyStr(ctx, doc, "body", bw);
    JS_SetPropertyStr(ctx, g, "document", doc);
    JS_FreeValue(ctx, g);
}
