/* css_selstatic_census: how much of 15 real sites does the static-pseudo
 * slice actually MOVE?
 *
 * A MEASUREMENT, not a gate -- test-css-selstatic is the gate next door, and
 * the two answer different questions. The gate proves :checked/:disabled/
 * :enabled/:target match the right elements on a fixture whose answer is known
 * by construction. This asks what that is worth on bytes nobody wrote for us.
 *
 * WHY IT DOES NOT COUNT SELECTORS. The obvious census -- grep the corpus for
 * ":checked" and report 252 -- is the shape of instrument this tree has been
 * burned by, and CLAUDE.md's rule 1 names the burn: the measurement is right
 * and the sentence around it sends the reader somewhere else. A `:checked` in
 * a sheet is worth nothing if no element on the page is a checked box, and it
 * is worth a whole panel if it gates a CSS-only disclosure widget. Counting
 * the text answers "how often is this written", never "what changed".
 *
 * SO IT MEASURES THE COMPUTED STYLE OF EVERY ELEMENT, and diffs two builds.
 * For each element it digests css_computed_text() over every property LibCSS
 * knows -- the same serialiser getComputedStyle() answers from -- and prints
 * one line per element. Run the shipped engine and the h_false engine, diff
 * the two files, and the differing lines ARE the elements whose style the
 * slice changed. Nothing is inferred: an element is in the count because its
 * computed style is literally different, property by property.
 *
 * THE DIRECTION OF THE ERROR IS THE POINT. This cannot over-report. If a
 * handler matched too much -- the `:enabled == not :disabled` mistake that
 * styles every div -- the number would be LARGER and would look like a better
 * result, which is exactly why the gate and not this file is what decides
 * whether the matching is right. A census that could be improved by a bug must
 * never be read as evidence of correctness; it is read as evidence of REACH,
 * and only after test-css-selstatic is green.
 *
 * :target needs a fragment to mean anything and a corpus page has no URL, so
 * the run takes one: --frag=NAME sets it for every page. Without it the
 * :target half of the slice is measured as the zero it correctly is.
 *
 * Usage:  css_selstatic_census [--frag=NAME] <dir>...
 * Build:  the css-selstatic-census rule in tests/cssweb.mk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit.h"
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

#define CSSMAX (8 * 1024 * 1024)
#define WINW   980
#define VIEWH  700

static char author_css[CSSMAX];
static char expanded[CSSMAX];
static char sheets[CSSMAX];

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

/* FNV-1a over every resolvable property's serialised computed value.
 *
 * Over ALL of them rather than a chosen few, because a chosen few is a guess
 * about which property the slice moves and the whole point is not to guess.
 * The serialiser is css_computed_text(), which is what getComputedStyle()
 * answers from, so a difference here is a difference a page could observe.
 *
 * THE BOUND IS CSSP__COUNT AND IT HAS TO BE. This loop was first written over
 * css_known_prop_count(), which is a DIFFERENT INDEX SPACE -- LibCSS's
 * alphabetical string table, ~300 entries -- while css_computed_text() takes
 * the CSSP_* enum, which has CSSP__COUNT. Indices past the end returned 0 and
 * hashed nothing, so the digest silently covered whichever properties the two
 * numberings happened to share at the bottom, and the census would have
 * reported a confident number for a subset it never named. It is written down
 * because the two counts are both "how many properties are there" and neither
 * name says which question it answers. */
static uint64_t elem_digest(struct node *n)
{
    uint64_t h = 1469598103934665603ULL;
    char buf[512];
    for (int p = 0; p < CSSP__COUNT; p++) {
        int len = css_computed_text(n, p, buf, (int)sizeof buf);
        if (len < 0) len = 0;
        buf[len < (int)sizeof buf ? len : (int)sizeof buf - 1] = 0;
        for (int i = 0; i < len; i++) { h ^= (unsigned char)buf[i]; h *= 1099511628211ULL; }
        h ^= (uint64_t)p + 0x9e37; h *= 1099511628211ULL;
    }
    return h;
}

static long g_idx;

static void walk(struct node *n, const char *page)
{
    if (!n) return;
    if (n->type == N_ELEM) {
        printf("%s\t%ld\t%s\t%016llx\n", page, g_idx++, n->tag,
               (unsigned long long)elem_digest(n));
    }
    for (struct node *c = n->first_child; c; c = c->next) walk(c, page);
}

/* ---- the SECOND half of the census, and the corpus forced it ------------
 *
 * The digest above answers "did any element's computed style change", which is
 * the question that matters for a page. On this corpus it answered ZERO, and
 * the reason is worth more than the number: apple ships
 * `button:disabled{cursor:default}` and really does have disabled buttons, so
 * the selector NOW MATCHES -- but `cursor` is one of the properties
 * css_engine.c parses and never reads into the computed style (audit-css's
 * "parsed but NEVER READ" table: content 1570, outline 374, cursor). A rule
 * that matches and carries only such properties moves nothing, and the digest
 * cannot tell that apart from a rule that never matched.
 *
 * Those are two completely different repairs -- one is this slice, the other
 * is the unread-property line -- so the census must not report them as one
 * number. This half counts MATCHES directly and is blind to what the rules
 * carry: each pseudo-class is probed with a sheet of this file's own, setting
 * a sentinel colour no corpus page uses, appended after the page's sheets with
 * !important so nothing can outrank it. An element whose computed colour comes
 * back as the sentinel satisfied the selector. That is the reach of the
 * handler, uncontaminated by the engine's property coverage. */
static const char *const k_probe[4] = {
    ":checked",  ":disabled",  ":enabled",  ":target"
};
static long g_match[4];

static void count_sentinel(struct node *n, int slot)
{
    if (!n) return;
    if (n->type == N_ELEM) {
        char buf[64];
        int len = css_computed_text(n, CSSP_COLOR, buf, (int)sizeof buf);
        if (len > 0) {
            buf[len < (int)sizeof buf ? len : (int)sizeof buf - 1] = 0;
            /* rgb(1, 2, 3) -- the sentinel. Compared on the serialised text so
             * this needs no knowledge of the colour representation. */
            if (strstr(buf, "1, 2, 3") || strstr(buf, "1,2,3")) g_match[slot]++;
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) count_sentinel(c, slot);
}

static void census_one(const char *label, const char *htmlpath,
                       char **sheetpaths, int nsheet)
{
    int htmllen = 0;
    char *html = slurp(htmlpath, &htmllen);
    if (!html) { fprintf(stderr, "census: cannot open %s\n", htmlpath); return; }

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

    struct node *root = dom_parse(html, htmllen);
    if (!root) { fprintf(stderr, "census: parse failed %s\n", htmlpath); free(html); return; }

    int css_len = collect_style(root, author_css, 0, CSSMAX);
    if (sheetlen && css_len + sheetlen < CSSMAX) {
        memcpy(author_css + css_len, sheets, sheetlen);
        css_len += sheetlen;
    }
    int exlen = css_expand_vars(author_css, css_len, expanded, CSSMAX);
    css_apply(root, expanded, exlen);
    css_extra_apply(root, expanded, exlen);

    g_idx = 0;
    walk(root, label);
    dom_free(root);

    /* The match half: one re-cascade per pseudo-class, each with the page's
     * own sheets plus a sentinel rule of ours. Re-parsed from the original
     * bytes every time so no probe can see another probe's rule. */
    for (int s = 0; s < 4; s++) {
        struct node *r2 = dom_parse(html, htmllen);
        if (!r2) break;
        int n2 = collect_style(r2, author_css, 0, CSSMAX);
        if (sheetlen && n2 + sheetlen < CSSMAX) {
            memcpy(author_css + n2, sheets, sheetlen);
            n2 += sheetlen;
        }
        int e2 = css_expand_vars(author_css, n2, expanded, CSSMAX);
        int add = snprintf(expanded + e2, (size_t)(CSSMAX - e2),
                           "\n%s{color:rgb(1,2,3)!important}\n", k_probe[s]);
        if (add > 0) e2 += add;
        css_apply(r2, expanded, e2);
        css_extra_apply(r2, expanded, e2);
        count_sentinel(r2, s);
        dom_free(r2);
    }
    free(html);
}

int main(int argc, char **argv)
{
    const char *frag = NULL;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strncmp(argv[argi], "--frag=", 7)) frag = argv[argi] + 7;
        else break;
    }

    css_init();
    css_viewport(WINW, VIEWH);
    css_set_post_pass(css_extra_apply);
    if (frag) css_set_target_fragment(frag, -1);

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
        char lbl[32];
        int dl = (int)strlen(dir);
        while (dl > 0 && (dir[dl-1] == '/' || dir[dl-1] == '\\')) dl--;
        int bs = dl;
        while (bs > 0 && dir[bs-1] != '/' && dir[bs-1] != '\\') bs--;
        snprintf(lbl, sizeof(lbl), "%.*s", dl - bs, dir + bs);
        census_one(lbl, html, sp, ns);
        for (int i = 0; i < ns; i++) free(sp[i]);
    }

    /* On stderr so the digest stream on stdout stays diffable. */
    for (int s = 0; s < 4; s++)
        fprintf(stderr, "MATCH\t%s\t%ld\n", k_probe[s], g_match[s]);
    return 0;
}
