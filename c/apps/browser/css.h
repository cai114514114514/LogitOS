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
enum { AL_STRETCH, AL_START, AL_END, AL_CENTER, AL_BASELINE, AL_AUTO,
/* align-content only. APPENDED, not inserted: layout.c and the grid and
 * flex modules all switch on these, and renumbering AL_STRETCH..AL_AUTO
 * would change the meaning of every stored byte in a way no compiler
 * would complain about. */
       AL_BETWEEN, AL_AROUND, AL_EVENLY };

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

/* The grid properties whose raw declaration text `struct cstyle` retains for
 * layout_grid.c to parse. See the grid_raw[] comment at the bottom of the
 * struct for why it is text and not values. APPEND ONLY -- css_extra.c indexes
 * this table by name and layout.c by position. */
/* Only the properties that have NO other producer are here. align-items,
 * align-self, align-content and justify-content are in LibCSS's table and
 * cascade properly into the typed fields above -- reading them from raw text
 * as well would replace a real cascade with css_extra's source-order scan,
 * which is a downgrade. justify-items and justify-self are not in the table at
 * all, which is why those two are. */
enum { GR_TEMPL_COLS = 0, GR_TEMPL_ROWS, GR_TEMPL_AREAS,
       GR_AUTO_COLS, GR_AUTO_ROWS, GR_AUTO_FLOW,
       GR_COL, GR_ROW, GR_AREA,
       GR_JUSTIFY_ITEMS, GR_JUSTIFY_SELF,
       GR__COUNT };

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
/* ---------------- inline direction, and the text properties ----------------
 *
 * `direction` is the one three lines were waiting on: it is what `text-align:
 * start`/`end` resolve against, what grid's `start`/`end` alignment means,
 * what the flex main axis reverses on, and the whole of css-writing-modes.
 *
 * THE TEXT PROPERTIES CARRY layout_text.h's OWN VALUES, deliberately. The text
 * line's `struct ltx_style`/`ltx_env` already decided these shapes, so their
 * consumption is an assignment and not a translation -- a second numbering
 * here would be a mapping table nobody owns, and mapping tables between two
 * enums for one property are how the two quietly stop agreeing.
 *
 * NOT EVERY ONE OF THEM HAS A PRODUCER YET, and which is which is stated
 * rather than left to be discovered: LibCSS knows direction, writing-mode,
 * text-indent, letter-spacing, word-spacing, text-transform and white-space,
 * and knows NOTHING about tab-size, word-break, overflow-wrap, line-break,
 * hyphens, text-align-last, text-justify, white-space-collapse or text-wrap --
 * they are not in its property table at all, so no amount of work in
 * css_engine.c can produce them. Those fields hold their CSS initial value and
 * the route to filling them is css_extra.c's raw-declaration pass or a
 * property added to the vendored parser; see the note above convert(). A field
 * at its initial value is the correct answer for every page that does not set
 * the property, which is nearly all of them. */
enum { DIR_LTR = 0, DIR_RTL };
enum { WM_HORIZ_TB = 0, WM_VERT_RL, WM_VERT_LR };

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
    int flex_basis, fb_pct, fb_off, has_fb;     /* flex-basis; !has_fb = auto or content */
    unsigned char fb_content;       /* flex-basis: content -- NOT auto. `auto`
                                     * defers to the item's width property,
                                     * `content` ignores it. */
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

    /* ---- inline direction ---- */
    unsigned char direction;        /* DIR_* -- see the note above */
    unsigned char writing_mode;     /* WM_*  */

    /* ---- text (values are layout_text.h's LTX_*) ---- */
    int text_indent;                /* px, or a percentage when ti_pct */
    unsigned char ti_pct;
    unsigned char ti_each_line;     /* text-indent: ... each-line   (no producer) */
    unsigned char ti_hanging;       /* text-indent: ... hanging     (no producer) */
    int letter_spacing;             /* px, may be negative; 0 = normal */
    int word_spacing;               /* px, may be negative; 0 = normal */
    unsigned char text_transform;   /* LTX_TT_*    */
    unsigned char wsc;              /* LTX_WSC_*, derived from white_space */
    unsigned char text_wrap;        /* LTX_WRAP_*, derived from white_space */
    unsigned char word_break;       /* LTX_WB_*    (no producer) */
    unsigned char overflow_wrap;    /* LTX_OW_*    (no producer) */
    unsigned char line_break;       /* LTX_LB_*    (no producer) */
    unsigned char hyphens;          /* LTX_HY_*    (no producer) */
    unsigned char text_align_last;  /* LTX_ALAST_* (no producer) */
    unsigned char text_justify;     /* LTX_TJ_*    (no producer) */
    int tab_size;                   /* `space` advances, or px when tab_px */
    unsigned char tab_px;           /*             (no producer; initial 8)  */

    /* ---- grid: the RAW DECLARATION TEXT ----
     *
     * Not a typed field per property, and that is the design rather than a
     * shortcut. Our vendored LibCSS has no grid properties AT ALL beyond
     * `display: grid` -- they are not in its property table, so nothing
     * css_engine.c can do produces them. layout_grid.c anticipated exactly
     * that: it ships its own parsers (grid_parse_template / _areas / _span2 /
     * _area / _flow / _align), because a `<track-list>` is a recursive grammar
     * (repeat(auto-fill, minmax(min-content, 1fr))) that no fixed set of ints
     * can hold. So what the style layer owes it is the TEXT, and twelve typed
     * fields would be twelve lossy summaries of the thing it wants.
     *
     * LIFETIME, which is the one thing to get right: these point into
     * css_extra.c's PRIVATE COPY of the author sheet (`g_src`, kept for the
     * rule cache), or into a node's own style="" attribute -- both owned by
     * somebody else and both alive from css_extra_apply() until the sheet
     * changes, which is strictly longer than the layout that reads them. They
     * are NOT NUL-terminated; the length is beside them. Nothing frees them
     * from here. css_extra's out-of-memory fallback path deliberately does not
     * set them (it scans the caller's buffer, which the caller rewrites in
     * place) -- so grid degrades to its old track list there rather than
     * reading freed text. */
    const char    *grid_raw[GR__COUNT];
    unsigned short grid_rawlen[GR__COUNT];
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
    CSSP__COUNT,

    /* A CUSTOM PROPERTY (`--x`), which is not a member of the enum in any
     * meaningful sense and is deliberately numbered past its end.
     *
     * It is here because there is no list to add it to. `--anything` is a
     * valid property name, so the set is unbounded, and the property-miss
     * histogram (LOGIT_CSS_PROP_MISS, css_engine.c) found custom properties to
     * be the single largest thing getComputedStyle turns away -- 1,298 of
     * 3,095 misses in css/css-values alone, spread over names like `--x`,
     * `--prop`, `--unregistered` that no table could have anticipated.
     *
     * Numbered ABOVE CSSP__COUNT so nothing that enumerates the resolvable
     * properties (cssd_item, cssd_get_length, g_prop_names) can reach it, and
     * so css_prop_paint_only()'s switch takes its default -- "assume it moves
     * boxes" -- which is the right answer for a custom property, since what
     * reads it is unknowable from the name.
     *
     * css_computed_text() answers it from the NAME, which css_prop_lookup
     * leaves in css_prop_last_custom(). */
    CSSP_CUSTOM = CSSP__COUNT + 1
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

/* ---------------- the property universe ----------------
 *
 * Every property NAME this engine's parser knows, read straight out of
 * LibCSS's own string table. It is the set a CSSStyleDeclaration must expose
 * as IDL attributes, and it has to be the SAME set css_supports_decl() answers
 * `true` for -- WPT's CSS-supports-CSSStyleDeclaration.html is 1,495 subtests
 * asserting exactly that agreement and nothing else. Taking the list from
 * LibCSS rather than keeping one here is what makes the agreement structural:
 * a property added to the vendored parser appears in both answers at once.
 *
 * Order is LibCSS's (alphabetical); the index is stable for one build only,
 * which is all any caller needs it for. */
int         css_known_prop_count(void);
const char *css_known_prop_at(int i, int *len);   /* NUL-terminated; len optional */

/* ---------------- "serialize a CSS value" (CSSOM 6.7.2) ----------------
 *
 * The BYTES a value reads back as: `#0000ff` -> `rgb(0, 0, 255)`, `.5%` ->
 * `0.5%`, `-0px` -> `0px`, `url(x)` -> `url("x")`, commas spaced, runs of
 * whitespace collapsed. Writes at most `outmax` bytes including the NUL and
 * returns the length written.
 *
 * IT NEVER JUDGES VALIDITY. A shape it does not recognise is copied through
 * unchanged, so this cannot become the second CSS parser css.h warns about --
 * `css_supports_decl()` remains the only thing that decides whether a
 * declaration is real. It is also idempotent, so the same call serves a
 * specified value (source bytes) and a computed one (already canonical). */
int  css_value_serialize(const char *value, int vlen, char *out, int outmax);

/* ---------------- the specified-value canonicaliser ----------------
 *
 * The CSS line's `libcss/canon.h`, forwarded (js_*.c builds without CSS_INC;
 * css_engine.c is the one TU on this side that sees LibCSS's headers).
 *
 *   -1 CSS_CANON_INVALID  neither that parser nor LibCSS can take it. The
 *                         CSSOM stores nothing and getPropertyValue answers "".
 *    0 CSS_CANON_OK       `out` holds the canonical specified serialization.
 *    1 CSS_CANON_PASS     not that parser's property. `out` is untouched, and
 *                         css_value_serialize() is what spells it.
 *
 * The two serializers claim disjoint sets -- canon.c takes the inset/size/
 * margin family and css-anchor-position, this file's takes the rest -- and
 * calling this FIRST is what keeps them disjoint instead of merely different. */
#define CSS_SPEC_INVALID (-1)
#define CSS_SPEC_OK      0
#define CSS_SPEC_PASS    1
int  css_specified_canon(const char *prop, int plen, const char *value, int vlen,
                         char *out, int outcap, int *outlen);

/* THE SECOND HALF OF THE PROPERTY UNIVERSE: every property canon.c claims.
 *
 * css_known_prop_at() reads LibCSS's own string table, which is the right
 * source and is not the whole source -- canon.c handles a set of properties
 * that never entered that table (the anchor family, position-area, the logical
 * box properties, and the grid track properties, of which LibCSS has literally
 * none: `grep -i grid` over its propstrings finds the `grid` value of
 * `display` and nothing else). A CSSStyleDeclaration must publish an IDL
 * attribute for those too, or their serializer is unreachable from script and
 * measures as zero however correct it is.
 *
 * These two are declared here rather than forwarded through css_engine.c the
 * way css_specified_canon is, because unlike that one they need no forwarding:
 * neither signature mentions a LibCSS type, so a plain declaration resolves at
 * link time against canon.o, which every target that links the CSS engine
 * already carries. The declarations are token-for-token the ones in
 * `third_party/css/libcss/include/libcss/canon.h`, so the one TU that sees
 * both headers (css_engine.c) sees one declaration twice and not two.
 *
 * Order and index are canon.c's and stable for one build only, exactly like
 * css_known_prop_at's. The two sets OVERLAP -- `margin`, `color` and `width`
 * are in both -- so a caller concatenating them has to tolerate a name
 * arriving twice; the LibCSS half goes first and owns any name it carries. */
int         css_canon_prop_count(void);
const char *css_canon_prop_at(int idx);

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
 * THAT DIVERGENCE IS NOW CLOSED. js_webapi.c shipped a matchMedia() with a
 * scanner of its own; js_cssom.c installs one backed by THIS function, and it
 * installs last, so its binding is the one a page gets. js_webapi.c's is still
 * in the tree (it is another line's file and was not edited) but is no longer
 * reachable from script -- worth knowing if you go looking for the code that
 * answered a query and find the wrong one. tests/unit/cssom_test.c asserts the
 * property that matters: matchMedia and an @media rule's own verdict agree,
 * for the same query, because they are one evaluator. */
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

/* "Update style" for `n`'s document, the way CSSOM requires before a resolved
 * value is read. Idempotent and cheap when nothing has been mutated.
 *
 * css_computed_text() calls this itself, so the CSSOM bindings need do nothing
 * -- which is deliberate: the defect it fixes was that getComputedStyle
 * answered "" for every property of every element whenever the cascade had not
 * been run by an embedder's render loop, and a fix that had to be REMEMBERED at
 * each call site is a fix that gets forgotten at the next one. See the long
 * note above the definition in css_engine.c for the measurement.
 *
 * When the document has never been styled at all, the author CSS is collected
 * from its own <style> elements. External <link> stylesheets are NOT fetched:
 * that is the embedder's job, and once the embedder has called css_apply once
 * its sheet set is the one re-used here. */
void css_ensure_styled(struct node *n);

/* Test seam: how many times css_ensure_styled has actually re-run the cascade.
 * Asserting on this is how a test tells "the flush happened" from "the value
 * happened to be right already", and it is the guard on the expensive
 * direction -- a flush per read on an unmutated document would show up here as
 * a count that tracks the number of reads. */
int  css_style_flushes(void);

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
