/* The actual DOM/CSS/layout producers, with a deterministic tiny image codec.
 * Geometry/cache ownership is measured here; guest pixels are the embedder's
 * integration gate. The negative builds restore each old singleton separately. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"
void layout_images_reset(void);

void *kmalloc(unsigned long n){return malloc(n);}
void kfree(void *p){free(p);}
int text_measure(const char *s,int len,int px,int face){
    (void)face;int n=0;for(int i=0;i<len;i++)if(((unsigned char)s[i]&192)!=128)n++;
    return n*(px/2);
}
int res_fetch(const char *u,uint8_t **b,int *l){(void)u;(void)b;(void)l;return -1;}
static int image_live, image_decodes, anim_notes;
int img_decode(const uint8_t *p,int n,struct image *o){
    int w=3,h=2,c=91;
    if(n==3){w=p[0];h=p[1];c=p[2];}
    else if(n==1&&p[0]==255){w=2048;h=1024;}
    else if(n<4||memcmp(p,"<svg",4))return -1;
    o->w=w;o->h=h;o->rgba=malloc((size_t)w*h*4);if(!o->rgba)return -1;
    memset(o->rgba,c,(size_t)w*h*4);image_live++;image_decodes++;return 0;
}
void img_free(struct image *o){if(o->rgba){free(o->rgba);o->rgba=0;image_live--;}}
void css_anim_note(struct node *root){(void)root;anim_notes++;}
static struct node *parent_modal;
int top_layer_count(void){return parent_modal?1:0;}
struct node *top_layer_at(int i){return i==0?parent_modal:0;}
int top_layer_is_modal(const struct node *n){return n==parent_modal;}
int top_layer_contains(const struct node *n){return n==parent_modal;}
int top_layer_is_hidden_popover(const struct node *n){(void)n;return 0;}

static int checks,fails;
#define CHECK(c,m) do{checks++;if(!(c)){printf("FAIL: %s\n",m);fails++;}}while(0)
static struct node *id(struct node *n,const char *name){
    const char *a=n->type==N_ELEM?dom_attr(n,"id"):0;if(a&&!strcmp(a,name))return n;
    for(struct node *c=n->first_child;c;c=c->next){struct node *r=id(c,name);if(r)return r;}return 0;
}
struct page {struct node *root,*box;char expanded[4096];int len,w,h,color;};
static void page_init(struct page *p,const char *label,int w,int h,int color){
    char html[1024],raw[2048];
    snprintf(html,sizeof html,"<!doctype html><body><div id=box>%s</div><img src=icon.img><svg width=3 height=2><rect width=3 height=2/></svg><dialog id=modal>top</dialog>",label);
    p->root=dom_parse(html,(int)strlen(html));p->box=id(p->root,"box");p->w=w;p->h=h;p->color=color;
    snprintf(raw,sizeof raw,":root{--owner:%s}body{margin:0;background:#%06x}#box{display:grid;grid-template-columns:17px 31px;height:%dpx;text-transform:uppercase;animation:%s 2s}dialog{display:none}@keyframes %s{from{opacity:0}to{opacity:1}}",label,color,h,label,label);
    css_viewport(w,h+20);p->len=css_expand_vars(raw,(int)strlen(raw),p->expanded,sizeof p->expanded);
    css_apply(p->root,p->expanded,p->len);css_extra_apply(p->root,p->expanded,p->len);
    uint8_t pixels[3]={4,3,(uint8_t)color};CHECK(layout_img_store("icon.img",pixels,3),"document accepts its decoded image");
    layout_page(p->root,w);
}
static int image_color(void){
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++)if(it[i].type==IT_IMAGE&&it[i].imgsrc&&it[i].img)return it[i].img->rgba[0];
    return -1;
}
static int has_text(struct node *box,const char *owner){
    char expected[64],actual[64];int n=(int)strlen(owner),used=0;
    for(int i=0;i<n;i++)expected[i]=owner[i]>='a'&&owner[i]<='z'?owner[i]-32:owner[i];
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++)if(it[i].type==IT_TEXT){
        struct node *a=it[i].node;while(a&&a!=box)a=a->parent;
        if(a&&used+it[i].len<=(int)sizeof actual){memcpy(actual+used,it[i].text,(size_t)it[i].len);used+=it[i].len;}
    }
    /* A 17px grid track intentionally wraps this word into several runs;
     * concatenate its owning node's runs rather than assuming a single item. */
    return used==n&&!memcmp(actual,expected,(size_t)n);
}

static void page_assert(struct page *p,const char *owner,const char *marker){
    int x,y,w,h;uint32_t bg=0;
    CHECK(layout_node_box(p->box,&x,&y,&w,&h),marker);
    CHECK(w==p->w&&h==p->h,"restored box geometry belongs to active document");
    CHECK(layout_page_bg(&bg)&&bg==(unsigned)p->color,"restored background belongs to active document");
    CHECK(css_media_width()==p->w&&css_media_height()==p->h+20,"CSS viewport restored with layout context");
    const char *v=css_vars_value("--owner");CHECK(v&&!strcmp(v,owner),"custom-property arena restored");
    const struct css_kf *kf=0;int same_sheet=css_keyframes_find(owner,(int)strlen(owner),&kf);
    CHECK(same_sheet,"extension keyframes cache restored");
    /* Only dereference the borrowed raw span when its owning sheet is live.
     * The old singleton control deliberately drops that sheet; it must fail
     * the ownership check above instead of turning the control into a UAF. */
    if(same_sheet){
        struct cstyle *st=p->box->style;
        CHECK(st->grid_raw[GR_TEMPL_COLS]&&st->grid_rawlen[GR_TEMPL_COLS]==9&&!memcmp(st->grid_raw[GR_TEMPL_COLS],"17px 31px",9),"borrowed grid span retains its document backing");
    }
    CHECK(has_text(p->box,owner),"processed uppercase text survives context restoration");
    CHECK(image_color()==(p->color&255),"same relative src retains document-specific pixels");
    CHECK(layout_height()>=p->h,"document height retained");
}
int main(void){
    struct page p,a,b;css_set_screen(1920,1080);
    page_init(&p,"parent",640,51,0x112233);
    parent_modal=id(p.root,"modal");
    /* Live dialog data must not be appended to a different document's list. */
    ((struct cstyle *)parent_modal->style)->display=DISP_BLOCK;layout_page(p.root,p.w);
    int parent_count=layout_count(),parent_height=layout_height();
    const struct item *parent_items=layout_items();
    int parent_parses=css_sheet_parses(),parent_compiles=css_extra_compiles();
    int parent_anim_notes=anim_notes;
    struct layout_context *ca=layout_context_create(),*cb=layout_context_create();
    CHECK(ca&&cb,"allocate two independent passive contexts");if(!ca||!cb)return 2;
    CHECK(layout_context_activate(ca)==0,"enter child from default context");
    page_init(&a,"first",240,73,0x445566);
    CHECK(anim_notes==parent_anim_notes,"passive cascade leaves parent animation registry alone");
    CHECK(!layout_node_box(p.box,0,0,0,0),"child list excludes parent boxes");
    CHECK(!layout_node_box(parent_modal,0,0,0,0),"child list excludes parent top-layer dialog");
    int aw,ah;CHECK(layout_img_dimensions("icon.img",&aw,&ah)==1&&aw==4&&ah==3,"child image cache is queryable");
    CHECK(layout_context_activate(cb)==ca,"switch sibling contexts returns previous child");
    page_init(&b,"second",180,95,0x778899);
    CHECK(layout_context_activate(ca)==cb,"return to first child");
    page_assert(&a,"first","first child box table survives sibling layout");
    CHECK(!layout_node_box(b.box,0,0,0,0),"sibling geometry is isolated");
    CHECK(layout_context_activate(0)==ca,"restore default context");
    CHECK(layout_items()==parent_items&&layout_count()==parent_count&&layout_height()==parent_height,"parent display list survives child layout");
    page_assert(&p,"parent","parent box table restored after children");
    CHECK(css_sheet_parses()==parent_parses&&css_extra_compiles()==parent_compiles,"parent style cache counters not replaced");
    css_apply(p.root,p.expanded,p.len);css_extra_apply(p.root,p.expanded,p.len);
    CHECK(css_sheet_parses()==parent_parses&&css_extra_compiles()==parent_compiles,"parent unchanged stylesheet reuses its own parses");
    /* A changed parent sheet must not free a child's raw grid/keyframe spans. */
    strcpy(p.expanded+p.len,"#box{color:#123456}");p.len+=(int)strlen("#box{color:#123456}");
    css_apply(p.root,p.expanded,p.len);css_extra_apply(p.root,p.expanded,p.len);
    layout_page(p.root,p.w);
    layout_context_activate(ca);
    page_assert(&a,"first","child survives parent stylesheet replacement");
    int decodes=image_decodes;
    css_viewport(120,160);css_apply(a.root,a.expanded,a.len);css_extra_apply(a.root,a.expanded,a.len);layout_page(a.root,120);
    int x,y,w,h;CHECK(layout_node_box(a.box,&x,&y,&w,&h)&&w==120,"child resize reflows its own document");
    CHECK(image_color()==0x66,"child resize retains image pixels");
    /* Previously one inline SVG decode was mandatory on every resize. Exact
     * serialized-input retention now keeps that raster across this reflow. */
    CHECK(image_decodes==decodes,"child resize reuses its unchanged inline SVG");
    uint8_t big=255;CHECK(!layout_img_store("too-large.img",&big,1),"passive decoded-image ownership budget refuses excess");
    CHECK(layout_img_dimensions("too-large.img",0,0)==-1,"refused image records failure without repeated decode");
    layout_context_activate(0);
    CHECK(layout_node_box(p.box,&x,&y,&w,&h)&&w==640,"child resize leaves parent geometry intact");
    CHECK(image_color()==0x33,"child resize leaves parent image cache intact");
    layout_context_destroy(ca);dom_free(a.root);
    CHECK(image_color()==0x33,"child destruction preserves parent image ownership");
    layout_context_activate(cb);page_assert(&b,"second","remaining sibling survives child destruction");
    layout_context_destroy(cb);dom_free(b.root);
    CHECK(layout_node_box(p.box,0,0,0,0),"destroying active child restores default context");
    CHECK(image_live==2,"only parent URL image and SVG remain after both children die");
    parent_modal=0;layout_free();layout_images_reset();dom_free(p.root);
    CHECK(image_live==0,"all document image owners release exactly once");
    printf("passive-layout-context: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
