/* M17 L4: JavaScript <-> DOM bindings.
 *
 * The page DOM (net/dom.c `struct node`) and QuickJS run in the same ring-3
 * address space, so JS manipulates the live DOM directly. Exposes:
 *   document.getElementById(id) / .querySelector(sel) / .body / .documentElement
 *         / .createElement(tag)
 *   Element.textContent (get/set), .innerHTML (get/set), .tagName, .id,
 *           .getAttribute(n), .setAttribute(n,v),
 *           .appendChild(c), .removeChild(c), .addEventListener(type,fn),
 *           .classList.add/remove/toggle/contains
 *   console.log/warn/error (only if the caller didn't install its own console)
 * Mutations set a dirty flag so the browser re-styles + re-lays-out + repaints. */
#include "quickjs.h"
#include "dom.h"
#include "dom_serialize.h"
#include "html_tree.h"
#include "js_dom.h"
#include <string.h>
#include <stdlib.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static struct node *g_root;
static int g_dirty;

int  js_dom_dirty(void) { return g_dirty; }
void js_dom_clear_dirty(void) { g_dirty = 0; }

static JSClassID elem_cid;

/* Wrappers don't hold a bare struct node*: textContent= recycles whole subtrees
 * while scripts may still hold wrappers into them (UAF / double free). Each
 * wrapper owns a {node, serial} handle and node_of() checks the serial.
 *
 * The serial is PER NODE (dom.c stamps every slot it hands out), replacing the
 * old global epoch counter: that one invalidated EVERY live wrapper on any
 * subtree free, so a script that did one textContent= lost document.body. It is
 * also what makes the DOM's node free list safe -- a recycled slot gets a fresh
 * serial, so an old handle can never silently address the new occupant. */
struct elem_handle { struct node *n; uint32_t serial; };

static void elem_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    struct elem_handle *h = JS_GetOpaque(val, elem_cid);
    if (!h) return;
    /* Clear the node's weak wrapper slot, but only if it still points at THIS
     * object: after a recycle the slot may belong to a different node, and a
     * DOMTokenList shares this finalizer without ever owning a slot.
     * Contract: the DOM must outlive the JSRuntime (browser.c frees the page
     * DOM only after run_js has torn its runtime down). */
    if (h->n && h->n->serial == h->serial && h->n->jsw == JS_VALUE_GET_PTR(val))
        dom_set_wrapper(h->n, NULL);
    free(h);
}

/* ---- helpers ---- */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ieq(const char *a, const char *b)
{ while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; } return lc((unsigned char)*a) == lc((unsigned char)*b); }

static struct node *node_of(JSValueConst v)
{
    struct elem_handle *h = JS_GetOpaque(v, elem_cid);
    if (!h || !h->n || h->n->serial != h->serial) return 0;   /* stale: node recycled */
    return h->n;
}

static struct elem_handle *new_handle(struct node *n)
{
    struct elem_handle *h = malloc(sizeof *h);
    if (h) { h->n = n; h->serial = n->serial; }
    return h;
}

/* One wrapper per node, cached in the node's weak `jsw` slot, so
 * document.body === document.body and a script can hang expandos off an
 * element. The slot takes no reference: the finalizer clears it, and
 * js_dom_cleanup() clears every slot in the document when the page's runtime
 * goes away. */
static JSValue wrap(JSContext *ctx, struct node *n)
{
    if (!n) return JS_NULL;
    if (n->jsw) return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, n->jsw));
    JSValue o = JS_NewObjectClass(ctx, (int)elem_cid);
    if (JS_IsException(o)) return o;
    struct elem_handle *h = new_handle(n);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    JS_SetOpaque(o, h);
    dom_set_wrapper(n, JS_VALUE_GET_PTR(o));
    return o;
}

/* ---- a growable byte buffer: textContent has no business being capped ---- */
struct sbuf { char *p; size_t len, cap; };

static int sb_push(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 256;
        while (ncap < b->len + n + 1) ncap *= 2;
        char *np = realloc(b->p, ncap);
        if (!np) return 0;
        b->p = np; b->cap = ncap;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 1;
}

/* Iterative: a script can build a tree far deeper than a recursive gather
 * would survive on the browser's stack. */
static void gather_text(struct node *root, struct sbuf *b)
{
    struct node *n = root;
    while (n) {
        if (n->type == N_TEXT && n->text) sb_push(b, n->text, (size_t)n->textlen);
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) break;
        n = n->next;
    }
}

static void set_text(struct node *n, const char *s)
{
    dom_destroy_children(n);
    struct node *t = dom_create_text(n->doc, s, -1);
    if (t) dom_append_child(n, t);
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
/* ---- document methods ---- */
static JSValue doc_getById(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 1 || !g_root) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    /* The document's id index, not a tree walk: getElementById is O(1) and,
     * per the DOM spec, case-sensitive (the old walk compared case-insensitively). */
    struct node *n = dom_get_element_by_id(g_root->doc, id);
    JS_FreeCString(ctx, id);
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
    struct sbuf b = { 0, 0, 0 };
    gather_text(n, &b);
    JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.len);
    free(b.p);
    return v;
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
/* Real innerHTML, both ways. The getter used to alias el_get_text, which is a
 * different property entirely: it returns the concatenated text with every tag
 * stripped, so a script that read innerHTML got markup-free text back and any
 * round trip (el.innerHTML = el.innerHTML) silently destroyed the subtree. */
static JSValue el_get_html(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t); if (!n) return JS_UNDEFINED;
    char *s = dom_serialize_html(n, 0);          /* children only == innerHTML */
    JSValue v = JS_NewString(ctx, s ? s : "");
    free(s);
    return v;
}

static JSValue el_set_html(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t); if (!n) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_UNDEFINED;

    /* The HTML fragment parsing algorithm, with this element as context -- so
     * "<td>x" inside a <tr> builds a cell and inside a <div> does not, exactly
     * as the spec requires. It parses into its OWN document, so the result has
     * to be imported (deep-copied) before that document is dropped. */
    struct dom_doc *fdoc = 0;
    struct node *frag = html_parse_fragment(&fdoc, s, (int)strlen(s),
                                            n->tag, (int)strlen(n->tag), n->ns);
    JS_FreeCString(ctx, s);
    if (frag) {
        dom_destroy_children(n);
        for (struct node *c = frag->first_child; c; c = c->next) {
            struct node *cp = dom_import_node(n->doc, c);
            if (cp) dom_append_child(n, cp);
        }
        g_dirty = 1;
    }
    if (fdoc) dom_free(dom_doc_root(fdoc));
    return JS_UNDEFINED;
}
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
    if (nm && vl) { dom_set_attr(n, nm, vl); g_dirty = 1; }
    if (nm) JS_FreeCString(ctx, nm);
    if (vl) JS_FreeCString(ctx, vl);
    return JS_UNDEFINED;
}

/* ---- tree mutation: createElement / appendChild / removeChild ---- */
static JSValue doc_createElement(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 1 || !g_root) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]); if (!tag) return JS_NULL;
    /* Created in the page's document so it shares the arena and the id index;
     * it stays detached (and so invisible to getElementById) until appended. */
    struct node *n = dom_create_element(g_root->doc, tag, -1);
    JS_FreeCString(ctx, tag);
    return wrap(ctx, n);
}

static JSValue el_appendChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c || c == n) return JS_NULL;
    dom_append_child(n, c);                    /* move semantics: detaches from any old parent */
    g_dirty = 1;
    return JS_DupValue(ctx, argv[0]);          /* like the DOM: return the appended child */
}

static JSValue el_removeChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c || c->parent != n) return JS_NULL;
    dom_destroy_subtree(c);                    /* wrappers into it go stale via serial */
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
    struct elem_handle *h = new_handle(n);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    JS_SetOpaque(o, h);
    return o;                                  /* not cached in n->jsw: that slot
                                                * belongs to the Element wrapper */
}

/* shared arg decode: token list receiver + class name from argv[0] */
static struct node *token_args(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                               const char **name)
{
    *name = 0;
    if (argc < 1) return 0;
    struct elem_handle *h = JS_GetOpaque(t, token_cid);
    if (!h || !h->n || h->n->serial != h->serial) return 0;
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
/* Both rebuild the class attribute through a growable buffer: real pages carry
 * class lists far past the 255 characters the old fixed buffers allowed, and
 * truncating one here would silently drop unrelated classes. */
static JSValue cl_add(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_UNDEFINED;
    const char *cls = dom_attr(n, "class");
    if (!word_has(cls, nm)) {
        struct sbuf b = { 0, 0, 0 };
        if (cls && *cls) { sb_push(&b, cls, strlen(cls)); sb_push(&b, " ", 1); }
        sb_push(&b, nm, strlen(nm));
        if (b.p) { dom_set_attr(n, "class", b.p); g_dirty = 1; }
        free(b.p);
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
        struct sbuf b = { 0, 0, 0 };
        size_t wl = strlen(nm);
        const char *p = cls;
        while (*p) {
            while (*p == ' ') p++;
            const char *s = p; while (*p && *p != ' ') p++;
            if (p == s) continue;
            if ((size_t)(p - s) == wl && memcmp(s, nm, wl) == 0) continue;
            if (b.len) sb_push(&b, " ", 1);
            sb_push(&b, s, (size_t)(p - s));
        }
        dom_set_attr(n, "class", b.p ? b.p : "");
        g_dirty = 1;
        free(b.p);
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
static struct { struct node *n; uint32_t serial; char type[32]; JSValue fn; }
    g_listeners[MAX_LISTENERS];
static int g_nlisteners;

static JSValue el_addEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    const char *ty = JS_ToCString(ctx, argv[0]); if (!ty) return JS_UNDEFINED;
    if (g_nlisteners < MAX_LISTENERS) {
        int i = g_nlisteners++;
        g_listeners[i].n = n; g_listeners[i].serial = n->serial;
        int j = 0; while (ty[j] && j < 31) { g_listeners[i].type[j] = ty[j]; j++; }
        g_listeners[i].type[j] = 0;
        g_listeners[i].fn = JS_DupValue(ctx, argv[1]);
    }
    JS_FreeCString(ctx, ty);
    return JS_UNDEFINED;
}
/* queryable from tests / future event dispatch */
int js_dom_listener_count(void) { return g_nlisteners; }

/* Free the dup'd listener handler refs AND drop every wrapper slot in the
 * document. Call before JS_FreeContext on the context js_dom_init() was last
 * run with.
 *
 * The wrapper part is not optional: browser.c's run_js() builds a fresh
 * JSRuntime for every page, so any node->jsw left behind points at a JSObject
 * in a runtime that no longer exists. The very next document.body on the next
 * page would hand that dangling pointer straight back to JS. */
void js_dom_cleanup(JSContext *ctx)
{
    for (int i = 0; i < g_nlisteners; i++) JS_FreeValue(ctx, g_listeners[i].fn);
    g_nlisteners = 0;
    if (g_root) dom_clear_wrappers(g_root->doc);
}

static const JSCFunctionListEntry elem_proto[] = {
    JS_CGETSET_DEF("textContent", el_get_text, el_set_text),
    JS_CGETSET_DEF("innerHTML", el_get_html, el_set_html),
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

/* document.body / .documentElement are getters, not wrappers captured at init:
 * a script may replace <body>'s subtree, and the getter must always resolve
 * against the live tree. (Under the old global epoch a captured wrapper also
 * went stale the moment ANY subtree was freed -- one textContent= and
 * document.body read as NULL. Per-node serials removed that failure mode, but
 * resolving per access is still the correct semantics.) The wrapper the getter
 * returns is the node's cached one, so document.body === document.body. */
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
    /* Defensive: if a previous page's runtime went away without a cleanup call,
     * its wrapper slots are dangling JSObject*s. Start every page with none. */
    if (root) dom_clear_wrappers(root->doc);
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
