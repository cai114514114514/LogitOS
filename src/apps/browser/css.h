#ifndef AQUA_CSS_H
#define AQUA_CSS_H

#include <stdint.h>
#include "dom.h"

enum { DISP_INLINE, DISP_BLOCK, DISP_INLINE_BLOCK, DISP_FLEX, DISP_NONE };
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* Computed style for one node. Lengths are px unless a *_pct flag says percent. */
struct cstyle {
    int display;
    uint32_t color;                 /* 0xRRGGBB (text) */
    uint32_t background; int has_bg;
    int font_px, bold, italic, mono;
    int mt, mr, mb, ml;             /* margins (px); ml/mr = -1 means auto */
    int pt, pr, pb, pl;             /* paddings (px) */
    int width, height; int has_w, has_h, w_pct, h_pct;
    int text_align;
    int line_px;                    /* line height (px); 0 = derive from font */
    int border_w; uint32_t border_color;
    int radius;
    int underline;
    int list_item;                  /* 1 for <li>-style markers */
    int inherited_from_ua;          /* internal bookkeeping (unused by callers) */
};

void css_init(void);                            /* build the UA default stylesheet */
/* Compute and attach a `struct cstyle*` to every node of `root` (in node->style),
 * cascading UA defaults + `page_css` (page_len bytes from <style>) + inline style=. */
void css_apply(struct node *root, const char *page_css, int page_len);

/* Resolve CSS custom properties: substitute var(--x[,fallback]) in `in` using
 * its --name:value declarations, writing the expanded sheet to `out`. Returns
 * the expanded length. (Our LibCSS predates native var() support.) */
int  css_expand_vars(const char *in, int inlen, char *out, int outmax);

#endif /* AQUA_CSS_H */
