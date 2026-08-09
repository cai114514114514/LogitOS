/* tests/unit/layout_box_survey.c -- the corpus survey behind tests/layoutbox.mk.
 *
 * c/apps/browser/js_cssom.h states its ask as a measurement: over
 * css/css-align, css/css-sizing, css/css-flexbox, css/css-grid and
 * css/cssom-view, 2,583 of 14,997 border-box reads found NO BOX AT ALL and
 * every geometry accessor on those elements answered 0. This answers the other
 * half of that: with the box table in layout.c, how many of those elements do
 * have a box.
 *
 * The classification here is border_box()'s, copied deliberately rather than
 * shared, because js_cssom.c is another line's file and this must not depend
 * on when they wire the two functions up:
 *
 *   EXACT      the element has its own IT_RECT in the display list
 *   INKUNION   it does not, but something in its subtree painted, so the old
 *              code answered with the subtree's ink -- which js_cssom.h's own
 *              header calls a known wrong answer
 *   NOBOX      nothing at all
 *
 * and against each class, whether layout_node_box() now answers.
 *
 * WHAT THIS IS NOT. border_box() is called on the elements a TEST asks about;
 * this walks every element in every document. The populations are different
 * and the percentages are therefore not the same percentages. What it settles
 * is whether the table answers the NOBOX class, which no per-call weighting
 * can turn into less than it is. It also does not fetch external stylesheets
 * (no network, no WEBAPI_FILE_ROOT here) -- an inline <style> and a style=""
 * attribute are what cascade, which under-counts boxes rather than over.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static long n_exact, n_ink, n_nobox;
static long t_exact, t_ink, t_nobox;      /* ...of which layout_node_box answers */
static long n_files;

/* The tags of the elements the table still cannot answer. Printed because the
 * residue is the next person's work order, and a percentage does not say where
 * to start. */
#define MISSTAGS 64
static struct { char tag[24]; long n; } g_miss[MISSTAGS];
static int g_nmiss;
static void miss(const char *tag)
{
    if (!tag) tag = "?";
    for (int i = 0; i < g_nmiss; i++)
        if (!strcmp(g_miss[i].tag, tag)) { g_miss[i].n++; return; }
    if (g_nmiss >= MISSTAGS) return;
    snprintf(g_miss[g_nmiss].tag, sizeof g_miss[0].tag, "%s", tag);
    g_miss[g_nmiss++].n = 1;
}

static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max){
    if(!n) return o;
    if(n->type==N_ELEM && tag_is(n->tag,"style"))
        for(struct node *c=n->first_child;c;c=c->next)
            if(c->type==N_TEXT && c->text)
                for(int i=0;i<c->textlen && o<max-1;i++) out[o++]=c->text[i];
    for(struct node *c=n->first_child;c;c=c->next) o=collect_style(c,out,o,max);
    return o;
}

static int in_subtree(const struct node *root, const struct node *n)
{ for(; n; n = n->parent) if (n == root) return 1; return 0; }

static void classify(struct node *el)
{
    const struct item *it = layout_items();
    int n = layout_count();
    int cls = 2;                                     /* 0 exact, 1 ink, 2 nobox */
    for (int i = 0; i < n; i++)
        if (it[i].node == el && it[i].type == IT_RECT) { cls = 0; break; }
    if (cls == 2)
        for (int i = 0; i < n; i++)
            if (it[i].node && in_subtree(el, it[i].node)) { cls = 1; break; }
    int x, y, w, h;
    int have = layout_node_box(el, &x, &y, &w, &h);
    if (cls == 0) { n_exact++; t_exact += have; }
    else if (cls == 1) { n_ink++; t_ink += have; if (!have) miss(el->tag); }
    else { n_nobox++; t_nobox += have; if (!have) miss(el->tag); }
}

/* Elements that generate no box BY DEFINITION, and counting them would make
 * the NOBOX class look enormous while saying nothing: everything in <head>,
 * the metadata elements wherever they appear, and any display:none subtree.
 * A geometry accessor is never called on these; border_box() never sees them.
 * Excluding them is what makes the remaining NOBOX count the thing this line
 * is about -- an element that WAS laid out and had nothing to show for it. */
static int is_metadata(const struct node *n)
{
    static const char *m[] = { "head","title","meta","link","base","style","script",
                               "template","noscript","param","source","track", 0 };
    if (!n->tag) return 0;
    for (int i = 0; m[i]; i++) if (!strcmp(n->tag, m[i])) return 1;
    return 0;
}

static void walk(struct node *n)
{
    if (n->type == N_ELEM) {
        if (is_metadata(n)) return;
        const struct cstyle *st = (const struct cstyle *)n->style;
        if (st && st->display == DISP_NONE) return;
        classify(n);
    }
    for (struct node *c = n->first_child; c; c = c->next) walk(c);
}

static char g_css[1 << 20];

static void do_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (8L << 20)) { fclose(f); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return; }
    fclose(f);
    buf[sz] = 0;
    struct node *root = dom_parse(buf, (int)sz);
    if (root) {
        int cl = collect_style(root, g_css, 0, (int)sizeof g_css);
        css_apply(root, g_css, cl);
        layout_page(root, 800);
        walk(root);
        layout_free();
        n_files++;
    }
    free(buf);
}

static int is_html(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d) return 0;
    return !strcmp(d, ".html") || !strcmp(d, ".htm") || !strcmp(d, ".xht");
}

static void do_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char sub[4096];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) do_dir(sub);
        else if (is_html(e->d_name)) do_file(sub);
    }
    closedir(d);
}

static void row(const char *name, long total, long answered)
{
    printf("  %-9s %7ld   layout_node_box answers %7ld  (%5.1f%%)\n",
           name, total, answered, total ? 100.0 * (double)answered / (double)total : 0.0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <wpt-root> <dir> [dir...]\n", argv[0]);
        return 2;
    }
    char path[4096];
    for (int i = 2; i < argc; i++) {
        snprintf(path, sizeof path, "%s/%s", argv[1], argv[i]);
        do_dir(path);
    }
    long tot = n_exact + n_ink + n_nobox;
    printf("\n---- box-table survey: %ld files, %ld elements ----\n", n_files, tot);
    printf("  classified the way js_cssom.c's border_box() classifies:\n");
    row("EXACT", n_exact, t_exact);
    row("INKUNION", n_ink, t_ink);
    row("NOBOX", n_nobox, t_nobox);
    if (tot)
        printf("\n  NOBOX was %.1f%% of elements; of those, %.1f%% now have a box.\n",
               100.0 * (double)n_nobox / (double)tot,
               n_nobox ? 100.0 * (double)t_nobox / (double)n_nobox : 0.0);
    /* The residue, by tag: the next person's work order. A percentage does not
     * say where to start and this does. */
    for (int i = 1; i < g_nmiss; i++) {
        int j = i;
        while (j > 0 && g_miss[j - 1].n < g_miss[j].n) {
            char tt[24]; long tn;
            memcpy(tt, g_miss[j - 1].tag, sizeof tt); tn = g_miss[j - 1].n;
            memcpy(g_miss[j - 1].tag, g_miss[j].tag, sizeof tt); g_miss[j - 1].n = g_miss[j].n;
            memcpy(g_miss[j].tag, tt, sizeof tt); g_miss[j].n = tn;
            j--;
        }
    }
    printf("\n  still unanswered, by tag (top 20):\n");
    for (int i = 0; i < g_nmiss && i < 20; i++)
        printf("    %-14s %ld\n", g_miss[i].tag, g_miss[i].n);
    return 0;
}
