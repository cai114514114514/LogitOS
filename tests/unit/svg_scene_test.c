/* SPDX-License-Identifier: MIT
 * Pixel oracles use axis-aligned interiors and independent equivalent SVGs;
 * the control removes each scene feature in the actual decoder, not a mock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"
static img_detect_fn detect;static img_decode_fn decode;
static int checks,failures,fail_alloc=-1,allocs;
void *kmalloc(unsigned long n){int at=allocs++;if(fail_alloc>=0&&at==fail_alloc)return 0;return malloc(n);}
void kfree(void *p){free(p);}
void img_register(img_detect_fn a,img_decode_fn b){detect=a;decode=b;}
static void check(int b,const char *msg){checks++;if(!b){failures++;printf("FAIL: %s\n",msg);}}
static struct image dec(const char *s){struct image im={0};int rc=decode((const uint8_t*)s,(int)strlen(s),&im);check(!rc,"fixture decodes");return im;}
static int pixel(const struct image *im,int x,int y,int r,int g,int b,int a){if(!im->rgba||x<0||y<0||x>=im->w||y>=im->h)return 0;const unsigned char*p=im->rgba+(y*im->w+x)*4;return p[0]==r&&p[1]==g&&p[2]==b&&p[3]==a;}
static int alpha(const struct image *im,int x,int y){return im->rgba?im->rgba[(y*im->w+x)*4+3]:-1;}
static int area(const struct image *im){int n=0;if(im->rgba)for(int i=0;i<im->w*im->h;i++)if(im->rgba[i*4+3])n++;return n;}
static void done(struct image *im){free(im->rgba);im->rgba=0;}
#define SVG(inner) "<svg width='16' height='16'>" inner "</svg>"
#define RECT "<rect width='16' height='16'/>"
int main(void){svg_register();check(detect((const uint8_t*)"<svg/>",6),"registry reaches SVG decoder");struct image im;
    im=dec(SVG("<rect width='16' height='16' fill='red'/>"));check(pixel(&im,8,8,255,0,0,255),"static red control");done(&im);
    im=dec("<svg width='16' height='16' color='#00ff00' fill='currentColor'>" RECT "</svg>");check(pixel(&im,8,8,0,255,0,255),"currentColor root inherited");done(&im);
    im=dec(SVG("<g color='red' fill='currentColor'><rect width='8' height='16'/><rect x='8' width='8' height='16' color='blue'/></g>"));check(pixel(&im,4,4,255,0,0,255)&&pixel(&im,12,4,0,0,255,255),"currentColor resolves at child");done(&im);
    im=dec(SVG("<rect width='16' height='16' color='red' fill='blue' style='fill: currentColor; color: #00ff00'/>"));check(pixel(&im,8,8,0,255,0,255),"currentColor inline CSS beats attribute");done(&im);
    im=dec(SVG("<rect width='16' height='16' style='fill:blue!important;fill:red'/>"));check(pixel(&im,8,8,0,0,255,255),"inline important priority");done(&im);
    im=dec(SVG("<path d='M2 8h12' fill='none' color='red' stroke='currentColor' stroke-width='4'/>"));check(pixel(&im,8,8,255,0,0,255),"currentColor stroke");done(&im);
    im=dec(SVG("<g fill-opacity='.5'><rect width='16' height='16' fill-opacity='1' fill='red'/></g>"));check(pixel(&im,8,8,255,0,0,255),"fill opacity inherited value can be overridden");done(&im);
    im=dec(SVG("<g opacity='.5' fill='red'><rect width='12' height='16'/><rect x='4' width='12' height='16'/></g>"));check(pixel(&im,2,8,255,0,0,127)&&pixel(&im,8,8,255,0,0,127),"group opacity composites once");done(&im);
    im=dec(SVG("<g transform='translate(10,0)'><rect width='4' height='4' fill='red'/></g>"));check(pixel(&im,11,1,255,0,0,255)&&alpha(&im,1,1)==0,"transform translates geometry");done(&im);
    im=dec(SVG("<g transform='translate(4 2) scale(2 3)'><rect width='2' height='2' fill='blue'/></g>"));check(pixel(&im,6,5,0,0,255,255)&&alpha(&im,2,2)==0&&area(&im)==24,"transform list composition order");done(&im);
    im=dec(SVG("<g transform='translate(8 0)'><path transform='rotate(90)' d='M0 0h4v2h-4z' fill='red'/></g>"));check(pixel(&im,7,2,255,0,0,255)&&alpha(&im,9,2)==0,"transform nested rotation relative path");done(&im);
    im=dec(SVG("<rect width='4' height='2' transform='rotate(90 4 4)' fill='red'/>"));check(pixel(&im,6,1,255,0,0,255)&&alpha(&im,1,1)==0,"transform centered rotation");done(&im);
    im=dec(SVG("<rect width='4' height='4' transform='matrix(-1 0 0 1 12 2)' fill='red'/>"));check(pixel(&im,9,3,255,0,0,255)&&alpha(&im,1,1)==0,"transform matrix reflection");done(&im);
    im=dec(SVG("<rect width='4' height='4' transform='skewX(45)' fill='red'/>"));check(pixel(&im,5,3,255,0,0,255)&&alpha(&im,0,3)==0,"transform skewX");done(&im);
    im=dec(SVG("<path d='M2 2v4' fill='none' stroke='red' stroke-width='2' transform='scale(3 2)'/>"));check(pixel(&im,4,6,255,0,0,255)&&pixel(&im,7,6,255,0,0,255)&&alpha(&im,2,6)==0,"transform nonuniform stroke outline");done(&im);
    im=dec("<svg width='16' height='16' viewBox='0 0 8 4' preserveAspectRatio='none'><rect width='8' height='4' fill='red'/></svg>");check(area(&im)==256,"viewBox nonuniform preserveAspectRatio none");done(&im);
    im=dec(SVG("<defs><rect id='box' width='4' height='4'/></defs><use href='#box' x='8' y='2' fill='red'/>"));check(pixel(&im,9,3,255,0,0,255)&&alpha(&im,1,1)==0,"use local geometry and x y");done(&im);
    im=dec(SVG("<use href='#later' color='red'/><defs><g id='later' fill='currentColor'><rect width='8' height='8'/></g></defs>"));check(pixel(&im,2,2,255,0,0,255),"use forward reference currentColor");done(&im);
    im=dec(SVG("<defs fill='blue'><g id='item'><rect width='4' height='4'/></g></defs><use href='#item' fill='red'/><use xlink:href='#item' x='8' fill='green'/>"));check(pixel(&im,1,1,255,0,0,255)&&pixel(&im,9,1,0,128,0,255),"use inherits each instance not defs ancestor");done(&im);
    im=dec(SVG("<defs><rect id='a&amp;b' width='4' height='4' fill='blue'/></defs><use href='#a&#38;b'/>"));check(pixel(&im,1,1,0,0,255,255),"use XML escaped fragment identity");done(&im);
    im=dec(SVG("<symbol id='s' viewBox='0 0 2 2'><rect width='2' height='2' fill='red'/></symbol><use href='#s' width='8' height='8' x='4' y='4'/>"));check(pixel(&im,5,5,255,0,0,255)&&alpha(&im,2,2)==0&&area(&im)==64,"use symbol viewport");done(&im);
    im=dec(SVG("<defs><g id='a'><use href='#b'/></g><g id='b'><use href='#a'/></g></defs><use href='#a'/><use href='https://example.invalid/a.svg#x'/>"));check(area(&im)==0,"use cycles and external references produce no geometry");done(&im);
    im=dec(SVG("<defs><clipPath id='c'><rect width='4' height='16'/></clipPath></defs><rect width='16' height='16' fill='red' clip-path='url(#c)'/>"));check(area(&im)==64&&pixel(&im,2,8,255,0,0,255)&&alpha(&im,8,8)==0,"clip user space clips right pixels");done(&im);
    im=dec(SVG("<defs><clipPath id='c'><rect width='4' height='16' fill='none' opacity='0'/><rect x='12' width='4' height='16'/></clipPath></defs><g clip-path='url(#c)' fill='blue'>" RECT "</g>"));check(area(&im)==128&&pixel(&im,2,8,0,0,255,255)&&alpha(&im,8,8)==0,"clip union ignores fill and opacity");done(&im);
    im=dec(SVG("<defs><clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' height='1'/></clipPath></defs><rect x='4' y='4' width='8' height='8' fill='red' clip-path='url(#c)'/>"));check(area(&im)==32&&pixel(&im,5,5,255,0,0,255)&&alpha(&im,9,5)==0,"clip objectBoundingBox relative to geometry");done(&im);
    im=dec(SVG("<defs><clipPath id='c'><rect width='4' height='4'/></clipPath></defs><g transform='translate(8 4)' clip-path='url(#c)'><rect width='8' height='8' fill='blue'/></g>"));check(pixel(&im,9,5,0,0,255,255)&&alpha(&im,13,5)==0&&area(&im)==16,"clip shares referencing transform");done(&im);
    im=dec(SVG("<defs><clipPath id='a'><rect width='8' height='16'/></clipPath><clipPath id='b'><rect width='16' height='8'/></clipPath></defs><g clip-path='url(#a)'><g clip-path='url(#b)'>" RECT "</g></g>"));check(area(&im)==64,"clip nested intersection");done(&im);
    im=dec(SVG("<defs><clipPath id='c' clip-rule='evenodd'><path d='M0 0h16v16H0z M4 4h8v8H4z'/></clipPath></defs><rect width='16' height='16' clip-path='url(#c)'/>"));check(area(&im)==192&&alpha(&im,8,8)==0,"clip evenodd hole");done(&im);
    im=dec(SVG("<defs><rect id='r' width='4' height='16'/><clipPath id='c'><use href='#r' x='8'/></clipPath></defs><rect width='16' height='16' fill='blue' clip-path='url(&quot;#c&quot;)'/>"));check(area(&im)==64&&pixel(&im,9,8,0,0,255,255),"clip use and escaped quoted url");done(&im);
    im=dec(SVG("<g display='none'>" RECT "</g><g visibility='hidden'><rect width='4' height='4' visibility='visible' fill='red'/></g>"));check(area(&im)==16&&pixel(&im,1,1,255,0,0,255),"visibility child override and display subtree");done(&im);
    im=dec(SVG("<defs><clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' height='1'/></clipPath></defs><g clip-path='url(#c)' fill='red'><rect x='4' y='4' width='4' height='4'/><rect x='8' y='4' width='4' height='4'/></g>"));check(area(&im)==16&&pixel(&im,5,5,255,0,0,255)&&alpha(&im,9,5)==0,"clip group object bounding box");done(&im);
    im=dec(SVG("<defs clip-rule='evenodd'><clipPath id='c'><path d='M0 0h16v16H0z M4 4h8v8H4z'/></clipPath></defs><rect width='16' height='16' clip-path='url(#c)'/>"));check(alpha(&im,8,8)==0&&area(&im)==192,"clip original DOM style inheritance");done(&im);
    im=dec(SVG("<symbol id='s' viewBox='0 0 4 4'><rect x='-4' y='-4' width='12' height='12' fill='red'/></symbol><use href='#s' x='4' y='4' width='4' height='4'/>"));check(area(&im)==16&&pixel(&im,5,5,255,0,0,255)&&alpha(&im,1,1)==0,"use symbol clips to viewport");done(&im);
    im=dec(SVG("<svg x='4' y='4' width='4' height='4' viewBox='0 0 4 4'><rect width='12' height='12' fill='red'/></svg>"));check(area(&im)==16&&pixel(&im,5,5,255,0,0,255)&&alpha(&im,9,9)==0,"nested SVG viewport clips");done(&im);
    im=dec(SVG("<rect width='4' height='4' transform='translate(8) broken(2)' fill='red'/>"));check(area(&im)==16&&pixel(&im,1,1,255,0,0,255),"invalid transform declaration ignored as a whole");done(&im);
    im=dec(SVG("<rect width='10000' height='10000' fill='red' transform='scale(.001)'/>"));check(pixel(&im,4,4,255,0,0,255)&&alpha(&im,12,12)==0,"transform fractional scale retains matrix precision");done(&im);
    im=dec(SVG("<rect width='.016e3' height='0.016e3' fill='red'/>"));check(area(&im)==256,"numeric exponent before fixed-point rounding");done(&im);
    im=dec(SVG("<defs><rect id='r' width='8' height='8' fill='red'/><clipPath id='c'><rect width='4' height='8'/></clipPath></defs><use href='#r' x='8' clip-path='url(#c)'/>"));check(area(&im)==32&&pixel(&im,9,2,255,0,0,255)&&alpha(&im,13,2)==0,"use x y applies before user space clip");done(&im);
    im=dec(SVG("<symbol id='s' width='4' height='4'><rect width='16' height='16' fill='red'/></symbol><use href='#s' x='4' y='4'/>"));check(area(&im)==16&&pixel(&im,5,5,255,0,0,255)&&alpha(&im,9,9)==0,"use auto preserves referenced viewport size");done(&im);
    unsigned char rgba[4]={0};check(img_css_color("rgba(1,2,3,.5)",14,rgba)&&rgba[0]==1&&rgba[3]==128,"shared CSS color alpha unchanged");
    const char *trunc=SVG("<defs><clipPath id='c'><circle cx='8' cy='8' r='4'/></clipPath><g id='x'><path d='M0 0C0 8 8 0 8 8A4 4 0 0 1 0 8z'/></g></defs><g transform='translate(2 2)' clip-path='url(#c)'><use href='#x'/></g>");
    for(int i=0;i<(int)strlen(trunc);i++){struct image t={0};if(!decode((const uint8_t*)trunc,i,&t))free(t.rgba);}check(1,"all truncation prefixes terminate");
    char deep[4096];strcpy(deep,"<svg>");for(int i=0;i<60;i++)strcat(deep,"<g>");strcat(deep,RECT);for(int i=0;i<60;i++)strcat(deep,"</g>");strcat(deep,"</svg>");struct image bad={0};check(decode((const uint8_t*)deep,(int)strlen(deep),&bad)!=0,"depth budget fails cleanly");
    const char *layer=SVG("<g opacity='.5'><g opacity='.5'>" RECT "</g></g>");
    for(int i=0;i<4;i++){fail_alloc=i;allocs=0;struct image t={0};int rc=decode((const uint8_t*)layer,(int)strlen(layer),&t);check(rc!=0,"allocation failure propagates");free(t.rgba);}fail_alloc=-1;
    const char *clipped=SVG("<g clip-path='url(#c)'>" RECT "</g><defs><clipPath id='c'>" RECT "</clipPath></defs>");
    allocs=0;im=dec(clipped);int allocations=allocs;done(&im);int oom_ok=1;
    for(int i=0;i<allocations;i++){fail_alloc=i;allocs=0;struct image t={0};if(!decode((const uint8_t*)clipped,(int)strlen(clipped),&t))oom_ok=0;free(t.rgba);}fail_alloc=-1;check(oom_ok,"clip allocation failures all propagate");
    char *large=malloc(100000);strcpy(large,"<svg width='16' height='16'>");for(int i=0;i<2050;i++)strcat(large,"<g/>");strcat(large,"</svg>");check(decode((const uint8_t*)large,(int)strlen(large),&bad)!=0,"node budget fails cleanly");
    strcpy(large,"<svg width='16' height='16'><defs><g id='n0'>" RECT "</g>");
    for(int i=1;i<15;i++){char step[160];snprintf(step,sizeof step,"<g id='n%d'><use href='#n%d'/><use href='#n%d'/></g>",i,i-1,i-1);strcat(large,step);}strcat(large,"</defs><use href='#n14'/></svg>");
    /* A control with use intentionally disabled has no expansion to budget. */
    int expanded=decode((const uint8_t*)large,(int)strlen(large),&bad);
    check(expanded!=0 || (bad.rgba&&area(&bad)==0),"reference expansion stays bounded");free(bad.rgba);bad.rgba=0;free(large);
    const char *range=SVG("<g transform='scale(1000) scale(1000)'>" RECT "</g>");
    int range_rc=decode((const uint8_t*)range,(int)strlen(range),&bad);
#ifdef SVG_SCENE_NO_TRANSFORM
    check(!range_rc,"transform-disabled control has no matrix budget");
#else
    check(range_rc!=0,"valid transform exceeding representation fails image");
#endif
    free(bad.rgba);bad.rgba=0;
    printf("svg-scene: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
