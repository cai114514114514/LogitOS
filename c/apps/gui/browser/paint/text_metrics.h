#ifndef BROWSER_CONTROL_TEXT_METRICS_H
#define BROWSER_CONTROL_TEXT_METRICS_H

#include <stdint.h>
#include "logit_abi.h"

/* All origins sent to WM are integer logical points. Keep the native ink in
 * device pixels until choosing the nearest realizable S(y); scaling relative
 * offsets separately is wrong at 150%, especially across negative origins. */
struct control_text_vertical {
    int draw_y, selection_y, selection_h, caret_y, caret_h;
};
static inline int64_t ctl_floor_div(int64_t a, int64_t b)
{ int64_t q = a / b; return q - (a % b < 0); }
static inline int64_t ctl_ceil_div(int64_t a, int64_t b)
{ int64_t q = a / b; return q + (a % b > 0); }
static inline int64_t ctl_device_y(int64_t y, int scale)
{ return ctl_floor_div(y * scale, 100); }
static inline struct control_text_vertical control_text_fallback(int cy, int h, int font)
{
    int y = cy + (h > font ? (h - font) / 2 : 0);
    struct control_text_vertical v = { y, y, font + font / 5, y - 1, font + 2 };
    return v;
}
/* Ink as small as a period still needs an ordinary readable caret. Expand
 * each marker to its existing font-height minimum, then clip to the declared
 * content box; only the text draw origin follows the tight ink rectangle. */
static inline void control_text_marker_box(int cy, int h, int64_t top,
    int64_t bottom, int minimum, int *out_y, int *out_h)
{
    if (minimum >= h) { *out_y = cy; *out_h = h; return; }
    if (bottom - top < minimum) {
        top -= (minimum - (bottom - top)) / 2;
        bottom = top + minimum;
    }
    if (top < cy) top = cy;
    if (bottom > (int64_t)cy + h) bottom = (int64_t)cy + h;
    if (bottom < top) bottom = top;
    *out_y = (int)top; *out_h = (int)(bottom - top);
}
static inline struct control_text_vertical control_text_center_ink(
    int cy, int h, int font, const struct logit_text_metrics *m)
{
    struct control_text_vertical v = control_text_fallback(cy, h, font);
    if (!m || !m->has_ink || m->scale_percent < 100 ||
        m->scale_percent > 400 || m->ink_bottom <= m->ink_top || h <= 0)
        return v;
    int scale = m->scale_percent;
    int64_t a = ctl_device_y(cy, scale), b = ctl_device_y((int64_t)cy + h, scale);
    int64_t twice_origin = a + b - m->ink_top - m->ink_bottom;
    int64_t floor_origin = ctl_floor_div(twice_origin, 2);
    /* Last logical origin whose S(y) is <= floor(ideal), then its successor.
     * These bracket the ideal even where the scale skips a device pixel. */
    int64_t y0 = ctl_ceil_div(100 * (floor_origin + 1), scale) - 1, y1 = y0 + 1;
    int64_t e0 = 2 * ctl_device_y(y0, scale) - twice_origin;
    int64_t e1 = 2 * ctl_device_y(y1, scale) - twice_origin;
    if (e0 < 0) e0 = -e0;
    if (e1 < 0) e1 = -e1;
    int64_t y = e1 < e0 ? y1 : y0;
    v.draw_y = (int)y;
    /* Selection and caret cover the actual positioned ink, not the possibly
     * negative draw origin. Outward conversion preserves both device edges;
     * the unchanged content clip remains authoritative for small controls. */
    int64_t top = ctl_device_y(y, scale) + m->ink_top;
    int64_t bottom = ctl_device_y(y, scale) + m->ink_bottom;
    int64_t pt = ctl_floor_div(top * 100, scale);
    int64_t pb = ctl_ceil_div(bottom * 100, scale);
    control_text_marker_box(cy, h, pt, pb, font + font / 5,
                            &v.selection_y, &v.selection_h);
    control_text_marker_box(cy, h, pt, pb, font + 2,
                            &v.caret_y, &v.caret_h);
    return v;
}
#endif
