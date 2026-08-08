/* refrender -- turn one file on disk into one picture, through the real
 * browser pipeline. See refhost/logit.h for the shared/not-shared statement.
 *
 * The whole harness rests on this being the SAME call sequence browser.c makes.
 * c/apps/browser/browser.c:982 does, in order:
 *     collect <style> text  ->  css_expand_vars  ->  css_apply
 *     ->  layout_page(root, win_w)  ->  browser_paint(...)
 * and then, once external stylesheets arrive, css_apply + layout_page + paint
 * again. rr_render below is that sequence with the network replaced by the
 * filesystem -- the sheets are already on disk, so the second pass is not
 * deferred, it just happens before the first paint. */
#ifndef REFRENDER_H
#define REFRENDER_H

#include <stdint.h>

/* Why 800x600: it is the WPT reference viewport. Reftests are authored against
 * it, and a page laid out at any other width produces a picture that is not
 * wrong so much as unrelated to what the reference expects. */
#define RR_VIEW_W 800
#define RR_VIEW_H 600

struct rr_stats {
    int nodes;          /* DOM nodes parsed */
    int css_bytes;      /* author CSS handed to css_apply */
    int sheets;         /* external <link rel=stylesheet> loaded */
    int sheets_missing; /* ... and referenced but not on disk */
    int items;          /* display-list length after layout */
    int doc_h;          /* layout_height() */
};

/* Render `rel` (root-relative) into a freshly allocated w*h buffer of
 * 0x00RRGGBB. Returns the pixels (owned by refhost, valid until the next
 * rr_render) or NULL if the file could not be read. `st` may be NULL.
 *
 * `nocss` is the second negative control: parse and lay out with the author
 * stylesheet withheld entirely. Nearly every reftest must fail under it -- a
 * suite that does not notice is not measuring CSS. */
uint32_t *rr_render(const char *root, const char *rel, int w, int h,
                    int nocss, struct rr_stats *st);

/* One-time setup: the UA stylesheet and the viewport LibCSS answers @media
 * with. Idempotent. */
void rr_init(int w, int h);

#endif /* REFRENDER_H */
