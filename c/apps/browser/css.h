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
                                     * Blended by the painter, combined with the
                                     * element's opacity. */
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
    unsigned char border_style[4];  /* CSS_BORDER_STYLE_* per edge. The painter
                                     * draws solid/dotted/dashed/double and the
                                     * four bevelled styles; double degrades to
                                     * solid below 3px, where it cannot show. */
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

/* ---------------- CSSOM: the property surface ----------------
 *
 * The named properties `getComputedStyle()` can answer and `element.style`'s
 * camelCase accessors expose. This is a TABLE, not the full CSS property set:
 * element.style can still carry any property at all (it is stored verbatim in
 * the style attribute and handed to LibCSS as an inline sheet, so `transform`
 * or a custom property round-trips through setProperty/getPropertyValue
 * untouched) -- the enum only names the ones we can also RESOLVE, i.e. read
 * back out of a computed style.
 *
 * Edges are always in CSS shorthand order -- top, right, bottom, left -- so the
 * four-per-property groups can be indexed arithmetically. */
enum {
    CSSP_WIDTH = 0, CSSP_HEIGHT,
    CSSP_MIN_WIDTH, CSSP_MAX_WIDTH, CSSP_MIN_HEIGHT, CSSP_MAX_HEIGHT,
    CSSP_MARGIN_TOP, CSSP_MARGIN_RIGHT, CSSP_MARGIN_BOTTOM, CSSP_MARGIN_LEFT,
    CSSP_PADDING_TOP, CSSP_PADDING_RIGHT, CSSP_PADDING_BOTTOM, CSSP_PADDING_LEFT,
    CSSP_COLOR, CSSP_BACKGROUND_COLOR,
    CSSP_FONT_SIZE, CSSP_FONT_FAMILY, CSSP_FONT_WEIGHT, CSSP_FONT_STYLE,
    CSSP_DISPLAY, CSSP_POSITION,
    CSSP_TOP, CSSP_RIGHT, CSSP_BOTTOM, CSSP_LEFT,
    CSSP_OPACITY, CSSP_Z_INDEX, CSSP_VISIBILITY,
    CSSP_OVERFLOW, CSSP_OVERFLOW_X, CSSP_OVERFLOW_Y,
    CSSP_FLEX_DIRECTION, CSSP_FLEX_WRAP, CSSP_FLEX_GROW, CSSP_FLEX_SHRINK,
    CSSP_FLEX_BASIS, CSSP_JUSTIFY_CONTENT, CSSP_ALIGN_ITEMS, CSSP_ALIGN_SELF,
    CSSP_ALIGN_CONTENT, CSSP_ORDER,
    CSSP_BORDER_TOP_WIDTH, CSSP_BORDER_RIGHT_WIDTH,
    CSSP_BORDER_BOTTOM_WIDTH, CSSP_BORDER_LEFT_WIDTH,
    CSSP_BORDER_TOP_STYLE, CSSP_BORDER_RIGHT_STYLE,
    CSSP_BORDER_BOTTOM_STYLE, CSSP_BORDER_LEFT_STYLE,
    CSSP_BORDER_TOP_COLOR, CSSP_BORDER_RIGHT_COLOR,
    CSSP_BORDER_BOTTOM_COLOR, CSSP_BORDER_LEFT_COLOR,
    CSSP_TEXT_ALIGN, CSSP_LINE_HEIGHT, CSSP_TEXT_DECORATION,
    CSSP_BOX_SIZING, CSSP_WHITE_SPACE, CSSP_FLOAT, CSSP_CLEAR,
    CSSP_LIST_STYLE_TYPE,
    CSSP__COUNT
};

/* Dashed CSS name of property `i` ("background-color"), or NULL out of range. */
const char *css_prop_name(int i);
/* Property id for a name given either dashed ("background-color") or in the
 * IDL's camelCase spelling ("backgroundColor"); -1 if we cannot resolve it.
 * `len` < 0 means NUL-terminated. */
int  css_prop_lookup(const char *name, int len);
/* 1 if changing this property can only change what gets PAINTED -- colour,
 * visibility, opacity, decoration, stacking order -- and never the geometry of
 * any box. The invalidation tiering in js_dom.c uses it to decide whether a
 * write to element.style needs to be able to move boxes. */
int  css_prop_paint_only(int prop);

/* Serialise property `prop` of `n`'s COMPUTED style (node->computed, the
 * LibCSS css_computed_style css_apply left behind) into `out` as CSS syntax:
 * "16px", "rgb(255, 0, 0)", "block". Returns the string length, or 0 if the
 * node has no computed style (never styled, or not an element).
 *
 * RESOLVED vs COMPUTED, and where they differ -- this matters if you compare
 * against a real browser:
 *
 *   - CSS's "resolved value" is the USED value for width/height, the four
 *     margins/paddings and the four box offsets: a real getComputedStyle
 *     reports `width:auto` on a block as the pixel width it ended up with.
 *     Used values live in the display list (layout.c), not in the cascade, so
 *     what comes back here is the COMPUTED value -- "auto" stays "auto" and a
 *     percentage stays a percentage. That is exactly what a real browser
 *     returns for a `display:none` element, and it is the honest answer for an
 *     engine whose layout is not queryable per node.
 *   - Everything else in the table (colour, font, display, position, opacity,
 *     z-index, overflow, the flex properties, border widths/styles/colours,
 *     text-align, line-height, visibility) has resolved value == computed
 *     value, so those match a real browser.
 *   - line-height:normal reports "normal" (Firefox reports a px number here,
 *     Chrome reports "normal"); the number depends on font metrics layout owns.
 *   - Lengths are whole pixels: the whole engine is integer-px, so there is no
 *     fractional value to report.
 */
int  css_computed_text(struct node *n, int prop, char *out, int outmax);

/* CSS.supports(): 1 if `prop: value` is a declaration THIS engine's cascade
 * would take. Answered by handing it to LibCSS's own value handlers, not by a
 * name table -- see the comment above css_supports_decl() in css_engine.c for
 * why that distinction is the whole point, and for the one direction in which
 * it deliberately under-reports. `plen`/`vlen` < 0 mean NUL-terminated. */
int  css_supports_decl(const char *prop, int plen, const char *value, int vlen);

void css_init(void);                            /* build the UA default stylesheet */
/* Set the real viewport size for @media evaluation + vw/vh units (css_init
 * defaults to 760x540 for host tests). */
void css_viewport(int w, int h);
/* Current viewport width in px. */
int  css_media_width(void);
/* Current viewport height in px. Together with css_media_width and
 * css_color_scheme these are every input an @media verdict depends on, which
 * is what lets css_extra.c cache a compiled sheet and know when to throw it
 * away. */
int  css_media_height(void);

/* ---------------- the ONE media-query evaluator ----------------
 *
 * 1 if `query` ("(prefers-color-scheme: dark)", "screen and (min-width:64rem)",
 * "" = all) holds for the viewport and preferences this document is being
 * rendered with. `len` < 0 means NUL-terminated.
 *
 * This is LibCSS's own parser and matcher, reached through the
 * css_select_ctx_media_matches patch -- byte for byte the test that decides
 * whether an @media block's rules take part in the cascade. Anything else in
 * the browser that has to answer the same question calls THIS; it must not
 * grow a scanner of its own.
 *
 * That is not tidiness. A second evaluator does not fail by being approximate,
 * it fails by DISAGREEING: css_vars.c used to collect custom properties with a
 * flat last-wins text scan that could not see @media at all, so wikipedia's
 * `@media (prefers-color-scheme: dark) { :root { --background-color-base:
 * #101418 } }` overwrote the light value for every reader, and the whole page
 * rendered dark while LibCSS -- correctly -- had declined that very block.
 * Three answers to one question is two too many.
 *
 * KNOWN REMAINING DIVERGENCE, and it is not mine to close: js_webapi.c ships a
 * matchMedia() with its own evaluator. It should call this. */
int  css_media_matches(const char *query, int len);

/* The colour scheme @media (prefers-color-scheme: ...) is evaluated against.
 * Default light; there is no user setting yet, and "light" is what a UA with no
 * preference reports. Set BEFORE css_apply -- it changes which rules cascade. */
void css_set_color_scheme(int dark);
int  css_color_scheme(void);
/* Post-pass for properties our LibCSS doesn't know (border-radius): scans the
 * author sheet's simple selectors + inline style= attrs and patches node->style
 * after css_apply. */
void css_extra_apply(struct node *root, const char *page_css, int page_len);
/* Test seam: rules in the last COMPILED sheet (see css_extra.c), or -1 if the
 * compile fell back to scanning the text. */
int  css_extra_rules(void);
/* Test seams for the two "do this once per sheet, not once per mutation"
 * caches: how many times the author stylesheet has been handed to LibCSS to
 * PARSE, and how many times css_extra has COMPILED it. Both are cumulative for
 * the life of the process.
 *
 * These exist because the alternative way to test a cache is to time it, and a
 * timing assertion on a shared machine is a flaky test rather than a strict
 * one. A counter says exactly what the optimisation claims -- that a re-style
 * over an unchanged sheet does no sheet work -- and it says it deterministically.
 * They are equally the guard on the dangerous direction: if a CHANGED sheet did
 * not bump these, the engine would be rendering the previous page's CSS. */
int  css_sheet_parses(void);
int  css_extra_compiles(void);
/* Compute and attach a `struct cstyle*` to every node of `root` (in node->style),
 * cascading UA defaults + `page_css` (page_len bytes from <style>) + inline style=. */
void css_apply(struct node *root, const char *page_css, int page_len);

/* What a re-style actually moved, as a bitmask. CSS_CHANGED_NONE is the answer
 * that pays for this whole mechanism: a class toggle whose class carries no
 * matching rule changes nothing, and the caller can then skip layout AND the
 * repaint instead of rebuilding the page to produce identical pixels. */
enum { CSS_CHANGED_NONE = 0, CSS_CHANGED_PAINT = 1, CSS_CHANGED_LAYOUT = 2 };

/* Re-run the cascade over JUST `n` and its subtree instead of the whole
 * document, resuming from the parent's already-computed style.
 *
 * `siblings` additionally re-styles every FOLLOWING element sibling and its
 * subtree. That is not belt and braces: `a + b` and `a ~ b` mean an attribute
 * or class change on `n` can change the style of elements after it, and
 * nothing else in this design would notice. Preceding siblings cannot be
 * affected (CSS has no "previous sibling" combinator), and ancestors cannot
 * either (we have no `:has()`), which is what makes the scope this narrow and
 * still correct.
 *
 * Returns a CSS_CHANGED_* mask over every node it re-styled: the old cstyle is
 * compared field-for-field against the new one, so "the cascade ran again and
 * produced the same answer" is reported as CSS_CHANGED_NONE.
 *
 * Requires that `n`'s ancestors are already styled (node->computed set) --
 * i.e. that a full css_apply has run for this sheet set at least once. */
int css_apply_scoped(struct node *n, int siblings, const char *page_css, int page_len);

/* Register the post-cascade pass that runs INSIDE css_apply_scoped's measured
 * window, over the same scope. The browser registers css_extra_apply here.
 *
 * It has to be inside: css_extra patches node->style AFTER the cascade (border
 * radius, grid tracks, the animation end-state approximation), so a change
 * diff taken before it ran would see every one of those patches as a fresh
 * change on every single re-style and css_apply_scoped could never answer
 * CSS_CHANGED_NONE. It is a hook rather than a direct call because css_extra.c
 * is not linked into the CSS host tests, and css_engine.c must keep building
 * without it. */
void css_set_post_pass(void (*fn)(struct node *root, const char *css, int len));

/* Cumulative selection statistics: elements styled, and LibCSS node-data cache
 * hits (parent-bloom lookups plus sibling style-sharing probes that found a
 * live entry). A zero hit count means the node-data handlers are dead again --
 * which is what the host test asserts against. */
void css_stats(int *styled, int *cache_hits);

/* Resolve CSS custom properties: substitute var(--x[,fallback]) in `in` using
 * its --name:value declarations, writing the expanded sheet to `out`. Returns
 * the expanded length. (Our LibCSS predates native var() support.) */
int  css_expand_vars(const char *in, int inlen, char *out, int outmax);

/* Test seam into the last css_expand_vars: how many distinct custom properties
 * it collected, and the value the CASCADE chose for one of them (`name` with or
 * without the leading `--`; NULL if it was never declared, or was declared only
 * inside an @media block that does not hold). Asserting on this is how a test
 * distinguishes "the right value won" from "some value was substituted". */
int  css_vars_count(void);
const char *css_vars_value(const char *name);

#endif /* LOGIT_CSS_H */
