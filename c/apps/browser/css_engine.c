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

static css_error h_set_libcss_node_data(void *pw, void *node, void *data)
{ (void)pw; (void)node; (void)data; return CSS_OK; }
static css_error h_get_libcss_node_data(void *pw, void *node, void **data)
{ (void)pw; (void)node; *data = NULL; return CSS_OK; }

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
    "pre{font-family:monospace;margin:8px 0}code{font-family:monospace}"
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

int css_media_width(void) { return g_vw ? g_vw : 760; }

/* ---------- computed style -> struct cstyle ---------- */
static int len_px(css_fixed val, css_unit unit, int font_px, int *pct)
{
    if (pct) *pct = 0;
    switch (unit) {
    case CSS_UNIT_PX:  return FIXTOINT(val);
    case CSS_UNIT_EM:  return FIXTOINT(FMUL(val, INTTOFIX(font_px)));
    case CSS_UNIT_REM: return FIXTOINT(FMUL(val, INTTOFIX(16)));
    case CSS_UNIT_EX:  return FIXTOINT(FMUL(val, INTTOFIX(font_px))) / 2;
    case CSS_UNIT_PT:  return FIXTOINT(val) * 96 / 72;
    case CSS_UNIT_PCT: if (pct) *pct = 1; return FIXTOINT(val);
    default:           return FIXTOINT(val);
    }
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

    if (css_computed_background_color(cs, &col) == CSS_BACKGROUND_COLOR_COLOR &&
        (col & 0xFF000000) != 0) { o->has_bg = 1; o->background = to_rgb(col); }

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
    }
    if (css_computed_height(cs, &len, &unit) == CSS_HEIGHT_SET) {
        int pct; o->height = clamp_px(len_px(len, unit, fp, &pct)); o->has_h = 1; o->h_pct = pct;
    }

    switch (css_computed_text_align(cs)) {
    case CSS_TEXT_ALIGN_CENTER: o->text_align = ALIGN_CENTER; break;
    case CSS_TEXT_ALIGN_RIGHT:  o->text_align = ALIGN_RIGHT; break;
    default:                    o->text_align = ALIGN_LEFT; break;
    }

    switch (css_computed_line_height(cs, &len, &unit)) {
    case CSS_LINE_HEIGHT_NUMBER:    o->line_px = clamp_px(FIXTOINT(FMUL(len, INTTOFIX(fp)))); break;
    case CSS_LINE_HEIGHT_DIMENSION: o->line_px = clamp_px(len_px(len, unit, fp, NULL)); break;
    default:                        o->line_px = 0; break;   /* normal -> layout derives */
    }

    /* borders: full per-edge model (top/right/bottom/left) */
#define EDGE_CONVERT(i, NAME) \
    if (css_computed_border_##NAME##_style(cs) != CSS_BORDER_STYLE_NONE) { \
        css_computed_border_##NAME##_width(cs, &len, &unit); \
        o->border_w[i] = clamp_px(len_px(len, unit, fp, NULL)); \
        if (css_computed_border_##NAME##_color(cs, &col) == CSS_BORDER_COLOR_COLOR) \
            o->border_color[i] = to_rgb(col); \
        else o->border_color[i] = 0x808080; \
    }
    EDGE_CONVERT(0, top)
    EDGE_CONVERT(1, right)
    EDGE_CONVERT(2, bottom)
    EDGE_CONVERT(3, left)
#undef EDGE_CONVERT

    if (css_computed_text_decoration(cs) & CSS_TEXT_DECORATION_UNDERLINE) o->underline = 1;

    { uint8_t v = css_computed_visibility(cs);
      if (v == CSS_VISIBILITY_HIDDEN || v == CSS_VISIBILITY_COLLAPSE) { o->hidden = 1; o->vis_hid = 1; } }
    { css_fixed op;
      if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET && op <= 0) { o->hidden = 1; o->op0 = 1; } }
    { uint8_t p = css_computed_position(cs);
      /* absolute: out of flow (dropdowns/overlays would smear the normal
       * flow). fixed stays in flow -- fixed headers sit at the top anyway. */
      if (p == CSS_POSITION_ABSOLUTE) o->pos_abs = 1; }
    { css_fixed v; css_unit u;         /* offsets for pos_abs overlay anchoring */
      if (css_computed_top(cs, &v, &u) == CSS_TOP_SET && u == CSS_UNIT_PX)
          { o->top = clamp_px(FIXTOINT(v)); o->has_top = 1; }
      if (css_computed_left(cs, &v, &u) == CSS_LEFT_SET && u == CSS_UNIT_PX)
          { o->left = clamp_px(FIXTOINT(v)); o->has_left = 1; } }
    { css_fixed fg = 0;
      if (css_computed_flex_grow(cs, &fg) == CSS_FLEX_GROW_SET && fg > 0)
          o->flex_grow = fg; }
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

    struct cstyle *o = kmalloc(sizeof *o);
    if (o) {
        memset(o, 0, sizeof *o);
        o->font_px = parent_font;          /* sensible default before convert */
        convert(eff, parent_font, o);
        if (n->style) kfree(n->style);
        n->style = o;
    }

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
        author = make_sheet(page_css, (size_t)page_len, false, g_allow_quirks);
        if (author) css_select_ctx_append_sheet(g_ctx, author, CSS_ORIGIN_AUTHOR, NULL);
    }

    for (struct node *c = root->first_child; c; c = c->next)
        style_node(c, NULL, 16);   /* top-level: no CSS parent (root absolutised by select) */

    if (author) {
        css_select_ctx_remove_sheet(g_ctx, author);
        css_stylesheet_destroy(author);
    }
}
