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




/* Inspect the real display-list text and boxes, not the cstyle presence. The
 * guest fixture uses these same literals so a screenshot can settle painting. */
static const struct item *ink(const char *text) {
    const struct item *v = layout_items();
    for (int i=0;i<layout_count();i++)
        if (v[i].type==IT_TEXT && v[i].len==(int)strlen(text) && !memcmp(v[i].text,text,v[i].len)) return &v[i];
    return NULL;
}
static void drop(void) { layout_free(); dom_free(g_root); g_root=NULL; }
static int count_dom(struct node *n) {
    int total=1; for(struct node *c=n->first_child;c;c=c->next) total+=count_dom(c); return total;
}
int main(void) {
    page("<!doctype html><style>body{margin:0;font-size:16px}#x:before{content:'BEFORE';color:#123456;font-size:20px;font-weight:bold}#x::after{content:attr(data-tail);color:#654321}</style><div id=x data-tail=AFTER>AUTHORED</div>",600);
    const struct item *b=ink("BEFORE"), *a=ink("AFTER"), *t=ink("AUTHORED");
    CHECK(b && a,"generated text reaches production display list");
    CHECK(b && a && t && b->x<t->x && t->x<a->x,"before authored after share ordered inline flow");
    CHECK(b && b->color==0x123456 && b->font_px==20 && b->bold,"pseudo own color font and weight reach text items");
    CHECK(a && a->color==0x654321 && a->font_px==16,"pseudo inherits owner font without leaking sibling style");
    CHECK(b && b->node==ID("x") && b->pseudo==1 && b->generated_style,"generated hit provenance remains real owner");
    CHECK(ID("x")->first_child==ID("x")->last_child && ID("x")->first_child->type==N_TEXT,"generated content never enters authored child links");
    int old=count_dom(g_root); layout_page(g_root,600);
    EQ(count_dom(g_root),old,"repeated layout leaves DOM unchanged");
    dom_set_attr(ID("x"),"data-tail","CHANGED");
    const char *css="#x::before{content:'BEFORE'}#x::after{content:attr(data-tail)}";
    int changed=css_apply_scoped(ID("x"),0,css,(int)strlen(css));
    CHECK(changed & CSS_CHANGED_LAYOUT,"attr content restyle requires fresh layout even if allocator reuses address");
    layout_page(g_root,600); CHECK(ink("CHANGED") && !ink("AFTER"),"attr mutation updates generated text"); drop();

    page("<!doctype html><style>#x::before{content:'LOST'}#x::before{content:none}#x::after{content:normal}#y::before{content:attr(missing)}#z::before{content:'PARTIAL' counter(n)}</style><div id=x>REAL</div><div id=y></div><div id=z></div>",600);
    CHECK(!ink("LOST") && !ink("PARTIAL"),"none normal and unsupported whole content never fabricate partial ink");
    CHECK(!((struct cstyle*)ID("x")->style)->generated[0] && !((struct cstyle*)ID("x")->style)->generated[1],"none and normal suppress pseudo boxes");
    CHECK(((struct cstyle*)ID("y")->style)->generated[0]!=NULL,"missing attr is empty content not absent pseudo"); drop();

    page("<!doctype html><style>body{margin:0}#x::before{content:'';display:block;width:80px;height:12px;background-color:#dd2211}#x::after{content:'BLOCK';display:block;height:24px}#a::before{content:'A';display:inline-block;width:40px;height:20px;padding:2px;border:1px solid red}</style><div id=x><div id=child style='height:10px'></div></div><span id=a></span><span>END</span>",600);
    const struct item *v=layout_items(); int saw=0;
    for(int i=0;i<layout_count();i++) if(v[i].type==IT_RECT && v[i].pseudo==1 && v[i].node==ID("x")) saw=v[i].w==80 && v[i].h==12 && v[i].bg==0xdd2211;
    CHECK(saw,"empty generated block paints specified background geometry");
    int x,y,w,hh;layout_node_box(ID("child"),&x,&y,&w,&hh); EQ(y,12,"generated block participates before real block child");
    layout_node_box(ID("x"),&x,&y,&w,&hh); EQ(hh,46,"generated before and after contribute block height");
    b=ink("A");a=ink("END");CHECK(b && a && a->x>=46,"generated atomic box contributes padding border and width");drop();

    page("<!doctype html><style>body{margin:0}#x{display:flex}#x::before{content:'FLEX';display:block;width:40px}#x::after{content:'TAIL';display:block;width:40px}</style><div id=x><div id=middle style='width:32px'>MID</div></div>",600);
    b=ink("FLEX");a=ink("TAIL");t=ink("MID");
    printf("flex positions: before=%d middle=%d after=%d\n",b?b->x:-1,t?t->x:-1,a?a->x:-1);
    CHECK(b && a && t && b->x==0 && t->x==40 && a->x==72,"generated flex items share real sizing and placement");drop();
    page("<!doctype html><style>body{margin:0}#x{display:flex}#x span{width:70px}</style><div id=x><span>FIRST</span><span>SECOND</span></div>",600);
    b=ink("FIRST");a=ink("SECOND");
    CHECK(b && a && b->x==0 && a->x==70,"ordinary inline flex children remain distinct sized items");drop();

    page("<!doctype html><style>body{margin:0}#x{display:grid;grid-template-columns:50px 60px 70px}#x::before{content:'GRID'}#x::after{content:'LAST'}</style><div id=x><span>MID</span></div>",600);
    b=ink("GRID");a=ink("LAST");t=ink("MID");
    CHECK(b && a && t && b->x==0 && t->x==50 && a->x==110,"generated grid items use owner track solver");drop();

    page("<!doctype html><style>body{margin:0}#x::before{content:'ESC\\41' 'PED' attr(data-v);display:block;font-size:20px;line-height:130%}#x::after{content:'HIDDEN';display:none}</style><div id=x data-v='!'></div>",600);
    CHECK(ink("ESCAPED!") && !ink("HIDDEN"),"CSS escapes concatenated strings attr and display none reach real text flow");
    layout_node_box(ID("x"),&x,&y,&w,&hh); EQ(hh,26,"pseudo own percentage line height resolves against its font");
    for (int i=0;i<32;i++) {
        const char *value=i%2?"SHORT":"A_LONGER_ATTRIBUTE_VALUE";
        dom_set_attr(ID("x"),"data-v",value);
        css="#x::before{content:attr(data-v)}";
        css_apply_scoped(ID("x"),0,css,(int)strlen(css));layout_page(g_root,600);
        CHECK(ink(value)!=NULL,"repeated generated allocation replacement retains current text");
    }
    css="#x::before{content:none}";
    css_apply_scoped(ID("x"),0,css,(int)strlen(css));layout_page(g_root,600);
    CHECK(!ink("SHORT"),"removing generated content discards old items");drop();
    printf("generated content: %d checks, %d failed\n",checks,fails);return fails?1:0;
}
