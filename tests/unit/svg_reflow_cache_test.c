/* Finite ordinary SVG reflows through real DOM/CSS/layout and raster pixels.
 * Count the actual decoder boundary; no stopwatch or DOM pointer is an oracle.
 * The sole unsupported-decoder specimen is an explicit local adapter result,
 * not malformed input, memory failure, or a third-party document. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
#include "css.h"
#include "dom.h"
void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int n,int px,int face){(void)s;(void)face;return n*px/2;}
int res_fetch(const char *s,unsigned char **p,int *n){(void)s;(void)p;(void)n;return -1;}
#define img_decode fixture_actual_img_decode
#include "../../c/lib/image/img.c"
#undef img_decode
static int decodes,checks,fails;
int img_decode(const unsigned char *p,int n,struct image *out)
{
    decodes++;
    if(n>0 && strstr((const char *)p,"data-decoder-status=\"unsupported\""))return -1;
    return fixture_actual_img_decode(p,n,out);
}
void layout_images_reset(void);
#define CHECK(c,m) do{checks++;if(!(c)){fails++;printf("FAIL: %s\n",m);}}while(0)
static struct node *find(struct node *n,const char *tag)
{
    if(n->type==N_ELEM && !strcmp(n->tag,tag))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *r=find(c,tag);if(r)return r;}return 0;
}
static struct node *parse(const char *html){return dom_parse(html,(int)strlen(html));}
static const struct item *image_at(int index)
{
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++)if(it[i].img && index--==0)return &it[i];
    return 0;
}
static unsigned long long pixels(const struct item *it)
{
    if(!it || !it->img)return 0;
    unsigned long long h=1469598103934665603ULL;
    for(int i=0;i<it->img->w*it->img->h*4;i++){h^=it->img->rgba[i];h*=1099511628211ULL;}
    return h;
}
static int center(const struct item *it,int r,int g,int b,int a)
{
    if(!it || !it->img)return 0;
    const struct image *im=it->img;const unsigned char *p=im->rgba+4*((im->h/2)*im->w+im->w/2);
    return abs(p[0]-r)<=1&&abs(p[1]-g)<=1&&abs(p[2]-b)<=1&&abs(p[3]-a)<=1;
}
static void paint(struct node *root,const char *css,int width)
{
    css_viewport(width,300);css_apply(root,css,(int)strlen(css));css_extra_apply(root,css,(int)strlen(css));layout_page(root,width);
}
static void done(struct node *root){layout_free();layout_images_reset();dom_free(root);}
static const char ordinary[]="<body><svg width=16 height=16 viewBox='0 0 16 16'><rect width=16 height=16 fill='red'/></svg></body>";
int main(void)
{
    css_init();struct node *r=parse(ordinary);
    paint(r,"",400);int count=decodes;unsigned long long first=pixels(image_at(0));
    CHECK(count==1&&center(image_at(0),255,0,0,255),"cold SVG uses the real decoder and paints red");
    paint(r,"",400);CHECK(decodes==count,"unchanged reflow avoids another decoder call");
    CHECK(pixels(image_at(0))==first,"unchanged reflow preserves every pixel");
    paint(r,"",480);CHECK(decodes==count,"outer viewport reflow reuses identical raster input");
    CHECK(pixels(image_at(0))==first,"viewport-only reflow preserves pixels");
    struct node *extra=dom_create_element(r->doc,"div",3);dom_append_child(find(r,"body"),extra);
    paint(r,"",480);CHECK(decodes==count,"unrelated DOM change reuses unchanged SVG");count=decodes;
    struct node *rect=find(r,"rect"),*svg=find(r,"svg");
    dom_set_attr(rect,"fill","lime");paint(r,"",480);
    CHECK(decodes==count+1&&center(image_at(0),0,255,0,255),"live fill mutation redecodes and changes pixels");count=decodes;
    paint(r,"rect{fill:blue!important}",480);
    CHECK(decodes==count+1&&center(image_at(0),0,0,255,255),"winning CSS paint invalidates serialized input");count=decodes;
    paint(r,"rect{fill:blue!important;fill:not-a-color}",480);
    CHECK(center(image_at(0),0,0,255,255),"invalid later CSS retains valid paint");
    dom_set_attr(rect,"fill","currentColor");paint(r,"body{color:red}",480);
    CHECK(center(image_at(0),255,0,0,255),"ancestor currentColor reaches decoder input");count=decodes;
    paint(r,"body{color:lime}",480);
    CHECK(decodes==count+1&&center(image_at(0),0,255,0,255),"ancestor color mutation invalidates SVG pixels");count=decodes;
    dom_set_attr(svg,"width","20");dom_set_attr(svg,"height","20");paint(r,"body{color:lime}",480);
    CHECK(decodes==count+1&&image_at(0)&&image_at(0)->img->w==20&&image_at(0)->img->h==20,"intrinsic dimensions participate in cache input");count=decodes;
    unsigned long long scaled=pixels(image_at(0));dom_set_attr(svg,"viewBox","0 0 32 32");paint(r,"body{color:lime}",480);
    CHECK(decodes==count+1&&pixels(image_at(0))!=scaled,"viewBox mutation rerenders real pixels");count=decodes;
    dom_set_attr(svg,"opacity",".5");paint(r,"body{color:lime}",480);
    CHECK(decodes==count,"outer SVG opacity does not need another raster");
    CHECK(((struct cstyle *)svg->style)->opacity==128,"root opacity still belongs to outer painter");
    done(r);
    r=parse("<body><svg width=2500 height=2500 viewBox='0 0 16 16'><rect width=16 height=16 fill='red'/></svg></body>");
    count=decodes;paint(r,"svg{width:32px;height:24px}",400);
    CHECK(decodes==count+1&&image_at(0)&&image_at(0)->img->w==32&&image_at(0)->img->h==24,
          "CSS used size controls the cold SVG raster");count=decodes;
    paint(r,"svg{width:32px;height:24px}",400);
    CHECK(decodes==count,"unchanged CSS-sized raster reuses exact pixels");
    count=decodes;
    paint(r,"svg{width:48px;height:36px}",400);
    CHECK(decodes==count+1&&image_at(0)&&image_at(0)->img->w==48&&image_at(0)->img->h==36,
          "CSS size change gets a distinct exact-key raster");
    done(r);
    r=parse(ordinary);count=decodes;paint(r,"",400);
    CHECK(decodes==count+1&&pixels(image_at(0))==first,"new document starts empty and renders the same pixels");
    struct layout_context *child=layout_context_create();CHECK(child!=0,"independent passive document context is available");
    struct layout_context *previous=layout_context_activate(child);
    struct node *cr=parse(ordinary);count=decodes;paint(cr,"",400);
    CHECK(decodes==count+1,"another document has independent raster ownership");count=decodes;
    paint(cr,"",400);CHECK(decodes==count,"passive document also reuses unchanged input");
    CHECK(pixels(image_at(0))==first,"passive cached pixels match original");
    layout_context_activate(previous);count=decodes;paint(r,"",400);
    CHECK(decodes==count,"restoring parent retains its own raster cache");
    layout_context_destroy(child);dom_free(cr);
    CHECK(pixels(image_at(0))==first,"child teardown preserves current parent pixels");done(r);
    r=parse("<body><svg width=16 height=16><g opacity='.5'><rect width=16 height=16 fill='lime'/></g></svg></body>");
    paint(r,"",400);CHECK(center(image_at(0),0,255,0,128),"inner group opacity is rasterized once");count=decodes;
    dom_set_attr(find(r,"g"),"opacity","1");paint(r,"",400);
    CHECK(decodes==count+1&&center(image_at(0),0,255,0,255),"inner opacity mutation invalidates pixels");done(r);
    r=parse("<body><svg width=16 height=16 data-decoder-status=unsupported><rect width=16 height='16'/></svg></body>");
    count=decodes;paint(r,"",400);CHECK(decodes==count+1&&!image_at(0),"ordinary unsupported decoder result remains absent");
    paint(r,"",400);CHECK(decodes==count+2&&!image_at(0),"unsupported result is retried on the next pass");done(r);
    r=parse("<body><svg width=16 height=16><rect width=16 height=16 fill='red'/></svg><svg width=16 height=16><rect width=16 height=16 fill='lime'/></svg><svg width=16 height=16><rect width=16 height=16 fill='blue'/></svg></body>");
    paint(r,"",400);
    CHECK(center(image_at(0),255,0,0,255)&&center(image_at(1),0,255,0,255)&&center(image_at(2),0,0,255,255),"finite distinct icons retain correct simultaneous pixels");
    unsigned long long a=pixels(image_at(0)),b=pixels(image_at(1)),c=pixels(image_at(2));
    paint(r,"",400);CHECK(a==pixels(image_at(0))&&b==pixels(image_at(1))&&c==pixels(image_at(2)),"bounded retention or fallback preserves all three icons");done(r);
    r=parse("<body><svg width=16 height=16><rect width=16 height=16 fill='red'/></svg><svg width=16 height=16><rect width=16 height=16 fill='red'/></svg></body>");
    count=decodes;paint(r,"",400);
    CHECK(decodes==count+1,"identical separate SVG nodes share exact serialized pixels");
    CHECK(image_at(0)&&image_at(1)&&pixels(image_at(0))==pixels(image_at(1)),"simultaneous aliases retain matching real pixels");
    done(r);r=parse(ordinary);paint(r,"",400);
    CHECK(center(image_at(0),255,0,0,255),"normal teardown and next document render remain valid");done(r);
    printf("svg-reflow-cache: %d checks, %d failures; decoder calls=%d\n",checks,fails,decodes);return fails?1:0;
}
