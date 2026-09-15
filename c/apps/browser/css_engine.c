/* M17 L2: LibCSS-backed CSS engine. Implements css.h (css_init/css_apply) on top
 * of NetSurf LibCSS, producing the same `struct cstyle` that net/layout.c reads,
 * so it is a drop-in replacement for net/css.c. */
#include "dom.h"
#include "css.h"
#include "layout_text.h"   /* the LTX_* vocabulary the text fields carry */
#include "forms.h"         /* FC_PAD_*: UA button defaults and intrinsic metrics agree */
#include "css_interp.h"    /* struct ci_xform -- see sup_beside_libcss() */
#include "css_report.h"    /* the ONE accounting of the stylesheet pipeline */
#include "../../../include/weaksym.h"   /* LOGIT_WEAK/_STUB/LOGIT_HAVE */

/* ...and the one function taken from it is WEAK, for the reason the block at
 * js_dom_dirty() below gives about that one. Re-measured 2026-08-20 with the
 * Makefile continuations JOINED (this line read "thirteen", and CLAUDE.md
 * records four in-tree tools that got that wrong the same way): FIFTY-ONE
 * source lists name css_engine.c and do NOT name css_interp.c -- 24 in the
 * Makefile and 27 across nineteen fragments under tests/. A hard reference
 * would turn a CSS.supports improvement into fifty-one link failures in other
 * lines' files. Weak and guarded, so a build without the transform parser answers
 * exactly what it answered before. Spelled through include/weaksym.h rather
 * than __attribute__((__weak__)) directly: an UNDEFINED weak reference is an
 * ELF property, and on the Mach-O dev host it was a hard link error -- which
 * is what test-layout-box died of, on this symbol, in a file nobody had
 * edited. The lowercase `weak` is still wrong here for the older reason: it is
 * a macro in the force-included features.h. */
extern int ci_transform_parse(const char *s, int len, double fs_px,
                              double root_px, struct ci_xform *out) LOGIT_WEAK;
LOGIT_WEAK_STUB(ci_transform_parse);
extern int ci_transform_parse_context(const char *,int,
    const struct ci_length_context *,struct ci_xform *) LOGIT_WEAK;
LOGIT_WEAK_STUB(ci_transform_parse_context);
extern int css_facing_keyword(const char *,int,int) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_facing_keyword);
extern int css_box_supports_decl(const char *,int,const char *,int) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_box_supports_decl);
/* The production SVG codec is also the shared literal colour evaluator. Host
 * CSS-only harnesses intentionally omit the image pipeline; they have no SVG
 * pixels to resolve. Keep that source-list boundary explicit. */
extern int img_css_color(const char *, int, unsigned char[4]) LOGIT_WEAK;
LOGIT_WEAK_STUB(img_css_color);
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <libcss/libcss.h>
#include <libcss/canon.h>
/* css__computed_style_ref lives in LibCSS's internal header; CSS_INC already
 * puts libcss/src on the include path (css_engine.c is LibCSS's adapter, not a
 * client of its public API alone). Needed to keep a reference to each node's
 * effective computed style -- see style_node. */
#include "select/computed.h"
/* propget.h is needed for ONE thing: css_computed_min_width/min_height report
 * `auto` as SET-with-value-0 unless the element is ITSELF a flex container
 * (select/computed.c) -- but min-*:auto is the initial value for a flex ITEM,
 * whose own display is usually block. Going through the public accessor would
 * make every flex item look like it had an authored `min-width:0`, which is
 * precisely the declaration pages use to switch the automatic minimum OFF. The
 * raw getters keep the two distinguishable. */
/* Correction (2026-09-09): the positioned-inset bridge also reads raw
 * composed offsets here, because relative opposite-edge synthesis loses units
 * in LibCSS's public getters. The reason for the original min-size use above
 * is unchanged. */
#include "select/propget.h"
#include "select/propset.h"
#include "select/arena.h"
#include "stylesheet.h"

/* cstyle is owned by the DOM node and freed via kfree in dom_free. */
void *kmalloc(unsigned long);
void  kfree(void *);

/* ---------- small helpers ---------- */
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Compare our node's (lowercased) tag to an lwc_string, case-insensitively. */
static int tag_is_ci(struct node *n, lwc_string *name)
{
    const char *a = n->tag;
    const char *b = lwc_string_data(name);
    size_t bl = lwc_string_length(name), al = strlen(a), i;
    if (al != bl) return 0;
    for (i = 0; i < al; i++) if (lc((unsigned char)a[i]) != lc((unsigned char)b[i])) return 0;
    return 1;
}

/* previous element sibling: the DOM is doubly linked now, so this is a short
 * hop backwards instead of a scan from the parent's first child (the old
 * version made every :first-child / A+B test O(siblings)). */
static struct node *prev_elem_sibling(struct node *n)
{
    struct node *p = n->prev;
    while (p && p->type != N_ELEM) p = p->prev;
    return p;
}

/* whitespace, for the space-separated attribute selectors ([rel~=me]) */
static int sp(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

/* ---------- select handler (drives LibCSS off our struct node) ---------- */
/* Ref contract: LibCSS unrefs the name, the id and every class it is handed
 * (css_select__finalise_selection_state), so each handler below returns a +1
 * reference. The arrays themselves stay ours -- LibCSS never frees those. */
static css_error h_node_name(void *pw, void *node, css_qname *qname)
{
    (void)pw; struct node *n = node;
    qname->ns = NULL;
    /* The name is already interned by the DOM: no per-call intern (hash of the
     * tag bytes) any more, just a refcount bump. */
    qname->name = n->name ? lwc_string_ref(n->name) : NULL;
    return CSS_OK;
}

static css_error h_node_classes(void *pw, void *node,
        lwc_string ***classes, uint32_t *n_classes)
{
    (void)pw; struct node *n = node;
    *classes = NULL; *n_classes = 0;
    if (n->type != N_ELEM || n->nclass <= 0) return CSS_OK;
    /* Hand back the node's own token array. The old code re-tokenised the class
     * attribute into a single static[32] on every call, which also silently
     * broke style sharing: the candidate node's classes overwrote the current
     * node's in the same buffer, so the comparison always matched. */
    for (int i = 0; i < n->nclass; i++) lwc_string_ref(n->classes[i]);
    *classes = n->classes;
    *n_classes = (uint32_t)n->nclass;
    return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
    (void)pw; struct node *n = node;
    *id = n->id ? lwc_string_ref(n->id) : NULL;
    return CSS_OK;
}

static css_error h_named_ancestor_node(void *pw, void *node,
        const css_qname *qname, void **ancestor)
{
    (void)pw; struct node *n = ((struct node *)node)->parent;
    *ancestor = NULL;
    for (; n; n = n->parent)
        if (n->type == N_ELEM && tag_is_ci(n, qname->name)) { *ancestor = n; break; }
    return CSS_OK;
}

static css_error h_named_parent_node(void *pw, void *node,
        const css_qname *qname, void **parent)
{
    (void)pw; struct node *p = ((struct node *)node)->parent;
    *parent = (p && p->type == N_ELEM && tag_is_ci(p, qname->name)) ? p : NULL;
    return CSS_OK;
}

static css_error h_named_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling)
{
    (void)pw; struct node *p = prev_elem_sibling(node);
    *sibling = (p && tag_is_ci(p, qname->name)) ? p : NULL;
    return CSS_OK;
}

static css_error h_named_generic_sibling_node(void *pw, void *node,
        const css_qname *qname, void **sibling)
{
    (void)pw; struct node *n = node, *found = 0;
    for (struct node *c = n->prev; c && !found; c = c->prev)
        if (c->type == N_ELEM && tag_is_ci(c, qname->name)) found = c;
    *sibling = found;
    return CSS_OK;
}

static css_error h_parent_node(void *pw, void *node, void **parent)
{
    (void)pw; struct node *p = ((struct node *)node)->parent;
    /* The #document node is not a CSS parent -> report root as NULL, so LibCSS
     * treats <html> as the root element and resolves absolute sizes. */
    *parent = (p && p->type == N_ELEM) ? p : NULL;
    return CSS_OK;
}

static css_error h_sibling_node(void *pw, void *node, void **sibling)
{
    (void)pw; *sibling = prev_elem_sibling(node);
    return CSS_OK;
}

static css_error h_node_has_name(void *pw, void *node,
        const css_qname *qname, bool *match)
{
    (void)pw;
    /* universal selector "*" matches anything */
    const char *b = lwc_string_data(qname->name);
    *match = (lwc_string_length(qname->name) == 1 && b[0] == '*')
             ? true : (tag_is_ci(node, qname->name) != 0);
    return CSS_OK;
}

static css_error h_node_has_class(void *pw, void *node,
        lwc_string *name, bool *match)
{
    (void)pw; struct node *n = node;
    /* Both sides are interned, so class matching is a pointer compare per
     * token -- no re-tokenising of the class attribute per selector. */
    *match = false;
    for (int i = 0; i < n->nclass; i++)
        if (n->classes[i] == name) { *match = true; break; }
    return CSS_OK;
}

static css_error h_node_has_id(void *pw, void *node, lwc_string *name, bool *match)
{
    (void)pw; struct node *n = node;
    *match = (n->id != NULL && n->id == name);
    return CSS_OK;
}

/* Attribute lookups go straight through the selector's interned name. Besides
 * being a pointer compare, this removes qattr()'s silent 63-character
 * attribute-name truncation (a selector on a longer data-* name used to match
 * every attribute sharing its first 63 characters). */
static css_error h_node_has_attribute(void *pw, void *node,
        const css_qname *qname, bool *match)
{
    (void)pw;
    *match = dom_has_attr_lw(node, qname->name) != 0;
    return CSS_OK;
}

static css_error h_node_has_attribute_equal(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    *match = v && strlen(v) == lwc_string_length(value) &&
             memcmp(v, lwc_string_data(value), lwc_string_length(value)) == 0;
    return CSS_OK;
}

static int substr(const char *h, const char *n, size_t nl)
{
    if (!nl) return 0;
    for (; *h; h++) if (strncmp(h, n, nl) == 0) return 1;
    return 0;
}

static css_error h_node_has_attribute_dashmatch(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = false;
    if (v && wl) { size_t vl = strlen(v);
        if (vl == wl && memcmp(v, w, wl) == 0) *match = true;
        else if (vl > wl && memcmp(v, w, wl) == 0 && v[wl] == '-') *match = true; }
    return CSS_OK;
}

static css_error h_node_has_attribute_includes(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = false;
    if (!v || !wl) return CSS_OK;
    const char *p = v;
    while (*p) { while (*p && sp(*p)) p++; const char *s = p; while (*p && !sp(*p)) p++;
        if ((size_t)(p - s) == wl && memcmp(s, w, wl) == 0) { *match = true; break; } }
    return CSS_OK;
}

static css_error h_node_has_attribute_prefix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = v && wl && strlen(v) >= wl && memcmp(v, w, wl) == 0;
    return CSS_OK;
}

static css_error h_node_has_attribute_suffix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = false;
    if (v && wl) { size_t vl = strlen(v); if (vl >= wl && memcmp(v + vl - wl, w, wl) == 0) *match = true; }
    return CSS_OK;
}

static css_error h_node_has_attribute_substring(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw;
    const char *v = dom_attr_lw(node, qname->name);
    *match = v && substr(v, lwc_string_data(value), lwc_string_length(value));
    return CSS_OK;
}

static css_error h_node_is_root(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = !n->parent || n->parent->type == N_DOCUMENT;
    return CSS_OK;
}

static css_error h_node_count_siblings(void *pw, void *node,
        bool same_name, bool after, int32_t *count)
{
    (void)pw; (void)after; struct node *n = node; int cnt = 0;
    /* same_name (:nth-of-type) counts only same-tag siblings -- an interned
     * pointer compare now. (The old tag_is_ci(c, NULL) here passed a NULL
     * lwc_string -> NULL deref crash.) */
    if (n)
        for (struct node *c = n->prev; c; c = c->prev)
            if (c->type == N_ELEM && (!same_name || c->name == n->name))
                cnt++;
    *count = cnt;
    return CSS_OK;
}

static css_error h_node_is_empty(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = (n->first_child == NULL);
    return CSS_OK;
}

static css_error h_node_is_link(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = (n->tag_id == TAG_A && dom_has_attr_lw(n, dom_atoms.a_href));
    return CSS_OK;
}

static css_error h_false(void *pw, void *node, bool *match)
{ (void)pw; (void)node; *match = false; return CSS_OK; }

/* Interaction changes do not mutate DOM attributes. Keep a separate revision
 * for CSSOM flushes and for the embedder's frame invalidation: consuming one
 * must not silently consume the other. The first implementation left these
 * selectors false (reason preserved below); browser.c now feeds pointer state
 * even when a page has no JavaScript listeners and settles a full cascade.
 * Full-document invalidation is intentional here: ancestor :hover and following
 * sibling selectors can change nodes outside the hit element's subtree.
 * :visited remains refused. :focus-visible/:focus-within need parser support
 * and are not claimed by this three-state foundation. */
#include "focus.h"
extern struct node *focus_current(void) LOGIT_WEAK;
LOGIT_WEAK_STUB(focus_current);
struct css_ui_handle { struct node *node; uint32_t serial; };
static struct css_ui_handle ui_hover, ui_active, ui_focus;
static unsigned long ui_revision = 1, ui_rendered;
static int g_css_passive;
static struct node *ui_live(struct css_ui_handle *h)
{
    struct node *n = h->node;
    if (!n || n->serial != h->serial) return NULL;
    struct node *r = n;
    while (r->parent) r = r->parent;
    return r->type == N_DOCUMENT ? n : NULL;
}
static void ui_assign(struct css_ui_handle *h, struct node *n)
{
    while (n && n->type != N_ELEM) n = n->parent;
    if (h->node == n && h->serial == (n ? n->serial : 0)) return;
    h->node = n; h->serial = n ? n->serial : 0; ui_revision++;
}
static void ui_sync(void)
{
    ui_assign(&ui_hover, ui_live(&ui_hover));
    ui_assign(&ui_active, ui_live(&ui_active));
    ui_assign(&ui_focus, !g_css_passive && LOGIT_HAVE(focus_current) ? focus_current() : NULL);
}
void css_interaction_hover(struct node *n) { ui_assign(&ui_hover, n); }
void css_interaction_active(struct node *n) { ui_assign(&ui_active, n); }
void css_interaction_reset(void)
{
    /* Called before DOM destruction; serial validation cannot make a freed
     * document arena safe to dereference. Reset does not inspect old nodes. */
    memset(&ui_hover, 0, sizeof ui_hover);
    memset(&ui_active, 0, sizeof ui_active);
    memset(&ui_focus, 0, sizeof ui_focus);
    ui_revision++; ui_rendered = ui_revision;
}
int css_interaction_take_change(void)
{
    ui_sync();
    int changed = ui_rendered != ui_revision;
    ui_rendered = ui_revision;
    return changed;
}
static int ui_contains(struct css_ui_handle *h, struct node *n)
{
    for (struct node *p = ui_live(h); p; p = p->parent)
        if (p == n) return 1;
    return 0;
}
static css_error h_ui_hover(void *pw, void *node, bool *match)
{
    (void)pw;
#ifdef CSS_NEGCTL_STATIC_INTERACTION
    (void)node; *match = false;
#else
    *match = ui_contains(&ui_hover, node);
#endif
    return CSS_OK;
}
static css_error h_ui_active(void *pw, void *node, bool *match)
{
    (void)pw;
#ifdef CSS_NEGCTL_STATIC_INTERACTION
    (void)node; *match = false;
#else
    *match = ui_contains(&ui_active, node);
#endif
    return CSS_OK;
}
static css_error h_ui_focus(void *pw, void *node, bool *match)
{
    (void)pw;
#ifdef CSS_NEGCTL_STATIC_INTERACTION
    (void)node; *match = false;
#else
    *match = LOGIT_HAVE(focus_current) && focus_current() == node;
#endif
    return CSS_OK;
}

/* ---------- the four STATIC pseudo-classes -------------------------------
 *
 * :checked, :disabled, :enabled and :target answered h_false unconditionally,
 * which meant every rule behind them was selected away -- 578 uses across the
 * 15-site corpus (:checked 252, :disabled 230, :target 93, :enabled 3).
 *
 * They are grouped here, and :hover/:active/:focus/:visited are deliberately
 * left on h_false, because the four below are a DIFFERENT KIND of question.
 * These are functions of the document as it stands: an attribute, a form
 * control's stored state, the URL's fragment. The other four are functions of
 * a transient UI state that CHANGES WITHOUT THE DOM CHANGING, so answering
 * them truthfully once is not enough -- the cascade has to be re-run at the
 * moment the state flips, and there is no invalidation path for that here.
 * Answering :hover truthfully with no re-style would be strictly worse than
 * answering it false: the page would style whatever happened to be under the
 * pointer during the ONE pass that ran and then keep those styles forever.
 * (:visited is not a machinery question at all -- every browser refuses it on
 * purpose, because answering it leaks the user's history to getComputedStyle.)
 *
 * forms.c owns checked/disabled and is WEAK here for the reason the block at
 * ci_transform_parse gives: css_engine.c appears in ~51 host source lists that
 * do not name forms.c, and a hard reference would turn this into fifty-one
 * link failures in other lines' files. $(BROWSER_PIPE) names forms.c, so the
 * shipped browser gets the live answers; a host gate without it degrades to
 * the CONTENT ATTRIBUTE, which is the right floor -- `<input checked>` in the
 * source still matches :checked, only a box the user or a script toggled since
 * parse does not. Degrading to "no" would have been the other option and is
 * worse: it is the h_false we are removing.
 *
 * fc_kind() is a static inline in forms.h and costs no link edge at all, so
 * the "is this even a form control" half of :enabled/:disabled is always
 * exact. */
#include "forms.h"
extern int fc_checked(struct node *n)  LOGIT_WEAK;
extern int fc_disabled(struct node *n) LOGIT_WEAK;
LOGIT_WEAK_STUB(fc_checked);
LOGIT_WEAK_STUB(fc_disabled);

/* :checked -- a checked checkbox/radio, or a selected <option>.
 *
 * <option selected> is in the spec's definition and is NOT reachable through
 * fc_checked (that function is about input toggles), so it is answered here
 * from the attribute. Selecting a different option through the DOM is a
 * dirtiness fc_selected_index() knows about; matching the attribute is this
 * pass's floor for the same reason as above. */
static css_error h_node_is_checked(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = false;
    if (!n || n->type != N_ELEM) return CSS_OK;
    if (n->tag_id == TAG_OPTION) {
        *match = dom_attr(n, "selected") != NULL;
        return CSS_OK;
    }
    if (!FC_IS_TOGGLE(fc_kind(n))) return CSS_OK;
    *match = LOGIT_HAVE(fc_checked) ? (fc_checked(n) != 0)
                                    : (dom_attr(n, "checked") != NULL);
    return CSS_OK;
}

/* Attribute-only :disabled, used when forms.c is not in the link. Mirrors
 * fc_disabled()'s two rules (own attribute, or an ancestor <fieldset
 * disabled>) rather than only the first -- a fieldset-disabled control that
 * reported :enabled would be a NEW wrong answer, and a wrong answer is worse
 * than the missing one this replaces. */
static int css_attr_disabled(struct node *n)
{
    if (dom_attr(n, "disabled")) return 1;
    for (struct node *p = n->parent; p; p = p->parent)
        if (p->type == N_ELEM && p->tag_id == TAG_FIELDSET &&
            dom_attr(p, "disabled")) return 1;
    return 0;
}

/* :disabled / :enabled are NOT each other's negation over all elements: both
 * are defined only on elements that CAN be disabled, so a <div> is neither.
 * Writing :enabled as "not disabled" would match every div, span and text
 * wrapper on the page -- present-and-wrong, and invisible to a drop counter
 * because nothing was dropped. fc_kind() decides candidacy; the two then
 * disagree only on the boolean. */
static css_error h_node_is_disabled(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = false;
    if (!n || n->type != N_ELEM || fc_kind(n) == FC_NONE) return CSS_OK;
    *match = LOGIT_HAVE(fc_disabled) ? (fc_disabled(n) != 0)
                                     : (css_attr_disabled(n) != 0);
    return CSS_OK;
}

static css_error h_node_is_enabled(void *pw, void *node, bool *match)
{
    bool dis = false;
    css_error e = h_node_is_disabled(pw, node, &dis);
    struct node *n = node;
    *match = (e == CSS_OK && !dis && n && n->type == N_ELEM &&
              fc_kind(n) != FC_NONE);
    return e;
}

/* :target -- the element the URL's fragment names.
 *
 * ONE spelling of the fragment, set through css_set_target_fragment() by
 * whoever owns the URL, rather than css_engine.c re-deriving it from a
 * document URL it would have to be handed anyway. An empty fragment matches
 * nothing at all, which is the spec's answer and also the only safe default:
 * a NULL here has to mean "no target", never "match everything".
 *
 * The id comparison is the interned pointer's BYTES, not the pointer: the
 * fragment arrives as text from a URL and was never interned. */
static char g_target_frag[128];
static int  g_target_fraglen;

void css_set_target_fragment(const char *frag, int len)
{
    if (!frag) { g_target_fraglen = 0; g_target_frag[0] = 0; return; }
    if (len < 0) len = (int)strlen(frag);
    if (len > (int)sizeof g_target_frag - 1) len = (int)sizeof g_target_frag - 1;
    memcpy(g_target_frag, frag, (size_t)len);
    g_target_frag[len] = 0;
    g_target_fraglen = len;
}

const char *css_target_fragment(int *len)
{
    if (len) *len = g_target_fraglen;
    return g_target_fraglen ? g_target_frag : NULL;
}

static css_error h_node_is_target(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = false;
    if (!n || n->type != N_ELEM || g_target_fraglen == 0) return CSS_OK;
    if (n->id &&
        (int)lwc_string_length(n->id) == g_target_fraglen &&
        memcmp(lwc_string_data(n->id), g_target_frag,
               (size_t)g_target_fraglen) == 0) {
        *match = true;
        return CSS_OK;
    }
    /* HTML's "find a potential indicated element" falls back to <a name=...>
     * when no id matches. Only <a>: `name` on a form control is the submission
     * name and has nothing to do with fragments. */
    if (n->tag_id == TAG_A) {
        const char *nm = dom_attr(n, "name");
        if (nm && (int)strlen(nm) == g_target_fraglen &&
            memcmp(nm, g_target_frag, (size_t)g_target_fraglen) == 0)
            *match = true;
    }
    return CSS_OK;
}

static css_error h_node_is_lang(void *pw, void *node, lwc_string *lang, bool *match)
{ (void)pw; (void)node; (void)lang; *match = false; return CSS_OK; }

static css_error h_node_presentational_hint(void *pw, void *node,
        uint32_t *nhints, css_hint **hints)
{
    (void)pw; struct node *n = node;
    /* LibCSS consumes (and does not free) these hints synchronously before
     * author rules. Feeding SVG colour through that path gives presentation
     * attributes their required low priority, and makes HTML -> SVG -> child
     * currentColor inheritance agree with the ordinary computed style. */
    static css_hint svg_hints[3];
    *nhints = 0; *hints = NULL;
    if (!n || n->ns != NS_SVG) return CSS_OK;
    const char *s = dom_attr(n, "color"); unsigned char rgba[4];
    if (s && LOGIT_HAVE(img_css_color) && img_css_color(s, (int)strlen(s), rgba)) {
        css_hint *h = &svg_hints[(*nhints)++]; memset(h, 0, sizeof *h);
        h->prop = CSS_PROP_COLOR; h->status = CSS_COLOR_COLOR;
        h->data.color = ((uint32_t)rgba[3] << 24) | ((uint32_t)rgba[0] << 16) |
                       ((uint32_t)rgba[1] << 8) | rgba[2];
    }
    s = dom_attr(n, "opacity");
    if (s && *s) {
        char *end; double v = strtod(s, &end);
        if (end != s) {
            if (*end == '%') { v /= 100; end++; }
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
            if (!*end && v == v) {
                if (v < 0) v = 0; if (v > 1) v = 1;
                css_hint *h = &svg_hints[(*nhints)++]; memset(h, 0, sizeof *h);
                h->prop = CSS_PROP_OPACITY; h->status = CSS_OPACITY_SET;
                h->data.fixed = (css_fixed)(v * 1024 + 0.5);
            }
        }
    }
    s = dom_attr(n, "display");
    if (s && !strcmp(s, "none")) {
        css_hint *h = &svg_hints[(*nhints)++]; memset(h, 0, sizeof *h);
        h->prop = CSS_PROP_DISPLAY; h->status = CSS_DISPLAY_NONE;
    }
    if (*nhints) *hints = svg_hints;
    return CSS_OK;
}

static css_error h_ua_default_for_property(void *pw, uint32_t property, css_hint *hint)
{
    (void)pw;
    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xff000000;          /* opaque black */
        hint->status = CSS_COLOR_COLOR;
    } else if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
    } else if (property == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
    } else if (property == CSS_PROP_VOICE_FAMILY) {
        hint->data.strings = NULL;
        hint->status = 0;
    } else {
        return CSS_INVALID;
    }
    return CSS_OK;
}

/* Defined below g_handler: they need it to release node data. */
static css_error h_set_libcss_node_data(void *pw, void *node, void *data);
static css_error h_get_libcss_node_data(void *pw, void *node, void **data);

static css_select_handler g_handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    h_node_name, h_node_classes, h_node_id,
    h_named_ancestor_node, h_named_parent_node, h_named_sibling_node,
    h_named_generic_sibling_node,
    h_parent_node, h_sibling_node,
    h_node_has_name, h_node_has_class, h_node_has_id,
    h_node_has_attribute, h_node_has_attribute_equal,
    h_node_has_attribute_dashmatch, h_node_has_attribute_includes,
    h_node_has_attribute_prefix, h_node_has_attribute_suffix,
    h_node_has_attribute_substring,
    h_node_is_root, h_node_count_siblings, h_node_is_empty,
    h_node_is_link,
    /* :visited is a deliberate refusal, not a gap -- see h_node_is_checked's
     * block. :hover/:active/:focus need re-style on state change, which does
     * not exist here; answering them once would freeze the styles of whatever
     * was hovered during that one pass. */
    h_false /*visited*/, h_ui_hover, h_ui_active, h_ui_focus,
    h_node_is_enabled, h_node_is_disabled, h_node_is_checked,
    h_node_is_target, h_node_is_lang,
    h_node_presentational_hint, h_ua_default_for_property,
    h_set_libcss_node_data, h_get_libcss_node_data,
};

/* ---------- LibCSS node-data cache ----------
 * LibCSS hands every styled element a `struct css_node_data` -- an ancestor
 * bloom filter plus references to that element's selection results -- and
 * expects the client to hang it off the DOM node. Ours were no-ops, which cost
 * twice over:
 *
 *   - css__get_parent_bloom (select/select.c) found no data on the parent, so
 *     for EVERY non-root element it malloc'd a fully saturated bloom filter.
 *     A saturated bloom rejects nothing, so the ancestor fast-reject that the
 *     whole descendant-combinator path is built around never fired.
 *   - css__set_node_data built a node_data per element per re-style and handed
 *     it to the no-op setter, which dropped it on the floor: the bloom malloc
 *     and CSS_PSEUDO_ELEMENT_COUNT computed-style references leaked, once per
 *     element per css_apply.
 *   - style sharing between same-named siblings needs to read a candidate's
 *     node data, so it could never fire either.
 *
 * `struct node` belongs to dom.c (which links without LibCSS), so the data
 * lives in a side table keyed by node pointer rather than in a node field.
 *
 * The table's lifetime is ONE css_apply pass, deliberately. What is cached is
 * selection RESULTS, and the sheet set changes between passes -- browser.c
 * re-applies after the external <link> sheets arrive and again after a script
 * mutates the DOM -- so entries carried across a pass would serve the next one
 * styles selected against the previous sheet set. Dropping the table at the
 * end of the pass also means it can never hold a pointer to a node that JS
 * destroyed in the meantime, which is the only way this side table could
 * outlive its key. Both caches are intra-pass wins anyway: a parent is always
 * styled before its children, and share candidates are earlier siblings. */
struct ndslot { struct node *key; void *data; };
static struct ndslot *g_nd;
static int g_ndcap, g_ndused;
static int g_stat_styled, g_stat_hits;

static unsigned nd_hash(const struct node *n)
{
    /* Fibonacci hash. Nodes are bump-allocated from the document arena, so
     * their addresses march in a fixed stride and the raw low bits would put
     * every node of a page into a handful of buckets. */
    uintptr_t v = (uintptr_t)n;
    return (unsigned)(((v >> 4) * 2654435761u) >> 8);
}

static void nd_release(void *data)
{
    if (data)
        css_libcss_node_data_handler(&g_handler, CSS_NODE_DELETED,
                                     NULL, NULL, NULL, data);
}

static int nd_grow(void)
{
    int ncap = g_ndcap ? g_ndcap * 2 : 1024;
    struct ndslot *nt = kmalloc((unsigned long)ncap * sizeof *nt);
    if (!nt) return 0;
    memset(nt, 0, (size_t)ncap * sizeof *nt);
    for (int i = 0; i < g_ndcap; i++) {
        if (!g_nd[i].key) continue;
        unsigned m = (unsigned)(ncap - 1), h = nd_hash(g_nd[i].key) & m;
        while (nt[h].key) h = (h + 1) & m;
        nt[h] = g_nd[i];
    }
    if (g_nd) kfree(g_nd);
    g_nd = nt; g_ndcap = ncap;
    return 1;
}

/* Open addressing, linear probing, no deletions within a pass (LibCSS only
 * ever replaces an entry). Kept below 70% load so probes stay short. */
static struct ndslot *nd_slot(struct node *n, int create)
{
    if (!g_nd || (create && (g_ndused + 1) * 10 >= g_ndcap * 7))
        if (create && !nd_grow()) return 0;
    if (!g_nd) return 0;
    unsigned m = (unsigned)(g_ndcap - 1), h = nd_hash(n) & m;
    while (g_nd[h].key) {
        if (g_nd[h].key == n) return &g_nd[h];
        h = (h + 1) & m;
    }
    if (!create) return 0;
    g_nd[h].key = n; g_nd[h].data = 0; g_ndused++;
    return &g_nd[h];
}

static void nd_reset(void)
{
    for (int i = 0; i < g_ndcap; i++) nd_release(g_nd[i].data);
    if (g_nd) kfree(g_nd);
    g_nd = 0; g_ndcap = 0; g_ndused = 0;
}

static css_error h_set_libcss_node_data(void *pw, void *node, void *data)
{
    (void)pw;
    struct ndslot *s = nd_slot(node, 1);
    /* On failure DON'T free `data`: css__set_node_data destroys it itself when
     * the handler reports an error, and freeing it here would double-free. */
    if (!s) return CSS_NOMEM;
    if (s->data && s->data != data) nd_release(s->data);   /* documented "replaces" */
    s->data = data;
    return CSS_OK;
}

static css_error h_get_libcss_node_data(void *pw, void *node, void **data)
{
    (void)pw;
    struct ndslot *s = nd_slot(node, 0);
    *data = s ? s->data : NULL;
    if (*data) g_stat_hits++;
    return CSS_OK;
}

void css_stats(int *styled, int *cache_hits)
{
    if (styled) *styled = g_stat_styled;
    if (cache_hits) *cache_hits = g_stat_hits;
}

/* ---------- engine state ---------- */
static css_select_ctx *g_ctx;
static css_stylesheet *g_ua_sheet;
static css_stylesheet *g_quirks_sheet;
static int g_quirks_appended;   /* g_quirks_sheet is currently in g_ctx */
static bool g_allow_quirks;     /* the document being styled is in quirks mode:
                                 * every sheet parsed for it (author, inline
                                 * style=) gets allow_quirks. Set by css_apply,
                                 * read by style_node. */
static css_unit_ctx g_unit;
static css_media g_media;
static int g_screen_w, g_screen_h; /* only css_set_screen supplies device metrics */
static int g_vw, g_vh;          /* last viewport set via css_viewport (0 = never) */
/* The colour scheme this document is being rendered for. Interned once and
 * pointed at by g_media.prefers_color_scheme.
 *
 * It must be a real string, not NULL. LibCSS's mq matcher compares the query's
 * ident against media->prefers_color_scheme, and with NULL BOTH
 * `(prefers-color-scheme: dark)` and `(prefers-color-scheme: light)` fail --
 * which is not "no preference", it is "neither theme". A stylesheet that puts
 * its light theme inside `@media (prefers-color-scheme: light)` (increasingly
 * common, and how a `light-dark()`-era sheet is written) then gets no theme at
 * all. Defaulting to "light" is the honest answer for a UA with no user setting
 * and it makes exactly one of the two blocks match, which is the shape every
 * such stylesheet is written against. */
static lwc_string *g_scheme;
static int g_scheme_dark;
/* System preference, shared across document contexts. */
static int g_reduced_motion;

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }

static const char UA_CSS[] =
    "body{display:block;margin:8px}"
    "div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,pre,header,footer,section,article,nav,main,blockquote,figure,figcaption,table,form,"
    /* The gap this line closes, measured on the Bing search results page
     * (tests/fixtures/webapi/bing/search: `curl -A "$LOGIT_UA" .../search?q=python`):
     * `<aside>` -- a named sectioning element, CLAUDE.md's own "sectioning
     * elements" bullet -- had NO rule in this sheet at all, so a page whose
     * external CSS is dropped, missing, or has not arrived (which is common;
     * see css_report.c) renders every sidebar/aside INLINE, smeared into the
     * surrounding text run. `address`, `dl`/`dt`/`dd`, `fieldset`/`legend`,
     * `details`/`summary` had the identical bug for the same reason: nobody
     * had gone through the HTML Standard's rendering section element by
     * element against this file. dt/dd/legend/hr get their own rules below
     * (dd needs an indent, hr needs to be visible at all -- `display:block`
     * alone is not enough for either). */
    "address,aside,dl,fieldset,details,summary{display:block}"
    "tr,td,th,thead,tbody,tfoot{display:block}"
    "th{font-weight:bold}"
    "h1{font-size:32px;font-weight:bold;margin:14px 0}"
    "h2{font-size:24px;font-weight:bold;margin:12px 0}"
    "h3{font-size:19px;font-weight:bold;margin:10px 0}"
    "h4{font-size:16px;font-weight:bold;margin:10px 0}"
    "h5{font-size:13px;font-weight:bold;margin:8px 0}"
    "h6{font-size:11px;font-weight:bold;margin:8px 0}"
    "p{margin:8px 0}"
    "a{color:#1a0dab;text-decoration:underline}"
    "b{font-weight:bold}strong{font-weight:bold}"
    "i{font-style:italic}em{font-style:italic}"
    "ul{margin:8px 0;padding-left:28px}ol{margin:8px 0;padding-left:28px}"
    "li{display:list-item}"
    /* list-style-type is inherited, so putting it on the list element is what
     * gives each <li> its marker alphabet. LibCSS's initial value is already
     * disc; ol needs decimal, and the nesting rules match what every real UA
     * sheet does (disc -> circle -> square). */
    "ol{list-style-type:decimal}ul{list-style-type:disc}"
    "ul ul,ol ul{list-style-type:circle}ul ul ul,ol ol ul{list-style-type:square}"
    "pre{font-family:monospace;margin:8px 0}code{font-family:monospace}"
    /* white-space: the whole point of <pre> and friends. They already had
     * that rule and NOT display:block -- xmp/plaintext/listing are obsolete
     * but still real (legacy-content sniffers, some code-dump pages), and
     * without a display rule the CSS initial `inline` applied: their
     * preformatted text ran inline into whatever followed it instead of
     * standing as its own block, on top of never wrapping (white-space:pre
     * inside an inline box still measures its line the normal way). */
    "pre,xmp,plaintext,listing{white-space:pre}"
    "xmp,plaintext,listing{display:block}"
    "textarea{white-space:pre-wrap}nobr{white-space:nowrap}"
    /* Control chrome used to exist only in paint_control: computed transparent
     * + zero borders was mistaken for "author did not style it" and restored
     * a white system button. The flex-control fix exposed that on real nav
     * buttons. Defaults belong in the UA cascade, where background:none and
     * border:0 can override them, including stylesheet and inline !important.
     * appearance is deliberately not inferred from those resets: there is no
     * appearance property channel yet, so appearance:none alone remains absent. */
    "button,input,select,textarea{background:#fff;border:1px solid #b0b4ba;"
    "border-radius:4px;color:#1d1d1f}"
    /* Former native child insets: 3px vertical / 5px horizontal. Keeping them
     * in UA CSS lets authored padding:0 remove them, like the chrome above. */
#define UA_PAD_TEXT_(v) #v
#define UA_PAD_TEXT(v) UA_PAD_TEXT_(v)
#ifdef FC_CONTENT_BOX_LEGACY
    "button{padding:" UA_PAD_TEXT(FC_PAD_Y) "px " UA_PAD_TEXT(FC_PAD_X) "px}"
#else
    "button,input,select,textarea{padding:" UA_PAD_TEXT(FC_PAD_Y) "px " UA_PAD_TEXT(FC_PAD_X) "px}"
    "input[type=checkbox],input[type=radio]{padding:0}"
#endif
#undef UA_PAD_TEXT
#undef UA_PAD_TEXT_
    "button,select,input[type=button],input[type=submit],input[type=reset],"
    "input[type=file]{background:#f6f7f8;border-radius:5px}"
    "button{text-align:center}"
    "button:disabled,input:disabled,select:disabled,textarea:disabled{"
    "background:#f1f2f4;color:#9aa0a6}"
    "svg{display:inline}"
    /* dt/dd: same missing-rule bug as address/aside above, in the one place
     * it also needs a NUMBER, not just a box type -- a definition list with
     * no CSS at all (a common FAQ/glossary shape) must still read as
     * term/answer, which means dd needs to be visually offset from dt. 28px
     * matches this file's own convention (the same indent ul/ol already use
     * for their markers, see padding-left above) rather than the spec's
     * literal 40px, so a page mixing dl and ul without any CSS gets one
     * consistent rhythm instead of two different indents that happen to both
     * be "roughly a tab stop". */
    "dt{display:block}dd{display:block;margin-left:28px}"
    /* legend: block, same bug, no numeric embellishment -- fieldset's own
     * border/padding is a real box-model feature this UA sheet does not
     * attempt (this section is about display DEFAULTS, the ones the CLAUDE.md
     * brief itself says matter most: "an element that should be block
     * rendering inline collapses a whole page into a run-on line"). */
    "legend{display:block}"
    /* hr: NOT in this sheet at all before this line, and display:block alone
     * would not have fixed it -- an hr with a block box, zero content, zero
     * border and zero explicit height is a REAL box that is completely
     * INVISIBLE, which is indistinguishable from "this element's CSS never
     * arrived" (the exact ambiguity item 1's css_report.c exists to close,
     * from the opposite direction: that instrument tells you whether author
     * CSS reached the cascade, this rule is what the page looks like when it
     * legitimately did not reach it, or never needed to for THIS element).
     * height:0 + a single top border is the smallest declaration set that
     * produces an actual line; margin:auto centers it if a width narrower
     * than the container is ever set by an author rule (layout.c already
     * supports margin:auto centering, see the comment at is_bfc_root's
     * caller). */
    "hr{display:block;height:0;margin:8px auto;border:none;border-top:1px solid #8a8a8a}"
    /* iframe: before this rule it had no display in this sheet at all, which
     * means the CSS default of `inline` applied and, being non-replaced by
     * this engine's own is_block()/blockish() rules, it took NO BOX -- an
     * <iframe> painted nothing and reserved nothing, indistinguishable from
     * one that failed to parse. inline-block is what lets an element's
     * width/height apply without content; 300x150 is the same generic
     * replaced-element default this file already uses for <video>/<canvas>
     * (layout.c, both comments say so) and is what HTML itself specifies for
     * a sizeless iframe. The border is cosmetic and matches the historical
     * UA default (an inset 2px groove) so an empty frame reads as a frame
     * and not as a stray blank rectangle. */
    "iframe{display:inline-block;width:300px;height:150px;border:2px inset #8a8a8a}"
    "script,style,head,title,meta,link,noscript,template,[hidden]{display:none}"
    /* HTML SS 4.11.4's UA sheet, missing outright: a closed <dialog> (no
     * `open` attribute) is display:none. Absent this rule a closed dialog
     * lays out as an ordinary block wherever the document puts it, and a
     * dialog placed before <main> and sized to the viewport (a common
     * pattern: mount the modal root once, near the top of the DOM, and
     * toggle `open` later) is now doubly wrong with position:fixed also
     * fixed to be out-of-flow above -- it would anchor to the viewport
     * origin at its full declared size instead of disappearing. */
    "dialog{display:block;background:white;color:black;padding:1em;border:3px solid black}"
    "dialog:not([open]){display:none}";

/* The quirks-mode UA sheet, appended ON TOP of UA_CSS (same UA origin, later
 * wins on equal specificity) when dom_doc_quirks() says QM_QUIRKS. This is the
 * cascade half of quirks mode; make_sheet's allow_quirks is the parser half.
 *
 * COVERS the one cascade quirk that visibly changes real pre-doctype pages:
 * a <table> does not inherit font or alignment from its ancestors. Legacy
 * pages routinely set `font-size:small` or `text-align:center` on <body> and
 * lay the page out in tables that they expect to stay at the default size and
 * left-aligned -- in standards mode those tables inherit and the page comes
 * out tiny and centred. (`font-size:medium` is 16px here, matching
 * g_unit.font_size_default.)
 *
 * DOES NOT COVER, deliberately, because each needs engine support we do not
 * have rather than a CSS rule: the "almost standards" inline-image line-box
 * quirk (QM_LIMITED_QUIRKS is treated exactly like QM_NO_QUIRKS -- limited
 * quirks differs from standards ONLY in that quirk); percentage heights
 * resolving against the viewport through auto-height ancestors; the
 * unitless-line-height and border-on-img-in-a quirks; and quirks-mode
 * table-cell width/height distribution. */
static const char QUIRKS_CSS[] =
    "table{font-size:medium;font-weight:normal;font-style:normal;"
    "line-height:normal;text-align:left}";

/* `quirks` is LibCSS's allow_quirks: it loosens the PARSER, not the cascade.
 * Two things become legal, both of them ubiquitous in pre-doctype HTML:
 * unitless lengths ("width:100" == 100px, and "0 px" with a stray space) and
 * hashless hex colours ("color:FF0000", "bgcolor=CCCCCC" carried into CSS).
 * Without it LibCSS drops those declarations outright and the page loses its
 * table widths and its colours. */
static css_stylesheet *make_sheet(const char *data, size_t len, bool inl, bool quirks)
{
    css_stylesheet_params p;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = "http://logit/";
    p.title = NULL;
    p.inline_style = inl;
    p.allow_quirks = quirks;
    p.resolve = resolve_url;
    css_stylesheet *s = NULL;
    if (css_stylesheet_create(&p, &s) != CSS_OK || !s) return NULL;
    css_stylesheet_append_data(s, (const uint8_t *)data, len);
    css_stylesheet_data_done(s);
    return s;
}

/* Exact selector matching for properties outside LibCSS's vocabulary.
 * The previous css_extra scan dropped ancestors and everything after ':': a
 * dark-theme ancestor painted light pages dark, and :hover::before transformed
 * the real link. Reuse this engine's parser, combinators and state handlers.
 * One synthetic sheet (z-index carries a rule ID) gives one indexed selection
 * per element, rather than a parse/selection for every element x rule pair.
 * No synthetic declaration reaches cstyle; it is only an internal ID channel.
 * The adapter uses the existing match-report hook because computed style only
 * retains the WINNING declaration, whereas extra properties need every match.
 */
extern void (*css__select_match_report)(const css_selector *, int);
extern void (*css__stylesheet_rule_decl_report)(const css_rule *);
extern void (*css__parse_drop_report)(const char *, size_t, int);
extern void (*css__parse_selector_drop_report)(const char *, size_t);
extern void (*css__parse_recover_report)(int);
struct css_extra_matcher {
    css_stylesheet *sheet;
    css_select_ctx *ctx;
    int count;
};
static unsigned char *extra_matches;
static int extra_match_count;
static uint32_t *extra_specificity;

static int extra_rule_id(const css_rule *r)
{
    if (!r || r->type != CSS_RULE_SELECTOR) return -1;
    const css_style *s = ((const css_rule_selector *)r)->style;
    if (!s || s->used != 2 || getOpcode(s->bytecode[0]) != CSS_PROP_Z_INDEX ||
        getValue(s->bytecode[0]) != Z_INDEX_SET) return -1;
    return FIXTOINT((css_fixed)s->bytecode[1]);
}
static int extra_element_selector(const css_selector *s)
{
    /* The parser forbids pseudo elements in ancestors. Inspect every detail
     * of the target, including :before's legacy single-colon spelling. */
    const css_selector_detail *d = &s->data;
    do {
        if (d->type == CSS_SELECTOR_PSEUDO_ELEMENT) return 0;
    } while ((d++)->next);
    return 1;
}
static void extra_match_report(const css_selector *s, int matched)
{
    if (!matched || !extra_element_selector(s)) return;
    int id = extra_rule_id(s->rule);
    if (id >= 0 && id < extra_match_count) {
        extra_matches[id] = 1;
        /* Only actually matching list arms count; an unmatched #id cannot
         * lend its weight to a matching .class in the same selector list. */
        if (extra_specificity && s->specificity > extra_specificity[id])
            extra_specificity[id] = s->specificity;
    }
}
void css_extra_matcher_destroy(void *ptr)
{
    struct css_extra_matcher *m = ptr;
    if (!m) return;
    if (m->ctx) css_select_ctx_destroy(m->ctx);
    if (m->sheet) css_stylesheet_destroy(m->sheet);
    kfree(m);
}
void *css_extra_matcher_create(const char *src, int len, int count,
                              unsigned char *accepted)
{
    struct css_extra_matcher *m = kmalloc(sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);
    memset(accepted, 0, (size_t)count);
    m->count = count;
    /* Census hooks describe the AUTHOR input. Reporting our ID declarations
     * would double-count rules, invent z-index usage and retain freed synthetic
     * rule pointers in the census. Unsupported selectors are instead counted
     * from the accepted array returned to css_extra. Parsing is synchronous. */
    void (*old_decl)(const css_rule *) = css__stylesheet_rule_decl_report;
    void (*old_drop)(const char *, size_t, int) = css__parse_drop_report;
    void (*old_selector)(const char *, size_t) = css__parse_selector_drop_report;
    void (*old_recover)(int) = css__parse_recover_report;
    css__stylesheet_rule_decl_report = NULL;
    css__parse_drop_report = NULL;
    css__parse_selector_drop_report = NULL;
    css__parse_recover_report = NULL;
    m->sheet = make_sheet(src, (size_t)len, false, false);
    css__stylesheet_rule_decl_report = old_decl;
    css__parse_drop_report = old_drop;
    css__parse_selector_drop_report = old_selector;
    css__parse_recover_report = old_recover;
    if (!m->sheet || css_select_ctx_create(&m->ctx) != CSS_OK ||
        css_select_ctx_append_sheet(m->ctx, m->sheet, CSS_ORIGIN_AUTHOR, NULL) != CSS_OK) {
        css_extra_matcher_destroy(m); return NULL;
    }
    /* data_done can succeed while dropping an unsupported/invalid selector.
     * Count accepted parsed rules, never equate successful parsing with support.
     * A pseudo-only rule is also refused: css_extra has no pseudo-element
     * cstyle target. Mixed lists keep their valid element-target arms. */
    for (css_rule *r = m->sheet->rule_list; r; r = r->next) {
        int id = extra_rule_id(r);
        if (id < 0 || id >= count) continue;
        css_rule_selector *rs = (css_rule_selector *)r;
        for (int j = 0; j < r->items; j++)
            if (extra_element_selector(rs->selectors[j])) accepted[id] = 1;
    }
    return m;
}
struct extra_node_data { void *data; struct extra_node_data *next; };
static css_error extra_get_data(void *pw, void *node, void **data)
{ (void)pw; (void)node; *data = NULL; return CSS_OK; }
static css_error extra_set_data(void *pw, void *node, void *data)
{
    (void)node;
    /* set_node_data transfers ownership only when this callback succeeds.
     * Release AFTER selection returns, not while LibCSS still owns its state. */
    /* LibCSS can transfer a synthesized PARENT bloom as well as this node's
     * data in one call. Keeping only the last pointer leaked one bloom per
     * element; keep every transfer until the selection has finished. */
    struct extra_node_data *entry = kmalloc(sizeof *entry);
    if (!entry) return CSS_NOMEM;
    entry->data = data;
    entry->next = *(struct extra_node_data **)pw;
    *(struct extra_node_data **)pw = entry;
    return CSS_OK;
}
int css_extra_matcher_match_specificity(void *ptr, struct node *node, unsigned char *matches, uint32_t *specificity)
{
    struct css_extra_matcher *m = ptr;
    if (!m) return 0;
    if (!g_ctx) css_init();
    memset(matches, 0, (size_t)m->count);
    if (specificity) memset(specificity, 0, (size_t)m->count * sizeof *specificity);
    css_select_handler handler = g_handler;
    /* Never reuse author node data: style sharing could bypass match callbacks
     * or let dummy z-index data contaminate the next ordinary cascade. */
    handler.get_libcss_node_data = extra_get_data;
    handler.set_libcss_node_data = extra_set_data;
    struct extra_node_data *data = NULL;
    css_select_results *res = NULL;
    void (*old_report)(const css_selector *, int) = css__select_match_report;
    extra_matches = matches; extra_match_count = m->count; extra_specificity = specificity;
    css__select_match_report = extra_match_report;
    css_error err = css_select_style(m->ctx, node, &g_unit, &g_media, NULL,
                                     &handler, &data, &res);
    css__select_match_report = old_report;
    extra_matches = NULL; extra_match_count = 0; extra_specificity = NULL;
    if (res) css_select_results_destroy(res);
    while (data) {
        struct extra_node_data *entry = data;
        data = entry->next;
        css_libcss_node_data_handler(&handler, CSS_NODE_DELETED,
                                     NULL, NULL, NULL, entry->data);
        kfree(entry);
    }
    return err == CSS_OK;
}

int css_extra_matcher_match(void *ptr, struct node *node, unsigned char *matches)
{ return css_extra_matcher_match_specificity(ptr, node, matches, NULL); }

/* ---------------- the author stylesheet, parsed ONCE ----------------
 *
 * css_apply and css_apply_scoped both used to make_sheet(page_css) on entry
 * and css_stylesheet_destroy it on exit. For a page load that is one parse per
 * sheet set, which is honest work. For a LIVE page it is a full LibCSS parse
 * of the entire stylesheet on every single DOM mutation -- and the scoped
 * invalidation that made the selection cheap left this in front of it.
 *
 * Measured host-side (tests/unit/css_bench.c, `make bench-css`), re-styling one
 * leaf plus its following siblings:
 *
 *              scoped re-style   of which the actual selection   sheet parse
 *   deepseek        4.47 ms                0.11 ms                ~1.4 ms
 *   wikipedia       8.09 ms                0.01 ms                ~5.0 ms
 *
 * So the parse is cached against the exact bytes it was produced from. The
 * browser hands back the same expanded buffer on every mutation, so the common
 * case is one memcmp (microseconds on 224 KB) and a pointer return. A genuinely
 * new sheet set -- external stylesheets arriving, a <style> a script wrote,
 * a different var() expansion -- differs in those bytes and re-parses exactly
 * as it did before, so this cannot serve a stale stylesheet.
 *
 * The comparison is against a private COPY rather than the caller's pointer:
 * callers reuse one buffer and rewrite it in place, so pointer identity says
 * nothing about content identity. It is a full memcmp rather than a hash
 * because a hash collision here does not fail loudly, it renders the previous
 * page's CSS.
 *
 * The cache holds one parsed sheet and one copy of its source for the life of
 * the process. That is reachable from a static root, so it is not a leak (the
 * LeakSanitizer gate in `make test-css-asan` agrees); the previous code's peak
 * was the same parsed sheet plus the caller's buffer anyway. */
static css_stylesheet *g_author_sheet;
static char           *g_author_src;
static size_t          g_author_srclen;
static bool            g_author_quirks;
static int             g_author_parses;   /* test seam; see css_sheet_parses() */

int css_sheet_parses(void) { return g_author_parses; }

static css_stylesheet *author_sheet(const char *data, size_t len, bool quirks)
{
    if (g_author_sheet && g_author_quirks == quirks && g_author_srclen == len &&
        (len == 0 || memcmp(g_author_src, data, len) == 0)) {
        /* No parse happened, so no drop hook fired. Said out loud, because
         * "parsed 0, dropped 0" otherwise reads exactly like "no stylesheet
         * ever reached the cascade". */
        css_report_parse_cached();
        return g_author_sheet;
    }

    /* Content differs: drop the old parse before building the new one, so the
     * cache never holds two whole stylesheets at once. */
    if (g_author_sheet) { css_stylesheet_destroy(g_author_sheet); g_author_sheet = NULL; }
    if (g_author_src)   { kfree(g_author_src); g_author_src = NULL; }
    g_author_srclen = 0;

    g_author_parses++;
    css_stylesheet *s = make_sheet(data, len, false, quirks);
    /* THE one site where a page's own stylesheet is handed to LibCSS. Reported
     * here rather than at the caller so the count cannot disagree with what
     * actually got parsed. */
    css_report_parsed(s != NULL, (int)len);
    if (!s) return NULL;
    /* Without the source copy we cannot answer "is this the same sheet?" next
     * time, so a failed copy means: use this parse, but do not cache it. */
    char *cp = (char *)kmalloc(len ? len : 1);
    if (!cp) { css_stylesheet_destroy(s); return make_sheet(data, len, false, quirks); }
    if (len) memcpy(cp, data, len);
    g_author_sheet = s; g_author_src = cp; g_author_srclen = len; g_author_quirks = quirks;
    return s;
}

/* dom.c owns node->computed but must not know what a css_computed_style is
 * (dom.c links without LibCSS in the standalone host tests), so it calls back
 * through a registered releaser. */
static void free_computed(void *p)
{ if (p) css_computed_style_destroy((css_computed_style *)p); }

void css_init(void)
{
    dom_set_computed_free(free_computed);
    /* Honour a viewport set before init (css_apply lazily inits on first use,
     * which must not clobber an explicit css_viewport call). */
    int vw = g_vw ? g_vw : 760, vh = g_vh ? g_vh : 540;
    memset(&g_media, 0, sizeof g_media);
    g_media.type = CSS_MEDIA_SCREEN;
    g_media.width  = INTTOFIX(vw);
    g_media.height = INTTOFIX(vh);
    g_media.device_width = INTTOFIX(g_screen_w);
    g_media.device_height = INTTOFIX(g_screen_h);
    css_set_color_scheme(g_scheme_dark);
    g_media.prefers_reduced_motion = g_reduced_motion;

    memset(&g_unit, 0, sizeof g_unit);
    g_unit.viewport_width  = INTTOFIX(vw);
    g_unit.viewport_height = INTTOFIX(vh);
    g_unit.font_size_default = INTTOFIX(16);
    g_unit.font_size_minimum = INTTOFIX(6);
    g_unit.device_dpi = INTTOFIX(96);
    g_unit.root_style = NULL;   /* g_unit.measure stays NULL (it is a const member) */

    if (css_select_ctx_create(&g_ctx) != CSS_OK) { g_ctx = NULL; return; }
    g_ua_sheet = make_sheet(UA_CSS, sizeof UA_CSS - 1, false, false);
    if (g_ua_sheet) css_select_ctx_append_sheet(g_ctx, g_ua_sheet, CSS_ORIGIN_UA, NULL);
    /* Parsed once and kept; it is appended to / removed from the context per
     * document, since a page's quirks mode is a property of its doctype. */
    g_quirks_sheet = make_sheet(QUIRKS_CSS, sizeof QUIRKS_CSS - 1, false, false);
    g_quirks_appended = 0;
}

/* Match the context's quirks UA sheet to `on`. Idempotent -- css_apply runs
 * several times per page (after external sheets, after a script mutation) and
 * appending the same sheet twice would style every table twice over. */
static void set_quirks_sheet(int on)
{
    if (!g_ctx || !g_quirks_sheet || on == g_quirks_appended) return;
    if (on) css_select_ctx_append_sheet(g_ctx, g_quirks_sheet, CSS_ORIGIN_UA, NULL);
    else    css_select_ctx_remove_sheet(g_ctx, g_quirks_sheet);
    g_quirks_appended = on;
}

/* Keep screen and viewport separate: dragging/resizing a browser window must
 * not change (min-device-width) or screen.width. Zero means no device facts
 * were supplied by this embedder; css_init preserves an earlier measurement. */
void css_set_screen(int w, int h)
{
    g_screen_w = w > 0 ? w : 0; g_screen_h = h > 0 ? h : 0;
    g_media.device_width = INTTOFIX(g_screen_w);
    g_media.device_height = INTTOFIX(g_screen_h);
}
int css_screen_width(void) { return g_screen_w; }
int css_screen_height(void) { return g_screen_h; }

void css_viewport(int w, int h)
{
    g_vw = w; g_vh = h;
    g_media.width  = INTTOFIX(w);
    g_media.height = INTTOFIX(h);
    g_unit.viewport_width  = INTTOFIX(w);
    g_unit.viewport_height = INTTOFIX(h);
}

void css_set_color_scheme(int dark)
{
    g_scheme_dark = dark ? 1 : 0;
    if (g_scheme) { lwc_string_unref(g_scheme); g_scheme = NULL; }
    const char *s = g_scheme_dark ? "dark" : "light";
    if (lwc_intern_string(s, g_scheme_dark ? 4 : 5, &g_scheme) != lwc_error_ok)
        g_scheme = NULL;
    g_media.prefers_color_scheme = g_scheme;
}

void css_set_reduced_motion(int reduced)
{
    g_reduced_motion = reduced != 0;
#ifndef OPENLOGIT_MOTION_INVALIDATION_DISABLED
    /* A preference change is not a DOM mutation. Without this revision a
     * getComputedStyle read can reuse the old media cascade indefinitely. */
    if (g_media.prefers_reduced_motion != g_reduced_motion) ui_revision++;
#endif
    g_media.prefers_reduced_motion = g_reduced_motion;
}

int css_color_scheme(void) { return g_scheme_dark; }

int css_media_matches(const char *query, int len)
{
    if (!query) return 1;
    if (len < 0) { len = 0; while (query[len]) len++; }
    /* Trim: an @media prelude arrives with the whitespace that separated it
     * from the '{', and LibCSS's query parser is happy with that -- but an
     * all-whitespace prelude must read as "all", not as a parse failure. */
    while (len > 0 && (*query == ' ' || *query == '\t' || *query == '\n' ||
                       *query == '\r' || *query == '\f')) { query++; len--; }
    while (len > 0 && (query[len-1] == ' ' || query[len-1] == '\t' ||
                       query[len-1] == '\n' || query[len-1] == '\r' ||
                       query[len-1] == '\f')) len--;
    if (len == 0) return 1;
    if (!g_ctx) css_init();
    if (!g_ctx) return 1;
    bool m = false;
    if (css_select_ctx_media_matches(g_ctx, query, (size_t)len,
                                     &g_unit, &g_media, &m) != CSS_OK)
        return 1;
    return m ? 1 : 0;
}

int css_media_width(void) { return g_vw ? g_vw : 760; }
int css_media_height(void) { return g_vh ? g_vh : 540; }
static int vw_px(void) { return g_vw ? g_vw : 760; }
static int vh_px(void) { return g_vh ? g_vh : 540; }

/* ---------- computed style -> struct cstyle ---------- */

/* font-size of the document root, for `rem`. Set once the root element has
 * been styled (see style_node); 16 until then, which is also what CSS says a
 * `rem` on the root element itself means. */
static int g_root_px = 16;

/* The `root_px` argument of css_gradient_parse / css_shadow_parse /
 * css_origin_parse, and the only way to answer it from outside this file.
 *
 * WITHOUT THIS THE THREE PARSERS ARE UNUSABLE FOR `rem`, which is not a corner
 * case: browser_paint.c holds a cstyle (so it has font_px for `em`) and has no
 * route at all to the root's font size, because g_root_px is static and the
 * value is not on any cstyle -- it is a property of the DOCUMENT. The two
 * alternatives were both worse. Walking to the root node and reading its
 * cstyle.font_px duplicates a fact this file already computes at two sites
 * with real logic (style_node's is_root branch and the js_dom re-style path),
 * so the copy drifts the first time either changes. Passing a constant 16 is
 * the plausible-wrong-number failure this whole line exists to avoid: every
 * page that sets `html{font-size:62.5%}` -- the 10px-root idiom -- would
 * resolve every rem in a shadow, an origin or a gradient stop at 1.6x. */
int css_root_px(void) { return g_root_px; }

/* CSS pixels per unit, in css_fixed.
 *
 * Written out here rather than calling LibCSS's css_unit_len2css_px() for two
 * reasons, both in select/unit.c:
 *
 *   - that helper does `px_per_unit += F_0_5; FMUL(length, TRUNCATEFIX(...))`,
 *     i.e. it rounds the SCALE to a whole pixel before multiplying. 1pt (1.333
 *     px/unit) becomes 1px and 1vw of a 760px viewport (7.6 px/unit) becomes
 *     8px, so `width:100vw` would come out 800. Keeping the fraction until the
 *     final multiply costs nothing and is exact.
 *   - css_unit__px_per_unit has vw and vh transposed (CSS_UNIT_VH returns
 *     viewport_width/100 and CSS_UNIT_VW returns viewport_height/100), which
 *     also makes its own vmin/vmax mapping select the wrong axis.
 *
 * Returns 0 for anything that is not a length (angles, times, an unresolved
 * calc()), which len_px reports as "no length". */
static css_fixed px_per_unit(css_unit unit, int font_px)
{
    switch (unit) {
    case CSS_UNIT_PX:   return F_1;
    case CSS_UNIT_EM:   return INTTOFIX(font_px);
    case CSS_UNIT_REM:  return INTTOFIX(g_root_px);
    /* ex/ch want real font metrics (x-height, the '0' advance). We have none
     * here, so use the same fixed ratios LibCSS falls back to when its measure
     * callback is NULL -- and note that compute_absolute_values has already
     * folded most authored `ex` into `em` before we see it. */
    case CSS_UNIT_EX:   return FMUL(INTTOFIX(font_px), FLTTOFIX(0.6));
    case CSS_UNIT_CH:   return FMUL(INTTOFIX(font_px), FLTTOFIX(0.4));
    /* lh: layout's default line box is font*5/4 (see flow_text). */
    case CSS_UNIT_LH:   return FMUL(INTTOFIX(font_px), FLTTOFIX(1.25));
    case CSS_UNIT_IN:   return F_96;
    case CSS_UNIT_CM:   return FDIV(F_96, FLTTOFIX(2.54));
    case CSS_UNIT_MM:   return FDIV(F_96, FLTTOFIX(25.4));
    case CSS_UNIT_Q:    return FDIV(F_96, FLTTOFIX(101.6));
    case CSS_UNIT_PT:   return FDIV(F_96, F_72);
    case CSS_UNIT_PC:   return INTTOFIX(16);          /* 1pc = 12pt = 16px */
    case CSS_UNIT_VW:   return FDIV(INTTOFIX(vw_px()), F_100);
    case CSS_UNIT_VH:   return FDIV(INTTOFIX(vh_px()), F_100);
    /* vi/vb are the writing-mode-relative pair; we only do horizontal-tb. */
    case CSS_UNIT_VI:   return FDIV(INTTOFIX(vw_px()), F_100);
    case CSS_UNIT_VB:   return FDIV(INTTOFIX(vh_px()), F_100);
    case CSS_UNIT_VMIN: return FDIV(INTTOFIX(vw_px() < vh_px() ? vw_px() : vh_px()), F_100);
    case CSS_UNIT_VMAX: return FDIV(INTTOFIX(vw_px() > vh_px() ? vw_px() : vh_px()), F_100);
    default:            return 0;
    }
}

static int len_px(css_fixed val, css_unit unit, int font_px, int *pct)
{
    if (pct) *pct = 0;
    if (unit == CSS_UNIT_PCT) { if (pct) *pct = 1; return FIXTOINT(val + F_0_5); }
    css_fixed per = px_per_unit(unit, font_px);
    if (per == 0) return 0;
    return FIXTOINT(FMUL(val, per) + F_0_5);
}

/* Computed font-size still carries fractional CSS px (or an absolute unit).
 * Do not round it to cstyle.font_px before resolving an inherited length: a
 * 14.5px font at 200% has a 29px line, even if raster text uses 15px glyphs. */
static css_fixed line_font_px(const css_computed_style *cs, int fallback)
{
    css_fixed size; css_unit unit;
    if (!cs || css_computed_font_size(cs, &size, &unit) != CSS_FONT_SIZE_DIMENSION)
        return INTTOFIX(fallback);
    return FMUL(size, px_per_unit(unit, fallback));
}

/* LibCSS's absolute-line-height pass only folds ex into em. It leaves %/em
 * on the composed style, and compose_line_height later copies that unit into
 * the child. The old convert() then sent 130% through len_px(NULL), producing
 * 130px; merely multiplying there would fix the parent but make an inherited
 * percentage/em depend on the CHILD font (20px/1.3em -> 40px child gave 52px,
 * not the parent's 26px, in the pass4 host reproduction).
 *
 * Normalize dimensions BEFORE this style becomes the inheritance parent.
 * NUMBER is deliberately untouched: 1.5 remains a ratio on descendants.
 * eff is already arena-interned and may be shared by siblings/node caches.
 * Never mutate it in place: clone, normalize, then intern the new value.
 * The local px_per_unit is used instead of LibCSS's length helper because
 * that helper rounds the per-unit scale (and swaps viewport axes), as its
 * comment above documents. Keep fixed precision until final layout rounding. */
static css_error normalize_line_height(const css_computed_style *eff,
                                      const css_computed_style *parent,
                                      int parent_font, css_computed_style **out)
{
    *out = NULL;
#ifndef CSS_NEGCTL_LINE_HEIGHT_DIMENSION
    css_fixed value; css_unit unit;
    if (css_computed_line_height(eff, &value, &unit) != CSS_LINE_HEIGHT_DIMENSION ||
        unit == CSS_UNIT_PX) return CSS_OK;
    css_fixed font = line_font_px(eff, parent_font), scale;
    switch (unit) {
    case CSS_UNIT_PCT:
        value = (css_fixed)(((int64_t)font * value) / (100 * (int64_t)F_1));
        scale = F_1;
        break;
    case CSS_UNIT_EM: scale = font; break;
    case CSS_UNIT_REM:
        scale = parent ? line_font_px(g_unit.root_style, g_root_px) : font;
        break;
    case CSS_UNIT_EX: scale = FMUL(font, FLTTOFIX(0.6)); break;
    case CSS_UNIT_CH: scale = FMUL(font, FLTTOFIX(0.4)); break;
    case CSS_UNIT_LH: {
        /* lh in line-height references the PARENT line, avoiding self
         * recursion. The root uses the initial font/normal-line model. */
        css_fixed pv; css_unit pu;
        css_fixed pf = parent ? line_font_px(parent, parent_font) : g_unit.font_size_default;
        uint8_t kind = parent ? css_computed_line_height(parent, &pv, &pu) : CSS_LINE_HEIGHT_NORMAL;
        scale = kind == CSS_LINE_HEIGHT_NUMBER ? FMUL(pv, pf) :
                kind == CSS_LINE_HEIGHT_DIMENSION && pu == CSS_UNIT_PX ? pv :
                FMUL(pf, FLTTOFIX(1.25));
        break;
    }
    default: scale = px_per_unit(unit, FIXTOINT(font + F_0_5)); break;
    }
    css_error err = css__computed_style_clone(eff, out);
    if (err != CSS_OK) return err;
    err = set_line_height(*out, CSS_LINE_HEIGHT_DIMENSION,
                          FMUL(value, scale), CSS_UNIT_PX);
    if (err == CSS_OK) err = css__arena_intern_style(out);
    if (err != CSS_OK) { css_computed_style_destroy(*out); *out = NULL; }
    return err;
#else
    (void)eff; (void)parent; (void)parent_font;
    return CSS_OK; /* Reproduce raw percent-as-px and child-relative em. */
#endif
}

/* A percentage in HUNDREDTHS of a percent. len_px() rounds a percentage to a
 * whole number, which is right for the properties that had it first (width,
 * height, min/max) and wrong for padding: 56.25% of 400 is 225, and 56% of it
 * is 224. One pixel, on the single most common percentage padding there is. */
static int pct_x100(css_fixed val)
{
    long v = (long)FIXTOINT(FMUL(val, INTTOFIX(100)) + F_0_5);
    if (v > 1000000) v = 1000000;      /* 10000%: past any sane box */
    if (v < -1000000) v = -1000000;
    return (int)v;
}

/* clamp absolutised lengths so a giant CSS value can't overflow the int
 * coordinates the layout engine accumulates (signed overflow = UB). */
static int clamp_px(int v) { if (v > 8192) return 8192; if (v < -8192) return -8192; return v; }

static uint32_t to_rgb(css_color c) { return (uint32_t)(c & 0x00FFFFFF); }

/* CSS Backgrounds 3 sec 5.5: border-radius DOES NOT APPLY to a table or its
 * internal elements when border-collapse is `collapse` -- the two boxes share
 * one edge in the collapsed model, so there is no second border to curve, and
 * every engine drops the radius rather than picking a winner.
 *
 * WHY THIS ASKS THE TAG AND NOT `display`, which is what the spec says: this
 * engine has no table formatting context, and its UA sheet (UA_CSS above) maps
 * `table, tr, td, th, thead, tbody, tfoot` to `display:block`. So the computed
 * display of a <td> here is BLOCK and the spec's condition is not expressible
 * through it. The HTML tag is the only place the same information survives.
 * That is a limitation of the table support, named here rather than left for
 * someone to find as a wrong render, and this function is where it stops being
 * true the day tables get a real display value.
 *
 * border-collapse itself is INHERITED, so the value read off a <td> is its
 * table's, which is what the rule is about -- and a <div> inside a collapsed
 * table inherits `collapse` too, which is exactly why the tag test is needed
 * and not merely convenient: without it that div would lose its own radius.
 *
 * MEASURED: WPT css-backgrounds/ttwf-reftest-borderRadius asserts the radius
 * is ignored here. The engine passed it by accident while it ignored
 * border-radius everywhere; implementing a property is what turns an
 * accidental pass into a real requirement. */
static int is_html_table_box(const struct node *n)
{
    static const char *const tags[] = {
        "table", "tr", "td", "th", "thead", "tbody", "tfoot",
        "col", "colgroup", "caption"
    };
    size_t i;
    if (!n || !n->tag) return 0;
    for (i = 0; i < sizeof tags / sizeof tags[0]; i++)
        if (!strcasecmp(n->tag, tags[i])) return 1;
    return 0;
}

static void convert(const css_computed_style *cs, int parent_font, struct cstyle *o)
{
    css_fixed len; css_unit unit; css_color col;

    /* font-size first (everything else may be em-relative) */
    css_computed_font_size(cs, &len, &unit);
    o->font_px = len_px(len, unit, parent_font, NULL);
    if (o->font_px < 6) o->font_px = 6;
    else if (o->font_px > 512) o->font_px = 512;
    int fp = o->font_px;

    switch (css_computed_display(cs, false)) {
    case CSS_DISPLAY_NONE:         o->display = DISP_NONE; break;
    /* Preserve contents through the real display cascade; treating it as a
     * post-CSS substring override would lose specificity, media and inherit. */
    case CSS_DISPLAY_CONTENTS:     o->display = DISP_CONTENTS; break;
    case CSS_DISPLAY_INLINE:       o->display = DISP_INLINE; break;
    case CSS_DISPLAY_INLINE_BLOCK: o->display = DISP_INLINE_BLOCK; o->outer_inline = 1; break;
    case CSS_DISPLAY_LIST_ITEM:    o->display = DISP_BLOCK; o->list_item = 1; break;
    case CSS_DISPLAY_FLEX:         o->display = DISP_FLEX; break;
    case CSS_DISPLAY_INLINE_FLEX:
        o->display = DISP_FLEX;
#ifndef CSS_NEGCTL_INLINE_FLEX_BLOCK
        /* css_computed_display already blockifies floats and positioned
         * inline-flex boxes. Preserve the inline outer only when it survives
         * that computation; every internal flex consumer still sees FLEX. */
        o->outer_inline = 1;
#endif
        break;
    case CSS_DISPLAY_GRID:         o->display = DISP_GRID; break;
    case CSS_DISPLAY_INLINE_GRID:  o->display = DISP_GRID; o->outer_inline = 1; break;
    default:                       o->display = DISP_BLOCK; break;  /* block + table-ish */
    }

    if (css_computed_color(cs, &col) == CSS_COLOR_COLOR) o->color = to_rgb(col);

    /* Any non-zero alpha counts as "has a background"; the alpha itself is
     * carried through to the display list so the painter can blend it (it does
     * not yet -- rgba(0,0,0,.5) still paints opaque). */
    if (css_computed_background_color(cs, &col) == CSS_BACKGROUND_COLOR_COLOR &&
        (col & 0xFF000000) != 0) {
        o->has_bg = 1; o->background = to_rgb(col);
        o->bg_alpha = (int)((col >> 24) & 0xFF);
    }

    if (css_computed_font_weight(cs) == CSS_FONT_WEIGHT_BOLD) o->bold = 1;
    else {
        /* numeric weights >= 700 are bold too */
        css_fixed w; if (css_computed_font_weight(cs) == CSS_FONT_WEIGHT_700 ||
                         css_computed_font_weight(cs) == CSS_FONT_WEIGHT_800 ||
                         css_computed_font_weight(cs) == CSS_FONT_WEIGHT_900) o->bold = 1;
        (void)w;
    }
    { uint8_t fs = css_computed_font_style(cs);
      if (fs == CSS_FONT_STYLE_ITALIC || fs == CSS_FONT_STYLE_OBLIQUE) o->italic = 1; }
    { lwc_string **fnames = NULL;
      if (css_computed_font_family(cs, &fnames) == CSS_FONT_FAMILY_MONOSPACE) o->mono = 1; }

    /* Preserve the percentage and auto kind; layout owns the containing block
     * width. The previous len_px(..., NULL) path lost the unit here. */
#define MARGIN(NAME, FIELD, EDGE) do { \
    int kind = css_computed_margin_##NAME(cs, &len, &unit); \
    o->FIELD = kind == CSS_MARGIN_AUTO ? 0 : clamp_px(len_px(len, unit, fp, NULL)); \
    if (kind == CSS_MARGIN_AUTO) o->margin_auto |= 1u << EDGE; \
    else if (unit == CSS_UNIT_PCT) { o->margin_pct[EDGE] = len; o->FIELD = 0; } \
} while (0)
    MARGIN(top, mt, 0); MARGIN(right, mr, 1);
    MARGIN(bottom, mb, 2); MARGIN(left, ml, 3);
#undef MARGIN
#ifdef LAYOUT_NEGCTL_MARGIN_KIND
    /* Executable old conversion: 10% becomes 10px, -1px becomes auto. */
    { int *m[4] = { &o->mt, &o->mr, &o->mb, &o->ml };
      for (int e = 0; e < 4; e++) {
          if (o->margin_pct[e]) *m[e] = FIXTOINT(o->margin_pct[e] + F_0_5);
          o->margin_pct[e] = 0;
          if (*m[e] == -1) { o->margin_auto |= 1u << e; *m[e] = 0; }
      } }
#endif

    /* PADDING PERCENTAGES ARE KEPT, and they used to be thrown away right
     * here: all four of these passed NULL for len_px's `pct` out-parameter, so
     * a percentage came back as its own bare number and was stored as PIXELS.
     * `padding-top:56.25%` became fifty-six pixels.
     *
     * That is the worst shape a units bug can take, because the result is
     * plausible: every box still has a size, nothing is zero, nothing
     * overflows, and the page paints. What it breaks is the aspect-ratio box
     * (`height:0; padding-top:56.25%` with an absolutely positioned cover
     * inside), which is how essentially every card grid on the web reserves
     * space for a picture -- so the wrapper collapses to a sliver and the
     * cover, sized to the card, swallows the title beneath it. Found on
     * bilibili, where the video titles are painted at coordinates INSIDE the
     * thumbnail and the area below the image is empty. See
     * tests/unit/layout_box_test.c t_pct_padding.
     *
     * The pixel field keeps its meaning -- RESOLVED -- so none of layout.c's
     * thirty-odd readers change; the specified percentage rides alongside in
     * pt0..pl0 and layout resolves it once, against the containing block's
     * width, at the top of layout_block(). */
    { int pc;
      css_computed_padding_top(cs, &len, &unit);
      o->pt = clamp_px(len_px(len, unit, fp, &pc));    o->pt0 = pc ? pct_x100(len) : 0;
      css_computed_padding_right(cs, &len, &unit);
      o->pr = clamp_px(len_px(len, unit, fp, &pc));    o->pr0 = pc ? pct_x100(len) : 0;
      css_computed_padding_bottom(cs, &len, &unit);
      o->pb = clamp_px(len_px(len, unit, fp, &pc));    o->pb0 = pc ? pct_x100(len) : 0;
      css_computed_padding_left(cs, &len, &unit);
      o->pl = clamp_px(len_px(len, unit, fp, &pc));    o->pl0 = pc ? pct_x100(len) : 0; }

    if (css_computed_width(cs, &len, &unit) == CSS_WIDTH_SET) {
        int pct; o->width = clamp_px(len_px(len, unit, fp, &pct)); o->has_w = 1; o->w_pct = pct;
    } else {
        /* `width` is the one property this LibCSS stores as css_fixed_or_calc,
         * and css_computed_width() reports an unresolved calc() as AUTO. The
         * used-value API does resolve it, but needs the available width, which
         * style time does not have. A calc() is linear in that available width,
         * so two probes recover the model exactly: slope = percentage,
         * intercept = px addend -- which is precisely calc(100% - 20px).
         * (1000 keeps LibCSS's internal `percentage * available` product inside
         * the 22:10 fixed-point range.) */
        int p0, p1;
        if (css_computed_width_px(cs, &g_unit, 0, &p0) == CSS_WIDTH_SET &&
            css_computed_width_px(cs, &g_unit, 1000, &p1) == CSS_WIDTH_SET) {
            int pct = (p1 - p0 + 5) / 10;
            o->has_w = 1;
            if (pct) { o->w_pct = 1; o->width = pct; o->w_off = clamp_px(p0); }
            else       o->width = clamp_px(p0);
        }
    }
    if (css_computed_height(cs, &len, &unit) == CSS_HEIGHT_SET) {
        int pct; o->height = clamp_px(len_px(len, unit, fp, &pct)); o->has_h = 1; o->h_pct = pct;
    } else {
        /* A CALC HEIGHT ARRIVES HERE, NOT ABOVE, and that is the same shape
         * the width block below uses: css_computed_height() reports an
         * unresolved calc() as AUTO on purpose, because what it would
         * otherwise hand back is a calc INDEX that a caller converting it as
         * a length turns into a small nonsense number -- and a small nonsense
         * number is worse than an absent one, since the box still has a size
         * and the page still paints.
         *
         * `height: calc(...)` reached this file as nothing at all until now:
         * upstream LibCSS gives calc() to `width` and to no other length
         * property, and the cascade dropped it before the computed style
         * existed. See the LOCAL PATCH note in
         * third_party/css/libcss/src/select/select_config.py.
         *
         * Probed exactly as width is: a calc() is linear in the available
         * length, so two evaluations recover slope (the percentage) and
         * intercept (the px addend). A calc with no percentage in it -- which
         * is the common case, and the one bilibili's card titles are -- gives
         * slope 0 and is answered correctly with no available height at all. */
        int q0, q1;
        if (css_computed_height_px(cs, &g_unit, 0, &q0) == CSS_HEIGHT_SET &&
            css_computed_height_px(cs, &g_unit, 1000, &q1) == CSS_HEIGHT_SET) {
            int pct = (q1 - q0 + 5) / 10;
            o->has_h = 1;
            if (pct) { o->h_pct = 1; o->height = pct; o->h_off = clamp_px(q0); }
            else       o->height = clamp_px(q0);
        }
    }

    /* min/max sizing. `auto` (the flex-item initial) leaves has_min_* clear, so
     * layout supplies the content-based automatic minimum instead. */
    len = 0; unit = CSS_UNIT_PX;
    if (get_min_width(cs, &len, &unit) == CSS_MIN_WIDTH_SET) {
        int pct; o->min_w = clamp_px(len_px(len, unit, fp, &pct));
        o->has_min_w = 1; o->min_w_pct = pct;
    }
    if (css_computed_max_width(cs, &len, &unit) == CSS_MAX_WIDTH_SET) {
        int pct; o->max_w = clamp_px(len_px(len, unit, fp, &pct));
        o->has_max_w = 1; o->max_w_pct = pct;
    }
    len = 0; unit = CSS_UNIT_PX;
    if (get_min_height(cs, &len, &unit) == CSS_MIN_HEIGHT_SET) {
        int pct; o->min_h = clamp_px(len_px(len, unit, fp, &pct));
        o->has_min_h = 1; o->min_h_pct = pct;
    }
    if (css_computed_max_height(cs, &len, &unit) == CSS_MAX_HEIGHT_SET) {
        int pct; o->max_h = clamp_px(len_px(len, unit, fp, &pct));
        o->has_max_h = 1; o->max_h_pct = pct;
    }

    o->box_sizing = css_computed_box_sizing(cs) == CSS_BOX_SIZING_BORDER_BOX
                    ? BOX_BORDER : BOX_CONTENT;

    switch (css_computed_white_space(cs)) {
    case CSS_WHITE_SPACE_PRE:      o->white_space = WS_PRE; break;
    case CSS_WHITE_SPACE_NOWRAP:   o->white_space = WS_NOWRAP; break;
    case CSS_WHITE_SPACE_PRE_WRAP: o->white_space = WS_PRE_WRAP; break;
    case CSS_WHITE_SPACE_PRE_LINE: o->white_space = WS_PRE_LINE; break;
    default:                       o->white_space = WS_NORMAL; break;
    }

    /* ---- inline direction, and the text properties ----
     *
     * The values stored are layout_text.h's own LTX_*, so the text engine
     * consumes them by assignment; see the note above `enum { DIR_LTR ... }`
     * in css.h for why a second numbering here would be a liability rather
     * than an abstraction.
     *
     * WHAT LibCSS CAN AND CANNOT PRODUCE, because the difference is not
     * discoverable from the field list: direction, writing-mode, text-indent,
     * letter-spacing, word-spacing, text-transform and white-space are in its
     * property table; tab-size, word-break, overflow-wrap, line-break,
     * hyphens, text-align-last, text-justify, white-space-collapse and
     * text-wrap are NOT IN IT AT ALL, so they cannot be read here by any
     * means. Those keep their CSS initial value -- which is the right answer
     * for every page that does not set them -- and closing them needs either
     * css_extra.c's raw-declaration pass or the properties added to the
     * vendored parser, which is the CSS line's file. Saying so here beats
     * leaving a reader to conclude the field was forgotten. */
    o->direction = (css_computed_direction(cs) == CSS_DIRECTION_RTL)
                   ? DIR_RTL : DIR_LTR;
    switch (css_computed_writing_mode(cs)) {
    case CSS_WRITING_MODE_VERTICAL_RL: o->writing_mode = WM_VERT_RL; break;
    case CSS_WRITING_MODE_VERTICAL_LR: o->writing_mode = WM_VERT_LR; break;
    default:                           o->writing_mode = WM_HORIZ_TB; break;
    }

    if (css_computed_text_indent(cs, &len, &unit) == CSS_TEXT_INDENT_SET) {
        int pct;
        o->text_indent = clamp_px(len_px(len, unit, fp, &pct));
        o->ti_pct = (unsigned char)(pct ? 1 : 0);
    }
    /* `normal` is zero here, not a sentinel: a spacing of exactly 0 and
     * `normal` are the same used value for both properties, so there is
     * nothing for a sentinel to distinguish. */
    if (css_computed_letter_spacing(cs, &len, &unit) == CSS_LETTER_SPACING_SET)
        o->letter_spacing = clamp_px(len_px(len, unit, fp, NULL));
    if (css_computed_word_spacing(cs, &len, &unit) == CSS_WORD_SPACING_SET)
        o->word_spacing = clamp_px(len_px(len, unit, fp, NULL));

    switch (css_computed_text_transform(cs)) {
    case CSS_TEXT_TRANSFORM_CAPITALIZE: o->text_transform = LTX_TT_CAPITALIZE; break;
    case CSS_TEXT_TRANSFORM_UPPERCASE:  o->text_transform = LTX_TT_UPPERCASE; break;
    case CSS_TEXT_TRANSFORM_LOWERCASE:  o->text_transform = LTX_TT_LOWERCASE; break;
    default:                            o->text_transform = LTX_TT_NONE; break;
    }

    /* white-space is ONE property in LibCSS and TWO longhands in the text
     * engine. Deriving both from it is exact -- css-text-4 defines the
     * shorthand's five values as precisely these pairs -- and it is the only
     * way to have the longhands at all until the parser learns them. */
    switch (o->white_space) {
    case WS_PRE:      o->wsc = LTX_WSC_PRESERVE;        o->text_wrap = LTX_WRAP_NOWRAP; break;
    case WS_NOWRAP:   o->wsc = LTX_WSC_COLLAPSE;        o->text_wrap = LTX_WRAP_NOWRAP; break;
    case WS_PRE_WRAP: o->wsc = LTX_WSC_PRESERVE;        o->text_wrap = LTX_WRAP_WRAP; break;
    case WS_PRE_LINE: o->wsc = LTX_WSC_PRESERVE_BREAKS; o->text_wrap = LTX_WRAP_WRAP; break;
    default:          o->wsc = LTX_WSC_COLLAPSE;        o->text_wrap = LTX_WRAP_WRAP; break;
    }

    /* The initial values of the nine LibCSS knows nothing about. Written
     * explicitly rather than left to the caller's zeroing: LTX_HY_NONE is 0
     * but hyphens' initial value is `manual`, and tab-size's is 8, so two of
     * these are NOT the zero the struct arrives with. */
    o->word_break      = LTX_WB_NORMAL;
    o->overflow_wrap   = LTX_OW_NORMAL;
    o->line_break      = LTX_LB_AUTO;
    o->hyphens         = LTX_HY_MANUAL;
    o->text_align_last = LTX_ALAST_AUTO;
    o->text_justify    = LTX_TJ_AUTO;
    o->tab_size        = 8;
    o->tab_px          = 0;

    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER:
    case CSS_TEXT_ALIGN_LIBCSS_CENTER: o->text_align = ALIGN_CENTER; break;
    case CSS_TEXT_ALIGN_RIGHT:
    case CSS_TEXT_ALIGN_LIBCSS_RIGHT:  o->text_align = ALIGN_RIGHT; break;
    case CSS_TEXT_ALIGN_JUSTIFY:       o->text_align = ALIGN_JUSTIFY; break;
    default:                           o->text_align = ALIGN_LEFT; break;
    }

    switch (css_computed_line_height(cs, &len, &unit)) {
    case CSS_LINE_HEIGHT_NUMBER:
        o->line_px = clamp_px(FIXTOINT(FMUL(len, line_font_px(cs, parent_font)) + F_0_5));
        o->has_line_px = 1;
        break;
    case CSS_LINE_HEIGHT_DIMENSION:
        /* normalize_line_height has already made inherited dimensions PX.
         * The negative control skips it to retain the old 130% -> 130px bug. */
        o->line_px = clamp_px(len_px(len, unit, fp, NULL));
        o->has_line_px = 1;
        break;
    default: o->line_px = 0; o->has_line_px = 0; break; /* normal -> layout derives */
    }

    /* borders: full per-edge model (top/right/bottom/left). `hidden` is a
     * border-conflict-resolution value for tables and paints as nothing, so it
     * joins `none` in taking no space here. */
#define EDGE_CONVERT(i, NAME) \
    { uint8_t bs = css_computed_border_##NAME##_style(cs); \
      o->border_style[i] = bs; \
      if (bs != CSS_BORDER_STYLE_NONE && bs != CSS_BORDER_STYLE_HIDDEN) { \
        css_computed_border_##NAME##_width(cs, &len, &unit); \
        o->border_w[i] = clamp_px(len_px(len, unit, fp, NULL)); \
        if (css_computed_border_##NAME##_color(cs, &col) == CSS_BORDER_COLOR_COLOR) { \
            o->border_color[i] = to_rgb(col); \
            /* A FULLY TRANSPARENT border still takes its space and paints
             * NOTHING. to_rgb drops the alpha byte, so `border-color:
             * transparent` used to arrive as opaque BLACK with its width
             * intact -- every quiet button in a modern design system
             * (Wikipedia's Codex writes
             * `border-color:var(--border-color-transparent,transparent)`)
             * came out ringed in black. `hidden` is the value this file
             * already documents as "paints as nothing", and setting it HERE,
             * after the width, keeps the box model intact. Partial alpha
             * still paints opaque -- the same honest limitation the
             * background-color path states a few lines down. */ \
            if ((col & 0xFF000000) == 0) o->border_style[i] = CSS_BORDER_STYLE_HIDDEN; \
        } \
        else o->border_color[i] = 0x808080; \
      } }
    EDGE_CONVERT(0, top)
    EDGE_CONVERT(1, right)
    EDGE_CONVERT(2, bottom)
    EDGE_CONVERT(3, left)
#undef EDGE_CONVERT

    /* border-radius, one corner at a time, TL TR BR BL.
     *
     * THE PAINTER DRAWS A CIRCULAR ARC and CSS's corner is an ELLIPSE
     * (`border-bottom-left-radius: 48px 28px`, or a percentage, which resolves
     * horizontally against the border box's WIDTH and vertically against its
     * HEIGHT). One number has to come out of two, and the rule is THE SMALLER
     * OF THE PAIR, for two reasons that are not aesthetic:
     *
     *   - CSS says a corner with EITHER radius zero is SQUARE. `25px 0` is a
     *     sharp corner, and taking the horizontal radius rounds a corner the
     *     author explicitly did not round -- ink where none was asked for,
     *     which is the failure direction this tree refuses. min() gets it
     *     right by construction rather than by a special case.
     *     (Measured: WPT css-backgrounds/border-bottom-left-radius-010 is
     *     exactly this test, and it went red with the horizontal rule.)
     *   - Under-curving withholds ink at a corner; over-curving removes ink
     *     from a corner the author filled. The first is a smaller lie.
     *
     * A percentage stays a percentage: browser_paint.c resolves it against
     * min(w,h), which is min(pct*w, pct*h) -- i.e. the same min() rule, in the
     * one place that knows the box. Mixing a length with a percentage in one
     * pair cannot be compared here (the box is not known yet) and is
     * vanishingly rare; the LENGTH wins, because it is exact.
     *
     * NOTHING IS SILENTLY DISCARDED: both halves of the pair are read, and
     * the reduction happens once, here, where it can be argued. */
#define RADIUS_CONVERT(i, NAME) \
    { css_fixed vlen = 0; css_unit vunit = CSS_UNIT_PX; \
      int hp = 0, vp = 0; \
      if (css_computed_border_##NAME##_radius(cs, &len, &unit, &vlen, &vunit) \
              == CSS_BORDER_RADIUS_SET) { \
        int hv = len_px(len, unit, fp, &hp); \
        int vv = len_px(vlen, vunit, fp, &vp); \
        int is_pct = hp && vp; \
        int v; \
        if (hp == vp) v = hv < vv ? hv : vv;   /* same kind: the smaller */ \
        else          v = hp ? vv : hv;        /* mixed: the LENGTH */ \
        if (v < 0) v = 0; \
        if (is_pct) { \
            if (v > 50) v = 50; \
            o->radius_pct[i] = v; o->radius[i] = 0; \
        } else { \
            o->radius[i] = clamp_px(v); o->radius_pct[i] = 0; \
        } \
      } }
    RADIUS_CONVERT(0, top_left)
    RADIUS_CONVERT(1, top_right)
    RADIUS_CONVERT(2, bottom_right)
    RADIUS_CONVERT(3, bottom_left)
#undef RADIUS_CONVERT


    { uint8_t td = css_computed_text_decoration(cs);
      if (td & CSS_TEXT_DECORATION_UNDERLINE)    o->underline = 1;
      if (td & CSS_TEXT_DECORATION_LINE_THROUGH) o->strike = 1;
      if (td & CSS_TEXT_DECORATION_OVERLINE)     o->overline = 1; }

    o->pointer_events_none = css_computed_pointer_events(cs) == CSS_POINTER_EVENTS_NONE;
#ifdef CSS_POINTER_ALL_HIT_LEGACY
    /* Required negative control: keep parsing/computed all present but restore
     * the non-clickable box failure. A capability-only test must not pass as
     * proof of real native targeting. The immutable pre-fix disk separately
     * proves the original parser-rejection path, without this seam. */
    o->pointer_events_none |= css_computed_pointer_events(cs) == CSS_POINTER_EVENTS_ALL;
#endif
    { uint8_t v = css_computed_visibility(cs);
      if (v == CSS_VISIBILITY_HIDDEN || v == CSS_VISIBILITY_COLLAPSE) { o->hidden = 1; o->vis_hid = 1; } }
    { css_fixed op;
      o->opacity = 255;o->opacity_context=0;
      if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET) {
          o->opacity_context=op<F_1;
          if (op <= 0) { o->hidden = 1; o->op0 = 1; o->opacity = 0; }
          else if (op < F_1) o->opacity = FIXTOINT(FMUL(op, INTTOFIX(255)) + F_0_5);
      } }
    { uint8_t p = css_computed_position(cs);
      switch (p) {
      /* absolute AND fixed: out of flow. Until 2026-08-30 fixed stayed IN
       * flow -- pos_abs was set only for CSS_POSITION_ABSOLUTE -- on the
       * argument that "fixed headers sit at the top anyway, and a
       * viewport-anchored box would need the painter to exempt it from
       * scrolling". That argument covers the 60px sticky-header case and
       * says nothing about the far more common one: a full-viewport
       * `position:fixed` overlay (a modal backdrop, a drawer, a nav that is
       * hidden until opened) declared before the real content. In flow, a
       * 100vh box costs a screen of layout height whether or not it paints,
       * and pushes everything after it below the fold -- measured landing
       * on a real page with no site-specific fix: header y=0, a hidden
       * 100vh fixed overlay y=20 IN FLOW, main content shoved to y=660 in a
       * 640px viewport.
       *
       * Painting is unaffected by this: `pos_abs` only takes the box out of
       * NORMAL FLOW LAYOUT and anchors it at the nearest positioned
       * ancestor's padding box (or the initial containing block, i.e. the
       * viewport, when there is none) -- see layout_abspos_child() in
       * layout.c. It does not make fixed track scroll; this engine never
       * scrolled a fixed box correctly either way (it used to scroll WITH
       * the page as an ordinary flow box, which is also wrong), so nothing
       * that worked before regresses, and the catastrophic case -- a
       * full-viewport box eating the first screen -- is closed. A real
       * `position:fixed` also ignores any `position:relative` ancestor and
       * anchors to the viewport specifically; this engine's out-of-flow
       * anchor is "nearest positioned ancestor" for both absolute and
       * fixed, so a fixed box inside a `position:relative` wrapper anchors
       * to that wrapper rather than skipping past it to the viewport. Left
       * as a known simplification rather than a new mechanism, because the
       * failure mode is "off by the wrapper's offset", not "gone".
       *
       * Correction (2026-09-09): fixed now anchors to the viewport in layout,
       * and paint/native hit/CSSOM share css_viewport_fixed_owner to ignore page
       * scroll. A transform-established fixed containing block remains absent.
       *
       * sticky is laid out as relative, which is what it is until the
       * scroll offset reaches it. */
      case CSS_POSITION_ABSOLUTE: o->position = POS_ABSOLUTE; o->pos_abs = 1; break;
      case CSS_POSITION_RELATIVE: o->position = POS_RELATIVE; break;
      case CSS_POSITION_FIXED:    o->position = POS_FIXED;
#ifndef LAYOUT_NEGCTL_FIXED_INFLOW
                                   o->pos_abs = 1;
#endif
                                   /* LAYOUT_NEGCTL_FIXED_INFLOW, kept
                                    * compilable as a control: reproduces the
                                    * pre-2026-08-30 behaviour (fixed stays IN
                                    * FLOW) so tests/qmp's zwprobe/f.html can
                                    * be watched pushing REAL-CONTENT-1/2 down
                                    * by a full viewport height again. */
                                   break;
      case CSS_POSITION_STICKY:   o->position = POS_STICKY; break;
      default:                    o->position = POS_STATIC; break;
      } }
    /* Formerly percentage offsets were dropped because cstyle had no deferred
     * unit. In the guest that turned a fixed top:20% search box into top:auto
     * at y=0, under a z-index:4 navigation row. Keep the specified fraction;
     * only layout knows whether its basis is viewport or positioned padding.
     * LibCSS's length-auto cascade still skips calc inset declarations. This
     * bridge does NOT synthesize a slope from an absent computed value.
     * Read the raw composed inset here: LibCSS's public relative-position
     * getters synthesize the opposite edge but get_*_bits returns type only,
     * losing a percentage unit and turning top:25% into bottom:-25px. Layout
     * now resolves relative auto/opposite edges with the real CB basis. */
#define INSET_VALUE(NAME, EDGE, SET) do { \
    css_fixed v=0; css_unit u=CSS_UNIT_PX; int pc; \
    if(o->position!=POS_STATIC && get_##NAME(cs,&v,&u)==SET) { \
        int q=len_px(v,u,fp,&pc); \
        if(!pc) {o->NAME=clamp_px(q);o->has_##NAME=1;} \
        else { \
            INSET_PERCENT(NAME,EDGE,v); \
        } \
    } \
} while(0)
#ifdef LAYOUT_INSET_PERCENT_LEGACY
#define INSET_PERCENT(NAME,EDGE,V) ((void)0)
#else
#define INSET_PERCENT(NAME,EDGE,V) do {o->NAME=0;o->has_##NAME=1; \
    o->inset_pct[EDGE]=(int)(V);o->inset_pct_mask|=1u<<(EDGE);} while(0)
#endif
    INSET_VALUE(top,0,CSS_TOP_SET); INSET_VALUE(right,1,CSS_RIGHT_SET);
    INSET_VALUE(bottom,2,CSS_BOTTOM_SET); INSET_VALUE(left,3,CSS_LEFT_SET);
#undef INSET_PERCENT
#undef INSET_VALUE

    { int32_t z;
      /* z-index is stored as a raw css_fixed by css__cascade_z_index (unlike
       * `order`, which the cascade already FIXTOINTs), so `z-index:5` arrives
       * as 5120. */
      if (css_computed_z_index(cs, &z) == CSS_Z_INDEX_SET) {
          int v = FIXTOINT((css_fixed)z);
          o->has_z = 1;
          o->z_index = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
      } }

    switch (css_computed_float(cs)) {
    case CSS_FLOAT_LEFT:  o->flt = FLT_LEFT; break;
    case CSS_FLOAT_RIGHT: o->flt = FLT_RIGHT; break;
    default:              o->flt = FLT_NONE; break;
    }
    switch (css_computed_clear(cs)) {
    case CSS_CLEAR_LEFT:  o->clr = CLR_LEFT; break;
    case CSS_CLEAR_RIGHT: o->clr = CLR_RIGHT; break;
    case CSS_CLEAR_BOTH:  o->clr = CLR_BOTH; break;
    default:              o->clr = CLR_NONE; break;
    }
#define OVF_CONVERT(v) ((v) == CSS_OVERFLOW_HIDDEN ? OVF_HIDDEN : \
                        (v) == CSS_OVERFLOW_SCROLL ? OVF_SCROLL : \
                        (v) == CSS_OVERFLOW_AUTO   ? OVF_AUTO : OVF_VISIBLE)
    o->overflow_x = OVF_CONVERT(css_computed_overflow_x(cs));
    o->overflow_y = OVF_CONVERT(css_computed_overflow_y(cs));
#undef OVF_CONVERT

    /* ---- flexbox ---- */
    switch (css_computed_flex_direction(cs)) {
    case CSS_FLEX_DIRECTION_ROW_REVERSE:    o->flex_dir = FDIR_ROW_REV; break;
    case CSS_FLEX_DIRECTION_COLUMN:         o->flex_dir = FDIR_COL; break;
    case CSS_FLEX_DIRECTION_COLUMN_REVERSE: o->flex_dir = FDIR_COL_REV; break;
    default:                                o->flex_dir = FDIR_ROW; break;
    }
    switch (css_computed_flex_wrap(cs)) {
    case CSS_FLEX_WRAP_WRAP:         o->flex_wrap = FWRAP_WRAP; break;
    case CSS_FLEX_WRAP_WRAP_REVERSE: o->flex_wrap = FWRAP_WRAP_REV; break;
    default:                         o->flex_wrap = FWRAP_NOWRAP; break;
    }
    switch (css_computed_justify_content(cs)) {
    case CSS_JUSTIFY_CONTENT_FLEX_END:      o->justify = JC_END; break;
    case CSS_JUSTIFY_CONTENT_CENTER:        o->justify = JC_CENTER; break;
    case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: o->justify = JC_BETWEEN; break;
    case CSS_JUSTIFY_CONTENT_SPACE_AROUND:  o->justify = JC_AROUND; break;
    case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:  o->justify = JC_EVENLY; break;
    default:                                o->justify = JC_START; break;
    }
#define ALIGN_CONVERT(v) ((v) == CSS_ALIGN_ITEMS_FLEX_START ? AL_START : \
                          (v) == CSS_ALIGN_ITEMS_FLEX_END   ? AL_END : \
                          (v) == CSS_ALIGN_ITEMS_CENTER     ? AL_CENTER : \
                          (v) == CSS_ALIGN_ITEMS_BASELINE   ? AL_BASELINE : AL_STRETCH)
    o->align_items = ALIGN_CONVERT(css_computed_align_items(cs));
    { uint8_t a = css_computed_align_self(cs);
      o->align_self = (a == CSS_ALIGN_SELF_AUTO) ? AL_AUTO : ALIGN_CONVERT(a); }
    switch (css_computed_align_content(cs)) {
    case CSS_ALIGN_CONTENT_FLEX_START:    o->align_content = AL_START; break;
    case CSS_ALIGN_CONTENT_FLEX_END:      o->align_content = AL_END; break;
    case CSS_ALIGN_CONTENT_CENTER:        o->align_content = AL_CENTER; break;
    /* The three space-* values are DISTINCT now.
     *
     * They used to fold onto stretch, on the argument that they only differ
     * when the container has spare cross space and our auto-height containers
     * never do. That argument was true of the flex containers this engine
     * could build when it was written and is not true of the ones it builds
     * now: a container with an explicit height, or a grid, has spare cross
     * space routinely -- and folding three values onto a fourth is invisible
     * in the style and shows up as a layout that is merely wrong. The cost of
     * carrying them is one byte per element that layout may still choose to
     * treat as stretch; the cost of NOT carrying them is that layout cannot
     * choose at all. */
    case CSS_ALIGN_CONTENT_SPACE_BETWEEN: o->align_content = AL_BETWEEN; break;
    case CSS_ALIGN_CONTENT_SPACE_AROUND:  o->align_content = AL_AROUND; break;
    case CSS_ALIGN_CONTENT_SPACE_EVENLY:  o->align_content = AL_EVENLY; break;
    default:                              o->align_content = AL_STRETCH; break;
    }
#undef ALIGN_CONVERT
    { css_fixed fg = 0;
      if (css_computed_flex_grow(cs, &fg) == CSS_FLEX_GROW_SET && fg > 0)
          o->flex_grow = fg; }
    { css_fixed fs = F_1;               /* flex-shrink's initial value is 1 */
      if (css_computed_flex_shrink(cs, &fs) != CSS_FLEX_SHRINK_SET) fs = F_1;
      o->flex_shrink = fs < 0 ? 0 : fs; }
    { uint8_t fbt = css_computed_flex_basis(cs, &len, &unit);
      if (fbt == CSS_FLEX_BASIS_SET) {
        int pct; o->flex_basis = clamp_px(len_px(len, unit, fp, &pct));
        o->has_fb = 1; o->fb_pct = pct;
        /* fb_off IS ZERO, AND THAT IS NOT AN OVERSIGHT ANY MORE.
         *
         * layout.c reads it, so a reader is entitled to know why it never
         * moves. `width` gets a px addend out of calc() through a two-probe
         * trick against css_computed_width_px() -- a calc() is linear in the
         * available width, so two evaluations recover slope and intercept
         * exactly. That accessor exists for `width` and for NOTHING ELSE:
         * LibCSS stores only width as css_fixed_or_calc, so a calc() on
         * flex-basis is not partially lost here, it is rejected by the parser
         * before this function ever sees it, and flex-basis stays `auto`.
         *
         * So this cannot be closed from our side. It needs
         * css_computed_flex_basis_px() (and min/max-width equivalents) in the
         * vendored parser -- third_party/css, which is another line's file.
         * Writing an addend here from anything else would be inventing a
         * number. Same story for min_w_pct/max_w_pct, which have no _off field
         * for exactly this reason. */
        o->fb_off = 0;
      } else if (fbt == CSS_FLEX_BASIS_CONTENT) {
        /* `content` is not `auto`: auto defers to the item's width property,
         * content ignores it and sizes to the content regardless. Folding
         * them together silently gives a width-bearing item the wrong basis. */
        o->has_fb = 0; o->fb_content = 1;
      }
    }
    { int32_t ord;
      if (css_computed_order(cs, &ord) == CSS_ORDER_SET)
          o->order = ord > 32767 ? 32767 : (ord < -32768 ? -32768 : (int)ord); }

    switch (css_computed_list_style_type(cs)) {
    case CSS_LIST_STYLE_TYPE_NONE:                 o->list_style = LST_NONE; break;
    case CSS_LIST_STYLE_TYPE_CIRCLE:               o->list_style = LST_CIRCLE; break;
    case CSS_LIST_STYLE_TYPE_SQUARE:               o->list_style = LST_SQUARE; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL:              o->list_style = LST_DECIMAL; break;
    case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO: o->list_style = LST_DECIMAL_ZERO; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ALPHA:
    case CSS_LIST_STYLE_TYPE_LOWER_LATIN:          o->list_style = LST_LOWER_ALPHA; break;
    case CSS_LIST_STYLE_TYPE_UPPER_ALPHA:
    case CSS_LIST_STYLE_TYPE_UPPER_LATIN:          o->list_style = LST_UPPER_ALPHA; break;
    case CSS_LIST_STYLE_TYPE_LOWER_ROMAN:          o->list_style = LST_LOWER_ROMAN; break;
    case CSS_LIST_STYLE_TYPE_UPPER_ROMAN:          o->list_style = LST_UPPER_ROMAN; break;
    case CSS_LIST_STYLE_TYPE_DISC:                 o->list_style = LST_DISC; break;
    /* Everything else LibCSS knows is a numeric alphabet we have no glyphs
     * for (armenian, hebrew, cjk-*, the Indic families). Numbering the list in
     * the wrong alphabet beats a row of missing-glyph boxes. */
    default:                                       o->list_style = LST_DECIMAL; break;
    }
}

/* ======================================================================
 * CSSOM readback: computed style -> CSS text
 *
 * `convert()` above digests a computed style into the ~45 fields LAYOUT needs.
 * This half answers a different question -- "what would getComputedStyle say?"
 * -- and so it reads node->computed (the real LibCSS block, ~110 properties)
 * and serialises single properties back into CSS syntax. The two must not be
 * merged: cstyle is lossy on purpose (font-family names, numeric font weights,
 * per-edge border styles as keywords, `auto` vs `0`), and it is exactly those
 * distinctions a script asking for a computed value is asking about.
 *
 * See css.h for which of these are resolved values and which are computed
 * values -- they differ for width/height/margin/padding/offsets, and the
 * difference is visible if you diff against Chrome.
 * ====================================================================== */

/* Dashed names, indexed by CSSP_*. Order must match the enum in css.h. */
static const char *const g_prop_names[CSSP__COUNT] = {
    "width", "height",
    "min-width", "max-width", "min-height", "max-height",
    "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding-top", "padding-right", "padding-bottom", "padding-left",
    "color", "background-color",
    "font-size", "font-family", "font-weight", "font-style",
    "display", "position",
    "top", "right", "bottom", "left",
    "opacity", "z-index", "visibility",
    "overflow", "overflow-x", "overflow-y",
    "flex-direction", "flex-wrap", "flex-grow", "flex-shrink",
    "flex-basis", "justify-content", "align-items", "align-self",
    "align-content", "order",
    "border-top-width", "border-right-width",
    "border-bottom-width", "border-left-width",
    "border-top-style", "border-right-style",
    "border-bottom-style", "border-left-style",
    "border-top-color", "border-right-color",
    "border-bottom-color", "border-left-color",
    "text-align", "line-height", "text-decoration",
    "box-sizing", "white-space", "float", "clear",
    "list-style-type",
    "pointer-events",
    "backface-visibility", "transform-style",
};

const char *css_prop_name(int i)
{ return (i >= 0 && i < CSSP__COUNT) ? g_prop_names[i] : 0; }

/* Accepts both spellings a script can hand us. The IDL name is the dashed name
 * with each "-x" folded to "X", so one comparison covers both: walk the dashed
 * name and let the caller's string either supply the '-' or the capital.
 * `cssFloat` is spelled out separately -- `float` was a reserved word when the
 * IDL was written, and pages still use both. */
/* ---- which property names does this engine turn away? ----
 *
 * A histogram behind an env var, and it exists because the STATIC way of
 * asking this question is wrong in a way that is easy to miss. Attributing a
 * failing subtest to a property by reading its TITLE credited 1,398 gradient
 * subtests to `background-image`, because the words appear in their names --
 * an overcount of 4x on the property that looked like the biggest single win.
 * Dropping the loose match left 11,928 of 12,563 rows unattributable, which is
 * not an answer either.
 *
 * The name is right here, at the one place an unresolvable property is turned
 * away, so ask here. LOGIT_CSS_PROP_MISS=<path> writes "<count>\t<name>" per
 * line at exit, and that is a work order rather than an inference.
 *
 * Off unless the variable is set: no allocation, no table, one predictable
 * branch. It is a measurement seam, not a feature, and it is in this file
 * rather than the runner because the runner belongs to the WPT line. */
/* One APPEND per miss, not a table dumped at exit, and the reason is the
 * runner's shape: tests/unit/wpt_test.c runs one file per PROCESS, so a
 * histogram accumulated in memory dies with each child and an atexit dump
 * either never runs or has every child overwrite the same path. Appending a
 * line and aggregating with sort|uniq -c afterwards is the version that
 * survives fork, _exit, and a crashed file. */
static int g_pmiss_on = -1;

static void pmiss_note(const char *name, int len)
{
    static const char *path;
    if (g_pmiss_on < 0) {
        path = getenv("LOGIT_CSS_PROP_MISS");
        g_pmiss_on = path ? 1 : 0;
    }
    if (!g_pmiss_on || len <= 0 || len > 64) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fwrite(name, 1, (size_t)len, f);
    fputc('\n', f);
    fclose(f);
}

/* The name behind the last CSSP_CUSTOM answer.
 *
 * A sentinel index cannot carry a name, and cssd_read() hands css_computed_text
 * only the index. The two calls are adjacent and nothing runs between them --
 * `prop = css_prop_lookup(name); ... css_computed_text(n, prop, ...)` -- so the
 * name is passed in a static rather than by widening an ABI that four other
 * callers share. It is a narrow coupling and it is written down here rather
 * than left to be rediscovered: css_prop_lookup is the ONLY writer, and
 * css_computed_text is the only reader, and it reads it only for CSSP_CUSTOM. */
static char g_last_custom[128];

const char *css_prop_last_custom(void) { return g_last_custom; }

int css_prop_lookup(const char *name, int len)
{
    if (!name) return -1;
    {
        int l = len;
        if (l < 0) { l = 0; while (name[l]) l++; }
        /* `--` alone is not a custom property; `--x` is. */
        if (l > 2 && name[0] == '-' && name[1] == '-') {
            if (l >= (int)sizeof g_last_custom) return -1;
            memcpy(g_last_custom, name, (size_t)l);
            g_last_custom[l] = 0;
            return CSSP_CUSTOM;
        }
    }
    if (len < 0) { len = 0; while (name[len]) len++; }
    if (len <= 0) return -1;
    if (len == 8 && memcmp(name, "cssFloat", 8) == 0) return CSSP_FLOAT;
    for (int p = 0; p < CSSP__COUNT; p++) {
        const char *d = g_prop_names[p];
        int i = 0, j = 0, ok = 1;
        while (d[i]) {
            if (j >= len) { ok = 0; break; }
            if (d[i] == '-') {
                if (name[j] == '-') { i++; j++; continue; }        /* dashed spelling */
                if (name[j] == (char)(d[i + 1] - 32)) { i += 2; j++; continue; }  /* camelCase */
                ok = 0; break;
            }
            /* CSS property names are ASCII case-insensitive, so
             * getPropertyValue("BACKGROUND-COLOR") has to resolve; only the
             * capital that STANDS IN for a dash is matched exactly. */
            if (lc((unsigned char)name[j]) != d[i]) { ok = 0; break; }
            i++; j++;
        }
        if (ok && j == len) return p;
    }
    pmiss_note(name, len);
    return -1;
}

int css_prop_paint_only(int prop)
{
    switch (prop) {
    /* No box moves and no box changes size when these change -- that is the
     * definition the invalidation tier uses, and it is the one a future
     * incremental repaint would key off. (border-*-style is here because
     * `none` -> `solid` also changes border-*-width, which is NOT in this
     * list, so a style change that does take space is still caught.) */
    case CSSP_COLOR: case CSSP_BACKGROUND_COLOR:
    case CSSP_BORDER_TOP_COLOR: case CSSP_BORDER_RIGHT_COLOR:
    case CSSP_BORDER_BOTTOM_COLOR: case CSSP_BORDER_LEFT_COLOR:
    case CSSP_BORDER_TOP_STYLE: case CSSP_BORDER_RIGHT_STYLE:
    case CSSP_BORDER_BOTTOM_STYLE: case CSSP_BORDER_LEFT_STYLE:
    case CSSP_OPACITY: case CSSP_VISIBILITY: case CSSP_Z_INDEX:
    case CSSP_BACKFACE_VISIBILITY: case CSSP_TRANSFORM_STYLE:
    case CSSP_TEXT_DECORATION:
        return 1;
    default:
        return 0;
    }
}

/* ---- a bounded output buffer (no stdio: css_engine.c is freestanding) ---- */
struct obuf { char *p; int n, max; };
static void ob_ch(struct obuf *b, char c) { if (b->n < b->max - 1) b->p[b->n++] = c; }
static void ob_s(struct obuf *b, const char *s) { while (s && *s) ob_ch(b, *s++); }
static void ob_sn(struct obuf *b, const char *s, int n) { for (int i = 0; i < n; i++) ob_ch(b, s[i]); }
static void ob_i(struct obuf *b, int v)
{
    char t[12]; int k = 0;
    if (v < 0) { ob_ch(b, '-'); v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k) ob_ch(b, t[--k]);
}

/* A css_fixed as a plain decimal, trailing zeros trimmed: 1, 0.5, 0.33.
 * Done in integers because css_fixed IS an integer (22:10) and the browser's
 * CSS path must not depend on the FPU being enabled. */
static void ob_fixed(struct obuf *b, css_fixed v)
{
    if (v < 0) { ob_ch(b, '-'); v = -v; }
    ob_i(b, (int)(v >> CSS_RADIX_POINT));
    int frac = (int)(((long long)(v & ((1 << CSS_RADIX_POINT) - 1)) * 10000 + 512) / 1024);
    if (frac > 9999) frac = 9999;
    if (frac) {
        char d[4];
        for (int i = 3; i >= 0; i--) { d[i] = (char)('0' + frac % 10); frac /= 10; }
        int last = 3;
        while (last > 0 && d[last] == '0') last--;
        ob_ch(b, '.');
        for (int i = 0; i <= last; i++) ob_ch(b, d[i]);
    }
}

/* The alpha channel of a COMPUTED colour, which reaches us as one byte.
 *
 * `a/255` printed straight out is wrong, and wrong in a way that looks right:
 * an authored `rgba(10, 20, 30, 0.5)` came back as `rgba(10, 20, 30, 0.497)`.
 * Two errors compounded -- LibCSS quantises the author's alpha to 8 bits on
 * the way in, and ob_fixed then truncated the 22:10 division on the way out.
 *
 * Browsers print the SHORTEST decimal that round-trips through the byte, which
 * is the only rule that can give `0.5` back for an authored 0.5 while still
 * giving `0.498` for a hex `#...7f` that genuinely is not a half. So: try one,
 * two, then three decimal places and take the first whose value maps back onto
 * the same byte. The round-trip is accepted under EITHER truncation or
 * rounding because 0.5*255 is exactly 127.5 -- the one value where the two
 * disagree, and the one value that matters most.
 *
 * This does not recover information LibCSS threw away; it stops adding a
 * second error on top of it. The quantisation itself is inside the vendored
 * parser and belongs to the CSS-value line, not this one.
 *
 * (The SPECIFIED-value serialiser has no such problem and must not be changed
 * to match: vsb_alphav carries alpha in thousandths from wherever it was read,
 * precisely so `rgba(5, 7, 10, 0.5)` and `#0000007f` can print differently.) */
static void ob_alpha_byte(struct obuf *b, unsigned a)
{
    if (a >= 255) { ob_ch(b, '1'); return; }
    if (a == 0)   { ob_ch(b, '0'); return; }
    int scale = 10;
    for (int digits = 1; digits <= 3; digits++, scale *= 10) {
        /* the nearest `digits`-place decimal to a/255 */
        int q = (int)(((long)a * scale * 2 + 255) / (255 * 2));
        long back = (long)q * 255;
        if ((int)(back / scale) == (int)a ||                  /* truncation */
            (int)((back + scale / 2) / scale) == (int)a) {    /* rounding   */
            ob_ch(b, '0'); ob_ch(b, '.');
            for (int d = scale / 10; d > 0; d /= 10) ob_ch(b, (char)('0' + (q / d) % 10));
            return;
        }
    }
    /* Unreachable for a byte -- three places always round-trip 0..255 -- but a
     * serialiser that can emit nothing is worse than one that emits a bound. */
    ob_ch(b, '0'); ob_ch(b, '.');
    {
        int q = (int)(((long)a * 1000 * 2 + 255) / (255 * 2));
        for (int d = 100; d > 0; d /= 10) ob_ch(b, (char)('0' + (q / d) % 10));
    }
}

/* CSS Color 4 serialisation, as every browser reports it: opaque colours are
 * `rgb(r, g, b)` and anything else `rgba(r, g, b, a)` with a in 0..1. */
static void ob_color(struct obuf *b, css_color c)
{
    unsigned a = (c >> 24) & 0xFF;
#ifdef CSSOM_NEGCTL_SERIALIZE
    /* THE NEGATIVE CONTROL (tests/cssom.mk). Serialisation is exactly where a
     * wrong CSSOM passes a weak test: every property is present, every call
     * returns a plausible string, and only the BYTES are wrong -- which is all
     * WPT ever compares. This emits the hex form a hand-rolled serialiser
     * naturally produces. `make test-cssom-negctl` requires the suite to fail
     * against it; if it passes, the suite is not measuring serialisation. */
    static const char HEXD[] = "0123456789abcdef";
    ob_ch(b, '#');
    for (int sh = 16; sh >= 0; sh -= 8) {
        ob_ch(b, HEXD[(c >> (sh + 4)) & 0xF]);
        ob_ch(b, HEXD[(c >> sh) & 0xF]);
    }
    return;
#endif
    ob_s(b, a == 0xFF ? "rgb(" : "rgba(");
    ob_i(b, (int)((c >> 16) & 0xFF)); ob_s(b, ", ");
    ob_i(b, (int)((c >> 8) & 0xFF));  ob_s(b, ", ");
    ob_i(b, (int)(c & 0xFF));
    if (a != 0xFF) { ob_s(b, ", "); ob_alpha_byte(b, a); }
    ob_ch(b, ')');
}

/* A length in the same integer pixels the rest of the engine speaks, or a
 * percentage kept as a percentage (see the resolved-vs-computed note in css.h). */
static void ob_len(struct obuf *b, css_fixed val, css_unit unit, int fp)
{
    if (unit == CSS_UNIT_PCT) { ob_fixed(b, val); ob_ch(b, '%'); return; }
    ob_i(b, len_px(val, unit, fp, 0));
#ifndef CSSOM_NEGCTL_SERIALIZE
    ob_s(b, "px");
#endif    /* the other half of the serialisation control: a bare number */
}

static const char *border_style_name(uint8_t s)
{
    switch (s) {
    case CSS_BORDER_STYLE_HIDDEN: return "hidden";
    case CSS_BORDER_STYLE_DOTTED: return "dotted";
    case CSS_BORDER_STYLE_DASHED: return "dashed";
    case CSS_BORDER_STYLE_SOLID:  return "solid";
    case CSS_BORDER_STYLE_DOUBLE: return "double";
    case CSS_BORDER_STYLE_GROOVE: return "groove";
    case CSS_BORDER_STYLE_RIDGE:  return "ridge";
    case CSS_BORDER_STYLE_INSET:  return "inset";
    case CSS_BORDER_STYLE_OUTSET: return "outset";
    default:                      return "none";
    }
}

static const char *overflow_name(uint8_t v)
{
    switch (v) {
    case CSS_OVERFLOW_HIDDEN: return "hidden";
    case CSS_OVERFLOW_SCROLL: return "scroll";
    case CSS_OVERFLOW_AUTO:   return "auto";
    default:                  return "visible";
    }
}

static const char *display_name(const css_computed_style *cs)
{
    switch (css_computed_display(cs, false)) {
    case CSS_DISPLAY_NONE:          return "none";
    case CSS_DISPLAY_CONTENTS:      return "contents";
    case CSS_DISPLAY_INLINE:        return "inline";
    case CSS_DISPLAY_INLINE_BLOCK:  return "inline-block";
    case CSS_DISPLAY_LIST_ITEM:     return "list-item";
    case CSS_DISPLAY_FLEX:          return "flex";
    case CSS_DISPLAY_INLINE_FLEX:   return "inline-flex";
    case CSS_DISPLAY_GRID:          return "grid";
    case CSS_DISPLAY_INLINE_GRID:   return "inline-grid";
    case CSS_DISPLAY_TABLE:         return "table";
    case CSS_DISPLAY_TABLE_ROW:     return "table-row";
    case CSS_DISPLAY_TABLE_CELL:    return "table-cell";
    case CSS_DISPLAY_INLINE_TABLE:  return "inline-table";
    default:                        return "block";
    }
}

/* The font-size of `n`'s PARENT, which is what an em-valued length on `n`
 * resolves against. convert() already computed it and left it in the parent's
 * cstyle, so this is a lookup, not a second cascade. */
static int parent_font_of(struct node *n)
{
    struct node *p = n ? n->parent : 0;
    while (p && p->type != N_ELEM) p = p->parent;
    return (p && p->style) ? ((struct cstyle *)p->style)->font_px : 16;
}

/* One declaration out of a `style=` block, by name. A narrow re-implementation
 * of what js_dom.c's sty_find does, and narrow on purpose: this wants ONE
 * custom property's value and nothing else, and reaching into that file's
 * static would couple the CSS engine to the JS bindings for four lines. */
static int cws(char c)
{ return c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d || c == 0x0c; }

static const char *inline_decl(struct node *e, const char *name, int nlen, int *vlen)
{
    const char *s = dom_attr(e, "style");
    if (!s) return 0;
    for (int i = 0; s[i]; ) {
        while (s[i] && (cws(s[i]) || s[i] == ';')) i++;
        if (!s[i]) break;
        int d0 = i;
        while (s[i] && s[i] != ';') i++;
        int d1 = i, c = d0;
        while (c < d1 && s[c] != ':') c++;
        if (c >= d1) continue;
        int n0 = d0, n1 = c;
        while (n1 > n0 && cws(s[n1 - 1])) n1--;
        /* Custom property names are CASE-SENSITIVE, unlike every other CSS
         * property name. `--X` and `--x` are two different properties. */
        if (n1 - n0 == nlen && memcmp(s + n0, name, (size_t)nlen) == 0) {
            int v0 = c + 1, v1 = d1;
            while (v0 < v1 && cws(s[v0])) v0++;
            while (v1 > v0 && cws(s[v1 - 1])) v1--;
            *vlen = v1 - v0;
            return s + v0;
        }
    }
    return 0;
}

/* The computed value of a custom property on `e`.
 *
 * Custom properties INHERIT, so the answer is the nearest ancestor-or-self
 * that declares one. Two sources, in cascade order as far as this engine can
 * see it:
 *
 *   the element's own `style=` block, walking up the tree -- which is where
 *   `el.style.setProperty('--x', ...)` puts it, and the case WPT exercises
 *   most.
 *
 *   the document's sheet table (css_vars.c), which is what `:root { --x: ... }`
 *   lands in.
 *
 * KNOWN LIMIT, and it is css_vars.c's documented one, not a new one: that
 * table is ONE table for the whole document, so a `--x` redefined on a subtree
 * by a stylesheet rule reads document-wide. Inline declarations are per
 * element and exact, which is why they are checked first rather than second.
 * An undeclared custom property reads "", which is what a real browser
 * answers for one that was never registered. */
static int custom_text(struct node *e, const char *name, char *out, int outmax)
{
    int nlen = (int)strlen(name);
    for (struct node *p = e; p; p = p->parent) {
        if (p->type != N_ELEM) continue;
        int vlen = 0;
        const char *v = inline_decl(p, name, nlen, &vlen);
        if (v) {
            if (vlen > outmax - 1) vlen = outmax - 1;
            memcpy(out, v, (size_t)vlen);
            out[vlen] = 0;
            return vlen;
        }
    }
    const char *v = css_vars_value(name);
    if (!v) return 0;
    int vlen = (int)strlen(v);
    if (vlen > outmax - 1) vlen = outmax - 1;
    memcpy(out, v, (size_t)vlen);
    out[vlen] = 0;
    return vlen;
}

int css_computed_text(struct node *n, int prop, char *out, int outmax)
{
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    if (!n || n->type != N_ELEM) return 0;
    if (prop == CSSP_CUSTOM) {
        /* The cascade still has to be current: css_vars.c's table is filled by
         * css_expand_vars on the way into the flush, so a document nobody
         * styled has no table at all until this runs. */
        css_ensure_styled(n);
        return custom_text(n, css_prop_last_custom(), out, outmax);
    }
    if (prop < 0 || prop >= CSSP__COUNT) return 0;
    /* CSSOM: reading a resolved value updates style first. Before this, an
     * embedder that never ran the cascade -- or a script that wrote a style
     * and read it back in the same turn -- got "" from the line below for
     * EVERY property, including the most ordinary ones. */
    css_ensure_styled(n);
    if (!n->computed) return 0;
    const css_computed_style *cs = (const css_computed_style *)n->computed;

    struct obuf b = { out, 0, outmax };
    css_fixed len = 0; css_unit unit = CSS_UNIT_PX; css_color col = 0;
    /* An em-valued length on any property BUT font-size resolves against the
     * element's OWN font-size, which convert() already worked out (and clamped)
     * into the cstyle. Only font-size itself would need the parent's, and that
     * one is read straight back out of the cstyle rather than re-derived. */
    int fp = n->style ? ((struct cstyle *)n->style)->font_px : parent_font_of(n);

    switch (prop) {
    case CSSP_POINTER_EVENTS:
        ob_s(&b, css_computed_pointer_events(cs)==CSS_POINTER_EVENTS_NONE ? "none" :
                 css_computed_pointer_events(cs)==CSS_POINTER_EVENTS_ALL ? "all" : "auto");
        break;
    case CSSP_BACKFACE_VISIBILITY:
        ob_s(&b,n->style && ((struct cstyle *)n->style)->backface_hidden?"hidden":"visible");
        break;
    case CSSP_TRANSFORM_STYLE:
        ob_s(&b,n->style && ((struct cstyle *)n->style)->preserve_3d?"preserve-3d":"flat");
        break;
    case CSSP_WIDTH:
        if (css_computed_width(cs, &len, &unit) == CSS_WIDTH_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "auto");
        break;
    case CSSP_HEIGHT:
        if (css_computed_height(cs, &len, &unit) == CSS_HEIGHT_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "auto");
        break;
    /* min-*: the RAW getter, for the same reason convert() uses it -- the public
     * accessor reports `auto` as a set 0 unless the element is itself a flex
     * container, and `min-width:auto` vs `min-width:0` is precisely the
     * distinction a script inspecting a flex item is asking about. */
    case CSSP_MIN_WIDTH:
        if (get_min_width(cs, &len, &unit) == CSS_MIN_WIDTH_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "auto");
        break;
    case CSSP_MIN_HEIGHT:
        if (get_min_height(cs, &len, &unit) == CSS_MIN_HEIGHT_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "auto");
        break;
    case CSSP_MAX_WIDTH:
        if (css_computed_max_width(cs, &len, &unit) == CSS_MAX_WIDTH_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "none");
        break;
    case CSSP_MAX_HEIGHT:
        if (css_computed_max_height(cs, &len, &unit) == CSS_MAX_HEIGHT_SET) ob_len(&b, len, unit, fp);
        else ob_s(&b, "none");
        break;

#define MARGIN_CASE(P, NAME) \
    case P: \
        if (css_computed_margin_##NAME(cs, &len, &unit) == CSS_MARGIN_AUTO) ob_s(&b, "auto"); \
        else ob_len(&b, len, unit, fp); \
        break;
    MARGIN_CASE(CSSP_MARGIN_TOP, top)
    MARGIN_CASE(CSSP_MARGIN_RIGHT, right)
    MARGIN_CASE(CSSP_MARGIN_BOTTOM, bottom)
    MARGIN_CASE(CSSP_MARGIN_LEFT, left)
#undef MARGIN_CASE

#define PADDING_CASE(P, NAME) \
    case P: css_computed_padding_##NAME(cs, &len, &unit); ob_len(&b, len, unit, fp); break;
    PADDING_CASE(CSSP_PADDING_TOP, top)
    PADDING_CASE(CSSP_PADDING_RIGHT, right)
    PADDING_CASE(CSSP_PADDING_BOTTOM, bottom)
    PADDING_CASE(CSSP_PADDING_LEFT, left)
#undef PADDING_CASE

    case CSSP_COLOR:
        css_computed_color(cs, &col);
        ob_color(&b, col);
        break;
    case CSSP_BACKGROUND_COLOR:
        /* An unset background is transparent, and every browser spells that
         * "rgba(0, 0, 0, 0)" rather than the keyword. */
        if (css_computed_background_color(cs, &col) == CSS_BACKGROUND_COLOR_COLOR) ob_color(&b, col);
        else ob_s(&b, "rgba(0, 0, 0, 0)");
        break;

    case CSSP_FONT_SIZE:
        /* Resolved through convert()'s clamp, so it agrees with what actually
         * got rendered rather than with what the cascade nominally said. */
        ob_i(&b, fp); ob_s(&b, "px");
        break;
    case CSSP_FONT_FAMILY: {
        /* LibCSS splits a font-family list in two: the named families land in
         * the string array and the trailing GENERIC family becomes the status
         * (select/properties/font_family.c drops everything after the first
         * generic). Re-joining them reproduces the authored list, which is what
         * a browser reports.
         *
         * The one inaccuracy: a list with NO generic in it inherits its status
         * from the parent (or the UA hint), so `font-family: Georgia` comes
         * back as "Georgia, sans-serif". The generic really is part of the
         * computed value here -- it is what font fallback uses -- so reporting
         * it is closer to right than dropping it, but it is a difference a
         * strict comparison against Chrome would see. */
        lwc_string **names = NULL;
        uint8_t st = css_computed_font_family(cs, &names);
        int wrote = 0;
        for (int i = 0; names && names[i]; i++) {
            if (wrote++) ob_s(&b, ", ");
            ob_sn(&b, lwc_string_data(names[i]), (int)lwc_string_length(names[i]));
        }
        const char *gen = st == CSS_FONT_FAMILY_SERIF ? "serif" :
                          st == CSS_FONT_FAMILY_CURSIVE ? "cursive" :
                          st == CSS_FONT_FAMILY_FANTASY ? "fantasy" :
                          st == CSS_FONT_FAMILY_MONOSPACE ? "monospace" :
                          st == CSS_FONT_FAMILY_SANS_SERIF ? "sans-serif" : 0;
        if (gen) { if (wrote) ob_s(&b, ", "); ob_s(&b, gen); }
        else if (!wrote) ob_s(&b, "sans-serif");
        break;
    }
    case CSSP_FONT_WEIGHT: {
        /* Numeric, like a real getComputedStyle: `bold` computes to 700. */
        static const int wmap[] = { 400, 400, 700, 700, 400,
                                    100, 200, 300, 400, 500, 600, 700, 800, 900 };
        uint8_t w = css_computed_font_weight(cs);
        ob_i(&b, w < (uint8_t)(sizeof wmap / sizeof wmap[0]) ? wmap[w] : 400);
        break;
    }
    case CSSP_FONT_STYLE: {
        uint8_t s = css_computed_font_style(cs);
        ob_s(&b, s == CSS_FONT_STYLE_ITALIC ? "italic" :
                 s == CSS_FONT_STYLE_OBLIQUE ? "oblique" : "normal");
        break;
    }

    case CSSP_DISPLAY: ob_s(&b, display_name(cs)); break;
    case CSSP_POSITION:
        switch (css_computed_position(cs)) {
        case CSS_POSITION_ABSOLUTE: ob_s(&b, "absolute"); break;
        case CSS_POSITION_RELATIVE: ob_s(&b, "relative"); break;
        case CSS_POSITION_FIXED:    ob_s(&b, "fixed"); break;
        case CSS_POSITION_STICKY:   ob_s(&b, "sticky"); break;
        default:                    ob_s(&b, "static"); break;
        }
        break;

#define OFFSET_CASE(P, NAME, SET) \
    case P: \
        if (css_computed_##NAME(cs, &len, &unit) == SET) ob_len(&b, len, unit, fp); \
        else ob_s(&b, "auto"); \
        break;
    OFFSET_CASE(CSSP_TOP, top, CSS_TOP_SET)
    OFFSET_CASE(CSSP_RIGHT, right, CSS_RIGHT_SET)
    OFFSET_CASE(CSSP_BOTTOM, bottom, CSS_BOTTOM_SET)
    OFFSET_CASE(CSSP_LEFT, left, CSS_LEFT_SET)
#undef OFFSET_CASE

    case CSSP_OPACITY: {
        css_fixed op = F_1;
        if (css_computed_opacity(cs, &op) != CSS_OPACITY_SET) op = F_1;
        ob_fixed(&b, op);
        break;
    }
    case CSSP_Z_INDEX: {
        int32_t z = 0;
        /* Stored raw (a css_fixed), unlike `order` which the cascade already
         * truncated -- so `z-index:5` arrives as 5120. */
        if (css_computed_z_index(cs, &z) == CSS_Z_INDEX_SET) ob_i(&b, FIXTOINT((css_fixed)z));
        else ob_s(&b, "auto");
        break;
    }
    case CSSP_VISIBILITY: {
        uint8_t v = css_computed_visibility(cs);
        ob_s(&b, v == CSS_VISIBILITY_HIDDEN ? "hidden" :
                 v == CSS_VISIBILITY_COLLAPSE ? "collapse" : "visible");
        break;
    }

    /* `overflow` is a shorthand: it serialises to the single value only when
     * both axes agree, which is what the CSSOM says a shorthand does. */
    case CSSP_OVERFLOW: {
        uint8_t x = css_computed_overflow_x(cs), y = css_computed_overflow_y(cs);
        ob_s(&b, overflow_name(x));
        if (x != y) { ob_ch(&b, ' '); ob_s(&b, overflow_name(y)); }
        break;
    }
    case CSSP_OVERFLOW_X: ob_s(&b, overflow_name(css_computed_overflow_x(cs))); break;
    case CSSP_OVERFLOW_Y: ob_s(&b, overflow_name(css_computed_overflow_y(cs))); break;

    case CSSP_FLEX_DIRECTION:
        switch (css_computed_flex_direction(cs)) {
        case CSS_FLEX_DIRECTION_ROW_REVERSE:    ob_s(&b, "row-reverse"); break;
        case CSS_FLEX_DIRECTION_COLUMN:         ob_s(&b, "column"); break;
        case CSS_FLEX_DIRECTION_COLUMN_REVERSE: ob_s(&b, "column-reverse"); break;
        default:                                ob_s(&b, "row"); break;
        }
        break;
    case CSSP_FLEX_WRAP:
        switch (css_computed_flex_wrap(cs)) {
        case CSS_FLEX_WRAP_WRAP:         ob_s(&b, "wrap"); break;
        case CSS_FLEX_WRAP_WRAP_REVERSE: ob_s(&b, "wrap-reverse"); break;
        default:                         ob_s(&b, "nowrap"); break;
        }
        break;
    case CSSP_FLEX_GROW: {
        css_fixed g = 0;
        if (css_computed_flex_grow(cs, &g) != CSS_FLEX_GROW_SET) g = 0;
        ob_fixed(&b, g);
        break;
    }
    case CSSP_FLEX_SHRINK: {
        css_fixed s = F_1;
        if (css_computed_flex_shrink(cs, &s) != CSS_FLEX_SHRINK_SET) s = F_1;
        ob_fixed(&b, s);
        break;
    }
    case CSSP_FLEX_BASIS:
        switch (css_computed_flex_basis(cs, &len, &unit)) {
        case CSS_FLEX_BASIS_SET:     ob_len(&b, len, unit, fp); break;
        case CSS_FLEX_BASIS_CONTENT: ob_s(&b, "content"); break;
        default:                     ob_s(&b, "auto"); break;
        }
        break;
    case CSSP_JUSTIFY_CONTENT:
        switch (css_computed_justify_content(cs)) {
        case CSS_JUSTIFY_CONTENT_FLEX_END:      ob_s(&b, "flex-end"); break;
        case CSS_JUSTIFY_CONTENT_CENTER:        ob_s(&b, "center"); break;
        case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: ob_s(&b, "space-between"); break;
        case CSS_JUSTIFY_CONTENT_SPACE_AROUND:  ob_s(&b, "space-around"); break;
        case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:  ob_s(&b, "space-evenly"); break;
        default:                                ob_s(&b, "flex-start"); break;
        }
        break;
    case CSSP_ALIGN_ITEMS:
    case CSSP_ALIGN_SELF: {
        uint8_t a = (prop == CSSP_ALIGN_ITEMS) ? css_computed_align_items(cs)
                                               : css_computed_align_self(cs);
        if (prop == CSSP_ALIGN_SELF && a == CSS_ALIGN_SELF_AUTO) { ob_s(&b, "auto"); break; }
        ob_s(&b, a == CSS_ALIGN_ITEMS_FLEX_START ? "flex-start" :
                 a == CSS_ALIGN_ITEMS_FLEX_END   ? "flex-end" :
                 a == CSS_ALIGN_ITEMS_CENTER     ? "center" :
                 a == CSS_ALIGN_ITEMS_BASELINE   ? "baseline" : "stretch");
        break;
    }
    case CSSP_ALIGN_CONTENT:
        switch (css_computed_align_content(cs)) {
        case CSS_ALIGN_CONTENT_FLEX_START:    ob_s(&b, "flex-start"); break;
        case CSS_ALIGN_CONTENT_FLEX_END:      ob_s(&b, "flex-end"); break;
        case CSS_ALIGN_CONTENT_CENTER:        ob_s(&b, "center"); break;
        case CSS_ALIGN_CONTENT_SPACE_BETWEEN: ob_s(&b, "space-between"); break;
        case CSS_ALIGN_CONTENT_SPACE_AROUND:  ob_s(&b, "space-around"); break;
        default:                              ob_s(&b, "stretch"); break;
        }
        break;
    case CSSP_ORDER: {
        int32_t o = 0;
        css_computed_order(cs, &o);
        ob_i(&b, (int)o);
        break;
    }

    /* border-*-width: a `none`/`hidden` border occupies no space whatever the
     * width says, and that is the used value every browser reports. */
#define BW_CASE(P, NAME) \
    case P: \
        if (css_computed_border_##NAME##_style(cs) == CSS_BORDER_STYLE_NONE || \
            css_computed_border_##NAME##_style(cs) == CSS_BORDER_STYLE_HIDDEN) ob_s(&b, "0px"); \
        else { css_computed_border_##NAME##_width(cs, &len, &unit); ob_len(&b, len, unit, fp); } \
        break;
    BW_CASE(CSSP_BORDER_TOP_WIDTH, top)
    BW_CASE(CSSP_BORDER_RIGHT_WIDTH, right)
    BW_CASE(CSSP_BORDER_BOTTOM_WIDTH, bottom)
    BW_CASE(CSSP_BORDER_LEFT_WIDTH, left)
#undef BW_CASE
#define BS_CASE(P, NAME) \
    case P: ob_s(&b, border_style_name(css_computed_border_##NAME##_style(cs))); break;
    BS_CASE(CSSP_BORDER_TOP_STYLE, top)
    BS_CASE(CSSP_BORDER_RIGHT_STYLE, right)
    BS_CASE(CSSP_BORDER_BOTTOM_STYLE, bottom)
    BS_CASE(CSSP_BORDER_LEFT_STYLE, left)
#undef BS_CASE
    /* border-*-color: `currentColor` is the initial value, and it resolves to
     * the element's own colour -- which is what a browser reports too. */
#define BC_CASE(P, NAME) \
    case P: \
        if (css_computed_border_##NAME##_color(cs, &col) != CSS_BORDER_COLOR_COLOR) \
            css_computed_color(cs, &col); \
        ob_color(&b, col); \
        break;
    BC_CASE(CSSP_BORDER_TOP_COLOR, top)
    BC_CASE(CSSP_BORDER_RIGHT_COLOR, right)
    BC_CASE(CSSP_BORDER_BOTTOM_COLOR, bottom)
    BC_CASE(CSSP_BORDER_LEFT_COLOR, left)
#undef BC_CASE

    case CSSP_TEXT_ALIGN:
        switch (css_computed_text_align(cs)) {
        case CSS_TEXT_ALIGN_CENTER:
        case CSS_TEXT_ALIGN_LIBCSS_CENTER: ob_s(&b, "center"); break;
        case CSS_TEXT_ALIGN_RIGHT:
        case CSS_TEXT_ALIGN_LIBCSS_RIGHT:  ob_s(&b, "right"); break;
        case CSS_TEXT_ALIGN_JUSTIFY:       ob_s(&b, "justify"); break;
        /* `start`, not `left`, and the two are different LibCSS values that
         * were being folded together by the default branch.
         *
         * CSS_TEXT_ALIGN_DEFAULT (0x6) is LibCSS's name for the INITIAL value
         * of text-align, and the initial value is `start` -- which is what
         * every browser reports and therefore what WPT recorded. Reporting
         * `left` for it is right only in a left-to-right document and is
         * indistinguishable from an author who actually wrote `text-align:
         * left`, which is the distinction a computed value exists to make.
         * css/css-text/inheritance.html asserts it directly:
         *
         *     Property text-align has initial value start
         *       expected "start" but got "left"
         *
         * CSS_TEXT_ALIGN_LEFT and CSS_TEXT_ALIGN_LIBCSS_LEFT stay `left`; they
         * are an authored keyword and the quirks-mode alignment respectively,
         * and both really are left. */
        case CSS_TEXT_ALIGN_DEFAULT:       ob_s(&b, "start"); break;
        default:                           ob_s(&b, "left"); break;
        }
        break;
    case CSSP_LINE_HEIGHT:
        switch (css_computed_line_height(cs, &len, &unit)) {
        /* A unitless line-height stays a number: it is inherited as a number,
         * so reporting the px it happens to make on THIS element would be a
         * different value from the one the cascade holds. */
        case CSS_LINE_HEIGHT_NUMBER:    ob_fixed(&b, len); break;
        case CSS_LINE_HEIGHT_DIMENSION: ob_len(&b, len, unit, fp); break;
        default:                        ob_s(&b, "normal"); break;
        }
        break;
    case CSSP_TEXT_DECORATION: {
        uint8_t td = css_computed_text_decoration(cs);
        int wrote = 0;
        if (td & CSS_TEXT_DECORATION_UNDERLINE)    { ob_s(&b, "underline"); wrote = 1; }
        if (td & CSS_TEXT_DECORATION_OVERLINE)     { if (wrote) ob_ch(&b, ' '); ob_s(&b, "overline"); wrote = 1; }
        if (td & CSS_TEXT_DECORATION_LINE_THROUGH) { if (wrote) ob_ch(&b, ' '); ob_s(&b, "line-through"); wrote = 1; }
        if (!wrote) ob_s(&b, "none");
        break;
    }

    case CSSP_BOX_SIZING:
        ob_s(&b, css_computed_box_sizing(cs) == CSS_BOX_SIZING_BORDER_BOX
                 ? "border-box" : "content-box");
        break;
    case CSSP_WHITE_SPACE:
        switch (css_computed_white_space(cs)) {
        case CSS_WHITE_SPACE_PRE:      ob_s(&b, "pre"); break;
        case CSS_WHITE_SPACE_NOWRAP:   ob_s(&b, "nowrap"); break;
        case CSS_WHITE_SPACE_PRE_WRAP: ob_s(&b, "pre-wrap"); break;
        case CSS_WHITE_SPACE_PRE_LINE: ob_s(&b, "pre-line"); break;
        default:                       ob_s(&b, "normal"); break;
        }
        break;
    case CSSP_FLOAT:
        switch (css_computed_float(cs)) {
        case CSS_FLOAT_LEFT:  ob_s(&b, "left"); break;
        case CSS_FLOAT_RIGHT: ob_s(&b, "right"); break;
        default:              ob_s(&b, "none"); break;
        }
        break;
    case CSSP_CLEAR:
        switch (css_computed_clear(cs)) {
        case CSS_CLEAR_LEFT:  ob_s(&b, "left"); break;
        case CSS_CLEAR_RIGHT: ob_s(&b, "right"); break;
        case CSS_CLEAR_BOTH:  ob_s(&b, "both"); break;
        default:              ob_s(&b, "none"); break;
        }
        break;
    case CSSP_LIST_STYLE_TYPE:
        switch (css_computed_list_style_type(cs)) {
        case CSS_LIST_STYLE_TYPE_NONE:                 ob_s(&b, "none"); break;
        case CSS_LIST_STYLE_TYPE_CIRCLE:               ob_s(&b, "circle"); break;
        case CSS_LIST_STYLE_TYPE_SQUARE:               ob_s(&b, "square"); break;
        case CSS_LIST_STYLE_TYPE_DECIMAL:              ob_s(&b, "decimal"); break;
        case CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO: ob_s(&b, "decimal-leading-zero"); break;
        case CSS_LIST_STYLE_TYPE_LOWER_ALPHA:
        case CSS_LIST_STYLE_TYPE_LOWER_LATIN:          ob_s(&b, "lower-alpha"); break;
        case CSS_LIST_STYLE_TYPE_UPPER_ALPHA:
        case CSS_LIST_STYLE_TYPE_UPPER_LATIN:          ob_s(&b, "upper-alpha"); break;
        case CSS_LIST_STYLE_TYPE_LOWER_ROMAN:          ob_s(&b, "lower-roman"); break;
        case CSS_LIST_STYLE_TYPE_UPPER_ROMAN:          ob_s(&b, "upper-roman"); break;
        default:                                       ob_s(&b, "disc"); break;
        }
        break;
    default:
        break;
    }
    out[b.n] = 0;
    return b.n;
}

/* A cstyle is freed in dom.c with one kfree, and scoped snapshots copy only
 * its prefix. Keep both proxies and copied UTF-8 in the SAME allocation, rather
 * than teach every DOM cleanup/clone path a second lifetime. LibCSS content
 * strings belong to the computed-style arena: borrowing them into the display
 * list would turn restyling into a use-after-free.
 *
 * This is the string/attr() subset. Counters, quote-depth and image content
 * require additional state/resources and are refused as a whole declaration;
 * silently printing only the string half of `url(...) "alt"` would be false
 * rendering. A 64 KiB per-pseudo budget refuses oversized content without
 * truncating the generated text into a plausible but incorrect value.
 * Measured 2026-09-09 by test-generated-content: with this producer disabled,
 * the empty 80x12 generated background disappears and the authored child moves
 * from y=12 to y=0. The positive gate inspects IT_TEXT/IT_RECT and geometry;
 * actual glyph rasterization remains a separate guest-fixture check. */
struct generated_box {
    struct cstyle style;
    struct node box, text;
};
/* The generator and feature queries share this whitelist. LibCSS also parses
 * counters, quotes and URIs, but accepting their syntax does not make them ink.
 * Keep capability separate from whether a particular attr currently has text. */
static unsigned long long g_generated_composes, g_generated_skips;
unsigned long long css_generated_compose_count(void) { return g_generated_composes; }
unsigned long long css_generated_skip_count(void) { return g_generated_skips; }

static int generated_item_supported(int type)
{
    return type == CSS_COMPUTED_CONTENT_STRING || type == CSS_COMPUTED_CONTENT_ATTR;
}

/* Value-aware @supports hook; input is the property parser's bytecode, never a
 * second CSS text scanner. CSS.supports uses this same hook on its inline rule.
 * Each supported list item carries one string-table index; reject the ENTIRE
 * list on an unsupported item. CSS-wide keywords/none/normal need no box. */
int logit_css_generated_supports(const uint32_t *code, uint32_t used)
{
#ifdef CSS_CONTENT_SUPPORTS_NEGCTL
    return 0; /* Restore the old name-only denial for the watched control. */
#endif
    if (!code || !used || getOpcode(code[0]) != CSS_PROP_CONTENT) return 0;
    if (getFlagValue(code[0]) != FLAG_VALUE__NONE) return used == 1;
    uint32_t value = getValue(code[0]), at = 1;
    if (value == CONTENT_NORMAL || value == CONTENT_NONE) return used == 1;
    for (;;) {
        int type = value == CONTENT_STRING ? CSS_COMPUTED_CONTENT_STRING :
                   value == CONTENT_ATTR ? CSS_COMPUTED_CONTENT_ATTR : -1;
        if (!generated_item_supported(type) || at + 1 >= used) return 0;
        at++; /* the string/attribute name belongs to the stylesheet */
        value = code[at++];
        if (value == CONTENT_NORMAL) return at == used;
    }
}

static int generated_text(const css_computed_style *cs, struct node *owner, char *out)
{
    const css_computed_content_item *v = NULL;
    if (css_computed_content(cs, &v) != CSS_CONTENT_SET || !v) return -1;
    int total = 0;
    for (; v->type != CSS_COMPUTED_CONTENT_NONE; v++) {
        const char *s; size_t len;
        if (!generated_item_supported(v->type)) return -1;
        if (v->type == CSS_COMPUTED_CONTENT_STRING) {
            s = lwc_string_data(v->data.string); len = lwc_string_length(v->data.string);
        } else if (v->type == CSS_COMPUTED_CONTENT_ATTR) {
            s = dom_attr_lw(owner, v->data.attr);
            if (!s) s = "";
            len = strlen(s);
        } else return -1;
        if (len > 65536u - (unsigned)total) return -1;
        if (out && len) memcpy(out + total, s, len);
        total += (int)len;
    }
    if (out) out[total] = 0;
    return total;
}

/* ---------- traversal ---------- */
static void style_node(struct node *n, const css_computed_style *parent, int parent_font)
{
    if (n->type != N_ELEM) return;

    css_select_results *res = NULL;
    css_stylesheet *inl = NULL;
    const char *istyle = dom_attr_lw(n, dom_atoms.a_style);
    if (istyle && *istyle) inl = make_sheet(istyle, strlen(istyle), true, g_allow_quirks);

    if (css_select_style(g_ctx, n, &g_unit, &g_media, inl,
                         &g_handler, NULL, &res) != CSS_OK || !res) {
        if (inl) css_stylesheet_destroy(inl);
        return;
    }

    /* select_style already absolutised the root; non-root nodes inherit + resolve
     * relative units against the parent's composed style via compose(). */
    css_computed_style *base = res->styles[CSS_PSEUDO_ELEMENT_NONE];
    css_computed_style *composed = NULL;
    css_computed_style *eff = base;
    if (parent && css_computed_style_compose(parent, base, &g_unit, &composed) == CSS_OK && composed)
        eff = composed;
    css_computed_style *line_resolved = NULL;
    if (normalize_line_height(eff, parent, parent_font, &line_resolved) != CSS_OK) {
        /* An allocation failure must not publish a style whose inherited
         * length changes meaning at the next generation. Retain the previous
         * node style, as the selection-failure path above does. */
        if (composed) css_computed_style_destroy(composed);
        css_select_results_destroy(res);
        if (inl) css_stylesheet_destroy(inl);
        return;
    }
    if (line_resolved) eff = line_resolved;

    /* The root element's own style is the reference for every `rem` below it.
     * Publish it before the subtree is walked and before convert() runs on any
     * descendant -- LibCSS reads g_unit.root_style when it absolutises a
     * rem-valued font-size, and px_per_unit() reads g_root_px for every other
     * rem-valued length. The root itself resolved its rem against
     * font_size_default (16), which is exactly what CSS specifies. */
    int is_root = (parent == NULL);

    css_computed_style *pseudo[2] = { NULL, NULL };
    int plen[2] = { -1, -1 };
    size_t extra = 0;
#ifndef CSS_NEGCTL_GENERATED
    for (int pi = 0; pi < 2; pi++) {
        css_computed_style *pc = res->styles[pi ? CSS_PSEUDO_ELEMENT_AFTER : CSS_PSEUDO_ELEMENT_BEFORE];
        if (!pc) continue;
#ifndef CSS_GENERATED_NO_EMPTY_SKIP
        /* A global ::before/::after reset creates partial pseudo styles even
         * without a generated box. NORMAL/NONE cannot be changed by font or
         * inheritance composition, so avoid composing them only to discard
         * them afterwards. Explicit INHERIT must still compose; an empty
         * STRING must still build its background/box. The host gate's reset
         * page drops actual composition attempts from 16 to 0 with identical
         * authored text; guest phase timing measures any speed benefit. */
        const css_computed_content_item *content_items;
        uint8_t content_type = css_computed_content(pc, &content_items);
        if (content_type == CSS_CONTENT_NORMAL || content_type == CSS_CONTENT_NONE) {
            g_generated_skips++;
            continue;
        }
#endif
        g_generated_composes++;
        if (css_computed_style_compose(eff, pc, &g_unit, &pseudo[pi]) != CSS_OK)
            continue;
        css_computed_style *pr = NULL;
        if (normalize_line_height(pseudo[pi], eff, parent_font, &pr) != CSS_OK) {
            css_computed_style_destroy(pseudo[pi]); pseudo[pi] = NULL;
            continue;
        }
        if (pr) { css_computed_style_destroy(pseudo[pi]); pseudo[pi] = pr; }
        plen[pi] = generated_text(pseudo[pi], n, NULL);
        if (plen[pi] >= 0) extra += sizeof(struct generated_box) + (size_t)plen[pi] + 1 + 7;
    }
#endif
    struct cstyle *o = kmalloc(sizeof *o + extra);
    if (o) {
        memset(o, 0, sizeof *o);
        o->font_px = parent_font;          /* sensible default before convert */
        convert(eff, parent_font, o);
        css_color local_color;
        o->svg_color_explicit = css_computed_color(base, &local_color) == CSS_COLOR_COLOR;
        char *tail = (char *)(o + 1);
        for (int pi = 0; pi < 2; pi++) if (plen[pi] >= 0) {
            /* Align each embedded struct after the previous variable text. */
            tail = (char *)(((uintptr_t)tail + 7u) & ~(uintptr_t)7u);
            struct generated_box *g = (struct generated_box *)tail;
            memset(g, 0, sizeof *g);
            convert(pseudo[pi], o->font_px, &g->style);
            g->style.generated_owner = n; g->style.generated_kind = (unsigned char)(pi + 1);
            g->box.type = N_ELEM; g->box.tag = "span"; g->box.tag_id = TAG_SPAN;
            g->box.parent = n; g->box.style = &g->style;
            g->box.first_child = g->box.last_child = &g->text;
            g->text.type = N_TEXT; g->text.parent = &g->box;
            g->text.text = (char *)(g + 1); g->text.textlen = plen[pi];
            generated_text(pseudo[pi], n, g->text.text);
            o->generated[pi] = &g->box;
            tail = g->text.text + plen[pi] + 1;
        }
        /* See is_html_table_box(): the collapsed-table exception, which needs
         * the element and so cannot live inside convert(). */
        if (css_computed_border_collapse(eff) == CSS_BORDER_COLLAPSE_COLLAPSE &&
            is_html_table_box(n)) {
            for (int ri = 0; ri < 4; ri++) { o->radius[ri] = 0; o->radius_pct[ri] = 0; }
        }
        if (n->style) kfree(n->style);
        n->style = o;
    }
    for (int pi = 0; pi < 2; pi++) if (pseudo[pi]) css_computed_style_destroy(pseudo[pi]);
    g_stat_styled++;
    if (is_root) { g_unit.root_style = eff; g_root_px = o ? o->font_px : 16; }

    /* Keep the effective computed style on the node. LibCSS arena-interns and
     * refcounts computed styles (select/arena.c), so identical styles across a
     * page share one block and holding a reference costs ~8 bytes per node --
     * cheap enough to make the real style available to anything that needs a
     * property `struct cstyle` does not carry. */
    if (n->computed) { css_computed_style_destroy((css_computed_style *)n->computed); n->computed = NULL; }
    n->computed = css__computed_style_ref(eff);

    int my_font = o ? o->font_px : parent_font;
    for (struct node *c = n->first_child; c; c = c->next)
        style_node(c, eff, my_font);

    if (line_resolved) css_computed_style_destroy(line_resolved);
    if (composed) css_computed_style_destroy(composed);
    css_select_results_destroy(res);
    if (inl) css_stylesheet_destroy(inl);
}

/* The sheet set and the document the last full cascade ran over. Read by
 * css_ensure_styled below, which is the only reason they exist -- see the long
 * note there for why re-styling with the WRONG sheet set is the failure mode
 * this records against. */
static struct node *g_auto_root;
/* Full synchronous computed-style flushes need the same extension producer
 * as scoped flushes. Otherwise the base pass zeroes a new backface keyword
 * immediately before getComputedStyle reads it. Registered by the browser. */
static void (*g_post_pass)(struct node *root, const char *css, int len);
static const char  *g_auto_css;
static int          g_auto_len;
static char        *g_auto_owned;      /* CSS we collected ourselves, if any */
static int          g_auto_ready;
static unsigned long g_auto_ui_revision;

void css_apply(struct node *root, const char *page_css, int page_len)
{
    ui_sync();
    if (!g_ctx) css_init();
    if (!g_ctx) return;

    /* The document's quirks mode, set from its doctype by html_tree.c. Only
     * full quirks changes anything: QM_LIMITED_QUIRKS ("almost standards")
     * differs from standards solely in the inline-image line-box quirk, which
     * our line layout does not implement either way. */
    g_allow_quirks = root && root->doc && dom_doc_quirks(root->doc) == QM_QUIRKS;
    set_quirks_sheet(g_allow_quirks ? 1 : 0);

    css_stylesheet *author = NULL;
    if (page_css && page_len > 0) {
        author = author_sheet(page_css, (size_t)page_len, g_allow_quirks);
        if (author) css_select_ctx_append_sheet(g_ctx, author, CSS_ORIGIN_AUTHOR, NULL);
    }

    /* Belt and braces: a pass that bailed out mid-way (OOM) would have left
     * entries behind, and they were selected against a different sheet set. */
    nd_reset();
    g_root_px = 16;
    g_unit.root_style = NULL;

    for (struct node *c = root->first_child; c; c = c->next)
        style_node(c, NULL, 16);   /* top-level: no CSS parent (root absolutised by select) */

    /* The node-data table and root_style are both pass-scoped: the cached
     * selection results belong to THIS sheet set, and root_style points into a
     * computed style only the nodes keep alive. */
    nd_reset();
    g_unit.root_style = NULL;

    /* Removed from the selection context but NOT destroyed: the parse belongs
     * to the cache now, and the next pass over the same bytes reuses it. */
    if (author) css_select_ctx_remove_sheet(g_ctx, author);

    /* Remember the sheet set THIS document was styled against, so a computed
     * read that has to flush (see css_ensure_styled) re-runs the cascade over
     * the same CSS instead of a subset of it. The embedder's text outlives the
     * call -- browser.c's css_expanded is a static -- and when we collected it
     * ourselves g_auto_owned holds it. */
    g_auto_root = root;
    g_auto_css  = page_css;
    g_auto_len  = (page_css && page_len > 0) ? page_len : 0;
    g_auto_ready = 1;
    g_auto_ui_revision = ui_revision;
}

/* ======================================================================
 * "Update style" before a computed value is read
 *
 * THE MEASUREMENT THAT PRODUCED THIS, because it is not what it looks like.
 * getComputedStyle() answered "" for 10,196 css/ subtests, and the obvious
 * reading -- 63 of LibCSS's 239 computed accessors are wired up, so ~176
 * properties have no getter -- is WRONG, and building 176 getters would have
 * gained nothing. A probe asking for `display` on a plain <div>, which is one
 * of the 63 and about as wired as a property gets, also answered "":
 *
 *     display= color= float= fs=          ('float' in gCS == true, length == 62)
 *
 * The property table was never the problem. `node->computed` was NULL, for
 * every node in the document, because NOTHING HAD RUN THE CASCADE. css_apply
 * is called from browser.c's render loop and from nowhere else, so any
 * embedder that is not that loop -- the WPT runner is one, and so is any
 * script reading a computed value in the same turn it wrote a style -- reads
 * back a document that was never styled. One missing call, not 176 missing
 * getters.
 *
 * So this is the flush CSSOM requires and names: getComputedStyle returns a
 * *live* resolved-value declaration, and reading from it must first "update
 * style" for the element. js_cssom.c already does the layout half of exactly
 * this (flush_layout, via the reflow hook); this is the style half, and it is
 * needed in the real browser too -- `el.style.color = 'red'` followed by
 * `getComputedStyle(el).color` in the same tick read the PREVIOUS frame's
 * cascade before this existed.
 *
 * TWO CASES, and the difference between them is who owns the author CSS:
 *
 *   the embedder has styled this document already -- css_apply recorded its
 *   page_css above, so re-running the cascade uses THE SAME sheet set. This
 *   matters more than it looks: browser.c's text includes external <link>
 *   stylesheets it fetched, and re-applying with only the inline <style> text
 *   would silently un-style every page that keeps its CSS in a file.
 *
 *   nobody has styled it -- then there is no recorded sheet set and we collect
 *   one from the document's own <style> elements, which is what browser.c's
 *   collect_style does, and expand var() through the same css_expand_vars it
 *   uses. External sheets are NOT fetched here: fetching is the embedder's job
 *   and doing it from a property read would turn a getter into a network call.
 *
 * The dirty signal is js_dom.c's, read WEAKLY and never cleared. Not clearing
 * it is deliberate: the embedder's render loop clears it to decide whether to
 * re-LAYOUT, and a flush that consumed the flag would leave the browser
 * painting a stale frame. The cost of not clearing is that a burst of reads
 * after one mutation re-styles once per read; the scope is the marked subtree,
 * which is what makes that affordable.
 * ====================================================================== */

/* js_dom.c is not linked into the CSS host tests, so every one of these is
 * weak and guarded -- through include/weaksym.h, because the bare weak
 * DECLARATION only resolves to NULL on ELF and was a link error on the Mach-O
 * dev host. The lowercase `weak` macro remains wrong here for its own reason:
 * c/apps/libc/include/features.h defines that name, and it has already cost
 * this tree three bugs. */
extern int          js_dom_dirty(void)                    LOGIT_WEAK;
extern unsigned long long js_dom_mutation_generation(void) LOGIT_WEAK;
extern int          js_dom_inval_roots(void)              LOGIT_WEAK;
extern struct node *js_dom_inval_root(int i, int *sibs)   LOGIT_WEAK;
LOGIT_WEAK_STUB(js_dom_dirty);
LOGIT_WEAK_STUB(js_dom_mutation_generation);
LOGIT_WEAK_STUB(js_dom_inval_roots);
LOGIT_WEAK_STUB(js_dom_inval_root);

static int g_auto_flushes;      /* test seam: how many flushes actually ran */

/* WHY THERE IS A FINGERPRINT AND NOT JUST js_dom_dirty().
 *
 * The dirty flag is the EMBEDDER's, and clearing it here would leave the
 * browser painting a stale frame -- the render loop reads it to decide whether
 * to re-layout. So it stays set for the whole turn, and a page that reads
 * twenty properties after one mutation would re-run the cascade twenty times.
 * That is not a theoretical cost: the suite in tests/unit/csstyle_test.c
 * measured exactly 20 flushes for 20 reads and failed on it.
 *
 * The fix is a signal of our own, and it has to be CONSERVATIVE in the one
 * direction that matters: it may say "changed" when nothing did (a wasted
 * cascade, invisible), and it must never say "unchanged" when something did (a
 * stale value, which is the whole defect this file is fixing). So it is built
 * only from things that provably move when a style-affecting mutation happens:
 *
 *   the arena high-water mark -- dom.c's attr_set arena_dup()s every non-empty
 *   attribute value it stores, and never frees, so any attribute write that
 *   carries text moves it. Node and text creation move it too.
 *
 *   the marked scope roots, by {node, serial} -- a mutation somewhere new
 *   marks a new root, and a recycled node slot changes serial.
 *
 *   the ATTRIBUTES of those roots, by value pointer and length. This is the
 *   one that closes the hole the other two leave: `el.style.color = ''` on a
 *   block that becomes empty stores the literal "" and allocates nothing, so
 *   the arena does not move and the same node is marked again. The value
 *   POINTER still changes -- from an arena address to the literal -- so the
 *   fingerprint does. Cheap because scope roots are capped at 8 and elements
 *   carry a handful of attributes.
 *
 * When js_dom cannot name the scopes (js_dom_inval_roots() == 0 means "the
 * whole document"), there is nothing to fingerprint attributes of, and the
 * answer is always "changed". Conservative, and the case is rare. */
static unsigned long g_auto_fp;

static unsigned long inval_fingerprint(struct node *root)
{
#ifndef CSS_NEGCTL_CAPACITY_FINGERPRINT
    /* Correction to the historical high-water claim above: dom_doc_bytes is
     * allocated CHUNK capacity, not a mutation epoch. none -> block -> none
     * inside an already-dirty subtree can reuse a chunk and its scope roots,
     * making that fingerprint unchanged while the actual style changed.
     * Read the DOM binding's mutation generation when available. Keep the
     * legacy fallback for reduced host embeddings without that optional API;
     * it is no longer the shipping browser's freshness authority. */
    if (LOGIT_HAVE(js_dom_mutation_generation))
        return (unsigned long)js_dom_mutation_generation();
#endif
    unsigned long h = 1469598103934665603UL;
#define MIX(x) do { h ^= (unsigned long)(x); h *= 1099511628211UL; } while (0)
    MIX(root->doc ? dom_doc_bytes(root->doc) : 0);
    int nroots = LOGIT_HAVE(js_dom_inval_roots) ? js_dom_inval_roots() : 0;
    MIX(nroots);
    if (nroots <= 0) return 0;                 /* 0 = "unknown", never equal */
    if (!LOGIT_HAVE(js_dom_inval_root)) return 0;
    for (int i = 0; i < nroots; i++) {
        int sibs = 0;
        struct node *r = js_dom_inval_root(i, &sibs);
        if (!r) return 0;                      /* destroyed: scope unknown */
        MIX((unsigned long)(size_t)r); MIX(r->serial); MIX(sibs);
        MIX(r->nattr);
        for (int a = 0; a < r->nattr; a++) {
            MIX((unsigned long)(size_t)r->attrs[a].value);
            MIX(r->attrs[a].vlen);
        }
    }
#undef MIX
    return h ? h : 1;                          /* 0 is reserved for "unknown" */
}

static int collect_inline_sheets(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && n->tag &&
        n->tag[0] == 's' && n->tag[1] == 't' && n->tag[2] == 'y' &&
        n->tag[3] == 'l' && n->tag[4] == 'e' && n->tag[5] == 0) {
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
        if (o < max - 1) out[o++] = '\n';
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_inline_sheets(c, out, o, max);
    return o;
}

/* Gather the document's own <style> text and expand var(). Returns a malloc'd
 * NUL-terminated buffer (ownership passes to g_auto_owned) and the length, or
 * NULL when the document has no inline CSS at all -- which is not a failure:
 * the UA sheet plus the inline style= attributes still produce a real computed
 * style, and that is exactly the case the probe above was in. */
static char *build_author_css(struct node *root, int *out_len)
{
    *out_len = 0;
    int raw_cap = (int)dom_doc_bytes(root->doc) + 1024;
    if (raw_cap < 4096) raw_cap = 4096;
    char *raw = malloc((size_t)raw_cap);
    if (!raw) return NULL;
    int raw_len = collect_inline_sheets(root, raw, 0, raw_cap);
    raw[raw_len] = 0;
    if (raw_len <= 0) { free(raw); return NULL; }

    /* var() substitution can only grow the text, and by an amount bounded by
     * the declarations it is substituting FROM -- which are in the same text.
     * 4x plus a floor is the same shape of headroom browser.c's static gives,
     * sized to the input instead of to the largest page imagined. */
    int exp_cap = raw_len * 4 + 4096;
    char *exp = malloc((size_t)exp_cap);
    if (!exp) { *out_len = raw_len; return raw; }
    int exp_len = css_expand_vars(raw, raw_len, exp, exp_cap);
    if (exp_len <= 0) { free(exp); *out_len = raw_len; return raw; }
    free(raw);
    exp[exp_len < exp_cap ? exp_len : exp_cap - 1] = 0;
    *out_len = exp_len;
    return exp;
}

void css_ensure_styled(struct node *n)
{
#ifdef CSS_NEGCTL_NOFLUSH
    /* The negative control for THIS mechanism, in tests/csstyle.mk. The engine
     * is otherwise whole -- every accessor is wired, every serialiser is
     * canonical, nothing throws -- and every computed read answers "" because
     * the cascade was never run. That is the state the tree was in, and a
     * suite that cannot tell it from a working one is not measuring the fix.
     *
     * It is the SECOND control and not the only one: on its own it is
     * "remove the getters", which any suite catches. The one that matters is
     * CSSOM_NEGCTL_SERIALIZE -- flush intact, every property answering, and
     * only the bytes wrong. */
    (void)n; return;
#else
    if (!n || !n->doc) return;
    struct node *root = n;
    while (root->parent) root = root->parent;

    ui_sync();
    int ui_dirty = g_auto_ui_revision != ui_revision;
    int fresh = (g_auto_ready && g_auto_root == root);
    /* Passive documents have no script mutation owner. The live realm's
     * invalidation roots may name its own nodes; never restyle those while a
     * child selection context is active. The embedder explicitly restyles a
     * passive document after resource/viewport changes. */
    int dirty = !g_css_passive && LOGIT_HAVE(js_dom_dirty) ? js_dom_dirty() : 0;
    if (fresh && !dirty && !ui_dirty) return;

    /* Dirty, but possibly dirty from a mutation we have already cascaded --
     * the embedder owns the flag and has not cleared it yet. See the note
     * above inval_fingerprint for why this may over-report and must never
     * under-report. */
    unsigned long fp = 0;
    if (fresh) {
        fp = inval_fingerprint(root);
        if (!ui_dirty && fp && fp == g_auto_fp) return;
    }

    if (!fresh) {
        /* First style pass for this document, and nobody supplied the CSS. */
        if (g_auto_owned) { free(g_auto_owned); g_auto_owned = NULL; }
        int len = 0;
        g_auto_owned = build_author_css(root, &len);
        g_auto_flushes++;
        css_apply(root, g_auto_owned, len);      /* records g_auto_* */
        if(g_post_pass)g_post_pass(root,g_auto_owned,len);
        g_auto_fp = inval_fingerprint(root);
        return;
    }

    /* Styled once, and something has been mutated since the embedder last
     * looked. Re-run the cascade over the marked scopes when js_dom named
     * them, and over the document when it could not. */
    g_auto_flushes++;
    g_auto_fp = fp;
    int nroots = LOGIT_HAVE(js_dom_inval_roots) ? js_dom_inval_roots() : 0;
    if (!ui_dirty && nroots > 0 && LOGIT_HAVE(js_dom_inval_root)) {
        int all = 0;
        for (int i = 0; i < nroots; i++) {
            int sibs = 0;
            struct node *r = js_dom_inval_root(i, &sibs);
            if (!r) { all = 1; break; }          /* destroyed: scope is unknown */
            css_apply_scoped(r, sibs, g_auto_css, g_auto_len);
        }
        if (!all) return;
    }
    css_apply(root, g_auto_css, g_auto_len);
    if(g_post_pass)g_post_pass(root,g_auto_css,g_auto_len);
#endif
}

int css_style_flushes(void) { return g_auto_flushes; }

/* ======================================================================
 * Scoped re-style
 *
 * The whole reason this exists: a live page mutates ONE element per tick, and
 * re-running the cascade over every element in the document to find that out
 * is the difference between a setInterval a machine can keep up with and one it
 * cannot. css_apply is still the right thing for a load or a new sheet set --
 * this is the incremental path.
 * ====================================================================== */

/* Snapshot of the styles in scope, taken BEFORE the cascade runs so the caller
 * can be told whether anything actually moved.
 *
 * It is a snapshot rather than a compare-on-write inside style_node because of
 * css_extra: that post-pass patches node->style AFTER the cascade (radius,
 * grid tracks, the animation end-state), so a diff taken inside style_node
 * would see every one of those patches as a change on every single re-style
 * and the mechanism would report "changed" forever. The caller registers it
 * through css_set_post_pass and it runs inside the measured window. */
struct snapent { struct node *n; struct cstyle s; unsigned char had; };
static struct snapent *g_snap;
static int g_snapn, g_snapcap;
static int g_snap_over;                 /* scope bigger than the cap: give up measuring */

/* Above this the scope is not "one element and its subtree" any more, it is a
 * document re-style wearing a hat -- and holding a copy of every style would
 * cost more than the pass it is measuring. Past it we stop recording and answer
 * conservatively (CHANGED_LAYOUT), which is exactly what css_apply does anyway. */
#define SNAP_MAX 1024

void css_set_post_pass(void (*fn)(struct node *root, const char *css, int len))
{ g_post_pass = fn; }

static int snap_push(struct node *n)
{
    if (g_snapn >= SNAP_MAX) { g_snap_over = 1; return 0; }
    if (g_snapn == g_snapcap) {
        int ncap = g_snapcap ? g_snapcap * 2 : 64;
        struct snapent *ns = kmalloc((unsigned long)ncap * sizeof *ns);
        if (!ns) { g_snap_over = 1; return 0; }
        for (int i = 0; i < g_snapn; i++) ns[i] = g_snap[i];
        if (g_snap) kfree(g_snap);
        g_snap = ns; g_snapcap = ncap;
    }
    struct snapent *e = &g_snap[g_snapn++];
    e->n = n;
    e->had = n->style ? 1 : 0;
    /* memcpy, not struct assignment: the comparison below is a memcmp over the
     * whole object, and only a byte copy is guaranteed to carry the padding
     * across (a member-wise copy may leave the destination's padding
     * indeterminate, and then every diff would report a change). */
    if (e->had) memcpy(&e->s, n->style, sizeof e->s);
    return 1;
}

/* The scope walk, used three times over (snapshot, cascade, diff) and therefore
 * written once: `n`'s subtree, then -- if `siblings` -- each following element
 * sibling's subtree. Deterministic order, so the snapshot and the diff line up
 * positionally without a second lookup table. */
static void snap_subtree(struct node *n)
{
    if (n->type == N_ELEM && !snap_push(n)) return;
    for (struct node *c = n->first_child; c; c = c->next) snap_subtree(c);
}

static void snap_scope(struct node *n, int siblings)
{
    snap_subtree(n);
    if (siblings)
        for (struct node *s = n->next; s; s = s->next)
            if (s->type == N_ELEM) snap_subtree(s);
}

/* Which half of the style moved?
 *
 * memcmp answers "anything at all" -- both blocks are memset to 0 before being
 * filled, so their padding bytes are identical and the compare is well defined.
 * To answer the narrower "anything LAYOUT reads", copy the old style, overwrite
 * exactly the fields only the PAINTER looks at, and compare again: if the
 * copies now agree, no box moved and no box changed size.
 *
 * Writing it as "overwrite the paint fields" rather than "compare the layout
 * fields" is deliberate -- a field added to struct cstyle and forgotten here
 * defaults to being treated as layout-affecting, which is the safe direction. */
static int cstyle_diff(const struct cstyle *a, const struct cstyle *b)
{
    if (!a || !b) return CSS_CHANGED_LAYOUT;
    /* Snapshot pointers may refer to the allocation just freed by style_node.
     * Never dereference them or infer equality from allocator address reuse:
     * attr() text can change while every prefix byte stays identical. */
    if (a->generated[0] || a->generated[1] || b->generated[0] || b->generated[1])
        return CSS_CHANGED_LAYOUT;
    if (memcmp(a, b, sizeof *a) == 0) return CSS_CHANGED_NONE;
    struct cstyle t;
    memcpy(&t, a, sizeof t);                    /* byte copy: see snap_push */
    t.color = b->color;
    t.background = b->background; t.has_bg = b->has_bg; t.bg_alpha = b->bg_alpha;
    for (int i = 0; i < 4; i++) {
        t.border_color[i] = b->border_color[i];
        t.border_style[i] = b->border_style[i];
    }
    t.underline = b->underline; t.strike = b->strike; t.overline = b->overline;
    t.opacity = b->opacity;
    t.hidden = b->hidden; t.op0 = b->op0; t.vis_hid = b->vis_hid;
    t.backface_hidden=b->backface_hidden;t.preserve_3d=b->preserve_3d;
    for (int i = 0; i < 4; i++) { t.radius[i] = b->radius[i]; t.radius_pct[i] = b->radius_pct[i]; }
    t.z_index = b->z_index; t.has_z = b->has_z;
    t.anim = b->anim; t.trans_op = b->trans_op;
    return memcmp(&t, b, sizeof t) == 0 ? CSS_CHANGED_PAINT : CSS_CHANGED_LAYOUT;
}

/* The document element (<html>): the reference `rem` resolves against and the
 * style LibCSS wants in g_unit.root_style. A scoped pass never visits it, so it
 * has to be re-published by hand before style_node runs. */
static struct node *doc_element_of(struct node *n)
{
    struct node *r = n;
    while (r->parent && r->parent->type == N_ELEM) r = r->parent;
    return r;
}

int css_apply_scoped(struct node *n, int siblings, const char *page_css, int page_len)
{
    if (!n || n->type != N_ELEM) return CSS_CHANGED_NONE;
    if (!g_ctx) css_init();
    if (!g_ctx) return CSS_CHANGED_LAYOUT;

    /* An unstyled ancestor means no full pass has run for this sheet set, so
     * there is nothing to resume from: tell the caller to do the whole thing. */
    struct node *pe = n->parent;
    while (pe && pe->type != N_ELEM) pe = pe->parent;
    if (pe && !pe->computed) return CSS_CHANGED_LAYOUT;

    g_allow_quirks = n->doc && dom_doc_quirks(n->doc) == QM_QUIRKS;
    set_quirks_sheet(g_allow_quirks ? 1 : 0);

    g_snapn = 0; g_snap_over = 0;
    snap_scope(n, siblings);

    css_stylesheet *author = NULL;
    if (page_css && page_len > 0) {
        author = author_sheet(page_css, (size_t)page_len, g_allow_quirks);
        if (author) css_select_ctx_append_sheet(g_ctx, author, CSS_ORIGIN_AUTHOR, NULL);
    }

    /* Re-publish the root reference. style_node only sets these when it styles
     * the root itself, and a scoped pass starts below it -- without this every
     * `rem` in the scope would resolve against whatever the last pass left, or
     * against 16 if this is the first mutation after a fresh css_apply. */
    struct node *docel = doc_element_of(n);
    g_unit.root_style = (const css_computed_style *)docel->computed;
    if (docel->style) g_root_px = ((struct cstyle *)docel->style)->font_px;

    nd_reset();                              /* the node-data cache is pass-scoped */
    const css_computed_style *parent_eff = pe ? (const css_computed_style *)pe->computed : NULL;
    int parent_font = (pe && pe->style) ? ((struct cstyle *)pe->style)->font_px : 16;

    style_node(n, parent_eff, parent_font);
    if (siblings)
        for (struct node *s = n->next; s; s = s->next)
            if (s->type == N_ELEM) style_node(s, parent_eff, parent_font);

    nd_reset();
    g_unit.root_style = NULL;

    if (author) css_select_ctx_remove_sheet(g_ctx, author);   /* cached; see author_sheet */

    /* css_extra's patches belong to the style being measured, so they land
     * before the diff, not after it. */
    if (g_post_pass) {
        g_post_pass(n, page_css, page_len);
        if (siblings)
            for (struct node *s = n->next; s; s = s->next)
                if (s->type == N_ELEM) g_post_pass(s, page_css, page_len);
    }

    if (g_snap_over) return CSS_CHANGED_LAYOUT;

    int changed = CSS_CHANGED_NONE;
    for (int i = 0; i < g_snapn; i++) {
        struct snapent *e = &g_snap[i];
        if (!e->had || !e->n->style) { changed |= CSS_CHANGED_LAYOUT; break; }
        changed |= cstyle_diff(&e->s, (struct cstyle *)e->n->style);
        if (changed & CSS_CHANGED_LAYOUT) break;      /* nothing coarser to learn */
    }
    return changed;
}

/* ======================================================================
 * CSS.supports() -- the CONDITION, answered by the parser that would have to
 * honour it.
 *
 * WPT feature-detects with this constantly, and a page's first line is often
 * `if (!CSS.supports('...')) return;`. The temptation is to answer it from a
 * property-name table, which is wrong in the direction that hurts most: a
 * table says `width: banana` is supported, because the NAME is in it, and a
 * page that feature-detects a VALUE then takes a branch this engine cannot
 * render.
 *
 * So the question is put to LibCSS itself: parse the declaration as a
 * one-declaration inline sheet and watch the drop hook the parser already
 * carries (`css__parse_drop_report`, third_party/css/.../parse/language.h).
 * CSS_DROP_ACCEPTED means the value handler took it. That is the same code
 * path the cascade runs, so supports() and the cascade cannot disagree --
 * which is the whole point, and the reason this lives here next to make_sheet
 * rather than in the JS bindings.
 *
 * The hook is a global, so this saves and restores whatever was installed
 * (css_audit.c installs its own reporter over the same slot).
 *
 * KNOWN UNDER-REPORT, and it is the honest direction: the handful of
 * properties css_extra.c honours BEHIND LibCSS's back (border-radius, the
 * grid track shorthands, animation/transition end-state) are answered `false`
 * here, because LibCSS drops them and the drop is what this reads. Saying
 * "no" about something partially supported costs a page its enhanced branch;
 * saying "yes" about something unparseable costs it the whole layout. If
 * css_extra's set ever stops being a handful, the fix is to teach LibCSS the
 * property (patch the vendored parser), not to special-case the list here --
 * that is what keeps supports() and the cascade the same answer.
 * ====================================================================== */

/* ======================================================================
 * THE PARSERS BESIDE LibCSS, AND WHY CONSULTING THEM CANNOT BREAK ANYTHING
 *
 * The comment above ends "if css_extra's set ever stops being a handful, the
 * fix is to teach LibCSS the property, not to special-case the list here".
 * That is still the rule for the CASCADE and this does not change it. What
 * changed is that there are now two real parsers on this side of the vendored
 * line -- canon.c (a specified-value parser for the properties LibCSS never
 * gained: the anchor family, position-area, the logical box family, the colour
 * functions, and <track-list> for grid) and css_interp.c (a <transform-list>
 * parser) -- and `CSS.supports` is not the cascade. It asks one question:
 * DOES THIS DECLARATION PARSE. A parser that can answer it is a legitimate
 * source for it whether or not the cascade can act on the result.
 *
 * THE SHAPE THAT MAKES THIS SAFE IS THAT IT ONLY EVER TURNS false INTO true.
 * This function is consulted BEFORE LibCSS and can only return 1; every path
 * that does not return 1 falls through to exactly the code that ran before.
 * So no declaration LibCSS accepts today can be refused by anything here, and
 * the three-way contract's CSS_CANON_PASS -- "not mine", which is most of CSS
 * -- is a fallthrough by construction rather than by care.
 *
 * WHY IT MATTERS FAR OUT OF PROPORTION TO CSS.supports() ITSELF: 35,708 of the
 * 47,140 interpolation-testcommon.js failures in css/ are a false answer here,
 * 30,497 of them over property names LibCSS does not know at all. And the
 * CSSOM's setter asks the same function, so a property that answers false is
 * additionally UNSETTABLE from script -- its own parser never runs and every
 * test of it fails on "property should be set". That is the same defect the
 * named-property set had, one layer down.
 *
 * A CSS-WIDE KEYWORD is the one value that is valid for every property that
 * exists, and it is here for a specific test rather than for tidiness:
 * css-conditional/js/CSS-supports-CSSStyleDeclaration.html asserts, for ~600
 * names, that CSS.supports(prop, "inherit") equals whether el.style has an IDL
 * attribute for prop. js_dom.c publishes canon.c's enumeration, so without
 * this line every canon-only property is a fresh disagreement in that file --
 * publishing a property and then denying it exists.
 * ====================================================================== */

extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

static int sup_ieq(const char *a, int alen, const char *b)
{
    int i;
    for (i = 0; i < alen && b[i]; i++)
        if ((a[i] | 32) != (b[i] | 32)) return 0;
    return i == alen && !b[i];
}

/* inherit | initial | unset | revert | revert-layer -- valid for every
 * property, by definition, whoever owns the property. */
static int sup_css_wide(const char *v, int n)
{
    return sup_ieq(v, n, "inherit") || sup_ieq(v, n, "initial") ||
           sup_ieq(v, n, "unset")   || sup_ieq(v, n, "revert")  ||
           sup_ieq(v, n, "revert-layer");
}

/* 1 only when a parser on this side takes the declaration outright. Never 0
 * meaning "invalid" -- 0 here means "no opinion", and the caller goes on to
 * ask LibCSS exactly as it did before. */
static int sup_beside_libcss(const char *prop, int plen, const char *value, int vlen)
{
#ifdef CSSSUP_LIBCSS_ONLY
    /* The negative control: the answer as it was, LibCSS's alone. See
     * tests/cssom.mk's test-cssd-canon-negctl. */
    (void)prop; (void)plen; (void)value; (void)vlen;
    return 0;
#else
    char buf[1024];
    int len = 0;

#ifndef CSS_PHYSICAL_SPACING_LEGACY
    if(LOGIT_HAVE(css_box_supports_decl) && css_box_supports_decl(prop,plen,value,vlen))return 1;
#endif

    if(LOGIT_HAVE(css_facing_keyword) &&
       (sup_ieq(prop,plen,"backface-visibility") || sup_ieq(prop,plen,"transform-style")))
        return css_facing_keyword(value,vlen,sup_ieq(prop,plen,"transform-style"))>=0;

    if (css_canon_decl(prop, plen, value, vlen, buf, (int)sizeof buf, &len)
            == CSS_CANON_OK)
        return 1;
    if (css_canon_knows_property(prop, plen) && sup_css_wide(value, vlen))
        return 1;
#ifndef CSSSUP_NO_TRANSFORM
    /* `transform` is LibCSS's by NAME and nobody's by VALUE: the string table
     * carries it, every function value is dropped, and css_extra.c honours the
     * declaration behind LibCSS's back -- so the cascade already acts on a
     * value this function called unsupported, and the CSSOM setter threw the
     * same value away before it got there. css_interp.c parses the list
     * properly (and is what computes it), so ask it. The em/rem bases are the
     * initial font-size: whether a value PARSES does not depend on them, and
     * CSS.supports has no element to take them from. */
    if (LOGIT_HAVE(ci_transform_parse_context) && sup_ieq(prop, plen, "transform")) {
        struct ci_xform t;
        /* These are syntax-validation bases only. Computed and used values
         * are resolved later with the owning element and document's metrics. */
        const struct ci_length_context context={16,16,20,20};
        if (ci_transform_parse_context(value,vlen,&context,&t) == 0) return 1;
    }
#endif
    return 0;
#endif
}

static int g_sup_seen;          /* a declaration reached the reporter at all */
static int g_sup_ok;            /* ... and was ACCEPTED */

static void sup_report(const char *name, size_t nlen, int reason)
{
    (void)name; (void)nlen;
    g_sup_seen = 1;
    if (reason == 3 /* CSS_DROP_ACCEPTED */) g_sup_ok = 1;
}

int css_supports_decl(const char *prop, int plen, const char *value, int vlen)
{
    if (!prop || !value) return 0;
    if (plen < 0) plen = (int)strlen(prop);
    if (vlen < 0) vlen = (int)strlen(value);
    while (plen && (prop[0] == ' ' || prop[0] == '\t' || prop[0] == '\n')) { prop++; plen--; }
    while (plen && (prop[plen-1] == ' ' || prop[plen-1] == '\t' || prop[plen-1] == '\n')) plen--;
    while (vlen && (value[0] == ' ' || value[0] == '\t' || value[0] == '\n')) { value++; vlen--; }
    while (vlen && (value[vlen-1] == ' ' || value[vlen-1] == '\t' || value[vlen-1] == '\n')) vlen--;
    if (plen <= 0 || vlen <= 0) return 0;
    /* A custom property is supported by definition: --x takes any value that
     * is a balanced token stream, and the cascade stores it verbatim. */
    if (plen >= 2 && prop[0] == '-' && prop[1] == '-') return 1;
    /* The parsers beside LibCSS, first and additively -- see the block comment
     * above sup_beside_libcss(). A 0 here is "no opinion", not "invalid", and
     * falls through to the LibCSS path unchanged. */
    if (sup_beside_libcss(prop, plen, value, vlen)) return 1;
    /* An empty value, or one that is only whitespace/`!important`, is not a
     * declaration. Reject before the parser sees it: LibCSS would report
     * nothing at all and "nothing reported" is our not-supported answer, but
     * being explicit here keeps the two reasons distinguishable. */

    return css_native_supports_decl(prop,plen,value,vlen);
}

/* Used only when the bounded box grammar declines a declaration. Asking
 * LibCSS directly avoids recursing through the extension supports hook and
 * preserves existing native values without duplicating its unit grammar. */
int css_native_supports_decl(const char *prop,int plen,const char *value,int vlen)
{
    if(!prop || !value || plen<=0 || vlen<=0)return 0;
    int need = plen + vlen + 2;
    char stackbuf[256];
    char *decl = (need <= (int)sizeof stackbuf) ? stackbuf : (char *)kmalloc((size_t)need);
    if (!decl) return 0;
    memcpy(decl, prop, (size_t)plen);
    decl[plen] = ':';
    memcpy(decl + plen + 1, value, (size_t)vlen);
    decl[plen + 1 + vlen] = 0;

    void (*saved)(const char *, size_t, int) = css__parse_drop_report;
    g_sup_seen = 0; g_sup_ok = 0;
    css__parse_drop_report = sup_report;
    css_stylesheet *s = make_sheet(decl, (size_t)(plen + 1 + vlen), true, false);
    css__parse_drop_report = saved;
    if (sup_ieq(prop, plen, "content")) {
        /* The old syntax-only answer said YES to counter()/url(), while the
         * actual generator refused both. Read the compiled declaration before
         * destroying its owner, exactly as @supports does in language.c. */
        const css_rule *r = s ? s->rule_list : NULL;
        const css_style *st = r && r->type == CSS_RULE_SELECTOR ?
            ((const css_rule_selector *)r)->style : NULL;
        g_sup_ok = g_sup_ok && st && logit_css_generated_supports(st->bytecode, st->used);
    }
    if (s) css_stylesheet_destroy(s);
    if (decl != stackbuf) kfree(decl);
    return g_sup_seen && g_sup_ok;
}

/* ======================================================================
 * THE PROPERTY UNIVERSE -- every property NAME this engine's parser knows.
 *
 * The CSSOM says a CSSStyleDeclaration exposes one IDL attribute per
 * *supported CSS property*, and WPT holds that to the letter: the whole of
 * css-conditional/js/CSS-supports-CSSStyleDeclaration.html does nothing but
 * assert, for ~600 property names, that
 *
 *     CSS.supports(prop, "inherit")  ==  camelCase(prop) in element.style
 *
 * -- an AGREEMENT test. It does not care how much CSS we implement; it cares
 * that the two answers come from the same set. Answering it from a hand-kept
 * list would be answering it from a THIRD source, which is how the two get to
 * disagree again the first time LibCSS gains a property.
 *
 * So the list is LibCSS's own. `stringmap` is the parser's string table and
 * FIRST_PROP..LAST_PROP is exactly the property-name span of it, both out of
 * the vendored headers -- so a property added to the parser (the css-color and
 * anchor-position work in flight is doing precisely that) shows up here, and
 * on every CSSStyleDeclaration, with no edit to this file.
 *
 * `stringmap` is not declared in propstrings.h -- it is a plain global in
 * propstrings.c. The extern below restates its element type, and the
 * compile-time check under it is what stops that restatement from rotting.
 * ====================================================================== */

#include "parse/propstrings.h"

struct css_smap_entry { const char *data; size_t len; };
extern const struct css_smap_entry stringmap[];

int css_known_prop_count(void)
{
    return (int)(LAST_PROP - FIRST_PROP + 1);
}

const char *css_known_prop_at(int i, int *len)
{
    if (i < 0 || i >= css_known_prop_count()) { if (len) *len = 0; return 0; }
    const struct css_smap_entry *e = &stringmap[FIRST_PROP + i];
    if (len) *len = (int)e->len;
    return e->data;
}

/* ======================================================================
 * "Serialize a CSS value" -- CSSOM 6.7.2. The BYTES, not the meaning.
 *
 * WPT compares strings. A correctly computed value emitted in the wrong form
 * is a failure, and this is the class of bug an implementation ships with
 * because everything WORKS -- `color: #0000ff` paints blue whichever way it
 * reads back out. css/CSS2/syntax/colors-007.html is 1,192 subtests of
 * exactly this and nothing else.
 *
 * WHAT THIS IS NOT. It is not a parser and must not become one: css.h is
 * emphatic that a second CSS scanner is how this engine ends up with two
 * answers to one question, and css_supports_decl() exists so VALIDITY has
 * exactly one source (LibCSS's own value handlers). This function decides
 * FORM, never validity -- when it does not recognise the shape in front of
 * it, it copies it through untouched. There is no input for which it answers
 * "invalid"; a caller that needs that asks css_supports_decl().
 *
 * It is IDEMPOTENT by construction, which is what lets one function serve
 * both the specified value (`el.style.color`, source bytes) and the computed
 * one (`getComputedStyle(el).color`, already canonical out of
 * css_computed_text) without the caller having to say which it is holding.
 * WPT checks that directly: test_valid_value re-assigns the value it read
 * back and requires the second read to equal the first.
 * ====================================================================== */

static int vs_ws(char c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int vs_dig(char c) { return c >= '0' && c <= '9'; }
static int vs_alpha(char c) { char l = (char)(c | 32); return l >= 'a' && l <= 'z'; }
static int vs_hex(char c)
{ return vs_dig(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static int vs_hexv(char c)
{ return vs_dig(c) ? c - '0' : (c | 32) - 'a' + 10; }

struct vsb { char *p; int len, cap; };

static void vsb_ch(struct vsb *b, char c)
{ if (b->len < b->cap - 1) b->p[b->len++] = c; }

static void vsb_str(struct vsb *b, const char *s, int n)
{ for (int i = 0; i < n; i++) vsb_ch(b, s[i]); }

static void vsb_int(struct vsb *b, int v)
{
    char t[16]; int k = 0;
    if (v < 0) { vsb_ch(b, '-'); v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) vsb_ch(b, t[--k]);
}

/* The alpha channel, in THOUSANDTHS, trailing zeros trimmed.
 *
 * The unit matters and it is the whole of what was wrong with the first
 * version, which quantised alpha to a byte and printed the shortest decimal
 * that round-tripped through it. That gives `rgba(0, 0, 0, 0.5)` for
 * `#00000080`, because 0.5 * 255 rounds back to 128 -- and every browser
 * prints `0.502` there, while still printing `0.5` for an authored
 * `rgba(5, 7, 10, 0.5)`. Both are true at once only if alpha is NOT a byte:
 * a hex alpha is 128/255 = 0.50196..., an authored one is exactly what was
 * written, and three decimal places tells them apart. So alpha travels as
 * 0..1000 from wherever it was read, and the byte is only ever an input.
 *
 * The expected bytes here are transcribed from what browsers produce, not
 * derived -- which is the only way to get a serialization rule right, because
 * the rule is defined by what WPT's expectations were recorded against. */
static void vsb_alphav(struct vsb *b, int a1000)
{
    if (a1000 >= 1000) { vsb_ch(b, '1'); return; }
    if (a1000 <= 0)    { vsb_ch(b, '0'); return; }
    int at = b->len;
    vsb_ch(b, '0'); vsb_ch(b, '.');
    vsb_ch(b, (char)('0' + (a1000 / 100) % 10));
    vsb_ch(b, (char)('0' + (a1000 / 10) % 10));
    vsb_ch(b, (char)('0' + a1000 % 10));
    while (b->len > at + 2 && b->p[b->len - 1] == '0') b->len--;
}

static void vsb_colour(struct vsb *b, int r, int g, int bl, int a1000)
{
    int alpha = (a1000 < 0) ? 1000 : a1000;
#ifdef CSSOM_NEGCTL_SERIALIZE
    /* The other end of the negative control in tests/cssom.mk. ob_color()
     * above sabotages the COMPUTED serialisation; this sabotages the
     * SPECIFIED one. They are separate code paths reached by separate calls,
     * so a control that covered only the computed half would sail straight
     * past an el.style that hands the author's hex back unchanged -- which is
     * exactly the bug this whole file exists to have fixed. */
    {
        static const char HEXD[] = "0123456789abcdef";
        int v[3];
        v[0] = r; v[1] = g; v[2] = bl;
        vsb_ch(b, '#');
        for (int i = 0; i < 3; i++) {
            int c = v[i] < 0 ? 0 : (v[i] > 255 ? 255 : v[i]);
            vsb_ch(b, HEXD[(c >> 4) & 0xF]);
            vsb_ch(b, HEXD[c & 0xF]);
        }
        (void)alpha;
        return;
    }
#endif
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (bl < 0) bl = 0; if (bl > 255) bl = 255;
    if (alpha >= 1000) vsb_str(b, "rgb(", 4);
    else               vsb_str(b, "rgba(", 5);
    vsb_int(b, r); vsb_str(b, ", ", 2);
    vsb_int(b, g); vsb_str(b, ", ", 2);
    vsb_int(b, bl);
    if (alpha < 1000) { vsb_str(b, ", ", 2); vsb_alphav(b, alpha); }
    vsb_ch(b, ')');
}

/* ---- numbers ----
 *
 * `.5` -> `0.5`, `+0` -> `0`, `1.0` -> `1`, `-0px` -> `0px`. The UNIT is never
 * touched and never dropped: CSSOM keeps `0px` as `0px` in a specified value,
 * and a serializer that helpfully shortened it to `0` would fail
 * cssom/serialize-values.html on the one case it looks most obviously right.
 *
 * Returns input bytes consumed, 0 if this is not a number. */
static int vs_number(struct vsb *b, const char *s, int n)
{
    int i = 0, neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = (s[i] == '-'); i++; }
    int int0 = i;
    while (i < n && vs_dig(s[i])) i++;
    int int1 = i, frac0 = i, frac1 = i;
    if (i < n && s[i] == '.' && i + 1 < n && vs_dig(s[i + 1])) {
        i++; frac0 = i;
        while (i < n && vs_dig(s[i])) i++;
        frac1 = i;
    }
    if (int1 == int0 && frac1 == frac0) return 0;        /* not a number at all */

    /* an exponent, kept verbatim -- rare enough in CSS that reformatting it
     * would be inventing a rule nothing tests */
    int exp0 = i, exp1 = i;
    if (i < n && (s[i] | 32) == 'e') {
        int j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) j++;
        if (j < n && vs_dig(s[j])) { while (j < n && vs_dig(s[j])) j++; exp1 = j; i = j; }
    }

    while (int0 < int1 - 1 && s[int0] == '0') int0++;     /* 007 -> 7  */
    while (frac1 > frac0 && s[frac1 - 1] == '0') frac1--; /* 1.500 -> 1.5 */

    int izero = (int1 <= int0) || (int1 - int0 == 1 && s[int0] == '0');
    if (neg && izero && frac1 == frac0) neg = 0;          /* -0 -> 0, but not -0.5 */

    if (neg) vsb_ch(b, '-');
    if (int1 > int0) vsb_str(b, s + int0, int1 - int0);
    else vsb_ch(b, '0');                                  /* .5 -> 0.5 */
    if (frac1 > frac0) { vsb_ch(b, '.'); vsb_str(b, s + frac0, frac1 - frac0); }
    if (exp1 > exp0) vsb_str(b, s + exp0, exp1 - exp0);
    return i;
}

/* One rgb()/hsl() channel: a number, optionally a percent. `pct_scale` is what
 * 100% means here. Returns bytes consumed, 0 if there is no number. */
static int vs_channel(const char *s, int n, int pct_scale, int *out)
{
    int i = 0;
    while (i < n && vs_ws(s[i])) i++;
    int sign = 1;
    if (i < n && (s[i] == '+' || s[i] == '-')) { sign = (s[i] == '-') ? -1 : 1; i++; }
    int start = i;
    double v = 0;
    while (i < n && vs_dig(s[i])) { v = v * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') {
        i++;
        double f = 0.1;
        while (i < n && vs_dig(s[i])) { v += (s[i] - '0') * f; f *= 0.1; i++; }
    }
    if (i == start) return 0;
    if (i < n && s[i] == '%') { i++; v = v * pct_scale / 100.0; }
    v *= sign;
    *out = (int)(v < 0 ? v - 0.5 : v + 0.5);
    while (i < n && vs_ws(s[i])) i++;
    return i;
}

/* hsl -> rgb: h in degrees, s and l in 0..100.
 *
 * Integer, at 100x -- 0..25500 rather than 0..255. The obvious version works
 * in bytes and gets `hsl(0, 100%, 50%)` wrong by ONE: lightness 50% is 127.5,
 * which truncates to 127, and the answer comes out rgb(254, 0, 0) instead of
 * rgb(255, 0, 0). One off by one in a colour is invisible on screen and a
 * failed byte comparison in every test that reads it back, which is the whole
 * hazard this file was written for. */
static void vs_hsl_rgb(int h, int sp, int lp, int *r, int *g, int *b)
{
    h = ((h % 360) + 360) % 360;
    if (sp < 0) sp = 0;
    if (sp > 100) sp = 100;
    if (lp < 0) lp = 0;
    if (lp > 100) lp = 100;
    const int FULL = 25500;                       /* 255 * 100 */
    int l = lp * 255, s = sp * 255;
    int *o[3];
    o[0] = r; o[1] = g; o[2] = b;
    if (s == 0) { *r = *g = *b = (l + 50) / 100; return; }
    int c2 = (l < FULL / 2) ? (int)((long long)l * (FULL + s) / FULL)
                            : (int)(l + s - (long long)l * s / FULL);
    int c1 = 2 * l - c2;
    int t[3];
    t[0] = h + 120; t[1] = h; t[2] = h - 120;
    for (int i = 0; i < 3; i++) {
        int tt = ((t[i] % 360) + 360) % 360;
        int v;
        if (tt < 60)       v = c1 + (int)((long long)(c2 - c1) * tt / 60);
        else if (tt < 180) v = c2;
        else if (tt < 240) v = c1 + (int)((long long)(c2 - c1) * (240 - tt) / 60);
        else               v = c1;
        if (v < 0) v = 0;
        if (v > FULL) v = FULL;
        *o[i] = (v + 50) / 100;
    }
}

/* A hex colour: `#rgb` `#rgba` `#rrggbb` `#rrggbbaa`. Returns bytes consumed
 * including the '#', or 0 if the run of hex digits is not one of those four
 * lengths -- `#00000` is not a colour, and it must reach the output UNCHANGED
 * so that a caller asking about validity still gets LibCSS's answer and not
 * one this function invented. */
static int vs_hex_colour(struct vsb *b, const char *s, int n)
{
    if (n < 1 || s[0] != '#') return 0;
    int k = 1;
    while (k < n && vs_hex(s[k])) k++;
    int d = k - 1;
    if (k < n && (vs_alpha(s[k]) || vs_dig(s[k]) || s[k] == '_' || s[k] == '-')) return 0;
    if (d != 3 && d != 4 && d != 6 && d != 8) return 0;
    const char *h = s + 1;
    int v[4];
    v[0] = v[1] = v[2] = 0; v[3] = 255;
    if (d <= 4) {
        for (int i = 0; i < d; i++) v[i] = vs_hexv(h[i]) * 17;
    } else {
        for (int i = 0; i < d / 2; i++) v[i] = vs_hexv(h[i * 2]) * 16 + vs_hexv(h[i * 2 + 1]);
    }
    /* the byte is an input, not the representation: 0x80 is 0.502, not 0.5 */
    vsb_colour(b, v[0], v[1], v[2], (v[3] * 1000 + 127) / 255);
    return k;
}

/* ASCII-case-insensitive `name(`, matched at s. */
static int vs_is_func(const char *s, int n, const char *name)
{
    int L = (int)strlen(name);
    if (n < L + 1 || s[L] != '(') return 0;
    for (int i = 0; i < L; i++)
        if ((s[i] | 32) != name[i]) return 0;
    return 1;
}

/* The ')' closing the '(' at s[at], honouring nesting and strings. */
static int vs_close(const char *s, int n, int at)
{
    int depth = 0;
    for (int i = at; i < n; i++) {
        char c = s[i];
        if (c == '"' || c == '\'') {
            char q = c; i++;
            while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')') { depth--; if (depth == 0) return i; }
    }
    return -1;
}

static void vsb_quoted(struct vsb *b, const char *s, int n)
{
    vsb_ch(b, '"');
    for (int i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') vsb_ch(b, '\\');
        vsb_ch(b, s[i]);
    }
    vsb_ch(b, '"');
}

int css_value_serialize(const char *val, int vlen, char *out, int outmax)
{
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    if (!val) return 0;
    if (vlen < 0) vlen = (int)strlen(val);
    while (vlen && vs_ws(val[0])) { val++; vlen--; }
    while (vlen && vs_ws(val[vlen - 1])) vlen--;
    if (vlen <= 0) return 0;

    struct vsb b;
    b.p = out; b.len = 0; b.cap = outmax;
    int i = 0;
    int pending_ws = 0;                 /* whitespace seen, not yet emitted */

    while (i < vlen) {
        char c = val[i];

        if (vs_ws(c)) { pending_ws = 1; i++; continue; }

        /* A comma eats the space before it and always has one after it:
         * `rgb(1 , 2,3)` and `rgb(1, 2, 3)` are one serialization. */
        if (c == ',') {
            while (b.len && b.p[b.len - 1] == ' ') b.len--;
            vsb_ch(&b, ','); vsb_ch(&b, ' ');
            pending_ws = 0; i++;
            continue;
        }
        if (b.len && pending_ws && b.p[b.len - 1] != ' ' && b.p[b.len - 1] != '(')
            vsb_ch(&b, ' ');
        pending_ws = 0;

        if (c == '(') { vsb_ch(&b, '('); i++; continue; }
        if (c == ')') {
            while (b.len && b.p[b.len - 1] == ' ') b.len--;
            vsb_ch(&b, ')'); i++;
            continue;
        }

        if (c == '#') {
            int k = vs_hex_colour(&b, val + i, vlen - i);
            if (k) { i += k; continue; }
        }

        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < vlen && val[j] != c) { if (val[j] == '\\' && j + 1 < vlen) j++; j++; }
            /* A string serializes double-quoted whichever quote the author
             * used. */
            vsb_quoted(&b, val + i + 1, j - i - 1);
            i = (j < vlen) ? j + 1 : vlen;
            continue;
        }

        if (c == '+' || c == '-' || c == '.' || vs_dig(c)) {
            int k = vs_number(&b, val + i, vlen - i);
            if (k) {
                i += k;
                if (i < vlen && val[i] == '%') { vsb_ch(&b, '%'); i++; }
                else while (i < vlen && (vs_alpha(val[i]) || vs_dig(val[i]) ||
                                         val[i] == '_' || val[i] == '-')) {
#ifndef CSSOM_NEGCTL_SERIALIZE
                    vsb_ch(&b, val[i]);   /* the control's second half drops
                                           * the unit: `10px` reads back `10` */
#endif
                    i++;
                }
                continue;
            }
        }

        /* an identifier, possibly a function name */
        int j = i;
        while (j < vlen && (vs_alpha(val[j]) || vs_dig(val[j]) || val[j] == '-' ||
                            val[j] == '_' || (unsigned char)val[j] >= 0x80))
            j++;
        if (j == i) { vsb_ch(&b, c); i++; continue; }

        const char *id = val + i;
        int idn = j - i;
        if (j < vlen && val[j] == '(') {
            int close = vs_close(val, vlen, j);
            int argn  = (close < 0 ? vlen : close) - (j + 1);
            const char *arg = val + j + 1;

            /* url(): the target is a <url> and serializes as a quoted string
             * whether or not the author quoted it. */
            if (vs_is_func(id, vlen - i, "url")) {
                int a0 = 0, a1 = argn;
                while (a0 < a1 && vs_ws(arg[a0])) a0++;
                while (a1 > a0 && vs_ws(arg[a1 - 1])) a1--;
                if (a1 - a0 >= 2 && (arg[a0] == '"' || arg[a0] == '\'') &&
                    arg[a1 - 1] == arg[a0]) { a0++; a1--; }
                vsb_str(&b, "url(", 4);
                vsb_quoted(&b, arg + a0, a1 - a0);
                vsb_ch(&b, ')');
                i = (close < 0) ? vlen : close + 1;
                continue;
            }

            int is_rgb = vs_is_func(id, vlen - i, "rgb") || vs_is_func(id, vlen - i, "rgba");
            int is_hsl = vs_is_func(id, vlen - i, "hsl") || vs_is_func(id, vlen - i, "hsla");
            if ((is_rgb || is_hsl) && close > 0) {
                int p = 0, ch[3], a1000 = 1000, ok = 1;
                ch[0] = ch[1] = ch[2] = 0;
                for (int q = 0; q < 3; q++) {
                    int used = vs_channel(arg + p, argn - p,
                                          is_rgb ? 255 : (q == 0 ? 360 : 100), &ch[q]);
                    if (!used) { ok = 0; break; }
                    p += used;
                    if (q < 2) {
                        while (p < argn && vs_ws(arg[p])) p++;
                        if (p < argn && (arg[p] == ',' || arg[p] == '/')) p++;
                    }
                }
                if (ok) {
                    while (p < argn && vs_ws(arg[p])) p++;
                    if (p < argn && (arg[p] == ',' || arg[p] == '/')) {
                        p++;
                        /* alpha is 0..1, so vs_channel's integer answer is no
                         * use -- read it here, in the small. */
                        double f = 0;
                        int q = p, sign = 1, seen = 0;
                        while (q < argn && vs_ws(arg[q])) q++;
                        if (q < argn && (arg[q] == '+' || arg[q] == '-'))
                            { sign = (arg[q] == '-') ? -1 : 1; q++; }
                        while (q < argn && vs_dig(arg[q])) { f = f * 10 + (arg[q] - '0'); q++; seen = 1; }
                        if (q < argn && arg[q] == '.') {
                            q++;
                            double m = 0.1;
                            while (q < argn && vs_dig(arg[q])) { f += (arg[q] - '0') * m; m *= 0.1; q++; seen = 1; }
                        }
                        if (seen) {
                            if (q < argn && arg[q] == '%') { f /= 100.0; q++; }
                            f *= sign;
                            if (f < 0) f = 0;
                            if (f > 1) f = 1;
                            a1000 = (int)(f * 1000.0 + 0.5);
                            p = q;
                        }
                    }
                    while (p < argn && vs_ws(arg[p])) p++;
                    if (p >= argn) {
                        int r = ch[0], g = ch[1], bl = ch[2];
                        if (is_hsl) vs_hsl_rgb(ch[0], ch[1], ch[2], &r, &g, &bl);
                        vsb_colour(&b, r, g, bl, a1000);
                        i = close + 1;
                        continue;
                    }
                }
            }
            /* every other function: the name, then the arguments through this
             * same pass, so a colour nested inside one is still normalised */
            vsb_str(&b, id, idn);
            vsb_ch(&b, '(');
            if (argn > 0) {
                char tmp[512];
                int tn = css_value_serialize(arg, argn, tmp, (int)sizeof tmp);
                if (tn > 0) vsb_str(&b, tmp, tn);
                else vsb_str(&b, arg, argn);
            }
            vsb_ch(&b, ')');
            i = (close < 0) ? vlen : close + 1;
            continue;
        }

        vsb_str(&b, id, idn);
        i = j;
    }

    while (b.len && b.p[b.len - 1] == ' ') b.len--;
    b.p[b.len] = 0;
    return b.len;
}

/* ======================================================================
 * The specified-value canonicaliser, forwarded.
 *
 * `third_party/css/libcss/include/libcss/canon.h` is the CSS line's
 * specified-value parser: a third entry point into LibCSS's grammar that
 * answers "what did the author write, spelled canonically?" for the
 * properties it claims (the inset/size/margin family and css-anchor-position),
 * and PASSES on everything else.
 *
 * It is forwarded through css.h rather than included directly by the CSSOM
 * because the browser's js_*.c objects build with JS_CF, which carries no
 * CSS_INC -- css_engine.c is the one TU on this side that sees LibCSS's
 * headers, and keeping it that way is the reason every other caller in the
 * app can say `#include "css.h"` and stop.
 *
 * WHY IT IS CALLED AT ALL, rather than letting css_value_serialize() have the
 * whole surface: two serializers over one surface is precisely the failure
 * css.h warns about everywhere else -- they do not fail by being approximate,
 * they fail by DISAGREEING, and the disagreement shows up as a value that
 * changes spelling depending on which path read it. The properties canon.c
 * claims are its answer; everything it passes on is ours. The two sets do not
 * overlap, and this call is what keeps them from starting to.
 * ====================================================================== */
int css_specified_canon(const char *prop, int plen, const char *value, int vlen,
                        char *out, int outcap, int *outlen)
{
    return css_canon_decl(prop, plen, value, vlen, out, outcap, outlen);
}

/* Quiescent style isolation, paired with layout_context. Holding only computed
 * nodes is insufficient: automatic CSSOM flush remembers the author buffer,
 * the parse cache owns that sheet, and extension fields borrow another cache.
 * Each passive context owns all three producers. Process-level parser reporter
 * hooks and the computed-style releaser are immutable services and stay shared. */
extern struct css_extra_context *css_extra_context_create(void) LOGIT_WEAK;
extern struct css_extra_context *css_extra_context_activate(struct css_extra_context *) LOGIT_WEAK;
extern void css_extra_context_destroy(struct css_extra_context *) LOGIT_WEAK;
extern struct css_vars_context *css_vars_context_create(void) LOGIT_WEAK;
extern struct css_vars_context *css_vars_context_activate(struct css_vars_context *) LOGIT_WEAK;
extern void css_vars_context_destroy(struct css_vars_context *) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_extra_context_create);
LOGIT_WEAK_STUB(css_extra_context_activate);
LOGIT_WEAK_STUB(css_extra_context_destroy);
LOGIT_WEAK_STUB(css_vars_context_create);
LOGIT_WEAK_STUB(css_vars_context_activate);
LOGIT_WEAK_STUB(css_vars_context_destroy);
#define CSS_CONTEXT_FIELDS(X) \
    X(ui_hover) \
    X(ui_active) \
    X(ui_focus) \
    X(ui_revision) \
    X(ui_rendered) \
    X(g_css_passive) \
    X(g_target_frag) \
    X(g_target_fraglen) \
    X(g_nd) \
    X(g_ndcap) \
    X(g_ndused) \
    X(g_stat_styled) \
    X(g_stat_hits) \
    X(g_ctx) \
    X(g_ua_sheet) \
    X(g_quirks_sheet) \
    X(g_quirks_appended) \
    X(g_allow_quirks) \
    X(g_unit) \
    X(g_media) \
    X(g_screen_w) \
    X(g_screen_h) \
    X(g_vw) \
    X(g_vh) \
    X(g_scheme) \
    X(g_scheme_dark) \
    X(extra_matches) \
    X(extra_match_count) \
    X(extra_specificity) \
    X(g_author_sheet) \
    X(g_author_src) \
    X(g_author_srclen) \
    X(g_author_quirks) \
    X(g_author_parses) \
    X(g_root_px) \
    X(g_last_custom) \
    X(g_generated_composes) \
    X(g_generated_skips) \
    X(g_auto_root) \
    X(g_auto_css) \
    X(g_auto_len) \
    X(g_auto_owned) \
    X(g_auto_ready) \
    X(g_auto_ui_revision) \
    X(g_auto_flushes) \
    X(g_auto_fp) \
    X(g_snap) \
    X(g_snapn) \
    X(g_snapcap) \
    X(g_snap_over) \
    X(g_post_pass)

struct css_context {
#define CC_FIELD(n) __typeof__(n) n;
    CSS_CONTEXT_FIELDS(CC_FIELD)
#undef CC_FIELD
    struct css_extra_context *extra;
    struct css_vars_context *vars;
};
static struct css_context css_default_context;
static struct css_context *css_active_context;
struct css_context *css_context_create(void)
{
    if(!LOGIT_HAVE(css_extra_context_create)||!LOGIT_HAVE(css_extra_context_activate)||!LOGIT_HAVE(css_extra_context_destroy)||
       !LOGIT_HAVE(css_vars_context_create)||!LOGIT_HAVE(css_vars_context_activate)||!LOGIT_HAVE(css_vars_context_destroy))return 0;
    struct css_context *c=kmalloc(sizeof *c);
    if(!c)return 0;
    memset(c,0,sizeof *c);
    c->extra=css_extra_context_create();c->vars=css_vars_context_create();
    if(!c->extra||!c->vars){css_extra_context_destroy(c->extra);css_vars_context_destroy(c->vars);kfree(c);return 0;}
    c->g_css_passive=1;c->ui_revision=1;c->g_root_px=16;
    c->g_screen_w=g_screen_w;c->g_screen_h=g_screen_h;
    c->g_scheme_dark=g_scheme_dark;
    c->g_post_pass=g_post_pass;
    return c;
}
struct css_context *css_context_activate(struct css_context *c)
{
    struct css_context *previous=css_active_context;
    if(previous==c)return previous;
#ifndef CSS_CONTEXT_LEGACY_SHARED
    struct css_context *save=previous?previous:&css_default_context;
    struct css_context *next=c?c:&css_default_context;
#define CC_SAVE(n) memcpy(&save->n,&n,sizeof n);
    CSS_CONTEXT_FIELDS(CC_SAVE)
#undef CC_SAVE
#define CC_LOAD(n) memcpy(&n,&next->n,sizeof n);
    CSS_CONTEXT_FIELDS(CC_LOAD)
#undef CC_LOAD
    css_extra_context_activate(c?c->extra:0);
    css_vars_context_activate(c?c->vars:0);
#endif
    /* Inactive documents keep their own style revision, but the preference
     * belongs to the system. Invalidate on activation before replacing the
     * saved value; updating only the media value leaves cached styles stale. */
    css_set_reduced_motion(g_reduced_motion);
    css_active_context=c;return previous;
}
void css_context_destroy(struct css_context *c)
{
    if(!c)return;
#ifndef CSS_CONTEXT_LEGACY_SHARED
    struct css_context *previous=css_context_activate(c);
    nd_reset();
    if(g_nd){kfree(g_nd);g_nd=0;g_ndcap=g_ndused=0;}
    if(g_ctx){css_select_ctx_destroy(g_ctx);g_ctx=0;}
    if(g_author_sheet){css_stylesheet_destroy(g_author_sheet);g_author_sheet=0;}
    if(g_ua_sheet){css_stylesheet_destroy(g_ua_sheet);g_ua_sheet=0;}
    if(g_quirks_sheet){css_stylesheet_destroy(g_quirks_sheet);g_quirks_sheet=0;}
    if(g_author_src){kfree(g_author_src);g_author_src=0;}
    if(g_auto_owned){free(g_auto_owned);g_auto_owned=0;}
    if(g_snap){kfree(g_snap);g_snap=0;}
    if(g_scheme){lwc_string_unref(g_scheme);g_scheme=0;}
    css_context_activate(previous==c?0:previous);
#endif
    css_extra_context_destroy(c->extra);css_vars_context_destroy(c->vars);
    kfree(c);
}
#undef CSS_CONTEXT_FIELDS
