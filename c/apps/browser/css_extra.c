/* css_extra.c -- capture properties our vendored LibCSS doesn't know about.
 * Currently: border-radius (px + %), the "visually hidden" pattern
 * (clip-path:inset(50%) / clip:rect(0,0,0,0)) which real browsers lift out of
 * flow via position:absolute -- we force display:none instead, minimal grid
 * tracks (grid-template-columns repeat(N,1fr)/px/fr lists + px gaps), and the
 * opacity:0 + animation/opacity-transition static-end-state approximation (an
 * element with a non-none animation, or a transition on opacity/all, will
 * become visible, so the opacity:0 hidden flag is cleared -- but only when
 * visibility:hidden isn't also in play, which keeps hover-reveal menus
 * hidden). The author sheet is scanned
 * for simple selectors (tag, .class, #id, tag.class, comma lists; descendant
 * selectors match on their last compound) and inline style= attributes, and
 * matching nodes' cstyle is patched after css_apply. @media blocks are gated
 * on the viewport width (min/max-width only), so tiered rules like Bilibili's
 * repeat(2..17,1fr) breakpoints apply only in their tier. */
#include <string.h>
#include "css.h"
#include "dom.h"

static int spc(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                 (c >= '0' && c <= '9') || c == '-' || c == '_'; }

/* Parse a border-radius value list: first length wins. "6px" -> 6, "50%" -> pct. */
static int parse_radius(const char *v, int len, int *px, int *pct)
{
    int i = 0, n = 0, is_pct = 0;
    while (i < len && spc(v[i])) i++;
    for (; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        if (n > 100000) break;
        n = n * 10 + (v[i] - '0');
    }
    if (i < len && v[i] == '%') is_pct = 1;
    if (n <= 0) return -1;
    if (n > 512) n = 512;
    if (is_pct) { if (n > 50) n = 50; *pct = n; }
    else *px = n;
    return 0;
}

/* 1 if the declarations block [d,dlen) sets border-radius; fills px/pct. */
static int decls_radius(const char *d, int dlen, int *px, int *pct)
{
    const char *key = "border-radius";
    int kl = 13, found = 0;
    for (int i = 0; i + kl < dlen; i++) {
        if (i > 0 && !spc(d[i-1]) && d[i-1] != ';' && d[i-1] != '{') continue;
        if (memcmp(d + i, key, kl)) continue;
        int j = i + kl;
        while (j < dlen && spc(d[j])) j++;
        if (j >= dlen || d[j] != ':') continue;
        j++;
        int vs = j;
        while (j < dlen && d[j] != ';' && d[j] != '}') j++;
        if (parse_radius(d + vs, j - vs, px, pct) == 0) found = 1;
        i = j;
    }
    return found;
}

/* 1 if the declarations block uses the classic "visually hidden" pattern
 * (clip-path:inset(50%) or clip:rect(0...)). Real browsers pull those out of
 * flow with position:absolute; we treat them as display:none. */
static int decls_vish(const char *d, int dlen)
{
    static const char *pats[] = { "clip-path:inset(50%", "clip:rect(0,0,0,0)",
                                  "clip:rect(0 0 0 0)", "clip:rect(1px" };
    for (int p = 0; p < 4; p++) {
        const char *pat = pats[p];
        int pl = (int)strlen(pat);
        for (int i = 0; i + pl <= dlen; i++)
            if (!memcmp(d + i, pat, pl)) return 1;
    }
    return 0;
}

/* Find the LAST "key: value" declaration in [d,dlen); *vs/*ve get the value
 * span. The key must start a declaration (preceded by start/ws/';'/'{', so
 * "gap" never matches inside "column-gap") and be followed by ':'. */
static int find_decl(const char *d, int dlen, const char *key, int *vs, int *ve)
{
    int kl = (int)strlen(key), found = 0;
    for (int i = 0; i + kl < dlen; i++) {
        if (i > 0 && !spc(d[i-1]) && d[i-1] != ';' && d[i-1] != '{') continue;
        if (memcmp(d + i, key, kl)) continue;
        int j = i + kl;
        while (j < dlen && spc(d[j])) j++;
        if (j >= dlen || d[j] != ':') continue;
        j++;
        int s = j;
        while (j < dlen && d[j] != ';' && d[j] != '}') j++;
        *vs = s; *ve = j; found = 1;
        i = j;
    }
    return found;
}

/* Parse one grid track: "<n>px" -> +px, "<n>fr" -> -weight (fr in tenths, so
 * 0.5fr still counts). Returns 0 on success. */
static int parse_track(const char *v, int len, int *i, int *track)
{
    int n = 0, frac = 0, any = 0;
    while (*i < len && spc(v[*i])) (*i)++;
    for (; *i < len && v[*i] >= '0' && v[*i] <= '9'; (*i)++) {
        if (n < 100000) n = n * 10 + (v[*i] - '0');
        any = 1;
    }
    if (*i < len && v[*i] == '.' && *i + 1 < len && v[*i+1] >= '0' && v[*i+1] <= '9') {
        frac = v[*i+1] - '0'; (*i) += 2;
        while (*i < len && v[*i] >= '0' && v[*i] <= '9') (*i)++;
    }
    if (!any) return -1;
    if (*i + 1 < len && v[*i] == 'p' && v[*i+1] == 'x') { *i += 2; *track = n; return 0; }
    if (*i + 1 < len && v[*i] == 'f' && v[*i+1] == 'r') { *i += 2; *track = -(n * 10 + frac); return 0; }
    return -1;                                  /* auto/minmax/...: unsupported */
}

/* grid-template-columns: repeat(N, 1fr) | repeat(N, <px>) | "<px|fr> ..." .
 * Fills tracks[] (>0 px, <0 fr weight in tenths) and *cols. -1 = unsupported. */
static int parse_grid_cols(const char *v, int len, int *cols, int tracks[])
{
    int i = 0;
    while (i < len && spc(v[i])) i++;
    if (i + 6 <= len && !memcmp(v + i, "repeat", 6)) {
        i += 6;
        while (i < len && spc(v[i])) i++;
        if (i >= len || v[i] != '(') return -1;
        i++;
        int n = 0;
        while (i < len && spc(v[i])) i++;
        for (; i < len && v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
        while (i < len && spc(v[i])) i++;
        if (i >= len || v[i] != ',') return -1;
        i++;
        int t;
        if (parse_track(v, len, &i, &t) != 0) return -1;
        if (n < 1) return -1;
        if (n > GRID_MAXCOL) n = GRID_MAXCOL;
        for (int k = 0; k < n; k++) tracks[k] = t;
        *cols = n;
        return 0;
    }
    int n = 0;
    while (n < GRID_MAXCOL) {
        int save = i, t;
        if (parse_track(v, len, &i, &t) != 0) { i = save; break; }
        tracks[n++] = t;
        while (i < len && spc(v[i])) i++;
        if (i >= len || v[i] == ';' || v[i] == '}') break;
    }
    if (!n) return -1;
    *cols = n;
    return 0;
}

/* Parse one or two px lengths: "20px" -> both, "14px 8px" -> row 14, col 8. */
static int parse_gap(const char *v, int len, int *gx, int *gy)
{
    int i = 0, t1, t2;
    if (parse_track(v, len, &i, &t1) != 0 || t1 < 0) return -1;
    int save = i;
    if (parse_track(v, len, &i, &t2) == 0 && t2 >= 0) { *gy = t1; *gx = t2; }
    else { i = save; *gx = *gy = t1; }
    return 0;
}

/* animation / animation-name present? 1 = non-none (will become visible),
 * -1 = explicitly none, 0 = not declared. !important stripped. */
static int decls_anim(const char *d, int dlen)
{
    int vs, ve;
    if (!find_decl(d, dlen, "animation-name", &vs, &ve) &&
        !find_decl(d, dlen, "animation", &vs, &ve)) return 0;
    for (int i = vs; i < ve; i++) if (d[i] == '!') { ve = i; break; }
    while (vs < ve && spc(d[vs])) vs++;
    while (ve > vs && spc(d[ve-1])) ve--;
    if (ve - vs >= 4 && !memcmp(d + vs, "none", 4) &&
        (ve - vs == 4 || !ident(d[vs+4]))) return -1;
    return 1;
}

/* transition declaring opacity (or all) present? 1 = yes, 0 = no.
 * Scroll-reveal patterns (IntersectionObserver adds a class, opacity fades in)
 * look like "opacity:0; transition:opacity ..." in the static stylesheet; the
 * element's steady state IS visible, so we treat it like the animation
 * end-state approximation. Hover-reveal menus additionally carry
 * visibility:hidden, which we still honor (see walk_anim). */
static int decls_trans_op(const char *d, int dlen)
{
    int vs, ve;
    int found = find_decl(d, dlen, "transition-property", &vs, &ve) ||
                find_decl(d, dlen, "transition", &vs, &ve);
    if (!found) return 0;
    for (int i = vs; i < ve; i++) if (d[i] == '!') { ve = i; break; }
    for (int i = vs; i + 3 < ve; i++) {
        if ((d[i]=='o'||d[i]=='O') && i + 7 <= ve && !memcmp(d + i, "opacity", 7) &&
            (i == vs || !ident(d[i-1])) && (i + 7 == ve || !ident(d[i+7]))) return 1;
        if ((d[i]=='a'||d[i]=='A') && i + 3 <= ve && !memcmp(d + i, "all", 3) &&
            (i == vs || !ident(d[i-1])) && (i + 3 == ve || !ident(d[i+3]))) return 1;
    }
    return 0;
}

/* Everything we may want to patch from one declarations block. */
struct xpatch {
    int do_none;                            /* visually-hidden -> display:none */
    int do_radius, px, pct;
    int do_grid, gcols, gtracks[GRID_MAXCOL];
    int gx_set, gx, gy_set, gy;
    int anim;                               /* 0 = untouched, 1 = animated, -1 = none */
    int trans_op;                           /* transition declares opacity/all */
};

static void parse_decls(const char *d, int dlen, struct xpatch *p)
{
    memset(p, 0, sizeof *p);
    if (decls_vish(d, dlen)) p->do_none = 1;
    if (decls_radius(d, dlen, &p->px, &p->pct)) p->do_radius = 1;
    int vs, ve;
    if (find_decl(d, dlen, "grid-template-columns", &vs, &ve) &&
        parse_grid_cols(d + vs, ve - vs, &p->gcols, p->gtracks) == 0)
        p->do_grid = 1;
    if (find_decl(d, dlen, "gap", &vs, &ve) && parse_gap(d + vs, ve - vs, &p->gx, &p->gy) == 0) {
        p->gx_set = p->gy_set = 1;
    }
    if (find_decl(d, dlen, "column-gap", &vs, &ve)) { int x, y;
        if (parse_gap(d + vs, ve - vs, &x, &y) == 0) { p->gx = x; p->gx_set = 1; } }
    if (find_decl(d, dlen, "row-gap", &vs, &ve)) { int x, y;
        if (parse_gap(d + vs, ve - vs, &x, &y) == 0) { p->gy = y; p->gy_set = 1; } }
    if (find_decl(d, dlen, "grid-gap", &vs, &ve) && parse_gap(d + vs, ve - vs, &p->gx, &p->gy) == 0) {
        p->gx_set = p->gy_set = 1;
    }
    if (find_decl(d, dlen, "grid-column-gap", &vs, &ve)) { int x, y;
        if (parse_gap(d + vs, ve - vs, &x, &y) == 0) { p->gx = x; p->gx_set = 1; } }
    if (find_decl(d, dlen, "grid-row-gap", &vs, &ve)) { int x, y;
        if (parse_gap(d + vs, ve - vs, &x, &y) == 0) { p->gy = y; p->gy_set = 1; } }
    p->anim = decls_anim(d, dlen);
    p->trans_op = decls_trans_op(d, dlen);
}

/* Match ONE compound selector (no combinators): [tag][#id][.cls][.cls]... */
static int match_compound(struct node *n, const char *s, int len)
{
    int i = 0, classes = 0, idok = 1, tagok = 1;
    char cls[2][64]; int ncls = 0;
    char id[64]; int has_id = 0;
    char tag[32]; int has_tag = 0;
    while (i < len && ncls < 2) {
        if (s[i] == '.') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) cls[ncls][o++] = s[i++];
            cls[ncls][o] = 0; if (!o) return 0; ncls++;
        } else if (s[i] == '#') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) id[o++] = s[i++];
            id[o] = 0; if (!o) return 0; has_id = 1;
        } else if (!spc(s[i])) {
            int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 31) tag[o++] = s[i++];
            tag[o] = 0; if (!o) return 0; has_tag = 1;
        } else break;
    }
    (void)classes;
    if (has_tag) {
        int k = 0; for (; tag[k]; k++) if (n->tag[k] != tag[k]) { tagok = 0; break; }
        if (tagok && n->tag[k]) tagok = 0;
        if (!tagok) return 0;
    }
    if (has_id) {
        const char *nid = dom_attr(n, "id");
        if (!nid || strcmp(nid, id)) idok = 0;
        if (!idok) return 0;
    }
    for (int c = 0; c < ncls; c++) {
        const char *nc = dom_attr(n, "class");
        if (!nc) return 0;
        /* class attr is a space-separated list; require whole-token match */
        int ok = 0, cl = (int)strlen(cls[c]);
        for (const char *p = nc; *p; p++) {
            if ((p == nc || spc(p[-1])) && !strncmp(p, cls[c], cl) && (!p[cl] || spc(p[cl]))) { ok = 1; break; }
        }
        if (!ok) return 0;
    }
    return 1;
}

/* 1 if any comma-separated selector matches; for descendant selectors only the
 * last compound is matched (documented simplification). */
static int match_selector(struct node *n, const char *s, int len)
{
    int i = 0;
    while (i < len) {
        while (i < len && (spc(s[i]) || s[i] == ',')) i++;
        int start = i;
        while (i < len && s[i] != ',') i++;
        int end = i; while (end > start && spc(s[end-1])) end--;
        /* last compound of a descendant chain */
        int c = end - 1;
        while (c > start && !spc(s[c])) c--;
        int cs = spc(s[c]) ? c + 1 : start;
        /* strip pseudo-classes/elements (:hover etc.) -- we match the base */
        int ce = end;
        for (int k = cs; k < ce; k++) if (s[k] == ':') { ce = k; break; }
        if (ce > cs && match_compound(n, s + cs, ce - cs)) return 1;
    }
    return 0;
}

static void apply_patch(struct node *n, const struct xpatch *p)
{
    if (!n->style) return;
    struct cstyle *st = n->style;
    if (p->do_none) { st->display = DISP_NONE; return; }
    if (p->do_radius) {
        if (p->pct > 0) { st->radius_pct = p->pct; st->radius = 0; }
        else { st->radius = p->px; st->radius_pct = 0; }
    }
    if (p->do_grid) {
        st->grid_cols = p->gcols;
        for (int i = 0; i < p->gcols && i < GRID_MAXCOL; i++) st->grid_tracks[i] = p->gtracks[i];
    }
    if (p->gx_set) st->grid_gap_x = p->gx;
    if (p->gy_set) st->grid_gap_y = p->gy;
    if (p->anim > 0) st->anim = 1;
    else if (p->anim < 0) st->anim = 0;
    if (p->trans_op) st->trans_op = 1;
}

static void walk(struct node *n, const char *sel, int slen, const struct xpatch *p)
{
    if (n->type == N_ELEM && match_selector(n, sel, slen)) apply_patch(n, p);
    for (struct node *c = n->first_child; c; c = c->next) walk(c, sel, slen, p);
}

/* opacity:0 + animation/opacity-transition -> the end state is visible (we
 * have no animation clock, so approximate the static end state): clear the
 * hidden flag opacity:0 set. Only when the hide came from opacity alone --
 * hover-reveal menus also carry visibility:hidden and stay hidden. */
static void walk_anim(struct node *n)
{
    struct cstyle *st = n->style;
    if (n->type == N_ELEM && st && (st->anim || st->trans_op) &&
        st->op0 && !st->vis_hid) st->hidden = 0;
    for (struct node *c = n->first_child; c; c = c->next) walk_anim(c);
}

/* inline style="border-radius:...;animation:..." on each element */
static void walk_inline(struct node *n)
{
    if (n->type == N_ELEM && n->style) {
        const char *st = dom_attr(n, "style");
        if (st) {
            struct xpatch p;
            parse_decls(st, (int)strlen(st), &p);
            apply_patch(n, &p);
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) walk_inline(c);
}

/* ---- @media gating ----
 * Pre-scan the sheet for @media blocks (brace-matched, so @keyframes /
 * @font-face nesting can't confuse it); each block records its span and
 * whether its min/max-width conditions hold for the current viewport. */
#define MAX_MREGION 512
struct mregion { int start, end, active; };
static struct mregion g_mr[MAX_MREGION];
static int g_nmr;

/* 1 if every min-width/max-width in the header holds (no width terms = match,
 * e.g. "@media screen"). Values in px, one decimal digit ("1299.9px"). */
static int media_eval(const char *h, int len)
{
    int vw10 = css_media_width() * 10, ok = 1, terms = 0;
    for (int i = 0; i + 6 < len && ok; i++) {
        int is_min = 0, is_max = 0;
        if (i + 10 <= len && !memcmp(h + i, "min-width:", 10)) { is_min = 1; i += 10; }
        else if (i + 10 <= len && !memcmp(h + i, "max-width:", 10)) { is_max = 1; i += 10; }
        else continue;
        int n = 0, frac = 0, any = 0;
        for (; i < len && h[i] >= '0' && h[i] <= '9'; i++) { n = n * 10 + (h[i] - '0'); any = 1; }
        if (i + 1 < len && h[i] == '.' && h[i+1] >= '0' && h[i+1] <= '9') { frac = h[i+1] - '0'; i++; }
        if (!any) continue;
        terms++;
        int v10 = n * 10 + frac;
        if (is_min && vw10 < v10) ok = 0;
        if (is_max && vw10 > v10) ok = 0;
    }
    (void)terms;
    return ok;
}

static void media_scan(const char *css, int len)
{
    int mdepths[64], nm = 0, depth = 0;
    g_nmr = 0;
    for (int i = 0; i < len; i++) {
        if (css[i] == '@' && i + 6 < len && !memcmp(css + i + 1, "media", 5) &&
            !ident(css[i+6])) {
            int b = i + 6;
            while (b < len && css[b] != '{' && css[b] != ';') b++;
            if (b < len && css[b] == '{') {
                if (g_nmr < MAX_MREGION && nm < 64) {
                    g_mr[g_nmr].start = b + 1;
                    g_mr[g_nmr].end = len;      /* unterminated: runs to EOF */
                    g_mr[g_nmr].active = media_eval(css + i + 6, b - (i + 6));
                    mdepths[nm++] = depth + 1;
                    g_nmr++;
                }
            }
        } else if (css[i] == '{') {
            depth++;
        } else if (css[i] == '}') {
            if (nm > 0 && depth == mdepths[nm-1]) { g_mr[g_nmr - 1].end = i; nm--; }
            if (depth > 0) depth--;
        }
    }
}

/* 1 if position s is not inside any inactive @media block. */
static int media_active_at(int s)
{
    for (int r = 0; r < g_nmr; r++)
        if (s >= g_mr[r].start && s < g_mr[r].end && !g_mr[r].active) return 0;
    return 1;
}

void css_extra_apply(struct node *root, const char *css, int len)
{
    if (!root) return;
    if (!css || len <= 0) { walk_inline(root); walk_anim(root); return; }
    media_scan(css, len);
    int i = 0;
    while (i < len) {
        /* selector up to '{' (skip @-blocks naively: their inner rules still
         * get matched, gated by the media pre-scan above). Stray '}' from
         * closed @-blocks must be skipped too, else it poisons the next
         * selector as a bogus tag name. */
        while (i < len && (spc(css[i]) || css[i] == '}')) i++;
        if (i >= len) break;
        if (css[i] == '@') {                       /* @media ... { -> scan inside */
            while (i < len && css[i] != '{') i++;
            if (i < len) i++;
            continue;
        }
        int s = i;
        while (i < len && css[i] != '{') i++;
        if (i >= len) break;
        int slen = i - s;
        i++;
        int d = i, depth = 1;
        while (i < len && depth) { if (css[i] == '{') depth++; else if (css[i] == '}') depth--; i++; }
        int dlen = i - 1 - d;
        if (dlen <= 0 || !media_active_at(s)) continue;
        struct xpatch p;
        parse_decls(css + d, dlen, &p);
        if (p.do_none || p.do_radius || p.do_grid || p.gx_set || p.gy_set || p.anim || p.trans_op)
            walk(root, css + s, slen, &p);
    }
    walk_inline(root);
    walk_anim(root);
}
