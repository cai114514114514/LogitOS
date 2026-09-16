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
#include "html_tree.h"
#include "js_dom.h"
#include "../../../include/weaksym.h"
/* Detached DOMParser users do not link the live-page DOM. Import/adopt is
 * an optional integration door, not a reason the standalone parser cannot
 * link. Leave its author-visible hook absent when no live DOM is available. */
extern struct node *js_dom_root(void) LOGIT_WEAK;
extern JSValue js_dom_wrap_node(JSContext *,struct node *) LOGIT_WEAK;
extern JSValue js_dom_throw_dom(JSContext *,const char *,const char *) LOGIT_WEAK;
LOGIT_WEAK_STUB(js_dom_root);
LOGIT_WEAK_STUB(js_dom_wrap_node);
LOGIT_WEAK_STUB(js_dom_throw_dom);
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* ---- the arena: one detached dom_doc, refcounted by its live wrappers ----
 * Moved up from its original spot (further down this file, right where the
 * ORIGINAL "static JSValue g_dp_node_proto..." comment still introduces it)
 * so the MUTATION SURFACE block below -- which needs the full definition of
 * both types, not just a name, because js_domparser_doc_of() dereferences
 * `h->arena->doc` -- can come after it instead of before it. This is a pure
 * reordering: nothing about dp_arena/dp_handle/dp_cid changed, and the
 * second copy that used to sit lower in the file is deleted, not duplicated
 * (a repeated `struct dp_arena {...}` definition is not the same type to a
 * strict reading of the standard, even with identical members, so this had
 * to be a move rather than a forward-declare-and-also-define). */
struct dp_arena {
    struct dom_doc *doc;
    struct node    *root;      /* N_DOCUMENT, what dom_free() takes */
    int             refcnt;
};

struct dp_handle { struct dp_arena *arena; struct node *n; uint32_t serial; };

static JSClassID dp_cid, dp_doc_proto_cid;

/* ============================================================================
 * MUTATION SURFACE -- added for js_frame.c (a same-origin second browsing
 * context, see js_frame.c's own header). Everything above this block is the
 * original read-only DOMParser surface; everything from here down is new.
 *
 * WHY HERE AND NOT A NEW FILE: dp_arena/dp_wrap/dp_of are all `static` to
 * this translation unit on purpose (see the file header's "what is reused"
 * section -- this is the ONLY multi-instance document representation this
 * browser has), so a mutation surface for that same representation has
 * nowhere else to live without exporting internals that were deliberately
 * kept private.
 *
 * THE SCRIPT SINK, mirroring js_dom.c's g_script_sink/offer_scripts
 * (js_dom.c:239-241,864-869) exactly on purpose: same shape, same reason.
 * js_domparser.c does not decide whether a document is a "live frame" --
 * that decision, and the second JSContext a script actually runs in, belong
 * to js_frame.c, which is the only reasonable owner of "does this detached
 * document get to execute code". This file just offers every <script> that
 * becomes reachable -- ~~inserted into a connected subtree, or given content
 * via innerHTML/textContent~~; the correction is kept beside the original
 * because the original is the sentence someone will arrive holding, and it
 * was wrong in the direction that reads as a bug report against the code
 * rather than against itself. What actually fires the sink is (a) INSERTION
 * into a connected subtree -- appendChild/insertBefore, and innerHTML=
 * because that inserts a parsed fragment -- and (b)
 * js_domparser_offer_scripts(), the once-per-document walk js_frame.c's
 * __frameAdopt makes so a script already present in the PARSED MARKUP is not
 * missed. textContent= is NOT a door and must not become one: see the comment
 * in dp_set_text for the gate that was written, watched failing, and settled
 * it. Exactly once per node EVER, enforced by dom.h's NF_SCRIPT_DONE -- the
 * same flag and the same event js_dom.c fires on.
 * An ordinary `new DOMParser().parseFromString(...)` caller that never calls
 * js_domparser_set_script_sink() gets a sink of NULL, i.e. no behaviour
 * change at all: this addition is inert until js_frame.c opts a document in. */
static void (*g_dp_script_sink)(struct node *);
void js_domparser_set_script_sink(void (*fn)(struct node *)) { g_dp_script_sink = fn; }

/* THE DOC-FREE SINK -- found NECESSARY by frame_test.c's own "never-adopted
 * document must stay inert" check, which failed against the FIRST version of
 * this file: `malloc`/`kfree` reuse addresses, so once scenario 1's arena
 * (referenced by nothing but a discarded IIFE-local variable) hit refcount 0
 * and its `struct dom_doc` was freed, js_frame.c's doc-keyed table still had
 * a LIVE-LOOKING entry for that exact pointer value -- and the NEXT
 * `dom_doc_new()` in the process (scenario 3's supposedly never-adopted
 * document) came back at the SAME address, by ordinary allocator reuse, and
 * inherited a frame context it was never given. That is a real
 * use-after-free-shaped correctness bug (a stale table keyed on identity
 * across a free), not a hypothetical one -- it reproduced on the second run
 * of this file's own three-scenario test, unprompted. This sink is the fix:
 * called with the doc about to be destroyed, immediately before dom_free()
 * runs, so whatever js_frame.c keyed on that pointer can drop the entry
 * before the pointer can be reborn as someone else's document. */
static void (*g_dp_docfree_sink)(struct dom_doc *);
void js_domparser_set_docfree_sink(void (*fn)(struct dom_doc *)) { g_dp_docfree_sink = fn; }

/* The other half of the same narrow door: js_frame.c's __frameAdopt needs the
 * `struct dom_doc *` a Document *wrapper* stands for, to key its own
 * doc->JSContext table by -- WITHOUT reaching past dp_cid/dp_h, which stay
 * static. NULL for anything that is not a live dp Document wrapper (a
 * recycled handle, an Element, a value from some other class entirely --
 * JS_GetOpaque2-style safety, checked the same way dp_of() already does). */
struct dom_doc *js_domparser_doc_of(JSValueConst v)
{
    struct dp_handle *h = JS_GetOpaque(v, dp_cid);
    if (!h || !h->n || h->n->serial != h->serial || h->n->type != N_DOCUMENT) return 0;
    return h->arena->doc;
}

/* "Connected" for a detached tree: reachable from ITS OWN document root, the
 * only root a dp arena ever has. Walking to a NULL parent lands on the
 * N_DOCUMENT node for any subtree still attached somewhere inside the arena
 * -- there is no second document to confuse it with, unlike js_dom.c's
 * connected() which has to check against one specific live g_root among
 * many possible detached trees floating in the same runtime. */
static int dp_connected(struct node *n)
{
    struct node *p = n;
    while (p->parent) p = p->parent;
    return p->type == N_DOCUMENT;
}

static int dp_tag_is(const struct node *n, const char *want)
{
    if (n->type != N_ELEM) return 0;
    return strcmp(n->tag, want) == 0;   /* tags are stored lowercase; see tagName's own comment */
}

static void dp_offer_scripts(struct node *n)
{
    /* dom_script_is_done is the SAME run-once flag js_dom.c's offer_scripts
     * consults (js_dom.c:864-870), for the same reason and deliberately not a
     * second one of this file's own -- one jar, one door. js_frame.c sets it
     * (dom_script_mark_done) the moment it has DECIDED about a node, whether
     * that decision was "run it" or a named refusal, so no node can be acted
     * on twice. Without it, `s.textContent = code` on a script already in the
     * tree runs it, and a later `s.textContent = more` runs BOTH the old and
     * the new -- the shape a real browser does not have, arrived at by
     * accident rather than chosen. */
    if (dp_tag_is(n, "script") && g_dp_script_sink && !dom_script_is_done(n))
        g_dp_script_sink(n);
    /* `next` captured BEFORE the recursion, because the sink runs JS: a script
     * that removes its own next sibling would otherwise leave this loop
     * holding a freed `c->next`. This does not make the walk fully
     * mutation-proof (nothing short of a live NodeList would) -- it removes
     * the one shape that costs nothing to remove. */
    for (struct node *c = n->first_child, *nx; c; c = nx) {
        nx = c->next;
        dp_offer_scripts(c);
    }
}

/* THE THIRD HALF OF js_frame.c's narrow door, and the reason it exists is a
 * gap the insertion-only sink above cannot see. dp_offer_scripts is called
 * from the MUTATION paths (appendChild/insertBefore/innerHTML=/textContent=),
 * which is exactly right for the specimen js_frame.c was built against --
 * a script CREATED and INSERTED after the document exists. It is blind to the
 * other, and on the open web far more common, case: a <script> that was
 * already in the frame's MARKUP when it was parsed (`<iframe srcdoc="...">`,
 * or a same-origin src whose response body contains one). Those nodes are
 * never inserted by anybody, so no sink ever sees them, and without this door
 * a same-origin frame with an inline script in its own HTML would be exactly
 * the "present and does nothing" shape js_frame.c exists to prevent.
 *
 * Called ONCE, by js_frame.c's __frameAdopt, immediately after a document is
 * given a JSContext -- never during parseFromString itself, because a plain
 * `new DOMParser().parseFromString(html)` must stay inert (that is the whole
 * premise of the sink being opt-in per document). NULL/not-a-Document is a
 * no-op, same JS_GetOpaque-checked safety js_domparser_doc_of uses. */
void js_domparser_offer_scripts(JSValueConst v)
{
    struct dp_handle *h = JS_GetOpaque(v, dp_cid);
    if (!h || !h->n || h->n->serial != h->serial || h->n->type != N_DOCUMENT) return;
    if (!g_dp_script_sink) return;
    dp_offer_scripts(h->n);
}

/* dp_arena / dp_handle / dp_cid / countof: moved to just after the includes,
 * above the MUTATION SURFACE block -- see the comment there for why. */

/* Set by js_domparser_install, valid for as long as the one page runtime is
 * (same singleton assumption js_dom.c's g_ctx/g_root make -- exactly one page
 * runtime exists at a time, and a new js_page_open always closes the old one
 * first). Re-created on every install call; the previous context's values
 * become unreachable garbage inside a runtime that install's caller has
 * already torn down, never dereferenced again. */
/* Correction to the singleton comment above: network documents now have
 * simultaneous runtimes. Borrowed process-global prototypes would attach a
 * child's object to another runtime (or to freed storage after child close).
 * Class-prototype slots are owned by the calling JSContext and survive an
 * author replacing globalThis.DOMParser. The second class is a prototype
 * anchor only; both document and node wrappers retain the existing dp_cid. */

static JSValue dp_wrap(JSContext *ctx, struct dp_arena *a, struct node *n);

static void dp_arena_unref(struct dp_arena *a)
{
    if (!a) return;
    if (--a->refcnt > 0) return;
    if (g_dp_docfree_sink) g_dp_docfree_sink(a->doc);   /* see the sink's own comment: BEFORE dom_free, not after */
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

/* Move/copy a parsed-data node across the one real document boundary.
 *
 * DOMParser owns a separate dom_doc and wrapper class, so implementing this
 * in JavaScript from tagName/textContent necessarily loses attributes. The
 * DOM already has the exact primitive we need: dom_import_node copies the
 * native node, attributes, namespace and descendants into the live page's
 * arena. The returned value is consequently a normal live Element wrapper,
 * not a DOMParser facade that happens to look similar.
 *
 * `adopt` has one unavoidable compatibility deviation: a QuickJS object's
 * native class cannot be changed from dp_cid to js_dom.c's element class, so
 * cross-arena adoption returns the imported live wrapper and detaches the old
 * parsed wrapper instead of preserving JavaScript object identity. Tree,
 * attributes, ownerDocument and detach semantics are real; callers that use
 * the returned value get the platform operation rather than a hard stop. */
static JSValue dp_transfer_live(JSContext *ctx, JSValueConst t, int argc,
                                JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_ThrowTypeError(ctx, "DOMParser node required");
    struct node *src = dp_of(argv[0]);
    struct node *live = js_dom_root();
    if (!src || !live || src->type == N_DOCUMENT || src->type == N_DOCTYPE)
        return js_dom_throw_dom(ctx, "NotSupportedError",
                                "this DOMParser node cannot be transferred");

    int deep = argc > 1 && JS_ToBool(ctx, argv[1]);
    int adopt = argc > 2 && JS_ToBool(ctx, argv[2]);
    struct node *copy = dom_import_node(live->doc, src);
    if (!copy) return JS_ThrowOutOfMemory(ctx);
    if (!deep) dom_destroy_children(copy);
    if (adopt && src->parent) dom_remove_child(src->parent, src);
    return js_dom_wrap_node(ctx, copy);
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
    /* A Comment's own textContent is its data, although comments must be
     * excluded when gathering an Element's descendant text. The old shared
     * gather path returned an empty string even for the Comment itself. */
    if (n->type == N_TEXT || n->type == N_COMMENT)
        return JS_NewStringLen(ctx, n->text ? n->text : "", (size_t)n->textlen);
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
    /* NO SCRIPT OFFER HERE, AND THE ATTEMPT TO ADD ONE IS WHY THIS COMMENT
     * EXISTS. This file's header used to say the sink fires for a script
     * "inserted into a connected subtree, or given content via
     * innerHTML/textContent" -- innerHTML was wired, textContent was not, so
     * the sentence was true of the intent and false of the code. The obvious
     * fix (offer here too) was written, gated, and WATCHED FAILING, which is
     * what settled it: appending a still-empty <script> already runs the
     * "prepare a script" step, so the node is marked NF_SCRIPT_DONE before any
     * text exists, and a later `t.textContent = code` must then do nothing.
     * That is not a limitation of this file -- it is what every real browser
     * does, because `already started` is set when a script is prepared, not
     * when it has content, and js_dom.c's own offer_scripts is insertion-only
     * for the identical reason. So the door stays shut and the header sentence
     * was corrected instead. The shape that DOES work, and the one real
     * loaders use, is detached-first: create, set textContent (or innerHTML),
     * THEN insert -- one offer, at the insert, with the text already there.
     * tests/unit/frame_test.c scenario 5 pins both halves. */
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

/* ---- mutation: createElement / createTextNode / createComment ----
 * document-only (like getElementById below): a node needs a `struct dom_doc`
 * arena to be created in, and only the Document wrapper's dp_handle carries
 * one (`h->arena->doc`). Real DOM has Document.createElement; a Node does
 * not, so this matches the spec shape as a side effect of matching the
 * implementation's, not by design intent. */
static JSValue dp_doc_createElement(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    if (!h || argc < 1) return JS_NULL;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_NULL;
    struct node *n = dom_create_element(h->arena->doc, tag, -1);
    JS_FreeCString(ctx, tag);
    return dp_wrap(ctx, h->arena, n);
}
static JSValue dp_doc_createTextNode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    if (!h || argc < 1) return JS_NULL;
    size_t sl = 0;
    const char *s = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!s) return JS_NULL;
    struct node *n = dom_create_text(h->arena->doc, s, (int)sl);
    JS_FreeCString(ctx, s);
    return dp_wrap(ctx, h->arena, n);
}
static JSValue dp_doc_createComment(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    if (!h || argc < 1) return JS_NULL;
    size_t sl = 0;
    const char *s = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!s) return JS_NULL;
    struct node *n = dom_create_comment(h->arena->doc, s, (int)sl);
    JS_FreeCString(ctx, s);
    return dp_wrap(ctx, h->arena, n);
}

/* ---- mutation: append/insert/remove, on Node (not Document-only) ----
 * NO DocumentFragment: real appendChild(frag) moves the fragment's children
 * rather than the fragment itself, which needs the same is_fragment()
 * special-casing js_dom.c carries (js_dom.c:779-854) and this mutation
 * surface does not reimplement -- appendChild(x) here always inserts `x`
 * itself. Nothing in the specimen this was built for needs fragments; a
 * script that passes one gets a real, if spec-incomplete, node inserted
 * rather than a silent no-op. */
static JSValue dp_appendChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *p = dp_of(t);
    if (!p || argc < 1) return JS_ThrowTypeError(ctx, "appendChild requires a node");
    struct node *c = dp_of(argv[0]);
    if (!c) return JS_ThrowTypeError(ctx, "appendChild: not a node from this document");
    dom_append_child(p, c);
    if (dp_connected(p)) dp_offer_scripts(c);
    return JS_DupValue(ctx, argv[0]);
}
static JSValue dp_insertBefore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *p = dp_of(t);
    if (!p || argc < 1) return JS_ThrowTypeError(ctx, "insertBefore requires a node");
    struct node *c = dp_of(argv[0]);
    if (!c) return JS_ThrowTypeError(ctx, "insertBefore: not a node from this document");
    struct node *ref = argc > 1 ? dp_of(argv[1]) : 0;
    dom_insert_before(p, c, ref);
    if (dp_connected(p)) dp_offer_scripts(c);
    return JS_DupValue(ctx, argv[0]);
}
static JSValue dp_removeChild(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *p = dp_of(t);
    if (!p || argc < 1) return JS_ThrowTypeError(ctx, "removeChild requires a node");
    struct node *c = dp_of(argv[0]);
    if (!c) return JS_ThrowTypeError(ctx, "removeChild: not a node from this document");
    dom_remove_child(p, c);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue dp_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = dp_of(t);
    if (!n || n->type == N_DOCUMENT) return JS_ThrowTypeError(ctx, "remove requires a ChildNode");
    /* This wrapper used to expose removeChild only. Querying a parsed document
     * then stripping script/style/header links with node.remove() therefore
     * threw before search summaries could read textContent. Reuse the same
     * detach operation as removeChild, not dom_destroy_subtree: the caller can
     * retain/reinsert this node, and its wrapper's arena reference must keep
     * it alive even after the parsed Document wrapper is garbage-collected. */
    if (n->parent) dom_remove_child(n->parent, n);
    return JS_UNDEFINED;
}

static JSValue dp_setAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = dp_of(t);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    size_t vlen = 0;
    const char *val = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!val) { JS_FreeCString(ctx, name); return JS_UNDEFINED; }
    dom_set_attr_raw(n, name, (int)strlen(name), val, (int)vlen);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}
static JSValue dp_removeAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = dp_of(t);
    if (!n || argc < 1) return JS_UNDEFINED;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_UNDEFINED;
    dom_set_attr_raw(n, name, (int)strlen(name), 0, 0);   /* dom.c: an empty/NULL value clears */
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* innerHTML SETTER only -- no getter. Same fragment-parse-then-import idiom
 * as js_dom.c's el_set_html (js_dom.c:573-597), copied rather than shared
 * because js_dom.c's version is `static` and hardwired to nothing this file
 * can reach anyway. No getter: nothing in the surface this was built for
 * (js_frame.c's script sink) ever reads innerHTML back, and a serialiser
 * good enough to be honest is a separate piece of work (dom_serialize.c is
 * built for the top-level page, not audited here for reuse against a
 * detached arena). Reading innerHTML on a dp node is `undefined`, not a
 * silently wrong string. */
static JSValue dp_set_innerHTML(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = dp_of(t);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_UNDEFINED;
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
    }
    if (fdoc) dom_free(dom_doc_root(fdoc));
    if (dp_connected(n)) dp_offer_scripts(n);
    return JS_UNDEFINED;
}

/* getElementsByTagName: a snapshot array, same "not live" deviation as
 * childNodes/children above (see the file header). '*' matches every
 * element, same as the real DOM. Lives on Node (not Document-only) so it
 * works identically whether called on the Document or on an Element --
 * exactly what dp_querySelector already does, and for the same reason. */
static void dp_walk_tagname(JSContext *ctx, struct dp_arena *a, struct node *n,
                            const char *tag, int all, JSValue arr, uint32_t *idx)
{
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type == N_ELEM && (all || dp_tag_is(c, tag)))
            JS_DefinePropertyValueUint32(ctx, arr, (*idx)++, dp_wrap(ctx, a, c), JS_PROP_C_W_E);
        dp_walk_tagname(ctx, a, c, tag, all, arr, idx);
    }
}
static JSValue dp_getElementsByTagName(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct dp_handle *h = dp_h(t);
    struct node *n = dp_of(t);
    JSValue arr = JS_NewArray(ctx);
    if (!n || !h || argc < 1 || JS_IsException(arr)) return arr;
    const char *tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return arr;
    int all = strcmp(tag, "*") == 0;
    uint32_t idx = 0;
    dp_walk_tagname(ctx, h->arena, n, tag, all, arr, &idx);
    JS_FreeCString(ctx, tag);
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

/* Parsed documents have no navigation request, hence HTML's default empty
 * referrer. This wrapper family has its own native receiver check; borrowing
 * the live document getter would misidentify its independent arena. */
static JSValue dp_doc_get_referrer(JSContext *ctx, JSValueConst t)
{
    struct node *n = dp_of(t);
    if (!n || n->type != N_DOCUMENT)
        return JS_ThrowTypeError(ctx, "Document.referrer requires a Document");
    return JS_NewString(ctx, "");
}

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
    JS_CFUNC_DEF("getElementsByTagName", 1, dp_getElementsByTagName),
    JS_CFUNC_DEF("appendChild", 1, dp_appendChild),
    JS_CFUNC_DEF("insertBefore", 2, dp_insertBefore),
    JS_CFUNC_DEF("removeChild", 1, dp_removeChild),
    JS_CFUNC_DEF("setAttribute", 2, dp_setAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, dp_removeAttribute),
    JS_CGETSET_DEF("innerHTML", NULL, dp_set_innerHTML),
};

/* A parsed data document has no keyboard browsing context. Same-origin
 * iframe adoption supplies its own owner-chain query in js_platform.c. */
static JSValue dp_doc_hasFocus(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = dp_of(t);
    if (!n || n->type != N_DOCUMENT)
        return JS_ThrowTypeError(ctx, "Document.hasFocus requires a Document");
    return JS_FALSE;
}

static const JSCFunctionListEntry dp_doc_funcs[] = {
#ifndef DOCUMENT_NO_HAS_FOCUS
    JS_CFUNC_DEF("hasFocus", 0, dp_doc_hasFocus),
#endif
    JS_CFUNC_DEF("getElementById", 1, dp_doc_getById),
    JS_CFUNC_DEF("createElement", 1, dp_doc_createElement),
    JS_CFUNC_DEF("createTextNode", 1, dp_doc_createTextNode),
    JS_CFUNC_DEF("createComment", 1, dp_doc_createComment),
    JS_CGETSET_DEF("body", dp_doc_get_body, NULL),
    JS_CGETSET_DEF("documentElement", dp_doc_get_docel, NULL),
#ifndef DOCUMENT_NO_REFERRER
    JS_CGETSET_DEF("referrer", dp_doc_get_referrer, NULL),
#endif
};

static JSValue dp_proto_for(JSContext *ctx,const struct node *n)
{ return JS_GetClassProto(ctx,n->type == N_DOCUMENT ? dp_doc_proto_cid : dp_cid); }

/* One wrapper per node, cached in node->jsw exactly like js_dom.c's wrap() --
 * the PUBLIC half of that mechanism (dom.h's dom_set_wrapper), so
 * `doc.body === doc.body` and a script can hang expandos off a parsed
 * element. */
static JSValue dp_wrap(JSContext *ctx, struct dp_arena *a, struct node *n)
{
    if (!n) return JS_NULL;
    if (n->jsw) return JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, n->jsw));
    JSValue proto=dp_proto_for(ctx,n);
    JSValue o = JS_NewObjectProtoClass(ctx, proto, dp_cid);
    JS_FreeValue(ctx,proto);
    if (JS_IsException(o)) return o;
    struct dp_handle *h = malloc(sizeof *h);
    if (!h) { JS_FreeValue(ctx, o); return JS_NULL; }
    h->arena = a; h->n = n; h->serial = n->serial;
    a->refcnt++;
    JS_SetOpaque(o, h);
    dom_set_wrapper(n, JS_VALUE_GET_PTR(o));
#ifndef DOMPARSER_NO_REMOVE
    /* The small parser wrapper has one shared Node prototype, also inherited
     * by Document. ChildNode's method belongs to Element, CharacterData and
     * DocumentType only, so install it on those wrappers instead of leaking it
     * onto Document. This is a real mutation, not a summary-specific filter. */
    if (n->type == N_ELEM || n->type == N_TEXT || n->type == N_COMMENT || n->type == N_DOCTYPE)
        JS_DefinePropertyValueStr(ctx, o, "remove",
            JS_NewCFunction(ctx, dp_remove, "remove", 0), JS_PROP_C_W_E);
#endif
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
    if (!dp_doc_proto_cid) JS_NewClassID(&dp_doc_proto_cid);
    JS_NewClass(rt, dp_cid, &dp_class);
    JS_NewClass(rt, dp_doc_proto_cid, &dp_class);

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
    JS_SetClassProto(ctx,dp_cid,JS_DupValue(ctx,node_proto));
    JS_SetClassProto(ctx,dp_doc_proto_cid,JS_DupValue(ctx,doc_proto));

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "DOMParser", ctor);
    if(LOGIT_HAVE(js_dom_root)&&LOGIT_HAVE(js_dom_wrap_node)&&LOGIT_HAVE(js_dom_throw_dom))
      JS_DefinePropertyValueStr(ctx, g, "__domParserTransfer",
        JS_NewCFunction(ctx, dp_transfer_live, "__domParserTransfer", 3), 0);
    JS_FreeValue(ctx, g);
}
