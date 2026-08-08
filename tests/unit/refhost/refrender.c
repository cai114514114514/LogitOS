/* refrender -- one file on disk -> one picture, through the real pipeline.
 * See refrender.h for the call sequence this mirrors and why. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit.h"            /* the rasterizing shim, via -Itests/unit/refhost */
#include "refhost.h"
#include "refrender.h"
#include "refmanifest.h"      /* rm_resolve */
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "browser_paint.h"

/* ------------------------------------------------------------ resources -- */
/* The page being rendered, so res_fetch can resolve a relative URL. layout.c
 * calls res_fetch for images; on the machine that is SYS_RES_FETCH into the
 * kernel's HTTP cache. Here the "network" is the WPT checkout. */
static char cur_root[RM_PATHMAX], cur_page[RM_PATHMAX];

static uint8_t *slurp(const char *root, const char *rel, int *len)
{
    char full[RM_PATHMAX * 2];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    FILE *f = fopen(full, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 16 * 1024 * 1024) { fclose(f); return 0; }
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return 0; }
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = 0; fclose(f);
    if (len) *len = (int)n;
    return b;
}

int res_fetch(const char *url, uint8_t **buf, int *len)
{
    char rel[RM_PATHMAX];
    if (rm_resolve(cur_page, url, rel, sizeof rel) != 0) return -1;
    uint8_t *b = slurp(cur_root, rel, len);
    if (!b) return -1;
    *buf = b;
    return 0;
}

/* --------------------------------------------------------- the CSS input -- */
/* browser.c's collect_style is static, so this is a reimplementation and one of
 * the few pieces of harness that is not the shipping code. It is the whole of
 * it: concatenate the text of every <style> element, in document order. */
static int collect_style(struct node *n, char *out, int o, int max)
{
    if (n->type == N_ELEM && !strcmp(n->tag, "style")) {
        for (struct node *c = n->first_child; c; c = c->next) {
            if (c->type != N_TEXT || !c->text) continue;
            int n2 = c->textlen;
            if (o + n2 >= max) n2 = max - o - 1;
            if (n2 > 0) { memcpy(out + o, c->text, (size_t)n2); o += n2; }
        }
        if (o < max - 1) out[o++] = '\n';
        return o;
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_style(c, out, o, max);
    return o;
}

/* case-insensitive substring, the same test browser.c:554 uses on `rel` */
static int has_ci(const char *hay, const char *needle)
{
    if (!hay) return 0;
    size_t m = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < m && p[k] && (p[k] | 32) == (needle[k] | 32)) k++;
        if (k == m) return 1;
    }
    return 0;
}

/* Load every <link rel=stylesheet href> off the disk and append it. Mirrors
 * browser.c's discovery loop; the fetch is a file read instead of hpool. */
static int collect_links(struct node *n, char *out, int o, int max, int *nsheet, int *nmiss)
{
    if (n->type == N_ELEM && !strcmp(n->tag, "link")) {
        const char *rel = dom_attr(n, "rel"), *href = dom_attr(n, "href");
        if (href && has_ci(rel, "stylesheet") && !has_ci(href, "data:")) {
            char path[RM_PATHMAX];
            if (rm_resolve(cur_page, href, path, sizeof path) == 0) {
                int len = 0;
                uint8_t *b = slurp(cur_root, path, &len);
                if (b) {
                    (*nsheet)++;
                    if (o + len >= max) len = max - o - 1;
                    if (len > 0) { memcpy(out + o, b, (size_t)len); o += len; }
                    if (o < max - 1) out[o++] = '\n';
                    free(b);
                } else (*nmiss)++;
            } else (*nmiss)++;
        }
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_links(c, out, o, max, nsheet, nmiss);
    return o;
}

static int count_nodes(struct node *n)
{
    int k = 1;
    for (struct node *c = n->first_child; c; c = c->next) k += count_nodes(c);
    return k;
}

/* ------------------------------------------------------------- the drive -- */
static int inited;
void rr_init(int w, int h)
{
    if (inited) return;
    css_init();
    css_viewport(w, h);
    inited = 1;
}

/* Sized for the corpus: the largest concatenated stylesheet in css/ is well
 * under 256 KiB, and a page that overflows is truncated rather than dropped --
 * with the overflow counted, because silently rendering half a stylesheet is
 * the sort of thing that makes a rate drift without anyone knowing why. */
#define RR_CSSMAX (512 * 1024)
static char author_css[RR_CSSMAX];
static char css_expanded[RR_CSSMAX];

uint32_t *rr_render(const char *root, const char *rel, int w, int h,
                    int nocss, struct rr_stats *st)
{
    struct rr_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof *st);

    snprintf(cur_root, sizeof cur_root, "%s", root);
    snprintf(cur_page, sizeof cur_page, "%s", rel);

    int srclen = 0;
    uint8_t *src = slurp(root, rel, &srclen);
    if (!src) return 0;

    rr_init(w, h);

    struct node *doc = dom_parse((const char *)src, srclen);
    free(src);
    if (!doc) return 0;
    st->nodes = count_nodes(doc);

    int clen = 0;
    if (!nocss) {
        clen = collect_style(doc, author_css, 0, RR_CSSMAX);
        clen = collect_links(doc, author_css, clen, RR_CSSMAX,
                             &st->sheets, &st->sheets_missing);
        author_css[clen] = 0;
        /* browser.c expands custom properties before the cascade, because
         * LibCSS resolves var() at parse time and would drop the declaration. */
        int xl = css_expand_vars(author_css, clen, css_expanded, RR_CSSMAX);
        if (xl > 0) { clen = xl; }
        else { memcpy(css_expanded, author_css, (size_t)clen); }
    }
    st->css_bytes = clen;

    css_apply(doc, nocss ? "" : css_expanded, clen);
    layout_page(doc, w);
    st->items = layout_count();
    st->doc_h = layout_height();

    /* The canvas starts white. A reftest's viewport is white unless the page
     * paints it, and "nothing painted" must not read as black -- half the
     * corpus would then match a blank reference for the wrong reason. */
    uint32_t bg = 0xFFFFFF;
    layout_page_bg(&bg);
    refhost_begin(w, h, bg);
    browser_paint(0, 0, w, h, 0);
    uint32_t *px = refhost_end();

    layout_free();
    return px;
}
