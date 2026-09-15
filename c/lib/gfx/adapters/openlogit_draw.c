#include "openlogit_draw.h"
#include "openlogit_sw.h"
static uint64_t calls;
uint64_t ol_raster_calls(void) { return calls; }
int ol_raster_fill_subs(struct gfx_surface *s, const struct gfx_path *p, int rule,
                        const struct gfx_paint *paint, const struct gfx_rect *clip, int samples)
{ calls++; return ol_sw_raster_fill(0,s,p,rule,paint,clip,samples,0); }
int ol_raster_fill(struct gfx_surface *s, const struct gfx_path *p, int rule,
                   const struct gfx_paint *paint, const struct gfx_rect *clip)
{ return ol_raster_fill_subs(s,p,rule,paint,clip,GFX_SUBS); }
int ol_raster_fill_clipped(struct gfx_surface *s, const struct gfx_path *p, int rule,
                           const struct gfx_paint *paint, const struct gfx_rect *clip, int samples,
                           const struct gfx_clip_mask *mask)
{ calls++; return mask && ol_sw_raster_fill(0,s,p,rule,paint,clip,samples,mask); }
int ol_raster_fill_workspace(void *ws, unsigned long bytes, struct gfx_surface *s,
                             const struct gfx_path *p, int rule, const struct gfx_paint *paint,
                             const struct gfx_rect *clip, int samples)
{ if(!ws || ((unsigned long)ws&7) || bytes<gfx_raster_workspace_size())return 0;
  calls++; return ol_sw_raster_fill(ws,s,p,rule,paint,clip,samples,0); }
int ol_raster_mask_subs(const struct gfx_path *p, int rule, unsigned char *cov, int w, int h,
                        int ox, int oy, int samples)
{ calls++; return ol_sw_raster_mask(0,p,rule,cov,w,h,ox,oy,samples,0); }
int ol_raster_mask(const struct gfx_path *p, int rule, unsigned char *cov, int w, int h, int ox, int oy)
{ return ol_raster_mask_subs(p,rule,cov,w,h,ox,oy,GFX_SUBS); }
int ol_raster_mask_clipped(const struct gfx_path *p, int rule, unsigned char *cov, int w, int h,
                           int ox, int oy, int samples, const struct gfx_clip_mask *clip)
{ calls++; return clip && ol_sw_raster_mask(0,p,rule,cov,w,h,ox,oy,samples,clip); }
void ol_raster_clear(struct gfx_surface *s) { calls++; ol_sw_surface_clear(s); }
