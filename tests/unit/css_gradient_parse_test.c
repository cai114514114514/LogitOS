/* Parser contract against the actual gradient producer and native literal
 * color validator. Geometry/pixels are independently gated by the sampler and
 * renderer tests; these assertions do not claim that an accepted form painted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css/style.h"
#include "css/gradient.h"

void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int mono)
{(void)s;(void)mono;return n*(px/2);}
static int checks,failures;
static void check(int ok,const char *label)
{checks++;printf("%s: %s\n",ok?"ok":"FAIL",label);if(!ok)failures++;}
static int parse(const char *s,struct cgradient *g)
{return css_gradient_parse(s,(int)strlen(s),20,10,g);}

static void types(void)
{
    struct cgradient g;
    check(parse("radial-gradient(red,blue)",&g)&&g.kind==CG_RADIAL&&g.shape==CG_ELLIPSE&&
          g.extent==CG_FARTHEST_CORNER&&g.center[0].percent==.5&&g.center[1].percent==.5,
          "types: default radial ellipse center and extent");
    check(parse("radial-gradient(circle closest-side at left bottom,red,blue)",&g)&&
          g.shape==CG_CIRCLE&&g.extent==CG_CLOSEST_SIDE&&g.center[0].percent==0&&g.center[1].percent==1,
          "types: circular closest-side and edge center");
    check(parse("radial-gradient(40px circle at 25% 50%,red,blue)",&g)&&
          g.extent==CG_EXPLICIT&&g.radius[0].px==40&&g.radius[1].px==40&&g.center[0].percent==.25,
          "types: explicit circle radius is retained before position");
    check(parse("radial-gradient(ellipse 50% 25% at left 10px bottom 20px,red,blue)",&g)&&
          g.extent==CG_EXPLICIT&&g.radius[0].percent==.5&&g.radius[1].percent==.25&&
          g.center[0].px==10&&g.center[1].px==-20&&g.center[1].percent==1,
          "types: ellipse radii and four-component edge position");
    check(parse("radial-gradient(farthest-side circle,red,blue)",&g)&&g.extent==CG_FARTHEST_SIDE,
          "types: shape and extent accept either order");
    check(parse("radial-gradient(closest-corner at 10px 20px,red,blue)",&g)&&g.extent==CG_CLOSEST_CORNER,
          "types: ellipse closest-corner extent");
    check(parse("radial-gradient(at left 10px,red,blue)",&g)&&
          g.center[0].percent==0&&g.center[1].px==10,
          "types: two-component left length means x keyword and y length");
    check(parse("conic-gradient(from .5turn at 10px 20px,red -90deg,blue 2turn)",&g)&&
          g.kind==CG_CONIC&&g.angle_mdeg==180000&&g.center[0].px==10&&g.center[1].px==20&&
          g.stop[0].pos_kind==CG_POS_TURN&&g.stop[0].pos==-.25&&g.stop[1].pos==2,
          "types: conic rotation center and unclipped signed angular stops");
    check(parse("conic-gradient(at top right from 0deg,red,blue)",&g)==0,
          "types: conic from must precede at in its grammar");
    check(parse("repeating-linear-gradient(to right,red 5px,blue 15px)",&g)&&g.kind==CG_LINEAR&&
          g.repeating&&g.angle_mdeg==90000&&g.stop[0].pos==5&&g.stop[1].pos==15,
          "types: repeating linear period endpoints");
    check(parse("repeating-radial-gradient(circle 20px,red -5px,blue 5px)",&g)&&
          g.kind==CG_RADIAL&&g.repeating&&g.radius[0].px==20&&g.stop[0].pos==-5,
          "types: repeating radial explicit geometry and negative endpoint");
    check(parse("repeating-conic-gradient(from 0,black 0deg 25%,white 0deg 50%)",&g)&&
          g.kind==CG_CONIC&&g.repeating&&g.nstop==4&&g.stop[1].pos_kind==CG_POS_PCT&&g.stop[1].pos==2500,
          "types: repeating conic mixed angular and percentage double stops");
}
static void stops(void)
{
    struct cgradient g;
    check(parse("linear-gradient(0,red,blue)",&g)&&g.angle_mdeg==0,
          "stops: Images4 unitless zero direction is valid");
    check(parse("linear-gradient(in srgb to bottom right,currentColor .25em,blue 2rem)",&g)&&
          g.dir==CG_DIR_CORNER&&g.corner==CG_CORNER_BR&&g.stop[0].current_color&&
          g.stop[0].pos==5&&g.stop[1].pos==20,
          "stops: explicit sRGB currentColor and precise font units");
    check(parse("linear-gradient(red 1e1px,blue 1.25px)",&g)&&g.stop[0].pos==10&&g.stop[1].pos==1.25,
          "stops: exponent and fraction remain precise for later fixup");
    check(parse("linear-gradient(red,40%,blue)",&g)&&g.nstop==3&&g.stop[1].hint&&
          !g.stop[1].color&&g.stop[1].pos_kind==CG_POS_PCT&&g.stop[1].pos==4000,
          "stops: color hint retains its own position without a color");
    check(parse("conic-gradient(red 0,.25turn,blue 1turn)",&g)&&g.stop[1].hint&&
          g.stop[1].pos_kind==CG_POS_TURN&&g.stop[1].pos==.25,
          "stops: angular conic hint");
    check(parse("linear-gradient(red -50%,blue 150%)",&g)&&g.stop[0].pos==-5000&&g.stop[1].pos==15000,
          "stops: out-of-range percentages are never clipped at parse time");
    check(parse("linear-gradient(/*start*/ red /**/ 0%,blue /*end*/)",&g)&&g.nstop==2,
          "stops: comments are token trivia");
    check(parse("linear-gradient(red 0px 10px,blue)",&g)&&g.nstop==3&&
          g.stop[0].color==g.stop[1].color&&g.stop[1].pos==10,
          "stops: two-position color expands to two borrowed identical spans");
    check(parse("radial-gradient(circle 50%,red)",&g)&&g.shape==CG_CIRCLE&&
          g.radius[0].percent==.5&&g.radius[1].percent==.5&&g.nstop==2&&
          g.stop[0].color==g.stop[1].color,
          "stops: circle percentage and single color retain uniform image metadata");
    check(parse("repeating-conic-gradient(red 10deg 20deg)",&g)&&g.repeating&&
          g.nstop==2&&g.stop[0].pos_kind==CG_POS_TURN,
          "stops: single double-position color is a valid uniform repeating image");
    char many[1200];int n=snprintf(many,sizeof many,"linear-gradient(");
    for(int i=0;i<CG_MAXSTOP;i++)n+=snprintf(many+n,sizeof many-(unsigned)n,"%sred",i?",":"");
    n+=snprintf(many+n,sizeof many-(unsigned)n,")");
    check(parse(many,&g)&&g.nstop==CG_MAXSTOP,"stops: full sampler stop capacity is accepted");
    many[n-1]=',';strcpy(many+n,"blue)");
    check(!parse(many,&g),"stops: capacity overflow refuses whole value rather than truncating");
}
static void invalid(void)
{
    static const char *const cases[]={
        "linear-gradient(red,blue) garbage", "linear-gradient(red,blue),url(x)",
        "linear-gradient(red,blue) !important", "linear-gradient (red,blue)",
        "linear-gradient(45,red,blue)", "linear-gradient(1.deg,red,blue)",
        "linear-gradient(red 1.px,blue)", "linear-gradient(nonsense,blue)",
        "linear-gradient(#1,blue)", "linear-gradient(red 0px 10px 20px)",
        "linear-gradient(inherit,blue)", "radial-gradient(initial,blue)",
        "conic-gradient(unset,blue)", "repeating-linear-gradient(revert,blue)",
        "linear-gradient(20%,red,blue)", "linear-gradient(red,20%,30%,blue)",
        "linear-gradient(red,blue,50%)", "linear-gradient(in oklab,red,blue)",
        "linear-gradient(in srgb in srgb,red,blue)", "-webkit-linear-gradient(top,red,blue)",
        "linear-gradient(to in srgb top,red,blue)",
        "radial-gradient(at left in srgb top,red,blue)",
        "radial-gradient(circle 20px 30px,red,blue)", "radial-gradient(ellipse 20px,red,blue)",
        "radial-gradient(10px ellipse 20px,red,blue)",
        "radial-gradient(circle -1%,red,blue)", "radial-gradient(-1px,red,blue)",
        "radial-gradient(circle closest-side 20px,red,blue)",
        "radial-gradient(circle circle,red,blue)", "radial-gradient(at left right,red,blue)",
        "conic-gradient(from 2,red,blue)",
        "conic-gradient(red 10px,blue)", "conic-gradient(from 0deg,red,blue) trailing",
        "radial-gradient(calc(20px),red,blue)", "linear-gradient(red var(--x),blue)"
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        struct cgradient g;int ok=!parse(cases[i],&g);
        if(!ok)printf("accepted invalid value: %s\n",cases[i]);
        check(ok,"invalid: whole grammar refusal");
    }
}
int main(int argc,char **argv)
{
    const char *group=argc>1?argv[1]:"all";
    if(!strcmp(group,"all")||!strcmp(group,"types"))types();
    if(!strcmp(group,"all")||!strcmp(group,"stops"))stops();
    if(!strcmp(group,"all")||!strcmp(group,"invalid"))invalid();
    printf("css-gradient: %d checks, %d failures\n",checks,failures);
    return failures?1:checks?0:2;
}
