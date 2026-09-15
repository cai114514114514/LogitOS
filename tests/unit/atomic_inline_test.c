/* Parsed CSS through actual DOM/layout, with fixed child boxes so font
 * metrics do not decide the sizing oracle. Each outer inline box is atomic in
 * its parent's line while retaining flex layout inside. Host geometry is not
 * a claim about guest pixels or complete inline-baseline conformance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
/* A monospace approximation, exactly like every other host layout harness:
 * these assertions are about BOX GEOMETRY, never about glyph advances. */
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)
#define EQ(got,want,m) do{ checks++; if((got)!=(want)){ \
        printf("  FAIL: %s -- got %d, want %d\n",(m),(int)(got),(int)(want)); fails++; } }while(0)

static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max){
    if(!n) return o;
    if(n->type==N_ELEM && tag_is(n->tag,"style"))
        for(struct node *c=n->first_child;c;c=c->next)
            if(c->type==N_TEXT && c->text)
                for(int i=0;i<c->textlen && o<max-1;i++) out[o++]=c->text[i];
    for(struct node *c=n->first_child;c;c=c->next) o=collect_style(c,out,o,max);
    return o;
}

static struct node *g_root;
static struct node *find_id(struct node *n, const char *id){
    if(n->type==N_ELEM){ const char *a=dom_attr(n,"id"); if(a && !strcmp(a,id)) return n; }
    for(struct node *c=n->first_child;c;c=c->next){ struct node *r=find_id(c,id); if(r) return r; }
    return 0;
}
static struct node *ID(const char *id){
    struct node *e = find_id(g_root, id);
    if(!e){ printf("  FAIL: no element #%s in the document\n", id); fails++; checks++; }
    return e;
}

/* Lay the page out at `cw` and leave it current for the queries below. */
static void page(const char *html, int cw){
    static char css[16384], exp[32768];
    g_root = dom_parse(html, (int)strlen(html));
    int cl = collect_style(g_root, css, 0, (int)sizeof css);
    /* THE BROWSER'S ORDER, not a shorter one. browser.c runs css_expand_vars
     * over the collected sheet and hands the EXPANDED text to css_apply; this
     * helper used to skip that step, so every `var()` in a test reached LibCSS
     * unsubstituted and resolved to nothing.
     *
     * That is not a harmless simplification -- it is a harness that disagrees
     * with the thing it is testing, and it produced a false finding the day it
     * was noticed: `padding-top: var(--cover-radio)` came out 0 here and was
     * about to be reported as a browser bug. bilibili writes exactly that
     * declaration. */
    int el = css_expand_vars(css, cl, exp, (int)sizeof exp);
    css_apply(g_root, exp, el);
    css_extra_apply(g_root, exp, el); /* browser.c:2040; grid/logical producers */
    layout_page(g_root, cw);
}




static int X(const char *id){ int x=0,y,w,h; layout_node_box(ID(id),&x,&y,&w,&h);return x; }
static int Y(const char *id){ int x,y=0,w,h; layout_node_box(ID(id),&x,&y,&w,&h);return y; }
static int W(const char *id){ int x,y,w=0,h; layout_node_box(ID(id),&x,&y,&w,&h);return w; }
static int H(const char *id){ int x,y,w,h=0; layout_node_box(ID(id),&x,&y,&w,&h);return h; }
/* An inline span opened before a wrap can have a union rectangle starting
 * on the previous line. The wrapping oracle must inspect its actual painted
 * text item, not mistake that rectangle's top for the glyph's line. */
static int ink_y(const char *id){
    struct node *n=ID(id); const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++) if(it[i].type==IT_TEXT)
        for(struct node *p=it[i].node;p;p=p->parent) if(p==n) return it[i].y;
    return -1;
}

static void page_type(const char *kind,const char *more,int width){
 char h[4096];snprintf(h,sizeof h,"<!doctype html><style>body{margin:0;font-size:16px;line-height:16px}#box{display:%s}#a{width:20px;height:20px;flex:none}#b{width:30px;height:20px;flex:none}%s</style><span id=before>X</span><div id=box><div id=a></div><div id=b></div></div><span id=after>Y</span>",kind,more);page(h,width);
}
static void done(void){dom_free(g_root);g_root=NULL;}
int main(void){
 const char *types[]={"inline-block","inline-flex","inline-grid"};
 for(int k=0;k<3;k++){
  const char *extra=k==2?"#box{grid-template-columns:20px 30px;gap:7px}":"";
  int want=k==0?30:k==1?50:57;
  page_type(types[k],extra,200);
  printf("atomic %s: x=%d y=%d width=%d height=%d\n",types[k],X("box"),Y("box"),W("box"),H("box"));
  EQ(Y("box"),Y("before"),"atomic box remains on preceding text line");
  EQ(X("box"),8,"atomic box starts at current pen");
  EQ(W("box"),want,"atomic auto width uses inner formatting context");
  EQ(X("after"),X("box")+W("box"),"following text advances past atomic box");
  if(k==0){EQ(Y("b")-Y("a"),20,"inline block retains block children");EQ(H("box"),40,"inline block contains child stack");}
  else {EQ(Y("a"),Y("b"),"flex/grid keeps its internal row");EQ(X("b")-X("a"),k==2?27:20,"flex/grid keeps internal spacing");}
  done();
  char more[256];snprintf(more,sizeof more,"%s#box{width:80px;padding:2px 3px;border:1px solid}",extra);
  page_type(types[k],more,200);
  EQ(W("box"),88,"shared specified size includes decorations once");
  EQ(X("a"),X("box")+4,"shared content origin includes border and padding");done();
  page_type(types[k],extra,want+4);
  CHECK(Y("box")>Y("before"),"atomic box wraps as one unit");EQ(X("box"),0,"wrapped atomic box starts at line edge");done();
  snprintf(more,sizeof more,"%sbody{display:flex}#box{flex-grow:1;order:1}",extra);
  page_type(types[k],more,200);CHECK(W("box")>want,"atomic child remains a growing flex item");CHECK(X("box")>X("after"),"atomic child retains flex order");done();
 }
 page("<!doctype html><style>body{margin:0;font-size:16px;line-height:16px}.box{display:inline-block;width:20px;height:20px}</style><span id=a class=box></span><span id=b class=box></span>",100);
 EQ(X("b")-X("a"),20,"adjacent inline blocks share a line");EQ(Y("a"),Y("b"),"adjacent inline block y equal");done();
 page_type("inline-grid","#box{grid-template-columns:auto auto;gap:7px}",200);
 EQ(W("box"),57,"auto grid tracks contribute max-content width");
 int old_count=layout_count(), old_width=W("box"), old_x=X("b");
 layout_page(g_root,200);
 EQ(layout_count(),old_count,"intrinsic trials leave no paint items behind");
 EQ(W("box"),old_width,"repeated layout preserves atomic intrinsic width");
 EQ(X("b"),old_x,"repeated layout preserves grid child position");done();
 page("<!doctype html><style>body{margin:0;font-size:16px;line-height:16px}#box{display:inline-grid;grid-template-columns:auto auto;gap:4px}#nested{display:inline-grid;grid-template-columns:10px 20px;gap:3px}.leaf{height:10px}#tail{width:15px;height:10px}</style><span id=before>X</span><div id=box><div id=nested><div class=leaf></div><div class=leaf></div></div><div id=tail></div></div>",200);
 EQ(W("nested"),33,"nested grid retains its intrinsic track size");
 EQ(W("box"),52,"nested grid contributes to outer intrinsic tracks");
 EQ(X("tail")-X("nested"),37,"nested grid placement includes parent gap");done();
 printf("atomic-inline: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
