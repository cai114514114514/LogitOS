#ifndef AETHER_LAYOUT_H
#define AETHER_LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "img.h"

/* Layout produces a flat display list (painted in order; hit-tested back-to-front). */
enum { IT_RECT, IT_TEXT, IT_IMAGE };
struct item {
    int type, x, y, w, h;
    /* RECT */ uint32_t bg; int has_bg, border_w; uint32_t border_color; int radius;
    /* TEXT */ const char *text; int len, font_px, bold, italic, mono, underline; uint32_t color;
    /* IMAGE */ struct image *img; const char *imgsrc;  /* decoded image + its URL */
    const char *href;                 /* link target for this item (or NULL) */
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

#endif /* AETHER_LAYOUT_H */
