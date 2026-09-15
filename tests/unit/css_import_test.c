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
#include "css_import.h"
#include "url.h"

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


struct fixture { const char *url, *css, *final; };
static const struct fixture fixtures[]={
 {"http://site/css/basic.css","div.related h3{display:none}div.related ul{margin:0;padding:0;list-style:none}div.related li{display:inline}",0},
 {"http://site/css/first.css","@import 'sub/second.css'; .first{color:red}",0},
 {"http://site/css/sub/second.css",".second{background:url(icon.png)}",0},
 {"http://site/css/redirect.css","@import 'dep.css'; .redirect{}","http://cdn/pkg/main.css"},
 {"http://cdn/pkg/dep.css",".redirect_dep{}",0},
 {"http://site/css/cycle.css","@import 'main.css'; .cycle{}",0},
 {"http://site/css/order.css","#x{margin-left:10px}",0},
 {0,0,0}
};
static int calls;
static int resolve(void *ctx,const char *base,const char *ref,char *out,int cap) {
 (void)ctx;struct url u;if(url_parse(base,&u))return -1;return url_resolve(&u,ref,out,cap);
}
static int fetch(void *ctx,const char *url,unsigned char **data,int *len,char *final,int cap) {
 (void)ctx;calls++;
 for(int i=0;fixtures[i].url;i++)if(!strcmp(fixtures[i].url,url)){
   *len=(int)strlen(fixtures[i].css);*data=malloc((size_t)*len+1);memcpy(*data,fixtures[i].css,(size_t)*len+1);
   snprintf(final,cap,"%s",fixtures[i].final?fixtures[i].final:url);return 0;
 }
 /* A small ordinary depth chain, one short rule per sheet, never a large allocation. */
 int depth=-1;
 if(sscanf(url,"http://site/css/depth%d.css",&depth)==1&&depth>=0&&depth<12){
   char s[96];snprintf(s,sizeof s,"@import 'depth%d.css'; .depth%d{}",depth+1,depth);
   *len=(int)strlen(s);*data=malloc((size_t)*len+1);memcpy(*data,s,(size_t)*len+1);snprintf(final,cap,"%s",url);return 0;
 }
 return -1;
}
static const struct css_import_io io={resolve,fetch,0};
static char expanded[32768];
static struct css_import_budget budget;
static int expand(const char *s){memset(&budget,0,sizeof budget);calls=0;return css_import_expand(s,(int)strlen(s),"http://site/css/main.css",expanded,0,sizeof expanded,&budget,&io);}
static void style_page(const char *css) {
 const char *h="<!doctype html><div class=related><h3 id=title>Navigation</h3><ul><li id=x>one</li><li id=two>two</li></ul></div>";
 g_root=dom_parse(h,(int)strlen(h));css_apply(g_root,css,(int)strlen(css));css_extra_apply(g_root,css,(int)strlen(css));layout_page(g_root,1000);
}
static int box_y(const char *id){int x,y,w,h;CHECK(layout_node_box(ID(id),&x,&y,&w,&h),"geometry exists");return y;}
int main(void) {
 CHECK(expand("@import 'basic.css'; body{margin:0}")>=0,"expand simple");
 style_page(expanded);int x,y,w,h;
 CHECK(!layout_node_box(ID("title"),&x,&y,&w,&h),"imported rule hides Navigation");
 int first_y=box_y("x"),second_y=box_y("two");
 EQ(first_y,second_y,"imported rule makes navigation horizontal");EQ(budget.loaded,1,"one dependency loaded");
 CHECK(expand("@import url('first.css'); .parent{}")>=0,"nested import");
 char *a=strstr(expanded,".second"),*b=strstr(expanded,".first"),*c=strstr(expanded,".parent");
 CHECK(a&&b&&c&&a<b&&b<c,"depth-first cascade order");
 CHECK(strstr(expanded,"http://site/css/sub/icon.png")!=0,"relative resource keeps imported sheet base");EQ(calls,2,"nested dependency requests");
 CHECK(expand("@import 'redirect.css';")>=0,"redirected sheet");CHECK(strstr(expanded,".redirect_dep")!=0,"nested import uses final response URL");
 CHECK(expand("@import 'order.css';#x{margin-left:20px}")>=0,"parent override");style_page(expanded);EQ(((struct cstyle *)ID("x")->style)->ml,20,"parent rule overrides imported rule");
 CHECK(expand("@import 'basic.css' screen and (min-width:600px);")>=0,"conditional import");
 css_viewport(500,800);style_page(expanded);CHECK(layout_node_box(ID("title"),&x,&y,&w,&h),"media false keeps heading");
 css_viewport(1000,800);style_page(expanded);CHECK(!layout_node_box(ID("title"),&x,&y,&w,&h),"media true hides heading after resize");
 CHECK(expand("@import 'cycle.css'; .parent{}")>=0,"cycle finite");EQ(budget.cycles,1,"path cycle recorded");EQ(calls,1,"cycle not fetched again");CHECK(strstr(expanded,".cycle")!=0,"cycle retains noncyclic rules");
 CHECK(expand("@import 'basic.css';@import 'basic.css';")>=0,"repeated import");EQ(budget.loaded,2,"distinct cascade occurrences retained");
 CHECK(expand("/* @import 'basic.css'; */ .x{content:\"@import 'basic.css';\"} @import 'basic.css';")>=0,"comments strings and late imports");EQ(calls,0,"non-import text never fetched");
 CHECK(expand("@import 'missing.css'; .survives{}")>=0,"missing dependency");EQ(budget.failed,1,"fetch failure recorded");CHECK(strstr(expanded,".survives")!=0,"parent survives failed dependency");
 CHECK(expand("@import 'basic.css' layer(theme);@import 'basic.css' supports(display:grid);")>=0,"unsupported qualifiers retained");EQ(calls,0,"unsupported conditions never applied unconditionally");EQ(budget.unsupported,2,"unsupported qualifiers counted");
 CHECK(expand("@import 'depth0.css';")>=0,"depth bounded");EQ(budget.loaded,CSS_IMPORT_DEPTH,"bounded graph traversed");EQ(budget.limited,1,"depth limit recorded");
 memset(&budget,0,sizeof budget);budget.requests=CSS_IMPORT_REQUESTS;calls=0;
 css_import_expand("@import 'basic.css';",(int)strlen("@import 'basic.css';"),"http://site/css/main.css",expanded,0,sizeof expanded,&budget,&io);
 EQ(calls,0,"request budget prevents fetch");EQ(budget.limited,1,"request limit recorded");
 memset(&budget,0,sizeof budget);budget.bytes=CSS_IMPORT_BYTES-1;
 const char *s="@import 'basic.css';";css_import_expand(s,(int)strlen(s),"http://site/css/main.css",expanded,0,sizeof expanded,&budget,&io);
 EQ(budget.limited,1,"byte limit recorded");EQ(budget.loaded,0,"overbudget sheet not applied");
 memset(&budget,0,sizeof budget);strcpy(expanded,"kept");
 CHECK(css_import_expand(".too_long{color:red}",20,"http://site/main.css",expanded,4,12,&budget,&io)<0,"output budget reported");CHECK(!strcmp(expanded,"kept"),"output rollback preserves preceding sheet");
 printf("css-import: %d checks, %d failures\n",checks,fails);return fails?1:0;
}
