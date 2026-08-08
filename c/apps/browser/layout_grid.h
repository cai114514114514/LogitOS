#ifndef LOGIT_LAYOUT_GRID_H
#define LOGIT_LAYOUT_GRID_H

/* CSS Grid Layout -- placement, track sizing and alignment, as a self-contained
 * module.
 *
 * WHY THIS IS NOT IN layout.c, AND WHAT THE BOUNDARY BUYS
 * ------------------------------------------------------
 * Grid is specified as an ALGORITHM WITH EXACT NUMERIC OUTCOMES: the track
 * sizing algorithm's steps produce specific base sizes and growth limits,
 * placement produces specific line indices, free-space distribution produces
 * specific pixel values. "Given this template, these items and this width, the
 * columns are [100, 250, 50]" is a complete, checkable assertion that needs no
 * renderer, no DOM and no font. So this module deliberately knows about NONE of
 * those: it takes parsed style values, a list of items, a callback that
 * measures an item, and the available space; it returns track sizes and each
 * item's rectangle. That is what makes tests/unit/grid_test.c able to be a
 * spec conformance suite rather than a smoke test.
 *
 * It also means integration is small: layout.c fills in a `struct gridcfg` plus
 * a `struct griditem` per child, supplies a measure callback that runs its own
 * intrinsic sizing, and reads rectangles back out. Nothing here reaches into
 * the layout engine.
 *
 * UNITS. Whole pixels everywhere, because the whole engine is integer-px.
 * Two exceptions, both internal to the value representation:
 *   - percentages are stored in HUNDREDTHS OF A PERCENT (50% -> 5000), so
 *     33.33% survives parsing;
 *   - flex factors are stored in MILLI-FR (1fr -> 1000), so 0.5fr and the
 *     flex-factor-sum < 1 case are representable.
 * Track sizes and positions handed back are plain px.
 *
 * ROUNDING. Where the spec divides space evenly and the division is not exact,
 * this module hands the remainder pixels out ONE EACH to the earliest
 * recipients in track order, so the parts always sum to the whole. A real
 * browser keeps subpixel track sizes and rounds at paint; we cannot, and
 * silently losing a pixel per track is worse than a documented bias. Every
 * place this happens is marked REMAINDER in layout_grid.c.
 *
 * WHAT IS NOT HERE. See the bottom of layout_grid.c for the honest list.
 */

#include <stddef.h>

/* An indefinite (unknown / content-dependent) size. Used for available space,
 * for min/max constraints that were not specified, and for `auto` preferred
 * sizes. Chosen to be recognisable in a debugger and never a legal px value. */
#define GRID_INDEFINITE  (-1073741824)
/* An infinite growth limit. int64 internally; this is the API-visible spelling. */
#define GRID_INF         (0x3fffffff)

/* Overlarge-grid clamp (css-grid-1 s5.5): an author can write
 * `grid-row: 1 / 1000000` and a UA is explicitly allowed to clamp rather than
 * allocate a million tracks. */
#define GRID_MAX_TRACKS  4096

/* ---------------------------------------------------------------- values -- */

/* One <track-breadth>. */
enum {
    GSF_PX = 0,        /* <length>, v = px */
    GSF_PCT,           /* <percentage>, v = hundredths of a percent */
    GSF_AUTO,          /* auto */
    GSF_MIN_CONTENT,   /* min-content */
    GSF_MAX_CONTENT,   /* max-content */
    GSF_FR             /* <flex>, v = milli-fr */
};
struct gsize { unsigned char kind; int v; };

/* One track's sizing function, already decomposed into the min and max track
 * sizing functions the algorithm actually consumes (css-grid-1 s11.2):
 *   minmax(a, b)     -> mn = a,    mx = b
 *   fit-content(x)   -> mn = auto, mx = max-content, fc = x, is_fc = 1
 *   <flex>           -> mn = auto, mx = <flex>
 *   anything else s  -> mn = s,    mx = s
 * Decomposing at parse time is what keeps the sizing code from re-deriving it
 * at every one of the dozen places that asks "is this min fn intrinsic?". */
struct gtrackfn {
    struct gsize mn, mx;
    struct gsize fc;              /* fit-content() argument (valid if is_fc) */
    unsigned char is_fc;
};

/* A line name. `line` is a 0-based line index within the track list the name
 * table belongs to: line 0 is the line before track 0. */
struct gname { char n[32]; int line; };

/* A flat track list -- no repeat() left in it. Owns its two arrays. */
struct gtracklist {
    struct gtrackfn *tr; int n, cap;
    struct gname *nm;    int nn, ncap;
};

/* A parsed grid-template-columns / grid-template-rows.
 *
 * repeat(auto-fill|auto-fit, ...) cannot be expanded at parse time because the
 * repetition count depends on the available space, so the template keeps the
 * three pieces apart and grid_template_expand() joins them. When there is no
 * auto-repeat, everything is in `pre` and `rep`/`post` are empty. */
enum { GREP_NONE = 0, GREP_AUTO_FILL, GREP_AUTO_FIT };
struct gtemplate {
    struct gtracklist pre, rep, post;
    unsigned char auto_repeat;    /* GREP_* */
};

/* grid-template-areas. cell[r*cols + c] is the area name, "" for a null cell. */
struct gridareas { int rows, cols; char (*cell)[32]; };

/* <grid-line>: auto | <ident> | <int> && <ident>? | span && [<int> || <ident>] */
enum { GL_AUTO = 0, GL_LINE, GL_SPAN };
struct gline {
    unsigned char kind;
    unsigned char has_n;   /* an explicit integer was written */
    int  n;                /* line number (may be negative) or span count */
    char name[32];         /* "" if none */
};

/* Alignment keywords. One vocabulary for justify-/align-, content/items/self:
 * the properties accept overlapping subsets and the resolution code branches on
 * the same values, so splitting them into four enums would only mean four
 * conversions. GA_AUTO is only ever a *-self value. */
enum {
    GA_AUTO = 0, GA_NORMAL, GA_STRETCH,
    GA_START, GA_END, GA_CENTER,
    GA_FLEX_START, GA_FLEX_END, GA_SELF_START, GA_SELF_END, GA_LEFT, GA_RIGHT,
    GA_BASELINE, GA_LAST_BASELINE,
    GA_SPACE_BETWEEN, GA_SPACE_AROUND, GA_SPACE_EVENLY
};

/* ------------------------------------------------------------- the input -- */

enum { GAX_COL = 0, GAX_ROW = 1 };

/* What the track sizing algorithm asks an item for. All three are OUTER sizes
 * (margin box), because that is what tracks have to fit.
 *
 * `minimum` is the spec's MINIMUM CONTRIBUTION (s12.5.1): the smallest outer
 * size the item can have -- its used minimum size when its preferred size is
 * auto or percentage-dependent, otherwise its min-content contribution. It is a
 * separate number from `min_content` on purpose; conflating them is the usual
 * way `min-width: 0` stops working inside a grid. */
struct gmeas { int minimum, min_content, max_content; };

/* Measure item `item` in `axis`.
 *
 * For GAX_COL, `avail` is GRID_INDEFINITE (columns are sized first, so no inline
 * size is known yet). For GAX_ROW, `avail` is the item's already-resolved inline
 * content size, so a paragraph can report the height it takes at that width.
 * That is exactly the two-pass order of css-grid-1 s11.1. */
typedef void (*grid_measure_fn)(void *ctx, int item, int axis, int avail,
                                struct gmeas *out);

struct griditem {
    struct gline cs, ce;      /* grid-column-start / -end */
    struct gline rs, re;      /* grid-row-start / -end */
    int order;                /* `order`; items are stable-sorted ascending */

    /* Definite preferred OUTER size per axis, or GRID_INDEFINITE for auto.
     * A definite size suppresses stretching; report it here rather than by
     * making min_content == max_content, because those two also have to be
     * right for the intrinsic phase. */
    int def_w, def_h;

    unsigned char justify_self, align_self;   /* GA_AUTO -> use *-items */
    int  margin[4];                           /* top, right, bottom, left (px) */
    unsigned char m_auto[4];                  /* 1 where that margin is `auto` */

    /* Distance from the item's margin-box start edge to its first baseline in
     * the block axis, or -1 if it synthesises no baseline. Only read when the
     * item participates in baseline alignment. */
    int baseline;
};

struct gridcfg {
    struct gtemplate cols, rows;      /* grid-template-columns / -rows */
    struct gtracklist auto_cols;      /* grid-auto-columns (cycled) */
    struct gtracklist auto_rows;      /* grid-auto-rows   (cycled) */
    const struct gridareas *areas;    /* grid-template-areas, or NULL */

    unsigned char flow_col;           /* grid-auto-flow: column */
    unsigned char flow_dense;         /* grid-auto-flow: dense */

    int gap_x, gap_y;                 /* column-gap / row-gap (px) */

    unsigned char justify_content, align_content;
    unsigned char justify_items, align_items;

    /* Available grid space = the container's CONTENT box, per axis.
     * GRID_INDEFINITE means content-sized (a max-content constraint). */
    int avail_w, avail_h;
    /* Definite min/max constraints on the content box, or GRID_INDEFINITE. */
    int min_w, max_w, min_h, max_h;

    unsigned char rtl;                /* direction: rtl (flips inline start/end) */
};

/* ------------------------------------------------------------ the output -- */

struct gridpos {
    int col, colspan;     /* 0-based track indices into the IMPLICIT grid */
    int row, rowspan;
    int x, y, w, h;       /* border box, relative to the container's content box */
    int area_x, area_y, area_w, area_h;   /* the grid area the item was placed in */
};

struct gridout {
    int ncols, nrows;
    int *colsz, *rowsz;   /* used size of each track, in track order */
    int *colpos, *rowpos; /* each track's start edge, from the content box origin */
    /* Where explicit line 1 sits in the implicit track array. Implicit tracks
     * created before the explicit grid land at indices < this. */
    int col_explicit, row_explicit;
    int ncols_explicit, nrows_explicit;
    struct gridpos *items; int nitems;
    int width, height;    /* used size of the whole grid, gaps included */
};

/* ------------------------------------------------------------------ API --- */

/* All of these return 0 on success and -1 on a parse error / allocation
 * failure. A parse error leaves the output zeroed, which is the property's
 * initial value in every case -- an invalid declaration must not half-apply. */

int  grid_parse_template(const char *s, int len, int font_px, struct gtemplate *out);
int  grid_parse_tracklist(const char *s, int len, int font_px, struct gtracklist *out);
int  grid_parse_areas(const char *s, int len, struct gridareas *out);
/* One <grid-line>. */
int  grid_parse_line(const char *s, int len, struct gline *out);
/* `grid-row` / `grid-column`: <grid-line> [ / <grid-line> ]? */
int  grid_parse_span2(const char *s, int len, struct gline *a, struct gline *b);
/* `grid-area`: row-start / column-start / row-end / column-end */
int  grid_parse_area(const char *s, int len, struct gline out[4]);
/* `grid-auto-flow`. */
int  grid_parse_flow(const char *s, int len, unsigned char *col, unsigned char *dense);
/* One alignment keyword (the positional/distribution part; `safe`/`unsafe` are
 * accepted and dropped). Returns GA_* or -1. */
int  grid_parse_align(const char *s, int len);

void grid_template_free(struct gtemplate *t);
void grid_tracklist_free(struct gtracklist *t);
void grid_areas_free(struct gridareas *a);

/* Expand an auto-repeat template against `avail` px of available space and
 * `gap` px of gutter, per css-grid-1 s7.2.3.1. `avail` may be GRID_INDEFINITE,
 * in which case the auto-repeat repeats once. `*nrep` receives the repetition
 * count. The result is a flat track list the caller must free. */
int  grid_template_expand(const struct gtemplate *t, int avail, int gap,
                          struct gtracklist *out, int *nrep);

/* THE ENTRY POINT layout.c should call.
 *
 * Runs placement, both track sizing passes, alignment and item positioning.
 * `items[0..nitems)` are the container's in-flow children in DOCUMENT order
 * (this module applies `order` itself). Returns 0 on success.
 *
 * The caller owns `out` and must grid_out_free() it. */
int  grid_layout(const struct gridcfg *cfg,
                 const struct griditem *items, int nitems,
                 grid_measure_fn measure, void *ctx,
                 struct gridout *out);
void grid_out_free(struct gridout *out);

/* Placement only -- the same algorithm grid_layout runs internally, exposed so
 * a test (and an inspector) can assert line indices without sizing anything.
 * Writes col/colspan/row/rowspan into `out[]` and the grid's dimensions into
 * ncols/nrows. */
int  grid_place(const struct gridcfg *cfg,
                const struct griditem *items, int nitems,
                struct gridpos *out, int *ncols, int *nrows,
                int *col_explicit, int *row_explicit);

/* Track sizing only, one axis, against explicitly supplied per-item
 * measurements. This is the numeric core; the test suite drives it directly so
 * an assertion about resolved track sizes names nothing but the spec. */
struct gtrackitem { int start, span; struct gmeas m; };
int  grid_size_tracks(const struct gtracklist *tracks, int ntracks,
                      const struct gtrackitem *items, int nitems,
                      int avail, int gap, unsigned char content_align,
                      int *out_size);

#endif /* LOGIT_LAYOUT_GRID_H */
