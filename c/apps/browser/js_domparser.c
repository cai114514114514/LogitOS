/* DOMParser: `new DOMParser().parseFromString(str, "text/html")`.
 *
 * Scoreboard motivation: 2345's vendor chunk dies on "ReferenceError:
 * DOMParser is not defined" -- it is a top-5 feature-detect in real bundles
 * (`typeof DOMParser !== 'undefined'` gates a whole code path in a lot of
 * shipped JS, independent of whether the result is ever used for much).
 *
 * WHAT THIS PARSES WITH: the exact same tree builder the page itself uses.
 * `dom_parse()` (dom.h) is a one-line call into `html_parse()`
 * (html_tree.c/html_tokenizer.c, the WHATWG tree construction algorithm) --
 * it is what browser.c calls to build the page's own DOM. Calling it again
 * here just hands back a SECOND, independent `struct dom_doc` instead of
 * replacing the live one: same parser, same spec conformance, a detached
 * result.
 *
 * ============================== LIFETIME ==================================
 * The hard part, and the one worth stating plainly before the code: a parsed
 * detached document must not dangle after navigation (js_page.h's ordering
 * rule -- the DOM must outlive the runtime, enforced today by js_page_close()
 * running js_dom_cleanup() before dom_free()).
 *
 * This file has NO js_page_close hook and needs none, because it never keeps
 * a C-side handle to anything JS can outlive. Every parsed document is owned
 * by a refcounted `struct dp_arena` (the dom_doc + its root, see below), and
 * the ONLY things that hold a reference to an arena are the JS wrapper
 * objects `parseFromString` and its Document/Node accessors hand back --
 * never a global, never anything js_domparser.c itself keeps alive. So:
 *
 *   - A script that drops every reference to a parsed document (or never
 *     takes one beyond the `parseFromString` return value it discards) lets
 *     QuickJS's ordinary GC collect the wrapper, the finalizer drops the
 *     arena's refcount, and at zero the whole detached dom_doc is freed --
 *     same as any other JS object leaking no native memory.
 *   - A script that keeps a Document (or a node from it) alive for the whole
 *     page: the arena lives exactly that long, plainly, refcounted per
 *     surviving wrapper.
 *   - Navigation: js_page_close() calls JS_FreeContext(g_ctx) before
 *     JS_FreeRuntime(g_rt), and js_page.c's own header states the invariant
 *     this depends on -- "JS_FreeRuntime... asserts on live GC objects", i.e.
 *     by the time it runs, JS_FreeContext has already finalized every object
 *     the context owned, cycles included. Our wrapper finalizer is ordinary
 *     QuickJS teardown, so every outstanding parsed document (and its arena)
 *     is freed as part of that -- automatically, with no separate close call
 *     to remember, and with NO way to reach a parsed document after close:
 *     the only handles to one were JS objects belonging to the context that
 *     no longer exists. "Refuse after close" is not a check this file
 *     performs -- it is what happens because there is nothing left to ask.
 *
 * ============================ WHAT IS REUSED ===============================
 * "Wrap the detached tree with the SAME node-wrapper machinery js_dom.c
 * already has" turned out to have a real boundary: js_dom.c's wrapper class
 * (`elem_cid`, `wrap()`, the whole Element/Text/Document interface
 * hierarchy in js_dom_iface.inc) is `static` to that file and hardwired to
 * ONE live document (`g_root`, `g_document`) -- getElementById,
 * querySelector, createElement, even the mutation invalidation record all
 * read or write those globals directly. None of it is exported for a second,
 * independent document to use, and this unit does not own js_dom.c to change
 * that. js_dom.h's public surface (`js_dom_node_from` and friends) is the
 * OTHER direction: it lets code OUTSIDE js_dom.c resolve an already-live
 * page wrapper back to its node, which is not this problem either.
 *
 * So this file builds its own small parallel wrapper -- one JSClassID
 * (`dp_cid`), one `{arena, node, serial}` handle struct, one weak-slot cache
 * (`node->jsw`, via dom.h's PUBLIC `dom_set_wrapper`/`n->jsw` -- that part
 * IS shared, because it is generic over any document, not js_dom.c's) -- and
 * documents the boundary here rather than reaching into js_dom.c's statics.
 * It mirrors js_dom.c's actual shapes closely on purpose (same
 * {node,serial}-recycle-safety idea, same textContent/nodeType/traversal
 * semantics, same "childNodes/children are snapshots, not live" deviation)
 * so a script sees the same behaviour whether it is looking at `document` or
 * a `DOMParser` result -- it just cannot be dispatched through the same
 * QuickJS class, because that class is privately owned elsewhere.
 *
 * ============================ WHAT IS NOT DONE ==============================
 * Only text/html is parsed. "application/xml" / "text/xml" /
 * "application/xhtml+xml" / "image/svg+xml" refuse LOUDLY, on purpose: this
 * tree has no XML parser (LibCSS needs none, and nothing else in the browser
 * has ever needed one), and the spec requires parseFromString to never
 * throw -- so an XML request gets a one-element detached document
 * (`<parsererror>`, spec-precedented name) explaining why, plus a printf so
 * the omission is loud on serial, never silent.
 *
 * Only what the caller listed as the minimum is implemented, plus the
 * handful of accessors (id/className/getAttribute/contains/tagName) that
 * cost nothing extra once the wrapper exists and that real querySelector
 * results are immediately used for: getElementById, querySelector(All)
 * (a hand-rolled matcher -- tag/#id/.class/[attr]/[attr=val], descendant and
 * child combinators, comma groups; no sibling combinators, no pseudo-classes
 * -- documented here rather than pretending to be LibCSS), .body,
 * .documentElement, textContent (get AND set -- cheap once dom_text_append
 * and dom_destroy_children are in scope, and symmetric with every other
 * read this file exposes), and Node traversal (parent/child/sibling,
 * element-only variants, childNodes/children as ARRAY SNAPSHOTS -- same
 * known deviation js_dom.c's child_array documents, for the same reason: a
 * live NodeList needs a second wrapper class with its own recycle-safety,
 * and nothing real depends on the list itself surviving a mutation the
 * script didn't just cause).
 */
#include "quickjs.h"
#include "dom.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---- the arena: one detached dom_doc, refcounted by its live wrappers ---- */
struct dp_arena {
    struct dom_doc *doc;
    struct node    *root;      /* N_DOCUMENT, what dom_free() takes */
    int             refcnt;
};

struct dp_handle { struct dp_arena *arena; struct node *n; uint32_t serial; };

static JSClassID dp_cid;

/* Set by js_domparser_install, valid for as long as the one page runtime is
 * (same singleton assumption js_dom.c's g_ctx/g_root make -- exactly one page
 * runtime exists at a time, and a new js_page_open always closes the old one
 * first). Re-created on every install call; the previous context's values
 * become unreachable garbage inside a runtime that install's caller has
 * already torn down, never dereferenced again. */
static JSValue g_dp_node_proto, g_dp_doc_proto;

static JSValue dp_wrap(JSContext *ctx, struct dp_arena *a, struct node *n);

static void dp_arena_unref(struct dp_arena *a)
{
    if (!a) return;
    if (--a->refcnt > 0) return;
    dom_free(a->root);
    free(a);
}

static void dp_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    struct dp_handle *h = JS_GetOpaque(val, dp_cid);
    if (!h) return;
    /* Same guard as js_dom.c's elem_finalizer: only clear the weak slot if it
     * still points at THIS object -- after a wrapper is replaced (can't
     * happen here today since nothing mutates the tree post-parse, but the
     * check costs nothing and keeps the two files' invariants identical). */
    if (h->n && h->n->serial == h->serial && h->n->jsw == JS_VALUE_GET_PTR(val))
        dom_set_wrapper(h->n, NULL);
    dp_arena_unref(h->arena);
    free(h);
}
static JSClassDef dp_class = { "DOMParserNode", dp_finalizer };

static struct dp_handle *dp_h(JSValueConst v) { return JS_GetOpaque(v, dp_cid); }

/* Resolve to the live node, or NULL if the slot was recycled underneath the
 * handle. Nothing in this file mutates a parsed tree's shape after parsing,
 * so today this can only fail if a future caller adds mutation -- the guard
 * is here so that addition inherits safety instead of having to invent it. */
static struct node *dp_of(JSValueConst v)
{
    struct dp_handle *h = dp_h(v);
    if (!h || !h->n || h->n->serial != h->serial) return 0;
    return h->n;
}

static int dp_lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ============================ selector engine =============================
 * A hand-rolled subset, not LibCSS: tag | * | #id | .class | [attr] |
 * [attr=value] | [attr="value"], any number of the last four chained onto one
 * compound, compounds joined by descendant (space) or child (>) combinators,
 * comma-separated groups (first match wins for querySelector, union in
 * document order for querySelectorAll). No sibling combinators (+ ~), no
 * pseudo-classes/elements, no attribute operators beyond exact match. That
 * covers ordinary "find the thing I just parsed" usage; anything fancier is
 * out of scope for a detached fragment parser and is refused by simply not
 * matching (never a JS exception -- a selector this engine can't understand
 * behaves like a selector that matched nothing, not a crash). */
#define DPSEL_MAXGROUPS 8
#define DPSEL_MAXSTEPS  6
#define DPSEL_COMPLEN   96

struct dpsel_step { char comb; char text[DPSEL_COMPLEN]; };   /* comb: 0 (first), ' ' (descendant), '>' (child) */
struct dpsel_group { struct dpsel_step steps[DPSEL_MAXSTEPS]; int n; };

static int dpsel_ieq_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (!a[i]) return 0;
        if (dp_lc((unsigned char)a[i]) != dp_lc((unsigned char)b[i])) return 0;
    }
    return a[n] == 0;
}

static int dpsel_word_has(const char *cls, const char *w)
{
    if (!cls) return 0;
    int wl = (int)strlen(w);
    const char *p = cls;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if ((int)(p - s) == wl && strncmp(s, w, (size_t)wl) == 0) return 1;
    }
    return 0;
}

/* One compound selector ("div.card#x[data-y=z]") against one element. */
static int dpsel_compound_match(struct node *n, const char *sel)
{
    if (n->type != N_ELEM) return 0;
    const char *p = sel;
    if (*p && *p != '.' && *p != '#' && *p != '[') {
        const char *start = p;
        while (*p && *p != '.' && *p != '#' && *p != '[') p++;
        int len = (int)(p - start);
        if (!(len == 1 && start[0] == '*') && !dpsel_ieq_n(n->tag, start, len))
            return 0;
    }
    while (*p) {
        if (*p == '.') {
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != '#' && *p != '[') p++;
            int len = (int)(p - start);
            char buf[DPSEL_COMPLEN]; int cl = len < DPSEL_COMPLEN - 1 ? len : DPSEL_COMPLEN - 1;
            memcpy(buf, start, (size_t)cl); buf[cl] = 0;
            if (!dpsel_word_has(dom_attr(n, "class"), buf)) return 0;
        } else if (*p == '#') {
            p++;
            const char *start = p;
            while (*p && *p != '.' && *p != '#' && *p != '[') p++;
            int len = (int)(p - start);
            const char *id = dom_attr(n, "id");
            if (!id || (int)strlen(id) != len || strncmp(id, start, (size_t)len) != 0) return 0;
        } else if (*p == '[') {
            p++;
            const char *nstart = p;
            while (*p && *p != ']' && *p != '=') p++;
            int nlen = (int)(p - nstart);
            char nbuf[64]; int ncl = nlen < (int)sizeof nbuf - 1 ? nlen : (int)sizeof nbuf - 1;
            memcpy(nbuf, nstart, (size_t)ncl); nbuf[ncl] = 0;
            if (*p == '=') {
                p++;
                char q = 0;
                if (*p == '"' || *p == '\'') { q = *p; p++; }
                const char *vstart = p;
                while (*p && (q ? *p != q : *p != ']')) p++;
                int vlen = (int)(p - vstart);
                char vbuf[128]; int vcl = vlen < (int)sizeof vbuf - 1 ? vlen : (int)sizeof vbuf - 1;
                memcpy(vbuf, vstart, (size_t)vcl); vbuf[vcl] = 0;
                if (q && *p == q) p++;
                const char *av = dom_attr(n, nbuf);
                if (!av || strcmp(av, vbuf) != 0) return 0;
            } else if (!dom_attr(n, nbuf)) return 0;
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
        } else break;
    }
    return 1;
}

/* Compile a full selector list into up to DPSEL_MAXGROUPS groups. Anything
 * past the caps (too many groups/steps, a token longer than the buffer) is
 * silently dropped from that group rather than overrunning anything -- a
 * selector nobody realistic writes just fails to match, same as an
 * unsupported combinator. Returns the group count. */
static int dpsel_compile(const char *sel, struct dpsel_group *out)
{
    int ng = 0;
    const char *p = sel;
    while (*p && ng < DPSEL_MAXGROUPS) {
        while (*p == ' ') p++;
        struct dpsel_group *g = &out[ng];
        g->n = 0;
        char pending_comb = 0;
        int have_step = 0;
        while (*p && *p != ',') {
            while (*p == ' ') p++;
            if (!*p || *p == ',') break;
            if (*p == '>') { pending_comb = '>'; p++; while (*p == ' ') p++; continue; }
            const char *start = p;
            while (*p && *p != ' ' && *p != '>' && *p != ',') {
                if (*p == '[') { while (*p && *p != ']') p++; if (*p == ']') p++; continue; }
                p++;
            }
            int len = (int)(p - start);
            if (len > 0 && g->n < DPSEL_MAXSTEPS) {
                struct dpsel_step *st = &g->steps[g->n];
                st->comb = have_step ? (pending_comb ? pending_comb : ' ') : 0;
                int cl = len < DPSEL_COMPLEN - 1 ? len : DPSEL_COMPLEN - 1;
                memcpy(st->text, start, (size_t)cl); st->text[cl] = 0;
                g->n++;
            }
            have_step = 1;
            pending_comb = 0;
        }
        if (g->n > 0) ng++;
        if (*p == ',') p++;
    }
    return ng;
}

static int dpsel_group_matches(struct node *el, const struct dpsel_group *g)
{
    if (g->n == 0) return 0;
    if (!dpsel_compound_match(el, g->steps[g->n - 1].text)) return 0;
    struct node *cur = el;
    for (int i = g->n - 2; i >= 0; i--) {
        char comb = g->steps[i + 1].comb;
        if (comb == '>') {
            cur = cur->parent;
            if (!cur || !dpsel_compound_match(cur, g->steps[i].text)) return 0;
        } else {
            struct node *p = cur->parent;
            int found = 0;
            while (p) {
                if (dpsel_compound_match(p, g->steps[i].text)) { cur = p; found = 1; break; }
                p = p->parent;
            }
            if (!found) return 0;
        }
    }
    return 1;
}

static int dpsel_any_matches(struct node *el, const struct dpsel_group *gs, int ng)
{
    for (int i = 0; i < ng; i++) if (dpsel_group_matches(el, &gs[i])) return 1;
    return 0;
}

/* Pre-order, descendants of `n` only (never `n` itself -- both
 * Element.querySelector and Document.querySelector are "within this
 * subtree", and a Document node is never itself a match candidate since
 * dpsel_compound_match refuses anything that isn't N_ELEM). */
static struct node *dpsel_walk_first(struct node *n, const struct dpsel_group *gs, int ng)
{
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type == N_ELEM && dpsel_any_matches(c, gs, ng)) return c;
        struct node *r = dpsel_walk_first(c, gs, ng);
        if (r) return r;
    }
    return 0;
}
static void dpsel_walk_all(JSContext *ctx, struct dp_arena *a, struct node *n,
                           const struct dpsel_group *gs, int ng, JSValue arr, uint32_t *idx)
{
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type == N_ELEM && dpsel_any_matches(c, gs, ng))
            JS_DefinePropertyValueUint32(ctx, arr, (*idx)++, dp_wrap(ctx, a, c), JS_PROP_C_W_E);
        dpsel_walk_all(ctx, a, c, gs, ng, arr, idx);
    }
}

/* ============================ node accessors =============================== */

static int dp_node_type(const struct node *n)
{
    switch (n->type) {
    case N_ELEM:     return 1;
    case N_TEXT:     return 3;
    case N_COMMENT:  return 8;
    case N_DOCUMENT: return 9;
    case N_DOCTYPE:  return 10;
    default:         return 0;
    }
}
static JSValue dp_get_nodeType(JSContext *ctx, JSValueConst t)
{ struct node *n = dp_of(t); return n ? JS_NewInt32(ctx, dp_node_type(n)) : JS_UNDEFINED; }

/* tagName/nodeName uppercase HTML elements, exactly as js_dom.c's
 * tagname_value does and for the same reason (`el.tagName === 'DIV'` is a
 * comparison the web writes constantly); SVG/MathML names stay as-authored. */
static JSValue dp_tagname_value(JSContext *ctx, const struct node *n)
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
static JSValue dp_get_tagName(JSContext *ctx, JSValueConst t)
{ struct node *n = dp_of(t); return (n && n->type == N_ELEM) ? dp_tagname_value(ctx, n) : JS_UNDEFINED; }
static JSValue dp_get_nodeName(JSContext *ctx, JSValueConst t)
{
    struct node *n = dp_of(t);
    if (!n) return JS_UNDEFINED;
    return n->type == N_ELEM ? dp_tagname_value(ctx, n) : JS_NewString(ctx, n->tag);
}

static JSValue dp_get_nodeValue(JSContext *ctx, JSValueConst t)
{
    struct node *n = dp_of(t);
    if (!n) return JS_UNDEFINED;
    if (n->type != N_TEXT && n->type != N_COMMENT) return JS_NULL;
    return JS_NewStringLen(ctx, n->text ? n->text : "", (size_t)(n->text ? n->textlen : 0));
}

/* ---- textContent: get and set. A growable byte buffer for the get side,
 * same shape as js_dom.c's sbuf -- textContent has no business being capped. */
struct dp_sbuf { char *p; size_t len, cap; };
static int dp_sb_push(struct dp_sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n; b->p[b->len] = 0;
    return 1;
}
static void dp_gather_text(struct node *root, struct dp_sbuf *b)
{
    struct node *n = root;
    while (n) {
        if (n->type == N_TEXT && n->text) dp_sb_push(b, n->text, (size_t)n->textlen);
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) break;
        n = n->next;
    }
}
static JSValue dp_get_text(JSContext *ctx, JSValueConst t)
{
    struct node *n = dp_of(t);
    if (!n) return JS_UNDEFINED;
    if (n->type == N_DOCUMENT || n->type == N_DOCTYPE) return JS_NULL;
    struct dp_sbuf b = { 0, 0, 0 };
    dp_gather_text(n, &b);
    JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.len);
    free(b.p);
    return v;
}
static JSValue dp_set_text(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = dp_of(t);
    if (!n || n->type == N_DOCUMENT || n->type == N_DOCTYPE) return JS_UNDEFINED;
    size_t sl = 0;
    const char *s = JS_ToCStringLen(ctx, &sl, v);
    if (!s) return JS_UNDEFINED;
    if (n->type == N_TEXT || n->type == N_COMMENT) {
        n->textlen = 0;
        if (n->text && n->textcap > 0) n->text[0] = 0;
        if (sl) dom_text_append(n, s, (int)sl);
    } else {
        dom_destroy_children(n);
        if (*s) { struct node *tn = dom_create_text(n->doc, s, -1); if (tn) dom_append_child(n, tn); }
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- navigation ---- */
static JSValue dp_get_parentNode(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t);
  if (!n) return JS_UNDEFINED; return n->parent ? dp_wrap(ctx, h->arena, n->parent) : JS_NULL; }
static JSValue dp_get_parentElement(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t);
  if (!n || !n->parent || n->parent->type != N_ELEM) return JS_NULL; return dp_wrap(ctx, h->arena, n->parent); }
static JSValue dp_get_firstChild(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, n->first_child) : JS_UNDEFINED; }
static JSValue dp_get_lastChild(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, n->last_child) : JS_UNDEFINED; }
static JSValue dp_get_nextSibling(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, n->next) : JS_UNDEFINED; }
static JSValue dp_get_prevSibling(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, n->prev) : JS_UNDEFINED; }

static struct node *dp_next_elem(struct node *n) { while (n && n->type != N_ELEM) n = n->next; return n; }
static struct node *dp_prev_elem(struct node *n) { while (n && n->type != N_ELEM) n = n->prev; return n; }
static JSValue dp_get_firstElemChild(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, dp_next_elem(n->first_child)) : JS_UNDEFINED; }
static JSValue dp_get_lastElemChild(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, dp_prev_elem(n->last_child)) : JS_UNDEFINED; }
static JSValue dp_get_nextElemSib(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, dp_next_elem(n->next)) : JS_UNDEFINED; }
static JSValue dp_get_prevElemSib(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_wrap(ctx, h->arena, dp_prev_elem(n->prev)) : JS_UNDEFINED; }

/* childNodes / children: ARRAY SNAPSHOTS, not live -- see the file header's
 * "what is reused" note. Same simplification js_dom.c's child_array makes. */
static JSValue dp_child_array(JSContext *ctx, struct dp_arena *a, struct node *n, int elems_only)
{
    JSValue arr = JS_NewArray(ctx);
    if (!n || JS_IsException(arr)) return arr;
    uint32_t i = 0;
    for (struct node *c = n->first_child; c; c = c->next) {
        if (elems_only && c->type != N_ELEM) continue;
        JS_DefinePropertyValueUint32(ctx, arr, i++, dp_wrap(ctx, a, c), JS_PROP_C_W_E);
    }
    return arr;
}
static JSValue dp_get_childNodes(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_child_array(ctx, h->arena, n, 0) : JS_UNDEFINED; }
static JSValue dp_get_children(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); struct node *n = dp_of(t); return n ? dp_child_array(ctx, h->arena, n, 1) : JS_UNDEFINED; }

static JSValue dp_get_ownerDocument(JSContext *ctx, JSValueConst t)
{
    struct dp_handle *h = dp_h(t); struct node *n = dp_of(t);
    if (!n) return JS_UNDEFINED;
    if (n->type == N_DOCUMENT) return JS_NULL;
    return dp_wrap(ctx, h->arena, h->arena->root);
}

static JSValue dp_get_id(JSContext *ctx, JSValueConst t)
{ struct node *n = dp_of(t); const char *v = n ? dom_attr(n, "id") : 0; return JS_NewString(ctx, v ? v : ""); }
static JSValue dp_get_className(JSContext *ctx, JSValueConst t)
{ struct node *n = dp_of(t); const char *v = n ? dom_attr(n, "class") : 0; return JS_NewString(ctx, v ? v : ""); }

static JSValue dp_getAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = dp_of(t);
    if (!n || argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]); if (!nm) return JS_NULL;
    const char *v = dom_attr(n, nm);
    JS_FreeCString(ctx, nm);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}
static JSValue dp_hasAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = dp_of(t);
    if (!n || argc < 1) return JS_FALSE;
    const char *nm = JS_ToCString(ctx, argv[0]); if (!nm) return JS_FALSE;
    int has = dom_attr(n, nm) != 0;
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, has);
}

static int dp_is_ancestor(const struct node *a, const struct node *b)
{ for (const struct node *p = b ? b->parent : 0; p; p = p->parent) if (p == a) return 1; return 0; }
static JSValue dp_contains(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = dp_of(t);
    if (!n || argc < 1) return JS_FALSE;
    struct node *o = dp_of(argv[0]);
    return JS_NewBool(ctx, o && (o == n || dp_is_ancestor(n, o)));
}

/* querySelector/querySelectorAll: subtree of `this`, `this` excluded. Works
 * identically whether `this` is the Document (subtree = the whole parsed
 * tree) or an Element (subtree = its descendants) -- one implementation,
 * inherited by both prototypes, exactly the way the real DOM's ParentNode
 * mixin is shared. */
static JSValue dp_querySelector(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    struct node *scope = dp_of(t);
    if (!scope || argc < 1) return JS_NULL;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return JS_NULL;
    struct dpsel_group gs[DPSEL_MAXGROUPS];
    int ng = dpsel_compile(sel, gs);
    struct node *found = ng > 0 ? dpsel_walk_first(scope, gs, ng) : 0;
    JS_FreeCString(ctx, sel);
    return dp_wrap(ctx, h->arena, found);
}
static JSValue dp_querySelectorAll(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    struct node *scope = dp_of(t);
    JSValue arr = JS_NewArray(ctx);
    if (!scope || argc < 1 || JS_IsException(arr)) return arr;
    const char *sel = JS_ToCString(ctx, argv[0]);
    if (!sel) return arr;
    struct dpsel_group gs[DPSEL_MAXGROUPS];
    int ng = dpsel_compile(sel, gs);
    uint32_t idx = 0;
    if (ng > 0) dpsel_walk_all(ctx, h->arena, scope, gs, ng, arr, &idx);
    JS_FreeCString(ctx, sel);
    return arr;
}

/* ---- document-only: getElementById, body, documentElement ---- */
static JSValue dp_doc_getById(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    if (!h || argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]); if (!id) return JS_NULL;
    struct node *n = dom_get_element_by_id(h->arena->doc, id);
    JS_FreeCString(ctx, id);
    return dp_wrap(ctx, h->arena, n);
}
static JSValue dp_doc_get_body(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); return h ? dp_wrap(ctx, h->arena, dom_doc_body(h->arena->doc)) : JS_UNDEFINED; }
static JSValue dp_doc_get_docel(JSContext *ctx, JSValueConst t)
{ struct dp_handle *h = dp_h(t); return h ? dp_wrap(ctx, h->arena, dom_doc_element(h->arena->doc)) : JS_UNDEFINED; }

/* ============================ property tables =============================
 * Node: everything an Element, Text, Comment, Doctype or the Document itself
 * can answer. Not split into a real Element/CharacterData/Document interface
 * hierarchy the way js_dom.c's js_dom_iface.inc is -- this file only needs
 * inspection, not spec-faithful `instanceof HTMLDivElement`, and every
 * accessor already checks the node kind it needs (tagName on a Text node
 * reads undefined, not a wrong string). */
static const JSCFunctionListEntry dp_node_funcs[] = {
    JS_CGETSET_DEF("nodeType", dp_get_nodeType, NULL),
    JS_CGETSET_DEF("nodeName", dp_get_nodeName, NULL),
    JS_CGETSET_DEF("nodeValue", dp_get_nodeValue, NULL),
    JS_CGETSET_DEF("textContent", dp_get_text, dp_set_text),
    JS_CGETSET_DEF("parentNode", dp_get_parentNode, NULL),
    JS_CGETSET_DEF("parentElement", dp_get_parentElement, NULL),
    JS_CGETSET_DEF("firstChild", dp_get_firstChild, NULL),
    JS_CGETSET_DEF("lastChild", dp_get_lastChild, NULL),
    JS_CGETSET_DEF("nextSibling", dp_get_nextSibling, NULL),
    JS_CGETSET_DEF("previousSibling", dp_get_prevSibling, NULL),
    JS_CGETSET_DEF("childNodes", dp_get_childNodes, NULL),
    JS_CGETSET_DEF("children", dp_get_children, NULL),
    JS_CGETSET_DEF("firstElementChild", dp_get_firstElemChild, NULL),
    JS_CGETSET_DEF("lastElementChild", dp_get_lastElemChild, NULL),
    JS_CGETSET_DEF("nextElementSibling", dp_get_nextElemSib, NULL),
    JS_CGETSET_DEF("previousElementSibling", dp_get_prevElemSib, NULL),
    JS_CGETSET_DEF("ownerDocument", dp_get_ownerDocument, NULL),
    JS_CGETSET_DEF("tagName", dp_get_tagName, NULL),
    JS_CGETSET_DEF("id", dp_get_id, NULL),
    JS_CGETSET_DEF("className", dp_get_className, NULL),
    JS_CFUNC_DEF("getAttribute", 1, dp_getAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, dp_hasAttribute),
    JS_CFUNC_DEF("contains", 1, dp_contains),
    JS_CFUNC_DEF("querySelector", 1, dp_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, dp_querySelectorAll),
};

static const JSCFunctionListEntry dp_doc_funcs[] = {
    JS_CFUNC_DEF("getElementById", 1, dp_doc_getById),
    JS_CGETSET_DEF("body", dp_doc_get_body, NULL),
    JS_CGETSET_DEF("documentElement", dp_doc_get_docel, NULL),
};

static JSValueConst dp_proto_for(const struct node *n)
{ return n->type == N_DOCUMENT ? g_dp_doc_proto : g_dp_node_proto; }

/* One wrapper per node, cached in node->jsw exactly like js_dom.c's wrap() --
 * the PUBLIC half of that mechanism (dom.h's dom_set_wrapper), so
 * `doc.body === doc.body` and a script can hang expandos off a parsed
 * element. */
static JSValue dp_wrap(JSContext *ctx, struct dp_arena *a, struct node *n)
{
    if (!n) return JS_NULL;
    if (n->jsw) return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, n->jsw));
    JSValue o = JS_NewObjectProtoClass(ctx, dp_proto_for(n), dp_cid);
    if (JS_IsException(o)) return o;
    struct dp_handle *h = malloc(sizeof *h);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    h->arena = a; h->n = n; h->serial = n->serial;
    a->refcnt++;
    JS_SetOpaque(o, h);
    dom_set_wrapper(n, JS_VALUE_GET_PTR(o));
    return o;
}

/* ============================ DOMParser itself ============================= */

int printf(const char *, ...);

static JSValue dp_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    return obj;
}

/* mimeType matching: ASCII case-insensitive, the way MIME types always are. */
static int dp_mime_is(const char *mime, const char *want)
{
    if (!mime) return 0;
    size_t i = 0;
    for (; mime[i] && want[i]; i++) if (dp_lc((unsigned char)mime[i]) != want[i]) return 0;
    return mime[i] == 0 && want[i] == 0;
}

static JSValue dp_parseFromString(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    size_t slen = 0;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &slen, argv[0]) : 0;
    const char *mime = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    /* Missing `type` is technically a spec TypeError (it is a required
     * argument); this engine is lenient the way its other constructors are
     * and defaults to text/html rather than throwing -- consistent with
     * "parseFromString never throws" being the header's whole point. */
    int is_html = !mime || dp_mime_is(mime, "text/html");

    struct node *root;
    if (is_html) {
        root = dom_parse(s ? s : "", s ? (int)slen : 0);
    } else {
        /* No XML parser anywhere in this tree (LibCSS needs none). Refuse
         * LOUDLY rather than silently returning an empty/wrong document --
         * spec-legal because parseFromString still never throws. */
        printf("[js_domparser] parseFromString: mimeType '%s' is not text/html -- "
               "this engine has no XML parser, returning a <parsererror> document.\n",
               mime ? mime : "?");
        struct dom_doc *ed = dom_doc_new();
        root = ed ? dom_doc_root(ed) : 0;
        if (root) {
            /* HTML-shaped on purpose, not the XML shape a real browser would
             * use (documentElement = <parsererror> directly): dom.c's
             * dom_doc_element()/dom_doc_body() specifically look for a
             * top-level <html>/<body>, so giving the refusal document that
             * same shape is what makes .documentElement/.body resolve at all
             * -- the alternative is a document that answers null for both,
             * which is a worse failure than an HTML-shaped error report. */
            struct node *html = dom_create_element(root->doc, "html", -1);
            struct node *body = html ? dom_create_element(root->doc, "body", -1) : 0;
            struct node *pe = body ? dom_create_element(root->doc, "parsererror", -1) : 0;
            if (html && body && pe) {
                dom_append_child(root, html);
                dom_append_child(html, body);
                dom_append_child(body, pe);
                static const char msg[] =
                    "DOMParser: XML parsing is not implemented in this engine; "
                    "only text/html is supported.";
                struct node *tn = dom_create_text(root->doc, msg, (int)(sizeof msg - 1));
                if (tn) dom_append_child(pe, tn);
            }
        }
    }
    if (s) JS_FreeCString(ctx, s);
    if (mime) JS_FreeCString(ctx, mime);
    if (!root) return JS_ThrowOutOfMemory(ctx);   /* dom_doc_new() itself failed -- true OOM, not a parse error */

    struct dp_arena *a = malloc(sizeof *a);
    if (!a) { dom_free(root); return JS_ThrowOutOfMemory(ctx); }
    a->doc = root->doc; a->root = root; a->refcnt = 0;
    JSValue docwrap = dp_wrap(ctx, a, root);
    if (JS_IsException(docwrap) || JS_IsNull(docwrap)) {
        /* dp_wrap never took a reference on failure -- ours to release. */
        free(a);
        dom_free(root);
    }
    return docwrap;
}

static const JSCFunctionListEntry dp_ctor_funcs[] = {
    JS_CFUNC_DEF("parseFromString", 2, dp_parseFromString),
};

/* Installed right after js_dom_init (see js_page.c's install sequence): this
 * file has no dependency on the live document js_dom_init binds, but placing
 * it there rather than earlier keeps every DOM-adjacent installer in one
 * visible block instead of scattered before it. */
void js_domparser_install(JSContext *ctx)
{
    if (!ctx) return;
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!dp_cid) JS_NewClassID(&dp_cid);
    JS_NewClass(rt, dp_cid, &dp_class);

    JSValue node_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, node_proto, dp_node_funcs, countof(dp_node_funcs));

    JSValue doc_proto = JS_NewObjectProto(ctx, node_proto);
    JS_SetPropertyFunctionList(ctx, doc_proto, dp_doc_funcs, countof(dp_doc_funcs));

    JSValue parser_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, parser_proto, dp_ctor_funcs, countof(dp_ctor_funcs));

    JSValue ctor = JS_NewCFunction2(ctx, dp_ctor, "DOMParser", 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, parser_proto);
    JS_FreeValue(ctx, parser_proto);

    /* Anchor the two shared node/document prototypes as hidden properties of
     * the constructor itself, purely so ordinary QuickJS object teardown --
     * which walks from `DOMParser` on globalThis -- frees them with
     * everything else. This is NOT decorative: `dp_cid` is one class shared
     * by both prototypes (see dp_proto_for), so neither can be registered as
     * the class's own default via JS_SetClassProto, and a JSValue that is
     * only ever a bare C static leaks -- QuickJS's refcounting has no notion
     * of "the process is ending, free everything anyway", and
     * JS_FreeRuntime asserts its object list is empty. `g_dp_node_proto`/
     * `g_dp_doc_proto` below are kept as BORROWED aliases for
     * dp_proto_for/dp_wrap (JSValueConst-only use, never dup'd or freed
     * there) -- these two property slots are the only real owners, and they
     * live exactly as long as the page runtime that installed them. */
    JS_DefinePropertyValueStr(ctx, ctor, "__dpNodeProto", node_proto, 0);
    JS_DefinePropertyValueStr(ctx, ctor, "__dpDocProto", doc_proto, 0);
    g_dp_node_proto = node_proto;
    g_dp_doc_proto = doc_proto;

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "DOMParser", ctor);
    JS_FreeValue(ctx, g);
}
