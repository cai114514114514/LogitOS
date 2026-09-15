#ifndef LOGIT_LAYOUT_H
#define LOGIT_LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "img.h"

/* Layout produces a flat display list (painted in order; hit-tested back-to-front).
 *
 * IT_VIDEO is a <video>/<audio> box. It carries no pixels of its own: layout
 * reserves the border box and browser_paint.c hands it to the media engine
 * (c/apps/browser/js_media*.c), which owns the decoded frame. It is a separate
 * type rather than an IT_IMAGE with a magic `img` because a video frame changes
 * thirty times a second and must never be freed by layout_free(). */
/* IT_CONTROL is a form control's border box -- an <input>, <textarea>,
 * <select> or <button>. Like IT_VIDEO it carries no content of its own: the
 * text in a field, the tick in a checkbox and the caret are STATE, they change
 * on every keystroke, and layout runs far less often than that. So layout
 * reserves the box and records which control it is, and browser_paint.c asks
 * forms.c what to draw inside it at paint time.
 *
 * That split is also what keeps layout.c free of any link dependency on
 * forms.c: eight host test binaries link layout.c, and none of them would
 * otherwise build. */
/* IT_CANVAS is a <canvas>'s border box, and it is a third instance of the same
 * split as IT_VIDEO and IT_CONTROL: layout reserves the box, and the PIXELS
 * belong to whoever owns the state -- here js_canvas.c's backing store, which
 * a script can repaint between two frames without layout running at all. Made
 * a distinct type rather than an IT_IMAGE with a borrowed `img` for the reason
 * IT_VIDEO gives: layout_free() must never own it.
 *
 * APPENDED at the end of the enum on purpose. tests/unit and browser_paint.c
 * switch on these values and `make test-cssom-abi` pins sizeof(struct item)
 * across two flag sets; inserting in the middle would renumber IT_CONTROL
 * under one build and not the other. */
/* IT_HIT is an actual inline whitespace advance, without ink. A node-box
 * union would cover unused ends of wrapped lines and steal clicks there.
 * Display-list order retains overlay/clip rules and the whitespace source. */
enum { IT_RECT, IT_TEXT, IT_IMAGE, IT_VIDEO, IT_CONTROL, IT_CANVAS, IT_HIT };
struct cstyle;
struct item {
    /* node remains the real event target. This borrowed style selects pseudo
     * background/border/text ink; it lives until the next style/layout pass. */
    const struct cstyle *generated_style;
    unsigned char pseudo;             /* 0 real DOM, 1 before, 2 after */
    int type, x, y, w, h;
    struct node *node;                /* DOM node this box came from (NULL only
                                       * if layout ran out of context). Lets a
                                       * hit test / inspector / future event
                                       * dispatch go from a painted box back to
                                       * the element without a second search. */
    int z;                            /* legacy scalar diagnostic; ordering is
                                       * now the flattened stacking-context tree
                                       * in layout_stacking.inc. This field is
                                       * retained for probes/old negative control. */
    /* RECT */ uint32_t bg; int has_bg;
    int bg_alpha;                     /* background-color's own alpha, 0..255.
                                       * Multiplied by `opacity` at paint time,
                                       * so rgba(0,0,0,.5) in an opacity:.5 box
                                       * is a quarter of the ink. */
    int border_w[4]; uint32_t border_color[4];
    unsigned char border_style[4];    /* CSS_BORDER_STYLE_* (LibCSS numbering);
                                       * browser_paint.c draws each edge's
                                       * pattern -- dashes, dots, the two lines
                                       * of `double`, the two tones of the 3D
                                       * styles */
    /* border-radius per corner, TL TR BR BL. Copied straight out of
     * struct cstyle; browser_paint.c's fill_round4() is the reader. */
    int radius[4], radius_pct[4];
    /* TEXT */ const char *text; int len, font_px, bold, italic, mono, underline; uint32_t color;
    int strike, overline;             /* the other two text-decoration lines */
    char marker[16];                    /* backing store for <li> markers (roman
                                       * numerals are the long case) */
    /* IMAGE */ struct image *img; const char *imgsrc;  /* decoded image + its URL */
    int h_auto;                       /* img height was derived (no explicit px): may be
                                       * corrected from the decoded image's aspect ratio */
    const char *href;                 /* link target for this item (or NULL) */
    int hidden;                       /* visibility:hidden / opacity:0 -- layout space kept, nothing painted */
    int opacity;                      /* 0..255 from the element's opacity. Real
                                       * group opacity: the painter blends rects,
                                       * folds it into the colour for text and
                                       * washes the backdrop back over images. */
    /* overflow clip inherited from the nearest ancestor whose overflow is not
     * `visible`, in DOCUMENT coordinates (the painter subtracts the scroll and
     * intersects with the viewport). `has_clip` is a flag rather than a
     * clip_w == 0 sentinel because a clip CAN legitimately collapse to zero
     * width, and that must paint nothing rather than paint everything. Stamped
     * per item the same way `z` is, because a flat display list has no box tree
     * for the painter to walk back up. */
    unsigned char has_clip;
    int clip_x, clip_y, clip_w, clip_h;
    unsigned char is_float;           /* emitted by a floated box (or inside one):
                                       * out of flow, so the line-box shifting
                                       * that text-align does must skip it */
    /* CONTROL */
    unsigned char ctl;                /* FC_* kind (forms.h); 0 for everything else.
                                       * An int rather than a re-derivation from
                                       * `node` at paint time because the painter
                                       * must not have to parse the `type`
                                       * attribute per box per frame. */
    unsigned char ctl_mono;           /* the field's font is monospace */
    int ctl_font;                     /* font size inside the control (px) */
};

/* Lay out `root` (a parsed+styled DOM) into a display list at the given canvas
 * width; fetches/decodes <img>. */
void  layout_page(struct node *root, int canvas_w);
/* Passive documents own their display list, box table, text/image/SVG storage
 * and CSS state. NULL selects the existing top-level document. Activate only
 * between completed layout/style calls; a painter may switch between finished
 * lists. The returned previous handle must be restored on every exit path.
 * No script realm, focus registry or animation clock is created by this API.
 * Destroy releases layout before its CSS backing; it does not free the DOM.
 * The embedder must stop using borrowed item/box/style pointers first and free
 * its DOM before or immediately after destroy. New contexts retain at most
 * 8 MiB of decoded images/SVG; decoder temporary allocations are separate. */
struct layout_context;
struct layout_context *layout_context_create(void);
struct layout_context *layout_context_activate(struct layout_context *context);
void layout_context_destroy(struct layout_context *context);
/* Fetch+decode up to `max` of the page's <img> resources (bounded, blocking);
 * call after layout_page. Returns the number of images successfully loaded. */
int   layout_load_images(int max);
/* fetch: 0 transfers malloc-owned bytes, 1 means pending, -1 failed.
 * Pending requests must never become permanent negative image cache entries. */
int layout_load_images_fetch(int max, int (*fetch)(const char *, unsigned char **, int *));
/* O(1): new decoded dimensions since layout_page, not a DOM mutation or IO
 * request. The embedder flushes layout before image-dependent CSSOM reads and
 * after a completed resource batch; layout_page consumes the generation. */
int layout_image_geometry_pending(void);
int   layout_height(void);
/* Page (canvas) background propagated from <html>/<body>; 1 if set, fills *out. */
int   layout_page_bg(uint32_t *out);
int   layout_count(void);
const struct item *layout_items(void);
/* Animation-only paint snapshots; does not change boxes, text or image ownership. */
int layout_refresh_opacity(struct node *root);
unsigned long long layout_build_count(void);
void  layout_free(void);

/* ---- per-element geometry, for the CSSOM ----
 *
 * The display list above is a list of INK: an element with no background, no
 * border and no text of its own is not in it, so a consumer that reads boxes
 * out of it answers 0 for every such element. The two functions here read the
 * BOX TABLE instead -- one record per element that generated a box, filled
 * whether or not anything painted. See the block comment on `struct boxrec` in
 * layout.c, and the ask these answer in c/apps/browser/js_cssom.h.
 *
 * Both are queries over the last layout_page(); both answer 0 and write zeroes
 * when the element generated no box at all (display:none, out of the tree,
 * never reached), which is a different statement from a box of size zero.
 *
 * FOR THE CALLER THAT LINKS A SUBSET OF THE BROWSER: several host harnesses
 * build js_cssom.c WITHOUT layout.c (tests/cssom.mk's wpt-cssom variant is
 * one), and js_cssom.c already handles that by RE-DECLARING each layout entry
 * point LOGIT_WEAK + LOGIT_WEAK_STUB (include/weaksym.h -- the bare
 * `__attribute__((__weak__))` is ELF-only) after including this header, gating on
 * `&layout_count != 0`. These two need the same two lines and the same gate;
 * the declarations here are ordinary externs and a plain call from a build
 * with no layout.c is a link error, exactly as it is for layout_count. */

/* The element's BORDER box in DOCUMENT coordinates -- the frame `struct item`
 * uses. An element that generates several boxes (an inline across line
 * fragments) answers with their union. */
int   layout_node_box(const struct node *n, int *x, int *y, int *w, int *h);

/* The element's SCROLLABLE OVERFLOW area: its in-flow descendants' margin
 * boxes unioned with its own padding box, measured from the padding edge --
 * i.e. exactly what scrollWidth/scrollHeight are. Content escaping past the
 * LEFT or TOP padding edge is unreachable overflow and is deliberately not
 * counted, which is the spec's rule and not a shortcut. */
int   layout_node_scroll(const struct node *n, int *w, int *h);

/* Optional guest clock; null leaves codec/trial counters silent on host gates. */
void layout_set_profile_clock(unsigned long long (*clock)(void));
/* Explicit about:images snapshot only; never called from layout/frame hot paths.
 * Browser owns request-slot/state diagnostics and prints them after this. */
void layout_dump_images(struct node *root); /* LOGIT_LAYOUT_H */

/* Shared decoded pixels for detached HTMLImageElement preloads: dimensions
 * returns 0 pending/unseen, 1 decoded, -1 broken; store succeeds only on decode. */
int layout_img_dimensions(const char *src, int *w, int *h);
int layout_img_store(const char *src, const unsigned char *data, int len);

#endif
