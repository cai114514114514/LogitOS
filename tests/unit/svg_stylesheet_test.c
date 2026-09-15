/* Ordinary local SVG images. The real registry decoder and rasterizer run in
 * every case; CSS output must equal the same geometry with explicit resolved
 * paint attributes. Interior color anchors validate the independent oracle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "img.h"
static img_detect_fn detect;static img_decode_fn decode;
static int checks,failures;
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
void img_register(img_detect_fn a,img_decode_fn b){detect=a;decode=b;}
static void check(int value,const char *name,const char *what){
    checks++;if(!value){failures++;printf("FAIL: %s: %s\n",name,what);}
}
struct sample{int x,y,r,g,b,a;};
static int pixel(const struct image *im,struct sample s){
    if(!im->rgba||s.x<0||s.y<0||s.x>=im->w||s.y>=im->h)return 0;
    const unsigned char*p=im->rgba+(s.y*im->w+s.x)*4;
    return p[0]==s.r&&p[1]==s.g&&p[2]==s.b&&p[3]==s.a;
}
static void pair(const char *name,const char *actual,const char *reference,
                 const struct sample *s,int count){
    struct image a={0},r={0};
    int ar=decode((const uint8_t*)actual,(int)strlen(actual),&a);
    int rr=decode((const uint8_t*)reference,(int)strlen(reference),&r);
    check(!ar,name,"actual SVG decodes");check(!rr,name,"reference SVG decodes");
    check(a.rgba&&a.w==16&&a.h==16,name,"actual dimensions");
    check(r.rgba&&r.w==16&&r.h==16,name,"reference dimensions");
    for(int i=0;i<count;i++){
        char label[64];snprintf(label,sizeof label,"reference RGBA anchor %d",i+1);
        check(pixel(&r,s[i]),name,label);
    }
    check(a.rgba&&r.rgba&&a.w==r.w&&a.h==r.h&&
          !memcmp(a.rgba,r.rgba,(size_t)r.w*r.h*4),name,"CSS pixels equal explicit paint");
    free(a.rgba);free(r.rgba);
}
#define SVG(s) "<svg width='16' height='16'>" s "</svg>"
#define RECT "<rect width='16' height='16'"
#define RED RECT " fill='#ff0000'/>"
#define BLUE RECT " fill='#0000ff'/>"
#define WHITE RECT " fill='#ffffff'/>"
#define GREEN RECT " fill='#00ff00'/>"
static const struct sample red[]={{8,8,255,0,0,255}};
static const struct sample black[]={{8,8,0,0,0,255}};
static const struct sample blue[]={{8,8,0,0,255,255}};
static const struct sample white[]={{8,8,255,255,255,255}};
static const struct sample green[]={{8,8,0,255,0,255}};
static const struct sample half[]={{2,8,255,0,0,127},{8,8,255,0,0,127}};
static const struct sample quarter[]={{8,8,255,0,0,63}};
static const struct sample clipped[]={{2,8,255,0,0,255},{8,8,0,0,0,0}};
static const struct sample used[]={{1,1,255,0,0,255},{9,1,0,0,255,255},{6,1,0,0,0,0}};
static const struct sample hole[]={{2,2,0,255,0,255},{8,8,0,0,0,0}};
static const struct sample halfline[]={{8,8,255,0,0,127},{8,2,0,0,0,0}};
static const struct sample line[]={{8,8,255,0,0,255},{8,2,0,0,0,0}};
static const struct sample hidden[]={{2,2,255,0,0,255},{8,8,0,0,0,0}};
#define P(n,a,r,c) pair(n,SVG(a),SVG(r),c,(int)(sizeof(c)/sizeof(c[0])))
int main(int argc,char **argv){
    svg_register();check(detect&&decode&&detect((const uint8_t*)"<svg/>",6),"literal/registry","SVG decoder is registered");
    P("literal/solid",RED,RED,red);
    P("literal/inline priority",RECT " fill='red' style='fill:blue'/>",BLUE,blue);
    P("literal/inline important",RECT " style='fill:blue!important;fill:red'/>",BLUE,blue);
    P("literal/currentColor","<g color='#00ff00' fill='currentColor'>" RECT "/></g>",GREEN,green);
    P("literal/group opacity","<g fill='red' opacity='.5'><rect width='12' height='16'/><rect x='4' width='12' height='16'/></g>",
      "<g fill='red' opacity='.5'><rect width='12' height='16'/><rect x='4' width='12' height='16'/></g>",half);
    P("literal/local use","<defs><rect id='shape' width='4' height='4'/></defs><use href='#shape' fill='red'/><use href='#shape' x='8' fill='blue'/>",
      "<rect width='4' height='4' fill='red'/><rect x='8' width='4' height='4' fill='blue'/>",used);
    P("literal/local clip","<defs><clipPath id='crop'><rect width='4' height='16'/></clipPath></defs>" RECT " fill='red' clip-path='url(#crop)'/>",
      "<rect width='4' height='16' fill='red'/>",clipped);
    if(argc==2&&!strcmp(argv[1],"literal"))goto finish;
    if(argc!=1)return 2;
    P("selector/type","<style>rect{fill:#ff0000}</style>" RECT "/>",RED,red);
    P("selector/id","<style>#shape{fill:#00ff00}</style>" RECT " id='shape'/>",GREEN,green);
    P("selector/class","<style>.ink{fill:#ffffff}</style>" RECT " class='ink'/>",WHITE,white);
    P("selector/compound","<style>rect.ink.selected#shape{fill:#0000ff}</style>" RECT " id='shape' class='selected ink'/>",BLUE,blue);
    P("selector/class token","<style>.ink{fill:#ffffff}</style>" RECT " class='before ink after'/>",WHITE,white);
    P("selector/unmatched token","<style>.ink{fill:red}</style>" RECT " class='inky' fill='blue'/>",BLUE,blue);
    P("selector/unmatched compound","<style>rect.ink.selected{fill:red}</style>" RECT " class='ink' fill='blue'/>",BLUE,blue);
    P("order/later rule","<style>.ink{fill:red}.ink{fill:blue}</style>" RECT " class='ink'/>",BLUE,blue);
    P("order/later declaration","<style>.ink{fill:red;fill:blue}</style>" RECT " class='ink'/>",BLUE,blue);
    P("order/style after shape","<style>.ink{fill:red}</style>" RECT " class='ink'/><style>.ink{fill:blue}</style>",BLUE,blue);
    P("specificity/class beats type","<style>.ink{fill:blue}rect{fill:red}</style>" RECT " class='ink'/>",BLUE,blue);
    P("specificity/id beats class","<style>#shape{fill:blue}.ink{fill:red}</style>" RECT " id='shape' class='ink'/>",BLUE,blue);
    P("specificity/compound beats class","<style>rect.ink{fill:blue}.ink{fill:red}</style>" RECT " class='ink'/>",BLUE,blue);
    P("cascade/sheet beats presentation","<style>.ink{fill:red}</style>" RECT " class='ink' fill='blue'/>",RED,red);
    P("cascade/inline beats sheet","<style>#shape{fill:red}</style>" RECT " id='shape' style='fill:blue'/>",BLUE,blue);
    P("important/sheet beats normal inline","<style>.ink{fill:red!important}</style>" RECT " class='ink' style='fill:blue'/>",RED,red);
    P("important/inline beats sheet","<style>#shape{fill:red!important}</style>" RECT " id='shape' style='fill:blue!important'/>",BLUE,blue);
    P("important/earlier declaration wins","<style>.ink{fill:blue!important;fill:red}</style>" RECT " class='ink'/>",BLUE,blue);
    P("important/type beats normal id","<style>rect{fill:blue!important}#shape{fill:red}</style>" RECT " id='shape'/>",BLUE,blue);
    P("inherit/group paint","<style>.group{fill:red}</style><g class='group'>" RECT "/></g>",RED,red);
    P("inherit/child attribute beats inherited important","<style>.group{fill:red!important}</style><g class='group'>" RECT " fill='blue'/></g>",BLUE,blue);
    P("inherit/currentColor","<style>.group{color:#00ff00;fill:currentColor}</style><g class='group'>" RECT "/></g>",GREEN,green);
    P("inherit/child color override","<style>.group{color:red;fill:currentColor}</style><g class='group'>" RECT " color='blue'/></g>",BLUE,blue);
    P("paint/fill opacity","<style>.ink{fill:red;fill-opacity:.5}</style>" RECT " class='ink'/>",RECT " fill='red' fill-opacity='.5'/>",half);
    P("paint/group opacity","<style>.group{fill:red;opacity:.5}</style><g class='group'><rect width='12' height='16'/><rect x='4' width='12' height='16'/></g>",
      "<g fill='red' opacity='.5'><rect width='12' height='16'/><rect x='4' width='12' height='16'/></g>",half);
    P("paint/stroke width","<style>.line{fill:none;stroke:red;stroke-width:4}</style><path class='line' d='M2 8h12'/>",
      "<path d='M2 8h12' fill='none' stroke='red' stroke-width='4'/>",line);
    P("paint/evenodd","<style>.ink{fill:#00ff00;fill-rule:evenodd}</style><path class='ink' d='M0 0h16v16H0z M4 4h8v8H4z'/>",
      "<path fill='#00ff00' fill-rule='evenodd' d='M0 0h16v16H0z M4 4h8v8H4z'/>",hole);
    P("paint/visibility inheritance","<style>.group{visibility:hidden}.shown{visibility:visible;fill:red}</style><g class='group'>" RECT "/><rect class='shown' width='4' height='4'/></g>",
      "<rect fill='red' width='4' height='4'/>",hidden);
    P("paint/display subtree","<style>.absent{display:none}</style><g class='absent'>" BLUE "</g><rect fill='red' width='4' height='4'/>",
      "<rect fill='red' width='4' height='4'/>",hidden);
    P("use/instance paint inheritance","<style>.warm{fill:red}.cool{fill:blue}</style><defs><rect id='shape' width='4' height='4'/></defs><use class='warm' href='#shape'/><use class='cool' href='#shape' x='8'/>",
      "<rect width='4' height='4' fill='red'/><rect x='8' width='4' height='4' fill='blue'/>",used);
    P("use/referenced class overrides instance","<style>.ink{fill:blue}</style><defs>" RECT " id='shape' class='ink'/></defs><use href='#shape' fill='red'/>",BLUE,blue);
    P("clip/styled paint still clips","<style>.ink{fill:red}</style><defs><clipPath id='crop'><rect width='4' height='16'/></clipPath></defs>" RECT " class='ink' clip-path='url(#crop)'/>",
      "<rect width='4' height='16' fill='red'/>",clipped);
    P("selector/universal","<style>*{fill:white}</style>" RECT "/>",WHITE,white);
    P("selector/list","<style>circle,.ink{fill:red}</style>" RECT " class='ink'/>",RED,red);
    P("sheet/CDATA","<style><![CDATA[.ink{fill:#ffffff}]]></style>" RECT " class='ink'/>",WHITE,white);
    P("paint/stroke opacity","<style>.line{fill:none;stroke:red;stroke-width:4;stroke-opacity:.5}</style><path class='line' d='M2 8h12'/>",
      "<path d='M2 8h12' fill='none' stroke='red' stroke-width='4' stroke-opacity='.5'/>",halfline);
    P("clip/stylesheet evenodd","<style>.rule{clip-rule:evenodd}</style><defs><clipPath class='rule' id='crop'><path d='M0 0h16v16H0z M4 4h8v8H4z'/></clipPath></defs>" RECT " fill='#00ff00' clip-path='url(#crop)'/>",
      "<defs><clipPath clip-rule='evenodd' id='crop'><path d='M0 0h16v16H0z M4 4h8v8H4z'/></clipPath></defs>" RECT " fill='#00ff00' clip-path='url(#crop)'/>",hole);
    P("keyword/inherit beats child presentation","<style>.ink{fill:inherit}</style><g fill='blue'>" RECT " class='ink' fill='red'/></g>",BLUE,blue);
    P("keyword/initial resets inherited paint","<style>.ink{fill:initial}</style><g fill='blue'>" RECT " class='ink'/></g>",RECT " fill='black'/>",black);
    P("keyword/unset restores inherited paint","<style>.ink{fill:unset}</style><g fill='blue'>" RECT " class='ink' fill='red'/></g>",BLUE,blue);
    P("keyword/explicit opacity inherit","<style>.child{opacity:inherit}</style><g fill='red' opacity='.5'>" RECT " class='child'/></g>",
      "<g fill='red' opacity='.5'>" RECT " opacity='.5'/></g>",quarter);
    P("comment/property separator","<style>.ink{fill/**/:red}</style>" RECT " class='ink'/>",RED,red);
    P("comment/important separator","<style>.ink{fill:blue/**/ !important;fill:red}</style>" RECT " class='ink'/>",BLUE,blue);
    P("selector/unsupported whole list","<style>rect#shape:hover,.ink{fill:red}</style>" RECT " id='shape' class='ink' fill='blue'/>",BLUE,blue);
    P("sheet/at rule remains unapplied","<style>@media all{.ink{fill:red}}</style>" RECT " class='ink' fill='blue'/>",BLUE,blue);
    P("validation/short hex keeps valid paint","<style>.ink{fill:blue;fill:#12}</style>" RECT " class='ink'/>",BLUE,blue);
    P("validation/long hex keeps valid paint","<style>.ink{fill:blue;fill:#123456789}</style>" RECT " class='ink'/>",BLUE,blue);
    P("validation/rgb arity keeps valid paint","<style>.ink{fill:blue;fill:rgb(255)}</style>" RECT " class='ink'/>",BLUE,blue);
    P("validation/rgb tail keeps valid paint","<style>.ink{fill:blue;fill:rgb(255,0,0) junk}</style>" RECT " class='ink'/>",BLUE,blue);
    P("validation/color none keeps valid color","<style>.ink{color:blue;color:none;fill:currentColor}</style>" RECT " class='ink'/>",BLUE,blue);
    P("comment/compound selector","<style>rect/**/.ink{fill:blue}</style>" RECT " class='ink'/>",BLUE,blue);
    P("comment/value tokens remain separate","<style>.ink{fill:blue;fill:re/**/d}</style>" RECT " class='ink'/>",BLUE,blue);
    P("sheet/CDATA bracketed selector text","<style><![CDATA[rect[data-mark='< >']{fill:red}.ink{fill:blue}]]></style>" RECT " class='ink'/>",BLUE,blue);
finish:
    printf("svg-stylesheet: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
