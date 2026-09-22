/* Inline SVG is a DOM subtree, not an immutable resource. The 2026-09-10
 * specimen produced no image for createElementNS (rawlen=0), and a red image
 * after setAttribute(fill, lime). Serialize live attributes and the winning
 * paint declarations at the layout boundary; never rewrite the page's DOM.
 * Node identity is stable during one synchronous layout pass, so flex trials
 * can still share the resulting bitmap. A later pass reads the new tree.
 *
 * This is a bounded raster input, not outerHTML. Styles have already gone
 * through LibCSS/css_extra; copying style= again would let an inline normal
 * declaration override an author !important winner inside the decoder. */
#define SVG_DOM_BYTES (1024u * 1024u)
#define SVG_DOM_NODES 16384
#define SVG_DOM_DEPTH 128
struct svg_dom_buf {
    char *p;
    unsigned len, cap;
    int failed, nodes;
    /* A CSS-sized outer SVG should be rasterised at that used size. Keeping
     * the override in the serialized bytes also makes the existing exact-key
     * reflow cache distinguish two viewport sizes of the same live subtree. */
    int raster_w, raster_h;
};
struct svg_dom_prop { const char *name, *initial; int inherited; };
static const struct svg_dom_prop svg_dom_props[] = {
#define SVG_DOM_PROP(id, name, initial, inherited) {name, initial, inherited},
    CSS_SVG_PAINT_PROPERTIES(SVG_DOM_PROP)
#undef SVG_DOM_PROP
};
static void svg_dom_put(struct svg_dom_buf *b, const char *s, unsigned n)
{
    if (b->failed) return;
    if (!s || n > SVG_DOM_BYTES - b->len) { b->failed = 1; return; }
    if (b->len + n + 1 > b->cap) {
        unsigned cap = b->cap ? b->cap : 512;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = kmalloc(cap);
        if (!p) { b->failed = 1; return; }
        if (b->len) memcpy(p, b->p, b->len);
        kfree(b->p); b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0;
}
static void svg_dom_str(struct svg_dom_buf *b, const char *s)
{ if (s) svg_dom_put(b, s, (unsigned)strlen(s)); }
static void svg_dom_escape(struct svg_dom_buf *b, const char *s, unsigned n)
{
    for (unsigned i = 0; i < n && !b->failed; i++) {
        switch (s[i]) {
        case '&': svg_dom_str(b, "&amp;"); break;
        case '<': svg_dom_str(b, "&lt;"); break;
        case '>': svg_dom_str(b, "&gt;"); break;
        case '"': svg_dom_str(b, "&quot;"); break;
        default: svg_dom_put(b, s + i, 1); break;
        }
    }
}
static int svg_dom_eq(const char *s, int n, const char *v)
{
    if (!s || n != (int)strlen(v)) return 0;
    for (int i = 0; i < n; i++) {
        int c = (unsigned char)s[i]; if (c >= 'A' && c <= 'Z') c += 32;
        if (c != v[i]) return 0;
    }
    return 1;
}
static const char *svg_dom_own(const struct node *n, int p, int *len)
{
    const struct cstyle *st = n->style;
    int xr = XR_SVG_FILL + p;
    if (st && st->xraw[xr] && st->xrawlen[xr]) {
        *len = st->xrawlen[xr]; return st->xraw[xr];
    }
    const char *s = n->ns == NS_SVG ? dom_attr(n, svg_dom_props[p].name) : 0;
    *len = s ? (int)strlen(s) : 0;
    return s;
}
static const char *svg_dom_resolve(const struct node *n, int p, int *len)
{
    const struct svg_dom_prop *prop = &svg_dom_props[p];
    for (int depth = 0; n && depth < SVG_DOM_DEPTH; n = n->parent, depth++) {
        const char *s = svg_dom_own(n, p, len);
        if (s && *len) {
            if (svg_dom_eq(s, *len, "inherit")) continue;
            if (svg_dom_eq(s, *len, "unset") && prop->inherited) continue;
            if (!svg_dom_eq(s, *len, "initial") && !svg_dom_eq(s, *len, "unset")) return s;
            break;
        }
        if (!prop->inherited) break;
    }
    *len = (int)strlen(prop->initial); return prop->initial;
}
static void svg_dom_attr(struct svg_dom_buf *b, const char *name, const char *s, int len)
{
    svg_dom_str(b, " ");
    /* The HTML tokenizer stores a namespace prefix as `xlink href`. Restore
     * XML spelling without losing the adjusted camelCase SVG local names. */
    if (!strcmp(name, "xmlns xmlns")) svg_dom_str(b, "xmlns");
    else for (const char *k = name; *k; k++) {
        if (*k == ' ') svg_dom_str(b, ":"); else svg_dom_put(b, k, 1);
    }
    svg_dom_str(b, "=\""); svg_dom_escape(b, s, (unsigned)len); svg_dom_str(b, "\"");
}
static void svg_dom_dim(struct svg_dom_buf *b, const char *name, int value)
{
    char s[16]; int n = 0;
    if (value <= 0) return;
    do { s[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < (int)sizeof s);
    for (int i = 0; i < n / 2; i++) { char c = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = c; }
    svg_dom_attr(b, name, s, n);
}
static void svg_dom_node(struct svg_dom_buf *b, const struct node *n, int depth)
{
    if (b->failed) return;
    if (++b->nodes > SVG_DOM_NODES || depth > SVG_DOM_DEPTH) { b->failed = 1; return; }
    if (n->type == N_TEXT) {
        if (n->text) svg_dom_escape(b, n->text, (unsigned)n->textlen);
        return;
    }
    if (n->type != N_ELEM || !n->tag) return;
    if (!strcmp(n->tag, "style") || !strcmp(n->tag, "script")) return;
    svg_dom_str(b, "<"); svg_dom_str(b, n->tag);
    for (int i = 0; i < n->nattr; i++) {
        const char *name = dom_attr_name_at(n, i); int paint = 0;
        if (!name || !strcmp(name, "style")) continue;
        if (!depth && b->raster_w > 0 && b->raster_h > 0 &&
            (!strcmp(name, "width") || !strcmp(name, "height"))) continue;
        for (unsigned p = 0; p < sizeof svg_dom_props / sizeof svg_dom_props[0]; p++)
            if (!strcmp(name, svg_dom_props[p].name)) { paint = 1; break; }
        if (paint || !strcmp(name, "color") || !strcmp(name, "opacity") ||
            !strcmp(name, "display") || !strcmp(name, "visibility")) continue;
        svg_dom_attr(b, name, n->attrs[i].value, n->attrs[i].vlen);
    }
    if (!depth && b->raster_w > 0 && b->raster_h > 0) {
        svg_dom_dim(b, "width", b->raster_w);
        svg_dom_dim(b, "height", b->raster_h);
    }
    const struct cstyle *st = n->style;
    /* Native properties come from LibCSS once, rather than re-collecting
     * every ordinary `body { color:... }` as an extension rule. Keeping the
     * pre-compose color bit is also what preserves <use> shadow inheritance. */
    if (st && (!depth || st->svg_color_explicit)) {
        static const char hex[] = "0123456789abcdef";
        char color[7] = {'#',0,0,0,0,0,0};
        for (int j = 0; j < 6; j++) color[j+1] = hex[(st->color >> ((5-j)*4)) & 15];
        svg_dom_attr(b, "color", color, 7);
    } else if (!st) {
        const char *color = dom_attr(n,"color");
        if(color)svg_dom_attr(b,"color",color,(int)strlen(color));
    }
    /* The outer painter owns root opacity. Inner groups receive their own
     * composed opacity (a non-inherited native property); do not multiply the
     * root into its raster as well, which would turn .5 into .25. */
    if (depth && st) {
        unsigned milli = ((unsigned)st->opacity * 1000 + 127) / 255;
        char opacity[5] = {(char)('0'+milli/1000),'.',
            (char)('0'+milli/100%10),(char)('0'+milli/10%10),(char)('0'+milli%10)};
        svg_dom_attr(b,"opacity",opacity,5);
    } else if (depth) {
        const char *opacity=dom_attr(n,"opacity");
        if(opacity)svg_dom_attr(b,"opacity",opacity,(int)strlen(opacity));
    }
    for (unsigned p = 0; p < sizeof svg_dom_props / sizeof svg_dom_props[0]; p++) {
        int len = 0; const char *s = svg_dom_own(n, (int)p, &len);
        if (depth && (!s || !len)) continue;
        if (depth && svg_dom_props[p].inherited &&
            (svg_dom_eq(s, len, "inherit") || svg_dom_eq(s, len, "unset"))) continue;
        s = svg_dom_resolve(n, (int)p, &len);
        svg_dom_attr(b, svg_dom_props[p].name, s, len);
    }
    const char *display = dom_attr(n, "display"), *visibility = dom_attr(n, "visibility");
    if (st && st->display == DISP_NONE) display = "none";
    if (st && st->vis_hid) visibility = "hidden";
    if (display) svg_dom_attr(b, "display", display, (int)strlen(display));
    if (visibility) svg_dom_attr(b, "visibility", visibility, (int)strlen(visibility));
    svg_dom_str(b, ">");
    for (const struct node *c = n->first_child; c && !b->failed; c = c->next)
        svg_dom_node(b, c, depth + 1);
    svg_dom_str(b, "</"); svg_dom_str(b, n->tag); svg_dom_str(b, ">");
}
static char *svg_dom_paint(const struct node *n, int *len, int raster_w, int raster_h)
{
    struct svg_dom_buf b = {0};
    b.raster_w = raster_w; b.raster_h = raster_h;
    svg_dom_node(&b, n, 0);
    if (b.failed) { kfree(b.p); *len = 0; return 0; }
    *len = (int)b.len; return b.p;
}
