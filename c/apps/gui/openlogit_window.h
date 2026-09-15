#ifndef OPENLOGIT_WINDOW_H
#define OPENLOGIT_WINDOW_H
/* LogitOS window transport. Compositing writes the current window's backing
 * surface; presenting schedules that window for the compositor. Keep these
 * operations distinct so a toolkit can combine text/widgets and OpenLogit
 * surfaces into ONE frame. Neither operation claims a GPU/vblank fence.
 * The software backend's RGBA8 straight-alpha format is checked explicitly. */
#include "openlogit.h"
#include "logit.h"
static inline int ol_window_key_down(int key)
{ return _sys(SYS_GUI_WIN_STATE,WINS_KEY_HELD,key,0)>0; }
static inline int ol_window_visible(void)
{ return _sys(SYS_GUI_WIN_STATE,WINS_MINIMIZED,0,0)==0; }
static inline int ol_window_composite(const struct ol_surface *s, int x, int y, int w, int h)
{
    struct ol_surface_view v;
    int r = ol_surface_view(s, &v);
    if (r)
        return r;
    if (w <= 0 || h <= 0)
        return OL_ARGUMENT;
    if (v.format != OL_FORMAT_RGBA8_STRAIGHT || v.stride != v.width * 4)
        return OL_UNSUPPORTED;
    struct logit_blit b = {x, y, w, h, v.pixels, (int)v.width, (int)v.height};
    return (long)_sys(SYS_GUI_BLIT, (long)&b, 0, 0) < 0 ? OL_BACKEND_FAILED : OL_OK;
}
static inline int ol_window_present(void)
{
    return (long)_sys(SYS_GUI_FLUSH, 0, 0, 0) < 0 ? OL_BACKEND_FAILED : OL_OK;
}
/* The existing blit ABI has no source stride. Pack only the damaged rectangle
 * into caller scratch before transport; passing an offset into the full image
 * would treat padded rows as packed and silently shear every row after one. */
static inline int ol_window_composite_region(const struct ol_surface *surface,
                                             struct gfx_rect region, void *scratch,
                                             unsigned long bytes)
{
    struct ol_surface_view view;
    if (ol_surface_view(surface,&view) != OL_OK || view.format != OL_FORMAT_RGBA8_STRAIGHT ||
        region.x < 0 || region.y < 0 || region.w < 0 || region.h < 0 ||
        (unsigned)region.w > view.width || (unsigned)region.h > view.height ||
        (unsigned)region.x > view.width-region.w || (unsigned)region.y > view.height-region.h)
        return OL_ARGUMENT;
    if (!region.w || !region.h) return OL_OK;
    unsigned long row_bytes = (unsigned long)region.w*4;
    if (!scratch || bytes < row_bytes*region.h) return OL_LIMIT;
    volatile unsigned char *destination = scratch;
    for (int y=0; y<region.h; y++) {
        const unsigned char *source = view.pixels + (unsigned long)(region.y+y)*view.stride+region.x*4;
        for (unsigned long x=0; x<row_bytes; x++) destination[(unsigned long)y*row_bytes+x]=source[x];
    }
    struct logit_blit blit = {region.x,region.y,region.w,region.h,scratch,region.w,region.h};
    return (long)_sys(SYS_GUI_BLIT,(long)&blit,0,0)<0 ? OL_BACKEND_FAILED : OL_OK;
}
static inline int ol_window_present_region(struct gfx_rect region)
{
    if (region.w < 0 || region.h < 0 || region.x < 0 || region.y < 0 ||
        region.x > 32767 || region.y > 32767 || region.w > 32767 || region.h > 32767)
        return OL_ARGUMENT;
    /* The syscall's zero rectangle requests FULL damage; SDK empty is a no-op. */
    if (!region.w || !region.h) return OL_OK;
    return (long)_sys(SYS_GUI_FLUSH_RECT,((long)region.x<<16)|region.y,
                       ((long)region.w<<16)|region.h,0)<0 ? OL_BACKEND_FAILED : OL_OK;
}
static inline void ol_window_clip_region(struct gfx_rect region)
{
    _sys(SYS_GUI_CLIP,((long)region.x<<16)|region.y,((long)region.w<<16)|region.h,0);
}
static inline void ol_window_reset_clip(void)
{
    _sys(SYS_GUI_CLIP,0,0,0);
}
#endif
