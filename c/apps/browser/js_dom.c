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
#include "css.h"
#include "dom_serialize.h"
#include "html_tree.h"
#include "js_dom.h"
#include "layout.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* getBoundingClientRect reads the display list, and the display list lives in
 * layout.c -- which is NOT linked into every consumer of this file. browser.aex
 * has it; the host binding test (tests/unit/js_dom_test.c) links the DOM, the
 * CSS engine and QuickJS and deliberately nothing else, and adding layout.c to
 * that link would drag in the image codecs and the whole paint surface.
 *
 * So the two accessors are reached through WEAK references: the linker resolves
 * them to 0 where layout is absent, `have_layout()` sees that, and the rect
 * comes back all-zero -- which is also what a real browser returns for an
 * element that has no box. Nothing else in this file depends on layout, so the
 * dependency stays exactly this wide.
 *
 * Spelt `__weak__` and not `weak`: the OS build force-includes
 * c/apps/libc/include/features.h, which defines a `weak` MACRO, so the plain
 * spelling expands to a nested __attribute__ and does not compile. */
extern int layout_count(void) __attribute__((__weak__));
extern const struct item *layout_items(void) __attribute__((__weak__));
static int have_layout(void) { return &layout_count != 0 && &layout_items != 0; }

static struct node *g_root;

/* The context the page is bound to. Dispatch is entered from C (a click, a
 * timer), not from JS, so there is no `ctx` argument to ride in on -- the
 * binding layer has to remember one. Exactly one page runtime exists at a time,
 * which is what makes a single static correct rather than a shortcut. */
static JSContext *g_ctx;

/* ---- the invalidation record ----
 *
 * See js_dom.h for what the tiers mean. The representation is deliberately
 * tiny: a level, and a handful of {node, serial} scope roots. A page that
 * touches more places than fit simply falls back to "the whole document",
 * which is what every mutation used to do unconditionally -- so the worst case
 * of this mechanism is the old behaviour, never worse.
 *
 * The serial is the same safety property the wrappers and the listener store
 * use: a marked node can be destroyed before the embedder gets round to
 * re-styling (a click handler that marks a node and then replaces its parent's
 * innerHTML does exactly that), and re-styling from a recycled slot would walk
 * a different element's children. A dead root reads as NULL and the embedder
 * re-styles everything. */
#define JS_DOM_MAX_DIRTY 8

struct dirty_root { struct node *n; uint32_t serial; unsigned char siblings; };
static struct dirty_root g_droot[JS_DOM_MAX_DIRTY];
static int g_ndroot;
static int g_level;                     /* INVAL_* */
static int g_whole;                     /* scope escaped the root set */

int  js_dom_dirty(void) { return g_level != INVAL_NONE; }
void js_dom_clear_dirty(void) { g_level = INVAL_NONE; g_ndroot = 0; g_whole = 0; }
int  js_dom_inval_level(void) { return g_level; }
int  js_dom_inval_roots(void) { return g_whole ? 0 : g_ndroot; }

struct node *js_dom_inval_root(int i, int *siblings)
{
    if (siblings) *siblings = 0;
    if (g_whole || i < 0 || i >= g_ndroot) return 0;
    struct dirty_root *r = &g_droot[i];
    if (!r->n || r->n->serial != r->serial) return 0;
    if (siblings) *siblings = r->siblings;
    return r->n;
}

static int is_ancestor(const struct node *a, const struct node *b)
{
    for (const struct node *p = b ? b->parent : 0; p; p = p->parent)
        if (p == a) return 1;
    return 0;
}

/* Record that `n`'s subtree needs re-styling at `level`. Roots are coalesced:
 * a scope already covered by one in the set is dropped, and one that c overs an
 * existing scope replaces it. Without that, a handler doing three appendChilds
 * under the same parent would ask for the same subtree three times. */
/* Is `n` attached to the document being rendered?
 *
 * An off-document node has no box and no pixel, so a mutation inside it needs
 * no scope: nothing on screen can be stale because of it. What eventually makes
 * it visible is the INSERTION, and insert_run() marks the destination with
 * INVAL_LAYOUT -- re-styling that scope covers every descendant that arrived
 * with it. So dropping these marks cannot lose a repaint; it can only stop the
 * record from spending scopes on work nobody can see.
 *
 * This is the difference between a React app being usable and not. react-dom
 * builds a subtree off-document -- createElement, className, setAttribute,
 * createTextNode, appendChild, thirty-odd calls -- and attaches it with ONE
 * insertBefore. Counting the off-document ones, every commit claimed more than
 * the JS_DOM_MAX_DIRTY scopes the record holds, overflowed, and fell back to
 * re-styling the whole document. Measured on a 3122-element page, one such
 * commit: 3122 elements / 4.0 ms before, 34 elements / 0.05 ms after (see
 * measure_commit() in tests/unit/js_dom_test.c).
 *
 * With no document bound (a host test that inits with a NULL root) everything
 * counts as connected, which is the conservative answer. */
static int connected(const struct node *n)
{
    if (!g_root) return 1;
    for (; n; n = n->parent) if (n == g_root) return 1;
    return 0;
}

static void mark(struct node *n, int level, int siblings)
{
    /* Before the level is even raised: an off-document mutation is not a
     * pending repaint, so it must not make the page report itself dirty. */
    if (n && !connected(n)) return;
    if (level > g_level) g_level = level;
    if (g_whole) return;
    if (!n || n->type != N_ELEM) { g_whole = 1; return; }

    for (int i = 0; i < g_ndroot; i++) {
        struct node *r = g_droot[i].n;
        if (!r || r->serial != g_droot[i].serial) { g_whole = 1; return; }
        if (r == n) { g_droot[i].siblings |= (unsigned char)!!siblings; return; }
        /* An ancestor's subtree already contains n AND every sibling of n
         * (siblings share n's parent, which is inside that subtree), so the
         * sibling flag needs no propagation here. */
        if (is_ancestor(r, n)) return;
    }
    /* n covers one or more existing roots: swallow them. */
    int w = 0;
    for (int i = 0; i < g_ndroot; i++)
        if (!is_ancestor(n, g_droot[i].n)) g_droot[w++] = g_droot[i];
    g_ndroot = w;

    if (g_ndroot >= JS_DOM_MAX_DIRTY) { g_whole = 1; return; }
    g_droot[g_ndroot].n = n;
    g_droot[g_ndroot].serial = n->serial;
    g_droot[g_ndroot].siblings = (unsigned char)!!siblings;
    g_ndroot++;
}

/* The two shapes every mutation in this file reduces to.
 *
 * mark_self: this element's own selector inputs changed (class, id, style, any
 * attribute an [attr] selector could key off). Its subtree needs the cascade
 * again -- descendant combinators and inheritance -- and so do its FOLLOWING
 * siblings, because `+`/`~` exist. Preceding siblings and ancestors cannot be
 * affected: CSS has no previous-sibling combinator and our LibCSS has no
 * `:has()`.
 *
 * mark_children: this element's child list changed. That is a layout change by
 * construction, and it also moves the sibling-position pseudo-classes
 * (:first-child, :nth-child, :empty) of everything under it -- hence the whole
 * subtree, rooted at the PARENT of what changed. */
static void mark_self(struct node *n, int level) { mark(n, level, 1); }
static void mark_children(struct node *n) { mark(n, INVAL_LAYOUT, 0); }

/* A TEXT/COMMENT node's data changed. mark() only records ELEMENT scopes (a
 * text node has no subtree to re-style and no selector can key off it), so the
 * scope is the nearest element ancestor -- its child list did not change but the
 * text INSIDE it did, which is a layout change all the same: a longer string
 * reflows the line boxes.
 *
 * A character-data node with no element ancestor at all is skipped rather than
 * escalated to whole-document; a node that HAS one but is off-document is
 * dropped a moment later by connected(). Both are the same argument: nothing on
 * screen depends on it yet, and the insertion that connects it marks the
 * insertion point. React hits this on every text update it makes -- it creates
 * the node, writes it, and only then appends. */
static void mark_chardata(struct node *n)
{
    struct node *p = n ? n->parent : 0;
    while (p && p->type != N_ELEM) p = p->parent;
    if (p) mark(p, INVAL_LAYOUT, 0);
}

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

/* The prototype a wrapper for `n` must carry -- HTMLBodyElement.prototype for a
 * <body>, Text.prototype for a text node, and so on down the real interface
 * hierarchy. Defined in js_dom_iface.inc, which is included below (it needs the
 * member tables). Returns a BORROWED value, or JS_UNDEFINED before the
 * hierarchy exists / when it is compiled out -- and then wrap() falls back to
 * the one shared class prototype, which is exactly what this file did before. */
static JSValueConst iface_proto_for(const struct node *n);
/* Give a childNodes/children snapshot NodeList.prototype / HTMLCollection
 * .prototype. Also in js_dom_iface.inc; a no-op before it is built. */
static void iface_tag_list(JSContext *ctx, JSValueConst arr, int elems_only);
static void iface_cleanup(JSContext *ctx);
/* One-shot repair of the seam with js_select.c / js_platform.c / js_media.c --
 * see section 6 of js_dom_iface.inc. Runs from js_dom_run_jobs, the first
 * js_dom entry point reached after js_page_open has run every installer. */
static void iface_bridge(JSContext *ctx);
/* The two synthetic node kinds js_dom_iface.inc adds on top of dom.c's four,
 * both elements with a name the HTML tokenizer cannot produce (see there):
 * a ProcessingInstruction and the detached Document createHTMLDocument returns.
 * node_type_of() has to know about them. */
static int is_pi(const struct node *n);
static int is_docx(const struct node *n);
/* Window named access: `<span id=x>` makes `x` a global. See section 7 of
 * js_dom_iface.inc for the mechanism and the two shadowing rules. */
static void named_scan(JSContext *ctx, struct node *root);
static void named_note_attr(JSContext *ctx, struct node *n, const char *attr);

/* One wrapper per node, cached in the node's weak `jsw` slot, so
 * document.body === document.body and a script can hang expandos off an
 * element. The slot takes no reference: the finalizer clears it, and
 * js_dom_cleanup() clears every slot in the document when the page's runtime
 * goes away. */
static JSValue wrap(JSContext *ctx, struct node *n)
{
    if (!n) return JS_NULL;
    /* The compatibility bridge's trigger. Every foreign installer probes the
     * element prototype with `document.createElement('div')`, which is a wrap,
     * and it must run AFTER all of them -- see BRIDGE_WRAPS in
     * js_dom_iface.inc for why this is here and not on an embedder hook. */
    iface_bridge(ctx);
    if (n->jsw) return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, n->jsw));
    JSValueConst proto = iface_proto_for(n);
    JSValue o = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, elem_cid)
                                   : JS_NewObjectClass(ctx, (int)elem_cid);
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

/* Replace a TEXT/COMMENT node's data in place. dom.c can only APPEND to a
 * payload (the tree builder never rewrites one), so a rewrite is "truncate,
 * then append" over the same public fields dom_text_append maintains. Done in
 * place rather than by swapping in a fresh node because every JS wrapper, every
 * listener bucket and every invalidation root already held is keyed on this
 * node -- React's commitTextUpdate writes the SAME text node over and over. */
static void chardata_set(struct node *n, const char *s, size_t len)
{
    if (!n || (n->type != N_TEXT && n->type != N_COMMENT)) return;
    n->textlen = 0;
    if (n->text && n->textcap > 0) n->text[0] = 0;
    if (len) dom_text_append(n, s, (int)len);
    mark_chardata(n);
}

static void set_text(struct node *n, const char *s)
{
    dom_destroy_children(n);
    /* The DOM's "string replace all" adds NO node for an empty string, and the
     * difference is observable: `el.textContent = ''` is how a page (and React)
     * empties a container, and the old unconditional append left an empty text
     * node behind -- so childNodes.length came back 1, and a script that then
     * appended and read firstChild got the leftover instead of what it just
     * inserted. */
    if (!s || !*s) return;
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
    /* A Document's textContent is null, not the concatenation of the page.
     * Only reachable since `document` became a real Node wrapper. */
    if (n->type == N_DOCUMENT || n->type == N_DOCTYPE) return JS_NULL;
    struct sbuf b = { 0, 0, 0 };
    gather_text(n, &b);
    JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.len);
    free(b.p);
    return v;
}
static JSValue el_set_text(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t); if (!n) return JS_UNDEFINED;
    size_t sl = 0;
    const char *s = JS_ToCStringLen(ctx, &sl, v);
    if (!s) return JS_UNDEFINED;
    /* On a TEXT/COMMENT node textContent IS the data. The old code ran the
     * element path unconditionally, which appended a text node as the CHILD of
     * a text node -- a shape nothing downstream can read, so the write silently
     * did nothing. It could not be reached before createTextNode existed. */
    if (n->type == N_TEXT || n->type == N_COMMENT) chardata_set(n, s, sl);
    else { set_text(n, s); mark_children(n); }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
/* tagName / nodeName ARE UPPERCASE for an HTML element.
 *
 * This file used to return them lowercase and called it a known deviation, on
 * the grounds that "library code that cares almost always writes
 * nodeName.toLowerCase()". Both halves of that turned out to be wrong. The web
 * writes `node.tagName === 'INPUT'` constantly and that comparison was silently
 * false here -- a wrong answer, not a missing feature, which is the worse kind.
 * And WPT tests it directly (10 subtests in html/syntax alone).
 *
 * Checked before changing it: every in-tree consumer -- js_select.c's tagOf,
 * js_platform.c's per-tag hasInstance and reflected-URL tag test, js_media.c --
 * lowercases before comparing, and layout/CSS read `node->tag` in C and never
 * see this at all. Only HTML elements are uppercased: an SVG element's name is
 * case-sensitive ("clipPath") and the spec leaves it exactly as authored. */
static JSValue tagname_value(JSContext *ctx, const struct node *n)
{
    if (n->ns != NS_HTML) return JS_NewString(ctx, n->tag);
    char buf[64], *p = buf;
    size_t len = strlen(n->tag);
    if (len + 1 > sizeof buf) { p = malloc(len + 1); if (!p) return JS_NewString(ctx, n->tag); }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n->tag[i];
        p[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    p[len] = 0;
    JSValue v = JS_NewString(ctx, p);
    if (p != buf) free(p);
    return v;
}

static JSValue el_get_tag(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n) return JS_UNDEFINED;
    return n->type == N_ELEM ? tagname_value(ctx, n) : JS_NewString(ctx, n->tag);
}
static JSValue el_get_id(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); const char *v = n ? dom_attr(n, "id") : 0; return JS_NewString(ctx, v ? v : ""); }
/* `id` was read-only, which is not a small omission: `el.id = 'x'` is the
 * ordinary way to name a node you just created, it fails SILENTLY on a
 * read-only accessor in sloppy mode, and the element then never enters the
 * document's id index -- so getElementById keeps answering null and the page
 * looks like it lost the node. */
static JSValue el_set_id(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (s) { if (dom_set_attr(n, "id", s)) mark_self(n, INVAL_STYLE);
             named_note_attr(ctx, n, "id"); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
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
        mark_children(n);
    }
    if (fdoc) dom_free(dom_doc_root(fdoc));
    return JS_UNDEFINED;
}
/* ---- attribute values are BYTE STRINGS, not C strings --------------------
 *
 * `struct dom_attr` has always carried a `vlen` and dom.c has always had
 * `dom_set_attr_raw` for exactly this reason: an attribute value may contain
 * U+0000, and dom.c stores it. The path through THIS file did not -- getAttribute
 * built its result with JS_NewString and setAttribute took JS_ToCString, so an
 * embedded NUL truncated the value on the way in and again on the way out.
 *
 * That is not a corner case in the corpus: html/dom/reflection-*.html sets
 * every attribute it tests to " \0\x01...\x1f foo " and to "\0", and then
 * requires getAttribute() to return exactly what was set. It is also the reason
 * a keyword comparison has to compare LENGTHS (js_reflect.c does): with
 * truncation, the enumerated value "text\0" would have matched the keyword
 * "text" and read back as valid.
 *
 * dom_set_attr_raw takes the name VERBATIM, so the ASCII-lowercasing
 * dom_set_attr would have done has to happen here. */
static char *lower_dup(const char *s, size_t len)
{
    char *o = malloc(len + 1);
    if (!o) return 0;
    for (size_t i = 0; i < len; i++) o[i] = (char)lc((unsigned char)s[i]);
    o[len] = 0;
    return o;
}

/* The attribute's value AND its length, or NULL when absent. dom_attr() answers
 * the pointer only, and the length lives one field over in the same struct. */
static const char *attr_val_len(const struct node *n, const char *name, int *len)
{
    if (len) *len = 0;
    if (!n || n->type != N_ELEM || !name) return 0;
    for (int i = 0; i < n->nattr; i++) {
        const char *an = dom_attr_name_at(n, i);
        if (an && ieq(an, name)) {
            if (len) *len = (int)n->attrs[i].vlen;
            return n->attrs[i].value;
        }
    }
    return 0;
}

/* Write an attribute with an explicit length, keeping every derived index in
 * sync exactly as el_setattr does. One writer, so a reflected setter and
 * setAttribute() cannot disagree about the invalidation tier. */
static void attr_write(JSContext *ctx, struct node *n, const char *name,
                       const char *val, int vlen)
{
    if (!n || n->type != N_ELEM || !name || !*name) return;
    char *ln = lower_dup(name, strlen(name));
    if (!ln) return;
    if (dom_set_attr_raw(n, ln, (int)strlen(ln), val ? val : "", vlen)) {
        mark_self(n, INVAL_STYLE);
        named_note_attr(ctx, n, ln);
    }
    free(ln);
}

static JSValue el_getattr(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]); if (!nm) return JS_NULL;
    int len = 0;
    const char *v = attr_val_len(n, nm, &len); JS_FreeCString(ctx, nm);
    return v ? JS_NewStringLen(ctx, v, (size_t)len) : JS_NULL;
}
static JSValue el_setattr(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 2) return JS_UNDEFINED;
    const char *nm = JS_ToCString(ctx, argv[0]);
    size_t vlen = 0;
    const char *vl = JS_ToCStringLen(ctx, &vlen, argv[1]);
    /* Any attribute at all, not just class/id/style: [data-state="on"] and
     * [href^="/"] are ordinary selectors, so there is no attribute whose value
     * provably cannot participate in the cascade. */
    if (nm && vl) attr_write(ctx, n, nm, vl, (int)vlen);
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

/* ---- DocumentFragment ----
 *
 * dom.c has four node types and none of them is a fragment. Adding one would
 * mean touching every switch in the tokenizer, the tree builder, the
 * serialiser, the CSS engine and layout -- none of which is this file's to
 * change -- so a fragment is instead an ELEMENT whose tag name the HTML
 * tokenizer can never produce ('#' is not a name-start character).
 *
 * What makes that safe is that the fragment is the one node which never enters
 * the tree: insert_run() below MOVES its children and leaves the fragment
 * behind, exactly as the DOM specifies. Layout, CSS and the serialiser
 * therefore never see one, and the only code that has to know is nodeType (11)
 * and the insertion helpers. */
#define FRAG_TAG "#document-fragment"
static int is_fragment(const struct node *n)
{
    return n && n->type == N_ELEM && n->tag && n->tag[0] == '#' && !strcmp(n->tag, FRAG_TAG);
}

/* Can `c` legally be inserted into `p`? The DOM throws HierarchyRequestError;
 * we refuse and return null, for the same reason a computed-style write is
 * ignored rather than thrown -- one bad call should cost the page that call,
 * not the rest of the script.
 *
 * The cycle test is not paranoia: `a.appendChild(a.parentNode)` would otherwise
 * splice the tree into a ring and the very next iterative walk (gather_text,
 * the propagation path, layout) would spin forever. Cross-document inserts are
 * refused too -- every string a node owns lives in ITS document's arena, so
 * splicing across would leave the tree pointing into an arena that can be freed
 * independently. */
static int can_insert(struct node *p, struct node *c)
{
    if (!p || !c || p == c) return 0;
    if (c->type == N_DOCUMENT || c->type == N_DOCTYPE) return 0;
    if (p->type != N_ELEM && p->type != N_DOCUMENT) return 0;   /* not a container */
    if (p->doc != c->doc) return 0;
    if (is_ancestor(c, p)) return 0;                            /* would make a cycle */
    return 1;
}

/* The one insertion path: insertBefore(c, ref) with ref == NULL meaning append,
 * and a fragment meaning "insert each of its children instead".
 *
 * Marking is the part that must not drift from el_appendChild's original: BOTH
 * the destination and any old parent are marked INVAL_LAYOUT, because a move
 * changes two child lists and the sibling-position pseudo-classes
 * (:first-child, :nth-child, :empty) under each. The old parent has to be read
 * BEFORE the link is broken. */
static int insert_run(struct node *p, struct node *c, struct node *ref)
{
    if (!can_insert(p, c)) return 0;
    if (ref && ref->parent != p) ref = 0;                       /* not ours: append */
    if (ref == c) return 1;                                     /* already in place */

    if (is_fragment(c)) {
        struct node *k = c->first_child;
        if (!k) return 1;                    /* an empty fragment inserts nothing */
        while (k) {
            struct node *nx = k->next;
            dom_insert_before(p, k, ref);
            k = nx;
        }
    } else {
        struct node *old = c->parent;
        dom_insert_before(p, c, ref);        /* move semantics: detaches from any old parent */
        if (old && old != p) mark_children(old);
    }
    mark_children(p);
    /* Anything that just entered the document can claim a window name. Only on
     * a CONNECTED destination: an off-document subtree exposes nothing, and
     * react-dom builds every commit off-document, so this costs nothing on the
     * path that runs thirty times a frame. */
    if (g_ctx && connected(p)) named_scan(g_ctx, is_fragment(c) ? p : c);
    return 1;
}

static JSValue el_appendChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c) return JS_NULL;
    if (!insert_run(n, c, 0)) return JS_NULL;
    return JS_DupValue(ctx, argv[0]);          /* like the DOM: return the appended child */
}

/* insertBefore(new, ref). `ref` null/undefined appends -- which is not a
 * courtesy, it is how react-dom appends at all: its host config calls
 * insertBefore(parent, child, before) with `before` null for the last position. */
static JSValue el_insertBefore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c) return JS_NULL;
    struct node *ref = (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]))
                       ? node_of(argv[1]) : 0;
    if (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]) && !ref)
        return JS_NULL;                        /* a stale/foreign reference node */
    if (ref && ref->parent != n) return JS_NULL;   /* NotFoundError */
    if (!insert_run(n, c, ref)) return JS_NULL;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue el_removeChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_NULL;
    struct node *c = node_of(argv[0]); if (!c || c->parent != n) return JS_NULL;
    dom_destroy_subtree(c);                    /* wrappers into it go stale via serial */
    mark_children(n);
    return JS_DupValue(ctx, argv[0]);
}

/* replaceChild(new, old) -> old.
 *
 * KNOWN DEVIATION, and it is the same one removeChild already carries: the
 * removed node is RECYCLED, not merely detached, so the returned wrapper is
 * stale and cannot be re-inserted. Detach-only would be the spec answer, but
 * dom.c reclaims nodes exclusively through dom_destroy_subtree, so an orphan
 * that a script drops on the floor would live until the page is freed -- and a
 * list that replaces a row per frame would grow the document arena without
 * bound. Recycling is the safe end of that trade because the {node, serial}
 * handle makes the staleness DETECTABLE (the wrapper reads as null) rather than
 * a use-after-free. */
static JSValue el_replaceChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 2) return JS_NULL;
    struct node *nw = node_of(argv[0]);
    struct node *od = node_of(argv[1]);
    if (!nw || !od || od->parent != n) return JS_NULL;
    if (nw == od) return JS_DupValue(ctx, argv[1]);
    if (!insert_run(n, nw, od)) return JS_NULL;
    dom_destroy_subtree(od);
    mark_children(n);
    return JS_DupValue(ctx, argv[1]);
}

/* ======================================================================
 * The Node surface: types, navigation, character data, namespaces.
 *
 * Everything above this point speaks Element. react-dom's host config does not:
 * it creates text nodes and comments, walks parentNode/nextSibling to find its
 * insertion points, and reads nodeType to tell a text instance from a host
 * instance. Without these a React commit cannot even start.
 *
 * All of it is installed on the ONE wrapper class. Splitting Node / Element /
 * Text / Comment into four prototypes would be the spec shape, but our wrapper
 * is keyed on a `struct node` whose type is a runtime field, so the split would
 * buy a nicer `instanceof` and cost a per-type wrapper cache. The accessors
 * check the node type themselves instead, and return what the spec says the
 * wrong type gets (null / undefined / "").
 * ====================================================================== */

/* The `document` object itself, so ownerDocument and the parentNode of
 * <html> can answer with it. A strong ref, freed by js_dom_cleanup: it is the
 * global object's own `document`, so this cannot create a cycle that keeps a
 * page alive. */
static JSValue g_document;

#define NSURI_HTML   "http://www.w3.org/1999/xhtml"
#define NSURI_SVG    "http://www.w3.org/2000/svg"
#define NSURI_MATHML "http://www.w3.org/1998/Math/MathML"

/* DOM nodeType numbers. Ours are a different enumeration (dom.h N_*), and the
 * two must not be conflated: N_ELEM is 0 and ELEMENT_NODE is 1. */
static int node_type_of(const struct node *n)
{
    if (!n) return 0;
    if (is_fragment(n)) return 11;              /* DOCUMENT_FRAGMENT_NODE */
    if (n->tag && n->tag[0] == '#') {           /* the synthetic kinds */
        if (is_pi(n)) return 7;                 /* PROCESSING_INSTRUCTION_NODE */
        if (is_docx(n)) return 9;               /* DOCUMENT_NODE */
    }
    switch (n->type) {
    case N_ELEM:     return 1;
    case N_TEXT:     return 3;
    case N_COMMENT:  return 8;
    case N_DOCUMENT: return 9;
    case N_DOCTYPE:  return 10;
    default:         return 0;
    }
}

static JSValue el_get_nodeType(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? JS_NewInt32(ctx, node_type_of(n)) : JS_UNDEFINED; }

/* nodeName: the uppercased tag name for an HTML element (see tagname_value),
 * and the literal "#text" / "#comment" / "#document" / the doctype's name for
 * everything else -- those are NOT uppercased, which is why this cannot just
 * be el_get_tag. */
static JSValue el_get_nodeName(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n) return JS_UNDEFINED;
    return n->type == N_ELEM && !is_fragment(n) ? tagname_value(ctx, n)
                                                : JS_NewString(ctx, n->tag);
}

/* nodeValue / data: the character-data payload, and null on anything else --
 * which is the spec's answer for an element, not an error. */
static JSValue el_get_nodeValue(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n) return JS_UNDEFINED;
    if (n->type != N_TEXT && n->type != N_COMMENT) return JS_NULL;
    return JS_NewStringLen(ctx, n->text ? n->text : "", (size_t)(n->text ? n->textlen : 0));
}
static JSValue el_set_nodeValue(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t);
    if (!n || (n->type != N_TEXT && n->type != N_COMMENT)) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (s) { chardata_set(n, s, len); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* ---- navigation ---- */
static JSValue el_get_parentNode(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n || !n->parent) return n ? JS_NULL : JS_UNDEFINED;
    /* <html>'s parent is the document, and a script comparing
     * `el.parentNode === document` has to get true -- so the synthetic
     * #document node resolves to the `document` object, not to an Element
     * wrapper around it. */
    if (n->parent == g_root && !JS_IsUndefined(g_document))
        return JS_DupValue(ctx, g_document);
    return wrap(ctx, n->parent);
}
static JSValue el_get_parentElement(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n || !n->parent || n->parent->type != N_ELEM) return JS_NULL;
    return wrap(ctx, n->parent);
}
static JSValue el_get_firstChild(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, n->first_child) : JS_UNDEFINED; }
static JSValue el_get_lastChild(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, n->last_child) : JS_UNDEFINED; }
static JSValue el_get_nextSibling(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, n->next) : JS_UNDEFINED; }
static JSValue el_get_prevSibling(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, n->prev) : JS_UNDEFINED; }

static struct node *next_elem(struct node *n) { while (n && n->type != N_ELEM) n = n->next; return n; }
static struct node *prev_elem(struct node *n) { while (n && n->type != N_ELEM) n = n->prev; return n; }

static JSValue el_get_firstElemChild(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, next_elem(n->first_child)) : JS_UNDEFINED; }
static JSValue el_get_lastElemChild(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, prev_elem(n->last_child)) : JS_UNDEFINED; }
static JSValue el_get_nextElemSib(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, next_elem(n->next)) : JS_UNDEFINED; }
static JSValue el_get_prevElemSib(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? wrap(ctx, prev_elem(n->prev)) : JS_UNDEFINED; }

/* childNodes / children.
 *
 * KNOWN DEVIATION: a real NodeList/HTMLCollection is LIVE. These are Array
 * SNAPSHOTS taken at property-access time. A live list needs an exotic object
 * whose every index lookup re-walks the child list, and the cost is not the
 * walk -- it is that the list must survive its nodes being recycled, i.e. a
 * second {node,serial} handle class. Snapshots satisfy every real use
 * (`for (var c of el.childNodes)`, `Array.from(el.children)`, `.length`) and a
 * page that caches one across a mutation is rare and gets a stale array rather
 * than a wrong one. */
static JSValue child_array(JSContext *ctx, struct node *n, int elems_only)
{
    JSValue a = JS_NewArray(ctx);
    if (!n || JS_IsException(a)) return a;
    uint32_t i = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        if (elems_only && c->type != N_ELEM) continue;
        JS_DefinePropertyValueUint32(ctx, a, i++, wrap(ctx, c), JS_PROP_C_W_E);
    }
    iface_tag_list(ctx, a, elems_only);
    return a;
}
static JSValue el_get_childNodes(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? child_array(ctx, n, 0) : JS_UNDEFINED; }
static JSValue el_get_children(JSContext *ctx, JSValueConst t)
{ struct node *n = node_of(t); return n ? child_array(ctx, n, 1) : JS_UNDEFINED; }

static JSValue el_hasChildNodes(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; struct node *n = node_of(t); return JS_NewBool(ctx, n && n->first_child != 0); }

/* contains(other): inclusive, as the DOM defines it. Widely used for
 * click-outside handling, which is why it is here and cloneNode is not. */
static JSValue el_contains(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || argc < 1) return JS_FALSE;
    struct node *o = node_of(argv[0]);
    return JS_NewBool(ctx, o && (o == n || is_ancestor(n, o)));
}

static JSValue el_get_ownerDocument(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n) return JS_UNDEFINED;
    if (n->type == N_DOCUMENT) return JS_NULL;      /* the document owns no document */
    return JS_IsUndefined(g_document) ? JS_NULL : JS_DupValue(ctx, g_document);
}

static JSValue el_get_namespaceURI(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM) return JS_NULL;
    return JS_NewString(ctx, n->ns == NS_SVG ? NSURI_SVG :
                             n->ns == NS_MATHML ? NSURI_MATHML : NSURI_HTML);
}

/* ---- attributes: the two halves setAttribute was missing ---- */

/* dom.c has no attribute REMOVAL: the tree builder only ever adds, so nothing
 * before now needed one, and dom.c is not this file's to extend. The removal is
 * therefore done over `struct node`'s public attribute array, in two steps that
 * must stay in this order:
 *
 *   1. dom_set_attr(n, name, "") -- this is what un-indexes the node from the
 *      document's id table and empties node->classes. Those two derived indexes
 *      are private to dom.c and re-derived only through attr_set, so shifting
 *      the array down without this first would leave a removed id="x" still
 *      answering getElementById('x').
 *   2. shift the entry out of attrs[] so dom_attr() reports it ABSENT, which is
 *      what [attr] selectors and hasAttribute() key off. Step 1 alone leaves an
 *      empty-valued attribute, and `[hidden]` matches an empty value.
 *
 * The interned name and the arena value are abandoned, exactly as an overwrite
 * through attr_set abandons the old value: the arena is bump-allocated and
 * reclaimed with the document. */
static int attr_remove(struct node *n, const char *name)
{
    if (!n || n->type != N_ELEM || !name || !*name) return 0;
    if (!dom_attr(n, name)) return 0;                    /* not present */
    dom_set_attr(n, name, "");                           /* step 1: sync id/class */
    for (int i = 0; i < n->nattr; i++) {
        const char *an = dom_attr_name_at(n, i);
        if (!an || !ieq(an, name)) continue;
        for (int k = i + 1; k < n->nattr; k++) n->attrs[k - 1] = n->attrs[k];
        n->nattr--;
        return 1;
    }
    return 0;
}

static JSValue el_removeAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_UNDEFINED;
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_UNDEFINED;
    /* Same tier as setAttribute: any attribute can be a selector input. */
    if (attr_remove(n, nm)) mark_self(n, INVAL_STYLE);
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}

static JSValue el_hasAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t); if (!n || argc < 1) return JS_FALSE;
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_FALSE;
    int has = dom_attr(n, nm) != 0;
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, has);
}

/* Defined with the DOMTokenList below: one writer for the class attribute, so
 * className= and classList.add() cannot disagree about the no-op check or the
 * invalidation tier. */
static void class_write(struct node *n, const char *text, int len);

/* className. React sets it on essentially every element it renders, and
 * classList -- which is all we had -- cannot express "replace the whole list"
 * in one call the way React needs. It goes through class_write(), so the
 * no-op check and the invalidation tier are shared with classList.
 *
 * It is length-carrying for the same reason getAttribute is (see attr_val_len
 * above): `class` is an ordinary attribute and "a\0b" is an ordinary value for
 * it. A class token can never CONTAIN a NUL -- the tokeniser splits on ASCII
 * whitespace and NUL is not whitespace, so the NUL simply lives inside one
 * token -- but className must still hand back what was put in. */
static JSValue el_get_className(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    int len = 0;
    const char *v = (n && n->type == N_ELEM) ? attr_val_len(n, "class", &len) : 0;
    return JS_NewStringLen(ctx, v ? v : "", v ? (size_t)len : 0);
}
static JSValue el_set_className(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (s) { class_write(n, s, (int)len); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* ---- getBoundingClientRect ----
 *
 * React does not call this; almost every UI library built on React does
 * (popovers, tooltips, virtualised lists, drag handles).
 *
 * layout.c produces a FLAT display list, not a box tree: `struct item` carries
 * the DOM node each painted box came from and nothing else. So the element's
 * border box is recovered as the UNION of every item whose node is the element
 * or one of its descendants. That is exact whenever the element paints its own
 * box (a background or any border makes layout emit an IT_RECT covering the
 * whole border box) and is the INK BOUNDS of the subtree otherwise -- a
 * background-less <div> reports the extent of its text and images, so the
 * height is right and the width is the widest line rather than the full
 * containing-block width. Recording that here rather than "fixing" it in
 * layout.c is deliberate: layout.c belongs to the render pipeline, and making
 * it emit an invisible rect per element would cost a display-list entry for
 * every node on every page to serve a call most pages never make.
 *
 * Two further honest limits:
 *  - The rect is read from the LAST COMPLETED LAYOUT. A real browser flushes a
 *    pending reflow on this call; we cannot, because layout runs from the
 *    browser's main loop with a canvas width this file does not have. A script
 *    that mutates and measures in the same turn measures the pre-mutation box.
 *  - Client coordinates are document coordinates minus the scroll offset, and
 *    the scroll offset has to be pushed in by the embedder (js_dom_set_scroll).
 *    Until it is, the two coincide -- which they do exactly at page load. */
static int g_scroll_x, g_scroll_y;
void js_dom_set_scroll(int x, int y) { g_scroll_x = x; g_scroll_y = y; }

static int in_subtree(const struct node *root, const struct node *n)
{
    for (; n; n = n->parent) if (n == root) return 1;
    return 0;
}

/* Union of the subtree's painted boxes. Returns 0 if the element has no box at
 * all, in which case the DOM says every field is zero. */
static int subtree_box(struct node *el, int *ox, int *oy, int *ow, int *oh)
{
    *ox = *oy = *ow = *oh = 0;
    if (!el || !have_layout()) return 0;
    const struct item *it = layout_items();
    int n = layout_count();
    if (!it || n <= 0) return 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, got = 0;
    for (int i = 0; i < n; i++) {
        if (!it[i].node || !in_subtree(el, it[i].node)) continue;
        int ax = it[i].x, ay = it[i].y, bx = it[i].x + it[i].w, by = it[i].y + it[i].h;
        if (!got) { x0 = ax; y0 = ay; x1 = bx; y1 = by; got = 1; continue; }
        if (ax < x0) x0 = ax;
        if (ay < y0) y0 = ay;
        if (bx > x1) x1 = bx;
        if (by > y1) y1 = by;
    }
    if (!got) return 0;
    *ox = x0 - g_scroll_x; *oy = y0 - g_scroll_y;
    *ow = x1 - x0;         *oh = y1 - y0;
    return 1;
}

static JSValue rect_num(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; return JS_DupValue(ctx, t); }    /* toJSON: the object itself */

static JSValue el_getBoundingClientRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = node_of(t);
    int x = 0, y = 0, w = 0, h = 0;
    if (n) subtree_box(n, &x, &y, &w, &h);
    JSValue r = JS_NewObject(ctx);
    if (JS_IsException(r)) return r;
    JS_SetPropertyStr(ctx, r, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, r, "height", JS_NewFloat64(ctx, h));
    JS_SetPropertyStr(ctx, r, "left", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, r, "top", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, r, "right", JS_NewFloat64(ctx, x + w));
    JS_SetPropertyStr(ctx, r, "bottom", JS_NewFloat64(ctx, y + h));
    JS_SetPropertyStr(ctx, r, "toJSON", JS_NewCFunction(ctx, rect_num, "toJSON", 0));
    return r;
}

/* ---- document factory methods ---- */
static JSValue doc_createTextNode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (!g_root) return JS_NULL;
    size_t len = 0;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : 0;
    /* createTextNode() with no argument makes an EMPTY text node, which React
     * relies on: it creates the node and writes nodeValue afterwards. */
    struct node *n = dom_create_text(g_root->doc, s ? s : "", (int)(s ? len : 0));
    if (s) JS_FreeCString(ctx, s);
    return wrap(ctx, n);
}

static JSValue doc_createComment(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (!g_root) return JS_NULL;
    size_t len = 0;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : 0;
    struct node *n = dom_create_comment(g_root->doc, s ? s : "", (int)(s ? len : 0));
    if (s) JS_FreeCString(ctx, s);
    return wrap(ctx, n);
}

static JSValue doc_createFragment(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    if (!g_root) return JS_NULL;
    return wrap(ctx, dom_create_element(g_root->doc, FRAG_TAG, (int)strlen(FRAG_TAG)));
}

/* createElementNS(ns, qualifiedName). The namespace decides the CASE rule, and
 * that is the whole reason this cannot just forward to createElement: SVG local
 * names are case-sensitive ("clipPath", "feGaussianBlur", "linearGradient") and
 * dom_create_element ASCII-lowercases, which would silently produce elements
 * no SVG renderer can match. dom_create_element_ns takes the name verbatim. */
static JSValue doc_createElementNS(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; if (argc < 2 || !g_root) return JS_NULL;
    const char *uri = JS_IsNull(argv[0]) ? 0 : JS_ToCString(ctx, argv[0]);
    const char *qn = JS_ToCString(ctx, argv[1]);
    if (!qn) { if (uri) JS_FreeCString(ctx, uri); return JS_NULL; }

    int ns = NS_HTML;
    if (uri) {
        if (!strcmp(uri, NSURI_SVG)) ns = NS_SVG;
        else if (!strcmp(uri, NSURI_MATHML)) ns = NS_MATHML;
    }
    /* A qualified name may carry a prefix ("svg:rect"); the node stores the
     * LOCAL name, the prefix being carried by the namespace we just resolved. */
    const char *local = qn;
    for (const char *p = qn; *p; p++) if (*p == ':') local = p + 1;

    struct node *n = (ns == NS_HTML)
        ? dom_create_element(g_root->doc, local, (int)strlen(local))
        : dom_create_element_ns(g_root->doc, local, (int)strlen(local), ns);
    if (uri) JS_FreeCString(ctx, uri);
    JS_FreeCString(ctx, qn);
    return wrap(ctx, n);
}

/* document.head. The tree builder always makes one, but a fragment-built
 * document may not have it, so this resolves against the live tree the same
 * way document.body does rather than being captured at init. */
static JSValue doc_get_head(JSContext *ctx, JSValueConst t)
{
    (void)t;
    struct node *head = 0;
    if (g_root) for (struct node *c = g_root->first_child; c && !head; c = c->next) head = find_sel(c, "head");
    return wrap(ctx, head);
}
static JSValue doc_get_nodeType(JSContext *ctx, JSValueConst t)
{ (void)t; return JS_NewInt32(ctx, 9); }

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

/* the token list receiver, serial-checked */
static struct node *token_node(JSValueConst t)
{
    struct elem_handle *h = JS_GetOpaque(t, token_cid);
    if (!h || !h->n || h->n->serial != h->serial) return 0;
    return h->n;
}

/* shared arg decode: token list receiver + class name from argv[0] */
static struct node *token_args(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                               const char **name)
{
    *name = 0;
    if (argc < 1) return 0;
    struct node *n = token_node(t);
    if (!n) return 0;
    *name = JS_ToCString(ctx, argv[0]);
    return *name ? n : 0;
}

/* Token `i` of a whitespace-separated list, or NULL. Whitespace runs collapse,
 * so "  a   b " has exactly two tokens -- which is what the class attribute
 * means and what the length/index surface has to agree with. */
static const char *tok_at(const char *cls, int i, int *len)
{
    if (!cls) return 0;
    const char *p = cls;
    for (int k = 0; ; k++) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
        if (!*p) return 0;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\f') p++;
        if (k == i) { if (len) *len = (int)(p - s); return s; }
    }
}

static int tok_count(const char *cls)
{
    int n = 0;
    while (tok_at(cls, n, 0)) n++;
    return n;
}

/* Write a rebuilt class attribute, but only if it differs from what is already
 * there. Two reasons, and the second is the important one: dom.c's attribute
 * store bump-allocates every value into the document arena (it never frees an
 * old one), and a script that re-adds a class it already has in a rAF loop
 * would grow the arena forever. Skipping the write also keeps the invalidation
 * record honest -- a no-op does not dirty the page. */
static void class_write(struct node *n, const char *text, int len)
{
    int cl = 0;
    const char *cur = attr_val_len(n, "class", &cl);
    if (cur && cl == len && memcmp(cur, text, (size_t)len) == 0) return;
    if (!cur && len == 0) return;
    if (dom_set_attr_raw(n, "class", 5, text, len)) mark_self(n, INVAL_STYLE);
}

/* The class attribute with `drop` removed (drop == NULL keeps everything) and
 * `add` appended (add == NULL adds nothing). One builder for add/remove/replace
 * so the three cannot disagree about tokenisation. It grows: real pages carry
 * class lists far past the 255 characters the old fixed buffers allowed, and
 * truncating one would silently drop unrelated classes. */
static void class_rebuild(struct node *n, const char *drop, const char *add)
{
    const char *cls = dom_attr(n, "class");
    struct sbuf b = { 0, 0, 0 };
    size_t dl = drop ? strlen(drop) : 0;
    int added = 0;
    for (int i = 0; ; i++) {
        int tl = 0;
        const char *t = tok_at(cls, i, &tl);
        if (!t) break;
        if (drop && (size_t)tl == dl && memcmp(t, drop, dl) == 0) {
            /* replace() keeps the token's POSITION, which is what makes
             * `classList.replace('a','b')` different from remove+add. */
            if (add && !added) {
                if (b.len) sb_push(&b, " ", 1);
                sb_push(&b, add, strlen(add));
                added = 1;
            }
            continue;
        }
        if (b.len) sb_push(&b, " ", 1);
        sb_push(&b, t, (size_t)tl);
    }
    if (add && !added) {
        if (b.len) sb_push(&b, " ", 1);
        sb_push(&b, add, strlen(add));
    }
    class_write(n, b.p ? b.p : "", (int)b.len);
    free(b.p);
}

static JSValue cl_contains(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_FALSE;
    int has = word_has(dom_attr(n, "class"), nm);
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, has);
}
/* add/remove take any number of tokens, as the DOM specifies. */
static JSValue cl_add(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = token_node(t);
    if (!n) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *nm = JS_ToCString(ctx, argv[i]);
        if (!nm) continue;
        if (*nm && !word_has(dom_attr(n, "class"), nm)) class_rebuild(n, 0, nm);
        JS_FreeCString(ctx, nm);
    }
    return JS_UNDEFINED;
}
static JSValue cl_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = token_node(t);
    if (!n) return JS_UNDEFINED;
    for (int i = 0; i < argc; i++) {
        const char *nm = JS_ToCString(ctx, argv[i]);
        if (!nm) continue;
        if (word_has(dom_attr(n, "class"), nm)) class_rebuild(n, nm, 0);
        JS_FreeCString(ctx, nm);
    }
    return JS_UNDEFINED;
}
/* toggle(token[, force]): the two-argument form is how frameworks drive a
 * boolean state without reading the current one first. */
static JSValue cl_toggle(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    const char *nm; struct node *n = token_args(ctx, t, argc, argv, &nm);
    if (!n) return JS_FALSE;
    int has = word_has(dom_attr(n, "class"), nm);
    int want = argc > 1 ? (JS_ToBool(ctx, argv[1]) > 0) : !has;
    if (want && !has) class_rebuild(n, 0, nm);
    else if (!want && has) class_rebuild(n, nm, 0);
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, want);
}
/* replace(old, new) -> did it replace anything? */
static JSValue cl_replace(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_FALSE;
    struct node *n = token_node(t);
    if (!n) return JS_FALSE;
    const char *a = JS_ToCString(ctx, argv[0]);
    const char *b = JS_ToCString(ctx, argv[1]);
    int did = 0;
    if (a && b && *a && *b && word_has(dom_attr(n, "class"), a)) {
        /* Replacing with a token that is already present degenerates to a
         * removal, which is exactly what the DOM's ordered-set semantics say. */
        class_rebuild(n, a, word_has(dom_attr(n, "class"), b) ? 0 : b);
        did = 1;
    }
    if (a) JS_FreeCString(ctx, a);
    if (b) JS_FreeCString(ctx, b);
    return JS_NewBool(ctx, did);
}
static JSValue cl_item(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = token_node(t);
    int32_t i = -1;
    if (!n || argc < 1) return JS_NULL;
    if (JS_ToInt32(ctx, &i, argv[0]) < 0) return JS_EXCEPTION;   /* leave nothing pending */
    if (i < 0) return JS_NULL;
    int tl = 0;
    const char *tk = tok_at(dom_attr(n, "class"), (int)i, &tl);
    return tk ? JS_NewStringLen(ctx, tk, (size_t)tl) : JS_NULL;
}
static JSValue cl_get_length(JSContext *ctx, JSValueConst t)
{
    struct node *n = token_node(t);
    return JS_NewInt32(ctx, n ? tok_count(dom_attr(n, "class")) : 0);
}
/* value / toString: the serialised list. Both spellings exist in the DOM and
 * both are used -- `String(el.classList)` and `el.classList.value`. */
static JSValue cl_get_value(JSContext *ctx, JSValueConst t)
{
    struct node *n = token_node(t);
    const char *v = n ? dom_attr(n, "class") : 0;
    return JS_NewString(ctx, v ? v : "");
}
static JSValue cl_set_value(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = token_node(t);
    if (!n) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (s) { class_write(n, s, (int)len); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue cl_toString(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)argc; (void)argv; return cl_get_value(ctx, t); }

/* Indexed access (cl[0]) through the exotic hook rather than by defining index
 * properties when the list is built: the token list is LIVE, so `cl.add('x')`
 * has to make `cl[n]` appear on the object the script is already holding.
 *
 * Only get_own_property is implemented, and only for array indices. That is the
 * one exotic hook that FALLS THROUGH: returning FALSE lets the lookup carry on
 * to the prototype, so `add`, `contains` and the rest stay reachable. (The
 * get_property hook would have shadowed every one of them.) */
static int cl_own_prop(JSContext *ctx, JSPropertyDescriptor *desc,
                       JSValueConst obj, JSAtom prop)
{
    struct node *n = token_node(obj);
    if (!n) return 0;
    JSValue key = JS_AtomToValue(ctx, prop);
    if (JS_IsException(key)) return -1;
    /* Decide on the VALUE's type, never by stringifying the atom. This hook is
     * consulted for every lookup on the object, and Symbol.iterator is one of
     * them -- JS_ToCString on a symbol throws, and the exception would be left
     * pending in the context to surface at some unrelated point later. */
    int idx = -1;
    if (JS_IsNumber(key)) {                     /* an array-index atom */
        int32_t v = -1;
        if (JS_ToInt32(ctx, &v, key) == 0 && v >= 0) idx = v;
    } else if (JS_IsString(key)) {
        const char *s = JS_ToCString(ctx, key);
        if (s) {
            int v = 0, ok = s[0] != 0;
            for (const char *p = s; *p; p++) {
                if (*p < '0' || *p > '9' || v > 100000) { ok = 0; break; }
                v = v * 10 + (*p - '0');
            }
            if (ok && s[0] == '0' && s[1]) ok = 0;   /* "01" is not an index */
            if (ok) idx = v;
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, key);
    if (idx < 0) return 0;

    int tl = 0;
    const char *tk = tok_at(dom_attr(n, "class"), idx, &tl);
    if (!tk) return 0;
    if (desc) {
        desc->flags = JS_PROP_ENUMERABLE;       /* read-only, like the DOM's */
        desc->value = JS_NewStringLen(ctx, tk, (size_t)tl);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static const JSCFunctionListEntry token_proto[] = {
    JS_CFUNC_DEF("add", 1, cl_add),
    JS_CFUNC_DEF("remove", 1, cl_remove),
    JS_CFUNC_DEF("toggle", 1, cl_toggle),
    JS_CFUNC_DEF("replace", 2, cl_replace),
    JS_CFUNC_DEF("contains", 1, cl_contains),
    JS_CFUNC_DEF("item", 1, cl_item),
    JS_CFUNC_DEF("toString", 0, cl_toString),
    JS_CGETSET_DEF("length", cl_get_length, NULL),
    JS_CGETSET_DEF("value", cl_get_value, cl_set_value),
};

static JSClassExoticMethods token_exotic = { cl_own_prop };
static JSClassDef token_class = { "DOMTokenList", token_finalizer, NULL, NULL, &token_exotic };

/* Iteration. A DOMTokenList is an array-like with `length` and index
 * properties, which is exactly the contract Array.prototype's iterator and
 * forEach are written against -- so borrowing them is not a shortcut, it is the
 * same generic algorithm the spec defines these methods in terms of. Writing
 * our own would be a second implementation of the same walk. */
static void install_arraylike(JSContext *ctx, JSValueConst proto)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, g, "Array");
    JSValue ap = JS_GetPropertyStr(ctx, arr, "prototype");
    JSValue values = JS_GetPropertyStr(ctx, ap, "values");
    JSValue foreach = JS_GetPropertyStr(ctx, ap, "forEach");
    JSValue sym = JS_GetPropertyStr(ctx, g, "Symbol");
    JSValue symit = JS_GetPropertyStr(ctx, sym, "iterator");

    if (JS_IsFunction(ctx, values) && !JS_IsUndefined(symit)) {
        JSAtom a = JS_ValueToAtom(ctx, symit);
        JS_DefinePropertyValue(ctx, proto, a, JS_DupValue(ctx, values),
                               JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        JS_FreeAtom(ctx, a);
        JS_SetPropertyStr(ctx, proto, "values", JS_DupValue(ctx, values));
        JS_SetPropertyStr(ctx, proto, "keys", JS_GetPropertyStr(ctx, ap, "keys"));
        JS_SetPropertyStr(ctx, proto, "entries", JS_GetPropertyStr(ctx, ap, "entries"));
    }
    if (JS_IsFunction(ctx, foreach))
        JS_SetPropertyStr(ctx, proto, "forEach", JS_DupValue(ctx, foreach));

    JS_FreeValue(ctx, symit); JS_FreeValue(ctx, sym);
    JS_FreeValue(ctx, foreach); JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, ap); JS_FreeValue(ctx, arr); JS_FreeValue(ctx, g);
}

/* ======================================================================
 * CSSStyleDeclaration -- element.style and getComputedStyle(el)
 *
 * One class, two modes, because the JS surface is the same object type in the
 * DOM and splitting it would mean two prototypes carrying the same six methods.
 *
 *   INLINE   backed by the element's `style` CONTENT ATTRIBUTE. Reads parse it,
 *            writes rewrite it. Nothing is cached: the attribute is the single
 *            source of truth, so a setAttribute('style', ...) and an
 *            el.style.color= can never disagree, and -- the part that actually
 *            matters -- the inline declarations keep their cascade priority for
 *            free. css_engine's style_node() already parses that attribute as
 *            a LibCSS *inline sheet*, which is where inline style's precedence
 *            over author rules comes from. Writing computed values straight
 *            into node->style instead would have been simpler and would have
 *            silently thrown that priority away.
 *
 *   COMPUTED backed by node->computed, the css_computed_style css_apply left
 *            on the node. Read-only. This is the half that was impossible
 *            before: the cascade used to compute a full style, digest it into
 *            the lossy 45-field cstyle and destroy the original, so there was
 *            nothing left to answer getComputedStyle FROM.
 * ====================================================================== */

static JSClassID cssd_cid;
enum { CSSD_INLINE = 0, CSSD_COMPUTED = 1 };

struct cssd_handle { struct node *n; uint32_t serial; unsigned char computed; };

static void cssd_finalizer(JSRuntime *rt, JSValue val)
{ (void)rt; free(JS_GetOpaque(val, cssd_cid)); }

static struct node *cssd_node(JSValueConst t, int *computed)
{
    struct cssd_handle *h = JS_GetOpaque(t, cssd_cid);
    if (computed) *computed = h ? h->computed : 0;
    if (!h || !h->n || h->n->serial != h->serial) return 0;
    return h->n;
}

/* ---- the style attribute, parsed in place ----
 *
 * A CSS declaration block is "name : value" separated by ';'. Everything below
 * works on spans into the attribute text rather than copies, so reading a
 * property costs no allocation at all -- which matters because a page that
 * animates does it every frame. */
static int cssws(int c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* Find declaration `name` (case-insensitive, as CSS property names are) in the
 * block `s`. Returns 1 and fills the spans: [ds,de) is the whole declaration
 * (so it can be dropped), [vs,ve) is the trimmed value INCLUDING any
 * !important, and *imp says whether it carried one. */
static int sty_find(const char *s, const char *name, size_t nlen,
                    int *ds, int *de, int *vs, int *ve, int *imp)
{
    if (!s || !name || !nlen) return 0;
    int i = 0;
    while (s[i]) {
        while (s[i] && (cssws(s[i]) || s[i] == ';')) i++;
        if (!s[i]) break;
        int d0 = i;
        while (s[i] && s[i] != ';') i++;
        int d1 = i;                                   /* declaration is [d0,d1) */
        int c = d0;
        while (c < d1 && s[c] != ':') c++;
        if (c >= d1) continue;                        /* no colon: not a declaration */
        int n0 = d0, n1 = c;
        while (n1 > n0 && cssws(s[n1 - 1])) n1--;
        if ((size_t)(n1 - n0) == nlen) {
            size_t k = 0;
            while (k < nlen && lc((unsigned char)s[n0 + k]) == lc((unsigned char)name[k])) k++;
            if (k == nlen) {
                int v0 = c + 1, v1 = d1;
                while (v0 < v1 && cssws(s[v0])) v0++;
                while (v1 > v0 && cssws(s[v1 - 1])) v1--;
                int important = 0;
                if (v1 - v0 >= 10) {
                    int b = v1 - 10;
                    if (s[b] == '!' && lc((unsigned char)s[b+1]) == 'i' &&
                        lc((unsigned char)s[b+2]) == 'm' && lc((unsigned char)s[b+3]) == 'p' &&
                        lc((unsigned char)s[b+4]) == 'o' && lc((unsigned char)s[b+5]) == 'r' &&
                        lc((unsigned char)s[b+6]) == 't' && lc((unsigned char)s[b+7]) == 'a' &&
                        lc((unsigned char)s[b+8]) == 'n' && lc((unsigned char)s[b+9]) == 't') {
                        important = 1;
                        while (b > v0 && cssws(s[b - 1])) b--;
                        v1 = b;
                    }
                }
                if (ds) *ds = d0; if (de) *de = d1;
                if (vs) *vs = v0; if (ve) *ve = v1;
                if (imp) *imp = important;
                return 1;
            }
        }
    }
    return 0;
}

/* The i-th declaration's property name span, for length/item(). */
static const char *sty_name_at(const char *s, int idx, int *len)
{
    if (!s) return 0;
    int i = 0, k = 0;
    while (s[i]) {
        while (s[i] && (cssws(s[i]) || s[i] == ';')) i++;
        if (!s[i]) break;
        int d0 = i;
        while (s[i] && s[i] != ';') i++;
        int c = d0;
        while (c < i && s[c] != ':') c++;
        if (c >= i) continue;
        int n1 = c;
        while (n1 > d0 && cssws(s[n1 - 1])) n1--;
        if (n1 == d0) continue;
        if (k++ == idx) { if (len) *len = n1 - d0; return s + d0; }
    }
    return 0;
}

static int sty_count(const char *s)
{ int n = 0; while (sty_name_at(s, n, 0)) n++; return n; }

/* Commit a rebuilt style attribute and record the invalidation.
 *
 * The no-op check is not an optimisation detail: dom.c's attribute store
 * bump-allocates every value into the document arena and never reclaims the
 * old one, so `el.style.left = x` writing the same string every animation
 * frame would grow the arena without bound. It also keeps a redundant write
 * from dirtying the page. */
static void style_write(struct node *n, const char *text, int level)
{
    const char *cur = dom_attr(n, "style");
    if (cur && strcmp(cur, text) == 0) return;
    if (!cur && !text[0]) return;
    if (dom_set_attr(n, "style", text)) mark_self(n, level);
}

/* setProperty / the camelCase setters / removeProperty, in one rebuild.
 * `value == NULL` (or empty) removes the declaration, which is what the CSSOM
 * says an empty value means. A replaced declaration keeps its POSITION, so
 * relative order -- and therefore the later-wins tiebreak inside the inline
 * block -- survives a rewrite. */
static void style_set(struct node *n, const char *name, const char *value, int important)
{
    if (!n || !name || !*name) return;
    size_t nlen = strlen(name);
    const char *cur = dom_attr(n, "style");
    int ds = 0, de = 0;
    int found = sty_find(cur, name, nlen, &ds, &de, 0, 0, 0);
    int empty = !value || !*value;
    if (!found && empty) return;

    struct sbuf b = { 0, 0, 0 };
    if (found) {
        /* copy the head, splice, copy the tail */
        int h = ds;
        while (h > 0 && (cssws(cur[h - 1]) || cur[h - 1] == ';')) h--;
        if (h) { sb_push(&b, cur, (size_t)h); }
        if (!empty) {
            if (b.len) sb_push(&b, "; ", 2);
            sb_push(&b, name, nlen);
            sb_push(&b, ": ", 2);
            sb_push(&b, value, strlen(value));
            if (important) sb_push(&b, " !important", 11);
        }
        const char *tail = cur + de;
        while (*tail == ';' || cssws(*tail)) tail++;
        if (*tail) { if (b.len) sb_push(&b, "; ", 2); sb_push(&b, tail, strlen(tail)); }
    } else {
        if (cur && *cur) {
            size_t cl = strlen(cur);
            while (cl && (cssws(cur[cl - 1]) || cur[cl - 1] == ';')) cl--;
            if (cl) { sb_push(&b, cur, cl); sb_push(&b, "; ", 2); }
        }
        sb_push(&b, name, nlen);
        sb_push(&b, ": ", 2);
        sb_push(&b, value, strlen(value));
        if (important) sb_push(&b, " !important", 11);
    }
    if (b.p || (cur && *cur)) {
        int prop = css_prop_lookup(name, (int)nlen);
        /* A property we can classify and that only the painter reads gets the
         * cheaper tier; anything we do not recognise is assumed to move boxes. */
        int level = (prop >= 0 && css_prop_paint_only(prop)) ? INVAL_PAINT : INVAL_STYLE;
        style_write(n, b.p ? b.p : "", level);
    }
    free(b.p);
}

/* ---- the JS surface ---- */

static JSValue cssd_new(JSContext *ctx, struct node *n, int computed)
{
    if (!n) return JS_NULL;
    JSValue o = JS_NewObjectClass(ctx, (int)cssd_cid);
    if (JS_IsException(o)) return o;
    struct cssd_handle *h = malloc(sizeof *h);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    h->n = n; h->serial = n->serial; h->computed = (unsigned char)!!computed;
    JS_SetOpaque(o, h);
    return o;
}

/* Read one property by name. `computed` picks the backing store. */
static JSValue cssd_read(JSContext *ctx, struct node *n, int computed, const char *name)
{
    if (!n || !name || !*name) return JS_NewString(ctx, "");
    if (computed) {
        int prop = css_prop_lookup(name, -1);
        /* An unresolvable property reads as "", the same answer a real browser
         * gives for one it does not implement. We cannot do better: what is on
         * the node is a css_computed_style, and it only holds what LibCSS
         * knows how to compute. */
        if (prop < 0) return JS_NewString(ctx, "");
        char buf[512];
        int len = css_computed_text(n, prop, buf, (int)sizeof buf);
        return JS_NewStringLen(ctx, buf, (size_t)len);
    }
    const char *sv = dom_attr(n, "style");
    int vs = 0, ve = 0;
    if (!sty_find(sv, name, strlen(name), 0, 0, &vs, &ve, 0)) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, sv + vs, (size_t)(ve - vs));
}

static JSValue cssd_getPropertyValue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    if (argc < 1) return JS_NewString(ctx, "");
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_NewString(ctx, "");
    JSValue r = cssd_read(ctx, n, computed, nm);
    JS_FreeCString(ctx, nm);
    return r;
}

static JSValue cssd_getPropertyPriority(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    if (!n || computed || argc < 1) return JS_NewString(ctx, "");
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_NewString(ctx, "");
    int imp = 0;
    int got = sty_find(dom_attr(n, "style"), nm, strlen(nm), 0, 0, 0, 0, &imp);
    JS_FreeCString(ctx, nm);
    return JS_NewString(ctx, (got && imp) ? "important" : "");
}

/* Defined with the property enumeration it belongs to, below: does canon.c
 * call this declaration INVALID? */
static int cssd_refuses(const char *name, const char *value);

static JSValue cssd_setProperty(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    /* A computed declaration is read-only. The DOM throws
     * NoModificationAllowedError here; we ignore the write instead, because a
     * page that accidentally assigns to a getComputedStyle result should lose
     * that one assignment, not its whole script. */
    if (!n || computed || argc < 1) return JS_UNDEFINED;
    const char *nm = JS_ToCString(ctx, argv[0]);
    const char *vl = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    const char *pr = argc > 2 ? JS_ToCString(ctx, argv[2]) : 0;
    /* The same refusal as the named setters, for the same reason -- see
     * cssd_refuses(). js_cssom.c's setProperty wrapper already refuses an
     * INVALID declaration before it reaches here, so in the browser this is a
     * second opinion agreeing with the first; it is here so the invariant "no
     * declaration canon.c calls INVALID enters the style attribute through the
     * CSSOM" holds in this file alone, for the host tests that build js_dom.c
     * without that shim. Re-checking a value the wrapper already canonicalised
     * is safe because canon.c round-trips: tests/unit/cssparse_test.c asserts
     * exactly that for every accepted value in the suite. */
    if (nm && vl && cssd_refuses(nm, vl)) { /* dropped */ }
    else if (nm) style_set(n, nm, vl, pr && (pr[0] == 'i' || pr[0] == 'I'));
    if (nm) JS_FreeCString(ctx, nm);
    if (vl) JS_FreeCString(ctx, vl);
    if (pr) JS_FreeCString(ctx, pr);
    return JS_UNDEFINED;
}

static JSValue cssd_removeProperty(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    if (!n || computed || argc < 1) return JS_NewString(ctx, "");
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_NewString(ctx, "");
    JSValue old = cssd_read(ctx, n, 0, nm);      /* removeProperty returns the old value */
    style_set(n, nm, 0, 0);
    JS_FreeCString(ctx, nm);
    return old;
}

static JSValue cssd_item(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    int32_t i = -1;
    if (!n || argc < 1) return JS_NewString(ctx, "");
    if (JS_ToInt32(ctx, &i, argv[0]) < 0) return JS_EXCEPTION;
    if (i < 0) return JS_NewString(ctx, "");
    if (computed) {
        const char *nm = css_prop_name((int)i);
        return JS_NewString(ctx, nm ? nm : "");
    }
    int len = 0;
    const char *nm = sty_name_at(dom_attr(n, "style"), (int)i, &len);
    return nm ? JS_NewStringLen(ctx, nm, (size_t)len) : JS_NewString(ctx, "");
}

static JSValue cssd_get_length(JSContext *ctx, JSValueConst t)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    if (!n) return JS_NewInt32(ctx, 0);
    /* A computed declaration enumerates every property we can resolve, which is
     * what a real one does (it enumerates every property the engine supports). */
    return JS_NewInt32(ctx, computed ? CSSP__COUNT : sty_count(dom_attr(n, "style")));
}

static JSValue cssd_get_cssText(JSContext *ctx, JSValueConst t)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    /* cssText is "" on a computed declaration, per the CSSOM: there is no
     * source text behind it. */
    const char *v = (n && !computed) ? dom_attr(n, "style") : 0;
    return JS_NewString(ctx, v ? v : "");
}

static JSValue cssd_set_cssText(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    if (!n || computed) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    /* Replacing the whole block can change anything, so it takes the
     * layout-affecting tier without inspecting what is in it. */
    if (s) { style_write(n, s, INVAL_STYLE); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* The named accessors (el.style.backgroundColor, getComputedStyle(el).display).
 *
 * `magic` INDEXES LIBCSS'S OWN PROPERTY TABLE, not css.h's CSSP_* enum, and
 * that is the fix to a defect that cost more than any missing feature here:
 *
 * CSSP_* names the ~60 properties this engine's cascade RESOLVES. The set a
 * CSSStyleDeclaration must expose as IDL attributes is a different and much
 * larger one -- every property the parser knows -- and it was being taken from
 * the enum. So `div.style['position-area'] = x` did nothing: no named property,
 * no store, and every test of that property failed on "property should be set"
 * before its parser was ever reached. The CSS line implemented position-area in
 * full, 2,598 checks, and gained ZERO for exactly this reason; the same cause
 * accounted for 4,310 more in css-anchor-position and 507 in css-fonts.
 *
 * css_known_prop_at() reads LibCSS's string table, which is also what
 * css_supports_decl() answers from -- so the two can no longer disagree, and a
 * property added to the vendored parser becomes settable and becomes supported
 * in the same commit. WPT's CSS-supports-CSSStyleDeclaration.html is 1,495
 * subtests asserting precisely that agreement.
 *
 * `cssd_item`/`cssd_get_length` deliberately stay on CSSP_*: enumerating a
 * COMPUTED declaration is the question "what can this engine resolve", which is
 * the enum and not the parser's vocabulary. Two sets, two questions. */
/* THE SECOND HALF OF THE PROPERTY UNIVERSE, and it is no longer a list here.
 *
 * css_known_prop_at() reads LibCSS's own stringmap, which is the right source
 * and is not the whole source: the CSS line's canonicaliser
 * (third_party/css/libcss/src/parse/canon.c) handles a set of modern properties
 * that never entered that stringmap -- position-area, the anchor family, the
 * logical box properties, and the grid track properties, of which LibCSS has
 * literally none. Sourcing only from the stringmap leaves exactly those
 * unsettable, which is the defect this change exists to fix, so it would have
 * fixed nothing.
 *
 * WHAT USED TO BE HERE: ~50 property names transcribed by hand from canon.c's
 * static tables, because that file published `css_canon_knows_property()` -- a
 * PREDICATE, which answers "is this one of yours?" and cannot answer "what are
 * yours?". This comment named the fix and called the copy a drift hazard:
 * "one function in canon.h beside the predicate -- a count and an index,
 * exactly the shape css_known_prop_count/at already has -- and then this array
 * deletes itself."
 *
 * That pair now exists (css_canon_prop_count/at, forwarded through css.h), the
 * array has deleted itself, and the drift it warned about had already
 * happened: the four grid track properties landed in canon.c with a full
 * <track-list> parser and were unsettable from script, so a correct serializer
 * measured as exactly nothing.
 *
 * ADOPTING THE ENUMERATION IS ONLY HALF THE CHANGE, and the other half is in
 * cssd_refuses() below. Read that before assuming this alone is the fix -- on
 * its own it makes the corpus worse in one direction while better in the
 * other. */
/* Magic at or above this is an index into canon.c's enumeration. 4096 is far
 * above any plausible LibCSS property count and is checked at install time. */
#define CSSD_EXTRA_BASE 4096

static const char *cssd_prop_of(int magic)
{
#ifdef CSSD_PROPS_FROM_ENUM
    /* The negative control: the set as it was, taken from the cascade's enum.
     * See tests/reflect.mk's test-cssprops-negctl for why this switch exists. */
    return css_prop_name(magic);
#else
    if (magic >= CSSD_EXTRA_BASE)
        return css_canon_prop_at(magic - CSSD_EXTRA_BASE);
    return css_known_prop_at(magic, 0);
#endif
}

/* THE OTHER HALF: a setter that refuses what canon.c calls INVALID.
 *
 * The CSSOM's setter has a validity step -- "if value is not a valid value for
 * property, return" -- and for a property LibCSS OWNS this file already gets
 * that step for free: the value goes through css_supports_decl() in
 * js_cssom.c's setProperty wrapper, and the cascade drops what it cannot
 * parse. For a property LibCSS has never heard of there is no such step
 * anywhere: the declaration is spliced into the style attribute unconditionally
 * and canonicalised only on the way back OUT, so `grid-template-columns: -10px`
 * is stored, read back as the raw bytes, and every "-invalid" subtest that
 * asserts the property stays unset goes red.
 *
 * Those subtests were passing before the enumeration landed, and they were
 * passing VACUOUSLY -- there was no named accessor, so nothing was stored, so
 * "should not set the property value" was true for a reason that had nothing
 * to do with validity. Publishing the accessors converts a vacuous pass into a
 * real failure, which is why the two changes have to arrive together.
 *
 * THE THREE-WAY ANSWER IS THE SAFETY ARGUMENT, and CSS_SPEC_PASS is the case
 * to be careful with:
 *
 *   CSS_SPEC_OK       canon.c parsed it. Store it -- and note this function
 *                     does NOT rewrite the value to the canonical spelling;
 *                     the read path (js_cssom.c's getPropertyValue wrapper)
 *                     canonicalises, and doing it in both places would be two
 *                     spellings of one answer.
 *   CSS_SPEC_INVALID  NEITHER canon.c NOR LibCSS can take it. Refuse. Such a
 *                     declaration is already dropped by the cascade and
 *                     renders nothing, so refusing to remember it costs no
 *                     rendering behaviour.
 *   CSS_SPEC_PASS     not canon.c's property at all -- which is most of CSS.
 *                     BEHAVE EXACTLY AS BEFORE. Anything else would put a
 *                     second validity opinion in front of every declaration
 *                     this browser already honours, including the handful
 *                     css_extra.c honours behind LibCSS's back.
 *
 * An empty value is a REMOVAL and removal is never invalid, so it never
 * reaches the check. */
static int cssd_refuses(const char *name, const char *value)
{
#ifdef CSSD_NO_CANON_REFUSE
    /* The negative control: adopt the enumeration and skip this half, which is
     * the obvious half-implementation. See tests/cssom.mk's
     * test-cssom-canon-negctl. */
    (void)name; (void)value;
    return 0;
#else
    char buf[1024];
    int len = 0;
    if (!name || !*name || !value) return 0;
    for (const char *p = value; *p; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            return css_specified_canon(name, -1, value, (int)strlen(value),
                                       buf, (int)sizeof buf, &len)
                   == CSS_SPEC_INVALID;
        }
    return 0;                                   /* whitespace only: a removal */
#endif
}

static JSValue cssd_prop_get(JSContext *ctx, JSValueConst t, int magic)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    const char *nm = cssd_prop_of(magic);
    if (!n || !nm) return JS_NewString(ctx, "");
    return cssd_read(ctx, n, computed, nm);
}

static JSValue cssd_prop_set(JSContext *ctx, JSValueConst t, JSValueConst v, int magic)
{
    int computed = 0;
    struct node *n = cssd_node(t, &computed);
    const char *nm = cssd_prop_of(magic);
    if (!n || computed || !nm) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (s) {
        if (!cssd_refuses(nm, s)) style_set(n, nm, s, 0);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry cssd_proto[] = {
    JS_CFUNC_DEF("getPropertyValue", 1, cssd_getPropertyValue),
    JS_CFUNC_DEF("getPropertyPriority", 1, cssd_getPropertyPriority),
    JS_CFUNC_DEF("setProperty", 2, cssd_setProperty),
    JS_CFUNC_DEF("removeProperty", 1, cssd_removeProperty),
    JS_CFUNC_DEF("item", 1, cssd_item),
    JS_CGETSET_DEF("length", cssd_get_length, NULL),
    JS_CGETSET_DEF("cssText", cssd_get_cssText, cssd_set_cssText),
};

static JSClassDef cssd_class = { "CSSStyleDeclaration", cssd_finalizer };

/* The IDL spelling of a dashed CSS name: "background-color" -> backgroundColor.
 * Returns 0 if the name had no dashes (the two spellings coincide).
 *
 * A LEADING dash is the vendor-prefix case and takes the CSSOM's other rule:
 * "-webkit-box" is `webkitBox`, not `WebkitBox`. The prefix is dropped and the
 * rest camel-cased with a lowercase first letter -- and getting that wrong
 * would publish a property under a name nothing looks for while leaving the
 * name everything looks for undefined. */
static int camel_of(const char *dashed, char *out, size_t cap)
{
    size_t o = 0;
    int had = 0;
    const char *p = dashed;
    if (*p == '-') { p++; had = 1; }            /* vendor prefix: drop the dash */
    for (; *p; p++) {
        if (o + 2 >= cap) return 0;
        if (*p == '-') { p++; if (!*p) break; had = 1;
                         out[o++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p); continue; }
        out[o++] = *p;
    }
    out[o] = 0;
    return had;
}

/* Install a getter/setter for every property THE PARSER KNOWS, under BOTH the
 * dashed name and the IDL camelCase name -- pages use both, and the CSSOM
 * defines both.
 *
 * The list is LibCSS's, read at runtime. It used to be css.h's CSSP_* enum,
 * which is the ~60 properties the CASCADE resolves, and the gap between the two
 * sets was the largest single blocker in the CSS corpus: a property the parser
 * handles perfectly is unreachable from script if `el.style` has no name for
 * it, and every test of it fails on "property should be set" without the parser
 * ever running. Sourcing both this and css_supports_decl() from the same table
 * is what stops the two answers drifting apart again. */
static void install_one_css_prop(JSContext *ctx, JSValueConst proto,
                                 const char *d, int magic)
{
    char cam[96];
    int has_camel = camel_of(d, cam, sizeof cam);
    for (int pass = 0; pass < 2; pass++) {
        const char *nm = pass ? cam : d;
        if (pass && (!has_camel || !*cam)) break;
        JSAtom a = JS_NewAtom(ctx, nm);
        /* Never redefine: the LibCSS set goes in first and owns any name it
         * carries, so a name canon.c's enumeration also claims -- `margin`,
         * `color`, `width` are all in both -- is a duplicate and not a
         * conflict. */
        JSPropertyDescriptor dsc;
        if (JS_GetOwnProperty(ctx, &dsc, proto, a) > 0) {
            JS_FreeValue(ctx, dsc.value); JS_FreeValue(ctx, dsc.getter);
            JS_FreeValue(ctx, dsc.setter);
            JS_FreeAtom(ctx, a);
            continue;
        }
        JSValue g = JS_NewCFunction2(ctx, (JSCFunction *)cssd_prop_get, nm, 0,
                                     JS_CFUNC_getter_magic, magic);
        JSValue s = JS_NewCFunction2(ctx, (JSCFunction *)cssd_prop_set, nm, 1,
                                     JS_CFUNC_setter_magic, magic);
        JS_DefinePropertyGetSet(ctx, proto, a, g, s, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}

static void install_css_props(JSContext *ctx, JSValueConst proto)
{
#ifdef CSSD_PROPS_FROM_ENUM
    for (int p = 0; p < CSSP__COUNT; p++) {
        const char *d = css_prop_name(p);
        if (d && *d) install_one_css_prop(ctx, proto, d, p);
    }
    install_one_css_prop(ctx, proto, "cssFloat", CSSP_FLOAT);
    return;
#endif
    int n = css_known_prop_count();
    int float_idx = -1;
    /* If LibCSS ever carries more properties than the extra-table base, the two
     * magic ranges would overlap and every name past the base would read the
     * wrong property. Fail loudly at that point rather than silently. */
    if (n > CSSD_EXTRA_BASE) n = CSSD_EXTRA_BASE;
    for (int p = 0; p < n; p++) {
        const char *d = css_known_prop_at(p, 0);
        if (!d || !*d) continue;
        if (!strcmp(d, "float")) float_idx = p;
        install_one_css_prop(ctx, proto, d, p);
    }
    /* and the properties canon.c claims that never entered that stringmap,
     * enumerated from canon.c itself rather than transcribed -- a name it
     * gains is settable in the same commit that adds its serializer. */
    int ne = css_canon_prop_count();
    for (int i = 0; i < ne; i++) {
        const char *e = css_canon_prop_at(i);
        if (e && *e) install_one_css_prop(ctx, proto, e, CSSD_EXTRA_BASE + i);
    }
    /* `float` was a reserved word when the CSSOM was written, so the IDL name
     * is cssFloat -- and both spellings are in use to this day. */
    if (float_idx >= 0) {
        JSAtom a = JS_NewAtom(ctx, "cssFloat");
        JS_DefinePropertyGetSet(ctx, proto, a,
            JS_NewCFunction2(ctx, (JSCFunction *)cssd_prop_get, "cssFloat", 0,
                             JS_CFUNC_getter_magic, float_idx),
            JS_NewCFunction2(ctx, (JSCFunction *)cssd_prop_set, "cssFloat", 1,
                             JS_CFUNC_setter_magic, float_idx),
            JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
}

static JSValue el_get_style(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    /* Not cached in the node: the Element wrapper owns the one weak slot a node
     * has, and a fresh declaration object costs one small malloc while a cache
     * would cost a second slot in every struct node in the document. */
    return n ? cssd_new(ctx, n, CSSD_INLINE) : JS_NULL;
}

/* window.getComputedStyle(el[, pseudo]).
 *
 * The pseudo-element argument is accepted and ignored: we have no ::before /
 * ::after boxes to report on, and returning the element's own style is a far
 * better answer than throwing at a page that passes `null` (which is by far the
 * most common second argument in the wild). */
static JSValue js_getComputedStyle(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_NULL;
    struct node *n = node_of(argv[0]);
    if (!n) return JS_NULL;
    return cssd_new(ctx, n, CSSD_COMPUTED);
}

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
    /* The one place that is reliably reached after js_page_open has run EVERY
     * installer and before a page script can observe the result. */
    iface_bridge(ctx);
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

/* The ONE writable slot on an event, and it exists for one caller.
 *
 * `document.createEvent('Event')` hands back an uninitialised event whose whole
 * point is that `initEvent(type, ...)` names it afterwards. `type` lives in the
 * C struct and had no setter, so js_events.c -- which owns initEvent and
 * createEvent -- could only shadow the JS-visible `type` with an own property
 * and then, at dispatch time, build a WHOLE FRESH native event carrying the
 * same prototype and slot, because the native dispatcher looks up listeners by
 * the struct's type and not by the shadow. The listener then received an object
 * that was not the one the page had initialised.
 *
 * DELIBERATE DEVIATION, stated because it is observable: the DOM says
 * Event.type is readonly, so `Object.getOwnPropertyDescriptor(Event.prototype,
 * 'type').set` is undefined in a browser and is a function here. That is the
 * price of deleting a whole re-dispatch workaround from the event line, and it
 * is nearly invisible in practice -- initEvent shadows `type` with an own data
 * property, so a page reads the shadow, not this pair. Refused mid-dispatch,
 * which is what initEvent's own no-op rule amounts to. */
static JSValue ev_set(JSContext *ctx, JSValueConst t, JSValueConst v, int magic)
{
    struct jsevent *e = JS_GetOpaque(t, event_cid);
    if (!e || magic != EG_TYPE || e->dispatching) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_UNDEFINED;
    char *nt = dupstr(s);
    JS_FreeCString(ctx, s);
    if (nt) { free(e->type); e->type = nt; }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry event_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("type", ev_get, ev_set, EG_TYPE),
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
    JS_FreeValue(ctx, g_document);    g_document    = JS_UNDEFINED;
    iface_cleanup(ctx);               /* the interface prototypes are strong refs too */
    if (g_root) dom_clear_wrappers(g_root->doc);
    g_ctx = 0;
}

/* ---- the members, SPLIT BY THE INTERFACE THAT DEFINES THEM ----
 *
 * This used to be one table on one prototype, and the comment above the Node
 * section said why: our wrapper is keyed on a `struct node` whose type is a
 * runtime field, so one flat prototype was cheaper than four. What it cost was
 * every question a page asks about the SHAPE of the platform -- `x instanceof
 * HTMLBodyElement`, `Object.getPrototypeOf(el) === HTMLDivElement.prototype`,
 * patching `HTMLDialogElement.prototype.showModal` -- and those are not
 * academic: a real site died on `ReferenceError: HTMLBodyElement is not
 * defined`, and WPT tests the chain directly.
 *
 * The tables are still installed on ONE class (elem_cid); what changed is that
 * wrap() picks WHICH prototype object the new wrapper gets, and those objects
 * are chained EventTarget -> Node -> Element -> HTMLElement -> HTML*Element by
 * js_dom_iface.inc. The accessors are unchanged and still type-check the node
 * themselves, so a member reached on the wrong kind of node answers exactly
 * what it answered before. */
static const JSCFunctionListEntry evtarget_proto_funcs[] = {
    JS_CFUNC_DEF("addEventListener", 2, el_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, el_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, el_dispatchEvent),
};

static const JSCFunctionListEntry node_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", el_get_text, el_set_text),
    JS_CGETSET_DEF("nodeType", el_get_nodeType, NULL),
    JS_CGETSET_DEF("nodeName", el_get_nodeName, NULL),
    JS_CGETSET_DEF("nodeValue", el_get_nodeValue, el_set_nodeValue),
    JS_CGETSET_DEF("parentNode", el_get_parentNode, NULL),
    JS_CGETSET_DEF("parentElement", el_get_parentElement, NULL),
    JS_CGETSET_DEF("firstChild", el_get_firstChild, NULL),
    JS_CGETSET_DEF("lastChild", el_get_lastChild, NULL),
    JS_CGETSET_DEF("nextSibling", el_get_nextSibling, NULL),
    JS_CGETSET_DEF("previousSibling", el_get_prevSibling, NULL),
    JS_CGETSET_DEF("childNodes", el_get_childNodes, NULL),
    JS_CGETSET_DEF("ownerDocument", el_get_ownerDocument, NULL),
    JS_CFUNC_DEF("hasChildNodes", 0, el_hasChildNodes),
    JS_CFUNC_DEF("contains", 1, el_contains),
    JS_CFUNC_DEF("appendChild", 1, el_appendChild),
    JS_CFUNC_DEF("insertBefore", 2, el_insertBefore),
    JS_CFUNC_DEF("replaceChild", 2, el_replaceChild),
    JS_CFUNC_DEF("removeChild", 1, el_removeChild),
};

/* CharacterData: `data` is its own, and the element-sibling walk is the
 * NonDocumentTypeChildNode mixin, which a Text node has and a Document
 * does not. */
static const JSCFunctionListEntry chardata_proto_funcs[] = {
    JS_CGETSET_DEF("data", el_get_nodeValue, el_set_nodeValue),
};

static const JSCFunctionListEntry nondoctype_child_funcs[] = {
    JS_CGETSET_DEF("nextElementSibling", el_get_nextElemSib, NULL),
    JS_CGETSET_DEF("previousElementSibling", el_get_prevElemSib, NULL),
};

static const JSCFunctionListEntry element_proto_funcs[] = {
    JS_CGETSET_DEF("innerHTML", el_get_html, el_set_html),
    JS_CGETSET_DEF("tagName", el_get_tag, NULL),
    JS_CGETSET_DEF("id", el_get_id, el_set_id),
    JS_CGETSET_DEF("classList", el_get_classlist, NULL),
    JS_CGETSET_DEF("className", el_get_className, el_set_className),
    JS_CGETSET_DEF("style", el_get_style, NULL),
    JS_CGETSET_DEF("namespaceURI", el_get_namespaceURI, NULL),
    JS_CFUNC_DEF("getAttribute", 1, el_getattr),
    JS_CFUNC_DEF("setAttribute", 2, el_setattr),
    JS_CFUNC_DEF("removeAttribute", 1, el_removeAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, el_hasAttribute),
    JS_CFUNC_DEF("getBoundingClientRect", 0, el_getBoundingClientRect),
};

/* ParentNode: Element, Document and DocumentFragment all have it. */
static const JSCFunctionListEntry parentnode_funcs[] = {
    JS_CGETSET_DEF("children", el_get_children, NULL),
    JS_CGETSET_DEF("firstElementChild", el_get_firstElemChild, NULL),
    JS_CGETSET_DEF("lastElementChild", el_get_lastElemChild, NULL),
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
    JS_CFUNC_DEF("createElementNS", 2, doc_createElementNS),
    JS_CFUNC_DEF("createTextNode", 1, doc_createTextNode),
    JS_CFUNC_DEF("createComment", 1, doc_createComment),
    JS_CFUNC_DEF("createDocumentFragment", 0, doc_createFragment),
    JS_CFUNC_DEF("addEventListener", 2, doc_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, doc_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, doc_dispatchEvent),
    JS_CGETSET_DEF("body", doc_get_body, NULL),
    JS_CGETSET_DEF("head", doc_get_head, NULL),
    JS_CGETSET_DEF("documentElement", doc_get_docel, NULL),
    JS_CGETSET_DEF("nodeType", doc_get_nodeType, NULL),
};

/* THE INTERFACE HIERARCHY. Included, not linked -- see the header of the file
 * for why. It needs every member table above it and is needed by js_dom_init
 * below, which is exactly this spot. */
#include "js_dom_iface.inc"

/* ===================== IDL attribute reflection ==========================
 *
 * `el.title` is the `title` content attribute seen through a typed coercion,
 * and there are several hundred such pairs. The machinery and the generated
 * table live in js_reflect.c, which this file only has to CALL -- see that
 * file's header for what the types are and why they are the point.
 *
 * THE REFERENCE IS WEAK, deliberately. tests/domiface.mk, tests/loader.mk,
 * tests/cssom.mk, tests/webapi_platform.mk and eight Makefile rules list this
 * directory's sources by hand, and three of those files belong to other lines.
 * A hard reference would break their link the moment this landed, in a commit
 * they did not make. Weak, the same pattern js_page.c already uses for
 * js_webapi_install: a build without js_reflect.o links and simply has no
 * reflected attributes. The browser and tests/wpt.mk both glob js_*.c, so the
 * shipping browser and the measurement always have it.
 *
 * SPELLED `__weak__`, NOT `weak`. c/apps/libc/include/features.h:4 defines a
 * lowercase `weak` as __attribute__((__weak__)) for musl's own sources, and the
 * browser and QuickJS builds pull that header into every translation unit with
 * -include -- so the plain spelling expands to
 * __attribute__((__attribute__((__weak__)))) and the error points at this line
 * with no mention of the macro. 49b5039 fixed the identical trap in
 * c/apps/libc/src/pthread.c two hours before this hit, and its message is worth
 * reading: the host test builds do not use that -include, so this compiles
 * everywhere except the one target that ships. */
__attribute__((__weak__)) void js_reflect_install(
    JSContext *ctx, JSValueConst html_proto,
    JSValueConst (*proto_for)(void *, const char *), void *ud);

/* The prototype an element name's reflected attributes belong on.
 *
 * A tag with no dedicated interface answers HTMLElement.prototype rather than
 * nothing. That is not a fallback, it is the right answer for the one tag in
 * the table it applies to: html/dom/elements-misc.js describes `enterKeyHint`
 * and `inputMode` on a made-up `<undefinedelement>` precisely because they are
 * HTMLElement's, and it wants them tested somewhere that has no other
 * attributes to confuse the result. */
static JSValueConst reflect_proto_for(void *ud, const char *tag)
{
    (void)ud;
    if (!g_iface_ready || !tag) return JS_UNDEFINED;
    int i = itag_get(tag);
    if (i <= 0) i = IF_HTMLELEMENT;
    return g_iproto[i];
}

/* ---- what js_reflect.c needs from this file ---------------------------- */

struct node *js_dom_node_from(JSValueConst v) { return node_of(v); }

const char *js_dom_attr_len(const struct node *n, const char *name, int *len)
{ return attr_val_len(n, name, len); }

void js_dom_attr_write(JSContext *ctx, struct node *n, const char *name,
                       const char *val, int vlen)
{ attr_write(ctx, n, name, val, vlen); }

int js_dom_attr_erase(JSContext *ctx, struct node *n, const char *name)
{
    if (!attr_remove(n, name)) return 0;
    mark_self(n, INVAL_STYLE);
    named_note_attr(ctx, n, name);
    return 1;
}

/* Throw a real DOMException, built by the page's own constructor so that
 * `e instanceof DOMException`, `e.name` and the legacy `e.code` all answer --
 * which is exactly what assert_throws_dom checks. A build with no DOMException
 * global falls back to a TypeError rather than to no exception at all: the
 * caller's contract is "this operation throws". */
JSValue js_dom_throw_dom(JSContext *ctx, const char *name, const char *msg)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "DOMException");
    JSValue exc = JS_UNDEFINED;
    if (JS_IsFunction(ctx, ctor)) {
        JSValue argv[2] = { JS_NewString(ctx, msg ? msg : ""),
                            JS_NewString(ctx, name) };
        exc = JS_CallConstructor(ctx, ctor, 2, (JSValueConst *)argv);
        JS_FreeValue(ctx, argv[0]);
        JS_FreeValue(ctx, argv[1]);
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, g);
    if (!JS_IsObject(exc)) {
        JS_FreeValue(ctx, exc);
        return JS_ThrowTypeError(ctx, "%s: %s", name, msg ? msg : "");
    }
    return JS_Throw(ctx, exc);
}

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
    g_root = root;
    js_dom_clear_dirty();               /* scope roots from the previous page are dead */
    g_proto_event = g_proto_ui = g_proto_mouse = g_proto_key = JS_UNDEFINED;
    g_document = JS_UNDEFINED;          /* the previous page's, if any, died with its runtime */
    g_scroll_x = g_scroll_y = 0;        /* a new page starts at the top */
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
    /* A placeholder class prototype: iface_install() replaces it with
     * Node.prototype (or, in the negative-control build, fills this one flat
     * with every member, which is what this file did before the hierarchy). */
    JS_SetClassProto(ctx, elem_cid, JS_NewObject(ctx));

    JSValue token_proto_obj = JS_UNDEFINED;
    JS_NewClassID(&token_cid);
    if (JS_NewClass(rt, token_cid, &token_class) >= 0) {
        JSValue tp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, tp, token_proto, countof(token_proto));
        install_arraylike(ctx, tp);        /* length + indices make it iterable */
        token_proto_obj = JS_DupValue(ctx, tp);   /* DOMTokenList is published over it */
        JS_SetClassProto(ctx, token_cid, tp);
    }

    JS_NewClassID(&cssd_cid);
    if (JS_NewClass(rt, cssd_cid, &cssd_class) >= 0) {
        JSValue sp = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, sp, cssd_proto, countof(cssd_proto));
        install_css_props(ctx, sp);
        JS_SetClassProto(ctx, cssd_cid, sp);
    }

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "getComputedStyle",
                      JS_NewCFunction(ctx, js_getComputedStyle, "getComputedStyle", 2));

    /* BEFORE anything that wraps a node: iface_install() is what makes
     * iface_proto_for() answer, and a wrapper made before it would be stuck
     * with the placeholder prototype for the life of the page (wrappers are
     * cached in node->jsw and never rebuilt). */
    iface_install(ctx, g, token_proto_obj);
    JS_FreeValue(ctx, token_proto_obj);
    /* Record what this file owns on HTMLDivElement.prototype before anything
     * else can add to it -- section 6 of js_dom_iface.inc explains why the
     * bridge needs to tell the two apart. Sealed again after reflection
     * installs, which adds <div>'s own `align`. */
    iface_seal_div(ctx);

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

    /* `document` IS THE #document NODE now, not a look-alike beside it.
     *
     * It used to be a bare JSObject carrying doc_funcs as own properties, and
     * that made three true things false: `document instanceof Document`,
     * `document instanceof Node`, and every Node member on it -- childNodes,
     * appendChild, contains, compareDocumentPosition. A page walking up from an
     * element with parentNode hit an object that was not a node and stopped.
     *
     * Wrapping g_root instead costs nothing and fixes all of it: doc_funcs now
     * live on Document.prototype (installed by iface_install), the wrapper is
     * cached in g_root->jsw so `el.ownerDocument === document` and
     * `html.parentNode === document` still hand back the SAME object, and
     * node_of(document) resolves to the real #document node.
     *
     * With no root -- a host test that inits with NULL -- there is no node to
     * wrap, so it falls back to a plain object over Document.prototype. */
    JSValue doc;
    if (root) {
        doc = wrap(ctx, root);
    } else if (JS_IsObject(g_iproto[IF_DOCUMENT])) {
        doc = JS_NewObjectProto(ctx, g_iproto[IF_DOCUMENT]);
    } else {
        doc = JS_NewObject(ctx);
    }
    if (!JS_IsObject(doc)) { JS_FreeValue(ctx, doc); doc = JS_NewObject(ctx); }
    /* In the negative-control build there is no Document.prototype to inherit
     * doc_funcs from, so they go on the object, exactly as before. */
    if (!g_iface_ready) {
        JS_SetPropertyFunctionList(ctx, doc, doc_funcs, countof(doc_funcs));
        JS_SetPropertyFunctionList(ctx, doc, document_extra_funcs,
                                   countof(document_extra_funcs));
    }
    install_on_props(ctx, doc, doc_on_get, doc_on_set);
    /* Held strongly so ownerDocument and <html>.parentNode can hand the SAME
     * object back -- `el.ownerDocument === document` has to be true. Released
     * by js_dom_cleanup, alongside the event prototypes. */
    g_document = JS_DupValue(ctx, doc);
    JS_SetPropertyStr(ctx, g, "document", doc);
    /* AFTER the prototypes exist and BEFORE any page script or any other
     * installer runs. js_forms.c and js_platform.c both add reflected
     * properties of their own and both refuse to clobber one that is already
     * there, so being first is what lets the typed implementation win where the
     * two overlap and lets theirs stand where they reach further. */
    if (js_reflect_install) {
        js_reflect_install(ctx, g_iproto[IF_HTMLELEMENT], reflect_proto_for, 0);
        iface_seal_div(ctx);          /* <div>.align is now ours too */
    }
    /* AFTER `document`: named access must never shadow a real global, and
     * `document` is one. Everything js_page.c installs after this point is
     * likewise protected -- named_define skips any name the global already
     * owns, and js_select/js_platform/js_media/js_events all install with
     * plain defines that overwrite, so a later `window.location` wins too. */
    /* BEFORE named_scan, which is the whole point of it: the rule that a real
     * global beats a named element can only protect a property that exists, and
     * `window.parent` did not. See named_install_window in js_dom_iface.inc. */
    named_install_window(ctx, g);
    named_scan(ctx, root);
    JS_FreeValue(ctx, g);
    maybe_install_console(ctx);
}
