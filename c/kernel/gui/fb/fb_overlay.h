#ifndef LOGIT_FB_OVERLAY_H
#define LOGIT_FB_OVERLAY_H

#include <stdint.h>

/* One shared ceiling for the WM sprite plane and the present scratch. */
#define FB_OVERLAY_SIZE 64

/* Half-open device coordinates, matching the compositor's damage rectangles. */
struct fb_irect {
    int x0, y0, x1, y1;
};

/* The sprite uses device-native RGB lanes and a straight alpha byte, exactly
 * as wm.c's draw_cursor_back does. Strides count pixels, never bytes. */
struct fb_overlay {
    const uint32_t *argb;
    int stride, bw, bh, hot_x, hot_y, x, y, active;
};

int fb_overlay_box(const struct fb_overlay *ov, int sw, int sh, struct fb_irect *out);

/* rect must already be clipped to back's screen. dst starts at rect's origin;
 * its row padding is left untouched. No allocation or framebuffer reads. */
void fb_overlay_compose(uint32_t *dst, int dst_stride,
                        const uint32_t *back, int back_stride,
                        const struct fb_irect *rect, const struct fb_overlay *ov,
                        unsigned rpos, unsigned gpos, unsigned bpos);

#endif
