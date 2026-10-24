/* Textually included by renderer.c after the existing gradient helpers. That
 * keeps every shipping/host painter consumer on the same implementation; a
 * standalone TU would silently disappear from hand-copied source lists.
 *
 * The old rounded path painted one ring using top-edge color and max width,
 * and the gradient suppressed background-color entirely. This module samples
 * real engine coverage, subtracts the inner border shape, and composites the
 * gradient over its own base color BEFORE applying coverage and opacity.
 * The small device tiles bound memory; large radii are rasterized by Open
 * Logit's ellipse path rather than stretched or independently reimplemented. */
#include "box.h"

enum { BOX_TILE=64 };
static unsigned char box_outer[BOX_TILE*BOX_TILE],box_inner[BOX_TILE*BOX_TILE];
static unsigned char box_content[BOX_TILE*BOX_TILE];
static unsigned char box_clip[BOX_TILE*BOX_TILE];
static unsigned char box_corner[BOX_TILE*BOX_TILE],box_pixels[BOX_TILE*BOX_TILE*4];
static int box_points[512*2],box_subpaths[8];
static int box_refused;

void browser_paint_box_stats(int *refused) { if(refused)*refused=box_refused; }

static void box_normalize(struct paint_box *b)
{
    long long num=1,den=1;
    const int pairs[4][2]={{0,1},{1,2},{3,2},{0,3}};
    for(int i=0;i<4;i++){
        long long sum=(i&1)?(long long)b->ry[pairs[i][0]]+b->ry[pairs[i][1]]:
                              (long long)b->rx[pairs[i][0]]+b->rx[pairs[i][1]];
        int length=(i&1)?b->h:b->w;
        if(sum>length && (long long)length*den<num*sum){num=length;den=sum;}
    }
    if(num<den)for(int i=0;i<4;i++){
        b->rx[i]=(int)((long long)b->rx[i]*num/den);
        b->ry[i]=(int)((long long)b->ry[i]*num/den);
    }
}

static void box_shape(struct paint_box *b,int x,int y,int w,int h,const int radius[4],const int pct[4])
{
    b->x=x;b->y=y;b->w=w;b->h=h;
    for(int i=0;i<4;i++){
        /* The old min(width,height) percent interpretation made wide 50%
         * boxes into stadiums. CSS Backgrounds 3 resolves each axis separately. */
        b->rx[i]=pct[i]?(int)((long long)w*pct[i]/100):radius[i];
        b->ry[i]=pct[i]?(int)((long long)h*pct[i]/100):radius[i];
        if(b->rx[i]<0)b->rx[i]=0;if(b->ry[i]<0)b->ry[i]=0;
    }
    box_normalize(b);
}

static void box_inset(struct paint_box *out,const struct paint_box *b,const int widths[4])
{
    *out=*b;out->x+=widths[3];out->y+=widths[0];
    out->w-=widths[3]+widths[1];out->h-=widths[0]+widths[2];
    for(int i=0;i<4;i++){
        int horizontal=(i==0||i==3)?widths[3]:widths[1];
        int vertical=i<2?widths[0]:widths[2];
        out->rx[i]=b->rx[i]>horizontal?b->rx[i]-horizontal:0;
        out->ry[i]=b->ry[i]>vertical?b->ry[i]-vertical:0;
    }
    if(out->w>0&&out->h>0)box_normalize(out);
}

/* Coverage of one device tile, from the authoritative gfx geometry. All four
 * corner predicates are intersected: opposite large corners can overlap their
 * rectangular tiles even though the normalized ellipses do not overlap. */
static void box_mask(const struct paint_box *b,int ox,int oy,int w,int h,unsigned char *mask)
{
    int left=dev(b->x),top=dev(b->y),right=dev(b->x+b->w),bottom=dev(b->y+b->h);
    for(int j=0;j<h;j++)for(int i=0;i<w;i++)
        mask[j*w+i]=(unsigned char)(b->w>0&&b->h>0&&ox+i>=left&&ox+i<right&&oy+j>=top&&oy+j<bottom?255:0);
    if(b->w<=0||b->h<=0)return;
    for(int k=0;k<4;k++){
        if(!b->rx[k]||!b->ry[k])continue;
        int flipx=k==1||k==2,flipy=k>=2;
        int x0=flipx?dev(b->x+b->w-b->rx[k]):left;
        int x1=flipx?right:dev(b->x+b->rx[k]);
        int y0=flipy?dev(b->y+b->h-b->ry[k]):top;
        int y1=flipy?bottom:dev(b->y+b->ry[k]);
        if(ox>=x1||oy>=y1||ox+w<=x0||oy+h<=y0)continue;
        int rx=x1-x0,ry=y1-y0;if(rx<=0||ry<=0)continue;
        const unsigned char *cached=0;
        if(rx<=GFX_MASK_MAX&&ry<=GFX_MASK_MAX)cached=gfx_mask_corner(GFX_MASK_FILL,rx,ry,0);
        if(!cached){
            struct gfx_path p;gfx_path_init(&p,box_points,512,box_subpaths,8);
            int centerx=flipx?x0:x1,centery=flipy?y0:y1;
            /* Local tile coordinates prevent a scrolled document origin from
             * overflowing the engine's signed 24.8 coordinates. */
            long long ax=(long long)(centerx-ox)*GFX_ONE,ay=(long long)(centery-oy)*GFX_ONE;
            if(rx>1000000||ry>1000000||ax>2147483647||ax<(-2147483647-1LL)||ay>2147483647||ay<(-2147483647-1LL)){
                box_refused++;continue;
            }
            gfx_path_ellipse(&p,(int)ax,(int)ay,rx*GFX_ONE,ry*GFX_ONE);
            if(!ol_raster_mask(&p,GFX_NONZERO,box_corner,w,h,0,0)){box_refused++;continue;}
        }
        for(int j=0;j<h;j++)for(int i=0;i<w;i++){
            int x=ox+i,y=oy+j;if(x<x0||x>=x1||y<y0||y>=y1)continue;
            int a;
            if(cached){int sx=flipx?x1-1-x:x-x0,sy=flipy?y1-1-y:y-y0;a=cached[sy*rx+sx];}
            else a=box_corner[j*w+i];
            if(a<mask[j*w+i])mask[j*w+i]=(unsigned char)a;
        }
    }
}

static int box_has_rounded_clip(const struct item *e,int image)
{
    struct node *boundary=LOGIT_HAVE(top_layer_owner)?top_layer_owner(e->node):0;
    for(struct node *p=e->node&&e->node!=boundary?e->node->parent:0;p;p=p==boundary?0:p->parent){
        if(p->type!=N_ELEM||!p->style)continue;
        const struct cstyle *s=p->style;
        if(s->overflow_x==OVF_VISIBLE||s->overflow_y==OVF_VISIBLE)continue;
        /* Keep the already-correct uniform solid-color clip on its cached
         * band path. The new path owns gradients and asymmetric/percent
         * corners, which the old single-radius clip cannot represent. */
        for(int k=0;k<4;k++){
            if(s->radius_pct[k] || s->radius[k]!=s->radius[0])return 1;
            if(image && s->radius[k])return 1;
        }
    }
    return 0;
}

static void box_clip_mask(const struct item *e,int vx,int vy,int scroll,int ox,int oy,int w,int h,unsigned char *mask)
{
#ifdef PAINT_NEGCTL_NO_RCLIP
    /* Same watched fault as the legacy consumer: retaining rounded coverage
     * here would let the new path silently bypass that negative control. The
     * ordinary rectangular overflow clip still comes from the outer loop. */
    (void)e;(void)vx;(void)vy;(void)scroll;(void)ox;(void)oy;(void)w;(void)h;(void)mask;return;
#else
    struct node *boundary=LOGIT_HAVE(top_layer_owner)?top_layer_owner(e->node):0;
    for(struct node *p=e->node&&e->node!=boundary?e->node->parent:0;p;p=p==boundary?0:p->parent){
        if(p->type!=N_ELEM||!p->style)continue;
        const struct cstyle *s=p->style;
        if(s->overflow_x==OVF_VISIBLE||s->overflow_y==OVF_VISIBLE)continue;
        int x,y,bw,bh;if(!layout_node_box(p,&x,&y,&bw,&bh))continue;
        scroll_box_origin(p,&x,&y);
        struct paint_box outer,inner;box_shape(&outer,vx+x,vy+y-scroll,bw,bh,s->radius,s->radius_pct);
        box_inset(&inner,&outer,s->border_w);box_mask(&inner,ox,oy,w,h,box_inner);
        for(int i=0;i<w*h;i++)mask[i]=(unsigned char)((unsigned)mask[i]*box_inner[i]/255);
    }
#endif
}

/* Select the edge using its inward distance divided by its own thickness.
 * CSS leaves the exact color join curve to the UA; this monotonic ratio keeps
 * a zero-width side from borrowing another side's ink and never paints an
 * entire ring with the top edge's color. */
static int box_edge(const struct paint_box *b,const struct item *e,int px,int py)
{
    long long distance[4]={py-dev(b->y),dev(b->x+b->w)-1-px,dev(b->y+b->h)-1-py,px-dev(b->x)};
    int chosen=-1,width=1;
    for(int k=0;k<4;k++){
        int n=e->border_w[k];if(n<=0)continue;
        if(chosen<0||distance[k]*width<distance[chosen]*n){chosen=k;width=n;}
    }
    /* Transparent colors use BS_HIDDEN in the existing style bridge, but the
     * width still reserves geometry. Omitting that side from ownership lets
     * a visible adjacent edge color flood the transparent side's whole band. */
    return chosen>=0 && e->border_style[chosen]==BS_SOLID ? chosen : -1;
}

static int box_supported_edges(const struct item *e)
{
    for(int k=0;k<4;k++)if(e->border_w[k]>0&&e->border_style[k]!=BS_SOLID&&e->border_style[k]!=BS_NONE&&e->border_style[k]!=BS_HIDDEN)return 0;
    return 1;
}

static int box_repair_needed(const struct item *e,int image)
{
    int rounded=0;
    for(int k=0;k<4;k++){
        if(e->radius_pct[k])return 1;
        if(e->radius[k])rounded=1;
        if(k&&e->radius[k]!=e->radius[0])return 1;
    }
    if(rounded)for(int k=1;k<4;k++)
        if(e->border_w[k]!=e->border_w[0]||e->border_color[k]!=e->border_color[0]||e->border_style[k]!=e->border_style[0])return 1;
    return box_has_rounded_clip(e,image);
}

/* Compose straight RGBA layers while retaining a transparent result. Applying
 * element opacity to each layer separately would expose the lower layer twice
 * at an overlap; the caller applies it once to this completed background. */
static unsigned box_source_over(unsigned top,int top_alpha,unsigned base,int base_alpha,int *alpha)
{
    int remaining = base_alpha * (255 - top_alpha) / 255;
    int total = top_alpha + remaining;
    unsigned result = 0;
    if (total) {
        for (int shift = 0; shift <= 16; shift += 8) {
            unsigned channel = (((top >> shift) & 255) * top_alpha +
                                ((base >> shift) & 255) * remaining) / total;
            result |= channel << shift;
        }
    }
    *alpha = total;
    return result;
}

static int paint_box_repaired(const struct item *e,const struct item *source,int x,int y,int op,int vx,int vy,int scroll)
{
    struct background_paint background;
    int layers=background_prepare(e,source,x,y,&background);
    /* A raw span may be `none` or a color shorthand reset. It is not evidence
     * of an image and must not bypass the cached uniform-color clip path. */
    int image=0;for(int k=0;k<background.count;k++)image|=background.image[k].kind;
#ifdef PAINT_NEGCTL_ROUND_HASBG
    if(!e->has_bg&&!image&&!box_has_rounded_clip(e,image))return 0;
#endif
    if(!layers&&!(background.has_gradient&&e->has_bg)&&!box_repair_needed(e,image))return 0;
    int solid_edges=box_supported_edges(e);
    struct paint_box outer,inner,content;box_shape(&outer,x,y,e->w,e->h,e->radius,e->radius_pct);
    box_inset(&inner,&outer,e->border_w);
    const struct cstyle *s=sty(e);
    const struct cstyle *original=sty(source);
    int content_inset[4];for(int k=0;k<4;k++)content_inset[k]=e->border_w[k];
    if(original&&source->w>0&&source->h>0){
        double sx=(double)e->w/source->w,sy=(double)e->h/source->h;
        content_inset[0]+=(int)(original->pt*sy);content_inset[1]+=(int)(original->pr*sx);
        content_inset[2]+=(int)(original->pb*sy);content_inset[3]+=(int)(original->pl*sx);
    }
    box_inset(&content,&outer,content_inset);
    /* prepare also supplies initial colors for repaired plain rounded boxes.
     * A refused mask must keep all decoration absent, not revive its color. */
    if(!background.count){background.count=1;background.color=e->bg;background.alpha=e->has_bg?e->bg_alpha:0;}
    if(s&&s->mask_refused)background.alpha=0;
    int step=(BOX_TILE-1)*100/(scale_pct?scale_pct:100);if(step<1)step=1;
    int x0=x>cl_x0?x:cl_x0,y0=y>cl_y0?y:cl_y0;
    int x1=x+e->w<cl_x1?x+e->w:cl_x1,y1=y+e->h<cl_y1?y+e->h:cl_y1;
    for(int ty=y0;ty<y1;ty+=step)for(int tx=x0;tx<x1;tx+=step){
        int tw=x1-tx<step?x1-tx:step,th=y1-ty<step?y1-ty:step;
        int dw=devlen(tx,tw),dh=devlen(ty,th),ox=dev(tx),oy=dev(ty);
        if(dw<=0||dh<=0||dw>BOX_TILE||dh>BOX_TILE)continue;
        box_mask(&outer,ox,oy,dw,dh,box_outer);
        memset(box_clip,255,(size_t)dw*dh);
        box_clip_mask(e,vx,vy,scroll,ox,oy,dw,dh,box_clip);
        box_mask(&inner,ox,oy,dw,dh,box_inner);
        box_mask(&content,ox,oy,dw,dh,box_content);
        for(int j=0;j<dh;j++)for(int i=0;i<dw;i++){
            int index=j*dw+i;
            int outside=box_outer[index],padding=box_inner[index],inside=box_content[index];
            if(padding>outside)padding=outside;if(inside>padding)inside=padding;
            int coverage[3]={outside-padding,padding-inside,inside};
            unsigned image_color[BG_MAX_LAYERS];int image_alpha[BG_MAX_LAYERS];
            for(int k=0;k<background.count;k++)
                background_sample(&background.image[k],ox+i,oy+j,&image_color[k],&image_alpha[k]);
            long long mass=0,channel[3]={0};
            int edge=solid_edges?box_edge(&outer,e,ox+i,oy+j):-1;
            for(int region=0;region<3;region++){
                if(!coverage[region])continue;
                int a=background.color_clip<=region?background.alpha:0;unsigned color=background.color;
#ifdef CSS_BACKGROUND_NO_BASE
                if(background.has_gradient)a=0;
#endif
                for(int k=background.count-1;k>=0;k--)if(background.image[k].clip<=region)
                    color=box_source_over(image_color[k],image_alpha[k],color,a,&a);
                if(region==0&&edge>=0){color=e->border_color[edge];a=255;}
                long long weight=(long long)a*coverage[region];mass+=weight;
                for(int k=0;k<3;k++)channel[k]+=((color>>(16-8*k))&255)*weight;
            }
            /* Border/padding/content are nested shapes. Compose layers within
             * each disjoint region, then weight by its coverage. Multiplying
             * every layer by the same antialias mask independently exposes a
             * lower image at overlaps and double-fades the shared inner edge. */
            unsigned color=0;int a=(int)(mass/255);
            if(mass)for(int k=0;k<3;k++)color|=(unsigned)(channel[k]/mass)<<(16-8*k);
            unsigned char *d=box_pixels+index*4;d[0]=(unsigned char)(color>>16);d[1]=(unsigned char)(color>>8);d[2]=(unsigned char)color;
            d[3]=(unsigned char)(a*box_clip[index]/255*op/255);
        }
        gui_blit(tx,ty,tw,th,box_pixels,dw,dh);
    }
    /* Background semantics do not depend on border-style. The old edge
     * painter retains dotted/dashed/double/bevel patterns; it must not erase
     * the image by repainting the color under those borders. */
    return solid_edges?1:2;
}
