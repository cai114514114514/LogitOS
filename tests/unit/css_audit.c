/* css_audit: what a corpus of REAL pages needs from the CSS engine.
 *
 * The failure this exists to prevent is documented in the project's own
 * history: a conformance score that climbed from 2/1643 to 1723/1818 while the
 * browser still could not run one real site. A property count is not a measure
 * of whether pages render. So this tool never asks "how many properties do we
 * support"; it asks, of the bytes 15 real high-traffic sites actually ship:
 *
 *   1. WHICH DECLARATIONS DOES THE CASCADE DROP ON THE FLOOR?
 *      Instrumented inside LibCSS at parseProperty(), the single funnel every
 *      declaration passes through (see the css__parse_drop_report patch in
 *      third_party/css/libcss/src/parse/language.c). Three ways to lose one:
 *      the property name is unknown to this LibCSS, the value handler refuses
 *      it, or there is trailing junk. Unknown AT-rules are reported the same
 *      way with a leading '@', because `@supports{...}` losing its whole block
 *      is a bigger hole than any single declaration.
 *
 *   2. WHICH DECLARATIONS DOES LIBCSS PARSE THAT WE THEN IGNORE?
 *      Parsing is not rendering. A property LibCSS understands but css_engine.c
 *      never reads into `struct cstyle` is dropped just as completely, only
 *      later and more quietly. The KNOWN_UNREAD table below lists them and the
 *      raw-source scan counts them.
 *
 *   3. WHICH LAYOUT MODES DO THE PAGES ACTUALLY NEED?
 *      Not "how many elements say display:flex" -- that number is small and
 *      misleading. The number that matters is how much of the box tree a mode
 *      GOVERNS: if a page's whole shell is one display:flex and we lay it out
 *      as a block, every box inside it is wrong. So the census is weighted by
 *      subtree size: `governs` = elements having a flex/grid ancestor.
 *
 * Everything is computed from the same pipeline browser.c runs (dom_parse ->
 * collect_style -> css_expand_vars -> css_apply -> css_extra_apply ->
 * layout_page), over committed fixtures, so the answer is about the engine and
 * not about the network.
 *
 * Usage:
 *   css_audit <dir>...             one fixture dir per page (index.html + sheet-*.css)
 *   css_audit --page p.html s.css  an explicit page + sheets
 *   css_audit --top=N              how many ranked rows to print (default 30)
 *   css_audit --csv                machine-readable rows, for diffing runs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "logit.h"                 /* paint recorder, via -Itests/unit/painthost */
#include "layout.h"
#include "css.h"
#include "dom.h"
#include "browser_paint.h"

struct paintop paint_ops[PAINT_MAXOPS];
int paint_nops;

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len * (px/2); }
int res_fetch(const char *url, uint8_t **buf, int *len){ (void)url;(void)buf;(void)len; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out){ (void)p;(void)n;(void)out; return -1; }

/* ---- the LibCSS drop hook (see parse/language.h) ---- */
enum { DROP_UNKNOWN = 0, DROP_BADVALUE = 1, DROP_TRAILING = 2, DROP_ACCEPTED = 3 };
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

#define MAXPROP 2048
struct prow {
    char name[48];
    long drop[4];          /* by reason; [3] == ACCEPTED, which is not a drop */
    long declared;         /* raw occurrences in the source bytes */
    int  pages;            /* how many corpus pages declare it */
    int  seen_this_page;
};
static struct prow g_prop[MAXPROP];
static int g_nprop;

static struct prow *prow(const char *name, size_t len)
{
    if (len >= sizeof(g_prop[0].name)) len = sizeof(g_prop[0].name) - 1;
    for (int i = 0; i < g_nprop; i++)
        if (strlen(g_prop[i].name) == len && !memcmp(g_prop[i].name, name, len))
            return &g_prop[i];
    if (g_nprop >= MAXPROP) return &g_prop[MAXPROP - 1];
    struct prow *p = &g_prop[g_nprop++];
    memcpy(p->name, name, len); p->name[len] = 0;
    return p;
}

static int g_collecting;
static void on_drop(const char *name, size_t nlen, int reason)
{
    if (!g_collecting || reason < 0 || reason > 3) return;
    char lower[48];
    size_t n = nlen < sizeof(lower) - 1 ? nlen : sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++)
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
    struct prow *p = prow(lower, n);
    p->drop[reason]++;
    if (!p->seen_this_page) { p->seen_this_page = 1; p->pages++; }
}

/* ---- properties LibCSS parses that css_engine.c never reads --------------
 * Kept as an explicit list rather than derived, because "derived" would mean
 * parsing css_engine.c, and a wrong answer here is worse than no answer: these
 * are the ones that look supported from the parser's side and are invisible in
 * the pixels. Verified by grepping css_computed_* out of css_engine.c. */
static const char *KNOWN_UNREAD[] = {
    "background-image", "background-position", "background-repeat",
    "background-attachment", "text-transform", "vertical-align", "content",
    "cursor", "letter-spacing", "word-spacing", "text-indent", "outline",
    "outline-color", "outline-style", "outline-width", "border-collapse",
    "border-spacing", "caption-side", "empty-cells", "table-layout",
    "counter-increment", "counter-reset", "quotes", "direction", "unicode-bidi",
    "min-height", "column-count", "column-width", "column-gap", "column-rule",
    "orphans", "widows", "page-break-after", "page-break-before", "clip",
    "font-variant", "font-stretch", "writing-mode", "text-shadow",
    NULL
};
static int is_known_unread(const char *n)
{
    for (int i = 0; KNOWN_UNREAD[i]; i++) if (!strcmp(KNOWN_UNREAD[i], n)) return 1;
    return 0;
}

/* ---- raw declaration scan: ground truth of what the page DECLARES --------
 * Independent of LibCSS on purpose. A property inside an at-rule LibCSS threw
 * away never reaches parseProperty at all, so the drop counters cannot see it;
 * this scan can. It is a scanner, not a parser: it finds `ident:` at a
 * declaration position, which over minified CSS is accurate enough to rank by
 * and is never used for anything but ranking. */
static int ident_ch(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}
static long g_declared_total;

static void scan_declared(const char *s, int n)
{
    int i = 0, at_start = 1;      /* offset 0 IS a declaration position: that is
                                   * what a style="" attribute is, and it is also
                                   * the first declaration of a sheet that opens
                                   * without a comment. */
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i+1] == '*') {          /* comment */
            i += 2; while (i + 1 < n && !(s[i] == '*' && s[i+1] == '/')) i++;
            i += 2; continue;
        }
        if (c == '"' || c == '\'') {                            /* string */
            char q = c; i++;
            while (i < n && s[i] != q) { if (s[i] == '\\') i++; i++; }
            i++; at_start = 0; continue;
        }
        if (at_start || c == '{' || c == ';' || c == '}') {
            if (!at_start) i++;
            at_start = 0;
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                             s[i] == '\r' || s[i] == '\f')) i++;
            int st = i;
            while (i < n && ident_ch(s[i])) i++;
            if (i > st && i < n && s[i] == ':' && !(i + 1 < n && s[i+1] == ':')) {
                /* a custom property (--x) is not a property we could support */
                if (!(i - st >= 2 && s[st] == '-' && s[st+1] == '-')) {
                    char low[48];
                    size_t len = (size_t)(i - st);
                    if (len >= sizeof(low)) len = sizeof(low) - 1;
                    for (size_t k = 0; k < len; k++)
                        low[k] = (s[st+k] >= 'A' && s[st+k] <= 'Z') ? s[st+k] + 32 : s[st+k];
                    struct prow *p = prow(low, len);
                    p->declared++;
                    g_declared_total++;
                    if (!p->seen_this_page) { p->seen_this_page = 1; p->pages++; }
                }
            } else if (i == st) {
                i++;              /* not an identifier here; do not spin */
            }
            continue;
        }
        i++;
    }
}

/* The cascade also parses every element's style="" attribute as its own inline
 * sheet, and those declarations DO reach parseProperty. Counting only the
 * stylesheet bytes in the denominator made `reach` exceed 100% on the pages
 * with heavy inline styling -- a number over 100% is the measurement telling
 * you the two sides are not counting the same thing. */
static void scan_inline_styles(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM) {
        const char *v = dom_attr(n, "style");
        if (v && *v) scan_declared(v, (int)strlen(v));
    }
    for (struct node *c = n->first_child; c; c = c->next) scan_inline_styles(c);
}

/* ---- layout-mode census -------------------------------------------------- */
struct census {
    long elems, styled;
    long disp[8];               /* DISP_* */
    long governs_flex, governs_grid;   /* elements with a flex/grid ancestor */
    long pos[5];                /* POS_* */
    long floats, clears;
    long ovf_clip;              /* overflow != visible */
    long has_z, radius, opacity_partial;
    long grid_containers, flex_containers;
    long items, zero_boxes;
    long fullw, cols, rects;   /* see the geometry note in audit_one() */
};
static struct census g_cs;

static void census_walk(struct node *n, int in_flex, int in_grid)
{
    if (!n) return;
    int nf = in_flex, ng = in_grid;
    if (n->type == N_ELEM) {
        g_cs.elems++;
        struct cstyle *st = (struct cstyle *)n->style;
        if (st) {
            g_cs.styled++;
            if (st->display >= 0 && st->display < 8) g_cs.disp[st->display]++;
            if (in_flex) g_cs.governs_flex++;
            if (in_grid) g_cs.governs_grid++;
            if (st->position < 5) g_cs.pos[st->position]++;
            if (st->flt != FLT_NONE) g_cs.floats++;
            if (st->clr != CLR_NONE) g_cs.clears++;
            if (st->overflow_x != OVF_VISIBLE || st->overflow_y != OVF_VISIBLE) g_cs.ovf_clip++;
            if (st->has_z) g_cs.has_z++;
            if (st->radius || st->radius_pct) g_cs.radius++;
            if (st->opacity < 255 && st->opacity > 0) g_cs.opacity_partial++;
            if (st->display == DISP_FLEX) { g_cs.flex_containers++; nf = 1; }
            if (st->display == DISP_GRID || st->grid_cols > 0) { g_cs.grid_containers++; ng = 1; }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) census_walk(c, nf, ng);
}

/* ---- fixture loading ---- */
static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0; fclose(f);
    *len = (int)n;
    return b;
}

static int tag_is(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style"))
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    for (struct node *c = n->first_child; c; c = c->next) o = collect_style(c, out, o, max);
    return o;
}

#define CSSMAX (8 * 1024 * 1024)
#define WINW   980
#define VIEWH  700

static char author_css[CSSMAX];
static char expanded[CSSMAX];
static char sheets[CSSMAX];
static const char *g_dumpcss;   /* --dumpcss=FILE: write the post-var-expansion sheet */

struct pagerow {
    char name[32];
    long elems, items, flexc, gridc, gov_flex, gov_grid, absfix, sticky, clip;
    long dropped, atdropped, accepted, declared;
    long fullw, cols, rects;
    int  htmlk, cssk;
};
static struct pagerow g_page[64];
static int g_npage;

static void audit_one(const char *label, const char *htmlpath, char **sheetpaths, int nsheet)
{
    int htmllen = 0;
    char *html = slurp(htmlpath, &htmllen);
    if (!html) { fprintf(stderr, "css_audit: cannot open %s\n", htmlpath); return; }

    int sheetlen = 0;
    for (int i = 0; i < nsheet; i++) {
        int l = 0; char *s = slurp(sheetpaths[i], &l);
        if (!s) continue;
        if (sheetlen + l + 1 < CSSMAX) {
            memcpy(sheets + sheetlen, s, l); sheetlen += l;
            sheets[sheetlen++] = '\n';
        }
        free(s);
    }

    for (int i = 0; i < g_nprop; i++) g_prop[i].seen_this_page = 0;

    struct node *root = dom_parse(html, htmllen);
    if (!root) { fprintf(stderr, "css_audit: parse failed %s\n", htmlpath); free(html); return; }

    int css_len = collect_style(root, author_css, 0, CSSMAX);
    if (sheetlen && css_len + sheetlen < CSSMAX) {
        memcpy(author_css + css_len, sheets, sheetlen);
        css_len += sheetlen;
    }
    int exlen = css_expand_vars(author_css, css_len, expanded, CSSMAX);
    if (g_dumpcss) {
        FILE *f = fopen(g_dumpcss, "wb");
        if (f) { fwrite(expanded, 1, exlen, f); fclose(f); }
        fprintf(stderr, "css_audit: %s: css_len=%d -> exlen=%d (dumped to %s)\n",
                label, css_len, exlen, g_dumpcss);
    }

    long decl_before = g_declared_total;
    scan_declared(expanded, exlen);
    scan_inline_styles(root);
    long decl_after = g_declared_total;

    long before = 0, atbefore = 0, accbefore = 0;
    for (int i = 0; i < g_nprop; i++) {
        long d = g_prop[i].drop[0] + g_prop[i].drop[1] + g_prop[i].drop[2];
        before += d; accbefore += g_prop[i].drop[3];
        if (g_prop[i].name[0] == '@') atbefore += d;
    }

    g_collecting = 1;
    css_apply(root, expanded, exlen);
    g_collecting = 0;
    css_extra_apply(root, expanded, exlen);

    long after = 0, atafter = 0, accafter = 0;
    for (int i = 0; i < g_nprop; i++) {
        long d = g_prop[i].drop[0] + g_prop[i].drop[1] + g_prop[i].drop[2];
        after += d; accafter += g_prop[i].drop[3];
        if (g_prop[i].name[0] == '@') atafter += d;
    }

    memset(&g_cs, 0, sizeof(g_cs));
    census_walk(root, 0, 0);

    layout_page(root, WINW);
    g_cs.items = layout_count();
    const struct item *it = layout_items();
    /* GEOMETRY, not property coverage. "The page came out as one unstyled
     * column" has a shape, and these two numbers are that shape:
     *   fullw  -- boxes spanning (nearly) the whole canvas. A block fallback
     *             makes almost every box full width; a laid-out page does not.
     *   cols   -- how many distinct LEFT EDGES the display list uses, bucketed
     *             to 8px. A single column has a handful; a real shell with a
     *             sidebar, a nav row and a card grid has dozens.
     * Neither needs a reference browser to be meaningful in the before/after
     * direction, which is what `make audit-css-before` compares. */
    unsigned char seen[WINW / 8 + 2];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < g_cs.items; i++) {
        if (it[i].w <= 0 || it[i].h <= 0) { g_cs.zero_boxes++; continue; }
        /* RECTs only. Text items are per-word, so counting their left edges
         * measures how much prose the page has, not how it is laid out; the
         * boxes are what a block fallback collapses into one column. */
        if (it[i].type != IT_RECT) continue;
        g_cs.rects++;
        if (it[i].w >= WINW * 90 / 100) g_cs.fullw++;
        int b = it[i].x / 8;
        if (b >= 0 && b < (int)sizeof seen && !seen[b]) { seen[b] = 1; g_cs.cols++; }
    }

    struct pagerow *r = &g_page[g_npage < 64 ? g_npage++ : 63];
    snprintf(r->name, sizeof(r->name), "%s", label);
    r->elems = g_cs.elems; r->items = g_cs.items;
    r->flexc = g_cs.flex_containers; r->gridc = g_cs.grid_containers;
    r->gov_flex = g_cs.governs_flex; r->gov_grid = g_cs.governs_grid;
    r->absfix = g_cs.pos[POS_ABSOLUTE] + g_cs.pos[POS_FIXED];
    r->sticky = g_cs.pos[POS_STICKY];
    r->clip = g_cs.ovf_clip;
    r->dropped = after - before;
    r->atdropped = atafter - atbefore;
    r->fullw = g_cs.fullw; r->cols = g_cs.cols; r->rects = g_cs.rects;
    r->accepted = accafter - accbefore;
    r->declared = decl_after - decl_before;
    r->htmlk = htmllen / 1024; r->cssk = exlen / 1024;

    layout_free();
    dom_free(root);
    free(html);
}

static int cmp_rank(const void *a, const void *b)
{
    const struct prow *x = a, *y = b;
    long dx = x->drop[0] + x->drop[1] + x->drop[2];
    long dy = y->drop[0] + y->drop[1] + y->drop[2];
    if (dx != dy) return dx < dy ? 1 : -1;
    if (x->pages != y->pages) return x->pages < y->pages ? 1 : -1;
    return 0;
}
static int cmp_declared(const void *a, const void *b)
{
    const struct prow *x = a, *y = b;
    if (x->declared != y->declared) return x->declared < y->declared ? 1 : -1;
    return 0;
}

int main(int argc, char **argv)
{
    int top = 30, csv = 0;
    int argi = 1;
    const char *explicit_page = NULL;
    for (; argi < argc; argi++) {
        if (!strncmp(argv[argi], "--top=", 6)) top = atoi(argv[argi] + 6);
        else if (!strcmp(argv[argi], "--csv")) csv = 1;
        else if (!strncmp(argv[argi], "--dumpcss=", 10)) g_dumpcss = argv[argi] + 10;
        else if (!strcmp(argv[argi], "--page")) { explicit_page = argv[++argi]; argi++; break; }
        else break;
    }

    css_init();
    css_viewport(WINW, VIEWH);
    css_set_post_pass(css_extra_apply);
    css__parse_drop_report = on_drop;

    if (explicit_page) {
        audit_one("page", explicit_page, argv + argi, argc - argi);
    } else {
        for (; argi < argc; argi++) {
            const char *dir = argv[argi];
            char html[512]; snprintf(html, sizeof(html), "%s/index.html", dir);
            char *sp[32]; int ns = 0;
            for (int i = 1; i <= 30 && ns < 32; i++) {
                char p[512]; snprintf(p, sizeof(p), "%s/sheet-%d.css", dir, i);
                FILE *f = fopen(p, "rb");
                if (!f) continue;
                fclose(f);
                sp[ns] = strdup(p); ns++;
            }
            /* the label is the directory's basename, and callers pass the dir
             * both with and without a trailing slash ($(dir ...) adds one) */
            char lbl[32];
            int dl = (int)strlen(dir);
            while (dl > 0 && (dir[dl-1] == '/' || dir[dl-1] == '\\')) dl--;
            int bs = dl;
            while (bs > 0 && dir[bs-1] != '/' && dir[bs-1] != '\\') bs--;
            snprintf(lbl, sizeof(lbl), "%.*s", dl - bs, dir + bs);
            audit_one(lbl, html, sp, ns);
            for (int i = 0; i < ns; i++) free(sp[i]);
        }
    }

    css__parse_drop_report = NULL;

    /* ---- report ---- */
    if (csv) {
        printf("kind,name,drop_unknown,drop_badvalue,drop_trailing,declared,pages\n");
        for (int i = 0; i < g_nprop; i++) {
            struct prow *p = &g_prop[i];
            printf("prop,%s,%ld,%ld,%ld,%ld,%d\n", p->name,
                   p->drop[0], p->drop[1], p->drop[2], p->declared, p->pages);
        }
        for (int i = 0; i < g_npage; i++) {
            struct pagerow *r = &g_page[i];
            printf("page,%s,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", r->name,
                   r->elems, r->items, r->flexc, r->gridc, r->gov_flex, r->gov_grid,
                   r->absfix, r->sticky, r->clip);
        }
        return 0;
    }

    /* `reach` is the headline. It is accepted declarations over declared
     * declarations, and it is the only column that catches an at-rule failure
     * at its true size: everything inside a block LibCSS threw away never
     * reaches parseProperty, so it appears in neither the accept nor the drop
     * counters -- only in the gap between them and the raw source scan. */
    printf("\n=== per page ===\n");
    printf("%-9s %6s %6s %5s %5s %7s %7s %6s %5s %5s %6s %5s %7s %5s\n",
           "page", "elems", "rects", "flexC", "gridC", "govFlex", "govGrid",
           "absfix", "clip", "cols", "fullW", "full%", "declared", "reach");
    long tot_gov_flex = 0, tot_gov_grid = 0, tot_el = 0, tot_dec = 0, tot_acc = 0;
    long tot_items = 0, tot_full = 0;
    for (int i = 0; i < g_npage; i++) {
        struct pagerow *r = &g_page[i];
        printf("%-9s %6ld %6ld %5ld %5ld %7ld %7ld %6ld %5ld %5ld %6ld %4ld%% %7ld %4ld%%\n",
               r->name, r->elems, r->rects, r->flexc, r->gridc, r->gov_flex,
               r->gov_grid, r->absfix, r->clip, r->cols, r->fullw,
               r->rects ? r->fullw * 100 / r->rects : 0, r->declared,
               r->declared ? r->accepted * 100 / r->declared : 100);
        tot_gov_flex += r->gov_flex; tot_gov_grid += r->gov_grid; tot_el += r->elems;
        tot_dec += r->declared; tot_acc += r->accepted;
        tot_items += r->rects; tot_full += r->fullw;
    }
    if (tot_el)
        printf("%-9s %6ld %6ld %5s %5s %6ld%% %6ld%% %6s %5s %5s %6ld %4ld%% %7ld %4ld%%\n",
               "TOTAL", tot_el, tot_items, "", "",
               tot_gov_flex * 100 / tot_el, tot_gov_grid * 100 / tot_el,
               "", "", "", tot_full, tot_items ? tot_full * 100 / tot_items : 0,
               tot_dec, tot_dec ? tot_acc * 100 / tot_dec : 100);

    qsort(g_prop, g_nprop, sizeof(g_prop[0]), cmp_rank);
    printf("\n=== declarations the cascade DROPS, ranked (the work order) ===\n");
    printf("%-28s %9s %9s %9s %6s  %s\n",
           "property", "unknown", "badvalue", "trailing", "pages", "note");
    int shown = 0;
    for (int i = 0; i < g_nprop && shown < top; i++) {
        struct prow *p = &g_prop[i];
        long d = p->drop[0] + p->drop[1] + p->drop[2];
        if (!d) break;
        printf("%-28s %9ld %9ld %9ld %6d  %s\n", p->name, p->drop[0], p->drop[1],
               p->drop[2], p->pages, p->name[0] == '@' ? "AT-RULE: whole block lost" : "");
        shown++;
    }

    qsort(g_prop, g_nprop, sizeof(g_prop[0]), cmp_declared);
    printf("\n=== parsed but NEVER READ into struct cstyle (silent drops) ===\n");
    printf("%-28s %10s %6s\n", "property", "declared", "pages");
    shown = 0;
    for (int i = 0; i < g_nprop && shown < top; i++) {
        struct prow *p = &g_prop[i];
        if (!p->declared) break;
        long d = p->drop[0] + p->drop[1] + p->drop[2];
        if (d) continue;                       /* already ranked above */
        if (!is_known_unread(p->name)) continue;
        printf("%-28s %10ld %6d\n", p->name, p->declared, p->pages);
        shown++;
    }

    printf("\n=== most-declared properties overall (context for the ranking) ===\n");
    printf("%-28s %10s %6s %9s\n", "property", "declared", "pages", "dropped");
    shown = 0;
    for (int i = 0; i < g_nprop && shown < top; i++) {
        struct prow *p = &g_prop[i];
        if (!p->declared) break;
        long d = p->drop[0] + p->drop[1] + p->drop[2];
        printf("%-28s %10ld %6d %9ld\n", p->name, p->declared, p->pages, d);
        shown++;
    }
    return 0;
}
