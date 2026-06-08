#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "vmm.h"
#include "text.h"
#include "virtio_gpu.h"

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

/* Optional clip rectangle on the current target (clx0..clx1, cly0..cly1),
 * half-open; disabled when clip_on == 0. */
static int clip_on, clx0, cly0, clx1, cly1;
void fb_set_clip(int x, int y, int w, int h) { clip_on = 1; clx0 = x; cly0 = y; clx1 = x + w; cly1 = y + h; }
void fb_clear_clip(void) { clip_on = 0; }

uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

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
        return 1;
    }

    uint32_t total = *(volatile uint32_t *)mb_info_addr;
    uint8_t *p = (uint8_t *)(mb_info_addr + 8);
    uint8_t *end = (uint8_t *)(mb_info_addr + total);
    struct mb2_fb_tag *fb = NULL;

    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0)
            break;
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
    return 1;
}

void fb_put(int x, int y, uint32_t color)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return;
    if (clip_on && (x < clx0 || y < cly0 || x >= clx1 || y >= cly1))
        return;
    s->px[y * s->w + x] = color;
}

static uint32_t fb_get(int x, int y)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    if (clip_on && (x < clx0 || y < cly0 || x >= clx1 || y >= cly1))
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
void fb_blit_surface(int dx, int dy, const struct surface *src)
{
    struct surface *t = T;
    if (!t || !t->px || !src->px) return;
    for (int y = 0; y < src->h; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= t->h) continue;
        int x0 = 0, x1 = src->w;
        if (dx + x0 < 0) x0 = -dx;
        if (dx + x1 > t->w) x1 = t->w - dx;
        const uint32_t *srow = src->px + (uint32_t)y * src->w;
        uint32_t *drow = t->px + (uint32_t)ty * t->w + dx;
        for (int x = x0; x < x1; x++) drow[x] = srow[x];
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

/* Push one rectangle of the back buffer to the framebuffer. Big rects are split
 * across CPUs; small ones (cursor, clock strip) copy locally (no IPI overhead). */
void fb_present_rect(int x, int y, int w, int h)
{
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
void fb_char(int x, int y, char ch, uint32_t color)
{
    char s[2] = { ch, 0 };
    text_draw(x, y, s, color);
}

void fb_text(int x, int y, const char *s, uint32_t color)
{
    int x0 = x, lh = text_line_height(TEXT_UI_PX);
    char line[512]; int li = 0;
    for (;;) {
        if (*s == '\n' || *s == 0) {
            line[li] = 0; text_draw(x0, y, line, color); li = 0;
            if (*s == 0) break;
            y += lh; s++;
        } else { if (li < 511) line[li++] = *s; s++; }
    }
}

int fb_text_width(const char *s) { return text_width(s); }

void fb_clear(uint32_t color)
{
    struct surface *s = T ? T : &screen;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            fb_put(x, y, color);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            fb_put(x + i, y + j, color);
}

void fb_fill_circle(int cx, int cy, int r, uint32_t color)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                fb_put(cx + i, cy + j, color);
}

/* Is local point (i,j) inside a rounded rect of size w x h, corner `rad`? */
static int inside_round(int i, int j, int w, int h, int rad)
{
    int dx = 0, dy = 0;
    if (i < rad)            dx = rad - i;
    else if (i >= w - rad)  dx = i - (w - rad - 1);
    if (j < rad)            dy = rad - j;
    else if (j >= h - rad)  dy = j - (h - rad - 1);
    return dx * dx + dy * dy <= rad * rad;
}

void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (inside_round(i, j, w, h, radius))
                fb_put(x + i, y + j, color);
}

/* Blit an 8-bit coverage bitmap as anti-aliased text: each cov[i] is the alpha
 * of `color` over the existing pixel. */
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{
    int cr, cg, cb; unpack(color, &cr, &cg, &cb);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
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
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
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
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (!inside_round(i, j, w, h, radius))
                continue;
            int br, bg, bb;
            unpack(fb_get(x + i, y + j), &br, &bg, &bb);
            int nr = (r * a + br * (255 - a)) / 255;
            int ng = (g * a + bg * (255 - a)) / 255;
            int nb = (b * a + bb * (255 - a)) / 255;
            fb_put(x + i, y + j, fb_rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
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
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t c = color_lerp(top, bottom, j, h > 1 ? h - 1 : 1);
        for (int i = 0; i < w; i++) fb_put(x + i, y + j, c);
    }
}

/* Rounded-rect vertical gradient (corners cut by inside_round). */
void fb_round_rect_vgrad(int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t c = color_lerp(top, bottom, j, h > 1 ? h - 1 : 1);
        for (int i = 0; i < w; i++)
            if (inside_round(i, j, w, h, radius)) fb_put(x + i, y + j, c);
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
    for (int i = 0; i < w; i++) {                  /* vertical: tmp column -> target */
        int sr = 0, sg = 0, sb = 0, cnt = 0, r, g, b;
        for (int k = 0; k <= radius && k < h; k++) { unpack(tmp[(long)k * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
        for (int j = 0; j < h; j++) {
            if (corner <= 0 || inside_round(i, j, w, h, corner))
                s->px[(long)(y + j) * s->w + (x + i)] = fb_rgb((uint8_t)(sr / cnt), (uint8_t)(sg / cnt), (uint8_t)(sb / cnt));
            int a = j + radius + 1; if (a < h)  { unpack(tmp[(long)a * w + i], &r, &g, &b); sr += r; sg += g; sb += b; cnt++; }
            int d = j - radius;     if (d >= 0) { unpack(tmp[(long)d * w + i], &r, &g, &b); sr -= r; sg -= g; sb -= b; cnt--; }
        }
    }
}

/* Blit a straight-RGBA source (sw x sh) into the dest rect (dx,dy,dw,dh) of the
 * current target with nearest-neighbour scaling and per-pixel alpha. */
void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh)
{
    if (!rgba || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int j = 0; j < dh; j++) {
        int sy = j * sh / dh;
        for (int i = 0; i < dw; i++) {
            int sx = i * sw / dw;
            const uint8_t *p = rgba + ((sy * sw + sx) * 4);
            int a = p[3];
            if (!a) continue;
            if (a >= 255) { fb_put(dx + i, dy + j, fb_rgb(p[0], p[1], p[2])); continue; }
            int br, bg, bb; unpack(fb_get(dx + i, dy + j), &br, &bg, &bb);
            int nr = (p[0] * a + br * (255 - a)) / 255;
            int ng = (p[1] * a + bg * (255 - a)) / 255;
            int nb = (p[2] * a + bb * (255 - a)) / 255;
            fb_put(dx + i, dy + j, fb_rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
        }
    }
}
