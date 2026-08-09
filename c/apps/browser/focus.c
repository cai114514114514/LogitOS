/* focus.c -- which element owns the keyboard.
 *
 * See focus.h for why this exists. The short version: the browser had no
 * concept of a focused element, so every keystroke went to <body> and no web
 * page could be typed into.
 *
 * THE ONE INVARIANT. The focused element is stored as {node, serial}, never as
 * a bare pointer. dom.c recycles node slots through a free list and stamps a
 * generation counter on every hand-out, so a script that removes the focused
 * element -- which every single-page application does, constantly -- would
 * otherwise leave this file holding a pointer into a slot that now belongs to
 * some other element. Re-checking the serial on every read turns that from a
 * wrong-element bug (silent, and it would look like "the page stole my
 * keyboard") into "focus was lost", which is also what a real browser does
 * when the focused element is removed.
 */

#include "focus.h"
#include "dom.h"
#include "css.h"

/* ------------------------------------------------------------ dispatch -- */

static fc_dispatch_fn g_dispatch;

void fc_set_dispatch(fc_dispatch_fn fn) { g_dispatch = fn; }

int fc_dispatch(struct node *target, const char *type, int bubbles, int cancelable)
{
    if (!g_dispatch || !target) return 1;
    return g_dispatch(target, type, bubbles, cancelable);
}

/* The editing dispatcher. See focus.h for why it is a second pointer: the
 * fallback below is the whole reason -- with no rich dispatcher installed the
 * editing events still fire, they simply carry no inputType. Nothing that works
 * without it breaks when it is absent. */
static fc_dispatch_input_fn g_dispatch_input;

void fc_set_dispatch_input(fc_dispatch_input_fn fn) { g_dispatch_input = fn; }

int fc_dispatch_input(struct node *target, const char *type,
                      const char *input_type, const char *data,
                      int bubbles, int cancelable)
{
    if (!target) return 1;
    if (g_dispatch_input)
        return g_dispatch_input(target, type, input_type, data, bubbles, cancelable);
    return fc_dispatch(target, type, bubbles, cancelable);
}

/* ------------------------------------------------------------- helpers -- */

static int tag_is(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

static int is_elem(const struct node *n) { return n && n->type == N_ELEM; }

/* A NUL-terminated attribute value read as a signed decimal. Returns
 * `dflt` when the attribute is absent or is not a number at all --
 * tabindex="banana" is invalid and the spec says to ignore it, which is
 * exactly "the element has no tabindex". */
static int attr_int(const struct node *n, const char *name, int dflt, int *present)
{
    const char *v = dom_attr(n, name);
    if (present) *present = 0;
    if (!v) return dflt;
    const char *p = v;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (*p < '0' || *p > '9') return dflt;
    int v2 = 0;
    while (*p >= '0' && *p <= '9') {
        if (v2 > 100000) { v2 = 100000; break; }
        v2 = v2 * 10 + (*p - '0');
        p++;
    }
    if (present) *present = 1;
    return sign * v2;
}

/* `disabled` is a content attribute AND it inherits from an ancestor
 * <fieldset disabled> -- which is how every "disable the whole form while it
 * submits" pattern is written. Missing the inherited half means a page that
 * disabled its form still accepts typing, which is worse than not having the
 * attribute at all. The <legend> exception is deliberate: the first legend of
 * a disabled fieldset stays enabled, per HTML. */
static int is_disabled(const struct node *n)
{
    if (!is_elem(n)) return 0;
    if (dom_attr(n, "disabled")) return 1;
    for (struct node *p = n->parent; p && p->type == N_ELEM; p = p->parent) {
        if (!tag_is(p->tag, "fieldset")) continue;
        if (!dom_attr(p, "disabled")) continue;
        /* Inside the fieldset's FIRST legend? Then not disabled. */
        struct node *legend = 0;
        for (struct node *c = p->first_child; c; c = c->next)
            if (c->type == N_ELEM && tag_is(c->tag, "legend")) { legend = c; break; }
        if (legend) {
            for (const struct node *q = n; q && q != p; q = q->parent)
                if (q == legend) return 0;
        }
        return 1;
    }
    return 0;
}

/* display:none (and an ancestor's) removes an element from the focus order
 * entirely; visibility:hidden does too. `hidden` on the cstyle covers
 * visibility:hidden and opacity:0 -- opacity:0 is NOT supposed to remove
 * focusability, but our cstyle folds the two together and an invisible focus
 * ring is a worse failure than an unreachable element, so this errs toward
 * skipping. Noted rather than hidden: it is a fidelity gap, not a bug. */
static int is_rendered(const struct node *n)
{
    for (const struct node *p = n; p && p->type == N_ELEM; p = p->parent) {
        const struct cstyle *st = (const struct cstyle *)p->style;
        if (st && st->display == DISP_NONE) return 0;
        if (st && st->vis_hid) return 0;
    }
    /* `hidden` content attribute; the UA sheet already maps it to display:none,
     * but a document that was never styled has no cstyle at all. */
    for (const struct node *p = n; p && p->type == N_ELEM; p = p->parent)
        if (dom_attr(p, "hidden")) return 0;
    return 1;
}

/* Does this element have focusable BEHAVIOUR, ignoring tabindex? */
static int natively_focusable(const struct node *n)
{
    const char *t = n->tag;
    if (tag_is(t, "input")) {
        const char *ty = dom_attr(n, "type");
        if (ty) {
            /* type=hidden is the one input that is never focusable. */
            const char *h = "hidden";
            int i = 0, ok = 1;
            for (; h[i]; i++) {
                char c = ty[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != h[i]) { ok = 0; break; }
            }
            if (ok && ty[i] == 0) return 0;
        }
        return 1;
    }
    if (tag_is(t, "select") || tag_is(t, "textarea") || tag_is(t, "button")) return 1;
    if (tag_is(t, "a") || tag_is(t, "area")) return dom_attr(n, "href") != 0;
    if (tag_is(t, "iframe") || tag_is(t, "summary")) return 1;
    if (tag_is(t, "audio") || tag_is(t, "video")) return dom_attr(n, "controls") != 0;
    /* contenteditable="" and ="true" are editable; ="false" is not. */
    const char *ce = dom_attr(n, "contenteditable");
    if (ce) {
        if (!ce[0]) return 1;
        if ((ce[0] == 't' || ce[0] == 'T') && (ce[1] == 'r' || ce[1] == 'R')) return 1;
    }
    return 0;
}

int focus_tab_index(struct node *n, int *explicit_out)
{
    if (explicit_out) *explicit_out = 0;
    if (!is_elem(n)) return -1;
    int present = 0;
    int ti = attr_int(n, "tabindex", 0, &present);
    if (present) { if (explicit_out) *explicit_out = 1; return ti; }
    return natively_focusable(n) ? 0 : -1;
}

int focus_is_focusable(struct node *n)
{
    if (!is_elem(n)) return 0;
    if (is_disabled(n)) return 0;
    if (!is_rendered(n)) return 0;
    int present = 0;
    (void)attr_int(n, "tabindex", 0, &present);
    /* tabindex="-1" IS focusable, just not tabbable. That distinction is the
     * whole reason focus_tab_index and focus_is_focusable are two functions. */
    if (present) return 1;
    return natively_focusable(n);
}

/* ------------------------------------------------- the focused element -- */

static struct node *g_focus;
static uint32_t     g_serial;

void focus_reset(void) { g_focus = 0; g_serial = 0; }

struct node *focus_current(void)
{
    if (!g_focus) return 0;
    /* The slot was recycled: the element we focused no longer exists. */
    if (g_focus->serial != g_serial) { g_focus = 0; g_serial = 0; return 0; }
    /* Detached from the document by a script. The DOM says focus goes back to
     * the body in that case; reporting NULL is how this file says that, and
     * the embedder resolves NULL to <body> for activeElement. */
    struct node *p = g_focus;
    while (p->parent) p = p->parent;
    if (p->type != N_DOCUMENT) { g_focus = 0; g_serial = 0; return 0; }
    return g_focus;
}

void focus_set_quiet(struct node *n)
{
    if (n && n->type != N_ELEM) n = 0;
    g_focus = n;
    g_serial = n ? n->serial : 0;
}

int focus_set(struct node *n)
{
    struct node *old = focus_current();
    if (n && n->type != N_ELEM) n = 0;
    if (n && !focus_is_focusable(n)) n = 0;
    if (old == n) return 0;

    /* The new element is recorded BEFORE the events fire, so a blur handler
     * that reads document.activeElement, or one that calls focus() on a third
     * element, composes correctly instead of being overwritten on return. */
    focus_set_quiet(n);

    if (old) {
        fc_dispatch(old, "blur", 0, 0);
        fc_dispatch(old, "focusout", 1, 0);
    }
    if (n) {
        /* A blur handler may have moved focus again; only announce the element
         * that actually ended up with it. */
        if (focus_current() == n) {
            fc_dispatch(n, "focus", 0, 0);
            fc_dispatch(n, "focusin", 1, 0);
        }
    }
    return 1;
}

/* ------------------------------------------------ sequential navigation -- */

/* Depth-first document order, elements only. */
static struct node *next_in_order(struct node *root, struct node *n)
{
    if (!n) {
        struct node *c = root ? root->first_child : 0;
        while (c && c->type != N_ELEM) c = c->next;
        if (c) return c;
        /* No element children: descend anyway (the document node's children
         * include the doctype and comments). */
        for (c = root ? root->first_child : 0; c; c = c->next) {
            struct node *d = next_in_order(c, 0);
            if (d) return d;
        }
        return 0;
    }
    if (n->first_child) {
        struct node *c = n->first_child;
        while (c) {
            if (c->type == N_ELEM) return c;
            struct node *d = next_in_order(c, 0);
            if (d) return d;
            c = c->next;
        }
    }
    for (struct node *p = n; p && p != root; p = p->parent) {
        for (struct node *s = p->next; s; s = s->next) {
            if (s->type == N_ELEM) return s;
            struct node *d = next_in_order(s, 0);
            if (d) return d;
        }
    }
    return 0;
}

/* The sequential focus navigation order, materialised.
 *
 * It is built as a LIST rather than walked lazily because the order is not the
 * document order: every element with a positive tabindex comes first, in
 * ascending index, and only then the tabindex-0 group. Walking that lazily
 * means re-scanning the document per step, and getting the tie-breaks wrong.
 * 512 is far past any real form; past it the tail simply is not reachable by
 * Tab, which is a bounded and visible failure rather than a corrupt order. */
#define FOCUS_MAXORDER 512

static int build_order(struct node *root, struct node **out, int max)
{
    int n = 0;
    /* Pass 1: positive tabindex, ascending, document order within a value.
     * Done as repeated minimum-above-last rather than a sort, because the
     * values are almost always {1,2,3} and this needs no scratch storage. */
    int last = 0;
    for (;;) {
        int best = 0;
        for (struct node *e = next_in_order(root, 0); e; e = next_in_order(root, e)) {
            int ex = 0, ti = focus_tab_index(e, &ex);
            if (ti <= last || !focus_is_focusable(e)) continue;
            if (!best || ti < best) best = ti;
        }
        if (!best) break;
        for (struct node *e = next_in_order(root, 0); e; e = next_in_order(root, e)) {
            int ex = 0, ti = focus_tab_index(e, &ex);
            if (ti != best || !focus_is_focusable(e)) continue;
            if (n < max) out[n++] = e;
        }
        last = best;
    }
    /* Pass 2: the tabindex-0 group, in document order. */
    for (struct node *e = next_in_order(root, 0); e; e = next_in_order(root, e)) {
        int ex = 0, ti = focus_tab_index(e, &ex);
        if (ti != 0 || !focus_is_focusable(e)) continue;
        if (n < max) out[n++] = e;
    }
    return n;
}

int focus_tabbable_count(struct node *root)
{
    struct node *ord[FOCUS_MAXORDER];
    return build_order(root, ord, FOCUS_MAXORDER);
}

struct node *focus_next(struct node *root, struct node *from, int back)
{
    struct node *ord[FOCUS_MAXORDER];
    int n = build_order(root, ord, FOCUS_MAXORDER);
    if (n <= 0) return 0;
    int at = -1;
    for (int i = 0; i < n; i++) if (ord[i] == from) { at = i; break; }
    if (at < 0) return back ? ord[n - 1] : ord[0];
    int nx = back ? at - 1 : at + 1;
    if (nx < 0) nx = n - 1;
    if (nx >= n) nx = 0;
    return ord[nx];
}

int focus_advance(struct node *root, int back)
{
    struct node *n = focus_next(root, focus_current(), back);
    if (!n) return 0;
    return focus_set(n);
}
