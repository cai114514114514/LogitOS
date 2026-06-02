#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "vmm.h"
#include "text.h"

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

static volatile uint8_t *fb_mem;
static uint32_t fb_pitch, fb_w, fb_h;
static uint8_t rpos, gpos, bpos;

static struct surface screen;     /* wraps the back buffer (the visible screen) */
static struct surface *T;         /* current draw target (defaults to screen) */

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
    s->px[y * s->w + x] = color;
}

static uint32_t fb_get(int x, int y)
{
    struct surface *s = T ? T : &screen;
    if (!s->px || x < 0 || y < 0 || x >= s->w || y >= s->h)
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

void fb_blit_surface(int dx, int dy, const struct surface *src)
{
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++)
            fb_put(dx + x, dy + y, src->px[y * src->w + x]);
}

void fb_present(void)
{
    if (!screen.px)
        return;
    for (uint32_t y = 0; y < fb_h; y++) {
        volatile uint32_t *dst = (volatile uint32_t *)(fb_mem + y * fb_pitch);
        const uint32_t *src = screen.px + y * fb_w;
        for (uint32_t x = 0; x < fb_w; x++)
            dst[x] = src[x];
    }
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
