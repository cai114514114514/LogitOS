/* Ring-3 paint (M17 L1): walk the layout display list and draw it with the GUI
 * render syscalls. Mirrors the old kernel net/paint.c, fb_* -> gui_*.
 *
 * Everything here has to be expressible with five primitives -- gui_rect,
 * gui_rrect, gui_text_run, gui_blit and gui_clip -- because ring 3 cannot read
 * the window surface back. That single constraint decides most of the design
 * below: alpha is done by blitting a 1x1 RGBA source (gui_blit is the only
 * primitive that blends), and group opacity on text is folded into the text
 * COLOUR against an estimated backdrop rather than composited. */
#include "logit.h"
#include "layout.h"
#include "browser_paint.h"

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

/* `a` of `top` over `base`, a in 0..255. */
static uint32_t mix(uint32_t base, uint32_t top, int a)
{
    if (a >= 255) return top;
    if (a <= 0) return base;
    return pack_rgb((chan_r(top) * a + chan_r(base) * (255 - a)) / 255,
                    (chan_g(top) * a + chan_g(base) * (255 - a)) / 255,
                    (chan_b(top) * a + chan_b(base) * (255 - a)) / 255);
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

static int isqrt_(int v)
{
    if (v <= 0) return 0;
    /* Digit-by-digit binary square root. `bit` MUST start at a power of FOUR
     * (2^30 here, not 2^31 or 2^15) or the restoring step is off by a shift. */
    int r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
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

/* A rounded fill that can be translucent. The opaque case still goes through
 * gui_rrect (one syscall, and the kernel's own corner test); the translucent
 * case is banded -- one blended strip per row of the two corner zones plus a
 * single strip for the straight middle, i.e. 2*radius + 1 calls rather than
 * one per pixel row of the whole box. */
static void fill_round(int x, int y, int w, int h, int r, uint32_t color, int alpha)
{
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (alpha >= 255) { gui_rrect(x, y, w, h, r, color); return; }
    if (r <= 0) { fill(x, y, w, h, color, alpha); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < r; j++) {
        int dy = r - 1 - j;                        /* rows above the corner centre */
        int dx = r - isqrt_(r * r - dy * dy);
        fill(x + dx, y + j, w - 2 * dx, 1, color, alpha);
        fill(x + dx, y + h - 1 - j, w - 2 * dx, 1, color, alpha);
    }
    fill(x, y + r, w, h - 2 * r, color, alpha);
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

void browser_paint(int vx, int vy, int vw, int vh, int scroll)
{
    const struct item *it = layout_items();
    int n = layout_count();
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
                if (bmax > 0) {
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
