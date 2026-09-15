/* Parsed CSS through actual DOM/layout, with fixed child boxes so font
 * metrics do not decide the flex sizing oracle. inline-flex must be atomic in
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
static void fixture(const char *extra, int width){
    char html[4096];
    snprintf(html,sizeof html,"<!doctype html><style>body{margin:0;font-size:16px;line-height:16px}#row{display:inline-flex}#a{width:20px;height:20px;flex:none}#b{width:30px;height:20px;flex:none}%s</style><span id=before>X</span><div id=row><div id=a></div><div id=b></div></div><span id=after>Y</span>",extra);
    page(html,width);
}
static void done(void){dom_free(g_root);g_root=NULL;}
int main(void){
    fixture("",200);
    EQ(Y("row"),Y("before"),"inline flex stays beside preceding text");
    EQ(Y("after"),Y("row"),"following text stays beside inline flex");
    EQ(X("row"),X("before")+W("before"),"atomic inline starts at current pen");
    EQ(W("row"),50,"auto inline flex shrink wraps children");
    EQ(X("b")-X("a"),20,"existing flex row positions retained");
    EQ(X("after"),X("row")+W("row"),"following text advances past whole flex box");
    done();
    fixture("#row{width:100px;justify-content:space-between}",200);
    EQ(W("row"),100,"specified inline flex width retained");
    EQ(X("b")-X("a"),70,"internal flex justification retained");
    EQ(Y("after"),Y("before"),"fixed width flex remains inline");
    done();
    fixture("",52);
    CHECK(Y("row")>Y("before"),"atomic flex wraps as a whole");
    EQ(X("row"),0,"wrapped flex starts at line edge");
    EQ(W("row"),50,"wrapping does not fill containing line");
    CHECK(ink_y("after")>Y("row"),"following text wraps after full atomic box");
    done();
    fixture("#row{flex-direction:column}",200);
    EQ(W("row"),30,"inline column uses widest child");
    EQ(H("row"),40,"inline column keeps internal stacking");
    EQ(Y("b")-Y("a"),20,"column children still stack");
    EQ(Y("after"),Y("before"),"inline column stays in parent line");
    done();
    fixture("#row{gap:7px}",200);
    EQ(W("row"),57,"inline intrinsic width includes flex gap");
    EQ(X("b")-X("a"),27,"gap stays inside flex layout");
    done();
    fixture("#row{margin:2px 4px 3px 3px}",200);
    EQ(X("row"),X("before")+W("before")+3,"inline atomic left margin takes space");
    EQ(X("after"),X("row")+W("row")+4,"inline atomic right margin takes space");
    done();
    fixture("#row{padding:2px 3px;border:1px solid}",200);
    EQ(W("row"),58,"padding and border counted once");
    EQ(X("a"),X("row")+4,"flex content starts after padding and border");
    done();
    fixture("#row{gap:7px}#a{display:none}",200);
    EQ(W("row"),30,"hidden child creates no phantom gap");
    done();
    fixture("#row{gap:7px}#a{width:0}",200);
    EQ(W("row"),37,"zero width child still creates a gap");
    done();
    fixture("body{white-space:nowrap}",52);
    EQ(Y("row"),Y("before"),"nowrap keeps atomic flex on current line");
    EQ(ink_y("after"),ink_y("before"),"nowrap keeps following text on line");
    done();
    fixture("#a,#b{display:none}",200);
    EQ(W("row"),0,"empty inline flex has zero content width");
    EQ(H("row"),0,"empty inline flex does not invent font height");
    done();
    fixture("body{display:flex}#row{flex-grow:1;order:1}",200);
    /* Anonymous text-item minimum sizes belong to the existing flex engine;
     * this guard verifies that the nested flex retains its grow/order inputs
     * rather than being swallowed into an anonymous inline run. */
    CHECK(W("row")>50,"nested inline flex remains a real growing flex item");
    CHECK(X("row")>X("after"),"nested inline flex keeps item order");
    done();
    fixture("#row{display:flex}",200);
    EQ(W("row"),200,"ordinary flex remains block width");
    CHECK(Y("row")>Y("before") && Y("after")>Y("row"),"ordinary flex still separates lines");
    done();
    fixture("#row{float:left}",200);
    EQ(W("row"),50,"floated inline flex still shrink wraps");
    done();
    fixture("#row{position:absolute;left:70px;top:30px;width:60px}",200);
    EQ(X("row"),70,"absolute inline flex still blockifies");
    EQ(Y("row"),30,"absolute flex keeps positioned top");
    done();
    printf("inline-flex: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
