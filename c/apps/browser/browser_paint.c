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

/* The media engine, weakly: a <video> box is painted by whoever owns the
 * decoded frame (c/apps/browser/js_media.c), and this file must keep linking in
 * the host paint/layout tests, which link neither the engine nor QuickJS.
 * Spelled `__weak__` and not `weak`, because mini-libc's features.h is
 * force-included into every browser TU and #defines the plain spelling. */
struct node;
extern void media_paint_box(struct node *node, int x, int y, int w, int h,
                            int clip_x, int clip_y, int clip_w, int clip_h)
    __attribute__((__weak__));

/* forms.c, weakly, for exactly the same reason. layout.c reserves a control's
 * box; what goes INSIDE it -- the value, the caret, the tick -- is state that
 * changes on every keystroke and lives in forms.c. Weak because eight host test
 * binaries link this painter without forms.c: with it absent every control
 * still draws its chrome, just empty, which is a visible and honest
 * degradation rather than a link error in someone else's test. */
extern int fc_paint_state(struct node *n, int font_px, int mono, int content_w,
                          struct fpaint *out) __attribute__((__weak__));

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
static void fill_round(int x, int y, int w, int h, int r, uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fill(x, y, w, h, color, alpha); return; }
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
    if (!cov) { fill(x, y, w, h, color, alpha); return; }
    fill(x + r, y,         w - 2 * r, r,         color, alpha);
    fill(x,     y + r,     w,         h - 2 * r, color, alpha);
    fill(x + r, y + h - r, w - 2 * r, r,         color, alpha);
    corner_blit(x,         y,         r, cov, cw, ch, color, alpha, 0, 0);
    corner_blit(x + w - r, y,         r, cov, cw, ch, color, alpha, 1, 0);
    corner_blit(x,         y + h - r, r, cov, cw, ch, color, alpha, 0, 1);
    corner_blit(x + w - r, y + h - r, r, cov, cw, ch, color, alpha, 1, 1);
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
static int ctl_pt[128], ctl_sub[8];

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
    set_clip(vx, vy, vx + vw, vy + vh);
    uint32_t pbg;
    if (layout_page_bg(&pbg)) fill(vx, vy, vw, vh, pbg, 255);  /* themed background */
    for (int i = 0; i < n; i++) {
        const struct item *e = &it[i];
        int top = e->y - scroll;                  /* viewport-local top */
        if (e->hidden) continue;                  /* visibility:hidden / opacity:0 */
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
            if (r > 0 && e->has_bg) {
                /* rounded box: border ring (uniform color/width approximation) +
                 * rounded fill inset by the widest edge */
                /* The rounded path draws ONE ring in the top edge's colour, so
                 * it has to consult that edge's style too -- border_edge does
                 * it for the square path and this branch used to not, which
                 * put a black ring around every rounded quiet button (a
                 * transparent border arrives as style HIDDEN; see the
                 * EDGE_CONVERT note in css_engine.c). */
                if (bmax > 0 && e->border_style[0] != BS_NONE && e->border_style[0] != BS_HIDDEN) {
                    fill_round(sx, sy, e->w, e->h, r, e->border_color[0], op);
                    fill_round(sx + bmax, sy + bmax, e->w - 2*bmax, e->h - 2*bmax,
                               r > bmax ? r - bmax : 0, e->bg, bga);
                } else {
                    fill_round(sx, sy, e->w, e->h, r, e->bg, bga);
                }
            } else {
                if (e->has_bg) fill(sx, sy, e->w, e->h, e->bg, bga);
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
