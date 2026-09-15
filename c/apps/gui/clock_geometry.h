#ifndef CLOCK_GEOMETRY_H
#define CLOCK_GEOMETRY_H
#include "gfx.h"
/* At the Clock's 384-device-pixel ceiling the largest double-circle outline
 * has 514 points (scan of radii 23..192, first reached at 116). The old 512
 * buffer lost the rim at 150% scale and after resize. 1024 adds only 4 KiB;
 * the shared geometry/capacity check below prevents a future silent repeat. */
#ifndef CLOCK_PATH_CAP
#define CLOCK_PATH_CAP 1024
#endif
static inline void clock_face_path(struct gfx_path *p, int cx, int cy, int radius, int rim)
{
    gfx_path_circle(p, GFX_PX(cx), GFX_PX(cy), GFX_PX(radius - 1));
    if (rim)
        gfx_path_circle(p, GFX_PX(cx), GFX_PX(cy), GFX_PX(radius - 2));
}
#endif
