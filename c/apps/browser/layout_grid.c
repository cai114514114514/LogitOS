/* CSS Grid Layout -- placement, track sizing, alignment.
 *
 * Written against the spec text, not against a memory of it:
 *   css-grid-1 (CRD)  -- s7 track lists, s7.2.3.1 auto-fill/auto-fit,
 *                        s7.3 grid-template-areas, s8 placement,
 *                        s8.5 the grid item placement algorithm,
 *                        s10 alignment, s11.1 the grid sizing algorithm,
 *                        s12.3-12.8 the TRACK SIZING ALGORITHM and its
 *                        "distribute extra space" sub-algorithm.
 *   css-align-3       -- s4 distribution values, s5/s6 self-alignment.
 *
 * The step order in size_tracks() is the spec's and the steps are NOT
 * interchangeable; the comments name the section each one comes from so a
 * future edit can be checked against the source rather than against taste.
 *
 * The one place implementations usually go wrong is distribute_extra_space():
 * a spanning item's intrinsic contribution is NOT split evenly across the
 * tracks it spans. It is distributed by the procedure in s12.5.1 -- equally
 * among the AFFECTED tracks only, freezing each as it reaches its limit,
 * then into non-affected tracks, then beyond limits into a subset chosen by
 * the tracks' max sizing functions. An even split produces sensible-looking
 * numbers with the correct total and wrong individual tracks, which is exactly
 * why tests/unit/grid_test.c's negative control (-DGRID_SPAN_EVEN_SPLIT)
 * replaces this function with an even split and must go red.
 */

#include <stdlib.h>
#include <string.h>
#include "layout_grid.h"

#define INF64  ((long long)1 << 40)

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static long long llmax(long long a, long long b) { return a > b ? a : b; }

/* ========================================================================== */
/* Parsing                                                                    */
/* ========================================================================== */

static int g_isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static int g_isdigit(int c) { return c>='0'&&c<='9'; }
static int g_isident(int c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||g_isdigit(c)||c=='-'||c=='_'||(unsigned char)c>=0x80;
}
static int g_lower(int c) { return (c>='A'&&c<='Z') ? c+32 : c; }

static int ieq(const char *a, int alen, const char *b) {
    int i;
    for (i = 0; i < alen; i++) {
        if (!b[i] || g_lower((unsigned char)a[i]) != b[i]) return 0;
    }
    return b[alen] == 0;
}

struct scan { const char *s; int i, n; };
static void sk(struct scan *z) { while (z->i < z->n && g_isspace((unsigned char)z->s[z->i])) z->i++; }
static int  at(struct scan *z, char c) { sk(z); return z->i < z->n && z->s[z->i] == c; }

/* An identifier run. Returns length, 0 if none. */
static int ident(struct scan *z, const char **p) {
    sk(z);
    *p = z->s + z->i;
    int st = z->i;
    while (z->i < z->n && g_isident((unsigned char)z->s[z->i])) z->i++;
    return z->i - st;
}

/* A signed integer. Returns 1 on success. */
static int integer(struct scan *z, int *out) {
    sk(z);
    int st = z->i, sign = 1;
    if (z->i < z->n && (z->s[z->i]=='+'||z->s[z->i]=='-')) { if (z->s[z->i]=='-') sign=-1; z->i++; }
    if (z->i >= z->n || !g_isdigit((unsigned char)z->s[z->i])) { z->i = st; return 0; }
    long v = 0;
    while (z->i < z->n && g_isdigit((unsigned char)z->s[z->i])) {
        v = v*10 + (z->s[z->i]-'0');
        if (v > 1000000000L) v = 1000000000L;
        z->i++;
    }
    *out = (int)(sign * v);
    return 1;
}

/* A <number> as a millionths-scaled integer, so "0.5" -> 500 when scale=1000. */
static int number(struct scan *z, int scale, long long *out) {
    sk(z);
    int st = z->i, sign = 1;
    if (z->i < z->n && (z->s[z->i]=='+'||z->s[z->i]=='-')) { if (z->s[z->i]=='-') sign=-1; z->i++; }
    int any = 0;
    long long ip = 0;
    while (z->i < z->n && g_isdigit((unsigned char)z->s[z->i])) {
        any = 1;
        if (ip < 100000000LL) ip = ip*10 + (z->s[z->i]-'0');
        z->i++;
    }
    long long fp = 0, fd = 1;
    if (z->i < z->n && z->s[z->i] == '.') {
        z->i++;
        while (z->i < z->n && g_isdigit((unsigned char)z->s[z->i])) {
            any = 1;
            if (fd < 1000000LL) { fp = fp*10 + (z->s[z->i]-'0'); fd *= 10; }
            z->i++;
        }
    }
    if (!any) { z->i = st; return 0; }
    *out = sign * (ip * scale + (fp * scale) / fd);
    return 1;
}

/* <track-breadth>. `font_px` resolves em/rem. Returns 1 on success. */
static int breadth(struct scan *z, int font_px, struct gsize *g)
{
    sk(z);
    int st = z->i;

    /* A number with a unit, or a percentage, or a flex. */
    long long v;
    if (number(z, 1000, &v)) {
        const char *u; int ul = ident(z, &u);
        if (ul == 0 && z->i < z->n && z->s[z->i] == '%') {
            z->i++;
            g->kind = GSF_PCT; g->v = (int)(v / 10);   /* 1000ths -> 100ths of a % */
            return 1;
        }
        if (ieq(u, ul, "fr"))  { g->kind = GSF_FR; g->v = (int)v; return 1; }
        if (ieq(u, ul, "px"))  { g->kind = GSF_PX; g->v = (int)(v/1000); return 1; }
        if (ieq(u, ul, "em") || ieq(u, ul, "rem"))
                               { g->kind = GSF_PX; g->v = (int)((v*font_px)/1000); return 1; }
        if (ul == 0 && v == 0) { g->kind = GSF_PX; g->v = 0; return 1; }
        /* Any other unit (ch, vw, pt, ...) is a length we cannot resolve here.
         * `auto` is the honest fallback: it is what an unsupported <length>
         * degrades to, and it keeps the track in the intrinsic family rather
         * than silently sizing it 0. */
        g->kind = GSF_AUTO; g->v = 0;
        return 1;
    }

    const char *p; int l = ident(z, &p);
    if (ieq(p, l, "auto"))        { g->kind = GSF_AUTO;        g->v = 0; return 1; }
    if (ieq(p, l, "min-content")) { g->kind = GSF_MIN_CONTENT; g->v = 0; return 1; }
    if (ieq(p, l, "max-content")) { g->kind = GSF_MAX_CONTENT; g->v = 0; return 1; }
    z->i = st;
    return 0;
}

static int tl_grow(struct gtracklist *t)
{
    if (t->n < t->cap) return 0;
    int nc = t->cap ? t->cap*2 : 8;
    if (nc > GRID_MAX_TRACKS) nc = GRID_MAX_TRACKS;
    if (t->n >= nc) return -1;
    struct gtrackfn *p = (struct gtrackfn *)realloc(t->tr, (size_t)nc*sizeof *p);
    if (!p) return -1;
    t->tr = p; t->cap = nc;
    return 0;
}
static int tl_add(struct gtracklist *t, const struct gtrackfn *f)
{
    if (tl_grow(t) < 0) return -1;
    t->tr[t->n++] = *f;
    return 0;
}
static int tl_name(struct gtracklist *t, const char *s, int len, int line)
{
    if (len <= 0) return 0;
    if (len > 31) len = 31;
    if (t->nn >= t->ncap) {
        int nc = t->ncap ? t->ncap*2 : 8;
        struct gname *p = (struct gname *)realloc(t->nm, (size_t)nc*sizeof *p);
        if (!p) return -1;
        t->nm = p; t->ncap = nc;
    }
    memcpy(t->nm[t->nn].n, s, (size_t)len);
    t->nm[t->nn].n[len] = 0;
    t->nm[t->nn].line = line;
    t->nn++;
    return 0;
}
/* Append `src` to `dst`, shifting src's line indices by dst's current length. */
static int tl_append(struct gtracklist *dst, const struct gtracklist *src)
{
    int base = dst->n, i;
    for (i = 0; i < src->n; i++) if (tl_add(dst, &src->tr[i]) < 0) return -1;
    for (i = 0; i < src->nn; i++)
        if (tl_name(dst, src->nm[i].n, (int)strlen(src->nm[i].n), src->nm[i].line + base) < 0) return -1;
    return 0;
}

void grid_tracklist_free(struct gtracklist *t)
{
    if (!t) return;
    free(t->tr); free(t->nm);
    memset(t, 0, sizeof *t);
}
void grid_template_free(struct gtemplate *t)
{
    if (!t) return;
    grid_tracklist_free(&t->pre);
    grid_tracklist_free(&t->rep);
    grid_tracklist_free(&t->post);
    t->auto_repeat = GREP_NONE;
}
void grid_areas_free(struct gridareas *a)
{
    if (!a) return;
    free(a->cell);
    memset(a, 0, sizeof *a);
}

/* One <track-size>: <track-breadth> | minmax(a,b) | fit-content(x). */
static int track_size(struct scan *z, int font_px, struct gtrackfn *f)
{
    memset(f, 0, sizeof *f);
    sk(z);
    int st = z->i;
    const char *p; int l = ident(z, &p);
    if (l && at(z, '(')) {
        if (ieq(p, l, "minmax")) {
            z->i++;                       /* '(' */
            if (!breadth(z, font_px, &f->mn)) return -1;
            sk(z); if (z->i >= z->n || z->s[z->i] != ',') return -1;
            z->i++;
            if (!breadth(z, font_px, &f->mx)) return -1;
            sk(z); if (z->i >= z->n || z->s[z->i] != ')') return -1;
            z->i++;
            /* <flex> is not a valid <inflexible-breadth>: an fr min fn is
             * invalid, and the spec's own terminology says a <flex>-sized
             * track's min track sizing function is `auto`. */
            if (f->mn.kind == GSF_FR) { f->mn.kind = GSF_AUTO; f->mn.v = 0; }
            return 0;
        }
        if (ieq(p, l, "fit-content")) {
            z->i++;
            if (!breadth(z, font_px, &f->fc)) return -1;
            sk(z); if (z->i >= z->n || z->s[z->i] != ')') return -1;
            z->i++;
            /* s11.2: fit-content()'s min fn is auto, its max fn is treated as
             * max-content except where the argument clamps it. */
            f->is_fc = 1;
            f->mn.kind = GSF_AUTO;
            f->mx.kind = GSF_MAX_CONTENT;
            return 0;
        }
        return -1;
    }
    z->i = st;
    struct gsize g;
    if (!breadth(z, font_px, &g)) return -1;
    if (g.kind == GSF_FR) { f->mn.kind = GSF_AUTO; f->mn.v = 0; f->mx = g; }
    else                  { f->mn = g; f->mx = g; }
    return 0;
}

/* `[a b c]` -> names on the line before the next track. */
static int line_names(struct scan *z, struct gtracklist *t)
{
    if (!at(z, '[')) return 0;
    z->i++;
    for (;;) {
        sk(z);
        if (z->i >= z->n) return -1;
        if (z->s[z->i] == ']') { z->i++; return 0; }
        const char *p; int l = ident(z, &p);
        if (!l) return -1;
        /* `span` and `auto` are excluded as line names. */
        if (ieq(p, l, "span") || ieq(p, l, "auto")) return -1;
        if (tl_name(t, p, l, t->n) < 0) return -1;
    }
}

static int parse_flat(struct scan *z, int font_px, struct gtracklist *out, int stop_paren);

/* repeat(N, <track-list>) -- the fixed-count form, expanded in place. */
static int repeat_fixed(struct scan *z, int font_px, int count, struct gtracklist *out)
{
    struct gtracklist body;
    memset(&body, 0, sizeof body);
    if (parse_flat(z, font_px, &body, 1) < 0) { grid_tracklist_free(&body); return -1; }
    if (body.n == 0 || count < 1) { grid_tracklist_free(&body); return -1; }
    if ((long long)count * body.n > GRID_MAX_TRACKS) count = GRID_MAX_TRACKS / (body.n ? body.n : 1);
    int k;
    for (k = 0; k < count; k++)
        if (tl_append(out, &body) < 0) { grid_tracklist_free(&body); return -1; }
    grid_tracklist_free(&body);
    return 0;
}

/* Parse tracks and line names until end-of-input, or until ')' if stop_paren. */
static int parse_flat(struct scan *z, int font_px, struct gtracklist *out, int stop_paren)
{
    for (;;) {
        sk(z);
        if (z->i >= z->n) break;
        if (stop_paren && z->s[z->i] == ')') { z->i++; break; }
        if (z->s[z->i] == '[') { if (line_names(z, out) < 0) return -1; continue; }

        /* repeat( */
        int save = z->i;
        const char *p; int l = ident(z, &p);
        if (l && ieq(p, l, "repeat") && at(z, '(')) {
            z->i++;
            int cnt;
            if (!integer(z, &cnt)) return -1;   /* auto-fill/auto-fit handled by caller */
            sk(z); if (z->i >= z->n || z->s[z->i] != ',') return -1;
            z->i++;
            if (repeat_fixed(z, font_px, cnt, out) < 0) return -1;
            continue;
        }
        z->i = save;

        struct gtrackfn f;
        if (track_size(z, font_px, &f) < 0) return -1;
        if (tl_add(out, &f) < 0) return -1;
    }
    return 0;
}

int grid_parse_tracklist(const char *s, int len, int font_px, struct gtracklist *out)
{
    memset(out, 0, sizeof *out);
    if (!s) return -1;
    if (len < 0) len = (int)strlen(s);
    struct scan z = { s, 0, len };
    sk(&z);
    if (z.i < z.n) {
        const char *p; int save = z.i, l = ident(&z, &p);
        if (ieq(p, l, "none")) { sk(&z); if (z.i >= z.n) return 0; }
        z.i = save;
    }
    if (parse_flat(&z, font_px, out, 0) < 0) { grid_tracklist_free(out); return -1; }
    return 0;
}

int grid_parse_template(const char *s, int len, int font_px, struct gtemplate *out)
{
    memset(out, 0, sizeof *out);
    if (!s) return -1;
    if (len < 0) len = (int)strlen(s);
    struct scan z = { s, 0, len };

    sk(&z);
    if (z.i < z.n) {
        const char *p; int save = z.i, l = ident(&z, &p);
        if (ieq(p, l, "none")) { sk(&z); if (z.i >= z.n) return 0; }
        z.i = save;
    }

    struct gtracklist *cur = &out->pre;
    for (;;) {
        sk(&z);
        if (z.i >= z.n) break;
        if (z.s[z.i] == '[') { if (line_names(&z, cur) < 0) goto bad; continue; }

        int save = z.i;
        const char *p; int l = ident(&z, &p);
        if (l && ieq(p, l, "repeat") && at(&z, '(')) {
            z.i++;
            sk(&z);
            int cnt;
            const char *q; int save2 = z.i, ql = ident(&z, &q);
            if (ieq(q, ql, "auto-fill") || ieq(q, ql, "auto-fit")) {
                /* s7.2.3: an auto-repeat can only appear once in a track list. */
                if (out->auto_repeat != GREP_NONE) goto bad;
                out->auto_repeat = ieq(q, ql, "auto-fill") ? GREP_AUTO_FILL : GREP_AUTO_FIT;
                sk(&z); if (z.i >= z.n || z.s[z.i] != ',') goto bad;
                z.i++;
                if (parse_flat(&z, font_px, &out->rep, 1) < 0) goto bad;
                if (out->rep.n == 0) goto bad;
                cur = &out->post;
                continue;
            }
            z.i = save2;
            if (!integer(&z, &cnt)) goto bad;
            sk(&z); if (z.i >= z.n || z.s[z.i] != ',') goto bad;
            z.i++;
            if (repeat_fixed(&z, font_px, cnt, cur) < 0) goto bad;
            continue;
        }
        z.i = save;

        struct gtrackfn f;
        if (track_size(&z, font_px, &f) < 0) goto bad;
        if (tl_add(cur, &f) < 0) goto bad;
    }
    return 0;
bad:
    grid_template_free(out);
    return -1;
}

/* ---- grid-template-areas ------------------------------------------------- */

int grid_parse_areas(const char *s, int len, struct gridareas *out)
{
    memset(out, 0, sizeof *out);
    if (!s) return -1;
    if (len < 0) len = (int)strlen(s);

    /* Two passes: count rows/cols, then fill. */
    int rows = 0, cols = -1, i = 0;
    char (*cells)[32] = NULL;
    int pass;
    for (pass = 0; pass < 2; pass++) {
        rows = 0;
        i = 0;
        for (;;) {
            while (i < len && g_isspace((unsigned char)s[i])) i++;
            if (i >= len) break;
            if (s[i] == 'n' && ieq(s+i, (len-i > 4 ? 4 : len-i), "none")) { i += 4; continue; }
            if (s[i] != '"' && s[i] != '\'') goto bad;
            char q = s[i++];
            int c = 0;
            for (;;) {
                while (i < len && (s[i]==' '||s[i]=='\t')) i++;
                if (i >= len) goto bad;
                if (s[i] == q) { i++; break; }
                if (s[i] == '.') {
                    while (i < len && s[i] == '.') i++;         /* one null cell token */
                    if (pass && cells) cells[rows*cols + c][0] = 0;
                    c++;
                } else if (g_isident((unsigned char)s[i])) {
                    int st = i;
                    while (i < len && g_isident((unsigned char)s[i])) i++;
                    int l = i - st; if (l > 31) l = 31;
                    if (pass && cells) {
                        memcpy(cells[rows*cols + c], s+st, (size_t)l);
                        cells[rows*cols + c][l] = 0;
                    }
                    c++;
                } else {
                    goto bad;                                    /* trash token */
                }
                if (pass == 0 && c > 4096) goto bad;
                if (pass && c >= cols) { if (i < len && s[i] != q) goto bad; }
            }
            if (pass == 0) {
                if (cols < 0) cols = c;
                else if (c != cols) goto bad;                    /* ragged -> invalid */
            } else if (c != cols) goto bad;
            rows++;
        }
        if (pass == 0) {
            if (rows == 0 || cols <= 0) goto bad;
            cells = (char (*)[32])calloc((size_t)rows*cols, 32);
            if (!cells) goto bad;
        }
    }

    /* s7.3: a named area whose cells do not form a single filled rectangle
     * makes the declaration invalid. Checked, not assumed -- placement by area
     * name derives four line indices from the bounding box, and a disconnected
     * area would silently produce a plausible wrong rectangle. */
    {
        int r, c, r2, c2;
        for (r = 0; r < rows; r++) for (c = 0; c < cols; c++) {
            const char *nm = cells[r*cols+c];
            if (!nm[0]) continue;
            int seen = 0;
            for (r2 = 0; r2 < r; r2++) for (c2 = 0; c2 < cols; c2++)
                if (!strcmp(cells[r2*cols+c2], nm)) seen = 1;
            if (seen) continue;
            int r0 = r, c0 = c, r1 = r, c1 = c;
            for (r2 = 0; r2 < rows; r2++) for (c2 = 0; c2 < cols; c2++)
                if (!strcmp(cells[r2*cols+c2], nm)) {
                    if (r2 < r0) r0 = r2;
                    if (r2 > r1) r1 = r2;
                    if (c2 < c0) c0 = c2;
                    if (c2 > c1) c1 = c2;
                }
            for (r2 = r0; r2 <= r1; r2++) for (c2 = c0; c2 <= c1; c2++)
                if (strcmp(cells[r2*cols+c2], nm)) { free(cells); goto bad; }
        }
    }

    out->rows = rows; out->cols = cols; out->cell = cells;
    return 0;
bad:
    free(cells);
    memset(out, 0, sizeof *out);
    return -1;
}

/* ---- <grid-line> --------------------------------------------------------- */

int grid_parse_line(const char *s, int len, struct gline *out)
{
    memset(out, 0, sizeof *out);
    if (!s) { out->kind = GL_AUTO; return 0; }
    if (len < 0) len = (int)strlen(s);
    struct scan z = { s, 0, len };
    int have_span = 0, have_int = 0, n = 0;
    char nm[32]; nm[0] = 0;

    for (;;) {
        sk(&z);
        if (z.i >= z.n) break;
        int v;
        if (integer(&z, &v)) {
            if (have_int || v == 0) return -1;     /* zero is invalid */
            have_int = 1; n = v;
            continue;
        }
        const char *p; int l = ident(&z, &p);
        if (!l) return -1;
        if (ieq(p, l, "span")) { if (have_span) return -1; have_span = 1; continue; }
        if (ieq(p, l, "auto")) {
            if (have_span || have_int || nm[0]) return -1;
            out->kind = GL_AUTO;
            sk(&z);
            return z.i >= z.n ? 0 : -1;
        }
        if (nm[0]) return -1;
        if (l > 31) l = 31;
        memcpy(nm, p, (size_t)l); nm[l] = 0;
    }

    if (have_span) {
        if (have_int && n < 1) return -1;
        out->kind = GL_SPAN;
        out->has_n = (unsigned char)have_int;
        out->n = have_int ? n : 1;
        memcpy(out->name, nm, sizeof nm);
        return 0;
    }
    if (have_int) {
        out->kind = GL_LINE; out->has_n = 1; out->n = n;
        memcpy(out->name, nm, sizeof nm);
        return 0;
    }
    if (nm[0]) {
        out->kind = GL_LINE; out->has_n = 0; out->n = 1;
        memcpy(out->name, nm, sizeof nm);
        return 0;
    }
    out->kind = GL_AUTO;
    return 0;
}

/* Split on '/' at depth 0. */
static int split_slash(const char *s, int len, int *cut)
{
    int d = 0, i;
    for (i = 0; i < len; i++) {
        if (s[i] == '(') d++;
        else if (s[i] == ')') d--;
        else if (s[i] == '/' && d == 0) { *cut = i; return 1; }
    }
    return 0;
}

int grid_parse_span2(const char *s, int len, struct gline *a, struct gline *b)
{
    if (len < 0) len = (int)strlen(s);
    int cut;
    if (!split_slash(s, len, &cut)) {
        if (grid_parse_line(s, len, a) < 0) return -1;
        /* s8.3: when the shorthand omits the end line, a <custom-ident> start
         * sets both edges to that name; anything else leaves the end auto. */
        if (a->kind == GL_LINE && !a->has_n && a->name[0]) *b = *a;
        else memset(b, 0, sizeof *b);
        return 0;
    }
    if (grid_parse_line(s, cut, a) < 0) return -1;
    if (grid_parse_line(s+cut+1, len-cut-1, b) < 0) return -1;
    return 0;
}

int grid_parse_area(const char *s, int len, struct gline out[4])
{
    if (len < 0) len = (int)strlen(s);
    int i, at_ = 0, st = 0, d = 0;
    int cuts[3], nc = 0;
    for (i = 0; i < len; i++) {
        if (s[i]=='(') d++;
        else if (s[i]==')') d--;
        else if (s[i]=='/' && d==0) { if (nc < 3) cuts[nc++] = i; else return -1; }
    }
    const char *seg[4]; int slen[4];
    st = 0;
    for (i = 0; i <= nc; i++) {
        int e = (i < nc) ? cuts[i] : len;
        seg[i] = s + st; slen[i] = e - st;
        st = e + 1;
    }
    at_ = nc + 1;
    for (i = 0; i < at_; i++)
        if (grid_parse_line(seg[i], slen[i], &out[i]) < 0) return -1;
    /* Omitted components: if the missing one is a <custom-ident>, it copies the
     * opposite edge; otherwise it is auto. */
    for (i = at_; i < 4; i++) {
        const struct gline *src = &out[i-2];
        if (i >= 2 && src->kind == GL_LINE && !src->has_n && src->name[0]) out[i] = *src;
        else memset(&out[i], 0, sizeof out[i]);
    }
    return 0;
}

int grid_parse_flow(const char *s, int len, unsigned char *col, unsigned char *dense)
{
    if (len < 0) len = (int)strlen(s);
    *col = 0; *dense = 0;
    struct scan z = { s, 0, len };
    int any = 0;
    for (;;) {
        const char *p; int l = ident(&z, &p);
        if (!l) break;
        if      (ieq(p, l, "row"))    { any = 1; }
        else if (ieq(p, l, "column")) { *col = 1; any = 1; }
        else if (ieq(p, l, "dense"))  { *dense = 1; any = 1; }
        else return -1;
    }
    sk(&z);
    return (any && z.i >= z.n) ? 0 : -1;
}

int grid_parse_align(const char *s, int len)
{
    if (len < 0) len = (int)strlen(s);
    struct scan z = { s, 0, len };
    int r = -1, first = 1;
    for (;;) {
        const char *p; int l = ident(&z, &p);
        if (!l) break;
        if (ieq(p, l, "safe") || ieq(p, l, "unsafe")) continue;   /* accepted, dropped */
        int v = -1;
        if      (ieq(p, l, "auto"))          v = GA_AUTO;
        else if (ieq(p, l, "normal"))        v = GA_NORMAL;
        else if (ieq(p, l, "stretch"))       v = GA_STRETCH;
        else if (ieq(p, l, "start"))         v = GA_START;
        else if (ieq(p, l, "end"))           v = GA_END;
        else if (ieq(p, l, "center"))        v = GA_CENTER;
        else if (ieq(p, l, "flex-start"))    v = GA_FLEX_START;
        else if (ieq(p, l, "flex-end"))      v = GA_FLEX_END;
        else if (ieq(p, l, "self-start"))    v = GA_SELF_START;
        else if (ieq(p, l, "self-end"))      v = GA_SELF_END;
        else if (ieq(p, l, "left"))          v = GA_LEFT;
        else if (ieq(p, l, "right"))         v = GA_RIGHT;
        else if (ieq(p, l, "baseline"))      v = GA_BASELINE;
        else if (ieq(p, l, "first"))         { first = 1; continue; }
        else if (ieq(p, l, "last"))          { first = 0; continue; }
        else if (ieq(p, l, "space-between")) v = GA_SPACE_BETWEEN;
        else if (ieq(p, l, "space-around"))  v = GA_SPACE_AROUND;
        else if (ieq(p, l, "space-evenly"))  v = GA_SPACE_EVENLY;
        else return -1;
        if (v == GA_BASELINE && !first) v = GA_LAST_BASELINE;
        if (r >= 0) return -1;
        r = v;
    }
    sk(&z);
    if (z.i < z.n) return -1;
    return r;
}

/* ========================================================================== */
/* Template expansion (auto-fill / auto-fit)                                  */
/* ========================================================================== */

/* The "definite size" a track contributes when counting auto-repetitions
 * (s7.2.3.1): its max track sizing function if definite, else its min if
 * definite, floored by the min when both are; and never below 1px, which is the
 * UA floor the spec suggests to avoid dividing by zero. Returns -1 if neither
 * function is definite, which makes the repetition count 1. */
static int repeat_track_px(const struct gtrackfn *f, int avail)
{
    int mn = -1, mx = -1;
    if (f->mn.kind == GSF_PX)  mn = f->mn.v;
    if (f->mn.kind == GSF_PCT && avail != GRID_INDEFINITE) mn = (int)((long long)avail*f->mn.v/10000);
    if (f->mx.kind == GSF_PX)  mx = f->mx.v;
    if (f->mx.kind == GSF_PCT && avail != GRID_INDEFINITE) mx = (int)((long long)avail*f->mx.v/10000);
    int r;
    if (mx >= 0 && mn >= 0) r = imax(mx, mn);
    else if (mx >= 0)       r = mx;
    else if (mn >= 0)       r = mn;
    else                    return -1;
    return r < 1 ? 1 : r;
}

static int fixed_sum(const struct gtracklist *t, int avail, int *ok)
{
    int i, s = 0;
    for (i = 0; i < t->n; i++) {
        int p = repeat_track_px(&t->tr[i], avail);
        if (p < 0) { *ok = 0; return 0; }
        s += p;
    }
    return s;
}

int grid_template_expand(const struct gtemplate *t, int avail, int gap,
                         struct gtracklist *out, int *nrep)
{
    memset(out, 0, sizeof *out);
    int reps = 1;

    if (t->auto_repeat != GREP_NONE && t->rep.n > 0) {
        if (avail == GRID_INDEFINITE) {
            reps = 1;                       /* "Otherwise, the track list repeats only once" */
        } else {
            int ok = 1;
            int R  = t->rep.n;
            int rp = fixed_sum(&t->rep,  avail, &ok);
            int pp = fixed_sum(&t->pre,  avail, &ok);
            int sp = fixed_sum(&t->post, avail, &ok);
            int P  = t->pre.n, S = t->post.n;
            if (!ok || rp <= 0) {
                reps = 1;                   /* not all track sizes definite */
            } else {
                /* total(n) = pp+sp + (P+S-1)*gap + n*(rp + R*gap) <= avail */
                long long room = (long long)avail - pp - sp - (long long)(P + S - 1)*gap;
                long long per  = (long long)rp + (long long)R*gap;
                long long n    = per > 0 ? room / per : 1;
                if (n < 1) n = 1;
                if (n * R > GRID_MAX_TRACKS) n = GRID_MAX_TRACKS / (R ? R : 1);
                reps = (int)n;
            }
        }
    } else {
        reps = 0;
    }

    if (tl_append(out, &t->pre) < 0) goto bad;
    int k;
    for (k = 0; k < reps; k++) if (tl_append(out, &t->rep) < 0) goto bad;
    if (tl_append(out, &t->post) < 0) goto bad;
    if (nrep) *nrep = reps;
    return 0;
bad:
    grid_tracklist_free(out);
    return -1;
}

/* ========================================================================== */
/* Placement (css-grid-1 s8.3, s8.4, s8.5)                                    */
/* ========================================================================== */

/* Explicit-grid line-name lookup. `nm`/`nn` come from the expanded template
 * plus the implicitly-assigned names grid-template-areas generates. */
struct lnames { struct gname *nm; int nn; int nlines; };

static int name_on_line(const struct lnames *L, const char *n, int line)
{
    int i;
    for (i = 0; i < L->nn; i++)
        if (L->nm[i].line == line && !strcmp(L->nm[i].n, n)) return 1;
    return 0;
}

/* The Nth (1-based) line named `n`, counting forward from line 1. If fewer than
 * N such lines exist, s8.3 says all implicit lines are assumed to carry the
 * name, so counting continues past the explicit grid's end. */
static int nth_named_fwd(const struct lnames *L, const char *n, int N)
{
    int line, k = 0;
    for (line = 1; line <= L->nlines; line++)
        if (name_on_line(L, n, line - 1) && ++k == N) return line;
    return L->nlines + (N - k);
}
/* The Nth line named `n` counting BACKWARD from the explicit grid's end line. */
static int nth_named_bwd(const struct lnames *L, const char *n, int N)
{
    int line, k = 0;
    for (line = L->nlines; line >= 1; line--)
        if (name_on_line(L, n, line - 1) && ++k == N) return line;
    return 1 - (N - k);
}
/* The Nth line named `n` strictly after `from` (dir +1) or before (dir -1),
 * extending into the implicit grid on that side if there are not enough. */
static int nth_named_from(const struct lnames *L, const char *n, int N, int from, int dir)
{
    int line, k = 0;
    if (dir > 0) {
        for (line = from + 1; line <= L->nlines; line++)
            if (name_on_line(L, n, line - 1) && ++k == N) return line;
        int base = imax(from + 1, L->nlines);
        return base + (N - k);
    }
    for (line = from - 1; line >= 1; line--)
        if (name_on_line(L, n, line - 1) && ++k == N) return line;
    int base = imin(from - 1, 1);
    return base - (N - k);
}

/* Resolve one <grid-line> that names a definite line. Returns 1 and writes
 * *out, or 0 if the value contributes no definite line (auto or a span). */
static int resolve_definite(const struct gline *g, const struct lnames *L, int is_start, int *out)
{
    if (g->kind != GL_LINE) return 0;
    if (!g->has_n && g->name[0]) {
        /* s8.3 case 1: match a named grid area's edge first. */
        char probe[40];
        int l = (int)strlen(g->name);
        if (l > 31) l = 31;
        memcpy(probe, g->name, (size_t)l);
        strcpy(probe + l, is_start ? "-start" : "-end");
        int line;
        for (line = 1; line <= L->nlines; line++)
            if (name_on_line(L, probe, line - 1)) { *out = line; return 1; }
        /* Otherwise treat as `1 <ident>`. */
        *out = nth_named_fwd(L, g->name, 1);
        return 1;
    }
    if (g->name[0]) {
        *out = g->n > 0 ? nth_named_fwd(L, g->name, g->n)
                        : nth_named_bwd(L, g->name, -g->n);
        return 1;
    }
    *out = g->n > 0 ? g->n : L->nlines + 1 + g->n;
    return 1;
}

struct axpl { unsigned char definite; int s, e, span; };

/* s8.3 + s8.6 conflict handling, for one axis. */
static void resolve_axis(struct gline start, struct gline end,
                         const struct lnames *L, struct axpl *p)
{
    /* "If the placement contains two spans, remove the one contributed by the
     * end grid-placement property." */
    if (start.kind == GL_SPAN && end.kind == GL_SPAN) memset(&end, 0, sizeof end);

    int s, e, hs, he;
    hs = resolve_definite(&start, L, 1, &s);
    he = resolve_definite(&end,   L, 0, &e);

    if (hs && he) {
        if (s > e) { int t = s; s = e; e = t; }
        if (s == e) e = s + 1;                 /* "remove the end line" */
        p->definite = 1; p->s = s; p->e = e; p->span = e - s;
        return;
    }
    if (hs) {
        if (end.kind == GL_SPAN) {
            e = end.name[0] ? nth_named_from(L, end.name, end.n, s, +1) : s + end.n;
            if (e <= s) e = s + 1;
        } else e = s + 1;
        p->definite = 1; p->s = s; p->e = e; p->span = e - s;
        return;
    }
    if (he) {
        if (start.kind == GL_SPAN) {
            s = start.name[0] ? nth_named_from(L, start.name, start.n, e, -1) : e - start.n;
            if (s >= e) s = e - 1;
        } else s = e - 1;
        p->definite = 1; p->s = s; p->e = e; p->span = e - s;
        return;
    }
    /* Auto-placed: only a span survives, and "if the placement contains only a
     * span for a named line, replace it with a span of 1". */
    {
        int span = 1;
        const struct gline *g = (start.kind == GL_SPAN) ? &start
                              : (end.kind   == GL_SPAN) ? &end : NULL;
        if (g) span = g->name[0] ? 1 : (g->n > 0 ? g->n : 1);
        p->definite = 0; p->s = 0; p->e = 0; p->span = span;
    }
}

/* Occupancy over the implicit grid, in (major, minor) coordinates where major
 * is the axis grid-auto-flow flows along. Rows grow on demand. */
struct occ { unsigned char *b; int w, h, cap; };

static int occ_need(struct occ *o, int h)
{
    if (h <= o->h) return 0;
    if (h > GRID_MAX_TRACKS) return -1;
    if (h > o->cap) {
        int nc = o->cap ? o->cap*2 : 16;
        while (nc < h) nc *= 2;
        if (nc > GRID_MAX_TRACKS) nc = GRID_MAX_TRACKS;
        unsigned char *p = (unsigned char *)realloc(o->b, (size_t)nc * o->w);
        if (!p) return -1;
        memset(p + (size_t)o->cap * o->w, 0, (size_t)(nc - o->cap) * o->w);
        o->b = p; o->cap = nc;
    }
    o->h = h;
    return 0;
}
static int occ_free_at(struct occ *o, int maj, int min, int mspan, int nspan)
{
    int r, c;
    if (min < 0 || min + nspan > o->w) return 0;
    for (r = maj; r < maj + mspan; r++) {
        if (r >= o->h) continue;             /* not yet materialised = empty */
        for (c = min; c < min + nspan; c++)
            if (o->b[(size_t)r*o->w + c]) return 0;
    }
    return 1;
}
static int occ_set(struct occ *o, int maj, int min, int mspan, int nspan)
{
    if (occ_need(o, maj + mspan) < 0) return -1;
    int r, c;
    for (r = maj; r < maj + mspan; r++)
        for (c = imax(min,0); c < imin(min + nspan, o->w); c++)
            o->b[(size_t)r*o->w + c] = 1;
    return 0;
}

/* Build the line-name table for one axis: the expanded template's names, plus
 * the implicitly-assigned `<area>-start` / `<area>-end` names that
 * grid-template-areas generates (s7.3). */
static int build_names(const struct gtracklist *tl, const struct gridareas *ar,
                       int is_col, int nlines, struct lnames *L)
{
    int extra = ar ? (ar->rows * ar->cols * 2) : 0;
    L->nm = (struct gname *)calloc((size_t)(tl->nn + extra + 1), sizeof *L->nm);
    if (!L->nm) return -1;
    L->nn = 0;
    int i;
    for (i = 0; i < tl->nn; i++) L->nm[L->nn++] = tl->nm[i];

    if (ar) {
        int r, c, r2, c2;
        for (r = 0; r < ar->rows; r++) for (c = 0; c < ar->cols; c++) {
            const char *nm = ar->cell[r*ar->cols + c];
            if (!nm[0]) continue;
            int seen = 0;
            for (r2 = 0; r2 <= r; r2++) for (c2 = 0; c2 < ar->cols; c2++) {
                if (r2 == r && c2 >= c) break;
                if (!strcmp(ar->cell[r2*ar->cols+c2], nm)) seen = 1;
            }
            if (seen) continue;
            int r0 = r, c0 = c, r1 = r, c1 = c;
            for (r2 = 0; r2 < ar->rows; r2++) for (c2 = 0; c2 < ar->cols; c2++)
                if (!strcmp(ar->cell[r2*ar->cols+c2], nm)) {
                    if (r2 < r0) r0 = r2;
                    if (r2 > r1) r1 = r2;
                    if (c2 < c0) c0 = c2;
                    if (c2 > c1) c1 = c2;
                }
            int a = is_col ? c0 : r0, b = is_col ? c1 : r1;
            char buf[40];
            int l = (int)strlen(nm); if (l > 31) l = 31;
            memcpy(buf, nm, (size_t)l); strcpy(buf+l, "-start");
            memcpy(L->nm[L->nn].n, buf, sizeof L->nm[L->nn].n - 1);
            L->nm[L->nn].n[sizeof L->nm[L->nn].n - 1] = 0;
            L->nm[L->nn].line = a;
            L->nn++;
            memcpy(buf, nm, (size_t)l); strcpy(buf+l, "-end");
            memcpy(L->nm[L->nn].n, buf, sizeof L->nm[L->nn].n - 1);
            L->nm[L->nn].n[sizeof L->nm[L->nn].n - 1] = 0;
            L->nm[L->nn].line = b + 1;
            L->nn++;
        }
    }
    L->nlines = nlines;
    return 0;
}

/* Order-modified document order: stable sort by `order`. */
static void order_sort(int *idx, const struct griditem *it, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        int k = idx[i];
        for (j = i - 1; j >= 0 && it[idx[j]].order > it[k].order; j--) idx[j+1] = idx[j];
        idx[j+1] = k;
    }
}

struct placement {
    struct axpl col, row;
};

/* The full placement pass. Writes 0-based track indices into `out`. */
static int place_items(const struct gridcfg *cfg,
                       const struct griditem *items, int nitems,
                       const struct lnames *Lc, const struct lnames *Lr,
                       struct gridpos *out, int *pncols, int *pnrows,
                       int *pcol0, int *prow0)
{
    int i, rc = -1;
    struct placement *pl = (struct placement *)calloc((size_t)(nitems ? nitems : 1), sizeof *pl);
    int *idx = (int *)malloc((size_t)(nitems ? nitems : 1) * sizeof *idx);
    struct occ o; memset(&o, 0, sizeof o);
    if (!pl || !idx) goto done;

    for (i = 0; i < nitems; i++) idx[i] = i;
    order_sort(idx, items, nitems);

    for (i = 0; i < nitems; i++) {
        resolve_axis(items[i].cs, items[i].ce, Lc, &pl[i].col);
        resolve_axis(items[i].rs, items[i].re, Lr, &pl[i].row);
    }

    /* Which axis the algorithm flows along. It is written assuming
     * grid-auto-flow: row, so for `column` we swap rows and columns wholesale
     * rather than duplicating the algorithm. */
    int flowcol = cfg->flow_col;
    #define MAJ(p)  (flowcol ? &(p)->col : &(p)->row)   /* the axis the cursor advances */
    #define MIN_(p) (flowcol ? &(p)->row : &(p)->col)   /* the axis packed within a line */

    /* Bounds of the implicit grid in the minor axis, in LINE numbers. Step 3 of
     * s8.5: start from the explicit grid, extend for every definite minor
     * position, then for the largest minor span among items without one. */
    int minor_lines = flowcol ? Lr->nlines : Lc->nlines;
    int major_lines = flowcol ? Lc->nlines : Lr->nlines;
    int min_lo = 1, min_hi = minor_lines + 1;
    int maj_lo = 1;
    int maxspan = 1;
    for (i = 0; i < nitems; i++) {
        struct axpl *m = MIN_(&pl[i]), *M = MAJ(&pl[i]);
        if (m->definite) { min_lo = imin(min_lo, m->s); min_hi = imax(min_hi, m->e); }
        else maxspan = imax(maxspan, m->span);
        if (M->definite) maj_lo = imin(maj_lo, M->s);
    }
    if (min_hi - min_lo < maxspan) min_hi = min_lo + maxspan;

    o.w = min_hi - min_lo;
    if (o.w < 1) o.w = 1;
    if (o.w > GRID_MAX_TRACKS) o.w = GRID_MAX_TRACKS;

    /* Step 1: position everything that is not auto-positioned. */
    for (i = 0; i < nitems; i++) {
        struct axpl *m = MIN_(&pl[i]), *M = MAJ(&pl[i]);
        if (m->definite && M->definite)
            if (occ_set(&o, M->s - maj_lo, m->s - min_lo, M->span, m->span) < 0) goto done;
    }

    /* Step 2: items locked to a given major line, auto in the minor axis. */
    {
        int *cursor = (int *)calloc((size_t)(GRID_MAX_TRACKS), sizeof *cursor);
        if (!cursor) goto done;
        int k;
        for (k = 0; k < nitems; k++) {
            i = idx[k];
            struct axpl *m = MIN_(&pl[i]), *M = MAJ(&pl[i]);
            if (!M->definite || m->definite) continue;
            int maj = M->s - maj_lo;
            int c = cfg->flow_dense ? 0 : (maj >= 0 && maj < GRID_MAX_TRACKS ? cursor[maj] : 0);
            if (c < 0) c = 0;
            /* This step runs BEFORE the minor-axis extent is fixed (step 3
             * accounts for what it placed), so the search is not bounded by
             * o.w -- it may push the implicit grid wider. */
            while (c + m->span > o.w || !occ_free_at(&o, maj, c, M->span, m->span)) {
                if (c + m->span > o.w) {
                    int nw = c + m->span;
                    if (nw > GRID_MAX_TRACKS) { c = 0; break; }
                    unsigned char *nb = (unsigned char *)calloc((size_t)nw * (o.cap ? o.cap : 1), 1);
                    if (!nb) { free(cursor); goto done; }
                    int r;
                    for (r = 0; r < o.h; r++) memcpy(nb + (size_t)r*nw, o.b + (size_t)r*o.w, (size_t)o.w);
                    free(o.b); o.b = nb; o.w = nw;
                    if (!o.cap) o.cap = 1;
                    continue;
                }
                c++;
            }
            m->definite = 1; m->s = c + min_lo; m->e = m->s + m->span;
            if (maj >= 0 && maj < GRID_MAX_TRACKS && !cfg->flow_dense) cursor[maj] = c + m->span;
            if (occ_set(&o, maj, c, M->span, m->span) < 0) { free(cursor); goto done; }
        }
        free(cursor);
    }

    /* Step 3: the minor axis is now final (step 2 may have widened it). */

    /* Step 4: position the remaining items with the auto-placement cursor. */
    {
        int cmaj = 0, cmin = 0;
        int k;
        for (k = 0; k < nitems; k++) {
            i = idx[k];
            struct axpl *m = MIN_(&pl[i]), *M = MAJ(&pl[i]);
            if (M->definite && m->definite) continue;

            if (m->definite) {
                /* Definite minor position, automatic major position. */
                int c = m->s - min_lo;
                if (cfg->flow_dense) { cmaj = 0; cmin = c; }
                else { if (c < cmin) cmaj++; cmin = c; }
                while (!occ_free_at(&o, cmaj, c, M->span, m->span)) cmaj++;
                if (occ_need(&o, cmaj + M->span) < 0) goto done;
                M->definite = 1; M->s = cmaj + maj_lo; M->e = M->s + M->span;
                if (occ_set(&o, cmaj, c, M->span, m->span) < 0) goto done;
            } else {
                if (cfg->flow_dense) { cmaj = 0; cmin = 0; }
                for (;;) {
                    if (cmin + m->span > o.w) { cmaj++; cmin = 0; continue; }
                    if (occ_free_at(&o, cmaj, cmin, M->span, m->span)) break;
                    cmin++;
                }
                if (occ_need(&o, cmaj + M->span) < 0) goto done;
                M->definite = 1; M->s = cmaj + maj_lo; M->e = M->s + M->span;
                m->definite = 1; m->s = cmin + min_lo; m->e = m->s + m->span;
                if (occ_set(&o, cmaj, cmin, M->span, m->span) < 0) goto done;
                if (!cfg->flow_dense) cmin += m->span;
            }
        }
        if (occ_need(&o, imax(o.h, major_lines - maj_lo + 1)) < 0) goto done;
    }

    /* Emit, converting line numbers to 0-based track indices. */
    {
        int nmin = o.w;
        int nmaj = imax(o.h, major_lines - maj_lo + 1);
        for (i = 0; i < nitems; i++) {
            struct axpl *m = MIN_(&pl[i]), *M = MAJ(&pl[i]);
            int mi = m->s - min_lo, ma = M->s - maj_lo;
            if (flowcol) {
                out[i].col = ma; out[i].colspan = M->span;
                out[i].row = mi; out[i].rowspan = m->span;
            } else {
                out[i].col = mi; out[i].colspan = m->span;
                out[i].row = ma; out[i].rowspan = M->span;
            }
        }
        *pncols = flowcol ? nmaj : nmin;
        *pnrows = flowcol ? nmin : nmaj;
        *pcol0  = flowcol ? (1 - maj_lo) : (1 - min_lo);
        *prow0  = flowcol ? (1 - min_lo) : (1 - maj_lo);
    }
    rc = 0;
done:
    free(o.b); free(pl); free(idx);
    return rc;
    #undef MAJ
    #undef MIN_
}

/* ========================================================================== */
/* Track sizing (css-grid-1 s12.3 - s12.8)                                    */
/* ========================================================================== */

struct track {
    struct gtrackfn f;
    long long base, lim;      /* base size, growth limit (INF64 = infinity) */
    long long fc;             /* resolved fit-content() argument, INF64 if none */
    unsigned char inf_grow;   /* "infinitely growable" (s12.5.3 step 5) */
    unsigned char flexible;   /* max fn is <flex> */
    unsigned char min_int, max_int;   /* min/max fn is an intrinsic sizing fn */
    long long planned, item_inc;
    unsigned char frozen;
};

static long long resolve_fixed(const struct gsize *g, int avail)
{
    if (g->kind == GSF_PX) return g->v;
    if (g->kind == GSF_PCT && avail != GRID_INDEFINITE)
        return (long long)avail * g->v / 10000;
    return -1;                                     /* not a fixed sizing function */
}

/* An `auto` MAX track sizing function is treated as max-content (s11.2); an
 * `auto` MIN track sizing function is not. Kept as two predicates because the
 * asymmetry is the single most common place to get this wrong. */
static int max_is_maxcontent(const struct track *t)
{
    return t->f.mx.kind == GSF_MAX_CONTENT || t->f.mx.kind == GSF_AUTO || t->f.is_fc;
}
static int max_is_mincontent(const struct track *t) { return t->f.mx.kind == GSF_MIN_CONTENT; }

/* The limit a track may not pass while space is distributed "up to limits". */
static long long limit_base(const struct track *t)
{
    long long l = t->lim;
    if (t->f.is_fc && t->fc < l) l = t->fc;
    return l;
}
static long long limit_grow(const struct track *t)
{
    if (t->lim < INF64 && !t->inf_grow) return t->lim;
    if (t->f.is_fc) return t->fc;
    return INF64;
}

/* Distribute `*space` equally among the listed tracks' item-incurred increases,
 * freezing a track as its (affected size + increase) reaches `limit`.
 *
 * REMAINDER: when the share falls below 1px the leftover pixels go one each to
 * the earliest unfrozen tracks in track order. */
static void dist_equal(struct track *t, const int *list, int n, int affect_base,
                       long long *space, int use_limits)
{
    int i;
    if (n <= 0 || *space <= 0) return;
    for (i = 0; i < n; i++) t[list[i]].frozen = 0;

    for (;;) {
        int nf = 0;
        for (i = 0; i < n; i++) {
            struct track *k = &t[list[i]];
            if (k->frozen) continue;
            if (use_limits) {
                long long lim = affect_base ? limit_base(k) : limit_grow(k);
                long long cur = affect_base ? k->base : (k->lim < INF64 ? k->lim : k->base);
                if (lim < INF64 && cur + k->item_inc >= lim) { k->frozen = 1; continue; }
            }
            nf++;
        }
        if (nf == 0 || *space <= 0) return;
        long long share = *space / nf;
        if (share == 0) {
            for (i = 0; i < n && *space > 0; i++) {
                struct track *k = &t[list[i]];
                if (k->frozen) continue;
                k->item_inc += 1;
                *space -= 1;
            }
            return;
        }
        int capped = 0;
        for (i = 0; i < n; i++) {
            struct track *k = &t[list[i]];
            if (k->frozen) continue;
            long long add = share;
            if (use_limits) {
                long long lim = affect_base ? limit_base(k) : limit_grow(k);
                long long cur = affect_base ? k->base : (k->lim < INF64 ? k->lim : k->base);
                if (lim < INF64 && cur + k->item_inc + add > lim) {
                    add = lim - cur - k->item_inc;
                    if (add < 0) add = 0;
                    capped = 1;
                }
            }
            k->item_inc += add;
            *space -= add;
        }
        if (!capped && *space < nf) {
            /* REMAINDER: exact division done; hand out what is left. */
            for (i = 0; i < n && *space > 0; i++) { t[list[i]].item_inc += 1; *space -= 1; }
            return;
        }
    }
}

/* Distribute proportionally to flex factors -- s12.5.4's variant for items that
 * span a flexible track. If the flex factors sum to less than 1, that
 * PROPORTION of the space goes out by ratio and the rest goes out equally. */
static void dist_flex(struct track *t, const int *list, int n, long long *space)
{
    int i;
    if (n <= 0 || *space <= 0) return;
    long long sum = 0;
    for (i = 0; i < n; i++) sum += t[list[i]].f.mx.v;      /* milli-fr */
    if (sum <= 0) { dist_equal(t, list, n, 1, space, 0); return; }

    long long ratio_part = *space, equal_part = 0;
    if (sum < 1000) {
        ratio_part = *space * sum / 1000;
        equal_part = *space - ratio_part;
    }
    long long given = 0;
    for (i = 0; i < n; i++) {
        long long a = ratio_part * t[list[i]].f.mx.v / sum;
        t[list[i]].item_inc += a;
        given += a;
    }
    *space -= given;
    if (equal_part > 0) {
        long long e = equal_part;
        dist_equal(t, list, n, 1, &e, 0);
        *space -= (equal_part - e);
    }
}

struct spanitem { int start, span; long long contrib; };

/* Gutters are fixed empty tracks (s10.1), so a per-GAP array rather than one
 * scalar: `gaps[i]` is the gutter after track i. auto-fit's collapsed tracks
 * make the array non-uniform, and every consumer of a gap goes through one of
 * these two helpers so there is no second place to forget. */
static long long span_gap(const int *gaps, int nt, int start, int span)
{
    long long s = 0; int i;
    for (i = start; i < start + span - 1 && i < nt - 1; i++) if (i >= 0) s += gaps[i];
    return s;
}
static long long all_gap(const int *gaps, int nt) { return span_gap(gaps, nt, 0, nt); }

#ifndef GRID_SPAN_EVEN_SPLIT
/* THE spec procedure (s12.5.1 "Distributing Extra Space Across Spanned
 * Tracks"). Read the four inner steps against the spec before editing; they are
 * not interchangeable and the ORDER is what makes the numbers come out right.
 *
 * beyond: which affected tracks get space past their limits (s12.5.1 step 2.4)
 *   0 = affected tracks with an intrinsic max fn, else all affected
 *   1 = affected tracks with a max-content max fn, else all affected
 *   2 = affected tracks with an intrinsic max fn (growth-limit case, no
 *       fallback to "all") */
static void distribute_extra_space(struct track *t, int nt, const int *gaps,
                                   const unsigned char *affected, int affect_base,
                                   const struct spanitem *it, int nit,
                                   int beyond, int flex_prop)
{
    int i, k, x;
    int *aff = (int *)malloc((size_t)(nt ? nt : 1) * sizeof *aff);
    int *non = (int *)malloc((size_t)(nt ? nt : 1) * sizeof *non);
    int *bey = (int *)malloc((size_t)(nt ? nt : 1) * sizeof *bey);
    if (!aff || !non || !bey) { free(aff); free(non); free(bey); return; }

    for (x = 0; x < nt; x++) t[x].planned = 0;

    for (k = 0; k < nit; k++) {
        int na = 0, nn = 0;
        for (x = 0; x < nt; x++) t[x].item_inc = 0;

        /* 2.1 Find the space to distribute: the contribution minus the affected
         * size of EVERY spanned track (not just the affected ones), with an
         * infinite growth limit standing in as the base size. Gutters are fixed
         * empty tracks (s10.1), so the span-1 gaps the item crosses come off
         * here too. */
        long long sum = span_gap(gaps, nt, it[k].start, it[k].span);
        for (i = it[k].start; i < it[k].start + it[k].span && i < nt; i++) {
            sum += affect_base ? t[i].base : (t[i].lim < INF64 ? t[i].lim : t[i].base);
            if (affected[i]) aff[na++] = i; else non[nn++] = i;
        }
        long long space = it[k].contrib - sum;
        if (space <= 0) continue;

        /* 2.2 Distribute up to limits, among the affected tracks. */
        if (flex_prop) dist_flex(t, aff, na, &space);
        else           dist_equal(t, aff, na, affect_base, &space, 1);

        /* 2.3 Then into the non-affected spanned tracks, so their headroom is
         * used before any affected track's growth limit is violated. Increases
         * to these tracks are computed and then DISCARDED -- only affected
         * tracks take a planned increase in 2.5. */
        if (space > 0 && nn > 0) dist_equal(t, non, nn, affect_base, &space, 1);

        /* 2.4 Beyond limits, to the subset chosen by max sizing function. */
        if (space > 0 && na > 0) {
            int nb = 0;
            for (i = 0; i < na; i++) {
                struct track *tr = &t[aff[i]];
                int ok = (beyond == 1) ? max_is_maxcontent(tr)
                                       : (tr->max_int || (tr->f.is_fc && tr->base + tr->item_inc < tr->fc));
                if (ok) bey[nb++] = aff[i];
            }
            if (nb == 0 && beyond != 2) { for (i = 0; i < na; i++) bey[i] = aff[i]; nb = na; }
            if (flex_prop) dist_flex(t, bey, nb, &space);
            else           dist_equal(t, bey, nb, affect_base, &space, 0);
        }

        /* 2.5 planned = max(planned, item-incurred). Taking the MAX rather than
         * the sum is what stops the result depending on item order. */
        for (i = 0; i < na; i++)
            if (t[aff[i]].item_inc > t[aff[i]].planned) t[aff[i]].planned = t[aff[i]].item_inc;
    }

    /* 3 Apply. */
    for (x = 0; x < nt; x++) {
        if (!affected[x] || t[x].planned <= 0) continue;
        if (affect_base) t[x].base += t[x].planned;
        else {
            if (t[x].lim >= INF64) { t[x].lim = t[x].base + t[x].planned; t[x].inf_grow = 1; }
            else                     t[x].lim += t[x].planned;
        }
    }
    free(aff); free(non); free(bey);
}
#else
/* NEGATIVE CONTROL (-DGRID_SPAN_EVEN_SPLIT).
 *
 * The plausible wrong implementation: split a spanning item's leftover
 * contribution EVENLY across the tracks it spans, ignoring which tracks are
 * affected, ignoring growth limits, and ignoring the beyond-limits priority
 * rules. Totals still come out right and every number still looks like a
 * reasonable track size -- which is the point. Only the per-track split is
 * wrong, and the suite must notice. */
static void distribute_extra_space(struct track *t, int nt, const int *gaps,
                                   const unsigned char *affected, int affect_base,
                                   const struct spanitem *it, int nit,
                                   int beyond, int flex_prop)
{
    int i, k, x;
    (void)beyond; (void)flex_prop;
    for (x = 0; x < nt; x++) t[x].planned = 0;
    for (k = 0; k < nit; k++) {
        long long sum = span_gap(gaps, nt, it[k].start, it[k].span);
        int n = 0;
        for (i = it[k].start; i < it[k].start + it[k].span && i < nt; i++) {
            sum += affect_base ? t[i].base : (t[i].lim < INF64 ? t[i].lim : t[i].base);
            n++;
        }
        long long space = it[k].contrib - sum;
        if (space <= 0 || n == 0) continue;
        long long each = space / n;
        for (i = it[k].start; i < it[k].start + it[k].span && i < nt; i++)
            if (each > t[i].planned) t[i].planned = each;
    }
    for (x = 0; x < nt; x++) {
        if (!affected[x] || t[x].planned <= 0) continue;
        if (affect_base) t[x].base += t[x].planned;
        else if (t[x].lim >= INF64) { t[x].lim = t[x].base + t[x].planned; t[x].inf_grow = 1; }
        else t[x].lim += t[x].planned;
    }
}
#endif

/* s12.7.1 Find the Size of an fr. Returns the flex fraction as the rational
 * num/den (px per 1fr), so nothing is rounded until a track size is computed. */
static void find_fr(struct track *t, const int *list, int n,
                    long long fill, long long *num, long long *den)
{
    unsigned char *inflex = (unsigned char *)calloc((size_t)(n ? n : 1), 1);
    if (!inflex) { *num = 0; *den = 1; return; }
    for (;;) {
        long long leftover = fill, flexsum = 0;
        int i, nflex = 0;
        for (i = 0; i < n; i++) {
            struct track *k = &t[list[i]];
            if (k->flexible && !inflex[i]) { flexsum += k->f.mx.v; nflex++; }
            else leftover -= k->base;
        }
        if (nflex == 0) { *num = 0; *den = 1; break; }
        if (leftover < 0) leftover = 0;
        if (flexsum < 1000) flexsum = 1000;          /* "If this value is less than 1, set it to 1" */
        int restart = 0;
        for (i = 0; i < n; i++) {
            struct track *k = &t[list[i]];
            if (!k->flexible || inflex[i]) continue;
            if ((long long)k->f.mx.v * leftover / flexsum < k->base) { inflex[i] = 1; restart = 1; }
        }
        if (restart) continue;
        *num = leftover; *den = flexsum;             /* fr size = num/den px per milli-fr */
        break;
    }
    free(inflex);
}

/* The track sizing algorithm for one axis. `avail` GRID_INDEFINITE means the
 * container is being sized under a max-content constraint. */
static int size_tracks_impl(struct track *t, int nt,
                            const struct gtrackitem *items, int nit,
                            int avail, const int *gaps, unsigned char content_align)
{
    int i, k, x;
    int rc = -1;
    unsigned char *aff = (unsigned char *)malloc((size_t)(nt ? nt : 1));
    struct spanitem *si = (struct spanitem *)malloc((size_t)(nit ? nit : 1) * sizeof *si);
    int *all = (int *)malloc((size_t)(nt ? nt : 1) * sizeof *all);
    if (!aff || !si || !all) goto done;
    for (x = 0; x < nt; x++) all[x] = x;

    int intrinsic_constraint = (avail == GRID_INDEFINITE);

    /* ---- s12.4 Initialize Track Sizes ---------------------------------- */
    for (x = 0; x < nt; x++) {
        struct track *k = &t[x];
        long long f = resolve_fixed(&k->f.mn, avail);
        k->base = (f >= 0) ? f : 0;
        f = resolve_fixed(&k->f.mx, avail);
        k->lim = (f >= 0) ? f : INF64;
        if (k->lim < k->base) k->lim = k->base;
        k->fc = INF64;
        if (k->f.is_fc) {
            long long a = resolve_fixed(&k->f.fc, avail);
            k->fc = (a >= 0) ? a : INF64;
        }
        k->flexible = (k->f.mx.kind == GSF_FR);
        /* A percentage against an indefinite available space behaves as auto
         * (s7.2.1), which makes both functions intrinsic. */
        k->min_int = !(resolve_fixed(&k->f.mn, avail) >= 0);
        k->max_int = !(resolve_fixed(&k->f.mx, avail) >= 0) && !k->flexible;
        k->inf_grow = 0;
    }

    /* ---- s12.5 Resolve Intrinsic Track Sizes ---------------------------- */

    /* Step 2: tracks with an intrinsic (and not flexible) sizing function,
     * items with a span of 1. */
    for (x = 0; x < nt; x++) {
        struct track *k = &t[x];
        if (k->flexible) continue;
        if (!k->min_int && !k->max_int) continue;
        long long b_minc = -1, b_maxc = -1, b_min = -1;
        for (i = 0; i < nit; i++) {
            if (items[i].span != 1 || items[i].start != x) continue;
            if (items[i].m.min_content > b_minc) b_minc = items[i].m.min_content;
            if (items[i].m.max_content > b_maxc) b_maxc = items[i].m.max_content;
            if (items[i].m.minimum     > b_min)  b_min  = items[i].m.minimum;
        }
        if (b_minc < 0) continue;                    /* no span-1 items in this track */

        if (k->f.mn.kind == GSF_MIN_CONTENT)      k->base = llmax(0, b_minc);
        else if (k->f.mn.kind == GSF_MAX_CONTENT) k->base = llmax(0, b_maxc);
        else if (k->min_int) {
            /* auto min fn */
            if (intrinsic_constraint) {
                long long lim = resolve_fixed(&k->f.mx, avail);
                if (k->f.is_fc && k->fc < INF64) lim = k->fc;
                long long c = b_minc;
                if (lim >= 0 && c > lim) c = lim;
                if (c < b_min) c = b_min;
                k->base = llmax(0, c);
            } else {
                k->base = llmax(0, b_min);
            }
        }
        if (max_is_mincontent(k))      k->lim = b_minc;
        else if (max_is_maxcontent(k)) {
            k->lim = b_maxc;
            if (k->f.is_fc && k->lim > k->fc) k->lim = k->fc;
        }
        if (k->lim < k->base) k->lim = k->base;
    }

    /* Step 3: items with a span of 2, then 3, ... that do NOT span a flexible
     * track. Then step 4: every item that DOES, all together. */
    {
        int maxspan = 1;
        for (i = 0; i < nit; i++) if (items[i].span > maxspan) maxspan = items[i].span;

        int pass;
        for (pass = 2; pass <= maxspan + 1; pass++) {
            int flexpass = (pass == maxspan + 1);
            int span = pass;

            /* Collect this pass's items. */
            int n = 0;
            for (i = 0; i < nit; i++) {
                int spans_flex = 0;
                for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                    if (t[k].flexible) spans_flex = 1;
                if (flexpass) { if (!spans_flex) continue; }
                else { if (spans_flex || items[i].span != span) continue; }
                si[n].start = items[i].start; si[n].span = items[i].span;
                si[n].contrib = 0;
                n++;
            }
            if (n == 0) continue;

            /* 3.1 For intrinsic minimums -> base sizes of tracks with an
             * intrinsic MIN track sizing function. */
            n = 0;
            for (i = 0; i < nit; i++) {
                int spans_flex = 0;
                for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                    if (t[k].flexible) spans_flex = 1;
                if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                si[n].start = items[i].start; si[n].span = items[i].span;
                si[n].contrib = items[i].m.minimum;
                if (intrinsic_constraint) {
                    /* limited min-content contribution */
                    long long lim = 0; int allfixed = 1;
                    for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++) {
                        long long f = resolve_fixed(&t[k].f.mx, avail);
                        if (t[k].f.is_fc && t[k].fc < INF64) f = t[k].fc;
                        if (f < 0) { allfixed = 0; break; }
                        lim += f;
                    }
                    long long c = items[i].m.min_content;
                    if (allfixed && c > lim) c = lim;
                    if (c < items[i].m.minimum) c = items[i].m.minimum;
                    si[n].contrib = c;
                }
                n++;
            }
            for (x = 0; x < nt; x++) aff[x] = (unsigned char)(t[x].min_int && (!flexpass || t[x].flexible));
            distribute_extra_space(t, nt, gaps, aff, 1, si, n, 0, flexpass);

            /* 3.2 For content-based minimums -> min fn of min-content or
             * max-content, accommodating min-content contributions. */
            {
                int j = 0;
                for (i = 0; i < nit; i++) {
                    int spans_flex = 0;
                    for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                        if (t[k].flexible) spans_flex = 1;
                    if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                    si[j].start = items[i].start; si[j].span = items[i].span;
                    si[j].contrib = items[i].m.min_content;
                    j++;
                }
                for (x = 0; x < nt; x++)
                    aff[x] = (unsigned char)((t[x].f.mn.kind == GSF_MIN_CONTENT ||
                                              t[x].f.mn.kind == GSF_MAX_CONTENT) &&
                                             (!flexpass || t[x].flexible));
                distribute_extra_space(t, nt, gaps, aff, 1, si, j, 0, flexpass);

                /* 3.3 For max-content minimums. Under a max-content constraint
                 * this also feeds `auto` min fns their limited max-content
                 * contributions; in all cases it feeds max-content min fns. */
                if (intrinsic_constraint) {
                    int m = 0;
                    for (i = 0; i < nit; i++) {
                        int spans_flex = 0;
                        for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                            if (t[k].flexible) spans_flex = 1;
                        if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                        long long lim = 0; int allfixed = 1;
                        for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++) {
                            long long f = resolve_fixed(&t[k].f.mx, avail);
                            if (t[k].f.is_fc && t[k].fc < INF64) f = t[k].fc;
                            if (f < 0) { allfixed = 0; break; }
                            lim += f;
                        }
                        long long c = items[i].m.max_content;
                        if (allfixed && c > lim) c = lim;
                        if (c < items[i].m.minimum) c = items[i].m.minimum;
                        si[m].start = items[i].start; si[m].span = items[i].span;
                        si[m].contrib = c;
                        m++;
                    }
                    for (x = 0; x < nt; x++)
                        aff[x] = (unsigned char)((t[x].f.mn.kind == GSF_AUTO ||
                                                  t[x].f.mn.kind == GSF_MAX_CONTENT) &&
                                                 (!flexpass || t[x].flexible));
                    distribute_extra_space(t, nt, gaps, aff, 1, si, m, 1, flexpass);
                }
                {
                    int m = 0;
                    for (i = 0; i < nit; i++) {
                        int spans_flex = 0;
                        for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                            if (t[k].flexible) spans_flex = 1;
                        if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                        si[m].start = items[i].start; si[m].span = items[i].span;
                        si[m].contrib = items[i].m.max_content;
                        m++;
                    }
                    for (x = 0; x < nt; x++)
                        aff[x] = (unsigned char)(t[x].f.mn.kind == GSF_MAX_CONTENT &&
                                                 (!flexpass || t[x].flexible));
                    distribute_extra_space(t, nt, gaps, aff, 1, si, m, 1, flexpass);
                }

                /* 3.4 Raise any growth limit now below its base size. */
                for (x = 0; x < nt; x++) if (t[x].lim < t[x].base) t[x].lim = t[x].base;

                /* 3.5 For intrinsic maximums -> growth limits, accommodating
                 * min-content contributions. Tracks whose growth limit went
                 * from infinite to finite here become "infinitely growable" for
                 * the next step -- see the Salas note in s12.5.1. */
                {
                    int m = 0;
                    for (i = 0; i < nit; i++) {
                        int spans_flex = 0;
                        for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                            if (t[k].flexible) spans_flex = 1;
                        if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                        si[m].start = items[i].start; si[m].span = items[i].span;
                        si[m].contrib = items[i].m.min_content;
                        m++;
                    }
                    for (x = 0; x < nt; x++) {
                        aff[x] = (unsigned char)(t[x].max_int && (!flexpass || t[x].flexible));
                        t[x].inf_grow = 0;
                    }
                    distribute_extra_space(t, nt, gaps, aff, 0, si, m, 2, flexpass);
                }

                /* 3.6 For max-content maximums -> growth limits. */
                {
                    int m = 0;
                    for (i = 0; i < nit; i++) {
                        int spans_flex = 0;
                        for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                            if (t[k].flexible) spans_flex = 1;
                        if (flexpass ? !spans_flex : (spans_flex || items[i].span != span)) continue;
                        si[m].start = items[i].start; si[m].span = items[i].span;
                        si[m].contrib = items[i].m.max_content;
                        m++;
                    }
                    for (x = 0; x < nt; x++)
                        aff[x] = (unsigned char)(max_is_maxcontent(&t[x]) && t[x].max_int &&
                                                 (!flexpass || t[x].flexible));
                    distribute_extra_space(t, nt, gaps, aff, 0, si, m, 2, flexpass);
                    for (x = 0; x < nt; x++) t[x].inf_grow = 0;
                }
            }
        }
    }

    /* Step 5: an infinite growth limit becomes the base size. */
    for (x = 0; x < nt; x++) if (t[x].lim >= INF64) t[x].lim = t[x].base;

    /* ---- s12.6 Maximize Tracks ------------------------------------------ */
    {
        long long total = all_gap(gaps, nt);
        for (x = 0; x < nt; x++) total += t[x].base;
        if (intrinsic_constraint) {
            /* Free space is infinite under a max-content constraint: every
             * track grows to its growth limit. */
            for (x = 0; x < nt; x++) if (t[x].lim > t[x].base) t[x].base = t[x].lim;
        } else {
            long long freesp = (long long)avail - total;
            if (freesp > 0) {
                for (x = 0; x < nt; x++) { t[x].item_inc = 0; t[x].frozen = 0; }
                dist_equal(t, all, nt, 1, &freesp, 1);
                for (x = 0; x < nt; x++) t[x].base += t[x].item_inc;
            }
        }
    }

    /* ---- s12.7 Expand Flexible Tracks ------------------------------------ */
    {
        int anyflex = 0;
        for (x = 0; x < nt; x++) if (t[x].flexible) anyflex = 1;
        if (anyflex) {
            long long num = 0, den = 1;
            long long total = all_gap(gaps, nt);
            for (x = 0; x < nt; x++) total += t[x].base;
            long long freesp = intrinsic_constraint ? -1 : (long long)avail - total;

            if (!intrinsic_constraint && freesp == 0) {
                num = 0; den = 1;
            } else if (!intrinsic_constraint) {
                find_fr(t, all, nt, (long long)avail - all_gap(gaps, nt), &num, &den);
            } else {
                /* Indefinite free space: the maximum of each flexible track's
                 * own hypothetical 1fr size and, per item crossing a flexible
                 * track, find-the-size-of-an-fr over the tracks it crosses. */
                for (x = 0; x < nt; x++) {
                    if (!t[x].flexible) continue;
                    long long n2, d2;
                    if (t[x].f.mx.v > 1000) { n2 = t[x].base * 1000; d2 = t[x].f.mx.v; }
                    else                    { n2 = t[x].base;        d2 = 1000; }
                    if (n2 * den > num * d2) { num = n2; den = d2; }
                }
                for (i = 0; i < nit; i++) {
                    int spans_flex = 0;
                    for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                        if (t[k].flexible) spans_flex = 1;
                    if (!spans_flex) continue;
                    int lst[GRID_MAX_TRACKS > 256 ? 256 : GRID_MAX_TRACKS], ln = 0;
                    for (k = items[i].start; k < items[i].start + items[i].span && k < nt; k++)
                        if (ln < (int)(sizeof lst / sizeof lst[0])) lst[ln++] = k;
                    long long n2, d2;
                    find_fr(t, lst, ln,
                            (long long)items[i].m.max_content
                              - span_gap(gaps, nt, items[i].start, items[i].span),
                            &n2, &d2);
                    if (n2 * den > num * d2) { num = n2; den = d2; }
                }
            }

            /* size = flex factor * fr size, and only if it beats the base. */
            long long sumf = 0, got = 0;
            for (x = 0; x < nt; x++) {
                if (!t[x].flexible) continue;
                long long sz = (long long)t[x].f.mx.v * num / den;
                t[x].frozen = 0;
                if (sz > t[x].base) { t[x].base = sz; t[x].frozen = 1; }
                if (t[x].frozen) { sumf += t[x].f.mx.v; got += t[x].base; }
            }
            /* REMAINDER: each size was truncated, so the tracks that took an fr
             * size can sum to a few px less than what their factors were
             * entitled to. Hand exactly that shortfall out, one px each, in
             * track order.
             *
             * The shortfall is measured against the FACTORS' entitlement
             * (sumf * fr), never against the container: when the flex factors
             * sum to less than 1 the tracks are supposed to leave space unused
             * (s7.2.4), and topping up to `avail` would silently destroy that
             * -- which is the whole point of the sub-1fr case. */
            {
                long long deficit = sumf * num / den - got;
                for (x = 0; x < nt && deficit > 0; x++)
                    if (t[x].flexible && t[x].frozen) { t[x].base++; deficit--; }
            }
        }
    }

    /* ---- s12.8 Stretch auto Tracks --------------------------------------- */
    if ((content_align == GA_NORMAL || content_align == GA_STRETCH) && !intrinsic_constraint) {
        int n = 0;
        for (x = 0; x < nt; x++) if (t[x].f.mx.kind == GSF_AUTO) all[n++] = x;
        if (n > 0) {
            long long total = all_gap(gaps, nt);
            for (x = 0; x < nt; x++) total += t[x].base;
            long long freesp = (long long)avail - total;
            if (freesp > 0) {
                long long each = freesp / n, rem = freesp % n;
                for (i = 0; i < n; i++) t[all[i]].base += each + (i < rem ? 1 : 0);   /* REMAINDER */
            }
        }
        for (x = 0; x < nt; x++) all[x] = x;
    }

    rc = 0;
done:
    free(aff); free(si); free(all);
    return rc;
}

int grid_size_tracks(const struct gtracklist *tracks, int ntracks,
                     const struct gtrackitem *items, int nitems,
                     int avail, int gap, unsigned char content_align,
                     int *out_size)
{
    int i;
    if (ntracks <= 0) return 0;
    struct track *t = (struct track *)calloc((size_t)ntracks, sizeof *t);
    int *gaps = (int *)calloc((size_t)(ntracks ? ntracks : 1), sizeof *gaps);
    if (!t || !gaps) { free(t); free(gaps); return -1; }
    for (i = 0; i < ntracks; i++) gaps[i] = gap;
    for (i = 0; i < ntracks; i++)
        t[i].f = (i < tracks->n) ? tracks->tr[i] : (struct gtrackfn){ {GSF_AUTO,0}, {GSF_AUTO,0}, {GSF_PX,0}, 0 };
    int rc = size_tracks_impl(t, ntracks, items, nitems, avail, gaps, content_align);
    if (rc == 0) for (i = 0; i < ntracks; i++) out_size[i] = (int)t[i].base;
    free(t); free(gaps);
    return rc;
}

/* ========================================================================== */
/* Alignment (css-grid-1 s10, css-align-3 s4-s6)                              */
/* ========================================================================== */

/* Distribute content alignment: fills pos[] with each track's start edge.
 * `free` may be negative (the grid overflows), in which case the distribution
 * values fall back per css-align-3 s4: space-between -> start, space-around and
 * space-evenly -> safe center, which is start when overflowing. */
static void align_content(int n, const int *size, const int *gaps, long long freesp,
                          unsigned char how, int *pos)
{
    long long off = 0, extra = 0, i;
    if (n <= 0) return;
    if (freesp < 0) freesp = 0;

    switch (how) {
    case GA_END: case GA_FLEX_END: case GA_RIGHT:
        off = freesp; break;
    case GA_CENTER:
        off = freesp / 2; break;
    case GA_SPACE_BETWEEN:
        if (n > 1) extra = freesp / (n - 1);
        else off = 0;
        break;
    case GA_SPACE_AROUND:
        extra = freesp / n; off = extra / 2; break;
    case GA_SPACE_EVENLY:
        extra = freesp / (n + 1); off = extra; break;
    default:                  /* normal, stretch, start, flex-start, self-start, left */
        off = 0; break;
    }
    /* REMAINDER: with space-* the leftover pixels go to the earliest gaps, so
     * the tracks still end exactly on the container's end edge. */
    long long rem = 0;
    if (how == GA_SPACE_BETWEEN && n > 1)   rem = freesp - extra * (n - 1);
    else if (how == GA_SPACE_AROUND)        rem = freesp - extra * n;
    else if (how == GA_SPACE_EVENLY)        rem = freesp - extra * (n + 1);

    long long x = off;
    for (i = 0; i < n; i++) {
        pos[i] = (int)x;
        x += size[i] + (i < n - 1 ? gaps[i] : 0) + extra;
        if (rem > 0 && i < n - 1) { x += 1; rem--; }
    }
}

/* Resolve a self-alignment keyword to a physical direction in one axis.
 * Returns 0 = start, 1 = center, 2 = end, 3 = stretch, 4 = baseline. */
static int self_dir(unsigned char a, int inline_axis, int rtl)
{
    switch (a) {
    case GA_CENTER:                          return 1;
    case GA_END: case GA_FLEX_END: case GA_SELF_END:
        return (inline_axis && rtl) ? 0 : 2;
    case GA_START: case GA_FLEX_START: case GA_SELF_START:
        return (inline_axis && rtl) ? 2 : 0;
    case GA_LEFT:                            return rtl && inline_axis ? 2 : 0;
    case GA_RIGHT:                           return rtl && inline_axis ? 0 : 2;
    case GA_BASELINE: case GA_LAST_BASELINE: return 4;
    case GA_STRETCH: case GA_NORMAL: case GA_AUTO: default: return 3;
    }
}

/* ========================================================================== */
/* grid_layout                                                                */
/* ========================================================================== */

void grid_out_free(struct gridout *out)
{
    if (!out) return;
    free(out->colsz); free(out->rowsz);
    free(out->colpos); free(out->rowpos);
    free(out->items);
    memset(out, 0, sizeof *out);
}

/* The sizing function for an implicit track. s7.5: the first implicit track
 * after the explicit grid gets grid-auto-*'s first size and so on forwards; the
 * last implicit track before it gets the last size and so on backwards. */
static struct gtrackfn implicit_fn(const struct gtracklist *ac, int before, int dist)
{
    struct gtrackfn f;
    memset(&f, 0, sizeof f);
    f.mn.kind = GSF_AUTO; f.mx.kind = GSF_AUTO;
    if (!ac || ac->n == 0) return f;
    int i = before ? ((ac->n - (dist % ac->n)) % ac->n) : (dist % ac->n);
    return ac->tr[i];
}

int grid_place(const struct gridcfg *cfg, const struct griditem *items, int nitems,
               struct gridpos *out, int *ncols, int *nrows, int *col0, int *row0)
{
    struct gtracklist ec, er;
    struct lnames Lc, Lr;
    int rc = -1;
    memset(&Lc, 0, sizeof Lc); memset(&Lr, 0, sizeof Lr);
    if (grid_template_expand(&cfg->cols, cfg->avail_w, cfg->gap_x, &ec, NULL) < 0) return -1;
    if (grid_template_expand(&cfg->rows, cfg->avail_h, cfg->gap_y, &er, NULL) < 0) {
        grid_tracklist_free(&ec); return -1;
    }
    int nec = ec.n, ner = er.n;
    if (cfg->areas) {
        if (cfg->areas->cols > nec) nec = cfg->areas->cols;
        if (cfg->areas->rows > ner) ner = cfg->areas->rows;
    }
    if (build_names(&ec, cfg->areas, 1, nec + 1, &Lc) < 0) goto done;
    if (build_names(&er, cfg->areas, 0, ner + 1, &Lr) < 0) goto done;
    rc = place_items(cfg, items, nitems, &Lc, &Lr, out, ncols, nrows, col0, row0);
done:
    free(Lc.nm); free(Lr.nm);
    grid_tracklist_free(&ec); grid_tracklist_free(&er);
    return rc;
}

/* Build the implicit-grid track array for one axis from the explicit template
 * plus grid-auto-*. */
static int build_axis(const struct gtracklist *ex, const struct gtracklist *autol,
                      int ntracks, int explicit_start, struct track **out)
{
    struct track *t = (struct track *)calloc((size_t)(ntracks ? ntracks : 1), sizeof *t);
    if (!t) return -1;
    int i;
    for (i = 0; i < ntracks; i++) {
        int e = i - explicit_start;                 /* index within the explicit grid */
        if (e >= 0 && e < ex->n) t[i].f = ex->tr[e];
        else if (e < 0)         t[i].f = implicit_fn(autol, 1, -e);
        else                    t[i].f = implicit_fn(autol, 0, e - ex->n);
    }
    *out = t;
    return 0;
}

int grid_layout(const struct gridcfg *cfg, const struct griditem *items, int nitems,
                grid_measure_fn measure, void *ctx, struct gridout *out)
{
    int i, x, rc = -1;
    struct gtracklist ec, er;
    struct lnames Lc, Lr;
    struct track *tc = NULL, *tr = NULL;
    struct gtrackitem *ti = NULL;
    struct gmeas *mc = NULL, *mr = NULL;
    int *gapc = NULL, *gapr = NULL;
    int nrepc = 0, nrepr = 0;

    memset(out, 0, sizeof *out);
    memset(&ec, 0, sizeof ec); memset(&er, 0, sizeof er);
    memset(&Lc, 0, sizeof Lc); memset(&Lr, 0, sizeof Lr);

    if (grid_template_expand(&cfg->cols, cfg->avail_w, cfg->gap_x, &ec, &nrepc) < 0) return -1;
    if (grid_template_expand(&cfg->rows, cfg->avail_h, cfg->gap_y, &er, &nrepr) < 0) goto done;

    int nec = ec.n, ner = er.n;
    if (cfg->areas) {
        if (cfg->areas->cols > nec) nec = cfg->areas->cols;
        if (cfg->areas->rows > ner) ner = cfg->areas->rows;
    }
    if (build_names(&ec, cfg->areas, 1, nec + 1, &Lc) < 0) goto done;
    if (build_names(&er, cfg->areas, 0, ner + 1, &Lr) < 0) goto done;

    out->items = (struct gridpos *)calloc((size_t)(nitems ? nitems : 1), sizeof *out->items);
    if (!out->items) goto done;
    out->nitems = nitems;

    int ncols = 0, nrows = 0, col0 = 0, row0 = 0;
    if (place_items(cfg, items, nitems, &Lc, &Lr, out->items, &ncols, &nrows, &col0, &row0) < 0)
        goto done;
    if (ncols < 1) ncols = 1;
    if (nrows < 1) nrows = 1;

    /* Explicit tracks that placement did not need still exist. */
    if (ncols < col0 + nec) ncols = col0 + nec;
    if (nrows < row0 + ner) nrows = row0 + ner;

    if (build_axis(&ec, &cfg->auto_cols, ncols, col0, &tc) < 0) goto done;
    if (build_axis(&er, &cfg->auto_rows, nrows, row0, &tr) < 0) goto done;

    gapc = (int *)calloc((size_t)ncols, sizeof *gapc);
    gapr = (int *)calloc((size_t)nrows, sizeof *gapr);
    if (!gapc || !gapr) goto done;
    for (x = 0; x < ncols; x++) gapc[x] = cfg->gap_x;
    for (x = 0; x < nrows; x++) gapr[x] = cfg->gap_y;

    /* s7.2.3.1 auto-fit: after placement, any EMPTY track in the auto-repeat
     * range collapses -- it is treated as a fixed 0px track and the gutters on
     * either side of it collapse into one. "Empty" means no in-flow item is
     * placed into it or spans across it, which is why this can only be decided
     * here and not during grid_template_expand.
     *
     * One gutter is removed per collapsed track (the one after it, or the one
     * before it when it is the last track), so a run of n collapsed tracks
     * leaves exactly one gutter behind, and a collapsed track at either edge
     * leaves none. */
    {
        int ax;
        for (ax = 0; ax < 2; ax++) {
            const struct gtemplate *tpl = ax ? &cfg->rows : &cfg->cols;
            int nrep = ax ? nrepr : nrepc;
            if (tpl->auto_repeat != GREP_AUTO_FIT || nrep <= 0 || tpl->rep.n <= 0) continue;
            int base = (ax ? row0 : col0) + tpl->pre.n;
            int end  = base + nrep * tpl->rep.n;
            int nt   = ax ? nrows : ncols;
            int *gp  = ax ? gapr : gapc;
            struct track *tt = ax ? tr : tc;
            for (x = base; x < end && x < nt; x++) {
                int used = 0;
                for (i = 0; i < nitems && !used; i++) {
                    int st = ax ? out->items[i].row : out->items[i].col;
                    int sp = ax ? out->items[i].rowspan : out->items[i].colspan;
                    if (x >= st && x < st + sp) used = 1;
                }
                if (used) continue;
                memset(&tt[x].f, 0, sizeof tt[x].f);
                tt[x].f.mn.kind = GSF_PX; tt[x].f.mn.v = 0;
                tt[x].f.mx.kind = GSF_PX; tt[x].f.mx.v = 0;
                if (x < nt - 1)      gp[x] = 0;
                else if (x > 0)      gp[x-1] = 0;
            }
        }
    }

    ti = (struct gtrackitem *)calloc((size_t)(nitems ? nitems : 1), sizeof *ti);
    mc = (struct gmeas *)calloc((size_t)(nitems ? nitems : 1), sizeof *mc);
    mr = (struct gmeas *)calloc((size_t)(nitems ? nitems : 1), sizeof *mr);
    if (!ti || !mc || !mr) goto done;

    /* ---- s11.1 step 1: size the COLUMNS ---------------------------------- */
    for (i = 0; i < nitems; i++) {
        struct gmeas m; memset(&m, 0, sizeof m);
        if (measure) measure(ctx, i, GAX_COL, GRID_INDEFINITE, &m);
        if (items[i].def_w != GRID_INDEFINITE) {
            int w = items[i].def_w;
            if (w > m.max_content) m.max_content = w;
            if (w > m.min_content) m.min_content = w;
            if (w > m.minimum)     m.minimum     = w;
        }
        if (m.min_content > m.max_content) m.max_content = m.min_content;
        if (m.minimum > m.min_content)     m.minimum     = m.min_content;
        mc[i] = m;
        ti[i].start = out->items[i].col;
        ti[i].span  = out->items[i].colspan;
        ti[i].m     = m;
    }
    if (size_tracks_impl(tc, ncols, ti, nitems, cfg->avail_w, gapc,
                         cfg->justify_content) < 0) goto done;

    out->colsz  = (int *)calloc((size_t)ncols, sizeof(int));
    out->colpos = (int *)calloc((size_t)ncols, sizeof(int));
    out->rowsz  = (int *)calloc((size_t)nrows, sizeof(int));
    out->rowpos = (int *)calloc((size_t)nrows, sizeof(int));
    if (!out->colsz || !out->colpos || !out->rowsz || !out->rowpos) goto done;
    for (x = 0; x < ncols; x++) out->colsz[x] = (int)tc[x].base;

    /* ---- s11.1 step 2: size the ROWS, at the inline sizes just found ------ */
    for (i = 0; i < nitems; i++) {
        int aw = 0, c;
        for (c = out->items[i].col; c < out->items[i].col + out->items[i].colspan && c < ncols; c++)
            aw += out->colsz[c];
        aw += (int)span_gap(gapc, ncols, out->items[i].col, out->items[i].colspan);
        int inner = aw - items[i].margin[1] - items[i].margin[3];
        if (inner < 0) inner = 0;
        struct gmeas m; memset(&m, 0, sizeof m);
        if (measure) measure(ctx, i, GAX_ROW, inner, &m);
        if (items[i].def_h != GRID_INDEFINITE) {
            int h = items[i].def_h;
            m.max_content = m.min_content = m.minimum = h;
        }
        if (m.min_content > m.max_content) m.max_content = m.min_content;
        if (m.minimum > m.min_content)     m.minimum     = m.min_content;
        mr[i] = m;
        ti[i].start = out->items[i].row;
        ti[i].span  = out->items[i].rowspan;
        ti[i].m     = m;
    }

    /* Baseline shims (s12.5 step 1), for the simple and by far commonest case:
     * items that occupy a single row and align to the first baseline. The shim
     * is extra margin that makes the item's block-axis contribution reflect
     * where its baseline has to land. Items spanning several rows do not get
     * one -- see the limitations note at the bottom of this file. */
    for (x = 0; x < nrows; x++) {
        int maxb = -1;
        for (i = 0; i < nitems; i++) {
            if (out->items[i].row != x || out->items[i].rowspan != 1) continue;
            unsigned char a = items[i].align_self != GA_AUTO ? items[i].align_self : cfg->align_items;
            if (a != GA_BASELINE || items[i].baseline < 0) continue;
            int b = items[i].baseline + items[i].margin[0];
            if (b > maxb) maxb = b;
        }
        if (maxb < 0) continue;
        for (i = 0; i < nitems; i++) {
            if (out->items[i].row != x || out->items[i].rowspan != 1) continue;
            unsigned char a = items[i].align_self != GA_AUTO ? items[i].align_self : cfg->align_items;
            if (a != GA_BASELINE || items[i].baseline < 0) continue;
            int shim = maxb - (items[i].baseline + items[i].margin[0]);
            if (shim <= 0) continue;
            ti[i].m.minimum     += shim;
            ti[i].m.min_content += shim;
            ti[i].m.max_content += shim;
        }
    }

    if (size_tracks_impl(tr, nrows, ti, nitems, cfg->avail_h, gapr,
                         cfg->align_content) < 0) goto done;
    for (x = 0; x < nrows; x++) out->rowsz[x] = (int)tr[x].base;

    /* ---- s11.1 step 5: align the tracks in the container ------------------ */
    {
        long long gw = all_gap(gapc, ncols), gh = all_gap(gapr, nrows);
        for (x = 0; x < ncols; x++) gw += out->colsz[x];
        for (x = 0; x < nrows; x++) gh += out->rowsz[x];
        long long fw = (cfg->avail_w == GRID_INDEFINITE) ? 0 : (long long)cfg->avail_w - gw;
        long long fh = (cfg->avail_h == GRID_INDEFINITE) ? 0 : (long long)cfg->avail_h - gh;
        unsigned char jc = cfg->justify_content, ac = cfg->align_content;
        if (cfg->rtl) {
            if (jc == GA_START || jc == GA_FLEX_START || jc == GA_LEFT) jc = GA_END;
            else if (jc == GA_END || jc == GA_FLEX_END || jc == GA_RIGHT) jc = GA_START;
        }
        align_content(ncols, out->colsz, gapc, fw, jc, out->colpos);
        align_content(nrows, out->rowsz, gapr, fh, ac, out->rowpos);
        out->width  = (int)(fw > 0 ? gw + (jc==GA_SPACE_BETWEEN||jc==GA_SPACE_AROUND||jc==GA_SPACE_EVENLY ? fw : 0) : gw);
        out->height = (int)(fh > 0 ? gh + (ac==GA_SPACE_BETWEEN||ac==GA_SPACE_AROUND||ac==GA_SPACE_EVENLY ? fh : 0) : gh);
    }

    /* ---- position each item inside its grid area -------------------------- */
    for (i = 0; i < nitems; i++) {
        struct gridpos *p = &out->items[i];
        int c0 = p->col, c1 = imin(p->col + p->colspan, ncols) - 1;
        int r0 = p->row, r1 = imin(p->row + p->rowspan, nrows) - 1;
        if (c1 < c0) c1 = c0;
        if (r1 < r0) r1 = r0;
        p->area_x = out->colpos[c0];
        p->area_w = out->colpos[c1] + out->colsz[c1] - out->colpos[c0];
        p->area_y = out->rowpos[r0];
        p->area_h = out->rowpos[r1] + out->rowsz[r1] - out->rowpos[r0];

        int ax, aw, ay, ah;
        ax = p->area_x + items[i].margin[3];
        aw = p->area_w - items[i].margin[1] - items[i].margin[3];
        ay = p->area_y + items[i].margin[0];
        ah = p->area_h - items[i].margin[0] - items[i].margin[2];
        if (aw < 0) aw = 0;
        if (ah < 0) ah = 0;

        unsigned char js = items[i].justify_self != GA_AUTO ? items[i].justify_self : cfg->justify_items;
        unsigned char as = items[i].align_self   != GA_AUTO ? items[i].align_self   : cfg->align_items;
        int jd = self_dir(js, 1, cfg->rtl), ad = self_dir(as, 0, 0);

        /* Used size: a definite preferred size wins; otherwise stretch fills the
         * area and every other value shrink-to-fits. */
        int w, h;
        if (items[i].def_w != GRID_INDEFINITE)      w = items[i].def_w;
        else if (jd == 3)                            w = aw;
        else                                         w = imax(mc[i].min_content, imin(mc[i].max_content, aw));
        if (items[i].def_h != GRID_INDEFINITE)      h = items[i].def_h;
        else if (ad == 3)                            h = ah;
        else                                         h = imax(mr[i].min_content, imin(mr[i].max_content, ah));

        /* s10.2: auto margins absorb the free space first and disable the
         * self-alignment property in that axis. */
        int fx = aw - w, fy = ah - h;
        int ml_auto = items[i].m_auto[3], mr_auto = items[i].m_auto[1];
        int mt_auto = items[i].m_auto[0], mb_auto = items[i].m_auto[2];
        if (fx > 0 && (ml_auto || mr_auto)) {
            int nauto = (ml_auto ? 1 : 0) + (mr_auto ? 1 : 0);
            p->x = ax + (ml_auto ? (nauto == 2 ? fx/2 : fx) : 0);
        } else {
            p->x = ax + (jd == 1 ? fx/2 : jd == 2 ? fx : 0);
        }
        if (fy > 0 && (mt_auto || mb_auto)) {
            int nauto = (mt_auto ? 1 : 0) + (mb_auto ? 1 : 0);
            p->y = ay + (mt_auto ? (nauto == 2 ? fy/2 : fy) : 0);
        } else {
            p->y = ay + (ad == 1 ? fy/2 : ad == 2 ? fy : 0);
        }
        p->w = w; p->h = h;
    }

    /* Baseline self-alignment, applied after positioning: within each row, the
     * participating items' baselines are brought onto the row's shared line. */
    for (x = 0; x < nrows; x++) {
        int maxb = -1;
        for (i = 0; i < nitems; i++) {
            if (out->items[i].row != x || out->items[i].rowspan != 1) continue;
            unsigned char a = items[i].align_self != GA_AUTO ? items[i].align_self : cfg->align_items;
            if (a != GA_BASELINE || items[i].baseline < 0) continue;
            int b = items[i].baseline + items[i].margin[0];
            if (b > maxb) maxb = b;
        }
        if (maxb < 0) continue;
        for (i = 0; i < nitems; i++) {
            if (out->items[i].row != x || out->items[i].rowspan != 1) continue;
            unsigned char a = items[i].align_self != GA_AUTO ? items[i].align_self : cfg->align_items;
            if (a != GA_BASELINE || items[i].baseline < 0) continue;
            out->items[i].y = out->items[i].area_y + maxb - items[i].baseline;
        }
    }

    out->ncols = ncols; out->nrows = nrows;
    out->col_explicit = col0; out->row_explicit = row0;
    out->ncols_explicit = nec; out->nrows_explicit = ner;
    rc = 0;
done:
    free(tc); free(tr); free(ti); free(mc); free(mr); free(gapc); free(gapr);
    free(Lc.nm); free(Lr.nm);
    grid_tracklist_free(&ec); grid_tracklist_free(&er);
    if (rc < 0) grid_out_free(out);
    return rc;
}

/* ==========================================================================
 * WHAT IS NOT IMPLEMENTED, said plainly
 *
 *  - subgrid (css-grid-2). Not started. A subgrid's tracks come from its
 *    parent's, which means the two containers cannot be sized independently;
 *    that is a different shape of algorithm, not a missing branch here.
 *  - masonry (css-grid-3). Not started.
 *  - Fragmentation (s13): no page/column breaking of a grid.
 *  - Absolutely-positioned children of a grid container (s9). They are out of
 *    flow; the caller should not pass them as items.
 *  - Baseline alignment is first-baseline, single-row/column groups only.
 *    `last baseline` parses and is treated as `baseline`; items spanning
 *    several tracks do not participate.
 *  - The s11.1 steps 3 and 4 re-resolution passes (re-size columns after rows
 *    if an item's min-content contribution changed) are not run. They matter
 *    for orthogonal writing modes and aspect-ratio items, neither of which this
 *    engine has.
 *  - `justify-content: stretch` on the container stretches only `auto`-max
 *    tracks (s12.8); it does not stretch items, which is correct, but note that
 *    the content-distribution fallback for a negative free space is applied as
 *    `start` for every distribution value.
 *  - Percentage GAPS are not accepted: gap_x/gap_y are px. The caller resolves.
 * ========================================================================== */
