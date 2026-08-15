#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "vmm.h"
#include "text.h"
#include "virtio_gpu.h"
#include "gfx.h"
#include "glass.h"        /* Open Logit's mask cache -- see corner_mask_for() below */

/* --- Multiboot2 framebuffer info tag (type 8) --- */
struct mb2_tag { uint32_t type, size; };

struct mb2_fb_tag {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  bpp, fb_type;
    uint16_t reserved;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
};

#define MB2_FB_TYPE_RGB 1

void *kmalloc(unsigned long);
void  kfree(void *);

static volatile uint8_t *fb_mem;
static uint32_t fb_pitch, fb_w, fb_h;
static uint8_t rpos, gpos, bpos;
static int using_gpu;             /* fb_mem is a virtio-gpu RAM resource, not VGA MMIO */

static struct surface screen;     /* wraps the back buffer (the visible screen) */
static struct surface *T;         /* current draw target (defaults to screen) */

/* The clip rectangle lives ON THE CURRENT TARGET (struct surface), not in a
 * global -- so a clip set while drawing into one app's surface can never affect
 * a draw into another app's surface (the cross-app leak that left a freshly
 * opened Terminal white). */
void fb_set_clip(int x, int y, int w, int h)
{
    struct surface *s = T ? T : &screen;
    s->clip_on = 1; s->clx0 = x; s->cly0 = y; s->clx1 = x + w; s->cly1 = y + h;
}
void fb_clear_clip(void) { struct surface *s = T ? T : &screen; s->clip_on = 0; }

uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

/* ---- UI scale ----------------------------------------------------------
 * See the long comment in fb.h. 100 until fb_init() picks one, so anything that
 * draws before a mode is known behaves exactly as it did before scaling existed. */
static int ui_scale = 100;

void fb_set_scale(int pct)
{
    if (pct < 100) pct = 100;          /* the UI is authored at 1x; never shrink it */
    if (pct > 400) pct = 400;
    ui_scale = pct;
}
int fb_scale(void) { return ui_scale; }

/* Floor toward negative infinity, not toward zero: a window dragged off the left
 * edge has negative content coordinates, and truncation there makes fb_pt
 * non-monotonic across 0 -- two adjacent logical columns would map to the same
 * device column and the frame would visibly shear at x=0. */
int fb_pt(int points)
{
    if (points >= 0) return points * ui_scale / 100;
    return -(((-points) * ui_scale + 99) / 100);
}
/* THE INVERSE OF A FLOOR IS NOT A FLOOR, and getting this wrong is the exact
 * shape of the classic scaled-UI bug: at 150% the point 3 lands on device pixel
 * 4, but 4*100/150 floors back to 2, so a click one row below a button's top
 * edge reports as being above it. Every third row is misattributed and the
 * widget's hit box quietly drifts from the pixels it drew.
 *
 * What is wanted is the point whose CELL contains this pixel -- the largest v
 * with fb_pt(v) <= px -- which is ceil(100*(px+1)/scale) - 1. The host test
 * (tests/unit/scale_test.c) asserts both directions of this for every scale, and
 * it is what caught the floor-inverse version. */
int fb_dev2pt(int px)
{
    int a = 100 * (px + 1), b = ui_scale;
    int q = (a >= 0) ? (a + b - 1) / b : -((-a) / b);   /* ceil, negatives too */
    return q - 1;
}
int fb_ui_px(void)    { return TEXT_UI_PX * ui_scale / 100; }
int fb_width_pt(void)  { return fb_dev2pt((int)fb_w); }
int fb_height_pt(void) { return fb_dev2pt((int)fb_h); }

/* The desktop is designed against a 1280x800 point canvas: the browser alone
 * asks for a 1180x620 window, and a logical desktop smaller than that would make
 * SYS_GUI_CREATE refuse it. So the rule is not "pick a pretty number", it is
 * "spend every pixel beyond the design canvas on DENSITY, never on shrinking the
 * UI" -- which is exactly what makes a resolution bump an improvement instead of
 * the usual make-everything-tiny regression.
 *
 * Quantised to 25% steps so the arithmetic stays predictable and a one-pixel
 * change in the reported mode cannot jitter the whole layout. */
#define DESIGN_W_PT 1280
#define DESIGN_H_PT 800
static int pick_scale(uint32_t w, uint32_t h)
{
    int sw = (int)w * 100 / DESIGN_W_PT, sh = (int)h * 100 / DESIGN_H_PT;
    int s = sw < sh ? sw : sh;
    s = (s / 25) * 25;
    if (s < 100) s = 100;              /* a mode smaller than the design canvas
                                        * stays 1x: shrinking is never the answer */
    if (s > 300) s = 300;
    return s;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << rpos) | ((uint32_t)g << gpos) | ((uint32_t)b << bpos);
}

static void unpack(uint32_t c, int *r, int *g, int *b)
{
    *r = (c >> rpos) & 0xFF;
    *g = (c >> gpos) & 0xFF;
    *b = (c >> bpos) & 0xFF;
}

int fb_init(uint64_t mb_info_addr)
{
    /* Prefer virtio-gpu: a normal-RAM framebuffer (fast CPU writes) presented by
     * DMA (TRANSFER+FLUSH) instead of byte-copying into uncached VGA MMIO. */
    if (virtio_gpu_init() == 0) {
        fb_w = virtio_gpu_width(); fb_h = virtio_gpu_height();
        fb_pitch = fb_w * 4;
        rpos = 16; gpos = 8; bpos = 0;          /* B8G8R8X8 backing == 0x00RRGGBB */
        fb_mem = (volatile uint8_t *)virtio_gpu_fb();
        using_gpu = 1;
        fb_set_scale(pick_scale(fb_w, fb_h));
        return 1;
    }

    uint32_t total = *(volatile uint32_t *)mb_info_addr;
    uint8_t *p = (uint8_t *)(mb_info_addr + 8);
    uint8_t *end = (uint8_t *)(mb_info_addr + total);
    struct mb2_fb_tag *fb = NULL;

    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0 || tag->size < 8)
            break;                       /* terminator, or a malformed zero-size tag (would loop forever) */
        if (tag->type == 8) {
            fb = (struct mb2_fb_tag *)tag;
            break;
        }
        p += (tag->size + 7) & ~7u;
    }

    if (!fb || fb->fb_type != MB2_FB_TYPE_RGB || fb->bpp != 32)
        return 0;

    fb_pitch = fb->pitch;
    fb_w     = fb->width;
    fb_h     = fb->height;
    rpos     = fb->red_pos;
    gpos     = fb->green_pos;
    bpos     = fb->blue_pos;

    /* The framebuffer is high-MMIO, outside the identity map — map it. */
    vmm_map_range(fb->addr, fb->addr, (uint64_t)fb->pitch * fb->height,
                  VMM_WRITABLE | VMM_NOCACHE);
    fb_mem = (volatile uint8_t *)fb->addr;
    fb_set_scale(pick_scale(fb_w, fb_h));
    return 1;
}

/* The clip rect is carried by the target surface (struct surface), so it bounds
 * only draws into that surface and never leaks onto another app's surface or the
 * WM's screen compositing. The screen back buffer's clip is never set, so chrome
 * is always drawn in full. */
void fb_put(int x, int y, uint32_t color)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1))
        return;
    s->px[y * s->w + x] = color;
}

/* ---- the clip as a LOOP BOUND, not a per-pixel test ------------------------
 *
 * fb_put has always tested the clip and dropped the pixel, which is correct and
 * was cheap enough while the clip was only ever an app's own scissor. It stops
 * being cheap the moment the COMPOSITOR draws the whole scene clipped to a
 * damage rectangle: the work a damage rectangle exists to remove is precisely
 * "iterate every pixel of a 1180x620 window and throw them away".
 *
 * So every rect-shaped primitive below asks for its surviving index range up
 * front. i and j keep their original meaning -- they index the SHAPE, not the
 * screen -- so a gradient row, a rounded corner or a glyph's coverage byte is
 * computed from exactly the same j it always was; only the rows and columns
 * that would have been discarded are never visited. Output is unchanged, which
 * is the property tests/unit/fb_clip_test.c pins down.
 *
 * Returns 0 when nothing survives. */
static int clip_ij(int x, int y, int w, int h, int *i0, int *j0, int *i1, int *j1)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || w <= 0 || h <= 0) return 0;
    int cx0 = 0, cy0 = 0, cx1 = s->w, cy1 = s->h;
    if (s->clip_on) {
        if (s->clx0 > cx0) cx0 = s->clx0;
        if (s->cly0 > cy0) cy0 = s->cly0;
        if (s->clx1 < cx1) cx1 = s->clx1;
        if (s->cly1 < cy1) cy1 = s->cly1;
    }
    *i0 = cx0 - x; if (*i0 < 0) *i0 = 0;
    *j0 = cy0 - y; if (*j0 < 0) *j0 = 0;
    *i1 = cx1 - x; if (*i1 > w) *i1 = w;
    *j1 = cy1 - y; if (*j1 > h) *j1 = h;
    return *i0 < *i1 && *j0 < *j1;
}

/* Is this pixel of the CURRENT TARGET writable? For the two primitives that
 * write s->px straight (the blur and the glass, which read a neighbourhood and
 * so cannot be expressed as a clamped loop over their own output). */
static int clip_px(const struct surface *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1)) return 0;
    return 1;
}

static uint32_t fb_get(int x, int y)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    if (s->clip_on && (x < s->clx0 || y < s->cly0 || x >= s->clx1 || y >= s->cly1))
        return 0;
    return s->px[y * s->w + x];
}

void fb_set_backbuffer(uint32_t *buf)
{
    screen.px = buf;
    screen.w  = (int)fb_w;
    screen.h  = (int)fb_h;
    T = &screen;
}

void fb_target(struct surface *s)
{
    T = s ? s : &screen;
}

/* Composite a window surface onto the current target with a direct row copy
 * (bounds-clamped once), not per-pixel fb_put. The browser's 1180x620 surface
 * is ~700K pixels; per-pixel fb_put (call + bounds check each) cost ~60-100 ms
 * under TCG and dominated every frame -- this is several times faster. */
/* HONOURS THE TARGET CLIP, which it did not before. That is what lets the
 * compositor blit only the part of an app's canvas that lies inside the damage
 * rectangle instead of copying the whole 1180x620 surface for a one-line
 * repaint -- and, more importantly, it is what guarantees a partial frame
 * cannot write a single pixel outside the region it is allowed to touch. The
 * old version was unclipped because the only caller was a full composite. */
void fb_blit_surface(int dx, int dy, const struct surface *src)
{
    struct surface *t = T;
    if (!t || !t->px || !src->px) return;
    int i0, j0, i1, j1;
    if (!clip_ij(dx, dy, src->w, src->h, &i0, &j0, &i1, &j1)) return;
    for (int y = j0; y < j1; y++) {
        const uint32_t *srow = src->px + (uint32_t)y * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + y) * t->w + dx;
        for (int x = i0; x < i1; x++) drow[x] = srow[x];
    }
}

/* Nearest-neighbour scaled, opaque blit of a surface into dest rect (dx,dy,dw,dh)
 * of the current target. Used for the window open "pop" (scale 0.85->1.0). */
void fb_blit_surface_scaled(int dx, int dy, int dw, int dh, const struct surface *src)
{
    struct surface *t = T ? T : &screen;
    if (!t->px || !src->px || dw <= 0 || dh <= 0) return;
    int i0, j0, i1, j1;
    if (!clip_ij(dx, dy, dw, dh, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        int sy = j * src->h / dh;               /* still against the FULL dest rect */
        const uint32_t *srow = src->px + (uint32_t)sy * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + j) * t->w;
        for (int i = i0; i < i1; i++)
            drow[dx + i] = srow[i * src->w / dw];
    }
}

/* Same contract as fb_blit_surface_scaled, BILINEAR: every dest pixel blends
 * the four source texels around its sample point instead of picking one.
 *
 * WHY NEAREST SHIMMERS AND THIS DOESN'T. Nearest maps dest pixel i to source
 * column i*src->w/dw -- an integer division, so as dw changes frame to frame
 * (a resize drag, the open/close pop's scale 0.85->1.0) the column that
 * division rounds to for a given i jumps discretely: one frame a source row
 * survives into the dest whole, the next it is skipped outright, because the
 * ratio crossed an integer boundary between them. That flip is what "rows pop
 * in and out" IS. A bilinear sample's four weights change continuously with
 * the ratio -- there is no boundary to cross -- so consecutive frames differ
 * by a small blend shift instead of by a row appearing or vanishing.
 *
 * FIXED POINT. `stepx`/`stepy` are 16.16 -- the same texture-step family as
 * Open Logit's transform matrices (gfx.h) -- and walked by simple addition
 * rather than re-deriving i*src->w/dw per pixel, matching how every other
 * per-pixel loop in this file (fb_blur_rect's moving sums, fb_liquid_glass's
 * displacement) turns an O(pixels) divide into an O(pixels) add. The -0.5
 * texel bias centres the sample on the dest pixel's centre rather than its
 * top-left corner, which is what makes a 1:1 scale reproduce the source
 * exactly instead of shifting it half a texel toward the bottom-right.
 *
 * The blend weights are then read off the TOP 8 BITS of each 16-bit fraction
 * (0..255), not the full 16 -- so a weight product (wx*wy) tops out at
 * 255*255=65025, comfortably inside a 32-bit int alongside a 0..255 colour
 * channel, with no 64-bit multiply needed in the innermost loop. Precision
 * lost below bit 8 of the fraction is under 1/256 of a texel step, far finer
 * than a screen pixel can show.
 *
 * ROUNDED, NOT TRUNCATED, on the final divide -- this tree has already found
 * the alternative's failure mode once (fb_liquid_glass's forerunner and
 * gfx_over both floored a /255 and quietly darkened every faint-over-faint
 * blend by up to 1/255 at the low end; see the note above gfx_over). The
 * same shape of bug here would show as a fully-mixed edge fading slightly
 * dark relative to its four source texels, frame after frame, in a way no
 * single screenshot flags but a repeated blend accumulates.
 *
 * NOT a box/area filter: this samples exactly four texels per dest pixel
 * regardless of how far dw has shrunk src->w, so a large downscale still
 * aliases somewhat (a real box filter would average every source texel a
 * dest pixel covers, at a real per-pixel cost that grows with the scale
 * ratio instead of staying flat at four taps). That trade is deliberate --
 * see tests/unit/fb_scale_bl_bench.c, which builds this file host-side (the
 * same stub pattern as fb_clip_test.c) and times both paths on a 1180x620
 * window surface (a real browser/Finder canvas size): bilinear cost 4.3x
 * nearest at a ~0.3x shrink and 4.4x at a ~1.5x grow -- flat regardless of
 * direction, matching "four fetches + three lerps vs. one fetch" rather than
 * scaling with how much the image moved. (Host cycles, not guest TCG --
 * fb.c cannot run under QEMU on the host that builds it -- but the RATIO
 * between two functions measured the same way on the same host is the
 * number that matters here, and it is what decides the trade below.) Cheap
 * enough for the ONE window currently under an open/close pop or a live
 * resize drag; not proposed here for every window a compositor redraws
 * every frame regardless of motion -- that math is 4x the cost for windows
 * that were not asked to look smoother, paid on every frame instead of only
 * the animating one. The same 4x-per-tap-count shape is why fb_liquid_glass
 * above samples R, G and B as three separate single-tap fetches rather than
 * three bilinear ones: a cost that is well spent on the rim band of one
 * glass panel would not be spent the same way if paid by every pixel of it. */
void fb_blit_surface_scaled_bl(int dx, int dy, int dw, int dh, const struct surface *src)
{
    struct surface *t = T ? T : &screen;
    if (!t->px || !src->px || dw <= 0 || dh <= 0 || src->w <= 0 || src->h <= 0) return;
    int i0, j0, i1, j1;
    if (!clip_ij(dx, dy, dw, dh, &i0, &j0, &i1, &j1)) return;

    uint32_t stepx = ((uint32_t)src->w << 16) / (uint32_t)dw;
    uint32_t stepy = ((uint32_t)src->h << 16) / (uint32_t)dh;
    int maxsx = src->w - 1, maxsy = src->h - 1;

    for (int j = j0; j < j1; j++) {
        int64_t sy16 = (int64_t)j * stepy + (int64_t)(stepy >> 1) - 0x8000;
        if (sy16 < 0) sy16 = 0;
        int sy0 = (int)(sy16 >> 16);
        if (sy0 > maxsy) sy0 = maxsy;
        int wy1 = (int)((sy16 >> 8) & 0xFF);      /* top 8 bits of the fraction */
        int wy0 = 256 - wy1;
        int sy1 = sy0 < maxsy ? sy0 + 1 : sy0;

        const uint32_t *row0 = src->px + (uint32_t)sy0 * src->w;
        const uint32_t *row1 = src->px + (uint32_t)sy1 * src->w;
        uint32_t *drow = t->px + (uint32_t)(dy + j) * t->w + dx;

        for (int i = i0; i < i1; i++) {
            int64_t sx16 = (int64_t)i * stepx + (int64_t)(stepx >> 1) - 0x8000;
            if (sx16 < 0) sx16 = 0;
            int sx0 = (int)(sx16 >> 16);
            if (sx0 > maxsx) sx0 = maxsx;
            int wx1 = (int)((sx16 >> 8) & 0xFF);
            int wx0 = 256 - wx1;
            int sx1 = sx0 < maxsx ? sx0 + 1 : sx0;

            /* Weights past this point are exact for the pixel sampled: when an
             * axis clamped (sx0==maxsx or sy0==maxsy) its "1" tap duplicates
             * the "0" tap (sx1==sx0 / sy1==sy0), so the two texels being
             * blended are identical and any wx1/wy1 split of that axis's 256
             * yields the same sum -- no separate zero-the-weight case needed. */
            uint32_t p00 = row0[sx0], p10 = row0[sx1];
            uint32_t p01 = row1[sx0], p11 = row1[sx1];
            int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(p00, &r00, &g00, &b00);
            unpack(p10, &r10, &g10, &b10);
            unpack(p01, &r01, &g01, &b01);
            unpack(p11, &r11, &g11, &b11);

            int w00 = wx0 * wy0, w10 = wx1 * wy0, w01 = wx0 * wy1, w11 = wx1 * wy1;
            int r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11 + 32768) >> 16;
            int g = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11 + 32768) >> 16;
            int b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11 + 32768) >> 16;
            drow[i] = fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

/* Raw back->framebuffer copy of a clamped rect. The parallel present workers
 * (one per CPU) each call this on a disjoint band of rows -- disjoint writes,
 * read-only shared source, so no locking is needed. */
void fb_copy_rect(int x, int y, int w, int h)
{
    if (!screen.px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        volatile uint32_t *dst = (volatile uint32_t *)(fb_mem + (uint32_t)(y + yy) * fb_pitch);
        const uint32_t *src = screen.px + (uint32_t)(y + yy) * fb_w;
        for (int xx = 0; xx < w; xx++) dst[x + xx] = src[x + xx];
    }
}

/* When SMP is up, smp.c registers a parallel implementation that splits a tall
 * rect's rows across all CPUs via work IPIs. */
static void (*g_par_present)(int, int, int, int);
void fb_set_present_par(void (*fn)(int, int, int, int)) { g_par_present = fn; }

void fb_present(void) { fb_present_rect(0, 0, (int)fb_w, (int)fb_h); }

/* Pixels actually pushed to the display since boot, and the number of pushes.
 * Clamped, so a caller that hands in a rect hanging off the edge is charged for
 * what was really copied and not for what it asked for. */
static uint64_t present_px, present_calls;
uint64_t fb_present_px(void)    { return present_px; }
uint64_t fb_present_calls(void) { return present_calls; }

/* Push one rectangle of the back buffer to the framebuffer. Big rects are split
 * across CPUs; small ones (cursor, clock strip) copy locally (no IPI overhead).
 *
 * The clamp moved up here from fb_copy_rect: virtio_gpu_flush clamps again for
 * itself, but the ACCOUNTING has to be done on the rect that was really pushed,
 * and a compositor that presents a rect straddling the edge would otherwise be
 * credited with pixels no one copied. */
void fb_present_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    present_px += (uint64_t)w * (uint64_t)h;
    present_calls++;
    if (g_par_present && h >= 128) g_par_present(x, y, w, h);   /* RAM-to-RAM now (fast) */
    else fb_copy_rect(x, y, w, h);
    if (using_gpu) virtio_gpu_flush(x, y, w, h);               /* DMA the rect to the host */
}

/* Flush a rect that was drawn straight into fb_mem (e.g. the cursor overlay):
 * for virtio-gpu it must be DMA'd to the host; for VGA MMIO it's already live. */
void fb_flush_rect(int x, int y, int w, int h)
{
    if (using_gpu) virtio_gpu_flush(x, y, w, h);
}

/* ---- the pointer ----------------------------------------------------------
 *
 * When the display has a cursor plane the pointer is not a framebuffer object
 * at all: fb_cursor_image() hands the arrow to the display once, and every
 * subsequent move is one small command with no pixel written anywhere. Callers
 * must ask fb_cursor_hw() FIRST -- on a plain multiboot LFB there is no plane,
 * fb_cursor_move() does nothing, and the compositor has to keep drawing the
 * pointer itself or the desktop loses its cursor. */
int fb_cursor_hw(void) { return using_gpu && virtio_gpu_cursor_ready(); }

int fb_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    if (!fb_cursor_hw()) return -1;
    return virtio_gpu_cursor_define(argb, w, h, hot_x, hot_y);
}

void fb_cursor_move(int x, int y)
{
    if (fb_cursor_hw()) virtio_gpu_cursor_move(x, y);
}

/* Write one pixel straight to the framebuffer (not the back buffer): for the
 * cursor overlay, which must not contaminate the cursor-free composite. */
void fb_fb_put(int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= (int)fb_w || y >= (int)fb_h) return;
    volatile uint32_t *dst = (volatile uint32_t *)(fb_mem + (uint32_t)y * fb_pitch);
    dst[x] = color;
}

/* Text now goes through the anti-aliased Unicode engine (kernel/text.c). These
 * stay as thin wrappers so existing callers keep working; `y` is the cell top. */
/* These three take DEVICE coordinates but draw at the SCALED default UI size --
 * they are the kernel chrome's text, and the chrome converts its own point
 * geometry with fb_pt() before calling in. Routing the size through fb_ui_px()
 * here rather than at each of the ~20 call sites is what makes the menu bar and
 * window titles get re-rasterized (sharper), not stretched. */
void fb_char(int x, int y, char ch, uint32_t color)
{
    char s[2] = { ch, 0 };
    text_draw_sz(x, y, s, fb_ui_px(), color);
}

void fb_text(int x, int y, const char *s, uint32_t color)
{
    int px = fb_ui_px();
    int x0 = x, lh = text_line_height(px);
    char line[512]; int li = 0;
    for (;;) {
        if (*s == '\n' || *s == 0) {
            line[li] = 0; text_draw_sz(x0, y, line, px, color); li = 0;
            if (*s == 0) break;
            y += lh; s++;
        } else { if (li < 511) line[li++] = *s; s++; }
    }
}

int fb_text_width(const char *s) { return text_width_sz(s, fb_ui_px()); }

void fb_clear(uint32_t color)
{
    struct surface *s = T ? T : &screen;
    fb_fill_rect(0, 0, s->w, s->h, color);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    struct surface *s = T ? T : &screen;
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        uint32_t *row = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) row[i] = color;
    }
}

/* How much of local point (i,j) is covered by a rounded rect of size w x h with
 * corner `rad`? 0..255, where the old inside_round() returned 0 or 1.
 *
 * WHY THIS CHANGED (the staircase). The test used to be `dx*dx + dy*dy <=
 * rad*rad` -- a point sample, so a corner came out as a staircase and a circle
 * came out as a square with a nub on each axis. aui.h:34-43 names it: "that
 * staircase is the single loudest thing that dates the UI", and it is on
 * screen constantly -- the three traffic lights, the dark-mode knob, the
 * dock's slab, every window frame corner, and (through fb_blur_rect's
 * `corner`) the edge of every glass panel.
 *
 * WHY IT ASKS OPEN LOGIT RATHER THAN COMPUTING COVERAGE HERE. A fifth coverage
 * rasterizer in this file is exactly the mistake c/lib/gfx was built to end --
 * it deleted two of them on the way in. The corner tile is generated by the
 * same scanline rasterizer the toolkit and the browser draw with, is cached
 * by exact device geometry, and is checked against a 16x supersampled
 * analytic oracle (worst pixel error 0.091) plus a second, independently
 * written oracle in test-aui-mask. Nothing here has to be trusted on its own.
 *
 * WHY THIS IS TWO FUNCTIONS AND NOT ONE (the split that replaced the old
 * single cover_round()). shadow_tile() below has always fetched its mask ONCE
 * per shape from its caller and then indexed straight into it per pixel --
 * that is the cheap, correct shape. cover_round() did not follow its own
 * neighbour's pattern: it called gfx_mask_corner() -- a linear scan of a
 * 16-slot cache -- from INSIDE the per-pixel loop, so one 40x40 button ran
 * the cache lookup ~1,600 times for a shape that needs exactly one. Splitting
 * "get the tile" (corner_mask_for, called once per shape, before the loop)
 * from "read one pixel of it" (corner_cov, called once per pixel, a straight
 * array index) is what makes every caller below match shadow_tile exactly.
 * The coverage values, the mask cache, and the fallback rule are UNCHANGED --
 * this is a call-site restructuring, not a new computation. */
static const unsigned char *corner_mask_for(int w, int h, int *rad)
{
    int r = *rad;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { *rad = 0; return 0; }
    *rad = r;
    return gfx_mask_corner(GFX_MASK_FILL, r, r, 0);
}

/* Coverage of local point (i,j) against the tile `m` that corner_mask_for()
 * already fetched for this shape, at the ALREADY-CLAMPED radius `rad` it
 * already wrote back -- callers must pass that one, not the radius they
 * originally asked for, or the clamp and the tile disagree on where the
 * straight bands end. */
static int corner_cov(int i, int j, int w, int h, int rad, const unsigned char *m)
{
    if (rad <= 0) return 255;
    int cx = -1, cy = -1;
    if (i < rad)            cx = i;
    else if (i >= w - rad)  cx = w - 1 - i;
    if (j < rad)            cy = j;
    else if (j >= h - rad)  cy = h - 1 - j;
    if (cx < 0 || cy < 0) return 255;       /* in one of the straight bands */

    if (!m) {
        /* Radius past GFX_MASK_MAX: the old boolean point sample, i.e. THE
         * STAIRCASE this whole function exists to remove -- kept as the
         * fallback here rather than dropping the corner altogether, because a
         * missing pixel is worse than a sharp one. THIS is the ACCEPTABLE
         * half of gfx_mask_corner's contract (see its comment in
         * gfx_mask.c): complete, no geometry dropped, and no longer silent --
         * every refusal is counted the instant gfx_mask_corner returns NULL
         * (mrefuse / gfx_mask_refused() there), so a run where the desktop
         * chrome keeps hitting this fallback is a number someone can read,
         * not a guess from a screenshot. What this function does NOT do is
         * grow its own buffer to avoid the fallback the way aui.c's BIG_MASK
         * tile does for the toolkit's shapes: that buffer would be KERNEL
         * .bss (this file is compiled into the kernel -- see gfx.h's top
         * comment), and GFX_MASK_MAX was sized deliberately small for
         * exactly that reason. A bigger kernel buffer is a real, measured
         * cost paid by every boot; a corner that is occasionally a staircase
         * on an oversized window-manager shape is not. */
        int dx = rad - cx, dy = rad - cy;
        return dx * dx + dy * dy <= rad * rad ? 255 : 0;
    }
    return m[(long)cy * rad + cx];
}

/* Blend `color` over the target at (x,y) by coverage `a` (0..255). The three
 * hand-written copies of this arithmetic that used to sit in the loops below
 * are now one place, which is also the only place the rounding is decided. */
static void blend_cov(int x, int y, uint32_t color, int a)
{
    if (a <= 0) return;
    if (a >= 255) { fb_put(x, y, color); return; }
    int cr, cg, cb, br, bg, bb;
    unpack(color, &cr, &cg, &cb);
    unpack(fb_get(x, y), &br, &bg, &bb);
    fb_put(x, y, fb_rgb((uint8_t)((cr * a + br * (255 - a)) / 255),
                        (uint8_t)((cg * a + bg * (255 - a)) / 255),
                        (uint8_t)((cb * a + bb * (255 - a)) / 255)));
}

/* A circle is a rounded rect whose corner radius is half its side, so this is
 * the same coverage the corners use and the two cannot drift apart. It matters
 * here more than anywhere: the traffic lights are 12 px across, and at that
 * size the old `i*i + j*j <= r*r` did not read as a circle at all -- it read as
 * a square with one pixel poking out on each axis. */
void fb_fill_circle(int cx, int cy, int r, uint32_t color)
{
    int d = 2 * r + 1;
    int i0, j0, i1, j1;
    if (!clip_ij(cx - r, cy - r, d, d, &i0, &j0, &i1, &j1)) return;
    int rad = r;
    const unsigned char *m = corner_mask_for(d, d, &rad);
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++)
            blend_cov(cx - r + i, cy - r + j, color, corner_cov(i, j, d, d, rad, m));
}

void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++)
            blend_cov(x + i, y + j, color, corner_cov(i, j, w, h, rad, m));
}

/* Blit an 8-bit coverage bitmap as anti-aliased text: each cov[i] is the alpha
 * of `color` over the existing pixel. */
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{
    int cr, cg, cb; unpack(color, &cr, &cg, &cb);
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            int a = cov[j * w + i];
            if (!a) continue;
            if (a >= 255) { fb_put(x + i, y + j, color); continue; }
            int br, bg, bb; unpack(fb_get(x + i, y + j), &br, &bg, &bb);
            int nr = (cr * a + br * (255 - a)) / 255;
            int ng = (cg * a + bg * (255 - a)) / 255;
            int nb = (cb * a + bb * (255 - a)) / 255;
            fb_put(x + i, y + j, fb_rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
        }
    }
}

void fb_blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            int br, bg, bb;
            unpack(fb_get(x + i, y + j), &br, &bg, &bb);
            int nr = (r * a + br * (255 - a)) / 255;
            int ng = (g * a + bg * (255 - a)) / 255;
            int nb = (b * a + bb * (255 - a)) / 255;
            fb_put(x + i, y + j, fb_rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
        }
    }
}

void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            /* Two alphas meet here and they MULTIPLY: the caller's opacity and
             * the corner's coverage. Taking either one alone gives a panel that
             * is translucent in the middle and hard-edged at the corner, which
             * is what the boolean test used to produce. */
            int cov = corner_cov(i, j, w, h, rad, m);
            if (cov <= 0) continue;
            blend_cov(x + i, y + j, fb_rgb(r, g, b), a * cov / 255);
        }
    }
}

/* Blit one shadow corner tile, optionally mirrored, so ONE rasterized quadrant
 * serves all four corners. */
static void shadow_tile(int x, int y, int T, const unsigned char *m,
                        int alpha, int flipx, int flipy)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, T, T, &i0, &j0, &i1, &j1)) return;
    for (int j = j0; j < j1; j++) {
        int sj = flipy ? T - 1 - j : j;
        for (int i = i0; i < i1; i++) {
            int si = flipx ? T - 1 - i : i;
            blend_cov(x + i, y + j, fb_rgb(0, 0, 0), m[(long)sj * T + si] * alpha / 255);
        }
    }
}

/* A drop shadow around a rounded rect: offset `dy` down, falling off over
 * `blur`, peak opacity `alpha`.
 *
 * WHAT THIS REPLACES. wm.c drew window shadows as three nested constant-alpha
 * rectangles -- thicknesses 8/4/2 at alphas 11/22/40 -- which is a shadow with
 * TWO defects, and the second is the one you actually see. The alpha was a
 * three-step staircase instead of a falloff, and the bands were SQUARE around a
 * window whose corners are rounded, so each corner carried a dark nub of shadow
 * sitting outside a corner that curves away from it. It was the band down the
 * left of every window frame in every screenshot in this tree.
 *
 * WHY IT IS NOT A ROUNDED-RECT BLEND. The comment on the old code was right
 * about the cost and is the constraint here: the window is opaque and overdraws
 * its own interior, so blending a whole window-sized shape is ~30x wasted work
 * on every repaint, and a big window lagged on each flush. So this paints the
 * PERIMETER ONLY, in the shape aui_shadow_ex already uses in ring 3: four
 * corner tiles from the engine's cache, four edge strips, and one flat band
 * closing the sliver the offset opens under the caster.
 *
 * It is also CHEAPER than what it replaces. The old bands touched perimeter*14
 * pixels (8+4+2). This touches perimeter*blur plus four (blur+radius)^2 tiles,
 * and the tiles are cached across frames -- at blur=8 that is perimeter*8 plus
 * about 1,300 pixels, against perimeter*14, on a 640x480 window 19k against
 * 31k. Each edge strip is one constant-alpha fb_blend_rect per row, because
 * along an edge the falloff depends only on distance.
 *
 * The corners and the edges MUST fall off by the same curve or the joins show
 * as seams, which is exactly why gfx.h exposes gfx_shadow_falloff next to
 * gfx_corner_shadow -- the tile calls it per pixel, the strips call it per row,
 * and the sample points are matched (pixel centres, distance measured from the
 * caster's edge). */
/* How many fb_shadow() calls this boot had to shrink `blur` below what was
 * asked for, to dodge gfx_mask_corner's refusal instead of hitting it -- see
 * the comment on the clamp below. THIS is the one call site in the tree that
 * AVOIDS the refusal rather than handling it, which sounds strictly better
 * until you notice the shadow it draws is quietly not the shadow that was
 * requested, and nothing said so: unlike every other site fixed this
 * milestone (all of which go through gfx_mask_corner and are covered by ITS
 * refusal counter, gfx_mask.c's mrefuse), a pre-clamp changes the request
 * BEFORE gfx_mask_corner ever sees it, so that counter never fires here.
 * `fb_shadow_clamped` is this call site's own count, for exactly that gap --
 * kernel .bss cost: one unsigned int, the cheapest fix available for a
 * function that (correctly, per gfx.h's LIMITS section) cannot afford
 * aui.c's BIG_MASK buffers to avoid the clamp altogether. */
static unsigned fb_shadow_clamped;
unsigned fb_shadow_clamp_count(void) { return fb_shadow_clamped; }

void fb_shadow(int x, int y, int w, int h, int radius, int dy, int blur, uint8_t alpha)
{
    if (w <= 0 || h <= 0 || blur <= 0 || alpha == 0) return;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    /* Clamp the blur rather than drop the corners. gfx_mask_corner refuses a
     * tile past GFX_MASK_MAX and returns NULL, and losing the corners is far
     * more visible than a slightly tighter shadow -- it is the square-nub bug
     * this function exists to remove, reintroduced at high display scales. */
    if (blur + radius > GFX_MASK_MAX) { fb_shadow_clamped++; blur = GFX_MASK_MAX - radius; }
    if (blur <= 0) return;

    int sy = y + dy, T = blur + radius;
    const unsigned char *m = gfx_mask_corner(GFX_MASK_SHADOW, T, T, radius);
    if (m) {
        shadow_tile(x - blur,        sy - blur,          T, m, alpha, 0, 0);
        shadow_tile(x + w - radius,  sy - blur,          T, m, alpha, 1, 0);
        shadow_tile(x - blur,        sy + h - radius,    T, m, alpha, 0, 1);
        shadow_tile(x + w - radius,  sy + h - radius,    T, m, alpha, 1, 1);
    }

    /* The offset exposes a sliver of the shadow box's interior below the
     * caster, and the slices above deliberately do not paint any interior.
     * Left out, every window shows a dy-pixel gap of clean background between
     * itself and its own shadow -- which is what a shadow never does. */
    if (dy > 0 && w > 2 * radius)
        fb_blend_rect(x + radius, y + h, w - 2 * radius, dy, 0, 0, 0, alpha);
    /* When the offset exceeds the corner radius the same exposure reaches the
     * CORNER columns: the bottom tiles do not begin until sy + h - radius,
     * which with dy > radius sits BELOW the caster's bottom edge, and nothing
     * else touches the two radius-wide spans under the corners -- the sliver
     * above covers only the straight middle, the side strips only columns
     * outside the box. Found as four rows of bright wallpaper punched out of
     * the shadow at each bottom corner, the first time dy (14pt) grew past
     * the corner radius (10pt); every earlier tuning had dy <= radius, which
     * makes these rects zero-height, which is why the gap was never seen.
     * Full alpha is correct here for the same reason it is in the sliver:
     * these rows are interior to the offset shadow's body, above where the
     * corner curve begins. */
    if (dy > radius) {
        fb_blend_rect(x,              y + h, radius, dy - radius, 0, 0, 0, alpha);
        fb_blend_rect(x + w - radius, y + h, radius, dy - radius, 0, 0, 0, alpha);
    }

    long blur256 = (long)blur * 256;
    for (int e = 0; e < blur; e++) {
        int a = gfx_shadow_falloff((long)e * 256 + 128, blur256) * alpha / 255;
        if (a <= 0) continue;
        if (w > 2 * radius) {
            fb_blend_rect(x + radius, sy - 1 - e, w - 2 * radius, 1, 0, 0, 0, (uint8_t)a);
            fb_blend_rect(x + radius, sy + h + e, w - 2 * radius, 1, 0, 0, 0, (uint8_t)a);
        }
        if (h > 2 * radius) {
            fb_blend_rect(x - 1 - e, sy + radius, 1, h - 2 * radius, 0, 0, 0, (uint8_t)a);
            fb_blend_rect(x + w + e, sy + radius, 1, h - 2 * radius, 0, 0, 0, (uint8_t)a);
        }
    }
}

/* Linear interpolate two packed colors: a*(den-num)/den + b*num/den. */
static uint32_t color_lerp(uint32_t a, uint32_t b, int num, int den)
{
    int ar, ag, ab, br, bg, bb;
    unpack(a, &ar, &ag, &ab);
    unpack(b, &br, &bg, &bb);
    int r = ar + (br - ar) * num / den;
    int g = ag + (bg - ag) * num / den;
    int bl = ab + (bb - ab) * num / den;
    return fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

/* Lighten (delta>0) or darken (delta<0) a packed color by delta per channel. */
uint32_t fb_shade(uint32_t c, int delta)
{
    int r, g, b;
    unpack(c, &r, &g, &b);
    r += delta; g += delta; b += delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return fb_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* Vertical gradient fill: row j gets lerp(top..bottom). Integer, one lerp/row. */
void fb_fill_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    struct surface *s = T ? T : &screen;
    for (int j = j0; j < j1; j++) {
        uint32_t c = color_lerp(top, bottom, j, h > 1 ? h - 1 : 1);
        uint32_t *row = s->px + (long)(y + j) * s->w + x;
        for (int i = i0; i < i1; i++) row[i] = c;
    }
}

/* Rounded-rect vertical gradient (corners antialiased by corner_cov). */
void fb_round_rect_vgrad(int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom)
{
    int i0, j0, i1, j1;
    if (!clip_ij(x, y, w, h, &i0, &j0, &i1, &j1)) return;
    int rad = radius;
    const unsigned char *m = corner_mask_for(w, h, &rad);
    for (int j = j0; j < j1; j++) {
        uint32_t c = color_lerp(top, bottom, j, h > 1 ? h - 1 : 1);
        for (int i = i0; i < i1; i++)
            blend_cov(x + i, y + j, c, corner_cov(i, j, w, h, rad, m));
    }
}

/* Real-time backdrop blur of a rect on the current target -- the basis for
 * "vibrancy" (frost the live content behind a translucent panel). A separable
 * box blur: a horizontal moving-sum pass into a scratch buffer, then a vertical
 * moving-sum pass back into the target. Both passes are O(1) per pixel (the
 * window sum slides), so cost is O(w*h), radius-independent. `corner` > 0 rounds
 * the written region (corner pixels keep their pre-blur value), so a rounded
 * panel doesn't leave blurred nubs outside its corners. Integer-only. */
static uint32_t *blur_scratch;
static int blur_scratch_n;
void fb_blur_rect(int x, int y, int w, int h, int radius, int corner)
{
    struct surface *s = T ? T : &screen;
    if (!s->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius < 1) return;
    if (w * h > blur_scratch_n) {
        if (blur_scratch) kfree(blur_scratch);
        blur_scratch = (uint32_t *)kmalloc((unsigned long)w * h * 4);
        blur_scratch_n = blur_scratch ? w * h : 0;
    }
    if (!blur_scratch) return;
    uint32_t *tmp = blur_scratch;

    for (int j = 0; j < h; j++) {                 /* horizontal: target row -> tmp */
        const uint32_t *srow = s->px + (long)(y + j) * s->w + x;
        uint32_t *trow = tmp + (long)j * w;
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b;
        for (int k = 0; k <= radius && k < w; k++) { unpack(srow[k], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
        for (int i = 0; i < w; i++) {
            trow[i] = fb_rgb((uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt));
            int a = i + radius + 1; if (a < w)  { unpack(srow[a],  &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
            int d = i - radius;     if (d >= 0) { unpack(srow[d],  &r, &g, &b); sr -= r; sg -= g; sb -= b; cnt--; }
        }
    }
    /* Fetched once for the whole rect, not once per pixel -- see the header
     * comment on corner_mask_for()/corner_cov() earlier in this file. `corner`
     * is constant for this call, so hoisting it out of the double loop below
     * is exactly the shadow_tile() pattern, not a behaviour change: `crad` is
     * `corner`, clamped to this rect's own w/2,h/2 the same way it always was. */
    int crad = corner;
    const unsigned char *cm = corner_mask_for(w, h, &crad);
    for (int i = 0; i < w; i++) {                  /* vertical: tmp column -> target */
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b;
        for (int k = 0; k <= radius && k < h; k++) { unpack(tmp[(long)k * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
        for (int j = 0; j < h; j++) {
            /* The clip bounds the WRITE only. This primitive reads a
             * neighbourhood, so it cannot simply be run over a smaller
             * rectangle: the pixels it samples outside the clip are still
             * needed. wm.c's damage tracking therefore never hands it a
             * partially-clipped panel -- see the glass-panel expansion there --
             * and this check is the belt to that braces. */
            /* The corner is a COVERAGE, not a yes/no. A glass panel's edge is
             * the most visible curve on the machine -- it is the dock, the menu
             * bar and every titlebar -- and a boolean here left it stepped no
             * matter how smooth the blur inside it was. Blending the blurred
             * value against the pixel's own pre-blur colour is what makes the
             * edge fade out instead of ending. */
            int cov = corner_cov(i, j, w, h, crad, cm);
            if (cov > 0 && clip_px(s, x + i, y + j)) {
                uint32_t bl = fb_rgb((uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt));
                uint32_t *px = &s->px[(long)(y + j) * s->w + (x + i)];
                if (cov >= 255) {
                    *px = bl;
                } else {
                    int nr, ng, nb, orr, og, ob;
                    unpack(bl, &nr, &ng, &nb);
                    unpack(*px, &orr, &og, &ob);
                    *px = fb_rgb((uint8_t)((nr * cov + orr * (255 - cov)) / 255),
                                 (uint8_t)((ng * cov + og  * (255 - cov)) / 255),
                                 (uint8_t)((nb * cov + ob  * (255 - cov)) / 255));
                }
            }
            int a = j + radius + 1; if (a < h)  { unpack(tmp[(long)a * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
            int d = j - radius;     if (d >= 0) { unpack(tmp[(long)d * w + i], &r, &g, &b); sr -= r; sg -= g; sb -= b; cnt--; }
        }
    }
}

/* "Liquid Glass": frost the live backdrop of a rounded-rect panel AND refract it
 * through the curved rim, with a specular rim highlight + body tint -- Apple's
 * Liquid Glass material, integer-only (modelled from a Python optics study,
 * /tmp/glass_study.py). Pipeline: copy the backdrop -> separable box blur -> for
 * each pixel compute the rounded-rect signed distance + outward normal; within an
 * edge band sample the blurred backdrop displaced INWARD along the normal (the
 * lens/bevel that bends the background at the rim); tint; add a rim highlight
 * where the bevel faces the (up-left) light and a faint contact shadow opposite.
 * Pixels outside the rounded rect are left untouched (so a drop shadow shows). */
static uint32_t *glass_buf;
static int glass_buf_n;
static int *glass_line;
static int glass_line_n;
void fb_liquid_glass(int x, int y, int w, int h, int radius,
                     uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    fb_liquid_glass_cut(x, y, w, h, radius, tr, tg, tb, ta, 0);
}

void fb_liquid_glass_cut(int x, int y, int w, int h, int radius,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                         unsigned cut)
{
    struct surface *s = T ? T : &screen;
    if (!s->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0 || radius < 1) return;
    if (w * h > glass_buf_n) { if (glass_buf) kfree(glass_buf); glass_buf = (uint32_t *)kmalloc((unsigned long)w * h * 4); glass_buf_n = glass_buf ? w * h : 0; }
    int lmax = w > h ? w : h;
    if (lmax * 3 > glass_line_n) { if (glass_line) kfree(glass_line); glass_line = (int *)kmalloc((unsigned long)lmax * 3 * sizeof(int)); glass_line_n = glass_line ? lmax * 3 : 0; }
    if (!glass_buf || !glass_line) { fb_blur_rect(x, y, w, h, 6, radius); return; }   /* frost-only fallback */
    uint32_t *g = glass_buf;

    for (int j = 0; j < h; j++) {            /* copy live backdrop -> scratch */
        const uint32_t *srow = s->px + (long)(y + j) * s->w + x;
        uint32_t *drow = g + (long)j * w;
        for (int i = 0; i < w; i++) drow[i] = srow[i];
    }
    const int RB = 6;
    for (int j = 0; j < h; j++) {            /* blur: horizontal moving sum */
        uint32_t *row = g + (long)j * w; int sr = 0, sg = 0, sb = 0, cnt = 0, r, gg, b;
        for (int k = 0; k <= RB && k < w; k++) { unpack(row[k], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
        for (int i = 0; i < w; i++) {
            glass_line[i * 3] = sr / cnt; glass_line[i * 3 + 1] = sg / cnt; glass_line[i * 3 + 2] = sb / cnt;
            int a = i + RB + 1; if (a < w)  { unpack(row[a], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
            int d = i - RB;     if (d >= 0) { unpack(row[d], &r, &gg, &b); sr -= r; sg -= gg; sb -= b; cnt--; }
        }
        for (int i = 0; i < w; i++) row[i] = fb_rgb((uint8_t)glass_line[i * 3], (uint8_t)glass_line[i * 3 + 1], (uint8_t)glass_line[i * 3 + 2]);
    }
    for (int i = 0; i < w; i++) {            /* blur: vertical moving sum */
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, gg, b;
        for (int k = 0; k <= RB && k < h; k++) { unpack(g[(long)k * w + i], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
        for (int j = 0; j < h; j++) {
            glass_line[j * 3] = sr / cnt; glass_line[j * 3 + 1] = sg / cnt; glass_line[j * 3 + 2] = sb / cnt;
            int a = j + RB + 1; if (a < h)  { unpack(g[(long)a * w + i], &r, &gg, &b); sr += r; sg += gg; sb += b; cnt++; }
            int d = j - RB;     if (d >= 0) { unpack(g[(long)d * w + i], &r, &gg, &b); sr -= r; sg -= gg; sb -= b; cnt--; }
        }
        for (int j = 0; j < h; j++) g[(long)j * w + i] = fb_rgb((uint8_t)glass_line[j * 3], (uint8_t)glass_line[j * 3 + 1], (uint8_t)glass_line[j * 3 + 2]);
    }

    int cx = w / 2, cy = h / 2, ix = cx - radius, iy = cy - radius;
    int minside = w < h ? w : h;
    int E = 22; if (E > minside / 2) E = minside / 2; if (E < 4) E = 4;   /* thin panels -> smaller band */
    int REFRACT = 18; if (REFRACT > E) REFRACT = E;
    glass_build_lut(E, REFRACT);           /* cached on (E, REFRACT); see above */
    int ELUT = E > GLASS_E_MAX ? GLASS_E_MAX : E;
    const int SPEC = 64;                   /* was 150, before the rim existed */
    int eband = E * 7 / 10; if (eband < 1) eband = 1;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int px = i - cx, py = j - cy, ax = px < 0 ? -px : px, ay = py < 0 ? -py : py;
            /* A CUT edge is not an edge (see fb.h): folding its half-axis to 0
             * puts every pixel on that side "deep inside" as far as the SDF,
             * the rim band and the Fresnel term are concerned, so the bevel
             * machinery below never fires there. The frost and tint above are
             * untouched -- a cut panel is still glass, it just has no rim. */
            if ((cut & GLASS_CUT_LEFT)   && px < 0) ax = 0;
            if ((cut & GLASS_CUT_RIGHT)  && px > 0) ax = 0;
            if ((cut & GLASS_CUT_TOP)    && py < 0) ay = 0;
            if ((cut & GLASS_CUT_BOTTOM) && py > 0) ay = 0;
            int qx = ax - ix, qy = ay - iy, qxc = qx > 0 ? qx : 0, qyc = qy > 0 ? qy : 0;
            /* The distance is carried in 8.8 -- the square sum is shifted by 16
             * before the root, so isqrt returns 256*d. A whole-pixel SDF cannot
             * describe an edge that falls between two pixels, and this panel's
             * edge is the most looked-at curve on the machine. */
            int outd = (qxc || qyc)
                     ? (int)gl_isqrt(((unsigned long)qxc * qxc + (unsigned long)qyc * qyc) << 16)
                     : 0;
            int ins = qx > qy ? qx : qy; if (ins > 0) ins = 0;
            int sdf = outd + (ins - radius) * 256;               /* 8.8, <0 inside */
            /* COVERAGE, not a yes/no. A pixel whose centre sits exactly on the
             * boundary is half covered, so the ramp is centred on sdf==0 and one
             * pixel wide. This is the whole reason the panel's edge stops being
             * a staircase: `if (sdf >= 0) continue` drew every edge pixel at
             * full strength and then stopped dead. */
            int gcov = 128 - sdf;
            if (gcov <= 0) continue;                             /* outside */
            if (gcov > 255) gcov = 255;
            int depth = -sdf / 256; if (depth < 0) depth = 0;
            if (!clip_px(s, x + i, y + j)) continue;             /* see the note in fb_blur_rect */
            int gx, gy;
            if (qxc > 0 || qyc > 0) { gx = (px < 0 ? -1 : 1) * qxc; gy = (py < 0 ? -1 : 1) * qyc; }
            else if (qx > qy)       { gx = (px < 0 ? -1 : 1); gy = 0; }
            else                    { gx = 0; gy = (py < 0 ? -1 : 1); }
            int nlen = (int)gl_isqrt((unsigned long)gx * gx + (unsigned long)gy * gy); if (!nlen) nlen = 1;
            int nx = gx * 256 / nlen, ny = gy * 256 / nlen;       /* outward unit x256 */
            /* One index instead of the old squared ramp, and three of them
             * because R, G and B leave the rim at different angles. Outside the
             * edge band all three are zero and the three samples collapse onto
             * the same pixel, so the interior costs what it always did. */
            int di = depth < ELUT ? depth : ELUT;
            int r, gg, b;
            {
                int dr = glass_disp[0][di], dg = glass_disp[1][di], db = glass_disp[2][di];
                int xr = i - nx * dr / 256, yr = j - ny * dr / 256;
                int xg = i - nx * dg / 256, yg = j - ny * dg / 256;
                int xb = i - nx * db / 256, yb = j - ny * db / 256;
                if (xr < 0) xr = 0; if (xr >= w) xr = w - 1;
                if (yr < 0) yr = 0; if (yr >= h) yr = h - 1;
                if (xg < 0) xg = 0; if (xg >= w) xg = w - 1;
                if (yg < 0) yg = 0; if (yg >= h) yg = h - 1;
                if (xb < 0) xb = 0; if (xb >= w) xb = w - 1;
                if (yb < 0) yb = 0; if (yb >= h) yb = h - 1;
                int t1, t2;
                unpack(g[(long)yr * w + xr], &r,  &t1, &t2);
                unpack(g[(long)yg * w + xg], &t1, &gg, &t2);
                unpack(g[(long)yb * w + xb], &t1, &t2, &b);
            }
            r += (tr - r) * ta / 255; gg += (tg - gg) * ta / 255; b += (tb - b) * ta / 255;
            int band = 256 - depth * 256 / eband; if (band < 0) band = 0;
            int facing = (nx * (-154) + ny * (-205)) / 256; if (facing < 0) facing = 0; if (facing > 256) facing = 256;
            /* THE EDGE. Reflectance goes to 1 at grazing incidence, so the
             * outermost pixel of the bevel is a mirror and the one after it
             * is not -- a hairline, all the way round, which is the cue that
             * makes this read as an object with thickness instead of a soft
             * patch. The lerp is toward the environment rather than toward
             * white, and it REPLACES what it reflects, so the slightly darker
             * pixel just inside the bright one is the same computation.
             * See glass.c for the table and for which part of this is faked. */
            /* The environment is directional only where the surface TILTS. The
             * flat middle of the panel faces the viewer, so it mirrors the same
             * thing everywhere, and it must not consult `facing` -- because in
             * the flat middle the normal is not measured, it is the guess two
             * branches up (`qx > qy ? horizontal : vertical`), and that guess
             * FLIPS across the 45-degree diagonal. Every earlier user of the
             * normal was multiplied by `band`, which is zero past 15 px, so the
             * discontinuity had never reached a pixel. R0 reflectance reaches
             * every pixel, and the first version of this line put a visible
             * diagonal seam down the middle of Finder's sidebar: 4% of an 87
             * level swing is 3 levels, and 3 levels on flat white is a line. */
            int tilt = depth < E ? (E - depth) * 256 / E : 0;
            int env = 176 + (facing - 128) * tilt / 256;
            if (env < 96)  env = 96;
            if (env > 250) env = 250;
            int fr = glass_fres[di];
            r += (env - r) * fr / 255; gg += (env - gg) * fr / 255; b += (env - b) * fr / 255;
            /* The wide wash stays, at less than half its old strength: it is
             * the body sheen, not the edge. Turning it up was the thing that
             * could not work -- a 15-pixel gradient is not a 1-pixel line. */
            int hi = facing * band / 256; hi = hi * hi / 256; hi = hi * SPEC / 256;
            r += (255 - r) * hi / 256; gg += (255 - gg) * hi / 256; b += (255 - b) * hi / 256;
            int op = (nx * 154 + ny * 205) / 256; if (op < 0) op = 0;
            int sh = op * band / 256; sh = sh * sh / 256; sh = sh * 46 / 256;
            r -= r * sh / 256; gg -= gg * sh / 256; b -= b * sh / 256;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            uint32_t *dstp = &s->px[(long)(y + j) * s->w + (x + i)];
            if (gcov >= 255) {
                *dstp = fb_rgb((uint8_t)r, (uint8_t)gg, (uint8_t)b);
            } else {
                /* The edge pixel is part panel, part whatever was behind it --
                 * and `behind it` is still in the target, because this loop
                 * reads from the saved copy `g` and writes here. */
                int orr, og, ob; unpack(*dstp, &orr, &og, &ob);
                *dstp = fb_rgb((uint8_t)((r  * gcov + orr * (255 - gcov)) / 255),
                               (uint8_t)((gg * gcov + og  * (255 - gcov)) / 255),
                               (uint8_t)((b  * gcov + ob  * (255 - gcov)) / 255));
            }
        }
    }
}

/* Blit a straight-RGBA source (sw x sh) into the dest rect (dx,dy,dw,dh) of the
 * current target with nearest-neighbour scaling and per-pixel alpha. The loops
 * are clipped to the visible target region (in 64-bit, so user-supplied dx/dw
 * can't overflow the math) WITHOUT rescaling: sx/sy are still computed against
 * the original dw/dh, so a partially off-screen blit crops exactly as before.
 * This is what bounds SYS_GUI_BLIT: an unclipped dw*dh double loop (up to
 * ~(2^31)^2 iterations) inside the syscall gate would freeze the machine. */
void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh)
{
    struct surface *t = T ? T : &screen;
    if (!rgba || !t->px || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    long i0 = dx < 0 ? -(long)dx : 0, i1 = dw;
    long j0 = dy < 0 ? -(long)dy : 0, j1 = dh;
    if (i1 > (long)t->w - dx) i1 = (long)t->w - dx;
    if (j1 > (long)t->h - dy) j1 = (long)t->h - dy;
    if (t->clip_on) {                           /* clamp to the target's scissor too */
        if (t->clx0 - dx > i0) i0 = t->clx0 - dx;
        if (t->cly0 - dy > j0) j0 = t->cly0 - dy;
        if (t->clx1 - dx < i1) i1 = t->clx1 - dx;
        if (t->cly1 - dy < j1) j1 = t->cly1 - dy;
    }
    if (i0 >= i1 || j0 >= j1) return;
    for (long j = j0; j < j1; j++) {
        int sy = (int)(j * sh / dh);
        for (long i = i0; i < i1; i++) {
            int sx = (int)(i * sw / dw);
            const uint8_t *p = rgba + ((sy * sw + sx) * 4);
            int a = p[3];
            int px = (int)(dx + i), py = (int)(dy + j);
            if (!a) continue;
            if (a >= 255) { fb_put(px, py, fb_rgb(p[0], p[1], p[2])); continue; }
            int br, bg, bb; unpack(fb_get(px, py), &br, &bg, &bb);
            int nr = (p[0] * a + br * (255 - a)) / 255;
            int ng = (p[1] * a + bg * (255 - a)) / 255;
            int nb = (p[2] * a + bb * (255 - a)) / 255;
            fb_put(px, py, fb_rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
        }
    }
}
