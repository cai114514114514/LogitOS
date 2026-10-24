/* Numeric oracle for CSS Images 3 geometry/fixup and Images 4 conic rays.
 * Values below come from the specified line/radius/turn positions, not from
 * the sampler's own parameter helpers. This complements (never replaces)
 * the actual parser -> layout -> GUI pixel fixture and guest scanout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "paint/gradient.h"
#include "img.h"

void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
void img_register(img_detect_fn a,img_decode_fn b){(void)a;(void)b;}
static int checks,failures;
static void check(int yes,const char *label)
{checks++;printf("%s: %s\n",yes?"ok":"FAIL",label);if(!yes)failures++;}
static int near_color(unsigned a,unsigned b)
{
    for(int k=0;k<3;k++) {
        int d=(int)((a>>(k*8))&255)-(int)((b>>(k*8))&255);
        if(d < -1 || d > 1)return 0;
    }
    return 1;
}
struct sample_case {
    const char *label,*css;
    double w,h,x,y;
    unsigned color;
    int alpha,pixel;
};
static const struct sample_case cases[]={
 {"implicit linear starts at top","linear-gradient(red,blue)",100,100,50,0,0xff0000,255,0},
 {"linear default ends at bottom","linear-gradient(red,blue)",100,100,50,100,0x0000ff,255,0},
 {"zero degrees goes upward","linear-gradient(0deg,red,blue)",100,100,50,0,0x0000ff,255,0},
 {"90 degrees is clockwise to right","linear-gradient(90deg,red,blue)",100,100,25,50,0xbf0040,255,0},
 {"270 degrees runs toward left","linear-gradient(270deg,red,blue)",100,100,25,50,0x4000bf,255,0},
 {"corner direction puts neighboring corner at half","linear-gradient(to top right,red,blue)",200,100,0,0,0x800080,255,0},
 {"corner direction ends at chosen corner","linear-gradient(to top right,red,blue)",200,100,200,0,0x0000ff,255,0},
 {"negative linear stops influence visible start","linear-gradient(red -50%,blue 150%)",100,100,50,0,0xbf0040,255,0},
 {"overrange linear stops influence visible end","linear-gradient(red -50%,blue 150%)",100,100,50,100,0x4000bf,255,0},
 {"fixup precedes auto distribution","linear-gradient(to right,red 80px,white 0px,black,blue 100px)",100,100,90,50,0x000000,255,0},
 {"negative first stop spaces omitted middle correctly","linear-gradient(red -50%,white,blue)",100,100,50,25,0xffffff,255,0},
 {"last auto stop clamps beyond 100 percent","linear-gradient(red,white -50%,black 150%,blue)",100,100,50,100,0x555555,255,0},
 {"two-position stop ends with a hard edge","linear-gradient(red 0% 50%,blue 50% 100%)",100,100,50,50,0x0000ff,255,0},
 {"transparent red interpolates in premultiplied sRGB","linear-gradient(to right,rgba(255,0,0,0),blue)",100,100,50,50,0x0000ff,128,0},
 {"explicit in srgb follows the same premultiplied rule","linear-gradient(in srgb,transparent,blue)",100,100,50,50,0x0000ff,128,0},
 {"transition hint selects the halfway color","linear-gradient(to right,black 0%,25%,white 100%)",100,100,25,50,0x808080,255,0},
 {"zero hint makes immediate transition","linear-gradient(to right,black 0%,0%,white 100%)",100,100,1,50,0xffffff,255,0},
 {"endpoint hint delays transition","linear-gradient(to right,black 0%,100%,white 100%)",100,100,99,50,0x000000,255,0},
 {"circle explicit radius resolves percentages","radial-gradient(circle 40px,red,blue)",100,100,70,50,0x800080,255,0},
 {"circle percentage radius uses scaled diagonal","radial-gradient(circle 100%,red,blue)",60,80,60,40,0x93006c,255,0},
 {"ellipse horizontal radius resolves percentages","radial-gradient(ellipse 40px 20px,red,blue)",100,100,70,50,0x800080,255,0},
 {"ellipse vertical radius resolves percentages","radial-gradient(ellipse 40px 20px,red,blue)",100,100,50,60,0x800080,255,0},
 {"circle closest side uses nearest box edge","radial-gradient(circle closest-side at 20px 30px,red,blue)",100,80,40,30,0x0000ff,255,0},
 {"circle farthest side uses farthest box edge","radial-gradient(circle farthest-side at 20px 30px,red,blue)",100,80,60,30,0x800080,255,0},
 {"ellipse closest corner keeps side aspect ratio","radial-gradient(ellipse closest-corner at 20px 30px,red,blue)",100,80,0,0,0x0000ff,255,0},
 {"ellipse farthest corner keeps side aspect ratio","radial-gradient(ellipse farthest-corner at 20px 30px,red,blue)",100,80,100,80,0x0000ff,255,0},
 {"radial outside center uses infinite box edge lines","radial-gradient(circle closest-side at -10px 50px,red,blue)",100,100,0,50,0x0000ff,255,0},
 {"radial em size follows actual font","radial-gradient(circle 2em,currentColor,blue)",100,100,70,50,0x0055a2,255,0},
 {"zero circle retains radial px stops","radial-gradient(circle 0px,red 0px,blue 20px)",100,100,60,50,0x800080,255,0},
 {"zero ellipse width mirrors horizontal px stops","radial-gradient(ellipse 0px 30px,red 0px,blue 20px)",100,100,40,5,0x800080,255,0},
 {"zero ellipse height takes final color","radial-gradient(ellipse 30px 0px,red 0px,blue 20px)",100,100,50,50,0x0000ff,255,0},
 {"conic upward ray is zero degrees","conic-gradient(red,blue)",100,100,50,0,0xff0000,255,0},
 {"conic clockwise right ray is one quarter","conic-gradient(red,blue)",100,100,100,50,0xbf0040,255,0},
 {"conic downward ray is one half","conic-gradient(red,blue)",100,100,50,100,0x800080,255,0},
 {"conic from angle rotates zero ray","conic-gradient(from 90deg,red,blue)",100,100,100,50,0xff0000,255,0},
 {"negative conic stops remain offrange","conic-gradient(white -180deg,black 540deg)",100,100,50,0,0xbfbfbf,255,0},
 {"conic angular units accept turns","conic-gradient(red 0turn,blue 1turn)",100,100,0,50,0x4000bf,255,0},
 {"conic center positioning resolves independently","conic-gradient(at 20px 30px,red,blue)",100,100,20,0,0xff0000,255,0},
 {"linear repeats beyond final stop","repeating-linear-gradient(to right,red 0px 10px,blue 10px 20px)",100,100,25,50,0xff0000,255,0},
 {"linear repeats before first stop","repeating-linear-gradient(to right,red 10px 20px,blue 20px 30px)",100,100,5,50,0x0000ff,255,0},
 {"radial repeats concentric bands","repeating-radial-gradient(circle 60px,red 0px 10px,blue 10px 20px)",100,100,75,50,0xff0000,255,0},
 {"conic repeats angular bands","repeating-conic-gradient(red 0deg 45deg,blue 45deg 90deg)",100,100,50,100,0xff0000,255,0},
 {"zero repeating period averages endpoint weights","repeating-linear-gradient(red 0px,white 0px,blue 0px)",100,100,25,50,0xbf80bf,255,0},
 {"zero height repeated ellipse averages color","repeating-radial-gradient(ellipse 30px 0px,black 0px,white 10px,black 20px)",100,100,50,50,0x808080,255,0},
 {"unresolvable repeating line averages at physical pixel scale","repeating-linear-gradient(to right,black 0px,white .1px)",100,100,25,50,0x808080,255,1},
 {"unresolvable repeating conic averages near center","repeating-conic-gradient(black 0deg,white .01deg)",100,100,51,50,0x808080,255,1},
 {"single color gradient covers its whole image","conic-gradient(red)",100,100,10,20,0xff0000,255,0},
 {"single color repeat remains a real solid gradient","repeating-linear-gradient(blue 10px)",100,100,10,20,0x0000ff,255,0}
};

int main(void)
{
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];i++) {
        const struct sample_case *s=&cases[i];
        struct cgradient parsed;struct paint_gradient paint;
        unsigned color=0;int alpha=0;
        int ok=css_gradient_parse(s->css,(int)strlen(s->css),20,16,&parsed) &&
               css_gradient_prepare(&parsed,s->w,s->h,0x00aa44,&paint);
        if(ok) {
            if(s->pixel)css_gradient_sample_pixel(&paint,s->x,s->y,1,1,&color,&alpha);
            else css_gradient_sample(&paint,s->x,s->y,&color,&alpha);
        }
        if(!ok || !near_color(color,s->color) || abs(alpha-s->alpha)>1)
            printf("sample got=%06x/%d expected=%06x/%d prepared=%d\n",color,alpha,s->color,s->alpha,ok);
        check(ok && near_color(color,s->color) && abs(alpha-s->alpha)<=1,s->label);
    }
    /* Construct every slot directly, independently of the token grammar. The
     * last white stop must survive the old GFX cap of eight entries. */
    struct cgradient full={0};full.kind=CG_LINEAR;full.angle_mdeg=90000;full.nstop=CG_MAXSTOP;
    for(int i=0;i<CG_MAXSTOP;i++) {
        full.stop[i].color=i==CG_MAXSTOP-1?"white":"black";
        full.stop[i].colorlen=5;full.stop[i].pos_kind=CG_POS_PX;full.stop[i].pos=i;
    }
    struct paint_gradient paint;unsigned color=0;int alpha=0;
    int ok=css_gradient_prepare(&full,100,100,0,&paint);
    if(ok)css_gradient_sample(&paint,CG_MAXSTOP-1,50,&color,&alpha);
    check(ok&&color==0xffffff&&alpha==255,"all 32 color stops reach the sampler without truncation");
    full.nstop=CG_MAXSTOP+1;
    check(!css_gradient_prepare(&full,100,100,0,&paint),"over-cap input refuses before reading the stop array");
    const char *current="linear-gradient(currentColor,currentColor)";
    ok=css_gradient_parse(current,(int)strlen(current),20,16,&full)&&
       css_gradient_prepare_rgba(&full,100,100,0x00aa44,128,&paint);
    if(ok)css_gradient_sample(&paint,50,50,&color,&alpha);
    check(ok&&color==0x00aa44&&alpha==128,"currentColor retains computed alpha through the RGBA prepare API");
    printf("css-gradient: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
