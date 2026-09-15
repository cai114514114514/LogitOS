/* Ordinary HTML/CSS through the browser's actual style and layout order.
 * Percent/font-relative line heights compute a length BEFORE inheritance;
 * unitless numbers retain their ratio and use each descendant's font size.
 * The negative build restores the old computed-value conversion, making
 * 130% mean 130px and inherited 1.3em re-evaluate on the child's font. */
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





static struct cstyle *ST(const char *id){return (struct cstyle *)ID(id)->style;}
static int text_y(const char *text){
    const struct item *it=layout_items();
    for(int i=0;i<layout_count();i++) if(it[i].type==IT_TEXT && it[i].len==(int)strlen(text) && !memcmp(it[i].text,text,it[i].len)) return it[i].y;
    return -10000;
}
static void done(void){dom_free(g_root);g_root=NULL;}
static void line_case(const char *value,int parent,int child,const char *cssom){
    char html[2048],msg[256],out[128];
    snprintf(html,sizeof html,"<!doctype html><style>body{margin:0}#p{font-size:20px;line-height:%s}#c{font-size:40px}#g{font-size:10px}</style><div id=p><span id=c>A<br>B</span><span id=g>C</span></div>",value);
    page(html,500);
    printf("value=%s parent-font=%d line=%d child-font=%d line=%d delta=%d\n",value,ST("p")->font_px,ST("p")->line_px,ST("c")->font_px,ST("c")->line_px,text_y("B")-text_y("A"));
    snprintf(msg,sizeof msg,"%s parent computes expected line height",value);EQ(ST("p")->line_px,parent,msg);
    snprintf(msg,sizeof msg,"%s inheritance keeps length or ratio",value);EQ(ST("c")->line_px,child,msg);
    snprintf(msg,sizeof msg,"%s painted line advance",value);EQ(text_y("B")-text_y("A"),child,msg);
    css_computed_text(ID("c"),CSSP_LINE_HEIGHT,out,sizeof out);
    snprintf(msg,sizeof msg,"%s computed CSSOM value",value);CHECK(!strcmp(out,cssom),msg);
    done();
}
int main(void){
    line_case("150%",30,30,"30px");
    line_case("130%",26,26,"26px");
    line_case("1.5",30,60,"1.5");
    line_case("1.3em",26,26,"26px");
    line_case("26px",26,26,"26px");
    line_case("0%",0,0,"0px");
    line_case("0px",0,0,"0px");
    line_case("0",0,0,"0");
    line_case("12.5%",3,3,"3px");
    printf("line-height: %d checks, %d failures\n",checks,fails);
    return fails?1:0;
}
