#ifndef OPENLOGIT_BITMAP_H
#define OPENLOGIT_BITMAP_H
#include "openlogit.h"
/* Borrowed RGBA8 bitmap operations. Pixel/storage validation remains with
 * the owner; dimensions/stride and region arithmetic are checked here.
 * Copy is replacement (Canvas ImageData), composite is straight-alpha over.
 * Coverage masks have no color and are never interpreted as display pixels. */
int ol_bitmap_copy(struct gfx_surface *dst,int dx,int dy,const struct gfx_surface *src,
                    int sx,int sy,int width,int height);
int ol_bitmap_read(const struct gfx_surface *src,int x,int y,struct gfx_surface *out);
int ol_bitmap_erase(struct gfx_surface *,struct gfx_rect,const struct gfx_clip_mask *);
void ol_mask_multiply(unsigned char *dst,const unsigned char *src,unsigned long pixels,
                       unsigned dst_step,unsigned src_step);
int ol_bitmap_composite(struct gfx_surface *,const unsigned char *rgba,
                         const unsigned char *rgba_mask,int opacity256);
void ol_bitmap_fill32(uint32_t *pixels,unsigned long count,uint32_t color);
void ol_bitmap_fill_padding(uint32_t *,int width,int height,int retained_w,int retained_h,uint32_t color);
/* Source and destination may be identical (in-place stride reshape). */
int ol_bitmap_reshape(uint32_t *dst,int width,int height,const uint32_t *src,
                       int old_width,int old_height,uint32_t background);
void ol_bitmap_tint_row(unsigned char *rgba,const unsigned char *mask,int count,
                         unsigned color,int opacity,int flip);
#endif
