/* API 1.0 geometry-era pixel entry points: forwarding ONLY. The implementation
 * lives behind OpenLogit's private backend, so compatibility cannot bypass or
 * recursively re-enter itself. Kept for source and static-library ABI users. */
#include "openlogit_draw.h"
int gfx_fill(struct gfx_surface *s,const struct gfx_path *p,int r,const struct gfx_paint *a,const struct gfx_rect *c)
{return ol_raster_fill(s,p,r,a,c);}
int gfx_fill_subs(struct gfx_surface *s,const struct gfx_path *p,int r,const struct gfx_paint *a,const struct gfx_rect *c,int n)
{return ol_raster_fill_subs(s,p,r,a,c,n);}
int gfx_fill_clipped(struct gfx_surface *s,const struct gfx_path *p,int r,const struct gfx_paint *a,const struct gfx_rect *c,int n,const struct gfx_clip_mask *m)
{return ol_raster_fill_clipped(s,p,r,a,c,n,m);}
int gfx_fill_with_workspace(void *ws,unsigned long z,struct gfx_surface *s,const struct gfx_path *p,int r,const struct gfx_paint *a,const struct gfx_rect *c,int n)
{return ol_raster_fill_workspace(ws,z,s,p,r,a,c,n);}
int gfx_fill_mask(const struct gfx_path *p,int r,unsigned char *m,int w,int h,int x,int y)
{return ol_raster_mask(p,r,m,w,h,x,y);}
int gfx_fill_mask_subs(const struct gfx_path *p,int r,unsigned char *m,int w,int h,int x,int y,int n)
{return ol_raster_mask_subs(p,r,m,w,h,x,y,n);}
int gfx_fill_mask_clipped(const struct gfx_path *p,int r,unsigned char *m,int w,int h,int x,int y,int n,const struct gfx_clip_mask *c)
{return ol_raster_mask_clipped(p,r,m,w,h,x,y,n,c);}
void gfx_surface_clear(struct gfx_surface *s) {ol_raster_clear(s);}
