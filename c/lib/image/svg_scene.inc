/* SPDX-License-Identifier: MIT
 * SVG scene semantics share the existing integer path/arc/raster code. A tree
 * is necessary: <use> may refer forward, and clipping/opacity apply to a whole
 * subtree, not independently to overlapping children. This is image decoding,
 * not a second browser: the caller supplies computed CSS as presentation
 * attributes; a small inline-declaration cascade also serves standalone SVG.
 * No network, script, text or external stylesheet/paint/use.
 * Correction: svg_stylesheet.inc now compiles embedded simple-selector solid
 * paint rules. Full CSS selectors, at-rules and gradients remain absent.
 * The former decoder skipped defs/style/transform and fixed currentColor to
 * black. Keep those old limitations here beside the replacement: neither a
 * <defs> registry nor the shared CSS color parser ever implied SVG gradients.
 */
#define SV_NODES 2048
#define SV_DEPTH 48
#define SV_VISITS 8192
#define SV_INPUT_BYTES (4 * 1024 * 1024)
#ifndef SV_WORK_LIMIT
#define SV_WORK_LIMIT (256UL * 1024 * 1024)
#endif
#define SV_MATRIX_MAX 1073741824LL /* 16,384 in 16.16; independent of pixel range */
#define SV_LAYER_BYTES (64UL * 1024 * 1024)
struct sv_node { struct tag t; int first, next, parent; unsigned char active;
    const uint8_t *text;int textlen; };
struct sv_style {
    struct paint fill, stroke, color;
    int fc, sc, sw, fo, so, opacity, cliprule, hidden, display;
};
struct sv_box { int x0, y0, x1, y1, any; };
struct sv_scene {
    struct sv_node *nodes; int count, visits, failed;
    struct sv_css_rule *css;int css_count;
    unsigned long layers, pixels, geometry_bytes, raster_pixels;
    struct gfx_path path;
};
static int sv_eq(const uint8_t *p, int n, const char *s)
{
    int i = 0;
    for (; i < n && s[i]; i++) {
        int a = p[i], b = s[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return i == n && !s[i];
}
static void sv_trim(const uint8_t **p, int *n)
{
    while (*n && is_ws(**p)) { ++*p; --*n; }
    while (*n && is_ws((*p)[*n - 1])) --*n;
}
/* One declaration scanner for inline styles and embedded sheets. Comments,
 * quotes and parentheses stay out of delimiter decisions; callers validate a
 * value before allowing it to displace an earlier valid declaration. */
static const uint8_t *sv_decl_space(const uint8_t *p,const uint8_t *end)
{
    for(;;){while(p<end&&is_ws(*p))p++;
        if(end-p>=2&&p[0]=='/'&&p[1]=='*'){p+=2;while(end-p>=2&&!(p[0]=='*'&&p[1]=='/'))p++;
            if(end-p<2)return end;p+=2;}else return p;}
}
static int sv_decl_next(const uint8_t **cursor,const uint8_t *end,
                       const uint8_t **key,int *kn,const uint8_t **val,int *vn,int *important)
{
    const uint8_t *p=*cursor;
    while(p<end) {
        p=sv_decl_space(p,end);if(p==end)break;if(*p==';'){p++;continue;}
        const uint8_t *k=p;while(p<end&&((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||*p=='-'))p++;
        const uint8_t *ke=p;p=sv_decl_space(p,end);
        if(p==end)break;
        if(*p!=':'){while(p<end&&*p!=';')p++;if(p<end)p++;continue;}
        *key=k;*kn=(int)(ke-k);p++;
        p=sv_decl_space(p,end);const uint8_t *v=p,*last=p,*bang=0,*before_bang=0;int quote=0,par=0;
        while(p<end) {
            int c=*p;
            if(quote){if(c=='\\'&&p+1<end)p++;else if(c==quote)quote=0;last=++p;continue;}
            if(c=='\''||c=='"'){quote=c;last=++p;continue;}
            if(end-p>=2&&c=='/'&&p[1]=='*'){p=sv_decl_space(p,end);continue;}
            if(c=='(')par++;else if(c==')'&&par)par--;
            else if(c==';'&&!par)break;
            else if(c=='!'&&!par){bang=p;before_bang=last;}
            if(!is_ws(c))last=p+1;p++;
        }
        *val=v;*vn=(int)(last-v);*important=0;
        if(bang){const uint8_t *q=sv_decl_space(bang+1,last);const uint8_t *qe=q;
            while(qe<last&&!is_ws(*qe)&&*qe!='/')qe++;
            if(sv_eq(q,(int)(qe-q),"important")&&sv_decl_space(qe,last)==last){
                *important=1;*vn=(int)(before_bang-v);sv_trim(val,vn);}}
        *cursor=p<end?p+1:end;return 1;
    }
    *cursor=end;return 0;
}
/* Non-paint users (transform and clip-path) retain the existing inline /
 * presentation contract; the paint cascade below consumes the same scanner. */
static int sv_prop(const struct tag *t,const char *name,const uint8_t **v,int *n)
{
    int found=!attr_get(t,name,v,n),important=0,len;const uint8_t *s;
    if(attr_get(t,"style",&s,&len))return found;
    const uint8_t *end=s+len,*key,*value;int kn,vn,imp;
    while(sv_decl_next(&s,end,&key,&kn,&value,&vn,&imp))
        if(sv_eq(key,kn,name)&&(!important||imp)){*v=value;*n=vn;important=imp;found=1;}
    if(found)sv_trim(v,n);return found;
}
/* Decode XML escapes for fragment IDs (including escaped quotes inside url()).
 * Compare decoded strings, not raw spelling: #a&amp;b and id="a&#38;b" name
 * the same resource. No DTD/entity expansion; unsupported entities fail. */
static int sv_unescape(const uint8_t *p, int n, uint8_t *out, int cap)
{
    int used = 0;
    for (int i = 0; i < n; i++) {
        unsigned c = p[i];
        if (c == '&') {
            int j = i + 1;
            while (j < n && j - i < 16 && p[j] != ';') j++;
            if (j == n || p[j] != ';') return -1;
            if (str_eq(p + i + 1, j - i - 1, "amp")) c = '&';
            else if (str_eq(p + i + 1, j - i - 1, "quot")) c = '"';
            else if (str_eq(p + i + 1, j - i - 1, "apos")) c = '\'';
            else if (str_eq(p + i + 1, j - i - 1, "lt")) c = '<';
            else if (str_eq(p + i + 1, j - i - 1, "gt")) c = '>';
            else if (p[i + 1] == '#') {
                int k = i + 2, base = 10; c = 0;
                if (k < j && (p[k] == 'x' || p[k] == 'X')) { base = 16; k++; }
                if (k == j) return -1;
                for (; k < j; k++) { int d = hexval(p[k]); if (d < 0 || d >= base || c > 0x10ffffU / base) return -1; c = c * base + d; }
                if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
            } else return -1;
            i = j;
        }
        int bytes = c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
        /* Raw UTF-8 is already encoded, unlike numeric character references. */
        if (p[i] != ';' && c >= 128) bytes = 1;
        if (used + bytes > cap) return -1;
        if (bytes == 1) out[used++] = (uint8_t)c;
        else {
            out[used++] = (uint8_t)((bytes == 2 ? 0xc0 : bytes == 3 ? 0xe0 : 0xf0) | (c >> ((bytes - 1) * 6)));
            for (int k = bytes - 2; k >= 0; k--) out[used++] = 0x80 | ((c >> (k * 6)) & 63);
        }
    }
    return used;
}
static int sv_literal(const uint8_t *v, int n, struct paint *paint, int *current)
{
    uint8_t decoded[256]; int dn = sv_unescape(v, n, decoded, sizeof decoded);
    if (dn < 0) return 0; v = decoded; n = dn; sv_trim(&v, &n);
    if (sv_eq(v, n, "inherit") || sv_eq(v, n, "unset")) return 0;
    if (sv_eq(v, n, "currentColor")) { *current = 1; paint->none = 0; return 1; }
    struct paint p = {0, 0, 0, 255, 2, paint->evenodd};
    parse_color(v, n, &p);
    if (p.none == 2) return 0;
    *paint = p; *current = 0; return 1;
}
static int sv_opacity(const uint8_t *v, int n, int old)
{
    int f; const uint8_t *p = v, *e = v + n;
    if (pnum_fx(&p, e, &f)) return old;
    if (p < e && *p == '%') { f /= 100; p++; }
    while (p < e && is_ws(*p)) p++;
    if (p != e) return old;
    return f < 0 ? 0 : f > 256 ? 256 : f;
}
#include "svg_stylesheet.inc"
static void sv_style_apply(struct sv_scene *scene,int idx,struct sv_style *st)
{
    struct sv_css_values values;sv_css_cascade(scene,idx,&values);
    const uint8_t *v; int n, unused = 0;
    int inherited_opacity=st->opacity,inherited_display=st->display;
    st->opacity = 256; st->display = 1; /* neither inherits without an explicit keyword */
    if(values.v[5]&&sv_css_keyword(values.v[5],values.n[5])==1)st->opacity=inherited_opacity;
    if(values.v[9]&&sv_css_keyword(values.v[9],values.n[9])==1)st->display=inherited_display;
    if (sv_css_get(&values, "color", &v, &n) && !sv_eq(v, n, "currentColor")) sv_literal(v, n, &st->color, &unused);
    if (sv_css_get(&values, "fill", &v, &n)) sv_literal(v, n, &st->fill, &st->fc);
    if (sv_css_get(&values, "stroke", &v, &n)) sv_literal(v, n, &st->stroke, &st->sc);
#ifdef SVG_SCENE_NO_CURRENTCOLOR
    st->color.r = st->color.g = st->color.b = 0;
#endif
    if (sv_css_get(&values, "fill-opacity", &v, &n)) st->fo = sv_opacity(v, n, st->fo);
    if (sv_css_get(&values, "stroke-opacity", &v, &n)) st->so = sv_opacity(v, n, st->so);
    if (sv_css_get(&values, "opacity", &v, &n)) st->opacity = sv_opacity(v, n, 256);
    if (sv_css_get(&values, "fill-rule", &v, &n)) {
        if (sv_eq(v, n, "evenodd")) st->fill.evenodd = 1;
        else if (sv_eq(v, n, "nonzero")) st->fill.evenodd = 0;
    }
    if (sv_css_get(&values, "clip-rule", &v, &n)) {
        if (sv_eq(v, n, "evenodd")) st->cliprule = 1;
        else if (sv_eq(v, n, "nonzero")) st->cliprule = 0;
    }
    if (sv_css_get(&values, "stroke-width", &v, &n)) {
        int w; const uint8_t *p = v;
        if (!pnum_fx(&p, v + n, &w) && w >= 0 && (p == v + n || str_eq(p, (int)(v+n-p), "px"))) st->sw = w;
    }
    if (sv_css_get(&values, "display", &v, &n) && sv_eq(v, n, "none")) st->display = 0;
    if (sv_css_get(&values, "visibility", &v, &n)) {
        if (sv_eq(v, n, "hidden") || sv_eq(v, n, "collapse")) st->hidden = 1;
        else if (sv_eq(v, n, "visible")) st->hidden = 0;
    }
}
static struct sv_style sv_default(void)
{
    struct sv_style st = {0};
    st.fill.a = st.stroke.a = st.color.a = 255; st.stroke.none = 1;
    st.sw = 256; st.fo = st.so = st.opacity = 256; st.display = 1;
    return st;
}
/* SVG matrices are untrusted. gfx matrices assume caller-bounded arithmetic;
 * reject an unrepresentable composition before narrowing, never wrap a distant
 * path back onto the icon. Linear coefficients use 16.16, offsets use 24.8. */
static int sv_matrix(struct gfx_matrix *out, const struct gfx_matrix *l, const struct gfx_matrix *r)
{
    long long q[6] = {
        ((long long)l->a*r->a + (long long)l->c*r->b) / 65536,
        ((long long)l->b*r->a + (long long)l->d*r->b) / 65536,
        ((long long)l->a*r->c + (long long)l->c*r->d) / 65536,
        ((long long)l->b*r->c + (long long)l->d*r->d) / 65536,
        ((long long)l->a*r->e + (long long)l->c*r->f) / 65536 + l->e,
        ((long long)l->b*r->e + (long long)l->d*r->f) / 65536 + l->f
    };
    for (int i = 0; i < 6; i++) { long long limit = i < 4 ? SV_MATRIX_MAX : 8000000; if (q[i] > limit || q[i] < -limit) return 0; }
    gfx_m_set(out, q[0], q[1], q[2], q[3], q[4], q[5]); return 1;
}
static int sv_transform(const struct tag *t, struct gfx_matrix *m)
{
    gfx_m_identity(m);
#ifdef SVG_SCENE_NO_TRANSFORM
    (void)t; return 1;
#else
    const uint8_t *p; int n;
    if (!sv_prop(t, "transform", &p, &n) || sv_eq(p, n, "none")) return 1;
    const uint8_t *end = p + n; int count = 0;
    while (p < end) {
        while (p < end && (is_ws(*p) || *p == ',')) p++;
        if (p == end) break;
        const uint8_t *name = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) p++;
        int nl = (int)(p - name); while (p < end && is_ws(*p)) p++;
        if (p == end || *p++ != '(') return 0;
        if (++count > 64) return -1;
        int v[6], vn = 0;
        while (p < end && *p != ')') {
            while (p < end && (is_ws(*p) || *p == ',')) p++;
            if (p < end && *p == ')') break;
            if (vn == 6) return 0;
            int linear = sv_eq(name,nl,"scale") || (sv_eq(name,nl,"matrix") && vn < 4);
            if (pnum_scaled(&p,end,&v[vn++],linear?65536:256,linear?(int)SV_MATRIX_MAX:8000000)) return 0;
            /* CSS matrix()/translate(px)/rotate(deg) emitted by the bridge
             * share the same affine values; percentages need a CSS box and
             * are deliberately rejected here instead of guessing one. */
            if (p + 2 <= end && p[0] == 'p' && p[1] == 'x') p += 2;
            else if (p + 3 <= end && p[0] == 'd' && p[1] == 'e' && p[2] == 'g') p += 3;
            while (p < end && is_ws(*p)) p++;
        }
        if (p == end || *p++ != ')') return 0;
        struct gfx_matrix a; gfx_m_identity(&a);
        if (sv_eq(name, nl, "matrix") && vn == 6) {
            gfx_m_set(&a, v[0], v[1],
                v[2], v[3], v[4], v[5]);
        } else if (sv_eq(name, nl, "translate") && (vn == 1 || vn == 2)) { a.e = v[0]; a.f = vn == 2 ? v[1] : 0; }
        else if (sv_eq(name, nl, "scale") && (vn == 1 || vn == 2)) {
            a.a = v[0]; a.d = vn == 2 ? v[1] : a.a;
        } else if (sv_eq(name, nl, "rotate") && (vn == 1 || vn == 3)) {
            int si = gfx_sin(v[0]), co = gfx_cos(v[0]); gfx_m_set(&a, co, si, -si, co, 0, 0);
            if (vn == 3) { a.e = (int)clampfx((long long)v[1] - ((long long)co*v[1]-(long long)si*v[2])/65536); a.f = (int)clampfx((long long)v[2]-((long long)si*v[1]+(long long)co*v[2])/65536); }
        } else if ((sv_eq(name, nl, "skewX") || sv_eq(name, nl, "skewY")) && vn == 1) {
            int co = gfx_cos(v[0]); if (!co) return 0;
            long long tangent = (long long)gfx_sin(v[0])*65536/co;
            if (tangent > SV_MATRIX_MAX || tangent < -SV_MATRIX_MAX) return -1;
            if (sv_eq(name, nl, "skewX")) a.c = (int)tangent; else a.b = (int)tangent;
        } else return 0;
        if (!sv_matrix(m, m, &a)) return -1;
    }
    return 1;
#endif
}
static int sv_viewport(const struct tag *t, int w, int h, struct gfx_matrix *m)
{
    const uint8_t *p; int n, v[4]; gfx_m_identity(m);
    if (attr_get(t, "viewBox", &p, &n)) return 1;
    const uint8_t *end = p + n;
    for (int i = 0; i < 4; i++) { while (p < end && (is_ws(*p) || *p == ',')) p++; if (pnum_fx(&p, end, &v[i])) return 1; }
    if (v[2] <= 0 || v[3] <= 0) return 0;
    int ax = 1, ay = 1, none = 0, slice = 0;
    if (!attr_get(t, "preserveAspectRatio", &p, &n)) {
        sv_trim(&p, &n);
        if (n >= 4 && str_eq(p, 4, "none")) none = 1;
        else if (n >= 8) {
            if (str_eq(p, 4, "xMin")) ax = 0; else if (str_eq(p, 4, "xMax")) ax = 2;
            if (str_eq(p+4, 4, "YMin")) ay = 0; else if (str_eq(p+4, 4, "YMax")) ay = 2;
            for (int i = 8; i + 5 <= n; i++) if (str_eq(p+i, 5, "slice")) slice = 1;
        }
    }
    long long sx = (long long)w*65536/v[2], sy = (long long)h*65536/v[3];
    if (!none) { long long s = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy); sx = sy = s; }
    if (sx > SV_MATRIX_MAX || sy > SV_MATRIX_MAX) return -1;
    m->a = (int)sx; m->d = (int)sy;
    m->e = (int)clampfx(-(long long)v[0]*sx/65536 + (none ? 0 : ((long long)w-(long long)v[2]*sx/65536)*ax/2));
    m->f = (int)clampfx(-(long long)v[1]*sy/65536 + (none ? 0 : ((long long)h-(long long)v[3]*sy/65536)*ay/2));
    return 1;
}
static void sv_dimension(const struct tag *t,const char *name,int *value)
{
    int parsed;if(!attr_dim_fx(t,name,&parsed))*value=parsed;
}
static struct sv_style sv_original_parent_style(struct sv_scene *s,int idx)
{
    int chain[SV_DEPTH],n=0;struct sv_style st=sv_default();
    for(int p=s->nodes[idx].parent;p>=0&&n<SV_DEPTH;p=s->nodes[p].parent)chain[n++]=p;
    while(n)sv_style_apply(s,chain[--n],&st);
    return st;
}
static int sv_parse(struct sv_scene *s, const struct tag *root, const uint8_t *p, const uint8_t *end)
{
    s->count = 1; s->nodes[0] = (struct sv_node){*root, -1, -1, -1, 0, 0, 0};
    if (root->selfclose) return 1;
    int stack[SV_DEPTH], last[SV_DEPTH], depth = 1; stack[0] = 0; last[0] = -1;
    while (p < end && depth) {
        while (p < end && *p != '<') p++;
        if (p == end) break;
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            p += 4; while (p + 2 < end && !(p[0]=='-' && p[1]=='-' && p[2]=='>')) p++;
            p = p + 2 < end ? p + 3 : end; continue;
        }
        if(end-p>=9&&str_eq(p,9,"<![CDATA[")) {
            p+=9;while(end-p>=3&&!(p[0]==']'&&p[1]==']'&&p[2]=='>'))p++;
            p=end-p>=3?p+3:end;continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) { while (p < end && *p != '>') p++; if (p < end) p++; continue; }
        const uint8_t *tag_begin=p;
        struct tag t; if (parse_tag(&p, end, &t)) break; /* historic partial-tag behavior */
        if (t.closing) {
            const struct tag *open = &s->nodes[stack[depth-1]].t;
            if (t.nlen != open->nlen) return 0;
            for (int i = 0; i < t.nlen; i++) if (t.name[i] != open->name[i]) return 0;
            struct sv_node *closed=&s->nodes[stack[depth-1]];
            if(tag_is(open,"style")&&closed->text)closed->textlen=(int)(tag_begin-closed->text);
            depth--; continue;
        }
        if (s->count == SV_NODES) return 0;
        int idx = s->count++; s->nodes[idx] = (struct sv_node){t, -1, -1, stack[depth-1], 0, 0, 0};
        if(tag_is(&t,"style"))s->nodes[idx].text=p;
        if (last[depth-1] < 0) s->nodes[stack[depth-1]].first = idx;
        else s->nodes[last[depth-1]].next = idx;
        last[depth-1] = idx;
        if (!t.selfclose) { if (depth == SV_DEPTH) return 0; stack[depth] = idx; last[depth++] = -1; }
    }
    return 1;
}
static int sv_ref(struct sv_scene *s, const uint8_t *v, int n, int url)
{
    uint8_t text[512], id[512]; int len = sv_unescape(v, n, text, sizeof text);
    if (len < 0) return -1; v = text; n = len; sv_trim(&v, &n);
    if (url) {
        if (n < 5 || !sv_eq(v, 4, "url(") || v[n-1] != ')') return -1;
        v += 4; n -= 5; sv_trim(&v, &n);
        if (n >= 2 && (v[0] == '\'' || v[0] == '"') && v[n-1] == v[0]) { v++; n -= 2; }
    }
    if (n < 2 || v[0] != '#') return -1; v++; n--;
    for (int i = 0; i < s->count; i++) {
        const uint8_t *raw; int rn;
        if (attr_get(&s->nodes[i].t, "id", &raw, &rn)) continue;
        int dn = sv_unescape(raw, rn, id, sizeof id);
        if (dn != n) continue;
        int j = 0; for (; j < n && v[j] == id[j]; j++) { } if (j == n) return i;
    }
    return -1;
}
static void sv_points(struct gfx_path *p, const struct gfx_matrix *m)
{
    for (int i = 0; i < p->npt; i++) {
        int x = p->pt[i*2], y = p->pt[i*2+1];
        p->pt[i*2] = (int)clampfx(((long long)m->a*x+(long long)m->c*y)/65536+m->e);
        p->pt[i*2+1] = (int)clampfx(((long long)m->b*x+(long long)m->d*y)/65536+m->f);
    }
}
static void sv_geometry(struct gfx_path *p, const struct tag *t, const struct gfx_matrix *m)
{
    static const struct vbxform identity = {65536, 0, 0};
    gfx_path_reset(p);
    /* The old parser's relative/arc arithmetic reads the current point back.
     * Keep it in user space, tighten flattening by an upper bound on the CTM
     * scale, and transform both fill and stroke outlines afterwards. A stroke
     * scaled non-uniformly is NOT a device-space constant-width stroke. The
     * 1/256 user-unit floor is the decoder's existing coordinate precision. */
    int a = m->a < 0 ? -m->a : m->a, b = m->b < 0 ? -m->b : m->b;
    int c = m->c < 0 ? -m->c : m->c, d = m->d < 0 ? -m->d : m->d;
    long long scale = (long long)a+b+c+d; int tol = scale > 65536 ? (int)((8*65536)/scale) : 8;
    gfx_path_tolerance(p, tol);
    const uint8_t *v; int n;
    if (tag_is(t, "path")) { if (!attr_get(t, "d", &v, &n)) parse_path(p, &identity, v, n); }
    else if (tag_is(t, "rect")) build_rect(p, &identity, t);
    else if (tag_is(t, "circle") || tag_is(t, "ellipse")) build_ellipse(p, &identity, t, tag_is(t, "ellipse"));
    else if (tag_is(t, "line")) {
        int x1=0,y1=0,x2=0,y2=0; attr_num(t,"x1",&x1);attr_num(t,"y1",&y1);attr_num(t,"x2",&x2);attr_num(t,"y2",&y2);
        gfx_move_to(p,x1,y1); gfx_line_to(p,x2,y2);
    } else if ((tag_is(t,"polygon") || tag_is(t,"polyline")) && !attr_get(t,"points",&v,&n)) {
        const uint8_t *end = v+n; int x,y,first=1,sx=0,sy=0;
        while (v < end) {
            while (v < end && (is_ws(*v)||*v==',')) v++;
            if (pnum_fx(&v,end,&x)) break;
            while (v < end && (is_ws(*v)||*v==',')) v++;
            if (pnum_fx(&v,end,&y)) break;
            if (first) {gfx_move_to(p,x,y);sx=x;sy=y;first=0;} else gfx_line_to(p,x,y);
        }
        if (!first && tag_is(t,"polygon")) {gfx_line_to(p,sx,sy);gfx_close(p);}
    }
}
/* Account for every full-surface pass, including EMPTY clipped/opacity
 * groups. Charging only path rasterization let thousands of empty groups
 * clear/composite a large canvas without ever consuming the pixel budget. */
static int sv_work(struct sv_scene *s,unsigned passes)
{
    unsigned long work=s->pixels*passes;
    if(work>SV_WORK_LIMIT || s->raster_pixels>SV_WORK_LIMIT-work){s->failed=1;return 0;}
    s->raster_pixels+=work;return 1;
}
static int sv_layer_work(struct sv_scene *s)
{
#ifdef SVG_SCENE_UNMETERED_LAYERS
    (void)s;return 1;
#else
    return sv_work(s,1);
#endif
}
static unsigned char *sv_layer(struct sv_scene *s)
{
    if(!sv_layer_work(s))return 0; /* caller immediately clears this layer */
    unsigned long bytes = s->pixels*4;
    if (s->layers + bytes > SV_LAYER_BYTES) { s->failed = 1; return 0; }
    unsigned char *p = kmalloc(bytes); if (!p) { s->failed = 1; return 0; }
    s->layers += bytes; return p;
}
static void sv_layer_free(struct sv_scene *s, unsigned char *p) { if (p) { kfree(p); s->layers -= s->pixels*4; } }
static void sv_draw(struct sv_scene *, struct gfx_surface *, int, struct sv_style, struct gfx_matrix, int, int, struct sv_box *, int, int, int);
static void sv_contents(struct sv_scene *s, struct gfx_surface *dst, int idx,
                        struct sv_style st, struct gfx_matrix m, int depth,
                        int mode, struct sv_box *box, int vw, int vh)
{
    const struct tag *t = &s->nodes[idx].t;
    if (tag_is(t,"use")) {
#ifndef SVG_SCENE_NO_USE
        const uint8_t *v; int n;
        if (attr_get(t,"href",&v,&n) && attr_get(t,"xlink:href",&v,&n)) return;
        int ref = sv_ref(s,v,n,0); if (ref < 0) return;
        int w=vw,h=vh;
        /* use:auto leaves the referenced viewport's own dimensions intact. */
        sv_dimension(&s->nodes[ref].t,"width",&w);sv_dimension(&s->nodes[ref].t,"height",&h);
        sv_dimension(t,"width",&w);sv_dimension(t,"height",&h);
        sv_draw(s,dst,ref,st,m,depth+1,mode,box,1,w,h);
#endif
    } else if (tag_is(t,"g") || tag_is(t,"svg") || tag_is(t,"symbol") || tag_is(t,"clipPath")) {
        for (int c=s->nodes[idx].first; c>=0 && !s->failed; c=s->nodes[c].next)
            sv_draw(s,dst,c,st,m,depth+1,mode,box,0,vw,vh);
    } else {
        struct gfx_path *p=&s->path;
        s->geometry_bytes += (unsigned long)(t->aend-t->at);
        if(s->geometry_bytes>8UL*1024*1024){s->failed=1;return;}
        sv_geometry(p,t,&m);
        if (p->overflow) {s->failed=1;return;}
        if (!p->npt || (mode != 2 && st.hidden)) return;
        if(mode!=2 && !sv_work(s,!mode&&!st.stroke.none&&st.sw>0?2:1))return;
        if (mode == 2) {
            sv_points(p,&m);
            for(int i=0;i<p->npt;i++) {
                int x=p->pt[2*i],y=p->pt[2*i+1];
                if(!box->any){box->x0=box->x1=x;box->y0=box->y1=y;box->any=1;}
                else {if(x<box->x0)box->x0=x;if(x>box->x1)box->x1=x;if(y<box->y0)box->y0=y;if(y>box->y1)box->y1=y;}
            }
            return;
        }
        int stroked=0;
        if (!mode && !st.stroke.none && st.sw>0) {
            struct gfx_stroke sd={0};sd.width=st.sw;sd.cap=GFX_CAP_BUTT;sd.join=GFX_JOIN_MITER;sd.miter_limit=4<<16;
            gfx_path_reset(&g_spath); stroked=gfx_stroke_path(&g_spath,p,&sd);
            if(g_spath.overflow){s->failed=1;return;}
            if(stroked)sv_points(&g_spath,&m);
        }
        sv_points(p,&m);
        struct paint fill=st.fc?st.color:st.fill, stroke=st.sc?st.color:st.stroke;
        if(mode || !st.fill.none || st.fc) {
            struct gfx_paint fp;
            gfx_paint_solid(&fp,mode?GFX_RGB(255,255,255):GFX_RGB(fill.r,fill.g,fill.b),mode?255:(fill.a*st.fo)>>8);
            ol_raster_fill(dst,p,(mode?st.cliprule:st.fill.evenodd)?GFX_EVENODD:GFX_NONZERO,&fp,0);
        }
        if(stroked) {struct gfx_paint sp;gfx_paint_solid(&sp,GFX_RGB(stroke.r,stroke.g,stroke.b),(stroke.a*st.so)>>8);ol_raster_fill(dst,&g_spath,GFX_NONZERO,&sp,0);}
    }
}
static void sv_draw(struct sv_scene *s, struct gfx_surface *dst, int idx,
                    struct sv_style inherited, struct gfx_matrix parent, int depth,
                    int mode, struct sv_box *box, int instance, int vw, int vh)
{
    if(s->failed)return;
    if(depth>=SV_DEPTH || ++s->visits>SV_VISITS){s->failed=1;return;}
    struct sv_node *node=&s->nodes[idx]; const struct tag *t=&node->t;
    if(node->active)return; /* a local reference cycle contributes no geometry */
    if(tag_is(t,"defs") || tag_is(t,"style") || (!instance&&(tag_is(t,"symbol")||tag_is(t,"clipPath"))))return;
    struct sv_style st=inherited;sv_style_apply(s,idx,&st);if(!st.display)return;
    struct gfx_matrix local,m;
    /* An invalid transform declaration is ignored as a whole. A valid list
     * whose composition exceeds our representable range fails the image. */
    int transform=sv_transform(t,&local);
    if(transform<0){s->failed=1;return;}
    if(!transform)gfx_m_identity(&local);
    if(!sv_matrix(&m,&parent,&local)){s->failed=1;return;}
    /* SVG2 use x/y extend the element's own CTM: userSpaceOnUse clipping
     * follows the translation as well as the instantiated geometry. */
    if(tag_is(t,"use")) {
        struct gfx_matrix xy;gfx_m_identity(&xy);attr_num(t,"x",&xy.e);attr_num(t,"y",&xy.f);
        if(!sv_matrix(&m,&m,&xy)){s->failed=1;return;}
    }
    int viewport_clip=0;struct gfx_matrix viewport_matrix=m;
    if(tag_is(t,"svg")||tag_is(t,"symbol")) {
        struct gfx_matrix xy,view;gfx_m_identity(&xy);
        if(idx){attr_num(t,"x",&xy.e);attr_num(t,"y",&xy.f);}
        if(!instance&&idx){sv_dimension(t,"width",&vw);sv_dimension(t,"height",&vh);}
        if(vw<=0||vh<=0)return;
        int viewport=sv_viewport(t,vw,vh,&view);
        if(viewport<0){s->failed=1;return;}if(!viewport)return;
        if(!sv_matrix(&m,&m,&xy)){s->failed=1;return;}
        viewport_matrix=m;
        const uint8_t *overflow;int olen;
        viewport_clip=idx && !(sv_prop(t,"overflow",&overflow,&olen)&&sv_eq(overflow,olen,"visible"));
        if(!sv_matrix(&m,&m,&view)){s->failed=1;return;}
    }
    node->active=1;
    int clip=-1;const uint8_t *v;int n;
#ifndef SVG_SCENE_NO_CLIP
    if(mode!=2 && sv_prop(t,"clip-path",&v,&n)) {clip=sv_ref(s,v,n,1);if(clip>=0&&!tag_is(&s->nodes[clip].t,"clipPath"))clip=-1;}
#endif
    unsigned char *layer=0,*mask=0;
    struct gfx_surface target=*dst;
    if(mode!=2 && (clip>=0 || viewport_clip || (!mode&&st.opacity<256))) {
        layer=sv_layer(s);if(!layer)goto done;
        gfx_surface_init(&target,layer,dst->w,dst->h,0);ol_raster_clear(&target);
    }
    if(clip>=0) {
        mask=sv_layer(s);if(!mask)goto done;
        struct gfx_surface cm;gfx_surface_init(&cm,mask,dst->w,dst->h,0);ol_raster_clear(&cm);
        struct gfx_matrix ctm=m;
        const struct tag *ct=&s->nodes[clip].t;
        if(!attr_get(ct,"clipPathUnits",&v,&n)&&str_eq(v,n,"objectBoundingBox")) {
            struct sv_box bb={0};struct gfx_matrix id;gfx_m_identity(&id);
            sv_contents(s,dst,idx,st,id,depth+1,2,&bb,vw,vh);
            if(bb.any&&bb.x1>bb.x0&&bb.y1>bb.y0) {
                long long bw=(long long)(bb.x1-bb.x0)*256,bh=(long long)(bb.y1-bb.y0)*256;
                if(bw>SV_MATRIX_MAX||bh>SV_MATRIX_MAX){s->failed=1;goto done;}
                struct gfx_matrix bm;gfx_m_set(&bm,(int)bw,0,0,(int)bh,bb.x0,bb.y0);
                if(!sv_matrix(&ctm,&m,&bm))s->failed=1;
                else sv_draw(s,&cm,clip,sv_original_parent_style(s,clip),ctm,depth+1,1,0,1,vw,vh);
            }
        } else sv_draw(s,&cm,clip,sv_original_parent_style(s,clip),ctm,depth+1,1,0,1,vw,vh);
    }
    /* A symbol/use or nested SVG establishes a viewport. Clip in that
     * viewport's coordinates, before viewBox mapping; otherwise content outside
     * a referenced symbol leaks into its neighbour despite a correct CTM. */
    if(mode!=2&&viewport_clip&&!s->failed) {
        unsigned char *vp=sv_layer(s);if(!vp)goto done;
        struct gfx_surface vm;gfx_surface_init(&vm,vp,dst->w,dst->h,0);ol_raster_clear(&vm);
        struct gfx_path *path=&s->path;gfx_path_reset(path);
        gfx_move_to(path,0,0);gfx_line_to(path,vw,0);gfx_line_to(path,vw,vh);gfx_line_to(path,0,vh);gfx_close(path);
        sv_points(path,&viewport_matrix);
        struct gfx_paint white;gfx_paint_solid(&white,GFX_RGB(255,255,255),255);
        if(!sv_layer_work(s)){sv_layer_free(s,vp);goto done;}
        ol_raster_fill(&vm,path,GFX_NONZERO,&white,0);
        if(mask) {if(!sv_layer_work(s)){sv_layer_free(s,vp);goto done;}ol_mask_multiply(mask+3,vp+3,s->pixels,4,4);sv_layer_free(s,vp);}
        else mask=vp;
    }
    if(!s->failed)sv_contents(s,&target,idx,st,m,depth,mode,box,vw,vh);
    if(layer&&!s->failed) {
        if(!sv_layer_work(s))goto done;
        ol_bitmap_composite(dst,layer,mask,mode?256:st.opacity);
    }
done:
    sv_layer_free(s,mask);sv_layer_free(s,layer);node->active=0;
}
static int sv_render(const struct tag *root,const uint8_t *cur,const uint8_t *end,struct gfx_surface *surf)
{
    struct sv_scene s={0};s.pixels=(unsigned long)surf->w*surf->h;
    if(!sv_work(&s,1))return 0; /* initial output clear in svg_decode */
    int capacity=(int)((end-cur)/3)+1;if(capacity>SV_NODES)capacity=SV_NODES;
    s.nodes=kmalloc(sizeof(*s.nodes)*(unsigned long)capacity);if(!s.nodes)return 0;
    if(!sv_parse(&s,root,cur,end)){kfree(s.nodes);return 0;}
    if(!sv_css_compile(&s)){kfree(s.css);kfree(s.nodes);return 0;}
    gfx_path_init(&s.path,g_pt,SVG_PTCAP,g_sub,SVG_SUBCAP);
    gfx_path_init(&g_spath,g_spt,SVG_SPTCAP,g_ssub,SVG_SSUBCAP);
    struct gfx_matrix id;gfx_m_identity(&id);
    sv_draw(&s,surf,0,sv_default(),id,0,0,0,1,surf->w*256,surf->h*256);
    kfree(s.css);kfree(s.nodes);return !s.failed;
}
