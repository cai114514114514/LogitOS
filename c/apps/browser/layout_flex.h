#ifndef LOGIT_LAYOUT_FLEX_H
#define LOGIT_LAYOUT_FLEX_H

#include "css.h"

/* CSS Flexible Box Layout -- the sizing and positioning algorithm, on its own.
 *
 * WHAT THIS FILE IS. `layout_flex_run()` is CSS Flexbox § 9 (the numbered
 * "Flex Layout Algorithm"), implemented as a pure function over numbers. It
 * touches no DOM, no display list, no font backend and no canvas. Its input is
 * the container's computed style, one record per item, and the available
 * space; its output is, per item, a used main size, a used cross size and a
 * position -- in both logical (main/cross) and physical (x/y/w/h) form.
 *
 * WHY IT IS NOT IN layout.c. Two reasons, one practical and one that outlives
 * the practical one:
 *
 *   - layout.c already contains an APPROXIMATION of flexbox (search for
 *     `layout_flex` there). Its own comment lists the liberties it takes: main
 *     sizes resolved in ONE pass rather than the spec's freeze-and-redistribute
 *     loop, no baseline alignment, no align-content, no wrapping in a column,
 *     no absolutely-positioned children. Those are not bugs to be patched one
 *     at a time -- the single-pass resolution is a different algorithm, and the
 *     spec's loop is the whole content of § 9.7.
 *   - A function with no DOM in it can be tested to the exact number the spec
 *     names, with no renderer and no reference image. tests/unit/flex_test.c
 *     does exactly that, and it stays the right test after a reftest harness
 *     arrives: a reftest asks whether the numbers reach the screen, which is a
 *     different question from whether they are right.
 *
 * UNITS AND FRAMES OF REFERENCE, stated once because every field depends on it:
 *
 *   - All sizes are whole pixels. The engine is integer-px throughout; the
 *     resolution loop runs in 16.16 fixed point internally and rounds once, at
 *     the end, with a largest-remainder pass so a line's items still sum to the
 *     line (see round_line() in the .c).
 *   - `main_size` / `cross_size` in the OUTPUT are CONTENT-box (inner) sizes.
 *     That is what the spec resolves and what a caller must pass back down to
 *     the item's own layout.
 *   - `main_pos` / `cross_pos` / `x` / `y` / `w` / `h` in the output are the
 *     item's BORDER box, measured from the flex container's CONTENT-box origin.
 *     Logical positions are measured from the main-start / cross-start edges,
 *     so they are unaffected by row-reverse / wrap-reverse; the physical x/y
 *     are, which is the point of having both.
 *   - Sizes supplied by the caller (`min_content_main`, `max_content_main`, and
 *     the `cross` callback's return) are CONTENT-box sizes of the item.
 *
 * WHAT THE CALLER STILL OWNS: generating anonymous flex items out of runs of
 * inline content (§ 4), and actually laying each item out at the size this
 * returns. This function decides sizes and positions; it does not draw.
 */

/* An available space that is not a definite length. Distinct from any real
 * size, and from -1, which several callers use for "unset". */
#define FLEX_INDEFINITE (-0x40000000)

/* Writing mode of the flex container. Which physical axis is "inline" and
 * which is "block" is decided here; flex-direction then picks one of them as
 * the main axis (row -> inline, column -> block). `struct cstyle` carries no
 * writing-mode or direction field today, so a caller that has not got one
 * passes FLEX_WM_HORIZ_TB and rtl = 0 and gets the same answer it would have
 * got without this parameter existing. */
enum { FLEX_WM_HORIZ_TB, FLEX_WM_VERT_RL, FLEX_WM_VERT_LR };

/* Which `flex-basis` an item has, when `struct cstyle` cannot say.
 *
 * cstyle has `has_fb` (a length was specified) and nothing else, so `auto` and
 * `content` -- which behave DIFFERENTLY, auto deferring to the main size
 * property and content ignoring it -- are the same value there.
 *
 *   FLEX_FB_FROM_STYLE  derive it: a specified length if has_fb, else `auto`.
 *                       The default, and correct for everything css_engine.c
 *                       can currently produce.
 *   FLEX_FB_AUTO        the computed basis IS `auto` (no length).
 *   FLEX_FB_CONTENT     the computed basis IS `content` (no length). */
enum { FLEX_FB_FROM_STYLE, FLEX_FB_AUTO, FLEX_FB_CONTENT };

/* One candidate flex item. */
struct flex_item_in {
    void *ref;                  /* opaque caller handle; echoed in the output */
    const struct cstyle *st;    /* the item's computed style. NULL is legal and
                                 * means "every property at its initial value",
                                 * which is what an anonymous item generated
                                 * from a run of text has. */

    /* Intrinsic contributions in the container's MAIN axis, content-box px.
     * These do not depend on flexing, so they are values rather than
     * callbacks. `max_content_main` is the flex base size of an item whose
     * used flex basis is `content`; `min_content_main` is the content size
     * suggestion the automatic minimum size (§ 4.5) is built out of. */
    int min_content_main;
    int max_content_main;

    /* Preferred aspect ratio as width:height, or 0/0 for none. Used for the
     * transferred size suggestion and for a `content` basis with a definite
     * cross size. */
    int ratio_w, ratio_h;

    unsigned char basis;        /* FLEX_FB_* */
    unsigned char replaced;     /* a replaced element (img/video/canvas): its
                                 * automatic minimum size takes the SMALLER of
                                 * the content and transferred suggestions, a
                                 * non-replaced element the larger (§ 4.5) */
    unsigned char abspos;       /* position:absolute/fixed. Not a flex item at
                                 * all; the algorithm skips it and reports only
                                 * its static position (§ 4.1). */
};

/* Measurement the algorithm cannot do for itself, because it needs real layout.
 *
 * Both are called with sizes the algorithm has already resolved, which is why
 * they are callbacks and not fields: an item's cross size in a row container is
 * its HEIGHT, and its height is not known until its width is. */
struct flex_metrics {
    /* Content-box cross size of `ref`, laid out with the given definite
     * content-box main size. Never called for an item whose cross size the
     * style already fixes. */
    int (*cross)(void *ref, int main_inner, void *ctx);
    /* Distance from the item's BORDER-box cross-start edge to its first
     * baseline, given its used inner main and cross sizes. Negative means the
     * item has no baseline, in which case baseline alignment falls back to
     * flex-start as the spec requires. May be NULL (same effect). */
    int (*baseline)(void *ref, int main_inner, int cross_inner, void *ctx);
    void *ctx;
};

/* The flex container. */
struct flex_in {
    const struct cstyle *st;    /* container's computed style; NULL = initial */
    int avail_main;             /* inner main size, or FLEX_INDEFINITE */
    int avail_cross;            /* inner cross size, or FLEX_INDEFINITE */
    unsigned char wm;           /* FLEX_WM_* */
    unsigned char rtl;          /* inline direction is right-to-left */
    /* align-content, which `struct cstyle` cannot express. css_engine.c folds
     * space-between/space-around/space-evenly onto AL_STRETCH, so a container
     * that uses them is indistinguishable there. Set this to a JC_* value to
     * say which one it really was; -1 (the default) means "use st->align_content".
     * See the note in the report: the fix belongs in cstyle, not here. */
    int align_content_space;
};

/* One item's resolved geometry. */
struct flex_item_out {
    void *ref;
    int idx;                    /* index into the caller's input array */
    int line;                   /* flex line number, or -1 for an abspos child */
    int main_size, cross_size;  /* USED content-box sizes */
    int main_pos, cross_pos;    /* BORDER-box start edges, logical, measured
                                 * from the container's content-box main-start
                                 * and cross-start corners */
    int main_outer, cross_outer;/* BORDER-box extents (main_size + padding +
                                 * border, etc.) -- what x/y/w/h are built from */
    int baseline;               /* used baseline offset from the border-box
                                 * cross-start edge, or -1 */
    int x, y, w, h;             /* PHYSICAL border box from the container's
                                 * content-box top-left */
};

struct flex_out {
    struct flex_item_out *items;/* one per input item, in ORDER-MODIFIED order
                                 * (`order` applied, stably); abspos children
                                 * come last */
    int nitems;
    int *line_cross;            /* used cross size of each flex line */
    int *line_pos;              /* each line's cross-start edge, from the
                                 * container's content-box cross-start */
    int nlines;
    int main_size, cross_size;  /* the container's used INNER sizes */
    void *blk;                  /* the single allocation everything above
                                 * points into; layout_flex_free() releases it */
};

/* Run § 9 over `n` items. Returns 0 on success, -1 if it could not allocate.
 * On success the caller must eventually call layout_flex_free(out). */
int  layout_flex_run(const struct flex_in *c, const struct flex_item_in *in, int n,
                     const struct flex_metrics *m, struct flex_out *out);
void layout_flex_free(struct flex_out *out);

#endif /* LOGIT_LAYOUT_FLEX_H */
