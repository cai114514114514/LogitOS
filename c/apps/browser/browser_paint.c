/* Ring-3 paint (M17 L1): walk the layout display list and draw it with the GUI
 * render syscalls. Mirrors the old kernel net/paint.c, fb_* -> gui_*.
 *
 * Everything here has to be expressible with five primitives -- gui_rect,
 * gui_rrect, gui_text_run, gui_blit and gui_clip -- because ring 3 cannot read
 * the window surface back. That single constraint decides most of the design
 * below: alpha is done by blitting a 1x1 RGBA source (gui_blit is the only
 * primitive that blends), and group opacity on text is folded into the text
 * COLOUR against an estimated backdrop rather than composited.
 *
 * GEOMETRY IS NOT THIS FILE'S JOB ANY MORE. Rounded corners used to be worked
 * out here -- an integer square root and a per-row band loop, which made this
 * the third coverage/paint path in the tree. They now come from c/lib/gfx
 * (Open Logit), the same engine the widget toolkit draws with, so a page's
 * border-radius and a button's corner are antialiased by the same rule at the
 * same size. Two consequences worth knowing: border-radius is no longer a
 * STAIRCASE (the opaque path went through gui_rrect, whose corner test is the
 * boolean dx*dx + dy*dy <= r*r), and a rounded box now costs a fixed seven
 * calls -- three bands plus four corner tiles -- instead of one for the opaque
 * case and 2*radius+1 blended strips for the translucent one. */
#include "logit.h"
#include "gfx.h"
#include "layout.h"
#include "browser_paint.h"
#include "forms.h"
#include "css.h"                   /* struct cstyle + the XR_* raw spans */
#include "css_interp.h"            /* struct ci_xform, for `transform` */

/* The media engine, weakly: a <video> box is painted by whoever owns the
 * decoded frame (c/apps/browser/js_media.c), and this file must keep linking in
 * the host paint/layout tests, which link neither the engine nor QuickJS.
 * Spelled `__weak__` and not `weak`, because mini-libc's features.h is
 * force-included into every browser TU and #defines the plain spelling. */
struct node;
extern void media_paint_box(struct node *node, int x, int y, int w, int h,
                            int clip_x, int clip_y, int clip_w, int clip_h)
    __attribute__((__weak__));

/* js_canvas.c, weakly, for exactly the same reason and with the same split:
 * layout reserves a <canvas>'s box and the PIXELS belong to whoever owns the
 * state -- here the 2d context's backing store, which a script can repaint
 * between two frames without layout running at all. Returns 0 when that
 * element has no context yet, which is the ordinary case for a canvas the
 * page has not drawn into, and the painter then leaves the box alone rather
 * than filling it: an undrawn canvas is TRANSPARENT, not black, and painting
 * it black would hide whatever the page put behind it. */
extern const unsigned char *canvas_pixels(struct node *node, int *w, int *h)
    __attribute__((__weak__));

/* forms.c, weakly, for exactly the same reason. layout.c reserves a control's
 * box; what goes INSIDE it -- the value, the caret, the tick -- is state that
 * changes on every keystroke and lives in forms.c. Weak because eight host test
 * binaries link this painter without forms.c: with it absent every control
 * still draws its chrome, just empty, which is a visible and honest
 * degradation rather than a link error in someone else's test. */
extern int fc_paint_state(struct node *n, int font_px, int mono, int content_w,
                          struct fpaint *out) __attribute__((__weak__));

/* ======================= the style layer, weakly ===========================
 *
 * WHY THE PAINTER READS `struct cstyle` AT ALL, when everything else it draws
 * arrives in `struct item`.
 *
 * `transform`, `box-shadow` and every gradient value are absent from the
 * vendored LibCSS property table, so they never reach the cascade and never
 * reach layout; css_extra.c captures the declaration TEXT into
 * `cstyle.xraw[XR_*]` and that is their only producer. Two roads led here from
 * that, and the one not taken is worth naming: adding four fields to
 * `struct item` would put the values where the painter already looks -- and
 * `make test-cssom-abi` PINS sizeof(struct item) across two flag sets
 * (layout.h:33), so growing it is a gate to negotiate rather than a field to
 * add. The item already carries `node`, and `node->style` is the same style
 * css_extra patched, so the value is one dereference away with no ABI to move
 * and no second copy to keep in step.
 *
 * EVERY ENTRY POINT BELOW IS WEAK, and that is not defensive coding -- it is
 * the shape of the link lines, measured with the continuations joined (see
 * CLAUDE.md on why they must be). Of the source lists that name
 * browser_paint.c: paint_test and paint_asan (Makefile:2818, :3145) link
 * neither css_extra.c nor css_interp.c; css_bench, css_audit (x2), the reftest
 * pipeline, the loader test and arena_page_mem link css_extra.c but NOT
 * css_interp.c -- seven lists in total that would stop linking on a direct
 * call. The file already carries three weak externs for exactly this reason
 * (media_paint_box, canvas_pixels, fc_paint_state); these are the same
 * mechanism for the same measurement, and every one of them is gated at the
 * call site so a build without the producer paints what it painted before
 * rather than crashing on a null call. */
extern int css_gradient_parse(const char *v, int len, int fs_px, int root_px,
                              struct cgradient *out) __attribute__((__weak__));
extern int css_shadow_parse(const char *v, int len, int fs_px, int root_px,
                            struct cshadow *out, int max) __attribute__((__weak__));
extern int css_origin_parse(const char *v, int len, int fs_px, int root_px,
                            struct corigin *out) __attribute__((__weak__));
extern int css_root_px(void) __attribute__((__weak__));
extern int ci_transform_parse(const char *s, int len, double fs_px, double root_px,
                              struct ci_xform *out) __attribute__((__weak__));
extern void ci_transform_matrix(const struct ci_xform *t, double refw, double refh,
                                double m[16]) __attribute__((__weak__));
/* img_css_color is declared by img.h (via layout.h). Re-declared weak here for
 * the same reason: it lives in c/lib/image/svg.c, which paint_test does not
 * link. This is the tree's ONE CSS colour evaluator -- see the argument in
 * svg.c and in css.h's XR_* comment -- so a stop colour is resolved by calling
 * it and never by a second parser written here. */
extern int img_css_color(const char *s, int len, unsigned char *rgba)
    __attribute__((__weak__));

/* The computed style behind a display-list item, or NULL.
 *
 * A text box hangs off the TEXT node, which has no style of its own, so the
 * walk climbs to the nearest element -- the same climb browser_hittest_node()
 * does at the bottom of this file, and for the same reason. */
static const struct cstyle *sty(const struct item *e)
{
    struct node *n = e ? e->node : 0;
    while (n && n->type != N_ELEM) n = n->parent;
    return n ? (const struct cstyle *)n->style : 0;
}

/* The root element's font-size, for `rem`. Not on any cstyle -- it is a
 * property of the DOCUMENT -- so it comes from css_engine.c. 16 is both the
 * fallback when that TU is absent and what CSS says a rem means before a root
 * has been styled, so the two answers coincide and there is no second rule. */
static int root_px(void) { return css_root_px ? css_root_px() : 16; }

/* css_border_style_e, mirrored rather than included: layout stores LibCSS's raw
 * value in `border_style`, and pulling <libcss/properties.h> into the painter
 * would drag the whole selection engine's headers in for ten integers. The
 * numbering is fixed public API, and css_extra_test pins it. */
enum { BS_NONE = 1, BS_HIDDEN = 2, BS_DOTTED = 3, BS_DASHED = 4, BS_SOLID = 5,
       BS_DOUBLE = 6, BS_GROOVE = 7, BS_RIDGE = 8, BS_INSET = 9, BS_OUTSET = 10 };

/* ---- colour ---- */
static int chan_r(uint32_t c) { return (int)((c >> 16) & 0xFF); }
static int chan_g(uint32_t c) { return (int)((c >> 8) & 0xFF); }
static int chan_b(uint32_t c) { return (int)(c & 0xFF); }
static uint32_t pack_rgb(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* `a` of `top` over `base`, a in 0..255. The channel lerp is the engine's, so
 * a colour folded together here and one produced by a gfx gradient land on the
 * same byte -- otherwise a card's flat fill and the top of its own gradient
 * differ by one and show a seam. */
static uint32_t mix(uint32_t base, uint32_t top, int a)
{
    if (a >= 255) return top;
    if (a <= 0) return base;
    return gfx_mix(base, top, a);
}

/* Lighten (pct > 0) or darken (pct < 0) toward white/black. This is how the
 * 3D border styles get their two tones: CSS leaves the exact shades up to the
 * UA, and a fixed +/-35% reads as bevelled at every base colour. */
static uint32_t shade(uint32_t c, int pct)
{
    int r = chan_r(c), g = chan_g(c), b = chan_b(c);
    if (pct >= 0) return pack_rgb(r + (255 - r) * pct / 100,
                                  g + (255 - g) * pct / 100,
                                  b + (255 - b) * pct / 100);
    return pack_rgb(r + r * pct / 100, g + g * pct / 100, b + b * pct / 100);
}

/* ---- points -> device pixels ----
 * Everything the painter handles is in POINTS; the compositor multiplies by the
 * display's backing scale. The engine's masks have to be generated at DEVICE
 * size so the compositor's nearest-neighbour rescale of the blit is the
 * identity -- a mask made at 1x and stretched to a 2x window is a blurry mask
 * of a sharp shape, which is worse than the staircase it replaced. devlen()
 * converts a SPAN as the difference of two converted edges rather than
 * converting the length: two abutting 5-point columns must become 7 and 8
 * device pixels at 150%, not 7 and 7, or the missing column shows as a moving
 * hairline. */
static int scale_pct;
static int dev(int p)
{
    if (!scale_pct) { scale_pct = ui_scale(); if (scale_pct < 100) scale_pct = 100; }
    return p >= 0 ? p * scale_pct / 100 : -(((-p) * scale_pct + 99) / 100);
}
static int devlen(int a, int len) { return dev(a + len) - dev(a); }

/* One corner tile, expanded to RGBA and blitted into a POINT rect of the same
 * device size. One rasterized quadrant serves all four corners; the mirroring
 * is what makes that true. */
static unsigned char corner_rgba[GFX_MASK_MAX * GFX_MASK_MAX * 4];
static void corner_blit(int x, int y, int r, const unsigned char *cov,
                        int cw, int ch, uint32_t color, int alpha, int fx, int fy)
{
    if (!cov || cw <= 0 || ch <= 0 || r <= 0) return;
    gfx_mask_to_rgba(corner_rgba, cov, cw, ch, color, alpha, fx, fy);
    gui_blit(x, y, r, r, corner_rgba, cw, ch);
}

/* ---- the device clip ----
 * Tracked here as well as in the kernel for two reasons: rects can be clipped
 * before the syscall (see fill()), and re-programming SYS_GUI_CLIP once per
 * item would be a syscall per box. */
static int cl_x0, cl_y0, cl_x1, cl_y1;   /* current clip, window coordinates */

static void set_clip(int x0, int y0, int x1, int y1)
{
    cl_x0 = x0; cl_y0 = y0; cl_x1 = x1; cl_y1 = y1;
    gui_clip(x0, y0, x1 - x0, y1 - y0);
}

/* Solid or blended fill, clipped in ring 3 first.
 *
 * gui_rect packs its coordinates into UNSIGNED 16-bit fields, so a rect that
 * begins above or left of the window used to wrap to a coordinate off the far
 * edge and vanish entirely -- which is what made a tall page background
 * disappear once it was scrolled past its own top. Clipping here guarantees
 * every coordinate handed to the kernel is inside the window. */
static void fill(int x, int y, int w, int h, uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < cl_x0) x0 = cl_x0;
    if (y0 < cl_y0) y0 = cl_y0;
    if (x1 > cl_x1) x1 = cl_x1;
    if (y1 > cl_y1) y1 = cl_y1;
    if (x1 <= x0 || y1 <= y0) return;
    if (alpha >= 255) { gui_rect(x0, y0, x1 - x0, y1 - y0, color); return; }
    /* There is no "fill with alpha" syscall, but SYS_GUI_BLIT scales a
     * straight-RGBA source with per-pixel alpha -- so a 1x1 source stretched
     * over the rect IS an alpha fill, at exactly the per-pixel cost
     * fb_fill_rect would have paid. That is why no new ABI call was added. */
    unsigned char px[4];
    px[0] = (unsigned char)chan_r(color);
    px[1] = (unsigned char)chan_g(color);
    px[2] = (unsigned char)chan_b(color);
    px[3] = (unsigned char)alpha;
    gui_blit(x0, y0, x1 - x0, y1 - y0, px, 1, 1);
}

/* (x,y,w,h) MINUS the rectangle (hx,hy,hw,hh), as up to four ordinary fills.
 *
 * The one caller is box-shadow, and CSS is the reason it exists: an outer
 * shadow is "drawn outside the border edge only -- clipped inside the
 * border-box of the element". Over an OPAQUE background that clip is
 * unobservable (the background is painted immediately afterwards and covers
 * every pixel of it), and the shadow is drawn whole. Over a transparent one it
 * is the whole difference between a ring and a wash of colour across the box.
 *
 * The hole is a RECTANGLE and the caster may be rounded, so at a rounded
 * caster's corner this removes slightly more than CSS says -- a sliver at most
 * `radius` across, and only in the case where the caster paints nothing there
 * anyway. The exact answer is the caster's rounded outline as a clip path, and
 * gfx_fill_clipped multiplies coverages rather than subtracting them, so it
 * would need a 1-minus-coverage the engine does not have. Named rather than
 * quietly approximated; the zero-blur no-offset case, which is where this
 * would show most, does not come through here at all (see shadow_outer). */
static void fill_hole(int x, int y, int w, int h, int hx, int hy, int hw, int hh,
                      uint32_t c, int a)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h, hx1 = hx + hw, hy1 = hy + hh;
    if (hw <= 0 || hh <= 0 || hx >= x1 || hx1 <= x || hy >= y1 || hy1 <= y) {
        fill(x, y, w, h, c, a);
        return;
    }
    if (hy > y)   fill(x, y, w, hy - y, c, a);
    if (hy1 < y1) fill(x, hy1, w, y1 - hy1, c, a);
    int ty = hy > y ? hy : y, by = hy1 < y1 ? hy1 : y1;
    if (by > ty) {
        if (hx > x)   fill(x, ty, hx - x, by - ty, c, a);
        if (hx1 < x1) fill(hx1, ty, x1 - hx1, by - ty, c, a);
    }
}

/* A rounded fill, opaque or translucent, as a 9-SLICE: three bands that are
 * ordinary rectangles plus four r x r corner tiles from the engine's cache. So
 * it costs O(r^2) of rasterization and not O(w*h), and the second box on the
 * page with the same radius rasterizes nothing at all.
 *
 * Both cases go this way now. The opaque one used to call gui_rrect, which is
 * one syscall but whose corner test is the boolean `dx*dx + dy*dy <= r*r` -- a
 * staircase, and on a page full of cards the loudest thing in the viewport.
 * Seven calls for an antialiased corner is the right trade, and the translucent
 * case got CHEAPER in the same move (it was 2*radius + 1 blended strips). */
static void fill_round_hole(int x, int y, int w, int h, int r, uint32_t color, int alpha,
                            int hx, int hy, int hw, int hh)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fill_hole(x, y, w, h, hx, hy, hw, hh, color, alpha); return; }
    int cw = devlen(x, r), ch = devlen(y, r);
    const unsigned char *cov = gfx_mask_corner(GFX_MASK_FILL, cw, ch, 0);
    /* A radius past the cache's tile limit is still drawn, just square: a
     * missing background is a worse answer than a sharp corner. This is the
     * ACCEPTABLE half of gfx_mask_corner's contract (see its comment in
     * gfx_mask.c) -- complete, no dropped geometry -- and it is no longer a
     * SILENT square: every refusal is counted the moment gfx_mask_corner
     * returns NULL (gfx_mask.c's mrefuse / gfx_mask_refused()), which is what
     * makes this fallback "reported" rather than a picture nobody can query.
     * A big `border-radius` (a CSS pill button, a large rounded card) is
     * exactly the real-world case this fires for; c/apps/gui/aui.c's
     * BIG_MASK tile is the fix for the equivalent toolkit shapes and is not
     * duplicated here on purpose -- this file has its own host test suite
     * (BTEST_INC in the Makefile) this unit did not want to put at risk for a
     * fallback that was already correct, just previously unobservable. */
    if (!cov) { fill_hole(x, y, w, h, hx, hy, hw, hh, color, alpha); return; }
    fill_hole(x + r, y,         w - 2 * r, r,         hx, hy, hw, hh, color, alpha);
    fill_hole(x,     y + r,     w,         h - 2 * r, hx, hy, hw, hh, color, alpha);
    fill_hole(x + r, y + h - r, w - 2 * r, r,         hx, hy, hw, hh, color, alpha);
    corner_blit(x,         y,         r, cov, cw, ch, color, alpha, 0, 0);
    corner_blit(x + w - r, y,         r, cov, cw, ch, color, alpha, 1, 0);
    corner_blit(x,         y + h - r, r, cov, cw, ch, color, alpha, 0, 1);
    corner_blit(x + w - r, y + h - r, r, cov, cw, ch, color, alpha, 1, 1);
}

/* The ordinary rounded fill: the same nine slices with no hole. Kept as the
 * name every existing caller uses -- the hole is a box-shadow concern and no
 * other caller should have to write four zeroes to say "no hole". */
static void fill_round(int x, int y, int w, int h, int r, uint32_t color, int alpha)
{
    fill_round_hole(x, y, w, h, r, color, alpha, 0, 0, 0, 0);
}

/* A rounded OUTLINE of thickness `t`, drawn as four straight bars plus four
 * ring corner tiles (GFX_MASK_RING, the tile whose inner arc shares the
 * outer's CENTRE -- see the trap recorded above gfx_corner_ring).
 *
 * This is the shape browser_paint.c did not have, and its absence was a real
 * bug: `border: 1px solid; border-radius: 8px` with NO background fell through
 * to the square border path, so the box drew with sharp corners. The rounded
 * branch was written for backgrounds and its `&& e->has_bg` guard was doing
 * two jobs -- "there is a fill to draw" and "the border is rounded" -- which
 * are not the same question. Two rounded fills (outer in the border colour,
 * inner in the background) cannot express it either: with no background there
 * is no colour to paint the middle with, and painting the page background
 * there would punch a hole through whatever the box is sitting on. */
static void stroke_round(int x, int y, int w, int h, int r, int t,
                         uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0 || t <= 0 || alpha <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (t * 2 > w) t = w / 2;
    if (t * 2 > h) t = h / 2;
    if (r <= 0) {
        fill(x, y, w, t, color, alpha);
        fill(x, y + h - t, w, t, color, alpha);
        fill(x, y + t, t, h - 2 * t, color, alpha);
        fill(x + w - t, y + t, t, h - 2 * t, color, alpha);
        return;
    }
    int cw = devlen(x, r), ch = devlen(y, r);
    int td = devlen(x, t);
    if (td < 1) td = 1;
    if (td > cw - 1) td = cw - 1;               /* t >= r: the tile is a full quadrant */
    if (td < 1) td = 1;
    const unsigned char *cov = gfx_mask_corner(GFX_MASK_RING, cw, ch, td);
    if (!cov) {
        /* Past the cache's tile ceiling. A SQUARE outline is the acceptable
         * degradation (gfx_mask.c's contract, case 2): complete, no missing
         * arcs -- the failure that comment names by name is drawing the four
         * straight bars and letting the corners simply not arrive, which is
         * what aui_stroke used to do. gfx_mask_corner has already counted the
         * refusal, so this is not silent. */
        fill(x, y, w, t, color, alpha);
        fill(x, y + h - t, w, t, color, alpha);
        fill(x, y + t, t, h - 2 * t, color, alpha);
        fill(x + w - t, y + t, t, h - 2 * t, color, alpha);
        return;
    }
    fill(x + r,         y,             w - 2 * r, t,         color, alpha);
    fill(x + r,         y + h - t,     w - 2 * r, t,         color, alpha);
    fill(x,             y + r,         t,         h - 2 * r, color, alpha);
    fill(x + w - t,     y + r,         t,         h - 2 * r, color, alpha);
    corner_blit(x,         y,         r, cov, cw, ch, color, alpha, 0, 0);
    corner_blit(x + w - r, y,         r, cov, cw, ch, color, alpha, 1, 0);
    corner_blit(x,         y + h - r, r, cov, cw, ch, color, alpha, 0, 1);
    corner_blit(x + w - r, y + h - r, r, cov, cw, ch, color, alpha, 1, 1);
}

/* ================== one-dimensional ramps: shadows and gradients ===========
 *
 * ONE buffer, because both features are the same trick: a 1 x n (or n x 1)
 * straight-RGBA source blitted over a rectangle, which the compositor
 * replicates along the constant axis. That is what makes a gradient cost what
 * a flat fill costs, and what makes a shadow's cost depend on its BLUR radius
 * and not on the size of the thing casting it.
 *
 * 1024 entries is the same bound c/apps/gui/aui.c uses (GRAD_MAX) and for the
 * same reason: past it the ramp is subsampled and the compositor stretches it
 * back. A linear ramp is the one thing where that is provably cheap -- 1024
 * samples of a 0..255 channel is under a quarter of a level per sample, so a
 * rescale cannot introduce a step the ramp did not already have. (Contrast the
 * corner tiles, which are generated at DEVICE size precisely because a mask of
 * a SHARP shape does not survive being stretched.) */
#define RAMP_MAX 1024
static unsigned char ramp_buf[RAMP_MAX * 4];

/* One shadow falloff strip: `n` device samples across the blur, one pixel
 * along the constant axis, stretched by the compositor. `reverse` picks which
 * end of the strip touches the caster. */
static void ramp_shadow_edge(int x, int y, int w, int h, int n, int vertical,
                             int reverse, uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0 || n <= 0 || alpha <= 0) return;
    if (n > RAMP_MAX) n = RAMP_MAX;
    long blur = (long)n * 256;
    for (int k = 0; k < n; k++) {
        int kk = reverse ? k : n - 1 - k;            /* distance from the caster */
        int a = gfx_shadow_falloff((long)kk * 256 + 128, blur) * alpha / 255;
        ramp_buf[k * 4 + 0] = (unsigned char)chan_r(color);
        ramp_buf[k * 4 + 1] = (unsigned char)chan_g(color);
        ramp_buf[k * 4 + 2] = (unsigned char)chan_b(color);
        ramp_buf[k * 4 + 3] = (unsigned char)a;
    }
    /* CULLED here, not clipped, and the difference matters. The ramp is
     * STRETCHED over the destination rect, so trimming the rect in ring 3
     * without trimming the ramp would sample it from the wrong end and the
     * falloff would run at the wrong rate -- the shadow would get harder as it
     * scrolls. gui_blit takes real ints (unlike gui_rect, which packs
     * unsigned 16-bit fields -- see fill()), so a negative or oversized rect
     * is the kernel's clip to resolve and it resolves it correctly. All ring 3
     * does is skip the syscall when nothing of the strip is on screen. */
    if (x + w <= cl_x0 || x >= cl_x1 || y + h <= cl_y0 || y >= cl_y1) return;
    if (vertical) gui_blit(x, y, w, h, ramp_buf, 1, n);
    else          gui_blit(x, y, w, h, ramp_buf, n, 1);
}

/* ---- box-shadow ----------------------------------------------------------
 *
 * WHAT THE CORPUS ACTUALLY CONTAINS, measured over the 102 sheets of
 * tests/fixtures/cssweb (87 .css + 15 .html), unprefixed `box-shadow:` only,
 * whole-name matched, declarations split on top-level commas:
 *
 *     1,045 individual shadows in non-`none` declarations
 *       725 carry a var() -- css_shadow_parse refuses those, so they are not
 *           this painter's business yet
 *       320 LITERAL, and this is the set below is written for:
 *             252  outer with a blur          -> the 8-slice
 *              16  `0 0 0 Npx c` (the ring)   -> stroke_round, EXACT
 *               6  zero blur with an offset   -> a rounded fill with a hole
 *               9  all four lengths zero      -> CSS says invisible; skipped
 *              37  inset (15 blur 0, 22 blurred)
 *
 * THE BLUR IS NOT A GAUSSIAN and that is the one approximation here. CSS
 * defines box-shadow's blur as a Gaussian of standard deviation blur/2;
 * c/lib/gfx has gfx_shadow_falloff, a quadratic ease-out from 255 at the
 * caster's edge to 0 at `blur`, which is what every window on this desktop has
 * been drawn with since M8. The ramp differs through the middle, so the shadow
 * reads slightly harder. The rejected alternative was refusing to paint a
 * blurred shadow at all, which loses 252 of the 320 literal shadows -- i.e.
 * essentially every card on the web -- to buy an exactness nothing else in
 * this system has either.
 *
 * BLURRED INSET SHADOWS ARE NOT PAINTED, and they are COUNTED rather than
 * silently dropped (browser_paint_shadow_stats). The inward falloff needs a
 * tile whose ramp runs the other way -- 255 at the border edge falling to 0 at
 * depth `blur` -- and inverting GFX_MASK_SHADOW does not produce it: the
 * engine's tile is 255*u^2 outside the arc, and one minus that is 255*(2t-t^2),
 * not the 255*t^2 the inward ramp needs. Writing the tile here instead would
 * put a second shadow-profile generator in the tree beside gfx_corner_shadow,
 * which is the exact duplication c/lib/gfx exists to have ended. 22 of 320
 * literal shadows; the 15 hard ones (github's whole `inset 0 -1px 0 0` chrome)
 * are painted exactly. */
static int g_sh_inset_blur;      /* blurred inset shadows refused this session */
static int g_sh_tight;           /* shadows whose blur was tightened to fit a tile */

void browser_paint_shadow_stats(int *inset_blur_skipped, int *blur_tightened)
{
    if (inset_blur_skipped) *inset_blur_skipped = g_sh_inset_blur;
    if (blur_tightened)     *blur_tightened = g_sh_tight;
}

/* The blurred shape: four corner tiles + four falloff strips + the three
 * interior bands, with (hx,hy,hw,hh) subtracted from the bands only. */
static void shadow_blurred(int x, int y, int w, int h, int r, int blur,
                           uint32_t col, int alpha,
                           int hx, int hy, int hw, int hh)
{
    int T = blur + r;
    if (T <= 0) { fill_hole(x, y, w, h, hx, hy, hw, hh, col, alpha); return; }
    int cw = devlen(x - blur, T), ch = devlen(y - blur, T);
    const unsigned char *m = 0;
    if (cw > 0 && ch > 0) {
        int rd = devlen(x, r);
        if (rd > cw - 1) rd = cw - 1;
        if (rd < 0) rd = 0;
        m = gfx_mask_corner(GFX_MASK_SHADOW, cw, ch, rd);
    }
    if (!m) {
        /* Past GFX_MASK_MAX. Tighten the blur until the tile fits and count
         * it -- aui_shadow_ex's tier 3, and the same reasoning: `blur` is in
         * POINTS and the ceiling is in DEVICE pixels, so the ceiling is
         * converted back to points BEFORE blur is shrunk, or the strips below
         * (which key off the same variable) fall off over a different
         * distance than the corners and the two seam. A tighter shadow is
         * gfx_mask.c's acceptable degradation; corners that never arrive is
         * the one it forbids. */
        int capT = GFX_MASK_MAX * 100 / (scale_pct ? scale_pct : 100);
        int nb = capT - r;
        if (nb <= 0) { fill_hole(x, y, w, h, hx, hy, hw, hh, col, alpha); return; }
        if (nb < blur) g_sh_tight++;
        if (nb < blur) blur = nb;
        T = blur + r;
        cw = devlen(x - blur, T); ch = devlen(y - blur, T);
        if (cw > GFX_MASK_MAX) cw = GFX_MASK_MAX;
        if (ch > GFX_MASK_MAX) ch = GFX_MASK_MAX;
        int rd = devlen(x, r);
        if (rd > cw - 1) rd = cw - 1;
        if (rd < 0) rd = 0;
        m = gfx_mask_corner(GFX_MASK_SHADOW, cw, ch, rd);
        if (!m) { fill_hole(x, y, w, h, hx, hy, hw, hh, col, alpha); return; }
    }
    /* corner_blit blits into an r x r POINT rect; the shadow's tile is T x T,
     * so it is called through the same helper with T. corner_rgba is
     * GFX_MASK_MAX^2 x 4, which is exactly the largest tile the cache will
     * ever hand back, so the buffer cannot be overrun by a bigger geometry --
     * only by a bigger CEILING, which is a change to gfx.h. */
    corner_blit(x - blur,     y - blur,     T, m, cw, ch, col, alpha, 0, 0);
    corner_blit(x + w - r,    y - blur,     T, m, cw, ch, col, alpha, 1, 0);
    corner_blit(x - blur,     y + h - r,    T, m, cw, ch, col, alpha, 0, 1);
    corner_blit(x + w - r,    y + h - r,    T, m, cw, ch, col, alpha, 1, 1);
    int bd = devlen(y - blur, blur);
    ramp_shadow_edge(x + r, y - blur, w - 2 * r, blur, bd, 1, 0, col, alpha);
    ramp_shadow_edge(x + r, y + h,    w - 2 * r, blur, bd, 1, 1, col, alpha);
    bd = devlen(x - blur, blur);
    ramp_shadow_edge(x - blur, y + r, blur, h - 2 * r, bd, 0, 0, col, alpha);
    ramp_shadow_edge(x + w,    y + r, blur, h - 2 * r, bd, 0, 1, col, alpha);
    /* The interior the tiles and strips leave: the box minus its four corner
     * squares, i.e. exactly fill_round's three bands. */
    fill_hole(x + r, y,         w - 2 * r, r,         hx, hy, hw, hh, col, alpha);
    fill_hole(x,     y + r,     w,         h - 2 * r, hx, hy, hw, hh, col, alpha);
    fill_hole(x + r, y + h - r, w - 2 * r, r,         hx, hy, hw, hh, col, alpha);
}

/* One shadow of the list. `bx,by,bw,bh,br` is the caster's border box in
 * window points; `covered` says the caster is about to paint an opaque
 * background over its own footprint, which is what makes CSS's "clipped inside
 * the border box" unobservable and lets the whole shape be drawn. */
static void shadow_one(int bx, int by, int bw, int bh, int br,
                       const struct cshadow *s, uint32_t col, int alpha, int covered)
{
    if (alpha <= 0 || bw <= 0 || bh <= 0) return;
    int sp = s->spread, blur = s->blur;
    if (blur < 0) blur = 0;

    if (s->inset) {
        /* The inner box: deflated by the spread and moved by the offset. The
         * shadow is what lies between it and the border box. */
        int ix = bx + s->dx + sp, iy = by + s->dy + sp;
        int iw = bw - 2 * sp, ih = bh - 2 * sp;
        int ir = br - sp; if (ir < 0) ir = 0;
        if (blur > 0) { g_sh_inset_blur++; return; }
        if (iw <= 0 || ih <= 0) { fill_round(bx, by, bw, bh, br, col, alpha); return; }
        if (s->dx == 0 && s->dy == 0 && sp > 0) {
            /* The exact case, and the common one: an inner RING of thickness
             * `spread` following the border's own curve at both edges. */
            stroke_round(bx, by, bw, bh, br, sp, col, alpha);
            return;
        }
        if (s->dx == 0 && s->dy == 0) return;           /* nothing to show */
        fill_round_hole(bx, by, bw, bh, br, col, alpha, ix, iy, iw, ih);
        return;
    }

    int x = bx + s->dx - sp, y = by + s->dy - sp;
    int w = bw + 2 * sp, h = bh + 2 * sp;
    int r = br + sp; if (r < 0) r = 0;
    if (w <= 0 || h <= 0) return;

#ifdef PAINT_NEGCTL_SHADOW_NO_CLIP
    /* NEGATIVE CONTROL (test-paint-gfx-negctl). The PLAUSIBLE wrong
     * implementation, not the absent one: paint the shadow's whole shape and
     * let the element's background cover the part CSS says to clip out. It
     * draws a perfectly good shadow on every card on the web -- opaque
     * backgrounds hide the difference entirely -- and washes the shadow colour
     * across every box that has no background, which is what a focus ring
     * (`0 0 0 2px`) on a transparent input is. Only the transparent-caster
     * rows may redden. */
    covered = 1;
#endif
    if (blur <= 0 && s->dx == 0 && s->dy == 0) {
        if (sp <= 0) return;             /* CSS: wholly behind the caster */
        if (!covered) {
            /* A rounded ring of thickness `spread`, EXACT at both edges --
             * this is the `0 0 0 1px #ccc` idiom, 16 of the corpus's 320
             * literal shadows and the one shape where fill_hole's rectangular
             * hole would show, because here the hole's rounded corner is the
             * only part of the shape there is. */
            stroke_round(x, y, w, h, r, sp, col, alpha);
            return;
        }
    }
    int hx = covered ? 0 : bx, hy = covered ? 0 : by;
    int hw = covered ? 0 : bw, hh = covered ? 0 : bh;
    if (blur <= 0) fill_round_hole(x, y, w, h, r, col, alpha, hx, hy, hw, hh);
    else           shadow_blurred(x, y, w, h, r, blur, col, alpha, hx, hy, hw, hh);
}

/* Read the element's box-shadow list and paint it. `pre` selects the half:
 * outer shadows go UNDER the box, inset shadows OVER it, and CSS paints the
 * FIRST-listed shadow on top -- so the outer pass walks the list backwards and
 * the inset pass forwards. Returns the number of shadows painted. */
static int paint_shadows(const struct item *e, int sx, int sy, int r, int op, int pre)
{
    const struct cstyle *st = sty(e);
    if (!st || !css_shadow_parse || !st->xraw[XR_BOX_SHADOW]) return 0;
    struct cshadow sh[CS_MAXSHADOW];
    int n = css_shadow_parse(st->xraw[XR_BOX_SHADOW], st->xrawlen[XR_BOX_SHADOW],
                             st->font_px > 0 ? st->font_px : 16, root_px(),
                             sh, CS_MAXSHADOW);
    if (n <= 0) return 0;
    int bga = e->has_bg ? e->bg_alpha * op / 255 : 0;
    int covered = e->has_bg && bga >= 255;
    int painted = 0;
    for (int k = 0; k < n; k++) {
        int i = pre ? n - 1 - k : k;             /* first listed paints last / on top */
        if (!!sh[i].inset == !!pre) continue;    /* the other half's pass */
        /* An omitted shadow colour means the element's own `color`. Three of
         * the corpus's declarations do that -- rare enough that a painter is
         * likely to leave the path unhandled and never notice, which is why it
         * is spelled out rather than folded into a default. */
        uint32_t col = st->color;
        int a = op;
        if (sh[i].color && img_css_color) {
            unsigned char rgba[4] = { 0, 0, 0, 255 };
            if (img_css_color(sh[i].color, sh[i].colorlen, rgba)) {
                col = pack_rgb(rgba[0], rgba[1], rgba[2]);
                a = rgba[3] * op / 255;
            }
        }
        shadow_one(sx, sy, e->w, e->h, r, &sh[i], col, a, covered);
        painted++;
    }
    return painted;
}

/* ---- linear-gradient backgrounds -----------------------------------------
 *
 * WHAT THE CORPUS LOOKS LIKE, and it is what decided the shape of this code.
 * 385 `linear-gradient(` calls across the 102 sheets of tests/fixtures/cssweb,
 * classified by their first argument:
 *
 *     203  no direction at all  -> `to bottom`   ]  349 of 385 = 91%
 *     146  an axis-aligned keyword or a 0/90/180/270deg angle  ]  AXIS-ALIGNED
 *      36  a corner keyword or an off-axis angle              -> diagonal
 *
 * So nine gradients in ten are a ONE-DIMENSIONAL ramp along x or y, which the
 * compositor can replicate for free from a 1 x n source -- the same trick
 * aui_vgrad has drawn the desktop with since M8, at the cost of a flat fill.
 * The diagonal tenth cannot be expressed that way at all and goes through a
 * bounded surface. Two paths, and the split is measured rather than assumed.
 *
 * THE RAMP IS BUILT IN DEVICE ORDER, not gradient order, which is why
 * gfx_gradient_strip_paint() is not what is called here: its contract is `n`
 * entries with t running 0 -> 65536 across the strip, and that is only the
 * right ramp when the strip IS the whole gradient extent, running the same way
 * the device rows do. `to top` runs UP the box, and a rounded box's three
 * bands each index a SUB-RANGE of the ramp. gfx_paint_sample() is the
 * primitive that answers both -- one call per device sample, at the sample's
 * own coordinate -- and it is what the corner tiles need too, so bands and
 * corners come out of the same evaluator and cannot disagree at the seam. */

/* The largest gradient surface, DEVICE px per side. Only the diagonal tenth
 * ever allocates it. 128 is chosen from the banding it can cost, not from
 * taste: a box wider than this is rendered smaller and stretched by the
 * compositor, and across 128 samples a channel that traverses its whole 0..255
 * range steps by at most 2 levels per sample -- so nearest-neighbour cannot
 * introduce a step the ramp did not already have. That is the one case where
 * stretching is provably safe, and it is exactly the opposite of the rule for
 * corner MASKS (generated at device size, never stretched, because a mask of a
 * SHARP shape does not survive it). 64 KiB of .bss; the alternative -- a
 * full-size surface -- is 2.8 MB for one hero gradient. */
#define GSURF_MAX 128
static unsigned char gsurf[GSURF_MAX * GSURF_MAX * 4];
/* `* 2` because gfx_path_init's capacity is in POINTS and the buffer holds x
 * and y interleaved -- the convention c/lib/gfx/gfx_mask.c's `cpt[512 * 2]`
 * with `gfx_path_init(&p, cpt, 512, ...)` sets, and the one this file's own
 * ctl_pt[] got wrong (see the note there). 64 points is a rounded rect with
 * room: four kappa cubics flatten to about 12 segments each at this
 * tolerance. */
static int grad_pt[64 * 2], grad_sub[8];

/* t at a device point, in the engine's own arithmetic. Copied deliberately
 * from gfx_paint.c's GFX_LINEAR row loop rather than approximated: a corner
 * tile and the band it abuts must land on the same byte or the seam shows, and
 * "the same formula" is the only way to be sure of that. */
static int grad_t_at(const struct gfx_paint *p, long long px8, long long py8)
{
    long long dx = p->x1 - p->x0, dy = p->y1 - p->y0;
    long long len2 = dx * dx + dy * dy;
    if (len2 == 0) return 0;
    long long t = ((px8 - p->x0) * dx + (py8 - p->y0) * dy) * GFX_MONE / len2;
    return (int)(t < 0 ? 0 : (t > GFX_MONE ? GFX_MONE : t));
}

/* Resolve one stop's colour span. `cur` is the element's own `color`, which is
 * what `currentColor` means and what an unreadable colour falls back to. */
static uint32_t grad_color(const char *s, int len, uint32_t cur, int *alpha)
{
    *alpha = 255;
    if (!s || len <= 0) return cur;
    /* currentColor is resolved HERE and not in css_extra, because it needs the
     * element's computed colour and the capture runs once per sheet. One of
     * the corpus's 385 gradient stops uses it; 782 declarations use it
     * somewhere. */
    if (len == 12) {
        int i = 0;
        const char *k = "currentcolor";
        for (; i < 12; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != k[i]) break;
        }
        if (i == 12) return cur;
    }
    if (!img_css_color) return cur;
    unsigned char rgba[4] = { 0, 0, 0, 255 };
    if (!img_css_color(s, len, rgba)) return cur;
    *alpha = rgba[3];
    return pack_rgb(rgba[0], rgba[1], rgba[2]);
}

/* Build the gfx_paint for `cg` over the box (x,y,w,h) in window POINTS. The
 * axis is written in ABSOLUTE device 24.8, so any sub-rect of the box -- a
 * 9-slice band, a corner tile, a downscaled surface -- can be expressed
 * against it without re-deriving the geometry. Returns 0 if the value is not
 * one we can paint, and 0 must mean "fall back to the background colour". */
static int grad_build(const struct cgradient *cg, int x, int y, int w, int h,
                      uint32_t cur, struct gfx_paint *p)
{
    if (!cg || cg->kind != CG_LINEAR || cg->nstop < 2) return 0;
    int dw = devlen(x, w), dh = devlen(y, h);
    if (dw <= 0 || dh <= 0) return 0;

    /* The unit direction, 16.16, y DOWN. CSS's angle is clockwise from
     * "to top", so the direction is (sin, -cos). */
    int ux, uy;
    if (cg->dir == CG_DIR_CORNER) {
        /* A corner keyword is NOT a fixed angle: CSS Images 3 puts the
         * gradient line perpendicular to the line through the ending corner's
         * two NEIGHBOURS, which is the box's other diagonal -- so it depends
         * on w and h. For `to top right` the other diagonal runs (0,0)->(w,h),
         * and (h,-w) is perpendicular to it and points into the top-right
         * quadrant. No trigonometry is needed for any of the four, which is
         * also why they are kept as a keyword all the way down here rather
         * than converted to an angle at capture time. */
        int vx, vy;
        switch (cg->corner) {
        case CG_CORNER_TR: vx =  dh; vy = -dw; break;
        case CG_CORNER_BR: vx =  dh; vy =  dw; break;
        case CG_CORNER_BL: vx = -dh; vy =  dw; break;
        default:           vx = -dh; vy = -dw; break;   /* CG_CORNER_TL */
        }
        long long d = (long long)gfx_isqrt((unsigned long long)((long long)dw * dw +
                                                                (long long)dh * dh));
        if (d <= 0) return 0;
        ux = (int)((long long)vx * GFX_MONE / d);
        uy = (int)((long long)vy * GFX_MONE / d);
    } else {
        int deg256 = (int)((long long)cg->angle_mdeg * GFX_ONE / 1000);
        ux = gfx_sin(deg256);
        uy = -gfx_cos(deg256);
    }

    /* The gradient line's length: the box's extent projected onto it. */
    long au = ux < 0 ? -ux : ux, av = uy < 0 ? -uy : uy;
    long long L8 = ((long long)dw * au + (long long)dh * av) * 256 / GFX_MONE;  /* 24.8 */
    if (L8 <= 0) return 0;
    long long cx8 = (long long)dev(x) * 256 + (long long)dw * 128;
    long long cy8 = (long long)dev(y) * 256 + (long long)dh * 128;
    long long hx = (L8 / 2) * ux / GFX_MONE, hy = (L8 / 2) * uy / GFX_MONE;
    gfx_paint_linear(p, (int)(cx8 - hx), (int)(cy8 - hy),
                        (int)(cx8 + hx), (int)(cy8 + hy));

    /* Stop positions -> t. A percentage is hundredths of a percent (so 62.5%
     * is 6250 and not 62 -- the units bug this tree already paid for once);
     * a length is CSS px, i.e. POINTS here, measured along the gradient line
     * whose length is L8. An unpositioned first/last stop is 0/100%, and an
     * unpositioned interior run is spread evenly between its defined
     * neighbours, which is the spec's rule and not a convenience. */
    int t[CG_MAXSTOP], have[CG_MAXSTOP];
    int n = cg->nstop;
    for (int i = 0; i < n; i++) {
        have[i] = 1;
        if (cg->stop[i].pos_kind == CG_POS_PCT)
            t[i] = (int)((long long)cg->stop[i].pos * GFX_MONE / 10000);
        else if (cg->stop[i].pos_kind == CG_POS_PX) {
            long long px8 = (long long)cg->stop[i].pos * (scale_pct ? scale_pct : 100) * 256 / 100;
            t[i] = (int)(px8 * GFX_MONE / L8);
        } else if (i == 0)          t[i] = 0;
        else if (i == n - 1)        t[i] = GFX_MONE;
        else                        have[i] = 0;
    }
    for (int i = 1; i < n - 1; i++) {
        if (have[i]) continue;
        int j = i;
        while (j < n && !have[j]) j++;            /* j is the next defined stop */
        int lo = t[i - 1], hi = t[j];
        for (int k = i; k < j; k++) {
            t[k] = lo + (int)((long long)(hi - lo) * (k - i + 1) / (j - i + 1));
            have[k] = 1;
        }
        i = j - 1;
    }
    /* Non-decreasing: CSS clamps a stop up to its predecessor rather than
     * letting the ramp run backwards. */
    for (int i = 1; i < n; i++) if (t[i] < t[i - 1]) t[i] = t[i - 1];

    for (int i = 0; i < n; i++) {
        int a;
        uint32_t c = grad_color(cg->stop[i].color, cg->stop[i].colorlen, cur, &a);
        gfx_paint_stop(p, t[i], c, a);
    }
    return p->nstop >= 2;
}

/* Is the paint's axis exactly along x or exactly along y? Exact, not
 * approximate: gfx_sin(0) is 0 and gfx_sin(90 deg) is 65536 to the bit, so the
 * 349 axis-aligned gradients land here and the 36 diagonal ones do not. */
static int grad_axis(const struct gfx_paint *p, int *vertical)
{
    if (p->x0 == p->x1) { *vertical = 1; return p->y0 != p->y1; }
    if (p->y0 == p->y1) { *vertical = 0; return 1; }
    return 0;
}

/* One axis-aligned band: `n` device samples of the ramp, blitted as a 1 x n
 * (or n x 1) source that the compositor replicates. The samples are taken at
 * the band's OWN device coordinates, so a band of a rounded box gets the slice
 * of the ramp that belongs to it. */
static void grad_band(int x, int y, int w, int h, const struct gfx_paint *p,
                      int vertical, int galpha)
{
    if (w <= 0 || h <= 0 || galpha <= 0) return;
    if (x + w <= cl_x0 || x >= cl_x1 || y + h <= cl_y0 || y >= cl_y1) return;
    int span = vertical ? devlen(y, h) : devlen(x, w);
    if (span <= 0) return;
    int n = span > RAMP_MAX ? RAMP_MAX : span;
    int base = vertical ? dev(y) : dev(x);
    for (int k = 0; k < n; k++) {
        long long d = base + (long long)k * span / n;
        int t = vertical ? grad_t_at(p, p->x0, d * 256 + 128)
                         : grad_t_at(p, d * 256 + 128, p->y0);
        int a;
        unsigned c = gfx_paint_sample(p, t, &a);
        ramp_buf[k * 4 + 0] = (unsigned char)GFX_R(c);
        ramp_buf[k * 4 + 1] = (unsigned char)GFX_G(c);
        ramp_buf[k * 4 + 2] = (unsigned char)GFX_B(c);
        ramp_buf[k * 4 + 3] = (unsigned char)(a * galpha / 255);
    }
    if (vertical) gui_blit(x, y, w, h, ramp_buf, 1, n);
    else          gui_blit(x, y, w, h, ramp_buf, n, 1);
}

/* One corner tile, tinted per pixel by the gradient at that pixel's own device
 * coordinate. Same evaluator as grad_band, so the arc and the band it meets
 * agree byte for byte. */
static void grad_corner(int x, int y, int r, const unsigned char *cov, int cw, int ch,
                        const struct gfx_paint *p, int fx, int fy, int galpha)
{
    if (!cov || cw <= 0 || ch <= 0 || r <= 0) return;
    if ((long)cw * ch * 4 > (long)sizeof corner_rgba) return;
    int bx = dev(x), by = dev(y);
    for (int j = 0; j < ch; j++) {
        int sj = fy ? ch - 1 - j : j;
        unsigned char *d = corner_rgba + (long)j * cw * 4;
        const unsigned char *s = cov + (long)sj * cw;
        for (int i = 0; i < cw; i++) {
            int si = fx ? cw - 1 - i : i;
            int t = grad_t_at(p, (long long)(bx + i) * 256 + 128,
                                 (long long)(by + j) * 256 + 128);
            int a;
            unsigned c = gfx_paint_sample(p, t, &a);
            d[i * 4 + 0] = (unsigned char)GFX_R(c);
            d[i * 4 + 1] = (unsigned char)GFX_G(c);
            d[i * 4 + 2] = (unsigned char)GFX_B(c);
            d[i * 4 + 3] = (unsigned char)(s[si] * a / 255 * galpha / 255);
        }
    }
    gui_blit(x, y, r, r, corner_rgba, cw, ch);
}

/* The diagonal path: composite the paint through a real path into a surface
 * and blit it. This is gfx_fill()'s shape -- a paint, a path and a rule -- and
 * the only place in the browser that has ever asked for it. */
static void grad_surface(int x, int y, int w, int h, int rpt,
                         const struct gfx_paint *P, int galpha)
{
    int dw = devlen(x, w), dh = devlen(y, h);
    if (dw <= 0 || dh <= 0 || galpha <= 0) return;
    if (x + w <= cl_x0 || x >= cl_x1 || y + h <= cl_y0 || y >= cl_y1) return;
    int sw = dw, sh = dh, down = 0;
    while (sw > GSURF_MAX || sh > GSURF_MAX) {
        sw = (sw + 1) / 2; sh = (sh + 1) / 2; down = 1;
    }
    if (sw <= 0 || sh <= 0) return;
    /* The paint's axis, mapped from absolute device into this surface's
     * coordinates. A scale and a translate, so the ramp keeps running in the
     * same direction at the same rate relative to the box. */
    struct gfx_paint p = *P;
    long long ox = (long long)dev(x) * 256, oy = (long long)dev(y) * 256;
    p.x0 = (int)((P->x0 - ox) * sw / dw); p.y0 = (int)((P->y0 - oy) * sh / dh);
    p.x1 = (int)((P->x1 - ox) * sw / dw); p.y1 = (int)((P->y1 - oy) * sh / dh);
    p.global_alpha = galpha;
    struct gfx_surface s;
    gfx_surface_init(&s, gsurf, sw, sh, sw * 4);
    gfx_surface_clear(&s);
    struct gfx_path pa;
    gfx_path_init(&pa, grad_pt, 64, grad_sub, 8);
    /* The radius is honoured only at 1:1. Downscaled, an antialiased corner is
     * a SHARP shape being stretched -- the one thing the mask discipline
     * forbids -- so a big rounded box is painted band-by-band with device-size
     * corner tiles instead, and this path is called with rpt = 0. */
    int rs = down ? 0 : devlen(x, rpt);
    if (rs > 0) gfx_path_rrect(&pa, 0, 0, sw * GFX_ONE, sh * GFX_ONE, rs * GFX_ONE);
    else        gfx_path_rect(&pa, 0, 0, sw * GFX_ONE, sh * GFX_ONE);
    gfx_fill(&s, &pa, GFX_NONZERO, &p, 0);
    gui_blit(x, y, w, h, gsurf, sw, sh);
}

/* Paint the element's background gradient over its border box. Returns 0 when
 * there is no gradient to paint or it is one we refuse, and the caller then
 * paints the background COLOUR exactly as it did before -- never something
 * approximate. */
static int paint_gradient(const struct item *e, int sx, int sy, int r, int op)
{
    const struct cstyle *st = sty(e);
    if (!st || !css_gradient_parse || !st->xraw[XR_BG_IMAGE] || op <= 0) return 0;
    struct cgradient cg;
    if (!css_gradient_parse(st->xraw[XR_BG_IMAGE], st->xrawlen[XR_BG_IMAGE],
                            st->font_px > 0 ? st->font_px : 16, root_px(), &cg))
        return 0;
    struct gfx_paint p;
    if (!grad_build(&cg, sx, sy, e->w, e->h, st->color, &p)) return 0;

    int vertical = 0;
    int axis = grad_axis(&p, &vertical);
    if (r <= 0) {
        if (axis) grad_band(sx, sy, e->w, e->h, &p, vertical, op);
        else      grad_surface(sx, sy, e->w, e->h, 0, &p, op);
        return 1;
    }
    int cw = devlen(sx, r), ch = devlen(sy, r);
    const unsigned char *cov = gfx_mask_corner(GFX_MASK_FILL, cw, ch, 0);
    if (!cov) {
        /* The tile ceiling. Square corners on a gradient is gfx_mask.c's
         * acceptable degradation -- complete, already counted there -- and
         * skipping the gradient entirely would silently hand the box back to
         * its flat background colour, which looks like the gradient was never
         * declared. */
        if (axis) grad_band(sx, sy, e->w, e->h, &p, vertical, op);
        else      grad_surface(sx, sy, e->w, e->h, 0, &p, op);
        return 1;
    }
    if (axis) {
        grad_band(sx + r, sy,             e->w - 2 * r, r,             &p, vertical, op);
        grad_band(sx,     sy + r,         e->w,         e->h - 2 * r,  &p, vertical, op);
        grad_band(sx + r, sy + e->h - r,  e->w - 2 * r, r,             &p, vertical, op);
    } else {
        grad_surface(sx + r, sy,            e->w - 2 * r, r,            0, &p, op);
        grad_surface(sx,     sy + r,        e->w,         e->h - 2 * r, 0, &p, op);
        grad_surface(sx + r, sy + e->h - r, e->w - 2 * r, r,            0, &p, op);
    }
    grad_corner(sx,             sy,            r, cov, cw, ch, &p, 0, 0, op);
    grad_corner(sx + e->w - r,  sy,            r, cov, cw, ch, &p, 1, 0, op);
    grad_corner(sx,             sy + e->h - r, r, cov, cw, ch, &p, 0, 1, op);
    grad_corner(sx + e->w - r,  sy + e->h - r, r, cov, cw, ch, &p, 1, 1, op);
    return 1;
}

/* ---- the ROUNDED overflow clip: path clipping's first consumer -----------
 *
 * `overflow:hidden` on a box with a `border-radius` is coverage TIMES
 * coverage, which is exactly what gfx_fill_mask_clipped does and what nothing
 * in this tree called. The engine grew it in phase 2 with its own independent
 * oracle and its own gate (test-gfx-clip, run by every test-gfx), and it had
 * zero callers -- a correct, gated, dead feature.
 *
 * The item already carries the clip RECTANGLE (`has_clip` + clip_x/y/w/h, in
 * document coordinates), which layout stamps per item because a flat display
 * list has no box tree to walk back up. What it does not carry is the
 * clipper's RADIUS, so that is fetched from the DOM: the nearest ancestor
 * whose overflow is not `visible`. The ancestor's own border box must equal
 * the stamped clip rect before its radius is used -- layout INTERSECTS nested
 * clips, and a radius belonging to a box bigger than the surviving rectangle
 * would round corners that are not there.
 *
 * BOUNDED BY THE RADIUS, NOT BY THE BOX, which is the whole reason this is
 * affordable. Only the four r x r corner squares need the path clip; every
 * other pixel of the item is exactly the rectangular clip it already had. So
 * the cost is O(r^2) with r a border-radius -- the same argument that makes a
 * rounded rect a 9-slice rather than a full rasterization. A clip radius past
 * XF_MAX device pixels falls back to the rectangular clip and is counted. */
/* The bound shared by everything in this file that rasterizes a shape bigger
 * than a corner tile: the transformed-box mask below and the clip coverage
 * here. DEVICE pixels a side. Past it the feature degrades to something
 * complete and the degradation is counted -- gfx_mask.c's contract, one level
 * up. 128 x 128 coverage is 16 KiB and its RGBA companion 64 KiB; the sizes
 * are what stopped this being 256, which would have been 320 KiB of .bss in a
 * binary whose whole .bss the libc line has just spent effort shrinking. */
#define XF_MAX 128
static unsigned char xf_cov[XF_MAX * XF_MAX];
static unsigned char xf_rgba[XF_MAX * XF_MAX * 4];
static int xf_ptbuf[256 * 2], xf_subbuf[8];

/* The clip rectangle in window POINTS, the radius to follow, and WHICH of its
 * four corners are really the clipper's own rounded corners.
 *
 * The per-corner flag is not fastidiousness, it is the difference between this
 * feature firing and not firing, and the number is measured. layout's clip
 * (clip_push, layout.c) is the PADDING box, it is INTERSECTED with every
 * ancestor clip, and its bottom is 0x3FFFFFFF whenever the height is auto --
 * so a whole-rectangle equality test against the clipper's box matches almost
 * nothing. Over the 15 captured pages in tests/fixtures/cssweb, 682 painted
 * boxes carry a clip and every one of them finds an overflow ancestor; a
 * whole-rectangle test on the BORDER box matched 3 of them. Testing each EDGE
 * against the PADDING box matches 179 edges, which is 100 real corners on 74
 * items. Same corpus, same walk, two orders of magnitude apart -- and the
 * version that matched three would have looked exactly like a feature the web
 * does not use. */
struct rclip {
    int x, y, w, h, r;
    unsigned char corner[4];        /* indexed fy*2 + fx, as the loops below */
};

static unsigned char rc_cov[XF_MAX * XF_MAX];   /* the CLIP's corner coverage */
static int g_rc_applied, g_rc_refused;

void browser_paint_rclip_stats(int *applied, int *refused)
{
    if (applied) *applied = g_rc_applied;
    if (refused) *refused = g_rc_refused;
}

/* The rounded clip in force for this item, or 0. */
static int rclip_of(const struct item *e, int vx, int vy, int scroll, struct rclip *out)
{
    if (!e->has_clip) return 0;
    struct node *n = e->node;
    while (n && n->type != N_ELEM) n = n->parent;
    for (struct node *p = n; p; p = p->parent) {
        if (p->type != N_ELEM || !p->style) continue;
        const struct cstyle *s = (const struct cstyle *)p->style;
        if (s->overflow_x == OVF_VISIBLE && s->overflow_y == OVF_VISIBLE) continue;
        int bx, by, bw, bh;
        if (!layout_node_box(p, &bx, &by, &bw, &bh)) return 0;
        int r = s->radius_pct ? (bw < bh ? bw : bh) * s->radius_pct / 100 : s->radius;
        int maxr = (bw < bh ? bw : bh) / 2;
        if (r > maxr) r = maxr;
        if (r <= 0) return 0;
        /* The PADDING box: what CSS clips overflow to and what layout stamped.
         * Its corner curvature is the border-radius REDUCED BY THE BORDER
         * WIDTH -- a 12px radius inside a 4px border leaves an 8px arc at the
         * padding edge, and using 12 there would cut a visible bite out of the
         * content. One width for both axes, which is this painter's standing
         * approximation for borders everywhere (see the rounded branch in the
         * IT_RECT case); with no border, which is the ordinary case for a
         * rounded scroller, it is exact. */
        int bt = s->border_w[0], brr = s->border_w[1];
        int bb = s->border_w[2], bl = s->border_w[3];
        int px0 = bx + bl, py0 = by + bt;
        int px1 = bx + bw - brr, py1 = by + bh - bb;
        /* An EDGE of the stamped clip counts only where it coincides with the
         * clipper's own padding edge; anywhere else an ancestor clip cut it
         * and the arc is not there. */
        int L = (e->clip_x == px0), T = (e->clip_y == py0);
        int R = (e->clip_x + e->clip_w == px1), B = (e->clip_y + e->clip_h == py1);
        /* A corner survives only if BOTH its edges are the clipper's own AND
         * the border it sits in leaves an arc at the padding edge. The border
         * width is taken PER CORNER, from the two edges that meet there --
         * `border-left: 8px` does not flatten the top-RIGHT corner, and using
         * the widest of the four (which this did at first) threw away 52 of
         * the corpus's 74 candidate items to a border on the other side of the
         * box. `r` itself is single-valued, so the wider of the two adjacent
         * edges is what it loses: CSS reduces each AXIS separately and would
         * give an ellipse, which is expressible here (the tiles are cw x ch)
         * and is not worth a second radius for the sub-pixel it buys. */
        int wtl = bl > bt ? bl : bt, wtr = brr > bt ? brr : bt;
        int wbl = bl > bb ? bl : bb, wbr = brr > bb ? brr : bb;
        out->corner[0] = (unsigned char)(L && T && r > wtl);   /* fx=0 fy=0 */
        out->corner[1] = (unsigned char)(R && T && r > wtr);   /* fx=1 fy=0 */
        out->corner[2] = (unsigned char)(L && B && r > wbl);   /* fx=0 fy=1 */
        out->corner[3] = (unsigned char)(R && B && r > wbr);   /* fx=1 fy=1 */
        if (!(out->corner[0] | out->corner[1] | out->corner[2] | out->corner[3]))
            return 0;
        /* One radius for all the surviving corners: the smallest arc any of
         * them leaves, so no corner is ever drawn with more curve than its own
         * border permits. */
        int wsub = 0;
        if (out->corner[0] && wtl > wsub) wsub = wtl;
        if (out->corner[1] && wtr > wsub) wsub = wtr;
        if (out->corner[2] && wbl > wsub) wsub = wbl;
        if (out->corner[3] && wbr > wsub) wsub = wbr;
        r -= wsub;
        if (r <= 0) return 0;
        /* The geometry the corners sit on is the CLIP RECTANGLE -- which, at
         * every corner that survived the test above, IS the padding box's. */
        out->x = vx + e->clip_x; out->y = vy + e->clip_y - scroll;
        out->w = e->clip_w; out->h = e->clip_h;
        /* clip_push leaves the bottom at 0x3FFFFFFF for an auto height, so the
         * stamped height is routinely a billion. Clamped, because `y + h` is
         * computed below and signed overflow is undefined -- and clamping is
         * safe precisely because a clip that reaches 0x3FFFFFFF has no bottom
         * edge coinciding with anything, so its bottom corners were not
         * flagged and nothing below reads that edge for a shape. */
        if (out->w > (1 << 24)) out->w = 1 << 24;
        if (out->h > (1 << 24)) out->h = 1 << 24;
        int m2 = (out->w < out->h ? out->w : out->h) / 2;
        if (r > m2) r = m2;
        if (r <= 0) return 0;
        out->r = r;
        return 1;
    }
    return 0;
}

/* Does this item's box reach into any of the clip's four rounded corners? If
 * not -- which is the case for almost every item inside a scroller -- nothing
 * below runs and the ordinary rectangular clip is exactly right. */
static int rclip_touches(int x, int y, int w, int h, const struct rclip *rc)
{
    int r = rc->r;
    for (int k = 0; k < 4; k++) {
        if (!rc->corner[k]) continue;
        int fx = k & 1, fy = k >> 1;
        int cx = fx ? rc->x + rc->w - r : rc->x;
        int cy = fy ? rc->y + rc->h - r : rc->y;
        if (x < cx + r && x + w > cx && y < cy + r && y + h > cy) return 1;
    }
    return 0;
}

/* One corner of the clip, as a coverage buffer the engine can multiply
 * through. gfx_mask_corner only ever produces the TOP-LEFT quadrant (one
 * rasterization serves four corners by mirroring at blit time), and
 * gfx_clip_mask reads a plain buffer with no mirroring of its own -- so the
 * mirror is materialised here. Copying r^2 bytes is cheaper than rasterizing
 * three more quadrants and is why the cache stays a cache. */
static const unsigned char *rc_corner(int cw, int ch, int fx, int fy)
{
    if (cw <= 0 || ch <= 0 || cw > XF_MAX || ch > XF_MAX) return 0;
    const unsigned char *q = gfx_mask_corner(GFX_MASK_FILL, cw, ch, 0);
    if (!q) return 0;
    if (!fx && !fy) return q;
    for (int j = 0; j < ch; j++) {
        const unsigned char *s = q + (long)(fy ? ch - 1 - j : j) * cw;
        unsigned char *d = rc_cov + (long)j * cw;
        for (int i = 0; i < cw; i++) d[i] = s[fx ? cw - 1 - i : i];
    }
    return rc_cov;
}

/* Fill (x,y,w,h) clipped to the clip's ROUNDED outline. Three bands are
 * ordinary rectangles; the up-to-four corner squares the item reaches go
 * through gfx_fill_mask_clipped, whose coverage multiply is the exact answer
 * where the item's own edge cuts across the clip's arc. */
static void fill_rclip(int x, int y, int w, int h, uint32_t color, int alpha,
                       const struct rclip *rc)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (!rclip_touches(x, y, w, h, rc)) { fill(x, y, w, h, color, alpha); return; }
    int r = rc->r;
    int cw = devlen(rc->x, r), ch = devlen(rc->y, r);
    /* DECIDE THE CORNER SOURCE BEFORE DRAWING ANYTHING. The bands below
     * deliberately exclude the four corner squares, so a corner that turns out
     * to be unavailable leaves an actual HOLE in the fill -- not a square
     * corner, a gap. That is the "drops geometry" failure gfx_mask.c's
     * contract says is never acceptable, and it is the same bug aui.c's
     * aui_vgrad_round paid for and fixed the same way. Two ceilings apply and
     * the tighter one is not this file's: gfx_mask_corner refuses past
     * GFX_MASK_MAX (72 device px), well below XF_MAX, so between the two a
     * NULL would arrive with the local bound satisfied. Asking for the mask
     * first is what makes a clean whole-rect fallback possible instead. */
    if (cw <= 0 || ch <= 0 || cw > XF_MAX || ch > XF_MAX ||
        !gfx_mask_corner(GFX_MASK_FILL, cw, ch, 0)) {
        /* The rectangular clip is what this box had before and it is complete
         * -- square corners on a rounded scroller, which is a wrong picture
         * nobody can mistake for a missing one. */
        g_rc_refused++;
        fill(x, y, w, h, color, alpha);
        return;
    }
    g_rc_applied++;

    /* The three bands, each the ITEM intersected with one band of the CLIP
     * minus its corner squares. The intersection is written out rather than
     * left to fill()'s device clip: that clip is the clip RECTANGLE, so it
     * bounds the bands to the clipper but not to the item, and a band drawn to
     * the clipper's full width would paint the item's colour where the item is
     * not. */
    int ix0 = x, ix1 = x + w, iy0 = y, iy1 = y + h;
    int mx0 = rc->x + r, mx1 = rc->x + rc->w - r;
    int sx0 = ix0 > mx0 ? ix0 : mx0, sx1 = ix1 < mx1 ? ix1 : mx1;
    /* top and bottom bands: the middle span only, so the corner squares below
     * are the ONLY thing that ever paints inside them. */
    if (sx1 > sx0) {
        int t0 = iy0 > rc->y ? iy0 : rc->y, t1 = iy1 < rc->y + r ? iy1 : rc->y + r;
        if (t1 > t0) fill(sx0, t0, sx1 - sx0, t1 - t0, color, alpha);
        int b0 = iy0 > rc->y + rc->h - r ? iy0 : rc->y + rc->h - r;
        int b1 = iy1 < rc->y + rc->h ? iy1 : rc->y + rc->h;
        if (b1 > b0) fill(sx0, b0, sx1 - sx0, b1 - b0, color, alpha);
    }
    /* the middle band: full width, no corners in it at all */
    {
        int m0 = iy0 > rc->y + r ? iy0 : rc->y + r;
        int m1 = iy1 < rc->y + rc->h - r ? iy1 : rc->y + rc->h - r;
        if (m1 > m0) fill(ix0, m0, ix1 - ix0, m1 - m0, color, alpha);
    }

    for (int k = 0; k < 4; k++) {
        int fx = k & 1, fy = k >> 1;
        int cx = fx ? rc->x + rc->w - r : rc->x;
        int cy = fy ? rc->y + rc->h - r : rc->y;
        if (x >= cx + r || x + w <= cx || y >= cy + r || y + h <= cy) continue;
        /* A corner square the bands skipped and the clipper does NOT round --
         * an edge the ancestor clip cut, so there is no arc there. It still
         * has to be painted, or the band decomposition leaves a square hole in
         * the middle of a flat fill. */
        if (!rc->corner[k]) { fill(cx, cy, r, r, color, alpha); continue; }
        const unsigned char *m = rc_corner(cw, ch, fx, fy);
        /* Cannot be NULL: the same geometry was asked for and granted above,
         * and the cache is keyed by exact geometry so the second ask is a hit.
         * Handled anyway, and as a FILL rather than a skip -- the bands left
         * this square empty, so `continue` here would be the hole the check
         * above exists to prevent. */
        if (!m) { fill(cx, cy, r, r, color, alpha); continue; }
        struct gfx_clip_mask cm;
        cm.cov = m; cm.w = cw; cm.h = ch; cm.ox = dev(cx); cm.oy = dev(cy);
        /* The SUBJECT is the item's own rectangle, in absolute device
         * coordinates. Where it covers the whole corner square the product is
         * the clip's coverage; where its edge cuts across the arc the product
         * is what neither shape alone can express, and that is the case this
         * entry point exists for. */
        struct gfx_path p;
        gfx_path_init(&p, xf_ptbuf, 256, xf_subbuf, 8);
        gfx_path_rect(&p, dev(x) * GFX_ONE, dev(y) * GFX_ONE,
                          devlen(x, w) * GFX_ONE, devlen(y, h) * GFX_ONE);
        if (!gfx_fill_mask_clipped(&p, GFX_NONZERO, xf_cov, cw, ch,
                                   dev(cx), dev(cy), GFX_SUBS, &cm)) {
            /* The engine refused the path. Same rule as the NULL above: the
             * bands did not paint this square, so falling through would leave
             * a gap rather than a square corner. */
            fill(cx, cy, r, r, color, alpha);
            continue;
        }
        gfx_mask_to_rgba(xf_rgba, xf_cov, cw, ch, color, alpha, 0, 0);
        gui_blit(cx, cy, r, r, xf_rgba, cw, ch);
    }
}

/* The same clip for an IMAGE, and it is a different mechanism for a reason
 * that is about the ABI and not about the engine.
 *
 * gui_blit stretches its WHOLE source over the destination rect and takes no
 * source sub-rectangle, so an image cannot be painted band by band the way a
 * colour can -- each band would need a slice of the bitmap and there is no
 * call that expresses one. Compositing the image through the clip into a
 * surface is the other road and it is bounded by the IMAGE's size, not the
 * radius, which is the wrong bound: a 900 x 600 photo in a rounded card is the
 * ordinary case.
 *
 * So the image is blitted whole, as before, and the four corner squares get
 * the BACKDROP washed back over them through the INVERSE of the clip's
 * coverage. That is the same composition this file already does for image
 * opacity ten lines below -- img*a + backdrop*(1-a) -- with the clip's
 * antialiased arc as the alpha, and it is exact wherever the backdrop estimate
 * is right, which is the same condition group opacity already lives under. */
static void img_rclip(int x, int y, int w, int h, uint32_t backdrop,
                      const struct rclip *rc)
{
    if (!rclip_touches(x, y, w, h, rc)) return;
    int r = rc->r;
    int cw = devlen(rc->x, r), ch = devlen(rc->y, r);
    /* Same two ceilings as fill_rclip, but a refusal here is benign: the image
     * has already been blitted whole, so not washing its corners leaves the
     * SQUARE corners this painter drew before -- complete, and counted. */
    if (cw <= 0 || ch <= 0 || cw > XF_MAX || ch > XF_MAX ||
        !gfx_mask_corner(GFX_MASK_FILL, cw, ch, 0)) { g_rc_refused++; return; }
    g_rc_applied++;
    for (int k = 0; k < 4; k++) {
        int fx = k & 1, fy = k >> 1;
        int cx = fx ? rc->x + rc->w - r : rc->x;
        int cy = fy ? rc->y + rc->h - r : rc->y;
        if (x >= cx + r || x + w <= cx || y >= cy + r || y + h <= cy) continue;
        if (!rc->corner[k]) continue;    /* no arc there: the square edge is right */
        const unsigned char *m = rc_corner(cw, ch, fx, fy);
        if (!m) continue;
        for (long i = 0; i < (long)cw * ch; i++) xf_cov[i] = (unsigned char)(255 - m[i]);
        gfx_mask_to_rgba(xf_rgba, xf_cov, cw, ch, backdrop, 255, 0, 0);
        gui_blit(cx, cy, r, r, xf_rgba, cw, ch);
    }
}

/* ---- transform -----------------------------------------------------------
 *
 * The parser (css_interp.c) and the affine layer (c/lib/gfx) have both existed
 * for a long time with nothing between them. This is the connection, and it is
 * a connection rather than an implementation: no matrix arithmetic is written
 * here that either side already owns.
 *
 * A transform belongs to an ELEMENT and applies to its whole SUBTREE, and the
 * display list is flat -- an item does not know it has a transformed ancestor.
 * So the chain is walked from the item's node upward. The walk is a pointer
 * test per ancestor (`style->xraw[XR_TRANSFORM] != NULL`) and nothing more
 * until one is found, which is the ordinary case for every item on every page:
 * three thousand items times a depth of fifteen is forty-five thousand
 * dereferences a frame and no parsing at all. When one IS found the result is
 * memoised on the item's own element, because consecutive items in a display
 * list overwhelmingly share their ancestry.
 *
 * WHAT IS EXACT AND WHAT IS NOT, and the split is chosen from what the web
 * actually writes rather than from what is easy:
 *
 *   b == 0 && c == 0  -- no rotation and no skew, i.e. TRANSLATE and SCALE.
 *       An axis-aligned box maps to an axis-aligned box, so every item type is
 *       handled EXACTLY by mapping its two corners: no mask, no bound, no
 *       size limit. Text additionally scales its font size by the matrix's
 *       vertical factor, which is what `scale()` on a heading means. This is
 *       the case CLAUDE.md measured as reached by 14 of 15 real pages
 *       (`translate(-50%,-50%)` centring) and it costs two multiplies.
 *
 *   otherwise -- rotation or skew. An IT_RECT is rasterized as a TRANSFORMED
 *       PATH through the engine, bounded by XF_MAX device pixels a side;
 *       past that, and for every other item type, the box is placed at its
 *       mapped origin and drawn unrotated. Glyphs are the honest limit here:
 *       gui_text_run takes an (x, y, px) and there is no rotated text
 *       primitive in the ABI to hand a matrix to.
 *
 * NOTE THE ENGINE CONTRACT, which points the opposite way from js_canvas.c's.
 * gfx_path_matrix() refuses a MID-BUILD call, so canvas -- where the CTM in
 * force when each point is added is what transforms it -- has to leave the
 * path at identity and transform every point itself. CSS is the other case:
 * one matrix for the whole shape, known before the first point. So here the
 * matrix goes on the path and gfx_path_rrect's arcs are flattened under it,
 * which is both the shorter code and the more accurate one -- flattening
 * happens in DEVICE space, so a scaled-up corner gets the segments it needs
 * instead of the segments its untransformed size would have earned. */
static int g_xf_unbounded;        /* rotated boxes too big for the mask */
static int g_xf_untransformed;    /* non-rect items placed but not rotated */

void browser_paint_xform_stats(int *unbounded, int *unrotated)
{
    if (unbounded)  *unbounded = g_xf_unbounded;
    if (unrotated)  *unrotated = g_xf_untransformed;
}

/* 24.8 -> integer, round to nearest, symmetric about zero. */
static int p8(int v) { return v >= 0 ? (v + 128) >> 8 : -((-v + 128) >> 8); }

/* double -> 16.16 and -> 24.8, clamped. A transform list is author data, so a
 * scale(1e9) is a value a page can write and an int is not a place it fits. */
static int d16(double v)
{
    if (v > 30000.0) v = 30000.0;
    if (v < -30000.0) v = -30000.0;
    return (int)(v * 65536.0);
}
static int d8(double v)
{
    if (v > 8000000.0) v = 8000000.0;
    if (v < -8000000.0) v = -8000000.0;
    return (int)(v * 256.0);
}

static struct node *g_xf_key;
static struct gfx_matrix g_xf_val;
static int g_xf_hit;              /* 1 = g_xf_val is real, 0 = no transform */

/* The transform in force for this item, expressed in WINDOW POINTS. 0 when
 * there is none, which is the answer for almost every item on almost every
 * page and is the only answer the fast reject ever has to produce. */
static int item_xform(const struct item *e, int vx, int vy, int scroll,
                      struct gfx_matrix *out)
{
    struct node *n = e ? e->node : 0;
    while (n && n->type != N_ELEM) n = n->parent;
    if (!n || !ci_transform_parse || !ci_transform_matrix) return 0;
    if (n == g_xf_key) {
        if (g_xf_hit) *out = g_xf_val;
        return g_xf_hit;
    }
    g_xf_key = n; g_xf_hit = 0;

    int any = 0;
    for (struct node *p = n; p; p = p->parent) {
        const struct cstyle *s = (p->type == N_ELEM) ? (const struct cstyle *)p->style : 0;
        if (s && s->xraw[XR_TRANSFORM]) { any = 1; break; }
    }
    if (!any) return 0;

    struct gfx_matrix acc;
    gfx_m_identity(&acc);
    int found = 0;
    for (struct node *p = n; p; p = p->parent) {
        const struct cstyle *s = (p->type == N_ELEM) ? (const struct cstyle *)p->style : 0;
        if (!s || !s->xraw[XR_TRANSFORM]) continue;
        int bx, by, bw, bh;
        if (!layout_node_box(p, &bx, &by, &bw, &bh)) continue;
        int fs = s->font_px > 0 ? s->font_px : 16;
        struct ci_xform xf;
        if (ci_transform_parse(s->xraw[XR_TRANSFORM], s->xrawlen[XR_TRANSFORM],
                               (double)fs, (double)root_px(), &xf) != 0) continue;
        if (xf.n == 0) continue;                       /* `none` is the identity */
        double m[16];
        /* Percentages in translate() resolve against the element's OWN border
         * box, which is why the box is fetched per ancestor and not taken from
         * the item -- `translate(-50%)` on a card and on the text inside it are
         * different distances. */
        ci_transform_matrix(&xf, (double)bw, (double)bh, m);
        struct gfx_matrix A;
        gfx_m_set(&A, d16(m[0]), d16(m[1]), d16(m[4]), d16(m[5]), d8(m[12]), d8(m[13]));

        struct corigin o;
        o.x = o.y = 5000; o.x_pct = o.y_pct = 1;       /* the CSS initial, 50% 50% */
        if (css_origin_parse)
            css_origin_parse(s->xraw[XR_TRANSFORM_ORIGIN], s->xrawlen[XR_TRANSFORM_ORIGIN],
                             fs, root_px(), &o);
        int ox = o.x_pct ? (int)((long long)bw * o.x / 10000) : o.x;
        int oy = o.y_pct ? (int)((long long)bh * o.y / 10000) : o.y;
#ifdef PAINT_NEGCTL_XF_NO_ORIGIN
        /* NEGATIVE CONTROL (test-paint-gfx-negctl). Transform about the box's
         * TOP-LEFT corner instead of its transform-origin. The plausible wrong
         * implementation: it is what falls out of applying the matrix to the
         * item's coordinates directly, every translate() is unaffected (the
         * origin cancels), and only rotate/scale move -- so a page full of
         * `translate(-50%,-50%)` centring still looks perfect and every
         * rotated badge is in the wrong place. */
        ox = 0; oy = 0;
#endif
        int wx = vx + bx + ox, wy = vy + by - scroll + oy;

        struct gfx_matrix T;
        gfx_m_identity(&T);
        gfx_m_translate(&T, wx * GFX_ONE, wy * GFX_ONE);
        gfx_m_mul(&T, &T, &A);
        gfx_m_translate(&T, -wx * GFX_ONE, -wy * GFX_ONE);
        /* The walk runs inner -> outer, and an outer transform applies AFTER
         * an inner one, so each ancestor's matrix is composed on the LEFT. */
        gfx_m_mul(&acc, &T, &acc);
        found = 1;
    }
    if (!found) return 0;
    g_xf_val = acc; g_xf_hit = 1;
    *out = acc;
    return 1;
}

/* No rotation and no skew, i.e. a translate/scale/flip. The paint loop maps
 * all four corners regardless of which case this is -- two are not enough
 * under a rotation, and a negative scale (`scaleX(-1)`, a real declaration)
 * delivers them out of order -- so this only chooses between the exact
 * placement and the rasterized path, not between two geometries. */
static int xf_is_axis(const struct gfx_matrix *m)
{ return m->b == 0 && m->c == 0; }

/* A rotated/skewed IT_RECT: the rounded rect as a PATH under the matrix, one
 * rasterization, one blit. Returns 0 when the shape does not fit the bounded
 * mask, and the caller then falls back to the axis-aligned placement -- which
 * is complete and in the right place, just not rotated, and is counted. */
static int paint_rect_xf(const struct item *e, int sx, int sy, int r, int op,
                         const struct gfx_matrix *m, uint32_t color, int alpha)
{
    if (e->w <= 0 || e->h <= 0 || alpha <= 0) return 1;
    /* The transformed bounding box, from all four corners: under a rotation
     * two corners are not enough. */
    int px[4], py[4];
    const int cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) {
        gfx_m_apply(m, (sx + cx[i] * e->w) * GFX_ONE, (sy + cy[i] * e->h) * GFX_ONE,
                    &px[i], &py[i]);
        px[i] = p8(px[i]); py[i] = p8(py[i]);
    }
    int x0 = px[0], x1 = px[0], y0 = py[0], y1 = py[0];
    for (int i = 1; i < 4; i++) {
        if (px[i] < x0) x0 = px[i];
        if (px[i] > x1) x1 = px[i];
        if (py[i] < y0) y0 = py[i];
        if (py[i] > y1) y1 = py[i];
    }
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) return 1;
    int cw = devlen(x0, bw), chh = devlen(y0, bh);
    if (cw <= 0 || chh <= 0) return 1;
    if (cw > XF_MAX || chh > XF_MAX) { g_xf_unbounded++; return 0; }
    if (x0 + bw <= cl_x0 || x0 >= cl_x1 || y0 + bh <= cl_y0 || y0 >= cl_y1) return 1;

    /* device = S * T(-bbox) * M * T(box origin), built right-to-left because
     * gfx_m_translate post-multiplies (it applies its translation FIRST). */
    int s16 = (scale_pct ? scale_pct : 100) * 65536 / 100;
    struct gfx_matrix q;
    gfx_m_identity(&q);
    gfx_m_scale(&q, s16, s16);
    gfx_m_translate(&q, -x0 * GFX_ONE, -y0 * GFX_ONE);
    gfx_m_mul(&q, &q, m);
    gfx_m_translate(&q, sx * GFX_ONE, sy * GFX_ONE);

    struct gfx_path p;
    gfx_path_init(&p, xf_ptbuf, 256, xf_subbuf, 8);
    gfx_path_matrix(&p, &q);            /* before the first point: allowed */
    if (r > 0) gfx_path_rrect(&p, 0, 0, e->w * GFX_ONE, e->h * GFX_ONE, r * GFX_ONE);
    else       gfx_path_rect(&p, 0, 0, e->w * GFX_ONE, e->h * GFX_ONE);
    if (!gfx_fill_mask(&p, GFX_NONZERO, xf_cov, cw, chh, 0, 0)) {
        /* The path refused itself -- storage overflow, or a matrix set
         * mid-build, which cannot happen here. Either way the geometry is not
         * to be trusted and the engine says so; falling back is the caller's
         * complete answer, not a drawing from an untrusted path. */
        g_xf_unbounded++;
        return 0;
    }
    gfx_mask_to_rgba(xf_rgba, xf_cov, cw, chh, color, alpha, 0, 0);
    gui_blit(x0, y0, bw, bh, xf_rgba, cw, chh);
    (void)op;
    return 1;
}

/* ---- borders ----
 *
 * One edge, with its style. `run` is the edge's length along the box, `thick`
 * its width; `vert` says the edge runs top-to-bottom (left/right edges) so the
 * dash pattern steps in y instead of x. `dark` is set for the edges CSS lights
 * from above treats as being in shadow (top and left for `inset`, bottom and
 * right for `outset`).
 *
 * Approximations, all visible only under a magnifier: dashes are a fixed 3:2
 * on:off ratio rather than the UA-chosen one that divides the edge evenly, and
 * dotted draws squares rather than circles (a 1px or 2px circle IS a square).
 * Corners are mitre-free -- each edge paints its full rectangle, so at a
 * corner the horizontal edge wins wherever they overlap. */
static void border_edge(int x, int y, int run, int thick, int vert,
                        int style, uint32_t color, int alpha, int dark)
{
    if (run <= 0 || thick <= 0) return;
    switch (style) {
    case BS_NONE:
    case BS_HIDDEN:
        return;
    case BS_DOTTED:
    case BS_DASHED: {
        int on = (style == BS_DOTTED) ? thick : thick * 3;
        int off = (style == BS_DOTTED) ? thick : thick * 2;
        if (on < 1) on = 1;
        if (off < 1) off = 1;
        for (int p = 0; p < run; p += on + off) {
            int len = (p + on > run) ? run - p : on;
            if (vert) fill(x, y + p, thick, len, color, alpha);
            else      fill(x + p, y, len, thick, color, alpha);
        }
        return;
    }
    case BS_DOUBLE: {
        /* Two lines with a gap, each a third of the width. Under 3px there is
         * no room for three visible stripes, so it degrades to solid -- which
         * is what every browser does too. */
        int t = thick / 3;
        if (t < 1) { break; }
        if (vert) {
            fill(x, y, t, run, color, alpha);
            fill(x + thick - t, y, t, run, color, alpha);
        } else {
            fill(x, y, run, t, color, alpha);
            fill(x, y + thick - t, run, t, color, alpha);
        }
        return;
    }
    case BS_GROOVE:
    case BS_RIDGE: {
        /* Split the thickness: outer half one tone, inner half the other. The
         * outer tone is dark for groove (a carved channel) and light for ridge
         * on the top/left edges, and swapped on bottom/right. */
        int t = thick / 2;
        if (t < 1) { break; }
        int outer_dark = (style == BS_GROOVE) ? !dark : dark;
        uint32_t a = shade(color, outer_dark ? -35 : 35);
        uint32_t b = shade(color, outer_dark ? 35 : -35);
        if (vert) {
            fill(x, y, t, run, a, alpha);
            fill(x + t, y, thick - t, run, b, alpha);
        } else {
            fill(x, y, run, t, a, alpha);
            fill(x, y + t, run, thick - t, b, alpha);
        }
        return;
    }
    case BS_INSET:
    case BS_OUTSET: {
        int lit = (style == BS_INSET) ? !dark : dark;
        uint32_t c = shade(color, lit ? 35 : -35);
        if (vert) fill(x, y, thick, run, c, alpha);
        else      fill(x, y, run, thick, c, alpha);
        return;
    }
    default:
        break;
    }
    if (vert) fill(x, y, thick, run, color, alpha);
    else      fill(x, y, run, thick, color, alpha);
}

/* ======================== form controls ====================================
 *
 * The chrome a control is drawn with, and where the value goes inside it.
 *
 * NOTHING HERE RASTERIZES. The tick in a checkbox, the dot in a radio and the
 * disclosure triangle in a <select> are PATHS handed to Open Logit
 * (c/lib/gfx), which is the same engine the widget toolkit and this file's
 * rounded corners already go through -- so a checkbox on a web page and a
 * checkbox in a native app are antialiased by the same rule at the same size.
 * Writing a fourth coverage path here is precisely what that engine exists to
 * stop, and a hand-drawn tick out of gui_rect would be the stair-stepped
 * diagonal that gives the whole page away.
 *
 * The palette is the system one, not a per-control invention: a page that does
 * not style its inputs gets the machine's look, and a page that DOES style them
 * gets its own colours (see `authored` at the call site -- an author background
 * or border switches the default chrome off entirely rather than being painted
 * over it).
 */
#define CTL_FIELD_BG   0xFFFFFFu
#define CTL_DIS_BG     0xF1F2F4u
#define CTL_EDGE       0xB0B4BAu
#define CTL_EDGE_FOCUS 0x2F6FEBu
#define CTL_INK        0x1D1D1Fu
#define CTL_INK_DIS    0x9AA0A6u
#define CTL_PLACEHOLD  0x9AA0A6u
#define CTL_BTN_BG     0xF6F7F8u
#define CTL_ACCENT     0x2563EBu
#define CTL_SELBG      0xB4D5FEu

/* Largest shape tile, DEVICE px. A checkbox is 13 pt, so 48 covers 200% with
 * room; past it the shape is skipped rather than overrunning the buffer, which
 * costs a tick and never a corrupted heap. */
#define CTL_SHAPE_MAX 48
static unsigned char ctl_cov[CTL_SHAPE_MAX * CTL_SHAPE_MAX];
static unsigned char ctl_rgba[CTL_SHAPE_MAX * CTL_SHAPE_MAX * 4];
/* A LATENT OVERFLOW, found while adding the buffers above and fixed here.
 * gfx_path_init's third argument is a capacity in POINTS and the buffer holds
 * x and y INTERLEAVED (gfx_path.c's push_pt writes pt[npt*2] and pt[npt*2+1]),
 * so `ctl_pt[128]` with a capacity of 128 described a buffer twice the size of
 * the one that exists. It has never been reached: the widest shape through
 * here is ctl_circle's four kappa cubics, about 50 points at this tolerance,
 * and the two polygons are 6 and 3. It was one bigger control away from
 * writing 512 bytes past a file static, and `overflow` -- the flag that exists
 * to catch exactly this -- would never have fired, because the count it checks
 * was the one that was right. c/lib/gfx/gfx_mask.c has the convention written
 * correctly (`cpt[512 * 2]` with a capacity of 512). */
static int ctl_pt[128 * 2], ctl_sub[8];

/* Rasterize a device-space path into a coverage tile and blit it into the POINT
 * rect (x,y,wpt,hpt) whose device size is exactly (wdev,hdev) -- the mask
 * discipline the engine's header spells out, so the compositor's rescale is the
 * identity and the antialiasing survives at 150%. */
static void shape_fill(struct gfx_path *p, int rule, int x, int y,
                       int wpt, int hpt, int wdev, int hdev, uint32_t color, int alpha)
{
    if (wdev <= 0 || hdev <= 0 || wdev > CTL_SHAPE_MAX || hdev > CTL_SHAPE_MAX) return;
    if (!gfx_fill_mask(p, rule, ctl_cov, wdev, hdev, 0, 0)) return;
    gfx_mask_to_rgba(ctl_rgba, ctl_cov, wdev, hdev, color, alpha, 0, 0);
    gui_blit(x, y, wpt, hpt, ctl_rgba, wdev, hdev);
}

/* A polygon given as N points in 1/1000ths of the tile, so one table describes
 * a shape at every size. */
static void poly_shape(const short *pts, int n, int x, int y, int wpt, int hpt,
                       uint32_t color, int alpha)
{
    int wdev = devlen(x, wpt), hdev = devlen(y, hpt);
    if (wdev <= 0 || hdev <= 0) return;
    struct gfx_path p;
    gfx_path_init(&p, ctl_pt, 128, ctl_sub, 8);
    for (int i = 0; i < n; i++) {
        int px = (int)((long)pts[i * 2] * wdev * GFX_ONE / 1000);
        int py = (int)((long)pts[i * 2 + 1] * hdev * GFX_ONE / 1000);
        if (i == 0) gfx_move_to(&p, px, py);
        else        gfx_line_to(&p, px, py);
    }
    gfx_close(&p);
    shape_fill(&p, GFX_NONZERO, x, y, wpt, hpt, wdev, hdev, color, alpha);
}

/* The tick. A filled polygon rather than a stroke, because Open Logit phase 1
 * fills and does not stroke -- and a check mark is a shape with a thickness, so
 * the outline IS the honest description of it. */
static const short CTL_TICK[] = {
    160, 520,  400, 762,  846, 250,  742, 168,  396, 566,  262, 436
};
/* The <select> disclosure triangle. */
static const short CTL_ARROW[] = { 120, 360,  880, 360,  500, 760 };

static void ctl_circle(int x, int y, int d, uint32_t color, int alpha)
{
    int dev_d = devlen(x, d);
    if (dev_d <= 0) return;
    struct gfx_path p;
    gfx_path_init(&p, ctl_pt, 128, ctl_sub, 8);
    gfx_path_circle(&p, dev_d * GFX_ONE / 2, dev_d * GFX_ONE / 2, dev_d * GFX_ONE / 2);
    shape_fill(&p, GFX_NONZERO, x, y, d, d, dev_d, dev_d, color, alpha);
}

/* Border box + inner fill. `bw` is the border thickness in points. */
static void ctl_frame(int x, int y, int w, int h, int r, uint32_t bg, uint32_t edge, int bw)
{
    if (w <= 0 || h <= 0) return;
    if (bw > 0) {
        fill_round(x, y, w, h, r, edge, 255);
        fill_round(x + bw, y + bw, w - 2 * bw, h - 2 * bw, r > bw ? r - bw : 0, bg, 255);
    } else {
        fill_round(x, y, w, h, r, bg, 255);
    }
}

/* Draw one control. `sx,sy` is its border box in window coordinates. */
static void paint_control(const struct item *e, int sx, int sy)
{
    struct fpaint fp;
    int have = 0;
    int k = e->ctl;
    int fw = e->w, fh = e->h;
    int font = e->ctl_font > 0 ? e->ctl_font : e->font_px;
    if (font <= 0) font = 14;
    int content_w = fw - 2 * (FC_PAD_X + FC_BORDER);

    if (fc_paint_state)
        have = fc_paint_state(e->node, font, e->ctl_mono, content_w, &fp);
    if (!have) {
        /* forms.c is not linked (the host paint test). Draw the chrome with no
         * state -- an empty control, which is what a control with no state IS. */
        for (unsigned i = 0; i < sizeof fp; i++) ((unsigned char *)&fp)[i] = 0;
        fp.kind = k; fp.caret_x = -1; fp.pad_x = FC_PAD_X; fp.pad_y = FC_PAD_Y;
        fp.line_h = font + font / 4; fp.nline = 1;
    }

    /* Did the page style this control itself? If so its background and border
     * are the truth and the system chrome must not be painted underneath --
     * every design system in existence restyles its inputs, and drawing our
     * frame first would show as a grey halo around theirs. */
    int authored = e->has_bg;
    for (int i = 0; i < 4; i++) if (e->border_w[i] > 0) authored = 1;

    uint32_t ink = fp.disabled ? CTL_INK_DIS : (authored ? e->color : CTL_INK);
    int radius = e->radius ? e->radius : (FC_IS_BUTTON(k) || k == FC_SELECT ? 5 : 4);

    if (FC_IS_TOGGLE(k)) {
        int d = fw < fh ? fw : fh;
        if (d > 24) d = 24;                       /* a tick does not grow forever */
        int bx = sx + (fw - d) / 2, by = sy + (fh - d) / 2;
        uint32_t face = fp.checked ? CTL_ACCENT : (fp.disabled ? CTL_DIS_BG : CTL_FIELD_BG);
        uint32_t edge = fp.focused ? CTL_EDGE_FOCUS : (fp.checked ? CTL_ACCENT : CTL_EDGE);
        if (k == FC_CHECKBOX) {
            ctl_frame(bx, by, d, d, 3, face, edge, fp.focused ? 2 : 1);
            if (fp.checked) poly_shape(CTL_TICK, 6, bx, by, d, d, 0xFFFFFFu, 255);
        } else {
            ctl_circle(bx, by, d, edge, 255);
            int inset = fp.focused ? 2 : 1;
            ctl_circle(bx + inset, by + inset, d - 2 * inset, face, 255);
            if (fp.checked) {
                int dd = d / 2; if (dd < 3) dd = 3;
                ctl_circle(bx + (d - dd) / 2, by + (d - dd) / 2, dd, 0xFFFFFFu, 255);
            }
        }
        return;
    }

    /* --- the frame, shared by every remaining kind --- */
    uint32_t face = fp.disabled ? CTL_DIS_BG
                  : FC_IS_BUTTON(k) || k == FC_SELECT || k == FC_FILE ? CTL_BTN_BG
                  : CTL_FIELD_BG;
    if (authored) {
        /* Author styling: reuse the IT_RECT path's colours exactly, so a styled
         * input and a styled <div> beside it land on the same pixels.
         *
         * THE FOCUS RING GOES FIRST, and that is not a cosmetic ordering. It is
         * drawn as a larger rounded rect that the border and background then
         * cover, so what survives is a 2px halo OUTSIDE the author's border.
         * Painting it afterwards -- which is what this did first -- washes
         * translucent blue over the whole control, including the border the
         * page chose, and the device test caught it by no longer being able to
         * find the field's own colour on screen. */
        if (fp.focused)
            fill_round(sx - 2, sy - 2, fw + 4, fh + 4, radius + 2, CTL_EDGE_FOCUS, 110);
        int bmax = 0;
        for (int i = 0; i < 4; i++) if (e->border_w[i] > bmax) bmax = e->border_w[i];
        /* A transparent border occupies space and paints nothing: it arrives
         * as style HIDDEN (see EDGE_CONVERT in css_engine.c). Codex's quiet
         * buttons -- the whole Wikipedia chrome -- are exactly that, and
         * drawing the ring anyway framed every one of them in black. */
        if (e->border_style[0] == BS_NONE || e->border_style[0] == BS_HIDDEN) bmax = 0;
        if (e->has_bg && bmax > 0) {
            fill_round(sx, sy, fw, fh, radius, e->border_color[0], 255);
            fill_round(sx + bmax, sy + bmax, fw - 2 * bmax, fh - 2 * bmax,
                       radius > bmax ? radius - bmax : 0, e->bg, e->bg_alpha ? e->bg_alpha : 255);
        } else if (e->has_bg) {
            fill_round(sx, sy, fw, fh, radius, e->bg, e->bg_alpha ? e->bg_alpha : 255);
        } else if (bmax > 0) {
            fill_round(sx, sy, fw, fh, radius, e->border_color[0], 255);
            fill_round(sx + bmax, sy + bmax, fw - 2 * bmax, fh - 2 * bmax,
                       radius > bmax ? radius - bmax : 0, CTL_FIELD_BG, 255);
        }
    } else {
        ctl_frame(sx, sy, fw, fh, radius,
                  face, fp.focused ? CTL_EDGE_FOCUS : CTL_EDGE, fp.focused ? 2 : 1);
    }

    if (k == FC_RANGE || k == FC_COLOR) {
        /* Not built: a slider needs a thumb the user can drag and a colour
         * field needs a picker. The box is drawn so the page's layout is right
         * and the control is visibly present rather than missing. */
        if (k == FC_RANGE) fill(sx + 4, sy + fh / 2 - 1, fw - 8, 3, CTL_EDGE, 255);
        return;
    }

    /* --- the content --- */
    int cx = sx + FC_BORDER + fp.pad_x;
    int cy = sy + FC_BORDER + fp.pad_y;
    int cw = fw - 2 * (FC_BORDER + fp.pad_x);
    int chh = fh - 2 * (FC_BORDER + fp.pad_y);
    if (k == FC_SELECT) cw -= 16;                 /* room for the triangle */
    if (cw < 0) cw = 0;

    /* Centre a single line vertically. gui_text_run's y is the top of the em
     * box (it adds the ascent itself), so the em box is what is centred. */
    int ty = cy;
    if (k != FC_TEXTAREA && chh > font) ty = cy + (chh - font) / 2;

    /* A button's and a select's label is centred / left-aligned respectively;
     * a field's text is left-aligned and may be scrolled. */
    int tx = cx;
    if (FC_IS_BUTTON(k) || k == FC_FILE) {
        int lw = fp.text_w;
        if (lw < cw) tx = cx + (cw - lw) / 2;
    } else {
        tx = cx - fp.scroll_x;
    }

    /* Clip to the content box: a value longer than the field must not spill
     * over the border and onto the page. */
    int ox0 = cl_x0, oy0 = cl_y0, ox1 = cl_x1, oy1 = cl_y1;
    int nx0 = cx > ox0 ? cx : ox0, ny0 = cy > oy0 ? cy : oy0;
    int nx1 = cx + cw < ox1 ? cx + cw : ox1, ny1 = cy + chh < oy1 ? cy + chh : oy1;
    if (nx1 > nx0 && ny1 > ny0) {
        set_clip(nx0, ny0, nx1, ny1);
        if (k == FC_TEXTAREA && fp.text && fp.len > 0) {
            int ls = 0, line = 0;
            for (int i = 0; i <= fp.len; i++) {
                if (i == fp.len || fp.text[i] == '\n') {
                    int yy = cy + line * fp.line_h;
                    if (yy > cy + chh) break;
                    if (i > ls)
                        gui_text_run(tx, yy, font, e->ctl_mono,
                                     fp.placeholder ? CTL_PLACEHOLD : ink, fp.text + ls, i - ls);
                    ls = i + 1; line++;
                }
            }
        } else if (fp.text && fp.len > 0) {
            if (fp.sel_x1 > fp.sel_x0)
                fill(cx - fp.scroll_x + fp.sel_x0, ty, fp.sel_x1 - fp.sel_x0,
                     font + font / 5, CTL_SELBG, 255);
            gui_text_run(tx, ty, font, e->ctl_mono,
                         fp.placeholder ? CTL_PLACEHOLD : ink, fp.text, fp.len);
        }
        if (fp.caret_x >= 0 && !fp.disabled) {
            int caret_y = (k == FC_TEXTAREA) ? cy + fp.caret_line * fp.line_h : ty;
            int th = font >= 28 ? 2 : 1;
            fill(cx - fp.scroll_x + fp.caret_x, caret_y - 1, th, font + 2, ink, 255);
        }
        set_clip(ox0, oy0, ox1, oy1);
    }

    if (k == FC_SELECT) {
        int aw = 9, ah = 6;
        poly_shape(CTL_ARROW, 3, sx + fw - FC_BORDER - FC_PAD_X - aw,
                   sy + (fh - ah) / 2, aw, ah, fp.disabled ? CTL_INK_DIS : CTL_INK, 255);
    }
}

/* ---- backdrop estimation ----
 *
 * Group opacity is defined against whatever is already painted underneath, and
 * ring 3 cannot read the surface back. So: walk BACKWARDS from the faded item
 * for the nearest already-painted opaque background rect that covers its
 * origin, and fall back to the page background (else white). The display list
 * is emitted parent-before-child, so the nearest such rect really is the box
 * the faded content sits on.
 *
 * The scan is bounded: only items with opacity < 255 pay for it at all, and 256
 * steps is far more than the depth between a text run and its card. Past that
 * the page background is a better guess than an O(n^2) paint. */
static uint32_t backdrop_at(const struct item *it, int i)
{
    const struct item *e = &it[i];
    int px = e->x, py = e->y;
    int lo = i - 256; if (lo < 0) lo = 0;
    for (int k = i - 1; k >= lo; k--) {
        const struct item *b = &it[k];
        if (b->type != IT_RECT || !b->has_bg || b->hidden) continue;
        if (b->bg_alpha < 255 || b->opacity < 255) continue;    /* not opaque: keep looking */
        if (px >= b->x && px < b->x + b->w && py >= b->y && py < b->y + b->h)
            return b->bg;
    }
    uint32_t pbg;
    return layout_page_bg(&pbg) ? pbg : 0xFFFFFF;
}

/* ---- the painted-text record (browser_paint.h says why) ------------------
 *
 * Appended at the ONE site that paints document text, so it cannot drift from
 * what was drawn: if a run does not reach gui_text_run it does not reach this
 * either, which is the whole point -- a record built from the layout tree
 * instead would report text that a clip, an opacity or a viewport cull threw
 * away. Chrome and the ordinary boxes are painted by wm.c and by the app's own
 * chrome path, not here, so what this collects is the DOCUMENT's text and
 * nothing else. */
int printf(const char *, ...);   /* the dump's only output; this TU has no stdio */

#define PTX_MAX 65536
static char g_ptx[PTX_MAX];
static int  g_ptx_n;          /* bytes kept */
static int  g_ptx_log;        /* print the record as it changes (browser only) */
static int  g_ptx_runs;       /* runs painted, counted even when not kept */
static int  g_ptx_chars;      /* text bytes painted, likewise */

/* Ten bytes of decimal per run buys the only question the words alone cannot
 * answer: a title that is MISSING and a title that is painted UNDER the box
 * drawn after it look identical in a list of strings, and completely different
 * in a list of coordinates. Measured on bilibili the day this was added, and
 * that is exactly the pair it had to separate. */
static void ptx_pos(int x, int y)
{
    char b[24]; int n = 0;
    int v = x < 0 ? -x : x, d[8], k = 0;
    if (x < 0) b[n++] = '-';
    do { d[k++] = v % 10; v /= 10; } while (v);
    while (k) b[n++] = (char)('0' + d[--k]);
    b[n++] = ',';
    v = y < 0 ? -y : y; k = 0;
    if (y < 0) b[n++] = '-';
    do { d[k++] = v % 10; v /= 10; } while (v);
    while (k) b[n++] = (char)('0' + d[--k]);
    b[n++] = ' ';
    for (int i = 0; i < n && g_ptx_n + 1 < PTX_MAX; i++) g_ptx[g_ptx_n++] = b[i];
}

static void ptx_note(int x, int y, const char *s, int len)
{
    if (len <= 0 || !s) return;
    g_ptx_runs++;
    g_ptx_chars += len;
    if (g_ptx_n + len + 24 >= PTX_MAX) return;  /* counts go on; bytes stop */
    ptx_pos(x, y);
    for (int i = 0; i < len; i++) {
        /* One run per line, so a newline INSIDE a run would forge a boundary.
         * There should not be one -- layout breaks lines -- and if there ever
         * is, it becomes a space rather than a lie about how many runs there
         * were. */
        char c = s[i];
        g_ptx[g_ptx_n++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    g_ptx[g_ptx_n++] = '\n';
}

void browser_paint_text_log(int on) { g_ptx_log = on ? 1 : 0; }

void browser_paint_text_dump(void)
{
    printf("[dl] painted text: %d run(s), %d byte(s)%s\n", g_ptx_runs, g_ptx_chars,
           g_ptx_n + 1 >= PTX_MAX ? " -- TRUNCATED, the text below is a prefix" : "");
    printf("[dl] ---8<--- begin painted text\n");
    /* Written in bounded pieces rather than one printf: the buffer is 64 KiB
     * and the serial console's own line handling is not something to hand a
     * string that long. */
    int i = 0;
    while (i < g_ptx_n) {
        int j = i;
        while (j < g_ptx_n && g_ptx[j] != '\n') j++;
        char save = g_ptx[j];
        g_ptx[j] = 0;
        printf("[dl] %s\n", g_ptx + i);
        g_ptx[j] = save;
        i = j + 1;
    }
    printf("[dl] ---8<--- end painted text\n");
}

void browser_paint(int vx, int vy, int vw, int vh, int scroll)
{
    const struct item *it = layout_items();
    int n = layout_count();
    /* Whole-pass, so the last paint wins: a repaint that covers half the
     * viewport must not leave the other half's runs from the pass before it
     * standing beside the new ones. */
    g_ptx_n = g_ptx_runs = g_ptx_chars = 0;
    /* The transform memo is keyed on a NODE but its value is in WINDOW points,
     * so it is only valid for one (vx, vy, scroll) triple. Cleared per pass
     * rather than made part of the key: a stale entry here does not fail, it
     * paints last frame's scroll position, which is the kind of wrong that
     * looks like a compositor bug. */
    g_xf_key = 0; g_xf_hit = 0;
    set_clip(vx, vy, vx + vw, vy + vh);
    uint32_t pbg;
    if (layout_page_bg(&pbg)) fill(vx, vy, vw, vh, pbg, 255);  /* themed background */
    for (int i = 0; i < n; i++) {
        const struct item *e0 = &it[i], *e = e0;
        if (e->hidden) continue;                  /* visibility:hidden / opacity:0 */

        /* `transform`, if any ancestor of this item has one. The item is
         * REPLACED by a copy carrying its transformed box, so everything below
         * -- the viewport cull, the clip intersection, all four paint branches
         * -- reads the geometry the element actually occupies. Copying a whole
         * struct item is ~200 bytes and happens only for a transformed item,
         * which on a real page is a handful out of thousands; the alternative,
         * threading x/y/w/h through forty read sites, is the same change spread
         * over the file where it can drift apart. */
        struct item tmp;
        struct gfx_matrix xm;
        int xkind = 0;                            /* 0 none, 1 exact, 2 rotate/skew */
        int sx0 = vx + e->x, sy0 = vy + e->y - scroll;
        if (item_xform(e, vx, vy, scroll, &xm)) {
            xkind = xf_is_axis(&xm) ? 1 : 2;
            int px[4], py[4];
            static const int qx[4] = { 0, 1, 1, 0 }, qy[4] = { 0, 0, 1, 1 };
            for (int k = 0; k < 4; k++) {
                gfx_m_apply(&xm, (sx0 + qx[k] * e->w) * GFX_ONE,
                                 (sy0 + qy[k] * e->h) * GFX_ONE, &px[k], &py[k]);
                px[k] = p8(px[k]); py[k] = p8(py[k]);
            }
            int bx0 = px[0], bx1 = px[0], by0 = py[0], by1 = py[0];
            for (int k = 1; k < 4; k++) {
                if (px[k] < bx0) bx0 = px[k];
                if (px[k] > bx1) bx1 = px[k];
                if (py[k] < by0) by0 = py[k];
                if (py[k] > by1) by1 = py[k];
            }
            tmp = *e;
            tmp.x = bx0 - vx; tmp.y = by0 - vy + scroll;
            tmp.w = bx1 - bx0; tmp.h = by1 - by0;
            /* Lengths that are not the box itself scale with the matrix: a
             * `scale(2)` heading is twice the type size, not the same type in
             * a bigger box, and its corner radius doubles with it. The uniform
             * approximation sqrt(|det|) is the engine's own gfx_m_scale_of and
             * is EXACT for a uniform scale, which is what every `scale()` and
             * `zoom` idiom on the web writes. */
            int us = gfx_m_scale_of(&xm);
            if (us != GFX_MONE && us > 0) {
                if (tmp.radius) tmp.radius = (int)((long long)tmp.radius * us >> 16);
                if (tmp.font_px > 0) {
                    tmp.font_px = (int)((long long)tmp.font_px * us >> 16);
                    if (tmp.font_px < 1) tmp.font_px = 1;
                }
                for (int k = 0; k < 4; k++)
                    tmp.border_w[k] = (int)((long long)tmp.border_w[k] * us >> 16);
            }
            /* Everything that is not a rect is placed at its transformed
             * bounding box and drawn unrotated: gui_text_run takes (x, y, px)
             * and the ABI has no rotated-text or rotated-blit primitive to
             * hand a matrix to. Counted, because "the heading is in the right
             * place but level" and "the heading is missing" are different
             * findings and only one of them is this file's. */
            if (xkind == 2 && e->type != IT_RECT) g_xf_untransformed++;
            e = &tmp;
        }
        int top = e->y - scroll;                  /* viewport-local top */
        if (top + e->h < 0 || top > vh) continue; /* fully outside */
        int sx = vx + e->x;
        int sy = vy + top;

        /* overflow: re-program the clip only when this item's differs from the
         * one already in force. Items under one clipping ancestor are
         * contiguous in the list, so in practice this is two syscalls per
         * scroller rather than two per box. */
        int wx0 = vx, wy0 = vy, wx1 = vx + vw, wy1 = vy + vh;
        if (e->has_clip) {
            int a0 = vx + e->clip_x, b0 = vy + e->clip_y - scroll;
            int a1 = a0 + e->clip_w, b1 = b0 + e->clip_h;
            if (a0 > wx0) wx0 = a0;
            if (b0 > wy0) wy0 = b0;
            if (a1 < wx1) wx1 = a1;
            if (b1 < wy1) wy1 = b1;
            if (wx1 <= wx0 || wy1 <= wy0) continue;     /* clipped away entirely */
        }
        if (wx0 != cl_x0 || wy0 != cl_y0 || wx1 != cl_x1 || wy1 != cl_y1)
            set_clip(wx0, wy0, wx1, wy1);

        /* The clipping ancestor's border-radius, if it has one. Costs a walk
         * up the DOM only for a clipped RECT or IMAGE -- the two things whose
         * paint the arc can actually change. A text run is drawn by the
         * kernel's glyph path, which takes a rectangle and no mask, so
         * fetching a radius for it would be paying for an answer with no
         * consumer. */
        struct rclip rcbuf;
        const struct rclip *rc = 0;
#ifndef PAINT_NEGCTL_NO_RCLIP
        if ((e->type == IT_RECT || e->type == IT_IMAGE) &&
            rclip_of(e, vx, vy, scroll, &rcbuf)) rc = &rcbuf;
#endif

        int op = e->opacity;                      /* 0..255 */

        if (e->type == IT_RECT) {
            /* background-color's own alpha and the element's opacity multiply:
             * rgba(0,0,0,.5) inside an opacity:.5 box is 25% ink. Borders have
             * no colour alpha of their own, so they take opacity alone. */
            int bga = e->has_bg ? e->bg_alpha * op / 255 : 0;
            int bmax = e->border_w[0];
            for (int k = 1; k < 4; k++) if (e->border_w[k] > bmax) bmax = e->border_w[k];
            int r = e->radius_pct ? (e->w < e->h ? e->w : e->h) * e->radius_pct / 100 : e->radius;
            int maxr = (e->w < e->h ? e->w : e->h) / 2; if (r > maxr) r = maxr;
            int rect_done = 0;
            if (xkind == 2) {
                /* Rotated or skewed: the box's own rounded rect as ONE PATH
                 * under the matrix, in ONE colour -- its background if it has
                 * one, otherwise its border's.
                 *
                 * What that leaves out is named rather than discovered: a
                 * rotated box with BOTH a background and a border draws the
                 * background only, and shadows and gradients are not painted
                 * for it at all. All three are 9-slices of an AXIS-ALIGNED
                 * box -- three bands and four corner tiles -- and none of that
                 * decomposition survives a rotation; the honest version is a
                 * second and a third transformed path, which is a bigger piece
                 * of work than the case earns. MEASURED over the 102 sheets of
                 * tests/fixtures/cssweb (57,257 declaration blocks): 1,061
                 * carry a non-`none` transform, 200 of those rotate or skew,
                 * and of THOSE, 0 also carry a linear-gradient and 2 carry a
                 * box-shadow. (This comment claimed "none at all" until the
                 * count was actually taken -- 35 blocks pair ANY transform
                 * with a gradient or a shadow, and it is the rotate/skew
                 * restriction that takes it to two.) A refusal, not an
                 * approximation. */
                int r0 = e0->radius_pct
                       ? (e0->w < e0->h ? e0->w : e0->h) * e0->radius_pct / 100
                       : e0->radius;
                int m0 = (e0->w < e0->h ? e0->w : e0->h) / 2;
                if (r0 > m0) r0 = m0;
                int ring = bmax > 0 && e0->border_style[0] != BS_NONE &&
                                       e0->border_style[0] != BS_HIDDEN;
                uint32_t col = e0->has_bg ? e0->bg : e0->border_color[0];
                int a = e0->has_bg ? bga : (ring ? op : 0);
                if (a <= 0) rect_done = 1;        /* nothing to draw at all */
                else rect_done = paint_rect_xf(e0, sx0, sy0, r0, op, &xm, col, a);
                /* rect_done == 0 means the shape did not fit the bounded mask
                 * and paint_rect_xf counted it; the axis-aligned placement
                 * below is then the complete-but-unrotated answer, which is
                 * gfx_mask.c's acceptable degradation and never a hole. */
            }
            /* box-shadow, outer half: UNDER everything this box paints. */
            if (!rect_done) paint_shadows(e, sx, sy, r, op, 1);
            /* A background gradient replaces the background COLOUR (it is a
             * background-IMAGE layer, painted over it) and nothing else -- the
             * border still comes from the branches below, which is why this
             * only reports whether it painted rather than taking the box
             * over. `grad` == 0 keeps every existing path bit for bit. */
            int grad = rect_done ? 0 : paint_gradient(e, sx, sy, r, op);
            int rounded_done = rect_done;
#ifdef PAINT_NEGCTL_ROUND_HASBG
            /* NEGATIVE CONTROL (test-paint-gfx-negctl): the `&& e->has_bg`
             * guard exactly as it stood, so a rounded border with no
             * background falls back to the square path. Nothing else changes,
             * so only the rounded-outline rows may redden. */
            if (0)
#else
            if (!rect_done && r > 0 && !e->has_bg && !grad)
#endif
            {
                /* THE ROUNDED BORDER WITH NO BACKGROUND. This case used to
                 * fall through to the square path below and draw sharp
                 * corners -- `&& e->has_bg` was answering "is there a fill to
                 * draw" for a question that asked "is the border rounded".
                 *
                 * Restricted to a UNIFORM border on purpose. The rounded path
                 * collapses four edges into one ring in the TOP edge's colour
                 * and the widest width, which is invisible when all four
                 * agree and is a fabrication when they do not: a page writing
                 * `border-bottom: 2px solid red; border-radius: 4px` would go
                 * from one red line to a red ring all the way round. The
                 * square path draws that correctly today, so it keeps it. */
                int uni = e->border_w[0] > 0 && e->border_style[0] == BS_SOLID;
                for (int k = 1; k < 4 && uni; k++)
                    uni = e->border_w[k] == e->border_w[0] &&
                          e->border_color[k] == e->border_color[0] &&
                          e->border_style[k] == e->border_style[0];
                if (uni)
                    stroke_round(sx, sy, e->w, e->h, r, e->border_w[0],
                                 e->border_color[0], op);
                rounded_done = uni;
            }
            if (rect_done) rounded_done = 1;      /* the transformed path drew it */
            if (rounded_done) {
                /* handled above */
            } else if (r > 0 && (e->has_bg || grad)) {
                /* rounded box: border ring (uniform color/width approximation) +
                 * rounded fill inset by the widest edge */
                /* The rounded path draws ONE ring in the top edge's colour, so
                 * it has to consult that edge's style too -- border_edge does
                 * it for the square path and this branch used to not, which
                 * put a black ring around every rounded quiet button (a
                 * transparent border arrives as style HIDDEN; see the
                 * EDGE_CONVERT note in css_engine.c). */
                int ring = bmax > 0 && e->border_style[0] != BS_NONE &&
                                       e->border_style[0] != BS_HIDDEN;
                if (grad) {
                    /* paint_gradient has already covered the whole border box
                     * (background-clip is border-box by default, so the
                     * gradient DOES run under the border). The border must
                     * therefore be a RING drawn on top -- the fill-then-inset
                     * pair below would repaint the whole box in the border
                     * colour first and the gradient would never be seen. */
                    if (ring) stroke_round(sx, sy, e->w, e->h, r, bmax,
                                           e->border_color[0], op);
                } else if (ring) {
                    fill_round(sx, sy, e->w, e->h, r, e->border_color[0], op);
                    fill_round(sx + bmax, sy + bmax, e->w - 2*bmax, e->h - 2*bmax,
                               r > bmax ? r - bmax : 0, e->bg, bga);
                } else {
                    fill_round(sx, sy, e->w, e->h, r, e->bg, bga);
                }
            } else {
                if (e->has_bg && !grad) {
                    /* The one place a rounded overflow clip changes what a
                     * square box paints: a plain background inside a rounded
                     * scroller must follow the scroller's arc, not its
                     * bounding rectangle. */
                    if (rc) fill_rclip(sx, sy, e->w, e->h, e->bg, bga, rc);
                    else    fill(sx, sy, e->w, e->h, e->bg, bga);
                }
                /* top, bottom, left, right -- each drawn over its full corner
                 * square, so a corner takes the horizontal edge's colour. */
                if (e->border_w[0] > 0)
                    border_edge(sx, sy, e->w, e->border_w[0], 0,
                                e->border_style[0], e->border_color[0], op, 1);
                if (e->border_w[2] > 0)
                    border_edge(sx, sy + e->h - e->border_w[2], e->w, e->border_w[2], 0,
                                e->border_style[2], e->border_color[2], op, 0);
                if (e->border_w[3] > 0)
                    border_edge(sx, sy, e->h, e->border_w[3], 1,
                                e->border_style[3], e->border_color[3], op, 1);
                if (e->border_w[1] > 0)
                    border_edge(sx + e->w - e->border_w[1], sy, e->h, e->border_w[1], 1,
                                e->border_style[1], e->border_color[1], op, 0);
            }
            /* box-shadow, inset half: OVER the box's own background and
             * border, which is what `inset` means. */
            if (!rect_done) paint_shadows(e, sx, sy, r, op, 0);
        } else if (e->type == IT_TEXT) {
            /* Opacity on text is folded into the COLOUR, and that is exact, not
             * an approximation of compositing: the glyph blend the kernel does
             * is dst = c*cov + backdrop*(1-cov), so feeding it
             * c' = c*op + backdrop*(1-op) yields c*op*cov + backdrop*(1-op*cov)
             * -- precisely group opacity. What IS estimated is the backdrop. */
            uint32_t col = e->color;
            if (op < 255) col = mix(backdrop_at(it, i), col, op);
            gui_text_run(sx, sy, e->font_px, e->mono, col, e->text, e->len);
            ptx_note(sx, sy, e->text, e->len);
            /* text-decoration. `y` is the top of the em box -- text_draw_run
             * adds the ascent itself, and the ascent lives in the kernel's font
             * tables, so ring 3 has no baseline to measure from. The underline
             * offset is the proxy: it was tuned to land a pixel under the
             * baseline, so line-through goes half an x-height (~0.3em) above
             * THAT, and overline on the em box's top edge. Deriving the strike
             * from a fixed fraction of the em box instead put it up near the
             * cap line on our 1.08-em-ascent fonts. */
            int th = e->font_px >= 32 ? 2 : 1;
            int uy = sy + e->font_px + (e->font_px > 20 ? 3 : 2);
            if (e->overline)  fill(sx, sy, e->w, th, col, 255);
            if (e->strike)    fill(sx, uy - e->font_px * 30 / 100, e->w, th, col, 255);
            if (e->underline) fill(sx, uy, e->w, th, col, 255);
        } else if (e->type == IT_VIDEO) {
            /* A <video>'s pixels are not layout's to own -- they are one frame
             * out of a decoder that will produce the next one in 33 ms. So the
             * painter hands the media engine the border box and the clip and
             * lets it blit; with no engine linked, the box paints black, which
             * is what a <video> with no source looks like anyway. Weak, so the
             * host layout/paint tests link without any of it. */
            if (media_paint_box)
                media_paint_box(e->node, sx, sy, e->w, e->h, cl_x0, cl_y0,
                                cl_x1 - cl_x0, cl_y1 - cl_y0);
            else
                fill(sx, sy, e->w, e->h, 0x000000, op);
        } else if (e->type == IT_CANVAS) {
            int cw = 0, ch = 0;
            const unsigned char *cpx = canvas_pixels
                                     ? canvas_pixels(e->node, &cw, &ch) : 0;
            /* Drawn at its OWN size into the box CSS gave it; when the two
             * differ the compositor's rescale applies, which is the spec's
             * behaviour and is why they are separate quantities. */
            if (cpx && cw > 0 && ch > 0) gui_blit(sx, sy, e->w, e->h, cpx, cw, ch);
        } else if (e->type == IT_CONTROL) {
            paint_control(e, sx, sy);
        } else if (e->type == IT_IMAGE && e->img) {
            gui_blit(sx, sy, e->w, e->h, e->img->rgba, e->img->w, e->img->h);
            /* An image cannot have its own alpha modulated without copying the
             * whole bitmap, so instead wash the backdrop back over it at
             * 1 - opacity. On top of the just-blitted image that composes to
             * img*op + backdrop*(1-op): the same exact result, one extra call,
             * and no per-frame allocation. */
            if (op < 255) fill(sx, sy, e->w, e->h, backdrop_at(it, i), 255 - op);
            /* A full-bleed picture in a rounded card: without this its square
             * corners poke out of the card's own arc, which is the loudest
             * single artefact `overflow:hidden` was supposed to prevent. */
            if (rc) img_rclip(sx, sy, e->w, e->h, backdrop_at(it, i), rc);
        }
    }
    gui_clip(0, 0, 0, 0);
    cl_x0 = cl_y0 = 0; cl_x1 = cl_y1 = 0;

    /* ONE LINE PER CONTENT CHANGE, automatically. The full text needs a
     * trigger (about:text) because it is 40 KB on a real page and a serial
     * console is not free; the COUNTS are two integers and belong in every
     * log next to `[browser] load done`, which is the line a reader is
     * already looking at when they ask why a page looks empty.
     *
     * Printed only when the pair CHANGES, because browser_paint runs per
     * frame: a settled page would otherwise emit this sixty times a second,
     * and an instrument that floods the log it writes to has replaced the
     * thing it was measuring. */
    {
        static int last_runs = -1, last_chars = -1;
        /* OFF unless the browser turns it on, and that is not a preference.
         * This TU is linked by five host harnesses that render pages without
         * being a browser -- reftest, the layout box tests, the WPT runner,
         * the semantics runner -- and every one of them reads its own stdout.
         * The first version printed unconditionally and put a `[dl]` line
         * between every reftest verdict, which is an instrument writing into
         * the output of the thing it is measuring. A harness that wants the
         * record calls browser_paint_text_log(1); browser.c does. */
        if (g_ptx_log && (g_ptx_runs != last_runs || g_ptx_chars != last_chars)) {
            last_runs = g_ptx_runs; last_chars = g_ptx_chars;
            printf("[dl] painted text: %d run(s), %d byte(s)\n",
                   g_ptx_runs, g_ptx_chars);
            /* ...and the WORDS, bounded. A trigger was tried first and is
             * still wired (about:text, and a Ctrl+Alt+D chord), and neither
             * arrives through the QMP harness -- four boots spent on it, and
             * the cause is upstream of anything this file can see. An
             * instrument that needs a keystroke nobody can deliver is not an
             * instrument, and the keystroke was never the point.
             *
             * PTX_LOG_MAX is the whole cost control. A page that paints a lot
             * of text would otherwise put tens of kilobytes on a serial
             * console once per content change, which is the "an instrument
             * that changes the machine it measures" trap named above
             * kbench.c's micro battery. Measured on bilibili the text painted
             * is 642 bytes, so the cap is not reached and not felt; it exists
             * for the page where it would be. Truncation says so. */
            printf("[dl] ---8<--- begin painted text\n");
            #define PTX_LOG_MAX 4096
            int lim = g_ptx_n < PTX_LOG_MAX ? g_ptx_n : PTX_LOG_MAX;
            int i = 0;
            while (i < lim) {
                int j = i;
                while (j < lim && g_ptx[j] != '\n') j++;
                char save = g_ptx[j];
                g_ptx[j] = 0;
                printf("[dl] %s\n", g_ptx + i);
                g_ptx[j] = save;
                i = j + 1;
            }
            if (lim < g_ptx_n)
                printf("[dl] ...TRUNCATED at %d of %d byte(s)\n", lim, g_ptx_n);
            printf("[dl] ---8<--- end painted text\n");
        }
    }
}

/* Does the point (doc coords) land on `e`, honouring its overflow clip? An
 * element scrolled out of a clipping ancestor is not merely invisible, it is
 * not clickable either. */
static int item_hit(const struct item *e, int x, int dy)
{
    if (!(x >= e->x && x < e->x + e->w && dy >= e->y && dy < e->y + e->h)) return 0;
    if (e->has_clip && !(x >= e->clip_x && x < e->clip_x + e->clip_w &&
                         dy >= e->clip_y && dy < e->clip_y + e->clip_h)) return 0;
    return 1;
}

int browser_hittest(int x, int y, int scroll, char *buf, int max)
{
    if (max <= 0) return 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;                          /* into doc coordinates */
    for (int i = n - 1; i >= 0; i--) {            /* topmost first */
        const struct item *e = &it[i];
        if (!e->href || e->hidden) continue;
        if (item_hit(e, x, dy)) {
            int o = 0;
            while (e->href[o] && o < max - 1) { buf[o] = e->href[o]; o++; }
            buf[o] = 0;
            return 1;
        }
    }
    return 0;
}

int browser_hittest_node(int x, int y, int scroll, struct node **node, char *href, int max)
{
    if (node) *node = 0;
    if (href && max > 0) href[0] = 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;
    /* Back to front. The display list is emitted parent-before-child (a block's
     * background rect, then its text), so the LAST box covering the point is the
     * innermost one -- which is the element a DOM event should target. */
    for (int i = n - 1; i >= 0; i--) {
        const struct item *e = &it[i];
        if (e->hidden || !item_hit(e, x, dy)) continue;
        if (node) {
            /* Text boxes hang off the TEXT node; an event target must be an
             * element, so climb until we find one. */
            struct node *p = e->node;
            while (p && p->type != N_ELEM && p->type != N_DOCUMENT) p = p->parent;
            *node = p;
        }
        if (href && max > 0 && e->href) {
            int o = 0;
            while (e->href[o] && o < max - 1) { href[o] = e->href[o]; o++; }
            href[o] = 0;
        }
        /* An inner box without its own href still navigates its ancestor link
         * (layout propagates the href down the inline flow, but a background
         * rect emitted for a nested element may carry none) -- so if this box
         * had no href, fall back to the link hit test over the same point. */
        if (href && max > 0 && !href[0]) browser_hittest(x, y, scroll, href, max);
        return 1;
    }
    return 0;
}
