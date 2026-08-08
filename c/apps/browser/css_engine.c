/* M17 L2: LibCSS-backed CSS engine. Implements css.h (css_init/css_apply) on top
 * of NetSurf LibCSS, producing the same `struct cstyle` that net/layout.c reads,
 * so it is a drop-in replacement for net/css.c. */
#include "dom.h"
#include "css.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <libcss/libcss.h>
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
#include "select/propget.h"

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

static css_error h_node_is_lang(void *pw, void *node, lwc_string *lang, bool *match)
{ (void)pw; (void)node; (void)lang; *match = false; return CSS_OK; }

static css_error h_node_presentational_hint(void *pw, void *node,
        uint32_t *nhints, css_hint **hints)
{ (void)pw; (void)node; *nhints = 0; *hints = NULL; return CSS_OK; }

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
    h_node_is_link, h_false /*visited*/, h_false /*hover*/, h_false /*active*/,
    h_false /*focus*/, h_false /*enabled*/, h_false /*disabled*/, h_false /*checked*/,
    h_false /*target*/, h_node_is_lang,
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

static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{ (void)pw; (void)base; *abs = lwc_string_ref(rel); return CSS_OK; }

static const char UA_CSS[] =
    "body{display:block;margin:8px}"
    "div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,pre,header,footer,section,article,nav,main,blockquote,figure,figcaption,table,form{display:block}"
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
    /* white-space: the whole point of <pre> and friends. */
    "pre,xmp,plaintext,listing{white-space:pre}"
    "textarea{white-space:pre-wrap}nobr{white-space:nowrap}"
    "svg{display:inline}"
    "script,style,head,title,meta,link,noscript,template,[hidden]{display:none}";

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
        (len == 0 || memcmp(g_author_src, data, len) == 0))
        return g_author_sheet;

    /* Content differs: drop the old parse before building the new one, so the
     * cache never holds two whole stylesheets at once. */
    if (g_author_sheet) { css_stylesheet_destroy(g_author_sheet); g_author_sheet = NULL; }
    if (g_author_src)   { kfree(g_author_src); g_author_src = NULL; }
    g_author_srclen = 0;

    g_author_parses++;
    css_stylesheet *s = make_sheet(data, len, false, quirks);
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
    css_set_color_scheme(g_scheme_dark);

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

/* clamp absolutised lengths so a giant CSS value can't overflow the int
 * coordinates the layout engine accumulates (signed overflow = UB). */
static int clamp_px(int v) { if (v > 8192) return 8192; if (v < -8192) return -8192; return v; }

static uint32_t to_rgb(css_color c) { return (uint32_t)(c & 0x00FFFFFF); }

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
    case CSS_DISPLAY_INLINE:       o->display = DISP_INLINE; break;
    case CSS_DISPLAY_INLINE_BLOCK: o->display = DISP_INLINE_BLOCK; break;
    case CSS_DISPLAY_LIST_ITEM:    o->display = DISP_BLOCK; o->list_item = 1; break;
    case CSS_DISPLAY_FLEX:         o->display = DISP_FLEX; break;
    case CSS_DISPLAY_INLINE_FLEX:  o->display = DISP_FLEX; break;
    case CSS_DISPLAY_GRID:         o->display = DISP_GRID; break;
    case CSS_DISPLAY_INLINE_GRID:  o->display = DISP_GRID; break;
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

    /* margins (auto -> -1) */
    o->mt = css_computed_margin_top(cs, &len, &unit)    == CSS_MARGIN_AUTO ? -1 : clamp_px(len_px(len, unit, fp, NULL));
    o->mr = css_computed_margin_right(cs, &len, &unit)  == CSS_MARGIN_AUTO ? -1 : clamp_px(len_px(len, unit, fp, NULL));
    o->mb = css_computed_margin_bottom(cs, &len, &unit) == CSS_MARGIN_AUTO ? -1 : clamp_px(len_px(len, unit, fp, NULL));
    o->ml = css_computed_margin_left(cs, &len, &unit)   == CSS_MARGIN_AUTO ? -1 : clamp_px(len_px(len, unit, fp, NULL));

    css_computed_padding_top(cs, &len, &unit);    o->pt = clamp_px(len_px(len, unit, fp, NULL));
    css_computed_padding_right(cs, &len, &unit);  o->pr = clamp_px(len_px(len, unit, fp, NULL));
    css_computed_padding_bottom(cs, &len, &unit); o->pb = clamp_px(len_px(len, unit, fp, NULL));
    css_computed_padding_left(cs, &len, &unit);   o->pl = clamp_px(len_px(len, unit, fp, NULL));

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

    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER:
    case CSS_TEXT_ALIGN_LIBCSS_CENTER: o->text_align = ALIGN_CENTER; break;
    case CSS_TEXT_ALIGN_RIGHT:
    case CSS_TEXT_ALIGN_LIBCSS_RIGHT:  o->text_align = ALIGN_RIGHT; break;
    case CSS_TEXT_ALIGN_JUSTIFY:       o->text_align = ALIGN_JUSTIFY; break;
    default:                           o->text_align = ALIGN_LEFT; break;
    }

    switch (css_computed_line_height(cs, &len, &unit)) {
    case CSS_LINE_HEIGHT_NUMBER:    o->line_px = clamp_px(FIXTOINT(FMUL(len, INTTOFIX(fp)))); break;
    case CSS_LINE_HEIGHT_DIMENSION: o->line_px = clamp_px(len_px(len, unit, fp, NULL)); break;
    default:                        o->line_px = 0; break;   /* normal -> layout derives */
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
        if (css_computed_border_##NAME##_color(cs, &col) == CSS_BORDER_COLOR_COLOR) \
            o->border_color[i] = to_rgb(col); \
        else o->border_color[i] = 0x808080; \
      } }
    EDGE_CONVERT(0, top)
    EDGE_CONVERT(1, right)
    EDGE_CONVERT(2, bottom)
    EDGE_CONVERT(3, left)
#undef EDGE_CONVERT

    { uint8_t td = css_computed_text_decoration(cs);
      if (td & CSS_TEXT_DECORATION_UNDERLINE)    o->underline = 1;
      if (td & CSS_TEXT_DECORATION_LINE_THROUGH) o->strike = 1;
      if (td & CSS_TEXT_DECORATION_OVERLINE)     o->overline = 1; }

    { uint8_t v = css_computed_visibility(cs);
      if (v == CSS_VISIBILITY_HIDDEN || v == CSS_VISIBILITY_COLLAPSE) { o->hidden = 1; o->vis_hid = 1; } }
    { css_fixed op;
      o->opacity = 255;
      if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET) {
          if (op <= 0) { o->hidden = 1; o->op0 = 1; o->opacity = 0; }
          else if (op < F_1) o->opacity = FIXTOINT(FMUL(op, INTTOFIX(255)) + F_0_5);
      } }
    { uint8_t p = css_computed_position(cs);
      switch (p) {
      /* absolute: out of flow (dropdowns/overlays would smear the normal
       * flow). fixed stays in flow -- fixed headers sit at the top anyway, and
       * a viewport-anchored box would need the painter to exempt it from
       * scrolling. sticky is laid out as relative, which is what it is until
       * the scroll offset reaches it. */
      case CSS_POSITION_ABSOLUTE: o->position = POS_ABSOLUTE; o->pos_abs = 1; break;
      case CSS_POSITION_RELATIVE: o->position = POS_RELATIVE; break;
      case CSS_POSITION_FIXED:    o->position = POS_FIXED; break;
      case CSS_POSITION_STICKY:   o->position = POS_STICKY; break;
      default:                    o->position = POS_STATIC; break;
      } }
    /* Box offsets, in any absolute unit. Percentages are skipped rather than
     * misapplied: they resolve against the containing block's size, which the
     * cstyle has no room to defer and layout does not thread through here. */
    { css_fixed v; css_unit u; int p;
      if (css_computed_top(cs, &v, &u) == CSS_TOP_SET)
          { int q = len_px(v, u, fp, &p); if (!p) { o->top = clamp_px(q); o->has_top = 1; } }
      if (css_computed_left(cs, &v, &u) == CSS_LEFT_SET)
          { int q = len_px(v, u, fp, &p); if (!p) { o->left = clamp_px(q); o->has_left = 1; } }
      if (css_computed_right(cs, &v, &u) == CSS_RIGHT_SET)
          { int q = len_px(v, u, fp, &p); if (!p) { o->right = clamp_px(q); o->has_right = 1; } }
      if (css_computed_bottom(cs, &v, &u) == CSS_BOTTOM_SET)
          { int q = len_px(v, u, fp, &p); if (!p) { o->bottom = clamp_px(q); o->has_bottom = 1; } } }

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
    /* space-between/around/evenly only differ from stretch when the container
     * has spare cross space, which for our auto-height containers it never
     * has; they fold onto stretch rather than pretending. */
    default:                              o->align_content = AL_STRETCH; break;
    }
#undef ALIGN_CONVERT
    { css_fixed fg = 0;
      if (css_computed_flex_grow(cs, &fg) == CSS_FLEX_GROW_SET && fg > 0)
          o->flex_grow = fg; }
    { css_fixed fs = F_1;               /* flex-shrink's initial value is 1 */
      if (css_computed_flex_shrink(cs, &fs) != CSS_FLEX_SHRINK_SET) fs = F_1;
      o->flex_shrink = fs < 0 ? 0 : fs; }
    if (css_computed_flex_basis(cs, &len, &unit) == CSS_FLEX_BASIS_SET) {
        int pct; o->flex_basis = clamp_px(len_px(len, unit, fp, &pct));
        o->has_fb = 1; o->fb_pct = pct;
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
};

const char *css_prop_name(int i)
{ return (i >= 0 && i < CSSP__COUNT) ? g_prop_names[i] : 0; }

/* Accepts both spellings a script can hand us. The IDL name is the dashed name
 * with each "-x" folded to "X", so one comparison covers both: walk the dashed
 * name and let the caller's string either supply the '-' or the capital.
 * `cssFloat` is spelled out separately -- `float` was a reserved word when the
 * IDL was written, and pages still use both. */
int css_prop_lookup(const char *name, int len)
{
    if (!name) return -1;
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
    if (a != 0xFF) { ob_s(b, ", "); ob_fixed(b, FDIV(INTTOFIX((int)a), INTTOFIX(255))); }
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

int css_computed_text(struct node *n, int prop, char *out, int outmax)
{
    if (!out || outmax <= 0) return 0;
    out[0] = 0;
    if (!n || n->type != N_ELEM || !n->computed || prop < 0 || prop >= CSSP__COUNT) return 0;
    const css_computed_style *cs = (const css_computed_style *)n->computed;

    struct obuf b = { out, 0, outmax };
    css_fixed len = 0; css_unit unit = CSS_UNIT_PX; css_color col = 0;
    /* An em-valued length on any property BUT font-size resolves against the
     * element's OWN font-size, which convert() already worked out (and clamped)
     * into the cstyle. Only font-size itself would need the parent's, and that
     * one is read straight back out of the cstyle rather than re-derived. */
    int fp = n->style ? ((struct cstyle *)n->style)->font_px : parent_font_of(n);

    switch (prop) {
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

    /* The root element's own style is the reference for every `rem` below it.
     * Publish it before the subtree is walked and before convert() runs on any
     * descendant -- LibCSS reads g_unit.root_style when it absolutises a
     * rem-valued font-size, and px_per_unit() reads g_root_px for every other
     * rem-valued length. The root itself resolved its rem against
     * font_size_default (16), which is exactly what CSS specifies. */
    int is_root = (parent == NULL);

    struct cstyle *o = kmalloc(sizeof *o);
    if (o) {
        memset(o, 0, sizeof *o);
        o->font_px = parent_font;          /* sensible default before convert */
        convert(eff, parent_font, o);
        if (n->style) kfree(n->style);
        n->style = o;
    }
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

    if (composed) css_computed_style_destroy(composed);
    css_select_results_destroy(res);
    if (inl) css_stylesheet_destroy(inl);
}

void css_apply(struct node *root, const char *page_css, int page_len)
{
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
}

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

static void (*g_post_pass)(struct node *root, const char *css, int len);
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
    t.radius = b->radius; t.radius_pct = b->radius_pct;
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

extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

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
    /* An empty value, or one that is only whitespace/`!important`, is not a
     * declaration. Reject before the parser sees it: LibCSS would report
     * nothing at all and "nothing reported" is our not-supported answer, but
     * being explicit here keeps the two reasons distinguishable. */

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
    if (s) css_stylesheet_destroy(s);
    if (decl != stackbuf) kfree(decl);
    return g_sup_seen && g_sup_ok;
}
