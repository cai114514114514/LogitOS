#include "openlogit_bitmap.h"
#include "openlogit_draw.h"
static int valid(const struct gfx_surface *s)
{return s&&s->px&&s->w>0&&s->h>0&&s->w<=GFX_MAX_W&&s->h<=GFX_MAX_W&&s->stride>=s->w*4;}
static void move_bytes(unsigned char *d,const unsigned char *s,unsigned long n)
{
    if((unsigned long)d>(unsigned long)s && (unsigned long)d-(unsigned long)s<n)
        while(n){n--;d[n]=s[n];}
    else for(unsigned long i=0;i<n;i++)d[i]=s[i];
}
int ol_bitmap_copy(struct gfx_surface *d,int dx,int dy,const struct gfx_surface *s,int sx,int sy,int w,int h)
{
    if(!valid(d)||!valid(s)||w<0||h<0)return OL_ARGUMENT;
    long x0=0,y0=0,x1=w,y1=h;
    if(dx<0)x0=-(long)dx;if(sx<0&&-(long)sx>x0)x0=-(long)sx;
    if(dy<0)y0=-(long)dy;if(sy<0&&-(long)sy>y0)y0=-(long)sy;
    if(x1>(long)d->w-dx)x1=(long)d->w-dx;if(x1>(long)s->w-sx)x1=(long)s->w-sx;
    if(y1>(long)d->h-dy)y1=(long)d->h-dy;if(y1>(long)s->h-sy)y1=(long)s->h-sy;
    if(x1<=x0||y1<=y0)return OL_OK;
    int reverse=d->px==s->px && dy>sy;
    for(long j=0;j<y1-y0;j++) {
        long y=reverse?y1-1-j:y0+j;
        move_bytes(d->px+(dy+y)*d->stride+(dx+x0)*4,
                    s->px+(sy+y)*s->stride+(sx+x0)*4,(x1-x0)*4);
    }
    return OL_OK;
}
int ol_bitmap_read(const struct gfx_surface *s,int x,int y,struct gfx_surface *out)
{
    if(!valid(s)||!valid(out)||out->px==s->px)return OL_ARGUMENT;
    ol_raster_clear(out);
    return ol_bitmap_copy(out,0,0,s,x,y,out->w,out->h);
}
int ol_bitmap_erase(struct gfx_surface *s,struct gfx_rect r,const struct gfx_clip_mask *m)
{
    if(!valid(s)||(m&&(!m->cov||m->w<=0||m->h<=0)))return OL_ARGUMENT;
    r=ol_damage_move(r,(struct gfx_rect){0},0,s->w,s->h);
    for(int y=r.y;y<r.y+r.h;y++)for(int x=r.x;x<r.x+r.w;x++) {
        unsigned char *d=s->px+(long)y*s->stride+x*4;int a=255;
        if(m) {long mx=(long)x-m->ox,my=(long)y-m->oy;
            a=mx>=0&&my>=0&&mx<m->w&&my<m->h?m->cov[my*m->w+mx]:0;}
        d[3]=(d[3]*(255-a)+127)/255;
        if(!d[3])d[0]=d[1]=d[2]=0;
    }
    return OL_OK;
}
void ol_mask_multiply(unsigned char *dst,const unsigned char *src,unsigned long n,unsigned ds,unsigned ss)
{if(!dst||!src||!ds||!ss)return;for(unsigned long i=0;i<n;i++)dst[i*ds]=(dst[i*ds]*src[i*ss]+127)/255;}
int ol_bitmap_composite(struct gfx_surface *dst,const unsigned char *rgba,const unsigned char *mask,int opacity)
{
    if(!valid(dst)||!rgba||opacity<0||opacity>256)return OL_ARGUMENT;
    for(int y=0;y<dst->h;y++)for(int x=0;x<dst->w;x++) {
        long i=(long)y*dst->w+x;const unsigned char *s=rgba+i*4;
        int a=s[3];if(mask)a=(a*mask[i*4+3]+127)/255;a=a*opacity/256;
        gfx_over(dst->px+(long)y*dst->stride+x*4,s[0],s[1],s[2],a,255);
    }
    return OL_OK;
}
void ol_bitmap_fill32(uint32_t *p,unsigned long n,uint32_t color)
{if(p)for(unsigned long i=0;i<n;i++)p[i]=color;}
void ol_bitmap_fill_padding(uint32_t *p,int w,int h,int cw,int ch,uint32_t color)
{
    if(!p||w<0||h<0||cw<0||ch<0||cw>w||ch>h)return;
    for(int y=0;y<h;y++)for(int x=y<ch?cw:0;x<w;x++)p[(long)y*w+x]=color;
}
int ol_bitmap_reshape(uint32_t *dst,int w,int h,const uint32_t *src,int ow,int oh,uint32_t color)
{
    if(!dst||!src||w<1||h<1||ow<1||oh<1||w>GFX_MAX_W||h>GFX_MAX_W||ow>GFX_MAX_W||oh>GFX_MAX_W)return OL_ARGUMENT;
    int cw=w<ow?w:ow,ch=h<oh?h:oh;
    for(int i=0;i<ch;i++) {
        int y=dst==src&&w>ow?ch-i-1:i;
        move_bytes((unsigned char *)(dst+(long)y*w),(const unsigned char *)(src+(long)y*ow),cw*4);
    }
    ol_bitmap_fill_padding(dst,w,h,cw,ch,color);return OL_OK;
}
void ol_bitmap_tint_row(unsigned char *rgba,const unsigned char *mask,int n,unsigned color,int alpha,int flip)
{
    if(!rgba||!mask||alpha<0||alpha>255)return;
    for(int i=0;i<n;i++){rgba[4*i]=GFX_R(color);rgba[4*i+1]=GFX_G(color);rgba[4*i+2]=GFX_B(color);rgba[4*i+3]=mask[flip?n-1-i:i]*alpha/255;}
}
