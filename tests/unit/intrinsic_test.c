/* Intrinsic widths through the real DOM -> LibCSS -> layout pipeline.
 * The negative build restores max(child) and empty-control measurement; it
 * must fail geometry assertions. Host glyph advances are fixed at px/2;
 * actual fonts, guest pixels and input remain the integration gate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "layout.h"
#include "css.h"
#include "dom.h"

void *kmalloc(unsigned long n){ return malloc(n); }
void  kfree(void *p){ free(p); }
/* A deterministic glyph oracle used by BOTH intrinsic sizing and placement.
 * This isolates their agreement; it does not measure guest font shaping. */
int text_measure(const char *s, int len, int px, int mono){ (void)s;(void)mono; return len*(px/2); }
int res_fetch(const char *u, uint8_t **b, int *l){ (void)u;(void)b;(void)l; return -1; }
void img_free(struct image *o){ (void)o; }
int img_decode(const uint8_t *p, int n, struct image *o){ (void)p;(void)n;(void)o; return -1; }

static int checks, fails;
#define CHECK(c,m) do{ checks++; if(!(c)){ printf("  FAIL: %s\n",(m)); fails++; } }while(0)
#define EQ(got,want,m) do{ int actual=(got), expected=(want); checks++; if(actual!=expected){ \
        printf("  FAIL: %s -- got %d, want %d\n",(m),actual,expected); fails++; } }while(0)

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


static void fixture(const char *body) {
    char html[4096];
    snprintf(html,sizeof html,"<style>body{margin:0;font-size:16px;line-height:24px}#f{float:right}</style>%s",body);
    page(html,400);
}
static int width(const char *id) { int x,y,w,h; CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"box exists"); return w; }
static int height(const char *id) { int x,y,w,h; CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"box exists"); return h; }
static void contained(const char *id) {
    int x,y,w,h,fx,fy,fw,fh;
    CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"child box exists");
    CHECK(layout_node_box(ID("f"),&fx,&fy,&fw,&fh),"float box exists");
    CHECK(x>=fx && x+w<=fx+fw,"control stays within float");
    CHECK(x+w<=400,"control stays within viewport");
}
int main(void) {
    /* Markup boundaries must not change an unwrapped line's width. These
     * numbers use the host's 8px glyph oracle, not guest font measurements. */
    fixture("<div id=f>modules |</div>");
    EQ(width("f"),72,"plain inline max-content");
    fixture("<div id=f><a>modules</a> |</div>");
    EQ(width("f"),72,"nested inline widths are summed");
    EQ(height("f"),24,"separator stays on the first line");
    fixture("<div id=f><b>a</b><i>b</i></div>");
    EQ(width("f"),16,"adjacent inline nodes invent no space");
    fixture("<div id=f>  <b>a </b> <i>b</i>  </div>");
    EQ(width("f"),24,"cross-node whitespace collapses once");
    fixture("<div id=f><span>ab</span><div>c</div><span>de</span><span>fg</span></div>");
    EQ(width("f"),32,"block children separate inline runs");
    fixture("<div id=f><input id=i size=20></div>");
    EQ(width("f"),172,"text input contributes its intrinsic box");
    EQ(width("i"),172,"text input preserves intrinsic width"); contained("i");
    fixture("<div id=f><div style='display:inline'><form style='display:inline'><input id=i size=20><input id=s type=submit value=Go></form></div> |</div>");
    EQ(width("i"),172,"nested form preserves input width");
    CHECK(width("f")>=width("i")+width("s")+8,"nested form sums controls and separator");
    EQ(height("f"),height("i"),"nested controls and separator stay on one line");
    contained("i"); contained("s");
    fixture("<div id=f>Theme <select id=s><option>Auto</option><option>Light</option></select> |</div>");
    EQ(width("s"),70,"select uses widest option plus chrome");
    CHECK(width("f")>=40+70+8,"select contributes its box rather than option text"); contained("s");
    fixture("<div id=f><input id=i size=20 style='width:80px;padding:3px;border:2px solid'></div>");
    EQ(width("f"),90,"control content-box CSS width counts frame once");
    fixture("<div id=f><input id=i size=20 style='width:80px;padding:3px;border:2px solid;box-sizing:border-box'></div>");
    EQ(width("f"),80,"control border-box CSS width counts frame once");
    fixture("<div id=f><input type=hidden><span style='display:none'>long hidden label</span>x</div>");
    EQ(width("f"),8,"hidden controls and display none contribute nothing");
    fixture("<div id=f style='display:flex;flex-direction:column'><span>ab</span><span>cdef</span></div>");
    EQ(width("f"),32,"flex columns keep the widest child");
    fixture("<div id=f><input size=20 style='display:block'><input size=10 style='display:block'></div>");
    EQ(width("f"),172,"block controls stack instead of summing");
    fixture("<div id=f>ab<br>cdef</div>");
    EQ(width("f"),32,"forced break separates max-content lines");
    fixture("<div id=f style='white-space:pre'>a  b\nc</div>");
    EQ(width("f"),32,"pre preserves spaces and hard breaks");
    fixture("<div id=f><span>a</span><img id=i width=40 height=16><span>b</span></div>");
    EQ(width("f"),56,"replaced image joins inline run atomically");
    contained("i");
    printf("intrinsic: %d checks, %d failures\n",checks,fails);
    return fails ? 1 : 0;
}
