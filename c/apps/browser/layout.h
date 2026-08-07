#ifndef LOGIT_LAYOUT_H
#define LOGIT_LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "img.h"

/* Layout produces a flat display list (painted in order; hit-tested back-to-front). */
enum { IT_RECT, IT_TEXT, IT_IMAGE };
struct item {
    int type, x, y, w, h;
    struct node *node;                /* DOM node this box came from (NULL only
                                       * if layout ran out of context). Lets a
                                       * hit test / inspector / future event
                                       * dispatch go from a painted box back to
                                       * the element without a second search. */
    int z;                            /* stacking level: the z-index of the
                                       * nearest positioned ancestor that set
                                       * one, 0 otherwise. layout_page stable-
                                       * sorts the list by it, so both the
                                       * forward paint and the backward hit test
                                       * see the right box on top. */
    /* RECT */ uint32_t bg; int has_bg; int bg_alpha; int border_w[4]; uint32_t border_color[4];
    unsigned char border_style[4];    /* CSS_BORDER_STYLE_*; the painter draws
                                       * every non-none style solid */
    int radius, radius_pct;
    /* TEXT */ const char *text; int len, font_px, bold, italic, mono, underline; uint32_t color;
    int strike, overline;             /* the other two text-decoration lines */
    char marker[16];                    /* backing store for <li> markers (roman
                                       * numerals are the long case) */
    /* IMAGE */ struct image *img; const char *imgsrc;  /* decoded image + its URL */
    int h_auto;                       /* img height was derived (no explicit px): may be
                                       * corrected from the decoded image's aspect ratio */
    const char *href;                 /* link target for this item (or NULL) */
    int hidden;                       /* visibility:hidden / opacity:0 -- layout space kept, nothing painted */
    int opacity;                      /* 0..255 from the element's opacity */
};

/* Lay out `root` (a parsed+styled DOM) into a display list at the given canvas
 * width; fetches/decodes <img>. */
void  layout_page(struct node *root, int canvas_w);
/* Fetch+decode up to `max` of the page's <img> resources (bounded, blocking);
 * call after layout_page. Returns the number of images successfully loaded. */
int   layout_load_images(int max);
int   layout_height(void);
/* Page (canvas) background propagated from <html>/<body>; 1 if set, fills *out. */
int   layout_page_bg(uint32_t *out);
int   layout_count(void);
const struct item *layout_items(void);
void  layout_free(void);

#endif /* LOGIT_LAYOUT_H */
