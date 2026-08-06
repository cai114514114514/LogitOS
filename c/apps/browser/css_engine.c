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

/* previous element sibling (our DOM is singly linked, walk from parent). */
static struct node *prev_elem_sibling(struct node *n)
{
    struct node *prev = 0, *c;
    if (!n->parent) return 0;
    for (c = n->parent->first_child; c && c != n; c = c->next)
        if (c->type == N_ELEM) prev = c;
    return prev;
}

/* iterate whitespace-delimited tokens of a node's class attribute */
static int sp(int c){ return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

/* ---------- select handler (drives LibCSS off our struct node) ---------- */
static css_error h_node_name(void *pw, void *node, css_qname *qname)
{
    (void)pw; struct node *n = node;
    qname->ns = NULL;
    lwc_intern_string(n->tag, strlen(n->tag), &qname->name);
    return CSS_OK;
}

static css_error h_node_classes(void *pw, void *node,
        lwc_string ***classes, uint32_t *n_classes)
{
    (void)pw; struct node *n = node;
    /* The array storage is the handler's responsibility and LibCSS only reads it
     * during the css_select_style call, so keep it in a static buffer: the old
     * malloc'd array leaked on every call (M11). */
    static lwc_string *arr[32];
    const char *cls = dom_attr(n, "class");
    *classes = NULL; *n_classes = 0;
    if (!cls || !*cls) return CSS_OK;
    int i = 0; const char *p = cls;
    while (*p && i < 32) {
        while (*p && sp(*p)) p++;
        const char *s = p;
        while (*p && !sp(*p)) p++;
        if (p > s) lwc_intern_string(s, (size_t)(p - s), &arr[i++]);
    }
    if (!i) return CSS_OK;
    *classes = arr; *n_classes = (uint32_t)i;
    return CSS_OK;
}

static css_error h_node_id(void *pw, void *node, lwc_string **id)
{
    (void)pw; struct node *n = node;
    const char *i = dom_attr(n, "id");
    *id = NULL;
    if (i && *i) lwc_intern_string(i, strlen(i), id);
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
    (void)pw; struct node *n = node, *c, *found = 0;
    if (n->parent)
        for (c = n->parent->first_child; c && c != n; c = c->next)
            if (c->type == N_ELEM && tag_is_ci(c, qname->name)) found = c;
    *sibling = found;
    return CSS_OK;
}

static css_error h_parent_node(void *pw, void *node, void **parent)
{
    (void)pw; struct node *p = ((struct node *)node)->parent;
    /* The synthetic #document root is not a CSS parent -> report root as NULL,
     * so LibCSS treats <html> as the root element and resolves absolute sizes. */
    *parent = (p && p->type == N_ELEM && p->tag[0] != '#') ? p : NULL;
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
    const char *want = lwc_string_data(name);
    size_t wl = lwc_string_length(name);
    const char *cls = dom_attr(n, "class");
    *match = false;
    if (!cls) return CSS_OK;
    const char *p = cls;
    while (*p) {
        while (*p && sp(*p)) p++;
        const char *s = p;
        while (*p && !sp(*p)) p++;
        if ((size_t)(p - s) == wl && memcmp(s, want, wl) == 0) { *match = true; break; }
    }
    return CSS_OK;
}

static css_error h_node_has_id(void *pw, void *node, lwc_string *name, bool *match)
{
    (void)pw; struct node *n = node;
    const char *id = dom_attr(n, "id");
    *match = id && strlen(id) == lwc_string_length(name) &&
             memcmp(id, lwc_string_data(name), lwc_string_length(name)) == 0;
    return CSS_OK;
}

/* attribute name from qname (NUL-terminate into a small buffer) */
static const char *qattr(const css_qname *qname, char *buf, int cap)
{
    size_t n = lwc_string_length(qname->name);
    if ((int)n >= cap) n = cap - 1;
    memcpy(buf, lwc_string_data(qname->name), n);
    buf[n] = 0;
    return buf;
}

static css_error h_node_has_attribute(void *pw, void *node,
        const css_qname *qname, bool *match)
{
    (void)pw; char b[64];
    *match = dom_attr(node, qattr(qname, b, sizeof b)) != NULL;
    return CSS_OK;
}

static css_error h_node_has_attribute_equal(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
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
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
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
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
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
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = v && wl && strlen(v) >= wl && memcmp(v, w, wl) == 0;
    return CSS_OK;
}

static css_error h_node_has_attribute_suffix(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
    const char *w = lwc_string_data(value); size_t wl = lwc_string_length(value);
    *match = false;
    if (v && wl) { size_t vl = strlen(v); if (vl >= wl && memcmp(v + vl - wl, w, wl) == 0) *match = true; }
    return CSS_OK;
}

static css_error h_node_has_attribute_substring(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match)
{
    (void)pw; char b[64];
    const char *v = dom_attr(node, qattr(qname, b, sizeof b));
    *match = v && substr(v, lwc_string_data(value), lwc_string_length(value));
    return CSS_OK;
}

static css_error h_node_is_root(void *pw, void *node, bool *match)
{
    (void)pw; struct node *n = node;
    *match = !n->parent || (n->parent->tag[0] == '#');   /* parent is #document */
    return CSS_OK;
}

static css_error h_node_count_siblings(void *pw, void *node,
        bool same_name, bool after, int32_t *count)
{
    (void)pw; (void)after; struct node *n = node; int cnt = 0;
    /* same_name (:nth-of-type) counts only same-tag siblings. (The old
     * tag_is_ci(c, NULL) here passed a NULL lwc_string -> NULL deref crash.) */
    if (n && n->parent)
        for (struct node *c = n->parent->first_child; c && c != n; c = c->next)
            if (c->type == N_ELEM && (!same_name || strcmp(c->tag, n->tag) == 0))
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
    *match = (strcmp(n->tag, "a") == 0 && dom_attr(n, "href") != NULL);
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
static css_unit_ctx g_unit;
static css_media g_media;

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
    "script,style,head,title,meta,link,noscript,template,[hidden]{display:none}";

static css_stylesheet *make_sheet(const char *data, size_t len, bool inl)
{
    css_stylesheet_params p;
    memset(&p, 0, sizeof p);
    p.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    p.level = CSS_LEVEL_DEFAULT;
    p.charset = "UTF-8";
    p.url = "http://logit/";
    p.title = NULL;
    p.inline_style = inl;
    p.resolve = resolve_url;
    css_stylesheet *s = NULL;
    if (css_stylesheet_create(&p, &s) != CSS_OK || !s) return NULL;
    css_stylesheet_append_data(s, (const uint8_t *)data, len);
    css_stylesheet_data_done(s);
    return s;
}

void css_init(void)
{
    memset(&g_media, 0, sizeof g_media);
    g_media.type = CSS_MEDIA_SCREEN;
    g_media.width  = INTTOFIX(760);
    g_media.height = INTTOFIX(540);

    memset(&g_unit, 0, sizeof g_unit);
    g_unit.viewport_width  = INTTOFIX(760);
    g_unit.viewport_height = INTTOFIX(540);
    g_unit.font_size_default = INTTOFIX(16);
    g_unit.font_size_minimum = INTTOFIX(6);
    g_unit.device_dpi = INTTOFIX(96);
    g_unit.root_style = NULL;   /* g_unit.measure stays NULL (it is a const member) */

    if (css_select_ctx_create(&g_ctx) != CSS_OK) { g_ctx = NULL; return; }
    g_ua_sheet = make_sheet(UA_CSS, sizeof UA_CSS - 1, false);
    if (g_ua_sheet) css_select_ctx_append_sheet(g_ctx, g_ua_sheet, CSS_ORIGIN_UA, NULL);
}

void css_viewport(int w, int h)
{
    g_media.width  = INTTOFIX(w);
    g_media.height = INTTOFIX(h);
    g_unit.viewport_width  = INTTOFIX(w);
    g_unit.viewport_height = INTTOFIX(h);
}

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
    default:                       o->display = DISP_BLOCK; break;  /* block + grid + table-ish */
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
      if (v == CSS_VISIBILITY_HIDDEN || v == CSS_VISIBILITY_COLLAPSE) o->hidden = 1; }
    { css_fixed op;
      if (css_computed_opacity(cs, &op) == CSS_OPACITY_SET && op <= 0) o->hidden = 1; }
}

/* ---------- traversal ---------- */
static void style_node(struct node *n, const css_computed_style *parent, int parent_font)
{
    if (n->type != N_ELEM) return;

    css_select_results *res = NULL;
    css_stylesheet *inl = NULL;
    const char *istyle = dom_attr(n, "style");
    if (istyle && *istyle) inl = make_sheet(istyle, strlen(istyle), true);

    if (css_select_style(g_ctx, n, &g_unit, &g_media, inl,
                         &g_handler, NULL, &res) != CSS_OK || !res) {
        if (inl) css_stylesheet_destroy(inl);
        return;
    }

    /* select_style already absolutised the root; non-root nodes inherit + resolve
     * relative units against the parent's composed style via compose(). */
    const css_computed_style *base = res->styles[CSS_PSEUDO_ELEMENT_NONE];
    css_computed_style *composed = NULL;
    const css_computed_style *eff = base;
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

    css_stylesheet *author = NULL;
    if (page_css && page_len > 0) {
        author = make_sheet(page_css, (size_t)page_len, false);
        if (author) css_select_ctx_append_sheet(g_ctx, author, CSS_ORIGIN_AUTHOR, NULL);
    }

    for (struct node *c = root->first_child; c; c = c->next)
        style_node(c, NULL, 16);   /* top-level: no CSS parent (root absolutised by select) */

    if (author) {
        css_select_ctx_remove_sheet(g_ctx, author);
        css_stylesheet_destroy(author);
    }
}
