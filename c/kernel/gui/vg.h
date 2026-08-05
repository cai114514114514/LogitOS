#ifndef LOGIT_VG_H
#define LOGIT_VG_H
#include <stdint.h>

/* Tiny from-scratch vector path rasterizer (integer/fixed-point; kernel is
 * -mno-sse). Reuses the AA coverage engine in raster.c (4x scanline, nonzero
 * winding). A path is a list of commands in a UNIT coordinate box (e.g. 0..100);
 * vg_render_path scales it to a px_size x px_size coverage bitmap (0..255 alpha)
 * that fb_blit_glyph can paint in any color -- the basis for vector icons. */

enum { VG_MOVE, VG_LINE, VG_QUAD, VG_CUBIC, VG_CLOSE };

/* op + up to 3 control/end points (icon-space units). MOVE/LINE use pt0;
 * QUAD uses pt0=ctrl,pt1=end; CUBIC uses pt0,pt1=ctrls,pt2=end; CLOSE uses none. */
struct vg_cmd { uint8_t op; short x[3], y[3]; };

/* Rasterize cmds (coords in [0,unit]) into a px_size x px_size coverage bitmap.
 * Returns 0 ok (fills wout,hout = px_size), -1 if too big for covcap/MAXW. */
int vg_render_path(const struct vg_cmd *cmds, int ncmd, int unit, int px_size,
                   uint8_t *cov, int covcap, int *wout, int *hout);

#endif /* LOGIT_VG_H */
