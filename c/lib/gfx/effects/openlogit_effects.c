#include "openlogit.h"
#include "openlogit_sw.h"

/* Native RGBA effects, independent of the opaque display backend. A sliding
 * vertical sum followed by a horizontal sum needs only 16*(width+2r) scratch
 * bytes. Keeping color*alpha sums until the final pixel avoids dark fringes
 * around transparent saturated images and rounding at each blur pass.
 * Radius 64 bounds a color sum to 65025*129*129, below UINT32_MAX. */
struct accum { unsigned v[4]; };
static int min(int a,int b){return a<b?a:b;}
static int max(int a,int b){return a>b?a:b;}
struct gfx_rect ol_sw_effect_bounds(struct gfx_rect r,int radius,int w,int h)
{
    int x=max(0,r.x-radius),y=max(0,r.y-radius);
    int right=min(w,r.x+r.w+radius),bottom=min(h,r.y+r.h+radius);
    if(right<=x||bottom<=y)return (struct gfx_rect){0,0,0,0};
    return (struct gfx_rect){x,y,right-x,bottom-y};
}
static struct gfx_rect intersect(struct gfx_rect a,struct gfx_rect b)
{
    int x=max(a.x,b.x),y=max(a.y,b.y),r=min(a.x+a.w,b.x+b.w),d=min(a.y+a.h,b.y+b.h);
    return (struct gfx_rect){x,y,max(0,r-x),max(0,d-y)};
}
static void sample(const struct gfx_surface *s,struct gfx_rect dst,int x,int y,int bilinear,unsigned v[4])
{
    for(int k=0;k<4;k++)v[k]=0;
    if(x<dst.x||y<dst.y||x>=dst.x+dst.w||y>=dst.y+dst.h)return;
    /* Offscreen groups and backdrop snapshots normally map one-to-one.
     * Their pixel centers are exact for both filters: avoid two 64-bit
     * divisions plus four-tap interpolation for every blur sample. */
    if(dst.w==s->w&&dst.h==s->h) {
        const unsigned char *p=s->px+(unsigned long)(y-dst.y)*s->stride+(x-dst.x)*4;
        v[3]=p[3];for(int k=0;k<3;k++)v[k]=p[k]*p[3];return;
    }
    /* Pixel centers, with clamped image edges inside the destination. The
     * surrounding layer is transparent; edge replication does not leak past
     * its geometry. Interpolate premultiplied channels, never hidden RGB. */
    long long fx=((2ll*(x-dst.x)+1)*s->w*65536)/(2*dst.w)-32768;
    long long fy=((2ll*(y-dst.y)+1)*s->h*65536)/(2*dst.h)-32768;
    if(!bilinear) {
        int sx=min(s->w-1,(int)((fx+32768)/65536));
        int sy=min(s->h-1,(int)((fy+32768)/65536));
        const unsigned char *p=s->px+(unsigned long)sy*s->stride+sx*4;
        v[3]=p[3];for(int k=0;k<3;k++)v[k]=p[k]*p[3];return;
    }
    fx=max(0,(int)fx);fy=max(0,(int)fy);
    int ix=min(s->w-1,(int)(fx/65536)),iy=min(s->h-1,(int)(fy/65536));
    unsigned tx=(unsigned)fx&65535,ty=(unsigned)fy&65535;
    unsigned long long sums[4]={0,0,0,0};
    for(int yy=0;yy<2;yy++)for(int xx=0;xx<2;xx++) {
        const unsigned char *p=s->px+(unsigned long)min(iy+yy,s->h-1)*s->stride+min(ix+xx,s->w-1)*4;
        unsigned long long weight=(unsigned long long)(xx?tx:65536-tx)*(yy?ty:65536-ty);
        for(int k=0;k<3;k++)sums[k]+=weight*p[k]*p[3];sums[3]+=weight*p[3];
    }
    for(int k=0;k<4;k++)v[k]=(unsigned)((sums[k]+(1ull<<31))>>32);
}
static void output(struct gfx_surface *t,int x,int y,const unsigned sums[4],unsigned area,
                    int opacity,int shadow,unsigned rgb,int replace)
{
    unsigned a=(sums[3]+area/2)/area;
    unsigned r=0,g=0,b=0;
    if(!shadow&&sums[3]){r=(sums[0]+sums[3]/2)/sums[3];g=(sums[1]+sums[3]/2)/sums[3];b=(sums[2]+sums[3]/2)/sums[3];}
    if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
    unsigned char *p=t->px+(unsigned long)y*t->stride+x*4;
    if(replace){p[0]=r;p[1]=g;p[2]=b;p[3]=a;}
    else gfx_over(p,shadow?GFX_R(rgb):r,shadow?GFX_G(rgb):g,shadow?GFX_B(rgb):b,
                  (a*opacity+127)/255,255);
}
static void filtered(struct gfx_surface *t,const struct gfx_surface *s,struct gfx_rect dst,
                      struct gfx_rect clip,int radius,int bilinear,int opacity,
                      int shadow,unsigned rgb,int replace,void *scratch)
{
    struct gfx_rect out=intersect(ol_sw_effect_bounds(dst,radius,t->w,t->h),clip);
    if(!out.w||!out.h)return;
#ifdef OPENLOGIT_EFFECT_BLUR_DISABLED
    radius=0; /* Keep expanded coverage; the independent convolution oracle fails. */
#endif
    if(!radius) {
        if(dst.w==s->w&&dst.h==s->h&&out.x>=dst.x&&out.y>=dst.y&&
           out.x+out.w<=dst.x+dst.w&&out.y+out.h<=dst.y+dst.h) {
            for(int y=out.y;y<out.y+out.h;y++)for(int x=out.x;x<out.x+out.w;x++) {
                const unsigned char *p=s->px+(unsigned long)(y-dst.y)*s->stride+(x-dst.x)*4;
                unsigned char *q=t->px+(unsigned long)y*t->stride+x*4;
                /* Opaque cached layers need no source-over arithmetic or
                 * per-pixel function call. Partial alpha keeps the exact oracle. */
                if(replace || (!shadow && opacity == 255 && p[3] == 255)) {
                    for(int k=0;k<4;k++)q[k]=p[k];
                }
                else gfx_over(q,shadow?GFX_R(rgb):p[0],shadow?GFX_G(rgb):p[1],shadow?GFX_B(rgb):p[2],
                              (p[3]*opacity+127)/255,255);
            }
            return;
        }
        for(int y=out.y;y<out.y+out.h;y++)for(int x=out.x;x<out.x+out.w;x++) {
            unsigned v[4];sample(s,dst,x,y,bilinear,v);output(t,x,y,v,1,opacity,shadow,rgb,replace);
        }
        return;
    }
    struct accum *col=scratch;
    int width=out.w+2*radius,left=out.x-radius,side=2*radius+1;
    for(int i=0;i<width;i++) {
        for(int k=0;k<4;k++)col[i].v[k]=0;
        for(int y=out.y-radius;y<=out.y+radius;y++) {
            unsigned v[4];sample(s,dst,left+i,y,bilinear,v);
            for(int k=0;k<4;k++)col[i].v[k]+=v[k];
        }
    }
    for(int y=out.y;y<out.y+out.h;y++) {
        unsigned sum[4]={0,0,0,0};
        for(int i=0;i<side;i++)for(int k=0;k<4;k++)sum[k]+=col[i].v[k];
        for(int x=0;x<out.w;x++) {
            output(t,out.x+x,y,sum,(unsigned)(side*side),opacity,shadow,rgb,replace);
            if(x+1<out.w)for(int k=0;k<4;k++)sum[k]=sum[k]-col[x].v[k]+col[x+side].v[k];
        }
        if(y+1<out.y+out.h)for(int i=0;i<width;i++) {
            unsigned add[4],sub[4];sample(s,dst,left+i,y+radius+1,bilinear,add);
            sample(s,dst,left+i,y-radius,bilinear,sub);
            for(int k=0;k<4;k++)col[i].v[k]=col[i].v[k]-sub[k]+add[k];
        }
    }
}
void ol_sw_layer(struct gfx_surface *t,const struct gfx_surface *s,struct gfx_rect dst,
                  const struct ol_layer_options *o,void *scratch)
{
    struct gfx_rect clip=o->clip?*o->clip:(struct gfx_rect){0,0,t->w,t->h};
    if(o->shadow_opacity) {
        struct gfx_rect shadow=dst;shadow.x+=o->shadow_dx;shadow.y+=o->shadow_dy;
        filtered(t,s,shadow,clip,o->shadow_radius,o->bilinear,
                 (o->shadow_opacity*o->opacity+127)/255,1,o->shadow_rgb,0,scratch);
    }
    filtered(t,s,dst,clip,o->blur_radius,o->bilinear,o->opacity,0,0,0,scratch);
}
unsigned long ol_sw_backdrop_size(struct gfx_rect r,int radius,int w,int h)
{
    struct gfx_rect read=ol_sw_effect_bounds(r,radius,w,h);
    if(!read.w||!read.h)return 0;
    return (((unsigned long)read.w*read.h*4+7)&~7ul)+(radius?(unsigned long)(w+2*radius)*sizeof(struct accum):0);
}
void ol_sw_backdrop(struct gfx_surface *t,struct gfx_rect r,int radius,unsigned rgb,int opacity,void *scratch)
{
    struct gfx_rect read=ol_sw_effect_bounds(r,radius,t->w,t->h);
    if(!read.w||!read.h)return;
    struct gfx_surface snap={scratch,read.w,read.h,read.w*4};
    for(int y=0;y<read.h;y++)for(int x=0;x<read.w*4;x++)
        snap.px[(unsigned long)y*snap.stride+x]=t->px[(unsigned long)(read.y+y)*t->stride+read.x*4+x];
    void *cols=(unsigned char *)scratch+(((unsigned long)read.w*read.h*4+7)&~7ul);
    filtered(t,&snap,read,r,radius,0,255,0,0,1,cols);
    r=ol_sw_effect_bounds(r,0,t->w,t->h);
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++)
        gfx_over(t->px+(unsigned long)y*t->stride+x*4,GFX_R(rgb),GFX_G(rgb),GFX_B(rgb),opacity,255);
}

/* ---- rounded / multi-pass backdrop (ol_cmd_backdrop_rrect) ----------------
 * passes==3 runs three box blurs of radius ceil(r/3); each pass reads the
 * previous one's replace-output, so the snapshot ping-pongs between two
 * buffers. Transparent beyond the read rect: a pass corrupts at most one
 * radius of band per pass, and the read rect is the full reach, so the output
 * rect itself is never contaminated. */
static int pass_radius(int radius,int passes){return passes>=3?(radius+2)/3:radius;}
int ol_sw_backdrop_reach(int radius,int passes)
{
    return passes>=3?3*pass_radius(radius,3):radius;
}
static int all_square(const int corner[4])
{
#ifdef OL_BACKDROP_IGNORE_RADIUS
    (void)corner;return 1;
#else
    return !(corner[0]|corner[1]|corner[2]|corner[3]);
#endif
}
unsigned long ol_sw_backdrop_rrect_size(struct gfx_rect r,const int corner[4],int radius,int passes,int w,int h)
{
    int reach=ol_sw_backdrop_reach(radius,passes);
    if(passes<=1&&all_square(corner))return ol_sw_backdrop_size(r,radius,w,h);
    struct gfx_rect read=ol_sw_effect_bounds(r,reach,w,h);
    if(!read.w||!read.h)return 0;
    int p=pass_radius(radius,passes);
    unsigned long snap=((unsigned long)read.w*read.h*4+7)&~7ul;
    return 2*snap+(p?(unsigned long)(read.w+2*p)*sizeof(struct accum):0);
}
/* Coverage 0..255 of pixel (tx,ty) of the top-left corner tile of radius r (arc
 * centred at (r,r)). gfx_mask_corner's rasteriser is too coarse here (up to
 * 18/255 off at r=16), so this is analytic: pixels wholly inside or outside the
 * circle are decided in integers; the thin band on the arc integrates the exact
 * covered height over 32 sub-columns in 1/256 px units (height error < 1/256). */
static int corner_cov(int r,int tx,int ty)
{
    long ix=r-tx,iy=r-ty,ox=r-tx-1,oy=r-ty-1;
    if(ix*ix+iy*iy<=(long)r*r)return 255;
    if(ox*ox+oy*oy>=(long)r*r)return 0;
    unsigned long long R=(unsigned long long)r*256,c=R;
    unsigned long long bottom=(unsigned long long)(ty+1)*256,top=(unsigned long long)ty*256,sum=0;
    for(int i=0;i<32;i++) {
        long long dx=(long long)c-((long long)tx*256+8*i+4);
        unsigned long long h=dx>=(long long)R?0:gfx_isqrt(R*R-(unsigned long long)(dx*dx));
        long long edge=(long long)c-(long long)h;        /* covered from here down */
        long long lo=edge<(long long)top?(long long)top:edge;
        if((long long)bottom>lo)sum+=bottom-lo;
    }
    return (int)((sum*255+16ull*256)/(32ull*256));
}
static int rr_cov(const int cr[4],int w,int h,int x,int y)
{
    int c=-1,tx=x,ty=y;
    if(x<cr[0]&&y<cr[0]){c=0;}
    else if(x>=w-cr[1]&&y<cr[1]){c=1;tx=w-1-x;}
    else if(x>=w-cr[2]&&y>=h-cr[2]){c=2;tx=w-1-x;ty=h-1-y;}
    else if(x<cr[3]&&y>=h-cr[3]){c=3;ty=h-1-y;}
    if(c<0)return 255;
    return corner_cov(cr[c],tx,ty);
}
void ol_sw_backdrop_rrect(struct gfx_surface *t,struct gfx_rect r,const int corner[4],int radius,
                           int passes,unsigned rgb,int opacity,void *scratch)
{
    if(passes<=1&&all_square(corner)){ol_sw_backdrop(t,r,radius,rgb,opacity,scratch);return;}
    int reach=ol_sw_backdrop_reach(radius,passes),p=pass_radius(radius,passes);
    struct gfx_rect read=ol_sw_effect_bounds(r,reach,t->w,t->h);
    if(!read.w||!read.h)return;
    unsigned long snap=((unsigned long)read.w*read.h*4+7)&~7ul;
    struct gfx_surface a={scratch,read.w,read.h,read.w*4};
    struct gfx_surface b={(unsigned char *)scratch+snap,read.w,read.h,read.w*4};
    void *cols=(unsigned char *)scratch+2*snap;
    for(int y=0;y<read.h;y++)for(int x=0;x<read.w*4;x++)
        a.px[(unsigned long)y*a.stride+x]=t->px[(unsigned long)(read.y+y)*t->stride+read.x*4+x];
    struct gfx_surface *src=&a,*dst=&b;
    if(p)for(int i=0;i<(passes>=3?3:1);i++) {
        struct gfx_rect all={0,0,read.w,read.h};
        filtered(dst,src,all,all,p,0,255,0,0,1,cols);
        struct gfx_surface *tmp=src;src=dst;dst=tmp;
    }
    int cr[4],ignore=all_square(corner);
    for(int i=0;i<4;i++)cr[i]=ignore?0:corner[i];
    struct gfx_rect o=ol_sw_effect_bounds(r,0,t->w,t->h);
    for(int y=o.y;y<o.y+o.h;y++)for(int x=o.x;x<o.x+o.w;x++) {
        int cov=rr_cov(cr,r.w,r.h,x-r.x,y-r.y);
        if(!cov)continue;
        unsigned char n[4],*d=t->px+(unsigned long)y*t->stride+x*4;
        const unsigned char *s=src->px+(unsigned long)(y-read.y)*src->stride+(x-read.x)*4;
        for(int k=0;k<4;k++)n[k]=s[k];
        gfx_over(n,GFX_R(rgb),GFX_G(rgb),GFX_B(rgb),opacity,255);
        if(cov==255){for(int k=0;k<4;k++)d[k]=n[k];}
        else for(int k=0;k<4;k++)d[k]=(unsigned char)((d[k]*(255-cov)+n[k]*cov+127)/255);
    }
}
