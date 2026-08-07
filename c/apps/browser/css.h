#ifndef LOGIT_CSS_H
#define LOGIT_CSS_H

#include <stdint.h>
#include "dom.h"

enum { DISP_INLINE, DISP_BLOCK, DISP_INLINE_BLOCK, DISP_FLEX, DISP_NONE, DISP_GRID };
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY };

/* box-sizing: what the used `width`/`height` measure. CONTENT is the CSS
 * initial value; BORDER makes width include padding + borders. */
enum { BOX_CONTENT, BOX_BORDER };

/* white-space, decomposed the way layout actually branches on it: whether
 * runs of spaces collapse, whether a literal newline breaks the line, and
 * whether a full line may wrap. */
enum { WS_NORMAL, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE };

/* flexbox */
enum { FDIR_ROW, FDIR_ROW_REV, FDIR_COL, FDIR_COL_REV };
enum { FWRAP_NOWRAP, FWRAP_WRAP, FWRAP_WRAP_REV };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
/* align-items / align-self / align-content share this vocabulary. AL_AUTO is
 * only ever an align-self value ("use the container's align-items"). */
enum { AL_STRETCH, AL_START, AL_END, AL_CENTER, AL_BASELINE, AL_AUTO };

enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };
enum { FLT_NONE, FLT_LEFT, FLT_RIGHT };
enum { CLR_NONE, CLR_LEFT, CLR_RIGHT, CLR_BOTH };
enum { OVF_VISIBLE, OVF_HIDDEN, OVF_SCROLL, OVF_AUTO };

/* list-style-type: the marker alphabets we can actually draw with the fonts we
 * ship. Everything else LibCSS knows (armenian, hebrew, cjk-*, the Indic
 * families, ...) folds onto LST_DECIMAL -- a wrong-alphabet number still
 * numbers the list, whereas a missing glyph is a blank box. */
enum { LST_NONE, LST_DISC, LST_CIRCLE, LST_SQUARE, LST_DECIMAL, LST_DECIMAL_ZERO,
       LST_LOWER_ALPHA, LST_UPPER_ALPHA, LST_LOWER_ROMAN, LST_UPPER_ROMAN };

#define GRID_MAXCOL 24

/* Computed style for one node. Lengths are px unless a *_pct flag says percent.
 *
 * A length that came from calc() can be BOTH: `w_pct` set with `width` the
 * percentage and `w_off` a px addend, which is exactly the calc(100% - 20px)
 * shape. Layout resolves all of them through one helper, so a plain px length
 * is just the pct == 0 case.
 *
 * The enum-valued members are `unsigned char`: a cstyle is allocated per
 * element and a 10k-element page keeps all of them live at once, so the
 * property count added here would otherwise cost ~1.5 MB of the browser's
 * ring-3 arena for nothing. */
struct cstyle {
    int display;
    uint32_t color;                 /* 0xRRGGBB (text) */
    uint32_t background; int has_bg;
    int bg_alpha;                   /* background-color alpha 0..255 (255 = opaque).
                                     * Carried through to the display list; the
                                     * painter does not blend yet. */
    int font_px, bold, italic, mono;
    int mt, mr, mb, ml;             /* margins (px); ml/mr = -1 means auto */
    int pt, pr, pb, pl;             /* paddings (px) */
    int width, height; int has_w, has_h, w_pct, h_pct;
    int w_off, h_off;               /* px addend when w_pct/h_pct (calc) */
    int min_w, max_w, min_h, max_h;             /* min/max sizing */
    int has_min_w, has_max_w, has_min_h, has_max_h;
    int min_w_pct, max_w_pct, min_h_pct, max_h_pct;
    int text_align;
    int line_px;                    /* line height (px); 0 = derive from font */
    int border_w[4];                /* top, right, bottom, left (px); 0 = none */
    uint32_t border_color[4];       /* per edge, 0xRRGGBB */
    unsigned char border_style[4];  /* CSS_BORDER_STYLE_* (solid/dashed/...); the
                                     * painter draws every non-none style solid */
    int radius;                     /* border-radius px (via css_extra; LibCSS predates it) */
    int radius_pct;                 /* border-radius % (of min(w,h) at paint) */
    int underline, strike, overline;            /* text-decoration bits */
    int opacity;                    /* 0..255; 255 = fully opaque */
    int list_item;                  /* 1 for <li>-style markers */
    unsigned char list_style;       /* LST_* */
    int hidden;                     /* visibility:hidden/collapse or opacity:0 */
    int op0;                        /* hidden was (also) caused by opacity:0 */
    int vis_hid;                    /* hidden was caused by visibility:hidden/collapse */
    unsigned char position;         /* POS_* */
    int pos_abs;                    /* position:absolute -- out of flow (laid out nowhere) */
    int top, left, right, bottom;   /* box offsets from the containing block */
    int has_top, has_left, has_right, has_bottom;
    int z_index; int has_z;         /* z-index (paint order); has_z == 0 means auto */
    unsigned char box_sizing;       /* BOX_* */
    unsigned char white_space;      /* WS_* */
    unsigned char flt, clr;         /* float / clear (FLT_* / CLR_*) */
    unsigned char overflow_x, overflow_y;       /* OVF_* */
    /* flex container */
    unsigned char flex_dir, flex_wrap, justify, align_items, align_content;
    /* flex item */
    unsigned char align_self;
    int flex_grow;                  /* flex-grow as css_fixed (1.0 = 1024); 0 = don't grow */
    int flex_shrink;                /* flex-shrink as css_fixed; default 1.0 */
    int flex_basis, fb_pct, fb_off, has_fb;     /* flex-basis (auto/content = !has_fb) */
    int order;                      /* flex `order`, items sorted ascending (stable) */
    int grid_cols;                  /* >0: grid container with this many columns (css_extra) */
    int grid_tracks[GRID_MAXCOL];   /* per column: >0 = px width, <0 = fr weight (-v) */
    int grid_gap_x, grid_gap_y;     /* column-gap / row-gap (px); also used by flex */
    int anim;                       /* has a non-none animation/animation-name (css_extra);
                                     * opacity:0 + animation approximates its visible end
                                     * state, so css_extra clears `hidden` for these */
    int trans_op;                   /* transition declares opacity/all (css_extra); same
                                     * end-state approximation as anim (scroll-reveal) */
    int inherited_from_ua;          /* internal bookkeeping (unused by callers) */
};

void css_init(void);                            /* build the UA default stylesheet */
/* Set the real viewport size for @media evaluation + vw/vh units (css_init
 * defaults to 760x540 for host tests). */
void css_viewport(int w, int h);
/* Current viewport width in px (for css_extra's naive @media gating). */
int  css_media_width(void);
/* Post-pass for properties our LibCSS doesn't know (border-radius): scans the
 * author sheet's simple selectors + inline style= attrs and patches node->style
 * after css_apply. */
void css_extra_apply(struct node *root, const char *page_css, int page_len);
/* Compute and attach a `struct cstyle*` to every node of `root` (in node->style),
 * cascading UA defaults + `page_css` (page_len bytes from <style>) + inline style=. */
void css_apply(struct node *root, const char *page_css, int page_len);

/* Cumulative selection statistics: elements styled, and LibCSS node-data cache
 * hits (parent-bloom lookups plus sibling style-sharing probes that found a
 * live entry). A zero hit count means the node-data handlers are dead again --
 * which is what the host test asserts against. */
void css_stats(int *styled, int *cache_hits);

/* Resolve CSS custom properties: substitute var(--x[,fallback]) in `in` using
 * its --name:value declarations, writing the expanded sheet to `out`. Returns
 * the expanded length. (Our LibCSS predates native var() support.) */
int  css_expand_vars(const char *in, int inlen, char *out, int outmax);

#endif /* LOGIT_CSS_H */
