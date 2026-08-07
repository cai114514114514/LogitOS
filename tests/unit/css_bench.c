/* css_bench: the per-phase cost of a REAL page, host-side.
 *
 * The complaint this exists to answer is "deepseek.com repaints extremely
 * slowly". Before that can be fixed it has to be attributed, and the guest is
 * the wrong place to attribute it: under TCG every phase is slow, the network
 * is in the same wall clock, and one number ("the page took 40 seconds") tells
 * you nothing about which of parse / cascade / layout / paint to open.
 *
 * So this runs the EXACT pipeline browser.c runs -- collect_style,
 * css_expand_vars, css_apply, css_extra_apply, layout_page, browser_paint --
 * over a fixture captured from the live site, with the network removed and
 * each phase timed separately. The painter is the real browser_paint.c with
 * the GUI syscalls replaced by recorders (tests/unit/painthost/logit.h), so
 * paint cost here is the display-list walk, which is the part we own.
 *
 * Two things are deliberately NOT modelled, and they are the honest gaps:
 *   - text_measure() is a stub returning len*(px/2). On the machine it is a
 *     syscall into the kernel's font shaper. Layout's *own* cost is measured;
 *     the shaping it calls into belongs to another line and would swamp the
 *     signal here.
 *   - the guest is TCG, so absolute numbers are ~10-30x these. RATIOS between
 *     phases, and before/after on the same phase, are what transfer.
 *
 * Usage:  css_bench <page.html> [sheet.css ...]
 *         css_bench --iters=N ...
 *         css_bench --phase=<name> ...   (print one median, for a make gate)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "logit.h"                 /* the paint recorder, via -Itests/unit/painthost */
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

/* ---- timing ---- */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* The host is shared with other agents' QEMU instances, so a single run is
 * worthless. Every phase is sampled ITERS times and reported as a MEDIAN with
 * the min and max around it -- if max/min is wild, the machine was busy and
 * the median is still the number to read. */
static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

#define MAXPHASE 16
#define MAXITER  201
struct phase {
    const char *name;
    double t[MAXITER];
    int n;
    long count;                 /* phase-specific unit (elements, items, ops) */
    const char *unit;
};
static struct phase g_ph[MAXPHASE];
static int g_nph;

static struct phase *phase(const char *name)
{
    for (int i = 0; i < g_nph; i++)
        if (!strcmp(g_ph[i].name, name)) return &g_ph[i];
    g_ph[g_nph].name = name;
    return &g_ph[g_nph++];
}
static void record(const char *name, double ms)
{
    struct phase *p = phase(name);
    if (p->n < MAXITER) p->t[p->n++] = ms;
}
static void note(const char *name, long count, const char *unit)
{
    struct phase *p = phase(name);
    p->count = count; p->unit = unit;
}
static double median(struct phase *p)
{
    if (!p->n) return 0;
    double tmp[MAXITER];
    memcpy(tmp, p->t, p->n * sizeof(double));
    qsort(tmp, p->n, sizeof(double), cmp_d);
    return tmp[p->n / 2];
}

/* ---- fixture loading ---- */
static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "css_bench: cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    b[n] = 0; fclose(f);
    *len = (int)n;
    return b;
}

/* same as browser.c collect_style */
static int tag_is(const char *t, const char *lit)
{
    int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0;
}
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

static long count_elems(struct node *n)
{
    if (!n) return 0;
    long c = (n->type == N_ELEM);
    for (struct node *k = n->first_child; k; k = k->next) c += count_elems(k);
    return c;
}

/* Find an element deep in the page to use as the scoped-invalidation subject:
 * the LAST element with a class attribute, which on a React page is a leaf. */
static struct node *g_leaf;
static void find_leaf(struct node *n)
{
    if (!n) return;
    if (n->type == N_ELEM && !n->first_child && dom_attr(n, "class")) g_leaf = n;
    for (struct node *k = n->first_child; k; k = k->next) find_leaf(k);
}

#define CSSMAX (2 * 1024 * 1024)
#define WINW   760
#define VIEWH  520

int main(int argc, char **argv)
{
    int iters = 9;
    const char *only = 0;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strncmp(argv[argi], "--iters=", 8)) iters = atoi(argv[argi] + 8);
        else if (!strncmp(argv[argi], "--phase=", 8)) only = argv[argi] + 8;
        else break;
    }
    if (argi >= argc) { fprintf(stderr, "usage: css_bench [--iters=N] [--phase=P] page.html [sheet.css...]\n"); return 2; }
    if (iters < 1) iters = 1;
    if (iters > MAXITER) iters = MAXITER;

    int htmllen = 0;
    char *html = slurp(argv[argi], &htmllen);

    /* External sheets, concatenated exactly the way browser.c appends them. */
    static char sheets[CSSMAX];
    int sheetlen = 0;
    for (int i = argi + 1; i < argc; i++) {
        int l = 0; char *s = slurp(argv[i], &l);
        if (sheetlen + l + 1 < CSSMAX) {
            memcpy(sheets + sheetlen, s, l); sheetlen += l;
            sheets[sheetlen++] = '\n';
        }
        free(s);
    }

    static char author_css[CSSMAX];
    static char expanded[CSSMAX];

    css_init();
    css_viewport(WINW, VIEWH);
    css_set_post_pass(css_extra_apply);

    if (!only) {
        printf("css_bench: %s (%d KiB html, %d KiB external css), %d iters\n",
               argv[argi], htmllen / 1024, sheetlen / 1024, iters);
    }

    struct node *root = 0;
    int css_len = 0, exlen = 0;

    for (int it = 0; it < iters; it++) {
        double t0;

        /* ---- phase: parse ---- */
        t0 = now_ms();
        root = dom_parse(html, htmllen);
        record("parse", now_ms() - t0);
        if (!root) { fprintf(stderr, "css_bench: parse failed\n"); return 2; }

        /* ---- phase: collect (inline <style> text) ---- */
        t0 = now_ms();
        css_len = collect_style(root, author_css, 0, CSSMAX);
        record("collect", now_ms() - t0);

        /* browser.c appends the external sheets after the first paint; the
         * numbers that matter are the SECOND pass, with the real stylesheet
         * set, because that is the one the user waits through. */
        if (sheetlen && css_len + sheetlen < CSSMAX) {
            memcpy(author_css + css_len, sheets, sheetlen);
            css_len += sheetlen;
        }

        /* ---- phase: expand-vars ---- */
        t0 = now_ms();
        exlen = css_expand_vars(author_css, css_len, expanded, CSSMAX);
        record("expand-vars", now_ms() - t0);

        /* ---- phase: cascade ---- */
        t0 = now_ms();
        css_apply(root, expanded, exlen);
        record("cascade", now_ms() - t0);

        /* ---- phase: css-extra ---- */
        t0 = now_ms();
        css_extra_apply(root, expanded, exlen);
        record("css-extra", now_ms() - t0);

        /* CONTROL for css-extra: the len<=0 branch runs walk_inline + walk_anim
         * and nothing else. What separates it from `css-extra` above is the
         * compiled rules' tree walk; what it costs on its own is the per-element
         * inline style= re-parse, which no sheet cache can remove. */
        t0 = now_ms();
        css_extra_apply(root, "", 0);
        record("extra-inline", now_ms() - t0);

        /* ---- phase: layout ---- */
        t0 = now_ms();
        layout_page(root, WINW);
        record("layout", now_ms() - t0);

        /* ---- phase: paint ---- */
        paint_nops = 0;
        t0 = now_ms();
        browser_paint(0, 0, WINW, VIEWH, 0);
        record("paint", now_ms() - t0);

        /* ---- REPAINT, which is the user's actual complaint ----
         * A scroll does not re-parse, re-expand or re-cascade. It repaints the
         * existing display list at a new offset. Measured separately, and at a
         * scrolled offset so the viewport-cull path is exercised the way a
         * real scroll exercises it. */
        paint_nops = 0;
        t0 = now_ms();
        browser_paint(0, 0, WINW, VIEWH, 400);
        record("repaint-scroll", now_ms() - t0);

        /* ---- SCOPED RESTYLE, the js-mutation repaint path ----
         * This is the number the invalidation work bought, and the one that
         * must not regress. */
        g_leaf = 0; find_leaf(root);
        if (g_leaf) {
            t0 = now_ms();
            css_apply_scoped(g_leaf, 1, expanded, exlen);
            record("restyle-scoped", now_ms() - t0);

            /* CONTROL, and the whole diagnosis: the same scoped re-style with
             * an EMPTY author sheet. Identical scope, identical element count,
             * identical diff -- the only thing removed is parsing the author
             * stylesheet. Whatever separates these two lines is what the
             * cascade is paying per mutation for a sheet that never changed. */
            t0 = now_ms();
            css_apply_scoped(g_leaf, 1, "", 0);
            record("restyle-nosheet", now_ms() - t0);

            /* The control above removes TWO things at once: the LibCSS parse of
             * the author sheet, and css_extra's textual re-scan of it (which
             * css_apply_scoped runs once per node in scope). Time one
             * css_extra_apply call over a single leaf to separate them: its
             * cost is almost entirely the whole-sheet scan, not the subtree. */
            t0 = now_ms();
            css_extra_apply(g_leaf, expanded, exlen);
            record("extra-1leaf", now_ms() - t0);

            int nsib = 0;
            for (struct node *s = g_leaf->next; s; s = s->next)
                if (s->type == N_ELEM) nsib++;
            note("extra-1leaf", nsib + 1, "calls per scoped restyle");
        }

        /* ---- relayout: what a scoped restyle still costs downstream ---- */
        t0 = now_ms();
        layout_page(root, WINW);
        record("relayout", now_ms() - t0);

        if (it == iters - 1) {
            note("parse", count_elems(root), "elements");
            note("expand-vars", css_vars_count(), "custom props");
            { int styled = 0, hits = 0; css_stats(&styled, &hits);
              note("cascade", styled, "styled (cumulative)"); }
            note("layout", layout_count(), "display items");
            paint_nops = 0;
            browser_paint(0, 0, WINW, VIEWH, 0);
            note("paint", paint_nops, "draw ops");
            note("collect", css_len, "css bytes");
            note("css-extra", css_extra_rules(), "compiled patch rules");
        } else {
            dom_free(root); root = 0;
        }
    }

    if (only) {
        struct phase *p = 0;
        for (int i = 0; i < g_nph; i++) if (!strcmp(g_ph[i].name, only)) p = &g_ph[i];
        if (!p) { fprintf(stderr, "css_bench: no phase '%s'\n", only); return 2; }
        printf("%.4f\n", median(p));
        return 0;
    }

    /* ---- the table ---- */
    double load_total = 0;
    static const char *load_phases[] = { "parse", "collect", "expand-vars",
                                         "cascade", "css-extra", "layout", "paint", 0 };
    for (int i = 0; load_phases[i]; i++) load_total += median(phase((char *)load_phases[i]));

    printf("\n  %-16s %10s %10s %10s %7s   %s\n",
           "phase", "median ms", "min", "max", "%load", "unit");
    printf("  %-16s %10s %10s %10s %7s   %s\n",
           "----------------", "---------", "---------", "---------", "------", "----");
    for (int i = 0; i < g_nph; i++) {
        struct phase *p = &g_ph[i];
        double tmp[MAXITER];
        memcpy(tmp, p->t, p->n * sizeof(double));
        qsort(tmp, p->n, sizeof(double), cmp_d);
        int isload = 0;
        for (int k = 0; load_phases[k]; k++) if (!strcmp(load_phases[k], p->name)) isload = 1;
        char pct[16];
        if (isload) snprintf(pct, sizeof pct, "%6.1f%%", 100.0 * median(p) / (load_total ? load_total : 1));
        else snprintf(pct, sizeof pct, "%7s", "-");
        printf("  %-16s %10.3f %10.3f %10.3f %s   %ld %s\n",
               p->name, median(p), tmp[0], tmp[p->n - 1], pct,
               p->count, p->unit ? p->unit : "");
    }
    printf("  %-16s %10.3f\n", "LOAD TOTAL", load_total);
    return 0;
}
