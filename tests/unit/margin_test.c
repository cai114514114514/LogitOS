/* Real DOM -> LibCSS -> layout geometry, not a hand-built cstyle. Fixed-size
 * empty boxes keep font metrics out of the oracle. The negative-control build
 * restores the old unit/sentinel conversion and must visibly fail this suite.
 * These are host geometry assertions; guest pixels/input remain a separate gate. */
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

static void box(const char *id, int ex, int ey, int ew, int eh) {
    int x=0,y=0,w=0,h=0; CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"box exists");
    EQ(x,ex,id); EQ(y,ey,id); EQ(w,ew,id); EQ(h,eh,id);
}
static void child(const char *container,const char *margin,int cw,int x,int y,int w) {
    char html[1800]; snprintf(html,sizeof html,
        "<!doctype html><style>body{margin:0}#p{width:%dpx;border:1px solid;%s}"
        "#x{width:%dpx;height:20px;%s}</style><div id=p><div id=x></div></div>",
        cw,container,w,margin);
    page(html,1000); box("x",x,y,w,20);
}
int main(void) {
    child("","margin:10%",400,41,41,100);
    child("","margin:10%",800,81,81,100);
    child("","margin:56.25%",400,226,226,100);
    child("","margin:-1%",400,-3,-3,100);
    child("","margin:-1px",400,0,0,100);
    child("","margin-left:-10px;margin-right:-10px",400,-9,1,100);
    child("","margin-left:auto;margin-right:auto",400,151,1,100);
    child("","margin-left:auto;margin-right:10px",400,291,1,100);
    child("","margin-left:10px;margin-right:auto",400,11,1,100);
    child("display:flex","margin:10%;flex-shrink:0",400,41,41,100);
    child("display:flex","margin-left:-1px;flex-shrink:0",400,0,1,100);
    child("display:flex;flex-direction:column","margin:10%;flex-shrink:0",400,41,41,100);
    child("display:flex","margin-left:auto;flex-shrink:0",400,301,1,100);
    child("display:flex;flex-direction:column","margin-left:auto",400,301,1,100);
    child("display:flex;flex-direction:column;height:200px","margin-top:auto",400,1,181,100);
    child("display:grid;grid-template-columns:200px 200px","margin:10%",400,21,21,100);
    child("display:grid;grid-template-columns:200px 200px","margin:-1px",400,0,0,100);
    child("display:grid;grid-template-columns:200px 200px","margin-left:auto;margin-right:auto",400,51,1,100);
    child("","margin-left:10%;margin-inline-start:-1px",400,0,1,100);
    child("","margin-left:auto;margin-inline-start:-1px",400,0,1,100);
    page("<!doctype html><style>body{margin:0}#p{width:400px;border:1px solid}"
         "#x{height:20px;margin-left:-10px;margin-right:-10px}</style>"
         "<div id=p><div id=x></div></div>",1000); box("x",-9,1,420,20);
    page("<!doctype html><style>body{margin:0}#p{width:50%;border:1px solid}"
         "#x{width:100px;height:20px;margin:10%}</style>"
         "<div id=p><div id=x></div></div>",800); box("x",41,41,100,20);
    layout_page(g_root,1600); box("x",81,81,100,20);
    /* The real example.com declaration: body centering used to bypass the
     * block auto-margin algorithm entirely. Viewport height is explicit. */
    css_viewport(1000,800);
    page("<!doctype html><style>body{width:60vw;margin:15vh auto;height:20px}</style><body id=x></body>",1000);
    box("x",200,120,600,20);
    printf("margin: %d checks, %d failures\n",checks,fails);
    return fails ? 1 : 0;
}
