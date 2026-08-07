/* M17 L4: JavaScript <-> DOM bindings.
 *
 * The page DOM (dom.c `struct node`) and QuickJS run in the same ring-3
 * address space, so JS manipulates the live DOM directly. Exposes:
 *   document.getElementById(id) / .querySelector(sel) / .body / .documentElement
 *         / .createElement(tag) / .addEventListener / .dispatchEvent
 *   Element.textContent (get/set), .innerHTML (get/set), .tagName, .id,
 *           .getAttribute(n), .setAttribute(n,v),
 *           .appendChild(c), .removeChild(c),
 *           .addEventListener(type,fn,opts) / .removeEventListener /
 *           .dispatchEvent, the on* handler properties,
 *           .classList.add/remove/toggle/contains
 *   Event / UIEvent / MouseEvent / KeyboardEvent, with the real three-phase
 *           dispatch (capture down the ancestor chain, target, bubble up),
 *           preventDefault / stopPropagation / stopImmediatePropagation
 *   console.log/warn/error (only if the caller didn't install its own console)
 * Mutations set a dirty flag so the browser re-styles + re-lays-out + repaints.
 *
 * The event half is what makes a loaded page LIVE, and it only works because
 * the JS runtime now outlives the load (js_page.c): a listener registered while
 * the page was parsing has to still be callable ten seconds later when the user
 * clicks. Everything here therefore assumes the context passed to js_dom_init()
 * stays alive until js_dom_cleanup() runs. */
#include "quickjs.h"
#include "dom.h"
#include "dom_serialize.h"
#include "html_tree.h"
#include "js_dom.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static struct node *g_root;
static int g_dirty;

/* The context the page is bound to. Dispatch is entered from C (a click, a
 * timer), not from JS, so there is no `ctx` argument to ride in on -- the
 * binding layer has to remember one. Exactly one page runtime exists at a time,
 * which is what makes a single static correct rather than a shortcut. */
static JSContext *g_ctx;

int  js_dom_dirty(void) { return g_dirty; }
void js_dom_clear_dirty(void) { g_dirty = 0; }
struct node *js_dom_root(void) { return g_root; }

static void (*g_note)(const char *);
void js_dom_set_note(void (*fn)(const char *)) { g_note = fn; }

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
     * object: after a recycle the slot may belong to a different node.
     * Contract: the DOM must outlive the JSRuntime (browser.c frees the page
     * DOM only after js_page_close has torn its runtime down). */
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

/* A DOMTokenList carries the same {node, serial} handle as an Element wrapper,
 * but NOT the same class id -- and JS_GetOpaque checks the class id. Sharing
 * elem_finalizer therefore handed it a NULL and leaked the handle on every
 * single `el.classList` access, which on a page that toggles classes in a loop
 * is unbounded growth. It needs its own unwrap. (It owns no jsw slot: that one
 * belongs to the Element wrapper, so there is nothing else to undo.) */
static void token_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    free(JS_GetOpaque(val, token_cid));
}

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

static JSClassDef token_class = { "DOMTokenList", token_finalizer };

/* ======================================================================
 * Events
 * ====================================================================== */

int printf(const char *, ...);

/* Report an uncaught exception thrown out of a handler. A listener that throws
 * must not abort the rest of the dispatch (that is what the DOM says, and it is
 * also the only way one bad handler doesn't kill the page), so this swallows
 * the exception after logging it. */
static void report_exc(JSContext *ctx, const char *where)
{
    JSValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    printf("[js] uncaught in %s: %s\n", where, m ? m : "?");
    if (g_note) { g_note("[exception] "); if (m) g_note(m); g_note("\n"); }
    if (m) JS_FreeCString(ctx, m);
    JS_FreeValue(ctx, e);
}

/* QuickJS never runs a queued job on its own -- JS_Eval only ENQUEUES promise
 * reactions and async-function resumptions. Everything that can re-enter script
 * (a script evaluation, an event callback, a timer) therefore has to drain the
 * queue afterwards, or `Promise.then` silently never fires and every `await`
 * stops at its first suspension point.
 *
 * The cap is not a spec behaviour; it is a liveness guard. A page that queues a
 * job from a job (`function f(){ Promise.resolve().then(f); } f();`) starves
 * this loop forever, and unlike a real browser we have one thread and no way to
 * kill the tab -- so past a cap we stop pumping for this turn and let the main
 * loop breathe. The page stays broken, but the OS does not. */
#define MAX_JOBS_PER_PUMP 100000

int js_dom_run_jobs(JSContext *ctx)
{
    if (!ctx) return 0;
    JSRuntime *rt = JS_GetRuntime(ctx);
    int n = 0;
    for (; n < MAX_JOBS_PER_PUMP; n++) {
        JSContext *jc = 0;
        int r = JS_ExecutePendingJob(rt, &jc);
        if (r == 0) break;                     /* queue empty */
        if (r < 0) { report_exc(jc ? jc : ctx, "promise job"); continue; }
    }
    if (n >= MAX_JOBS_PER_PUMP)
        printf("[js] microtask queue did not drain in %d jobs -- giving up this turn\n",
               MAX_JOBS_PER_PUMP);
    return n;
}

/* ---- the listener store ----
 *
 * One list per node, in registration order (which IS the dispatch order the DOM
 * specifies). It cannot be the old fixed 64-entry global -- a page attaches as
 * many handlers as it likes -- and it cannot be a field on `struct node`
 * either: dom.h is the parser's data model and has no business knowing QuickJS
 * exists. So the list hangs off the node through a small open hash keyed by the
 * node pointer, with the node's `serial` stored beside it.
 *
 * That serial is the safety property. dom_destroy_subtree() recycles slots
 * through the document's free list, so a node pointer alone can silently come
 * to mean a DIFFERENT element; every lookup re-checks the serial and drops the
 * whole bucket on a mismatch. A listener can therefore never be invoked with a
 * target that has been freed out from under it. */
struct listener {
    struct listener *next;
    JSValue fn;                 /* strong ref while registered */
    char   *type;               /* event type, exact case (DOM types are case-sensitive) */
    unsigned char capture;      /* runs in the capture phase */
    unsigned char once;         /* remove before invoking */
    unsigned char passive;      /* preventDefault() from this listener is ignored */
    unsigned char attr;         /* came from an on<type> property/attribute, so a
                                 * second assignment REPLACES it in place instead
                                 * of appending -- that is what makes
                                 * el.onclick = f; el.onclick = g run only g. */
    unsigned char removed;      /* unregistered; a live dispatch snapshot may still hold it */
    int refs;                   /* 1 for "in the list" + 1 per dispatch snapshot */
};

struct nlist {
    struct nlist *hnext;
    struct node  *n;
    uint32_t      serial;
    struct listener *head;
};

#define LBUCKETS 127
static struct nlist *g_lbucket[LBUCKETS];
static int g_lcount;                        /* live (registered) listeners */

static unsigned lhash(const struct node *n)
{ return (unsigned)((((uintptr_t)n) >> 4) % LBUCKETS); }

static void listener_unref(JSContext *ctx, struct listener *l)
{
    if (--l->refs > 0) return;
    if (ctx) JS_FreeValue(ctx, l->fn);
    free(l->type);
    free(l);
}

/* Unregister: unlink from the list, then drop the list's reference. A snapshot
 * that is mid-dispatch keeps the object alive but sees `removed` and skips it,
 * which is precisely the DOM's "the removed flag is checked at invoke time". */
static void listener_unlink(JSContext *ctx, struct nlist *e, struct listener *l)
{
    for (struct listener **pp = &e->head; *pp; pp = &(*pp)->next)
        if (*pp == l) { *pp = l->next; break; }
    if (l->removed) return;                  /* already unlinked once */
    l->removed = 1;
    g_lcount--;
    listener_unref(ctx, l);
}

static void nlist_free(JSContext *ctx, struct nlist *e)
{
    while (e->head) listener_unlink(ctx, e, e->head);
    free(e);
}

static struct nlist *nlist_for(JSContext *ctx, struct node *n, int create)
{
    struct nlist **pp = &g_lbucket[lhash(n)];
    while (*pp) {
        struct nlist *e = *pp;
        if (e->n == n) {
            if (e->serial == n->serial) return e;
            *pp = e->hnext;                  /* slot recycled: this bucket is a ghost */
            nlist_free(ctx, e);
            continue;
        }
        pp = &e->hnext;
    }
    if (!create) return 0;
    struct nlist *e = calloc(1, sizeof *e);
    if (!e) return 0;
    e->n = n; e->serial = n->serial;
    e->hnext = g_lbucket[lhash(n)];
    g_lbucket[lhash(n)] = e;
    return e;
}

/* Drop buckets whose node has been recycled. Cheap (127 heads) and run once per
 * dispatch, so listeners on a subtree a script deleted are reclaimed at the next
 * event instead of accumulating until the page unloads. */
static void nlist_prune(JSContext *ctx)
{
    for (int i = 0; i < LBUCKETS; i++) {
        struct nlist **pp = &g_lbucket[i];
        while (*pp) {
            struct nlist *e = *pp;
            if (e->n && e->serial == e->n->serial) { pp = &e->hnext; continue; }
            *pp = e->hnext;
            nlist_free(ctx, e);
        }
    }
}

static void nlist_free_all(JSContext *ctx)
{
    for (int i = 0; i < LBUCKETS; i++) {
        while (g_lbucket[i]) {
            struct nlist *e = g_lbucket[i];
            g_lbucket[i] = e->hnext;
            nlist_free(ctx, e);
        }
    }
    g_lcount = 0;
}

int js_dom_listener_count(void) { return g_lcount; }

static struct listener *listener_add(JSContext *ctx, struct node *n, const char *type,
                                     JSValueConst fn, int capture, int once,
                                     int passive, int attr)
{
    struct nlist *e = nlist_for(ctx, n, 1);
    if (!e) return 0;
    size_t tl = strlen(type);
    /* An on* assignment replaces the existing handler in place: the DOM keeps
     * the event handler at the position it was FIRST set, so onclick=a;
     * addEventListener(b); onclick=c still runs c before b. */
    if (attr)
        for (struct listener *l = e->head; l; l = l->next)
            if (l->attr && !l->capture && !strcmp(l->type, type)) {
                JS_FreeValue(ctx, l->fn);
                l->fn = JS_DupValue(ctx, fn);
                return l;
            }
    /* addEventListener is idempotent for an identical (type, callback, capture)
     * triple -- a page that registers the same handler twice must be called once. */
    if (!attr)
        for (struct listener *l = e->head; l; l = l->next)
            if (!l->attr && l->capture == (unsigned char)!!capture &&
                !strcmp(l->type, type) &&
                JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(fn) &&
                JS_VALUE_GET_TAG(l->fn) == JS_VALUE_GET_TAG(fn))
                return l;
    struct listener *l = calloc(1, sizeof *l);
    char *ty = malloc(tl + 1);
    if (!l || !ty) { free(l); free(ty); return 0; }
    memcpy(ty, type, tl + 1);
    l->type = ty;
    l->fn = JS_DupValue(ctx, fn);
    l->capture = (unsigned char)!!capture;
    l->once = (unsigned char)!!once;
    l->passive = (unsigned char)!!passive;
    l->attr = (unsigned char)!!attr;
    l->refs = 1;
    struct listener **pp = &e->head;
    while (*pp) pp = &(*pp)->next;           /* append: registration order */
    *pp = l;
    g_lcount++;
    return l;
}

static void listener_remove(JSContext *ctx, struct node *n, const char *type,
                            JSValueConst fn, int capture, int attr)
{
    struct nlist *e = nlist_for(ctx, n, 0);
    if (!e) return;
    for (struct listener *l = e->head; l; l = l->next)
        if (l->attr == (unsigned char)!!attr &&
            l->capture == (unsigned char)!!capture &&
            !strcmp(l->type, type) &&
            (attr || (JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(fn) &&
                      JS_VALUE_GET_TAG(l->fn) == JS_VALUE_GET_TAG(fn)))) {
            listener_unlink(ctx, e, l);
            return;
        }
}

/* ---- the Event objects ----
 *
 * One class id for Event/UIEvent/MouseEvent/KeyboardEvent with three
 * prototypes, so `e instanceof MouseEvent` answers correctly while
 * JS_GetOpaque(v, event_cid) still works on any of them. The alternative --
 * four class ids -- would mean four finalizers and four unwrap paths for one
 * payload struct. */
enum { EPHASE_NONE = 0, EPHASE_CAPTURING = 1, EPHASE_AT_TARGET = 2, EPHASE_BUBBLING = 3 };

struct nref { struct node *n; uint32_t serial; };

struct jsevent {
    char *type;
    struct nref target, current;
    int phase;
    unsigned char bubbles, cancelable, composed, trusted;
    unsigned char prevented, stop_prop, stop_imm, dispatching, in_passive;
    double timestamp;
    int detail;                                     /* UIEvent */
    int client_x, client_y, button, buttons;        /* MouseEvent */
    unsigned char shift, ctrl, alt, meta;
    double delta_x, delta_y;                        /* WheelEvent */
    char *key, *code;                               /* KeyboardEvent */
    int key_code;
    unsigned char repeat;
};

static JSClassID event_cid;
/* Strong refs: the prototypes have to be reachable from C to build an event
 * without a global lookup, and they are freed in js_dom_cleanup() -- BEFORE the
 * runtime goes away, which is the one ordering rule this whole file lives by. */
static JSValue g_proto_event, g_proto_ui, g_proto_mouse, g_proto_key;

static void event_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    struct jsevent *ev = JS_GetOpaque(val, event_cid);
    if (!ev) return;
    free(ev->type); free(ev->key); free(ev->code);
    free(ev);
}

static JSClassDef event_class = { "Event", event_finalizer };

static char *dupstr(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static struct nref nref_of(struct node *n)
{ struct nref r; r.n = n; r.serial = n ? n->serial : 0; return r; }

static struct node *nref_live(const struct nref *r)
{ return (r->n && r->n->serial == r->serial) ? r->n : 0; }

/* Build an event object on the requested prototype. Ownership of `ev` moves to
 * the object: the finalizer frees it, so every early return after this point
 * must free the JSValue, never the struct. */
static JSValue event_new(JSContext *ctx, JSValueConst proto, struct jsevent *ev)
{
    JSValue o = JS_NewObjectProtoClass(ctx, proto, event_cid);
    if (JS_IsException(o)) { free(ev->type); free(ev->key); free(ev->code); free(ev); return o; }
    JS_SetOpaque(o, ev);
    return o;
}

enum {
    EG_TYPE, EG_TARGET, EG_CURRENT, EG_PHASE, EG_BUBBLES, EG_CANCELABLE,
    EG_PREVENTED, EG_TRUSTED, EG_TIMESTAMP, EG_COMPOSED,
    EG_DETAIL,
    EG_CLIENTX, EG_CLIENTY, EG_PAGEX, EG_PAGEY, EG_SCREENX, EG_SCREENY,
    EG_BUTTON, EG_BUTTONS, EG_SHIFT, EG_CTRL, EG_ALT, EG_META,
    EG_DELTAX, EG_DELTAY, EG_DELTAMODE,
    EG_KEY, EG_CODE, EG_KEYCODE, EG_WHICH, EG_REPEAT
};

static JSValue ev_get(JSContext *ctx, JSValueConst t, int magic)
{
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    if (!e) return JS_UNDEFINED;
    switch (magic) {
    case EG_TYPE:       return JS_NewString(ctx, e->type);
    case EG_TARGET:     return wrap(ctx, nref_live(&e->target));
    case EG_CURRENT:    return wrap(ctx, nref_live(&e->current));
    case EG_PHASE:      return JS_NewInt32(ctx, e->phase);
    case EG_BUBBLES:    return JS_NewBool(ctx, e->bubbles);
    case EG_CANCELABLE: return JS_NewBool(ctx, e->cancelable);
    case EG_PREVENTED:  return JS_NewBool(ctx, e->prevented);
    case EG_TRUSTED:    return JS_NewBool(ctx, e->trusted);
    case EG_COMPOSED:   return JS_NewBool(ctx, e->composed);
    case EG_TIMESTAMP:  return JS_NewFloat64(ctx, e->timestamp);
    case EG_DETAIL:     return JS_NewInt32(ctx, e->detail);
    /* pageX/pageY differ from clientX/clientY by the scroll offset. Layout hands
     * us viewport coordinates and the page scroll lives in browser.c, so the two
     * are reported equal rather than guessed at. */
    case EG_CLIENTX: case EG_PAGEX: case EG_SCREENX: return JS_NewInt32(ctx, e->client_x);
    case EG_CLIENTY: case EG_PAGEY: case EG_SCREENY: return JS_NewInt32(ctx, e->client_y);
    case EG_BUTTON:     return JS_NewInt32(ctx, e->button);
    case EG_BUTTONS:    return JS_NewInt32(ctx, e->buttons);
    case EG_SHIFT:      return JS_NewBool(ctx, e->shift);
    case EG_CTRL:       return JS_NewBool(ctx, e->ctrl);
    case EG_ALT:        return JS_NewBool(ctx, e->alt);
    case EG_META:       return JS_NewBool(ctx, e->meta);
    case EG_DELTAX:     return JS_NewFloat64(ctx, e->delta_x);
    case EG_DELTAY:     return JS_NewFloat64(ctx, e->delta_y);
    case EG_DELTAMODE:  return JS_NewInt32(ctx, 0);        /* DOM_DELTA_PIXEL */
    case EG_KEY:        return JS_NewString(ctx, e->key ? e->key : "");
    case EG_CODE:       return JS_NewString(ctx, e->code ? e->code : "");
    case EG_KEYCODE: case EG_WHICH: return JS_NewInt32(ctx, e->key_code);
    case EG_REPEAT:     return JS_NewBool(ctx, e->repeat);
    }
    return JS_UNDEFINED;
}

static JSValue ev_preventDefault(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    /* Only a cancelable event has a default action to cancel, and a passive
     * listener has promised not to cancel one. Both are silent no-ops per spec. */
    if (e && e->cancelable && !e->in_passive) e->prevented = 1;
    return JS_UNDEFINED;
}
static JSValue ev_stopPropagation(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    if (e) e->stop_prop = 1;
    return JS_UNDEFINED;
}
static JSValue ev_stopImmediate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    if (e) { e->stop_prop = 1; e->stop_imm = 1; }
    return JS_UNDEFINED;
}
/* composedPath(): the ancestor chain, recomputed live. Kept because feature
 * detection for it is common; it is exact for our flat (shadow-less) tree. */
static JSValue ev_composedPath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    JSValue arr = JS_NewArray(ctx);
    if (!e || JS_IsException(arr)) return arr;
    uint32_t i = 0;
    for (struct node *p = nref_live(&e->target); p; p = p->parent)
        JS_SetPropertyUint32(ctx, arr, i++, wrap(ctx, p));
    return arr;
}

static const JSCFunctionListEntry event_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("type", ev_get, NULL, EG_TYPE),
    JS_CGETSET_MAGIC_DEF("target", ev_get, NULL, EG_TARGET),
    JS_CGETSET_MAGIC_DEF("srcElement", ev_get, NULL, EG_TARGET),
    JS_CGETSET_MAGIC_DEF("currentTarget", ev_get, NULL, EG_CURRENT),
    JS_CGETSET_MAGIC_DEF("eventPhase", ev_get, NULL, EG_PHASE),
    JS_CGETSET_MAGIC_DEF("bubbles", ev_get, NULL, EG_BUBBLES),
    JS_CGETSET_MAGIC_DEF("cancelable", ev_get, NULL, EG_CANCELABLE),
    JS_CGETSET_MAGIC_DEF("defaultPrevented", ev_get, NULL, EG_PREVENTED),
    JS_CGETSET_MAGIC_DEF("isTrusted", ev_get, NULL, EG_TRUSTED),
    JS_CGETSET_MAGIC_DEF("composed", ev_get, NULL, EG_COMPOSED),
    JS_CGETSET_MAGIC_DEF("timeStamp", ev_get, NULL, EG_TIMESTAMP),
    JS_CFUNC_DEF("preventDefault", 0, ev_preventDefault),
    JS_CFUNC_DEF("stopPropagation", 0, ev_stopPropagation),
    JS_CFUNC_DEF("stopImmediatePropagation", 0, ev_stopImmediate),
    JS_CFUNC_DEF("composedPath", 0, ev_composedPath),
    JS_PROP_INT32_DEF("NONE", EPHASE_NONE, 0),
    JS_PROP_INT32_DEF("CAPTURING_PHASE", EPHASE_CAPTURING, 0),
    JS_PROP_INT32_DEF("AT_TARGET", EPHASE_AT_TARGET, 0),
    JS_PROP_INT32_DEF("BUBBLING_PHASE", EPHASE_BUBBLING, 0),
};
static const JSCFunctionListEntry ui_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("detail", ev_get, NULL, EG_DETAIL),
};
static const JSCFunctionListEntry mouse_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("clientX", ev_get, NULL, EG_CLIENTX),
    JS_CGETSET_MAGIC_DEF("clientY", ev_get, NULL, EG_CLIENTY),
    JS_CGETSET_MAGIC_DEF("pageX", ev_get, NULL, EG_PAGEX),
    JS_CGETSET_MAGIC_DEF("pageY", ev_get, NULL, EG_PAGEY),
    JS_CGETSET_MAGIC_DEF("screenX", ev_get, NULL, EG_SCREENX),
    JS_CGETSET_MAGIC_DEF("screenY", ev_get, NULL, EG_SCREENY),
    JS_CGETSET_MAGIC_DEF("button", ev_get, NULL, EG_BUTTON),
    JS_CGETSET_MAGIC_DEF("buttons", ev_get, NULL, EG_BUTTONS),
    JS_CGETSET_MAGIC_DEF("shiftKey", ev_get, NULL, EG_SHIFT),
    JS_CGETSET_MAGIC_DEF("ctrlKey", ev_get, NULL, EG_CTRL),
    JS_CGETSET_MAGIC_DEF("altKey", ev_get, NULL, EG_ALT),
    JS_CGETSET_MAGIC_DEF("metaKey", ev_get, NULL, EG_META),
    JS_CGETSET_MAGIC_DEF("deltaX", ev_get, NULL, EG_DELTAX),
    JS_CGETSET_MAGIC_DEF("deltaY", ev_get, NULL, EG_DELTAY),
    JS_CGETSET_MAGIC_DEF("deltaMode", ev_get, NULL, EG_DELTAMODE),
};
static const JSCFunctionListEntry key_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("key", ev_get, NULL, EG_KEY),
    JS_CGETSET_MAGIC_DEF("code", ev_get, NULL, EG_CODE),
    JS_CGETSET_MAGIC_DEF("keyCode", ev_get, NULL, EG_KEYCODE),
    JS_CGETSET_MAGIC_DEF("which", ev_get, NULL, EG_WHICH),
    JS_CGETSET_MAGIC_DEF("repeat", ev_get, NULL, EG_REPEAT),
    JS_CGETSET_MAGIC_DEF("shiftKey", ev_get, NULL, EG_SHIFT),
    JS_CGETSET_MAGIC_DEF("ctrlKey", ev_get, NULL, EG_CTRL),
    JS_CGETSET_MAGIC_DEF("altKey", ev_get, NULL, EG_ALT),
    JS_CGETSET_MAGIC_DEF("metaKey", ev_get, NULL, EG_META),
};

/* new Event(type, init) / new MouseEvent(...) / new KeyboardEvent(...).
 * `magic` picks which prototype the result gets, which is also what decides
 * which of the property groups above are visible on it. */
enum { CTOR_EVENT, CTOR_UI, CTOR_MOUSE, CTOR_KEY };

static int init_bool(JSContext *ctx, JSValueConst o, const char *k)
{
    if (!JS_IsObject(o)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}
static int init_int(JSContext *ctx, JSValueConst o, const char *k)
{
    if (!JS_IsObject(o)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n = 0;
    if (!JS_IsUndefined(v)) JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return (int)n;
}
static double init_num(JSContext *ctx, JSValueConst o, const char *k)
{
    if (!JS_IsObject(o)) return 0;
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    double d = 0;
    if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}
static char *init_str(JSContext *ctx, JSValueConst o, const char *k)
{
    if (!JS_IsObject(o)) return dupstr("");
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    char *r = 0;
    if (!JS_IsUndefined(v)) { const char *s = JS_ToCString(ctx, v);
                              if (s) { r = dupstr(s); JS_FreeCString(ctx, s); } }
    JS_FreeValue(ctx, v);
    return r ? r : dupstr("");
}

static JSValue ev_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                       JSValueConst *argv, int magic)
{
    (void)new_target;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Event: type argument required");
    const char *ty = JS_ToCString(ctx, argv[0]);
    if (!ty) return JS_EXCEPTION;
    struct jsevent *ev = calloc(1, sizeof *ev);
    if (!ev) { JS_FreeCString(ctx, ty); return JS_ThrowOutOfMemory(ctx); }
    ev->type = dupstr(ty);
    JS_FreeCString(ctx, ty);
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    ev->bubbles    = (unsigned char)init_bool(ctx, init, "bubbles");
    ev->cancelable = (unsigned char)init_bool(ctx, init, "cancelable");
    ev->composed   = (unsigned char)init_bool(ctx, init, "composed");
    JSValueConst proto = g_proto_event;
    if (magic == CTOR_UI || magic == CTOR_MOUSE || magic == CTOR_KEY) {
        ev->detail = init_int(ctx, init, "detail");
        proto = g_proto_ui;
    }
    if (magic == CTOR_MOUSE) {
        ev->client_x = init_int(ctx, init, "clientX");
        ev->client_y = init_int(ctx, init, "clientY");
        ev->button   = init_int(ctx, init, "button");
        ev->buttons  = init_int(ctx, init, "buttons");
        ev->delta_x  = init_num(ctx, init, "deltaX");
        ev->delta_y  = init_num(ctx, init, "deltaY");
        proto = g_proto_mouse;
    }
    if (magic == CTOR_KEY) {
        ev->key    = init_str(ctx, init, "key");
        ev->code   = init_str(ctx, init, "code");
        ev->key_code = init_int(ctx, init, "keyCode");
        ev->repeat = (unsigned char)init_bool(ctx, init, "repeat");
        proto = g_proto_key;
    }
    if (magic == CTOR_MOUSE || magic == CTOR_KEY) {
        ev->shift = (unsigned char)init_bool(ctx, init, "shiftKey");
        ev->ctrl  = (unsigned char)init_bool(ctx, init, "ctrlKey");
        ev->alt   = (unsigned char)init_bool(ctx, init, "altKey");
        ev->meta  = (unsigned char)init_bool(ctx, init, "metaKey");
    }
    return event_new(ctx, proto, ev);
}

/* ---- three-phase dispatch ---- */

/* An event target must be an element (or the document): layout hangs its text
 * boxes off the TEXT node, and `event.target` being a text node is both wrong
 * per spec and useless to a handler. */
static struct node *event_target_of(struct node *n)
{
    while (n && n->type != N_ELEM && n->type != N_DOCUMENT) n = n->parent;
    return n;
}

/* An inline on<type>="..." content attribute, compiled on first use.
 *
 * The spec calls this "the event handler content attribute", and it is compiled
 * lazily exactly like this -- a page with a hundred onclick= attributes must not
 * pay a hundred compilations at load. Once compiled it is installed as the
 * node's attr-listener, so `el.onclick` reads it back and a later
 * `el.onclick = f` replaces it in the right position. (A later
 * setAttribute('onclick', ...) does NOT recompile: the DOM's dirty-flag path
 * does not carry enough information to know the value changed, and no page in
 * the wild rewrites a handler attribute after load.) */
static void ensure_attr_handler(JSContext *ctx, struct node *n, const char *type)
{
    if (n->type != N_ELEM) return;
    char name[40];
    size_t tl = strlen(type);
    if (tl + 3 > sizeof name) return;
    name[0] = 'o'; name[1] = 'n';
    memcpy(name + 2, type, tl + 1);
    const char *body = dom_attr(n, name);
    if (!body || !*body) return;
    struct nlist *e = nlist_for(ctx, n, 0);
    if (e) for (struct listener *l = e->head; l; l = l->next)
        if (l->attr && !strcmp(l->type, type)) return;    /* already compiled/assigned */
    /* Wrapped so `event` is the parameter and `this` is the current target,
     * which is what a handler attribute sees in a real browser. */
    struct sbuf src = { 0, 0, 0 };
    sb_push(&src, "(function(event){", 17);
    sb_push(&src, body, strlen(body));
    sb_push(&src, "\n})", 3);
    if (!src.p) return;
    JSValue f = JS_Eval(ctx, src.p, src.len, "<on-attr>", JS_EVAL_TYPE_GLOBAL);
    free(src.p);
    if (JS_IsException(f)) { report_exc(ctx, name); JS_FreeValue(ctx, f); return; }
    if (JS_IsFunction(ctx, f)) listener_add(ctx, n, type, f, 0, 0, 0, 1);
    JS_FreeValue(ctx, f);
}

/* Run the listeners registered on one node. `want_capture` is 1 for the capture
 * pass, 0 for the bubble pass and -1 at the target, where the DOM runs BOTH
 * kinds in registration order. */
static void invoke_at(JSContext *ctx, struct jsevent *ev, JSValueConst evobj,
                      struct nref *nr, int want_capture)
{
    struct node *n = nref_live(nr);
    if (!n) return;
    if (want_capture != 1) ensure_attr_handler(ctx, n, ev->type);
    struct nlist *e = nlist_for(ctx, n, 0);
    if (!e || !e->head) return;

    /* Snapshot first. The DOM says the listener list is copied before any
     * handler runs, so a handler that adds a listener does not get called by
     * the event it is handling, and one that removes a listener still leaves a
     * valid iteration -- the copy holds a reference and the `removed` flag is
     * what suppresses the call. */
    int cnt = 0;
    for (struct listener *l = e->head; l; l = l->next) cnt++;
    struct listener **snap = malloc((size_t)cnt * sizeof *snap);
    if (!snap) return;
    int k = 0;
    for (struct listener *l = e->head; l; l = l->next) { l->refs++; snap[k++] = l; }

    ev->current = *nr;
    for (k = 0; k < cnt; k++) {
        struct listener *l = snap[k];
        if (ev->stop_imm) break;
        if (l->removed) continue;
        if (want_capture >= 0 && (int)l->capture != want_capture) continue;
        if (strcmp(l->type, ev->type)) continue;
        if (!nref_live(nr)) break;              /* a previous handler destroyed the node */
        if (l->once) listener_unlink(ctx, e, l);
        JSValue this_val = wrap(ctx, n);
        JSValueConst args[1] = { evobj };
        ev->in_passive = l->passive;
        JSValue r = JS_Call(ctx, l->fn, this_val, 1, args);
        ev->in_passive = 0;
        if (JS_IsException(r)) report_exc(ctx, "event listener");
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, this_val);
        /* A handler may have resolved a promise; run its reactions before the
         * next handler so ordering matches a real browser's microtask
         * checkpoint per callback. */
        js_dom_run_jobs(ctx);
    }
    for (k = 0; k < cnt; k++) listener_unref(ctx, snap[k]);
    free(snap);
}

/* Returns 1 if the default action should proceed. */
static int dispatch_event(JSContext *ctx, struct node *target, JSValueConst evobj,
                          struct jsevent *ev)
{
    nlist_prune(ctx);
    ev->target = nref_of(target);
    ev->dispatching = 1;
    ev->stop_prop = ev->stop_imm = 0;

    /* The propagation path, captured up front. Node identity is (pointer,
     * serial): a handler is allowed to delete an ancestor mid-dispatch, and
     * when it does the remaining nodes on the path simply stop matching and are
     * skipped rather than being followed into recycled memory. */
    int cap = 16, np = 0;
    struct nref *path = malloc((size_t)cap * sizeof *path);
    if (!path) return 1;
    for (struct node *p = target; p; p = p->parent) {
        if (np == cap) {
            int ncap = cap * 2;
            struct nref *q = realloc(path, (size_t)ncap * sizeof *path);
            if (!q) break;
            path = q; cap = ncap;
        }
        path[np++] = nref_of(p);
    }

    ev->phase = EPHASE_CAPTURING;
    for (int i = np - 1; i >= 1 && !ev->stop_prop; i--)
        invoke_at(ctx, ev, evobj, &path[i], 1);
    if (!ev->stop_prop) {
        ev->phase = EPHASE_AT_TARGET;
        invoke_at(ctx, ev, evobj, &path[0], -1);
    }
    if (ev->bubbles) {
        ev->phase = EPHASE_BUBBLING;
        for (int i = 1; i < np && !ev->stop_prop; i++)
            invoke_at(ctx, ev, evobj, &path[i], 0);
    }
    free(path);

    ev->phase = EPHASE_NONE;
    ev->current.n = 0; ev->current.serial = 0;
    ev->dispatching = 0;
    return !ev->prevented;
}

int js_dom_dispatch(struct node *target, const char *type,
                    const struct js_event_init *init)
{
    /* No page runtime -> no listeners -> the default action always proceeds.
     * A page whose script failed to compile must still have working links. */
    if (!g_ctx || !type) return 1;
    JSContext *ctx = g_ctx;
    if (!target) target = g_root;
    target = event_target_of(target);
    if (!target) return 1;

    struct jsevent *ev = calloc(1, sizeof *ev);
    if (!ev) return 1;
    ev->type = dupstr(type);
    ev->trusted = 1;
    JSValueConst proto = g_proto_event;
    if (init) {
        ev->bubbles    = (unsigned char)!!init->bubbles;
        ev->cancelable = (unsigned char)!!init->cancelable;
        ev->detail     = init->detail;
        ev->client_x   = init->client_x;
        ev->client_y   = init->client_y;
        ev->button     = init->button;
        ev->buttons    = init->buttons;
        ev->shift      = (unsigned char)!!init->shift;
        ev->ctrl       = (unsigned char)!!init->ctrl;
        ev->alt        = (unsigned char)!!init->alt;
        ev->meta       = (unsigned char)!!init->meta;
        ev->delta_x    = init->delta_x;
        ev->delta_y    = init->delta_y;
        ev->key_code   = init->key_code;
        ev->repeat     = (unsigned char)!!init->repeat;
        if (init->key || init->code) {
            ev->key  = dupstr(init->key ? init->key : "");
            ev->code = dupstr(init->code ? init->code : "");
            proto = g_proto_key;
        } else {
            proto = g_proto_mouse;   /* every other native event we raise is pointer-shaped */
        }
    }
    JSValue evobj = event_new(ctx, proto, ev);
    if (JS_IsException(evobj)) { JS_FreeValue(ctx, evobj); return 1; }
    int ok = dispatch_event(ctx, target, evobj, ev);
    JS_FreeValue(ctx, evobj);
    js_dom_run_jobs(ctx);
    return ok;
}

/* ---- the JS-visible EventTarget API ---- */

/* addEventListener's third argument is either a boolean `capture` or an options
 * dictionary; both spellings are in wide use, so both are decoded. */
static void decode_opts(JSContext *ctx, JSValueConst v, int *capture, int *once, int *passive)
{
    *capture = *once = *passive = 0;
    if (JS_IsUndefined(v) || JS_IsNull(v)) return;
    if (JS_IsObject(v) && !JS_IsFunction(ctx, v)) {
        *capture = init_bool(ctx, v, "capture");
        *once    = init_bool(ctx, v, "once");
        *passive = init_bool(ctx, v, "passive");
        return;
    }
    *capture = JS_ToBool(ctx, v) > 0;
}

/* One implementation for Element, document and window: they differ only in
 * which node the listener hangs off, which the caller resolves. */
static JSValue target_add(JSContext *ctx, struct node *n, int argc, JSValueConst *argv)
{
    if (!n || argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
    const char *ty = JS_ToCString(ctx, argv[0]);
    if (!ty) return JS_UNDEFINED;
    int capture, once, passive;
    decode_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &capture, &once, &passive);
    listener_add(ctx, n, ty, argv[1], capture, once, passive, 0);
    JS_FreeCString(ctx, ty);
    return JS_UNDEFINED;
}
static JSValue target_remove(JSContext *ctx, struct node *n, int argc, JSValueConst *argv)
{
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *ty = JS_ToCString(ctx, argv[0]);
    if (!ty) return JS_UNDEFINED;
    int capture, once, passive;
    decode_opts(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &capture, &once, &passive);
    listener_remove(ctx, n, ty, argv[1], capture, 0);
    JS_FreeCString(ctx, ty);
    return JS_UNDEFINED;
}
static JSValue target_dispatch(JSContext *ctx, struct node *n, int argc, JSValueConst *argv)
{
    if (!n || argc < 1) return JS_FALSE;
    struct jsevent *ev = JS_GetOpaque(argv[0], event_cid);
    if (!ev) return JS_ThrowTypeError(ctx, "dispatchEvent: argument is not an Event");
    if (ev->dispatching) return JS_ThrowTypeError(ctx, "dispatchEvent: event is already being dispatched");
    /* A script-made event is not trusted, and re-dispatching one clears the
     * flags a previous dispatch left behind (the DOM's "initialised flag"
     * dance, minus the deprecated initEvent path). */
    ev->trusted = 0;
    ev->prevented = 0;
    int ok = dispatch_event(ctx, n, argv[0], ev);
    js_dom_run_jobs(ctx);
    return JS_NewBool(ctx, ok);
}

static JSValue el_addEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return target_add(ctx, node_of(t), argc, argv); }
static JSValue el_removeEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return target_remove(ctx, node_of(t), argc, argv); }
static JSValue el_dispatchEvent(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ return target_dispatch(ctx, node_of(t), argc, argv); }

static JSValue doc_addEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; return target_add(ctx, g_root, argc, argv); }
static JSValue doc_removeEventListener(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; return target_remove(ctx, g_root, argc, argv); }
static JSValue doc_dispatchEvent(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; return target_dispatch(ctx, g_root, argc, argv); }

/* ---- the on* handler properties ----
 * A table, not one getter/setter pair per name: the only thing that varies is
 * the event type string. */
static const char *const on_names[] = {
    "click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover",
    "mouseout", "mouseenter", "mouseleave", "contextmenu", "wheel",
    "keydown", "keyup", "keypress",
    "load", "unload", "scroll", "resize", "error",
    "input", "change", "submit", "reset", "focus", "blur",
};
#define N_ON_NAMES ((int)(sizeof on_names / sizeof on_names[0]))

static JSValue on_get_for(JSContext *ctx, struct node *n, int magic)
{
    if (!n || magic < 0 || magic >= N_ON_NAMES) return JS_NULL;
    struct nlist *e = nlist_for(ctx, n, 0);
    if (e) for (struct listener *l = e->head; l; l = l->next)
        if (l->attr && !strcmp(l->type, on_names[magic])) return JS_DupValue(ctx, l->fn);
    return JS_NULL;
}
static JSValue on_set_for(JSContext *ctx, struct node *n, JSValueConst v, int magic)
{
    if (!n || magic < 0 || magic >= N_ON_NAMES) return JS_UNDEFINED;
    if (JS_IsFunction(ctx, v)) listener_add(ctx, n, on_names[magic], v, 0, 0, 0, 1);
    else                       listener_remove(ctx, n, on_names[magic], JS_UNDEFINED, 0, 1);
    return JS_UNDEFINED;
}
static JSValue el_on_get(JSContext *ctx, JSValueConst t, int magic)
{ return on_get_for(ctx, node_of(t), magic); }
static JSValue el_on_set(JSContext *ctx, JSValueConst t, JSValueConst v, int magic)
{ return on_set_for(ctx, node_of(t), v, magic); }
static JSValue doc_on_get(JSContext *ctx, JSValueConst t, int magic)
{ (void)t; return on_get_for(ctx, g_root, magic); }
static JSValue doc_on_set(JSContext *ctx, JSValueConst t, JSValueConst v, int magic)
{ (void)t; return on_set_for(ctx, g_root, v, magic); }

/* Install on<name> accessors onto a prototype/object. Built at runtime from the
 * table above rather than as 25 more JSCFunctionListEntry lines. */
static void install_on_props(JSContext *ctx, JSValueConst obj,
                             JSValue (*get)(JSContext *, JSValueConst, int),
                             JSValue (*set)(JSContext *, JSValueConst, JSValueConst, int))
{
    for (int i = 0; i < N_ON_NAMES; i++) {
        char nm[32];
        nm[0] = 'o'; nm[1] = 'n';
        size_t l = strlen(on_names[i]);
        if (l + 3 > sizeof nm) continue;
        memcpy(nm + 2, on_names[i], l + 1);
        JSAtom a = JS_NewAtom(ctx, nm);
        JSValue g = JS_NewCFunction2(ctx, (JSCFunction *)get, nm, 0, JS_CFUNC_getter_magic, i);
        JSValue s = JS_NewCFunction2(ctx, (JSCFunction *)set, nm, 1, JS_CFUNC_setter_magic, i);
        JS_DefinePropertyGetSet(ctx, obj, a, g, s, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}

/* Bind window.addEventListener / removeEventListener / dispatchEvent and the
 * on* properties to the document root.
 *
 * Known simplification: `window` is not a distinct event target here, so a
 * window listener sees currentTarget === document. Giving window its own target
 * needs a node that is not in the tree, and every consumer (the propagation
 * path, the listener hash, the serial check) is written over real nodes. Pages
 * use window listeners to catch bubbled events, and that works exactly right. */
void js_dom_bind_event_target(JSContext *ctx, JSValueConst obj)
{
    JS_SetPropertyStr(ctx, obj, "addEventListener",
                      JS_NewCFunction(ctx, doc_addEventListener, "addEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "removeEventListener",
                      JS_NewCFunction(ctx, doc_removeEventListener, "removeEventListener", 2));
    JS_SetPropertyStr(ctx, obj, "dispatchEvent",
                      JS_NewCFunction(ctx, doc_dispatchEvent, "dispatchEvent", 1));
    install_on_props(ctx, obj, doc_on_get, doc_on_set);
}

/* Free every listener ref, the cached prototypes AND every wrapper slot in the
 * document. Call before JS_FreeContext on the context js_dom_init() was last
 * run with.
 *
 * The wrapper part is not optional: js_page.c builds a fresh JSRuntime per page,
 * so any node->jsw left behind points at a JSObject in a runtime that no longer
 * exists. The very next document.body on the next page would hand that dangling
 * pointer straight back to JS. */
void js_dom_cleanup(JSContext *ctx)
{
    nlist_free_all(ctx);
    JS_FreeValue(ctx, g_proto_event); g_proto_event = JS_UNDEFINED;
    JS_FreeValue(ctx, g_proto_ui);    g_proto_ui    = JS_UNDEFINED;
    JS_FreeValue(ctx, g_proto_mouse); g_proto_mouse = JS_UNDEFINED;
    JS_FreeValue(ctx, g_proto_key);   g_proto_key   = JS_UNDEFINED;
    if (g_root) dom_clear_wrappers(g_root->doc);
    g_ctx = 0;
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
    JS_CFUNC_DEF("removeEventListener", 2, el_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, el_dispatchEvent),
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
    JS_CFUNC_DEF("addEventListener", 2, doc_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, doc_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, doc_dispatchEvent),
    JS_CGETSET_DEF("body", doc_get_body, NULL),
    JS_CGETSET_DEF("documentElement", doc_get_docel, NULL),
};

/* Register one event constructor and its prototype. `parent` chains the
 * prototypes so MouseEvent.prototype inherits Event.prototype's members and
 * `instanceof Event` is true for a MouseEvent. */
static JSValue make_event_class(JSContext *ctx, JSValue g, const char *name,
                                JSValueConst parent, const JSCFunctionListEntry *tab,
                                int ntab, int magic)
{
    JSValue proto = JS_IsUndefined(parent) ? JS_NewObject(ctx) : JS_NewObjectProto(ctx, parent);
    if (JS_IsException(proto)) return proto;
    if (tab) JS_SetPropertyFunctionList(ctx, proto, tab, ntab);
    JSValue ctor = JS_NewCFunction2(ctx, (JSCFunction *)ev_ctor, name, 1,
                                    JS_CFUNC_constructor_magic, magic);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPropertyStr(ctx, g, name, ctor);
    return proto;                                  /* caller keeps the strong ref */
}

void js_dom_init(JSContext *ctx, struct node *root)
{
    g_ctx = ctx;
    g_root = root; g_dirty = 0;
    g_proto_event = g_proto_ui = g_proto_mouse = g_proto_key = JS_UNDEFINED;
    /* Defensive: if a previous page's runtime went away without a cleanup call,
     * its wrapper slots are dangling JSObject*s. Start every page with none. */
    if (root) dom_clear_wrappers(root->doc);
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* class ids index per-runtime arrays: js_page.c builds a fresh JSRuntime for
     * every page, so the class must be registered on each init. The old
     * `if (!elem_cid)` guard reused the first runtime's id -> out-of-bounds
     * access on ctx->class_proto[] from the second page on. */
    JS_NewClassID(&elem_cid);
    if (JS_NewClass(rt, elem_cid, &elem_class) < 0) return;
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, elem_proto, countof(elem_proto));
    install_on_props(ctx, proto, el_on_get, el_on_set);
    JS_SetClassProto(ctx, elem_cid, proto);

    JS_NewClassID(&token_cid);
    if (JS_NewClass(rt, token_cid, &token_class) >= 0) {
        JSValue tp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, tp, token_proto, countof(token_proto));
        JS_SetClassProto(ctx, token_cid, tp);
    }

    JSValue g = JS_GetGlobalObject(ctx);

    JS_NewClassID(&event_cid);
    if (JS_NewClass(rt, event_cid, &event_class) >= 0) {
        g_proto_event = make_event_class(ctx, g, "Event", JS_UNDEFINED,
                                         event_proto_funcs, countof(event_proto_funcs), CTOR_EVENT);
        g_proto_ui    = make_event_class(ctx, g, "UIEvent", g_proto_event,
                                         ui_proto_funcs, countof(ui_proto_funcs), CTOR_UI);
        g_proto_mouse = make_event_class(ctx, g, "MouseEvent", g_proto_ui,
                                         mouse_proto_funcs, countof(mouse_proto_funcs), CTOR_MOUSE);
        g_proto_key   = make_event_class(ctx, g, "KeyboardEvent", g_proto_ui,
                                         key_proto_funcs, countof(key_proto_funcs), CTOR_KEY);
        /* WheelEvent is MouseEvent here: the delta* getters already live on the
         * mouse prototype, and a separate class would only add a name. Pages
         * feature-detect `'onwheel' in window`, not the constructor identity. */
        JS_SetPropertyStr(ctx, g, "WheelEvent", JS_GetPropertyStr(ctx, g, "MouseEvent"));
        JS_SetClassProto(ctx, event_cid, JS_DupValue(ctx, g_proto_event));
    }

    JSValue doc = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, doc, doc_funcs, countof(doc_funcs));
    install_on_props(ctx, doc, doc_on_get, doc_on_set);
    JS_SetPropertyStr(ctx, g, "document", doc);
    JS_FreeValue(ctx, g);
    maybe_install_console(ctx);
}
