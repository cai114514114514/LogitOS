#ifndef OPENLOGIT_DISPLAY_SHADOW_H
#define OPENLOGIT_DISPLAY_SHADOW_H
#include "openlogit_display.h"

/* Analytic gaussian shadow of a rounded rectangle.
 *
 *   x,y,w,h      caster box in whole display pixels
 *   radius[4]    corner radii in pixels, order TL, TR, BR, BL (NULL = square)
 *   dx24,dy24    shadow offset, 24.8 pixels
 *   sigma24      gaussian standard deviation, 24.8 pixels (fractional is
 *                fine; there is no upper clamp, unlike ol_display_shadow)
 *   spread24     24.8 pixels the shadow shape grows by (negative shrinks);
 *                a non-zero radius grows with it, never below 0
 *   rgb          0xRRGGBB logical colour, alpha 0..255 peak opacity
 *   inset        0: shadow outside the caster; 1: shadow inside the caster
 *                (the blurred complement of the offset, shrunk hole, clipped
 *                to the caster, so inset(s) + outer(-s) == full coverage)
 *
 * Honours the display clip. Returns 0, or a negative OL_* code when the
 * scratch slot (OL_DS_SHADOW) cannot be grown; nothing is drawn then.
 *
 * Like the legacy ol_display_shadow, the outer shadow is NOT painted under the
 * caster's inscribed cross (it would be hidden by the opaque caster), which is
 * what keeps the cost proportional to the perimeter band. Pass
 * OL_SHADOW_EX_BODY to paint it everywhere.
 */
#define OL_SHADOW_EX_BODY 1u

int ol_display_shadow_ex_opt(struct ol_display *ctx, int x, int y, int w, int h,
                             const int radius[4], int dx24, int dy24, int sigma24,
                             int spread24, uint32_t rgb, int alpha, int inset,
                             unsigned flags);
int ol_display_shadow_ex(struct ol_display *ctx, int x, int y, int w, int h,
                         const int radius[4], int dx24, int dy24, int sigma24,
                         int spread24, uint32_t rgb, int alpha, int inset);
#endif
