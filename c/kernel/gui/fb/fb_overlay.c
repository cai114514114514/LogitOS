#include "fb_overlay.h"
#include "openlogit_display.h"
#include <stddef.h>

/* Keep dirty_cursor's extra restoration row/column, even though those pixels
 * are transparent. Widen before subtracting so an off-screen hotspot cannot
 * wrap into a plausible on-screen rectangle. */
int fb_overlay_box(const struct fb_overlay *ov, int sw, int sh, struct fb_irect *out)
{
    if (!ov || !out || !ov->active || !ov->argb ||
        ov->bw <= 0 || ov->bh <= 0 || ov->stride < ov->bw || sw <= 0 || sh <= 0) {
        return 0;
    }
    int64_t x0 = (int64_t)ov->x - ov->hot_x;
    int64_t y0 = (int64_t)ov->y - ov->hot_y;
    int64_t x1 = x0 + ov->bw + 1;
    int64_t y1 = y0 + ov->bh + 1;
#ifndef GFX_NEGCTL_OVERLAY_NO_CLAMP
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
#endif
    if (x0 >= x1 || y0 >= y1) return 0;
    *out = (struct fb_irect){(int)x0, (int)y0, (int)x1, (int)y1};
    return 1;
}

/* Compose from RAM, never from the WC scanout (Intel SDM Vol.3A 11.3.1).
 * Reuse the old cursor blitter rather than approximating its alpha rounding
 * or its treatment of the reserved byte: transparent pixels retain that byte,
 * while painted pixels clear it. Linux v6.12 soft_cursor() in
 * drivers/video/fbdev/core/softcursor.c similarly builds an image before
 * fb_imageblit; this implementation keeps the RAM back buffer cursor-free.
 * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/video/fbdev/core/softcursor.c?h=v6.12
 */
void fb_overlay_compose(uint32_t *dst, int dst_stride,
                        const uint32_t *back, int back_stride,
                        const struct fb_irect *rect, const struct fb_overlay *ov,
                        unsigned rpos, unsigned gpos, unsigned bpos)
{
    int width = rect->x1 - rect->x0;
    int height = rect->y1 - rect->y0;
    if (width <= 0 || height <= 0 || dst_stride < width) return;
    for (int row = 0; row < height; row++) {
        const uint32_t *source = back + (size_t)(rect->y0 + row) * back_stride + rect->x0;
        __builtin_memcpy(dst + (size_t)row * dst_stride, source, (size_t)width * 4);
    }
    if (!ov || !ov->active || !ov->argb || ov->bw <= 0 || ov->bh <= 0 ||
        ov->stride < ov->bw) return;

    int64_t origin_x = (int64_t)ov->x - ov->hot_x;
    int64_t origin_y = (int64_t)ov->y - ov->hot_y;
    if (origin_x >= rect->x1 || origin_y >= rect->y1 ||
        origin_x + ov->bw <= rect->x0 || origin_y + ov->bh <= rect->y0) return;

    struct ol_display draw = {0};
    struct ol_pixel_target target = {
        .px = dst,
        .w = dst_stride,
        .h = height,
        .clip_on = 1,
        .clx0 = 0,
        .cly0 = 0,
        .clx1 = width,
        .cly1 = height,
    };
    if (ol_display_bind(&draw, &target, rpos, gpos, bpos) != OL_OK) return;
    ol_display_blit_argb(&draw, (int)(origin_x - rect->x0), (int)(origin_y - rect->y0),
                         ov->argb, ov->bw, ov->bh, ov->stride);
}
