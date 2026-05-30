#ifndef AQUA_FB_H
#define AQUA_FB_H

#include <stdint.h>

/* Find GRUB's framebuffer tag, map the framebuffer, and prepare drawing.
 * Returns 0 if no usable 32-bpp RGB framebuffer is available. */
int fb_init(uint64_t mb_info_addr);

uint32_t fb_width(void);
uint32_t fb_height(void);

/* Pack 8-bit channels into the framebuffer's native pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_clear(uint32_t color);
void fb_put(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_fill_circle(int cx, int cy, int r, uint32_t color);
void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color);

/* Alpha-blended (0 = transparent, 255 = opaque) — for frosted-glass surfaces. */
void fb_blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif /* AQUA_FB_H */
