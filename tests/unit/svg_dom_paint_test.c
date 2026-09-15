/* Live DOM -> shipping CSS cascade -> layout -> real SVG pixels. Source spans
 * and display-list existence are insufficient: the old pipeline could emit a
 * perfectly valid bitmap containing yesterday's fill colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layout.h"
#include "css.h"
#include "dom.h"
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
int text_measure(const char *s,int n,int px,int face) { (void)s;(void)face;return n*px/2; }
int res_fetch(const char *s,unsigned char **p,int *n) { (void)s;(void)p;(void)n;return -1; }
static int checks, fails;
#define CHECK(c,m) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n",m); } } while(0)
static struct node *find(struct node *n,const char *name) {
    if(n->type==N_ELEM&&!strcmp(n->tag,name))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *r=find(c,name);if(r)return r;}return 0;
}
static struct node *parse(const char *s) { return dom_parse(s,(int)strlen(s)); }
static const struct item *paint(struct node *root,const char *css) {
    css_apply(root,css,css?(int)strlen(css):0);
    css_extra_apply(root,css,css?(int)strlen(css):0);
    layout_page(root,400);
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++)if(it[i].type==IT_IMAGE&&it[i].img)return &it[i];
    return 0;
}
static void color(const struct item *it,int r,int g,int b,const char *msg) {
    int pixels=0, wrong=0;
    if(it) for(int i=0;i<it->img->w*it->img->h;i++){
        const unsigned char *p=it->img->rgba+4*i;
        if(p[3]){pixels++;if(abs(p[0]-r)>1||abs(p[1]-g)>1||abs(p[2]-b)>1)wrong++;}
    }
    CHECK(pixels==256&&!wrong,msg);
}
static void finish(struct node *r) { layout_free(); dom_free(r); }
static const char red[]="<body><svg width='24' height='24' viewBox='0 0 24 24'><rect width='16' height='16' fill='red'/></svg></body>";
static void css_case(const char *html,const char *css,const char *msg) {
    struct node *r=parse(html);color(paint(r,css),0,255,0,msg);finish(r);
}
int main(void) {
    css_init(); struct node *r=parse(red);
    color(paint(r,0),255,0,0,"static SVG remains a positive control");
    dom_set_attr(find(r,"rect"),"fill","lime");
    color(paint(r,0),0,255,0,"DOM mutation changes real pixels on the next layout");
    dom_set_attr(find(r,"rect"),"fill","blue");
    color(paint(r,0),0,0,255,"second mutation releases the previous raster");
    finish(r);

    r=parse("<body></body>");struct node *svg=dom_create_element_ns(r->doc,"svg",3,NS_SVG);
    struct node *rect=dom_create_element_ns(r->doc,"rect",4,NS_SVG);
    dom_set_attr(svg,"width","24");dom_set_attr(svg,"height","24");dom_set_attr(svg,"viewBox","0 0 24 24");
    dom_set_attr(rect,"width","16");dom_set_attr(rect,"height","16");dom_set_attr(rect,"fill","lime");
    dom_append_child(svg,rect);dom_append_child(find(r,"body"),svg);
    CHECK(!svg->rawlen,"dynamic fixture has no parser source span");
    color(paint(r,0),0,255,0,"createElementNS subtree renders real pixels");finish(r);

    css_case(red,"svg rect {fill:lime}","author CSS beats presentation fill");
    css_case(red,"svg rect {fill:lime} rect {fill:red}","selector specificity chooses paint");
    css_case(red,"rect {fill:red;fill:lime}","last valid declaration chooses paint");
    css_case(red,"rect {fill:lime;fill:not-a-color}","invalid later paint preserves valid declaration");
    css_case("<body><svg width='24' height='24'><rect width='16' height='16' style='fill:red'/></svg>","rect{fill:lime!important}","author important beats inline normal");
    css_case("<body><svg width='24' height='24'><rect width='16' height='16' style='fill:lime!important'/></svg>","rect{fill:red!important}","inline important wins over author important");
    css_case("<body><svg width='24' height='24'><rect width='16' height='16'/></svg>","body {fill:lime}","HTML paint inheritance reaches SVG root");
    css_case("<body><svg width='24' height='24' fill='lime'><g fill='inherit'><rect width='16' height='16'/></g></svg>",0,"presentation paint inherits through groups");
    css_case("<body><svg width='24' height='24' color='lime' fill='currentColor'><rect width='16' height='16'/></svg>",0,"presentation color participates in computed currentColor");
    css_case("<body><svg width='24' height='24' fill='currentColor'><rect width='16' height='16'/></svg>","body{color:lime}","HTML currentColor inheritance reaches SVG");
    css_case("<body><svg width='24' height='24' color='red' fill='currentColor'><rect width='16' height='16'/></svg>","svg{color:lime}","CSS color overrides root presentation color");
    css_case("<body><svg width='24' height='24' fill='currentColor'><g color='lime'><rect width='16' height='16'/></g></svg>",0,"nested color presentation hint inherits to shape");
    css_case("<body><svg width='24' height='24'><defs><rect id='r' width='16' height='16' fill='currentColor'/></defs><use href='#r' color='lime'/></svg>",0,"use instance inherits its own currentColor");
    css_case("<body><svg width='24' height='24'><defs><rect id='a&amp;b' width='16' height='16' fill='lime'/></defs><use href='#a&amp;b'/></svg>",0,"live serialization preserves escaped local references");
    r=parse(red);color(paint(r,"rect{fill:lime}"),0,255,0,"first stylesheet affects raster");
    color(paint(r,"rect{fill:blue}"),0,0,255,"new stylesheet invalidates raster");
    color(paint(r,0),255,0,0,"removed stylesheet restores presentation attribute");finish(r);

    r=parse("<body><svg width='24' viewBox='0 0 24 12'><rect width='24' height='12'/></svg>");
    const struct item *it=paint(r,0); CHECK(it&&it->w==24&&it->h==12,"initial viewBox controls missing axis");
    dom_set_attr(find(r,"svg"),"viewBox","0 0 24 48");it=paint(r,0);
    CHECK(it&&it->w==24&&it->h==48,"mutated viewBox controls missing axis");finish(r);

    r=parse("<body><svg width='24' height='24' opacity='.5'><rect width='16' height='16' fill='lime'/></svg>");it=paint(r,0);
    CHECK(it&&it->img->rgba[3]==255,"root group opacity is not multiplied into raster twice");
    CHECK(find(r,"svg")->style&&((struct cstyle*)find(r,"svg")->style)->opacity==128,"root opacity presentation reaches outer painter style");finish(r);
    r=parse("<body><svg width='24' height='24'><g style='opacity:.5'><rect width='16' height='16' fill='lime'/></g></svg>");it=paint(r,0);
    CHECK(it&&it->img->rgba[3]>=127&&it->img->rgba[3]<=128,"child group opacity is rasterized once");finish(r);
    r=parse("<body><svg width='24' height='24'><rect width='16' height='16' style='display:none'/></svg>");it=paint(r,0);
    CHECK(it&&it->img->rgba[3]==0,"computed display none suppresses SVG shape");finish(r);
    printf("svg DOM paint: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
