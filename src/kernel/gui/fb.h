#ifndef AQUA_FB_H
#define AQUA_FB_H

#include <stdint.h>

/* Find GRUB's framebuffer tag, map the framebuffer, and prepare drawing.
 * Returns 0 if no usable 32-bpp RGB framebuffer is available. */
int fb_init(uint64_t mb_info_addr);

uint32_t fb_width(void);
uint32_t fb_height(void);

#define AQUA_FONT_W 8
#define AQUA_FONT_H 16

/* Pack 8-bit channels into the framebuffer's native pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Double buffering: route drawing to `buf` (a width*height*4 RAM buffer),
 * then blit to the visible framebuffer with fb_present(). */
void fb_set_backbuffer(uint32_t *buf);
void fb_present(void);
void fb_present_rect(int x, int y, int w, int h);   /* push one rect back->framebuffer */
void fb_copy_rect(int x, int y, int w, int h);      /* raw band copy (parallel workers) */
void fb_set_present_par(void (*fn)(int, int, int, int));  /* register SMP parallel present */
void fb_fb_put(int x, int y, uint32_t color);       /* write straight to the framebuffer */

/* An off-screen drawing target (e.g. an application window's canvas). */
struct surface {
    uint32_t *px;
    int w, h;
};

/* Route subsequent drawing to `s`, or NULL for the screen back buffer. */
void fb_target(struct surface *s);

/* Restrict drawing to a rectangle on the current target (half-open). */
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);

/* Copy `src` into the current target at (dx,dy), clipped. */
void fb_blit_surface(int dx, int dy, const struct surface *src);

/* Text (8x16 bitmap font). */
void fb_char(int x, int y, char c, uint32_t color);
void fb_text(int x, int y, const char *s, uint32_t color);
int  fb_text_width(const char *s);

void fb_clear(uint32_t color);
void fb_put(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_fill_circle(int cx, int cy, int r, uint32_t color);
void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color);

/* Blit an 8-bit coverage bitmap (w*h) as anti-aliased text in `color`. */
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color);

/* Blit a straight-RGBA image (sw*sh) into dest rect (dx,dy,dw,dh), scaled
 * (nearest) with per-pixel alpha. */
void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh);

/* Alpha-blended (0 = transparent, 255 = opaque) — for frosted-glass surfaces. */
void fb_blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif /* AQUA_FB_H */
