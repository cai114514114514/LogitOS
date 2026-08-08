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

/* ---- CSS logical properties ----------------------------------------------
 *
 * `margin-inline-start`, `padding-block`, `inset` and the rest of that family
 * are what a stylesheet written after about 2020 uses instead of the physical
 * four. Our LibCSS predates all of them, so every one is an unknown property
 * and the box simply does not get the margin. Measured over the
 * tests/fixtures/cssweb corpus (`make audit-css`) they are declared 460+ times
 * across 6 of the 15 pages -- and unlike a colour, a lost margin MOVES text.
 *
 * We render left-to-right, top-to-bottom only: there is no `direction:rtl` and
 * no `writing-mode` in this engine (both are in the audit's "parsed but never
 * read" list). So the mapping is the fixed LTR/horizontal-tb one --
 * inline-start = left, block-start = top -- which is right for every page in
 * the corpus and wrong in exactly the cases where the rest of the engine is
 * already wrong. Doing it any other way would mean claiming a bidi capability
 * that does not exist.
 *
 * The shorthands take 1 or 2 values (start then end); `inset` takes the full
 * 1-to-4 physical shorthand order because that is what it is defined as. */
enum { LGX_ML = 0, LGX_MR, LGX_MT, LGX_MB,
       LGX_PL, LGX_PR, LGX_PT, LGX_PB,
       LGX_LEFT, LGX_RIGHT, LGX_TOP, LGX_BOTTOM, LGX__COUNT };

/* Everything we may want to patch from one declarations block. */
struct xpatch {
    int do_none;                            /* visually-hidden -> display:none */
    int do_radius, px, pct;
    int do_grid, gcols, gtracks[GRID_MAXCOL];
    int gx_set, gx, gy_set, gy;
    int anim;                               /* 0 = untouched, 1 = animated, -1 = none */
    int trans_op;                           /* transition declares opacity/all */
    int lg_set[LGX__COUNT], lg[LGX__COUNT]; /* logical properties, resolved to physical */
};

/* One px length from `v`, advancing *i. Accepts a leading '-'; returns -1 and
 * leaves *i alone for `auto`, a percentage or anything else we cannot turn
 * into a pixel count here (the physical longhand, if any, still cascaded
 * normally through LibCSS -- this pass only fills what LibCSS could not see). */
static int one_px(const char *v, int len, int *i, int *out)
{
    int p = *i;
    while (p < len && spc(v[p])) p++;
    int neg = 0;
    if (p < len && v[p] == '-') { neg = 1; p++; }
    if (p >= len || v[p] < '0' || v[p] > '9') return -1;
    int n = 0;
    while (p < len && v[p] >= '0' && v[p] <= '9') {
        if (n > 100000) break;
        n = n * 10 + (v[p++] - '0');
    }
    if (p < len && v[p] == '.') { p++; while (p < len && v[p] >= '0' && v[p] <= '9') p++; }
    /* Only bare px (or a unitless 0) is a pixel count we can trust here. */
    if (p + 1 < len && v[p] == 'p' && v[p+1] == 'x') p += 2;
    else if (p < len && (ident(v[p]) || v[p] == '%')) return -1;
    *i = p;
    *out = neg ? -n : n;
    return 0;
}

/* `key: a [b]` -> phys_start/phys_end (a alone sets both). */
static void logical_pair(const char *d, int dlen, const char *key,
                         int s_idx, int e_idx, struct xpatch *p)
{
    int vs, ve, i = 0, a, b;
    if (!find_decl(d, dlen, key, &vs, &ve)) return;
    if (one_px(d + vs, ve - vs, &i, &a) != 0) return;
    if (one_px(d + vs, ve - vs, &i, &b) != 0) b = a;
    p->lg[s_idx] = a; p->lg_set[s_idx] = 1;
    p->lg[e_idx] = b; p->lg_set[e_idx] = 1;
}

/* `key: v` -> one physical edge. */
static void logical_one(const char *d, int dlen, const char *key, int idx,
                        struct xpatch *p)
{
    int vs, ve, i = 0, a;
    if (!find_decl(d, dlen, key, &vs, &ve)) return;
    if (one_px(d + vs, ve - vs, &i, &a) != 0) return;
    p->lg[idx] = a; p->lg_set[idx] = 1;
}

static int xpatch_has_logical(const struct xpatch *p)
{
    for (int i = 0; i < LGX__COUNT; i++) if (p->lg_set[i]) return 1;
    return 0;
}

static void parse_logical(const char *d, int dlen, struct xpatch *p)
{
    logical_pair(d, dlen, "margin-inline",  LGX_ML, LGX_MR, p);
    logical_pair(d, dlen, "margin-block",   LGX_MT, LGX_MB, p);
    logical_pair(d, dlen, "padding-inline", LGX_PL, LGX_PR, p);
    logical_pair(d, dlen, "padding-block",  LGX_PT, LGX_PB, p);
    /* Longhands after the shorthands: `margin-inline: 0; margin-inline-start:
     * 8px` must end at 0/8, which is source order for a real cascade and is
     * what this ordering reproduces for the overwhelmingly common case. */
    logical_one(d, dlen, "margin-inline-start",  LGX_ML, p);
    logical_one(d, dlen, "margin-inline-end",    LGX_MR, p);
    logical_one(d, dlen, "margin-block-start",   LGX_MT, p);
    logical_one(d, dlen, "margin-block-end",     LGX_MB, p);
    logical_one(d, dlen, "padding-inline-start", LGX_PL, p);
    logical_one(d, dlen, "padding-inline-end",   LGX_PR, p);
    logical_one(d, dlen, "padding-block-start",  LGX_PT, p);
    logical_one(d, dlen, "padding-block-end",    LGX_PB, p);

    /* inset: the physical 1-4 shorthand (top right bottom left). */
    {
        int vs, ve, i = 0, v[4], n = 0;
        if (find_decl(d, dlen, "inset", &vs, &ve)) {
            while (n < 4 && one_px(d + vs, ve - vs, &i, &v[n]) == 0) n++;
            if (n > 0) {
                int t = v[0];
                int r = n > 1 ? v[1] : t;
                int b = n > 2 ? v[2] : t;
                int l = n > 3 ? v[3] : r;
                p->lg[LGX_TOP] = t;    p->lg_set[LGX_TOP] = 1;
                p->lg[LGX_RIGHT] = r;  p->lg_set[LGX_RIGHT] = 1;
                p->lg[LGX_BOTTOM] = b; p->lg_set[LGX_BOTTOM] = 1;
                p->lg[LGX_LEFT] = l;   p->lg_set[LGX_LEFT] = 1;
            }
        }
    }
    logical_pair(d, dlen, "inset-inline", LGX_LEFT, LGX_RIGHT, p);
    logical_pair(d, dlen, "inset-block",  LGX_TOP,  LGX_BOTTOM, p);
    logical_one(d, dlen, "inset-inline-start", LGX_LEFT, p);
    logical_one(d, dlen, "inset-inline-end",   LGX_RIGHT, p);
    logical_one(d, dlen, "inset-block-start",  LGX_TOP, p);
    logical_one(d, dlen, "inset-block-end",    LGX_BOTTOM, p);
}

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
    parse_logical(d, dlen, p);
    p->anim = decls_anim(d, dlen);
    p->trans_op = decls_trans_op(d, dlen);
}

/* ONE compound selector (no combinators) taken apart: [tag][#id][.cls][.cls].
 *
 * This used to be parsed out of the selector TEXT once per node visited. A
 * rule is matched against every element in the document, so a 19-rule sheet
 * over wikipedia's 5604 elements re-parsed 106,000 selectors to patch a
 * handful of boxes -- 5.8 ms of a 26.8 ms load. The text is parsed once now,
 * at sheet-compile time, and the walk only compares. */
struct xcomp {
    char tag[32]; int has_tag;
    char id[64];  int has_id;
    char cls[2][64]; int ncls;
    int  bad;                              /* unparseable: matches nothing */
};

static void parse_compound(const char *s, int len, struct xcomp *c)
{
    int i = 0;
    c->has_tag = c->has_id = c->ncls = c->bad = 0;
    while (i < len && c->ncls < 2) {
        if (s[i] == '.') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) c->cls[c->ncls][o++] = s[i++];
            c->cls[c->ncls][o] = 0; if (!o) { c->bad = 1; return; } c->ncls++;
        } else if (s[i] == '#') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) c->id[o++] = s[i++];
            c->id[o] = 0; if (!o) { c->bad = 1; return; } c->has_id = 1;
        } else if (!spc(s[i])) {
            int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 31) c->tag[o++] = s[i++];
            c->tag[o] = 0; if (!o) { c->bad = 1; return; } c->has_tag = 1;
        } else break;
    }
}

static int match_xcomp(struct node *n, const struct xcomp *c)
{
    if (c->bad) return 0;
    if (c->has_tag) {
        int k = 0;
        for (; c->tag[k]; k++) if (n->tag[k] != c->tag[k]) return 0;
        if (n->tag[k]) return 0;
    }
    if (c->has_id) {
        const char *nid = dom_attr(n, "id");
        if (!nid || strcmp(nid, c->id)) return 0;
    }
    if (c->ncls) {
        const char *nc = dom_attr(n, "class");
        if (!nc) return 0;
        for (int k = 0; k < c->ncls; k++) {
            /* class attr is a space-separated list; require whole-token match */
            int ok = 0, cl = (int)strlen(c->cls[k]);
            for (const char *p = nc; *p; p++) {
                if ((p == nc || spc(p[-1])) && !strncmp(p, c->cls[k], cl) &&
                    (!p[cl] || spc(p[cl]))) { ok = 1; break; }
            }
            if (!ok) return 0;
        }
    }
    return 1;
}

/* Match ONE compound selector straight from text (the uncompiled fallback). */
static int match_compound(struct node *n, const char *s, int len)
{
    struct xcomp c;
    parse_compound(s, len, &c);
    return match_xcomp(n, &c);
}

/* Reduce one comma-separated alternative [s,e) to the compound we actually
 * match on: the LAST compound of a descendant chain, with pseudo-classes
 * stripped (documented simplification -- `a b.c:hover` matches on `b.c`).
 * Returns 0 if there is nothing left to match. */
static int last_compound(const char *s, int start, int end, int *cs, int *ce)
{
    while (end > start && spc(s[end-1])) end--;
    int c = end - 1;
    while (c > start && !spc(s[c])) c--;
    *cs = spc(s[c]) ? c + 1 : start;
    *ce = end;
    for (int k = *cs; k < *ce; k++) if (s[k] == ':') { *ce = k; break; }
    return *ce > *cs;
}

/* 1 if any comma-separated selector matches (text path: the fallback only). */
static int match_selector(struct node *n, const char *s, int len)
{
    int i = 0;
    while (i < len) {
        while (i < len && (spc(s[i]) || s[i] == ',')) i++;
        int start = i;
        while (i < len && s[i] != ',') i++;
        int cs, ce;
        if (last_compound(s, start, i, &cs, &ce) && match_compound(n, s + cs, ce - cs)) return 1;
    }
    return 0;
}

/* A whole selector list, pre-parsed. Alternatives past XSEL_MAXALT keep their
 * text and are matched the slow way, so a 60-selector comma list stays correct
 * rather than silently losing its tail. */
#define XSEL_MAXALT 6
struct xsel {
    struct xcomp alt[XSEL_MAXALT];
    int nalt;
    int spill;                             /* alternatives that did not fit */
};

static void compile_selector(const char *s, int len, struct xsel *x)
{
    int i = 0;
    x->nalt = 0; x->spill = 0;
    while (i < len) {
        while (i < len && (spc(s[i]) || s[i] == ',')) i++;
        int start = i;
        while (i < len && s[i] != ',') i++;
        int cs, ce;
        if (!last_compound(s, start, i, &cs, &ce)) continue;
        if (x->nalt >= XSEL_MAXALT) { x->spill = 1; continue; }
        parse_compound(s + cs, ce - cs, &x->alt[x->nalt++]);
    }
}

static int match_xsel(struct node *n, const struct xsel *x, const char *s, int len)
{
    for (int i = 0; i < x->nalt; i++)
        if (match_xcomp(n, &x->alt[i])) return 1;
    return x->spill ? match_selector(n, s, len) : 0;
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
    /* Logical properties, already resolved to physical edges by parse_logical.
     * A box offset additionally has to set its has_* flag, or layout treats the
     * value as "not specified" and the number is stored and never read. */
    if (p->lg_set[LGX_ML]) st->ml = p->lg[LGX_ML];
    if (p->lg_set[LGX_MR]) st->mr = p->lg[LGX_MR];
    if (p->lg_set[LGX_MT]) st->mt = p->lg[LGX_MT];
    if (p->lg_set[LGX_MB]) st->mb = p->lg[LGX_MB];
    if (p->lg_set[LGX_PL]) st->pl = p->lg[LGX_PL];
    if (p->lg_set[LGX_PR]) st->pr = p->lg[LGX_PR];
    if (p->lg_set[LGX_PT]) st->pt = p->lg[LGX_PT];
    if (p->lg_set[LGX_PB]) st->pb = p->lg[LGX_PB];
    if (p->lg_set[LGX_LEFT])   { st->left = p->lg[LGX_LEFT];     st->has_left = 1; }
    if (p->lg_set[LGX_RIGHT])  { st->right = p->lg[LGX_RIGHT];   st->has_right = 1; }
    if (p->lg_set[LGX_TOP])    { st->top = p->lg[LGX_TOP];       st->has_top = 1; }
    if (p->lg_set[LGX_BOTTOM]) { st->bottom = p->lg[LGX_BOTTOM]; st->has_bottom = 1; }
    if (p->anim > 0) st->anim = 1;
    else if (p->anim < 0) st->anim = 0;
    if (p->trans_op) st->trans_op = 1;
}

static void walk(struct node *n, const char *sel, int slen, const struct xpatch *p)
{
    if (n->type == N_ELEM && match_selector(n, sel, slen)) apply_patch(n, p);
    for (struct node *c = n->first_child; c; c = c->next) walk(c, sel, slen, p);
}

/* The same walk against a pre-parsed selector -- the compiled path. */
static void walk_x(struct node *n, const struct xsel *x, const char *sel, int slen,
                   const struct xpatch *p)
{
    if (n->type == N_ELEM && match_xsel(n, x, sel, slen)) apply_patch(n, p);
    for (struct node *c = n->first_child; c; c = c->next) walk_x(c, x, sel, slen, p);
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
 * @font-face nesting can't confuse it); each block records its span and whether
 * its query holds.
 *
 * The verdict comes from css_media_matches(), which is LibCSS's own parser and
 * matcher. This file used to carry a scanner that understood `min-width:` and
 * `max-width:` and silently answered "matches" to everything else -- so
 * `@media (prefers-color-scheme: dark)` was a match here while LibCSS, one call
 * later, correctly declined it, and the two disagreed about the same block of
 * the same sheet. Any place in the browser that needs this answer asks for it;
 * nowhere computes it twice. */
#define MAX_MREGION 512
struct mregion { int start, end, active; };
static struct mregion g_mr[MAX_MREGION];
static int g_nmr;

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
                    g_mr[g_nmr].active = css_media_matches(css + i + 6, b - (i + 6));
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

/* ---------------- the sheet, COMPILED once ----------------
 *
 * Everything above this line -- media_scan, the rule loop, parse_decls with its
 * dozen linear searches per declaration block -- reads the stylesheet TEXT and
 * nothing else. None of it depends on the tree being patched. Yet
 * css_extra_apply ran all of it on every call, and css_apply_scoped calls it
 * once per node in scope, so one DOM mutation re-scanned the whole stylesheet
 * once per element in the invalidation scope.
 *
 * Measured host-side (`make bench-css`), one css_extra_apply over a single leaf:
 * 0.71 ms on deepseek's 56 KB expanded sheet, 1.52 ms on wikipedia's 224 KB --
 * for a call whose actual work is matching one element. Times the number of
 * nodes in scope, that was two thirds of every repaint.
 *
 * So the text half is compiled once into the rules that can actually patch
 * something, and only the tree walk runs per call. The rule list is a small
 * subset of the sheet: only border-radius / grid / gap / animation /
 * visually-hidden rules survive, which is tens of rules out of thousands.
 *
 * Cache key is the sheet bytes PLUS the three inputs an @media verdict depends
 * on (viewport width, viewport height, colour scheme) -- media gating is
 * resolved at compile time, so a rule list compiled for one viewport must not
 * be reused at another. A private copy of the source is kept because the
 * selector spans point into it and the caller rewrites its buffer in place. */
struct xrule { int sel, slen; struct xsel x; struct xpatch p; };
static char        *g_src;
static int          g_srclen;
static struct xrule *g_rules;
static int          g_nrules, g_rulecap;
static int          g_compiled;
static int          g_key_vw, g_key_vh, g_key_dark;
static int          g_compiles;            /* test seam; see css_extra_compiles() */

int css_extra_compiles(void) { return g_compiles; }

void *kmalloc(unsigned long);
void  kfree(void *);

static void compile_drop(void)
{
    if (g_src)   { kfree(g_src);   g_src = 0; }
    if (g_rules) { kfree(g_rules); g_rules = 0; }
    g_srclen = g_nrules = g_rulecap = g_compiled = 0;
}

static int rules_push(int sel, int slen, const struct xpatch *p)
{
    if (g_nrules == g_rulecap) {
        int cap = g_rulecap ? g_rulecap * 2 : 64;
        struct xrule *nr = (struct xrule *)kmalloc((unsigned long)cap * sizeof *nr);
        if (!nr) return 0;
        for (int i = 0; i < g_nrules; i++) nr[i] = g_rules[i];
        if (g_rules) kfree(g_rules);
        g_rules = nr; g_rulecap = cap;
    }
    g_rules[g_nrules].sel = sel;
    g_rules[g_nrules].slen = slen;
    g_rules[g_nrules].p = *p;
    compile_selector(g_src + sel, slen, &g_rules[g_nrules].x);
    g_nrules++;
    return 1;
}

/* Scan [css,len) for the rules that can patch something, storing them against a
 * private copy. Returns 1 if g_rules is usable, 0 if the caller must fall back
 * to scanning the text itself (only on allocation failure). */
static int compile_sheet(const char *css, int len)
{
    int vw = css_media_width(), vh = css_media_height(), dark = css_color_scheme();
    if (g_compiled && g_srclen == len && g_key_vw == vw && g_key_vh == vh &&
        g_key_dark == dark && memcmp(g_src, css, (unsigned)len) == 0)
        return 1;

    compile_drop();
    g_compiles++;
    char *cp = (char *)kmalloc((unsigned long)(len ? len : 1));
    if (!cp) return 0;
    memcpy(cp, css, (unsigned)len);
    g_src = cp; g_srclen = len;
    g_key_vw = vw; g_key_vh = vh; g_key_dark = dark;

    media_scan(g_src, len);
    int i = 0;
    while (i < len) {
        while (i < len && (spc(g_src[i]) || g_src[i] == '}')) i++;
        if (i >= len) break;
        if (g_src[i] == '@') { while (i < len && g_src[i] != '{') i++; if (i < len) i++; continue; }
        int s = i;
        while (i < len && g_src[i] != '{') i++;
        if (i >= len) break;
        int slen = i - s;
        i++;
        int d = i, depth = 1;
        while (i < len && depth) { if (g_src[i] == '{') depth++; else if (g_src[i] == '}') depth--; i++; }
        int dlen = i - 1 - d;
        if (dlen <= 0 || !media_active_at(s)) continue;
        struct xpatch p;
        parse_decls(g_src + d, dlen, &p);
        if (p.do_none || p.do_radius || p.do_grid || p.gx_set || p.gy_set || p.anim || p.trans_op ||
            xpatch_has_logical(&p))
            if (!rules_push(s, slen, &p)) { compile_drop(); return 0; }
    }
    g_compiled = 1;
    return 1;
}

/* The pre-cache path, kept verbatim as the allocation-failure fallback. */
static void apply_uncompiled(struct node *root, const char *css, int len)
{
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
        if (p.do_none || p.do_radius || p.do_grid || p.gx_set || p.gy_set || p.anim || p.trans_op ||
            xpatch_has_logical(&p))
            walk(root, css + s, slen, &p);
    }
}

/* Test seam (same shape as css_vars_count): how many rules of the last
 * compiled sheet can actually patch something. Asserting on it is how a test
 * tells "the compiled cache was used" from "the fallback scan happened to
 * produce the same pixels". */
int css_extra_rules(void) { return g_compiled ? g_nrules : -1; }

void css_extra_apply(struct node *root, const char *css, int len)
{
    if (!root) return;
    if (!css || len <= 0) { walk_inline(root); walk_anim(root); return; }
    if (compile_sheet(css, len))
        for (int r = 0; r < g_nrules; r++)
            walk_x(root, &g_rules[r].x, g_src + g_rules[r].sel, g_rules[r].slen, &g_rules[r].p);
    else
        apply_uncompiled(root, css, len);       /* out of memory: scan as before */
    walk_inline(root);
    walk_anim(root);
}
