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
#endif
