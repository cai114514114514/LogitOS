/* Included before box.c. This module prepares every background image in CSS
 * coordinates; box.c owns rounded coverage, layer composition and opacity.
 * No IO, decoder or allocation lives in the painter. Cached pixels are borrowed
 * only for this synchronous call. There is one interpretation of every layer,
 * rather than a bitmap path followed by an unrelated gradient pass. */
#include "css/background.h"
#include "paint/gradient.h"
#include <math.h>
extern int css_background_layers(const struct cstyle *,struct bg_layers *) LOGIT_WEAK;
extern int css_background_color(const struct cstyle *,unsigned *,int *) LOGIT_WEAK;
extern const struct image *layout_img_get(const char *) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_background_layers);
LOGIT_WEAK_STUB(css_background_color);
LOGIT_WEAK_STUB(layout_img_get);

struct background_image {
    const struct image *image;
    struct paint_gradient gradient;
    int kind,clip,repeat[2];
    double x,y,w,h,scale_x,scale_y,period[2];
};
struct background_paint {
    int count,color_clip,has_gradient,needed;
    unsigned color;
    int alpha;
    struct background_image image[BG_MAX_LAYERS];
};
static void background_area(const struct item *source,int box,double *x,double *y,double *w,double *h)
{
    double inset[4]={0};
    if(box!=BG_BORDER_BOX)for(int i=0;i<4;i++)inset[i]=source->border_w[i];
    if(box==BG_CONTENT_BOX){
        const struct cstyle *s=sty(source);
        if(s){inset[0]+=s->pt;inset[1]+=s->pr;inset[2]+=s->pb;inset[3]+=s->pl;}
    }
    *x=inset[3];*y=inset[0];*w=source->w-inset[3]-inset[1];*h=source->h-inset[0]-inset[2];
}
static int background_image_prepare(const struct bg_layer *layer,const struct cstyle *style,
                                    const struct item *source,const struct item *painted,
                                    int x,int y,struct background_image *out)
{
    memset(out,0,sizeof *out);out->clip=layer->clip;
    if(layer->kind==BG_NONE||source->w<=0||source->h<=0)return 0;
    int natural_w=0,natural_h=0;
    if(layer->kind==BG_URL){
#ifdef CSS_BACKGROUND_NO_BITMAP
        return 0;
#else
        char url[4096];
        if(!LOGIT_HAVE(layout_img_get)||!css_background_url(layer->image.value,layer->image.len,url,sizeof url))return 0;
        out->image=layout_img_get(url);
        if(!out->image||!out->image->rgba||out->image->w<=0||out->image->h<=0)return 0;
        natural_w=out->image->w;natural_h=out->image->h;
#endif
    }
    double area_x,area_y,area_w,area_h;
    background_area(source,layer->origin,&area_x,&area_y,&area_w,&area_h);
    if(area_w<=0||area_h<=0)return 0;
    const struct bg_geometry *g=&layer->geometry;
    double w,h;
    if(g->mode==BG_COVER||g->mode==BG_CONTAIN){
        if(natural_w){double sx=area_w/natural_w,sy=area_h/natural_h;
            double scale=g->mode==BG_COVER?fmax(sx,sy):fmin(sx,sy);w=natural_w*scale;h=natural_h*scale;}
        else {w=area_w;h=area_h;}
    }else{
        w=g->size[0].automatic?-1:g->size[0].px+g->size[0].percent*area_w;
        h=g->size[1].automatic?-1:g->size[1].px+g->size[1].percent*area_h;
        if(natural_w){
            if(w<0&&h<0){w=natural_w;h=natural_h;}
            else if(w<0)w=h*natural_w/natural_h;
            else if(h<0)h=w*natural_h/natural_w;
        }else {if(w<0)w=area_w;if(h<0)h=area_h;}
    }
    if(!isfinite(w)||!isfinite(h)||w<=0||h<=0||w>1e9||h>1e9)return 0;
    double old_w=w,old_h=h;
    if(g->repeat[0]==BG_ROUND){double n=floor(area_w/w+.5);if(n<1)n=1;w=area_w/n;}
    if(g->repeat[1]==BG_ROUND){double n=floor(area_h/h+.5);if(n<1)n=1;h=area_h/n;}
    /* Round first changes the concrete image size. An auto other dimension
     * restores its ORIGINAL ratio; ignoring this step distorts sprite tiles. */
    if(g->repeat[0]==BG_ROUND&&g->repeat[1]!=BG_ROUND&&g->size[1].automatic)h=old_h*w/old_w;
    if(g->repeat[1]==BG_ROUND&&g->repeat[0]!=BG_ROUND&&g->size[0].automatic)w=old_w*h/old_h;
    double pos[2]={area_x+g->position[0].px+g->position[0].percent*(area_w-w),
                   area_y+g->position[1].px+g->position[1].percent*(area_h-h)};
    double area[2]={area_w,area_h},extent[2]={w,h},origin[2]={area_x,area_y};
    for(int i=0;i<2;i++){
        out->repeat[i]=g->repeat[i];out->period[i]=extent[i];
        if(g->repeat[i]==BG_SPACE){
            double n=floor(area[i]/extent[i]);
            if(n<2)out->repeat[i]=BG_NO_REPEAT;
            else {pos[i]=origin[i];out->period[i]=(area[i]-extent[i])/(n-1);}
        }
    }
    if(layer->kind==BG_GRADIENT){
        struct cgradient parsed;
        int font=style->background_font[2]>0?style->background_font[2]:style->font_px;
        if(!LOGIT_HAVE(css_gradient_parse)||!css_gradient_parse(layer->image.value,layer->image.len,font,root_px(),&parsed)||
           !css_gradient_prepare_rgba(&parsed,w,h,style->color,style->color_alpha,&out->gradient))return 0;
    }
    /* Resolve origin, cover/contain, round and authored lengths BEFORE an
     * axis transform. Re-solving in a stretched rectangle changes the crop. */
    out->scale_x=(double)painted->w/source->w;out->scale_y=(double)painted->h/source->h;
    if(out->scale_x<=0||out->scale_y<=0)return 0;
    out->x=x+pos[0]*out->scale_x;out->y=y+pos[1]*out->scale_y;
    out->w=w;out->h=h;out->kind=layer->kind;return 1;
}
static int background_prepare(const struct item *e,const struct item *source,int x,int y,struct background_paint *out)
{
    memset(out,0,sizeof *out);const struct cstyle *style=sty(e);struct bg_layers parsed;
    if(!style||style->mask_refused||!LOGIT_HAVE(css_background_layers)||!css_background_layers(style,&parsed))return 0;
    out->color=e->bg;out->alpha=e->has_bg?e->bg_alpha:0;
    if(LOGIT_HAVE(css_background_color))css_background_color(style,&out->color,&out->alpha);
    out->count=parsed.count;out->color_clip=parsed.layer[parsed.count-1].clip;
    out->needed=parsed.count>1||out->color_clip!=BG_BORDER_BOX;
    for(int i=0;i<parsed.count;i++){
        const struct bg_layer *layer=&parsed.layer[i];
        int ready=background_image_prepare(layer,style,source,e,x,y,&out->image[i]);
        if(layer->kind==BG_GRADIENT)out->has_gradient=1;
        if(layer->kind!=BG_NONE){
            /* The legacy single linear ramp is a band-output optimization
             * using the SAME sampler. Keep it only for initial geometry with
             * no border; it cannot represent a differently sized image tile. */
            int initial=layer->geometry.mode==BG_EXPLICIT&&layer->geometry.size[0].automatic&&layer->geometry.size[1].automatic&&
                !layer->geometry.position[0].px&&!layer->geometry.position[0].percent&&
                !layer->geometry.position[1].px&&!layer->geometry.position[1].percent&&
                layer->geometry.repeat[0]==BG_REPEAT&&layer->geometry.repeat[1]==BG_REPEAT&&
                layer->origin==BG_PADDING_BOX&&layer->clip==BG_BORDER_BOX;
            int border=0;for(int j=0;j<4;j++)border|=source->border_w[j];
            /* Original routing considered only READY images. A 0px image
             * prepared no pixels, so a transparent box fell back to the old
             * full-box ramp and silently painted it anyway. Routing belongs
             * to the declaration: zero-sized, pending and failed images must
             * stay transparent in this consumer, not revive in another one. */
#ifdef CSS_BACKGROUND_READY_ONLY_ROUTING
            if(ready)
#endif
            if(!ready||layer->kind==BG_URL||!initial||border||e->w!=source->w||e->h!=source->h||out->image[i].gradient.kind!=CG_LINEAR)out->needed=1;
        }
    }
    return out->needed;
}
static int background_axis(double *p,double size,double period,int repeat)
{
    if(repeat!=BG_NO_REPEAT){*p=fmod(*p,period);if(*p<0)*p+=period;}
    return *p>=0&&*p<size;
}
static unsigned background_cached_signature(const struct cstyle *style)
{
    struct bg_layers layers;unsigned hash=2166136261u;
    if(!style||style->mask_refused||!LOGIT_HAVE(css_background_layers)||
       !LOGIT_HAVE(layout_img_get)||!css_background_layers(style,&layers))return hash;
    for(int i=0;i<layers.count;i++)if(layers.layer[i].kind==BG_URL){
        char url[4096];
        if(!css_background_url(layers.layer[i].image.value,layers.layer[i].image.len,url,sizeof url))continue;
        /* Decoded entries are immutable until document teardown. Identity
         * therefore describes the exact missing-to-ready paint transition;
         * hashing the URL alone would describe intent, not visible pixels. */
        uint64_t address=(uint64_t)(uintptr_t)layout_img_get(url);
        hash=(hash^(unsigned)address)*16777619u;
        hash=(hash^(unsigned)(address>>32))*16777619u;
    }
    return hash;
}
static void background_sample(const struct background_image *image,int dx,int dy,unsigned *rgb,int *alpha)
{
    *rgb=0;*alpha=0;if(!image->kind)return;
    double device=100.0/(scale_pct?scale_pct:100);
    double x=((dx+.5)*device-image->x)/image->scale_x;
    double y=((dy+.5)*device-image->y)/image->scale_y;
    if(!background_axis(&x,image->w,image->period[0],image->repeat[0])||
       !background_axis(&y,image->h,image->period[1],image->repeat[1]))return;
    if(image->kind==BG_GRADIENT){
        css_gradient_sample_pixel(&image->gradient,x,y,device/image->scale_x,device/image->scale_y,rgb,alpha);return;
    }
    int ix=(int)(x*image->image->w/image->w),iy=(int)(y*image->image->h/image->h);
    if(ix>=image->image->w)ix=image->image->w-1;if(iy>=image->image->h)iy=image->image->h-1;
    const unsigned char *pixel=image->image->rgba+4*((long long)iy*image->image->w+ix);
    *rgb=((unsigned)pixel[0]<<16)|((unsigned)pixel[1]<<8)|pixel[2];*alpha=pixel[3];
}
