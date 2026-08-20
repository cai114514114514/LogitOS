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

/* 1 if the block masks its own background: `mask-image` (or the -webkit-
 * prefixed spelling) set to anything but `none`.
 *
 * WHY THIS MATTERS MORE THAN IT LOOKS. The modern way to draw a monochrome
 * icon is a solid background clipped by a mask:
 *
 *   .vector-icon{ mask-image:url(...arrow.svg); background-color:#202122 }
 *
 * We do not implement masks. Painting the declaration we DO understand leaves
 * the solid rectangle with none of the shape -- Wikipedia's table of contents
 * grew a row of dark blocks where the expand arrows belong, and a page full
 * of those reads as a rendering crash rather than as a missing icon. So an
 * element whose background is masked paints NO background at all: the icon is
 * invisible, which is what a missing icon should look like, and the layout is
 * unchanged because the box keeps its size.
 *
 * The trade, stated: a mask that is mostly opaque (a photo vignette, say)
 * loses a background it should mostly have shown. Every mask in the corpus is
 * an icon. When masks are implemented this whole function goes away. */
static int decls_masked(const char *d, int dlen)
{
    static const char *keys[] = { "mask-image", "-webkit-mask-image" };
    for (int k = 0; k < 2; k++) {
        int vs, ve;
        if (!find_decl(d, dlen, keys[k], &vs, &ve)) continue;
        while (vs < ve && spc(d[vs])) vs++;
        while (ve > vs && spc(d[ve-1])) ve--;
        if (ve - vs == 4 && !memcmp(d + vs, "none", 4)) continue;
        /* A FRAGMENT REFERENCE IS NOT AN IMAGE MASK, and treating it as one
         * DELETED CONTENT. `mask-image:url("#svg-luminance")` points at an
         * SVG <mask> element in the document; CSS Masking says an unresolvable
         * reference applies NO mask, so the element paints normally. The first
         * version of this function did not look at the value and dropped the
         * background for any mask at all -- and WPT's
         * css-masking/mask-image/mask-mode-to-mask-type.html, four blue
         * squares in the reference, came out ENTIRELY BLANK. That is the
         * failure this whole file is supposed to prevent: refusing to draw
         * something we cannot draw is right, erasing something we could have
         * drawn is not.
         *
         * We resolve no masks of any kind, so the honest split is by which
         * mistake each shape makes when ignored: an IMAGE mask (url(icon.svg),
         * a data: URI, a gradient) is the SHAPE of a monochrome icon, and
         * painting its unclipped rectangle is a dark block where an arrow
         * belongs; a FRAGMENT mask is a reference we cannot follow, and
         * ignoring it shows content that is at worst unclipped. Erring toward
         * showing beats erring toward erasing, every time. */
        int i = vs;
        if (ve - i >= 4 && !memcmp(d + i, "url(", 4)) {
            i += 4;
            while (i < ve && (spc(d[i]) || d[i] == '"' || d[i] == '\'')) i++;
            if (i < ve && d[i] == '#') continue;      /* in-document reference */
        }
        if (ve > vs) return 1;
    }
    return 0;
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
    int do_masked;                          /* mask-image set: the background is
                                             * a SHAPE we cannot cut -- see
                                             * decls_masked */
    int do_radius, px, pct;
    int do_grid, gcols, gtracks[GRID_MAXCOL];
    int gx_set, gx, gy_set, gy;
    int anim;                               /* 0 = untouched, 1 = animated, -1 = none */
    int trans_op;                           /* transition declares opacity/all */
    int lg_set[LGX__COUNT], lg[LGX__COUNT]; /* logical properties, resolved to physical */
    /* The grid properties, kept as TEXT rather than values -- see the
     * grid_raw[] comment in css.h for why, and for the lifetime rule these
     * pointers depend on. They point straight into the declarations block
     * parse_decls was handed, so they are only retained when that block is
     * stable storage: the compiled sheet's private copy, or a node's own
     * style="" attribute. gr_drop() clears them for the one caller whose
     * buffer is not (the out-of-memory rescan). */
    const char *gr[GR__COUNT];
    int gr_len[GR__COUNT];
    int gr_any;
    /* The paint properties, same shape and the same lifetime rule: transform,
     * transform-origin, the background's linear-gradient() call and the shadow
     * list. See parse_xraw() below and css.h's XR_* comment. */
    const char *xr[XR__COUNT];
    int xr_len[XR__COUNT];
    int xr_any;
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

/* The grid properties, in GR_* order. Every one of them is absent from our
 * vendored LibCSS's property table, so this scan is their ONLY producer.
 * find_decl requires a ':' straight after the key, which is what keeps
 * "grid-column" from matching "grid-column-gap" and "grid-template" (were it
 * here) from matching "grid-template-columns". */
static const char *const gr_names[GR__COUNT] = {
    "grid-template-columns", "grid-template-rows", "grid-template-areas",
    "grid-auto-columns", "grid-auto-rows", "grid-auto-flow",
    "grid-column", "grid-row", "grid-area",
    "justify-items", "justify-self"
};

/* The `grid-template` SHORTHAND, split into the halves the longhands name.
 *
 * MEASURED, not guessed. Wikipedia's Vector-2022 skin -- the CONTROL page of
 * this browser's own site scoreboard -- never writes grid-template-columns for
 * its real two-column layout. Its narrow default writes the longhand
 * (`grid-template-columns:minmax(0,1fr)`, ONE column) and the desktop rule,
 * inside @media (min-width:1120px), writes only
 *
 *   grid-template: min-content 1fr min-content / 12.25rem minmax(0,1fr)
 *
 * So a scan that reads only longhands sees the one-column mobile rule and
 * never the two-column desktop one: the table of contents loses its 196 px
 * track, is squeezed to about one character per line, and paints over the
 * article. That is what a screenshot showed after months of the scoreboard
 * calling this page PAINTED -- Chrome's COMPUTED style says
 * `grid-template-columns: 248px 1220px`, so anyone checking the computed value
 * instead of the stylesheet sees nothing wrong.
 *
 * The value is `<rows> / <columns>`. The split is at the FIRST top-level
 * slash -- top level meaning outside parens (minmax(0,1fr), repeat(...)) and
 * outside quotes (the area strings a page may write in the rows position).
 * `grid-template:none` and the areas-only form have no slash and are declined.
 *
 * TWO DELIBERATE LIMITS, both stated rather than silently half-done:
 *  - a half is filled ONLY if that longhand is absent from the SAME block, so
 *    within one block the longhand wins. Real CSS says the later declaration
 *    wins whichever it is; modelling that needs per-declaration ordering this
 *    scan does not keep (find_decl already collapses to the last occurrence
 *    per property). Across blocks the cascade is intact, which is the case
 *    that matters here.
 *  - if the rows half contains a quote it is AREAS, not rows, and the rows
 *    half is left alone; only the columns are taken. Mixed rows+areas in one
 *    shorthand is not modelled.
 * The `grid` shorthand has the same column half and is NOT handled: it also
 * carries auto-flow forms this would misread, and nothing in the corpus uses
 * it. */
static void parse_grid_shorthand(const char *d, int dlen, struct xpatch *p)
{
    int vs, ve;
    if (!find_decl(d, dlen, "grid-template", &vs, &ve)) return;
    const char *v = d + vs; int len = ve - vs;
    int depth = 0, slash = -1, quoted = 0;
    char q = 0;
    for (int i = 0; i < len; i++) {
        char c = v[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '\'' || c == '"') { q = c; quoted = 1; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if (c == '/' && depth == 0) { slash = i; break; }
    }
    if (slash < 0) return;
    if (!p->gr[GR_TEMPL_COLS] && slash + 1 < len) {
        p->gr[GR_TEMPL_COLS] = v + slash + 1;
        p->gr_len[GR_TEMPL_COLS] = len - slash - 1;
        p->gr_any = 1;
    }
    if (!quoted && !p->gr[GR_TEMPL_ROWS] && slash > 0) {
        p->gr[GR_TEMPL_ROWS] = v;
        p->gr_len[GR_TEMPL_ROWS] = slash;
        p->gr_any = 1;
    }
}

static void parse_grid_raw(const char *d, int dlen, struct xpatch *p)
{
    for (int g = 0; g < GR__COUNT; g++) {
        int vs, ve;
        if (!find_decl(d, dlen, gr_names[g], &vs, &ve)) continue;
        if (ve <= vs) continue;
        p->gr[g] = d + vs;
        p->gr_len[g] = ve - vs;
        p->gr_any = 1;
    }
    parse_grid_shorthand(d, dlen, p);   /* after the longhands: see above */
}

/* The one caller whose declarations block is NOT stable storage: the
 * out-of-memory rescan reads the buffer the CALLER owns and rewrites in place,
 * so a retained pointer into it would be read long after it meant something.
 * Grid falls back to its old track list on that path rather than reading text
 * that has moved. */
static void gr_drop(struct xpatch *p)
{
    for (int g = 0; g < GR__COUNT; g++) { p->gr[g] = 0; p->gr_len[g] = 0; }
    p->gr_any = 0;
    /* The paint spans point into the same non-stable buffer and go for the
     * same reason. Forgetting them here would not fail at the free: it would
     * hand browser_paint.c a pointer into a buffer the caller has since
     * rewritten IN PLACE, so the shadow it draws is whatever declaration
     * happens to occupy those bytes now. */
    for (int g = 0; g < XR__COUNT; g++) { p->xr[g] = 0; p->xr_len[g] = 0; }
    p->xr_any = 0;
}

/* ---- the PAINT declarations: transform, box-shadow, gradients ------------
 *
 * Four properties LibCSS has no representation for at all, captured as spans
 * exactly the way the grid properties above are. The argument for text rather
 * than values is in css.h above the XR_* enum and is not repeated here; the
 * one thing to keep in view while editing THIS half is that it runs ONCE PER
 * SHEET, so it may not look at an element and must not try.
 *
 * find_decl already refuses a key that does not start a declaration, which is
 * what keeps `transform` out of `text-transform` and `-webkit-transform`, and
 * `background` out of `background-image` and `background-color`.
 *
 * THE PREFIXED SPELLINGS ARE A FALLBACK, AND ORDER IS NOT THE ARGUMENT FOR IT.
 * This comment used to say the unprefixed spelling wins because "it is later
 * in every sheet that writes both". That is false, and measuring it is what
 * found the right reason. Over the 102 sheets of tests/fixtures/cssweb, split
 * into declaration blocks:
 *
 *     transform         both spellings in 452 blocks -- unprefixed LAST in
 *                       449 of them and FIRST in 3
 *     box-shadow        both in 98 blocks -- unprefixed last in 59, FIRST IN 39
 *
 * Two fifths of the box-shadow blocks put the prefix last, so an order rule
 * would take the prefixed value there. The real reason is that these are
 * DIFFERENT PROPERTIES: they do not cascade against each other at all, and a
 * real browser applies `box-shadow` and drops `-webkit-box-shadow` as an
 * unknown property no matter which came last. So unprefixed always wins, by
 * key order in the table below and not by position in the sheet.
 *
 * WHICH PREFIXES ARE WORTH CARRYING is then a question about blocks that write
 * ONLY a prefix, since every other block is served by the unprefixed key.
 * Measured the same way -- prefix-only blocks, by prefix:
 *
 *     transform         24   (-webkit- 11, -moz- 7, -o- 6, -ms- 2)
 *     box-shadow         3   (-webkit- 3, -moz- 2; one block writes both)
 *     transform-origin   0
 *
 * -moz- and -o- are each worth MORE than -ms- here, which is why all four are
 * in the transform table: taking -webkit- and -ms- alone covered 13 of those
 * 24 blocks. transform-origin's prefixed spellings reach nothing in this
 * corpus and `-webkit-transform-origin` is kept anyway -- it costs one table
 * entry, the corpus is 102 sheets rather than the web, and the alternative is
 * an element that transforms about the wrong point with nothing to explain it.
 * Every prefixed spelling taken here shares the unprefixed GRAMMAR, which is
 * what makes the fallback safe.
 *
 * `-webkit-linear-gradient` is NOT taken, and that asymmetry is the point: the
 * prefixed gradient's angle convention is a different one (0deg = to right,
 * counter-clockwise), so accepting it under the modern rule rotates every one
 * of them by ninety degrees. 22 of those, plus 7 `-moz-`, 7 `-o-` and 28 of
 * the ancient two-point `-webkit-gradient()`. */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive compare of [s,s+n) against a lowercase literal. */
static int ieq(const char *s, int n, const char *lit)
{
    int i = 0;
    for (; i < n; i++) { if (!lit[i] || lc((unsigned char)s[i]) != lit[i]) return 0; }
    return lit[i] == 0;
}

/* Trim whitespace and a trailing `!important` off a value span. The '!' cut is
 * safe because none of the four values can otherwise contain one. */
static void trim_val(const char *d, int *vs, int *ve)
{
    for (int i = *vs; i < *ve; i++) if (d[i] == '!') { *ve = i; break; }
    while (*vs < *ve && spc(d[*vs])) (*vs)++;
    while (*ve > *vs && spc(d[*ve - 1])) (*ve)--;
}

/* The first of `keys` that is declared in the block, trimmed. 1 if found.
 * Order is preference order, and it is the caller's cascade: the unprefixed
 * spelling first, always. */
static int decl_first(const char *d, int dlen, const char *const *keys, int nkeys,
                      int *vs, int *ve)
{
    for (int k = 0; k < nkeys; k++) {
        if (!find_decl(d, dlen, keys[k], vs, ve)) continue;
        trim_val(d, vs, ve);
        if (*ve > *vs) return 1;
    }
    return 0;
}

/* Number of TOP-LEVEL (outside parens and quotes) comma-separated components
 * in [v,v+len). A background with more than one layer is refused rather than
 * half-painted: the layers composite, and drawing only the first is a guess
 * about what is underneath it. */
static int top_commas(const char *v, int len)
{
    int depth = 0, n = 1; char q = 0;
    for (int i = 0; i < len; i++) {
        char c = v[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '\'' || c == '"') { q = c; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (c == ',' && depth == 0) n++;
    }
    return n;
}

/* Locate the `linear-gradient(` CALL inside a background value and return its
 * whole span, parens included. The ident-boundary check on the left is what
 * declines `repeating-linear-gradient(` and `-webkit-linear-gradient(`, both
 * of which contain this name as a substring and neither of which means what
 * this one means. */
static int find_lgrad(const char *v, int len, int *gs, int *ge)
{
    static const char *nm = "linear-gradient";
    int nl = 15;
    for (int i = 0; i + nl < len; i++) {
        if (i > 0 && ident((unsigned char)v[i-1])) continue;
        if (!ieq(v + i, nl, nm)) continue;
        int j = i + nl;
        while (j < len && spc(v[j])) j++;
        if (j >= len || v[j] != '(') continue;
        int depth = 0, k = j;
        for (; k < len; k++) {
            if (v[k] == '(') depth++;
            else if (v[k] == ')') { depth--; if (!depth) { k++; break; } }
        }
        if (depth) return 0;                    /* unbalanced: not a value */
        *gs = i; *ge = k;
        return 1;
    }
    return 0;
}

static void xr_set(struct xpatch *p, int idx, const char *s, int len)
{
    if (len <= 0) return;
    p->xr[idx] = s; p->xr_len[idx] = len; p->xr_any = 1;
}

#ifdef CSS_NEGCTL_NO_XCAPTURE
/* NEGATIVE CONTROL: the capture reverted. Everything else in this file --
 * radius, grid, gaps, the logical properties, the animation approximation --
 * is untouched, so exactly the cases that read cstyle.xraw[] may redden. */
static void parse_xraw(const char *d, int dlen, struct xpatch *p)
{
    (void)d; (void)dlen; (void)p;
    /* The capture helpers stay compiled (they are the thing being reverted,
     * not deleted) so the control differs from the shipped build in exactly
     * one behaviour and not in what the file contains. */
    (void)decl_first; (void)top_commas; (void)find_lgrad; (void)xr_set;
}
#else
static void parse_xraw(const char *d, int dlen, struct xpatch *p)
{
    /* Unprefixed FIRST in every one of these -- see the comment above for why
     * that is a property-identity rule and not a source-order one. */
    static const char *const k_xform[]  = { "transform", "-webkit-transform",
                                            "-moz-transform", "-o-transform",
                                            "-ms-transform" };
    static const char *const k_orig[]   = { "transform-origin", "-webkit-transform-origin" };
    static const char *const k_shadow[] = { "box-shadow", "-webkit-box-shadow",
                                            "-moz-box-shadow" };
    static const char *const k_bg[]     = { "background-image", "background" };
    int vs, ve;

    if (decl_first(d, dlen, k_xform, 5, &vs, &ve))
        xr_set(p, XR_TRANSFORM, d + vs, ve - vs);
    if (decl_first(d, dlen, k_orig, 2, &vs, &ve))
        xr_set(p, XR_TRANSFORM_ORIGIN, d + vs, ve - vs);
    if (decl_first(d, dlen, k_shadow, 3, &vs, &ve))
        xr_set(p, XR_BOX_SHADOW, d + vs, ve - vs);

    /* The gradient is looked for in BOTH `background-image` and the
     * `background` shorthand, because more than a third of them are written in
     * the shorthand. Measured over tests/fixtures/cssweb by splitting the 102
     * sheets into declarations and asking which values contain an unprefixed,
     * non-repeating `linear-gradient(`: 134 in `background-image` and 77 in
     * `background`, so 36% of them. Reading only the longhand would have lost
     * those 77 silently -- the same mistake the grid-template shorthand
     * comment above records, one property along. */
    for (int k = 0; k < 2; k++) {
        if (!find_decl(d, dlen, k_bg[k], &vs, &ve)) continue;
        trim_val(d, &vs, &ve);
        if (ve <= vs) continue;
        if (top_commas(d + vs, ve - vs) != 1) continue;   /* multiple layers: refused */
        int gs, ge;
        if (!find_lgrad(d + vs, ve - vs, &gs, &ge)) continue;
        xr_set(p, XR_BG_IMAGE, d + vs + gs, ge - gs);
        break;                                  /* longhand wins over shorthand */
    }
}
#endif

static void parse_decls(const char *d, int dlen, struct xpatch *p)
{
    memset(p, 0, sizeof *p);
    parse_grid_raw(d, dlen, p);
    parse_xraw(d, dlen, p);
    if (decls_vish(d, dlen)) p->do_none = 1;
    if (decls_masked(d, dlen)) p->do_masked = 1;
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
    int  approx;                           /* the selector had a combinator we
                                            * dropped: see match_xcomp */
};

static void parse_compound(const char *s, int len, struct xcomp *c)
{
    int i = 0;
    c->has_tag = c->has_id = c->ncls = c->bad = c->approx = 0;
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
    /* AN APPROXIMATED SELECTOR MAY NOT END IN A BARE TAG.
     *
     * This file matches a descendant/sibling chain on its LAST compound only
     * -- `a b.c:hover` on `b.c` -- which over-matches by design and is
     * harmless while that compound names a class or an id. It is NOT harmless
     * when the last compound is a plain element name: Wikipedia hides the
     * label inside an icon-only button with
     *
     *   .cdx-button.cdx-button--icon-only span + span { ...clip:rect(1px...) }
     *
     * whose last compound is `span`. Reduced that way it matched EVERY
     * classless <span> in the document, and because that rule carries the
     * visually-hidden pattern this file turns into display:none, every one of
     * them vanished -- which is why twelve of the thirteen entries in
     * Wikipedia's table of contents painted as empty boxes while `(Top)`,
     * whose text is not wrapped in a span, came out fine. Found by dumping our
     * own computed styles over the real page, not by reading this code.
     *
     * A selector with NO combinator (`span{...}`, `.c span` is not one) is
     * exact and still matches: only the approximation is refused, and it is
     * refused on the side that loses content rather than the side that shows
     * too much. */
    if (c->approx && !c->has_id && !c->ncls) return 0;
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

/* Match ONE compound selector straight from text (the uncompiled fallback).
 * `approx` carries what last_compound learned: see match_xcomp. */
static int match_compound(struct node *n, const char *s, int len, int approx)
{
    struct xcomp c;
    parse_compound(s, len, &c);
    c.approx = approx;
    return match_xcomp(n, &c);
}

/* Reduce one comma-separated alternative [s,e) to the compound we actually
 * match on: the LAST compound of a descendant chain, with pseudo-classes
 * stripped (documented simplification -- `a b.c:hover` matches on `b.c`).
 * Returns 0 if there is nothing left to match. */
static int last_compound(const char *s, int start, int end, int *cs, int *ce,
                         int *approx)
{
    while (end > start && spc(s[end-1])) end--;
    int c = end - 1;
    while (c > start && !spc(s[c])) c--;
    *cs = spc(s[c]) ? c + 1 : start;
    *ce = end;
    /* A combinator written WITHOUT spaces (`span+span`, `li>a`, `h2~p`) is one
     * space-delimited token, so the split above leaves it whole and
     * parse_compound would read "span+span" as a tag name. Cut at the last
     * one, and remember that we did. Minified sheets -- which is every sheet
     * that matters here -- write them this way. */
    if (approx) *approx = 0;
    for (int k = *cs; k < *ce; k++)
        if (s[k] == '+' || s[k] == '>' || s[k] == '~') {
            *cs = k + 1;
            if (approx) *approx = 1;
        }
    /* Anything before the last compound -- a descendant chain, or a spaced
     * combinator -- means what we match on is an APPROXIMATION of the
     * selector, not the selector. */
    if (approx)
        for (int k = start; k < *cs; k++)
            if (!spc(s[k])) { *approx = 1; break; }
    while (*cs < *ce && spc(s[*cs])) (*cs)++;
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
        int ap = 0;
        if (last_compound(s, start, i, &cs, &ce, &ap) &&
            match_compound(n, s + cs, ce - cs, ap)) return 1;
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
        int ap = 0;
        if (!last_compound(s, start, i, &cs, &ce, &ap)) continue;
        if (x->nalt >= XSEL_MAXALT) { x->spill = 1; continue; }
        parse_compound(s + cs, ce - cs, &x->alt[x->nalt]);
        x->alt[x->nalt].approx = ap;
        x->nalt++;
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
    /* A masked background is a SHAPE, and we have no mask: painting the solid
     * colour is strictly wrong (see decls_masked). Drop the background and
     * keep the box. */
    if (p->do_masked) st->has_bg = 0;
    if (p->do_radius) {
        if (p->pct > 0) { st->radius_pct = p->pct; st->radius = 0; }
        else { st->radius = p->px; st->radius_pct = 0; }
    }
    if (p->do_grid) {
        st->grid_cols = p->gcols;
        for (int i = 0; i < p->gcols && i < GRID_MAXCOL; i++) st->grid_tracks[i] = p->gtracks[i];
    }
    /* Grid's raw declaration text, merged PER PROPERTY rather than as a block:
     * a node matching two rules that each set a different grid property must
     * end up with both, and later rules win only the properties they name.
     * That is what a cascade does, and copying the whole set would silently
     * make the second rule delete the first one's answers. */
    if (p->gr_any) {
        for (int g = 0; g < GR__COUNT; g++) {
            if (!p->gr[g]) continue;
            int l = p->gr_len[g];
            if (l > 0xffff) l = 0xffff;
            st->grid_raw[g] = p->gr[g];
            st->grid_rawlen[g] = (unsigned short)l;
        }
    }
    /* The paint spans, merged PER PROPERTY for the same reason grid's are: an
     * element matching one rule that sets `transform` and another that sets
     * `box-shadow` must end up with both. */
    if (p->xr_any) {
        for (int g = 0; g < XR__COUNT; g++) {
            if (!p->xr[g]) continue;
            int l = p->xr_len[g];
            if (l > 0xffff) l = 0xffff;
            st->xraw[g] = p->xr[g];
            st->xrawlen[g] = (unsigned short)l;
        }
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
        if (p.do_none || p.do_masked || p.do_radius || p.do_grid || p.gx_set || p.gy_set || p.anim || p.trans_op ||
            p.gr_any || p.xr_any || xpatch_has_logical(&p))
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
        gr_drop(&p);            /* the caller.s buffer moves; see gr_drop() */
        if (p.do_none || p.do_masked || p.do_radius || p.do_grid || p.gx_set || p.gy_set || p.anim || p.trans_op ||
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

/* ======================================================================
 * The paint values, PARSED
 *
 * Everything above this line runs once per SHEET and captures spans. This
 * half runs at the point of use -- where a font size and, for the box-relative
 * parts, a box are in scope -- and turns a span into numbers. The split is the
 * whole design and css.h's XR_* comment argues it; the short version is that
 * `translateY(-50%)`, `0 .5em 1em` and a stop at `62.5%` have no numeric value
 * until an element is named, and inventing one at capture time is exactly how
 * `padding-top:56.25%` became fifty-six pixels.
 *
 * Integer only, string.h only. No libm, no allocator, no LibCSS, nothing from
 * c/lib/gfx and nothing from css_interp.c -- see css.h on the eighteen host
 * source lists that constraint comes from.
 * ====================================================================== */

struct vscan { const char *s; int n, i; };

/* A CSS <number> in MILLI-UNITS (value * 1000), plus the unit token that
 * followed it. Returns 1 on success and leaves *k past the unit.
 *
 * Milli rather than double, and the reason is a link line rather than taste
 * (above). Three decimals is what every consumer here needs: an angle in
 * millidegrees, a percentage in hundredths, a length rounded to whole pixels.
 * DIGITS PAST THE THIRD ARE TRUNCATED, stated because `0.0005em` is
 * representable in a sheet and is not representable here -- it truncates to
 * zero, which is the right direction (a sub-milli length is a sub-pixel
 * nothing) but is a loss and not a rounding.
 *
 * Scientific notation is NOT accepted. css_interp.c's scanner takes it because
 * WPT asks it to; no declaration in tests/fixtures/cssweb writes one, and
 * accepting `1e3px` here would only widen what this file claims to render. */
static int scan_num(struct vscan *k, long long *out, char *unit, int umax)
{
    while (k->i < k->n && spc(k->s[k->i])) k->i++;
    int st = k->i, neg = 0, seen = 0;
    long long v = 0;
    if (k->i < k->n && (k->s[k->i] == '+' || k->s[k->i] == '-')) {
        neg = (k->s[k->i] == '-'); k->i++;
    }
    while (k->i < k->n && k->s[k->i] >= '0' && k->s[k->i] <= '9') {
        if (v < 1000000) v = v * 10 + (k->s[k->i] - '0');
        k->i++; seen = 1;
    }
    v *= 1000;
    if (k->i < k->n && k->s[k->i] == '.') {
        k->i++;
        long long scale = 100;
        while (k->i < k->n && k->s[k->i] >= '0' && k->s[k->i] <= '9') {
            if (scale) { v += (k->s[k->i] - '0') * scale; scale /= 10; }
            k->i++; seen = 1;
        }
    }
    if (!seen) { k->i = st; return 0; }
    int o = 0;
    if (k->i < k->n && k->s[k->i] == '%') { k->i++; if (umax > 1) unit[o++] = '%'; }
    else while (k->i < k->n && ((k->s[k->i] >= 'a' && k->s[k->i] <= 'z') ||
                                (k->s[k->i] >= 'A' && k->s[k->i] <= 'Z'))) {
        if (o < umax - 1) unit[o++] = (char)lc((unsigned char)k->s[k->i]);
        k->i++;
    }
    unit[o] = 0;
    *out = neg ? -v : v;
    return 1;
}

/* Rounded division, half away from zero. Truncation here would put every
 * converted length one step low and always in the same direction, which is the
 * error the gfx rasterizer's own g_acc fix was about one layer down. */
static int mdiv(long long num, long long den)
{
    if (den == 0) return 0;
    if ((num < 0) != (den < 0)) return (int)((num - den / 2) / den);
    return (int)((num + den / 2) / den);
}

/* milli-units + unit -> px. 0 if the unit is one we decline.
 *
 * ex and ch are fs/2, which is what css_interp.c's len_px() uses -- the same
 * approximation in both, deliberately, because two different guesses at an
 * x-height would make the same declaration mean two things depending on which
 * file read it. Viewport units go through css_media_width/height, already this
 * file's dependency (compile_sheet keys its cache on them). */
static int milli_px(long long m, const char *u, int fs, int root, int *out)
{
    if (!u[0]) { *out = 0; return m == 0; }         /* unitless: only 0 is a length */
    if (!strcmp(u, "px"))  { *out = mdiv(m, 1000); return 1; }
    if (!strcmp(u, "em"))  { *out = mdiv(m * fs, 1000); return 1; }
    if (!strcmp(u, "rem")) { *out = mdiv(m * root, 1000); return 1; }
    if (!strcmp(u, "ex") || !strcmp(u, "ch")) { *out = mdiv(m * fs, 2000); return 1; }
    if (!strcmp(u, "pt"))  { *out = mdiv(m * 96, 72 * 1000); return 1; }
    if (!strcmp(u, "pc"))  { *out = mdiv(m * 16, 1000); return 1; }
    if (!strcmp(u, "in"))  { *out = mdiv(m * 96, 1000); return 1; }
    if (!strcmp(u, "cm"))  { *out = mdiv(m * 9600, 254 * 1000); return 1; }
    if (!strcmp(u, "mm"))  { *out = mdiv(m * 9600, 2540 * 1000); return 1; }
    if (!strcmp(u, "q"))   { *out = mdiv(m * 9600, 10160 * 1000); return 1; }
    if (!strcmp(u, "vw"))  { *out = mdiv(m * css_media_width(), 100 * 1000); return 1; }
    if (!strcmp(u, "vh"))  { *out = mdiv(m * css_media_height(), 100 * 1000); return 1; }
    return 0;
}

/* Split [v,len) at top-level whitespace -- outside parens and quotes, so
 * `rgba(0, 0, 0, .5)` and `rgb(0 0 0 / 50%)` are ONE token and not four.
 * Returns the count, capped at `max`. */
static int ws_tokens(const char *v, int len, int *ts, int *te, int max)
{
    int n = 0, i = 0, depth = 0;
    char q = 0;
    while (i < len && n < max) {
        while (i < len && spc(v[i])) i++;
        if (i >= len) break;
        int s = i;
        for (; i < len; i++) {
            char c = v[i];
            if (q) { if (c == q) q = 0; continue; }
            if (c == 0x27 /* ' */ || c == '"') { q = c; continue; }
            if (c == '(') depth++;
            else if (c == ')') { if (depth) depth--; }
            else if (spc(c) && depth == 0) break;
        }
        ts[n] = s; te[n] = i; n++;
    }
    return n;
}

/* The same, at top-level commas. */
static int comma_parts(const char *v, int len, int *ps, int *pe, int max)
{
    int n = 0, i = 0, depth = 0, s = 0;
    char q = 0;
    for (; i < len && n < max; i++) {
        char c = v[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == 0x27 /* ' */ || c == '"') { q = c; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (depth) depth--; }
        else if (c == ',' && depth == 0) { ps[n] = s; pe[n] = i; n++; s = i + 1; }
    }
    if (n < max) { ps[n] = s; pe[n] = len; n++; }
    return n;
}

static void span_trim(const char *v, int *s, int *e)
{
    while (*s < *e && spc(v[*s])) (*s)++;
    while (*e > *s && spc(v[*e - 1])) (*e)--;
}

/* ---------------------------------------------------------- transform-origin */
enum { OK_NONE = 0, OK_LEFT, OK_RIGHT, OK_CENTER, OK_TOP, OK_BOTTOM };

static int origin_kw(const char *v, int len)
{
    if (ieq(v, len, "left")) return OK_LEFT;
    if (ieq(v, len, "right")) return OK_RIGHT;
    if (ieq(v, len, "center")) return OK_CENTER;
    if (ieq(v, len, "top")) return OK_TOP;
    if (ieq(v, len, "bottom")) return OK_BOTTOM;
    return OK_NONE;
}

/* One component -> (value, is_pct). A keyword's percentage is its axis
 * position; `center` is 50% on either axis, which is why the keyword table is
 * shared and the caller decides which axis it landed on. */
static int origin_one(const char *v, int len, int fs, int root, int horiz,
                      int *val, int *pct)
{
    int kw = origin_kw(v, len);
    if (kw) {
        if (kw == OK_CENTER) { *val = 5000; *pct = 1; return 1; }
        if (horiz) {
            if (kw == OK_LEFT) *val = 0;
            else if (kw == OK_RIGHT) *val = 10000;
            else return 0;
        } else {
            if (kw == OK_TOP) *val = 0;
            else if (kw == OK_BOTTOM) *val = 10000;
            else return 0;
        }
        *pct = 1;
        return 1;
    }
    struct vscan k = { v, len, 0 };
    long long m;
    char u[8];
    if (!scan_num(&k, &m, u, 8)) return 0;
    while (k.i < k.n && spc(k.s[k.i])) k.i++;
    if (k.i != k.n) return 0;                      /* trailing junk: not a value */
    if (!strcmp(u, "%")) { *val = (int)(m / 10); *pct = 1; return 1; }
    if (!milli_px(m, u, fs, root, val)) return 0;
    *pct = 0;
    return 1;
}

int css_origin_parse(const char *v, int len, int fs_px, int root_px, struct corigin *out)
{
    if (!out) return 0;
    /* The CSS initial value, and it is filled in FIRST so that an absent or
     * unreadable declaration still leaves a usable origin. (0,0) would rotate
     * and scale every element about its top-left corner, which is a plausible
     * picture and the wrong one -- and the caller could not tell, because a
     * struct full of zeroes is what a failed parse looks like too. */
    out->x = out->y = 5000;
    out->x_pct = out->y_pct = 1;
    if (!v || len <= 0) return 1;

    int ts[4], te[4];
    int n = ws_tokens(v, len, ts, te, 4);
    if (n < 1 || n > 3) return 0;

    if (n == 1) {
        int kw = origin_kw(v + ts[0], te[0] - ts[0]);
        /* `transform-origin: top` is a VERTICAL keyword, so it sets y and
         * leaves x centred -- reading it into x would put the origin on the
         * left edge of every element that writes it (7 of the corpus's 107
         * unprefixed transform-origin declarations). */
        if (kw == OK_TOP || kw == OK_BOTTOM)
            return origin_one(v + ts[0], te[0] - ts[0], fs_px, root_px, 0, &out->y, &out->y_pct);
        return origin_one(v + ts[0], te[0] - ts[0], fs_px, root_px, 1, &out->x, &out->x_pct);
    }

    int a = ts[0], al = te[0] - ts[0], b = ts[1], bl = te[1] - ts[1];
    int ka = origin_kw(v + a, al);
    /* `top left` is legal and means `left top`: when the first component is an
     * exclusively-vertical keyword the pair is written y-then-x. 9 of the
     * corpus's 107 unprefixed transform-origin declarations are written that
     * way. */
    if (ka == OK_TOP || ka == OK_BOTTOM) {
        int t = a, tl = al;
        a = b; al = bl; b = t; bl = tl;
    }
    if (!origin_one(v + a, al, fs_px, root_px, 1, &out->x, &out->x_pct)) return 0;
    if (!origin_one(v + b, bl, fs_px, root_px, 0, &out->y, &out->y_pct)) return 0;
    /* A third component is the Z origin. Parsed for validity and DISCARDED --
     * this is a 2D painter, and silently accepting a value nothing reads is
     * better than refusing a declaration whose 2D half we do render. */
    if (n == 3) {
        int z, zp;
        if (!origin_one(v + ts[2], te[2] - ts[2], fs_px, root_px, 1, &z, &zp)) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------- box-shadow */
int css_shadow_parse(const char *v, int len, int fs_px, int root_px,
                     struct cshadow *out, int max)
{
    if (!v || len <= 0 || !out || max <= 0) return 0;
    int s = 0, e = len;
    span_trim(v, &s, &e);
    for (int i = s; i < e; i++) if (v[i] == '!') { e = i; break; }
    span_trim(v, &s, &e);
    if (e <= s) return 0;
    if (ieq(v + s, e - s, "none")) return 0;

    int ps[CS_MAXSHADOW + 1], pe[CS_MAXSHADOW + 1];
    int np = comma_parts(v + s, e - s, ps, pe, CS_MAXSHADOW + 1);
    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        int cs = ps[i] + s, ce = pe[i] + s;
        span_trim(v, &cs, &ce);
        if (ce <= cs) continue;
        int ts[8], te[8];
        int nt = ws_tokens(v + cs, ce - cs, ts, te, 8);
        int lens[4], nl = 0, inset = 0, bad = 0;
        const char *col = 0;
        int collen = 0;
        for (int t = 0; t < nt; t++) {
            const char *tk = v + cs + ts[t];
            int tl = te[t] - ts[t];
            if (ieq(tk, tl, "inset")) { inset = 1; continue; }
            struct vscan k = { tk, tl, 0 };
            long long m;
            char u[8];
            int px = 0;
            int isnum = scan_num(&k, &m, u, 8);
            if (isnum) while (k.i < k.n && spc(k.s[k.i])) k.i++;
            if (isnum && k.i == k.n && strcmp(u, "%") && milli_px(m, u, fs_px, root_px, &px)) {
                if (nl < 4) lens[nl++] = px; else bad = 1;
                continue;
            }
            /* Not a length and not `inset`: it is the colour. The FIRST such
             * token wins -- CSS lets the colour sit at either end and a second
             * one is a parse error, so taking the first and refusing the rest
             * is the honest reading of a malformed value. */
            if (!col) { col = tk; collen = tl; }
            else bad = 1;
        }
        /* <length>{2,4} is required, and a negative blur is a parse error
         * rather than a zero -- clamping it would paint a hard-edged shadow
         * for a declaration a real browser drops entirely. */
        if (bad || nl < 2 || nl > 4) continue;
        if (nl >= 3 && lens[2] < 0) continue;
        out[n].dx = lens[0];
        out[n].dy = lens[1];
        out[n].blur = nl >= 3 ? lens[2] : 0;
        out[n].spread = nl >= 4 ? lens[3] : 0;
        out[n].color = col;
        out[n].colorlen = collen;
        out[n].inset = inset;
        n++;
    }
    return n;
}

/* ---------------------------------------------------------- linear-gradient */

/* `to <side-or-corner>` -> a direction. 0 if the keywords are not a pair we
 * recognise. */
static int grad_to(const char *v, int len, struct cgradient *out)
{
    int ts[3], te[3];
    int n = ws_tokens(v, len, ts, te, 3);
    if (n < 2 || n > 3 || !ieq(v + ts[0], te[0] - ts[0], "to")) return 0;
    int k1 = origin_kw(v + ts[1], te[1] - ts[1]);
    int k2 = n == 3 ? origin_kw(v + ts[2], te[2] - ts[2]) : OK_NONE;
    if (n == 2) {
        out->dir = CG_DIR_ANGLE;
        switch (k1) {
        case OK_TOP:    out->angle_mdeg = 0;      return 1;
        case OK_RIGHT:  out->angle_mdeg = 90000;  return 1;
        case OK_BOTTOM: out->angle_mdeg = 180000; return 1;
        case OK_LEFT:   out->angle_mdeg = 270000; return 1;
        default: return 0;
        }
    }
    int vert = (k1 == OK_TOP || k1 == OK_BOTTOM) ? k1
             : ((k2 == OK_TOP || k2 == OK_BOTTOM) ? k2 : OK_NONE);
    int horz = (k1 == OK_LEFT || k1 == OK_RIGHT) ? k1
             : ((k2 == OK_LEFT || k2 == OK_RIGHT) ? k2 : OK_NONE);
    if (!vert || !horz) return 0;
    out->dir = CG_DIR_CORNER;
    if (vert == OK_TOP) out->corner = (horz == OK_LEFT) ? CG_CORNER_TL : CG_CORNER_TR;
    else                out->corner = (horz == OK_LEFT) ? CG_CORNER_BL : CG_CORNER_BR;
    return 1;
}

/* A single <angle> component -> millidegrees, or 0 if it is not one. An angle
 * ALWAYS carries a unit -- `linear-gradient(45, ...)` is a parse error and so
 * is a bare 0 -- which is the same rule css_interp.c's ang_rad() states, and
 * for the same reason: a unitless number is a valid <length>, never a valid
 * <angle>, so accepting one here would take a declaration the cascade drops. */
static int grad_angle(const char *v, int len, int *mdeg)
{
    struct vscan k = { v, len, 0 };
    long long m;
    char u[8];
    if (!scan_num(&k, &m, u, 8)) return 0;
    while (k.i < k.n && spc(k.s[k.i])) k.i++;
    if (k.i != k.n) return 0;
    long long d;
    if (!strcmp(u, "deg")) d = m;
    else if (!strcmp(u, "turn")) d = m * 360;
    else if (!strcmp(u, "grad")) d = m * 9 / 10;
    /* rad -> mdeg is m * (180/pi). 57295779/1000000 is 180/pi to eight
     * figures; over the whole legal angle range that is under a thousandth of
     * a degree of error, and it needs no libm. */
    else if (!strcmp(u, "rad")) d = m * 57295779LL / 1000000LL;
    else return 0;
    d %= 360000;
    if (d < 0) d += 360000;
    *mdeg = (int)d;
    return 1;
}

/* One stop position token -> (kind, value). */
static int grad_pos(const char *v, int len, int fs, int root, int *kind, int *val)
{
    struct vscan k = { v, len, 0 };
    long long m;
    char u[8];
    if (!scan_num(&k, &m, u, 8)) return 0;
    while (k.i < k.n && spc(k.s[k.i])) k.i++;
    if (k.i != k.n) return 0;
    if (!strcmp(u, "%")) {
#ifdef CSS_NEGCTL_GRAD_PCT_AS_PX
        /* NEGATIVE CONTROL: a percentage stored as a pixel count. This is the
         * shape of the bug this tree already paid for once (CLAUDE.md, M17:
         * `padding-top:56.25%` became fifty-six pixels) -- nothing is zero,
         * nothing overflows, every gradient still has its stops in a plausible
         * order, and only the stops that are percentages land in the wrong
         * place. */
        *kind = CG_POS_PX; *val = (int)(m / 1000);
#else
        *kind = CG_POS_PCT; *val = (int)(m / 10);   /* hundredths of a percent */
#endif
        return 1;
    }
    if (!milli_px(m, u, fs, root, val)) return 0;
    *kind = CG_POS_PX;
    return 1;
}

int css_gradient_parse(const char *v, int len, int fs_px, int root_px,
                       struct cgradient *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!v || len <= 0) return 0;
    int s = 0, e = len;
    span_trim(v, &s, &e);
    for (int i = s; i < e; i++) if (v[i] == '!') { e = i; break; }
    span_trim(v, &s, &e);

    /* The function name, matched WHOLE. `repeating-linear-gradient`,
     * `-webkit-linear-gradient`, `-moz-` and `-o-` all contain this name and
     * none of them means what it means; a suffix match would render 49 calls
     * in the corpus as something they are not (13 + 22 + 7 + 7, counted over
     * the same 102 sheets as everything else here). */
    int i = s;
    while (i < e && ident((unsigned char)v[i])) i++;
    if (!ieq(v + s, i - s, "linear-gradient")) return 0;
    while (i < e && spc(v[i])) i++;
    if (i >= e || v[i] != '(') return 0;
    int bs = i + 1, depth = 0, be = -1;
    for (int k = i; k < e; k++) {
        if (v[k] == '(') depth++;
        else if (v[k] == ')') { depth--; if (!depth) { be = k; break; } }
    }
    if (be < 0) return 0;

    int ps[CG_MAXSTOP + 3], pe[CG_MAXSTOP + 3];
    int np = comma_parts(v + bs, be - bs, ps, pe, CG_MAXSTOP + 3);
    if (np < 1) return 0;
    for (int k = 0; k < np; k++) { ps[k] += bs; pe[k] += bs; span_trim(v, &ps[k], &pe[k]); }

    int first = 0;
    out->kind = CG_LINEAR;
    out->dir = CG_DIR_ANGLE;
    out->angle_mdeg = 180000;                  /* the implicit direction: to bottom */
    {
        const char *c = v + ps[0];
        int cl = pe[0] - ps[0];
        int ts[3], te[3];
        int nt = ws_tokens(c, cl, ts, te, 3);
        if (nt >= 1 && ieq(c + ts[0], te[0] - ts[0], "to")) {
            if (!grad_to(c, cl, out)) return 0;
            first = 1;
        } else if (nt >= 1 && ieq(c + ts[0], te[0] - ts[0], "in")) {
            /* `in oklab` / `in hsl longer hue`: a colour interpolation space
             * we do not have. Interpolating in sRGB instead is not a rounding
             * difference -- it is visibly a different ramp through the middle
             * -- so the declaration is refused and the background-color shows.
             * 20 in the corpus. */
            return 0;
        } else if (grad_angle(c, cl, &out->angle_mdeg)) {
            first = 1;
        }
    }

    int n = 0;
    for (int k = first; k < np; k++) {
        const char *c = v + ps[k];
        int cl = pe[k] - ps[k];
        if (cl <= 0) return 0;
        int ts[4], te[4];
        int nt = ws_tokens(c, cl, ts, te, 4);
        if (nt < 1 || nt > 3) return 0;
        /* THE COLOUR TOKEN MUST NOT BE A NUMBER, and two different malformed
         * values arrive that way -- both of which would otherwise become a
         * stop with an INVENTED colour rather than a refusal:
         *
         *   `#fff, 40%, #000`  a bare percentage between two colours is a
         *                      COLOUR HINT: it moves the halfway point of the
         *                      ramp without adding a colour. Read as a stop it
         *                      needs a colour nobody wrote; dropped silently it
         *                      moves every colour after it.
         *   `linear-gradient(45, #fff, #000)`
         *                      a unitless first component is a malformed angle
         *                      (an <angle> always carries a unit), so it falls
         *                      through to the stop loop. Read as a colour it
         *                      makes a THREE-stop gradient whose first stop is
         *                      whatever the colour parser does with "45" --
         *                      which is nothing, so the first band comes out
         *                      the previous colour. Found by the gate: this
         *                      row was written as a refusal and returned 1.
         *
         * No CSS colour begins with a digit, a sign or a dot, so "the whole
         * token scans as a number" is an exact test rather than a heuristic. */
        {
            struct vscan nk = { c + ts[0], te[0] - ts[0], 0 };
            long long nm; char nu[8];
            if (scan_num(&nk, &nm, nu, 8)) {
                while (nk.i < nk.n && spc(nk.s[nk.i])) nk.i++;
                if (nk.i == nk.n) return 0;
            }
        }
        int kind1 = CG_POS_AUTO, val1 = 0, kind2 = CG_POS_AUTO, val2 = 0;
        if (nt >= 2 && !grad_pos(c + ts[1], te[1] - ts[1], fs_px, root_px, &kind1, &val1)) return 0;
        if (nt == 3 && !grad_pos(c + ts[2], te[2] - ts[2], fs_px, root_px, &kind2, &val2)) return 0;
        if (n >= CG_MAXSTOP) return 0;
        out->stop[n].color = c + ts[0];
        out->stop[n].colorlen = te[0] - ts[0];
        out->stop[n].pos_kind = kind1;
        out->stop[n].pos = val1;
        n++;
        /* `red 10% 20%` is the two-stop shorthand: the same colour at both
         * positions, i.e. a hard band. Expanded here rather than making every
         * consumer know the shorthand, which keeps a stop list a stop list. */
        if (nt == 3) {
            if (n >= CG_MAXSTOP) return 0;
            out->stop[n] = out->stop[n - 1];
            out->stop[n].pos_kind = kind2;
            out->stop[n].pos = val2;
            n++;
        }
    }
    /* Fewer than two stops is not a gradient -- `linear-gradient(red)` is a
     * parse error, and the 43 one-component values in the corpus are all
     * var() references that survived expansion unresolved. Refusing them
     * leaves the background-color, which is what the page falls back to. */
    if (n < 2) return 0;
    out->nstop = n;
    return 1;
}
